/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/ClipFrame.h>

namespace Web::Painting {

void ClipFrame::add_clip_rect(CSSPixelRect rect, BorderRadiiData radii, RefPtr<ScrollFrame const> enclosing_scroll_frame)
{
    for (auto& existing_clip : m_clip_rects) {
        if (rect == existing_clip.rect && enclosing_scroll_frame == existing_clip.enclosing_scroll_frame) {
            existing_clip.corner_radii.union_max_radii(radii);
            return;
        }
    }
    m_clip_rects.append({ rect, radii, move(enclosing_scroll_frame) });
}

CSSPixelRect ClipFrame::clip_rect_for_hit_testing() const
{
    VERIFY(!m_clip_rects.is_empty());
    auto rect = m_clip_rects[0].rect;
    if (m_clip_rects[0].enclosing_scroll_frame) {
        rect.translate_by(m_clip_rects[0].enclosing_scroll_frame->cumulative_offset());
    }
    for (size_t i = 1; i < m_clip_rects.size(); ++i) {
        auto clip_rect = m_clip_rects[i].rect;
        if (m_clip_rects[i].enclosing_scroll_frame) {
            clip_rect.translate_by(m_clip_rects[i].enclosing_scroll_frame->cumulative_offset());
        }
        rect.intersect(clip_rect);
    }
    return rect;
}

}
