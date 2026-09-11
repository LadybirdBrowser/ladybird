/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

// Scroll snap position selection over geometry that has already been collected from layout, so that the same
// selection runs on the main thread and in the compositor process. Everything here is in CSS pixels.

namespace Web::Compositor {

// The box a snap area belongs to, which identifies the area across relayouts and across processes. A node's unique ID
// is never reused, so an area removed from the document keeps an identity no live area ever takes again and is no
// longer a snap area the container can be returned to.
struct SnapAreaIdentity {
    UniqueNodeID node_id {};
    // 0 for the element's own box; otherwise the pseudo-element the box is generated for, plus one.
    u8 pseudo_element_type { 0 };

    bool operator==(SnapAreaIdentity const&) const = default;

    bool is_valid() const { return node_id.value() != 0; }
    bool is_pseudo_element() const { return pseudo_element_type != 0; }
    Optional<CSS::PseudoElement> pseudo_element() const
    {
        if (!is_pseudo_element())
            return {};
        return static_cast<CSS::PseudoElement>(pseudo_element_type - 1);
    }
};

struct SnapAxes {
    bool x { false };
    bool y { false };

    bool is_empty() const { return !x && !y; }
};

// https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-area
struct SnapAreaGeometry {
    SnapAreaIdentity identity;
    // The transformed border box in the snap container's coordinate space, with the scroll margin added.
    CSSPixelRect rect;
    // The alignment along each physical axis, resolved from the writing mode it applies in.
    CSS::ScrollSnapAlign align_x { CSS::ScrollSnapAlign::None };
    CSS::ScrollSnapAlign align_y { CSS::ScrollSnapAlign::None };
    bool always_stop { false };
};

// https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-container
struct SnapContainerGeometry {
    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-padding
    CSSPixelRect snapport;
    CSSPixelPoint min_scroll_offset;
    CSSPixelPoint max_scroll_offset;
    CSS::ScrollSnapStrictness strictness { CSS::ScrollSnapStrictness::None };
    SnapAxes axes;
    bool horizontal_writing_mode { true };
};

// A closed range of scroll offsets that are all valid snap positions.
struct CoveringRange {
    CSSPixels start;
    CSSPixels end;
};

// A candidate scroll offset in one axis, produced by aligning a snap area with the snapport.
struct SnapPositionCandidate {
    CSSPixels offset;
    SnapAreaIdentity area {};

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

// https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
struct SnapSelectionStrategy {
    enum class Type : u8 {
        // An absolute scroll, or any other operation with only an intended end position.
        EndPosition,
        // A relative scroll with only an intended direction, such as a mouse wheel step or an arrow key press.
        Direction,
        // A relative scroll with both an intended direction and end position, such as scrollBy().
        EndPositionAndDirection,
    };

    Type type { Type::EndPosition };
    // The scroll offset the operation travels from; a snap position with `scroll-snap-stop: always` must not be
    // passed over on the way from there to the selected snap position.
    Optional<CSSPixelPoint> start_offset {};
    // The net offset change the operation's input produced; an axis the operation did not travel in selects no snap
    // position.
    CSSPixelPoint displacement {};
    // Snap positions short of this offset in the direction of travel are not selected; it defaults to the start
    // offset.
    Optional<CSSPixelPoint> starting_positions_boundary {};
};

// The snap areas a scroll container is snapped to in each axis, so that it can be re-snapped to those same snap areas
// after a content change.
struct SnappedAreas {
    Vector<SnapAreaIdentity> x;
    Vector<SnapAreaIdentity> y;

    bool is_empty() const { return x.is_empty() && y.is_empty(); }
};

struct SnapDestination {
    CSSPixelPoint position;
    // Whether a snap position was selected in each axis; an axis whose snap positions are all ineligible for the
    // scroll keeps the destination it was given.
    bool snapped_x { false };
    bool snapped_y { false };
    // Whether snap position selection ran in each axis. An axis the scroll did not travel in is not evaluated, and
    // whatever snap area the container is snapped to there remains snapped.
    bool evaluated_x { false };
    bool evaluated_y { false };
    SnappedAreas snapped_areas {};
};

// The selection a scroll makes in one axis.
struct SnapAxisSelection {
    CSSPixels destination;
    CSSPixels start;
    CSSPixels direction;
    Optional<CSSPixels> starting_positions_boundary;
};

struct SnapAxisChoice {
    CSSPixels offset;
    SnapAreaIdentity area;
};

// The axes a scroll selects a snap position in: those the container snaps in that the scroll traveled in, or every
// axis the container snaps in when the scroll traveled in none.
WEB_API SnapAxes axes_to_evaluate(SnapAxes container_axes, SnapSelectionStrategy const&);

WEB_API SnapAxisCandidates build_snap_candidates(SnapContainerGeometry const&, ReadonlySpan<SnapAreaGeometry>, SnapAxes collect_axes);

// https://drafts.csswg.org/css-scroll-snap-1/#choosing
WEB_API SnapDestination select_snap_destination(SnapContainerGeometry const&, SnapAxisCandidates const&, CSSPixelPoint destination, SnapSelectionStrategy const&, SnapAxes evaluated_axes);
WEB_API SnapDestination select_snap_destination(SnapContainerGeometry const&, ReadonlySpan<SnapAreaGeometry>, CSSPixelPoint destination, SnapSelectionStrategy const& = {});

WEB_API Optional<SnapAxisChoice> choose_snap_offset_for_axis(Vector<SnapPositionCandidate> const&, SnapAxisSelection const&, CSSPixels snapport_size, CSS::ScrollSnapStrictness, Optional<CSSPixels> cross_axis_offset, Optional<SnapAreaIdentity> only_area = {});
WEB_API bool candidate_has_snap_position_at(SnapPositionCandidate const&, CSSPixels offset);
WEB_API bool snap_area_is_visible_at_cross_axis_offset(SnapPositionCandidate const&, CSSPixels cross_axis_offset);
WEB_API bool chosen_offsets_are_mutually_visible(SnapAxisCandidates const&, CSSPixels x_offset, CSSPixels y_offset);
WEB_API SnapDestination snap_destination_for(CSSPixelPoint unsnapped_destination, Optional<CSSPixels> x_offset, Optional<CSSPixels> y_offset, SnapAxes evaluated_axes);
WEB_API Vector<SnapAreaIdentity> snap_areas_at_offset(Vector<SnapPositionCandidate> const&, CSSPixels offset, CSSPixels cross_axis_offset);

// The displacement the momentum of a flick has left to travel, estimated from the deltas that momentum has produced
// so far.
class WEB_API MomentumFlingEstimator {
public:
    void reset();

    // The displacement left to travel, including the delta given; momentum that has not yet decayed far enough to
    // tell where it is headed reports no estimate.
    Optional<CSSPixelPoint> estimate_remaining_displacement(CSSPixelPoint momentum_delta);

private:
    Optional<CSSPixelPoint> m_previous_momentum_delta;
    u32 m_consecutively_decaying_momentum_deltas { 0 };
};

}
