/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/ScrollFrame.h>

namespace Web::Painting {

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

    CSSPixelPoint cumulative_offset_for_frame_with_id(size_t id) const
    {
        return m_scroll_frames[id]->cumulative_offset();
    }

    CSSPixelPoint own_offset_for_frame_with_id(size_t id) const
    {
        return m_scroll_frames[id]->own_offset();
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

private:
    Vector<NonnullRefPtr<ScrollFrame>> m_scroll_frames;
};

}
