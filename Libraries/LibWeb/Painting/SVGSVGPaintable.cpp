/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(SVGSVGPaintable);

GC::Ref<SVGSVGPaintable> SVGSVGPaintable::create(Layout::SVGSVGBox const& layout_box)
{
    return layout_box.heap().allocate<SVGSVGPaintable>(layout_box);
}

SVGSVGPaintable::SVGSVGPaintable(Layout::SVGSVGBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::SVGSVGBox const& SVGSVGPaintable::layout_box() const
{
    return static_cast<Layout::SVGSVGBox const&>(layout_node());
}

void SVGSVGPaintable::before_children_paint(PaintContext& context, PaintPhase phase) const
{
    PaintableBox::before_children_paint(context, phase);
    if (phase != PaintPhase::Foreground)
        return;
    context.display_list_recorder().push_scroll_frame_id(scroll_frame_id());
}

void SVGSVGPaintable::after_children_paint(PaintContext& context, PaintPhase phase) const
{
    PaintableBox::after_children_paint(context, phase);
    if (phase != PaintPhase::Foreground)
        return;
    context.display_list_recorder().pop_scroll_frame_id();
}

static Gfx::FloatMatrix4x4 matrix_with_scaled_translation(Gfx::FloatMatrix4x4 matrix, float scale)
{
    auto* m = matrix.elements();
    m[0][3] *= scale;
    m[1][3] *= scale;
    m[2][3] *= scale;
    return matrix;
}

void SVGSVGPaintable::paint_descendants(PaintContext& context, PaintableBox const& paintable, PaintPhase phase)
{
    if (phase != PaintPhase::Foreground)
        return;

    auto paint_svg_box = [&](auto& svg_box) {
        auto const& computed_values = svg_box.computed_values();

        auto masking_area = svg_box.get_masking_area();
        auto needs_to_save_state = computed_values.opacity() < 1 || svg_box.has_css_transform() || svg_box.get_masking_area().has_value();

        if (needs_to_save_state) {
            context.display_list_recorder().save();
        }

        if (computed_values.opacity() < 1) {
            context.display_list_recorder().apply_opacity(computed_values.opacity());
        }

        if (svg_box.has_css_transform()) {
            auto transform_matrix = svg_box.transform();
            Gfx::FloatPoint transform_origin = svg_box.transform_origin().template to_type<float>();
            auto to_device_pixels_scale = float(context.device_pixels_per_css_pixel());
            context.display_list_recorder().apply_transform(transform_origin.scaled(to_device_pixels_scale), matrix_with_scaled_translation(transform_matrix, to_device_pixels_scale));
        }

        if (masking_area.has_value()) {
            if (masking_area->is_empty())
                return;
            auto mask_bitmap = svg_box.calculate_mask(context, *masking_area);
            if (mask_bitmap) {
                auto source_paintable_rect = context.enclosing_device_rect(*masking_area).template to_type<int>();
                auto origin = source_paintable_rect.location();
                context.display_list_recorder().apply_mask_bitmap(origin, mask_bitmap.release_nonnull(), *svg_box.get_mask_type());
            }
        }

        svg_box.before_paint(context, PaintPhase::Foreground);
        svg_box.paint(context, PaintPhase::Foreground);
        svg_box.after_paint(context, PaintPhase::Foreground);

        paint_descendants(context, svg_box, phase);

        if (needs_to_save_state) {
            context.display_list_recorder().restore();
        }
    };

    paintable.before_children_paint(context, PaintPhase::Foreground);
    paintable.for_each_child_of_type<PaintableBox>([&](PaintableBox& child) {
        paint_svg_box(child);
        return IterationDecision::Continue;
    });
    paintable.after_children_paint(context, PaintPhase::Foreground);
}

}
