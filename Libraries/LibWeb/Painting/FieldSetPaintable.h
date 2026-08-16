/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class FieldSetPaintable final : public Paintable {
public:
    static NonnullRefPtr<FieldSetPaintable> create(Layout::BlockContainer const&);
    virtual StringView class_name() const override { return "FieldSetPaintable"sv; }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;
    virtual void paint_background(DisplayListRecordingContext&) const override;

private:
    explicit FieldSetPaintable(Layout::BlockContainer const&);

    Layout::BlockContainer const& layout_box() const;

    CSSPixels effective_border_top() const;
    CSSPixelRect visual_border_box_rect() const;
};

}
