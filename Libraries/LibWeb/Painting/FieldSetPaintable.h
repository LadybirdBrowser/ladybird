/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class FieldSetPaintable final : public PaintableBox {
    GC_CELL(FieldSetPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(FieldSetPaintable);

public:
    static GC::Ref<FieldSetPaintable> create(Layout::FieldSetBox const&);

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;
    virtual void paint_background(DisplayListRecordingContext&) const override;

private:
    explicit FieldSetPaintable(Layout::FieldSetBox const&);

    Layout::FieldSetBox& layout_box();
    Layout::FieldSetBox const& layout_box() const;

    CSSPixels effective_border_top() const;
    CSSPixelRect visual_border_box_rect() const;
};

}
