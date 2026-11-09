/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_types::{ComputedResolvedTransform, ComputedStyleValueHandle};
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use libgfx_rust::{FloatMatrix4x4, translation_matrix};

include!(concat!(env!("OUT_DIR"), "/transform_functions_generated.rs"));

fn resolved_translate_axis_px(px: f32, percentage: &ComputedStyleValueHandle, reference: CssPixels) -> f32 {
    let Some(length_percentage) = percentage.length_percentage() else {
        return px;
    };
    if length_percentage.is_calculated() {
        return CssPixels::nearest_value_for(crate::css::computed_value_views::resolve_calc_to_px_without_rounding(
            length_percentage.calculated_pointer(),
            reference,
        ))
        .to_float();
    }
    CssPixels::nearest_value_for(reference.to_double() * length_percentage.as_fraction()).to_float()
}

fn resolved_transform_to_matrix(
    entry: &ComputedResolvedTransform,
    reference_width: CssPixels,
    reference_height: CssPixels,
) -> FloatMatrix4x4 {
    if entry.is_translate {
        return translation_matrix(
            resolved_translate_axis_px(entry.x_px, &entry.x_percentage, reference_width),
            resolved_translate_axis_px(entry.y_px, &entry.y_percentage, reference_height),
            entry.z_px,
        );
    }
    let mut elements = [[0.0f32; 4]; 4];
    for (index, value) in entry.matrix.into_iter().enumerate() {
        elements[index / 4][index % 4] = value;
    }
    FloatMatrix4x4 { elements }
}

pub(crate) fn multiply_transform_functions(
    mut matrix: FloatMatrix4x4,
    entries: &[ComputedResolvedTransform],
    reference_box: CssPixelRect,
) -> FloatMatrix4x4 {
    for entry in entries {
        matrix = matrix.multiplied(resolved_transform_to_matrix(
            entry,
            reference_box.width,
            reference_box.height,
        ));
    }
    matrix
}
