/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/String.h>
#include <LibGC/RootVector.h>
#include <LibGfx/Rect.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/WebDriver/Capabilities.h>
#include <LibWeb/WebDriver/ElementLocationStrategies.h>
#include <LibWeb/WebDriver/ExecuteScript.h>
#include <LibWeb/WebDriver/Response.h>
#include <LibWeb/WebDriver/TimeoutsConfiguration.h>
#include <WebContent/Forward.h>

namespace WebContent {

class ElementLocator;

class WebDriverConnection final : public RefCounted<WebDriverConnection> {
public:
    static NonnullRefPtr<WebDriverConnection> create(PageClient&);
    ~WebDriverConnection() = default;

    void visit_edges(JS::Cell::Visitor&);

    void page_did_open_dialog(Badge<PageClient>);

    void run_command(u64 command_id, String const& name, JsonValue payload, Vector<String> arguments);
    void set_session_config(Web::WebDriver::PageLoadStrategy, bool strict_file_interactability, JsonValue const& timeouts);

private:
    explicit WebDriverConnection(PageClient&);

    void driver_execution_complete(Web::WebDriver::Response);

    void set_current_browsing_context_to_top_level();
    Web::WebDriver::Response get_current_url();
    Web::WebDriver::Response get_title();
    Web::WebDriver::Response close_window();
    Web::WebDriver::Response switch_to_window(String handle);
    Web::WebDriver::Response new_window(JsonValue payload);
    Web::WebDriver::Response switch_to_frame(JsonValue payload);
    Web::WebDriver::Response switch_to_parent_frame(JsonValue payload);
    Web::WebDriver::Response get_window_rect();
    Web::WebDriver::Response set_window_rect(JsonValue payload);
    Web::WebDriver::Response maximize_window();
    Web::WebDriver::Response minimize_window();
    Web::WebDriver::Response fullscreen_window();
    Web::WebDriver::Response consume_user_activation();
    void crash_current_page();
    Web::WebDriver::Response find_element(JsonValue payload);
    Web::WebDriver::Response find_elements(JsonValue payload);
    Web::WebDriver::Response find_element_from_element(JsonValue payload, String element_id);
    Web::WebDriver::Response find_elements_from_element(JsonValue payload, String element_id);
    Web::WebDriver::Response find_element_from_shadow_root(JsonValue payload, String shadow_id);
    Web::WebDriver::Response find_elements_from_shadow_root(JsonValue payload, String shadow_id);
    Web::WebDriver::Response get_active_element();
    Web::WebDriver::Response get_element_shadow_root(String element_id);
    Web::WebDriver::Response is_element_selected(String element_id);
    Web::WebDriver::Response get_element_attribute(String element_id, String name);
    Web::WebDriver::Response get_element_property(String element_id, String name);
    Web::WebDriver::Response get_element_css_value(String element_id, String name);
    Web::WebDriver::Response get_element_text(String element_id);
    Web::WebDriver::Response get_element_tag_name(String element_id);
    Web::WebDriver::Response get_element_rect(String element_id);
    Web::WebDriver::Response is_element_enabled(String element_id);
    Web::WebDriver::Response get_computed_role(String element_id);
    Web::WebDriver::Response get_computed_label(String element_id);
    Web::WebDriver::Response element_click(String element_id);
    Web::WebDriver::Response element_clear(String element_id);
    Web::WebDriver::Response element_send_keys(String element_id, JsonValue payload);
    Web::WebDriver::Response get_source();
    Web::WebDriver::Response execute_script(JsonValue payload);
    Web::WebDriver::Response execute_async_script(JsonValue payload);
    Web::WebDriver::Response get_all_cookies();
    Web::WebDriver::Response get_named_cookie(String name);
    Web::WebDriver::Response add_cookie(JsonValue payload);
    Web::WebDriver::Response delete_cookie(String name);
    Web::WebDriver::Response delete_all_cookies();
    Web::WebDriver::Response perform_actions(JsonValue payload);
    Web::WebDriver::Response release_actions();
    Web::WebDriver::Response dismiss_alert();
    Web::WebDriver::Response accept_alert();
    Web::WebDriver::Response get_alert_text();
    Web::WebDriver::Response send_alert_text(JsonValue payload);
    Web::WebDriver::Response take_screenshot();
    Web::WebDriver::Response take_element_screenshot(String element_id);
    Web::WebDriver::Response print_page(JsonValue payload);
    Web::WebDriver::Response ensure_top_level_browsing_context_is_open();

    void set_current_browsing_context(Web::HTML::BrowsingContext&);
    Web::HTML::BrowsingContext& current_browsing_context() { return *m_current_browsing_context; }
    GC::Ptr<Web::HTML::BrowsingContext> current_parent_browsing_context() { return m_current_parent_browsing_context; }

    void set_current_top_level_browsing_context(Web::HTML::BrowsingContext&);
    GC::Ptr<Web::HTML::BrowsingContext> current_top_level_browsing_context() { return m_current_top_level_browsing_context; }

    ErrorOr<void, Web::WebDriver::Error> ensure_current_browsing_context_is_open();
    ErrorOr<void, Web::WebDriver::Error> ensure_current_top_level_browsing_context_is_open();

    Web::WebDriver::Response element_click_impl(StringView element_id);
    Web::WebDriver::Response element_clear_impl(StringView element_id);
    Web::WebDriver::Response element_send_keys_impl(StringView element_id, String const& text);
    Web::WebDriver::Response add_cookie_impl(JsonObject const&);

    void handle_any_user_prompts(Function<void()> on_dialog_closed);

    void maximize_the_window();
    void iconify_the_window(GC::Ref<GC::Function<void()>>);
    void restore_the_window(GC::Ref<GC::Function<void()>>);
    void wait_for_visibility_state(GC::Ref<GC::Function<void()>>, Web::HTML::VisibilityState);

    using OnNavigationComplete = GC::Ref<GC::Function<void(Web::WebDriver::Response)>>;
    void wait_for_navigation_to_complete(OnNavigationComplete);

    Gfx::IntPoint calculate_absolute_position_of_element(Web::CSSPixelRect);
    Gfx::IntRect calculate_absolute_rect_of_element(Web::DOM::Element const& element);

    using GetStartNode = GC::Ref<GC::Function<ErrorOr<GC::Ref<Web::DOM::ParentNode>, Web::WebDriver::Error>()>>;
    using OnFindComplete = GC::Ref<GC::Function<void(Web::WebDriver::Response)>>;
    void find(Web::WebDriver::LocationStrategy, String, GetStartNode, OnFindComplete);

    struct ScriptArguments {
        String script;
        GC::RootVector<JS::Value> arguments;
    };
    ErrorOr<ScriptArguments, Web::WebDriver::Error> extract_the_script_arguments_from_a_request(JsonValue const& payload);
    void handle_script_response(Web::WebDriver::ExecutionResult, size_t script_execution_id);

    void delete_cookies(Optional<StringView> const& name = {});

    GC::Ref<PageClient> m_page_client;

    Optional<u64> m_current_command_id;

    // https://w3c.github.io/webdriver/#dfn-page-load-strategy
    Web::WebDriver::PageLoadStrategy m_page_load_strategy { Web::WebDriver::PageLoadStrategy::Normal };

    // https://w3c.github.io/webdriver/#dfn-strict-file-interactability
    bool m_strict_file_interactability { false };

    // https://w3c.github.io/webdriver/#dfn-session-script-timeout
    Web::WebDriver::TimeoutsConfiguration m_timeouts_configuration;

    // https://w3c.github.io/webdriver/#dfn-current-browsing-context
    GC::Ptr<Web::HTML::BrowsingContext> m_current_browsing_context;

    // https://w3c.github.io/webdriver/#dfn-current-parent-browsing-context
    GC::Ptr<Web::HTML::BrowsingContext> m_current_parent_browsing_context;

    // https://w3c.github.io/webdriver/#dfn-current-top-level-browsing-context
    GC::Ptr<Web::HTML::BrowsingContext> m_current_top_level_browsing_context;

    size_t m_pending_window_rect_requests { 0 };

    size_t m_script_execution_id_counter { 0 };
    Optional<size_t> m_current_script_execution_id;

    friend class ElementLocator;
    GC::Ptr<ElementLocator> m_element_locator;

    GC::Ptr<JS::Cell> m_action_executor;

    GC::Ptr<Web::DOM::DocumentObserver> m_document_observer;
    GC::Ptr<Web::HTML::NavigationObserver> m_navigation_observer;
    GC::Ptr<GC::Timer> m_navigation_timer;
};

}
