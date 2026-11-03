/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/LingeringIndicator.h>

static constexpr auto LINGER = AK::Duration::from_seconds(3);

static AK::Duration seconds(i64 seconds)
{
    return AK::Duration::from_seconds(seconds);
}

TEST_CASE(shows_only_once_active)
{
    WebView::LingeringIndicator indicator { LINGER };
    auto now = MonotonicTime::now();
    EXPECT(!indicator.is_visible(now));

    indicator.set_active(false, now);
    EXPECT(!indicator.is_visible(now));

    indicator.set_active(true, now);
    EXPECT(indicator.is_visible(now + seconds(60)));
    EXPECT(!indicator.visible_until().has_value());
}

TEST_CASE(lingers_for_exactly_the_linger_once_inactive)
{
    WebView::LingeringIndicator indicator { LINGER };
    auto started_at = MonotonicTime::now();
    indicator.set_active(true, started_at);

    auto stopped_at = started_at + seconds(10);
    indicator.set_active(false, stopped_at);
    EXPECT(!indicator.is_active());
    EXPECT(indicator.visible_until() == stopped_at + LINGER);
    EXPECT(indicator.is_visible(stopped_at + LINGER - AK::Duration::from_milliseconds(1)));
    EXPECT(!indicator.is_visible(stopped_at + LINGER));
}

TEST_CASE(being_told_again_that_it_is_inactive_does_not_extend_the_linger)
{
    WebView::LingeringIndicator indicator { LINGER };
    auto stopped_at = MonotonicTime::now();
    indicator.set_active(true, stopped_at);
    indicator.set_active(false, stopped_at);

    indicator.set_active(false, stopped_at + seconds(1));
    indicator.set_active(false, stopped_at + seconds(2));
    EXPECT(indicator.visible_until() == stopped_at + LINGER);
    EXPECT(!indicator.is_visible(stopped_at + LINGER));
}

TEST_CASE(becoming_active_again_cancels_the_linger)
{
    WebView::LingeringIndicator indicator { LINGER };
    auto stopped_at = MonotonicTime::now();
    indicator.set_active(true, stopped_at);
    indicator.set_active(false, stopped_at);

    indicator.set_active(true, stopped_at + seconds(1));
    EXPECT(!indicator.visible_until().has_value());
    EXPECT(indicator.is_visible(stopped_at + LINGER + seconds(10)));

    auto stopped_again_at = stopped_at + LINGER + seconds(10);
    indicator.set_active(false, stopped_again_at);
    EXPECT(indicator.visible_until() == stopped_again_at + LINGER);
}
