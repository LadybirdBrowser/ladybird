/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Painting/MediaPaintable.h>

namespace Web::Painting {

class VideoPaintable final : public MediaPaintable {
    JS_CELL(VideoPaintable, MediaPaintable);
    JS_DECLARE_ALLOCATOR(VideoPaintable);

public:
    static JS::NonnullGCPtr<VideoPaintable> create(Layout::VideoBox const&);

    virtual void paint(PaintContext&, PaintPhase) const override;

    Layout::VideoBox& layout_box();
    Layout::VideoBox const& layout_box() const;

private:
    VideoPaintable(Layout::VideoBox const&);

    void paint_placeholder_video_controls(PaintContext&, DevicePixelRect video_rect, Optional<DevicePixelPoint> const& mouse_position) const;
};

}
