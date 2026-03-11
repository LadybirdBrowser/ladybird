/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Point.h>
#include <LibWeb/Painting/ScrollFrame.h>

namespace Web::Painting {

class ScrollStateSnapshot {
public:
    static ScrollStateSnapshot create(Vector<ScrollFrame> const& scroll_frames, double device_pixels_per_css_pixel);

    Gfx::FloatPoint device_offset_for_index(ScrollFrameIndex index) const
    {
        if (index.value() >= m_device_offsets.size())
            return {};
        return m_device_offsets[index.value()];
    }

private:
    Vector<Gfx::FloatPoint> m_device_offsets;
};

class ScrollState {
public:
    // ScrollFrameIndex is 1-based: value 0 means "no frame".
    // Index 0 in m_scroll_frames is a sentinel (never accessed by callers).
    // Value N maps directly to m_scroll_frames[N].
    ScrollState()
    {
        m_scroll_frames.empend(); // Sentinel at index 0
    }

    ScrollFrameIndex create_scroll_frame_for(PaintableBox const& paintable_box, ScrollFrameIndex parent)
    {
        auto index = ScrollFrameIndex { m_scroll_frames.size() };
        m_scroll_frames.empend(paintable_box, false, parent);
        return index;
    }

    ScrollFrameIndex create_sticky_frame_for(PaintableBox const& paintable_box, ScrollFrameIndex parent)
    {
        auto index = ScrollFrameIndex { m_scroll_frames.size() };
        m_scroll_frames.empend(paintable_box, true, parent);
        return index;
    }

    ScrollFrame const& frame_at(ScrollFrameIndex index) const { return m_scroll_frames[index.value()]; }
    ScrollFrame& frame_at(ScrollFrameIndex index) { return m_scroll_frames[index.value()]; }

    CSSPixelPoint cumulative_offset(ScrollFrameIndex index) const
    {
        CSSPixelPoint offset;
        while (index.value()) {
            offset += frame_at(index).own_offset();
            index = frame_at(index).parent_index();
        }
        return offset;
    }

    ScrollFrameIndex nearest_scrolling_ancestor(ScrollFrameIndex index) const
    {
        auto ancestor = frame_at(index).parent_index();
        while (ancestor.value()) {
            if (!frame_at(ancestor).is_sticky())
                return ancestor;
            ancestor = frame_at(ancestor).parent_index();
        }
        return {};
    }

    template<typename Callback>
    void for_each_scroll_frame(Callback callback)
    {
        for (size_t i = 1; i < m_scroll_frames.size(); ++i) {
            if (m_scroll_frames[i].is_sticky())
                continue;
            callback(ScrollFrameIndex { i }, m_scroll_frames[i]);
        }
    }

    template<typename Callback>
    void for_each_sticky_frame(Callback callback)
    {
        for (size_t i = 1; i < m_scroll_frames.size(); ++i) {
            if (!m_scroll_frames[i].is_sticky())
                continue;
            callback(ScrollFrameIndex { i }, m_scroll_frames[i]);
        }
    }

    void clear()
    {
        m_scroll_frames.resize_and_keep_capacity(1); // Keep sentinel at index 0
    }

private:
    friend class ViewportPaintable;

    ScrollStateSnapshot snapshot(double device_pixels_per_css_pixel) const
    {
        return ScrollStateSnapshot::create(m_scroll_frames, device_pixels_per_css_pixel);
    }

    Vector<ScrollFrame> m_scroll_frames;
};

}
