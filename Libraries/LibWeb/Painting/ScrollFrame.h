/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <LibGC/Weak.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

AK_TYPEDEF_DISTINCT_ORDERED_ID(size_t, ScrollFrameIndex);

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

class ScrollFrame {
public:
    ScrollFrame() = default;
    ScrollFrame(PaintableBox const& paintable_box, bool sticky, ScrollFrameIndex parent_index);

    PaintableBox const& paintable_box() const { return *m_paintable_box; }

    bool is_sticky() const { return m_sticky; }

    CSSPixelPoint own_offset() const { return m_own_offset; }

    void set_own_offset(CSSPixelPoint offset)
    {
        m_own_offset = offset;
    }

    ScrollFrameIndex parent_index() const { return m_parent_index; }

    void set_sticky_constraints(StickyConstraints constraints) { m_sticky_constraints = constraints; }
    bool has_sticky_constraints() const { return m_sticky_constraints.has_value(); }
    StickyConstraints const& sticky_constraints() const { return m_sticky_constraints.value(); }

private:
    friend class ScrollState;
    friend class ScrollStateSnapshot;

    GC::Weak<PaintableBox> m_paintable_box;
    bool m_sticky { false };
    ScrollFrameIndex m_parent_index;
    CSSPixelPoint m_own_offset;
    Optional<StickyConstraints> m_sticky_constraints;
};

}
