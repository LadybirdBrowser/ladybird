/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::svg_formatting_context::FfiSvgNumberPercentage;
use crate::painting::display_list::commands::DisplayListGradientSpreadMethod;
use crate::painting::display_list::recorder::{ColorStops, PaintStyle};
use crate::painting::host::{FfiResolvedSvgGradient, FfiSvgGradientSpreadMethod, FfiSvgPaintContext};
use libgfx_rust::{AffineTransform, FloatPoint, multiply_affine};

fn resolve(value: FfiSvgNumberPercentage, basis: f32) -> f32 {
    if value.is_percentage {
        value.value * basis
    } else {
        value.value
    }
}

// Geometry stays in gradient coordinates. A single matrix maps it into the same
// recording space as the path; element and viewport transforms apply at replay.
pub(crate) fn instantiate_gradient(
    definition: &FfiResolvedSvgGradient,
    is_radial: bool,
    color_stops: ColorStops,
    context: &FfiSvgPaintContext,
) -> Option<PaintStyle> {
    if color_stops.colors.is_empty() {
        return None;
    }
    let (width, height, units_transform) = if definition.object_bounding_box {
        let bounds = context.path_bounding_box;
        if bounds.is_empty() {
            return None;
        }
        (
            1.0,
            1.0,
            AffineTransform::new(bounds.width, 0.0, 0.0, bounds.height, bounds.x, bounds.y),
        )
    } else {
        (
            context.viewport.width,
            context.viewport.height,
            AffineTransform::identity(),
        )
    };
    let gradient_transform = Some(multiply_affine(
        context.paint_transform,
        multiply_affine(units_transform, definition.transform),
    ))
    .into();
    let spread_method = match definition.spread_method {
        FfiSvgGradientSpreadMethod::Pad => DisplayListGradientSpreadMethod::Pad,
        FfiSvgGradientSpreadMethod::Repeat => DisplayListGradientSpreadMethod::Repeat,
        FfiSvgGradientSpreadMethod::Reflect => DisplayListGradientSpreadMethod::Reflect,
    };
    let start = FloatPoint {
        x: resolve(definition.start_x, width),
        y: resolve(definition.start_y, height),
    };
    let end = FloatPoint {
        x: resolve(definition.end_x, width),
        y: resolve(definition.end_y, height),
    };
    if is_radial {
        let radius_basis = if definition.object_bounding_box {
            1.0
        } else {
            width.hypot(height) / std::f32::consts::SQRT_2
        };
        Some(PaintStyle::RadialGradient {
            gradient_transform,
            spread_method,
            color_space: definition.color_space,
            color_stops,
            start_center: start,
            start_radius: resolve(definition.start_radius, radius_basis),
            end_center: end,
            end_radius: resolve(definition.end_radius, radius_basis),
        })
    } else {
        Some(PaintStyle::LinearGradient {
            gradient_transform,
            spread_method,
            color_space: definition.color_space,
            color_stops,
            start_point: start,
            end_point: end,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libgfx_rust::{Color, FloatRect};

    fn stops() -> ColorStops {
        ColorStops {
            colors: vec![Color::from_rgb(255, 0, 0), Color::from_rgb(0, 0, 255)],
            positions: vec![0.0, 1.0],
            repeating: false,
        }
    }

    #[test]
    fn bounding_box_transform_applies_after_gradient_transform() {
        let definition = FfiResolvedSvgGradient {
            object_bounding_box: true,
            transform: AffineTransform::new(1.0, 0.0, 0.0, 1.0, 0.25, 0.5),
            ..Default::default()
        };
        let context = FfiSvgPaintContext {
            path_bounding_box: FloatRect::new(10.0, 20.0, 200.0, 100.0),
            paint_transform: AffineTransform::new(2.0, 0.0, 0.0, 2.0, 0.0, 0.0),
            ..Default::default()
        };
        let Some(PaintStyle::LinearGradient { gradient_transform, .. }) =
            instantiate_gradient(&definition, false, stops(), &context)
        else {
            panic!("expected a linear gradient")
        };
        assert_eq!(
            gradient_transform.value.map_point(FloatPoint { x: 0.0, y: 0.0 }),
            FloatPoint { x: 120.0, y: 140.0 }
        );
        assert_eq!(
            gradient_transform.value.map_point(FloatPoint { x: 1.0, y: 1.0 }),
            FloatPoint { x: 520.0, y: 340.0 }
        );
    }

    #[test]
    fn radial_percentage_uses_normalized_viewport_diagonal() {
        let definition = FfiResolvedSvgGradient {
            end_radius: FfiSvgNumberPercentage {
                value: 0.5,
                is_percentage: true,
            },
            ..Default::default()
        };
        let context = FfiSvgPaintContext {
            viewport: FloatRect::new(0.0, 0.0, 300.0, 400.0),
            ..Default::default()
        };
        let Some(PaintStyle::RadialGradient { end_radius, .. }) =
            instantiate_gradient(&definition, true, stops(), &context)
        else {
            panic!("expected a radial gradient")
        };
        assert!((end_radius - 250.0 / std::f32::consts::SQRT_2).abs() < 0.0001);
    }

    #[test]
    fn empty_gradient_or_bounding_box_has_no_paint() {
        let definition = FfiResolvedSvgGradient {
            object_bounding_box: true,
            ..Default::default()
        };
        assert!(instantiate_gradient(&definition, false, stops(), &FfiSvgPaintContext::default()).is_none());
        assert!(
            instantiate_gradient(
                &definition,
                false,
                ColorStops::default(),
                &FfiSvgPaintContext::default()
            )
            .is_none()
        );
    }
}
