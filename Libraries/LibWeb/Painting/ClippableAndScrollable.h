/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/ClipFrame.h>
#include <LibWeb/Painting/ScrollFrame.h>

namespace Web::Painting {

class ClippableAndScrollable {
public:
    virtual ~ClippableAndScrollable() = default;

    void set_enclosing_scroll_frame(RefPtr<ScrollFrame> scroll_frame) { m_enclosing_scroll_frame = scroll_frame; }
    void set_enclosing_clip_frame(RefPtr<ClipFrame> clip_frame) { m_enclosing_clip_frame = clip_frame; }

    [[nodiscard]] RefPtr<ScrollFrame const> enclosing_scroll_frame() const { return m_enclosing_scroll_frame; }
    [[nodiscard]] Optional<int> scroll_frame_id() const;
    [[nodiscard]] CSSPixelPoint cumulative_offset_of_enclosing_scroll_frame() const;
    [[nodiscard]] Optional<CSSPixelRect> clip_rect_for_hit_testing() const;

    [[nodiscard]] RefPtr<ScrollFrame const> own_scroll_frame() const { return m_own_scroll_frame; }
    [[nodiscard]] Optional<int> own_scroll_frame_id() const;
    [[nodiscard]] CSSPixelPoint own_scroll_frame_offset() const
    {
        if (m_own_scroll_frame)
            return m_own_scroll_frame->own_offset();
        return {};
    }
    void set_own_scroll_frame(RefPtr<ScrollFrame> scroll_frame) { m_own_scroll_frame = scroll_frame; }

    void apply_clip(PaintContext&) const;
    void restore_clip(PaintContext&) const;

    Gfx::AffineTransform const& combined_css_transform() const { return m_combined_css_transform; }
    void set_combined_css_transform(Gfx::AffineTransform const& transform) { m_combined_css_transform = transform; }

private:
    RefPtr<ScrollFrame const> m_enclosing_scroll_frame;
    RefPtr<ScrollFrame const> m_own_scroll_frame;
    RefPtr<ClipFrame const> m_enclosing_clip_frame;

    Gfx::AffineTransform m_combined_css_transform;
};

}
