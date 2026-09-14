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
// And replacing the document ends the hover too, with the pointer resting: The link the page reported belongs to the
// outgoing document, and a keyboard navigation moves no pointer. But only the handler that reported the hover ends it
// that way: an iframe navigating underneath a hover in its parent document leaves the parent's link reported.

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

    move_mouse_to(*view, { 20, 20 });
    Core::EventLoop::current().spin_until([&]() { return hovers_reported >= 4; });
    VERIFY(hovered_url.has_value());

    // Replacing the document reports the unhover before the load finishes, with the pointer resting. WebContent sends
    // both reports over the one connection, in order, so the unhover is in by the time the load is.
    view->load_html("<!DOCTYPE html><a href=\"https://example.org/\" style=\"position:fixed;left:0;top:0;width:400px;height:300px\">Link</a>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 3; });
    VERIFY(unhovers_reported == 4);
    VERIFY(!hovered_url.has_value());
    VERIFY(hovers_reported == 4);

    // The new document's link at the same spot is a fresh target at the pointer's next move.
    move_mouse_to(*view, { 20, 20 });
    Core::EventLoop::current().spin_until([&]() { return hovers_reported >= 5; });
    VERIFY(hovered_url.has_value());
    VERIFY(hovered_url->serialize() == "https://example.org/"sv);

    // A page with an iframe over the top-left quarter, holding a link that fills it, and a link of the page's own over
    // the top-right quarter. The page retitles itself on each load of the iframe, so the test can wait one out.
    Utf16String last_title;
    view->on_title_change = [&](auto const& title) { last_title = title; };
    view->load_html(R"~~~(<!DOCTYPE html>
<iframe id="frame" style="position:fixed;left:0;top:0;width:400px;height:300px;border:0" srcdoc="<a href='https://example.com/in-iframe' style='position:fixed;left:0;top:0;width:400px;height:300px'>Link</a>"></iframe>
<a href="https://example.com/parent" style="position:fixed;left:400px;top:0;width:400px;height:300px">Link</a>
<script>
let frame_loads = 0;
frame.addEventListener("load", () => { document.title = "frame load " + (++frame_loads); });
function navigate_frame() { frame.srcdoc = frame.getAttribute("srcdoc") + "<!-- " + frame_loads + " -->"; }
</script>)~~~"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 4; });
    // The pointer rested on the previous page's link, so that page's replacement reported the unhover.
    VERIFY(unhovers_reported == 5);
    VERIFY(!hovered_url.has_value());

    // The iframe's own handler reports its link. And the iframe navigating underneath the pointer replaces the document
    // that link is in, so it reports the unhover, with the pointer resting.
    move_mouse_to(*view, { 20, 20 });
    Core::EventLoop::current().spin_until([&]() { return hovers_reported >= 6; });
    VERIFY(hovered_url->serialize() == "https://example.com/in-iframe"sv);
    view->run_javascript("navigate_frame()"_string);
    Core::EventLoop::current().spin_until([&]() { return unhovers_reported >= 6; });
    VERIFY(!hovered_url.has_value());
    Core::EventLoop::current().spin_until([&]() { return last_title == "frame load 2"sv; });

    // The new iframe document's link is a fresh target. Then over to the page's own link: The page's handler reports
    // it, while the iframe's handler still holds the iframe's link as the pointer's position, never having heard that
    // the pointer left the iframe.
    move_mouse_to(*view, { 20, 20 });
    Core::EventLoop::current().spin_until([&]() { return hovers_reported >= 7; });
    VERIFY(hovered_url->serialize() == "https://example.com/in-iframe"sv);
    move_mouse_to(*view, { 600, 20 });
    Core::EventLoop::current().spin_until([&]() { return hovers_reported >= 8; });
    VERIFY(hovered_url->serialize() == "https://example.com/parent"sv);
    VERIFY(unhovers_reported == 6);

    // So, the iframe navigating now must leave the page's link reported: The hover the client shows is the page's
    // handler's to end, not the iframe's. The iframe's load comes after any unhover its replacement would have sent.
    view->run_javascript("navigate_frame()"_string);
    Core::EventLoop::current().spin_until([&]() { return last_title == "frame load 3"sv; });
    VERIFY(unhovers_reported == 6);
    VERIFY(hovered_url->serialize() == "https://example.com/parent"sv);

    // Back over the iframe's link, then a new top-level document: The iframe dies along with the old one, and its
    // handler reports the link gone as the iframe's document is destroyed.
    move_mouse_to(*view, { 20, 20 });
    Core::EventLoop::current().spin_until([&]() { return hovers_reported >= 9; });
    VERIFY(hovered_url->serialize() == "https://example.com/in-iframe"sv);
    view->load_html("<!DOCTYPE html><p>Plain</p>"sv);
    Core::EventLoop::current().spin_until([&]() { return unhovers_reported >= 7; });
    VERIFY(!hovered_url.has_value());
    VERIFY(hovers_reported == 9);

    outln("PASS: a MouseLeave or a document replacement ends the link hover, and the next move over the link reports it again");
    return 0;
}
