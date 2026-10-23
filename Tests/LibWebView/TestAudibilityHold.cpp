/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudio/Audibility.h>
#include <LibTest/TestCase.h>
#include <LibWebView/AudibilityHold.h>

static constexpr Web::PageId PAGE { 1 };
static constexpr Web::PageId OTHER_PAGE { 2 };
static constexpr auto LINGER = Audio::AUDIBILITY_LINGER;

static AK::Duration seconds(i64 seconds)
{
    return AK::Duration::from_seconds(seconds);
}

TEST_CASE(a_page_that_leaves_holds_the_tab_until_its_linger_has_passed)
{
    WebView::AudibilityHold hold;
    auto left_at = MonotonicTime::now();
    EXPECT(!hold.is_held(left_at));

    EXPECT(hold.page_left(PAGE, left_at));
    EXPECT(hold.is_held(left_at));
    EXPECT(hold.is_held(left_at + LINGER - AK::Duration::from_milliseconds(1)));
    EXPECT(!hold.is_held(left_at + LINGER));
}

TEST_CASE(a_page_leaves_once_each_time_it_begins_hosting)
{
    WebView::AudibilityHold hold;
    auto left_at = MonotonicTime::now();
    EXPECT(hold.page_left(PAGE, left_at));

    // The page is let go of after it stops being displayed, or is let go of again.
    EXPECT(!hold.page_left(PAGE, left_at + seconds(1)));
    EXPECT(hold.held_until() == left_at + LINGER);

    hold.page_began_hosting(PAGE);
    EXPECT(hold.page_left(PAGE, left_at + seconds(1)));
    EXPECT(hold.held_until() == left_at + seconds(1) + LINGER);
}

TEST_CASE(the_tab_is_held_until_the_last_page_to_leave_has_lingered)
{
    WebView::AudibilityHold hold;
    auto left_at = MonotonicTime::now();
    EXPECT(hold.page_left(PAGE, left_at));
    EXPECT(hold.page_left(OTHER_PAGE, left_at + seconds(1)));
    EXPECT(hold.is_held(left_at + LINGER));
    EXPECT(!hold.is_held(left_at + seconds(1) + LINGER));

    // A page whose linger would end sooner doesn't shorten the hold.
    hold.page_began_hosting(PAGE);
    EXPECT(!hold.page_left(PAGE, left_at));
    EXPECT(hold.held_until() == left_at + seconds(1) + LINGER);
}

TEST_CASE(a_closed_page_holds_nothing_but_keeps_the_hold_it_left)
{
    WebView::AudibilityHold hold;
    auto now = MonotonicTime::now();
    hold.page_closed(PAGE);
    EXPECT(!hold.is_held(now));

    EXPECT(hold.page_left(PAGE, now));
    hold.page_closed(PAGE);
    EXPECT(hold.is_held(now + LINGER - AK::Duration::from_milliseconds(1)));
    EXPECT(hold.held_until() == now + LINGER);
}
