/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/NumericLimits.h>
#include <AK/WeakPtr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/VisualContextIndex.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

// Position of a scroll or sticky node's entry in the ScrollState store. Slot 0 is the viewport's
// scroll node, so "no slot" needs a value outside the vector.
AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, ScrollStateSlot);
static constexpr ScrollStateSlot NO_SCROLL_STATE_SLOT { NumericLimits<size_t>::max() };

struct StickyInsets {
    Optional<CSSPixels> top;
    Optional<CSSPixels> right;
    Optional<CSSPixels> bottom;
    Optional<CSSPixels> left;
};

struct StickyConstraints {
    CSSPixelPoint position_relative_to_scroll_ancestor;
    CSSPixelSize border_box_size;
    CSSPixelSize scrollport_size;
    CSSPixelRect containing_block_region;
    bool needs_parent_offset_adjustment { false };
    StickyInsets insets;
};

// Dynamic state of one scroll or sticky node in the accumulated visual context tree. The tree owns
// structure and identity, and the node's ScrollData addresses its entry here by slot; the entry
// carries the values that change without a rebuild: the scroll offset and sticky constraints. The
// scroll-parent reference is derived from the containing block chain at build time, which
// deliberately differs from the node's visual context parent chain for sticky content inside
// fixed-position ancestors.
class ScrollFrame {
public:
    ScrollFrame() = default;
    ScrollFrame(VisualContextIndex node_index, Paintable const& paintable_box, bool sticky, ScrollStateSlot parent_slot);

    RefPtr<Paintable const> paintable_box_if_alive() const;

    bool is_sticky() const { return m_sticky; }

    CSSPixelPoint own_offset() const { return m_own_offset; }

    void set_own_offset(CSSPixelPoint offset)
    {
        m_own_offset = offset;
    }

    VisualContextIndex node_index() const { return m_node_index; }

    ScrollStateSlot parent_slot() const { return m_parent_slot; }

    void set_sticky_constraints(StickyConstraints constraints) { m_sticky_constraints = constraints; }
    bool has_sticky_constraints() const { return m_sticky_constraints.has_value(); }
    StickyConstraints const& sticky_constraints() const { return m_sticky_constraints.value(); }

private:
    WeakPtr<Paintable> m_paintable_box;
    bool m_sticky { false };
    VisualContextIndex m_node_index;
    ScrollStateSlot m_parent_slot { NO_SCROLL_STATE_SLOT };
    CSSPixelPoint m_own_offset;
    Optional<StickyConstraints> m_sticky_constraints;
};

}
