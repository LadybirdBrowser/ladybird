/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_types::{ComputedFilter, ComputedFilterOperation};
use crate::css::css_pixels::CssPixels;
use crate::painting::ffi::FilterOperationType;

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

pub(crate) fn may_affect_output_bounds(bytes: &[u8]) -> bool {
    fn skip(bytes: &[u8], offset: &mut usize, count: usize) -> Option<()> {
        *offset = offset.checked_add(count)?;
        (*offset <= bytes.len()).then_some(())
    }

    fn parse_optional_filter(bytes: &[u8], offset: &mut usize) -> Option<bool> {
        let has_filter = *bytes.get(*offset)? != 0;
        *offset += 1;
        if has_filter {
            parse_filter(bytes, offset)
        } else {
            Some(false)
        }
    }

    fn parse_filter(bytes: &[u8], offset: &mut usize) -> Option<bool> {
        let operation = *bytes.get(*offset)?;
        *offset += 1;
        if operation == FilterOperationType::Compose as u8 {
            let outer_affects_bounds = parse_filter(bytes, offset)?;
            if outer_affects_bounds {
                return Some(true);
            }
            return parse_filter(bytes, offset);
        }
        if operation == FilterOperationType::ColorFilter as u8 {
            skip(bytes, offset, size_of::<i32>() + size_of::<f32>())?;
            return parse_optional_filter(bytes, offset);
        }
        if operation == FilterOperationType::HueRotate as u8 {
            skip(bytes, offset, size_of::<f32>())?;
            return parse_optional_filter(bytes, offset);
        }
        Some(true)
    }

    let mut offset = 0;
    parse_filter(bytes, &mut offset).is_none_or(|affects_bounds| affects_bounds || offset != bytes.len())
}

pub(crate) fn serialize_non_url_filter(filter: &ComputedFilter, device_pixels_per_css_pixel: f64) -> Option<Vec<u8>> {
    let operations = filter.operations.as_slice();
    if operations.is_empty() || operations.iter().any(|operation| operation.kind == FILTER_KIND_URL) {
        return None;
    }

    let mut bytes = Vec::new();
    encode_composed_filter(&mut bytes, operations, device_pixels_per_css_pixel);
    Some(bytes)
}

fn encode_composed_filter(
    bytes: &mut Vec<u8>,
    operations: &[ComputedFilterOperation],
    device_pixels_per_css_pixel: f64,
) {
    if let Some((last, previous)) = operations.split_last() {
        if previous.is_empty() {
            encode_operation(bytes, last, device_pixels_per_css_pixel);
        } else {
            write_u8(bytes, FilterOperationType::Compose as u8);
            encode_operation(bytes, last, device_pixels_per_css_pixel);
            encode_composed_filter(bytes, previous, device_pixels_per_css_pixel);
        }
    }
}

fn encode_operation(bytes: &mut Vec<u8>, operation: &ComputedFilterOperation, device_pixels_per_css_pixel: f64) {
    match operation.kind {
        FILTER_KIND_BLUR => {
            write_u8(bytes, FilterOperationType::Blur as u8);
            let radius =
                (CssPixels::nearest_value_for_f32(operation.amount).to_double() * device_pixels_per_css_pixel) as f32;
            write_f32(bytes, radius);
            write_f32(bytes, radius);
            write_bool(bytes, false);
        }
        FILTER_KIND_DROP_SHADOW => {
            write_u8(bytes, FilterOperationType::DropShadow as u8);
            let scale = |raw| (CssPixels::from_raw(raw).to_double() * device_pixels_per_css_pixel) as f32;
            write_f32(bytes, scale(operation.shadow_offset_x));
            write_f32(bytes, scale(operation.shadow_offset_y));
            write_f32(bytes, scale(operation.shadow_radius));
            write_u32(bytes, operation.shadow_color);
            write_bool(bytes, false);
        }
        FILTER_KIND_COLOR => {
            write_u8(bytes, FilterOperationType::ColorFilter as u8);
            write_i32(bytes, i32::from(operation.color_operation));
            write_f32(bytes, operation.amount);
            write_bool(bytes, false);
        }
        FILTER_KIND_HUE_ROTATE => {
            write_u8(bytes, FilterOperationType::HueRotate as u8);
            write_f32(bytes, operation.amount);
            write_bool(bytes, false);
        }
        _ => unreachable!("computed filter holds an unknown operation kind"),
    }
}

fn write_bool(bytes: &mut Vec<u8>, value: bool) {
    write_u8(bytes, value as u8);
}

fn write_u8(bytes: &mut Vec<u8>, value: u8) {
    bytes.push(value);
}

fn write_i32(bytes: &mut Vec<u8>, value: i32) {
    bytes.extend_from_slice(&value.to_ne_bytes());
}

fn write_u32(bytes: &mut Vec<u8>, value: u32) {
    bytes.extend_from_slice(&value.to_ne_bytes());
}

fn write_f32(bytes: &mut Vec<u8>, value: f32) {
    bytes.extend_from_slice(&value.to_ne_bytes());
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
    fn serializes_blur_with_css_pixel_rounding() {
        let mut blur = operation(FILTER_KIND_BLUR);
        blur.amount = 1.257;
        let mut bytes = Vec::new();
        encode_composed_filter(&mut bytes, &[blur], 2.0);

        let radius = 2.5f32;
        let mut expected = vec![FilterOperationType::Blur as u8];
        expected.extend_from_slice(&radius.to_ne_bytes());
        expected.extend_from_slice(&radius.to_ne_bytes());
        expected.push(0);
        assert_eq!(bytes, expected);
    }

    #[test]
    fn composes_new_operations_outside_previous_operations() {
        let mut color = operation(FILTER_KIND_COLOR);
        color.color_operation = 5;
        color.amount = 0.75;
        let mut hue_rotate = operation(FILTER_KIND_HUE_ROTATE);
        hue_rotate.amount = 90.0;
        let mut bytes = Vec::new();
        encode_composed_filter(&mut bytes, &[color, hue_rotate], 1.0);

        assert_eq!(bytes[0], FilterOperationType::Compose as u8);
        assert_eq!(bytes[1], FilterOperationType::HueRotate as u8);
        assert_eq!(bytes[7], FilterOperationType::ColorFilter as u8);
    }
}
