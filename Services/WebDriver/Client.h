/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
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

    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> new_session(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> delete_session(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_status(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_timeouts(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> set_timeouts(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> navigate_to(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_current_url(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> back(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> forward(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> refresh(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_title(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_window_handle(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> close_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> switch_to_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_window_handles(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> new_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> switch_to_frame(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> switch_to_parent_frame(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_window_rect(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> set_window_rect(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> maximize_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> minimize_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> fullscreen_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> consume_user_activation(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> find_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> find_elements(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> find_element_from_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> find_elements_from_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> find_element_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> find_elements_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_active_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_element_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> is_element_selected(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_element_attribute(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_element_property(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_element_css_value(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_element_text(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_element_tag_name(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_element_rect(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> is_element_enabled(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_computed_role(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_computed_label(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> element_click(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> element_clear(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> element_send_keys(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_source(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> execute_script(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> execute_async_script(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_all_cookies(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_named_cookie(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> add_cookie(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> delete_cookie(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> delete_all_cookies(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> perform_actions(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> release_actions(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> dismiss_alert(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> accept_alert(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> get_alert_text(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> send_alert_text(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> take_screenshot(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> take_element_screenshot(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Core::Promise<JsonValue, Web::WebDriver::Error>> print_page(Web::WebDriver::Parameters parameters, JsonValue payload) override;

    LaunchBrowserCallback m_launch_browser_callback;
};

}
