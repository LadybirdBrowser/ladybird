/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/ScrollSnap.h>
#include <LibWeb/Painting/Scrolling.h>

namespace Web::Painting {

using Compositor::SnapAxisCandidates;
using Compositor::SnapAxisSelection;
using Compositor::SnapPositionCandidate;

static SnapAreaIdentity snap_area_identity_for(Layout::Node const& snap_area)
{
    if (auto pseudo_element = snap_area.generated_for_pseudo_element(); pseudo_element.has_value())
        return { snap_area.pseudo_element_generator()->unique_id(), static_cast<u8>(to_underlying(*pseudo_element) + 1) };
    if (auto const* element = as_if<DOM::Element>(snap_area.dom_node()))
        return { element->unique_id(), 0 };
    return {};
}

// The element a snap area's box belongs to, which is the generating element of a pseudo-element's box.
static DOM::Element const* element_of_snap_area(SnapAreaIdentity const& area)
{
    if (!area.is_valid())
        return nullptr;
    return as_if<DOM::Element>(DOM::Node::from_unique_id(area.node_id));
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
    auto rect = rust_apply_css_transform_to_rect(snap_area, absolute_border_box_rect(snap_area));
    for (auto const* containing_block = snap_area.containing_block(); containing_block && containing_block != &snap_container; containing_block = containing_block->containing_block())
        rect = rust_apply_css_transform_to_rect(*containing_block, rect);

    auto const& scroll_margin = snap_area.scroll_margin();
    rect.inflate(
        scroll_margin.top().to_px_or_zero(CSSPixels { 0 }),
        scroll_margin.right().to_px_or_zero(CSSPixels { 0 }),
        scroll_margin.bottom().to_px_or_zero(CSSPixels { 0 }),
        scroll_margin.left().to_px_or_zero(CSSPixels { 0 }));
    return rect;
}

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

// https://drafts.csswg.org/css-scroll-snap-1/#snap-axis
SnapAxes snap_axes_of_scroll_container(Layout::Node const& snap_container)
{
    auto axes = Layout::RustFFI::layout_arena_scroll_snap_axes(snap_container.arena_handle(), Layout::Node::slot_id(&snap_container));
    return { .x = axes.x, .y = axes.y };
}

Optional<SnapContainerGeometry> snap_container_geometry(Layout::Node const& snap_container)
{
    auto const* style_source = style_source_for_snap_container(snap_container);
    if (!style_source)
        return {};

    if (!Painting::scrollable_overflow_rect(snap_container).has_value())
        return {};

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-padding
    // For a scroll snap container this region also defines the scroll snapport—the area of the scrollport that is
    // used as the alignment container for the scroll snap areas when calculating snap positions.
    return SnapContainerGeometry {
        .snapport = scroll_snapport_rect(snap_container),
        .min_scroll_offset = minimum_scroll_offset(snap_container),
        .max_scroll_offset = maximum_scroll_offset(snap_container),
        .strictness = style_source->scroll_snap_type().strictness,
        .axes = snap_axes_of_scroll_container(snap_container),
        .horizontal_writing_mode = style_source->writing_mode() == CSS::WritingMode::HorizontalTb,
    };
}

Vector<SnapAreaGeometry> collect_snap_areas(Layout::Node const& snap_container)
{
    auto const* style_source = style_source_for_snap_container(snap_container);
    if (!style_source)
        return {};

    auto snapport = scroll_snapport_rect(snap_container);

    Vector<SnapAreaGeometry> areas;
    for_each_descendant_snap_area(snap_container, snap_container, [&](Layout::NodeWithStyle const& snap_area) {
        auto area_rect = snap_area_rect(snap_area, snap_container);

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

        areas.append({
            .identity = snap_area_identity_for(snap_area),
            .rect = area_rect,
            .align_x = alignment.x,
            .align_y = alignment.y,
            .always_stop = snap_area.scroll_snap_stop() == CSS::ScrollSnapStop::Always,
        });
    });
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
    auto geometry = snap_container_geometry(snap_container);
    if (!geometry.has_value() || Compositor::axes_to_evaluate(geometry->axes, strategy).is_empty())
        return { destination };
    return Compositor::select_snap_destination(*geometry, collect_snap_areas(snap_container), destination, strategy);
}

static bool snap_area_contains_node(SnapAreaIdentity const& area, DOM::Node const& node)
{
    // A box generated by a pseudo-element has no content of its own that could be focused or targeted.
    if (area.is_pseudo_element())
        return false;
    auto const* element = element_of_snap_area(area);
    return element && element->is_inclusive_ancestor_of(node);
}

static CSSPixels resnap_offset_for_candidate(SnapPositionCandidate const& candidate, CSSPixels axis_current_offset)
{
    // A scroll container resting at one of its snapped area's valid snap positions is still snapped to that area and
    // stays where it is.
    if (Compositor::candidate_has_snap_position_at(candidate, axis_current_offset))
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
    auto aligned_boxes = [](Vector<SnapPositionCandidate> const& axis_candidates, Vector<SnapAreaIdentity> const& snapped_areas, CSSPixels cross_axis_offset) {
        Vector<size_t> boxes;
        for (size_t candidate_index = 0; candidate_index < axis_candidates.size(); ++candidate_index) {
            auto const& candidate = axis_candidates[candidate_index];
            if (!candidate.area.is_valid() || !snapped_areas.contains_slow(candidate.area))
                continue;
            if (!Compositor::snap_area_is_visible_at_cross_axis_offset(candidate, cross_axis_offset))
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
        bool kept_focused_boxes = selection.focused_node && keep_only_boxes_matching([&](SnapAreaIdentity const& area) { return snap_area_contains_node(area, *selection.focused_node); });

        // 2. Else if list contains one or more boxes that are targetted or have a targetted descendant, remove all
        //    other boxes from list.
        if (!kept_focused_boxes && selection.targeted_element)
            keep_only_boxes_matching([&](SnapAreaIdentity const& area) { return snap_area_contains_node(area, *selection.targeted_element); });

        // 3. For each box in list:
        // 1. Remove any box from list which is an ancestor of box.
        auto boxes_before_ancestor_removal = boxes;
        boxes.remove_all_matching([&](size_t candidate_index) {
            auto const& area = axis_candidates[candidate_index].area;
            if (area.is_pseudo_element())
                return false;
            auto const* element = element_of_snap_area(area);
            if (!element)
                return false;
            return any_of(boxes_before_ancestor_removal, [&](size_t other_index) {
                auto const* other_element = element_of_snap_area(axis_candidates[other_index].area);
                return other_element && other_element != element && element->is_inclusive_ancestor_of(*other_element);
            });
        });
    };
    remove_superseded_boxes(x_boxes, candidates.x_candidates);
    remove_superseded_boxes(y_boxes, candidates.y_candidates);

    auto boxes_contain_area = [](Vector<size_t> const& boxes, Vector<SnapPositionCandidate> const& axis_candidates, SnapAreaIdentity const& area) {
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
    auto geometry = snap_container_geometry(snap_container);
    if (!geometry.has_value() || geometry->axes.is_empty())
        return { current_offset };
    auto snap_axes = geometry->axes;

    auto candidates = Compositor::build_snap_candidates(*geometry, collect_snap_areas(snap_container), snap_axes);

    // NB: A container that was not snapped before the change, or whose snapped areas are all gone, re-snaps the way a
    //     fresh scroll to the current position would.
    auto resnap_afresh = [&] {
        return Compositor::select_snap_destination(*geometry, candidates, current_offset, {}, snap_axes);
    };
    if (selection.snapped_areas.is_empty())
        return resnap_afresh();

    auto [x_chosen_candidate, y_chosen_candidate] = select_between_multiple_aligned_snap_areas(candidates, selection, current_offset);

    if (!x_chosen_candidate.has_value() && !y_chosen_candidate.has_value())
        return resnap_afresh();

    auto fallback_offset_for_axis = [&](Vector<SnapPositionCandidate> const& axis_candidates, CSSPixels axis_current_offset, CSSPixels axis_snapport_size, CSSPixels cross_axis_offset) {
        SnapAxisSelection axis_selection {
            .destination = axis_current_offset,
            .start = axis_current_offset,
            .direction = 0,
            .starting_positions_boundary = {},
        };
        auto choice = Compositor::choose_snap_offset_for_axis(axis_candidates, axis_selection, axis_snapport_size, geometry->strictness, cross_axis_offset);
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
        x_offset = fallback_offset_for_axis(candidates.x_candidates, current_offset.x(), geometry->snapport.width(), y_offset.value_or(current_offset.y()));
    if (snap_axes.y && !y_chosen_candidate.has_value())
        y_offset = fallback_offset_for_axis(candidates.y_candidates, current_offset.y(), geometry->snapport.height(), x_offset.value_or(current_offset.x()));

    // https://drafts.csswg.org/css-scroll-snap-1/#re-snap
    // If it is not possible to snap to both (e.g. if snapping to one resulted in the other being offscreen), it must
    // prefer the focused box, followed by the targeted box, followed by the block axis if neither box is focused or
    // targeted.
    // NB: The preferred box is snapped in both axes: the other axis takes the box's own snap position when it defines
    //     one, and otherwise re-snaps among the positions at which the preferred box remains visible.
    if (x_offset.has_value() && y_offset.has_value() && !Compositor::chosen_offsets_are_mutually_visible(candidates, *x_offset, *y_offset)) {
        bool block_axis_is_y = geometry->horizontal_writing_mode;

        struct ResnapAxis {
            Vector<SnapPositionCandidate> const& candidates;
            Optional<size_t>& chosen_candidate;
            Optional<CSSPixels>& offset;
            Optional<SnapAreaIdentity> area;
            CSSPixels current_offset;
            CSSPixels snapport_size;
        };
        auto x_area = x_chosen_candidate.map([&](size_t candidate_index) { return candidates.x_candidates[candidate_index].area; });
        auto y_area = y_chosen_candidate.map([&](size_t candidate_index) { return candidates.y_candidates[candidate_index].area; });
        ResnapAxis x_axis { candidates.x_candidates, x_chosen_candidate, x_offset, move(x_area), current_offset.x(), geometry->snapport.width() };
        ResnapAxis y_axis { candidates.y_candidates, y_chosen_candidate, y_offset, move(y_area), current_offset.y(), geometry->snapport.height() };

        auto area_contains_focus = [&](Optional<SnapAreaIdentity> const& area) {
            return area.has_value() && selection.focused_node && snap_area_contains_node(*area, *selection.focused_node);
        };
        auto area_contains_target = [&](Optional<SnapAreaIdentity> const& area) {
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

    auto snap_destination = Compositor::snap_destination_for(current_offset, x_offset, y_offset, snap_axes);

    // An axis a re-snap left where it was selected nothing, so an area that happens to have become aligned at the
    // kept position does not take the place of the areas the container was snapped to.
    auto record_snapped_areas = [&](Optional<size_t> const& chosen_candidate, Vector<SnapPositionCandidate> const& axis_candidates, Vector<SnapAreaIdentity> const& previously_snapped_areas, CSSPixels axis_offset, CSSPixels axis_current_offset, CSSPixels cross_axis_offset) {
        auto areas = Compositor::snap_areas_at_offset(axis_candidates, axis_offset, cross_axis_offset);
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

}
