/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/ClippableAndScrollable.h>

namespace Web::Painting {

Optional<int> ClippableAndScrollable::scroll_frame_id() const
{
    if (m_enclosing_scroll_frame)
        return m_enclosing_scroll_frame->id;
    return {};
}

CSSPixelPoint ClippableAndScrollable::enclosing_scroll_frame_offset() const
{
    if (m_enclosing_scroll_frame)
        return m_enclosing_scroll_frame->offset;
    return {};
}

Optional<CSSPixelRect> ClippableAndScrollable::clip_rect() const
{
    if (m_enclosing_clip_frame)
        return m_enclosing_clip_frame->rect();
    return {};
}

Span<BorderRadiiClip const> ClippableAndScrollable::border_radii_clips() const
{
    if (m_enclosing_clip_frame)
        return m_enclosing_clip_frame->border_radii_clips();
    return {};
}

}
