/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibCore/EventLoop.h>
#include <LibWebView/Application.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebDriverBrowserConnection.h>
#if defined(AK_OS_MACOS)
#    include <LibIPC/TransportBootstrapMach.h>
#else
#    include <LibCore/Socket.h>
#endif

namespace WebView {

ErrorOr<NonnullRefPtr<WebDriverBrowserConnection>> WebDriverBrowserConnection::connect(ByteString const& webdriver_endpoint)
{
#if defined(AK_OS_MACOS)
    auto transport_ports = TRY(IPC::bootstrap_transport_from_mach_server(webdriver_endpoint));
    auto transport = make<IPC::Transport>(move(transport_ports.receive_right), move(transport_ports.send_right));
#else
    auto socket = TRY(Core::LocalSocket::connect(webdriver_endpoint));
    auto transport = TRY(IPC::Transport::from_socket(move(socket)));
#endif
    return adopt_nonnull_ref_or_enomem(new (nothrow) WebDriverBrowserConnection(move(transport)));
}

WebDriverBrowserConnection::WebDriverBrowserConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<WebDriverBrowserClientEndpoint, WebDriverBrowserServerEndpoint>(*this, move(transport))
{
}

void WebDriverBrowserConnection::die()
{
    Application::the().webdriver_browser_connection_died({});
}

void WebDriverBrowserConnection::close_session()
{
    Core::EventLoop::current().quit(0);
}

// 10.3 Back, https://w3c.github.io/webdriver/#dfn-back
// 10.4 Forward, https://w3c.github.io/webdriver/#dfn-forward
void WebDriverBrowserConnection::traverse_history(u64 command_id, String window_handle, i32 delta, bool handle_user_prompts)
{
    // 1. If session's current top-level browsing context is no longer open, return error with error code no such window.
    auto view = ViewImplementation::find_view_by_handle(window_handle);
    if (!view.has_value()) {
        async_command_complete(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
        return;
    }

    auto strong_this = NonnullRefPtr { *this };
    auto run_traversal = [strong_this, command_id, view_id = view->view_id(), delta]() {
        // Defer the traversal, so its cancelation checks can safely call back into the WebContent
        // process whose message dispatch we may currently be inside.
        Core::deferred_invoke([strong_this, command_id, view_id, delta]() {
            auto view = ViewImplementation::find_view_by_id(view_id);
            if (!view.has_value()) {
                strong_this->async_command_complete(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
                return;
            }

            view->traverse_the_history_by_delta(delta, CheckForCancelation::Yes, [strong_this, command_id]() {
                strong_this->async_command_complete(command_id, JsonValue {});
            });
        });
    };

    if (!handle_user_prompts) {
        run_traversal();
        return;
    }

    // 2. Try to handle any user prompts with session.
    view->run_webdriver_user_prompt_handling([strong_this, command_id, run_traversal = move(run_traversal)](Web::WebDriver::Response response) mutable {
        if (response.is_error()) {
            strong_this->async_command_complete(command_id, move(response));
            return;
        }

        run_traversal();
    });
}

void WebDriverBrowserConnection::get_session_history(u64 command_id, String window_handle)
{
    auto view = ViewImplementation::find_view_by_handle(window_handle);
    if (!view.has_value()) {
        async_command_complete(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
        return;
    }

    JsonObject result;
    result.set("ui"sv, view->webdriver_session_history());
    async_command_complete(command_id, JsonValue { move(result) });
}

}
