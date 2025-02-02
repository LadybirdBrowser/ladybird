/*
 * Copyright (c) 2024, Kostya Farber <kostya.farber@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

void FieldSetPaintable::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    if (phase != PaintPhase::Border) {
        PaintableBox::paint(context, phase);
        return;
    }

    if (!(layout_box().has_rendered_legend())) {
        PaintableBox::paint(context, phase);
        return;
    }

    auto& display_list_recorder = context.display_list_recorder();

    auto const* legend_box = layout_box().first_child_of_type<Layout::LegendBox>();
    auto const* const legend_paintable = legend_box->paintable_box();

    auto legend_border_rect = context.rounded_device_rect(legend_paintable->absolute_border_box_rect());
    auto fieldset_border_rect = context.rounded_device_rect(absolute_border_box_rect());

    BordersData borders_data = BordersData {
        .top = CSS::BorderData(),
        .right = box_model().border.right == 0 ? CSS::BorderData() : computed_values().border_right(),
        .bottom = box_model().border.bottom == 0 ? CSS::BorderData() : computed_values().border_bottom(),
        .left = box_model().border.left == 0 ? CSS::BorderData() : computed_values().border_left(),
    };

    paint_all_borders(display_list_recorder, fieldset_border_rect, normalized_border_radii_data().as_corners(context), borders_data.to_device_pixels(context));

    auto top_border_data = box_model().border.top == 0 ? CSS::BorderData() : computed_values().border_top();
    auto top_border = context.enclosing_device_pixels(top_border_data.width).value();

    // if fieldset has a rendered legend, the top border is not
    // expected to be painted behind the border box of the legend
    DevicePixelRect left_segment = {
        fieldset_border_rect.x(),
        fieldset_border_rect.y(),
        legend_border_rect.x() - fieldset_border_rect.x(),
        top_border
    };

    DevicePixelRect right_segment = {
        legend_border_rect.right(),
        fieldset_border_rect.y(),
        fieldset_border_rect.right() - legend_border_rect.right(),
        top_border
    };

    BordersData top_border_only = BordersData {
        .top = top_border_data,
        .right = CSS::BorderData(),
        .bottom = CSS::BorderData(),
        .left = CSS::BorderData(),
    };

    display_list_recorder.save();
    display_list_recorder.add_clip_rect(left_segment.to_type<int>());
    paint_all_borders(display_list_recorder, fieldset_border_rect, normalized_border_radii_data().as_corners(context), top_border_only.to_device_pixels(context));
    display_list_recorder.restore();

    display_list_recorder.save();
    display_list_recorder.add_clip_rect(right_segment.to_type<int>());
    paint_all_borders(
        display_list_recorder,
        fieldset_border_rect,
        normalized_border_radii_data().as_corners(context),
        top_border_only.to_device_pixels(context));
    display_list_recorder.restore();
}

}
