/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/NumericLimits.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#if !defined(AK_OS_MACOS)
#    include <LibCore/LocalServer.h>
#    include <LibCore/Socket.h>
#    include <LibCore/StandardPaths.h>
#else
#    include <LibIPC/TransportBootstrapMach.h>
#    include <LibWebView/Utilities.h>
#endif
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibIPC/Transport.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/WebDriver/Proxy.h>
#include <LibWeb/WebDriver/TimeoutsConfiguration.h>
#include <LibWeb/WebDriver/UserPrompt.h>
#include <WebDriver/Session.h>

namespace WebDriver {

static HashMap<String, NonnullRefPtr<Session>> s_sessions;
static HashMap<String, NonnullRefPtr<Session>> s_http_sessions;

struct SessionCreationState;
static WeakPtr<SessionCreationState> s_http_session_creation;

struct SessionCreationState
    : public RefCounted<SessionCreationState>
    , public Weakable<SessionCreationState> {
    SessionCreationState(JsonValue capabilities, Web::WebDriver::SessionFlags flags)
        : capabilities(move(capabilities))
        , promise(Session::NewSessionPromise::construct())
    {
        if (has_flag(flags, Web::WebDriver::SessionFlags::Http))
            s_http_session_creation = *this;
    }

    JsonValue capabilities;
    NonnullRefPtr<Session::NewSessionPromise> promise;
};

static Web::WebDriver::Error session_not_created_error(AK::Error const& error)
{
    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::SessionNotCreated, MUST(String::formatted("Browser startup failed: {}", error)));
}

// https://w3c.github.io/webdriver/#dfn-create-a-session
ErrorOr<NonnullRefPtr<Session::NewSessionPromise>> Session::create(NonnullRefPtr<Client> client, JsonValue capabilities, Web::WebDriver::SessionFlags flags)
{
    if (has_flag(flags, Web::WebDriver::SessionFlags::Http) && s_http_session_creation)
        return Error::from_string_literal("An HTTP session is already being created");

    auto state = adopt_ref(*new SessionCreationState(move(capabilities), flags));

    // 1. Let session id be the result of generating a UUID.
    auto session_id = Web::Crypto::generate_random_uuid();

    // 2. Let session be a new session with session ID session id, and HTTP flag flags contains "http".
    auto session = adopt_ref(*new Session(client, state->capabilities.as_object(), move(session_id), flags));
    auto start_result = session->start(client->launch_browser_callback());
    if (start_result.is_error()) {
        auto error = start_result.release_error();
        session->close();
        return error;
    }
    auto start_promise = start_result.release_value();
    state->promise->add_child(start_promise);

    start_promise->when_resolved([state, session, flags](Empty&) mutable {
                     auto& capabilities = state->capabilities.as_object();

                     // 3. Let proxy be the result of getting property "proxy" from capabilities and run the substeps of the first matching statement:
                     // -> proxy is a proxy configuration object
                     if (auto proxy = capabilities.get_object("proxy"sv); proxy.has_value()) {
                         // Take implementation-defined steps to set the user agent proxy using the extracted proxy configuration. If the
                         // defined proxy cannot be configured return error with error code session not created. Otherwise set the has
                         // proxy configuration flag to true.
                         session->close();
                         state->promise->reject(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::SessionNotCreated, "Proxy configuration is not yet supported"sv));
                         return;
                     }
                     // -> Otherwise
                     else {
                         // Set a property of capabilities with name "proxy" and a value that is a new JSON Object.
                         capabilities.set("proxy"sv, JsonObject {});
                     }

                     // FIXME: 4. If capabilites has a property named "acceptInsecureCerts", set the endpoint node's accept insecure TLS flag
                     //           to the result of getting a property named "acceptInsecureCerts" from capabilities.

                     // 5. Let user prompt handler capability be the result of getting property "unhandledPromptBehavior" from capabilities.
                     auto user_prompt_handler_capability = capabilities.get_object("unhandledPromptBehavior"sv);

                     // 6. If user prompt handler capability is not undefined, update the user prompt handler with user prompt handler capability.
                     if (user_prompt_handler_capability.has_value())
                         Web::WebDriver::update_the_user_prompt_handler(*user_prompt_handler_capability);

                     session->m_browser_connection->async_set_user_prompt_handler(Web::WebDriver::user_prompt_handler());

                     // 7. Let serialized user prompt handler be serialize the user prompt handler.
                     auto serialized_user_prompt_handler = Web::WebDriver::serialize_the_user_prompt_handler();

                     // 8. Set a property on capabilities with the name "unhandledPromptBehavior", and the value serialized user prompt handler.
                     capabilities.set("unhandledPromptBehavior"sv, move(serialized_user_prompt_handler));

                     // 9. If flags contains "http":
                     if (has_flag(flags, Web::WebDriver::SessionFlags::Http)) {
                         // 1. Let strategy be the result of getting property "pageLoadStrategy" from capabilities. If strategy is a
                         //    string, set the session's page loading strategy to strategy. Otherwise, set the page loading strategy to
                         //    normal and set a property of capabilities with name "pageLoadStrategy" and value "normal".
                         if (auto strategy = capabilities.get_string("pageLoadStrategy"sv); strategy.has_value()) {
                             session->m_page_load_strategy = Web::WebDriver::page_load_strategy_from_string(*strategy);
                             session->m_browser_connection->async_set_page_load_strategy(session->m_page_load_strategy);
                         } else {
                             capabilities.set("pageLoadStrategy"sv, "normal"sv);
                         }

                         // 3. Let strictFileInteractability be the result of getting property "strictFileInteractability" from .
                         //    capabilities. If strictFileInteractability is a boolean, set session's strict file interactability to
                         //    strictFileInteractability.
                         if (auto strict_file_interactiblity = capabilities.get_bool("strictFileInteractability"sv); strict_file_interactiblity.has_value()) {
                             session->m_strict_file_interactiblity = *strict_file_interactiblity;
                             session->m_browser_connection->async_set_strict_file_interactability(session->m_strict_file_interactiblity);
                         }

                         // 4. Let timeouts be the result of getting a property "timeouts" from capabilities. If timeouts is not
                         //    undefined, set session's session timeouts to timeouts.
                         if (auto timeouts = capabilities.get_object("timeouts"sv); timeouts.has_value()) {
                             auto result = session->set_timeouts(*timeouts);
                             if (result.is_error()) {
                                 session->close();
                                 state->promise->reject(result.release_error());
                                 return;
                             }
                         }

                         // 5. Set a property on capabilities with name "timeouts" and value serialize the timeouts configuration with
                         //    session's session timeouts.
                         capabilities.set("timeouts"sv, session->m_timeouts_configuration.value_or_lazy_evaluated([]() {
                             return Web::WebDriver::timeouts_object({});
                         }));
                     }

                     // FIXME: 10. Process any extension capabilities in capabilities in an implementation-defined manner.

                     // FIXME: 11. Run any WebDriver new session algorithm defined in external specifications, with arguments session, capabilities, and flags.

                     // 12. Append session to active sessions.
                     s_sessions.set(session->session_id(), session);

                     // 13. If flags contains "http", append session to active HTTP sessions.
                     if (has_flag(flags, Web::WebDriver::SessionFlags::Http))
                         s_http_sessions.set(session->session_id(), session);

                     // 14. Set the webdriver-active flag to true.
                     // NB: WebContent sets the flag when a page's WebDriver session is created.

                     state->promise->resolve(NewSession { session, move(state->capabilities) });
                 })
        .when_rejected([state, session](Error& error) {
            auto web_driver_error = session_not_created_error(error);
            session->close();
            state->promise->reject(move(web_driver_error));
        });

    return state->promise;
}

Session::Session(NonnullRefPtr<Client> client, JsonObject const& capabilities, String session_id, Web::WebDriver::SessionFlags flags)
    : m_client(move(client))
    , m_options(capabilities)
    , m_session_id(move(session_id))
    , m_session_flags(flags)
    , m_event_loop(Core::EventLoop::current())
{
}

Session::~Session() = default;

ErrorOr<NonnullRefPtr<Session>, Web::WebDriver::Error> Session::find_session(StringView session_id, Web::WebDriver::SessionFlags session_flags, AllowInvalidWindowHandle allow_invalid_window_handle)
{
    auto& sessions = has_flag(session_flags, Web::WebDriver::SessionFlags::Http) ? s_http_sessions : s_sessions;

    if (auto session = sessions.get(session_id); session.has_value()) {
        if (allow_invalid_window_handle == AllowInvalidWindowHandle::No)
            TRY(session.value()->ensure_current_window_handle_is_valid());

        return *session.release_value();
    }

    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidSessionId, "Invalid session id"sv);
}

size_t Session::session_count(Web::WebDriver::SessionFlags session_flags)
{
    if (has_flag(session_flags, Web::WebDriver::SessionFlags::Http))
        return s_http_sessions.size();
    return s_sessions.size();
}

bool Session::has_pending_http_session_creation()
{
    return s_http_session_creation;
}

NonnullRefPtr<Session::WebDriverPromise> Session::enqueue_http_request(Function<NonnullRefPtr<WebDriverPromise>()> handler)
{
    auto promise = WebDriverPromise::construct();
    m_http_request_queue.enqueue({ move(handler), promise });
    if (m_http_request_queue.size() == 1)
        process_next_http_request();
    return promise;
}

void Session::process_next_http_request()
{
    m_event_loop.deferred_invoke([this_ref = NonnullRefPtr { *this }] {
        auto& request = this_ref->m_http_request_queue.head();
        auto promise = request.promise;

        // https://w3c.github.io/webdriver/#processing-model
        // 1. If session is no longer in the list of active sessions, then send an error with error code invalid session id and return.
        if (!s_sessions.contains(this_ref->m_session_id)) {
            promise->reject(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidSessionId, "Invalid session id"sv));
            this_ref->dequeue_current_http_request();
            return;
        }

        auto request_promise = request.handler();
        promise->add_child(request_promise);
        request_promise->when_resolved([this_ref, promise](JsonValue& value) {
                           promise->resolve(value);
                           this_ref->dequeue_current_http_request();
                       })
            .when_rejected([this_ref, promise](Web::WebDriver::Error& error) {
                promise->reject(Web::WebDriver::Error(error));
                this_ref->dequeue_current_http_request();
            });
    });
}

void Session::dequeue_current_http_request()
{
    (void)m_http_request_queue.dequeue();
    if (!m_http_request_queue.is_empty())
        process_next_http_request();
}

void Session::close_all()
{
    // close() unregisters each session as it runs, so snapshot first. s_sessions holds every
    // session (HTTP ones are also in s_http_sessions), so iterating it alone covers all of them.
    Vector<NonnullRefPtr<Session>> sessions;
    sessions.ensure_capacity(s_sessions.size());
    for (auto& session : s_sessions)
        sessions.unchecked_append(session.value);
    for (auto& session : sessions)
        session->close();
}

void Session::reject_pending_browser_commands()
{
    auto pending_browser_commands = move(m_pending_browser_commands);
    for (auto& command : pending_browser_commands) {
        command.value->reject(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Browser connection lost"sv));
    }
}

void Session::arm_browser_startup_timeout(NonnullRefPtr<ServerPromise> promise)
{
    m_start_promise = move(promise);
    static constexpr u32 BROWSER_STARTUP_TIMEOUT_MS = 30'000;
    m_start_timer = Core::Timer::create_single_shot(BROWSER_STARTUP_TIMEOUT_MS, [this] {
        [[maybe_unused]] auto timer_lifetime_guard = move(m_start_timer);
        reject_start_promise(Error::from_string_literal("Timed out waiting for browser startup"));
        close();
    });
    m_start_timer->start();
}

void Session::cancel_browser_startup_timeout()
{
    if (auto timer = move(m_start_timer))
        timer->stop();
}

void Session::reject_start_promise(AK::Error error)
{
    cancel_browser_startup_timeout();
    if (auto start_promise = move(m_start_promise))
        start_promise->reject(move(error));
}

// https://w3c.github.io/webdriver/#dfn-close-the-session
void Session::close()
{
    if (m_closing)
        return;
    m_closing = true;

    // NB: Step 2 removes this session from the active-sessions map — usually dropping the last reference to it. So hold
    //     a strong reference across close() — so removal can't destroy the session while the steps below still use it.
    auto protector = NonnullRefPtr { *this };

    // 1. If session's HTTP flag is set, remove session from active HTTP sessions.
    if (has_flag(session_flags(), Web::WebDriver::SessionFlags::Http))
        s_http_sessions.remove(m_session_id);

    // 2. Remove session from active sessions.
    s_sessions.remove(m_session_id);

    // 3. Perform the following substeps based on the remote end's type:
    // -> Remote end is an endpoint node
    //     1. If the list of active sessions is empty:
    if (s_sessions.is_empty()) {
        // 1. Set the webdriver-active flag to false
        // NOTE: This is handled by the WebContent process.

        // 2. Set the user prompt handler to null.
        Web::WebDriver::set_user_prompt_handler({});

        // FIXME: 3. Unset the accept insecure TLS flag.

        // 4. Reset the has proxy configuration flag to its default value.
        Web::WebDriver::reset_has_proxy_configuration();

        // 5. Optionally, close all top-level browsing contexts, without prompting to unload.
        // NB: The browser process closes its windows when the session tells it to shut down below.
    }
    // -> Remote end is an intermediary node
    //     1. Close the associated session. If this causes an error to occur, complete the remainder of this algorithm
    //        before returning the error.

    // 4. Perform any implementation-specific cleanup steps.
    reject_start_promise(Error::from_string_literal("Session closed during browser startup"));
    reject_pending_browser_commands();
    auto window_handle_callbacks = move(m_window_handle_became_available_callbacks);
    for (auto& callbacks : window_handle_callbacks) {
        for (auto& callback : callbacks.value)
            callback.on_session_close();
    }
    if (m_browser_connection) {
        m_browser_connection->on_close = nullptr;
        m_browser_connection->on_did_create_window = nullptr;
        m_browser_connection->on_did_close_window = nullptr;
        m_browser_connection->on_command_complete = nullptr;
        m_browser_connection->async_close_session();
        m_browser_connection = nullptr;
    }

    // The browser may have exited on its own already; its death is one of the triggers for
    // closing the session, so the process being gone is not an error here.
    if (m_browser_process.has_value()) {
        if (auto result = Core::Process::terminate_process(m_browser_process->pid(), Core::Process::TerminationMode::Graceful);
            result.is_error() && result.error().code() != ESRCH) {
            dbgln("Unable to terminate the browser process: {}", result.error());
        }
    }

#if defined(AK_OS_MACOS)
    m_browser_mach_port_server = nullptr;
#else
    if (!m_browser_endpoint.is_empty())
        MUST(FileSystem::remove(m_browser_endpoint, FileSystem::RecursionMode::Disallowed));
#endif
    m_browser_endpoint = {};

    // 5. If an error has occurred in any of the steps above, return the error, otherwise return success with data null.
}

ErrorOr<void> Session::accept_browser_transport(NonnullOwnPtr<IPC::Transport> transport)
{
    if (m_browser_connection)
        return {};

    auto browser_connection = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) BrowserConnection(move(transport))));
    dbgln("WebDriver is connected to the browser process");

    browser_connection->on_close = [this]() {
        // Keep the connection alive through this callback, while preventing close() from sending to the dead connection.
        [[maybe_unused]] auto connection_lifetime_guard = move(m_browser_connection);
        reject_start_promise(Error::from_string_literal("Browser connection lost"));
        reject_pending_browser_commands();
        close();
    };
    browser_connection->on_did_create_window = [this](String window_handle) {
        cancel_browser_startup_timeout();
        if (!m_windows.contains(window_handle))
            m_windows.set(window_handle, Session::Window { window_handle });
        if (m_current_window_handle.is_empty())
            m_current_window_handle = window_handle;

        dispatch_window_handle_became_available_callbacks(window_handle);
        if (auto start_promise = move(m_start_promise))
            start_promise->resolve({});
    };
    browser_connection->on_did_close_window = [this](String window_handle) {
        remove_window(window_handle);
    };
    browser_connection->on_command_complete = [this](u64 command_id, Web::WebDriver::Response response) {
        auto promise = m_pending_browser_commands.take(command_id);
        if (!promise.has_value()) {
            m_browser_connection->did_misbehave("Received response for unknown WebDriver command ID");
            return;
        }

        if (response.is_error())
            promise.value()->reject(response.release_error());
        else
            promise.value()->resolve(response.release_value());
    };

    m_browser_connection = move(browser_connection);
#if defined(AK_OS_MACOS)
    m_browser_mach_port_server = nullptr;
#else
    m_event_loop.deferred_invoke([session = NonnullRefPtr { *this }] {
        session->m_browser_server = nullptr;
    });
#endif
    return {};
}

NonnullRefPtr<Session::WebDriverPromise> Session::perform_browser_command(Function<void(u64 command_id)> send_command)
{
    auto promise = WebDriverPromise::construct();
    if (!m_browser_connection) {
        promise->reject(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::UnknownError, "Browser connection lost"sv));
        return promise;
    }

    auto command_id = m_next_browser_command_id++;
    m_pending_browser_commands.set(command_id, promise);

    send_command(command_id);
    return promise;
}

NonnullRefPtr<Session::WebDriverPromise> continue_with_promise(NonnullRefPtr<Session::WebDriverPromise> source, Function<NonnullRefPtr<Session::WebDriverPromise>()> continuation)
{
    auto promise = Session::WebDriverPromise::construct();
    promise->add_child(source);
    source->when_resolved([promise, continuation = move(continuation)](JsonValue&) mutable {
              auto continuation_promise = continuation();
              promise->add_child(continuation_promise);
              continuation_promise->when_resolved([promise](JsonValue& value) {
                                      promise->resolve(value);
                                  })
                  .when_rejected([promise](Web::WebDriver::Error& error) {
                      promise->reject(Web::WebDriver::Error(error));
                  });
          })
        .when_rejected([promise](Web::WebDriver::Error& error) {
            promise->reject(Web::WebDriver::Error(error));
        });
    return promise;
}

NonnullRefPtr<Session::WebDriverPromise> Session::navigate_to(URL::URL url)
{
    auto navigate_promise = perform_browser_command([this, url = move(url)](u64 command_id) {
        m_browser_connection->async_navigate_to(command_id, m_current_window_handle, url);
    });
    auto navigation_completion_promise = continue_with_promise(move(navigate_promise), [this_ref = NonnullRefPtr { *this }] {
        return this_ref->wait_for_navigation_completion();
    });

    // https://w3c.github.io/webdriver/#dfn-navigate-to
    // 8. Run these steps, but abort when timer's timeout fired flag is set:
    //    3. Set the current browsing context with session and current top-level browsing context.
    return continue_with_promise(move(navigation_completion_promise), [this_ref = NonnullRefPtr { *this }] {
        return this_ref->reset_current_browsing_context();
    });
}

NonnullRefPtr<Session::WebDriverPromise> Session::refresh()
{
    auto refresh_promise = perform_browser_command([this](u64 command_id) {
        m_browser_connection->async_refresh(command_id, m_current_window_handle);
    });
    auto navigation_completion_promise = continue_with_promise(move(refresh_promise), [this_ref = NonnullRefPtr { *this }] {
        return this_ref->wait_for_navigation_completion();
    });

    // https://w3c.github.io/webdriver/#dfn-refresh
    // 5. Set the current browsing context with session and session's current top-level browsing context.
    return continue_with_promise(move(navigation_completion_promise), [this_ref = NonnullRefPtr { *this }] {
        return this_ref->reset_current_browsing_context();
    });
}

NonnullRefPtr<Session::WebDriverPromise> Session::wait_for_navigation_completion()
{
    if (m_page_load_strategy == Web::WebDriver::PageLoadStrategy::None) {
        auto promise = WebDriverPromise::construct();
        promise->resolve(JsonValue {});
        return promise;
    }

    return perform_browser_command([this](u64 command_id) {
        m_browser_connection->async_wait_for_navigation_completion(command_id, m_current_window_handle, page_load_timeout());
    });
}

NonnullRefPtr<Session::WebDriverPromise> Session::traverse_history(i32 delta, HandleUserPrompts handle_user_prompts)
{
    return perform_browser_command([this, delta, handle_user_prompts](u64 command_id) {
        m_browser_connection->async_traverse_history(command_id, m_current_window_handle, delta, handle_user_prompts == HandleUserPrompts::Yes);
    });
}

NonnullRefPtr<Session::WebDriverPromise> Session::session_history()
{
    return perform_browser_command([this](u64 command_id) {
        m_browser_connection->async_get_session_history(command_id, m_current_window_handle);
    });
}

NonnullRefPtr<Session::WebDriverPromise> Session::load_url(URL::URL url)
{
    return perform_browser_command([this, url = move(url)](u64 command_id) {
        m_browser_connection->async_load_url(command_id, m_current_window_handle, url);
    });
}

NonnullRefPtr<Session::WebDriverPromise> Session::run_content_command(StringView name, JsonValue payload, Vector<String> arguments)
{
    return perform_browser_command([this, name = MUST(String::from_utf8(name)), payload = move(payload), arguments = move(arguments)](u64 command_id) mutable {
        m_browser_connection->async_run_content_command(command_id, m_current_window_handle, move(name), move(payload), move(arguments));
    });
}

NonnullRefPtr<Session::WebDriverPromise> Session::reset_current_browsing_context()
{
    return run_content_command("set_current_browsing_context_to_top_level"sv);
}

void Session::remove_window(StringView window_handle)
{
    if (!m_windows.remove(window_handle))
        return;

    if (m_current_window_handle == window_handle)
        m_current_window_handle = "NoSuchWindowPleaseSelectANewOne"_string;

    if (m_windows.is_empty())
        close();
}

ErrorOr<void> Session::create_server()
{
#if defined(AK_OS_WINDOWS)
    static_assert(IsSame<IPC::Transport, IPC::TransportSocketWindows>, "Need to handle other IPC transports here");
#elif defined(AK_OS_MACOS)
    static_assert(IsSame<IPC::Transport, IPC::TransportMachPort>, "Need to handle other IPC transports here");
#else
    static_assert(IsSame<IPC::Transport, IPC::TransportSocket>, "Need to handle other IPC transports here");
#endif

    dbgln("Listening for WebDriver connection on {}", m_browser_endpoint);

#if defined(AK_OS_MACOS)
    m_browser_mach_port_server = make<IPC::MachBootstrapListener>(m_browser_endpoint);
    if (!m_browser_mach_port_server->is_initialized())
        return Error::from_string_literal("Failed to initialize Mach port server for WebDriver");

    m_browser_mach_port_server->on_bootstrap_request = [this](auto request) {
        auto result = m_transport_bootstrap_server.handle_bootstrap_request(request.pid, move(request.reply_port));
        if (result.is_error()) {
            m_event_loop.deferred_invoke([this, error = result.release_error()]() mutable {
                reject_start_promise(move(error));
            });
            return;
        }

        result.release_value().visit(
            [](IPC::TransportBootstrapMachServer::ChildTransportHandled) {
                VERIFY_NOT_REACHED();
            },
            [this](IPC::TransportBootstrapMachServer::OnDemandTransport& transport) {
                m_event_loop.deferred_invoke([this, transport = move(transport.ports)]() mutable {
                    if (auto result = accept_browser_transport(make<IPC::Transport>(move(transport.receive_right), move(transport.send_right))); result.is_error())
                        reject_start_promise(result.release_error());
                });
            });
    };

    return {};
#else
    (void)FileSystem::remove(m_browser_endpoint, FileSystem::RecursionMode::Disallowed);

    auto server = Core::LocalServer::construct();
    server->listen(m_browser_endpoint);

    server->on_accept = [this](auto client_socket) {
        auto maybe_transport = IPC::Transport::from_socket(move(client_socket));
        if (maybe_transport.is_error()) {
            reject_start_promise(maybe_transport.release_error());
            return;
        }
        if (auto result = accept_browser_transport(maybe_transport.release_value()); result.is_error())
            reject_start_promise(result.release_error());
    };

    server->on_accept_error = [this](auto error) {
        if (m_browser_connection)
            return;
        reject_start_promise(move(error));
    };

    m_browser_server = server;
    return {};
#endif
}

ErrorOr<NonnullRefPtr<Session::ServerPromise>> Session::start(LaunchBrowserCallback const& launch_browser_callback)
{
    auto promise = ServerPromise::construct();

#if defined(AK_OS_MACOS)
    m_browser_endpoint = ByteString::formatted("{}.{}", WebView::mach_server_name_for_process("WebDriver"sv, Core::System::getpid()), m_session_id);
#else
    m_browser_endpoint = ByteString::formatted("{}/webdriver/session_{}_{}", TRY(Core::StandardPaths::runtime_directory()), Core::System::getpid(), m_session_id);
#endif
    TRY(create_server());

    m_browser_process = TRY(launch_browser_callback(m_browser_endpoint, m_options.headless));
    arm_browser_startup_timeout(promise);
    return promise;
}

// 9.1 Get Timeouts, https://w3c.github.io/webdriver/#dfn-get-timeouts
Web::WebDriver::Response Session::get_timeouts() const
{
    // 1. Let timeouts be the timeouts object for session’s timeouts configuration
    // 2. Return success with data timeouts.
    if (m_timeouts_configuration.has_value())
        return JsonValue { *m_timeouts_configuration };
    return JsonValue { Web::WebDriver::timeouts_object({}) };
}

// 9.2 Set Timeouts, https://w3c.github.io/webdriver/#dfn-set-timeouts
Web::WebDriver::Response Session::set_timeouts(JsonValue payload)
{
    // FIXME: Spec issue: As written, the spec replaces the timeouts configuration with the newly provided values. But
    //        all other implementations update the existing configuration with any new values instead. WPT relies on
    //        this behavior, and sends us one timeout value at time.
    //        https://github.com/w3c/webdriver/issues/1596

    // 1. Let timeouts be the result of trying to JSON deserialize as a timeouts configuration the request’s parameters.
    // 2. Make the session timeouts the new timeouts.
    TRY(Web::WebDriver::json_deserialize_as_a_timeouts_configuration_into(payload, m_timeouts));
    m_timeouts_configuration = Web::WebDriver::timeouts_object(m_timeouts);

    if (m_browser_connection)
        m_browser_connection->async_set_timeouts_configuration(*m_timeouts_configuration);

    // 3. Return success with data null.
    return JsonValue {};
}

Optional<u64> Session::page_load_timeout() const
{
    Optional<u64> page_load_timeout = Web::WebDriver::TimeoutsConfiguration {}.page_load_timeout;
    if (m_timeouts_configuration.has_value() && m_timeouts_configuration->is_object()) {
        if (auto value = m_timeouts_configuration->as_object().get("pageLoad"sv); value.has_value()) {
            if (value->is_null())
                page_load_timeout = {};
            else
                page_load_timeout = value->get_integer<u64>().value_or(*page_load_timeout);
        }
    }
    return page_load_timeout;
}

// 11.2 Close Window, https://w3c.github.io/webdriver/#dfn-close-window
NonnullRefPtr<Session::WebDriverPromise> Session::close_window()
{
    auto promise = WebDriverPromise::construct();

    // 3. Close the current top-level browsing context.
    auto close_window_promise = run_content_command("close_window"sv);
    promise->add_child(close_window_promise);
    close_window_promise->when_resolved([this_ref = NonnullRefPtr { *this }, promise](JsonValue&) {
                            // 4. If there are no more open top-level browsing contexts, then close the session.
                            auto closed_window_handle = this_ref->m_current_window_handle;
                            this_ref->remove_window(closed_window_handle);

                            // 5. Return the result of running the remote end steps for the Get Window Handles command.
                            auto result = this_ref->get_window_handles();
                            if (result.is_error())
                                promise->reject(result.release_error());
                            else
                                promise->resolve(result.release_value());
                        })
        .when_rejected([promise](Web::WebDriver::Error& error) {
            promise->reject(Web::WebDriver::Error(error));
        });
    return promise;
}

// 11.3 Switch to Window, https://w3c.github.io/webdriver/#dfn-switch-to-window
NonnullRefPtr<Session::WebDriverPromise> Session::switch_to_window(StringView handle)
{
    auto promise = WebDriverPromise::construct();

    // 4. If handle is equal to the associated window handle for some top-level browsing context, let context be the that
    //    browsing context, and set the current top-level browsing context with session and context.
    //    Otherwise, return error with error code no such window.
    if (auto it = m_windows.find(handle); it != m_windows.end()) {
        m_current_window_handle = it->key;
    } else {
        promise->reject(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
        return promise;
    }

    // 5. Update any implementation-specific state that would result from the user selecting the current
    //    browsing context for interaction, without altering OS-level focus.
    return run_content_command("switch_to_window"sv, {}, { m_current_window_handle });
}

// 11.4 Get Window Handles, https://w3c.github.io/webdriver/#dfn-get-window-handles
Web::WebDriver::Response Session::get_window_handles() const
{
    // 1. Let handles be a JSON List.
    JsonArray handles {};

    // 2. For each top-level browsing context in the remote end, push the associated window handle onto handles.
    for (auto const& window_handle : m_windows.keys()) {
        handles.must_append(JsonValue(window_handle));
    }

    // 3. Return success with data handles.
    return JsonValue { move(handles) };
}

void Session::dispatch_window_handle_became_available_callbacks(String const& window_handle)
{
    // Registrations are one-shot: take() erases them before invoking their callbacks.
    auto callbacks = m_window_handle_became_available_callbacks.take(window_handle);
    if (!callbacks.has_value())
        return;

    for (auto& callback : callbacks.value())
        callback.callback();
}

Session::WindowHandleBecameAvailableCallbackID Session::add_window_handle_became_available_callback(String const& handle, Function<void()> callback, Function<void()> on_session_close)
{
    auto id = m_next_window_handle_became_available_callback_id++;
    m_window_handle_became_available_callbacks.ensure(handle).append({ id, move(callback), move(on_session_close) });
    return id;
}

void Session::remove_window_handle_became_available_callback(String const& handle, WindowHandleBecameAvailableCallbackID id)
{
    auto callbacks = m_window_handle_became_available_callbacks.find(handle);
    if (callbacks == m_window_handle_became_available_callbacks.end())
        return;

    callbacks->value.remove_all_matching([id](auto const& callback) {
        return callback.id == id;
    });

    if (callbacks->value.is_empty())
        m_window_handle_became_available_callbacks.remove(callbacks);
}

ErrorOr<void, Web::WebDriver::Error> Session::ensure_current_window_handle_is_valid() const
{
    if (!m_windows.contains(m_current_window_handle))
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv);

    return {};
}

}
