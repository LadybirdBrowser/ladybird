/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/ClippableAndScrollable.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/PaintContext.h>

namespace Web::Painting {

Optional<int> ClippableAndScrollable::own_scroll_frame_id() const
{
    if (m_own_scroll_frame)
        return m_own_scroll_frame->id();
    return {};
}

Optional<int> ClippableAndScrollable::scroll_frame_id() const
{
    if (m_enclosing_scroll_frame)
        return m_enclosing_scroll_frame->id();
    return {};
}

CSSPixelPoint ClippableAndScrollable::cumulative_offset_of_enclosing_scroll_frame() const
{
    if (m_enclosing_scroll_frame)
        return m_enclosing_scroll_frame->cumulative_offset();
    return {};
}

Optional<CSSPixelRect> ClippableAndScrollable::clip_rect_for_hit_testing() const
{
    if (m_enclosing_clip_frame)
        return m_enclosing_clip_frame->clip_rect_for_hit_testing();
    return {};
}

void ClippableAndScrollable::apply_clip(PaintContext& context) const
{
    if (!m_enclosing_clip_frame)
        return;
    auto const& clip_rects = m_enclosing_clip_frame->clip_rects();
    if (clip_rects.is_empty())
        return;

    auto& display_list_recorder = context.display_list_recorder();
    display_list_recorder.save();
    for (auto const& clip_rect : clip_rects) {
        Optional<i32> clip_scroll_frame_id;
        if (clip_rect.enclosing_scroll_frame)
            clip_scroll_frame_id = clip_rect.enclosing_scroll_frame->id();
        display_list_recorder.push_scroll_frame_id(clip_scroll_frame_id);
        auto rect = context.rounded_device_rect(clip_rect.rect).to_type<int>();
        auto corner_radii = clip_rect.corner_radii.as_corners(context);
        if (corner_radii.has_any_radius()) {
            display_list_recorder.add_rounded_rect_clip(corner_radii, rect, CornerClip::Outside);
        } else {
            display_list_recorder.add_clip_rect(rect);
        }
        display_list_recorder.pop_scroll_frame_id();
    }
}

void ClippableAndScrollable::restore_clip(PaintContext& context) const
{
    if (!m_enclosing_clip_frame)
        return;
    auto const& clip_rects = m_enclosing_clip_frame->clip_rects();
    if (clip_rects.is_empty())
        return;

    context.display_list_recorder().restore();
}

}
