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
#include <LibWeb/WebDriver/Error.h>
#include <LibWeb/WebDriver/Response.h>

namespace Web::WebDriver {

using Parameters = Vector<String>;

class Client : public Core::EventReceiver {
    C_OBJECT_ABSTRACT(Client);

public:
    virtual ~Client();

    // 8. Sessions, https://w3c.github.io/webdriver/#sessions
    virtual Response new_session(Parameters parameters, JsonValue payload) = 0;
    virtual Response delete_session(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_status(Parameters parameters, JsonValue payload) = 0;

    // 9. Timeouts, https://w3c.github.io/webdriver/#timeouts
    virtual Response get_timeouts(Parameters parameters, JsonValue payload) = 0;
    virtual Response set_timeouts(Parameters parameters, JsonValue payload) = 0;

    // 10. Navigation, https://w3c.github.io/webdriver/#navigation
    virtual Response navigate_to(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_current_url(Parameters parameters, JsonValue payload) = 0;
    virtual Response back(Parameters parameters, JsonValue payload) = 0;
    virtual Response forward(Parameters parameters, JsonValue payload) = 0;
    virtual Response refresh(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_title(Parameters parameters, JsonValue payload) = 0;

    // 11. Contexts, https://w3c.github.io/webdriver/#contexts
    virtual Response get_window_handle(Parameters parameters, JsonValue payload) = 0;
    virtual Response close_window(Parameters parameters, JsonValue payload) = 0;
    virtual Response new_window(Parameters parameters, JsonValue payload) = 0;
    virtual Response switch_to_window(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_window_handles(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_window_rect(Parameters parameters, JsonValue payload) = 0;
    virtual Response set_window_rect(Parameters parameters, JsonValue payload) = 0;
    virtual Response maximize_window(Parameters parameters, JsonValue payload) = 0;
    virtual Response minimize_window(Parameters parameters, JsonValue payload) = 0;
    virtual Response fullscreen_window(Parameters parameters, JsonValue payload) = 0;
    virtual Response switch_to_frame(Parameters parameters, JsonValue payload) = 0;
    virtual Response switch_to_parent_frame(Parameters parameters, JsonValue payload) = 0;

    // Extension: https://html.spec.whatwg.org/multipage/interaction.html#user-activation-user-agent-automation
    virtual Response consume_user_activation(Parameters parameters, JsonValue payload) = 0;

    // 12. Elements, https://w3c.github.io/webdriver/#elements
    virtual Response find_element(Parameters parameters, JsonValue payload) = 0;
    virtual Response find_elements(Parameters parameters, JsonValue payload) = 0;
    virtual Response find_element_from_element(Parameters parameters, JsonValue payload) = 0;
    virtual Response find_elements_from_element(Parameters parameters, JsonValue payload) = 0;
    virtual Response find_element_from_shadow_root(Parameters parameters, JsonValue payload) = 0;
    virtual Response find_elements_from_shadow_root(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_active_element(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_element_shadow_root(Parameters parameters, JsonValue payload) = 0;
    virtual Response is_element_selected(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_element_attribute(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_element_property(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_element_css_value(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_element_text(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_element_tag_name(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_element_rect(Parameters parameters, JsonValue payload) = 0;
    virtual Response is_element_enabled(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_computed_role(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_computed_label(Parameters parameters, JsonValue payload) = 0;
    virtual Response element_click(Parameters parameters, JsonValue payload) = 0;
    virtual Response element_clear(Parameters parameters, JsonValue payload) = 0;
    virtual Response element_send_keys(Parameters parameters, JsonValue payload) = 0;

    // 13. Document, https://w3c.github.io/webdriver/#document
    virtual Response get_source(Parameters parameters, JsonValue payload) = 0;
    virtual Response execute_script(Parameters parameters, JsonValue payload) = 0;
    virtual Response execute_async_script(Parameters parameters, JsonValue payload) = 0;

    // 14. Cookies, https://w3c.github.io/webdriver/#cookies
    virtual Response get_all_cookies(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_named_cookie(Parameters parameters, JsonValue payload) = 0;
    virtual Response add_cookie(Parameters parameters, JsonValue payload) = 0;
    virtual Response delete_cookie(Parameters parameters, JsonValue payload) = 0;
    virtual Response delete_all_cookies(Parameters parameters, JsonValue payload) = 0;

    // 15. Actions, https://w3c.github.io/webdriver/#actions
    virtual Response perform_actions(Parameters parameters, JsonValue payload) = 0;
    virtual Response release_actions(Parameters parameters, JsonValue payload) = 0;

    // 16. User prompts, https://w3c.github.io/webdriver/#user-prompts
    virtual Response dismiss_alert(Parameters parameters, JsonValue payload) = 0;
    virtual Response accept_alert(Parameters parameters, JsonValue payload) = 0;
    virtual Response get_alert_text(Parameters parameters, JsonValue payload) = 0;
    virtual Response send_alert_text(Parameters parameters, JsonValue payload) = 0;

    // 17. Screen capture, https://w3c.github.io/webdriver/#screen-capture
    virtual Response take_screenshot(Parameters parameters, JsonValue payload) = 0;
    virtual Response take_element_screenshot(Parameters parameters, JsonValue payload) = 0;

    // 18. Print, https://w3c.github.io/webdriver/#print
    virtual Response print_page(Parameters parameters, JsonValue payload) = 0;

protected:
    Client(NonnullOwnPtr<Core::BufferedTCPSocket>, Core::EventReceiver* parent);

private:
    using WrappedError = Variant<AK::Error, HTTP::HttpRequest::ParseError, WebDriver::Error>;

    void die();

    ErrorOr<void, WrappedError> on_ready_to_read();
    static ErrorOr<JsonValue, WrappedError> read_body_as_json(HTTP::HttpRequest const&);

    ErrorOr<void, WrappedError> handle_request(HTTP::HttpRequest const&, JsonValue body);
    void handle_error(HTTP::HttpRequest const&, WrappedError const&);

    ErrorOr<void, WrappedError> send_success_response(HTTP::HttpRequest const&, JsonValue result);
    ErrorOr<void, WrappedError> send_error_response(HTTP::HttpRequest const&, Error const& error);
    static void log_response(HTTP::HttpRequest const&, unsigned code);

    NonnullOwnPtr<Core::BufferedTCPSocket> m_socket;
    StringBuilder m_remaining_request;
};

}
