/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/AnyOf.h>
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/PermissionsPolicy/PermissionsPolicy.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/GamepadManager.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace {

// Test mode keeps the gamepads of the machine running the test away from its pages.
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
        browser_options.allow_popups = WebView::AllowPopups::Yes;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }
};

void give_user_attention(WebView::ViewImplementation& view, bool has_user_attention)
{
    view.set_system_visibility_state(has_user_attention ? Web::HTML::VisibilityState::Visible : Web::HTML::VisibilityState::Hidden);
    view.set_has_system_focus(has_user_attention);
}

void start_using_gamepads(WebView::ViewImplementation& view)
{
    bool title_changed = false;
    view.on_title_change = [&](auto const&) { title_changed = true; };
    view.run_javascript("navigator.getGamepads(); document.title = 'Using gamepads';"_string);
    Core::EventLoop::current().spin_until([&] { return title_changed; });
    view.on_title_change = nullptr;
}

void press_button(WebView::WebContentClient& client, Web::Gamepad::VirtualGamepad const& gamepad)
{
    auto& gamepad_manager = WebView::GamepadManager::the();
    gamepad_manager.set_virtual_gamepad_button(client, gamepad.handle, gamepad.buttons[0], true);
    gamepad_manager.set_virtual_gamepad_button(client, gamepad.handle, gamepad.buttons[0], false);
}

bool gamepad_is_announced_to(WebView::WebContentClient& client)
{
    auto events = WebView::GamepadManager::the().pump_gamepad_events(client);
    return any_of(events, [](auto const& event) { return event.template has<Web::Gamepad::GamepadConnectedEvent>(); });
}

}

// A process learns of gamepads only once the user interacts with one while a page of the process uses gamepads, has
// the user's attention, and holds a document allowed to use gamepads.

TEST_CASE(gamepad_exposure)
{
    Test::HeadlessTestEnvironment environment { "TestGamepadExposure"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    auto& gamepad_manager = WebView::GamepadManager::the();

    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        auto& client = view->page().client();
        give_user_attention(*view, true);
        auto gamepad = gamepad_manager.create_virtual_gamepad(client).release_value();

        press_button(client, gamepad);
        VERIFY(!gamepad_is_announced_to(client));
        outln("PASS: a process with no page using gamepads learns of none");

        start_using_gamepads(*view);

        view->set_has_system_focus(false);
        press_button(client, gamepad);
        VERIFY(!gamepad_is_announced_to(client));
        view->set_has_system_focus(true);
        outln("PASS: a gesture while the page has no system focus is not one");

        view->set_system_visibility_state(Web::HTML::VisibilityState::Hidden);
        press_button(client, gamepad);
        VERIFY(!gamepad_is_announced_to(client));
        view->set_system_visibility_state(Web::HTML::VisibilityState::Visible);
        outln("PASS: a gesture while the page is hidden is not one");

        auto& document = view->traversable().active_document();
        auto permissions_policy = document.permissions_policy();
        auto permissions_policy_without_gamepads = permissions_policy;
        permissions_policy_without_gamepads.inherited_policy[to_underlying(Web::PermissionsPolicy::PolicyControlledFeature::Gamepad)] = Web::PermissionsPolicy::FeatureValue::Disabled;
        document.set_permissions_policy(move(permissions_policy_without_gamepads));
        press_button(client, gamepad);
        VERIFY(!gamepad_is_announced_to(client));
        document.set_permissions_policy(move(permissions_policy));
        outln("PASS: a gesture for a page not allowed to use gamepads is not one");

        press_button(client, gamepad);
        VERIFY(gamepad_is_announced_to(client));
        outln("PASS: a gesture while the page has the user's attention announces the gamepad");
    }

    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        auto& client = view->page().client();
        give_user_attention(*view, true);

        OwnPtr<WebView::HeadlessWebView> popup;
        bool popup_loaded = false;
        view->on_new_web_view = [&](auto, auto, WebView::CanonicalTraversable& traversable) {
            popup = WebView::HeadlessWebView::create_child(*view, traversable);
            popup->on_load_finish = [&](auto const&) { popup_loaded = true; };
            return popup->handle();
        };
        view->run_javascript("window.open('about:blank')"_string);
        Core::EventLoop::current().spin_until([&] { return popup_loaded; });
        VERIFY(&popup->page().client() == &client);

        start_using_gamepads(*view);
        start_using_gamepads(*popup);
        give_user_attention(*view, false);
        give_user_attention(*popup, true);

        auto gamepad = gamepad_manager.create_virtual_gamepad(client).release_value();
        press_button(client, gamepad);
        VERIFY(gamepad_is_announced_to(client));
        outln("PASS: a gesture for any page of the process using gamepads announces the gamepad");
    }

    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        auto& page = view->page();
        auto& client = page.client();
        auto& traversable = view->traversable();
        give_user_attention(*view, true);

        size_t loads_finished = 0;
        view->on_load_finish = [&](auto const&) { ++loads_finished; };
        Core::EventLoop::current().spin_until([&] { return loads_finished >= 1; });
        view->load_html("<!DOCTYPE html><iframe srcdoc=\"<!DOCTYPE html>\"></iframe>"sv);
        WebView::CanonicalNavigable* iframe = nullptr;
        Core::EventLoop::current().spin_until([&] {
            traversable.for_each_in_subtree([&](WebView::CanonicalNavigable& navigable) {
                iframe = &navigable;
                return IterationDecision::Break;
            });
            return loads_finished >= 2 && iframe && page.hosts_active_document_of(*iframe);
        });
        start_using_gamepads(*view);
        VERIFY(iframe->active_document_is_fully_active_descendant_of_a_top_level_traversable_with_user_attention());
        VERIFY(iframe->active_document().is_allowed_to_use(Web::PermissionsPolicy::PolicyControlledFeature::Gamepad));

        // A crashed process leaves this, with the page of the iframe's container standing in for the lost document.
        iframe->did_lose_active_document();
        VERIFY(!iframe->active_document().host());
        VERIFY(traversable.hosts(*iframe, page));
        VERIFY(!iframe->active_document_is_fully_active_descendant_of_a_top_level_traversable_with_user_attention());

        auto& top_document = traversable.active_document();
        auto permissions_policy = top_document.permissions_policy();
        auto permissions_policy_without_gamepads = permissions_policy;
        permissions_policy_without_gamepads.inherited_policy[to_underlying(Web::PermissionsPolicy::PolicyControlledFeature::Gamepad)] = Web::PermissionsPolicy::FeatureValue::Disabled;
        top_document.set_permissions_policy(move(permissions_policy_without_gamepads));
        VERIFY(!page.hosts_a_document_allowed_to_use(Web::PermissionsPolicy::PolicyControlledFeature::Gamepad));

        auto gamepad = gamepad_manager.create_virtual_gamepad(client).release_value();
        press_button(client, gamepad);
        VERIFY(!gamepad_is_announced_to(client));
        outln("PASS: a page standing in for a lost document gets no gamepads through it");

        top_document.set_permissions_policy(move(permissions_policy));
        press_button(client, gamepad);
        VERIFY(gamepad_is_announced_to(client));
        outln("PASS: the page gets gamepads through a document it hosts");
    }
}
