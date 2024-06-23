/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Layout/SVGClipBox.h>
#include <LibWeb/Layout/SVGMaskBox.h>
#include <LibWeb/Painting/DisplayListPlayerCPU.h>
#include <LibWeb/Painting/SVGClipPaintable.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Painting {

template<typename T>
static T const* first_child_layout_node_of_type(SVG::SVGGraphicsElement const& graphics_element)
{
    return graphics_element.layout_node()->first_child_of_type<T>();
}

static auto get_mask_box(SVG::SVGGraphicsElement const& graphics_element)
{
    return first_child_layout_node_of_type<Layout::SVGMaskBox>(graphics_element);
}

static auto get_clip_box(SVG::SVGGraphicsElement const& graphics_element)
{
    return first_child_layout_node_of_type<Layout::SVGClipBox>(graphics_element);
}

Optional<CSSPixelRect> SVGMaskable::get_masking_area_of_svg() const
{
    auto const& graphics_element = verify_cast<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    Optional<CSSPixelRect> masking_area = {};
    if (auto* mask_box = get_mask_box(graphics_element)) {
        masking_area = mask_box->dom_node().resolve_masking_area(mask_box->paintable_box()->absolute_border_box_rect());
    }
    if (auto* clip_box = get_clip_box(graphics_element)) {
        // This is a bit ad-hoc, but if we have both a mask and a clip-path, intersect the two areas to find the masking area.
        auto clip_area = clip_box->paintable_box()->absolute_border_box_rect();
        if (masking_area.has_value())
            masking_area = masking_area->intersected(clip_area);
        else
            masking_area = clip_area;
    }
    return masking_area;
}

static Gfx::Bitmap::MaskKind mask_type_to_gfx_mask_kind(CSS::MaskType mask_type)
{
    switch (mask_type) {
    case CSS::MaskType::Alpha:
        return Gfx::Bitmap::MaskKind::Alpha;
    case CSS::MaskType::Luminance:
        return Gfx::Bitmap::MaskKind::Luminance;
    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<Gfx::Bitmap::MaskKind> SVGMaskable::get_mask_type_of_svg() const
{
    auto const& graphics_element = verify_cast<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    if (auto* mask_box = get_mask_box(graphics_element))
        return mask_type_to_gfx_mask_kind(mask_box->computed_values().mask_type());
    if (get_clip_box(graphics_element))
        return Gfx::Bitmap::MaskKind::Alpha;
    return {};
}

RefPtr<Gfx::Bitmap> SVGMaskable::calculate_mask_of_svg(PaintContext& context, CSSPixelRect const& masking_area) const
{
    auto const& graphics_element = verify_cast<SVG::SVGGraphicsElement const>(*dom_node_of_svg());
    auto mask_rect = context.enclosing_device_rect(masking_area);
    auto paint_mask_or_clip = [&](PaintableBox const& paintable) -> RefPtr<Gfx::Bitmap> {
        auto mask_bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, mask_rect.size().to_type<int>());
        RefPtr<Gfx::Bitmap> mask_bitmap = {};
        if (mask_bitmap_or_error.is_error())
            return {};
        mask_bitmap = mask_bitmap_or_error.release_value();
        DisplayList display_list;
        DisplayListRecorder display_list_recorder(display_list);
        display_list_recorder.translate(-mask_rect.location().to_type<int>());
        auto paint_context = context.clone(display_list_recorder);
        paint_context.set_svg_transform(graphics_element.get_transform());
        paint_context.set_draw_svg_geometry_for_clip_path(is<SVGClipPaintable>(paintable));
        StackingContext::paint_node_as_stacking_context(paintable, paint_context);
        DisplayListPlayerCPU executor { *mask_bitmap };
        display_list.execute(executor);
        return mask_bitmap;
    };
    RefPtr<Gfx::Bitmap> mask_bitmap = {};
    if (auto* mask_box = get_mask_box(graphics_element)) {
        auto& mask_paintable = static_cast<PaintableBox const&>(*mask_box->paintable());
        mask_bitmap = paint_mask_or_clip(mask_paintable);
    }
    if (auto* clip_box = get_clip_box(graphics_element)) {
        auto& clip_paintable = static_cast<PaintableBox const&>(*clip_box->paintable());
        auto clip_bitmap = paint_mask_or_clip(clip_paintable);
        // Combine the clip-path with the mask (if present).
        if (mask_bitmap && clip_bitmap)
            mask_bitmap->apply_mask(*clip_bitmap, Gfx::Bitmap::MaskKind::Alpha);
        if (!mask_bitmap)
            mask_bitmap = clip_bitmap;
    }
    return mask_bitmap;
}

}
