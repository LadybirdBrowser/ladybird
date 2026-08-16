/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/SVGMaskable.h>
#include <LibWeb/Painting/SVGPaintable.h>

namespace Web::Painting {

class SVGGraphicsPaintable : public SVGPaintable
    , public SVGMaskable {
public:
    static NonnullRefPtr<SVGGraphicsPaintable> create(Layout::Box const&);
    virtual StringView class_name() const override { return "SVGGraphicsPaintable"sv; }

    virtual GC::Ptr<DOM::Node const> dom_node_of_svg() const override { return dom_node(); }
    virtual Optional<CSSPixelRect> get_mask_area() const override { return get_svg_mask_area(); }
    virtual Optional<Gfx::MaskKind> get_mask_type() const override { return get_svg_mask_type(); }
    virtual Optional<DisplayListResource> calculate_mask(DisplayListRecordingContext& context, CSSPixelRect const& mask_area) const override { return calculate_svg_mask_display_list(context, mask_area); }
    virtual Optional<CSSPixelRect> get_clip_area() const override { return get_svg_clip_area(); }
    virtual Optional<DisplayListResource> calculate_clip(DisplayListRecordingContext& context, CSSPixelRect const& clip_area) const override { return calculate_svg_clip_display_list(context, clip_area); }

protected:
    SVGGraphicsPaintable(Layout::Box const&);

private:
    virtual bool is_svg_graphics_paintable() const final { return true; }
};

template<>
inline bool Paintable::fast_is<SVGGraphicsPaintable>() const { return is_svg_graphics_paintable(); }

}
