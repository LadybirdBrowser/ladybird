/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>

// A view whose location indicator is overdue to stop showing, as when the timer that would hide it hasn't run yet.
class ViewWithAnOverdueIndicator final : public WebView::HeadlessWebView {
public:
    static NonnullOwnPtr<ViewWithAnOverdueIndicator> create(Core::AnonymousBuffer theme)
    {
        auto view = adopt_own(*new ViewWithAnOverdueIndicator(move(theme)));
        view->initialize_tab(Web::HTML::VisibilityState::Visible);
        return view;
    }

    void stop_using_location_long_ago()
    {
        auto long_ago = MonotonicTime::now() - WebView::TabIndicators::LINGER - WebView::TabIndicators::LINGER;
        m_tab_indicators.set_active(WebView::TabIndicator::UsingLocation, true, long_ago);
        m_tab_indicators.set_active(WebView::TabIndicator::UsingLocation, false, long_ago);
    }

private:
    explicit ViewWithAnOverdueIndicator(Core::AnonymousBuffer theme)
        : HeadlessWebView(move(theme), { 800, 600 })
    {
    }
};

// A frontend destroys its widgets before the view they hold: the view's teardown, which ends its location requests and
// closes its page, must not call back into the frontend.
TEST_CASE(a_view_being_destroyed_does_not_report_what_its_tab_shows)
{
    Test::HeadlessTestEnvironment environment { "TestViewDestruction"sv };

    auto app = TRY_OR_FAIL(environment.create_application());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));
    OwnPtr<ViewWithAnOverdueIndicator> view = ViewWithAnOverdueIndicator::create(move(theme));

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&] { return loads_finished == 1; });

    // Ending the location requests is the next time the view looks at what its tab shows.
    view->stop_using_location_long_ago();
    EXPECT(view->tab_indicator_visible(WebView::TabIndicator::UsingLocation));

    Optional<bool> reported_visibility;
    view->on_tab_indicator_change = [&](auto, bool visible) { reported_visibility = visible; };
    view = nullptr;
    EXPECT(!reported_visibility.has_value());
}
