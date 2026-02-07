/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/String.h>
#include <LibGC/RootVector.h>
#include <LibGfx/Rect.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/WebDriver/ElementLocationStrategies.h>
#include <LibWeb/WebDriver/ExecuteScript.h>
#include <LibWeb/WebDriver/Response.h>
#include <LibWeb/WebDriver/TimeoutsConfiguration.h>
#include <WebContent/Forward.h>
#include <WebContent/WebDriverClientEndpoint.h>
#include <WebContent/WebDriverServerEndpoint.h>

namespace WebContent {

class ElementLocator;

class WebDriverConnection final
    : public IPC::ConnectionToServer<WebDriverClientEndpoint, WebDriverServerEndpoint> {
    C_OBJECT_ABSTRACT(WebDriverConnection)

public:
    static ErrorOr<NonnullRefPtr<WebDriverConnection>> connect(Web::PageClient& page_client, ByteString const& webdriver_ipc_path);
    virtual ~WebDriverConnection() = default;

    void visit_edges(JS::Cell::Visitor&);

    void page_did_open_dialog(Badge<PageClient>);

private:
    WebDriverConnection(NonnullOwnPtr<IPC::Transport> transport, Web::PageClient& page_client);

    virtual void die() override { }

    virtual void close_session(int request_id) override;
    virtual void set_page_load_strategy(int request_id, Web::WebDriver::PageLoadStrategy page_load_strategy) override;
    virtual void set_user_prompt_handler(int request_id, Web::WebDriver::UserPromptHandler user_prompt_handler) override;
    virtual void set_strict_file_interactability(int request_id, bool strict_file_interactability) override;
    virtual void set_is_webdriver_active(int request_id, bool) override;
    virtual void get_timeouts(int request_id) override;
    virtual void set_timeouts(int request_id, JsonValue payload) override;
    virtual void navigate_to(int request_id, JsonValue payload) override;
    virtual void get_current_url(int request_id) override;
    virtual void back(int request_id) override;
    virtual void forward(int request_id) override;
    virtual void refresh(int request_id) override;
    virtual void get_title(int request_id) override;
    virtual void get_window_handle(int request_id) override;
    virtual void close_window(int request_id) override;
    virtual void switch_to_window(int request_id, String handle) override;
    virtual void new_window(int request_id, JsonValue payload) override;
    virtual void switch_to_frame(int request_id, JsonValue payload) override;
    virtual void switch_to_parent_frame(int request_id, JsonValue payload) override;
    virtual void get_window_rect(int request_id) override;
    virtual void set_window_rect(int request_id, JsonValue payload) override;
    virtual void maximize_window(int request_id) override;
    virtual void minimize_window(int request_id) override;
    virtual void fullscreen_window(int request_id) override;
    virtual void consume_user_activation(int request_id) override;
    virtual void find_element(int request_id, JsonValue payload) override;
    virtual void find_elements(int request_id, JsonValue payload) override;
    virtual void find_element_from_element(int request_id, JsonValue payload, String element_id) override;
    virtual void find_elements_from_element(int request_id, JsonValue payload, String element_id) override;
    virtual void find_element_from_shadow_root(int request_id, JsonValue payload, String shadow_id) override;
    virtual void find_elements_from_shadow_root(int request_id, JsonValue payload, String shadow_id) override;
    virtual void get_active_element(int request_id) override;
    virtual void get_element_shadow_root(int request_id, String element_id) override;
    virtual void is_element_selected(int request_id, String element_id) override;
    virtual void get_element_attribute(int request_id, String element_id, String name) override;
    virtual void get_element_property(int request_id, String element_id, String name) override;
    virtual void get_element_css_value(int request_id, String element_id, String name) override;
    virtual void get_element_text(int request_id, String element_id) override;
    virtual void get_element_tag_name(int request_id, String element_id) override;
    virtual void get_element_rect(int request_id, String element_id) override;
    virtual void is_element_enabled(int request_id, String element_id) override;
    virtual void get_computed_role(int request_id, String element_id) override;
    virtual void get_computed_label(int request_id, String element_id) override;
    virtual void element_click(int request_id, String element_id) override;
    virtual void element_clear(int request_id, String element_id) override;
    virtual void element_send_keys(int request_id, String element_id, JsonValue payload) override;
    virtual void get_source(int request_id) override;
    virtual void execute_script(int request_id, JsonValue payload) override;
    virtual void execute_async_script(int request_id, JsonValue payload) override;
    virtual void get_all_cookies(int request_id) override;
    virtual void get_named_cookie(int request_id, String name) override;
    virtual void add_cookie(int request_id, JsonValue payload) override;
    virtual void delete_cookie(int request_id, String name) override;
    virtual void delete_all_cookies(int request_id) override;
    virtual void perform_actions(int request_id, JsonValue payload) override;
    virtual void release_actions(int request_id) override;
    virtual void dismiss_alert(int request_id) override;
    virtual void accept_alert(int request_id) override;
    virtual void get_alert_text(int request_id) override;
    virtual void send_alert_text(int request_id, JsonValue payload) override;
    virtual void take_screenshot(int request_id) override;
    virtual void take_element_screenshot(int request_id, String element_id) override;
    virtual void print_page(int request_id, JsonValue payload) override;
    virtual void ensure_top_level_browsing_context_is_open(int request_id) override;

    void set_current_browsing_context(Web::HTML::BrowsingContext&);
    Web::HTML::BrowsingContext& current_browsing_context() { return *m_current_browsing_context; }
    GC::Ptr<Web::HTML::BrowsingContext> current_parent_browsing_context() { return m_current_parent_browsing_context; }

    void set_current_top_level_browsing_context(Web::HTML::BrowsingContext&);
    GC::Ptr<Web::HTML::BrowsingContext> current_top_level_browsing_context() { return m_current_top_level_browsing_context; }

    ErrorOr<void, Web::WebDriver::Error> ensure_current_browsing_context_is_open();
    ErrorOr<void, Web::WebDriver::Error> ensure_current_top_level_browsing_context_is_open();

    void element_click_impl(int request_id, StringView element_id);
    Web::WebDriver::Response element_clear_impl(StringView element_id);
    void element_send_keys_impl(int request_id, StringView element_id, String const& text);
    Web::WebDriver::Response add_cookie_impl(JsonObject const&);

    Web::WebDriver::PromptHandlerConfiguration get_the_prompt_handler(Web::WebDriver::PromptType type) const;
    void handle_any_user_prompts(int request_id, Function<void()> on_dialog_closed);

    void maximize_the_window(GC::Ref<GC::Function<void()>>);
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
    ErrorOr<ScriptArguments, Web::WebDriver::Error> extract_the_script_arguments_from_a_request(JS::VM&, JsonValue const& payload);
    void handle_script_response(int request_id, Web::WebDriver::ExecutionResult, GC::Ptr<GC::Function<void()>> script_interrupted_callback);

    void delete_cookies(Optional<StringView> const& name = {});

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

    Vector<GC::Ref<GC::Function<void()>>> m_pending_window_rect_requests;

    GC::Ptr<GC::Function<void()>> m_script_execution_interrupted_callback;

    friend class ElementLocator;
    GC::Ptr<ElementLocator> m_element_locator;

    GC::Ptr<JS::Cell> m_action_executor;

    GC::Ptr<Web::DOM::DocumentObserver> m_document_observer;
    GC::Ptr<Web::HTML::NavigationObserver> m_navigation_observer;
    GC::Ptr<Web::WebDriver::HeapTimer> m_navigation_timer;
};

}
