/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS color interpolation over resolved color snapshots.

// Color values use the same thread-confined shared graph as the rest of the style core.
#![allow(clippy::arc_with_non_send_sync)]

use std::sync::Arc;

use crate::css::color_conversion;
use crate::css::color_resolution::make_powerless_hue_missing;
use crate::css::style_value::{ColorBase, RetainedStyleValueData, StyleValueData};
use crate::css::value_codes::*;

pub(crate) struct ResolvedColor {
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
pub(crate) fn carry_forward_missing_components(
    source_missing: [bool; 4],
    source_type: u8,
    target_type: u8,
) -> [bool; 4] {
    let source_categories = categories_for_color_type(source_type);
    let target_categories = categories_for_color_type(target_type);
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
    RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number { value: value as f64 }))
}

fn retained_none() -> RetainedStyleValueData {
    RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Keyword {
        keyword: crate::css::style_compute::none_keyword(),
    }))
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
        // https://drafts.csswg.org/css-color-5/#serial-color-mix
        // However, if the result of mixing in the hsl or hwb color space has at least one missing color
        // component (including a missing alpha carried forward per [[css-color-4#interpolation-missing]]), the
        // form used is the modern hsl(h s l / a) or hwb(h w b / a) syntax respectively, preserving the original
        // color function and each none value per [[css-color-4#css-serialization-of-srgb]], rather than
        // degrading to color(srgb r g b) (which would lose the hsl/hwb identity).
        color_conversion::HSL | color_conversion::HWB if missing.contains(&true) => (
            color_type,
            [
                components[0],
                components[1] * 100.0,
                components[2] * 100.0,
                components[3],
            ],
            missing,
        ),
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
        name: unsafe { crate::css::style_value::CssString::from_leaked_raw(0) },
        origin_color: RetainedStyleValueData::none(),
    }
}

// https://drafts.csswg.org/css-color-4/#interpolation
fn interpolate(
    from: &ResolvedColor,
    to: &ResolvedColor,
    target_type: u8,
    is_polar: bool,
    hue_interpolation_method: u8,
    delta: f32,
) -> Option<ResolvedColor> {
    // 1. checking the two colors for analogous components and analogous sets which will be carried forward
    // 2. prepare both colors for conversion. this changes any powerless components to missing values
    // 3. converting them both to a given color space which will be referred to as the interpolation color space
    //    below.
    let (mut from_components, from_missing) = convert_for_interpolation(from, target_type, is_polar)?;
    let (mut to_components, to_missing) = convert_for_interpolation(to, target_type, is_polar)?;

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
    let result = if interpolated_alpha == 0.0 && !both_alpha_missing {
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
    let missing = [
        from_missing[0] && to_missing[0],
        from_missing[1] && to_missing[1],
        from_missing[2] && to_missing[2],
        from_missing[3] && to_missing[3],
    ];
    Some(ResolvedColor {
        color_type: target_type,
        components: result,
        missing,
        has_native_components: true,
    })
}

/// Converts one color to the interpolation color space, returning its components and which of
/// them are missing after carrying forward.
fn convert_for_interpolation(color: &ResolvedColor, target_type: u8, is_polar: bool) -> Option<([f32; 4], [bool; 4])> {
    let mut source_missing = color.missing;
    if !color.has_native_components && !color.missing[3] && color.components[3] == 0.0 {
        source_missing[..3].fill(true);
    }
    let mut source_components = color.components;
    if color.color_type != target_type {
        // https://drafts.csswg.org/css-color-4/#color-convert-algorithm
        // To prepare a color col1 for conversion:
        // 1. Change any powerless components in src to missing components
        // NB: The powerless rules use percentages for the hsl() and hwb() components other than hue.
        let scale = if matches!(color.color_type, color_conversion::HSL | color_conversion::HWB) {
            100.0
        } else {
            1.0
        };
        let mut components = [
            f64::from(source_components[0]),
            f64::from(source_components[1]) * scale,
            f64::from(source_components[2]) * scale,
        ];
        let mut missing = [source_missing[0], source_missing[1], source_missing[2]];
        make_powerless_hue_missing(color.color_type, &mut components, &mut missing);
        source_components = [
            components[0] as f32,
            (components[1] / scale) as f32,
            (components[2] / scale) as f32,
            source_components[3],
        ];
        source_missing[..3].copy_from_slice(&missing);
    }
    let components = color_conversion::convert(color.color_type, target_type, source_components)?;
    let mut missing = carry_forward_missing_components(source_missing, color.color_type, target_type);
    if is_polar {
        mark_powerless_hue_after_conversion(color.color_type, target_type, components, &mut missing);
    }
    Some((components, missing))
}

fn interpolation_method_parts(color_interpolation_method: &StyleValueData) -> Option<(u8, bool, u8)> {
    let StyleValueData::ColorInterpolationMethod {
        is_polar,
        color_space,
        hue_interpolation_method,
    } = color_interpolation_method
    else {
        return None;
    };
    Some((
        interpolation_color_type(*is_polar, *color_space)?,
        *is_polar,
        *hue_interpolation_method,
    ))
}

fn finish(mut color: ResolvedColor, alpha_multiplier: f32) -> StyleValueData {
    color.components[3] *= alpha_multiplier;
    make_result(color.color_type, color.components, color.missing)
}

/// One color of a color-mix() with its normalized percentage (from 0 to 100).
pub(crate) struct MixItem {
    pub color: ResolvedColor,
    pub percentage: f64,
}

// https://drafts.csswg.org/css-color-5/#calculate-a-color-mix
pub(crate) fn mix_colors(
    items: Vec<MixItem>,
    color_interpolation_method: &StyleValueData,
    alpha_multiplier: f32,
) -> Option<StyleValueData> {
    let (target_type, is_polar, hue_interpolation_method) = interpolation_method_parts(color_interpolation_method)?;

    // 3. If items is length 1, set color to the color of that sole item, converted to the specified
    //    interpolation <color-space>.
    let color = if items.len() == 1 {
        let (components, missing) = convert_for_interpolation(&items[0].color, target_type, is_polar)?;
        ResolvedColor {
            color_type: target_type,
            components,
            missing,
            has_native_components: true,
        }
    } else {
        // Otherwise:
        // 1. Let item stack be a stack made by reversing items. (Thus, with the first item at the top of the
        //    stack.)
        let mut item_stack: Vec<MixItem> = items.into_iter().rev().collect();
        // 2. While item stack has length 2 or greater:
        while item_stack.len() >= 2 {
            // 1. Pop from item stack twice, letting a and b be the two results in order. Let combined percentage
            //    be the sum of a and b’s percentages.
            let a = item_stack.pop()?;
            let b = item_stack.pop()?;
            let combined_percentage = a.percentage + b.percentage;
            // 2. Interpolate a and b’s colors as described in [[css-color-4#interpolation]], with a progress
            //    percentage equal to (b’s percentage) / combined percentage), if combined percentage is greater
            //    than 0, and 0.5 otherwise. If the specified color space is a cylindrical polar color space, then
            //    the <hue-interpolation-method> controls the interpolation of hue, as described in
            //    [[css-color-4#hue-interpolation]]. If no <hue-interpolation-method> is specified, assume
            //    shorter.
            let progress = if combined_percentage > 0.0 {
                b.percentage / combined_percentage
            } else {
                0.5
            };
            let color = interpolate(
                &a.color,
                &b.color,
                target_type,
                is_polar,
                hue_interpolation_method,
                progress as f32,
            )?;
            // 3. Create a new mix item with the resulting color and a percentage of combined percentage, and push
            //    it onto item stack.
            item_stack.push(MixItem {
                color,
                percentage: combined_percentage,
            });
        }
        // 3. Set color to the color of the sole remaining item in item stack.
        item_stack.pop()?.color
    };

    // 4. Multiply the alpha component of color by alpha mult.
    // 5. Return color.
    Some(finish(color, alpha_multiplier))
}
