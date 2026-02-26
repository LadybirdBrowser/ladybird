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
    static ScrollStateSnapshot create(Vector<NonnullRefPtr<ScrollFrame>> const& scroll_frames, double device_pixels_per_css_pixel);

    Gfx::FloatPoint device_offset_for_frame_with_id(size_t id) const
    {
        if (id >= m_device_offsets.size())
            return {};
        return m_device_offsets[id];
    }

private:
    Vector<Gfx::FloatPoint> m_device_offsets;
};

class ScrollState {
public:
    NonnullRefPtr<ScrollFrame> create_scroll_frame_for(PaintableBox const& paintable_box, RefPtr<ScrollFrame const> parent)
    {
        auto scroll_frame = adopt_ref(*new ScrollFrame(paintable_box, m_scroll_frames.size(), false, move(parent)));
        m_scroll_frames.append(scroll_frame);
        return scroll_frame;
    }

    NonnullRefPtr<ScrollFrame> create_sticky_frame_for(PaintableBox const& paintable_box, RefPtr<ScrollFrame const> parent)
    {
        auto scroll_frame = adopt_ref(*new ScrollFrame(paintable_box, m_scroll_frames.size(), true, move(parent)));
        m_scroll_frames.append(scroll_frame);
        return scroll_frame;
    }

    template<typename Callback>
    void for_each_scroll_frame(Callback callback) const
    {
        for (auto const& scroll_frame : m_scroll_frames) {
            if (scroll_frame->is_sticky())
                continue;
            callback(scroll_frame);
        }
    }

    template<typename Callback>
    void for_each_sticky_frame(Callback callback) const
    {
        for (auto const& scroll_frame : m_scroll_frames) {
            if (!scroll_frame->is_sticky())
                continue;
            callback(scroll_frame);
        }
    }

    void clear()
    {
        m_scroll_frames.clear();
    }

private:
    friend class ViewportPaintable;

    ScrollStateSnapshot snapshot(double device_pixels_per_css_pixel) const
    {
        return ScrollStateSnapshot::create(m_scroll_frames, device_pixels_per_css_pixel);
    }

    Vector<NonnullRefPtr<ScrollFrame>> m_scroll_frames;
};

}
