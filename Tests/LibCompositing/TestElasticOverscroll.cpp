/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibCompositing/Scrolling/ElasticOverscroll.h>
#include <LibTest/TestCase.h>

using Compositing::ElasticOverscroll;

static constexpr Gfx::FloatSize scrollport_size { 800, 600 };

static MonotonicTime time_after_release(MonotonicTime released_at, i64 milliseconds)
{
    return released_at + AK::Duration::from_milliseconds(milliseconds);
}

TEST_CASE(pull_stretches_by_a_fraction_of_the_overscroll_in_whole_pixels)
{
    ElasticOverscroll overscroll { scrollport_size };
    EXPECT(overscroll.is_at_rest());

    overscroll.pull_by({ 0, -19 });
    EXPECT_EQ(overscroll.stretch(), Gfx::FloatPoint(0, 0));
    EXPECT(!overscroll.is_at_rest());

    overscroll.pull_by({ 0, -381 });
    EXPECT_EQ(overscroll.stretch(), Gfx::FloatPoint(0, -20));
}

TEST_CASE(pull_never_stretches_past_the_scrollport_length)
{
    ElasticOverscroll overscroll { scrollport_size };
    overscroll.pull_by({ 1'000'000, 1'000'000 });
    EXPECT_EQ(overscroll.stretch(), Gfx::FloatPoint(800, 600));
}

TEST_CASE(delta_back_to_the_scroll_range_relaxes_the_stretch_one_to_one_before_it_scrolls)
{
    ElasticOverscroll overscroll { scrollport_size };
    overscroll.pull_by({ 0, -400 });
    EXPECT_EQ(overscroll.stretch(), Gfx::FloatPoint(0, -20));

    EXPECT_EQ(overscroll.relax_by({ 7, 5 }), Gfx::FloatPoint(7, 0));
    EXPECT_EQ(overscroll.stretch(), Gfx::FloatPoint(0, -15));

    EXPECT_EQ(overscroll.relax_by({ 0, -50 }), Gfx::FloatPoint(0, -50));
    EXPECT_EQ(overscroll.stretch(), Gfx::FloatPoint(0, -15));

    EXPECT_EQ(overscroll.relax_by({ 0, 65 }), Gfx::FloatPoint(0, 50));
    EXPECT(overscroll.is_at_rest());
}

TEST_CASE(released_stretch_springs_back_without_crossing_the_edge)
{
    ElasticOverscroll overscroll { scrollport_size };
    overscroll.pull_by({ 0, -2000 });
    auto released_at = MonotonicTime::now();
    overscroll.release({}, released_at);
    EXPECT(overscroll.is_rebounding());
    EXPECT_EQ(overscroll.stretch(), Gfx::FloatPoint(0, -100));

    // A display tick from before the release must not rewind the rebound.
    EXPECT(overscroll.advance_rebound_to(released_at - AK::Duration::from_milliseconds(5)));
    EXPECT_EQ(overscroll.stretch(), Gfx::FloatPoint(0, -100));

    auto previous_stretch = overscroll.stretch().y();
    bool rebounding = true;
    for (i64 milliseconds = 8; milliseconds <= 1000 && rebounding; milliseconds += 8) {
        rebounding = overscroll.advance_rebound_to(time_after_release(released_at, milliseconds));
        auto stretch = overscroll.stretch().y();
        EXPECT(stretch <= 0);
        EXPECT(stretch >= previous_stretch);
        previous_stretch = stretch;
    }
    EXPECT(!rebounding);
    EXPECT(overscroll.is_at_rest());
    EXPECT_EQ(overscroll.stretch(), Gfx::FloatPoint(0, 0));
}

TEST_CASE(release_velocity_back_to_the_scroll_range_stops_at_the_edge)
{
    ElasticOverscroll overscroll { scrollport_size };
    overscroll.pull_by({ 0, -400 });
    auto released_at = MonotonicTime::now();
    overscroll.release({ 0, 5000 }, released_at);

    bool rebounding = true;
    for (i64 milliseconds = 8; milliseconds <= 1000 && rebounding; milliseconds += 8) {
        rebounding = overscroll.advance_rebound_to(time_after_release(released_at, milliseconds));
        EXPECT(overscroll.stretch().y() <= 0);
    }
    EXPECT(!rebounding);
}

TEST_CASE(momentum_into_an_edge_bounces_out_and_back_within_a_share_of_the_scrollport)
{
    ElasticOverscroll overscroll { scrollport_size };
    overscroll.pull_by({ 0, 4 });
    auto released_at = MonotonicTime::now();
    // The velocity of the unstretched axis is not the velocity of a scroller at an edge.
    overscroll.release({ 1'000'000, 1'000'000 }, released_at);

    float furthest_stretch = 0;
    bool rebounding = true;
    for (i64 milliseconds = 8; milliseconds <= 2000 && rebounding; milliseconds += 8) {
        rebounding = overscroll.advance_rebound_to(time_after_release(released_at, milliseconds));
        EXPECT_EQ(overscroll.stretch().x(), 0.0f);
        furthest_stretch = max(furthest_stretch, overscroll.stretch().y());
    }
    EXPECT(!rebounding);
    EXPECT(furthest_stretch > 60);
    EXPECT(furthest_stretch <= 600 * 0.2f + 1);
}

TEST_CASE(fingers_take_a_rebound_over_from_where_it_has_got_to)
{
    ElasticOverscroll overscroll { scrollport_size };
    overscroll.pull_by({ 0, -2000 });
    auto released_at = MonotonicTime::now();
    overscroll.release({}, released_at);
    EXPECT(overscroll.advance_rebound_to(time_after_release(released_at, 50)));
    auto stretch_when_taken_over = overscroll.stretch();
    EXPECT(stretch_when_taken_over.y() < 0);
    EXPECT(stretch_when_taken_over.y() > -100);

    EXPECT_EQ(overscroll.relax_by({ 0, -40 }), Gfx::FloatPoint(0, -40));
    EXPECT(!overscroll.is_rebounding());
    EXPECT(AK::abs(overscroll.stretch().y() - stretch_when_taken_over.y()) <= 1);

    overscroll.pull_by({ 0, -40 });
    EXPECT(overscroll.stretch().y() < stretch_when_taken_over.y());
}

TEST_CASE(magnified_scroller_stretches_and_springs_back_in_whole_pixels_of_the_screen)
{
    ElasticOverscroll overscroll { scrollport_size, 2 };
    overscroll.pull_by({ 0, -150 });
    EXPECT_EQ(overscroll.stretch(), Gfx::FloatPoint(0, -7.5f));

    overscroll.pull_by({ 0, -1850 });
    auto released_at = MonotonicTime::now();
    overscroll.release({}, released_at);

    float smallest_stretch_presented = 100;
    bool rebounding = true;
    for (i64 milliseconds = 1; milliseconds <= 1000 && rebounding; ++milliseconds) {
        rebounding = overscroll.advance_rebound_to(time_after_release(released_at, milliseconds));
        auto stretch = AK::abs(overscroll.stretch().y());
        EXPECT_EQ(stretch * 2, AK::trunc(stretch * 2));
        if (stretch != 0)
            smallest_stretch_presented = min(smallest_stretch_presented, stretch);
    }
    EXPECT(!rebounding);
    EXPECT_EQ(smallest_stretch_presented, 0.5f);
}
