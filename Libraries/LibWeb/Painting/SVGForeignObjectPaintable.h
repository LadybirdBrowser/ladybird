/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/SVGForeignObjectBox.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/SVGMaskable.h>

namespace Web::Painting {

class SVGForeignObjectPaintable final : public PaintableWithLines
    , public SVGMaskable {
    JS_CELL(SVGForeignObjectPaintable, PaintableWithLines);
    JS_DECLARE_ALLOCATOR(SVGForeignObjectPaintable);

public:
    static JS::NonnullGCPtr<SVGForeignObjectPaintable> create(Layout::SVGForeignObjectBox const&);

    virtual TraversalDecision hit_test(CSSPixelPoint, HitTestType, Function<TraversalDecision(HitTestResult)> const& callback) const override;

    virtual void paint(PaintContext&, PaintPhase) const override;

    Layout::SVGForeignObjectBox const& layout_box() const;

    virtual JS::GCPtr<DOM::Node const> dom_node_of_svg() const override { return dom_node(); }
    virtual Optional<CSSPixelRect> get_masking_area() const override { return get_masking_area_of_svg(); }
    virtual Optional<Gfx::Bitmap::MaskKind> get_mask_type() const override { return get_mask_type_of_svg(); }
    virtual RefPtr<Gfx::Bitmap> calculate_mask(PaintContext& paint_context, CSSPixelRect const& masking_area) const override { return calculate_mask_of_svg(paint_context, masking_area); }

protected:
    SVGForeignObjectPaintable(Layout::SVGForeignObjectBox const&);
};

}
