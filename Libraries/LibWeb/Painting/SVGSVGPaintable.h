/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>

namespace Web::Painting {

class SVGSVGPaintable final : public PaintableBox {
public:
    static NonnullRefPtr<SVGSVGPaintable> create(Layout::SVGSVGBox const&);
    virtual StringView class_name() const override { return "SVGSVGPaintable"sv; }

    static void paint_svg_box(DisplayListRecordingContext& context, PaintableBox const& svg_box, PaintPhase phase);
    static void paint_descendants(DisplayListRecordingContext& context, PaintableBox const& paintable, PaintPhase phase);

    void set_computed_transforms(SVGGraphicsPaintable::ComputedTransforms computed_transforms) { m_computed_transforms = computed_transforms; }
    SVGGraphicsPaintable::ComputedTransforms const& computed_transforms() const { return m_computed_transforms; }

protected:
    SVGSVGPaintable(Layout::SVGSVGBox const&);

private:
    virtual bool is_svg_svg_paintable() const final { return true; }

    SVGGraphicsPaintable::ComputedTransforms m_computed_transforms;
};

template<>
inline bool Paintable::fast_is<SVGSVGPaintable>() const { return is_svg_svg_paintable(); }

}
