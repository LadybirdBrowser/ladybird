/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace {

// Neither WebDriver nor the test harness drives this application, so its renderers cannot ask for a change to the
// window.
class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::No;
    }
};

}

// Whether a tab is visible and has system focus decides what the user is paying attention to, so the UI process keeps
// both for itself. A renderer claiming otherwise is refused and terminated.

TEST_CASE(renderer_attention_claims)
{
    Test::HeadlessTestEnvironment environment { "TestRendererAttentionClaims"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto focus_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto& focus_client = focus_view->page().client();
    auto& focus_traversable = focus_view->traversable();
    auto has_system_focus = focus_traversable.has_system_focus();

    static_cast<WebContentClientStub&>(focus_client).did_request_set_system_focus(focus_view->page().id(), !has_system_focus);
    VERIFY(focus_traversable.has_system_focus() == has_system_focus);
    VERIFY(!focus_client.is_open());

    auto visibility_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto& visibility_client = visibility_view->page().client();
    VERIFY(&visibility_client != &focus_client);
    auto& visibility_traversable = visibility_view->traversable();
    auto visibility_state = visibility_traversable.system_visibility_state();
    auto claimed_visibility_state = visibility_state == Web::HTML::VisibilityState::Visible ? Web::HTML::VisibilityState::Hidden : Web::HTML::VisibilityState::Visible;

    static_cast<WebContentClientStub&>(visibility_client).did_request_set_system_visibility_state(visibility_view->page().id(), claimed_visibility_state);
    VERIFY(visibility_traversable.system_visibility_state() == visibility_state);
    VERIFY(!visibility_client.is_open());

    outln("PASS: renderers cannot claim system focus or visibility");
}
