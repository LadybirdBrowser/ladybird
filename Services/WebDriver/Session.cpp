/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Session.h"
#include "Client.h"
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <LibCore/LocalServer.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/WebDriver/Proxy.h>
#include <LibWeb/WebDriver/TimeoutsConfiguration.h>
#include <LibWeb/WebDriver/UserPrompt.h>
#include <unistd.h>

namespace WebDriver {

static HashMap<String, NonnullRefPtr<Session>> s_sessions;
static HashMap<String, NonnullRefPtr<Session>> s_http_sessions;

// https://w3c.github.io/webdriver/#dfn-create-a-session
ErrorOr<NonnullRefPtr<Session>> Session::create(NonnullRefPtr<Client> client, JsonObject& capabilities, Web::WebDriver::SessionFlags flags)
{
    // 1. Let session id be the result of generating a UUID.
    auto session_id = MUST(Web::Crypto::generate_random_uuid());

    // 2. Let session be a new session with session ID session id, and HTTP flag flags contains "http".
    auto session = adopt_ref(*new Session(client, capabilities, move(session_id), flags));
    TRY(session->start(client->launch_browser_callbacks()));

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

    session->web_content_connection().async_set_user_prompt_handler(Web::WebDriver::user_prompt_handler());

    // 7. Let serialized user prompt handler be serialize the user prompt handler.
    auto serialized_user_prompt_handler = Web::WebDriver::serialize_the_user_prompt_handler();

    // 8. Set a property on capabilities with the name "unhandledPromptBehavior", and the value serialized user prompt handler.
    capabilities.set("unhandledPromptBehavior"sv, move(serialized_user_prompt_handler));

    // 9. If flags contains "http":
    if (has_flag(flags, Web::WebDriver::SessionFlags::Http)) {
        // 1. Let strategy be the result of getting property "pageLoadStrategy" from capabilities. If strategy is a
        //    string, set the session's page loading strategy to strategy. Otherwise, set the page loading strategy to
        //    normal and set a property of capabilities with name "pageLoadStrategy" and value "normal".
        if (auto strategy = capabilities.get_byte_string("pageLoadStrategy"sv); strategy.has_value()) {
            session->m_page_load_strategy = Web::WebDriver::page_load_strategy_from_string(*strategy);
            session->web_content_connection().async_set_page_load_strategy(session->m_page_load_strategy);
        } else {
            capabilities.set("pageLoadStrategy"sv, "normal"sv);
        }

        // 3. Let strictFileInteractability be the result of getting property "strictFileInteractability" from .
        //    capabilities. If strictFileInteractability is a boolean, set session's strict file interactability to
        //    strictFileInteractability.
        if (auto strict_file_interactiblity = capabilities.get_bool("strictFileInteractability"sv); strict_file_interactiblity.has_value()) {
            session->m_strict_file_interactiblity = *strict_file_interactiblity;
            session->web_content_connection().async_set_strict_file_interactability(session->m_strict_file_interactiblity);
        }

        // 4. Let timeouts be the result of getting a property "timeouts" from capabilities. If timeouts is not
        //    undefined, set session's session timeouts to timeouts.
        if (auto timeouts = capabilities.get_object("timeouts"sv); timeouts.has_value()) {
            MUST(session->set_timeouts(*timeouts));
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
    session->web_content_connection().async_set_is_webdriver_active(true);

    return session;
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
    auto const& sessions = has_flag(session_flags, Web::WebDriver::SessionFlags::Http) ? s_http_sessions : s_sessions;

    if (auto session = sessions.get(session_id); session.has_value()) {
        if (allow_invalid_window_handle == AllowInvalidWindowHandle::No)
            TRY(session.value()->ensure_current_window_handle_is_valid());

        return *session.release_value();
    }

    return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::InvalidSessionId, "Invalid session id");
}

size_t Session::session_count(Web::WebDriver::SessionFlags session_flags)
{
    if (has_flag(session_flags, Web::WebDriver::SessionFlags::Http))
        return s_http_sessions.size();
    return s_sessions.size();
}

// https://w3c.github.io/webdriver/#dfn-close-the-session
void Session::close()
{
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
        for (auto& it : m_windows)
            it.value.web_content_connection->close_session();
    }
    // -> Remote end is an intermediary node
    //     1. Close the associated session. If this causes an error to occur, complete the remainder of this algorithm
    //        before returning the error.

    // 4. Perform any implementation-specific cleanup steps.
    if (m_browser_process.has_value())
        MUST(Core::System::kill(m_browser_process->pid(), SIGTERM));

    if (m_web_content_socket_path.has_value()) {
        MUST(Core::System::unlink(*m_web_content_socket_path));
        m_web_content_socket_path = {};
    }

    // 5. If an error has occurred in any of the steps above, return the error, otherwise return success with data null.
}

ErrorOr<NonnullRefPtr<Core::LocalServer>> Session::create_server(NonnullRefPtr<ServerPromise> promise)
{
    static_assert(IsSame<IPC::Transport, IPC::TransportSocket>, "Need to handle other IPC transports here");

    dbgln("Listening for WebDriver connection on {}", *m_web_content_socket_path);

    (void)Core::System::unlink(*m_web_content_socket_path);

    auto server = TRY(Core::LocalServer::try_create());
    server->listen(*m_web_content_socket_path);

    server->on_accept = [this, promise](auto client_socket) {
        auto maybe_connection = adopt_nonnull_ref_or_enomem(new (nothrow) WebContentConnection(IPC::Transport(move(client_socket))));
        if (maybe_connection.is_error()) {
            promise->resolve(maybe_connection.release_error());
            return;
        }

        dbgln("WebDriver is connected to WebContent socket");
        auto web_content_connection = maybe_connection.release_value();

        auto maybe_window_handle = web_content_connection->get_window_handle();
        if (maybe_window_handle.is_error()) {
            promise->reject(Error::from_string_literal("Window was closed immediately"));
            return;
        }

        auto const& window_handle = maybe_window_handle.value().as_string();

        web_content_connection->on_close = [this, window_handle]() {
            dbgln_if(WEBDRIVER_DEBUG, "Window {} was closed remotely.", window_handle);
            m_windows.remove(window_handle);
            if (m_windows.is_empty())
                close();
        };

        web_content_connection->async_set_page_load_strategy(m_page_load_strategy);
        web_content_connection->async_set_strict_file_interactability(m_strict_file_interactiblity);
        web_content_connection->async_set_user_prompt_handler(Web::WebDriver::user_prompt_handler());
        if (m_timeouts_configuration.has_value())
            web_content_connection->async_set_timeouts(*m_timeouts_configuration);

        m_windows.set(window_handle, Session::Window { window_handle, move(web_content_connection) });

        if (m_current_window_handle.is_empty())
            m_current_window_handle = window_handle;

        promise->resolve({});
    };

    server->on_accept_error = [promise](auto error) {
        promise->resolve(move(error));
    };

    return server;
}

ErrorOr<void> Session::start(LaunchBrowserCallbacks const& callbacks)
{
    auto promise = TRY(ServerPromise::try_create());

    m_web_content_socket_path = ByteString::formatted("{}/webdriver/session_{}_{}", TRY(Core::StandardPaths::runtime_directory()), getpid(), m_session_id);
    m_web_content_server = TRY(create_server(promise));

    if (m_options.headless)
        m_browser_process = TRY(callbacks.launch_headless_browser(*m_web_content_socket_path));
    else
        m_browser_process = TRY(callbacks.launch_browser(*m_web_content_socket_path));

    // FIXME: Allow this to be more asynchronous. For now, this at least allows us to propagate
    //        errors received while accepting the Browser and WebContent sockets.
    TRY(TRY(promise->await()));

    return {};
}

Web::WebDriver::Response Session::set_timeouts(JsonValue payload)
{
    m_timeouts_configuration = TRY(web_content_connection().set_timeouts(move(payload)));
    return JsonValue {};
}

// 11.2 Close Window, https://w3c.github.io/webdriver/#dfn-close-window
Web::WebDriver::Response Session::close_window()
{
    // 3. Close the current top-level browsing context.
    TRY(perform_async_action([&](auto& connection) {
        return connection.close_window();
    }));

    {
        // Defer removing the window handle from this session until after we know we are done with its connection.
        ScopeGuard guard { [this] { m_windows.remove(m_current_window_handle); m_current_window_handle = "NoSuchWindowPleaseSelectANewOne"_string; } };

        // 4. If there are no more open top-level browsing contexts, then close the session.
        if (m_windows.size() == 1)
            close();
    }

    // 5. Return the result of running the remote end steps for the Get Window Handles command.
    return get_window_handles();
}

// 11.3 Switch to Window, https://w3c.github.io/webdriver/#dfn-switch-to-window
Web::WebDriver::Response Session::switch_to_window(StringView handle)
{
    // 4. If handle is equal to the associated window handle for some top-level browsing context, let context be the that
    //    browsing context, and set the current top-level browsing context with session and context.
    //    Otherwise, return error with error code no such window.
    if (auto it = m_windows.find(handle); it != m_windows.end())
        m_current_window_handle = it->key;
    else
        return Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found");

    // 5. Update any implementation-specific state that would result from the user selecting the current
    //    browsing context for interaction, without altering OS-level focus.
    TRY(web_content_connection().switch_to_window(m_current_window_handle));

    // 6. Return success with data null.
    return JsonValue {};
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

}
