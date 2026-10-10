/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    // NB: Outside test mode, automation, and the internals object, a renderer cannot synthesize input, so the UI process
    //     honors only user activation from input it delivered.
    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

size_t view_count()
{
    size_t count = 0;
    WebView::ViewImplementation::for_each_view([&](auto&) {
        ++count;
        return IterationDecision::Continue;
    });
    return count;
}

void click(WebView::ViewImplementation& view, Web::DevicePixelPoint position, Web::UIEvents::MouseButton button)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseDown,
        .position = position,
        .screen_position = position,
        .button = button,
        .buttons = button,
        .click_count = 1,
        .browser_data = nullptr,
    });
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseUp,
        .position = position,
        .screen_position = position,
        .button = button,
        .click_count = 1,
        .browser_data = nullptr,
    });
}

}

// Each key or mouse press the UI process delivers gives the page one popup, and one link opened in a new tab, even when
// presses queue up before the page handles the first. Consuming the activation of the press the page is handling leaves
// the presses queued behind it theirs, and a click that opens a popup still opens its link in a new tab.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestUserActivation-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_config_directory, Core::Environment::Overwrite::Yes));

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif
    VERIFY(app->blocks_pop_ups());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };

    // NB: The UI process counts the popups it lets the page open. The view creates no window for them.
    size_t popups_opened = 0;
    view->on_new_web_view = [&](auto, auto, auto&) {
        ++popups_opened;
        return String {};
    };

    // Wait out the initial about:blank load; navigating before it completes would drop the navigation.
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    // A button over the left half of the viewport opens a popup per click, and a link filling the right half opens one
    // per middle-click too, before the browser opens the link in a new tab.
    view->load_html("<!DOCTYPE html>"
                    "<button onclick=\"window.open('about:blank')\" style=\"position:fixed;left:0;top:0;width:400px;height:600px\">Popup</button>"
                    "<a href=\"https://example.com/\" onauxclick=\"window.open('about:blank')\" style=\"position:fixed;left:400px;top:0;width:400px;height:600px\">Link</a>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 2; });

    click(*view, { 200, 300 }, Web::UIEvents::MouseButton::Primary);
    click(*view, { 200, 300 }, Web::UIEvents::MouseButton::Primary);
    Core::EventLoop::current().spin_until([&]() { return popups_opened >= 2; });
    outln("two queued clicks open two popups");

    // NB: The headless view opens each new tab in a view of its own.
    auto views_before_links = view_count();
    click(*view, { 600, 300 }, Web::UIEvents::MouseButton::Middle);
    click(*view, { 600, 300 }, Web::UIEvents::MouseButton::Middle);
    Core::EventLoop::current().spin_until([&]() { return view_count() >= views_before_links + 2 && popups_opened >= 4; });
    outln("two queued middle-clicks open two popups and two tabs");

    VERIFY(popups_opened == 4);
    return 0;
}
