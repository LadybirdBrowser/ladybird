/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/FieldSetBox.h>
#include <LibWeb/Layout/LegendBox.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/FieldSetPaintable.h>

namespace Web::Painting {

GC_DEFINE_ALLOCATOR(FieldSetPaintable);

GC::Ref<FieldSetPaintable> FieldSetPaintable::create(Layout::FieldSetBox const& layout_box)
{
    return layout_box.heap().allocate<FieldSetPaintable>(layout_box);
}

FieldSetPaintable::FieldSetPaintable(Layout::FieldSetBox const& layout_box)
    : PaintableBox(layout_box)
{
}

Layout::FieldSetBox& FieldSetPaintable::layout_box()
{
    return static_cast<Layout::FieldSetBox&>(layout_node());
}

Layout::FieldSetBox const& FieldSetPaintable::layout_box() const
{
    return static_cast<Layout::FieldSetBox const&>(layout_node());
}

// https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
CSSPixels FieldSetPaintable::effective_border_top() const
{
    // The space allocated for the element's border on the block-start side is expected to be the element's
    // 'border-block-start-width' or the rendered legend's margin box size in the fieldset's block-flow direction,
    // whichever is greater.
    auto css_border_top = computed_values().border_top().width;
    if (auto legend = layout_box().rendered_legend()) {
        auto const* legend_paintable = legend->paintable_box();
        auto legend_margin_box_height = legend_paintable->box_model().margin.top
            + legend_paintable->absolute_border_box_rect().height()
            + legend_paintable->box_model().margin.bottom;
        return max(css_border_top, legend_margin_box_height);
    }
    return css_border_top;
}

void FieldSetPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    if (phase != PaintPhase::Border) {
        PaintableBox::paint(context, phase);
        return;
    }

    auto legend = layout_box().rendered_legend();
    if (!legend) {
        PaintableBox::paint(context, phase);
        return;
    }

    auto const* legend_paintable = legend->paintable_box();

    auto legend_border_rect = context.rounded_device_rect(legend_paintable->absolute_border_box_rect());
    auto fieldset_border_rect = context.rounded_device_rect(absolute_border_box_rect());

    auto top_border_data = computed_values().border_top();
    auto top_border = context.enclosing_device_pixels(top_border_data.width).value();

    // The CSS border is painted centered within the effective border area (which may be larger than the CSS border
    // when the legend is taller).
    auto allocated_border_top = context.enclosing_device_pixels(effective_border_top()).value();
    auto device_border_rect = fieldset_border_rect;
    if (allocated_border_top > top_border) {
        auto border_y_offset = (allocated_border_top - top_border) / 2;
        device_border_rect.set_y(fieldset_border_rect.y() + border_y_offset);
        device_border_rect.set_height(fieldset_border_rect.height() - border_y_offset);
    }

    auto& display_list_recorder = context.display_list_recorder();
    auto paint_borders_with_optional_clip = [&](BordersDataDevicePixels borders, Optional<Gfx::IntRect> clip) {
        if (clip.has_value()) {
            display_list_recorder.save();
            display_list_recorder.add_clip_rect(clip.value());
        }

        auto corners = normalized_border_radii_data().as_corners(context.device_pixel_converter());
        paint_all_borders(display_list_recorder, device_border_rect, corners, borders);

        if (clip.has_value())
            display_list_recorder.restore();
    };

    BordersData borders_data {
        .top = {},
        .right = computed_values().border_right(),
        .bottom = computed_values().border_bottom(),
        .left = computed_values().border_left(),
    };
    paint_borders_with_optional_clip(borders_data.to_device_pixels(context), {});

    // The top border is not expected to be painted behind the border box of the legend.
    auto top_border_only = BordersData { .top = top_border_data, .right = {}, .bottom = {}, .left = {} }.to_device_pixels(context);

    DevicePixelRect left_segment = {
        device_border_rect.x(),
        device_border_rect.y(),
        legend_border_rect.x() - device_border_rect.x(),
        top_border
    };
    paint_borders_with_optional_clip(top_border_only, left_segment.to_type<int>());

    DevicePixelRect right_segment = {
        legend_border_rect.right(),
        device_border_rect.y(),
        device_border_rect.right() - legend_border_rect.right(),
        top_border
    };
    paint_borders_with_optional_clip(top_border_only, right_segment.to_type<int>());
}

}
