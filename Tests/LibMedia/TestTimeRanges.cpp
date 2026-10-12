/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/DemuxerScanState.h>
#include <LibMedia/TimeRanges.h>
#include <LibTest/TestCase.h>

using Media::TimeRanges;

static AK::Duration ms(i64 milliseconds)
{
    return AK::Duration::from_milliseconds(milliseconds);
}

TEST_CASE(buffered_sources_without_sources_are_not_complete)
{
    auto report = Media::DemuxerScanState::buffered_ranges_of_sources({});
    EXPECT(report.time_ranges.is_empty());
    EXPECT(!report.resource_is_complete);
}

TEST_CASE(buffered_sources_intersect_ranges_and_completeness)
{
    Array<Media::CombinedScanState, 2> sources;
    sources[0].time_ranges = TimeRanges { { ms(0), ms(800) } };
    sources[0].duration = ms(1000);
    sources[0].resource_is_complete = true;
    sources[1].time_ranges = TimeRanges { { ms(200), ms(1000) } };
    sources[1].duration = ms(1000);

    auto report = Media::DemuxerScanState::buffered_ranges_of_sources(sources);
    EXPECT_EQ(report.time_ranges, (TimeRanges { { ms(200), ms(800) } }));
    EXPECT(!report.resource_is_complete);

    sources[1].resource_is_complete = true;
    report = Media::DemuxerScanState::buffered_ranges_of_sources(sources);
    EXPECT_EQ(report.time_ranges, (TimeRanges { { ms(200), ms(800) } }));
    EXPECT(report.resource_is_complete);

    // Completeness is not inferred from EOF or time coverage, and must include every source.
    sources[0].resource_is_complete = false;
    sources[0].reached_end_of_stream = true;
    report = Media::DemuxerScanState::buffered_ranges_of_sources(sources);
    EXPECT_EQ(report.time_ranges, (TimeRanges { { ms(200), ms(1000) } }));
    EXPECT(!report.resource_is_complete);
}

TEST_CASE(buffered_sources_with_disjoint_ranges_can_still_be_complete)
{
    Array<Media::CombinedScanState, 2> sources;
    sources[0].time_ranges = TimeRanges { { ms(0), ms(100) } };
    sources[1].time_ranges = TimeRanges { { ms(200), ms(300) } };
    for (auto& source : sources) {
        source.duration = ms(300);
        source.resource_is_complete = true;
    }
    auto report = Media::DemuxerScanState::buffered_ranges_of_sources(sources);
    EXPECT(report.time_ranges.is_empty());
    EXPECT(report.resource_is_complete);
}

TEST_CASE(file_buffering_uses_enabled_track_identifiers)
{
    Media::DemuxerScanState state;
    state.duration = ms(1000);
    state.track_buffered_ranges = {
        { 41, TimeRanges { { ms(100), ms(1000) } }, false },
        { 7, TimeRanges { { ms(200), ms(1000) } }, false },
        { 99, TimeRanges { { ms(0), ms(1000) } }, false },
    };
    EXPECT_EQ(state.combined_scan_state([](u64 id) { return id == 99 || id == 41; }).time_ranges, (TimeRanges { { ms(100), ms(1000) } }));
    EXPECT_EQ(state.combined_scan_state([](u64 id) { return id == 7 || id == 99; }).time_ranges, (TimeRanges { { ms(200), ms(1000) } }));
    EXPECT_EQ(state.track_buffered_ranges.size(), 3u);
}

TEST_CASE(file_buffering_includes_empty_tracks)
{
    Vector<Media::BufferedRangesScan> scans {
        { TimeRanges { { ms(0), ms(1000) } }, true, 4 },
        { {}, false, 8 },
    };
    auto state = Media::DemuxerScanState::create_from_track_scans(move(scans), ms(1000), true);
    EXPECT_EQ(state.track_buffered_ranges.size(), 2u);
    state.resource_is_complete = true;
    auto filtered = state.combined_scan_state();
    EXPECT(filtered.time_ranges.is_empty());
    EXPECT(!filtered.reached_end_of_stream);
    EXPECT(filtered.resource_is_complete);
    auto none = state.combined_scan_state([](u64) { return false; });
    EXPECT(none.time_ranges.is_empty());
    EXPECT(!none.reached_end_of_stream);
    EXPECT(none.resource_is_complete);
    EXPECT_EQ(state.combined_scan_state([](u64 id) { return id == 4; }).time_ranges, (TimeRanges { { ms(0), ms(1000) } }));
}

TEST_CASE(inactive_file_tracks_do_not_limit_the_playback_endpoint)
{
    Media::DemuxerScanState state;
    state.duration = ms(1000);
    state.resource_is_complete = false;
    state.track_buffered_ranges = {
        { 1, TimeRanges { { ms(0), ms(900) } }, true },
        { 2, TimeRanges { { ms(200), ms(500) } }, false },
    };
    auto filtered = state.combined_scan_state([](u64 id) { return id == 1; });
    EXPECT(filtered.reached_end_of_stream);
    EXPECT(!filtered.resource_is_complete);
    Array<Media::CombinedScanState, 1> sources { filtered };
    EXPECT_EQ(Media::DemuxerScanState::buffered_ranges_of_sources(sources).time_ranges, (TimeRanges { { ms(0), ms(1000) } }));
}

TEST_CASE(file_tracks_extend_independently_but_mse_tracks_do_not)
{
    Media::DemuxerScanState state;
    state.track_buffered_ranges = {
        { 1, TimeRanges { { ms(0), ms(500) } }, true },
        { 2, TimeRanges { { ms(100), ms(900) } }, false },
    };
    EXPECT_EQ(state.combined_scan_state().time_ranges, (TimeRanges { { ms(100), ms(900) } }));

    state.buffered_ranges_policy = Media::BufferedRangesPolicy::AllTracks;
    Function<bool(u64)> reject_all = [](u64) -> bool { VERIFY_NOT_REACHED(); };
    auto mse = state.combined_scan_state(reject_all);
    EXPECT(!mse.reached_end_of_stream);
    EXPECT_EQ(mse.time_ranges, (TimeRanges { { ms(100), ms(500) } }));
    state.reached_end_of_stream = true;
    mse = state.combined_scan_state(reject_all);
    EXPECT(mse.reached_end_of_stream);
    EXPECT_EQ(mse.time_ranges, (TimeRanges { { ms(100), ms(900) } }));
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
    EXPECT_EQ(ranges.range_at_or_after(ms(500)), (TimeRanges::Range { ms(0), ms(500) }));
    EXPECT_EQ(ranges.range_at_or_after(ms(501)), (TimeRanges::Range { ms(700), ms(1000) }));
}
