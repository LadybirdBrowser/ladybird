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
    NonnullRefPtr<ScrollFrame> create_scroll_frame_for(PaintableBox const& paintable, RefPtr<ScrollFrame const> parent)
    {
        auto scroll_frame = adopt_ref(*new ScrollFrame(paintable, m_next_id++, parent));
        m_scroll_frames.append(scroll_frame);
        return scroll_frame;
    }

    NonnullRefPtr<ScrollFrame> create_sticky_frame_for(PaintableBox const& paintable, RefPtr<ScrollFrame const> parent)
    {
        auto scroll_frame = adopt_ref(*new ScrollFrame(paintable, m_next_id++, parent));
        m_sticky_frames.append(scroll_frame);
        return scroll_frame;
    }

    void clear()
    {
        m_scroll_frames.clear();
    }

    Vector<NonnullRefPtr<ScrollFrame>> const& scroll_frames() const { return m_scroll_frames; }
    Vector<NonnullRefPtr<ScrollFrame>> const& sticky_frames() const { return m_sticky_frames; }

private:
    size_t m_next_id { 0 };
    Vector<NonnullRefPtr<ScrollFrame>> m_scroll_frames;
    Vector<NonnullRefPtr<ScrollFrame>> m_sticky_frames;
};

}
