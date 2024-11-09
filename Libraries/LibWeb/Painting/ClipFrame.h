/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/ScrollFrame.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

struct ClipRectWithScrollFrame {
    CSSPixelRect rect;
    BorderRadiiData corner_radii;
    RefPtr<ScrollFrame const> enclosing_scroll_frame;
};

struct ClipFrame : public RefCounted<ClipFrame> {
    Vector<ClipRectWithScrollFrame> const& clip_rects() const { return m_clip_rects; }
    void add_clip_rect(CSSPixelRect rect, BorderRadiiData radii, RefPtr<ScrollFrame const> enclosing_scroll_frame);

    CSSPixelRect clip_rect_for_hit_testing() const;

private:
    Vector<ClipRectWithScrollFrame> m_clip_rects;
};

}
