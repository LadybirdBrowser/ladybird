/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Socket.h>
#include <LibHTTP/Forward.h>
#include <LibHTTP/HttpRequest.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWeb/WebDriver/Response.h>

namespace Web::WebDriver {

using Parameters = Vector<String>;

class WEB_API Client : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(Client);

public:
    virtual ~Client();

    // 8. Sessions, https://w3c.github.io/webdriver/#sessions
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> new_session(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> delete_session(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_status(Parameters parameters, JsonValue payload) = 0;

    // 9. Timeouts, https://w3c.github.io/webdriver/#timeouts
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_timeouts(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> set_timeouts(Parameters parameters, JsonValue payload) = 0;

    // 10. Navigation, https://w3c.github.io/webdriver/#navigation
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> navigate_to(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_current_url(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> back(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> forward(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> refresh(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_title(Parameters parameters, JsonValue payload) = 0;

    // 11. Contexts, https://w3c.github.io/webdriver/#contexts
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_window_handle(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> close_window(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> new_window(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> switch_to_window(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_window_handles(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_window_rect(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> set_window_rect(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> maximize_window(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> minimize_window(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> fullscreen_window(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> switch_to_frame(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> switch_to_parent_frame(Parameters parameters, JsonValue payload) = 0;

    // Extension: https://html.spec.whatwg.org/multipage/interaction.html#user-activation-user-agent-automation
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> consume_user_activation(Parameters parameters, JsonValue payload) = 0;

    // 12. Elements, https://w3c.github.io/webdriver/#elements
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> find_element(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> find_elements(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> find_element_from_element(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> find_elements_from_element(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> find_element_from_shadow_root(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> find_elements_from_shadow_root(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_active_element(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_element_shadow_root(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> is_element_selected(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_element_attribute(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_element_property(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_element_css_value(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_element_text(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_element_tag_name(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_element_rect(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> is_element_enabled(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_computed_role(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_computed_label(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> element_click(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> element_clear(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> element_send_keys(Parameters parameters, JsonValue payload) = 0;

    // 13. Document, https://w3c.github.io/webdriver/#document
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_source(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> execute_script(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> execute_async_script(Parameters parameters, JsonValue payload) = 0;

    // 14. Cookies, https://w3c.github.io/webdriver/#cookies
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_all_cookies(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_named_cookie(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> add_cookie(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> delete_cookie(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> delete_all_cookies(Parameters parameters, JsonValue payload) = 0;

    // 15. Actions, https://w3c.github.io/webdriver/#actions
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> perform_actions(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> release_actions(Parameters parameters, JsonValue payload) = 0;

    // 16. User prompts, https://w3c.github.io/webdriver/#user-prompts
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> dismiss_alert(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> accept_alert(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> get_alert_text(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> send_alert_text(Parameters parameters, JsonValue payload) = 0;

    // 17. Screen capture, https://w3c.github.io/webdriver/#screen-capture
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> take_screenshot(Parameters parameters, JsonValue payload) = 0;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> take_element_screenshot(Parameters parameters, JsonValue payload) = 0;

    // 18. Print, https://w3c.github.io/webdriver/#print
    virtual NonnullRefPtr<Core::Promise<JsonValue, Error>> print_page(Parameters parameters, JsonValue payload) = 0;

    Function<void()> on_death;

protected:
    explicit Client(NonnullOwnPtr<Core::BufferedTCPSocket>);

private:
    using WrappedError = Variant<AK::Error, HTTP::HttpRequest::ParseError, WebDriver::Error>;

    void die();

    ErrorOr<void, WrappedError> on_ready_to_read();
    static ErrorOr<JsonValue, WrappedError> read_body_as_json(HTTP::HttpRequest const&);

    ErrorOr<NonnullRefPtr<Core::Promise<JsonValue, Error>>, WrappedError> handle_request(HTTP::HttpRequest const&, JsonValue body);
    void handle_error(HTTP::HttpRequest const&, WrappedError const&);

    ErrorOr<void, WrappedError> send_success_response(HTTP::HttpRequest const&, JsonValue result);
    ErrorOr<void, WrappedError> send_error_response(HTTP::HttpRequest const&, Error const& error);
    static void log_response(HTTP::HttpRequest const&, unsigned code);

    void process_next_pending_request();
    void dequeue_current_pending_request();

    NonnullOwnPtr<Core::BufferedTCPSocket> m_socket;
    StringBuilder m_remaining_request;

    struct PendingRequest final : public RefCounted<PendingRequest> {
        PendingRequest(HTTP::HttpRequest&& http_request)
            : http_request(move(http_request))
        {
        }

        ~PendingRequest() = default;

        HTTP::HttpRequest http_request;
    };
    Queue<NonnullRefPtr<PendingRequest>> m_pending_requests;
};

}
