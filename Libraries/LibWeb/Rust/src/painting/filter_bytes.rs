/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lowers CSS filter functions to the filter graphs painting hands over as bytes.

use libgfx_rust::filter::Filter;
use libgfx_rust::{Color, ColorFilterType};

use crate::css::computed_value_types::{ComputedFilter, ComputedFilterOperation, ComputedStyleValueHandle};
use crate::css::css_pixels::CssPixels;
use crate::painting::ffi::{FfiFilterFunction, FfiFilterFunctionKind};
use crate::painting::host::visual_context::ResolvedSvgFilter;

const FILTER_KIND_BLUR: u8 = 0;
const FILTER_KIND_DROP_SHADOW: u8 = 1;
const FILTER_KIND_HUE_ROTATE: u8 = 2;
const FILTER_KIND_COLOR: u8 = 3;
const FILTER_KIND_URL: u8 = 4;

impl From<FfiFilterFunction> for Filter {
    fn from(function: FfiFilterFunction) -> Self {
        match function.kind {
            FfiFilterFunctionKind::Blur => Filter::blur(function.amount, function.amount, None),
            FfiFilterFunctionKind::DropShadow => Filter::drop_shadow(
                function.offset_x,
                function.offset_y,
                function.amount,
                function.color,
                None,
            ),
            FfiFilterFunctionKind::Color => Filter::color(function.color_operation, function.amount, None),
            FfiFilterFunctionKind::HueRotate => Filter::hue_rotate(function.amount, None),
        }
    }
}

/// One graph applying each function to the output of the one before it, so the last function in
/// the list ends up outermost; `None` for an empty list.
pub(crate) fn filter_functions_graph(functions: impl IntoIterator<Item = Filter>) -> Option<Filter> {
    functions.into_iter().fold(None, |inner, outer| {
        Some(match inner {
            Some(inner) => Filter::compose(outer, inner),
            None => outer,
        })
    })
}

pub(crate) fn contains_url(filter: &ComputedFilter) -> bool {
    filter
        .operations
        .as_slice()
        .iter()
        .any(|operation| operation.kind == FILTER_KIND_URL)
}

/// Whether a serialized filter can change the painted bounds of what it is applied to.
pub(crate) fn may_affect_output_bounds(bytes: &[u8]) -> bool {
    Filter::serialized_may_affect_output_bounds(bytes)
}

/// The serialized graph for a filter list's functions; `None` when the list has none.
pub(crate) fn serialize_non_url_filter(filter: &ComputedFilter, device_pixels_per_css_pixel: f64) -> Option<Vec<u8>> {
    css_filter(filter.operations.as_slice(), device_pixels_per_css_pixel).map(|filter| filter.serialize())
}

/// Resolves the `url()` references of a filter list through the host, one reference at a time.
/// The last reference's graph and region are the ones that apply; a reference the host cannot
/// resolve fails the list as a whole.
pub(crate) fn resolve_svg_filter_references(
    filter: &ComputedFilter,
    mut resolve: impl FnMut(&ComputedStyleValueHandle) -> ResolvedSvgFilter,
) -> ResolvedSvgFilter {
    let mut resolved = ResolvedSvgFilter::default();
    for operation in filter.operations.as_slice() {
        if operation.kind != FILTER_KIND_URL {
            continue;
        }
        resolved = resolve(&operation.url_value);
        if resolved.failed {
            break;
        }
    }
    resolved
}

/// The serialized graph for a filter list whose `url()` references the host has resolved.
pub(crate) fn serialize_filter_with_resolved_svg(
    filter: &ComputedFilter,
    resolved_svg_filter: ResolvedSvgFilter,
    device_pixels_per_css_pixel: f64,
) -> Option<Vec<u8>> {
    if resolved_svg_filter.failed {
        return None;
    }
    css_filter_with_svg(
        filter.operations.as_slice(),
        resolved_svg_filter.filter,
        device_pixels_per_css_pixel,
    )
    .map(|filter| filter.serialize())
}

/// Combines the functions of a filter list with the SVG filter its `url()` references resolved
/// to, which is applied last, after every function.
fn css_filter_with_svg(
    operations: &[ComputedFilterOperation],
    svg_filter: Option<Filter>,
    device_pixels_per_css_pixel: f64,
) -> Option<Filter> {
    match (svg_filter, css_filter(operations, device_pixels_per_css_pixel)) {
        (Some(svg_filter), Some(css_filter)) => Some(Filter::compose(svg_filter, css_filter)),
        (Some(filter), None) | (None, Some(filter)) => Some(filter),
        (None, None) => None,
    }
}

/// The graph of a computed filter list's functions, skipping the `url()` references the host
/// resolves.
pub(crate) fn css_filter(operations: &[ComputedFilterOperation], device_pixels_per_css_pixel: f64) -> Option<Filter> {
    filter_functions_graph(
        operations
            .iter()
            .filter(|operation| operation.kind != FILTER_KIND_URL)
            .map(|operation| filter_function(operation, device_pixels_per_css_pixel)),
    )
}

fn filter_function(operation: &ComputedFilterOperation, device_pixels_per_css_pixel: f64) -> Filter {
    match operation.kind {
        FILTER_KIND_BLUR => {
            let radius =
                (CssPixels::nearest_value_for_f32(operation.amount).to_double() * device_pixels_per_css_pixel) as f32;
            Filter::blur(radius, radius, None)
        }
        FILTER_KIND_DROP_SHADOW => {
            let scale = |raw| (CssPixels::from_raw(raw).to_double() * device_pixels_per_css_pixel) as f32;
            Filter::drop_shadow(
                scale(operation.shadow_offset_x),
                scale(operation.shadow_offset_y),
                scale(operation.shadow_radius),
                Color(operation.shadow_color),
                None,
            )
        }
        FILTER_KIND_COLOR => {
            let kind = ColorFilterType::from_i32(i32::from(operation.color_operation))
                .expect("computed filter holds an unknown color operation");
            Filter::color(kind, operation.amount, None)
        }
        FILTER_KIND_HUE_ROTATE => Filter::hue_rotate(operation.amount, None),
        _ => unreachable!("computed filter holds an unknown operation kind"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn operation(kind: u8) -> ComputedFilterOperation {
        ComputedFilterOperation {
            kind,
            color_operation: 0,
            amount: 0.0,
            shadow_offset_x: 0,
            shadow_offset_y: 0,
            shadow_radius: 0,
            shadow_color: 0,
            url_value: ComputedStyleValueHandle {
                pointer: std::ptr::null(),
            },
        }
    }

    #[test]
    fn blur_rounds_to_css_pixels_before_scaling_to_device_pixels() {
        let mut blur = operation(FILTER_KIND_BLUR);
        blur.amount = 1.257;
        assert_eq!(css_filter(&[blur], 2.0), Some(Filter::blur(2.5, 2.5, None)));
    }

    #[test]
    fn later_operations_wrap_earlier_ones() {
        let mut color = operation(FILTER_KIND_COLOR);
        color.color_operation = ColorFilterType::Saturate as u8;
        color.amount = 0.75;
        let mut hue_rotate = operation(FILTER_KIND_HUE_ROTATE);
        hue_rotate.amount = 90.0;
        assert_eq!(
            css_filter(&[color, hue_rotate], 1.0),
            Some(Filter::compose(
                Filter::hue_rotate(90.0, None),
                Filter::color(ColorFilterType::Saturate, 0.75, None)
            ))
        );
    }

    #[test]
    fn device_pixel_functions_lower_the_same_way() {
        let drop_shadow = FfiFilterFunction {
            kind: FfiFilterFunctionKind::DropShadow,
            amount: 3.0,
            offset_x: 1.0,
            offset_y: 2.0,
            color: Color(0x7f00ff00),
            color_operation: ColorFilterType::Brightness,
        };
        let invert = FfiFilterFunction {
            kind: FfiFilterFunctionKind::Color,
            amount: 1.0,
            offset_x: 0.0,
            offset_y: 0.0,
            color: Color::TRANSPARENT,
            color_operation: ColorFilterType::Invert,
        };
        assert_eq!(filter_functions_graph([]), None);
        assert_eq!(
            filter_functions_graph([drop_shadow, invert].map(Filter::from)),
            Some(Filter::compose(
                Filter::color(ColorFilterType::Invert, 1.0, None),
                Filter::drop_shadow(1.0, 2.0, 3.0, Color(0x7f00ff00), None)
            ))
        );
    }

    #[test]
    fn an_empty_list_lowers_to_no_filter() {
        assert_eq!(css_filter(&[], 1.0), None);
        assert_eq!(css_filter(&[operation(FILTER_KIND_URL)], 1.0), None);
    }

    #[test]
    fn a_resolved_svg_filter_wraps_the_filter_functions() {
        let hue_rotate = || {
            let mut hue_rotate = operation(FILTER_KIND_HUE_ROTATE);
            hue_rotate.amount = 90.0;
            hue_rotate
        };
        let svg_filter = Filter::blur(3.0, 3.0, None);
        assert_eq!(
            css_filter_with_svg(&[hue_rotate()], Some(svg_filter.clone()), 1.0),
            Some(Filter::compose(svg_filter.clone(), Filter::hue_rotate(90.0, None)))
        );
        assert_eq!(
            css_filter_with_svg(&[], Some(svg_filter.clone()), 1.0),
            Some(svg_filter)
        );
        assert_eq!(
            css_filter_with_svg(&[hue_rotate()], None, 1.0),
            Some(Filter::hue_rotate(90.0, None))
        );
        assert_eq!(css_filter_with_svg(&[], None, 1.0), None);
    }

    #[test]
    fn unreadable_bytes_are_assumed_to_affect_bounds() {
        assert!(may_affect_output_bounds(&[]));
        assert!(may_affect_output_bounds(&[0xff]));
        assert!(!may_affect_output_bounds(&Filter::hue_rotate(90.0, None).serialize()));
    }
}
