/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
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

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

void scroll_wheel_at(WebView::ViewImplementation& view, Web::DevicePixelPoint position, double wheel_delta_y)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseWheel,
        .position = position,
        .screen_position = position,
        .wheel_delta_y = wheel_delta_y,
        .wheel_delta_precision = Web::WheelDeltaPrecision::Precise,
        .browser_data = nullptr,
    });
}

void move_mouse_to(WebView::ViewImplementation& view, Web::DevicePixelPoint position)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseMove,
        .position = position,
        .screen_position = position,
        .browser_data = nullptr,
    });
}

void press_mouse_at(WebView::ViewImplementation& view, Web::DevicePixelPoint position)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseDown,
        .position = position,
        .screen_position = position,
        .button = Web::UIEvents::MouseButton::Primary,
        .buttons = Web::UIEvents::MouseButton::Primary,
        .click_count = 1,
        .browser_data = nullptr,
    });
}

void cancel_mouse_press(WebView::ViewImplementation& view)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseCancel,
        .position = {},
        .screen_position = {},
        .browser_data = nullptr,
    });
}

}

// Mouse input reaches the compositor without the UI waiting for it. A wheel step is scrolled by the compositor and then
// forwarded to WebContent, whose acknowledgement settles the UI's pending entry; a plain move is forwarded and settled
// the same way. Either way the pending queue drains, which is what lets later compositor key scrolling proceed. A press
// that the UI cancels reaches the page along the same route, so the page sees its pointer stream suppressed.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestCompositorInputRouting-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
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

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };

    // Wait out the initial about:blank load; navigating before it completes would drop the navigation.
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    Utf16String last_title;
    view->on_title_change = [&](auto const& title) { last_title = title; };

    // A page three viewports tall that reports its scroll position, and presses being cancelled, through its title.
    view->load_html(R"~~~(<!DOCTYPE html>
<title>unscrolled</title>
<body style="margin:0;height:3000px">
<script>
addEventListener("scroll", () => { document.title = "scrolled to " + scrollY; });
addEventListener("pointerdown", () => { document.title = "pressed"; });
addEventListener("pointercancel", () => { document.title = "press cancelled"; });
</script>
</body>)~~~"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 2; });
    VERIFY(last_title == "unscrolled"sv);

    scroll_wheel_at(*view, { 400, 300 }, 120);
    Core::EventLoop::current().spin_until([&]() { return last_title != "unscrolled"sv; });
    VERIFY(last_title.starts_with("scrolled to "sv));
    VERIFY(last_title != "scrolled to 0"sv);
    Core::EventLoop::current().spin_until([&]() { return view->pending_input_event_count_for_testing() == 0; });

    // A move over plain content is forwarded to WebContent, and its acknowledgement settles the pending entry.
    move_mouse_to(*view, { 100, 100 });
    VERIFY(view->pending_input_event_count_for_testing() == 1);
    Core::EventLoop::current().spin_until([&]() { return view->pending_input_event_count_for_testing() == 0; });

    press_mouse_at(*view, { 100, 100 });
    Core::EventLoop::current().spin_until([&]() { return last_title == "pressed"sv; });
    cancel_mouse_press(*view);
    Core::EventLoop::current().spin_until([&]() { return last_title == "press cancelled"sv; });
    Core::EventLoop::current().spin_until([&]() { return view->pending_input_event_count_for_testing() == 0; });

    outln("PASS: mouse input routed through the compositor scrolls the page, reaches it and settles the UI's pending events");
    return 0;
}
