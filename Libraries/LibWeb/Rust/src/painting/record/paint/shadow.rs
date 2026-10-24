/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::paint_read::PaintRead;
use crate::painting::record::trace::Observer;

use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::painting::border_radii::BorderRadii;
use crate::painting::corner_shape::{CornerShapes, Insets, border_contour_path};
use crate::painting::display_list::commands::{PaintInnerBoxShadow, PaintOuterBoxShadow};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::record::PaintRecorder;
use libgfx_rust::path::Path;
use libgfx_rust::{Color, CornerRadii, FloatPoint, IntRect};

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

pub(crate) fn paint_box_shadow<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    bordered_content_rect: CssPixelRect,
    borderless_content_rect: CssPixelRect,
    border_radii: BorderRadii,
) {
    let layout_arena = recorder.source;
    let Some(style) = layout_arena.node_style_if_live(paintable) else {
        return;
    };
    let layers = style.effects().box_shadows.as_slice();
    if layers.is_empty() {
        return;
    }
    paint_box_shadow_layers(
        recorder,
        paintable,
        bordered_content_rect,
        borderless_content_rect,
        border_radii,
        layers,
    );
}

fn paint_box_shadow_layers<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    paintable: NodeSlotId,
    bordered_content_rect: CssPixelRect,
    borderless_content_rect: CssPixelRect,
    border_radii: BorderRadii,
    layers: &[crate::css::computed_value_types::ComputedShadow],
) {
    let converter = recorder.converter;
    let corner_radii = border_radii.as_corners(&converter);
    let shadow_shapes = ShadowShapes::of(recorder, paintable, bordered_content_rect, corner_radii);

    // Force-dark judges a box shadow as a background, so a dark drop shadow stays dark rather than turning into a
    // light glow — as Blink's ResolveShadowColor does, passing DarkModeFilter::ElementRole::kBackground.
    // Box-shadow layers are ordered front-to-back, so we paint them in reverse.
    for layer in layers.iter().rev() {
        let offset_x = converter.rounded_device_pixels(CssPixels::from_raw(layer.offset_x));
        let offset_y = converter.rounded_device_pixels(CssPixels::from_raw(layer.offset_y));
        let blur_radius = converter.rounded_device_pixels(CssPixels::from_raw(layer.blur_radius));
        let spread_distance = converter.rounded_device_pixels(CssPixels::from_raw(layer.spread_distance));
        let inner = layer.is_inner();

        let device_content_rect = if inner {
            converter.rounded_device_rect(borderless_content_rect)
        } else {
            converter.rounded_device_rect(bordered_content_rect)
        };

        if let Some(shadow_shapes) = &shadow_shapes {
            recorder.recorder.paint_path_box_shadow(
                Color(layer.color),
                blur_radius,
                spread_distance as f32,
                FloatPoint {
                    x: offset_x as f32,
                    y: offset_y as f32,
                },
                if inner {
                    &shadow_shapes.inner
                } else {
                    &shadow_shapes.outer
                },
                inner,
                ForceDarkRole::Background,
            );
        } else if inner {
            let mut outer_shadow_rect = IntRect::new(
                device_content_rect.x + offset_x,
                device_content_rect.y + offset_y,
                device_content_rect.width,
                device_content_rect.height,
            );
            let mut inner_shadow_rect = outer_shadow_rect.inflated_edges(
                -spread_distance,
                -spread_distance,
                -spread_distance,
                -spread_distance,
            );

            if inner_shadow_rect.width < 0 {
                inner_shadow_rect.width = 0;
            }

            if inner_shadow_rect.height < 0 {
                inner_shadow_rect.height = 0;
            }

            outer_shadow_rect = outer_shadow_rect.inflated_edges(
                blur_radius + offset_y,
                blur_radius + offset_x.abs(),
                blur_radius + offset_y.abs(),
                blur_radius + offset_x,
            );

            let mut inner_shadow_corner_radii = corner_radii;
            adjust_corners_for_spread_distance(&mut inner_shadow_corner_radii, -spread_distance);

            recorder.recorder.paint_inner_box_shadow(
                PaintInnerBoxShadow {
                    color: Color(layer.color),
                    blur_radius,
                    device_content_rect,
                    content_corner_radii: corner_radii,
                    outer_shadow_rect,
                    inner_shadow_rect,
                    inner_shadow_corner_radii,
                },
                ForceDarkRole::Background,
            );
        } else {
            let mut shadow_rect =
                device_content_rect.inflated_edges(spread_distance, spread_distance, spread_distance, spread_distance);
            shadow_rect.x += offset_x;
            shadow_rect.y += offset_y;

            let mut shadow_corner_radii = corner_radii;
            adjust_corners_for_spread_distance(&mut shadow_corner_radii, spread_distance);

            recorder.recorder.paint_outer_box_shadow(
                PaintOuterBoxShadow {
                    color: Color(layer.color),
                    blur_radius,
                    device_content_rect,
                    content_corner_radii: corner_radii,
                    shadow_rect,
                    shadow_corner_radii,
                },
                ForceDarkRole::Background,
            );
        }
    }
}

/// The shapes a box's shadows are cast from when they are not its rounded border and padding boxes: the shape outer
/// shadows are cast from, and the shape whose outside casts inner shadows.
struct ShadowShapes {
    outer: Path,
    inner: Path,
}

impl ShadowShapes {
    fn of<O: Observer>(
        recorder: &PaintRecorder<'_, O>,
        paintable: NodeSlotId,
        border_box_rect: CssPixelRect,
        corner_radii: CornerRadii,
    ) -> Option<Self> {
        let converter = recorder.converter;
        // https://drafts.csswg.org/css-borders-4/#border-shape-shadow-interaction
        if let Some(border_shape) =
            crate::painting::border_shape::BorderShape::for_paintable(recorder.source, paintable, &converter)
        {
            let (outer, inner) = border_shape.into_shadow_paths();
            return Some(Self { outer, inner });
        }
        // https://drafts.csswg.org/css-borders-4/#corner-shape-rendering
        // Each shadow of an element's box-shadow is shaped by the border contour path given the element and the inset
        // between its border edge and the shadow's edge.
        let style = recorder.source.node_style_if_live(paintable)?;
        let shapes = CornerShapes::of(style);
        if !shapes.needs_contour(corner_radii) {
            return None;
        }
        let border_rect = converter.rounded_device_rect(border_box_rect).to_float();
        let width = |width: CssPixels| f64::from(converter.rounded_device_pixels(width));
        let border_widths = Insets {
            top: width(style.border_top_width()),
            right: width(style.border_right_width()),
            bottom: width(style.border_bottom_width()),
            left: width(style.border_left_width()),
        };
        Some(Self {
            outer: border_contour_path(border_rect, corner_radii, shapes, Insets::default()),
            inner: border_contour_path(border_rect, corner_radii, shapes, border_widths),
        })
    }
}
