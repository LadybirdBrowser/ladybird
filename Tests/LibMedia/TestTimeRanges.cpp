/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/TimeRanges.h>
#include <LibTest/TestCase.h>

using Media::TimeRanges;

static AK::Duration ms(i64 milliseconds)
{
    return AK::Duration::from_milliseconds(milliseconds);
}

// add_range()

TEST_CASE(add_range_to_empty)
{
    TimeRanges ranges;
    ranges.add_range(ms(100), ms(200));
    EXPECT_EQ(ranges, (TimeRanges { { ms(100), ms(200) } }));
}

TEST_CASE(add_range_invalid_is_no_op)
{
    TimeRanges ranges;
    ranges.add_range(ms(200), ms(100));
    EXPECT(ranges.is_empty());
    ranges.add_range(ms(100), ms(100));
    EXPECT(ranges.is_empty());
}

TEST_CASE(add_range_before_existing)
{
    TimeRanges ranges;
    ranges.add_range(ms(500), ms(1000));
    ranges.add_range(ms(100), ms(300));
    EXPECT_EQ(ranges, (TimeRanges { { ms(100), ms(300) }, { ms(500), ms(1000) } }));
}

TEST_CASE(add_range_after_existing)
{
    TimeRanges ranges;
    ranges.add_range(ms(100), ms(300));
    ranges.add_range(ms(500), ms(1000));
    EXPECT_EQ(ranges, (TimeRanges { { ms(100), ms(300) }, { ms(500), ms(1000) } }));
}

TEST_CASE(add_range_adjacent_extends_end)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(500));
    ranges.add_range(ms(500), ms(600));
    EXPECT_EQ(ranges, (TimeRanges { { ms(0), ms(600) } }));
}

TEST_CASE(add_range_adjacent_extends_start)
{
    TimeRanges ranges;
    ranges.add_range(ms(500), ms(1000));
    ranges.add_range(ms(300), ms(500));
    EXPECT_EQ(ranges, (TimeRanges { { ms(300), ms(1000) } }));
}

TEST_CASE(add_range_overlapping_extends_end)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(500));
    ranges.add_range(ms(300), ms(700));
    EXPECT_EQ(ranges, (TimeRanges { { ms(0), ms(700) } }));
}

TEST_CASE(add_range_overlapping_extends_start)
{
    TimeRanges ranges;
    ranges.add_range(ms(500), ms(1000));
    ranges.add_range(ms(300), ms(700));
    EXPECT_EQ(ranges, (TimeRanges { { ms(300), ms(1000) } }));
}

TEST_CASE(add_range_inside_existing_is_no_op)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(1000));
    ranges.add_range(ms(300), ms(700));
    EXPECT_EQ(ranges, (TimeRanges { { ms(0), ms(1000) } }));
}

TEST_CASE(add_range_enclosing_existing)
{
    TimeRanges ranges;
    ranges.add_range(ms(300), ms(700));
    ranges.add_range(ms(0), ms(1000));
    EXPECT_EQ(ranges, (TimeRanges { { ms(0), ms(1000) } }));
}

TEST_CASE(add_range_bridges_two_ranges)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(500));
    ranges.add_range(ms(700), ms(1000));
    ranges.add_range(ms(400), ms(800));
    EXPECT_EQ(ranges, (TimeRanges { { ms(0), ms(1000) } }));
}

TEST_CASE(add_range_bridges_three_ranges)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(200));
    ranges.add_range(ms(400), ms(600));
    ranges.add_range(ms(800), ms(1000));
    ranges.add_range(ms(100), ms(900));
    EXPECT_EQ(ranges, (TimeRanges { { ms(0), ms(1000) } }));
}

TEST_CASE(add_range_between_existing_no_overlap)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(200));
    ranges.add_range(ms(800), ms(1000));
    ranges.add_range(ms(400), ms(600));
    EXPECT_EQ(ranges, (TimeRanges { { ms(0), ms(200) }, { ms(400), ms(600) }, { ms(800), ms(1000) } }));
}

TEST_CASE(add_range_sequential_frames)
{
    TimeRanges ranges;
    for (i64 i = 0; i < 100; i++)
        ranges.add_range(ms(i * 33), ms((i + 1) * 33));
    EXPECT_EQ(ranges, (TimeRanges { { ms(0), ms(3300) } }));
}

// remove_range()

TEST_CASE(remove_range_from_empty)
{
    TimeRanges ranges;
    ranges.remove_range(ms(0), ms(100));
    EXPECT(ranges.is_empty());
}

TEST_CASE(remove_range_no_overlap)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(500));
    ranges.remove_range(ms(600), ms(700));
    EXPECT_EQ(ranges, (TimeRanges { { ms(0), ms(500) } }));
}

TEST_CASE(remove_range_entire_range)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(500));
    ranges.remove_range(ms(0), ms(500));
    EXPECT(ranges.is_empty());
}

TEST_CASE(remove_range_splits_range)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(1000));
    ranges.remove_range(ms(300), ms(700));
    EXPECT_EQ(ranges, (TimeRanges { { ms(0), ms(300) }, { ms(700), ms(1000) } }));
}

TEST_CASE(remove_range_trims_start)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(1000));
    ranges.remove_range(ms(0), ms(500));
    EXPECT_EQ(ranges, (TimeRanges { { ms(500), ms(1000) } }));
}

TEST_CASE(remove_range_trims_end)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(1000));
    ranges.remove_range(ms(500), ms(1000));
    EXPECT_EQ(ranges, (TimeRanges { { ms(0), ms(500) } }));
}

TEST_CASE(remove_range_across_multiple_ranges)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(200));
    ranges.add_range(ms(400), ms(600));
    ranges.add_range(ms(800), ms(1000));
    ranges.remove_range(ms(100), ms(900));
    EXPECT_EQ(ranges, (TimeRanges { { ms(0), ms(100) }, { ms(900), ms(1000) } }));
}

// highest_end_time()

TEST_CASE(highest_end_time_empty)
{
    TimeRanges ranges;
    EXPECT_EQ(ranges.highest_end_time(), AK::Duration::zero());
}

TEST_CASE(highest_end_time)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(500));
    ranges.add_range(ms(700), ms(1000));
    EXPECT_EQ(ranges.highest_end_time(), ms(1000));
}

// coalesced()

TEST_CASE(coalesced_no_change)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(500));
    ranges.add_range(ms(1000), ms(1500));
    EXPECT_EQ(ranges.coalesced(ms(100)), (TimeRanges { { ms(0), ms(500) }, { ms(1000), ms(1500) } }));
}

TEST_CASE(coalesced_merges_small_gap)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(500));
    ranges.add_range(ms(550), ms(1000));
    EXPECT_EQ(ranges.coalesced(ms(100)), (TimeRanges { { ms(0), ms(1000) } }));
}

TEST_CASE(coalesced_merges_exact_threshold)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(500));
    ranges.add_range(ms(600), ms(1000));
    EXPECT_EQ(ranges.coalesced(ms(100)), (TimeRanges { { ms(0), ms(1000) } }));
}

// intersection()

TEST_CASE(intersection_no_overlap)
{
    TimeRanges a;
    TimeRanges b;
    a.add_range(ms(0), ms(500));
    b.add_range(ms(600), ms(1000));
    EXPECT(a.intersection(b).is_empty());
}

TEST_CASE(intersection_partial_overlap)
{
    TimeRanges a;
    TimeRanges b;
    a.add_range(ms(0), ms(700));
    b.add_range(ms(300), ms(1000));
    EXPECT_EQ(a.intersection(b), (TimeRanges { { ms(300), ms(700) } }));
}

TEST_CASE(intersection_one_inside_other)
{
    TimeRanges a;
    TimeRanges b;
    a.add_range(ms(0), ms(1000));
    b.add_range(ms(300), ms(700));
    EXPECT_EQ(a.intersection(b), (TimeRanges { { ms(300), ms(700) } }));
}

TEST_CASE(intersection_multiple_ranges)
{
    TimeRanges a;
    TimeRanges b;
    a.add_range(ms(0), ms(500));
    a.add_range(ms(700), ms(1000));
    b.add_range(ms(300), ms(800));
    EXPECT_EQ(a.intersection(b), (TimeRanges { { ms(300), ms(500) }, { ms(700), ms(800) } }));
}

TEST_CASE(intersection_with_empty)
{
    TimeRanges a;
    TimeRanges b;
    a.add_range(ms(0), ms(1000));
    EXPECT(a.intersection(b).is_empty());
}

// range_at_or_after()

TEST_CASE(range_at_or_after_empty)
{
    TimeRanges ranges;
    EXPECT(!ranges.range_at_or_after(ms(0)).has_value());
}

TEST_CASE(range_at_or_after_inside_range)
{
    TimeRanges ranges;
    ranges.add_range(ms(100), ms(500));
    EXPECT_EQ(ranges.range_at_or_after(ms(300)), (TimeRanges::Range { ms(100), ms(500) }));
}

TEST_CASE(range_at_or_after_before_range)
{
    TimeRanges ranges;
    ranges.add_range(ms(100), ms(500));
    EXPECT_EQ(ranges.range_at_or_after(ms(0)), (TimeRanges::Range { ms(100), ms(500) }));
}

TEST_CASE(range_at_or_after_after_all_ranges)
{
    TimeRanges ranges;
    ranges.add_range(ms(100), ms(500));
    EXPECT(!ranges.range_at_or_after(ms(600)).has_value());
}

TEST_CASE(range_at_or_after_in_gap_returns_next)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(200));
    ranges.add_range(ms(500), ms(700));
    EXPECT_EQ(ranges.range_at_or_after(ms(300)), (TimeRanges::Range { ms(500), ms(700) }));
}

TEST_CASE(range_at_or_after_at_range_boundary)
{
    TimeRanges ranges;
    ranges.add_range(ms(0), ms(500));
    ranges.add_range(ms(700), ms(1000));
    EXPECT_EQ(ranges.range_at_or_after(ms(0)), (TimeRanges::Range { ms(0), ms(500) }));
    EXPECT_EQ(ranges.range_at_or_after(ms(500)), (TimeRanges::Range { ms(700), ms(1000) }));
}
