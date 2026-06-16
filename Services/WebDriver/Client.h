/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Process.h>
#include <LibWeb/WebDriver/Client.h>
#include <LibWeb/WebDriver/Response.h>

namespace WebDriver {

using LaunchBrowserCallback = Function<ErrorOr<Core::Process>(ByteString const& socket_path, bool headless)>;

class Client final : public Web::WebDriver::Client {
    C_OBJECT_ABSTRACT(Client);

public:
    static ErrorOr<NonnullRefPtr<Client>> try_create(NonnullOwnPtr<Core::BufferedTCPSocket>, LaunchBrowserCallback);
    virtual ~Client() override;

    LaunchBrowserCallback const& launch_browser_callback() const { return m_launch_browser_callback; }

private:
    Client(NonnullOwnPtr<Core::BufferedTCPSocket>, LaunchBrowserCallback);

    virtual ResponsePromise enqueue_session_request(StringView session_id, SessionRequestHandler) override;
    virtual ResponsePromise new_session(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise delete_session(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_status(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_timeouts(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise set_timeouts(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise navigate_to(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_current_url(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise back(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise forward(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise refresh(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_title(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_window_handle(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise close_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise switch_to_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_window_handles(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise new_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise switch_to_frame(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise switch_to_parent_frame(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_window_rect(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise set_window_rect(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise maximize_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise minimize_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise fullscreen_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise consume_user_activation(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise crash_current_page(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise load_url_from_ui(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise traverse_history_from_ui(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_session_history(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise find_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise find_elements(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise find_element_from_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise find_elements_from_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise find_element_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise find_elements_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_active_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_element_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise is_element_selected(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_element_attribute(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_element_property(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_element_css_value(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_element_text(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_element_tag_name(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_element_rect(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise is_element_enabled(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_computed_role(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_computed_label(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise element_click(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise element_clear(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise element_send_keys(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_source(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise execute_script(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise execute_async_script(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_all_cookies(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_named_cookie(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise add_cookie(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise delete_cookie(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise delete_all_cookies(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise perform_actions(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise release_actions(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise dismiss_alert(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise accept_alert(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise get_alert_text(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise send_alert_text(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise take_screenshot(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise take_element_screenshot(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual ResponsePromise print_page(Web::WebDriver::Parameters parameters, JsonValue payload) override;

    LaunchBrowserCallback m_launch_browser_callback;
};

}
