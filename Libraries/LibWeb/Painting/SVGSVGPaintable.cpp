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

void SVGSVGPaintable::paint_svg_box(DisplayListRecordingContext& context, PaintableBox const& svg_box, PaintPhase phase)
{
    auto const& computed_values = svg_box.computed_values();

    auto const& filter = computed_values.filter();
    auto masking_area = svg_box.get_masking_area();

    Gfx::CompositingAndBlendingOperator compositing_and_blending_operator = mix_blend_mode_to_compositing_and_blending_operator(computed_values.mix_blend_mode());

    Optional<Gfx::Filter> resolved_filter;
    if (filter.has_filters())
        resolved_filter = svg_box.resolve_filter(context, filter);

    auto needs_effects_layer = computed_values.opacity() < 1 || resolved_filter.has_value() || compositing_and_blending_operator != Gfx::CompositingAndBlendingOperator::Normal;
    auto needs_to_save_state = computed_values.isolation() == CSS::Isolation::Isolate || masking_area.has_value();

    auto effective_state = svg_box.accumulated_visual_context();
    bool has_own_transform = svg_box.has_css_transform();
    auto stacking_state = (has_own_transform && effective_state) ? effective_state->parent() : effective_state;

    context.display_list_recorder().set_accumulated_visual_context(stacking_state);
    if (needs_effects_layer) {
        context.display_list_recorder().apply_effects(computed_values.opacity(), compositing_and_blending_operator, resolved_filter);
    } else if (needs_to_save_state) {
        context.display_list_recorder().save();
    }
    context.display_list_recorder().set_accumulated_visual_context(effective_state);

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

    context.display_list_recorder().set_accumulated_visual_context(effective_state);

    if (needs_effects_layer || needs_to_save_state)
        context.display_list_recorder().restore();
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
