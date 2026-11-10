/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibWebCommon/PixelUnits.h>

// What scroll snap position selection is asked and answers, on the main thread and in the compositor process alike.
// The selection is libcompositing_rust's scrolling::scroll_snap_selection. Everything here is in CSS pixels.

namespace Compositing {

// The box a snap area belongs to, which identifies the area across relayouts and across processes. A node's unique ID
// is never reused, so an area removed from the document keeps an identity no live area ever takes again and is no
// longer a snap area the container can be returned to.
struct SnapAreaIdentity {
    Web::UniqueNodeID node_id {};
    // 0 for the element's own box; otherwise the pseudo-element the box is generated for, plus one.
    u8 pseudo_element_type { 0 };

    bool operator==(SnapAreaIdentity const&) const = default;

    bool is_valid() const { return node_id.value() != 0; }
    bool is_pseudo_element() const { return pseudo_element_type != 0; }
};

// https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
struct COMPOSITING_API SnapSelectionStrategy {
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
    Optional<Web::CSSPixelPoint> start_offset {};
    // The net offset change the operation's input produced; an axis the operation did not travel in selects no snap
    // position.
    Web::CSSPixelPoint displacement {};
    // Snap positions short of this offset in the direction of travel are not selected; it defaults to the start
    // offset.
    Optional<Web::CSSPixelPoint> starting_positions_boundary {};
};

// The snap areas a scroll container is snapped to in each axis, so that it can be re-snapped to those same snap areas
// after a content change.
struct SnappedAreas {
    Vector<SnapAreaIdentity> x;
    Vector<SnapAreaIdentity> y;

    bool is_empty() const { return x.is_empty() && y.is_empty(); }
};

struct SnapDestination {
    Web::CSSPixelPoint position;
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

}
