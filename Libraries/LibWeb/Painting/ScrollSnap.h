/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGC/WeakInlines.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/Export.h>
#include <LibWeb/PixelUnits.h>

namespace Web::DOM {

class Element;

}

namespace Web::Layout {

class Node;

}

namespace Web::Painting {

// The element a snap area belongs to, which identifies the area across relayouts. The reference is weak: an area
// removed from the document is no longer a snap area the container can be returned to.
struct SnapAreaReference {
    GC::Weak<DOM::Element const> element;
    Optional<CSS::PseudoElement> pseudo_element;

    // Two references to areas that have both gone away compare equal, which no remembered area is ever tested against.
    bool operator==(SnapAreaReference const& other) const
    {
        return element.ptr() == other.element.ptr() && pseudo_element == other.pseudo_element;
    }
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

struct SnapAxes {
    bool x { false };
    bool y { false };

    bool is_empty() const { return !x && !y; }
};

WEB_API SnapAxes snap_axes_of_scroll_container(Layout::Node const& snap_container);

struct SnapDestination {
    CSSPixelPoint position;
    // Whether a snap position was selected in each axis; an axis whose snap positions are all ineligible for the
    // scroll keeps the destination it was given.
    bool snapped_x { false };
    bool snapped_y { false };
};

WEB_API SnapDestination adjust_scroll_destination_for_snapping(Layout::Node const& snap_container, CSSPixelPoint destination, SnapSelectionStrategy const& strategy = {});

}
