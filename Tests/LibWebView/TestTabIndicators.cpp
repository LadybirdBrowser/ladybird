/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>
#include <LibWebView/TabIndicators.h>

using WebView::TabIndicator;

static constexpr auto LINGER = WebView::TabIndicators::LINGER;

static AK::Duration seconds(i64 seconds)
{
    return AK::Duration::from_seconds(seconds);
}

struct VisibilityChange {
    TabIndicator indicator;
    bool visible;

    bool operator==(VisibilityChange const&) const = default;
};

// The indicators' timer is never run: each test moves time forward itself.
struct IndicatorsFixture {
    Core::EventLoop loop;
    WebView::TabIndicators indicators;
    Vector<VisibilityChange> changes;

    IndicatorsFixture()
    {
        indicators.on_visibility_change = [this](TabIndicator indicator, bool visible) {
            changes.append({ indicator, visible });
        };
    }

    Vector<VisibilityChange> take_changes() { return exchange(changes, {}); }
};

TEST_CASE(an_indicator_shows_as_soon_as_it_is_active)
{
    IndicatorsFixture fixture;
    auto now = MonotonicTime::now();
    fixture.indicators.set_active(TabIndicator::PlayingAudio, true, now);

    EXPECT(fixture.take_changes() == Vector<VisibilityChange> { { TabIndicator::PlayingAudio, true } });
    EXPECT(fixture.indicators.is_visible(TabIndicator::PlayingAudio));
    EXPECT(!fixture.indicators.is_visible(TabIndicator::UsingLocation));
    EXPECT(!fixture.indicators.next_deadline(now).has_value());
}

TEST_CASE(an_indicator_shows_until_its_linger_has_passed)
{
    IndicatorsFixture fixture;
    auto now = MonotonicTime::now();
    fixture.indicators.set_active(TabIndicator::PlayingAudio, true, now);
    (void)fixture.take_changes();

    fixture.indicators.set_active(TabIndicator::PlayingAudio, false, now + seconds(1));
    EXPECT(fixture.take_changes().is_empty());
    EXPECT(!fixture.indicators.is_active(TabIndicator::PlayingAudio));
    EXPECT(fixture.indicators.is_visible(TabIndicator::PlayingAudio));
    EXPECT(fixture.indicators.next_deadline(now + seconds(1)) == now + seconds(1) + LINGER);

    fixture.indicators.update_visibility(now + seconds(1) + LINGER);
    EXPECT(fixture.take_changes() == Vector<VisibilityChange> { { TabIndicator::PlayingAudio, false } });
    EXPECT(!fixture.indicators.next_deadline(now + seconds(1) + LINGER).has_value());
}

TEST_CASE(indicators_linger_on_deadlines_of_their_own)
{
    IndicatorsFixture fixture;
    auto now = MonotonicTime::now();
    fixture.indicators.set_active(TabIndicator::PlayingAudio, true, now);
    fixture.indicators.set_active(TabIndicator::UsingLocation, true, now);
    fixture.indicators.set_active(TabIndicator::PlayingAudio, false, now + seconds(1));
    fixture.indicators.set_active(TabIndicator::UsingLocation, false, now + seconds(2));
    (void)fixture.take_changes();
    EXPECT(fixture.indicators.next_deadline(now + seconds(2)) == now + seconds(1) + LINGER);

    auto first_deadline = now + seconds(1) + LINGER;
    fixture.indicators.update_visibility(first_deadline);
    EXPECT(fixture.take_changes() == Vector<VisibilityChange> { { TabIndicator::PlayingAudio, false } });
    EXPECT(fixture.indicators.is_visible(TabIndicator::UsingLocation));
    EXPECT(fixture.indicators.next_deadline(first_deadline) == now + seconds(2) + LINGER);

    fixture.indicators.update_visibility(now + seconds(2) + LINGER);
    EXPECT(fixture.take_changes() == Vector<VisibilityChange> { { TabIndicator::UsingLocation, false } });
}

TEST_CASE(indicators_that_stop_together_go_together)
{
    IndicatorsFixture fixture;
    auto now = MonotonicTime::now();
    fixture.indicators.set_active(TabIndicator::PlayingAudio, true, now);
    fixture.indicators.set_active(TabIndicator::UsingLocation, true, now);
    fixture.indicators.set_active(TabIndicator::PlayingAudio, false, now);
    fixture.indicators.set_active(TabIndicator::UsingLocation, false, now);
    (void)fixture.take_changes();
    EXPECT(fixture.indicators.next_deadline(now) == now + LINGER);

    fixture.indicators.update_visibility(now + LINGER);
    EXPECT(fixture.take_changes() == (Vector<VisibilityChange> { { TabIndicator::PlayingAudio, false }, { TabIndicator::UsingLocation, false } }));
}

TEST_CASE(one_indicator_resuming_leaves_the_other_lingering)
{
    IndicatorsFixture fixture;
    auto now = MonotonicTime::now();
    fixture.indicators.set_active(TabIndicator::PlayingAudio, true, now);
    fixture.indicators.set_active(TabIndicator::UsingLocation, true, now);
    fixture.indicators.set_active(TabIndicator::PlayingAudio, false, now);
    fixture.indicators.set_active(TabIndicator::UsingLocation, false, now);

    fixture.indicators.set_active(TabIndicator::UsingLocation, true, now + seconds(1));
    (void)fixture.take_changes();
    EXPECT(fixture.indicators.next_deadline(now + seconds(1)) == now + LINGER);

    fixture.indicators.update_visibility(now + LINGER);
    EXPECT(fixture.take_changes() == Vector<VisibilityChange> { { TabIndicator::PlayingAudio, false } });
    EXPECT(fixture.indicators.is_visible(TabIndicator::UsingLocation));
}

TEST_CASE(an_indicator_a_listener_makes_active_again_is_not_reported_twice)
{
    IndicatorsFixture fixture;
    auto now = MonotonicTime::now();
    fixture.indicators.set_active(TabIndicator::PlayingAudio, true, now);
    fixture.indicators.set_active(TabIndicator::UsingLocation, true, now);
    fixture.indicators.set_active(TabIndicator::PlayingAudio, false, now);
    fixture.indicators.set_active(TabIndicator::UsingLocation, false, now);
    (void)fixture.take_changes();

    // As the playing indicator stops showing, its listener makes the location one active again.
    fixture.indicators.on_visibility_change = [&](TabIndicator indicator, bool visible) {
        fixture.changes.append({ indicator, visible });
        if (indicator == TabIndicator::PlayingAudio)
            fixture.indicators.set_active(TabIndicator::UsingLocation, true, now + LINGER);
    };
    fixture.indicators.update_visibility(now + LINGER);

    EXPECT(fixture.take_changes() == Vector<VisibilityChange> { { TabIndicator::PlayingAudio, false } });
    EXPECT(fixture.indicators.is_visible(TabIndicator::UsingLocation));
    EXPECT(!fixture.indicators.next_deadline(now + LINGER).has_value());
}

TEST_CASE(a_passed_deadline_is_noticed_when_any_indicator_is_updated)
{
    IndicatorsFixture fixture;
    auto now = MonotonicTime::now();
    fixture.indicators.set_active(TabIndicator::PlayingAudio, true, now);
    fixture.indicators.set_active(TabIndicator::PlayingAudio, false, now);
    (void)fixture.take_changes();

    fixture.indicators.set_active(TabIndicator::UsingLocation, false, now + LINGER);
    EXPECT(fixture.take_changes() == Vector<VisibilityChange> { { TabIndicator::PlayingAudio, false } });
}
