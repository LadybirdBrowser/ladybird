/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_types::{ComputedSvgPaint, SVG_PAINT_COLOR, SVG_PAINT_NONE};

/// The color `paint` paints with, with `currentcolor` resolved to `current_color`.
pub(crate) fn svg_paint_color(paint: &ComputedSvgPaint, current_color: u32) -> Option<u32> {
    match paint.kind {
        SVG_PAINT_NONE => None,
        SVG_PAINT_COLOR => Some(paint.color.resolve(current_color)),
        _ => paint.has_color.then(|| paint.color.resolve(current_color)),
    }
}
