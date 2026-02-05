/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/ImmutableBitmap.h>
#include <LibWeb/Layout/SVGForeignObjectBox.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/SVGMaskable.h>

namespace Web::Painting {

class SVGForeignObjectPaintable final : public PaintableWithLines
    , public SVGMaskable {
    GC_CELL(SVGForeignObjectPaintable, PaintableWithLines);
    GC_DECLARE_ALLOCATOR(SVGForeignObjectPaintable);

public:
    static GC::Ref<SVGForeignObjectPaintable> create(Layout::SVGForeignObjectBox const&);

    virtual TraversalDecision hit_test(CSSPixelPoint, HitTestType, Function<TraversalDecision(HitTestResult)> const& callback) const override;

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

    Layout::SVGForeignObjectBox const& layout_box() const;

    virtual GC::Ptr<DOM::Node const> dom_node_of_svg() const override { return dom_node(); }
    virtual Optional<CSSPixelRect> get_mask_area() const override { return get_svg_mask_area(); }
    virtual Optional<Gfx::MaskKind> get_mask_type() const override { return get_svg_mask_type(); }
    virtual RefPtr<DisplayList> calculate_mask(DisplayListRecordingContext& context, CSSPixelRect const& mask_area) const override { return calculate_svg_mask_display_list(context, mask_area); }
    virtual Optional<CSSPixelRect> get_clip_area() const override { return get_svg_clip_area(); }
    virtual RefPtr<DisplayList> calculate_clip(DisplayListRecordingContext& context, CSSPixelRect const& clip_area) const override { return calculate_svg_clip_display_list(context, clip_area); }

protected:
    SVGForeignObjectPaintable(Layout::SVGForeignObjectBox const&);
};

}
