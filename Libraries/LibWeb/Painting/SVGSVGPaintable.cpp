/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibWeb/Layout/ImageBox.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/Painting/StackingContext.h>

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

static Gfx::FloatMatrix4x4 matrix_with_scaled_translation(Gfx::FloatMatrix4x4 matrix, float scale)
{
    matrix[0, 3] *= scale;
    matrix[1, 3] *= scale;
    matrix[2, 3] *= scale;
    return matrix;
}

void SVGSVGPaintable::paint_svg_box(DisplayListRecordingContext& context, PaintableBox const& svg_box, PaintPhase phase)
{
    auto const& computed_values = svg_box.computed_values();

    auto const& filter = computed_values.filter();
    auto masking_area = svg_box.get_masking_area();

    Gfx::CompositingAndBlendingOperator compositing_and_blending_operator = mix_blend_mode_to_compositing_and_blending_operator(computed_values.mix_blend_mode());

    auto needs_to_save_state = computed_values.isolation() == CSS::Isolation::Isolate || compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal || svg_box.has_css_transform() || masking_area.has_value();

    if (needs_to_save_state) {
        context.display_list_recorder().save();
    }

    if (computed_values.opacity() < 1) {
        context.display_list_recorder().apply_opacity(computed_values.opacity());
    }

    auto filter_applied = false;
    if (filter.has_filters()) {
        if (auto resolved_filter = svg_box.resolve_filter(context, filter); resolved_filter.has_value()) {
            context.display_list_recorder().apply_filter(*resolved_filter);
            filter_applied = true;
        }
    }

    if (compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal) {
        context.display_list_recorder().apply_compositing_and_blending_operator(compositing_and_blending_operator);
    }

    if (svg_box.has_css_transform()) {
        auto transform_matrix = svg_box.transform();
        Gfx::FloatPoint transform_origin = svg_box.transform_origin().template to_type<float>();
        auto to_device_pixels_scale = float(context.device_pixels_per_css_pixel());
        context.display_list_recorder().apply_transform(transform_origin.scaled(to_device_pixels_scale), matrix_with_scaled_translation(transform_matrix, to_device_pixels_scale));
    }

    bool skip_painting = false;
    if (masking_area.has_value()) {
        if (masking_area->is_empty()) {
            skip_painting = true;
        } else {
            auto mask_bitmap = svg_box.calculate_mask(context, *masking_area);
            if (mask_bitmap) {
                auto source_paintable_rect = context.enclosing_device_rect(*masking_area).template to_type<int>();
                auto origin = source_paintable_rect.location();
                context.display_list_recorder().apply_mask_bitmap(origin, mask_bitmap.release_nonnull(), *svg_box.get_mask_type());
            }
        }
    }

    if (!skip_painting) {
        svg_box.paint(context, PaintPhase::Foreground);
        paint_descendants(context, svg_box, phase);
    }

    if (compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal) {
        context.display_list_recorder().restore();
    }

    if (filter_applied) {
        context.display_list_recorder().restore();
    }

    if (computed_values.opacity() < 1) {
        context.display_list_recorder().restore();
    }

    if (needs_to_save_state) {
        context.display_list_recorder().restore();
    }
}

void SVGSVGPaintable::paint_descendants(DisplayListRecordingContext& context, PaintableBox const& paintable, PaintPhase phase)
{
    if (phase != PaintPhase::Foreground)
        return;

    paintable.for_each_child_of_type<PaintableBox>([&](PaintableBox& child) {
        paint_svg_box(context, child, phase);
        return IterationDecision::Continue;
    });
}

}
