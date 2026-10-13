/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/OverlayFade.h>
#include <LibTest/TestCase.h>

using namespace Compositing;

static MonotonicTime after(MonotonicTime start, i64 milliseconds)
{
    return start + AK::Duration::from_milliseconds(milliseconds);
}

TEST_CASE(a_fade_starts_transparent_and_at_rest)
{
    OverlayFade fade;
    EXPECT_EQ(fade.opacity(), 0.0f);
    EXPECT(!fade.is_fading());
}

TEST_CASE(a_fade_eases_in_and_out_to_its_target)
{
    auto start = MonotonicTime::now();
    OverlayFade fade;
    fade.fade_to(1, start);
    EXPECT(fade.is_fading());

    EXPECT(!fade.advance_to(start));
    EXPECT_EQ(fade.opacity(), 0.0f);

    EXPECT(fade.advance_to(after(start, 30)));
    EXPECT_APPROXIMATE_WITH_ERROR(fade.opacity(), 0.104f, 0.0001f);

    fade.advance_to(after(start, 75));
    EXPECT_APPROXIMATE(fade.opacity(), 0.5f);

    fade.advance_to(after(start, 150));
    EXPECT_EQ(fade.opacity(), 1.0f);
    EXPECT(!fade.is_fading());

    EXPECT(!fade.advance_to(after(start, 300)));
    EXPECT_EQ(fade.opacity(), 1.0f);
}

TEST_CASE(a_fade_does_not_go_back_in_time)
{
    auto start = MonotonicTime::now();
    OverlayFade fade;
    fade.fade_to(1, start);

    EXPECT(!fade.advance_to(after(start, -10)));
    EXPECT_EQ(fade.opacity(), 0.0f);
}

TEST_CASE(a_fade_turns_back_from_where_it_has_reached)
{
    auto start = MonotonicTime::now();
    OverlayFade fade;
    fade.fade_to(1, start);
    fade.advance_to(after(start, 75));

    auto turned_at = after(start, 75);
    fade.fade_to(0, turned_at);
    EXPECT(!fade.advance_to(turned_at));
    EXPECT_APPROXIMATE(fade.opacity(), 0.5f);

    fade.advance_to(after(turned_at, 75));
    EXPECT_APPROXIMATE(fade.opacity(), 0.25f);

    fade.advance_to(after(turned_at, 150));
    EXPECT_EQ(fade.opacity(), 0.0f);
    EXPECT(!fade.is_fading());
}

TEST_CASE(fading_to_the_same_target_keeps_the_fade_going)
{
    auto start = MonotonicTime::now();
    OverlayFade fade;
    fade.fade_to(1, start);
    fade.advance_to(after(start, 75));

    fade.fade_to(1, after(start, 75));
    fade.advance_to(after(start, 150));
    EXPECT_EQ(fade.opacity(), 1.0f);
}
