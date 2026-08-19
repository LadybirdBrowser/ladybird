/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::painting::border_radii::BorderRadii;
use crate::painting::display_list::recorder::{PaintStyleOrColor, StrokePathParams};
use crate::painting::paintable_data::PaintableSlotId;
use crate::painting::paintable_geometry;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::border::{BorderDataDevicePixels, BordersDataDevicePixels, paint_all_borders};
use libgfx_rust::{CapStyle, Color, JoinStyle, ShouldAntiAlias};

pub(crate) fn paint_outline_phase(recorder: &mut PaintRecorder<'_>, paintable: PaintableSlotId) {
    let node = recorder.data(paintable).layout_node;
    let outline = crate::painting::style_queries::outline_data(
        recorder.layout_arena,
        node,
        recorder.inputs.window_is_focused,
        recorder.inputs.outline_auto_color,
    );
    let outline_offset = crate::painting::style_queries::outline_offset(recorder.layout_arena, node);
    let border_box_rect = paintable_geometry::absolute_border_box_rect(recorder.paintables, paintable);
    let border_radii = BorderRadii::from_raw(recorder.hit_test_facts(paintable).border_radii);
    paint_outline(recorder, outline, outline_offset, border_box_rect, border_radii);
    let facts = recorder.paint_host.outline_facts(recorder.shell(paintable));
    paint_focused_area_outline(recorder, paintable, &facts);
}

pub(crate) fn paint_outline(
    recorder: &mut PaintRecorder<'_>,
    outline: Option<crate::painting::style_queries::OutlineData>,
    outline_offset: CssPixels,
    border_box_rect: crate::css::css_pixels::CssPixelRect,
    border_radii: BorderRadii,
) {
    let Some(outline) = outline else {
        return;
    };
    let converter = recorder.converter;
    let mut border_radius_data = border_radii;
    let mut borders_rect = border_box_rect;
    let two = CssPixels::from_integer(2);
    let zero = CssPixels::from_raw(0);

    let mut outline_offset_x = outline_offset;
    let mut outline_offset_y = outline_offset;
    // https://www.w3.org/TR/css-ui-4/#outline-offset
    if borders_rect.width.div_as_fraction(two) + outline_offset_x < zero {
        outline_offset_x = (-borders_rect.width).div_as_fraction(two);
    }
    if borders_rect.height.div_as_fraction(two) + outline_offset_y < zero {
        outline_offset_y = (-borders_rect.height).div_as_fraction(two);
    }

    let width = outline.width;
    border_radius_data.inflate(
        width + outline_offset_y,
        width + outline_offset_x,
        width + outline_offset_y,
        width + outline_offset_x,
    );
    borders_rect.inflate(
        width + outline_offset_y,
        width + outline_offset_x,
        width + outline_offset_y,
        width + outline_offset_x,
    );

    let side = BorderDataDevicePixels {
        color: Color(outline.color),
        line_style: outline.line_style,
        width: converter.enclosing_device_pixels(width),
    };
    let borders_data = BordersDataDevicePixels {
        top: side,
        right: side,
        bottom: side,
        left: side,
    };
    paint_all_borders(
        &mut recorder.recorder,
        converter.rounded_device_rect(borders_rect),
        border_radius_data.as_corners(&converter),
        &borders_data,
    );
}

fn paint_focused_area_outline(
    recorder: &mut PaintRecorder<'_>,
    paintable: PaintableSlotId,
    facts: &crate::painting::host::FfiOutlineFacts,
) {
    // https://html.spec.whatwg.org/multipage/interaction.html#focusable-area
    // The shapes of area elements in an image map associated with an img element that is being rendered and is not
    // inert.
    // NB: Focused area elements have no paintable of their own, so the image whose rendering makes the area's shape a
    // focusable area paints the focus outline along that shape.
    if !facts.paints_focused_area_outline || facts.focused_area_path.is_null() {
        return;
    }
    // SAFETY: The host hands out a heap-allocated Gfx::Path the receiver owns.
    let path = unsafe { libgfx_rust::path::OwnedPath::adopt(facts.focused_area_path) };
    let converter = recorder.converter;
    let image_rect = paintable_geometry::absolute_rect(recorder.paintables, paintable);
    let scale = recorder.inputs.device_pixels_per_css_pixel as f32;
    let device_origin = converter.rounded_device_point(image_rect.location());
    let transformed = path.copy_transformed([scale, 0.0, 0.0, scale, device_origin.x as f32, device_origin.y as f32]);

    // AD-HOC: Only the user agent focus ring is painted. Other engines do not let author outline values style the
    // focus indicator of an image map area.
    recorder.recorder.save();
    recorder
        .recorder
        .add_clip_rect_int(converter.enclosing_device_rect(image_rect));
    recorder.recorder.stroke_path(StrokePathParams {
        cap_style: CapStyle::Round,
        join_style: JoinStyle::Round,
        miter_limit: 4.0,
        dash_array: Vec::new(),
        dash_offset: 0.0,
        path: &transformed,
        opacity: 1.0,
        paint_style_or_color: PaintStyleOrColor::Color(Color(facts.focused_area_color)),
        thickness: facts.focused_area_width.to_double() as f32 * scale,
        should_anti_alias: ShouldAntiAlias::Yes,
    });
    recorder.recorder.restore();
}
