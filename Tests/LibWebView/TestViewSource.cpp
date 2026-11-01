/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Menu.h>
#include <LibWebView/Utilities.h>

namespace {

Core::AnonymousBuffer load_theme()
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    return MUST(Gfx::load_system_theme(theme_path.string()));
}

// The source of a page opens in a new tab, which becomes the active one.
class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab) const override
    {
        m_opened_tabs.append(WebView::HeadlessWebView::create(load_theme(), { 800, 600 }));
        auto& tab = m_opened_tabs.last();
        tab->on_load_finish = [this](auto const&) { ++m_opened_tab_loads; };
        m_active_view = tab.ptr();
        return *tab;
    }

    virtual Optional<WebView::ViewImplementation&> active_web_view() const override
    {
        if (!m_active_view)
            return {};
        return *m_active_view;
    }

    void activate(WebView::ViewImplementation& view)
    {
        m_active_view = &view;
        update_actions_for_active_web_view();
    }

    size_t opened_tab_count() const { return m_opened_tabs.size(); }
    WebView::HeadlessWebView& opened_tab(size_t index) { return *m_opened_tabs[index]; }
    size_t opened_tab_loads() const { return m_opened_tab_loads; }

private:
    mutable Vector<NonnullOwnPtr<WebView::HeadlessWebView>> m_opened_tabs;
    mutable WebView::ViewImplementation* m_active_view { nullptr };
    mutable size_t m_opened_tab_loads { 0 };
};

}

// Viewing the source of a page opens a tab that shows it. That tab offers no source of its own to view, so the source
// of a source page can't be viewed over and over, each time wrapped in another layer of markup. A source tab that
// moves on to another page offers its source again.
TEST_CASE(view_source_of_source_page)
{
    Test::HeadlessTestEnvironment environment { "TestViewSource"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto view = WebView::HeadlessWebView::create(load_theme(), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    view->load_html("<!DOCTYPE html><p>A page</p>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 2; });

    app->activate(*view);
    VERIFY(view->can_view_source());
    VERIFY(app->view_source_action().enabled());

    app->view_source_action().activate();
    Core::EventLoop::current().spin_until([&]() {
        return app->opened_tab_count() == 1 && app->opened_tab(0).url() == URL::about_srcdoc();
    });

    auto& source_tab = app->opened_tab(0);
    VERIFY(!source_tab.can_view_source());
    VERIFY(!app->view_source_action().enabled());

    source_tab.get_source();
    VERIFY(app->opened_tab_count() == 1);

    // A fragment navigation keeps the source page's document, so the restriction stays.
    source_tab.run_javascript("location.hash = 'fragment'"_string);
    Core::EventLoop::current().spin_until([&]() { return source_tab.url().fragment() == "fragment"sv; });
    VERIFY(!source_tab.can_view_source());

    app->activate(*view);
    VERIFY(app->view_source_action().enabled());

    app->activate(source_tab);
    VERIFY(!app->view_source_action().enabled());

    auto loads_before_navigation = app->opened_tab_loads();
    source_tab.load(URL::Parser::basic_parse("data:text/html,<p>Another page</p>"sv).release_value());
    Core::EventLoop::current().spin_until([&]() { return app->opened_tab_loads() > loads_before_navigation; });

    VERIFY(source_tab.can_view_source());
    VERIFY(app->view_source_action().enabled());

    // History traversal brings the source page back, and with it the restriction.
    source_tab.traverse_the_history_by_delta(-1);
    Core::EventLoop::current().spin_until([&]() { return source_tab.url().equals(URL::about_srcdoc(), URL::ExcludeFragment::Yes); });
    VERIFY(!source_tab.can_view_source());
    VERIFY(!app->view_source_action().enabled());

    source_tab.traverse_the_history_by_delta(1);
    Core::EventLoop::current().spin_until([&]() { return !source_tab.url().equals(URL::about_srcdoc(), URL::ExcludeFragment::Yes); });
    VERIFY(source_tab.can_view_source());
    VERIFY(app->view_source_action().enabled());

    outln("PASS: A tab showing the source of a page offers no source of its own, while it shows that source");
}
