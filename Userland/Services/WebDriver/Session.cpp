/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Session.h"
#include "Client.h"
#include <AK/JsonObject.h>
#include <AK/ScopeGuard.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <unistd.h>

namespace WebDriver {

Session::Session(unsigned session_id, NonnullRefPtr<Client> client, Web::WebDriver::LadybirdOptions options)
    : m_client(move(client))
    , m_options(move(options))
    , m_id(session_id)
{
}

// https://w3c.github.io/webdriver/#dfn-close-the-session
Session::~Session()
{
    if (!m_started)
        return;

    // 1. Perform the following substeps based on the remote end’s type:
    // NOTE: We perform the "Remote end is an endpoint node" steps in the WebContent process.
    for (auto& it : m_windows) {
        it.value.web_content_connection->close_session();
    }

    // 2. Remove the current session from active sessions.
    // NOTE: We are in a session destruction which means it is already removed
    // from active sessions

    // 3. Perform any implementation-specific cleanup steps.
    if (m_browser_pid.has_value()) {
        MUST(Core::System::kill(*m_browser_pid, SIGTERM));
        m_browser_pid = {};
    }
    if (m_web_content_socket_path.has_value()) {
        MUST(Core::System::unlink(*m_web_content_socket_path));
        m_web_content_socket_path = {};
    }
}

ErrorOr<NonnullRefPtr<Core::LocalServer>> Session::create_server(NonnullRefPtr<ServerPromise> promise)
{
    dbgln("Listening for WebDriver connection on {}", *m_web_content_socket_path);

    (void)Core::System::unlink(*m_web_content_socket_path);

    auto server = TRY(Core::LocalServer::try_create());
    server->listen(*m_web_content_socket_path);

    server->on_accept = [this, promise](auto client_socket) {
        auto maybe_connection = adopt_nonnull_ref_or_enomem(new (nothrow) WebContentConnection(move(client_socket)));
        if (maybe_connection.is_error()) {
            promise->resolve(maybe_connection.release_error());
            return;
        }

        dbgln("WebDriver is connected to WebContent socket");
        auto web_content_connection = maybe_connection.release_value();

        auto window_handle = web_content_connection->get_window_handle();
        web_content_connection->on_close = [this, window_handle]() {
            dbgln_if(WEBDRIVER_DEBUG, "Window {} was closed remotely.", window_handle);
            m_windows.remove(window_handle);
            if (m_windows.is_empty())
                m_client->close_session(session_id());
        };
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

    m_web_content_socket_path = ByteString::formatted("{}/webdriver/session_{}_{}", TRY(Core::StandardPaths::runtime_directory()), getpid(), m_id);
    m_web_content_server = TRY(create_server(promise));

    if (m_options.headless)
        m_browser_pid = TRY(callbacks.launch_headless_browser(*m_web_content_socket_path));
    else
        m_browser_pid = TRY(callbacks.launch_browser(*m_web_content_socket_path));

    // FIXME: Allow this to be more asynchronous. For now, this at least allows us to propagate
    //        errors received while accepting the Browser and WebContent sockets.
    TRY(TRY(promise->await()));

    m_started = true;
    return {};
}

// 11.2 Close Window, https://w3c.github.io/webdriver/#dfn-close-window
Web::WebDriver::Response Session::close_window()
{
    {
        // Defer removing the window handle from this session until after we know we are done with its connection.
        ScopeGuard guard { [this] { m_windows.remove(m_current_window_handle); m_current_window_handle = "NoSuchWindowPleaseSelectANewOne"_string; } };

        // 3. Close the current top-level browsing context.
        TRY(web_content_connection().close_window());

        // 4. If there are no more open top-level browsing contexts, then close the session.
        if (m_windows.size() == 1)
            m_client->close_session(session_id());
    }

    // 5. Return the result of running the remote end steps for the Get Window Handles command.
    return get_window_handles();
}

// 11.3 Switch to Window, https://w3c.github.io/webdriver/#dfn-switch-to-window
Web::WebDriver::Response Session::switch_to_window(StringView handle)
{
    // 4. If handle is equal to the associated window handle for some top-level browsing context in the
    //    current session, let context be the that browsing context, and set the current top-level
    //    browsing context with context.
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

Web::WebDriver::Response Session::execute_script(JsonValue payload, ScriptMode mode) const
{
    ScopeGuard guard { [&]() { web_content_connection().on_script_executed = nullptr; } };

    Optional<Web::WebDriver::Response> response;
    web_content_connection().on_script_executed = [&](auto result) {
        response = move(result);
    };

    switch (mode) {
    case ScriptMode::Sync:
        TRY(web_content_connection().execute_script(move(payload)));
        break;
    case ScriptMode::Async:
        TRY(web_content_connection().execute_async_script(move(payload)));
        break;
    }

    Core::EventLoop::current().spin_until([&]() {
        return response.has_value();
    });

    return response.release_value();
}

Web::WebDriver::Response Session::perform_actions(JsonValue payload) const
{
    ScopeGuard guard { [&]() { web_content_connection().on_actions_performed = nullptr; } };

    Optional<Web::WebDriver::Response> response;
    web_content_connection().on_actions_performed = [&](auto result) {
        response = move(result);
    };

    TRY(web_content_connection().perform_actions(move(payload)));

    Core::EventLoop::current().spin_until([&]() {
        return response.has_value();
    });

    return response.release_value();
}

}
