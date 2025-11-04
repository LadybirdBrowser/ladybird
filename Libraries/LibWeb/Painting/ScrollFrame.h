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

    CSSPixelPoint own_offset() const { return m_own_offset; }
    void set_own_offset(CSSPixelPoint offset)
    {
        m_cached_cumulative_offset.clear();
        m_own_offset = offset;
    }

private:
    GC::Weak<PaintableBox> m_paintable_box;
    size_t m_id { 0 };
    bool m_sticky { false };
    RefPtr<ScrollFrame const> m_parent;
    CSSPixelPoint m_own_offset;

    // Caching here relies on the fact that offsets of all scroll frames are invalidated when any of them changes,
    // so we don't need to worry about invalidating the cache when the parent's offset changes.
    mutable Optional<CSSPixelPoint> m_cached_cumulative_offset;
};

}
