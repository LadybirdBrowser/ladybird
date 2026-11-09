/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_pixels::{CssPixels, FfiCssPixelSize};
use libgfx_rust::{CapStyle, JoinStyle};

/// The properties that shape the stroke of an SVG shape, in its user units.
pub(crate) struct SvgStrokeStyle {
    pub(crate) cap_style: CapStyle,
    pub(crate) join_style: JoinStyle,
    pub(crate) miter_limit: f32,
    pub(crate) width: f32,
    pub(crate) dash_array: Vec<f32>,
    pub(crate) dash_offset: f32,
    pub(crate) non_scaling_stroke: bool,
}

/// What the stroke percentages of a shape in an SVG viewport of size `viewport` resolve against, its "scaled viewport
/// size": https://www.w3.org/TR/2017/WD-fill-stroke-3-20170413/#scaled-viewport-size
// FIXME: The spec formula is the normalized diagonal sqrt((width² + height²) / 2); this keeps
//        the historical (width + height) / 2 approximation.
pub(crate) fn percentage_basis(viewport: FfiCssPixelSize) -> CssPixels {
    (viewport.width + viewport.height) * CssPixels::nearest_value_for(0.5)
}

/// The stroke properties of an SVG shape, with percentages resolved against the given basis.
pub(crate) fn svg_stroke_style_for_percentage_basis(
    style: &ComputedValuesView<'_>,
    basis: CssPixels,
) -> SvgStrokeStyle {
    use crate::css::css_enums::{stroke_linecap, stroke_linejoin, vector_effect};
    let svg = style.inherited_svg();
    let resolve = |handle: &crate::css::computed_value_types::ComputedStyleValueHandle, default: f32| {
        handle
            .length_percentage()
            .map_or(default, |value| value.to_px(basis).to_double() as f32)
    };

    let mut dash_array: Vec<f32> = svg
        .stroke_dasharray
        .as_slice()
        .iter()
        .map(|dash| {
            if dash.is_number {
                dash.number as f32
            } else {
                resolve(&dash.value, 0.0)
            }
        })
        .collect();
    if dash_array.len() % 2 == 1 {
        dash_array.extend_from_within(..);
    }
    if dash_array.iter().any(|value| *value < 0.0) || dash_array.iter().all(|value| *value == 0.0) {
        dash_array.clear();
    }

    SvgStrokeStyle {
        cap_style: match svg.stroke_linecap {
            stroke_linecap::ROUND => CapStyle::Round,
            stroke_linecap::SQUARE => CapStyle::Square,
            _ => CapStyle::Butt,
        },
        join_style: match svg.stroke_linejoin {
            stroke_linejoin::ROUND => JoinStyle::Round,
            stroke_linejoin::BEVEL => JoinStyle::Bevel,
            _ => JoinStyle::Miter,
        },
        miter_limit: svg.stroke_miterlimit as f32,
        width: resolve(&svg.stroke_width, 1.0),
        dash_array,
        dash_offset: resolve(&svg.stroke_dashoffset, 0.0),
        non_scaling_stroke: style.svg_reset().vector_effect == vector_effect::NON_SCALING_STROKE,
    }
}
