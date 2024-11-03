/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/EventReceiver.h>
#include <LibWeb/WebDriver/Client.h>
#include <LibWeb/WebDriver/Error.h>
#include <LibWeb/WebDriver/Response.h>
#include <WebDriver/Session.h>

namespace WebDriver {

struct LaunchBrowserCallbacks {
    Function<ErrorOr<pid_t>(ByteString const&)> launch_browser;
    Function<ErrorOr<pid_t>(ByteString const&)> launch_headless_browser;
};

class Client final : public Web::WebDriver::Client {
    C_OBJECT_ABSTRACT(Client);

public:
    static ErrorOr<NonnullRefPtr<Client>> try_create(NonnullOwnPtr<Core::BufferedTCPSocket>, LaunchBrowserCallbacks, Core::EventReceiver* parent);
    virtual ~Client() override;

    void close_session(unsigned session_id);

private:
    Client(NonnullOwnPtr<Core::BufferedTCPSocket>, LaunchBrowserCallbacks, Core::EventReceiver* parent);

    enum class AllowInvalidWindowHandle {
        No,
        Yes,
    };
    ErrorOr<NonnullRefPtr<Session>, Web::WebDriver::Error> find_session_with_id(StringView session_id, AllowInvalidWindowHandle = AllowInvalidWindowHandle::No);

    virtual Web::WebDriver::Response new_session(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response delete_session(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_status(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_timeouts(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response set_timeouts(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response navigate_to(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_current_url(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response back(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response forward(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response refresh(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_title(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_window_handle(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response close_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response switch_to_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_window_handles(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response new_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response switch_to_frame(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response switch_to_parent_frame(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_window_rect(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response set_window_rect(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response maximize_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response minimize_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response fullscreen_window(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response consume_user_activation(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response find_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response find_elements(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response find_element_from_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response find_elements_from_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response find_element_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response find_elements_from_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_active_element(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_element_shadow_root(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response is_element_selected(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_element_attribute(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_element_property(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_element_css_value(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_element_text(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_element_tag_name(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_element_rect(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response is_element_enabled(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_computed_role(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_computed_label(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response element_click(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response element_clear(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response element_send_keys(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_source(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response execute_script(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response execute_async_script(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_all_cookies(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_named_cookie(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response add_cookie(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response delete_cookie(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response delete_all_cookies(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response perform_actions(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response release_actions(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response dismiss_alert(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response accept_alert(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response get_alert_text(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response send_alert_text(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response take_screenshot(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response take_element_screenshot(Web::WebDriver::Parameters parameters, JsonValue payload) override;
    virtual Web::WebDriver::Response print_page(Web::WebDriver::Parameters parameters, JsonValue payload) override;

    static HashMap<unsigned, NonnullRefPtr<Session>> s_sessions;
    static Atomic<unsigned> s_next_session_id;

    LaunchBrowserCallbacks m_callbacks;
};

}
