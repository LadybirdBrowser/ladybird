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

// 10.1 Navigate To, https://w3c.github.io/webdriver/#navigate-to
void WebDriverBrowserConnection::navigate_to(u64 command_id, String window_handle, URL::URL url)
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    auto view = ViewImplementation::find_view_by_handle(window_handle);
    if (!view.has_value()) {
        async_command_complete(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
        return;
    }

    // 4. Handle any user prompts and return its value if it is an error.
    auto strong_this = NonnullRefPtr { *this };
    view->run_webdriver_user_prompt_handling([strong_this, command_id, view_id = view->view_id(), url = move(url)](Web::WebDriver::Response response) {
        if (response.is_error()) {
            strong_this->async_command_complete(command_id, move(response));
            return;
        }

        auto view = ViewImplementation::find_view_by_id(view_id);
        if (!view.has_value()) {
            strong_this->async_command_complete(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
            return;
        }

        // 5. Let current URL be the current top-level browsing context’s active document’s URL.
        // NB: The URL replica is ordered after every navigation report WebContent sent before it
        //     answered the user prompt job, so it reflects the document state this command observed.
        auto const& current_url = view->url();

        // FIXME: 6. If current URL and url do not have the same absolute URL:
        // FIXME:     a. If timer has not been started, start a timer. If this algorithm has not completed before timer reaches the session’s session page load timeout in milliseconds, return an error with error code timeout.

        // 7. Navigate the current top-level browsing context to url.
        // NB: "Navigate to a javascript: URL" can evaluate without producing a new Document,
        //     in which case "we will not perform a navigation".
        // https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate-to-a-javascript:-url
        auto is_same_document_fragment_navigation = url.fragment().has_value()
            && url.equals(current_url, URL::ExcludeFragment::Yes);
        if (url.scheme() != "javascript"sv && !is_same_document_fragment_navigation)
            view->did_start_webdriver_navigation();
        view->load_for_webdriver_navigation(url);

        // FIXME: 10. If the current top-level browsing context contains a refresh state pragma directive of time 1 second or less, wait until the refresh timeout has elapsed, a new navigate has begun, and return to the first step of this algorithm.

        // 11. Return success with data null.
        strong_this->async_command_complete(command_id, JsonValue {});
    });
}

// 10.5 Refresh, https://w3c.github.io/webdriver/#dfn-refresh
void WebDriverBrowserConnection::refresh(u64 command_id, String window_handle)
{
    // 1. If the current top-level browsing context is no longer open, return error with error code no such window.
    auto view = ViewImplementation::find_view_by_handle(window_handle);
    if (!view.has_value()) {
        async_command_complete(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
        return;
    }

    // 2. Handle any user prompts and return its value if it is an error.
    auto strong_this = NonnullRefPtr { *this };
    view->run_webdriver_user_prompt_handling([strong_this, command_id, view_id = view->view_id()](Web::WebDriver::Response response) {
        if (response.is_error()) {
            strong_this->async_command_complete(command_id, move(response));
            return;
        }

        auto view = ViewImplementation::find_view_by_id(view_id);
        if (!view.has_value()) {
            strong_this->async_command_complete(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
            return;
        }

        // 3. Initiate an overridden reload of the current top-level browsing context’s active document.
        view->did_start_webdriver_navigation();
        view->reload();

        // FIXME: 4. If url is special except for file:
        // FIXME:     1. Try to wait for navigation to complete.
        // FIXME:     2. Try to run the post-navigation checks.

        // 6. Return success with data null.
        strong_this->async_command_complete(command_id, JsonValue {});
    });
}

void WebDriverBrowserConnection::wait_for_navigation_completion(u64 command_id, String window_handle, Optional<u64> page_load_timeout)
{
    auto view = ViewImplementation::find_view_by_handle(window_handle);
    if (!view.has_value()) {
        async_command_complete(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
        return;
    }

    auto strong_this = NonnullRefPtr { *this };
    view->wait_for_webdriver_navigation_completion(page_load_timeout, [strong_this, command_id](Web::WebDriver::Response response) {
        strong_this->async_command_complete(command_id, move(response));
    });
}

void WebDriverBrowserConnection::load_url(u64 command_id, String window_handle, URL::URL url)
{
    auto view = ViewImplementation::find_view_by_handle(window_handle);
    if (!view.has_value()) {
        async_command_complete(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
        return;
    }

    if (url.scheme() != "javascript"sv)
        view->did_start_webdriver_navigation();

    auto strong_this = NonnullRefPtr { *this };
    Core::deferred_invoke([strong_this, command_id, view_id = view->view_id(), url = move(url)]() {
        auto view = ViewImplementation::find_view_by_id(view_id);
        if (!view.has_value()) {
            strong_this->async_command_complete(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
            return;
        }

        view->load(url);
        strong_this->async_command_complete(command_id, JsonValue {});
    });
}

void WebDriverBrowserConnection::run_content_command(u64 command_id, String window_handle, String name, JsonValue payload, Vector<String> arguments)
{
    auto view = ViewImplementation::find_view_by_handle(window_handle);
    if (!view.has_value()) {
        async_command_complete(command_id, Web::WebDriver::Error::from_code(Web::WebDriver::ErrorCode::NoSuchWindow, "Window not found"sv));
        return;
    }

    view->run_webdriver_content_command(command_id, name, move(payload), move(arguments));
}

void WebDriverBrowserConnection::set_user_prompt_handler(Web::WebDriver::UserPromptHandler user_prompt_handler)
{
    Application::the().update_webdriver_session_config({}, [user_prompt_handler = move(user_prompt_handler)](auto& config) {
        config.user_prompt_handler = user_prompt_handler;
    });
}

void WebDriverBrowserConnection::set_page_load_strategy(Web::WebDriver::PageLoadStrategy page_load_strategy)
{
    Application::the().update_webdriver_session_config({}, [page_load_strategy](auto& config) {
        config.page_load_strategy = page_load_strategy;
    });
}

void WebDriverBrowserConnection::set_strict_file_interactability(bool strict_file_interactability)
{
    Application::the().update_webdriver_session_config({}, [strict_file_interactability](auto& config) {
        config.strict_file_interactability = strict_file_interactability;
    });
}

void WebDriverBrowserConnection::set_timeouts_configuration(JsonValue timeouts)
{
    Application::the().update_webdriver_session_config({}, [timeouts = move(timeouts)](auto& config) {
        config.timeouts = timeouts;
    });
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
