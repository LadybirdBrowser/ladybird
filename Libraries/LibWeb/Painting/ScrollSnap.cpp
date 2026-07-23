/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Math.h>
#include <AK/QuickSort.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ScrollSnap.h>
#include <LibWeb/Painting/Scrolling.h>

namespace Web::Painting {

// A closed range of scroll offsets that are all valid snap positions.
struct CoveringRange {
    CSSPixels start;
    CSSPixels end;
};

// A candidate scroll offset in one axis, produced by aligning a snap area with the snapport.
struct SnapPositionCandidate {
    CSSPixels offset;
    SnapAreaReference area {};

    Vector<CoveringRange, 1> covering_ranges;

    // The open range of scroll offsets in the container's other axis at which the snap area overlaps the snapport.
    CSSPixels cross_axis_visible_range_start { 0 };
    CSSPixels cross_axis_visible_range_end { 0 };

    bool always_stop { false };
};

struct SnapAxisCandidates {
    Vector<SnapPositionCandidate> x_candidates;
    Vector<SnapPositionCandidate> y_candidates;
};

static SnapAreaReference snap_area_reference_for(Layout::Node const& snap_area)
{
    if (auto pseudo_element = snap_area.generated_for_pseudo_element(); pseudo_element.has_value())
        return { snap_area.pseudo_element_generator().ptr(), pseudo_element };
    return { as_if<DOM::Element>(snap_area.dom_node()), {} };
}

static Layout::NodeWithStyle const* style_source_for_snap_container(Layout::Node const& snap_container)
{
    if (snap_container.is_viewport()) {
        auto const* document_element = snap_container.document().document_element();
        if (!document_element)
            return nullptr;
        return document_element->unsafe_layout_node();
    }
    return &as<Layout::NodeWithStyle>(snap_container);
}

static bool has_snap_alignment(CSS::ScrollSnapAlignData alignment)
{
    return alignment.block_alignment != CSS::ScrollSnapAlign::None || alignment.inline_alignment != CSS::ScrollSnapAlign::None;
}

static bool is_captured_by_snap_container(Layout::Node const& snap_area, Layout::Node const& snap_container)
{
    for (auto const* containing_block = snap_area.containing_block(); containing_block; containing_block = containing_block->containing_block()) {
        if (containing_block == &snap_container)
            return true;
        // The box whose overflow was propagated to the viewport is left with a used overflow of visible, so it is not
        // a scroll container and cannot capture snap areas of its own.
        if (containing_block->is_scroll_container())
            return false;
    }
    return false;
}

template<typename Callback>
static void for_each_descendant_snap_area(Layout::Node const& parent, Layout::Node const& snap_container, Callback const& callback)
{
    parent.for_each_child([&](Layout::Node const& child) {
        // Snap areas are captured by the nearest scroll container in their containing block chain, so areas inside a
        // nested scroll container may still belong to an outer container when they are positioned.
        auto const* child_with_style = as_if<Layout::NodeWithStyle>(child);
        if (child_with_style && has_committed_box(child) && has_snap_alignment(child_with_style->scroll_snap_align()) && is_captured_by_snap_container(child, snap_container))
            callback(*child_with_style);
        for_each_descendant_snap_area(child, snap_container, callback);
        return IterationDecision::Continue;
    });
}

// https://drafts.csswg.org/css-scroll-snap-1/#scroll-margin
static CSSPixelRect snap_area_rect(Layout::NodeWithStyle const& snap_area, Layout::Node const& snap_container)
{
    // The scroll snap area is determined by taking the transformed border box, finding its rectangular bounding box
    // (axis-aligned in the scroll container's coordinate space), then adding the specified outsets.

    // NB: A snap area is captured by the nearest scroll container in its containing block chain, so the boxes between
    //     an area and its container contribute transforms only, and mapping the border box through each of them in
    //     turn lands it in the container's coordinate space.
    auto rect = apply_css_transform_to_rect(snap_area, absolute_border_box_rect(snap_area));
    for (auto const* containing_block = snap_area.containing_block(); containing_block && containing_block != &snap_container; containing_block = containing_block->containing_block())
        rect = apply_css_transform_to_rect(*containing_block, rect);

    auto const& scroll_margin = snap_area.scroll_margin();
    rect.inflate(
        scroll_margin.top().to_px_or_zero(CSSPixels { 0 }),
        scroll_margin.right().to_px_or_zero(CSSPixels { 0 }),
        scroll_margin.bottom().to_px_or_zero(CSSPixels { 0 }),
        scroll_margin.left().to_px_or_zero(CSSPixels { 0 }));
    return rect;
}

struct SnapAxisGeometry {
    CSSPixels snapport_start;
    CSSPixels snapport_size;
    CSSPixels min_offset;
    CSSPixels max_offset;
};

struct PhysicalSnapAlignment {
    CSS::ScrollSnapAlign x;
    CSS::ScrollSnapAlign y;
};

// https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-align
static PhysicalSnapAlignment physical_snap_alignment(CSS::ScrollSnapAlignData alignment, Layout::NodeWithStyle const& writing_mode_source)
{
    // The two values specify the snapping alignment in the block axis and inline axis, respectively, as determined by the
    // snap container's writing mode.

    // NB: start and end name the edges an axis begins and ends at, which are its lesser and greater physical edges
    //     only while the axis runs in the same direction as the physical one.
    auto alignment_along_axis = [](CSS::ScrollSnapAlign axis_alignment, bool axis_is_reverse) {
        if (!axis_is_reverse)
            return axis_alignment;
        switch (axis_alignment) {
        case CSS::ScrollSnapAlign::Start:
            return CSS::ScrollSnapAlign::End;
        case CSS::ScrollSnapAlign::End:
            return CSS::ScrollSnapAlign::Start;
        case CSS::ScrollSnapAlign::None:
        case CSS::ScrollSnapAlign::Center:
            return axis_alignment;
        }
        VERIFY_NOT_REACHED();
    };

    bool horizontal_writing_mode = writing_mode_source.writing_mode() == CSS::WritingMode::HorizontalTb;
    auto x_alignment = horizontal_writing_mode ? alignment.inline_alignment : alignment.block_alignment;
    auto y_alignment = horizontal_writing_mode ? alignment.block_alignment : alignment.inline_alignment;
    bool x_axis_is_reverse = horizontal_writing_mode ? writing_mode_source.inline_axis_is_reverse() : writing_mode_source.block_axis_is_reverse();
    bool y_axis_is_reverse = horizontal_writing_mode ? writing_mode_source.block_axis_is_reverse() : writing_mode_source.inline_axis_is_reverse();
    return {
        .x = alignment_along_axis(x_alignment, x_axis_is_reverse),
        .y = alignment_along_axis(y_alignment, y_axis_is_reverse),
    };
}

static Optional<SnapPositionCandidate> snap_position_candidate_for_axis(CSS::ScrollSnapAlign alignment, CSSPixels area_start, CSSPixels area_size, SnapAxisGeometry const& geometry)
{
    CSSPixels offset;
    switch (alignment) {
    case CSS::ScrollSnapAlign::None:
        return {};
    case CSS::ScrollSnapAlign::Start:
        offset = area_start - geometry.snapport_start;
        break;
    case CSS::ScrollSnapAlign::End:
        offset = area_start + area_size - (geometry.snapport_start + geometry.snapport_size);
        break;
    case CSS::ScrollSnapAlign::Center:
        offset = area_start + area_size / 2 - (geometry.snapport_start + geometry.snapport_size / 2);
        break;
    }

    // https://drafts.csswg.org/css-scroll-snap-1/#unreachable
    // If a snap position is unreachable as specified, such that aligning to it would require scrolling the scroll
    // container's viewport past the edge of its scrollable overflow area, the used snap position for this snap area is
    // the position resulting from scrolling as much as possible in each relevant axis toward the desired snap position.
    SnapPositionCandidate candidate { .offset = clamp(offset, geometry.min_offset, geometry.max_offset), .covering_ranges = {} };

    if (area_size > geometry.snapport_size) {
        candidate.covering_ranges.append({
            .start = clamp(area_start - geometry.snapport_start, geometry.min_offset, geometry.max_offset),
            .end = clamp(area_start + area_size - (geometry.snapport_start + geometry.snapport_size), geometry.min_offset, geometry.max_offset),
        });
    }

    return candidate;
}

static void restrict_covering_ranges_to_valid_snap_positions(Vector<SnapPositionCandidate>& candidates, CSSPixels snapport_size)
{
    Vector<CSSPixels> snap_positions;
    snap_positions.ensure_capacity(candidates.size());
    for (auto const& candidate : candidates)
        snap_positions.unchecked_append(candidate.offset);
    quick_sort(snap_positions);

    for (auto& candidate : candidates) {
        if (candidate.covering_ranges.is_empty())
            continue;

        auto covering_range = candidate.covering_ranges.first();
        candidate.covering_ranges.clear_with_capacity();

        auto append_valid_offsets_between = [&](CSSPixels start, CSSPixels end) {
            start = max(start, covering_range.start);
            end = min(end, covering_range.end);
            if (start <= end)
                candidate.covering_ranges.append({ .start = start, .end = end });
        };

        append_valid_offsets_between(covering_range.start, snap_positions.first());
        for (size_t i = 1; i < snap_positions.size(); ++i) {
            if (snap_positions[i] - snap_positions[i - 1] > snapport_size)
                append_valid_offsets_between(snap_positions[i - 1], snap_positions[i]);
        }
        append_valid_offsets_between(snap_positions.last(), covering_range.end);
    }
}

// AD-HOC: Offsets within one pixel of the offset a scroll travels from count as being at it, so that a fractional
//         scroll offset cannot re-select the snap position the scroll started from. This matches other engines.
static constexpr CSSPixels SNAP_POSITION_BOUNDARY_TOLERANCE = 1;

static bool is_beyond_in_direction(CSSPixels offset, CSSPixels boundary, CSSPixels direction)
{
    if (direction > 0)
        return offset >= boundary + SNAP_POSITION_BOUNDARY_TOLERANCE;
    return offset <= boundary - SNAP_POSITION_BOUNDARY_TOLERANCE;
}

static bool is_at_or_beyond_in_direction(CSSPixels offset, CSSPixels boundary, CSSPixels direction)
{
    if (direction > 0)
        return offset >= boundary;
    return offset <= boundary;
}

struct SnapAxisSelection {
    CSSPixels destination;
    CSSPixels start;
    CSSPixels direction;
    Optional<CSSPixels> starting_positions_boundary;
};

static bool snap_area_is_visible_at_cross_axis_offset(SnapPositionCandidate const& candidate, CSSPixels cross_axis_offset)
{
    return cross_axis_offset > candidate.cross_axis_visible_range_start && cross_axis_offset < candidate.cross_axis_visible_range_end;
}

// Every offset within a covering range is a valid snap position of the area that contributes it.
static bool candidate_has_snap_position_at(SnapPositionCandidate const& candidate, CSSPixels offset)
{
    if (candidate.offset == offset)
        return true;
    return any_of(candidate.covering_ranges, [&](auto const& covering_range) { return offset >= covering_range.start && offset <= covering_range.end; });
}

static bool chosen_offset_is_visible_at_cross_axis_offset(Vector<SnapPositionCandidate> const& candidates, CSSPixels offset, CSSPixels cross_axis_offset)
{
    return any_of(candidates, [&](auto const& candidate) {
        return snap_area_is_visible_at_cross_axis_offset(candidate, cross_axis_offset) && candidate_has_snap_position_at(candidate, offset);
    });
}

static SnapDestination snap_destination_for(CSSPixelPoint unsnapped_destination, Optional<CSSPixels> x_offset, Optional<CSSPixels> y_offset)
{
    return {
        .position = { x_offset.value_or(unsnapped_destination.x()), y_offset.value_or(unsnapped_destination.y()) },
        .snapped_x = x_offset.has_value(),
        .snapped_y = y_offset.has_value(),
    };
}

static bool chosen_offsets_are_mutually_visible(SnapAxisCandidates const& candidates, CSSPixels x_offset, CSSPixels y_offset)
{
    return chosen_offset_is_visible_at_cross_axis_offset(candidates.x_candidates, x_offset, y_offset)
        && chosen_offset_is_visible_at_cross_axis_offset(candidates.y_candidates, y_offset, x_offset);
}

struct SnapAxisChoice {
    CSSPixels offset;
    SnapAreaReference area;
};

static Optional<SnapAxisChoice> choose_snap_offset_for_axis(Vector<SnapPositionCandidate> const& candidates, SnapAxisSelection const& selection, CSSPixels snapport_size, CSS::ScrollSnapStrictness strictness, Optional<CSSPixels> cross_axis_offset, Optional<SnapAreaReference> only_area = {})
{
    // AD-HOC: The parameters under which a proximity snap container snaps are left to the user agent. Match the
    //         threshold used by other engines, one third of the snapport size in the snapping axis.
    auto proximity_range = snapport_size / 3;

    Optional<SnapAxisChoice> best_choice;
    CSSPixels best_distance = 0;
    auto consider_candidate = [&](CSSPixels offset, SnapPositionCandidate const& candidate) {
        auto distance = abs(offset - selection.destination);
        if (strictness == CSS::ScrollSnapStrictness::Proximity && distance > proximity_range)
            return;
        if (!best_choice.has_value() || distance < best_distance) {
            best_choice = SnapAxisChoice { offset, candidate.area };
            best_distance = distance;
        }
    };

    Optional<SnapAxisChoice> first_always_stop_choice;
    auto track_always_stop_candidate = [&](SnapPositionCandidate const& candidate) {
        if (!first_always_stop_choice.has_value() || abs(candidate.offset - selection.start) < abs(first_always_stop_choice->offset - selection.start))
            first_always_stop_choice = SnapAxisChoice { candidate.offset, candidate.area };
    };

    for (auto const& candidate : candidates) {
        if (only_area.has_value() && candidate.area != *only_area)
            continue;

        // https://drafts.csswg.org/css-scroll-snap-1/#snap-scope
        // Since the purpose of scroll snapping is to align content within the scrollport for optimal viewing, a
        // scroll position cannot be considered a valid snap position if snapping to it would leave the contributing
        // snap area entirely outside the snapport, even if it otherwise satisfies the required alignment of the snap
        // area.
        if (cross_axis_offset.has_value() && !snap_area_is_visible_at_cross_axis_offset(candidate, *cross_axis_offset))
            continue;

        if (selection.direction != 0 && candidate.always_stop && is_beyond_in_direction(candidate.offset, selection.start, selection.direction))
            track_always_stop_candidate(candidate);

        if (!selection.starting_positions_boundary.has_value()
            || (is_beyond_in_direction(candidate.offset, selection.start, selection.direction)
                && is_at_or_beyond_in_direction(candidate.offset, *selection.starting_positions_boundary, selection.direction)))
            consider_candidate(candidate.offset, candidate);

        // NB: Every offset in a covering range is a valid snap position, so such a range contributes the offset in it
        //     nearest the destination. A relative scroll may not select the part of a range it has already traveled
        //     past, which is the part at or behind the offset it started from.
        // FIXME: Limit covering ranges to the offsets at which no snap area with scroll-snap-stop: always has entered
        //        the snapport yet, so a scroll within a covering snap area still stops ahead of such an area.
        for (auto const& covering_range : candidate.covering_ranges) {
            auto range_start = covering_range.start;
            auto range_end = covering_range.end;
            if (selection.starting_positions_boundary.has_value()) {
                if (selection.direction > 0) {
                    range_start = max(range_start, selection.start + SNAP_POSITION_BOUNDARY_TOLERANCE);
                } else if (selection.direction < 0) {
                    range_end = min(range_end, selection.start - SNAP_POSITION_BOUNDARY_TOLERANCE);
                }
            }
            if (range_start > range_end)
                continue;

            consider_candidate(clamp(selection.destination, range_start, range_end), candidate);
        }
    }

    // https://drafts.csswg.org/css-scroll-snap-1/#valdef-scroll-snap-type-mandatory
    // If a valid snap position exists then the scroll container must snap at the termination of a scroll (if none
    // exist then no snapping occurs).
    // NB: A mandatory container whose scroll has no snap position ahead of it in the direction of travel therefore
    //     falls back to the snap position nearest the destination.
    if (!best_choice.has_value() && selection.starting_positions_boundary.has_value() && strictness == CSS::ScrollSnapStrictness::Mandatory) {
        SnapAxisSelection fallback_selection {
            .destination = selection.destination,
            .start = selection.destination,
            .direction = 0,
            .starting_positions_boundary = {},
        };
        best_choice = choose_snap_offset_for_axis(candidates, fallback_selection, snapport_size, strictness, cross_axis_offset, only_area);
    }

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-stop
    // always
    //     The scroll container must not pass over a snap position defined by this element during the execution of a
    //     scrolling operation; it must instead snap to the first of this element's snap positions.
    if (best_choice.has_value() && first_always_stop_choice.has_value()) {
        bool always_stop_position_is_passed_over = selection.direction > 0
            ? best_choice->offset > first_always_stop_choice->offset
            : best_choice->offset < first_always_stop_choice->offset;
        if (always_stop_position_is_passed_over)
            best_choice = first_always_stop_choice;
    }

    return best_choice;
}

// https://drafts.csswg.org/css-scroll-snap-1/#snap-axis
SnapAxes snap_axes_of_scroll_container(Layout::Node const& snap_container)
{
    auto const* style_source = style_source_for_snap_container(snap_container);
    if (!style_source)
        return {};

    auto snap_type = style_source->scroll_snap_type();
    if (snap_type.strictness == CSS::ScrollSnapStrictness::None)
        return {};

    bool horizontal_writing_mode = style_source->writing_mode() == CSS::WritingMode::HorizontalTb;
    switch (snap_type.axis) {
    case CSS::ScrollSnapAxis::X:
        return { .x = true, .y = false };
    case CSS::ScrollSnapAxis::Y:
        return { .x = false, .y = true };
    case CSS::ScrollSnapAxis::Inline:
        return { .x = horizontal_writing_mode, .y = !horizontal_writing_mode };
    case CSS::ScrollSnapAxis::Block:
        return { .x = !horizontal_writing_mode, .y = horizontal_writing_mode };
    case CSS::ScrollSnapAxis::Both:
        return { .x = true, .y = true };
    }
    VERIFY_NOT_REACHED();
}

struct SnapCandidateCollection {
    CSSPixelRect snapport;
    SnapAxisGeometry x_geometry;
    SnapAxisGeometry y_geometry;
    CSS::ScrollSnapType snap_type;
    SnapAxisCandidates candidates;
};

static Optional<SnapCandidateCollection> collect_snap_position_candidates(Layout::Node const& snap_container, bool collect_x, bool collect_y)
{
    auto const* style_source = style_source_for_snap_container(snap_container);
    if (!style_source)
        return {};

    auto snap_type = style_source->scroll_snap_type();

    auto scrollable_overflow_rect = Painting::scrollable_overflow_rect(snap_container);
    if (!scrollable_overflow_rect.has_value())
        return {};

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-padding
    // For a scroll snap container this region also defines the scroll snapport—the area of the scrollport that is
    // used as the alignment container for the scroll snap areas when calculating snap positions.
    auto snapport = scroll_snapport_rect(snap_container);

    auto min_scroll_offset = minimum_scroll_offset(snap_container);
    auto max_scroll_offset = maximum_scroll_offset(snap_container);
    SnapCandidateCollection collection {
        .snapport = snapport,
        .x_geometry = {
            .snapport_start = snapport.left(),
            .snapport_size = snapport.width(),
            .min_offset = min_scroll_offset.x(),
            .max_offset = max_scroll_offset.x(),
        },
        .y_geometry = {
            .snapport_start = snapport.top(),
            .snapport_size = snapport.height(),
            .min_offset = min_scroll_offset.y(),
            .max_offset = max_scroll_offset.y(),
        },
        .snap_type = snap_type,
        .candidates = {},
    };

    for_each_descendant_snap_area(snap_container, snap_container, [&](Layout::NodeWithStyle const& snap_area) {
        auto always_stop = snap_area.scroll_snap_stop() == CSS::ScrollSnapStop::Always;
        auto area_rect = snap_area_rect(snap_area, snap_container);
        auto area_reference = snap_area_reference_for(snap_area);

        // https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-align
        // Start and end alignments are resolved with respect to the writing mode of the snap container unless the
        // scroll snap area is larger than the snapport, in which case they are resolved with respect to the writing
        // mode of the box itself.
        // NB: The size the area is compared in is the one it lays its content out along, so that an area whose
        //     content no longer fits the snapport aligns the edge that content begins at. This matches other engines.
        bool area_is_larger_than_snapport = snap_area.writing_mode() == CSS::WritingMode::HorizontalTb
            ? area_rect.width() > snapport.width()
            : area_rect.height() > snapport.height();
        auto alignment = physical_snap_alignment(snap_area.scroll_snap_align(), area_is_larger_than_snapport ? snap_area : *style_source);

        if (collect_x) {
            if (auto candidate = snap_position_candidate_for_axis(alignment.x, area_rect.left(), area_rect.width(), collection.x_geometry); candidate.has_value()) {
                candidate->area = area_reference;
                candidate->always_stop = always_stop;
                candidate->cross_axis_visible_range_start = area_rect.top() - collection.y_geometry.snapport_start - collection.y_geometry.snapport_size;
                candidate->cross_axis_visible_range_end = area_rect.bottom() - collection.y_geometry.snapport_start;
                collection.candidates.x_candidates.append(*candidate);
            }
        }
        if (collect_y) {
            if (auto candidate = snap_position_candidate_for_axis(alignment.y, area_rect.top(), area_rect.height(), collection.y_geometry); candidate.has_value()) {
                candidate->area = area_reference;
                candidate->always_stop = always_stop;
                candidate->cross_axis_visible_range_start = area_rect.left() - collection.x_geometry.snapport_start - collection.x_geometry.snapport_size;
                candidate->cross_axis_visible_range_end = area_rect.right() - collection.x_geometry.snapport_start;
                collection.candidates.y_candidates.append(*candidate);
            }
        }
    });

    if (collect_x)
        restrict_covering_ranges_to_valid_snap_positions(collection.candidates.x_candidates, collection.x_geometry.snapport_size);
    if (collect_y)
        restrict_covering_ranges_to_valid_snap_positions(collection.candidates.y_candidates, collection.y_geometry.snapport_size);

    return collection;
}

// https://drafts.csswg.org/css-scroll-snap-1/#choosing
SnapDestination adjust_scroll_destination_for_snapping(Layout::Node const& snap_container, CSSPixelPoint destination, SnapSelectionStrategy const& strategy)
{
    auto snap_axes = snap_axes_of_scroll_container(snap_container);
    if (snap_axes.is_empty())
        return { destination };

    // NB: A scroll selects a snap position only in the axes it traveled in, so that the offset of an axis its input
    //     never moved is left where it is. A scroll that traveled in no axis, such as one with only an intended end
    //     position, selects a snap position in every axis the container snaps in.
    auto snaps_in_axis = [&](bool container_snaps_in_axis, CSSPixels axis_displacement) {
        return container_snaps_in_axis && (strategy.displacement.is_zero() || axis_displacement != 0);
    };
    bool snaps_x = snaps_in_axis(snap_axes.x, strategy.displacement.x());
    bool snaps_y = snaps_in_axis(snap_axes.y, strategy.displacement.y());
    if (!snaps_x && !snaps_y)
        return { destination };

    auto collection = collect_snap_position_candidates(snap_container, snaps_x, snaps_y);
    if (!collection.has_value())
        return { destination };

    auto snap_type = collection->snap_type;
    auto const& snapport = collection->snapport;
    auto const& candidates = collection->candidates;

    auto axis_selection = [&](CSSPixels axis_destination, CSSPixels axis_displacement, Optional<CSSPixels> axis_start, Optional<CSSPixels> axis_boundary) {
        if (!axis_start.has_value() || axis_displacement == 0) {
            return SnapAxisSelection {
                .destination = axis_destination,
                .start = axis_destination,
                .direction = 0,
                .starting_positions_boundary = {},
            };
        }

        Optional<CSSPixels> boundary;
        if (strategy.type != SnapSelectionStrategy::Type::EndPosition)
            boundary = axis_boundary.value_or(*axis_start);

        return SnapAxisSelection {
            .destination = axis_destination,
            .start = *axis_start,
            .direction = axis_displacement,
            .starting_positions_boundary = boundary,
        };
    };
    auto axis_of = [](Optional<CSSPixelPoint> const& offset, bool horizontal) -> Optional<CSSPixels> {
        if (!offset.has_value())
            return {};
        return horizontal ? offset->x() : offset->y();
    };
    auto x_selection = axis_selection(destination.x(), strategy.displacement.x(), axis_of(strategy.start_offset, true), axis_of(strategy.starting_positions_boundary, true));
    auto y_selection = axis_selection(destination.y(), strategy.displacement.y(), axis_of(strategy.start_offset, false), axis_of(strategy.starting_positions_boundary, false));

    auto choose_x = [&](Optional<CSSPixels> cross_axis_offset, Optional<SnapAreaReference> only_area = {}) {
        return choose_snap_offset_for_axis(candidates.x_candidates, x_selection, snapport.width(), snap_type.strictness, cross_axis_offset, only_area);
    };
    auto choose_y = [&](Optional<CSSPixels> cross_axis_offset, Optional<SnapAreaReference> only_area = {}) {
        return choose_snap_offset_for_axis(candidates.y_candidates, y_selection, snapport.height(), snap_type.strictness, cross_axis_offset, only_area);
    };

    Optional<SnapAxisChoice> x_choice;
    Optional<SnapAxisChoice> y_choice;
    if (snaps_x && snaps_y) {
        x_choice = choose_x({});
        y_choice = choose_y({});
        if (x_choice.has_value() && y_choice.has_value() && !chosen_offsets_are_mutually_visible(candidates, x_choice->offset, y_choice->offset)) {
            // AD-HOC: A snap area is visible at its own snap positions, so one axis keeps the position it chose while
            //         the other gives up its own and takes the one the same area offers. Of the two areas, the one
            //         leaving the scroll container nearest its destination is followed. This matches other engines.
            Optional<SnapAxisChoice> y_of_x_area;
            Optional<SnapAxisChoice> x_of_y_area;
            if (x_choice->area.element)
                y_of_x_area = choose_y(x_choice->offset, x_choice->area);
            if (y_choice->area.element)
                x_of_y_area = choose_x(y_choice->offset, y_choice->area);

            auto distance_to_destination = [&](SnapAxisChoice const& x_axis_choice, SnapAxisChoice const& y_axis_choice) {
                return AK::hypot((x_axis_choice.offset - x_selection.destination).to_double(), (y_axis_choice.offset - y_selection.destination).to_double());
            };
            bool follows_x_area = y_of_x_area.has_value();
            if (follows_x_area && x_of_y_area.has_value())
                follows_x_area = distance_to_destination(*x_choice, *y_of_x_area) < distance_to_destination(*x_of_y_area, *y_choice);

            if (follows_x_area) {
                y_choice = y_of_x_area;
            } else if (x_of_y_area.has_value()) {
                x_choice = x_of_y_area;
            } else {
                // NB: Neither area offers a snap position in both axes, so the axis whose chosen offset is farther
                //     from its destination is chosen again from the positions visible at the other axis's offset.
                if (abs(x_choice->offset - x_selection.destination) <= abs(y_choice->offset - y_selection.destination)) {
                    y_choice = choose_y(x_choice->offset);
                } else {
                    x_choice = choose_x(y_choice->offset);
                }
            }
        }
        if (x_choice.has_value() && !y_choice.has_value()) {
            x_choice = choose_x(destination.y());
        } else if (y_choice.has_value() && !x_choice.has_value()) {
            y_choice = choose_y(destination.x());
        }
    } else if (snaps_x) {
        x_choice = choose_x(destination.y());
    } else if (snaps_y) {
        y_choice = choose_y(destination.x());
    }

    auto x_offset = x_choice.map([](auto const& choice) { return choice.offset; });
    auto y_offset = y_choice.map([](auto const& choice) { return choice.offset; });

    return snap_destination_for(destination, x_offset, y_offset);
}

}
