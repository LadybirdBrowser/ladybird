/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::NodeSlotId;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::visual_context::{IncludeVisualViewportTransform, VisualContextTree};
use libgfx_rust::{FloatPoint, FloatRect};

pub(crate) struct RectToViewportTransform<'a> {
    pub visual_context_tree: &'a VisualContextTree,
    pub scroll_offsets: &'a [FloatPoint],
    pub device_pixels_per_css_pixel: f32,
}

impl RectToViewportTransform<'_> {
    pub(crate) fn transform_rect_to_viewport(
        &self,
        arena: &impl PaintableRowsRead,
        node: NodeSlotId,
        rect: CssPixelRect,
    ) -> CssPixelRect {
        if !arena.paintable_row_is_populated(node) {
            return CssPixelRect::default();
        }
        let pixel_ratio = self.device_pixels_per_css_pixel;
        let device_rect = FloatRect {
            x: rect.x.to_float() * pixel_ratio,
            y: rect.y.to_float() * pixel_ratio,
            width: rect.width.to_float() * pixel_ratio,
            height: rect.height.to_float() * pixel_ratio,
        };
        let spatial = arena.paintable_data(node).accumulated_visual_context.spatial;
        let transformed = self.visual_context_tree.transform_rect_to_viewport(
            spatial,
            device_rect,
            self.scroll_offsets,
            IncludeVisualViewportTransform::No,
        );
        let inverse_pixel_ratio = 1.0 / pixel_ratio;
        CssPixelRect {
            x: CssPixels::nearest_value_for_f32(transformed.x * inverse_pixel_ratio),
            y: CssPixels::nearest_value_for_f32(transformed.y * inverse_pixel_ratio),
            width: CssPixels::nearest_value_for_f32(transformed.width * inverse_pixel_ratio),
            height: CssPixels::nearest_value_for_f32(transformed.height * inverse_pixel_ratio),
        }
    }
}

pub(crate) fn transform_rect_to_viewport_or_identity(
    transform: Option<&RectToViewportTransform<'_>>,
    arena: &impl PaintableRowsRead,
    node: NodeSlotId,
    rect: CssPixelRect,
) -> CssPixelRect {
    match transform {
        None => rect,
        Some(transform) => transform.transform_rect_to_viewport(arena, node, rect),
    }
}
