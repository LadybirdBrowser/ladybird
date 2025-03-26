/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
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

namespace IPCMessages = Messages::WebDriverClient;

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
    WebDriverConnection(IPC::Transport transport, Web::PageClient& page_client);

    virtual void die() override { }

    virtual void close_session(IPCMessages::CloseSession::Resolver resolver) override;
    virtual void set_page_load_strategy(Web::WebDriver::PageLoadStrategy page_load_strategy) override;
    virtual void set_user_prompt_handler(Web::WebDriver::UserPromptHandler user_prompt_handler) override;
    virtual void set_strict_file_interactability(bool strict_file_interactability) override;
    virtual void set_is_webdriver_active(bool) override;
    virtual void get_timeouts(IPCMessages::GetTimeouts::Resolver resolver) override;
    virtual void set_timeouts(JsonValue payload, IPCMessages::SetTimeouts::Resolver resolver) override;
    virtual void navigate_to(JsonValue payload, IPCMessages::NavigateTo::Resolver resolver) override;
    virtual void get_current_url(IPCMessages::GetCurrentUrl::Resolver resolver) override;
    virtual void back(IPCMessages::Back::Resolver resolver) override;
    virtual void forward(IPCMessages::Forward::Resolver resolver) override;
    virtual void refresh(IPCMessages::Refresh::Resolver resolver) override;
    virtual void get_title(IPCMessages::GetTitle::Resolver resolver) override;
    virtual void get_window_handle(IPCMessages::GetWindowHandle::Resolver resolver) override;
    virtual void close_window(IPCMessages::CloseWindow::Resolver resolver) override;
    virtual void switch_to_window(String handle, IPCMessages::SwitchToWindow::Resolver resolver) override;
    virtual void new_window(JsonValue payload, IPCMessages::NewWindow::Resolver resolver) override;
    virtual void switch_to_frame(JsonValue payload, IPCMessages::SwitchToFrame::Resolver resolver) override;
    virtual void switch_to_parent_frame(JsonValue payload, IPCMessages::SwitchToParentFrame::Resolver resolver) override;
    virtual void get_window_rect(IPCMessages::GetWindowRect::Resolver resolver) override;
    virtual void set_window_rect(JsonValue payload, IPCMessages::SetWindowRect::Resolver resolver) override;
    virtual void maximize_window(IPCMessages::MaximizeWindow::Resolver resolver) override;
    virtual void minimize_window(IPCMessages::MinimizeWindow::Resolver resolver) override;
    virtual void fullscreen_window(IPCMessages::FullscreenWindow::Resolver resolver) override;
    virtual void consume_user_activation(IPCMessages::ConsumeUserActivation::Resolver resolver) override;
    virtual void find_element(JsonValue payload, IPCMessages::FindElement::Resolver resolver) override;
    virtual void find_elements(JsonValue payload, IPCMessages::FindElements::Resolver resolver) override;
    virtual void find_element_from_element(JsonValue payload, String element_id, IPCMessages::FindElementFromElement::Resolver resolver) override;
    virtual void find_elements_from_element(JsonValue payload, String element_id, IPCMessages::FindElementsFromElement::Resolver resolver) override;
    virtual void find_element_from_shadow_root(JsonValue payload, String shadow_id, IPCMessages::FindElementFromShadowRoot::Resolver resolver) override;
    virtual void find_elements_from_shadow_root(JsonValue payload, String shadow_id, IPCMessages::FindElementsFromShadowRoot::Resolver resolver) override;
    virtual void get_active_element(IPCMessages::GetActiveElement::Resolver resolver) override;
    virtual void get_element_shadow_root(String element_id, IPCMessages::GetElementShadowRoot::Resolver resolver) override;
    virtual void is_element_selected(String element_id, IPCMessages::IsElementSelected::Resolver resolver) override;
    virtual void get_element_attribute(String element_id, String name, IPCMessages::GetElementAttribute::Resolver resolver) override;
    virtual void get_element_property(String element_id, String name, IPCMessages::GetElementProperty::Resolver resolver) override;
    virtual void get_element_css_value(String element_id, String name, IPCMessages::GetElementCssValue::Resolver resolver) override;
    virtual void get_element_text(String element_id, IPCMessages::GetElementText::Resolver resolver) override;
    virtual void get_element_tag_name(String element_id, IPCMessages::GetElementTagName::Resolver resolver) override;
    virtual void get_element_rect(String element_id, IPCMessages::GetElementRect::Resolver resolver) override;
    virtual void is_element_enabled(String element_id, IPCMessages::IsElementEnabled::Resolver resolver) override;
    virtual void get_computed_role(String element_id, IPCMessages::GetComputedRole::Resolver resolver) override;
    virtual void get_computed_label(String element_id, IPCMessages::GetComputedLabel::Resolver resolver) override;
    virtual void element_click(String element_id, IPCMessages::ElementClick::Resolver resolver) override;
    virtual void element_clear(String element_id, IPCMessages::ElementClear::Resolver resolver) override;
    virtual void element_send_keys(String element_id, JsonValue payload, IPCMessages::ElementSendKeys::Resolver resolver) override;
    virtual void get_source(IPCMessages::GetSource::Resolver resolver) override;
    virtual void execute_script(JsonValue payload, IPCMessages::ExecuteScript::Resolver resolver) override;
    virtual void execute_async_script(JsonValue payload, IPCMessages::ExecuteAsyncScript::Resolver resolver) override;
    virtual void get_all_cookies(IPCMessages::GetAllCookies::Resolver resolver) override;
    virtual void get_named_cookie(String name, IPCMessages::GetNamedCookie::Resolver resolver) override;
    virtual void add_cookie(JsonValue payload, IPCMessages::AddCookie::Resolver resolver) override;
    virtual void delete_cookie(String name, IPCMessages::DeleteCookie::Resolver resolver) override;
    virtual void delete_all_cookies(IPCMessages::DeleteAllCookies::Resolver resolver) override;
    virtual void perform_actions(JsonValue payload, IPCMessages::PerformActions::Resolver resolver) override;
    virtual void release_actions(IPCMessages::ReleaseActions::Resolver resolver) override;
    virtual void dismiss_alert(IPCMessages::DismissAlert::Resolver resolver) override;
    virtual void accept_alert(IPCMessages::AcceptAlert::Resolver resolver) override;
    virtual void get_alert_text(IPCMessages::GetAlertText::Resolver resolver) override;
    virtual void send_alert_text(JsonValue payload, IPCMessages::SendAlertText::Resolver resolver) override;
    virtual void take_screenshot(IPCMessages::TakeScreenshot::Resolver resolver) override;
    virtual void take_element_screenshot(String element_id, IPCMessages::TakeElementScreenshot::Resolver resolver) override;
    virtual void print_page(JsonValue payload, IPCMessages::PrintPage::Resolver resolver) override;
    virtual void ensure_top_level_browsing_context_is_open(IPCMessages::EnsureTopLevelBrowsingContextIsOpen::Resolver resolver) override;


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

    Web::WebDriver::PromptHandlerConfiguration get_the_prompt_handler(Web::WebDriver::PromptType type) const;
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
    ErrorOr<ScriptArguments, Web::WebDriver::Error> extract_the_script_arguments_from_a_request(JS::VM&, JsonValue const& payload);
    void handle_script_response(Web::WebDriver::ExecutionResult, size_t script_execution_id);

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

    size_t m_pending_window_rect_requests { 0 };

    size_t m_script_execution_id_counter { 0 };
    Optional<size_t> m_current_script_execution_id;

    friend class ElementLocator;
    GC::Ptr<ElementLocator> m_element_locator;

    GC::Ptr<JS::Cell> m_action_executor;

    GC::Ptr<Web::DOM::DocumentObserver> m_document_observer;
    GC::Ptr<Web::HTML::NavigationObserver> m_navigation_observer;
    GC::Ptr<Web::WebDriver::HeapTimer> m_navigation_timer;
};

}
