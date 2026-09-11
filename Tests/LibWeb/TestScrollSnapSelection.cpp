/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/Compositor/ScrollSnapSelection.h>

using namespace Web;
using namespace Web::Compositor;

static SnapAreaIdentity area_identity(i64 node_id)
{
    return { .node_id = UniqueNodeID(node_id), .pseudo_element_type = 0 };
}

static SnapAreaGeometry snap_area(i64 node_id, CSSPixelRect rect, CSS::ScrollSnapAlign align_y, CSS::ScrollSnapAlign align_x = CSS::ScrollSnapAlign::None, bool always_stop = false)
{
    return {
        .identity = area_identity(node_id),
        .rect = rect,
        .align_x = align_x,
        .align_y = align_y,
        .always_stop = always_stop,
    };
}

static SnapContainerGeometry vertical_container(CSS::ScrollSnapStrictness strictness = CSS::ScrollSnapStrictness::Mandatory)
{
    return {
        .snapport = { 0, 0, 200, 200 },
        .min_scroll_offset = { 0, 0 },
        .max_scroll_offset = { 0, 800 },
        .strictness = strictness,
        .axes = { .x = false, .y = true },
        .horizontal_writing_mode = true,
    };
}

static SnapContainerGeometry two_axis_container()
{
    return {
        .snapport = { 0, 0, 200, 200 },
        .min_scroll_offset = { 0, 0 },
        .max_scroll_offset = { 600, 600 },
        .strictness = CSS::ScrollSnapStrictness::Mandatory,
        .axes = { .x = true, .y = true },
        .horizontal_writing_mode = true,
    };
}

// Four 200px tall areas stacked in a 200px tall snapport, each aligned to the start.
static Vector<SnapAreaGeometry> stacked_areas()
{
    Vector<SnapAreaGeometry> areas;
    for (i64 i = 0; i < 4; ++i)
        areas.append(snap_area(i + 1, { 0, CSSPixels(200 * i), 200, 200 }, CSS::ScrollSnapAlign::Start));
    return areas;
}

TEST_CASE(start_alignment_selects_the_nearest_snap_position)
{
    auto destination = select_snap_destination(vertical_container(), stacked_areas(), { 0, 190 });
    EXPECT_EQ(destination.position, CSSPixelPoint(0, 200));
    EXPECT(destination.snapped_y);
    EXPECT(!destination.snapped_x);
    EXPECT(destination.evaluated_y);
    EXPECT(!destination.evaluated_x);
    EXPECT_EQ(destination.snapped_areas.y.size(), 1u);
    EXPECT_EQ(destination.snapped_areas.y.first(), area_identity(2));
    EXPECT(destination.snapped_areas.x.is_empty());
}

TEST_CASE(end_and_center_alignments_align_the_matching_area_edge)
{
    Vector<SnapAreaGeometry> end_aligned { snap_area(1, { 0, 400, 200, 200 }, CSS::ScrollSnapAlign::End) };
    EXPECT_EQ(select_snap_destination(vertical_container(), end_aligned, { 0, 350 }).position, CSSPixelPoint(0, 400));

    Vector<SnapAreaGeometry> center_aligned { snap_area(1, { 0, 300, 200, 200 }, CSS::ScrollSnapAlign::Center) };
    EXPECT_EQ(select_snap_destination(vertical_container(), center_aligned, { 0, 250 }).position, CSSPixelPoint(0, 300));
}

TEST_CASE(proximity_snaps_only_within_a_third_of_the_snapport)
{
    auto container = vertical_container(CSS::ScrollSnapStrictness::Proximity);

    auto far = select_snap_destination(container, stacked_areas(), { 0, 90 });
    EXPECT_EQ(far.position, CSSPixelPoint(0, 90));
    EXPECT(!far.snapped_y);
    EXPECT(far.evaluated_y);

    auto near = select_snap_destination(container, stacked_areas(), { 0, 40 });
    EXPECT_EQ(near.position, CSSPixelPoint(0, 0));
    EXPECT(near.snapped_y);
}

TEST_CASE(a_directional_scroll_skips_the_position_it_starts_from)
{
    SnapSelectionStrategy strategy {
        .type = SnapSelectionStrategy::Type::Direction,
        .start_offset = CSSPixelPoint { 0, 0 },
        .displacement = { 0, 50 },
        .starting_positions_boundary = CSSPixelPoint { 0, 50 },
    };
    auto destination = select_snap_destination(vertical_container(), stacked_areas(), { 0, 50 }, strategy);
    EXPECT_EQ(destination.position, CSSPixelPoint(0, 200));
}

TEST_CASE(a_scroll_snap_stop_always_area_is_not_passed_over)
{
    auto areas = stacked_areas();
    areas[1].always_stop = true;

    SnapSelectionStrategy strategy {
        .type = SnapSelectionStrategy::Type::Direction,
        .start_offset = CSSPixelPoint { 0, 0 },
        .displacement = { 0, 600 },
        .starting_positions_boundary = CSSPixelPoint { 0, 600 },
    };
    auto destination = select_snap_destination(vertical_container(), areas, { 0, 600 }, strategy);
    EXPECT_EQ(destination.position, CSSPixelPoint(0, 200));
    EXPECT_EQ(destination.snapped_areas.y.first(), area_identity(2));
}

TEST_CASE(a_mandatory_container_falls_back_to_the_nearest_position_when_none_is_ahead)
{
    SnapSelectionStrategy strategy {
        .type = SnapSelectionStrategy::Type::Direction,
        .start_offset = CSSPixelPoint { 0, 600 },
        .displacement = { 0, 100 },
        .starting_positions_boundary = CSSPixelPoint { 0, 700 },
    };
    auto destination = select_snap_destination(vertical_container(), stacked_areas(), { 0, 700 }, strategy);
    EXPECT_EQ(destination.position, CSSPixelPoint(0, 600));
    EXPECT(destination.snapped_y);
}

TEST_CASE(an_area_larger_than_the_snapport_offers_every_offset_that_keeps_it_covering_the_snapport)
{
    Vector<SnapAreaGeometry> areas {
        snap_area(1, { 0, 0, 200, 600 }, CSS::ScrollSnapAlign::Start),
        snap_area(2, { 0, 700, 200, 200 }, CSS::ScrollSnapAlign::Start),
    };

    // Every offset from 0 to 400 keeps the tall area covering the snapport.
    EXPECT_EQ(select_snap_destination(vertical_container(), areas, { 0, 250 }).position, CSSPixelPoint(0, 250));
    EXPECT_EQ(select_snap_destination(vertical_container(), areas, { 0, 500 }).position, CSSPixelPoint(0, 400));

    // Offsets short of another area's snap position that is within a snapport of the range's start are not valid
    // snap positions, so the range only resumes at that position.
    Vector<SnapAreaGeometry> interrupted_areas {
        snap_area(1, { 0, 0, 200, 600 }, CSS::ScrollSnapAlign::Start),
        snap_area(2, { 0, 100, 200, 200 }, CSS::ScrollSnapAlign::Start),
    };
    EXPECT_EQ(select_snap_destination(vertical_container(), interrupted_areas, { 0, 60 }).position, CSSPixelPoint(0, 100));
}

TEST_CASE(a_two_axis_selection_follows_one_area_when_the_chosen_offsets_are_not_mutually_visible)
{
    auto container = two_axis_container();
    Vector<SnapAreaGeometry> areas {
        snap_area(1, { 0, 0, 200, 200 }, CSS::ScrollSnapAlign::Start, CSS::ScrollSnapAlign::Start),
        snap_area(2, { 300, 300, 200, 200 }, CSS::ScrollSnapAlign::Start, CSS::ScrollSnapAlign::Start),
    };

    // The nearest x position belongs to the first area and the nearest y position to the second; the area leaving the
    // container nearest its destination wins both axes.
    auto follows_second_area = select_snap_destination(container, areas, { 0, 320 });
    EXPECT_EQ(follows_second_area.position, CSSPixelPoint(300, 300));
    EXPECT_EQ(follows_second_area.snapped_areas.x.first(), area_identity(2));
    EXPECT_EQ(follows_second_area.snapped_areas.y.first(), area_identity(2));

    auto follows_first_area = select_snap_destination(container, areas, { 0, 280 });
    EXPECT_EQ(follows_first_area.position, CSSPixelPoint(0, 0));
    EXPECT_EQ(follows_first_area.snapped_areas.x.first(), area_identity(1));
    EXPECT_EQ(follows_first_area.snapped_areas.y.first(), area_identity(1));
}

TEST_CASE(only_the_axes_a_scroll_traveled_in_are_evaluated)
{
    auto container = two_axis_container();
    SnapSelectionStrategy strategy {
        .type = SnapSelectionStrategy::Type::EndPosition,
        .start_offset = CSSPixelPoint { 50, 0 },
        .displacement = { 0, 10 },
    };
    auto evaluated = axes_to_evaluate(container.axes, strategy);
    EXPECT(!evaluated.x);
    EXPECT(evaluated.y);

    Vector<SnapAreaGeometry> areas {
        snap_area(1, { 0, 0, 200, 200 }, CSS::ScrollSnapAlign::Start, CSS::ScrollSnapAlign::Start),
    };
    auto destination = select_snap_destination(container, areas, { 50, 10 }, strategy);
    EXPECT_EQ(destination.position, CSSPixelPoint(50, 0));
    EXPECT(!destination.evaluated_x);
    EXPECT(destination.evaluated_y);
    EXPECT(destination.snapped_areas.x.is_empty());
    EXPECT_EQ(destination.snapped_areas.y.size(), 1u);
}
