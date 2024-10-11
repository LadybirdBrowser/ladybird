/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class ScrollFrame : public RefCounted<ScrollFrame> {
public:
    ScrollFrame(PaintableBox const& paintable_box, size_t id, RefPtr<ScrollFrame const> parent);

    PaintableBox const& paintable_box() const { return *m_paintable_box; }

    size_t id() const { return m_id; }

    CSSPixelPoint cumulative_offset() const
    {
        if (!m_cached_cumulative_offset.has_value()) {
            m_cached_cumulative_offset = m_own_offset;
            if (m_parent) {
                m_cached_cumulative_offset.value() += m_parent->cumulative_offset();
            }
        }
        return m_cached_cumulative_offset.value();
    }

    CSSPixelPoint own_offset() const { return m_own_offset; }
    void set_own_offset(CSSPixelPoint offset)
    {
        m_cached_cumulative_offset.clear();
        m_own_offset = offset;
    }

private:
    WeakPtr<PaintableBox> m_paintable_box;
    size_t m_id { 0 };
    RefPtr<ScrollFrame const> m_parent;
    CSSPixelPoint m_own_offset;

    // Caching here relies on the fact that offsets of all scroll frames are invalidated when any of them changes,
    // so we don't need to worry about invalidating the cache when the parent's offset changes.
    mutable Optional<CSSPixelPoint> m_cached_cumulative_offset;
};

}
