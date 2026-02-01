/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibGC/Weak.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

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

class ScrollFrame : public RefCounted<ScrollFrame> {
public:
    ScrollFrame(PaintableBox const& paintable_box, size_t id, bool sticky, RefPtr<ScrollFrame const> parent);

    PaintableBox const& paintable_box() const { return *m_paintable_box; }

    size_t id() const { return m_id; }

    bool is_sticky() const { return m_sticky; }

    CSSPixelPoint cumulative_offset() const
    {
        return m_cached_cumulative_offset.ensure([&] {
            auto offset = m_own_offset;
            if (m_parent)
                offset += m_parent->cumulative_offset();
            return offset;
        });
    }

    void set_own_offset(CSSPixelPoint offset)
    {
        m_cached_cumulative_offset.clear();
        m_own_offset = offset;
    }

    RefPtr<ScrollFrame const> parent() const { return m_parent; }
    RefPtr<ScrollFrame const> nearest_scrolling_ancestor() const;

    void set_sticky_constraints(StickyConstraints constraints) { m_sticky_constraints = constraints; }
    bool has_sticky_constraints() const { return m_sticky_constraints.has_value(); }
    StickyConstraints const& sticky_constraints() const { return m_sticky_constraints.value(); }

private:
    friend class ScrollStateSnapshot;

    GC::Weak<PaintableBox> m_paintable_box;
    size_t m_id { 0 };
    bool m_sticky { false };
    RefPtr<ScrollFrame const> m_parent;
    CSSPixelPoint m_own_offset;
    Optional<StickyConstraints> m_sticky_constraints;

    // Caching here relies on the fact that offsets of all scroll frames are invalidated when any of them changes,
    // so we don't need to worry about invalidating the cache when the parent's offset changes.
    mutable Optional<CSSPixelPoint> m_cached_cumulative_offset;
};

}
