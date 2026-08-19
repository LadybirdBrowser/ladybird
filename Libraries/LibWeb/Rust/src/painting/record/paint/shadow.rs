/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::painting::border_radii::BorderRadii;
use crate::painting::display_list::commands::{PaintInnerBoxShadow, PaintOuterBoxShadow};
use crate::painting::paintable_data::PaintableSlotId;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::{begin_corner_clip, end_corner_clip};
use libgfx_rust::{Color, CornerClip, CornerRadii, IntRect};

fn add_spread_distance_to_border_radius(border_radius: &mut i32, spread_distance: i32) {
    if *border_radius == 0 || spread_distance == 0 {
        return;
    }
    if *border_radius > spread_distance.abs() {
        *border_radius += spread_distance;
    } else {
        let r = *border_radius as f32 / spread_distance.abs() as f32;
        let base = r - 1.0;
        let cubed = if base == 0.0 { 0.0 } else { base * base * base };
        *border_radius = (*border_radius as f32 + spread_distance as f32 * (1.0 + cubed)) as i32;
    }
}

fn adjust_corners_for_spread_distance(corner_radii: &mut CornerRadii, spread_distance: i32) {
    add_spread_distance_to_border_radius(&mut corner_radii.top_left.horizontal_radius, spread_distance);
    add_spread_distance_to_border_radius(&mut corner_radii.top_left.vertical_radius, spread_distance);
    add_spread_distance_to_border_radius(&mut corner_radii.top_right.horizontal_radius, spread_distance);
    add_spread_distance_to_border_radius(&mut corner_radii.top_right.vertical_radius, spread_distance);
    add_spread_distance_to_border_radius(&mut corner_radii.bottom_right.horizontal_radius, spread_distance);
    add_spread_distance_to_border_radius(&mut corner_radii.bottom_right.vertical_radius, spread_distance);
    add_spread_distance_to_border_radius(&mut corner_radii.bottom_left.horizontal_radius, spread_distance);
    add_spread_distance_to_border_radius(&mut corner_radii.bottom_left.vertical_radius, spread_distance);
}

pub(crate) fn paint_box_shadow(
    recorder: &mut PaintRecorder<'_>,
    paintable: PaintableSlotId,
    bordered_content_rect: CssPixelRect,
    borderless_content_rect: CssPixelRect,
    border_radii: BorderRadii,
) {
    let layout_arena = recorder.layout_arena;
    let Some(style) = layout_arena.node_style_if_live(recorder.data(paintable).layout_node) else {
        return;
    };
    let layers = style.effects().box_shadows.as_slice();
    if layers.is_empty() {
        return;
    }
    let (top, right, bottom, left) = (
        style.border_top_width(),
        style.border_right_width(),
        style.border_bottom_width(),
        style.border_left_width(),
    );
    paint_box_shadow_layers(
        recorder,
        bordered_content_rect,
        borderless_content_rect,
        (top, right, bottom, left),
        border_radii,
        layers,
    );
}

fn paint_box_shadow_layers(
    recorder: &mut PaintRecorder<'_>,
    bordered_content_rect: CssPixelRect,
    borderless_content_rect: CssPixelRect,
    border_widths: (CssPixels, CssPixels, CssPixels, CssPixels),
    border_radii: BorderRadii,
    layers: &[crate::css::computed_value_types::ComputedShadow],
) {
    let converter = recorder.converter;
    let corner_radii = border_radii.as_corners(&converter);

    // Box-shadow layers are ordered front-to-back, so we paint them in reverse.
    for layer in layers.iter().rev() {
        let offset_x = converter.rounded_device_pixels(CssPixels::from_raw(layer.offset_x));
        let offset_y = converter.rounded_device_pixels(CssPixels::from_raw(layer.offset_y));
        let blur_radius = converter.rounded_device_pixels(CssPixels::from_raw(layer.blur_radius));
        let spread_distance = converter.rounded_device_pixels(CssPixels::from_raw(layer.spread_distance));
        let inner = layer.placement == 1;

        let device_content_rect = if inner {
            converter.rounded_device_rect(borderless_content_rect)
        } else {
            converter.rounded_device_rect(bordered_content_rect)
        };

        if inner {
            let mut outer_shadow_rect = IntRect::new(
                device_content_rect.x + offset_x,
                device_content_rect.y + offset_y,
                device_content_rect.width,
                device_content_rect.height,
            );
            let inner_shadow_rect = outer_shadow_rect.inflated_edges(
                -spread_distance,
                -spread_distance,
                -spread_distance,
                -spread_distance,
            );
            outer_shadow_rect = outer_shadow_rect.inflated_edges(
                blur_radius + offset_y,
                blur_radius + offset_x.abs(),
                blur_radius + offset_y.abs(),
                blur_radius + offset_x,
            );

            let mut inner_shadow_corner_radii = corner_radii;
            adjust_corners_for_spread_distance(&mut inner_shadow_corner_radii, -spread_distance);

            let mut shrinked_border_radii = border_radii;
            shrinked_border_radii.shrink(border_widths.0, border_widths.1, border_widths.2, border_widths.3);
            let clipped = begin_corner_clip(
                recorder,
                device_content_rect,
                &shrinked_border_radii,
                CornerClip::Outside,
            );
            recorder.recorder.paint_inner_box_shadow(PaintInnerBoxShadow {
                color: Color(layer.color),
                blur_radius,
                device_content_rect,
                content_corner_radii: corner_radii,
                outer_shadow_rect,
                inner_shadow_rect,
                inner_shadow_corner_radii,
            });
            end_corner_clip(recorder, clipped);
        } else {
            let mut shadow_rect =
                device_content_rect.inflated_edges(spread_distance, spread_distance, spread_distance, spread_distance);
            shadow_rect.x += offset_x;
            shadow_rect.y += offset_y;

            let mut shadow_corner_radii = corner_radii;
            adjust_corners_for_spread_distance(&mut shadow_corner_radii, spread_distance);

            let clipped = begin_corner_clip(recorder, device_content_rect, &border_radii, CornerClip::Inside);
            recorder.recorder.paint_outer_box_shadow(PaintOuterBoxShadow {
                color: Color(layer.color),
                blur_radius,
                device_content_rect,
                content_corner_radii: corner_radii,
                shadow_rect,
                shadow_corner_radii,
            });
            end_corner_clip(recorder, clipped);
        }
    }
}
