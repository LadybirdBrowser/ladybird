/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <LibCore/LocalServer.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/WebDriver/Proxy.h>
#include <LibWeb/WebDriver/TimeoutsConfiguration.h>
#include <LibWeb/WebDriver/UserPrompt.h>
#include <WebDriver/Session.h>

namespace WebDriver {

static HashMap<String, NonnullRefPtr<Session>> s_sessions;
static HashMap<String, NonnullRefPtr<Session>> s_http_sessions;

// https://w3c.github.io/webdriver/#dfn-create-a-session
ErrorOr<NonnullRefPtr<Core::Promise<Session::NewSession, Web::WebDriver::Error>>> Session::create(NonnullRefPtr<Client> client, JsonValue initial_capabilities, Web::WebDriver::SessionFlags flags)
{
    auto session_creation_promise = Core::Promise<NewSession, Web::WebDriver::Error>::construct();

    // 1. Let session id be the result of generating a UUID.
    auto session_id = MUST(Web::Crypto::generate_random_uuid());

    // 2. Let session be a new session with session ID session id, and HTTP flag flags contains "http".
    auto session = adopt_ref(*new Session(client, initial_capabilities.as_object(), move(session_id), flags));
    auto session_start_promise = TRY(session->start(client->launch_browser_callback()));
    session_creation_promise->add_child(session_start_promise);

    session_start_promise->when_resolved([final_capabilities = move(initial_capabilities), session, flags, session_creation_promise](auto&) mutable -> ErrorOr<void> {
        auto& capabilities = final_capabilities.as_object();

        // 3. Let proxy be the result of getting property "proxy" from capabilities and run the substeps of the first matching statement:
        // -> proxy is a proxy configuration object
        if (auto proxy = capabilities.get_object("proxy"sv); proxy.has_value()) {
            // Take implementation-defined steps to set the user agent proxy using the extracted proxy configuration. If the
            // defined proxy cannot be configured return error with error code session not created. Otherwise set the has
            // proxy configuration flag to true.
            return Error::from_string_literal("Proxy configuration is not yet supported");
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

        Vector<NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>>> setup_promises;

        auto set_user_prompt_handler_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
        session->perform_async_action(set_user_prompt_handler_promise, [](auto& connection, auto request_id) {
            connection.async_set_user_prompt_handler(request_id, Web::WebDriver::user_prompt_handler());
        });
        setup_promises.append(move(set_user_prompt_handler_promise));

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
                auto page_load_strategy = Web::WebDriver::page_load_strategy_from_string(*strategy);
                session->m_page_load_strategy = page_load_strategy;

                auto set_page_load_strategy_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
                session->perform_async_action(set_page_load_strategy_promise, [page_load_strategy](auto& connection, auto request_id) {
                    connection.async_set_page_load_strategy(request_id, page_load_strategy);
                });
                setup_promises.append(move(set_page_load_strategy_promise));
            } else {
                capabilities.set("pageLoadStrategy"sv, "normal"sv);
            }

            // 3. Let strictFileInteractability be the result of getting property "strictFileInteractability" from .
            //    capabilities. If strictFileInteractability is a boolean, set session's strict file interactability to
            //    strictFileInteractability.
            if (auto strict_file_interactability = capabilities.get_bool("strictFileInteractability"sv); strict_file_interactability.has_value()) {
                session->m_strict_file_interactability = *strict_file_interactability;

                auto set_strict_file_interactiblity_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
                session->perform_async_action(set_strict_file_interactiblity_promise, [strict_file_interactability = *strict_file_interactability](auto& connection, auto request_id) {
                    connection.async_set_strict_file_interactability(request_id, strict_file_interactability);
                });
                setup_promises.append(set_strict_file_interactiblity_promise);
            }

            // 4. Let timeouts be the result of getting a property "timeouts" from capabilities. If timeouts is not
            //    undefined, set session's session timeouts to timeouts.
            if (auto timeouts = capabilities.get_object("timeouts"sv); timeouts.has_value()) {
                auto set_timeouts_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
                session->set_timeouts(*timeouts, set_timeouts_promise);
                setup_promises.append(move(set_timeouts_promise));
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
        auto set_is_webdriver_active_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
        session->perform_async_action(set_is_webdriver_active_promise, [](auto& connection, auto request_id) {
            connection.async_set_is_webdriver_active(request_id, true);
        });
        setup_promises.append(move(set_is_webdriver_active_promise));

        auto after_all_setup_promises = Core::Promise<JsonValue, Web::WebDriver::Error>::after(move(setup_promises));

        after_all_setup_promises->when_resolved([session, session_creation_promise, final_capabilities = move(final_capabilities)](auto&) {
            session_creation_promise->resolve(NewSession {
                .session = session,
                .capabilities = move(final_capabilities),
            });
        });

        after_all_setup_promises->when_rejected([session_creation_promise](Web::WebDriver::Error& error) {
            session_creation_promise->reject(Web::WebDriver::Error(error));
        });

        session_creation_promise->add_child(move(after_all_setup_promises));
        return {};
    });

    session_start_promise->when_rejected([session_creation_promise](Error& error) {
        session_creation_promise->reject(error);
    });

    return session_creation_promise;
}

Session::Session(NonnullRefPtr<Client> client, JsonObject const& capabilities, String session_id, Web::WebDriver::SessionFlags flags)
    : m_client(move(client))
    , m_options(capabilities)
    , m_session_id(move(session_id))
    , m_session_flags(flags)
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

// https://w3c.github.io/webdriver/#dfn-close-the-session
NonnullRefPtr<Core::Promise<Empty, Web::WebDriver::Error>> Session::close()
{
    if (m_close_promise)
        return *m_close_promise;

    m_close_promise = Core::Promise<Empty, Web::WebDriver::Error>::construct();

    // 1. If session's HTTP flag is set, remove session from active HTTP sessions.
    if (has_flag(session_flags(), Web::WebDriver::SessionFlags::Http))
        s_http_sessions.remove(m_session_id);

    // 2. Remove session from active sessions.
    s_sessions.remove(m_session_id);

    Vector<NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>>> all_session_close_promises;

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
        for (auto& it : m_windows) {
            auto close_session_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
            auto close_session_request_id = it.value.web_content_connection->create_pending_request(close_session_promise);
            it.value.web_content_connection->async_close_session(close_session_request_id);
            all_session_close_promises.append(move(close_session_promise));
        }
    }
    // -> Remote end is an intermediary node
    //     1. Close the associated session. If this causes an error to occur, complete the remainder of this algorithm
    //        before returning the error.

    auto after_all_sessions_closed_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::after(move(all_session_close_promises));
    after_all_sessions_closed_promise->when_resolved([this_ref = NonnullRefPtr { *this }](auto&) -> ErrorOr<void, Web::WebDriver::Error> {
        // 4. Perform any implementation-specific cleanup steps.
        if (this_ref->m_browser_process.has_value())
            TRY(Core::System::kill(this_ref->m_browser_process->pid(), SIGTERM));

        if (this_ref->m_web_content_socket_path.has_value()) {
            TRY(Core::System::unlink(*this_ref->m_web_content_socket_path));
            this_ref->m_web_content_socket_path = {};
        }

        this_ref->m_close_promise->resolve({});
        return {};
    });

    after_all_sessions_closed_promise->when_rejected([this_ref = NonnullRefPtr { *this }](Web::WebDriver::Error& error) {
        this_ref->m_close_promise->reject(Web::WebDriver::Error(error));
    });

    // 5. If an error has occurred in any of the steps above, return the error, otherwise return success with data null.
    m_close_promise->add_child(move(after_all_sessions_closed_promise));
    return *m_close_promise;
}

ErrorOr<NonnullRefPtr<Core::LocalServer>> Session::create_server(NonnullRefPtr<ServerPromise> promise)
{
#if defined(AK_OS_WINDOWS)
    static_assert(IsSame<IPC::Transport, IPC::TransportSocketWindows>, "Need to handle other IPC transports here");
#else
    static_assert(IsSame<IPC::Transport, IPC::TransportSocket>, "Need to handle other IPC transports here");
#endif

    dbgln("Listening for WebDriver connection on {}", *m_web_content_socket_path);

    (void)Core::System::unlink(*m_web_content_socket_path);

    auto server = Core::LocalServer::construct();
    server->listen(*m_web_content_socket_path);

    server->on_accept = [this_ref = NonnullRefPtr { *this }, promise](auto client_socket) {
        auto maybe_connection = adopt_nonnull_ref_or_enomem(new (nothrow) WebContentConnection(make<IPC::Transport>(move(client_socket))));
        if (maybe_connection.is_error()) {
            promise->reject(maybe_connection.release_error());
            return;
        }

        dbgln("WebDriver is connected to WebContent socket");
        auto web_content_connection = maybe_connection.release_value();

        auto inner_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
        inner_promise->when_resolved([web_content_connection, this_ref, promise](JsonValue& window_handle_value) {
            auto const& window_handle = window_handle_value.as_string();

            web_content_connection->on_close = [this_ref, window_handle] {
                dbgln_if(WEBDRIVER_DEBUG, "Window {} was closed remotely.", window_handle);
                this_ref->m_windows.remove(window_handle);
                if (this_ref->m_windows.is_empty())
                    (void)this_ref->close();
            };

            Vector<NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>>> setup_promises;

            auto set_page_load_strategy_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
            auto set_page_load_strategy_request_id = web_content_connection->create_pending_request(set_page_load_strategy_promise);
            web_content_connection->async_set_page_load_strategy(set_page_load_strategy_request_id, this_ref->m_page_load_strategy);
            setup_promises.append(move(set_page_load_strategy_promise));

            auto set_strict_file_interactability_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
            auto set_strict_file_interactability_request_id = web_content_connection->create_pending_request(set_strict_file_interactability_promise);
            web_content_connection->async_set_strict_file_interactability(set_strict_file_interactability_request_id, this_ref->m_strict_file_interactability);
            setup_promises.append(move(set_strict_file_interactability_promise));

            auto set_user_prompt_handler_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
            auto set_user_prompt_handler_request_id = web_content_connection->create_pending_request(set_user_prompt_handler_promise);
            web_content_connection->async_set_user_prompt_handler(set_user_prompt_handler_request_id, Web::WebDriver::user_prompt_handler());
            setup_promises.append(move(set_user_prompt_handler_promise));

            if (this_ref->m_timeouts_configuration.has_value()) {
                auto set_timeouts_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
                auto set_timeouts_request_id = web_content_connection->create_pending_request(set_timeouts_promise);
                web_content_connection->async_set_timeouts(set_timeouts_request_id, *this_ref->m_timeouts_configuration);
                setup_promises.append(move(set_timeouts_promise));
            }

            auto after_all_setup_promises = Core::Promise<JsonValue, Web::WebDriver::Error>::after(move(setup_promises));

            after_all_setup_promises->when_resolved([this_ref, promise, web_content_connection, window_handle](auto&) {
                this_ref->m_windows.set(window_handle, Session::Window { window_handle, move(web_content_connection) });

                if (this_ref->m_current_window_handle.is_empty())
                    this_ref->m_current_window_handle = window_handle;

                if (auto callback_iterator = this_ref->m_window_handle_became_available_callbacks.find(window_handle); callback_iterator != this_ref->m_window_handle_became_available_callbacks.end())
                    callback_iterator->value();

                promise->resolve({});
            });

            after_all_setup_promises->when_rejected([promise](Web::WebDriver::Error& error) {
                promise->reject(Error::from_string_view(error.error.bytes_as_string_view()));
            });

            promise->add_child(move(after_all_setup_promises));
        });

        inner_promise->when_rejected([promise](auto&) {
            promise->reject(Error::from_string_literal("Window was closed immediately"));
        });

        auto inner_promise_request_id = web_content_connection->create_pending_request(inner_promise);
        web_content_connection->async_get_window_handle(inner_promise_request_id);
        promise->add_child(move(inner_promise));
    };

    server->on_accept_error = [promise](auto error) {
        promise->reject(move(error));
    };

    return server;
}

ErrorOr<NonnullRefPtr<Session::ServerPromise>> Session::start(LaunchBrowserCallback const& launch_browser_callback)
{
    auto promise = ServerPromise::construct();

    m_web_content_socket_path = ByteString::formatted("{}/webdriver/session_{}_{}", TRY(Core::StandardPaths::runtime_directory()), Core::System::getpid(), m_session_id);
    m_web_content_server = TRY(create_server(promise));

    m_browser_process = TRY(launch_browser_callback(*m_web_content_socket_path, m_options.headless));

    return promise;
}

void Session::set_timeouts(JsonValue payload, NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> top_level_promise)
{
    auto inner_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
    top_level_promise->add_child(inner_promise);
    inner_promise->when_resolved([this_ref = NonnullRefPtr { *this }, top_level_promise](JsonValue& value) {
        this_ref->m_timeouts_configuration = value;
        top_level_promise->resolve(JsonValue {});
    });

    inner_promise->when_rejected([top_level_promise](Web::WebDriver::Error& error) {
        top_level_promise->reject(Web::WebDriver::Error(error));
    });

    perform_async_action(inner_promise, [payload = move(payload)](auto& connection, auto request_id) {
        connection.async_set_timeouts(request_id, move(payload));
    });
}

// 11.2 Close Window, https://w3c.github.io/webdriver/#dfn-close-window
void Session::close_window(NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> top_level_promise)
{
    auto inner_promise = Core::Promise<JsonValue, Web::WebDriver::Error>::construct();
    top_level_promise->add_child(inner_promise);
    inner_promise->when_resolved([this_ref = NonnullRefPtr { *this }, top_level_promise](JsonValue&) {
        auto after_closure = [this_ref, top_level_promise] {
            this_ref->m_windows.remove(this_ref->m_current_window_handle);
            this_ref->m_current_window_handle = "NoSuchWindowPleaseSelectANewOne"_string;

            top_level_promise->resolve(MUST(this_ref->get_window_handles()));
        };

        // 4. If there are no more open top-level browsing contexts, then close the session.
        if (this_ref->m_windows.size() == 1) {
            auto close_promise = this_ref->close();
            close_promise->when_resolved([after_closure = move(after_closure)](auto&) {
                after_closure();
            });

            close_promise->when_rejected([top_level_promise](Web::WebDriver::Error& error) {
                top_level_promise->reject(Web::WebDriver::Error(error));
            });

            top_level_promise->add_child(move(close_promise));
        } else {
            after_closure();
        }
    });

    inner_promise->when_rejected([top_level_promise](Web::WebDriver::Error& error) {
        top_level_promise->reject(Web::WebDriver::Error(error));
    });

    // 3. Close the current top-level browsing context.
    perform_async_action(inner_promise, [](auto& connection, auto request_id) {
        connection.async_close_window(request_id);
    });
}

// 11.3 Switch to Window, https://w3c.github.io/webdriver/#dfn-switch-to-window
void Session::switch_to_window(StringView handle, NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> top_level_promise)
{
    // 4. If handle is equal to the associated window handle for some top-level browsing context, let context be the that
    //    browsing context, and set the current top-level browsing context with session and context.
    //    Otherwise, return error with error code no such window.
    if (auto it = m_windows.find(handle); it != m_windows.end()) {
        m_current_window_handle = it->key;
    } else {
        top_level_promise->reject(Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
        return;
    }

    // 5. Update any implementation-specific state that would result from the user selecting the current
    //    browsing context for interaction, without altering OS-level focus.
    perform_async_action(top_level_promise, [this](auto& connection, auto request_id) {
        connection.async_switch_to_window(request_id, m_current_window_handle);
    });
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

ErrorOr<void, Web::WebDriver::Error> Session::ensure_current_window_handle_is_valid() const
{
    if (auto current_window = m_windows.get(m_current_window_handle); current_window.has_value())
        return {};
    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv);
}

void Session::add_window_handle_became_available_callback(String const& handle, Function<void()> callback)
{
    m_window_handle_became_available_callbacks.set(handle, move(callback));
}

void Session::remove_window_handle_became_available_callback(String const& handle)
{
    m_window_handle_became_available_callbacks.remove(handle);
}

}
