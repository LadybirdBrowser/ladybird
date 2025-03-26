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

    virtual NonnullRefPtr<Messages::WebDriverClient::CloseSession::Promise> close_session() override;
    virtual void set_page_load_strategy(Web::WebDriver::PageLoadStrategy page_load_strategy) override;
    virtual void set_user_prompt_handler(Web::WebDriver::UserPromptHandler user_prompt_handler) override;
    virtual void set_strict_file_interactability(bool strict_file_interactability) override;
    virtual void set_is_webdriver_active(bool) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetTimeouts::Promise> get_timeouts() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::SetTimeouts::Promise> set_timeouts(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::NavigateTo::Promise> navigate_to(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetCurrentUrl::Promise> get_current_url() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::Back::Promise> back() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::Forward::Promise> forward() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::Refresh::Promise> refresh() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetTitle::Promise> get_title() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetWindowHandle::Promise> get_window_handle() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::CloseWindow::Promise> close_window() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::SwitchToWindow::Promise> switch_to_window(String handle) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::NewWindow::Promise> new_window(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::SwitchToFrame::Promise> switch_to_frame(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::SwitchToParentFrame::Promise> switch_to_parent_frame(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetWindowRect::Promise> get_window_rect() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::SetWindowRect::Promise> set_window_rect(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::MaximizeWindow::Promise> maximize_window() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::MinimizeWindow::Promise> minimize_window() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::FullscreenWindow::Promise> fullscreen_window() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::ConsumeUserActivation::Promise> consume_user_activation() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::FindElement::Promise> find_element(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::FindElements::Promise> find_elements(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::FindElementFromElement::Promise> find_element_from_element(JsonValue payload, String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::FindElementsFromElement::Promise> find_elements_from_element(JsonValue payload, String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::FindElementFromShadowRoot::Promise> find_element_from_shadow_root(JsonValue payload, String shadow_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::FindElementsFromShadowRoot::Promise> find_elements_from_shadow_root(JsonValue payload, String shadow_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetActiveElement::Promise> get_active_element() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetElementShadowRoot::Promise> get_element_shadow_root(String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::IsElementSelected::Promise> is_element_selected(String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetElementAttribute::Promise> get_element_attribute(String element_id, String name) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetElementProperty::Promise> get_element_property(String element_id, String name) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetElementCssValue::Promise> get_element_css_value(String element_id, String name) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetElementText::Promise> get_element_text(String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetElementTagName::Promise> get_element_tag_name(String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetElementRect::Promise> get_element_rect(String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::IsElementEnabled::Promise> is_element_enabled(String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetComputedRole::Promise> get_computed_role(String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetComputedLabel::Promise> get_computed_label(String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::ElementClick::Promise> element_click(String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::ElementClear::Promise> element_clear(String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::ElementSendKeys::Promise> element_send_keys(String element_id, JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetSource::Promise> get_source() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::ExecuteScript::Promise> execute_script(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::ExecuteAsyncScript::Promise> execute_async_script(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetAllCookies::Promise> get_all_cookies() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetNamedCookie::Promise> get_named_cookie(String name) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::AddCookie::Promise> add_cookie(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::DeleteCookie::Promise> delete_cookie(String name) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::DeleteAllCookies::Promise> delete_all_cookies() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::PerformActions::Promise> perform_actions(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::ReleaseActions::Promise> release_actions() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::DismissAlert::Promise> dismiss_alert() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::AcceptAlert::Promise> accept_alert() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::GetAlertText::Promise> get_alert_text() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::SendAlertText::Promise> send_alert_text(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::TakeScreenshot::Promise> take_screenshot() override;
    virtual NonnullRefPtr<Messages::WebDriverClient::TakeElementScreenshot::Promise> take_element_screenshot(String element_id) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::PrintPage::Promise> print_page(JsonValue payload) override;
    virtual NonnullRefPtr<Messages::WebDriverClient::EnsureTopLevelBrowsingContextIsOpen::Promise> ensure_top_level_browsing_context_is_open() override;

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
