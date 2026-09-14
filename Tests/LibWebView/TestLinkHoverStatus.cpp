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
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/Page/InputEvent.h>
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

void move_mouse_to(WebView::ViewImplementation& view, Web::DevicePixelPoint position)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseMove,
        .position = position,
        .screen_position = position,
        .browser_data = nullptr,
    });
}

void leave_view(WebView::ViewImplementation& view)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseLeave,
        .position = {},
        .screen_position = {},
        .browser_data = nullptr,
    });
}

}

// The UI shows a link-preview label while WebContent reports a hovered link, and hides it on the unhover report. A view
// hidden underneath the pointer (keyboard tab switch, minimized window) gets no Leave from its toolkit, so the UI sends
// WebContent the MouseLeave a real Leave would. And WebContent must end the hover on it, and report the link fresh once
// the pointer moves over it again, rather than treating it as a no-op because the link under the pointer never changed.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestLinkHoverStatus-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
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

    // A link over the top-left quarter of the viewport, with plain page below and to the right of it.
    view->load_html("<!DOCTYPE html><a href=\"https://example.com/\" style=\"position:fixed;left:0;top:0;width:400px;height:300px\">Link</a>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 2; });

    size_t hovers_reported = 0;
    size_t unhovers_reported = 0;
    Optional<URL::URL> hovered_url;
    view->on_link_hover = [&](auto const& url) {
        ++hovers_reported;
        hovered_url = url;
    };
    view->on_link_unhover = [&] {
        ++unhovers_reported;
        hovered_url.clear();
    };

    move_mouse_to(*view, { 20, 20 });
    Core::EventLoop::current().spin_until([&]() { return hovers_reported >= 1; });
    VERIFY(hovered_url.has_value());
    VERIFY(hovered_url->serialize() == "https://example.com/"sv);

    // A MouseLeave ends the hover, with the pointer's last position still over the link.
    leave_view(*view);
    Core::EventLoop::current().spin_until([&]() { return unhovers_reported >= 1; });
    VERIFY(!hovered_url.has_value());

    // The next move over the same link reports it again: The leave unset the pointer position WebContent tracks, so
    // the link is a fresh target, and not one the pointer never left.
    move_mouse_to(*view, { 20, 20 });
    Core::EventLoop::current().spin_until([&]() { return hovers_reported >= 2; });
    VERIFY(hovered_url.has_value());
    VERIFY(unhovers_reported == 1);

    // The UI sends the leave for a hidden view while the page is hidden, and in the browser WebContent handles input
    // at a rendering opportunity, which the compositor grants a hidden page none of — so the leave is handled once the
    // page is visible again, and must still end the hover then, before any move that follows it.
    view->set_system_visibility_state(Web::HTML::VisibilityState::Hidden);
    leave_view(*view);
    view->set_system_visibility_state(Web::HTML::VisibilityState::Visible);
    Core::EventLoop::current().spin_until([&]() { return unhovers_reported >= 2; });
    VERIFY(!hovered_url.has_value());
    VERIFY(hovers_reported == 2);

    move_mouse_to(*view, { 20, 20 });
    Core::EventLoop::current().spin_until([&]() { return hovers_reported >= 3; });
    VERIFY(hovered_url.has_value());

    // Moving off the link onto plain page reports the unhover, as before.
    move_mouse_to(*view, { 600, 500 });
    Core::EventLoop::current().spin_until([&]() { return unhovers_reported >= 3; });
    VERIFY(!hovered_url.has_value());
    VERIFY(hovers_reported == 3);

    outln("PASS: a MouseLeave ends the link hover, and the next move over the link reports it again");
    return 0;
}
