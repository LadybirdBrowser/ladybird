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

static SnapDestination snap_destination_for(CSSPixelPoint unsnapped_destination, Optional<CSSPixels> x_offset, Optional<CSSPixels> y_offset, SnapAxes evaluated_axes)
{
    return {
        .position = { x_offset.value_or(unsnapped_destination.x()), y_offset.value_or(unsnapped_destination.y()) },
        .snapped_x = x_offset.has_value(),
        .snapped_y = y_offset.has_value(),
        .evaluated_x = evaluated_axes.x,
        .evaluated_y = evaluated_axes.y,
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

static Vector<SnapAreaReference> snap_areas_at_offset(Vector<SnapPositionCandidate> const& candidates, CSSPixels offset, CSSPixels cross_axis_offset)
{
    Vector<SnapAreaReference> areas;
    for (auto const& candidate : candidates) {
        if (!snap_area_is_visible_at_cross_axis_offset(candidate, cross_axis_offset))
            continue;
        if (candidate_has_snap_position_at(candidate, offset) && candidate.area.element)
            areas.append(candidate.area);
    }
    return areas;
}

// https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-container
bool is_scroll_snap_container(Layout::Node const& node)
{
    auto const* node_with_style = as_if<Layout::NodeWithStyle>(node);
    if (!node_with_style || !node_with_style->is_scroll_container())
        return false;
    return !snap_axes_of_scroll_container(node).is_empty();
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

    auto snap_destination = snap_destination_for(destination, x_offset, y_offset, { snaps_x, snaps_y });
    if (x_offset.has_value())
        snap_destination.snapped_areas.x = snap_areas_at_offset(candidates.x_candidates, *x_offset, snap_destination.position.y());
    if (y_offset.has_value())
        snap_destination.snapped_areas.y = snap_areas_at_offset(candidates.y_candidates, *y_offset, snap_destination.position.x());
    return snap_destination;
}

static bool snap_area_contains_node(SnapAreaReference const& area, DOM::Node const& node)
{
    // A box generated by a pseudo-element has no content of its own that could be focused or targeted.
    if (!area.element || area.pseudo_element.has_value())
        return false;
    return area.element->is_inclusive_ancestor_of(node);
}

static CSSPixels resnap_offset_for_candidate(SnapPositionCandidate const& candidate, CSSPixels axis_current_offset)
{
    // A scroll container resting at one of its snapped area's valid snap positions is still snapped to that area and
    // stays where it is.
    if (candidate_has_snap_position_at(candidate, axis_current_offset))
        return axis_current_offset;
    return candidate.offset;
}

// The box each axis is snapped to, as an index into that axis's candidate list.
struct SnappedAxisBoxes {
    Optional<size_t> x_candidate;
    Optional<size_t> y_candidate;
};

// https://drafts.csswg.org/css-scroll-snap-1/#multiple-aligned-snap-areas
// When snapping to a scroll position that is aligned with multiple scroll snap areas, the following algorithm procedure
// is used to determined which box is snapped on the block and inline axes for a particular scroll container:
// NB: Every step treats the block and inline lists alike, so the steps are carried out on the physical axes directly.
static SnappedAxisBoxes select_between_multiple_aligned_snap_areas(SnapAxisCandidates const& candidates, ResnapSelection const& selection, CSSPixelPoint scroll_position)
{
    // 1. Let scroll position be the scroll position of the scroll container
    // NB: The scroll position is the offset the content change left the container resting at.

    // 2. Let inline be the set of boxes whose scroll snap areas are aligned at this scroll position in the inline axis.
    // 3. Let block be the set of boxes whose scroll snap areas are aligned at this scroll position in the block axis.
    // AD-HOC: Only the snap areas the container was snapped to before the content change take part, since a re-snap
    //         must return the container to those same areas rather than to whichever areas the change left aligned. An
    //         area whose snap position would leave it outside the snapport in the other axis no longer offers a valid
    //         snap position and does not take part either.
    // NB: A box is held as the index of its snap position candidate in its axis's candidate list, which holds its
    //     candidates in tree order.
    auto aligned_boxes = [](Vector<SnapPositionCandidate> const& axis_candidates, Vector<SnapAreaReference> const& snapped_areas, CSSPixels cross_axis_offset) {
        Vector<size_t> boxes;
        for (size_t candidate_index = 0; candidate_index < axis_candidates.size(); ++candidate_index) {
            auto const& candidate = axis_candidates[candidate_index];
            if (!candidate.area.element || !snapped_areas.contains_slow(candidate.area))
                continue;
            if (!snap_area_is_visible_at_cross_axis_offset(candidate, cross_axis_offset))
                continue;
            boxes.append(candidate_index);
        }
        return boxes;
    };
    auto x_boxes = aligned_boxes(candidates.x_candidates, selection.snapped_areas.x, scroll_position.y());
    auto y_boxes = aligned_boxes(candidates.y_candidates, selection.snapped_areas.y, scroll_position.x());

    // 4. For each list of block and inline:
    auto remove_superseded_boxes = [&](Vector<size_t>& boxes, Vector<SnapPositionCandidate> const& axis_candidates) {
        auto keep_only_boxes_matching = [&](auto const& predicate) {
            if (!any_of(boxes, [&](size_t candidate_index) { return predicate(axis_candidates[candidate_index].area); }))
                return false;
            boxes.remove_all_matching([&](size_t candidate_index) { return !predicate(axis_candidates[candidate_index].area); });
            return true;
        };

        // 1. If list contains one or more boxes that are focused or have a focused descendant, remove all other boxes
        //    from list
        bool kept_focused_boxes = selection.focused_node && keep_only_boxes_matching([&](SnapAreaReference const& area) { return snap_area_contains_node(area, *selection.focused_node); });

        // 2. Else if list contains one or more boxes that are targetted or have a targetted descendant, remove all
        //    other boxes from list.
        if (!kept_focused_boxes && selection.targeted_element)
            keep_only_boxes_matching([&](SnapAreaReference const& area) { return snap_area_contains_node(area, *selection.targeted_element); });

        // 3. For each box in list:
        // 1. Remove any box from list which is an ancestor of box.
        auto boxes_before_ancestor_removal = boxes;
        boxes.remove_all_matching([&](size_t candidate_index) {
            auto const& area = axis_candidates[candidate_index].area;
            if (area.pseudo_element.has_value())
                return false;
            return any_of(boxes_before_ancestor_removal, [&](size_t other_index) {
                auto const& other_area = axis_candidates[other_index].area;
                return other_area.element != area.element && area.element->is_inclusive_ancestor_of(*other_area.element);
            });
        });
    };
    remove_superseded_boxes(x_boxes, candidates.x_candidates);
    remove_superseded_boxes(y_boxes, candidates.y_candidates);

    auto boxes_contain_area = [](Vector<size_t> const& boxes, Vector<SnapPositionCandidate> const& axis_candidates, SnapAreaReference const& area) {
        return any_of(boxes, [&](size_t candidate_index) { return axis_candidates[candidate_index].area == area; });
    };
    bool axis_sets_overlap = any_of(x_boxes, [&](size_t candidate_index) {
        return boxes_contain_area(y_boxes, candidates.y_candidates, candidates.x_candidates[candidate_index].area);
    });
    // 5. If inline and block are overlapping sets:
    if (axis_sets_overlap) {
        // 1. Replace inline with the intersection of inline and block.
        x_boxes.remove_all_matching([&](size_t candidate_index) {
            return !boxes_contain_area(y_boxes, candidates.y_candidates, candidates.x_candidates[candidate_index].area);
        });

        // 2. Replace block with the intersection of inline and block.
        // NB: The intersection is unchanged by the step before it, so the narrowed list gives the same result the
        //     original one would.
        y_boxes.remove_all_matching([&](size_t candidate_index) {
            return !boxes_contain_area(x_boxes, candidates.x_candidates, candidates.y_candidates[candidate_index].area);
        });
    }

    // 6. Select the first element in tree order from inline as the snapped inline axis box.
    // 7. Select the first element in tree order from block as the snapped block axis box.
    return {
        .x_candidate = x_boxes.is_empty() ? OptionalNone {} : Optional<size_t> { x_boxes.first() },
        .y_candidate = y_boxes.is_empty() ? OptionalNone {} : Optional<size_t> { y_boxes.first() },
    };
}

SnapDestination select_resnap_destination(Layout::Node const& snap_container, CSSPixelPoint current_offset, ResnapSelection const& selection)
{
    auto snap_axes = snap_axes_of_scroll_container(snap_container);
    if (snap_axes.is_empty())
        return { current_offset };

    // NB: A container that was not snapped before the change re-snaps the way a fresh scroll to the current position
    //     would.
    if (selection.snapped_areas.is_empty())
        return adjust_scroll_destination_for_snapping(snap_container, current_offset);

    auto collection = collect_snap_position_candidates(snap_container, snap_axes.x, snap_axes.y);
    if (!collection.has_value())
        return { current_offset };

    auto const& candidates = collection->candidates;

    auto [x_chosen_candidate, y_chosen_candidate] = select_between_multiple_aligned_snap_areas(candidates, selection, current_offset);

    if (!x_chosen_candidate.has_value() && !y_chosen_candidate.has_value())
        return adjust_scroll_destination_for_snapping(snap_container, current_offset);

    auto fallback_offset_for_axis = [&](Vector<SnapPositionCandidate> const& axis_candidates, CSSPixels axis_current_offset, CSSPixels axis_snapport_size, CSSPixels cross_axis_offset) {
        SnapAxisSelection axis_selection {
            .destination = axis_current_offset,
            .start = axis_current_offset,
            .direction = 0,
            .starting_positions_boundary = {},
        };
        auto choice = choose_snap_offset_for_axis(axis_candidates, axis_selection, axis_snapport_size, collection->snap_type.strictness, cross_axis_offset);
        return choice.map([](auto const& axis_choice) { return axis_choice.offset; });
    };

    // An axis whose snapped areas are all gone re-snaps afresh, from the snap positions reachable while the other
    // axis follows its own snapped area.
    Optional<CSSPixels> x_offset;
    Optional<CSSPixels> y_offset;
    if (x_chosen_candidate.has_value())
        x_offset = resnap_offset_for_candidate(candidates.x_candidates[*x_chosen_candidate], current_offset.x());
    if (y_chosen_candidate.has_value())
        y_offset = resnap_offset_for_candidate(candidates.y_candidates[*y_chosen_candidate], current_offset.y());
    if (snap_axes.x && !x_chosen_candidate.has_value())
        x_offset = fallback_offset_for_axis(candidates.x_candidates, current_offset.x(), collection->x_geometry.snapport_size, y_offset.value_or(current_offset.y()));
    if (snap_axes.y && !y_chosen_candidate.has_value())
        y_offset = fallback_offset_for_axis(candidates.y_candidates, current_offset.y(), collection->y_geometry.snapport_size, x_offset.value_or(current_offset.x()));

    // https://drafts.csswg.org/css-scroll-snap-1/#re-snap
    // If it is not possible to snap to both (e.g. if snapping to one resulted in the other being offscreen), it must
    // prefer the focused box, followed by the targeted box, followed by the block axis if neither box is focused or
    // targeted.
    // NB: The preferred box is snapped in both axes: the other axis takes the box's own snap position when it defines
    //     one, and otherwise re-snaps among the positions at which the preferred box remains visible.
    if (x_offset.has_value() && y_offset.has_value() && !chosen_offsets_are_mutually_visible(candidates, *x_offset, *y_offset)) {
        auto const* style_source = style_source_for_snap_container(snap_container);
        bool block_axis_is_y = style_source->writing_mode() == CSS::WritingMode::HorizontalTb;

        struct ResnapAxis {
            Vector<SnapPositionCandidate> const& candidates;
            Optional<size_t>& chosen_candidate;
            Optional<CSSPixels>& offset;
            Optional<SnapAreaReference> area;
            CSSPixels current_offset;
            CSSPixels snapport_size;
        };
        auto x_area = x_chosen_candidate.map([&](size_t candidate_index) { return candidates.x_candidates[candidate_index].area; });
        auto y_area = y_chosen_candidate.map([&](size_t candidate_index) { return candidates.y_candidates[candidate_index].area; });
        ResnapAxis x_axis { candidates.x_candidates, x_chosen_candidate, x_offset, move(x_area), current_offset.x(), collection->x_geometry.snapport_size };
        ResnapAxis y_axis { candidates.y_candidates, y_chosen_candidate, y_offset, move(y_area), current_offset.y(), collection->y_geometry.snapport_size };

        auto area_contains_focus = [&](Optional<SnapAreaReference> const& area) {
            return area.has_value() && selection.focused_node && snap_area_contains_node(*area, *selection.focused_node);
        };
        auto area_contains_target = [&](Optional<SnapAreaReference> const& area) {
            return area.has_value() && selection.targeted_element && snap_area_contains_node(*area, *selection.targeted_element);
        };

        bool preferred_axis_is_x;
        if (area_contains_focus(x_axis.area) || area_contains_focus(y_axis.area)) {
            preferred_axis_is_x = area_contains_focus(x_axis.area);
        } else if (area_contains_target(x_axis.area) || area_contains_target(y_axis.area)) {
            preferred_axis_is_x = area_contains_target(x_axis.area);
        } else if (x_axis.area.has_value() != y_axis.area.has_value()) {
            preferred_axis_is_x = x_axis.area.has_value();
        } else {
            preferred_axis_is_x = !block_axis_is_y;
        }
        auto const& preferred_axis = preferred_axis_is_x ? x_axis : y_axis;
        auto& other_axis = preferred_axis_is_x ? y_axis : x_axis;

        auto preferred_area_candidate_in_other_axis = other_axis.candidates.find_if([&](auto const& candidate) { return candidate.area == *preferred_axis.area; });
        if (preferred_area_candidate_in_other_axis != other_axis.candidates.end()) {
            other_axis.offset = resnap_offset_for_candidate(*preferred_area_candidate_in_other_axis, other_axis.current_offset);
            other_axis.chosen_candidate = preferred_area_candidate_in_other_axis.index();
        } else {
            other_axis.offset = fallback_offset_for_axis(other_axis.candidates, other_axis.current_offset, other_axis.snapport_size, *preferred_axis.offset);
            other_axis.chosen_candidate = {};
        }
    }

    auto snap_destination = snap_destination_for(current_offset, x_offset, y_offset, snap_axes);

    // An axis a re-snap left where it was selected nothing, so an area that happens to have become aligned at the
    // kept position does not take the place of the areas the container was snapped to.
    auto record_snapped_areas = [&](Optional<size_t> const& chosen_candidate, Vector<SnapPositionCandidate> const& axis_candidates, Vector<SnapAreaReference> const& previously_snapped_areas, CSSPixels axis_offset, CSSPixels axis_current_offset, CSSPixels cross_axis_offset) {
        auto areas = snap_areas_at_offset(axis_candidates, axis_offset, cross_axis_offset);
        if (chosen_candidate.has_value() && axis_offset == axis_current_offset) {
            auto const& chosen_area = axis_candidates[*chosen_candidate].area;
            areas.remove_all_matching([&](auto const& area) { return area != chosen_area && !previously_snapped_areas.contains_slow(area); });
        }
        return areas;
    };
    if (x_offset.has_value())
        snap_destination.snapped_areas.x = record_snapped_areas(x_chosen_candidate, candidates.x_candidates, selection.snapped_areas.x, *x_offset, current_offset.x(), snap_destination.position.y());
    if (y_offset.has_value())
        snap_destination.snapped_areas.y = record_snapped_areas(y_chosen_candidate, candidates.y_candidates, selection.snapped_areas.y, *y_offset, current_offset.y(), snap_destination.position.x());
    return snap_destination;
}

// The largest share of a momentum delta that the delta after it may keep for the momentum to be considered decaying.
static constexpr double maximum_momentum_decay_share = 0.96;

// The share a momentum delta keeps of the one before it is treated as no larger than this, so that momentum which
// barely decays is predicted to travel a hundred times the distance of its latest delta rather than forever.
static constexpr double maximum_slow_momentum_decay_share = 0.99;

// The number of consecutively smaller momentum deltas after which momentum that decays only slowly is predicted from
// anyway.
static constexpr u32 decaying_deltas_before_slow_decay_is_predicted_from = 3;

void MomentumFlingEstimator::reset()
{
    m_previous_momentum_delta = {};
    m_consecutively_decaying_momentum_deltas = 0;
}

Optional<CSSPixelPoint> MomentumFlingEstimator::estimate_remaining_displacement(CSSPixelPoint momentum_delta)
{
    auto previous_momentum_delta = m_previous_momentum_delta;
    m_previous_momentum_delta = momentum_delta;

    // The share the delta keeps of the one before it is what the momentum decays by, so the first delta of a flick
    // says nothing about where it is headed.
    if (!previous_momentum_delta.has_value())
        return {};
    auto distance = AK::hypot(momentum_delta.x().to_double(), momentum_delta.y().to_double());
    auto previous_distance = AK::hypot(previous_momentum_delta->x().to_double(), previous_momentum_delta->y().to_double());
    if (previous_distance <= 0)
        return {};
    auto decay_share = distance / previous_distance;

    if (decay_share < 1) {
        ++m_consecutively_decaying_momentum_deltas;
    } else {
        m_consecutively_decaying_momentum_deltas = 0;
    }

    auto momentum_is_decaying = decay_share < maximum_momentum_decay_share
        || (m_consecutively_decaying_momentum_deltas >= decaying_deltas_before_slow_decay_is_predicted_from && decay_share < 1);
    if (!momentum_is_decaying)
        return {};

    // Each delta keeps the same share of the one before it, so the deltas still to come sum to the delta given
    // divided by the share it loses each time.
    auto remaining_distance_factor = 1 / (1 - min(decay_share, maximum_slow_momentum_decay_share));
    return CSSPixelPoint {
        CSSPixels::nearest_value_for(momentum_delta.x().to_double() * remaining_distance_factor),
        CSSPixels::nearest_value_for(momentum_delta.y().to_double() * remaining_distance_factor),
    };
}

}
