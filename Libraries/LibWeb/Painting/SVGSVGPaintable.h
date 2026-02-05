/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class SVGSVGPaintable final : public PaintableBox {
    GC_CELL(SVGSVGPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(SVGSVGPaintable);

public:
    static GC::Ref<SVGSVGPaintable> create(Layout::SVGSVGBox const&);

    static void paint_svg_box(DisplayListRecordingContext& context, PaintableBox const& svg_box, PaintPhase phase);
    static void paint_descendants(DisplayListRecordingContext& context, PaintableBox const& paintable, PaintPhase phase);

protected:
    SVGSVGPaintable(Layout::SVGSVGBox const&);

private:
    virtual bool is_svg_svg_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<SVGSVGPaintable>() const { return is_svg_svg_paintable(); }

}
