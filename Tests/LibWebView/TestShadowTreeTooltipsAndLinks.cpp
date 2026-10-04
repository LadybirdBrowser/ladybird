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
#include <LibURL/Parser.h>
#include <LibWebCommon/HTML/VisibilityState.h>
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

// A view that records the URL each tab it opens is loaded with.
class TestWebView final : public WebView::HeadlessWebView {
public:
    static NonnullOwnPtr<TestWebView> create(Core::AnonymousBuffer theme, Web::DevicePixelSize window_size)
    {
        auto view = adopt_own(*new TestWebView(move(theme), window_size));
        view->initialize_tab(Web::HTML::VisibilityState::Visible);
        return view;
    }

    Vector<URL::URL> const& new_tab_urls() const { return m_new_tab_urls; }

private:
    TestWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size)
        : HeadlessWebView(move(theme), viewport_size)
    {
    }

    virtual WebView::ViewImplementation* create_view_for_new_tab_or_window(WebView::IsPrivate is_private) override
    {
        auto* view = HeadlessWebView::create_view_for_new_tab_or_window(is_private);
        view->on_url_change = [this](auto const& url) { m_new_tab_urls.append(url); };
        return view;
    }

    Vector<URL::URL> m_new_tab_urls;
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

void middle_click_at(WebView::ViewImplementation& view, Web::DevicePixelPoint position)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseDown,
        .position = position,
        .screen_position = position,
        .button = Web::UIEvents::MouseButton::Middle,
        .buttons = Web::UIEvents::MouseButton::Middle,
        .click_count = 1,
        .browser_data = nullptr,
    });
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseUp,
        .position = position,
        .screen_position = position,
        .button = Web::UIEvents::MouseButton::Middle,
        .click_count = 1,
        .browser_data = nullptr,
    });
}

}

// The UI shows a tooltip for the advisory information of the element under the pointer, and a link-preview label for
// the link it's in. Both come from an ancestor walk, which must follow the flat tree: An element in a shadow tree takes
// the title and the link of its shadow host and the host's ancestors, and an element assigned to a slot takes those of
// the slot and the slot's ancestors.
// Middle-clicking opens the link the walk finds in a new tab. And an empty title is no advisory information at all,
// while carriage returns in a title become line feeds.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestShadowTreeTooltipsAndLinks-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
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

    auto view = TestWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };

    // Wait out the initial about:blank load; navigating before it completes would drop the navigation.
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    // Rows 100px tall: an element in a shadow tree whose host has a title and is in a link; an element assigned to a
    // slot that's in a link with a title in a shadow tree; plain text; text in an element with an empty title, in an
    // element with a title; text in an element whose title has carriage returns; and text in a link, in an element with
    // a title. The pointer's target is always an
    // element, so the shadow tree and the slot each hold one: Bare text would make its host the target.
    view->load_html(R"~~~(<!DOCTYPE html>
<style>
body { margin: 0; font: 80px/100px monospace }
.row { position: fixed; left: 0; width: 800px; height: 100px }
</style>
<a href="data:text/html,around-host"><span id="host-in-link" class="row" style="top:0" title="Title on the host"></span></a>
<span id="slot-host" class="row" style="top:100px"><span>Slotted</span></span>
<span class="row" style="top:200px">Plain</span>
<span class="row" style="top:300px" title="Outer title"><span title="">Empty</span></span>
<span id="carriage-returns" class="row" style="top:400px">Lines</span>
<a href="data:text/html,start"><span class="row" style="top:500px" title="Start">Start</span></a>
<script>
document.getElementById("host-in-link").attachShadow({ mode: "open" }).innerHTML = "<span>Shadow</span>";
document.getElementById("slot-host").attachShadow({ mode: "open" }).innerHTML = `<a href="data:text/html,in-shadow-tree" title="Title in the shadow tree"><slot></slot></a>`;
document.getElementById("carriage-returns").setAttribute("title", "One\r\nTwo\rThree");
</script>)~~~"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 2; });

    size_t tooltip_enters = 0;
    size_t tooltip_leaves = 0;
    Optional<ByteString> tooltip;
    view->on_enter_tooltip_area = [&](auto const& text) {
        ++tooltip_enters;
        tooltip = text;
    };
    view->on_leave_tooltip_area = [&] {
        ++tooltip_leaves;
        tooltip.clear();
    };

    size_t link_hovers = 0;
    size_t link_unhovers = 0;
    Optional<URL::URL> hovered_url;
    view->on_link_hover = [&](auto const& url) {
        ++link_hovers;
        hovered_url = url;
    };
    view->on_link_unhover = [&] {
        ++link_unhovers;
        hovered_url.clear();
    };

    // A move can bring a tooltip report and a link report in either order, so each wait counts the reports from a
    // point taken before the move.
    struct ReportCounts {
        size_t tooltip { 0 };
        size_t link { 0 };
    };
    auto report_counts = [&] { return ReportCounts { tooltip_enters + tooltip_leaves, link_hovers + link_unhovers }; };
    auto wait_for_tooltip_report = [&](ReportCounts since) {
        Core::EventLoop::current().spin_until([&]() { return tooltip_enters + tooltip_leaves > since.tooltip; });
    };
    auto wait_for_link_report = [&](ReportCounts since) {
        Core::EventLoop::current().spin_until([&]() { return link_hovers + link_unhovers > since.link; });
    };

    // Each check below starts from a tooltip and a link that the move under test changes, so a broken walk shows up as
    // the wrong report, rather than as no report at all.
    auto since = report_counts();
    move_mouse_to(*view, { 20, 550 });
    wait_for_tooltip_report(since);
    VERIFY(tooltip == "Start"sv);
    wait_for_link_report(since);
    VERIFY(hovered_url.has_value());
    VERIFY(hovered_url->serialize() == "data:text/html,start"sv);

    // An element in a shadow tree: The host's title and the link around the host apply to it.
    since = report_counts();
    move_mouse_to(*view, { 20, 50 });
    wait_for_tooltip_report(since);
    VERIFY(tooltip == "Title on the host"sv);
    wait_for_link_report(since);
    VERIFY(hovered_url.has_value());
    VERIFY(hovered_url->serialize() == "data:text/html,around-host"sv);

    // A middle-click on it opens that link in a new tab.
    middle_click_at(*view, { 20, 50 });
    Core::EventLoop::current().spin_until([&]() {
        return view->new_tab_urls().contains_slow(URL::Parser::basic_parse("data:text/html,around-host"sv).release_value());
    });

    // An element assigned to a slot: The title and the link around the slot apply to it.
    since = report_counts();
    move_mouse_to(*view, { 20, 150 });
    wait_for_tooltip_report(since);
    VERIFY(tooltip == "Title in the shadow tree"sv);
    wait_for_link_report(since);
    VERIFY(hovered_url.has_value());
    VERIFY(hovered_url->serialize() == "data:text/html,in-shadow-tree"sv);

    // Plain text has neither.
    since = report_counts();
    move_mouse_to(*view, { 20, 250 });
    wait_for_tooltip_report(since);
    VERIFY(!tooltip.has_value());
    wait_for_link_report(since);
    VERIFY(!hovered_url.has_value());

    // An empty title is no advisory information, and hides the outer element's: From a title over to it, the tooltip
    // goes away.
    since = report_counts();
    move_mouse_to(*view, { 20, 50 });
    wait_for_tooltip_report(since);
    VERIFY(tooltip == "Title on the host"sv);
    since = report_counts();
    move_mouse_to(*view, { 20, 350 });
    wait_for_tooltip_report(since);
    VERIFY(!tooltip.has_value());

    // Newlines in a title are normalized: Each CR LF pair and each lone CR becomes an LF.
    since = report_counts();
    move_mouse_to(*view, { 20, 450 });
    wait_for_tooltip_report(since);
    VERIFY(tooltip == "One\nTwo\nThree"sv);

    outln("PASS: titles and links apply across shadow trees and slots, and an empty title is no advisory information");
    return 0;
}
