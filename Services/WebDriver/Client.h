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
#include <LibWeb/WebDriver/Promise.h>

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

    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> new_session(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> delete_session(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_status(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_timeouts(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> set_timeouts(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> navigate_to(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_current_url(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> back(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> forward(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> refresh(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_title(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_window_handle(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> close_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> switch_to_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_window_handles(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> new_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> switch_to_frame(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> switch_to_parent_frame(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_window_rect(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> set_window_rect(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> maximize_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> minimize_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> fullscreen_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> consume_user_activation(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> find_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> find_elements(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> find_element_from_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> find_elements_from_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> find_element_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> find_elements_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_active_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_element_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> is_element_selected(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_element_attribute(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_element_property(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_element_css_value(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_element_text(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_element_tag_name(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_element_rect(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> is_element_enabled(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_computed_role(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_computed_label(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> element_click(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> element_clear(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> element_send_keys(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_source(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> execute_script(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> execute_async_script(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_all_cookies(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_named_cookie(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> add_cookie(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> delete_cookie(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> delete_all_cookies(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> perform_actions(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> release_actions(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> dismiss_alert(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> accept_alert(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> get_alert_text(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> send_alert_text(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> take_screenshot(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> take_element_screenshot(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual NonnullRefPtr<Web::WebDriver::Promise<JsonValue>> print_page(Web::WebDriver::Parameters parameters, JsonValue payload) override;

    LaunchBrowserCallback m_launch_browser_callback;
};

}
