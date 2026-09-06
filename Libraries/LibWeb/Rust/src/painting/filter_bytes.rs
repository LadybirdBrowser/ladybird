/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lowers computed CSS `filter` values to the filter graphs painting hands over as bytes.

use libgfx_rust::filter::Filter;
use libgfx_rust::{Color, ColorFilterType};

use crate::css::computed_value_types::{ComputedFilter, ComputedFilterOperation};
use crate::css::css_pixels::CssPixels;

const FILTER_KIND_BLUR: u8 = 0;
const FILTER_KIND_DROP_SHADOW: u8 = 1;
const FILTER_KIND_HUE_ROTATE: u8 = 2;
const FILTER_KIND_COLOR: u8 = 3;
const FILTER_KIND_URL: u8 = 4;

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

/// The serialized graph for a filter list without `url()` references, or `None` when the list is
/// empty or needs the host to resolve an SVG filter.
pub(crate) fn serialize_non_url_filter(filter: &ComputedFilter, device_pixels_per_css_pixel: f64) -> Option<Vec<u8>> {
    if contains_url(filter) {
        return None;
    }
    css_filter(filter.operations.as_slice(), device_pixels_per_css_pixel).map(|filter| filter.serialize())
}

/// Lowers a list of filter functions to one graph that applies each function to the output of the
/// one before it, so the last function in the list ends up outermost.
pub(crate) fn css_filter(operations: &[ComputedFilterOperation], device_pixels_per_css_pixel: f64) -> Option<Filter> {
    operations.iter().fold(None, |inner, operation| {
        let outer = css_filter_operation(operation, device_pixels_per_css_pixel);
        Some(match inner {
            Some(inner) => Filter::compose(outer, inner),
            None => outer,
        })
    })
}

fn css_filter_operation(operation: &ComputedFilterOperation, device_pixels_per_css_pixel: f64) -> Filter {
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
    use crate::css::computed_value_types::ComputedStyleValueHandle;

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
    fn an_empty_list_lowers_to_no_filter() {
        assert_eq!(css_filter(&[], 1.0), None);
    }

    #[test]
    fn unreadable_bytes_are_assumed_to_affect_bounds() {
        assert!(may_affect_output_bounds(&[]));
        assert!(may_affect_output_bounds(&[0xff]));
        assert!(!may_affect_output_bounds(&Filter::hue_rotate(90.0, None).serialize()));
    }
}
