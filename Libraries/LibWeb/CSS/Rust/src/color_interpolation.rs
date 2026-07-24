/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS color interpolation over resolved color snapshots.

// Color values use the same thread-confined shared graph as the rest of the style core.
#![allow(clippy::arc_with_non_send_sync)]

use std::sync::Arc;

use crate::color_conversion;
use crate::style_value::{ColorBase, RetainedStyleValueData, StyleValueData};

const COLOR_SYNTAX_MODERN: u8 = 1;

#[repr(C)]
pub struct FfiResolvedColor {
    pub color_type: u8,
    pub components: [f32; 4],
    pub missing: [bool; 4],
    pub has_native_components: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ComponentCategory {
    Red,
    Green,
    Blue,
    Lightness,
    Colorfulness,
    Hue,
    OpponentA,
    OpponentB,
    NotAnalogous,
}

fn categories_for_color_type(color_type: u8) -> [ComponentCategory; 3] {
    use ComponentCategory::*;
    use color_conversion::*;
    match color_type {
        HSL => [Hue, Colorfulness, Lightness],
        HWB => [Hue, NotAnalogous, NotAnalogous],
        LAB | OKLAB => [Lightness, OpponentA, OpponentB],
        LCH | OKLCH => [Lightness, Colorfulness, Hue],
        RGB | A98_RGB | DISPLAY_P3 | DISPLAY_P3_LINEAR | SRGB | SRGB_LINEAR | PROPHOTO_RGB | REC2020 | XYZ_D50
        | XYZ_D65 => [Red, Green, Blue],
        _ => [NotAnalogous, NotAnalogous, NotAnalogous],
    }
}

// https://drafts.csswg.org/css-color-4/#interpolation-missing
// Carry forward missing components from the input color space to the interpolation color space.
// A missing component is carried forward if it has an analogous component in the target space.
// Additionally, if ALL components of an analogous set are missing, they are all carried forward.
fn carry_forward_missing_components(
    source_missing: [bool; 4],
    source_categories: [ComponentCategory; 3],
    target_categories: [ComponentCategory; 3],
) -> [bool; 4] {
    let mut result = [false; 4];

    // Same-space: all components map to themselves, including NotAnalogous ones (e.g. HWB W/B).
    if source_categories == target_categories {
        return source_missing;
    }

    // Carry forward individual analogous components
    for target_index in 0..3 {
        if target_categories[target_index] == ComponentCategory::NotAnalogous {
            continue;
        }
        for source_index in 0..3 {
            if source_missing[source_index] && source_categories[source_index] == target_categories[target_index] {
                result[target_index] = true;
                break;
            }
        }
    }

    // If every component of an analogous set is missing in the source, carry forward as a set.
    // The analogous set consists of the components that remain after removing individually analogous ones.
    let mut all_non_analogous_missing = true;
    let mut has_non_analogous = false;
    for source_index in 0..3 {
        let is_individually_analogous = source_categories[source_index] != ComponentCategory::NotAnalogous
            && target_categories.contains(&source_categories[source_index]);
        if !is_individually_analogous {
            has_non_analogous = true;
            if !source_missing[source_index] {
                all_non_analogous_missing = false;
            }
        }
    }
    if has_non_analogous && all_non_analogous_missing {
        for target_index in 0..3 {
            let is_individually_analogous = target_categories[target_index] != ComponentCategory::NotAnalogous
                && source_categories.contains(&target_categories[target_index]);
            if !is_individually_analogous {
                result[target_index] = true;
            }
        }
    }

    // Alpha is always analogous to itself.
    result[3] = source_missing[3];
    result
}

fn interpolation_color_type(is_polar: bool, color_space: u8) -> Option<u8> {
    use color_conversion::*;
    if is_polar {
        return match color_space {
            0 => Some(HSL),
            1 => Some(HWB),
            2 => Some(LCH),
            3 => Some(OKLCH),
            _ => None,
        };
    }
    match color_space {
        0 => Some(SRGB),
        1 => Some(SRGB_LINEAR),
        2 => Some(DISPLAY_P3),
        3 => Some(DISPLAY_P3_LINEAR),
        4 => Some(A98_RGB),
        5 => Some(PROPHOTO_RGB),
        6 => Some(REC2020),
        7 => Some(LAB),
        8 => Some(OKLAB),
        9 | 11 => Some(XYZ_D65),
        10 => Some(XYZ_D50),
        _ => None,
    }
}

fn hue_index(color_type: u8) -> Option<usize> {
    match color_type {
        color_conversion::HSL | color_conversion::HWB => Some(0),
        color_conversion::LCH | color_conversion::OKLCH => Some(2),
        _ => None,
    }
}

// https://drafts.csswg.org/css-color-4/#hue-interpolation
fn fixup_hues(from: &mut f32, to: &mut f32, hue_interpolation_method: u8) {
    let difference = *to - *from;
    match hue_interpolation_method {
        // https://drafts.csswg.org/css-color-4/#hue-shorter
        0 => {
            if difference > 180.0 {
                *from += 360.0;
            } else if difference < -180.0 {
                *to += 360.0;
            }
        }
        // https://drafts.csswg.org/css-color-4/#hue-longer
        1 => {
            if difference > 0.0 && difference < 180.0 {
                *from += 360.0;
            } else if difference > -180.0 && difference <= 0.0 {
                *to += 360.0;
            }
        }
        // https://drafts.csswg.org/css-color-4/#hue-increasing
        2 if *to < *from => *to += 360.0,
        // https://drafts.csswg.org/css-color-4/#hue-decreasing
        3 if *from < *to => *from += 360.0,
        _ => {}
    }
}

fn substitute_missing_components(
    from_components: &mut [f32; 4],
    to_components: &mut [f32; 4],
    from_missing: [bool; 4],
    to_missing: [bool; 4],
) {
    for index in 0..3 {
        if from_missing[index] && !to_missing[index] {
            from_components[index] = to_components[index];
        } else if to_missing[index] && !from_missing[index] {
            to_components[index] = from_components[index];
        }
    }
    if from_missing[3] && !to_missing[3] {
        from_components[3] = to_components[3];
    } else if to_missing[3] && !from_missing[3] {
        to_components[3] = from_components[3];
    } else if from_missing[3] && to_missing[3] {
        from_components[3] = 1.0;
        to_components[3] = 1.0;
    }
}

fn mark_powerless_hue_after_conversion(
    source_type: u8,
    target_type: u8,
    components: [f32; 4],
    missing: &mut [bool; 4],
) {
    if source_type == target_type {
        return;
    }
    // NB: Achromatic colors converted through the sRGB -> XYZ-D65 -> XYZ-D50 -> Lab -> LCH chain accumulate
    //     floating-point error of ~0.016 in the chroma component due to the Bradford chromatic adaptation matrices.
    //     This is the worst case for all color conversion types, so the threshold is large enough to account for this.
    const ACHROMATIC_THRESHOLD: f32 = 0.02;
    let powerless = match target_type {
        color_conversion::HSL => components[1].abs() < ACHROMATIC_THRESHOLD,
        color_conversion::HWB => components[1] + components[2] >= 1.0 - ACHROMATIC_THRESHOLD,
        color_conversion::LCH | color_conversion::OKLCH => components[1].abs() < ACHROMATIC_THRESHOLD,
        _ => false,
    };
    if powerless {
        missing[hue_index(target_type).unwrap()] = true;
    }
}

fn retained_number(value: f32) -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::Number { value: value as f64 }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn retained_none() -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::Keyword {
        keyword: crate::style_compute::none_keyword(),
    }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn retained_component(value: f32, missing: bool) -> RetainedStyleValueData {
    if missing {
        retained_none()
    } else {
        retained_number(value)
    }
}

fn make_result(color_type: u8, components: [f32; 4], missing: [bool; 4]) -> StyleValueData {
    let (color_type, components, missing) = match color_type {
        color_conversion::HSL | color_conversion::HWB => (
            color_conversion::SRGB,
            color_conversion::legacy_color_to_srgb(color_type, components).unwrap(),
            [false, false, false, missing[3]],
        ),
        _ => (color_type, components, missing),
    };
    StyleValueData::ColorFunction {
        color_base: ColorBase {
            has_color_type: true,
            color_type,
            color_syntax: COLOR_SYNTAX_MODERN,
        },
        channel_0: retained_component(components[0], missing[0]),
        channel_1: retained_component(components[1], missing[1]),
        channel_2: retained_component(components[2], missing[2]),
        alpha: retained_component(components[3], missing[3]),
        has_name: false,
        name: unsafe { crate::style_value::RetainedUtf16FlyString::from_leaked_raw(0) },
        origin_color: unsafe { RetainedStyleValueData::from_retained_optional_pointer(std::ptr::null()) },
    }
}

// https://drafts.csswg.org/css-color-4/#interpolation
fn interpolate(
    from: &FfiResolvedColor,
    to: &FfiResolvedColor,
    is_polar: bool,
    color_space: u8,
    hue_interpolation_method: u8,
    delta: f32,
    alpha_multiplier: f32,
) -> Option<StyleValueData> {
    // 1. checking the two colors for analogous components and analogous sets which will be carried forward
    let target_type = interpolation_color_type(is_polar, color_space)?;

    // 2. prepare both colors for conversion. this changes any powerless components to missing values
    let mut from_source_missing = from.missing;
    let mut to_source_missing = to.missing;
    if !from.has_native_components && !from.missing[3] && from.components[3] == 0.0 {
        from_source_missing[..3].fill(true);
    }
    if !to.has_native_components && !to.missing[3] && to.components[3] == 0.0 {
        to_source_missing[..3].fill(true);
    }

    // 3. converting them both to a given color space which will be referred to as the interpolation color space
    //    below.
    let mut from_components = color_conversion::convert(from.color_type, target_type, from.components)?;
    let mut to_components = color_conversion::convert(to.color_type, target_type, to.components)?;
    let target_categories = categories_for_color_type(target_type);
    let mut from_missing = carry_forward_missing_components(
        from_source_missing,
        categories_for_color_type(from.color_type),
        target_categories,
    );
    let mut to_missing = carry_forward_missing_components(
        to_source_missing,
        categories_for_color_type(to.color_type),
        target_categories,
    );
    if is_polar {
        mark_powerless_hue_after_conversion(from.color_type, target_type, from_components, &mut from_missing);
        mark_powerless_hue_after_conversion(to.color_type, target_type, to_components, &mut to_missing);
    }

    // 4. (if required) re-inserting carried forward values in the converted colors
    let both_alpha_missing = from_missing[3] && to_missing[3];
    substitute_missing_components(&mut from_components, &mut to_components, from_missing, to_missing);

    // 5. (if required) fixing up the hues, depending on the selected <hue-interpolation-method>
    if let Some(index) = hue_index(target_type) {
        fixup_hues(
            &mut from_components[index],
            &mut to_components[index],
            hue_interpolation_method,
        );
    }

    let interpolate = |from: f32, to: f32| from + (to - from) * delta;
    let interpolated_alpha = interpolate(from_components[3], to_components[3]).clamp(0.0, 1.0);
    let mut result = if interpolated_alpha == 0.0 && !both_alpha_missing {
        // OPTIMIZATION: Fully transparent results can skip the premultiply/interpolate/unpremultiply cycle.
        [0.0; 4]
    } else {
        // 6. changing the color components to premultiplied form
        // https://drafts.csswg.org/css-color-4/#interpolation-alpha
        // For rectangular orthogonal color coordinate systems, all component values are multiplied by the alpha value.
        // For cylindrical polar color coordinate systems, the hue angle is NOT premultiplied.
        let hue_index = hue_index(target_type);
        let premultiply = |components: [f32; 4], index: usize| {
            if Some(index) == hue_index {
                components[index]
            } else {
                components[index] * components[3]
            }
        };

        // 7. linearly interpolating each component of the computed value of the color separately
        let premultiplied = [
            interpolate(premultiply(from_components, 0), premultiply(to_components, 0)),
            interpolate(premultiply(from_components, 1), premultiply(to_components, 1)),
            interpolate(premultiply(from_components, 2), premultiply(to_components, 2)),
        ];

        // 8. undoing premultiplication
        let unpremultiply = |value: f32, index: usize| {
            if Some(index) == hue_index {
                value
            } else {
                value / interpolated_alpha
            }
        };
        [
            unpremultiply(premultiplied[0], 0),
            unpremultiply(premultiplied[1], 1),
            unpremultiply(premultiplied[2], 2),
            interpolated_alpha,
        ]
    };
    result[3] *= alpha_multiplier;
    let missing = [
        from_missing[0] && to_missing[0],
        from_missing[1] && to_missing[1],
        from_missing[2] && to_missing[2],
        from_missing[3] && to_missing[3],
    ];
    Some(make_result(target_type, result, missing))
}

/// # Safety
///
/// All pointers must be non-null and point at live values for the duration of this call. The returned pointer owns
/// one strong reference and must be adopted or released by the caller.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_interpolate_color(
    from: *const FfiResolvedColor,
    to: *const FfiResolvedColor,
    color_interpolation_method: *const StyleValueData,
    delta: f32,
    alpha_multiplier: f32,
) -> *const StyleValueData {
    crate::abort_on_panic(|| {
        let (Some(from), Some(to), Some(method)) = (unsafe { from.as_ref() }, unsafe { to.as_ref() }, unsafe {
            color_interpolation_method.as_ref()
        }) else {
            return std::ptr::null();
        };
        let StyleValueData::ColorInterpolationMethod {
            is_polar,
            color_space,
            hue_interpolation_method,
        } = method
        else {
            return std::ptr::null();
        };
        interpolate(
            from,
            to,
            *is_polar,
            *color_space,
            *hue_interpolation_method,
            delta,
            alpha_multiplier,
        )
        .map(|result| Arc::into_raw(Arc::new(result)))
        .unwrap_or(std::ptr::null())
    })
}
