/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/FieldSetPaintable.h>

namespace Web::Painting {

NonnullRefPtr<FieldSetPaintable> FieldSetPaintable::create(Layout::BlockContainer const& layout_box)
{
    return adopt_ref(*new FieldSetPaintable(layout_box));
}

FieldSetPaintable::FieldSetPaintable(Layout::BlockContainer const& layout_box)
    : Paintable(layout_box)
{
}

Layout::BlockContainer const& FieldSetPaintable::layout_box() const
{
    return static_cast<Layout::BlockContainer const&>(layout_node());
}

// https://html.spec.whatwg.org/multipage/rendering.html#rendered-legend
// If the element's box has a child box that matches the conditions in the list below, then the first such child box
// is the 'fieldset' element's rendered legend:
//   * The child is a legend element.
//   * The child's used value of 'float' is 'none'.
//   * The child's used value of 'position' is not 'absolute' or 'fixed'.
static Layout::Box const* rendered_legend_of_fieldset(Layout::BlockContainer const& fieldset_box)
{
    Layout::Box const* legend = nullptr;
    fieldset_box.for_each_child_of_type<Layout::Box>([&](Layout::Box const& child) {
        if (!child.is_legend_box() || !child.is_in_flow())
            return IterationDecision::Continue;
        legend = &child;
        return IterationDecision::Break;
    });
    return legend;
}

// https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
CSSPixels FieldSetPaintable::effective_border_top() const
{
    // The space allocated for the element's border on the block-start side is expected to be the element's
    // 'border-block-start-width' or the rendered legend's margin box size in the fieldset's block-flow direction,
    // whichever is greater.
    auto css_border_top = layout_box().border_top().width;
    if (auto const* legend = rendered_legend_of_fieldset(layout_box())) {
        auto legend_paintable = legend->paintable_box();
        auto legend_margin_box_height = legend_paintable->box_model().margin.top
            + legend_paintable->absolute_border_box_rect().height()
            + legend_paintable->box_model().margin.bottom;
        return max(css_border_top, legend_margin_box_height);
    }
    return css_border_top;
}

CSSPixelRect FieldSetPaintable::visual_border_box_rect() const
{
    auto css_border_top = layout_box().border_top().width;
    auto allocated_border_top = effective_border_top();

    // The CSS border is painted centered within the effective border area (which may be larger than the CSS border when
    // the legend is taller).
    auto visual_border_box_rect = absolute_border_box_rect();
    if (allocated_border_top <= css_border_top)
        return visual_border_box_rect;

    visual_border_box_rect.take_from_top((allocated_border_top - css_border_top) / 2);
    return visual_border_box_rect;
}

void FieldSetPaintable::paint_background(DisplayListRecordingContext& context) const
{
    auto& recorder = context.display_list_recorder();
    recorder.save();
    recorder.add_clip_rect(context.rounded_device_rect(visual_border_box_rect()).to_type<int>());
    Paintable::paint_background(context);
    recorder.restore();
}

void FieldSetPaintable::paint(DisplayListRecordingContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    if (phase != PaintPhase::Border) {
        Paintable::paint(context, phase);
        return;
    }

    auto const* legend = rendered_legend_of_fieldset(layout_box());
    if (!legend) {
        Paintable::paint(context, phase);
        return;
    }

    auto legend_paintable = legend->paintable_box();

    auto legend_border_rect = context.rounded_device_rect(legend_paintable->absolute_border_box_rect());

    auto top_border_data = layout_box().border_top();
    auto top_border = context.enclosing_device_pixels(top_border_data.width).value();

    auto device_border_rect = context.rounded_device_rect(visual_border_box_rect());

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
        .right = layout_box().border_right(),
        .bottom = layout_box().border_bottom(),
        .left = layout_box().border_left(),
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
