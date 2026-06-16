/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Queue.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Promise.h>
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
    using ResponsePromise = NonnullRefPtr<Core::Promise<JsonValue, Error>>;

    virtual ~Client();

    // 8. Sessions, https://w3c.github.io/webdriver/#sessions
    virtual ResponsePromise new_session(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise delete_session(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_status(Parameters parameters, JsonValue payload) = 0;

    // 9. Timeouts, https://w3c.github.io/webdriver/#timeouts
    virtual ResponsePromise get_timeouts(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise set_timeouts(Parameters parameters, JsonValue payload) = 0;

    // 10. Navigation, https://w3c.github.io/webdriver/#navigation
    virtual ResponsePromise navigate_to(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_current_url(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise back(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise forward(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise refresh(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_title(Parameters parameters, JsonValue payload) = 0;

    // 11. Contexts, https://w3c.github.io/webdriver/#contexts
    virtual ResponsePromise get_window_handle(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise close_window(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise new_window(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise switch_to_window(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_window_handles(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_window_rect(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise set_window_rect(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise maximize_window(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise minimize_window(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise fullscreen_window(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise switch_to_frame(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise switch_to_parent_frame(Parameters parameters, JsonValue payload) = 0;

    // Extension: https://html.spec.whatwg.org/multipage/interaction.html#user-activation-user-agent-automation
    virtual ResponsePromise consume_user_activation(Parameters parameters, JsonValue payload) = 0;

    // Ladybird extension for browser integration tests.
    virtual ResponsePromise crash_current_page(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise load_url_from_ui(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise traverse_history_from_ui(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_session_history(Parameters parameters, JsonValue payload) = 0;

    // 12. Elements, https://w3c.github.io/webdriver/#elements
    virtual ResponsePromise find_element(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise find_elements(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise find_element_from_element(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise find_elements_from_element(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise find_element_from_shadow_root(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise find_elements_from_shadow_root(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_active_element(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_element_shadow_root(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise is_element_selected(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_element_attribute(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_element_property(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_element_css_value(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_element_text(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_element_tag_name(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_element_rect(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise is_element_enabled(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_computed_role(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_computed_label(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise element_click(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise element_clear(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise element_send_keys(Parameters parameters, JsonValue payload) = 0;

    // 13. Document, https://w3c.github.io/webdriver/#document
    virtual ResponsePromise get_source(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise execute_script(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise execute_async_script(Parameters parameters, JsonValue payload) = 0;

    // 14. Cookies, https://w3c.github.io/webdriver/#cookies
    virtual ResponsePromise get_all_cookies(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_named_cookie(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise add_cookie(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise delete_cookie(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise delete_all_cookies(Parameters parameters, JsonValue payload) = 0;

    // 15. Actions, https://w3c.github.io/webdriver/#actions
    virtual ResponsePromise perform_actions(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise release_actions(Parameters parameters, JsonValue payload) = 0;

    // 16. User prompts, https://w3c.github.io/webdriver/#user-prompts
    virtual ResponsePromise dismiss_alert(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise accept_alert(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise get_alert_text(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise send_alert_text(Parameters parameters, JsonValue payload) = 0;

    // 17. Screen capture, https://w3c.github.io/webdriver/#screen-capture
    virtual ResponsePromise take_screenshot(Parameters parameters, JsonValue payload) = 0;
    virtual ResponsePromise take_element_screenshot(Parameters parameters, JsonValue payload) = 0;

    // 18. Print, https://w3c.github.io/webdriver/#print
    virtual ResponsePromise print_page(Parameters parameters, JsonValue payload) = 0;

    Function<void()> on_death;

protected:
    using SessionRequestHandler = Function<ResponsePromise()>;

    explicit Client(NonnullOwnPtr<Core::BufferedTCPSocket>);
    virtual ResponsePromise enqueue_session_request(StringView session_id, SessionRequestHandler) = 0;

private:
    using WrappedError = Variant<AK::Error, HTTP::HttpRequest::ParseError, WebDriver::Error>;

    void die();

    ErrorOr<void, WrappedError> on_ready_to_read();
    static ErrorOr<JsonValue, WrappedError> read_body_as_json(HTTP::HttpRequest const&);

    ErrorOr<ResponsePromise, WrappedError> handle_request(HTTP::HttpRequest const&, JsonValue body);
    void handle_error(HTTP::HttpRequest const&, WrappedError const&);

    ErrorOr<void, WrappedError> send_success_response(HTTP::HttpRequest const&, JsonValue result);
    ErrorOr<void, WrappedError> send_error_response(HTTP::HttpRequest const&, Error const& error);
    static void log_response(HTTP::HttpRequest const&, unsigned code);

    void process_next_pending_request();
    void dequeue_current_pending_request();

    NonnullOwnPtr<Core::BufferedTCPSocket> m_socket;
    StringBuilder m_remaining_request;
    bool m_is_dying { false };

    struct PendingRequest final : public RefCounted<PendingRequest> {
        explicit PendingRequest(HTTP::HttpRequest&& http_request)
            : http_request(move(http_request))
        {
        }

        HTTP::HttpRequest http_request;
    };
    Queue<NonnullRefPtr<PendingRequest>> m_pending_requests;
};

}
