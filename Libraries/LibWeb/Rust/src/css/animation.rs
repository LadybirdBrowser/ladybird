/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS animation value interpolation.

// Animation values use the same thread-confined shared graph as the rest of the style core.
#![allow(clippy::arc_with_non_send_sync)]

use std::sync::Arc;

use crate::css::property_metadata::{property_animation_type, property_numeric_ranges};
use crate::css::style_value::{
    ColorBase, GridTrackEntryKind, RetainedGridTrackEntry, RetainedGridTrackEntryList, RetainedNumericRangeList,
    RetainedShapePoint, RetainedShapePointList, RetainedStyleValueData, RetainedStyleValueDataList,
    RetainedUtf16FlyString, RetainedUtf16FlyStringList, StyleValueData,
};

pub(crate) const ANIMATION_TYPE_DISCRETE: u8 = 0;
pub(crate) const ANIMATION_TYPE_BY_COMPUTED_VALUE: u8 = 1;
const ANIMATION_TYPE_REPEATABLE_LIST: u8 = 2;
const ANIMATION_TYPE_CUSTOM: u8 = 3;
pub(crate) const ANIMATION_TYPE_NONE: u8 = 4;
const VALUE_TYPE_ANGLE: u8 = 2;
const VALUE_TYPE_FLEX: u8 = 15;
const VALUE_TYPE_FREQUENCY: u8 = 21;
const VALUE_TYPE_INTEGER: u8 = 24;
const VALUE_TYPE_LENGTH: u8 = 25;
const VALUE_TYPE_NUMBER: u8 = 27;
const VALUE_TYPE_PERCENTAGE: u8 = 31;
const VALUE_TYPE_RATIO: u8 = 33;
const VALUE_TYPE_RESOLUTION: u8 = 35;
const VALUE_TYPE_TIME: u8 = 38;
const TRANSFORM_FUNCTION_MATRIX: u8 = 0;
const TRANSFORM_FUNCTION_MATRIX_3D: u8 = 1;
const TRANSFORM_FUNCTION_PERSPECTIVE: u8 = 2;
const TRANSFORM_FUNCTION_TRANSLATE: u8 = 3;
const TRANSFORM_FUNCTION_TRANSLATE_3D: u8 = 4;
const TRANSFORM_FUNCTION_TRANSLATE_X: u8 = 5;
const TRANSFORM_FUNCTION_TRANSLATE_Y: u8 = 6;
const TRANSFORM_FUNCTION_TRANSLATE_Z: u8 = 7;
const TRANSFORM_FUNCTION_SCALE: u8 = 8;
const TRANSFORM_FUNCTION_SCALE_3D: u8 = 9;
const TRANSFORM_FUNCTION_SCALE_X: u8 = 10;
const TRANSFORM_FUNCTION_SCALE_Y: u8 = 11;
const TRANSFORM_FUNCTION_SCALE_Z: u8 = 12;
const TRANSFORM_FUNCTION_ROTATE: u8 = 13;
const TRANSFORM_FUNCTION_ROTATE_3D: u8 = 14;
const TRANSFORM_FUNCTION_ROTATE_X: u8 = 15;
const TRANSFORM_FUNCTION_ROTATE_Y: u8 = 16;
const TRANSFORM_FUNCTION_ROTATE_Z: u8 = 17;
const TRANSFORM_FUNCTION_SKEW: u8 = 18;
const TRANSFORM_FUNCTION_SKEW_X: u8 = 19;
const TRANSFORM_FUNCTION_SKEW_Y: u8 = 20;
const OPEN_TYPE_MODE_FONT_VARIATION_SETTINGS: u8 = 1;
const FONT_STYLE_NORMAL: u8 = 0;
const FONT_STYLE_OBLIQUE: u8 = 4;
const STEP_POSITION_JUMP_START: u8 = 0;
const STEP_POSITION_JUMP_NONE: u8 = 2;
const STEP_POSITION_JUMP_BOTH: u8 = 3;
const STEP_POSITION_START: u8 = 4;
const GRID_REPEAT_FIXED: u8 = 2;
const BASIC_SHAPE_INSET: u8 = 0;
const BASIC_SHAPE_CIRCLE: u8 = 3;
const BASIC_SHAPE_ELLIPSE: u8 = 4;
const BASIC_SHAPE_POLYGON: u8 = 5;
const COLOR_TYPE_RGB: u8 = 0;
const COLOR_TYPE_A98_RGB: u8 = 1;
const COLOR_TYPE_DISPLAY_P3: u8 = 2;
const COLOR_TYPE_DISPLAY_P3_LINEAR: u8 = 3;
const COLOR_TYPE_HSL: u8 = 4;
const COLOR_TYPE_HWB: u8 = 5;
const COLOR_TYPE_LAB: u8 = 6;
const COLOR_TYPE_LCH: u8 = 7;
const COLOR_TYPE_OKLAB: u8 = 8;
const COLOR_TYPE_OKLCH: u8 = 9;
const COLOR_TYPE_SRGB: u8 = 10;
const COLOR_TYPE_SRGB_LINEAR: u8 = 11;
const COLOR_TYPE_PROPHOTO_RGB: u8 = 12;
const COLOR_TYPE_REC2020: u8 = 13;
const COLOR_TYPE_XYZ_D50: u8 = 14;
const COLOR_TYPE_XYZ_D65: u8 = 15;
const COLOR_SYNTAX_LEGACY: u8 = 0;
const COLOR_SYNTAX_MODERN: u8 = 1;

#[derive(Clone, Copy)]
struct NumericRangeOverride {
    value_type: u8,
    min: f64,
    max: f64,
}

const BORDER_RADIUS_RECT_RANGES: &[NumericRangeOverride] = &[
    NumericRangeOverride {
        value_type: VALUE_TYPE_LENGTH,
        min: 0.0,
        max: f32::MAX as f64,
    },
    NumericRangeOverride {
        value_type: VALUE_TYPE_PERCENTAGE,
        min: 0.0,
        max: f32::MAX as f64,
    },
];

const RADIAL_SIZE_RANGES: &[NumericRangeOverride] = &[NumericRangeOverride {
    value_type: VALUE_TYPE_LENGTH,
    min: 0.0,
    max: f32::MAX as f64,
}];

const NONNEGATIVE_LENGTH_RANGE: &[NumericRangeOverride] = &[NumericRangeOverride {
    value_type: VALUE_TYPE_LENGTH,
    min: 0.0,
    max: f32::MAX as f64,
}];

#[repr(C)]
pub struct FfiAnimationValueResult {
    pub value: *const StyleValueData,
    pub handled: bool,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiEasingKind {
    Linear,
    CubicBezier,
    Steps,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLinearEasingPoint {
    pub input: f64,
    pub output: f64,
}

#[repr(C)]
pub struct FfiEasingDescriptor {
    pub kind: FfiEasingKind,
    pub linear_points: *const FfiLinearEasingPoint,
    pub linear_point_count: usize,
    pub x1: f64,
    pub y1: f64,
    pub x2: f64,
    pub y2: f64,
    pub interval_count: i32,
    pub step_position: u8,
}

fn evaluate_linear_easing(points: &[FfiLinearEasingPoint], input_progress: f64, before_flag: bool) -> f64 {
    // https://drafts.csswg.org/css-easing/#linear-easing-function-output
    // To calculate linear easing output progress for a given linear easing function func,
    // an input progress value inputProgress, and an optional before flag (defaulting to false),
    // perform the following:

    // 1. Let points be func’s control points.

    // 2. If points holds only a single item, return the output progress value of that item.
    if points.len() == 1 {
        return points[0].output;
    }

    // 3. If inputProgress matches the input progress value of the first point in points,
    // and the before flag is true, return the first point’s output progress value.
    if input_progress == points[0].input && before_flag {
        return points[0].output;
    }

    // 4. If inputProgress matches the input progress value of at least one point in points,
    // return the output progress value of the last such point.
    if let Some(point) = points.iter().rfind(|point| input_progress == point.input) {
        return point.output;
    }

    // 5. Otherwise, find two control points in points, A and B, which will be used for interpolation:
    let (a, b) = if input_progress < points[0].input {
        // 1. If inputProgress is smaller than any input progress value in points,
        // let A and B be the first two items in points.
        // If A and B have the same input progress value, return A’s output progress value.
        let (a, b) = (&points[0], &points[1]);
        if a.input == b.input {
            return a.output;
        }
        (a, b)
    } else if input_progress > points[points.len() - 1].input {
        // 2. If inputProgress is larger than any input progress value in points,
        // let A and B be the last two items in points.
        // If A and B have the same input progress value, return B’s output progress value.
        let (a, b) = (&points[points.len() - 2], &points[points.len() - 1]);
        if a.input == b.input {
            return b.output;
        }
        (a, b)
    } else {
        // 3. Otherwise, let A be the last control point whose input progress value is smaller than inputProgress,
        // and let B be the first control point whose input progress value is larger than inputProgress.
        let a = points
            .iter()
            .rfind(|point| point.input < input_progress)
            .expect("canonical linear easing has a preceding point");
        let b = points
            .iter()
            .find(|point| point.input > input_progress)
            .expect("canonical linear easing has a following point");
        (a, b)
    };

    // 6. Linearly interpolate (or extrapolate) inputProgress along the line defined by A and B, and return the result.
    let factor = (input_progress - a.input) / (b.input - a.input);
    a.output + factor * (b.output - a.output)
}

fn cubic_bezier_at(first: f64, second: f64, parameter: f64) -> f64 {
    let a = 1.0 - 3.0 * second + 3.0 * first;
    let b = 3.0 * second - 6.0 * first;
    let c = 3.0 * first;
    (a * parameter * parameter * parameter) + (b * parameter * parameter) + (c * parameter)
}

fn evaluate_cubic_bezier_easing(x1: f64, y1: f64, x2: f64, y2: f64, input_progress: f64) -> f64 {
    // https://drafts.csswg.org/css-easing-1/#cubic-bezier-algo
    // For input progress values outside the range [0, 1], the curve is extended infinitely using tangent of the curve
    // at the closest endpoint as follows:

    // - For input progress values less than zero,
    if input_progress < 0.0 {
        // 1. If the x value of P1 is greater than zero, use a straight line that passes through P1 and P0 as the
        //    tangent.
        if x1 > 0.0 {
            return y1 / x1 * input_progress;
        }

        // 2. Otherwise, if the x value of P2 is greater than zero, use a straight line that passes through P2 and P0 as
        //    the tangent.
        if x2 > 0.0 {
            return y2 / x2 * input_progress;
        }

        // 3. Otherwise, let the output progress value be zero for all input progress values in the range [-∞, 0).
        return 0.0;
    }

    // - For input progress values greater than one,
    if input_progress > 1.0 {
        // 1. If the x value of P2 is less than one, use a straight line that passes through P2 and P3 as the tangent.
        if x2 < 1.0 {
            return (1.0 - y2) / (1.0 - x2) * (input_progress - 1.0) + 1.0;
        }

        // 2. Otherwise, if the x value of P1 is less than one, use a straight line that passes through P1 and P3 as the
        //    tangent.
        if x1 < 1.0 {
            return (1.0 - y1) / (1.0 - x1) * (input_progress - 1.0) + 1.0;
        }

        // 3. Otherwise, let the output progress value be one for all input progress values in the range (1, ∞].
        return 1.0;
    }

    // The evaluation of this curve is covered in many sources such as [FUND-COMP-GRAPHICS].
    // NB: Use Newton-Raphson iteration to solve x(t) = inputProgress, then fall back to bisection.
    let derivative = |parameter: f64| {
        let a = 1.0 - 3.0 * x2 + 3.0 * x1;
        let b = 3.0 * x2 - 6.0 * x1;
        let c = 3.0 * x1;
        3.0 * a * parameter * parameter + 2.0 * b * parameter + c
    };
    let epsilon = 1e-7;
    let mut parameter = input_progress;
    for _ in 0..8 {
        let difference = cubic_bezier_at(x1, x2, parameter) - input_progress;
        if difference.abs() < epsilon {
            return cubic_bezier_at(y1, y2, parameter);
        }
        let derivative = derivative(parameter);
        if derivative.abs() < 1e-12 {
            break;
        }
        parameter -= difference / derivative;
    }

    let mut low = 0.0;
    let mut high = 1.0;
    parameter = input_progress;
    for _ in 0..64 {
        let value = cubic_bezier_at(x1, x2, parameter);
        if (value - input_progress).abs() < epsilon {
            return cubic_bezier_at(y1, y2, parameter);
        }
        if input_progress > value {
            low = parameter;
        } else {
            high = parameter;
        }
        parameter = (low + high) / 2.0;
    }
    cubic_bezier_at(y1, y2, parameter)
}

fn evaluate_steps_easing(interval_count: i32, position: u8, input_progress: f64, before_flag: bool) -> f64 {
    // https://drafts.csswg.org/css-easing-1/#step-easing-algo
    let mut current_step = (input_progress * f64::from(interval_count)).floor();

    // 2. If the step position property is one of:
    //    - jump-start,
    //    - jump-both,
    //    increment current step by one.
    if matches!(
        position,
        STEP_POSITION_JUMP_START | STEP_POSITION_START | STEP_POSITION_JUMP_BOTH
    ) {
        current_step += 1.0;
    }

    // 3. If both of the following conditions are true:
    //    - the before flag is set, and
    //    - input progress value × steps mod 1 equals zero (that is, if input progress value × steps is integral), then
    //    decrement current step by one.
    let step_progress = input_progress * f64::from(interval_count);
    if before_flag && step_progress.trunc() == step_progress {
        current_step -= 1.0;
    }

    // 4. If input progress value ≥ 0 and current step < 0, let current step be zero.
    if input_progress >= 0.0 && current_step < 0.0 {
        current_step = 0.0;
    }

    // 5. Calculate jumps based on the step position as follows:

    //    jump-start or jump-end -> steps
    //    jump-none -> steps - 1
    //    jump-both -> steps + 1
    let jumps = match position {
        STEP_POSITION_JUMP_NONE => interval_count - 1,
        STEP_POSITION_JUMP_BOTH => interval_count + 1,
        _ => interval_count,
    };

    // 6. If input progress value ≤ 1 and current step > jumps, let current step be jumps.
    if input_progress <= 1.0 && current_step > f64::from(jumps) {
        current_step = f64::from(jumps);
    }

    // 7. The output progress value is current step / jumps.
    current_step / f64::from(jumps)
}

/// Evaluate a resolved easing descriptor without consulting C++.
///
/// # Safety
/// `descriptor` must point to a live descriptor. Its linear point range must be live when the
/// descriptor kind is linear.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_evaluate_easing(
    descriptor: *const FfiEasingDescriptor,
    input_progress: f64,
    before_flag: bool,
) -> f64 {
    crate::abort_on_panic(|| evaluate_easing_descriptor(unsafe { &*descriptor }, input_progress, before_flag))
}

fn evaluate_easing_descriptor(descriptor: &FfiEasingDescriptor, input_progress: f64, before_flag: bool) -> f64 {
    match descriptor.kind {
        FfiEasingKind::Linear => {
            let points = unsafe { std::slice::from_raw_parts(descriptor.linear_points, descriptor.linear_point_count) };
            assert!(!points.is_empty());
            evaluate_linear_easing(points, input_progress, before_flag)
        }
        FfiEasingKind::CubicBezier => evaluate_cubic_bezier_easing(
            descriptor.x1,
            descriptor.y1,
            descriptor.x2,
            descriptor.y2,
            input_progress,
        ),
        FfiEasingKind::Steps => evaluate_steps_easing(
            descriptor.interval_count,
            descriptor.step_position,
            input_progress,
            before_flag,
        ),
    }
}

#[repr(C)]
pub struct FfiAnimationFontMetrics {
    pub font_size: f64,
    pub x_height: f64,
    pub cap_height: f64,
    pub zero_advance: f64,
    pub line_height: f64,
}

#[repr(C)]
pub struct FfiAnimationLengthResolutionContext {
    pub viewport_width: f64,
    pub viewport_height: f64,
    pub font_metrics: FfiAnimationFontMetrics,
    pub root_font_metrics: FfiAnimationFontMetrics,
    pub font_metrics_depend_on_viewport_metrics: bool,
    pub root_font_metrics_depend_on_viewport_metrics: bool,
}

#[repr(C)]
pub struct FfiAnimationContext {
    pub allow_discrete: bool,
    pub current_color: *const StyleValueData,
    pub has_length_resolution_context: bool,
    pub length_resolution_context: FfiAnimationLengthResolutionContext,
    pub has_transform_reference_box: bool,
    pub transform_reference_box_width: f64,
    pub transform_reference_box_height: f64,
}

#[repr(C)]
pub struct FfiAnimationKeyframeValue {
    pub key: i64,
    pub value: *const StyleValueData,
    pub easing: FfiEasingDescriptor,
    pub composite: FfiCompositeOperation,
}

#[repr(C)]
pub struct FfiAnimationValueInput {
    pub property_id: u16,
    pub result_of_transition: bool,
    pub underlying: *const StyleValueData,
    pub initial: *const StyleValueData,
    pub current_key: f64,
    pub keyframes: *const FfiAnimationKeyframeValue,
    pub keyframe_count: usize,
}

#[repr(C)]
pub struct FfiAnimationBatch {
    pub declarations: *const FfiAnimationDeclaration,
    pub declaration_count: usize,
    pub writing_mode: u8,
    pub direction: u8,
    pub important_property_bitmap: *const u8,
    pub important_property_bitmap_length: usize,
}

#[repr(C)]
pub struct FfiComputedAnimationBatch {
    pub context: FfiAnimationContext,
    pub values: *const FfiAnimationValueInput,
    pub value_count: usize,
    pub overlay: *mut std::ffi::c_void,
}

#[repr(C)]
pub struct FfiAnimatedProperty {
    pub property_id: u16,
    pub value: *const StyleValueData,
    pub progress: f32,
    pub start_index: usize,
    pub end_index: usize,
    pub handled: bool,
    pub apply: bool,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiAnimationStyleSheetResourceContext {
    pub base_url: *const u8,
    pub base_url_length: usize,
    pub has_value: bool,
    pub origin_clean: bool,
}

impl FfiAnimationStyleSheetResourceContext {
    #[cfg(test)]
    const fn empty() -> Self {
        Self {
            base_url: std::ptr::null(),
            base_url_length: 0,
            has_value: false,
            origin_clean: false,
        }
    }
}

#[repr(C)]
pub struct FfiAnimationDeclaration {
    pub keyframe_index: usize,
    pub property_id: u16,
    pub value: *const StyleValueData,
    pub style_sheet_resource_context: FfiAnimationStyleSheetResourceContext,
    pub use_initial: bool,
    pub is_transition: bool,
}

struct AnimationPropertyConflictCandidate {
    keyframe_index: usize,
    physical_property_id: u16,
    source_property_id: u16,
    source_longhand_id: u16,
    value: RetainedStyleValueData,
    style_sheet_resource_context: FfiAnimationStyleSheetResourceContext,
    use_initial: bool,
    is_transition: bool,
    suppressed_by_important: bool,
}

fn property_is_important(property_id: u16, bitmap: &[u8]) -> bool {
    let Some(index) = property_id
        .checked_sub(crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID)
        .map(usize::from)
    else {
        return false;
    };
    bitmap.get(index / 8).is_some_and(|byte| byte & (1 << (index % 8)) != 0)
}

fn animation_property_is_suppressed(is_transition: bool, property_id: u16, important_bitmap: &[u8]) -> bool {
    !is_transition && property_is_important(property_id, important_bitmap)
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum FfiAnimationSpecifiedValueSource {
    Value,
    Inherited,
    Initial,
    Underlying,
}

fn animation_specified_value_source(value: &StyleValueData, property_id: u16) -> FfiAnimationSpecifiedValueSource {
    let StyleValueData::Keyword { keyword } = value else {
        return FfiAnimationSpecifiedValueSource::Value;
    };

    // https://www.w3.org/TR/css-cascade-4/#inherit
    // If the cascaded value of a property is the inherit keyword, the property's specified and
    // computed values are the inherited value.
    if *keyword == crate::css::style_compute::keyword::INHERIT {
        return FfiAnimationSpecifiedValueSource::Inherited;
    }

    // https://www.w3.org/TR/css-cascade-4/#inherit-initial
    // If the cascaded value of a property is the unset keyword, then if it is an inherited
    // property, this is treated as inherit, and if it is not, this is treated as initial.
    if *keyword == crate::css::style_compute::keyword::UNSET {
        return if crate::css::property_metadata::property_is_inherited(property_id) {
            FfiAnimationSpecifiedValueSource::Inherited
        } else {
            FfiAnimationSpecifiedValueSource::Initial
        };
    }

    // https://www.w3.org/TR/css-cascade-4/#initial
    // If the cascaded value of a property is the initial keyword, the property's specified value
    // is its initial value.
    if *keyword == crate::css::style_compute::keyword::INITIAL {
        return FfiAnimationSpecifiedValueSource::Initial;
    }
    if matches!(
        *keyword,
        crate::css::style_compute::keyword::REVERT | crate::css::style_compute::keyword::REVERT_LAYER
    ) {
        return FfiAnimationSpecifiedValueSource::Underlying;
    }
    FfiAnimationSpecifiedValueSource::Value
}

fn resolve_animation_property_conflicts(
    candidates: &[AnimationPropertyConflictCandidate],
    selected: &mut [bool],
    value_sources: &mut [FfiAnimationSpecifiedValueSource],
) {
    assert_eq!(candidates.len(), selected.len());
    assert_eq!(candidates.len(), value_sources.len());
    selected.fill(false);
    for (candidate, source) in candidates.iter().zip(value_sources.iter_mut()) {
        *source = animation_specified_value_source(candidate.value.data(), candidate.physical_property_id);
    }
    let mut winners = std::collections::HashMap::<(usize, u16), usize>::new();
    for (candidate_index, candidate) in candidates.iter().enumerate() {
        let key = (candidate.keyframe_index, candidate.physical_property_id);
        let Some(&winner_index) = winners.get(&key) else {
            winners.insert(key, candidate_index);
            selected[candidate_index] = true;
            continue;
        };
        let winner = &candidates[winner_index];
        if candidate.use_initial {
            continue;
        }
        if !winner.use_initial
            && !crate::css::property_metadata::animation_property_is_preferred(
                candidate.source_property_id,
                winner.source_property_id,
            )
        {
            continue;
        }
        selected[winner_index] = false;
        selected[candidate_index] = true;
        winners.insert(key, candidate_index);
    }
}

#[repr(C)]
pub struct FfiResolvedAnimationProperty {
    pub keyframe_index: usize,
    pub physical_property_id: u16,
    pub source_longhand_id: u16,
    pub value: *const StyleValueData,
    pub value_source: FfiAnimationSpecifiedValueSource,
    pub style_sheet_resource_context: FfiAnimationStyleSheetResourceContext,
    pub is_transition: bool,
}

#[repr(C)]
pub struct FfiResolvedAnimationProperties {
    pub properties: *const FfiResolvedAnimationProperty,
    pub count: usize,
    pub uses_tree_counting_function: bool,
    pub container_relative_length_unit_mask: u8,
    pub needs_document_base_url: bool,
    pub unfixed_random_sharings: *const FfiAnimationUnfixedRandomSharing,
    pub unfixed_random_sharing_count: usize,
    pub storage: *mut std::ffi::c_void,
}

#[repr(C)]
pub struct FfiAnimationUnfixedRandomSharing {
    pub source: *const StyleValueData,
    pub name: usize,
    pub element_shared: bool,
}

fn resolve_animation_declarations(
    declarations: &[FfiAnimationDeclaration],
    writing_mode: u8,
    direction: u8,
    important_property_bitmap: &[u8],
) -> ResolvedAnimationDeclarations {
    let mut candidates = Vec::new();
    for declaration in declarations {
        assert!(
            !declaration.value.is_null(),
            "animation declaration value must not be null"
        );
        crate::css::style_compute::expand_shorthands_with(
            declaration.property_id,
            declaration.value.cast(),
            false,
            &mut |longhand_id, data, _| {
                let physical_property_id =
                    crate::css::style_compute::map_logical_alias_to_physical(longhand_id, writing_mode, direction);
                candidates.push(AnimationPropertyConflictCandidate {
                    keyframe_index: declaration.keyframe_index,
                    physical_property_id,
                    source_property_id: declaration.property_id,
                    source_longhand_id: longhand_id,
                    value: unsafe {
                        RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(
                            data.cast(),
                        ))
                    },
                    style_sheet_resource_context: declaration.style_sheet_resource_context,
                    use_initial: declaration.use_initial,
                    is_transition: declaration.is_transition,
                    // OPTIMIZATION: Values resulting from animations other than CSS transitions
                    // are overridden by important properties, so there is no need to compute or
                    // evaluate them.
                    suppressed_by_important: animation_property_is_suppressed(
                        declaration.is_transition,
                        physical_property_id,
                        important_property_bitmap,
                    ),
                });
            },
        );
    }

    let mut selected = vec![false; candidates.len()];
    let mut value_sources = vec![FfiAnimationSpecifiedValueSource::Value; candidates.len()];
    resolve_animation_property_conflicts(&candidates, &mut selected, &mut value_sources);
    let mut properties = Vec::new();
    let mut retained_values = Vec::new();
    for ((candidate, selected), value_source) in candidates.into_iter().zip(selected).zip(value_sources) {
        if selected && !candidate.suppressed_by_important {
            properties.push(FfiResolvedAnimationProperty {
                keyframe_index: candidate.keyframe_index,
                physical_property_id: candidate.physical_property_id,
                source_longhand_id: candidate.source_longhand_id,
                value: candidate.value.pointer(),
                value_source,
                style_sheet_resource_context: candidate.style_sheet_resource_context,
                is_transition: candidate.is_transition,
            });
            retained_values.push(candidate.value);
        }
    }
    let mut uses_tree_counting_function = false;
    let mut container_relative_length_unit_mask = 0;
    let mut needs_document_base_url = false;
    let mut random_sharing_sources = Vec::new();
    for property in &properties {
        if property.value_source != FfiAnimationSpecifiedValueSource::Value {
            continue;
        }
        let value = unsafe { &*property.value };
        if matches!(
            value,
            StyleValueData::Unresolved { .. } | StyleValueData::PendingSubstitution { .. }
        ) {
            continue;
        }
        let dependencies = crate::css::style_compute::external_value_dependencies(value);
        uses_tree_counting_function |= dependencies.uses_tree_counting_function;
        container_relative_length_unit_mask |= dependencies.container_relative_length_unit_mask;
        needs_document_base_url |= dependencies.needs_document_base_url;
        if dependencies.has_unfixed_random_sharing {
            crate::css::style_compute::collect_unfixed_random_sharings_in_value(value, &mut random_sharing_sources);
        }
    }
    let unfixed_random_sharings = random_sharing_sources
        .into_iter()
        .map(|source| {
            let StyleValueData::RandomValueSharing {
                has_name,
                name,
                element_shared,
                ..
            } = (unsafe { &*source })
            else {
                unreachable!();
            };
            FfiAnimationUnfixedRandomSharing {
                source,
                name: if *has_name { name.raw() } else { 0 },
                element_shared: *element_shared,
            }
        })
        .collect();
    ResolvedAnimationDeclarations {
        properties,
        _retained_values: retained_values,
        uses_tree_counting_function,
        container_relative_length_unit_mask,
        needs_document_base_url,
        unfixed_random_sharings,
    }
}

struct ResolvedAnimationDeclarations {
    properties: Vec<FfiResolvedAnimationProperty>,
    _retained_values: Vec<RetainedStyleValueData>,
    uses_tree_counting_function: bool,
    container_relative_length_unit_mask: u8,
    needs_document_base_url: bool,
    unfixed_random_sharings: Vec<FfiAnimationUnfixedRandomSharing>,
}

#[repr(u8)]
#[derive(Clone, Copy)]
pub enum FfiCompositeOperation {
    Replace,
    Add,
    Accumulate,
}

fn accepted_range(property_id: u16, value_type: u8, range_overrides: &[NumericRangeOverride]) -> Option<(f64, f64)> {
    if let Some(range) = range_overrides.iter().find(|range| range.value_type == value_type) {
        return Some((range.min, range.max));
    }
    property_numeric_ranges(property_id)
        .iter()
        .find(|range| range.value_type == value_type)
        .map(|range| (range.min, range.max))
}

fn clamp_to_range(value: f64, range: Option<(f64, f64)>) -> f64 {
    let Some((min, max)) = range else {
        return value;
    };
    if value < min {
        min
    } else if value > max {
        max
    } else {
        value
    }
}

fn interpolate_f64(from: f64, to: f64, delta: f32, range: Option<(f64, f64)>) -> f64 {
    clamp_to_range(from + (to - from) * f64::from(delta), range)
}

fn interpolate_i32(from: i32, to: i32, delta: f32, range: Option<(f64, f64)>) -> i32 {
    // https://drafts.csswg.org/css-values/#combine-integers
    // Interpolation of <integer> is defined as Vresult = round((1 - p) × VA + p × VB);
    // that is, interpolation happens in the real number space as for <number>s, and the result is converted to an <integer> by rounding to the nearest integer.
    let value = (from as f32 + (to as f32 - from as f32) * delta).round();
    clamp_to_range(f64::from(value), range) as i32
}

// https://drafts.csswg.org/css-borders-4/#normalized-superellipse-half-corner
fn normalized_super_ellipse_half_corner(s: f64) -> f64 {
    //  To compute the normalized superellipse half corner given a superellipse parameter s, return the first matching statement, switching on s:

    // -∞ Return 0.
    if s == f64::NEG_INFINITY {
        return 0.0;
    }

    // ∞ Return 1.
    if s == f64::INFINITY {
        return 1.0;
    }

    // Otherwise
    // 1. Let k be 0.5^abs(s).
    let k = 0.5_f64.powf(s.abs());

    // 2. Let convexHalfCorner be 0.5^k.
    let convex_half_corner = 0.5_f64.powf(k);

    // 3. If s is less than 0, return 1 - convexHalfCorner.
    if s < 0.0 {
        return 1.0 - convex_half_corner;
    }

    // 4. Return convexHalfCorner.
    convex_half_corner
}

fn interpolation_value_to_super_ellipse_parameter(interpolation_value: f64) -> f64 {
    // To convert a <number [0,1]> interpolationValue back to a superellipse parameter, switch on interpolationValue:

    // 0 Return -∞.
    if interpolation_value == 0.0 {
        return f64::NEG_INFINITY;
    }

    // 0.5 Return 0.
    if interpolation_value == 0.5 {
        return 0.0;
    }

    // 1 Return ∞.
    if interpolation_value == 1.0 {
        return f64::INFINITY;
    }

    // Otherwise
    // 1. Let convexHalfCorner be interpolationValue.
    let mut convex_half_corner = interpolation_value;

    // 2. If interpolationValue is less than 0.5, set convexHalfCorner to 1 - interpolationValue.
    if interpolation_value < 0.5 {
        convex_half_corner = 1.0 - interpolation_value;
    }

    // 3. Let k be ln(0.5) / ln(convexHalfCorner).
    let k = 0.5_f64.ln() / convex_half_corner.ln();

    // 4. Let s be log2(k).
    let mut s = k.log2();

    // AD-HOC: The logs above can introduce slight inaccuracies, this can interfere with the behaviour of
    //         serializing superellipse style values as their equivalent keywords as that relies on exact
    //         equality. To mitigate this we simply round to a whole number if we are sufficiently near
    if (s.round() - s).abs() < f64::from(f32::EPSILON) {
        s = s.round();
    }

    // 5. If interpolationValue is less than 0.5, return -s.
    if interpolation_value < 0.5 {
        return -s;
    }

    // 6. Return s.
    s
}

fn angle_to_degrees(value: f64, unit: u8) -> Option<f64> {
    let ratio = match unit {
        0 => 1.0,
        1 => 0.9,
        2 => 57.295_779_513_082_32,
        3 => 360.0,
        _ => return None,
    };
    Some(value * ratio)
}

fn owned(value: StyleValueData) -> FfiAnimationValueResult {
    FfiAnimationValueResult {
        value: Arc::into_raw(Arc::new(value)),
        handled: true,
    }
}

fn not_handled() -> FfiAnimationValueResult {
    FfiAnimationValueResult {
        value: std::ptr::null(),
        handled: false,
    }
}

fn handled_without_value() -> FfiAnimationValueResult {
    FfiAnimationValueResult {
        value: std::ptr::null(),
        handled: true,
    }
}

fn handled_retained_value(value: RetainedStyleValueData) -> FfiAnimationValueResult {
    let pointer = unsafe { crate::css::style_value::retain_style_value(value.data()) };
    FfiAnimationValueResult {
        value: pointer,
        handled: true,
    }
}

fn discrete_value(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    if !context.is_some_and(|context| context.allow_discrete) {
        return handled_without_value();
    }
    let value = if delta < 0.5 { from } else { to };
    FfiAnimationValueResult {
        value: unsafe { crate::css::style_value::retain_style_value(value) },
        handled: true,
    }
}

fn interpolate_visibility(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    let (StyleValueData::Keyword { keyword: from_keyword }, StyleValueData::Keyword { keyword: to_keyword }) =
        (from, to)
    else {
        return not_handled();
    };

    if from_keyword == to_keyword {
        return FfiAnimationValueResult {
            value: unsafe { crate::css::style_value::retain_style_value(from) },
            handled: true,
        };
    }

    // https://drafts.csswg.org/web-animations-1/#animating-visibility
    // For the visibility property, visible is interpolated as a discrete step where values of p between 0 and 1 map to visible and other values of p map to the closer endpoint.
    // If neither value is visible, then discrete animation is used.
    let visible = crate::css::style_compute::keyword::VISIBLE;
    if *from_keyword == visible || *to_keyword == visible {
        let value = if delta <= 0.0 {
            from
        } else if delta >= 1.0 {
            to
        } else if *from_keyword == visible {
            from
        } else {
            to
        };
        return FfiAnimationValueResult {
            value: unsafe { crate::css::style_value::retain_style_value(value) },
            handled: true,
        };
    }

    discrete_value(context, from, to, delta)
}

fn interpolate_content_visibility(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    let (StyleValueData::Keyword { keyword: from_keyword }, StyleValueData::Keyword { keyword: to_keyword }) =
        (from, to)
    else {
        return not_handled();
    };

    if from_keyword == to_keyword {
        return FfiAnimationValueResult {
            value: unsafe { crate::css::style_value::retain_style_value(from) },
            handled: true,
        };
    }

    // https://drafts.csswg.org/css-contain/#content-visibility-animation
    // In general, the content-visibility property’s animation type is discrete.
    // However, similar to interpolation of visibility, during interpolation between hidden and any other content-visibility value,
    // p values between 0 and 1 map to the non-hidden value.
    let hidden = crate::css::style_compute::keyword::HIDDEN;
    if *from_keyword == hidden || *to_keyword == hidden {
        if !context.is_some_and(|context| context.allow_discrete) {
            return handled_without_value();
        }
        let value = if delta <= 0.0 {
            from
        } else if delta >= 1.0 || *from_keyword == hidden {
            to
        } else {
            from
        };
        return FfiAnimationValueResult {
            value: unsafe { crate::css::style_value::retain_style_value(value) },
            handled: true,
        };
    }

    discrete_value(context, from, to, delta)
}

fn interpolate_display(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    let (StyleValueData::Display { raw: from_raw }, StyleValueData::Display { raw: to_raw }) = (from, to) else {
        return not_handled();
    };

    if from_raw == to_raw {
        return FfiAnimationValueResult {
            value: unsafe { crate::css::style_value::retain_style_value(from) },
            handled: true,
        };
    }

    // https://drafts.csswg.org/css-display-4/#display-animation
    // In general, the display property’s animation type is discrete. However, similar to interpolation of
    // visibility (see Web Animations §  Animation of visibility), during interpolation between none and any
    // other display value, p values between 0 and 1 map to the non-none value. Additionally, the element is
    // inert as long as its display value would compute to none when ignoring the Transitions and Animations
    // cascade origins.
    // FIXME: Implement the inertness portion of this.
    let from_is_none = crate::css::style_compute::display_is_none(*from_raw);
    let to_is_none = crate::css::style_compute::display_is_none(*to_raw);
    if from_is_none || to_is_none {
        if !context.is_some_and(|context| context.allow_discrete) {
            return handled_without_value();
        }
        let value = if delta <= 0.0 {
            from
        } else if delta >= 1.0 || from_is_none {
            to
        } else {
            from
        };
        return FfiAnimationValueResult {
            value: unsafe { crate::css::style_value::retain_style_value(value) },
            handled: true,
        };
    }

    discrete_value(context, from, to, delta)
}

fn decode_scale_components(value: &StyleValueData) -> Option<Vec<f64>> {
    if matches!(value, StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword()) {
        return Some(vec![1.0, 1.0]);
    }
    let StyleValueData::Transformation { values, .. } = value else {
        return None;
    };
    if !matches!(values.as_slice().len(), 2 | 3) {
        return None;
    }
    values
        .as_slice()
        .iter()
        .map(|value| match value.data() {
            StyleValueData::Number { value } => Some(*value),
            StyleValueData::Percentage { value } => Some(*value / 100.0),
            calculated @ StyleValueData::Calculated { .. } => {
                crate::css::calc::resolve_calculated_number_without_context(calculated).or_else(|| {
                    crate::css::calc::resolve_calculated_percentage_without_context(calculated)
                        .map(|value| value / 100.0)
                })
            }
            _ => None,
        })
        .collect::<Option<Vec<_>>>()
}

fn interpolate_scale(from: &StyleValueData, to: &StyleValueData, delta: f32) -> FfiAnimationValueResult {
    let none = crate::css::style_compute::none_keyword();
    if matches!(from, StyleValueData::Keyword { keyword } if *keyword == none)
        && matches!(to, StyleValueData::Keyword { keyword } if *keyword == none)
    {
        return FfiAnimationValueResult {
            value: unsafe { crate::css::style_value::retain_style_value(from) },
            handled: true,
        };
    }

    // https://drafts.csswg.org/css-transforms-2/#propdef-scale
    // Animation type: by computed value, but see below for none
    // The scale property accepts 1-3 values, each specifying a scale along one axis, in order X, Y, then Z.
    // If the Y value is not given, then it defaults to being the same as the X value.
    // If the Z value is not given, then it defaults to 1.
    // A <percentage> is equivalent to a <number>, for example scale: 100% is equivalent to scale: 1. Numbers are used
    // during serialization of specified and computed values.
    // When translate, rotate or scale are animating or transitioning, and the from value or to value (but not both) is
    // none, the value none is replaced by the equivalent identity value (0px for translate, 0deg for rotate, 1 for scale).
    let (Some(mut from), Some(mut to)) = (decode_scale_components(from), decode_scale_components(to)) else {
        return not_handled();
    };
    let is_3d = from.len() == 3 || to.len() == 3;
    if is_3d {
        from.resize(3, 1.0);
        to.resize(3, 1.0);
    }
    let values = from
        .into_iter()
        .zip(to)
        .map(|(from, to)| retained_number(interpolate_f64(from, to, delta, None)))
        .collect();
    owned(StyleValueData::Transformation {
        property: crate::css::property_metadata::property_id::SCALE,
        transform_function: if is_3d {
            TRANSFORM_FUNCTION_SCALE_3D
        } else {
            TRANSFORM_FUNCTION_SCALE
        },
        values: RetainedStyleValueDataList::from_retained_values(values),
    })
}

fn length_percentage_calculation_node(value: &StyleValueData) -> Option<Arc<crate::css::calc::CalcNode>> {
    match value {
        StyleValueData::Length { value, unit } => Some(Arc::new(crate::css::calc::CalcNode::Numeric(
            crate::css::calc::CalcNumericValue::Length {
                value: *value,
                unit: *unit,
            },
        ))),
        StyleValueData::Percentage { value } => Some(Arc::new(crate::css::calc::CalcNode::Numeric(
            crate::css::calc::CalcNumericValue::Percentage(*value),
        ))),
        StyleValueData::Calculated { rust_calculation, .. } => Some(rust_calculation.node_arc()),
        _ => None,
    }
}

fn numeric_calculation_node(value: &StyleValueData) -> Option<Arc<crate::css::calc::CalcNode>> {
    let numeric = match value {
        StyleValueData::Number { value } => crate::css::calc::CalcNumericValue::Number {
            value: *value,
            number_type: 0,
        },
        StyleValueData::Integer { value } => crate::css::calc::CalcNumericValue::Number {
            value: *value as f64,
            number_type: 2,
        },
        StyleValueData::Angle { value, unit } => crate::css::calc::CalcNumericValue::Angle {
            value: *value,
            unit: *unit,
        },
        StyleValueData::Flex { value, unit } => crate::css::calc::CalcNumericValue::Flex {
            value: *value,
            unit: *unit,
        },
        StyleValueData::Frequency { value, unit } => crate::css::calc::CalcNumericValue::Frequency {
            value: *value,
            unit: *unit,
        },
        StyleValueData::Length { value, unit } => crate::css::calc::CalcNumericValue::Length {
            value: *value,
            unit: *unit,
        },
        StyleValueData::Percentage { value } => crate::css::calc::CalcNumericValue::Percentage(*value),
        StyleValueData::Resolution { value, unit } => crate::css::calc::CalcNumericValue::Resolution {
            value: *value,
            unit: *unit,
        },
        StyleValueData::Time { value, unit } => crate::css::calc::CalcNumericValue::Time {
            value: *value,
            unit: *unit,
        },
        StyleValueData::Calculated { rust_calculation, .. } => return Some(rust_calculation.node_arc()),
        _ => return None,
    };
    Some(Arc::new(crate::css::calc::CalcNode::Numeric(numeric)))
}

struct AnimationCalculationContext<'a> {
    resolve_as_is_number: bool,
    resolve_as_base: u8,
    has_percentages_resolve_as: bool,
    percentages_resolve_as: u8,
    resolve_numbers_as_integers: bool,
    accepted_ranges: &'a RetainedNumericRangeList,
}

fn animation_calculation_context(value: &StyleValueData) -> Option<AnimationCalculationContext<'_>> {
    let StyleValueData::Calculated {
        resolve_as_is_number,
        resolve_as_base,
        has_percentages_resolve_as,
        percentages_resolve_as,
        resolve_numbers_as_integers,
        accepted_ranges,
        ..
    } = value
    else {
        return None;
    };
    Some(AnimationCalculationContext {
        resolve_as_is_number: *resolve_as_is_number,
        resolve_as_base: *resolve_as_base,
        has_percentages_resolve_as: *has_percentages_resolve_as,
        percentages_resolve_as: *percentages_resolve_as,
        resolve_numbers_as_integers: *resolve_numbers_as_integers,
        accepted_ranges,
    })
}

fn retained_calculation(
    calculation: Arc<crate::css::calc::CalcNode>,
    resolved_type: crate::css::calc::FfiNumericType,
    context: &AnimationCalculationContext<'_>,
) -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::Calculated {
        rust_calculation: crate::css::calc::CalcNodeHandle::from_arc(calculation),
        resolve_as_is_number: context.resolve_as_is_number,
        resolve_as_base: context.resolve_as_base,
        resolved_type,
        has_percentages_resolve_as: context.has_percentages_resolve_as,
        percentages_resolve_as: context.percentages_resolve_as,
        resolve_numbers_as_integers: context.resolve_numbers_as_integers,
        accepted_ranges: context.accepted_ranges.clone_owned(),
    }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn retained_length_percentage_calculation(
    calculation: Arc<crate::css::calc::CalcNode>,
    resolved_type: crate::css::calc::FfiNumericType,
) -> RetainedStyleValueData {
    let value = match &*calculation {
        crate::css::calc::CalcNode::Numeric(crate::css::calc::CalcNumericValue::Length { value, unit }) => {
            StyleValueData::Length {
                value: *value,
                unit: *unit,
            }
        }
        crate::css::calc::CalcNode::Numeric(crate::css::calc::CalcNumericValue::Percentage(value)) => {
            StyleValueData::Percentage { value: *value }
        }
        _ => crate::css::calc::length_percentage_calculated_style_value(calculation, resolved_type),
    };
    let value = Arc::into_raw(Arc::new(value));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn interpolate_translate_component(
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> Option<RetainedStyleValueData> {
    let direct = interpolate_scalar_value(property_id, from, to, delta, &[]);
    if direct.handled && !direct.value.is_null() {
        return Some(unsafe { RetainedStyleValueData::from_retained_pointer(direct.value) });
    }

    // https://drafts.csswg.org/css-values-4/#combine-mixed
    // The computed value of a percentage-dimension mix is defined as
    // a computed percentage if the dimension component is zero
    let dimension_component_is_zero = match (from, to) {
        (StyleValueData::Length { value, .. }, StyleValueData::Percentage { .. }) => {
            *value * (1.0 - delta as f64) == 0.0
        }
        (StyleValueData::Percentage { .. }, StyleValueData::Length { value, .. }) => *value * delta as f64 == 0.0,
        _ => false,
    };
    if dimension_component_is_zero {
        let percentage = match (from, to) {
            (StyleValueData::Length { .. }, StyleValueData::Percentage { value }) => *value * delta as f64,
            (StyleValueData::Percentage { value }, StyleValueData::Length { .. }) => *value * (1.0 - delta as f64),
            _ => unreachable!(),
        };
        let value = Arc::into_raw(Arc::new(StyleValueData::Percentage { value: percentage }));
        return Some(unsafe { RetainedStyleValueData::from_retained_pointer(value) });
    }

    let from = length_percentage_calculation_node(from)?;
    let to = length_percentage_calculation_node(to)?;
    let (calculation, resolved_type) = crate::css::calc::interpolate_length_percentage_calculations(from, to, delta)?;
    Some(retained_length_percentage_calculation(calculation, resolved_type))
}

fn decode_translate_components(value: &StyleValueData) -> Option<Vec<RetainedStyleValueData>> {
    if matches!(value, StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword()) {
        return Some(vec![retained_zero_px(), retained_zero_px()]);
    }
    let StyleValueData::Transformation { values, .. } = value else {
        return None;
    };
    if !matches!(values.as_slice().len(), 2 | 3) {
        return None;
    }
    Some(
        values
            .as_slice()
            .iter()
            .map(|value| value.clone_retained())
            .collect::<Vec<_>>(),
    )
}

fn interpolate_translate(from: &StyleValueData, to: &StyleValueData, delta: f32) -> FfiAnimationValueResult {
    let none = crate::css::style_compute::none_keyword();
    if matches!(from, StyleValueData::Keyword { keyword } if *keyword == none)
        && matches!(to, StyleValueData::Keyword { keyword } if *keyword == none)
    {
        return FfiAnimationValueResult {
            value: unsafe { crate::css::style_value::retain_style_value(from) },
            handled: true,
        };
    }

    // https://drafts.csswg.org/css-transforms-2/#propdef-translate
    // Animation type: by computed value, but see below for none
    // The translate property accepts 1-3 values, each specifying a translation against one axis, in the order X, Y,
    // then Z. When the second or third values are missing, they default to 0px.
    // If the third value is omitted or zero, this specifies a 2d translation, equivalent to the translate() function.
    // Otherwise, this specifies a 3d translation, equivalent to the translate3d() function.
    // When translate, rotate or scale are animating or transitioning, and the from value or to value (but not both) is
    // none, the value none is replaced by the equivalent identity value (0px for translate, 0deg for rotate, 1 for scale).
    let (Some(mut from), Some(mut to)) = (decode_translate_components(from), decode_translate_components(to)) else {
        return not_handled();
    };
    let is_3d = from.len() == 3 || to.len() == 3;
    let zero = || {
        let zero = Arc::into_raw(Arc::new(StyleValueData::Length {
            value: 0.0,
            unit: crate::css::calc::canonical_pixel_unit(),
        }));
        unsafe { RetainedStyleValueData::from_retained_pointer(zero) }
    };
    if is_3d {
        from.resize_with(3, zero);
        to.resize_with(3, zero);
    }
    let values = from
        .iter()
        .zip(to.iter())
        .map(|(from, to)| {
            interpolate_translate_component(
                crate::css::property_metadata::property_id::TRANSLATE,
                from.data(),
                to.data(),
                delta,
            )
        })
        .collect::<Option<Vec<_>>>();
    let Some(values) = values else {
        return not_handled();
    };
    owned(StyleValueData::Transformation {
        property: crate::css::property_metadata::property_id::TRANSLATE,
        transform_function: if is_3d {
            TRANSFORM_FUNCTION_TRANSLATE_3D
        } else {
            TRANSFORM_FUNCTION_TRANSLATE
        },
        values: RetainedStyleValueDataList::from_retained_values(values),
    })
}

fn interpolate_individual_rotate(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    let none = crate::css::style_compute::none_keyword();
    if matches!(from, StyleValueData::Keyword { keyword } if *keyword == none)
        && matches!(to, StyleValueData::Keyword { keyword } if *keyword == none)
    {
        return FfiAnimationValueResult {
            value: unsafe { crate::css::style_value::retain_style_value(from) },
            handled: true,
        };
    }

    // https://drafts.csswg.org/css-transforms-2/#propdef-rotate
    // Animation type: as SLERP, but see below for none
    // The rotate property accepts an angle to rotate an element, and optionally an axis to rotate it around.
    // When translate, rotate or scale are animating or transitioning, and the from value or to value (but not both) is
    // none, the value none is replaced by the equivalent identity value (0px for translate, 0deg for rotate, 1 for scale).
    let is_none = |value: &StyleValueData| matches!(value, StyleValueData::Keyword { keyword } if *keyword == none);
    let is_2d = |value: &StyleValueData| {
        is_none(value)
            || matches!(value, StyleValueData::Transformation { transform_function, values, .. }
                if matches!(*transform_function, TRANSFORM_FUNCTION_ROTATE | TRANSFORM_FUNCTION_ROTATE_Z)
                    && values.as_slice().len() == 1)
    };
    if is_2d(from) && is_2d(to) {
        let angle = |value: &StyleValueData| {
            if is_none(value) {
                return Some(0.0);
            }
            let StyleValueData::Transformation { values, .. } = value else {
                return None;
            };
            let [angle] = values.as_slice() else {
                return None;
            };
            resolve_animation_angle(context, angle.data())
        };
        let (Some(from), Some(to)) = (angle(from), angle(to)) else {
            return not_handled();
        };
        let angle = Arc::into_raw(Arc::new(StyleValueData::Angle {
            value: interpolate_f64(from, to, delta, None),
            unit: 0,
        }));
        return owned(StyleValueData::Transformation {
            property: crate::css::property_metadata::property_id::ROTATE,
            transform_function: TRANSFORM_FUNCTION_ROTATE,
            values: RetainedStyleValueDataList::from_retained_values(vec![unsafe {
                RetainedStyleValueData::from_retained_pointer(angle)
            }]),
        });
    }

    let axis_rotation = |x: f64, y: f64, z: f64, angle: &RetainedStyleValueData| {
        Some(RetainedStyleValueDataList::from_retained_values(vec![
            retained_number(x),
            retained_number(y),
            retained_number(z),
            angle.clone_retained(),
        ]))
    };
    let normalize = |value: &StyleValueData| {
        if is_none(value) {
            let angle = Arc::into_raw(Arc::new(StyleValueData::Angle { value: 0.0, unit: 0 }));
            return Some(RetainedStyleValueDataList::from_retained_values(vec![
                retained_number(0.0),
                retained_number(0.0),
                retained_number(1.0),
                unsafe { RetainedStyleValueData::from_retained_pointer(angle) },
            ]));
        }
        let StyleValueData::Transformation {
            transform_function,
            values,
            ..
        } = value
        else {
            return None;
        };
        match (*transform_function, values.as_slice()) {
            (TRANSFORM_FUNCTION_ROTATE | TRANSFORM_FUNCTION_ROTATE_Z, [angle]) => axis_rotation(0.0, 0.0, 1.0, angle),
            (TRANSFORM_FUNCTION_ROTATE_X, [angle]) => axis_rotation(1.0, 0.0, 0.0, angle),
            (TRANSFORM_FUNCTION_ROTATE_Y, [angle]) => axis_rotation(0.0, 1.0, 0.0, angle),
            (TRANSFORM_FUNCTION_ROTATE_3D, [..]) if values.as_slice().len() == 4 => {
                Some(RetainedStyleValueDataList::from_retained_values(
                    values
                        .as_slice()
                        .iter()
                        .map(RetainedStyleValueData::clone_retained)
                        .collect(),
                ))
            }
            _ => None,
        }
    };
    let (Some(from), Some(to)) = (normalize(from), normalize(to)) else {
        return not_handled();
    };
    interpolate_rotate_3d(
        context,
        crate::css::property_metadata::property_id::ROTATE,
        TRANSFORM_FUNCTION_ROTATE_3D,
        &from,
        &to,
        delta,
    )
    .map_or_else(not_handled, owned)
}

fn interpolate_font_variation_settings(
    context: Option<&FfiAnimationContext>,
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    // https://drafts.csswg.org/css-fonts/#font-variation-settings-def
    // Two declarations of font-feature-settings can be animated between if they are "like". "Like" declarations
    // are ones where the same set of properties appear (in any order). Because successive duplicate properties
    // are applied instead of prior duplicate properties, two declarations can be "like" even if they have
    // differing number of properties. If two declarations are "like" then animation occurs pairwise between
    // corresponding values in the declarations. Otherwise, animation is not possible.
    if !matches!(from, StyleValueData::ValueList { .. }) || !matches!(to, StyleValueData::ValueList { .. }) {
        return discrete_value(context, from, to, delta);
    }

    // NB: The values in these lists have already been deduplicated and sorted at this point, so we can
    //     interpolate them pairwise.
    let result = interpolate_scalar_value(property_id, from, to, delta, &[]);
    if !result.handled || result.value.is_null() {
        return discrete_value(context, from, to, delta);
    }
    result
}

fn radius_components_equal(first: &StyleValueData, second: &StyleValueData) -> bool {
    match (first, second) {
        (
            StyleValueData::Length {
                value: first_value,
                unit: first_unit,
            },
            StyleValueData::Length {
                value: second_value,
                unit: second_unit,
            },
        ) => first_value == second_value && first_unit == second_unit,
        (StyleValueData::Percentage { value: first }, StyleValueData::Percentage { value: second }) => first == second,
        _ => false,
    }
}

struct ExpandedGridTrack<'a> {
    track: &'a RetainedGridTrackEntry,
    line_names: Option<&'a RetainedUtf16FlyStringList>,
}

fn empty_retained_style_value() -> RetainedStyleValueData {
    unsafe { RetainedStyleValueData::from_retained_optional_pointer(std::ptr::null()) }
}

fn empty_grid_line_names() -> RetainedUtf16FlyStringList {
    RetainedUtf16FlyStringList::from_retained_strings(Vec::new())
}

fn grid_nested_entries(entry: &RetainedGridTrackEntry) -> &[RetainedGridTrackEntry] {
    if entry.repeat_entries_pointer.is_null() {
        return &[];
    }
    unsafe { std::slice::from_raw_parts(entry.repeat_entries_pointer, entry.repeat_entries_length) }
}

fn grid_entries_into_raw_parts(entries: Vec<RetainedGridTrackEntry>) -> (*mut RetainedGridTrackEntry, usize) {
    let entries = entries.into_boxed_slice();
    let length = entries.len();
    (Box::into_raw(entries) as *mut RetainedGridTrackEntry, length)
}

fn clone_grid_track_entry(entry: &RetainedGridTrackEntry) -> RetainedGridTrackEntry {
    let (repeat_entries_pointer, repeat_entries_length) =
        grid_entries_into_raw_parts(grid_nested_entries(entry).iter().map(clone_grid_track_entry).collect());
    RetainedGridTrackEntry {
        kind: entry.kind,
        names: entry.names.clone_retained(),
        size_value: entry.size_value.clone_retained(),
        min_value: entry.min_value.clone_retained(),
        max_value: entry.max_value.clone_retained(),
        repeat_type: entry.repeat_type,
        repeat_count: entry.repeat_count.clone_retained(),
        repeat_is_subgrid: entry.repeat_is_subgrid,
        repeat_preserve_line_name_sets: entry.repeat_preserve_line_name_sets,
        repeat_entries_pointer,
        repeat_entries_length,
    }
}

fn grid_line_names_entry(names: &RetainedUtf16FlyStringList) -> RetainedGridTrackEntry {
    RetainedGridTrackEntry {
        kind: GridTrackEntryKind::LineNames,
        names: names.clone_retained(),
        size_value: empty_retained_style_value(),
        min_value: empty_retained_style_value(),
        max_value: empty_retained_style_value(),
        repeat_type: 0,
        repeat_count: empty_retained_style_value(),
        repeat_is_subgrid: false,
        repeat_preserve_line_name_sets: false,
        repeat_entries_pointer: std::ptr::null_mut(),
        repeat_entries_length: 0,
    }
}

fn expand_grid_tracks_and_lines(entries: &[RetainedGridTrackEntry]) -> Option<Vec<ExpandedGridTrack<'_>>> {
    let mut result = Vec::new();
    let mut current_track = None;
    let mut current_line_names = None;

    for entry in entries {
        if entry.kind == GridTrackEntryKind::LineNames {
            if current_line_names.is_some() {
                return None;
            }
            current_line_names = Some(&entry.names);
        } else {
            if let Some(track) = current_track.take() {
                result.push(ExpandedGridTrack {
                    track,
                    line_names: current_line_names.take(),
                });
            }
            current_track = Some(entry);
        }

        if current_track.is_some() && current_line_names.is_some() {
            result.push(ExpandedGridTrack {
                track: current_track.take().expect("checked above"),
                line_names: current_line_names.take(),
            });
        }
    }
    if let Some(track) = current_track {
        result.push(ExpandedGridTrack {
            track,
            line_names: current_line_names,
        });
    }
    Some(result)
}

fn append_grid_track_with_line_names(
    result: &mut Vec<RetainedGridTrackEntry>,
    track: RetainedGridTrackEntry,
    line_names: Option<&RetainedUtf16FlyStringList>,
) {
    result.push(track);
    if let Some(line_names) = line_names {
        result.push(grid_line_names_entry(line_names));
    }
}

fn interpolate_grid_component(
    property_id: u16,
    from: &RetainedStyleValueData,
    to: &RetainedStyleValueData,
    delta: f32,
) -> RetainedStyleValueData {
    if let Some(value) = interpolate_translate_component(property_id, from.data(), to.data(), delta) {
        return value;
    }
    if delta < 0.5 {
        from.clone_retained()
    } else {
        to.clone_retained()
    }
}

fn interpolate_grid_track_entries(
    property_id: u16,
    from_is_subgrid: bool,
    from_entries: &[RetainedGridTrackEntry],
    to_is_subgrid: bool,
    to_entries: &[RetainedGridTrackEntry],
    delta: f32,
) -> Option<Vec<RetainedGridTrackEntry>> {
    // https://drafts.csswg.org/css-grid-2/#track-sizing
    // Animation type: if the list lengths match, by computed value type per item in the computed track list;
    // discrete otherwise.
    //
    // https://drafts.csswg.org/css-grid-2/#computed-track-list-subgrid
    // The computed track list of a subgrid axis is the subgrid keyword followed by a list of line names.
    if from_is_subgrid || to_is_subgrid {
        return None;
    }

    let expanded_from = expand_grid_tracks_and_lines(from_entries)?;
    let expanded_to = expand_grid_tracks_and_lines(to_entries)?;
    if expanded_from.len() != expanded_to.len() {
        return None;
    }

    let mut result = Vec::new();
    for (from, to) in expanded_from.iter().zip(&expanded_to) {
        let line_names = if delta < 0.5 { from.line_names } else { to.line_names };
        let track = match (from.track.kind, to.track.kind) {
            (GridTrackEntryKind::Repeat, GridTrackEntryKind::Repeat) => {
                // https://drafts.csswg.org/css-grid/#repeat-interpolation
                if from.track.repeat_type != GRID_REPEAT_FIXED || to.track.repeat_type != GRID_REPEAT_FIXED {
                    return None;
                }
                let (StyleValueData::Integer { value: from_count }, StyleValueData::Integer { value: to_count }) =
                    (from.track.repeat_count.data(), to.track.repeat_count.data())
                else {
                    return None;
                };
                if from_count != to_count {
                    return None;
                }
                let nested = interpolate_grid_track_entries(
                    property_id,
                    from.track.repeat_is_subgrid,
                    grid_nested_entries(from.track),
                    to.track.repeat_is_subgrid,
                    grid_nested_entries(to.track),
                    delta,
                )?;
                let (repeat_entries_pointer, repeat_entries_length) = grid_entries_into_raw_parts(nested);
                RetainedGridTrackEntry {
                    kind: GridTrackEntryKind::Repeat,
                    names: empty_grid_line_names(),
                    size_value: empty_retained_style_value(),
                    min_value: empty_retained_style_value(),
                    max_value: empty_retained_style_value(),
                    repeat_type: from.track.repeat_type,
                    repeat_count: from.track.repeat_count.clone_retained(),
                    repeat_is_subgrid: false,
                    repeat_preserve_line_name_sets: false,
                    repeat_entries_pointer,
                    repeat_entries_length,
                }
            }
            (GridTrackEntryKind::Repeat, _) | (_, GridTrackEntryKind::Repeat) => return None,
            (GridTrackEntryKind::MinMax, GridTrackEntryKind::MinMax) => RetainedGridTrackEntry {
                kind: GridTrackEntryKind::MinMax,
                names: empty_grid_line_names(),
                size_value: empty_retained_style_value(),
                min_value: interpolate_grid_component(property_id, &from.track.min_value, &to.track.min_value, delta),
                max_value: interpolate_grid_component(property_id, &from.track.max_value, &to.track.max_value, delta),
                repeat_type: 0,
                repeat_count: empty_retained_style_value(),
                repeat_is_subgrid: false,
                repeat_preserve_line_name_sets: false,
                repeat_entries_pointer: std::ptr::null_mut(),
                repeat_entries_length: 0,
            },
            (GridTrackEntryKind::Size, GridTrackEntryKind::Size) => RetainedGridTrackEntry {
                kind: GridTrackEntryKind::Size,
                names: empty_grid_line_names(),
                size_value: interpolate_grid_component(
                    property_id,
                    &from.track.size_value,
                    &to.track.size_value,
                    delta,
                ),
                min_value: empty_retained_style_value(),
                max_value: empty_retained_style_value(),
                repeat_type: 0,
                repeat_count: empty_retained_style_value(),
                repeat_is_subgrid: false,
                repeat_preserve_line_name_sets: false,
                repeat_entries_pointer: std::ptr::null_mut(),
                repeat_entries_length: 0,
            },
            _ => {
                if delta < 0.5 {
                    clone_grid_track_entry(from.track)
                } else {
                    clone_grid_track_entry(to.track)
                }
            }
        };
        append_grid_track_with_line_names(&mut result, track, line_names);
    }
    Some(result)
}

fn composite_grid_component(
    underlying: &RetainedStyleValueData,
    animated: &RetainedStyleValueData,
    operation: FfiCompositeOperation,
) -> RetainedStyleValueData {
    let result = composite_scalar_value(underlying.data(), animated.data(), operation);
    if result.handled && !result.value.is_null() {
        return unsafe { RetainedStyleValueData::from_retained_pointer(result.value) };
    }

    if let (Some(underlying), Some(animated)) = (
        length_percentage_calculation_node(underlying.data()),
        length_percentage_calculation_node(animated.data()),
    ) && let Some((calculation, resolved_type)) =
        crate::css::calc::add_length_percentage_calculations(underlying, animated)
    {
        // https://drafts.csswg.org/css-values-4/#combine-mixed
        // Addition of <percentage> is defined the same as interpolation except by adding each component rather than interpolating it.
        return retained_length_percentage_calculation(calculation, resolved_type);
    }

    animated.clone_retained()
}

fn composite_grid_track_entries(
    underlying_is_subgrid: bool,
    underlying_entries: &[RetainedGridTrackEntry],
    animated_is_subgrid: bool,
    animated_entries: &[RetainedGridTrackEntry],
    operation: FfiCompositeOperation,
) -> Option<Vec<RetainedGridTrackEntry>> {
    // https://drafts.csswg.org/css-grid-2/#track-sizing
    // Animation type: if the list lengths match, by computed value type per item in the computed track list;
    // discrete otherwise.
    //
    // https://drafts.csswg.org/css-grid-2/#computed-track-list-subgrid
    // The computed track list of a subgrid axis is the subgrid keyword followed by a list of line names.
    if underlying_is_subgrid || animated_is_subgrid {
        return None;
    }

    let expanded_underlying = expand_grid_tracks_and_lines(underlying_entries)?;
    let expanded_animated = expand_grid_tracks_and_lines(animated_entries)?;
    if expanded_underlying.len() != expanded_animated.len() {
        return None;
    }

    let mut result = Vec::new();
    for (underlying, animated) in expanded_underlying.iter().zip(&expanded_animated) {
        let track = match (underlying.track.kind, animated.track.kind) {
            (GridTrackEntryKind::Repeat, GridTrackEntryKind::Repeat) => {
                if underlying.track.repeat_type != GRID_REPEAT_FIXED || animated.track.repeat_type != GRID_REPEAT_FIXED
                {
                    return None;
                }
                let (
                    StyleValueData::Integer {
                        value: underlying_count,
                    },
                    StyleValueData::Integer { value: animated_count },
                ) = (underlying.track.repeat_count.data(), animated.track.repeat_count.data())
                else {
                    return None;
                };
                if underlying_count != animated_count {
                    return None;
                }
                let nested = composite_grid_track_entries(
                    underlying.track.repeat_is_subgrid,
                    grid_nested_entries(underlying.track),
                    animated.track.repeat_is_subgrid,
                    grid_nested_entries(animated.track),
                    operation,
                )?;
                let (repeat_entries_pointer, repeat_entries_length) = grid_entries_into_raw_parts(nested);
                RetainedGridTrackEntry {
                    kind: GridTrackEntryKind::Repeat,
                    names: empty_grid_line_names(),
                    size_value: empty_retained_style_value(),
                    min_value: empty_retained_style_value(),
                    max_value: empty_retained_style_value(),
                    repeat_type: underlying.track.repeat_type,
                    repeat_count: underlying.track.repeat_count.clone_retained(),
                    repeat_is_subgrid: false,
                    repeat_preserve_line_name_sets: false,
                    repeat_entries_pointer,
                    repeat_entries_length,
                }
            }
            (GridTrackEntryKind::Repeat, _) | (_, GridTrackEntryKind::Repeat) => return None,
            (GridTrackEntryKind::MinMax, GridTrackEntryKind::MinMax) => RetainedGridTrackEntry {
                kind: GridTrackEntryKind::MinMax,
                names: empty_grid_line_names(),
                size_value: empty_retained_style_value(),
                min_value: composite_grid_component(&underlying.track.min_value, &animated.track.min_value, operation),
                max_value: composite_grid_component(&underlying.track.max_value, &animated.track.max_value, operation),
                repeat_type: 0,
                repeat_count: empty_retained_style_value(),
                repeat_is_subgrid: false,
                repeat_preserve_line_name_sets: false,
                repeat_entries_pointer: std::ptr::null_mut(),
                repeat_entries_length: 0,
            },
            (GridTrackEntryKind::Size, GridTrackEntryKind::Size) => RetainedGridTrackEntry {
                kind: GridTrackEntryKind::Size,
                names: empty_grid_line_names(),
                size_value: composite_grid_component(
                    &underlying.track.size_value,
                    &animated.track.size_value,
                    operation,
                ),
                min_value: empty_retained_style_value(),
                max_value: empty_retained_style_value(),
                repeat_type: 0,
                repeat_count: empty_retained_style_value(),
                repeat_is_subgrid: false,
                repeat_preserve_line_name_sets: false,
                repeat_entries_pointer: std::ptr::null_mut(),
                repeat_entries_length: 0,
            },
            _ => clone_grid_track_entry(animated.track),
        };
        append_grid_track_with_line_names(&mut result, track, animated.line_names);
    }
    Some(result)
}

fn retained_animation_result(result: FfiAnimationValueResult) -> Option<RetainedStyleValueData> {
    if !result.handled || result.value.is_null() {
        return None;
    }
    Some(unsafe { RetainedStyleValueData::from_retained_pointer(result.value) })
}

fn retained_percentage(value: f64) -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::Percentage { value }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn retained_computed_center_position() -> RetainedStyleValueData {
    let edge = || {
        let edge = Arc::into_raw(Arc::new(StyleValueData::Edge {
            has_edge: false,
            edge: 0,
            offset: retained_percentage(50.0),
        }));
        unsafe { RetainedStyleValueData::from_retained_pointer(edge) }
    };
    let position = Arc::into_raw(Arc::new(StyleValueData::Position {
        edge_x: edge(),
        edge_y: edge(),
    }));
    unsafe { RetainedStyleValueData::from_retained_pointer(position) }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ColorComponentCategory {
    Red,
    Green,
    Blue,
    Lightness,
    OpponentA,
    OpponentB,
    Colorfulness,
    Hue,
    NotAnalogous,
}

#[derive(Clone, Copy)]
struct NativeColor {
    color_type: u8,
    components: [f32; 4],
    missing: [bool; 4],
}

fn resolve_color_component(value: &StyleValueData, reference_value: f32) -> Option<(f32, bool)> {
    match value {
        StyleValueData::Number { value } => Some((*value as f32, false)),
        StyleValueData::Percentage { value } => Some((*value as f32 / 100.0 * reference_value, false)),
        StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword() => {
            Some((0.0, true))
        }
        _ => None,
    }
}

fn native_color_components(value: &StyleValueData) -> Option<NativeColor> {
    let StyleValueData::ColorFunction {
        color_base,
        channel_0,
        channel_1,
        channel_2,
        alpha,
        origin_color,
        ..
    } = value
    else {
        return None;
    };
    if !color_base.has_color_type || origin_color.optional_data().is_some() {
        return None;
    }

    let resolve_hue = |value: &StyleValueData| match value {
        StyleValueData::Number { value } => Some((*value as f32, false)),
        StyleValueData::Angle { value, unit } => angle_to_degrees(*value, *unit).map(|value| (value as f32, false)),
        StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword() => {
            Some((0.0, true))
        }
        _ => None,
    };
    let resolve = |value: &RetainedStyleValueData, reference: f32| resolve_color_component(value.data(), reference);
    let resolve_fraction = |value: &RetainedStyleValueData| {
        let (value, missing) = resolve(value, 100.0)?;
        Some((value / 100.0, missing))
    };
    let ((first, first_missing), (second, second_missing), (third, third_missing)) = match color_base.color_type {
        COLOR_TYPE_RGB => (
            resolve(channel_0, 255.0)?,
            resolve(channel_1, 255.0)?,
            resolve(channel_2, 255.0)?,
        ),
        COLOR_TYPE_HSL | COLOR_TYPE_HWB => (
            resolve_hue(channel_0.data())?,
            resolve_fraction(channel_1)?,
            resolve_fraction(channel_2)?,
        ),
        COLOR_TYPE_LAB => (
            resolve(channel_0, 100.0)?,
            resolve(channel_1, 125.0)?,
            resolve(channel_2, 125.0)?,
        ),
        COLOR_TYPE_OKLAB => (
            resolve(channel_0, 1.0)?,
            resolve(channel_1, 0.4)?,
            resolve(channel_2, 0.4)?,
        ),
        COLOR_TYPE_LCH => (
            resolve(channel_0, 100.0)?,
            resolve(channel_1, 150.0)?,
            resolve_hue(channel_2.data())?,
        ),
        COLOR_TYPE_OKLCH => (
            resolve(channel_0, 1.0)?,
            resolve(channel_1, 0.4)?,
            resolve_hue(channel_2.data())?,
        ),
        _ => (
            resolve(channel_0, 1.0)?,
            resolve(channel_1, 1.0)?,
            resolve(channel_2, 1.0)?,
        ),
    };
    let (alpha, alpha_missing) = match alpha.optional_data() {
        Some(value) => resolve_color_component(value, 1.0)?,
        None => (1.0, false),
    };
    let mut components = [first, second, third, alpha.clamp(0.0, 1.0)];
    if color_base.color_type == COLOR_TYPE_RGB {
        components[0] = components[0].clamp(0.0, 255.0) / 255.0;
        components[1] = components[1].clamp(0.0, 255.0) / 255.0;
        components[2] = components[2].clamp(0.0, 255.0) / 255.0;
    }
    Some(NativeColor {
        color_type: color_base.color_type,
        components,
        missing: [first_missing, second_missing, third_missing, alpha_missing],
    })
}

fn color_component_categories(color_type: u8) -> [ColorComponentCategory; 3] {
    use ColorComponentCategory::*;
    match color_type {
        COLOR_TYPE_HSL => [Hue, Colorfulness, Lightness],
        COLOR_TYPE_HWB => [Hue, NotAnalogous, NotAnalogous],
        COLOR_TYPE_LAB | COLOR_TYPE_OKLAB => [Lightness, OpponentA, OpponentB],
        COLOR_TYPE_LCH | COLOR_TYPE_OKLCH => [Lightness, Colorfulness, Hue],
        COLOR_TYPE_RGB
        | COLOR_TYPE_A98_RGB
        | COLOR_TYPE_DISPLAY_P3
        | COLOR_TYPE_DISPLAY_P3_LINEAR
        | COLOR_TYPE_SRGB
        | COLOR_TYPE_SRGB_LINEAR
        | COLOR_TYPE_PROPHOTO_RGB
        | COLOR_TYPE_REC2020
        | COLOR_TYPE_XYZ_D50
        | COLOR_TYPE_XYZ_D65 => [Red, Green, Blue],
        _ => [NotAnalogous, NotAnalogous, NotAnalogous],
    }
}

// https://drafts.csswg.org/css-color-4/#interpolation-missing
// Carry forward missing components from the input color space to the interpolation color space.
// A missing component is carried forward if it has an analogous component in the target space.
// Additionally, if ALL components of an analogous set are missing, they are all carried forward.
fn carry_forward_missing_components(
    source_missing: [bool; 4],
    source_categories: [ColorComponentCategory; 3],
    target_categories: [ColorComponentCategory; 3],
) -> [bool; 4] {
    let mut result = [false; 4];

    // Same-space: all components map to themselves, including NotAnalogous ones (e.g. HWB W/B).
    if source_categories == target_categories {
        result = source_missing;
        return result;
    }

    // Carry forward individual analogous components
    for target_index in 0..3 {
        if target_categories[target_index] == ColorComponentCategory::NotAnalogous {
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
        let is_individually_analogous = source_categories[source_index] != ColorComponentCategory::NotAnalogous
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
            let is_individually_analogous = target_categories[target_index] != ColorComponentCategory::NotAnalogous
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

fn legacy_srgb_components(value: &StyleValueData) -> Option<([f32; 4], [bool; 4])> {
    let color = native_color_components(value)?;
    let components = crate::css::color_conversion::legacy_color_to_srgb(color.color_type, color.components)?;
    let missing = carry_forward_missing_components(
        color.missing,
        color_component_categories(color.color_type),
        [
            ColorComponentCategory::Red,
            ColorComponentCategory::Green,
            ColorComponentCategory::Blue,
        ],
    );
    Some((components, missing))
}

// How two colors are combined channel-wise: linear interpolation at a given progress, or addition
// of both inputs for effect composition.
#[derive(Clone, Copy)]
enum ColorCombination {
    Interpolate(f32),
    Add,
}

impl ColorCombination {
    fn combine(self, from: f32, to: f32) -> f32 {
        match self {
            ColorCombination::Interpolate(delta) => from + (to - from) * delta,
            ColorCombination::Add => from + to,
        }
    }
}

fn combine_modern_color(
    from: &StyleValueData,
    to: &StyleValueData,
    combination: ColorCombination,
) -> Option<StyleValueData> {
    // https://drafts.csswg.org/css-color-4/#interpolation
    // 1. checking the two colors for analogous components and analogous sets which will be carried forward
    let from = native_color_components(from)?;
    let to = native_color_components(to)?;

    // 2. prepare both colors for conversion. this changes any powerless components to missing values
    // NB: Every color handled here has native components, so step 2 does not mark additional components missing.

    // 3. converting them both to a given color space which will be referred to as the interpolation color space
    //    below.
    let mut from_components = crate::css::color_conversion::to_oklab(from.color_type, from.components)?;
    let mut to_components = crate::css::color_conversion::to_oklab(to.color_type, to.components)?;
    let target_categories = color_component_categories(COLOR_TYPE_OKLAB);
    let from_missing = carry_forward_missing_components(
        from.missing,
        color_component_categories(from.color_type),
        target_categories,
    );
    let to_missing =
        carry_forward_missing_components(to.missing, color_component_categories(to.color_type), target_categories);

    // 4. (if required) re-inserting carried forward values in the converted colors
    substitute_missing_components(&mut from_components, &mut to_components, from_missing, to_missing);
    let combined_alpha = combination
        .combine(from_components[3], to_components[3])
        .clamp(0.0, 1.0);

    // https://drafts.csswg.org/css-color-4/#interpolation
    let result = if combined_alpha == 0.0 {
        // OPTIMIZATION: Fully transparent results can skip the premultiply/interpolate/unpremultiply cycle.
        [0.0, 0.0, 0.0, 0.0]
    } else {
        // 6. changing the color components to premultiplied form
        // https://drafts.csswg.org/css-color-4/#interpolation-alpha
        // For rectangular orthogonal color coordinate systems, all component values are multiplied by the alpha value.
        let from_premultiplied = [
            from_components[0] * from_components[3],
            from_components[1] * from_components[3],
            from_components[2] * from_components[3],
        ];
        let to_premultiplied = [
            to_components[0] * to_components[3],
            to_components[1] * to_components[3],
            to_components[2] * to_components[3],
        ];

        // 7. linearly interpolating each component of the computed value of the color separately
        let premultiplied = [
            combination.combine(from_premultiplied[0], to_premultiplied[0]),
            combination.combine(from_premultiplied[1], to_premultiplied[1]),
            combination.combine(from_premultiplied[2], to_premultiplied[2]),
        ];

        // 8. undoing premultiplication
        [
            premultiplied[0] / combined_alpha,
            premultiplied[1] / combined_alpha,
            premultiplied[2] / combined_alpha,
            combined_alpha,
        ]
    };

    // https://drafts.csswg.org/css-color-4/#interpolation-space
    // If the host syntax does not define what color space interpolation should take place in, it defaults to Oklab.
    let result_missing = [
        from_missing[0] && to_missing[0],
        from_missing[1] && to_missing[1],
        from_missing[2] && to_missing[2],
        from_missing[3] && to_missing[3],
    ];
    let number_or_none = |value: f32, is_missing: bool| {
        if is_missing {
            retained_none_keyword()
        } else {
            retained_number(value as f64)
        }
    };
    Some(StyleValueData::ColorFunction {
        color_base: ColorBase {
            has_color_type: true,
            color_type: COLOR_TYPE_OKLAB,
            color_syntax: COLOR_SYNTAX_MODERN,
        },
        channel_0: number_or_none(result[0], result_missing[0]),
        channel_1: number_or_none(result[1], result_missing[1]),
        channel_2: number_or_none(result[2], result_missing[2]),
        alpha: number_or_none(result[3], result_missing[3]),
        has_name: false,
        name: empty_retained_fly_string(),
        origin_color: empty_retained_style_value(),
    })
}

fn combine_legacy_rgb(
    from: &StyleValueData,
    to: &StyleValueData,
    combination: ColorCombination,
) -> Option<StyleValueData> {
    // https://drafts.csswg.org/css-color-4/#interpolation-space
    // If the host syntax does not define what color space interpolation should take place in, it defaults to Oklab.
    // However, user agents must handle interpolation between legacy sRGB color formats (hex colors, named colors,
    // rgb(), hsl() or hwb() and the equivalent alpha-including forms) in gamma-encoded sRGB space.
    let (mut from, from_missing) = legacy_srgb_components(from)?;
    let (mut to, to_missing) = legacy_srgb_components(to)?;
    substitute_missing_components(&mut from, &mut to, from_missing, to_missing);
    let combined_alpha = combination.combine(from[3], to[3]).clamp(0.0, 1.0);

    // https://drafts.csswg.org/css-color-4/#interpolation
    let result = if combined_alpha == 0.0 {
        // OPTIMIZATION: Fully transparent results can skip the premultiply/interpolate/unpremultiply cycle.
        [0.0, 0.0, 0.0, 0.0]
    } else {
        // 6. changing the color components to premultiplied form
        // https://drafts.csswg.org/css-color-4/#interpolation-alpha
        // For rectangular orthogonal color coordinate systems, all component values are multiplied by the alpha value.
        let from_premultiplied = [from[0] * from[3], from[1] * from[3], from[2] * from[3]];
        let to_premultiplied = [to[0] * to[3], to[1] * to[3], to[2] * to[3]];

        // 7. linearly interpolating each component of the computed value of the color separately
        let premultiplied = [
            combination.combine(from_premultiplied[0], to_premultiplied[0]),
            combination.combine(from_premultiplied[1], to_premultiplied[1]),
            combination.combine(from_premultiplied[2], to_premultiplied[2]),
        ];

        // 8. undoing premultiplication
        [
            premultiplied[0] / combined_alpha,
            premultiplied[1] / combined_alpha,
            premultiplied[2] / combined_alpha,
            combined_alpha,
        ]
    };

    // https://drafts.csswg.org/css-color-4/#interpolation-space
    // NB: Legacy sRGB content interpolates in sRGB and produces a legacy rgb() result.
    let to_byte = |value: f32| (value * 255.0).round().clamp(0.0, 255.0) as u8;
    let alpha = to_byte(result[3]);
    Some(StyleValueData::ColorFunction {
        color_base: ColorBase {
            has_color_type: true,
            color_type: COLOR_TYPE_RGB,
            color_syntax: COLOR_SYNTAX_LEGACY,
        },
        channel_0: retained_number(to_byte(result[0]) as f64),
        channel_1: retained_number(to_byte(result[1]) as f64),
        channel_2: retained_number(to_byte(result[2]) as f64),
        alpha: retained_number(alpha as f64 / 255.0),
        has_name: false,
        name: empty_retained_fly_string(),
        origin_color: empty_retained_style_value(),
    })
}

fn empty_shape_points() -> RetainedShapePointList {
    RetainedShapePointList::from_retained_points(Vec::new())
}

fn empty_retained_fly_string() -> RetainedUtf16FlyString {
    unsafe { RetainedUtf16FlyString::from_leaked_raw(0) }
}

fn interpolate_basic_shape_component(
    property_id: u16,
    from: &RetainedStyleValueData,
    to: &RetainedStyleValueData,
    delta: f32,
) -> RetainedStyleValueData {
    interpolate_translate_component(property_id, from.data(), to.data(), delta).unwrap_or_else(|| {
        if delta < 0.5 {
            from.clone_retained()
        } else {
            to.clone_retained()
        }
    })
}

struct RadialSizeComponents<'a> {
    component_count: u8,
    is_extent_0: bool,
    value_0: &'a RetainedStyleValueData,
    is_extent_1: bool,
    value_1: &'a RetainedStyleValueData,
}

fn interpolate_radial_size(
    property_id: u16,
    from: RadialSizeComponents<'_>,
    to: RadialSizeComponents<'_>,
    delta: f32,
) -> Option<StyleValueData> {
    // https://drafts.csswg.org/css-images-4/#interpolating-gradients
    // https://drafts.csswg.org/css-shapes-1/#basic-shape-interpolation
    // FIXME: Radial extents should disallow interpolation for basic-shape values but should be converted into their
    //        equivalent length-percentage values for radial gradients
    if from.is_extent_0
        || (from.component_count == 2 && from.is_extent_1)
        || to.is_extent_0
        || (to.component_count == 2 && to.is_extent_1)
    {
        return None;
    }

    let interpolate = |from: &RetainedStyleValueData, to: &RetainedStyleValueData| {
        let direct = interpolate_scalar_value(property_id, from.data(), to.data(), delta, RADIAL_SIZE_RANGES);
        if let Some(value) = retained_animation_result(direct) {
            return Some(value);
        }
        interpolate_translate_component(property_id, from.data(), to.data(), delta)
    };
    if from.component_count == 1 && to.component_count == 1 {
        return Some(StyleValueData::RadialSize {
            component_count: 1,
            is_extent_0: false,
            extent_0: 0,
            value_0: interpolate(from.value_0, to.value_0)?,
            is_extent_1: false,
            extent_1: 0,
            value_1: empty_retained_style_value(),
        });
    }

    let from_vertical = if from.component_count == 2 {
        from.value_1
    } else {
        from.value_0
    };
    let to_vertical = if to.component_count == 2 {
        to.value_1
    } else {
        to.value_0
    };
    Some(StyleValueData::RadialSize {
        component_count: 2,
        is_extent_0: false,
        extent_0: 0,
        value_0: interpolate(from.value_0, to.value_0)?,
        is_extent_1: false,
        extent_1: 0,
        value_1: interpolate(from_vertical, to_vertical)?,
    })
}

fn interpolate_basic_shape(
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> Option<StyleValueData> {
    let (
        StyleValueData::BasicShape {
            kind: from_kind,
            v0: from_v0,
            v1: from_v1,
            v2: from_v2,
            v3: from_v3,
            v4: from_v4,
            fill_rule: from_fill_rule,
            points: from_points,
            ..
        },
        StyleValueData::BasicShape {
            kind: to_kind,
            v0: to_v0,
            v1: to_v1,
            v2: to_v2,
            v3: to_v3,
            v4: to_v4,
            fill_rule: to_fill_rule,
            points: to_points,
            ..
        },
    ) = (from, to)
    else {
        return None;
    };

    // https://drafts.csswg.org/css-shapes-1/#basic-shape-interpolation
    if from_kind != to_kind {
        return None;
    }

    let empty = empty_retained_style_value;
    match *from_kind {
        BASIC_SHAPE_INSET => {
            // If both shapes are of type inset(), interpolate between each value in the shape functions.
            Some(StyleValueData::BasicShape {
                kind: *from_kind,
                v0: interpolate_basic_shape_component(property_id, from_v0, to_v0, delta),
                v1: interpolate_basic_shape_component(property_id, from_v1, to_v1, delta),
                v2: interpolate_basic_shape_component(property_id, from_v2, to_v2, delta),
                v3: interpolate_basic_shape_component(property_id, from_v3, to_v3, delta),
                v4: interpolate_basic_shape_component(property_id, from_v4, to_v4, delta),
                fill_rule: 0,
                points: empty_shape_points(),
                path_string: empty_retained_fly_string(),
            })
        }
        BASIC_SHAPE_CIRCLE | BASIC_SHAPE_ELLIPSE => {
            // If both shapes are the same type, that type is ellipse() or circle(), and the radiuses are specified
            // as <length-percentage> (rather than keywords), interpolate between each value in the shape functions.
            let radius = retained_animation_result(interpolate_scalar_value(
                property_id,
                from_v0.data(),
                to_v0.data(),
                delta,
                &[],
            ))?;
            let position = match (from_v1.optional_data(), to_v1.optional_data()) {
                (None, None) => empty(),
                _ => {
                    let from_default;
                    let to_default;
                    let from = if from_v1.optional_data().is_some() {
                        from_v1
                    } else {
                        from_default = retained_computed_center_position();
                        &from_default
                    };
                    let to = if to_v1.optional_data().is_some() {
                        to_v1
                    } else {
                        to_default = retained_computed_center_position();
                        &to_default
                    };
                    retained_animation_result(interpolate_scalar_value(
                        property_id,
                        from.data(),
                        to.data(),
                        delta,
                        &[],
                    ))?
                }
            };
            Some(StyleValueData::BasicShape {
                kind: *from_kind,
                v0: radius,
                v1: position,
                v2: empty(),
                v3: empty(),
                v4: empty(),
                fill_rule: 0,
                points: empty_shape_points(),
                path_string: empty_retained_fly_string(),
            })
        }
        BASIC_SHAPE_POLYGON => {
            // If both shapes are of type polygon(), both polygons have the same number of vertices, and use the
            // same <'fill-rule'>, interpolate between each value in the shape functions.
            if from_fill_rule != to_fill_rule || from_points.as_slice().len() != to_points.as_slice().len() {
                return None;
            }
            let points = from_points
                .as_slice()
                .iter()
                .zip(to_points.as_slice())
                .map(|(from, to)| {
                    let [from_x, from_y] = from.values();
                    let [to_x, to_y] = to.values();
                    RetainedShapePoint::from_retained_values(
                        interpolate_basic_shape_component(property_id, from_x, to_x, delta),
                        interpolate_basic_shape_component(property_id, from_y, to_y, delta),
                    )
                })
                .collect();
            Some(StyleValueData::BasicShape {
                kind: *from_kind,
                v0: empty(),
                v1: empty(),
                v2: empty(),
                v3: empty(),
                v4: empty(),
                fill_rule: *from_fill_rule,
                points: RetainedShapePointList::from_retained_points(points),
                path_string: empty_retained_fly_string(),
            })
        }
        _ => None,
    }
}

fn composite_retained_value(
    underlying: &RetainedStyleValueData,
    animated: &RetainedStyleValueData,
    operation: FfiCompositeOperation,
) -> Option<RetainedStyleValueData> {
    retained_animation_result(composite_scalar_value(underlying.data(), animated.data(), operation))
}

fn composite_radial_size(
    underlying: RadialSizeComponents<'_>,
    animated: RadialSizeComponents<'_>,
    operation: FfiCompositeOperation,
) -> Option<StyleValueData> {
    // https://drafts.csswg.org/css-images-4/#interpolating-gradients
    // https://drafts.csswg.org/css-shapes-1/#basic-shape-interpolation
    // FIXME: Radial extents should disallow composition for basic-shape values but should be converted into their
    //        equivalent length-percentage values for radial gradients
    if underlying.is_extent_0
        || (underlying.component_count == 2 && underlying.is_extent_1)
        || animated.is_extent_0
        || (animated.component_count == 2 && animated.is_extent_1)
    {
        return None;
    }

    let composite = |underlying: &RetainedStyleValueData, animated: &RetainedStyleValueData| {
        composite_retained_value(underlying, animated, operation)
    };
    if underlying.component_count == 1 && animated.component_count == 1 {
        return Some(StyleValueData::RadialSize {
            component_count: 1,
            is_extent_0: false,
            extent_0: 0,
            value_0: composite(underlying.value_0, animated.value_0)?,
            is_extent_1: false,
            extent_1: 0,
            value_1: empty_retained_style_value(),
        });
    }

    let underlying_vertical = if underlying.component_count == 2 {
        underlying.value_1
    } else {
        underlying.value_0
    };
    let animated_vertical = if animated.component_count == 2 {
        animated.value_1
    } else {
        animated.value_0
    };
    Some(StyleValueData::RadialSize {
        component_count: 2,
        is_extent_0: false,
        extent_0: 0,
        value_0: composite(underlying.value_0, animated.value_0)?,
        is_extent_1: false,
        extent_1: 0,
        value_1: composite(underlying_vertical, animated_vertical)?,
    })
}

fn composite_basic_shape(
    underlying: &StyleValueData,
    animated: &StyleValueData,
    operation: FfiCompositeOperation,
) -> Option<StyleValueData> {
    let (
        StyleValueData::BasicShape {
            kind: underlying_kind,
            v0: underlying_v0,
            v1: underlying_v1,
            v2: underlying_v2,
            v3: underlying_v3,
            v4: underlying_v4,
            fill_rule: underlying_fill_rule,
            points: underlying_points,
            ..
        },
        StyleValueData::BasicShape {
            kind: animated_kind,
            v0: animated_v0,
            v1: animated_v1,
            v2: animated_v2,
            v3: animated_v3,
            v4: animated_v4,
            fill_rule: animated_fill_rule,
            points: animated_points,
            ..
        },
    ) = (underlying, animated)
    else {
        return None;
    };
    if underlying_kind != animated_kind {
        return None;
    }

    let empty = empty_retained_style_value;
    match *underlying_kind {
        BASIC_SHAPE_INSET => Some(StyleValueData::BasicShape {
            kind: *underlying_kind,
            v0: composite_retained_value(underlying_v0, animated_v0, operation)?,
            v1: composite_retained_value(underlying_v1, animated_v1, operation)?,
            v2: composite_retained_value(underlying_v2, animated_v2, operation)?,
            v3: composite_retained_value(underlying_v3, animated_v3, operation)?,
            v4: composite_retained_value(underlying_v4, animated_v4, operation)?,
            fill_rule: 0,
            points: empty_shape_points(),
            path_string: empty_retained_fly_string(),
        }),
        BASIC_SHAPE_CIRCLE | BASIC_SHAPE_ELLIPSE => {
            let position = match (underlying_v1.optional_data(), animated_v1.optional_data()) {
                (None, None) => empty(),
                _ => {
                    let underlying_default;
                    let animated_default;
                    let underlying = if underlying_v1.optional_data().is_some() {
                        underlying_v1
                    } else {
                        underlying_default = retained_computed_center_position();
                        &underlying_default
                    };
                    let animated = if animated_v1.optional_data().is_some() {
                        animated_v1
                    } else {
                        animated_default = retained_computed_center_position();
                        &animated_default
                    };
                    composite_retained_value(underlying, animated, operation)?
                }
            };
            Some(StyleValueData::BasicShape {
                kind: *underlying_kind,
                v0: composite_retained_value(underlying_v0, animated_v0, operation)?,
                v1: position,
                v2: empty(),
                v3: empty(),
                v4: empty(),
                fill_rule: 0,
                points: empty_shape_points(),
                path_string: empty_retained_fly_string(),
            })
        }
        BASIC_SHAPE_POLYGON => {
            if underlying_fill_rule != animated_fill_rule
                || underlying_points.as_slice().len() != animated_points.as_slice().len()
            {
                return None;
            }
            let points = underlying_points
                .as_slice()
                .iter()
                .zip(animated_points.as_slice())
                .map(|(underlying, animated)| {
                    let [underlying_x, underlying_y] = underlying.values();
                    let [animated_x, animated_y] = animated.values();
                    Some(RetainedShapePoint::from_retained_values(
                        composite_retained_value(underlying_x, animated_x, operation)?,
                        composite_retained_value(underlying_y, animated_y, operation)?,
                    ))
                })
                .collect::<Option<Vec<_>>>()?;
            Some(StyleValueData::BasicShape {
                kind: *underlying_kind,
                v0: empty(),
                v1: empty(),
                v2: empty(),
                v3: empty(),
                v4: empty(),
                fill_rule: *underlying_fill_rule,
                points: RetainedShapePointList::from_retained_points(points),
                path_string: empty_retained_fly_string(),
            })
        }
        _ => None,
    }
}

fn is_individual_transform_property(property: u16) -> bool {
    matches!(
        property,
        crate::css::property_metadata::property_id::ROTATE
            | crate::css::property_metadata::property_id::SCALE
            | crate::css::property_metadata::property_id::TRANSLATE
    )
}

// Decode a computed rotate property value into an unnormalized axis and an angle in degrees.
fn decode_rotation(value: &StyleValueData) -> Option<([f64; 3], f64)> {
    if matches!(value, StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword()) {
        return Some(([0.0, 0.0, 1.0], 0.0));
    }
    let StyleValueData::Transformation {
        transform_function,
        values,
        ..
    } = value
    else {
        return None;
    };
    match (*transform_function, values.as_slice()) {
        (TRANSFORM_FUNCTION_ROTATE | TRANSFORM_FUNCTION_ROTATE_Z, [angle]) => {
            Some(([0.0, 0.0, 1.0], resolve_animation_angle(None, angle.data())?))
        }
        (TRANSFORM_FUNCTION_ROTATE_X, [angle]) => Some(([1.0, 0.0, 0.0], resolve_animation_angle(None, angle.data())?)),
        (TRANSFORM_FUNCTION_ROTATE_Y, [angle]) => Some(([0.0, 1.0, 0.0], resolve_animation_angle(None, angle.data())?)),
        (TRANSFORM_FUNCTION_ROTATE_3D, [x, y, z, angle]) => Some((
            [
                resolve_animation_number(None, x.data())?,
                resolve_animation_number(None, y.data())?,
                resolve_animation_number(None, z.data())?,
            ],
            resolve_animation_angle(None, angle.data())?,
        )),
        _ => None,
    }
}

// Find a shared rotation axis for two rotations, treating a rotation with a zero axis or a zero
// angle as contributing no rotation. Returns the shared normalized axis with both angles, or None
// when the axes genuinely differ.
fn common_rotation_axis(
    underlying_axis: [f64; 3],
    underlying_angle: f64,
    animated_axis: [f64; 3],
    animated_angle: f64,
) -> Option<([f64; 3], f64, f64)> {
    let epsilon = 1e-4;
    let length_squared = |axis: [f64; 3]| axis.iter().map(|component| component * component).sum::<f64>();
    let normalize = |axis: [f64; 3]| {
        let length = length_squared(axis).sqrt();
        [axis[0] / length, axis[1] / length, axis[2] / length]
    };
    let underlying_axis_is_zero = length_squared(underlying_axis) < epsilon * epsilon;
    let animated_axis_is_zero = length_squared(animated_axis) < epsilon * epsilon;
    let (underlying_is_zero, animated_is_zero) = if underlying_axis_is_zero || animated_axis_is_zero {
        (underlying_axis_is_zero, animated_axis_is_zero)
    } else {
        (underlying_angle.abs() < epsilon, animated_angle.abs() < epsilon)
    };
    if underlying_is_zero && animated_is_zero {
        return Some(([0.0, 0.0, 1.0], 0.0, 0.0));
    }
    if underlying_is_zero {
        return Some((normalize(animated_axis), 0.0, animated_angle));
    }
    if animated_is_zero {
        return Some((normalize(underlying_axis), underlying_angle, 0.0));
    }
    let dot = underlying_axis[0] * animated_axis[0]
        + underlying_axis[1] * animated_axis[1]
        + underlying_axis[2] * animated_axis[2];
    if dot < 0.0 {
        return None;
    }
    let error = (1.0 - (dot * dot) / (length_squared(underlying_axis) * length_squared(animated_axis))).abs();
    if error > epsilon {
        return None;
    }
    Some((normalize(underlying_axis), underlying_angle, animated_angle))
}

fn individual_rotation_value(axis: [f64; 3], angle_degrees: f64) -> StyleValueData {
    let angle = Arc::into_raw(Arc::new(StyleValueData::Angle {
        value: angle_degrees,
        unit: 0,
    }));
    let angle = unsafe { RetainedStyleValueData::from_retained_pointer(angle) };
    if axis == [0.0, 0.0, 1.0] {
        return StyleValueData::Transformation {
            property: crate::css::property_metadata::property_id::ROTATE,
            transform_function: TRANSFORM_FUNCTION_ROTATE,
            values: RetainedStyleValueDataList::from_retained_values(vec![angle]),
        };
    }
    StyleValueData::Transformation {
        property: crate::css::property_metadata::property_id::ROTATE,
        transform_function: TRANSFORM_FUNCTION_ROTATE_3D,
        values: RetainedStyleValueDataList::from_retained_values(vec![
            retained_number(axis[0]),
            retained_number(axis[1]),
            retained_number(axis[2]),
            angle,
        ]),
    }
}

fn composite_rotate(underlying: &StyleValueData, animated: &StyleValueData) -> Option<StyleValueData> {
    let (underlying_axis, underlying_angle) = decode_rotation(underlying)?;
    let (animated_axis, animated_angle) = decode_rotation(animated)?;

    if let Some((axis, underlying_angle, animated_angle)) =
        common_rotation_axis(underlying_axis, underlying_angle, animated_axis, animated_angle)
    {
        return Some(individual_rotation_value(axis, underlying_angle + animated_angle));
    }

    let to_quaternion = |axis: [f64; 3], angle_degrees: f64| {
        let length = axis.iter().map(|component| component * component).sum::<f64>().sqrt();
        let half_angle = angle_degrees.to_radians() / 2.0;
        let sin_half_angle = half_angle.sin();
        [
            axis[0] / length * sin_half_angle,
            axis[1] / length * sin_half_angle,
            axis[2] / length * sin_half_angle,
            half_angle.cos(),
        ]
    };
    let [x1, y1, z1, w1] = to_quaternion(underlying_axis, underlying_angle);
    let [x2, y2, z2, w2] = to_quaternion(animated_axis, animated_angle);

    let mut product = [
        w1 * x2 + x1 * w2 + y1 * z2 - z1 * y2,
        w1 * y2 - x1 * z2 + y1 * w2 + z1 * x2,
        w1 * z2 + x1 * y2 - y1 * x2 + z1 * w2,
        w1 * w2 - x1 * x2 - y1 * y2 - z1 * z2,
    ];
    if product[3] < 0.0 {
        product = product.map(|component| -component);
    }
    let angle = 2.0 * product[3].clamp(-1.0, 1.0).acos();
    let sin_half_angle = (1.0 - product[3] * product[3]).max(0.0).sqrt();
    let axis = if sin_half_angle < 1e-5 {
        [0.0, 0.0, 1.0]
    } else {
        [
            product[0] / sin_half_angle,
            product[1] / sin_half_angle,
            product[2] / sin_half_angle,
        ]
    };
    Some(individual_rotation_value(axis, angle.to_degrees()))
}

fn composite_scale(
    underlying: &StyleValueData,
    animated: &StyleValueData,
    operation: FfiCompositeOperation,
) -> Option<StyleValueData> {
    let (Some(mut underlying), Some(mut animated)) =
        (decode_scale_components(underlying), decode_scale_components(animated))
    else {
        return None;
    };
    let is_3d = underlying.len() == 3 || animated.len() == 3;
    if is_3d {
        underlying.resize(3, 1.0);
        animated.resize(3, 1.0);
    }
    let values = underlying
        .into_iter()
        .zip(animated)
        .map(|(underlying, animated)| {
            let value = match operation {
                FfiCompositeOperation::Accumulate => underlying + animated - 1.0,
                _ => underlying * animated,
            };
            retained_number(value)
        })
        .collect();
    Some(StyleValueData::Transformation {
        property: crate::css::property_metadata::property_id::SCALE,
        transform_function: if is_3d {
            TRANSFORM_FUNCTION_SCALE_3D
        } else {
            TRANSFORM_FUNCTION_SCALE
        },
        values: RetainedStyleValueDataList::from_retained_values(values),
    })
}

fn composite_translate(
    underlying: &StyleValueData,
    animated: &StyleValueData,
    operation: FfiCompositeOperation,
) -> Option<StyleValueData> {
    let (Some(mut underlying), Some(mut animated)) = (
        decode_translate_components(underlying),
        decode_translate_components(animated),
    ) else {
        return None;
    };
    let is_3d = underlying.len() == 3 || animated.len() == 3;
    if is_3d {
        underlying.resize_with(3, retained_zero_px);
        animated.resize_with(3, retained_zero_px);
    }
    let values = underlying
        .iter()
        .zip(animated.iter())
        .map(|(underlying, animated)| {
            let result = composite_scalar_value(underlying.data(), animated.data(), operation);
            if !result.handled || result.value.is_null() {
                return None;
            }
            Some(unsafe { RetainedStyleValueData::from_retained_pointer(result.value) })
        })
        .collect::<Option<Vec<_>>>()?;
    Some(StyleValueData::Transformation {
        property: crate::css::property_metadata::property_id::TRANSLATE,
        transform_function: if is_3d {
            TRANSFORM_FUNCTION_TRANSLATE_3D
        } else {
            TRANSFORM_FUNCTION_TRANSLATE
        },
        values: RetainedStyleValueDataList::from_retained_values(values),
    })
}

fn composite_individual_transform(
    property: u16,
    underlying: &StyleValueData,
    animated: &StyleValueData,
    operation: FfiCompositeOperation,
) -> Option<StyleValueData> {
    if property == crate::css::property_metadata::property_id::ROTATE {
        return composite_rotate(underlying, animated);
    }
    if property == crate::css::property_metadata::property_id::SCALE {
        return composite_scale(underlying, animated, operation);
    }
    if property == crate::css::property_metadata::property_id::TRANSLATE {
        return composite_translate(underlying, animated, operation);
    }
    None
}

fn composite_scalar_value(
    underlying: &StyleValueData,
    animated: &StyleValueData,
    operation: FfiCompositeOperation,
) -> FfiAnimationValueResult {
    if matches!(operation, FfiCompositeOperation::Replace) {
        return handled_without_value();
    }

    if let Some(context) = animation_calculation_context(underlying).or_else(|| animation_calculation_context(animated))
        && let (Some(underlying), Some(animated)) =
            (numeric_calculation_node(underlying), numeric_calculation_node(animated))
        && let Some((calculation, resolved_type)) = crate::css::calc::add_calculations(
            underlying,
            animated,
            context.has_percentages_resolve_as,
            context.resolve_as_is_number,
            context.resolve_as_base,
        )
    {
        // https://drafts.csswg.org/css-values-4/#combine-math
        // Addition of math functions, with each other or with numeric values and other numeric-valued functions, is defined as Vresult = calc(VA + VB).
        return handled_retained_value(retained_calculation(calculation, resolved_type, &context));
    }

    if (matches!(underlying, StyleValueData::Calculated { .. })
        || matches!(animated, StyleValueData::Calculated { .. })
        || std::mem::discriminant(underlying) != std::mem::discriminant(animated))
        && let (Some(underlying), Some(animated)) = (
            length_percentage_calculation_node(underlying),
            length_percentage_calculation_node(animated),
        )
        && let Some((calculation, resolved_type)) =
            crate::css::calc::add_length_percentage_calculations(underlying, animated)
    {
        // https://drafts.csswg.org/css-values-4/#combine-mixed
        // Addition of <percentage> is defined the same as interpolation except by adding each component rather than interpolating it.
        return handled_retained_value(retained_length_percentage_calculation(calculation, resolved_type));
    }

    match (underlying, animated) {
        (StyleValueData::ColorFunction { .. }, StyleValueData::ColorFunction { .. }) => {
            // AD-HOC: css-values-4 states that the <color> type is not additive, but other engines
            // combine colors by adding their premultiplied components in the interpolation color space,
            // and WPT css/css-color/animation/color-composition.html expects that behavior.
            combine_legacy_rgb(underlying, animated, ColorCombination::Add)
                .or_else(|| combine_modern_color(underlying, animated, ColorCombination::Add))
                .map_or_else(handled_without_value, owned)
        }
        (StyleValueData::BasicShape { .. }, StyleValueData::BasicShape { .. }) => {
            composite_basic_shape(underlying, animated, operation).map_or_else(handled_without_value, owned)
        }
        (
            StyleValueData::RadialSize {
                component_count: underlying_component_count,
                is_extent_0: underlying_is_extent_0,
                value_0: underlying_value_0,
                is_extent_1: underlying_is_extent_1,
                value_1: underlying_value_1,
                ..
            },
            StyleValueData::RadialSize {
                component_count: animated_component_count,
                is_extent_0: animated_is_extent_0,
                value_0: animated_value_0,
                is_extent_1: animated_is_extent_1,
                value_1: animated_value_1,
                ..
            },
        ) => composite_radial_size(
            RadialSizeComponents {
                component_count: *underlying_component_count,
                is_extent_0: *underlying_is_extent_0,
                value_0: underlying_value_0,
                is_extent_1: *underlying_is_extent_1,
                value_1: underlying_value_1,
            },
            RadialSizeComponents {
                component_count: *animated_component_count,
                is_extent_0: *animated_is_extent_0,
                value_0: animated_value_0,
                is_extent_1: *animated_is_extent_1,
                value_1: animated_value_1,
            },
            operation,
        )
        .map_or_else(handled_without_value, owned),
        (StyleValueData::Number { value: underlying }, StyleValueData::Number { value: animated }) => {
            // https://drafts.csswg.org/css-values-4/#combine-numbers
            // Addition of <number> is defined as Vresult = VA + VB.
            owned(StyleValueData::Number {
                value: underlying + animated,
            })
        }
        (StyleValueData::Integer { value: underlying }, StyleValueData::Integer { value: animated }) => {
            // https://drafts.csswg.org/css-values-4/#combine-integers
            // Addition of <integer> is defined as Vresult = VA + VB.
            owned(StyleValueData::Integer {
                value: underlying.saturating_add(*animated),
            })
        }
        (
            StyleValueData::Angle {
                value: underlying,
                unit: underlying_unit,
            },
            StyleValueData::Angle {
                value: animated,
                unit: animated_unit,
            },
        ) if underlying_unit == animated_unit => {
            // https://drafts.csswg.org/css-values-4/#combine-dimensions
            // Addition of compatible dimensions is defined as Vresult = VA + VB.
            owned(StyleValueData::Angle {
                value: underlying + animated,
                unit: *underlying_unit,
            })
        }
        (
            StyleValueData::Flex {
                value: underlying,
                unit: underlying_unit,
            },
            StyleValueData::Flex {
                value: animated,
                unit: animated_unit,
            },
        ) if underlying_unit == animated_unit => owned(StyleValueData::Flex {
            value: underlying + animated,
            unit: *underlying_unit,
        }),
        (
            StyleValueData::Frequency {
                value: underlying,
                unit: underlying_unit,
            },
            StyleValueData::Frequency {
                value: animated,
                unit: animated_unit,
            },
        ) if underlying_unit == animated_unit => owned(StyleValueData::Frequency {
            value: underlying + animated,
            unit: *underlying_unit,
        }),
        (
            StyleValueData::Length {
                value: underlying,
                unit: underlying_unit,
            },
            StyleValueData::Length {
                value: animated,
                unit: animated_unit,
            },
        ) if underlying_unit == animated_unit => owned(StyleValueData::Length {
            value: underlying + animated,
            unit: *underlying_unit,
        }),
        (StyleValueData::Percentage { value: underlying }, StyleValueData::Percentage { value: animated }) => {
            // https://drafts.csswg.org/css-values-4/#combine-mixed
            // Addition of <percentage> is defined the same as interpolation except by adding each component rather than interpolating it.
            owned(StyleValueData::Percentage {
                value: underlying + animated,
            })
        }
        (
            StyleValueData::Resolution {
                value: underlying,
                unit: underlying_unit,
            },
            StyleValueData::Resolution {
                value: animated,
                unit: animated_unit,
            },
        ) if underlying_unit == animated_unit => owned(StyleValueData::Resolution {
            value: underlying + animated,
            unit: *underlying_unit,
        }),
        (
            StyleValueData::Time {
                value: underlying,
                unit: underlying_unit,
            },
            StyleValueData::Time {
                value: animated,
                unit: animated_unit,
            },
        ) if underlying_unit == animated_unit => owned(StyleValueData::Time {
            value: underlying + animated,
            unit: *underlying_unit,
        }),
        (StyleValueData::OpacityValue { value: underlying }, StyleValueData::OpacityValue { value: animated }) => {
            let (StyleValueData::Number { value: underlying }, StyleValueData::Number { value: animated }) =
                (underlying.data(), animated.data())
            else {
                return handled_without_value();
            };

            // https://drafts.csswg.org/css-color-4/#propdef-opacity
            // Computed value: specified number, clamped to the range [0,1]
            let number = Arc::into_raw(Arc::new(StyleValueData::Number {
                value: (underlying + animated).clamp(0.0, 1.0),
            }));
            owned(StyleValueData::OpacityValue {
                value: unsafe { RetainedStyleValueData::from_retained_pointer(number) },
            })
        }
        (
            StyleValueData::BackgroundSize {
                size_x: underlying_x,
                size_y: underlying_y,
            },
            StyleValueData::BackgroundSize {
                size_x: animated_x,
                size_y: animated_y,
            },
        ) => {
            let x = composite_scalar_value(underlying_x.data(), animated_x.data(), operation);
            let y = composite_scalar_value(underlying_y.data(), animated_y.data(), operation);
            if !x.handled || !y.handled {
                return handled_without_value();
            }
            if x.value.is_null() || y.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::BackgroundSize {
                size_x: unsafe { RetainedStyleValueData::from_retained_pointer(x.value) },
                size_y: unsafe { RetainedStyleValueData::from_retained_pointer(y.value) },
            })
        }
        (StyleValueData::Edge { offset: underlying, .. }, StyleValueData::Edge { offset: animated, .. }) => {
            let (Some(underlying), Some(animated)) = (underlying.optional_data(), animated.optional_data()) else {
                return handled_without_value();
            };
            let result = composite_scalar_value(underlying, animated, operation);
            if !result.handled {
                return handled_without_value();
            }
            if result.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::Edge {
                has_edge: false,
                edge: 0,
                offset: unsafe { RetainedStyleValueData::from_retained_pointer(result.value) },
            })
        }
        (
            StyleValueData::Position {
                edge_x: underlying_x,
                edge_y: underlying_y,
            },
            StyleValueData::Position {
                edge_x: animated_x,
                edge_y: animated_y,
            },
        ) => {
            let x = composite_scalar_value(underlying_x.data(), animated_x.data(), operation);
            let y = composite_scalar_value(underlying_y.data(), animated_y.data(), operation);
            if !x.handled || !y.handled {
                return handled_without_value();
            }
            if x.value.is_null() || y.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::Position {
                edge_x: unsafe { RetainedStyleValueData::from_retained_pointer(x.value) },
                edge_y: unsafe { RetainedStyleValueData::from_retained_pointer(y.value) },
            })
        }
        (
            StyleValueData::Rect {
                top: underlying_top,
                right: underlying_right,
                bottom: underlying_bottom,
                left: underlying_left,
            },
            StyleValueData::Rect {
                top: animated_top,
                right: animated_right,
                bottom: animated_bottom,
                left: animated_left,
            },
        ) => {
            let combine = |underlying: &RetainedStyleValueData, animated: &RetainedStyleValueData| {
                let result = composite_scalar_value(underlying.data(), animated.data(), operation);
                if !result.handled {
                    return Err(());
                }
                if result.value.is_null() {
                    return Ok(None);
                }
                Ok(Some(unsafe {
                    RetainedStyleValueData::from_retained_pointer(result.value)
                }))
            };
            let value = (|| {
                let Some(top) = combine(underlying_top, animated_top)? else {
                    return Ok(None);
                };
                let Some(right) = combine(underlying_right, animated_right)? else {
                    return Ok(None);
                };
                let Some(bottom) = combine(underlying_bottom, animated_bottom)? else {
                    return Ok(None);
                };
                let Some(left) = combine(underlying_left, animated_left)? else {
                    return Ok(None);
                };
                Ok(Some(StyleValueData::Rect {
                    top,
                    right,
                    bottom,
                    left,
                }))
            })();
            match value {
                Err(()) => handled_without_value(),
                Ok(None) => handled_without_value(),
                Ok(Some(value)) => owned(value),
            }
        }
        (
            StyleValueData::BorderRadius {
                horizontal_radius: underlying_horizontal,
                vertical_radius: underlying_vertical,
                ..
            },
            StyleValueData::BorderRadius {
                horizontal_radius: animated_horizontal,
                vertical_radius: animated_vertical,
                ..
            },
        ) => {
            let horizontal =
                composite_scalar_value(underlying_horizontal.data(), animated_horizontal.data(), operation);
            if !horizontal.handled {
                return handled_without_value();
            }
            if horizontal.value.is_null() {
                return handled_without_value();
            }
            let horizontal = unsafe { RetainedStyleValueData::from_retained_pointer(horizontal.value) };
            let vertical = composite_scalar_value(underlying_vertical.data(), animated_vertical.data(), operation);
            if !vertical.handled {
                return handled_without_value();
            }
            if vertical.value.is_null() {
                return handled_without_value();
            }
            let vertical = unsafe { RetainedStyleValueData::from_retained_pointer(vertical.value) };
            owned(StyleValueData::BorderRadius {
                is_elliptical: !radius_components_equal(horizontal.data(), vertical.data()),
                horizontal_radius: horizontal,
                vertical_radius: vertical,
            })
        }
        (
            StyleValueData::BorderRadiusRect {
                top_left: underlying_top_left,
                top_right: underlying_top_right,
                bottom_right: underlying_bottom_right,
                bottom_left: underlying_bottom_left,
            },
            StyleValueData::BorderRadiusRect {
                top_left: animated_top_left,
                top_right: animated_top_right,
                bottom_right: animated_bottom_right,
                bottom_left: animated_bottom_left,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            let combine = |underlying: &RetainedStyleValueData, animated: &RetainedStyleValueData| {
                let result = composite_scalar_value(underlying.data(), animated.data(), operation);
                if !result.handled {
                    return Err(());
                }
                if result.value.is_null() {
                    return Ok(None);
                }
                Ok(Some(unsafe {
                    RetainedStyleValueData::from_retained_pointer(result.value)
                }))
            };
            let value = (|| {
                let Some(top_left) = combine(underlying_top_left, animated_top_left)? else {
                    return Ok(None);
                };
                let Some(top_right) = combine(underlying_top_right, animated_top_right)? else {
                    return Ok(None);
                };
                let Some(bottom_right) = combine(underlying_bottom_right, animated_bottom_right)? else {
                    return Ok(None);
                };
                let Some(bottom_left) = combine(underlying_bottom_left, animated_bottom_left)? else {
                    return Ok(None);
                };
                Ok(Some(StyleValueData::BorderRadiusRect {
                    top_left,
                    top_right,
                    bottom_right,
                    bottom_left,
                }))
            })();
            match value {
                Err(()) => handled_without_value(),
                Ok(None) => handled_without_value(),
                Ok(Some(value)) => owned(value),
            }
        }
        (
            StyleValueData::BorderImageSlice {
                top: underlying_top,
                right: underlying_right,
                bottom: underlying_bottom,
                left: underlying_left,
                fill: underlying_fill,
            },
            StyleValueData::BorderImageSlice {
                top: animated_top,
                right: animated_right,
                bottom: animated_bottom,
                left: animated_left,
                fill: animated_fill,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if underlying_fill != animated_fill {
                return handled_without_value();
            }
            let combine = |underlying: &RetainedStyleValueData, animated: &RetainedStyleValueData| {
                let result = composite_scalar_value(underlying.data(), animated.data(), operation);
                if !result.handled {
                    return Err(());
                }
                if result.value.is_null() {
                    return Ok(None);
                }
                Ok(Some(unsafe {
                    RetainedStyleValueData::from_retained_pointer(result.value)
                }))
            };
            let value = (|| {
                let Some(top) = combine(underlying_top, animated_top)? else {
                    return Ok(None);
                };
                let Some(right) = combine(underlying_right, animated_right)? else {
                    return Ok(None);
                };
                let Some(bottom) = combine(underlying_bottom, animated_bottom)? else {
                    return Ok(None);
                };
                let Some(left) = combine(underlying_left, animated_left)? else {
                    return Ok(None);
                };
                Ok(Some(StyleValueData::BorderImageSlice {
                    top,
                    right,
                    bottom,
                    left,
                    fill: *underlying_fill,
                }))
            })();
            match value {
                Err(()) => handled_without_value(),
                Ok(None) => handled_without_value(),
                Ok(Some(value)) => owned(value),
            }
        }
        (
            StyleValueData::OpenTypeTagged {
                tag: underlying_tag,
                packed_tag: underlying_packed_tag,
                value: underlying_value,
                ..
            },
            StyleValueData::OpenTypeTagged {
                tag: animated_tag,
                value: animated_value,
                ..
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if underlying_tag.raw() != animated_tag.raw() {
                return handled_without_value();
            }
            let value = composite_scalar_value(underlying_value.data(), animated_value.data(), operation);
            if !value.handled {
                return handled_without_value();
            }
            if value.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::OpenTypeTagged {
                mode: OPEN_TYPE_MODE_FONT_VARIATION_SETTINGS,
                tag: underlying_tag.clone(),
                packed_tag: *underlying_packed_tag,
                value: unsafe { RetainedStyleValueData::from_retained_pointer(value.value) },
            })
        }
        (
            StyleValueData::Function {
                name: underlying_name,
                value: underlying_value,
            },
            StyleValueData::Function {
                name: animated_name,
                value: animated_value,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if underlying_name.raw() != animated_name.raw() {
                return handled_without_value();
            }
            let value = composite_scalar_value(underlying_value.data(), animated_value.data(), operation);
            if !value.handled {
                return handled_without_value();
            }
            if value.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::Function {
                name: underlying_name.clone(),
                value: unsafe { RetainedStyleValueData::from_retained_pointer(value.value) },
            })
        }
        (
            StyleValueData::TextIndent {
                length_percentage: underlying,
                hanging: underlying_hanging,
                each_line: underlying_each_line,
            },
            StyleValueData::TextIndent {
                length_percentage: animated,
                hanging: animated_hanging,
                each_line: animated_each_line,
            },
        ) => {
            if underlying_hanging != animated_hanging || underlying_each_line != animated_each_line {
                return handled_without_value();
            }
            let result = composite_scalar_value(underlying.data(), animated.data(), operation);
            if !result.handled {
                return handled_without_value();
            }
            if result.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::TextIndent {
                length_percentage: unsafe { RetainedStyleValueData::from_retained_pointer(result.value) },
                hanging: *underlying_hanging,
                each_line: *underlying_each_line,
            })
        }
        (StyleValueData::Ratio { .. }, StyleValueData::Ratio { .. }) => {
            // https://drafts.csswg.org/css-values-4/#combine-ratio
            // Addition of <ratio>s is not possible.
            handled_without_value()
        }
        (
            StyleValueData::ValueList {
                values: underlying_values,
                separator: underlying_separator,
                ..
            },
            StyleValueData::ValueList {
                values: animated_values,
                separator: animated_separator,
                collapsible,
            },
        ) => {
            if underlying_values.as_slice().len() != animated_values.as_slice().len()
                || underlying_separator != animated_separator
            {
                return handled_without_value();
            }

            let mut values = Vec::with_capacity(underlying_values.as_slice().len());
            for (underlying, animated) in underlying_values.as_slice().iter().zip(animated_values.as_slice()) {
                let result = composite_scalar_value(underlying.data(), animated.data(), operation);
                if !result.handled {
                    return handled_without_value();
                }
                if result.value.is_null() {
                    return handled_without_value();
                }
                values.push(unsafe { RetainedStyleValueData::from_retained_pointer(result.value) });
            }
            owned(StyleValueData::ValueList {
                values: RetainedStyleValueDataList::from_retained_values(values),
                separator: *underlying_separator,
                collapsible: *collapsible,
            })
        }
        (
            StyleValueData::GridTrackSizeList {
                is_subgrid: underlying_is_subgrid,
                entries: underlying_entries,
                ..
            },
            StyleValueData::GridTrackSizeList {
                is_subgrid: animated_is_subgrid,
                entries: animated_entries,
                ..
            },
        ) => {
            let Some(entries) = composite_grid_track_entries(
                *underlying_is_subgrid,
                underlying_entries.as_slice(),
                *animated_is_subgrid,
                animated_entries.as_slice(),
                operation,
            ) else {
                return handled_without_value();
            };
            owned(StyleValueData::GridTrackSizeList {
                is_subgrid: false,
                preserve_line_name_sets: false,
                entries: RetainedGridTrackEntryList::from_retained_entries(entries),
            })
        }
        (
            StyleValueData::Transformation {
                property: underlying_property,
                ..
            },
            StyleValueData::Transformation {
                property: animated_property,
                ..
            },
        ) if underlying_property == animated_property && is_individual_transform_property(*underlying_property) => {
            composite_individual_transform(*underlying_property, underlying, animated, operation)
                .map_or_else(handled_without_value, owned)
        }
        // none is the identity value of the individual transform properties, so composing onto or
        // with it yields the other value unchanged.
        (StyleValueData::Keyword { keyword }, StyleValueData::Transformation { property, .. })
            if *keyword == crate::css::style_compute::none_keyword() && is_individual_transform_property(*property) =>
        {
            FfiAnimationValueResult {
                value: unsafe { crate::css::style_value::retain_style_value(animated) },
                handled: true,
            }
        }
        (StyleValueData::Transformation { property, .. }, StyleValueData::Keyword { keyword })
            if *keyword == crate::css::style_compute::none_keyword() && is_individual_transform_property(*property) =>
        {
            FfiAnimationValueResult {
                value: unsafe { crate::css::style_value::retain_style_value(underlying) },
                handled: true,
            }
        }
        _ => handled_without_value(),
    }
}

fn interpolate_scalar_value(
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
    range_overrides: &[NumericRangeOverride],
) -> FfiAnimationValueResult {
    if let Some(context) = animation_calculation_context(from).or_else(|| animation_calculation_context(to))
        && let (Some(from), Some(to)) = (numeric_calculation_node(from), numeric_calculation_node(to))
        && let Some((calculation, resolved_type)) = crate::css::calc::interpolate_calculations(
            from,
            to,
            delta,
            context.has_percentages_resolve_as,
            context.resolve_as_is_number,
            context.resolve_as_base,
        )
    {
        // https://drafts.csswg.org/css-values-4/#combine-math
        // Interpolation of math functions, with each other or with numeric values and other numeric-valued functions, is defined as Vresult = calc((1 - p) * VA + p * VB).
        return handled_retained_value(retained_calculation(calculation, resolved_type, &context));
    }

    // https://drafts.csswg.org/css-values-4/#combine-mixed
    // The computed value of a percentage-dimension mix is defined as
    // a computed percentage if the dimension component is zero
    let dimension_component_is_zero = match (from, to) {
        (StyleValueData::Length { value, .. }, StyleValueData::Percentage { .. }) => {
            *value * (1.0 - delta as f64) == 0.0
        }
        (StyleValueData::Percentage { .. }, StyleValueData::Length { value, .. }) => *value * delta as f64 == 0.0,
        _ => false,
    };
    if dimension_component_is_zero {
        let percentage = match (from, to) {
            (StyleValueData::Length { .. }, StyleValueData::Percentage { value }) => *value * delta as f64,
            (StyleValueData::Percentage { value }, StyleValueData::Length { .. }) => *value * (1.0 - delta as f64),
            _ => unreachable!(),
        };
        return owned(StyleValueData::Percentage { value: percentage });
    }

    if (matches!(from, StyleValueData::Calculated { .. })
        || matches!(to, StyleValueData::Calculated { .. })
        || std::mem::discriminant(from) != std::mem::discriminant(to))
        && let (Some(from), Some(to)) = (
            length_percentage_calculation_node(from),
            length_percentage_calculation_node(to),
        )
        && let Some((calculation, resolved_type)) =
            crate::css::calc::interpolate_length_percentage_calculations(from, to, delta)
    {
        return handled_retained_value(retained_length_percentage_calculation(calculation, resolved_type));
    }

    match (from, to) {
        (StyleValueData::ColorFunction { .. }, StyleValueData::ColorFunction { .. }) => {
            combine_legacy_rgb(from, to, ColorCombination::Interpolate(delta))
                .or_else(|| combine_modern_color(from, to, ColorCombination::Interpolate(delta)))
                .map_or_else(not_handled, owned)
        }
        (StyleValueData::BasicShape { .. }, StyleValueData::BasicShape { .. }) => {
            interpolate_basic_shape(property_id, from, to, delta).map_or_else(handled_without_value, owned)
        }
        (
            StyleValueData::RadialSize {
                component_count: from_component_count,
                is_extent_0: from_is_extent_0,
                value_0: from_value_0,
                is_extent_1: from_is_extent_1,
                value_1: from_value_1,
                ..
            },
            StyleValueData::RadialSize {
                component_count: to_component_count,
                is_extent_0: to_is_extent_0,
                value_0: to_value_0,
                is_extent_1: to_is_extent_1,
                value_1: to_value_1,
                ..
            },
        ) => interpolate_radial_size(
            property_id,
            RadialSizeComponents {
                component_count: *from_component_count,
                is_extent_0: *from_is_extent_0,
                value_0: from_value_0,
                is_extent_1: *from_is_extent_1,
                value_1: from_value_1,
            },
            RadialSizeComponents {
                component_count: *to_component_count,
                is_extent_0: *to_is_extent_0,
                value_0: to_value_0,
                is_extent_1: *to_is_extent_1,
                value_1: to_value_1,
            },
            delta,
        )
        .map_or_else(handled_without_value, owned),
        (from_value @ StyleValueData::Keyword { keyword: from }, StyleValueData::Keyword { keyword: to })
            if from == to =>
        {
            FfiAnimationValueResult {
                value: unsafe { crate::css::style_value::retain_style_value(from_value) },
                handled: true,
            }
        }
        (StyleValueData::Number { value: from }, StyleValueData::Number { value: to }) => {
            owned(StyleValueData::Number {
                value: interpolate_f64(
                    *from,
                    *to,
                    delta,
                    accepted_range(property_id, VALUE_TYPE_NUMBER, range_overrides),
                ),
            })
        }
        (StyleValueData::Integer { value: from }, StyleValueData::Integer { value: to }) => {
            owned(StyleValueData::Integer {
                value: interpolate_i32(
                    *from,
                    *to,
                    delta,
                    accepted_range(property_id, VALUE_TYPE_INTEGER, range_overrides),
                ),
            })
        }
        (
            StyleValueData::Angle {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Angle {
                value: to,
                unit: to_unit,
            },
        ) => {
            let (Some(from), Some(to)) = (angle_to_degrees(*from, *from_unit), angle_to_degrees(*to, *to_unit)) else {
                return not_handled();
            };
            owned(StyleValueData::Angle {
                value: interpolate_f64(
                    from,
                    to,
                    delta,
                    accepted_range(property_id, VALUE_TYPE_ANGLE, range_overrides),
                ),
                unit: 0,
            })
        }
        (
            StyleValueData::Flex {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Flex {
                value: to,
                unit: to_unit,
            },
        ) if from_unit == to_unit => owned(StyleValueData::Flex {
            value: interpolate_f64(
                *from,
                *to,
                delta,
                accepted_range(property_id, VALUE_TYPE_FLEX, range_overrides),
            ),
            unit: *from_unit,
        }),
        (
            StyleValueData::Frequency {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Frequency {
                value: to,
                unit: to_unit,
            },
        ) if from_unit == to_unit => owned(StyleValueData::Frequency {
            value: interpolate_f64(
                *from,
                *to,
                delta,
                accepted_range(property_id, VALUE_TYPE_FREQUENCY, range_overrides),
            ),
            unit: *from_unit,
        }),
        (
            StyleValueData::Length {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Length {
                value: to,
                unit: to_unit,
            },
        ) if from_unit == to_unit => owned(StyleValueData::Length {
            value: interpolate_f64(
                *from,
                *to,
                delta,
                accepted_range(property_id, VALUE_TYPE_LENGTH, range_overrides),
            ),
            unit: *from_unit,
        }),
        (StyleValueData::Percentage { value: from }, StyleValueData::Percentage { value: to }) => {
            owned(StyleValueData::Percentage {
                value: interpolate_f64(
                    *from,
                    *to,
                    delta,
                    accepted_range(property_id, VALUE_TYPE_PERCENTAGE, range_overrides),
                ),
            })
        }
        (
            StyleValueData::Resolution {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Resolution {
                value: to,
                unit: to_unit,
            },
        ) if from_unit == to_unit => owned(StyleValueData::Resolution {
            value: interpolate_f64(
                *from,
                *to,
                delta,
                accepted_range(property_id, VALUE_TYPE_RESOLUTION, range_overrides),
            ),
            unit: *from_unit,
        }),
        (
            StyleValueData::Time {
                value: from,
                unit: from_unit,
            },
            StyleValueData::Time {
                value: to,
                unit: to_unit,
            },
        ) if from_unit == to_unit => owned(StyleValueData::Time {
            value: interpolate_f64(
                *from,
                *to,
                delta,
                accepted_range(property_id, VALUE_TYPE_TIME, range_overrides),
            ),
            unit: *from_unit,
        }),
        (StyleValueData::OpacityValue { value: from }, StyleValueData::OpacityValue { value: to }) => {
            let (StyleValueData::Number { value: from }, StyleValueData::Number { value: to }) =
                (from.data(), to.data())
            else {
                return not_handled();
            };
            let number = Arc::into_raw(Arc::new(StyleValueData::Number {
                value: interpolate_f64(*from, *to, delta, Some((0.0, 1.0))),
            }));
            owned(StyleValueData::OpacityValue {
                value: unsafe { RetainedStyleValueData::from_retained_pointer(number) },
            })
        }
        (StyleValueData::Superellipse { parameter: from }, StyleValueData::Superellipse { parameter: to }) => {
            let (StyleValueData::Number { value: from }, StyleValueData::Number { value: to }) =
                (from.data(), to.data())
            else {
                return not_handled();
            };

            // https://drafts.csswg.org/css-borders-4/#corner-shape-interpolation
            let from_normalized_value = normalized_super_ellipse_half_corner(*from);
            let to_normalized_value = normalized_super_ellipse_half_corner(*to);
            let interpolated_value =
                interpolate_f64(from_normalized_value, to_normalized_value, delta, Some((0.0, 1.0)));
            let parameter = Arc::into_raw(Arc::new(StyleValueData::Number {
                value: interpolation_value_to_super_ellipse_parameter(interpolated_value),
            }));
            owned(StyleValueData::Superellipse {
                parameter: unsafe { RetainedStyleValueData::from_retained_pointer(parameter) },
            })
        }
        (
            StyleValueData::BackgroundSize {
                size_x: from_x,
                size_y: from_y,
            },
            StyleValueData::BackgroundSize {
                size_x: to_x,
                size_y: to_y,
            },
        ) => {
            let x = interpolate_scalar_value(property_id, from_x.data(), to_x.data(), delta, range_overrides);
            let y = interpolate_scalar_value(property_id, from_y.data(), to_y.data(), delta, range_overrides);
            if !x.handled || !y.handled {
                return not_handled();
            }
            if x.value.is_null() || y.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::BackgroundSize {
                size_x: unsafe { RetainedStyleValueData::from_retained_pointer(x.value) },
                size_y: unsafe { RetainedStyleValueData::from_retained_pointer(y.value) },
            })
        }
        (StyleValueData::Edge { offset: from, .. }, StyleValueData::Edge { offset: to, .. }) => {
            let (Some(from), Some(to)) = (from.optional_data(), to.optional_data()) else {
                return not_handled();
            };
            let Some(offset) = interpolate_translate_component(property_id, from, to, delta) else {
                return handled_without_value();
            };
            owned(StyleValueData::Edge {
                has_edge: false,
                edge: 0,
                offset,
            })
        }
        (
            StyleValueData::Position {
                edge_x: from_x,
                edge_y: from_y,
            },
            StyleValueData::Position {
                edge_x: to_x,
                edge_y: to_y,
            },
        ) => {
            // https://www.w3.org/TR/css-values-4/#combine-positions
            // FIXME: Interpolation of <position> is defined as the independent interpolation of each component (x, y) normalized as an offset from the top left corner as a <length-percentage>.
            let x = interpolate_scalar_value(property_id, from_x.data(), to_x.data(), delta, range_overrides);
            let y = interpolate_scalar_value(property_id, from_y.data(), to_y.data(), delta, range_overrides);
            if !x.handled || !y.handled {
                return not_handled();
            }
            if x.value.is_null() || y.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::Position {
                edge_x: unsafe { RetainedStyleValueData::from_retained_pointer(x.value) },
                edge_y: unsafe { RetainedStyleValueData::from_retained_pointer(y.value) },
            })
        }
        (
            StyleValueData::Rect {
                top: from_top,
                right: from_right,
                bottom: from_bottom,
                left: from_left,
            },
            StyleValueData::Rect {
                top: to_top,
                right: to_right,
                bottom: to_bottom,
                left: to_left,
            },
        ) => {
            let combine = |from: &RetainedStyleValueData, to: &RetainedStyleValueData| {
                let result = interpolate_scalar_value(property_id, from.data(), to.data(), delta, range_overrides);
                if !result.handled {
                    return Err(());
                }
                if result.value.is_null() {
                    return Ok(None);
                }
                Ok(Some(unsafe {
                    RetainedStyleValueData::from_retained_pointer(result.value)
                }))
            };
            let value = (|| {
                let Some(top) = combine(from_top, to_top)? else {
                    return Ok(None);
                };
                let Some(right) = combine(from_right, to_right)? else {
                    return Ok(None);
                };
                let Some(bottom) = combine(from_bottom, to_bottom)? else {
                    return Ok(None);
                };
                let Some(left) = combine(from_left, to_left)? else {
                    return Ok(None);
                };
                Ok(Some(StyleValueData::Rect {
                    top,
                    right,
                    bottom,
                    left,
                }))
            })();
            match value {
                Err(()) => not_handled(),
                Ok(None) => handled_without_value(),
                Ok(Some(value)) => owned(value),
            }
        }
        (
            StyleValueData::BorderRadius {
                horizontal_radius: from_horizontal,
                vertical_radius: from_vertical,
                ..
            },
            StyleValueData::BorderRadius {
                horizontal_radius: to_horizontal,
                vertical_radius: to_vertical,
                ..
            },
        ) => {
            let horizontal = interpolate_scalar_value(
                property_id,
                from_horizontal.data(),
                to_horizontal.data(),
                delta,
                range_overrides,
            );
            if !horizontal.handled {
                return not_handled();
            }
            if horizontal.value.is_null() {
                return handled_without_value();
            }
            let horizontal = unsafe { RetainedStyleValueData::from_retained_pointer(horizontal.value) };
            let vertical = interpolate_scalar_value(
                property_id,
                from_vertical.data(),
                to_vertical.data(),
                delta,
                range_overrides,
            );
            if !vertical.handled {
                return not_handled();
            }
            if vertical.value.is_null() {
                return handled_without_value();
            }
            let vertical = unsafe { RetainedStyleValueData::from_retained_pointer(vertical.value) };
            owned(StyleValueData::BorderRadius {
                is_elliptical: !radius_components_equal(horizontal.data(), vertical.data()),
                horizontal_radius: horizontal,
                vertical_radius: vertical,
            })
        }
        (
            StyleValueData::BorderRadiusRect {
                top_left: from_top_left,
                top_right: from_top_right,
                bottom_right: from_bottom_right,
                bottom_left: from_bottom_left,
            },
            StyleValueData::BorderRadiusRect {
                top_left: to_top_left,
                top_right: to_top_right,
                bottom_right: to_bottom_right,
                bottom_left: to_bottom_left,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            let combine = |from: &RetainedStyleValueData, to: &RetainedStyleValueData| {
                let result =
                    interpolate_scalar_value(property_id, from.data(), to.data(), delta, BORDER_RADIUS_RECT_RANGES);
                if !result.handled {
                    return Err(());
                }
                if result.value.is_null() {
                    return Ok(None);
                }
                Ok(Some(unsafe {
                    RetainedStyleValueData::from_retained_pointer(result.value)
                }))
            };
            let value = (|| {
                let Some(top_left) = combine(from_top_left, to_top_left)? else {
                    return Ok(None);
                };
                let Some(top_right) = combine(from_top_right, to_top_right)? else {
                    return Ok(None);
                };
                let Some(bottom_right) = combine(from_bottom_right, to_bottom_right)? else {
                    return Ok(None);
                };
                let Some(bottom_left) = combine(from_bottom_left, to_bottom_left)? else {
                    return Ok(None);
                };
                Ok(Some(StyleValueData::BorderRadiusRect {
                    top_left,
                    top_right,
                    bottom_right,
                    bottom_left,
                }))
            })();
            match value {
                Err(()) => not_handled(),
                Ok(None) => handled_without_value(),
                Ok(Some(value)) => owned(value),
            }
        }
        (
            StyleValueData::BorderImageSlice {
                top: from_top,
                right: from_right,
                bottom: from_bottom,
                left: from_left,
                fill: from_fill,
            },
            StyleValueData::BorderImageSlice {
                top: to_top,
                right: to_right,
                bottom: to_bottom,
                left: to_left,
                fill: to_fill,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if from_fill != to_fill {
                return handled_without_value();
            }
            let combine = |from: &RetainedStyleValueData, to: &RetainedStyleValueData| {
                let result = interpolate_scalar_value(property_id, from.data(), to.data(), delta, range_overrides);
                if !result.handled {
                    return Err(());
                }
                if result.value.is_null() {
                    return Ok(None);
                }
                Ok(Some(unsafe {
                    RetainedStyleValueData::from_retained_pointer(result.value)
                }))
            };
            let value = (|| {
                let Some(top) = combine(from_top, to_top)? else {
                    return Ok(None);
                };
                let Some(right) = combine(from_right, to_right)? else {
                    return Ok(None);
                };
                let Some(bottom) = combine(from_bottom, to_bottom)? else {
                    return Ok(None);
                };
                let Some(left) = combine(from_left, to_left)? else {
                    return Ok(None);
                };
                Ok(Some(StyleValueData::BorderImageSlice {
                    top,
                    right,
                    bottom,
                    left,
                    fill: *from_fill,
                }))
            })();
            match value {
                Err(()) => not_handled(),
                Ok(None) => handled_without_value(),
                Ok(Some(value)) => owned(value),
            }
        }
        (
            StyleValueData::OpenTypeTagged {
                tag: from_tag,
                packed_tag: from_packed_tag,
                value: from_value,
                ..
            },
            StyleValueData::OpenTypeTagged {
                tag: to_tag,
                value: to_value,
                ..
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if from_tag.raw() != to_tag.raw() {
                return handled_without_value();
            }
            let value =
                interpolate_scalar_value(property_id, from_value.data(), to_value.data(), delta, range_overrides);
            if !value.handled {
                return not_handled();
            }
            if value.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::OpenTypeTagged {
                mode: OPEN_TYPE_MODE_FONT_VARIATION_SETTINGS,
                tag: from_tag.clone(),
                packed_tag: *from_packed_tag,
                value: unsafe { RetainedStyleValueData::from_retained_pointer(value.value) },
            })
        }
        (
            StyleValueData::Function {
                name: from_name,
                value: from_value,
            },
            StyleValueData::Function {
                name: to_name,
                value: to_value,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if from_name.raw() != to_name.raw() {
                return handled_without_value();
            }
            let value =
                interpolate_scalar_value(property_id, from_value.data(), to_value.data(), delta, range_overrides);
            if !value.handled {
                return not_handled();
            }
            if value.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::Function {
                name: from_name.clone(),
                value: unsafe { RetainedStyleValueData::from_retained_pointer(value.value) },
            })
        }
        (
            StyleValueData::TextIndent {
                length_percentage: from,
                hanging: from_hanging,
                each_line: from_each_line,
            },
            StyleValueData::TextIndent {
                length_percentage: to,
                hanging: to_hanging,
                each_line: to_each_line,
            },
        ) => {
            if from_hanging != to_hanging || from_each_line != to_each_line {
                return handled_without_value();
            }
            let result = interpolate_scalar_value(property_id, from.data(), to.data(), delta, range_overrides);
            if !result.handled {
                return not_handled();
            }
            if result.value.is_null() {
                return handled_without_value();
            }
            owned(StyleValueData::TextIndent {
                length_percentage: unsafe { RetainedStyleValueData::from_retained_pointer(result.value) },
                hanging: *from_hanging,
                each_line: *from_each_line,
            })
        }
        (
            StyleValueData::Ratio {
                numerator: from_numerator,
                denominator: from_denominator,
            },
            StyleValueData::Ratio {
                numerator: to_numerator,
                denominator: to_denominator,
            },
        ) => {
            let (
                StyleValueData::Number { value: from_numerator },
                StyleValueData::Number {
                    value: from_denominator,
                },
                StyleValueData::Number { value: to_numerator },
                StyleValueData::Number { value: to_denominator },
            ) = (
                from_numerator.data(),
                from_denominator.data(),
                to_numerator.data(),
                to_denominator.data(),
            )
            else {
                return not_handled();
            };

            // https://drafts.csswg.org/css-values/#combine-ratio
            // If either <ratio> is degenerate, the values cannot be interpolated.
            if !from_numerator.is_finite()
                || *from_numerator == 0.0
                || !from_denominator.is_finite()
                || *from_denominator == 0.0
                || !to_numerator.is_finite()
                || *to_numerator == 0.0
                || !to_denominator.is_finite()
                || *to_denominator == 0.0
            {
                return handled_without_value();
            }

            // The interpolation of a <ratio> is defined by converting each <ratio> to a number by dividing the first value
            // by the second (so a ratio of 3 / 2 would become 1.5), taking the logarithm of that result (so the 1.5 would
            // become approximately 0.176), then interpolating those values. The result during the interpolation is
            // converted back to a <ratio> by inverting the logarithm, then interpreting the result as a <ratio> with the
            // result as the first value and 1 as the second value.
            let from_number = (from_numerator / from_denominator).ln();
            let to_number = (to_numerator / to_denominator).ln();
            let value = interpolate_f64(
                from_number,
                to_number,
                delta,
                accepted_range(property_id, VALUE_TYPE_RATIO, range_overrides),
            )
            .exp();
            let numerator = Arc::into_raw(Arc::new(StyleValueData::Number { value }));
            let denominator = Arc::into_raw(Arc::new(StyleValueData::Number { value: 1.0 }));
            owned(StyleValueData::Ratio {
                numerator: unsafe { RetainedStyleValueData::from_retained_pointer(numerator) },
                denominator: unsafe { RetainedStyleValueData::from_retained_pointer(denominator) },
            })
        }
        (
            StyleValueData::ValueList {
                values: from_values,
                separator,
                collapsible,
            },
            StyleValueData::ValueList { values: to_values, .. },
        ) => {
            // https://www.w3.org/TR/web-animations/#by-computed-value
            // If the number of components or the types of corresponding components do not match,
            // or if any component value uses discrete animation and the two corresponding values do not match,
            // then the property values combine as discrete.
            if from_values.as_slice().len() != to_values.as_slice().len() {
                return not_handled();
            }

            let mut values = Vec::with_capacity(from_values.as_slice().len());
            for (from, to) in from_values.as_slice().iter().zip(to_values.as_slice()) {
                let result = interpolate_scalar_value(property_id, from.data(), to.data(), delta, range_overrides);
                if !result.handled {
                    return not_handled();
                }
                if result.value.is_null() {
                    return handled_without_value();
                }
                values.push(unsafe { RetainedStyleValueData::from_retained_pointer(result.value) });
            }
            owned(StyleValueData::ValueList {
                values: RetainedStyleValueDataList::from_retained_values(values),
                separator: *separator,
                collapsible: *collapsible,
            })
        }
        _ => not_handled(),
    }
}

fn interpolate_rotate_3d(
    context: Option<&FfiAnimationContext>,
    property: u16,
    transform_function: u8,
    from_arguments: &RetainedStyleValueDataList,
    to_arguments: &RetainedStyleValueDataList,
    delta: f32,
) -> Option<StyleValueData> {
    let ([from_x, from_y, from_z, from_angle], [to_x, to_y, to_z, to_angle]) =
        (from_arguments.as_slice(), to_arguments.as_slice())
    else {
        return None;
    };
    let from_angle = resolve_animation_angle(context, from_angle.data())?.to_radians();
    let to_angle = resolve_animation_angle(context, to_angle.data())?.to_radians();
    let from_axis = [
        resolve_animation_number(context, from_x.data())?,
        resolve_animation_number(context, from_y.data())?,
        resolve_animation_number(context, from_z.data())?,
    ];
    let to_axis = [
        resolve_animation_number(context, to_x.data())?,
        resolve_animation_number(context, to_y.data())?,
        resolve_animation_number(context, to_z.data())?,
    ];

    let length = |vector: [f64; 3]| vector.iter().map(|component| component * component).sum::<f64>().sqrt();
    let normalize = |vector: [f64; 3]| {
        let length = length(vector);
        [vector[0] / length, vector[1] / length, vector[2] / length]
    };
    let epsilon = 1e-5;
    let from_axis_normalized = if length(from_axis) > epsilon {
        normalize(from_axis)
    } else {
        [0.0, 0.0, 1.0]
    };
    let to_axis_normalized = if length(to_axis) > epsilon {
        normalize(to_axis)
    } else {
        [0.0, 0.0, 1.0]
    };
    let axis_difference = [
        from_axis_normalized[0] - to_axis_normalized[0],
        from_axis_normalized[1] - to_axis_normalized[1],
        from_axis_normalized[2] - to_axis_normalized[2],
    ];

    // https://drafts.csswg.org/css-transforms-2/#interpolation-of-transform-functions
    // For interpolations with the primitive rotate3d(), the direction vectors of the transform functions get
    // normalized first. If the normalized vectors are not equal and both rotation angles are non-zero the
    // transform functions get converted into 4x4 matrices first and interpolated as defined in section
    // Interpolation of Matrices afterwards. Otherwise the rotation angle gets interpolated numerically and the
    // rotation vector of the non-zero angle is used or (0, 0, 1) if both angles are zero.
    let (result_axis, result_angle) = if length(axis_difference) < epsilon || from_angle == 0.0 || to_angle == 0.0 {
        let result_axis = if to_angle != 0.0 {
            to_axis_normalized
        } else if from_angle != 0.0 {
            from_axis_normalized
        } else {
            [0.0, 0.0, 1.0]
        };
        (result_axis, interpolate_f64(from_angle, to_angle, delta, None))
    } else {
        let to_quaternion = |axis: [f64; 3], angle: f64| {
            let half_angle = angle / 2.0;
            let sin_half_angle = half_angle.sin();
            [
                axis[0] * sin_half_angle,
                axis[1] * sin_half_angle,
                axis[2] * sin_half_angle,
                half_angle.cos(),
            ]
        };
        let mut from_quaternion = to_quaternion(from_axis_normalized, from_angle);
        let to_quaternion = to_quaternion(to_axis_normalized, to_angle);

        // https://drafts.csswg.org/css-transforms-2/#interpolation-of-decomposed-3d-matrix-values
        let mut product = from_quaternion
            .iter()
            .zip(to_quaternion)
            .map(|(from, to)| from * to)
            .sum::<f64>()
            .clamp(-1.0, 1.0);

        // AD-HOC: The specification's slerp pseudocode interpolates the quaternions as given, but a
        // quaternion and its negation represent the same rotation. A negative dot product means
        // the interpolation would travel the long way around the sphere. Negate one input to take
        // the shortest path, as other engines do.
        if product < 0.0 {
            from_quaternion = from_quaternion.map(|component| -component);
            product = -product;
        }
        let interpolated_quaternion = if product.abs() >= 1.0 {
            from_quaternion
        } else {
            let theta = product.acos();
            let weight = (f64::from(delta) * theta).sin() / (1.0 - product * product).sqrt();
            let from_multiplier = (f64::from(delta) * theta).cos() - product * weight;
            if weight.abs() < f64::from(f32::EPSILON) {
                from_quaternion.map(|component| component * from_multiplier)
            } else if from_multiplier.abs() < f64::from(f32::EPSILON) {
                to_quaternion.map(|component| component * weight)
            } else {
                std::array::from_fn(|index| from_quaternion[index] * from_multiplier + to_quaternion[index] * weight)
            }
        };

        let mut axis = [
            interpolated_quaternion[0],
            interpolated_quaternion[1],
            interpolated_quaternion[2],
        ];
        let sin_half_angle = (1.0 - interpolated_quaternion[3] * interpolated_quaternion[3])
            .max(0.0)
            .sqrt();
        let angle = 2.0 * interpolated_quaternion[3].clamp(-1.0, 1.0).acos();
        if sin_half_angle >= epsilon {
            axis = axis.map(|component| component / sin_half_angle);
        } else {
            // The rotation angle is zero, so the axis is arbitrary.
            axis = [0.0, 0.0, 1.0];
        }
        (axis, angle)
    };

    let mut arguments = Vec::with_capacity(4);
    for value in result_axis {
        let argument = Arc::into_raw(Arc::new(StyleValueData::Number { value }));
        arguments.push(unsafe { RetainedStyleValueData::from_retained_pointer(argument) });
    }
    let angle = Arc::into_raw(Arc::new(StyleValueData::Angle {
        value: result_angle.to_degrees(),
        unit: 0,
    }));
    arguments.push(unsafe { RetainedStyleValueData::from_retained_pointer(angle) });

    Some(StyleValueData::Transformation {
        property,
        transform_function,
        values: RetainedStyleValueDataList::from_retained_values(arguments),
    })
}

fn retained_number(value: f64) -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::Number { value }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn retained_zero_px() -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::Length {
        value: 0.0,
        unit: crate::css::style_compute::px_length_unit(),
    }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn retained_length(value: f64, unit: u8) -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::Length { value, unit }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn retained_none_keyword() -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::Keyword {
        keyword: crate::css::style_compute::none_keyword(),
    }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn is_2d_transform(function: u8) -> bool {
    matches!(
        function,
        TRANSFORM_FUNCTION_ROTATE
            | TRANSFORM_FUNCTION_SCALE
            | TRANSFORM_FUNCTION_SCALE_X
            | TRANSFORM_FUNCTION_SCALE_Y
            | TRANSFORM_FUNCTION_TRANSLATE
            | TRANSFORM_FUNCTION_TRANSLATE_X
            | TRANSFORM_FUNCTION_TRANSLATE_Y
    )
}

fn is_3d_primitive(function: u8) -> bool {
    matches!(
        function,
        TRANSFORM_FUNCTION_ROTATE_3D | TRANSFORM_FUNCTION_SCALE_3D | TRANSFORM_FUNCTION_TRANSLATE_3D
    )
}

fn is_3d_transform(function: u8) -> bool {
    is_2d_transform(function)
        || is_3d_primitive(function)
        || matches!(
            function,
            TRANSFORM_FUNCTION_ROTATE_X
                | TRANSFORM_FUNCTION_ROTATE_Y
                | TRANSFORM_FUNCTION_ROTATE_Z
                | TRANSFORM_FUNCTION_SCALE_Z
                | TRANSFORM_FUNCTION_TRANSLATE_Z
        )
}

fn convert_2d_transform_to_primitive(
    function: u8,
    arguments: &RetainedStyleValueDataList,
) -> Option<(u8, Vec<RetainedStyleValueData>)> {
    let arguments = arguments.as_slice();
    match (function, arguments) {
        (TRANSFORM_FUNCTION_SCALE, [x]) => {
            Some((TRANSFORM_FUNCTION_SCALE, vec![x.clone_retained(), x.clone_retained()]))
        }
        (TRANSFORM_FUNCTION_SCALE, [x, y]) => {
            Some((TRANSFORM_FUNCTION_SCALE, vec![x.clone_retained(), y.clone_retained()]))
        }
        (TRANSFORM_FUNCTION_SCALE_X, [x]) => {
            Some((TRANSFORM_FUNCTION_SCALE, vec![x.clone_retained(), retained_number(1.0)]))
        }
        (TRANSFORM_FUNCTION_SCALE_Y, [y]) => {
            Some((TRANSFORM_FUNCTION_SCALE, vec![retained_number(1.0), y.clone_retained()]))
        }
        (TRANSFORM_FUNCTION_ROTATE, [angle]) => Some((TRANSFORM_FUNCTION_ROTATE, vec![angle.clone_retained()])),
        (TRANSFORM_FUNCTION_TRANSLATE, [x]) => Some((
            TRANSFORM_FUNCTION_TRANSLATE,
            vec![x.clone_retained(), retained_zero_px()],
        )),
        (TRANSFORM_FUNCTION_TRANSLATE, [x, y]) => Some((
            TRANSFORM_FUNCTION_TRANSLATE,
            vec![x.clone_retained(), y.clone_retained()],
        )),
        (TRANSFORM_FUNCTION_TRANSLATE_X, [x]) => Some((
            TRANSFORM_FUNCTION_TRANSLATE,
            vec![x.clone_retained(), retained_zero_px()],
        )),
        (TRANSFORM_FUNCTION_TRANSLATE_Y, [y]) => Some((
            TRANSFORM_FUNCTION_TRANSLATE,
            vec![retained_zero_px(), y.clone_retained()],
        )),
        _ => None,
    }
}

// https://drafts.csswg.org/css-transforms-1/#transform-primitives
// https://drafts.csswg.org/css-transforms-2/#transform-primitives
fn convert_3d_transform_to_primitive(
    function: u8,
    arguments: &RetainedStyleValueDataList,
) -> Option<(u8, Vec<RetainedStyleValueData>)> {
    let converted_2d;
    let (function, arguments) = if is_2d_transform(function) {
        converted_2d = convert_2d_transform_to_primitive(function, arguments)?;
        (converted_2d.0, converted_2d.1.as_slice())
    } else {
        (function, arguments.as_slice())
    };

    match (function, arguments) {
        (TRANSFORM_FUNCTION_ROTATE | TRANSFORM_FUNCTION_ROTATE_Z, [angle]) => Some((
            TRANSFORM_FUNCTION_ROTATE_3D,
            vec![
                retained_number(0.0),
                retained_number(0.0),
                retained_number(1.0),
                angle.clone_retained(),
            ],
        )),
        (TRANSFORM_FUNCTION_ROTATE_X, [angle]) => Some((
            TRANSFORM_FUNCTION_ROTATE_3D,
            vec![
                retained_number(1.0),
                retained_number(0.0),
                retained_number(0.0),
                angle.clone_retained(),
            ],
        )),
        (TRANSFORM_FUNCTION_ROTATE_Y, [angle]) => Some((
            TRANSFORM_FUNCTION_ROTATE_3D,
            vec![
                retained_number(0.0),
                retained_number(1.0),
                retained_number(0.0),
                angle.clone_retained(),
            ],
        )),
        (TRANSFORM_FUNCTION_SCALE, [x, y]) => Some((
            TRANSFORM_FUNCTION_SCALE_3D,
            vec![x.clone_retained(), y.clone_retained(), retained_number(1.0)],
        )),
        (TRANSFORM_FUNCTION_SCALE_Z, [z]) => Some((
            TRANSFORM_FUNCTION_SCALE_3D,
            vec![retained_number(1.0), retained_number(1.0), z.clone_retained()],
        )),
        (TRANSFORM_FUNCTION_TRANSLATE, [x, y]) => Some((
            TRANSFORM_FUNCTION_TRANSLATE_3D,
            vec![x.clone_retained(), y.clone_retained(), retained_zero_px()],
        )),
        (TRANSFORM_FUNCTION_TRANSLATE_Z, [z]) => Some((
            TRANSFORM_FUNCTION_TRANSLATE_3D,
            vec![retained_zero_px(), retained_zero_px(), z.clone_retained()],
        )),
        _ => None,
    }
}

fn convert_transform_pair_to_common_primitive(
    from_function: u8,
    from_arguments: &RetainedStyleValueDataList,
    to_function: u8,
    to_arguments: &RetainedStyleValueDataList,
) -> Option<(u8, Vec<RetainedStyleValueData>, Vec<RetainedStyleValueData>)> {
    if matches!(
        (from_function, to_function),
        (
            TRANSFORM_FUNCTION_MATRIX | TRANSFORM_FUNCTION_MATRIX_3D | TRANSFORM_FUNCTION_PERSPECTIVE,
            _,
        ) | (
            _,
            TRANSFORM_FUNCTION_MATRIX | TRANSFORM_FUNCTION_MATRIX_3D | TRANSFORM_FUNCTION_PERSPECTIVE,
        )
    ) {
        return None;
    }
    // https://drafts.csswg.org/css-transforms-2/#interpolation-of-transform-functions
    // If both transform functions share a primitive in the two-dimensional space, both transform functions get
    // converted to the two-dimensional primitive. If one or both transform functions are three-dimensional
    // transform functions, the common three-dimensional primitive is used.
    let (from_function, from_arguments, to_function, to_arguments) =
        if is_2d_transform(from_function) && is_2d_transform(to_function) {
            let (from_function, from_arguments) = convert_2d_transform_to_primitive(from_function, from_arguments)?;
            let (to_function, to_arguments) = convert_2d_transform_to_primitive(to_function, to_arguments)?;
            (from_function, from_arguments, to_function, to_arguments)
        } else if is_3d_transform(from_function) || is_3d_transform(to_function) {
            let (from_function, from_arguments) = if is_3d_primitive(from_function) {
                (
                    from_function,
                    from_arguments
                        .as_slice()
                        .iter()
                        .map(RetainedStyleValueData::clone_retained)
                        .collect(),
                )
            } else {
                convert_3d_transform_to_primitive(from_function, from_arguments)?
            };
            let (to_function, to_arguments) = if is_3d_primitive(to_function) {
                (
                    to_function,
                    to_arguments
                        .as_slice()
                        .iter()
                        .map(RetainedStyleValueData::clone_retained)
                        .collect(),
                )
            } else {
                convert_3d_transform_to_primitive(to_function, to_arguments)?
            };
            (from_function, from_arguments, to_function, to_arguments)
        } else {
            (
                from_function,
                from_arguments
                    .as_slice()
                    .iter()
                    .map(RetainedStyleValueData::clone_retained)
                    .collect(),
                to_function,
                to_arguments
                    .as_slice()
                    .iter()
                    .map(RetainedStyleValueData::clone_retained)
                    .collect(),
            )
        };
    (from_function == to_function && from_arguments.len() == to_arguments.len()).then_some((
        from_function,
        from_arguments,
        to_arguments,
    ))
}

fn identity_transformation(property: u16, function: u8) -> Option<RetainedStyleValueData> {
    // https://drafts.csswg.org/css-transforms-1/#identity-transform-function
    // A transform function that is equivalent to a identity 4x4 matrix (see Mathematical Description of Transform
    // Functions). Examples for identity transform functions are translate(0), translateX(0), translateY(0), scale(1),
    // scaleX(1), scaleY(1), rotate(0), skew(0, 0), skewX(0), skewY(0) and matrix(1, 0, 0, 1, 0, 0).

    // https://drafts.csswg.org/css-transforms-2/#identity-transform-function
    // In addition to the identity transform function in CSS Transforms, examples for identity transform functions
    // include translate3d(0, 0, 0), translateZ(0), scaleZ(1), rotate3d(1, 1, 1, 0), rotateX(0), rotateY(0), rotateZ(0)
    // and matrix3d(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1). A special case is perspective: perspective(none).
    // The value of m34 becomes infinitesimal small and the transform function is therefore assumed to be equal to the
    // identity matrix.
    let arguments = match function {
        TRANSFORM_FUNCTION_MATRIX => vec![
            retained_number(1.0),
            retained_number(0.0),
            retained_number(0.0),
            retained_number(1.0),
            retained_number(0.0),
            retained_number(0.0),
        ],
        TRANSFORM_FUNCTION_MATRIX_3D => (0..16)
            .map(|index| retained_number(if index % 5 == 0 { 1.0 } else { 0.0 }))
            .collect(),
        TRANSFORM_FUNCTION_PERSPECTIVE => vec![retained_none_keyword()],
        TRANSFORM_FUNCTION_ROTATE
        | TRANSFORM_FUNCTION_ROTATE_X
        | TRANSFORM_FUNCTION_ROTATE_Y
        | TRANSFORM_FUNCTION_ROTATE_Z
        | TRANSFORM_FUNCTION_SKEW
        | TRANSFORM_FUNCTION_SKEW_X
        | TRANSFORM_FUNCTION_SKEW_Y => {
            let angle = Arc::into_raw(Arc::new(StyleValueData::Angle { value: 0.0, unit: 0 }));
            vec![unsafe { RetainedStyleValueData::from_retained_pointer(angle) }]
        }
        TRANSFORM_FUNCTION_ROTATE_3D => vec![
            retained_number(1.0),
            retained_number(1.0),
            retained_number(1.0),
            unsafe {
                RetainedStyleValueData::from_retained_pointer(Arc::into_raw(Arc::new(StyleValueData::Angle {
                    value: 0.0,
                    unit: 0,
                })))
            },
        ],
        TRANSFORM_FUNCTION_TRANSLATE
        | TRANSFORM_FUNCTION_TRANSLATE_X
        | TRANSFORM_FUNCTION_TRANSLATE_Y
        | TRANSFORM_FUNCTION_TRANSLATE_Z => vec![retained_zero_px()],
        TRANSFORM_FUNCTION_TRANSLATE_3D => vec![retained_zero_px(), retained_zero_px(), retained_zero_px()],
        TRANSFORM_FUNCTION_SCALE
        | TRANSFORM_FUNCTION_SCALE_X
        | TRANSFORM_FUNCTION_SCALE_Y
        | TRANSFORM_FUNCTION_SCALE_Z => vec![retained_number(1.0)],
        TRANSFORM_FUNCTION_SCALE_3D => vec![retained_number(1.0), retained_number(1.0), retained_number(1.0)],
        _ => return None,
    };
    let transformation = Arc::into_raw(Arc::new(StyleValueData::Transformation {
        property,
        transform_function: function,
        values: RetainedStyleValueDataList::from_retained_values(arguments),
    }));
    Some(unsafe { RetainedStyleValueData::from_retained_pointer(transformation) })
}

type Matrix4 = [[f64; 4]; 4];

struct DecomposedMatrix {
    translation: [f64; 3],
    scale: [f64; 3],
    skew: [f64; 3],
    rotation: [f64; 4],
    perspective: [f64; 4],
}

fn identity_matrix() -> Matrix4 {
    std::array::from_fn(|row| std::array::from_fn(|column| if row == column { 1.0 } else { 0.0 }))
}

fn multiply_matrices(left: Matrix4, right: Matrix4) -> Matrix4 {
    std::array::from_fn(|row| {
        std::array::from_fn(|column| (0..4).map(|index| left[row][index] * right[index][column]).sum())
    })
}

fn invert_matrix(matrix: Matrix4) -> Option<Matrix4> {
    let mut augmented = [[0.0; 8]; 4];
    for row in 0..4 {
        augmented[row][..4].copy_from_slice(&matrix[row]);
        augmented[row][row + 4] = 1.0;
    }
    for column in 0..4 {
        let pivot_row = (column..4).max_by(|left, right| {
            augmented[*left][column]
                .abs()
                .total_cmp(&augmented[*right][column].abs())
        })?;
        if augmented[pivot_row][column] == 0.0 {
            return None;
        }
        augmented.swap(column, pivot_row);
        let pivot = augmented[column][column];
        for value in &mut augmented[column] {
            *value /= pivot;
        }
        let pivot_values = augmented[column];
        for (row, values) in augmented.iter_mut().enumerate() {
            if row == column {
                continue;
            }
            let factor = values[column];
            for index in 0..8 {
                values[index] -= factor * pivot_values[index];
            }
        }
    }
    Some(std::array::from_fn(|row| {
        std::array::from_fn(|column| augmented[row][column + 4])
    }))
}

fn vector_length(vector: [f64; 3]) -> f64 {
    vector.iter().map(|component| component * component).sum::<f64>().sqrt()
}

fn vector_dot(left: [f64; 3], right: [f64; 3]) -> f64 {
    left.iter().zip(right).map(|(left, right)| left * right).sum()
}

fn vector_cross(left: [f64; 3], right: [f64; 3]) -> [f64; 3] {
    [
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    ]
}

// https://drafts.csswg.org/css-transforms-1/#supporting-functions
fn combine_vectors(left: [f64; 3], right: [f64; 3], left_scale: f64, right_scale: f64) -> [f64; 3] {
    std::array::from_fn(|index| left_scale * left[index] + right_scale * right[index])
}

// https://drafts.csswg.org/css-transforms-2/#decomposing-a-3d-matrix
fn decompose_matrix(mut matrix: Matrix4) -> Option<DecomposedMatrix> {
    // Normalize the matrix.
    if matrix[3][3] == 0.0 {
        return None;
    }
    let normalization = matrix[3][3];
    for row in &mut matrix {
        for value in row {
            *value /= normalization;
        }
    }

    // perspectiveMatrix is used to solve for perspective, but it also provides
    // an easy way to test for singularity of the upper 3x3 component.
    let mut perspective_matrix = matrix;
    perspective_matrix[3][..3].fill(0.0);
    perspective_matrix[3][3] = 1.0;
    // Solve the equation by inverting perspectiveMatrix and multiplying
    // rightHandSide by the inverse.
    let inverse_perspective_matrix = invert_matrix(perspective_matrix)?;

    // First, isolate perspective.
    let perspective = if matrix[3][0] != 0.0 || matrix[3][1] != 0.0 || matrix[3][2] != 0.0 {
        // rightHandSide is the right hand side of the equation.
        // Note: It is the bottom side in a row-major matrix
        let bottom_side = matrix[3];
        std::array::from_fn(|row| {
            (0..4)
                .map(|column| inverse_perspective_matrix[column][row] * bottom_side[column])
                .sum()
        })
    } else {
        // No perspective.
        [0.0, 0.0, 0.0, 1.0]
    };

    // Next take care of translation
    let translation = [matrix[0][3], matrix[1][3], matrix[2][3]];

    // Now get scale and shear. 'row' is a 3 element array of 3 component vectors
    let mut row: [[f64; 3]; 3] =
        std::array::from_fn(|column| [matrix[0][column], matrix[1][column], matrix[2][column]]);

    // Compute X scale factor and normalize first row.
    let mut scale = [0.0; 3];
    scale[0] = vector_length(row[0]);
    row[0] = row[0].map(|value| value / scale[0]);

    // Compute XY shear factor and make 2nd row orthogonal to 1st.
    let mut skew = [0.0; 3];
    skew[0] = vector_dot(row[0], row[1]);
    row[1] = combine_vectors(row[1], row[0], 1.0, -skew[0]);

    // Now, compute Y scale and normalize 2nd row.
    scale[1] = vector_length(row[1]);
    row[1] = row[1].map(|value| value / scale[1]);
    skew[0] /= scale[1];

    // Compute XZ and YZ shears, orthogonalize 3rd row
    skew[1] = vector_dot(row[0], row[2]);
    row[2] = combine_vectors(row[2], row[0], 1.0, -skew[1]);
    skew[2] = vector_dot(row[1], row[2]);
    row[2] = combine_vectors(row[2], row[1], 1.0, -skew[2]);

    // Next, get Z scale and normalize 3rd row.
    scale[2] = vector_length(row[2]);
    row[2] = row[2].map(|value| value / scale[2]);
    skew[1] /= scale[2];
    skew[2] /= scale[2];

    // At this point, the matrix (in rows) is orthonormal.
    // Check for a coordinate system flip.  If the determinant
    // is -1, then negate the matrix and the scaling factors.
    let pdum3 = vector_cross(row[1], row[2]);
    if vector_dot(row[0], pdum3) < 0.0 {
        for index in 0..3 {
            scale[index] *= -1.0;
            row[index] = row[index].map(|value| -value);
        }
    }

    // Now, get the rotations out
    let mut rotation = [
        0.5 * (1.0 + row[0][0] - row[1][1] - row[2][2]).max(0.0).sqrt(),
        0.5 * (1.0 - row[0][0] + row[1][1] - row[2][2]).max(0.0).sqrt(),
        0.5 * (1.0 - row[0][0] - row[1][1] + row[2][2]).max(0.0).sqrt(),
        0.5 * (1.0 + row[0][0] + row[1][1] + row[2][2]).max(0.0).sqrt(),
    ];
    if row[2][1] > row[1][2] {
        rotation[0] = -rotation[0];
    }
    if row[0][2] > row[2][0] {
        rotation[1] = -rotation[1];
    }
    if row[1][0] > row[0][1] {
        rotation[2] = -rotation[2];
    }

    // FIXME: This accounts for the fact that the browser coordinate system is left-handed instead of right-handed.
    //        The reason for this is that the positive Y-axis direction points down instead of up. To fix this, we
    //        invert the Y axis. However, it feels like the spec pseudo-code above should have taken something like
    //        this into account, so we're probably doing something else wrong.
    rotation[2] *= -1.0;

    Some(DecomposedMatrix {
        translation,
        scale,
        skew,
        rotation,
        perspective,
    })
}

// https://drafts.csswg.org/css-transforms-2/#recomposing-to-a-3d-matrix
fn recompose_matrix(values: DecomposedMatrix) -> Matrix4 {
    let mut matrix = identity_matrix();

    // apply perspective
    matrix[3] = values.perspective;

    // apply translation
    for row in &mut matrix {
        for column in 0..3 {
            row[3] += values.translation[column] * row[column];
        }
    }

    // apply rotation
    let [x, y, z, w] = values.rotation;
    // Construct a composite rotation matrix from the quaternion values
    // rotationMatrix is a identity 4x4 matrix initially
    let mut rotation_matrix = identity_matrix();
    rotation_matrix[0][0] = 1.0 - 2.0 * (y * y + z * z);
    rotation_matrix[1][0] = 2.0 * (x * y - z * w);
    rotation_matrix[2][0] = 2.0 * (x * z + y * w);
    rotation_matrix[0][1] = 2.0 * (x * y + z * w);
    rotation_matrix[1][1] = 1.0 - 2.0 * (x * x + z * z);
    rotation_matrix[2][1] = 2.0 * (y * z - x * w);
    rotation_matrix[0][2] = 2.0 * (x * z - y * w);
    rotation_matrix[1][2] = 2.0 * (y * z + x * w);
    rotation_matrix[2][2] = 1.0 - 2.0 * (x * x + y * y);
    matrix = multiply_matrices(matrix, rotation_matrix);

    // apply skew
    // temp is a identity 4x4 matrix initially
    let mut temp = identity_matrix();
    if values.skew[2] != 0.0 {
        temp[1][2] = values.skew[2];
        matrix = multiply_matrices(matrix, temp);
    }
    if values.skew[1] != 0.0 {
        temp[1][2] = 0.0;
        temp[0][2] = values.skew[1];
        matrix = multiply_matrices(matrix, temp);
    }
    if values.skew[0] != 0.0 {
        temp[0][2] = 0.0;
        temp[0][1] = values.skew[0];
        matrix = multiply_matrices(matrix, temp);
    }

    // apply scale
    for index in 0..3 {
        for row in &mut matrix {
            row[index] *= values.scale[index];
        }
    }
    matrix
}

// https://drafts.csswg.org/css-transforms-2/#interpolation-of-decomposed-3d-matrix-values
fn slerp_quaternions(from: [f64; 4], to: [f64; 4], delta: f32) -> [f64; 4] {
    let product = from
        .iter()
        .zip(to)
        .map(|(from, to)| from * to)
        .sum::<f64>()
        .clamp(-1.0, 1.0);
    if product.abs() >= 1.0 {
        return from;
    }
    let theta = product.acos();
    let weight = (f64::from(delta) * theta).sin() / (1.0 - product * product).sqrt();
    let from_multiplier = (f64::from(delta) * theta).cos() - product * weight;
    if weight.abs() < f64::from(f32::EPSILON) {
        return from.map(|component| component * from_multiplier);
    }
    if from_multiplier.abs() < f64::from(f32::EPSILON) {
        return to.map(|component| component * weight);
    }
    std::array::from_fn(|index| from[index] * from_multiplier + to[index] * weight)
}

fn interpolate_matrices(from: Matrix4, to: Matrix4, delta: f32) -> Option<Matrix4> {
    let from = decompose_matrix(from)?;
    let to = decompose_matrix(to)?;
    let interpolate_array = |from: [f64; 3], to: [f64; 3]| {
        std::array::from_fn(|index| interpolate_f64(from[index], to[index], delta, None))
    };
    let perspective =
        std::array::from_fn(|index| interpolate_f64(from.perspective[index], to.perspective[index], delta, None));
    Some(recompose_matrix(DecomposedMatrix {
        translation: interpolate_array(from.translation, to.translation),
        scale: interpolate_array(from.scale, to.scale),
        skew: interpolate_array(from.skew, to.skew),
        rotation: slerp_quaternions(from.rotation, to.rotation, delta),
        perspective,
    }))
}

fn transformation_to_matrix(
    context: Option<&FfiAnimationContext>,
    function: u8,
    arguments: &[RetainedStyleValueData],
) -> Option<Matrix4> {
    let number = |argument: &RetainedStyleValueData| match argument.data() {
        StyleValueData::Number { value } => Some(*value),
        StyleValueData::Percentage { value } => Some(*value / 100.0),
        _ => None,
    };
    let length = |argument: &RetainedStyleValueData, reference_length: Option<f64>| match argument.data() {
        StyleValueData::Length { value, unit } => crate::css::style_compute::absolute_length_to_px(*value, *unit),
        StyleValueData::Percentage { value } => {
            reference_length.map(|reference_length| value / 100.0 * reference_length)
        }
        _ => None,
    };
    let angle = |argument: &RetainedStyleValueData| match argument.data() {
        StyleValueData::Angle { value, unit } => angle_to_degrees(*value, *unit).map(f64::to_radians),
        _ => None,
    };
    let numbers = || arguments.iter().map(number).collect::<Option<Vec<_>>>();
    let reference_box = context
        .filter(|context| context.has_transform_reference_box)
        .map(|context| {
            (
                context.transform_reference_box_width,
                context.transform_reference_box_height,
            )
        });
    let translation_matrix = |x: f64, y: f64, z: f64| {
        let mut matrix = identity_matrix();
        matrix[0][3] = x;
        matrix[1][3] = y;
        matrix[2][3] = z;
        matrix
    };
    let scale_matrix = |x: f64, y: f64, z: f64| {
        let mut matrix = identity_matrix();
        matrix[0][0] = x;
        matrix[1][1] = y;
        matrix[2][2] = z;
        matrix
    };
    let rotation_matrix = |axis: [f64; 3], angle: f64| {
        let axis_length = vector_length(axis);
        if axis_length < 1e-5 {
            return identity_matrix();
        }
        let [x, y, z] = axis.map(|component| component / axis_length);
        let cosine = angle.cos();
        let sine = angle.sin();
        let one_minus_cosine = 1.0 - cosine;
        [
            [
                cosine + x * x * one_minus_cosine,
                x * y * one_minus_cosine - z * sine,
                x * z * one_minus_cosine + y * sine,
                0.0,
            ],
            [
                y * x * one_minus_cosine + z * sine,
                cosine + y * y * one_minus_cosine,
                y * z * one_minus_cosine - x * sine,
                0.0,
            ],
            [
                z * x * one_minus_cosine - y * sine,
                z * y * one_minus_cosine + x * sine,
                cosine + z * z * one_minus_cosine,
                0.0,
            ],
            [0.0, 0.0, 0.0, 1.0],
        ]
    };

    match (function, arguments) {
        (TRANSFORM_FUNCTION_MATRIX, [a, b, c, d, e, f]) => Some([
            [number(a)?, number(c)?, 0.0, number(e)?],
            [number(b)?, number(d)?, 0.0, number(f)?],
            [0.0, 0.0, 1.0, 0.0],
            [0.0, 0.0, 0.0, 1.0],
        ]),
        (TRANSFORM_FUNCTION_MATRIX_3D, values) if values.len() == 16 => {
            let values = numbers()?;
            Some(std::array::from_fn(|row| {
                std::array::from_fn(|column| values[column * 4 + row])
            }))
        }
        (TRANSFORM_FUNCTION_PERSPECTIVE, [argument]) => match argument.data() {
            StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword() => {
                Some(identity_matrix())
            }
            _ => {
                let depth = length(argument, None)?.max(1.0);
                let mut matrix = identity_matrix();
                matrix[3][2] = -1.0 / depth;
                Some(matrix)
            }
        },
        (TRANSFORM_FUNCTION_TRANSLATE | TRANSFORM_FUNCTION_TRANSLATE_X, [x]) => Some(translation_matrix(
            length(x, reference_box.map(|(width, _)| width))?,
            0.0,
            0.0,
        )),
        (TRANSFORM_FUNCTION_TRANSLATE, [x, y]) => Some(translation_matrix(
            length(x, reference_box.map(|(width, _)| width))?,
            length(y, reference_box.map(|(_, height)| height))?,
            0.0,
        )),
        (TRANSFORM_FUNCTION_TRANSLATE_Y, [y]) => Some(translation_matrix(
            0.0,
            length(y, reference_box.map(|(_, height)| height))?,
            0.0,
        )),
        (TRANSFORM_FUNCTION_TRANSLATE_Z, [z]) => Some(translation_matrix(0.0, 0.0, length(z, None)?)),
        (TRANSFORM_FUNCTION_TRANSLATE_3D, [x, y, z]) => Some(translation_matrix(
            length(x, reference_box.map(|(width, _)| width))?,
            length(y, reference_box.map(|(_, height)| height))?,
            length(z, None)?,
        )),
        (TRANSFORM_FUNCTION_SCALE, [value]) => {
            let value = number(value)?;
            Some(scale_matrix(value, value, 1.0))
        }
        (TRANSFORM_FUNCTION_SCALE, [x, y]) => Some(scale_matrix(number(x)?, number(y)?, 1.0)),
        (TRANSFORM_FUNCTION_SCALE_X, [x]) => Some(scale_matrix(number(x)?, 1.0, 1.0)),
        (TRANSFORM_FUNCTION_SCALE_Y, [y]) => Some(scale_matrix(1.0, number(y)?, 1.0)),
        (TRANSFORM_FUNCTION_SCALE_Z, [z]) => Some(scale_matrix(1.0, 1.0, number(z)?)),
        (TRANSFORM_FUNCTION_SCALE_3D, [x, y, z]) => Some(scale_matrix(number(x)?, number(y)?, number(z)?)),
        (TRANSFORM_FUNCTION_ROTATE | TRANSFORM_FUNCTION_ROTATE_Z, [value]) => {
            Some(rotation_matrix([0.0, 0.0, 1.0], angle(value)?))
        }
        (TRANSFORM_FUNCTION_ROTATE_X, [value]) => Some(rotation_matrix([1.0, 0.0, 0.0], angle(value)?)),
        (TRANSFORM_FUNCTION_ROTATE_Y, [value]) => Some(rotation_matrix([0.0, 1.0, 0.0], angle(value)?)),
        (TRANSFORM_FUNCTION_ROTATE_3D, [x, y, z, value]) => {
            Some(rotation_matrix([number(x)?, number(y)?, number(z)?], angle(value)?))
        }
        (TRANSFORM_FUNCTION_SKEW | TRANSFORM_FUNCTION_SKEW_X, [x]) => {
            let mut matrix = identity_matrix();
            matrix[0][1] = angle(x)?.tan();
            Some(matrix)
        }
        (TRANSFORM_FUNCTION_SKEW, [x, y]) => {
            let mut matrix = identity_matrix();
            matrix[0][1] = angle(x)?.tan();
            matrix[1][0] = angle(y)?.tan();
            Some(matrix)
        }
        (TRANSFORM_FUNCTION_SKEW_Y, [y]) => {
            let mut matrix = identity_matrix();
            matrix[1][0] = angle(y)?.tan();
            Some(matrix)
        }
        _ => None,
    }
}

fn matrix_transformation(property: u16, matrix: Matrix4) -> StyleValueData {
    let arguments = (0..16)
        .map(|index| retained_number(matrix[index % 4][index / 4]))
        .collect();
    StyleValueData::Transformation {
        property,
        transform_function: TRANSFORM_FUNCTION_MATRIX_3D,
        values: RetainedStyleValueDataList::from_retained_values(arguments),
    }
}

enum TransformMatrixInterpolationError {
    NotConvertible,
    NonInvertible,
}

fn interpolate_transform_matrix_suffix(
    context: Option<&FfiAnimationContext>,
    property: u16,
    from: &[RetainedStyleValueData],
    to: &[RetainedStyleValueData],
    delta: f32,
) -> Result<RetainedStyleValueData, TransformMatrixInterpolationError> {
    let post_multiply = |transformations: &[RetainedStyleValueData]| {
        let mut result = identity_matrix();
        for transformation in transformations {
            let StyleValueData::Transformation {
                transform_function,
                values,
                ..
            } = transformation.data()
            else {
                return None;
            };
            result = multiply_matrices(
                result,
                transformation_to_matrix(context, *transform_function, values.as_slice())?,
            );
        }
        Some(result)
    };
    let from = post_multiply(from).ok_or(TransformMatrixInterpolationError::NotConvertible)?;
    let to = post_multiply(to).ok_or(TransformMatrixInterpolationError::NotConvertible)?;
    let matrix = interpolate_matrices(from, to, delta).ok_or(TransformMatrixInterpolationError::NonInvertible)?;
    let transformation = Arc::into_raw(Arc::new(matrix_transformation(property, matrix)));
    Ok(unsafe { RetainedStyleValueData::from_retained_pointer(transformation) })
}

fn interpolate_transform_list(
    context: Option<&FfiAnimationContext>,
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> Option<Option<StyleValueData>> {
    if matches!(from, StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword())
        && matches!(to, StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword())
    {
        // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
        // * If both Va and Vb are none:
        //   * Vresult is none.
        return Some(Some(StyleValueData::Keyword {
            keyword: crate::css::style_compute::none_keyword(),
        }));
    }

    let decode_transform_list = |value: &StyleValueData| match value {
        StyleValueData::ValueList {
            values,
            separator,
            collapsible,
        } => Some((
            values
                .as_slice()
                .iter()
                .map(RetainedStyleValueData::clone_retained)
                .collect::<Vec<_>>(),
            *separator,
            *collapsible,
        )),
        StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword() => {
            Some((Vec::new(), 0, false))
        }
        _ => None,
    };
    let (mut from_values, from_separator, from_collapsible) = decode_transform_list(from)?;
    let (mut to_values, to_separator, to_collapsible) = decode_transform_list(to)?;
    let (separator, collapsible) = if from_values.is_empty() {
        (to_separator, to_collapsible)
    } else {
        (from_separator, from_collapsible)
    };
    if !from_values.is_empty()
        && !to_values.is_empty()
        && (from_separator != to_separator || from_collapsible != to_collapsible)
    {
        return None;
    }
    // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
    // * Treating none as a list of zero length, if Va or Vb differ in length:
    //   * extend the shorter list to the length of the longer list, setting the function at each additional
    //     position to the identity transform function matching the function at the corresponding position in the
    //     longer list. Both transform function lists are then interpolated following the next rule.
    if from_values.len() != to_values.len() {
        let (shorter, longer) = if from_values.len() < to_values.len() {
            (&mut from_values, &to_values)
        } else {
            (&mut to_values, &from_values)
        };
        for transformation in &longer[shorter.len()..] {
            let StyleValueData::Transformation {
                property,
                transform_function,
                ..
            } = transformation.data()
            else {
                return None;
            };
            shorter.push(identity_transformation(*property, *transform_function)?);
        }
    }

    // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
    // *  Let Vresult be an empty list. Beginning at the start of Va and Vb, compare the corresponding functions at each
    //    position:
    //   * While the functions have either the same name, or are derivatives of the same primitive transform
    //     function, interpolate the corresponding pair of functions as described in § 10 Interpolation of
    //     primitives and derived transform functions and append the result to Vresult.
    let mut transformations = Vec::with_capacity(from_values.len());
    for (index, (from, to)) in from_values.iter().zip(&to_values).enumerate() {
        let (
            StyleValueData::Transformation {
                property: from_property,
                transform_function: from_function,
                values: from_arguments,
            },
            StyleValueData::Transformation {
                transform_function: to_function,
                values: to_arguments,
                ..
            },
        ) = (from.data(), to.data())
        else {
            return None;
        };
        if from_function == to_function
            && *from_function == TRANSFORM_FUNCTION_PERSPECTIVE
            && index + 1 == from_values.len()
        {
            let ([from_argument], [to_argument]) = (from_arguments.as_slice(), to_arguments.as_slice()) else {
                return None;
            };
            let reciprocal_depth = |argument: &RetainedStyleValueData| match argument.data() {
                StyleValueData::Length { value, unit } => Some((1.0 / value.max(1.0), Some(*unit))),
                StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword() => {
                    Some((0.0, None))
                }
                _ => None,
            };
            let (Some((from_reciprocal_depth, from_unit)), Some((to_reciprocal_depth, to_unit))) =
                (reciprocal_depth(from_argument), reciprocal_depth(to_argument))
            else {
                return None;
            };
            if from_unit.is_some() && to_unit.is_some() && from_unit != to_unit {
                return None;
            }

            // https://drafts.csswg.org/css-transforms-2/#interpolation-of-transform-functions
            // The transform functions <matrix()>, matrix3d() and perspective() get converted into 4x4 matrices first and
            // interpolated as defined in section Interpolation of Matrices afterwards.
            // OPTIMIZATION: A perspective matrix's only varying component is the negative reciprocal of its depth, so
            //               interpolating that component and inverting it produces the same result without materializing
            //               and decomposing two matrices.
            let reciprocal_depth = interpolate_f64(from_reciprocal_depth, to_reciprocal_depth, delta, None);
            let argument = if reciprocal_depth == 0.0 {
                retained_none_keyword()
            } else {
                let value = Arc::into_raw(Arc::new(StyleValueData::Length {
                    value: 1.0 / reciprocal_depth,
                    unit: from_unit
                        .or(to_unit)
                        .unwrap_or_else(crate::css::style_compute::px_length_unit),
                }));
                unsafe { RetainedStyleValueData::from_retained_pointer(value) }
            };
            let transformation = Arc::into_raw(Arc::new(StyleValueData::Transformation {
                property: *from_property,
                transform_function: *from_function,
                values: RetainedStyleValueDataList::from_retained_values(vec![argument]),
            }));
            transformations.push(unsafe { RetainedStyleValueData::from_retained_pointer(transformation) });
            continue;
        }
        let Some((transform_function, from_arguments, to_arguments)) =
            convert_transform_pair_to_common_primitive(*from_function, from_arguments, *to_function, to_arguments)
        else {
            // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
            //   * If the pair do not have a common name or primitive transform function, post-multiply the remaining
            //     transform functions in each of Va and Vb respectively to produce two 4x4 matrices. Interpolate these two
            //     matrices as described in § 11 Interpolation of Matrices, append the result to Vresult, and cease
            //     iterating over Va and Vb.
            let transformation = interpolate_transform_matrix_suffix(
                context,
                *from_property,
                &from_values[index..],
                &to_values[index..],
                delta,
            );
            match transformation {
                Ok(transformation) => transformations.push(transformation),
                Err(TransformMatrixInterpolationError::NotConvertible) => return None,
                Err(TransformMatrixInterpolationError::NonInvertible) => {
                    // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
                    // In some cases, an animation might cause a transformation matrix to be singular or non-invertible.
                    // For example, an animation in which scale moves from 1 to -1. At the time when the matrix is in
                    // such a state, the transformed element is not rendered.
                    // If one of the matrices for interpolation is non-invertible, the used animation function must
                    // fall-back to a discrete animation according to the rules of the respective animation specification.
                    return Some(None);
                }
            }
            break;
        };

        // https://drafts.csswg.org/css-transforms-2/#interpolation-of-transform-functions
        // Two different types of transform functions that share the same primitive, or transform functions of the same
        // type with different number of arguments can be interpolated. Both transform functions need a former
        // conversion to the common primitive first and get interpolated numerically afterwards. The computed value will
        // be the primitive with the resulting interpolated arguments.
        if transform_function == TRANSFORM_FUNCTION_ROTATE_3D {
            let from_arguments = RetainedStyleValueDataList::from_retained_values(
                from_arguments
                    .iter()
                    .map(RetainedStyleValueData::clone_retained)
                    .collect(),
            );
            let to_arguments = RetainedStyleValueDataList::from_retained_values(
                to_arguments
                    .iter()
                    .map(RetainedStyleValueData::clone_retained)
                    .collect(),
            );
            let transformation = interpolate_rotate_3d(
                context,
                *from_property,
                transform_function,
                &from_arguments,
                &to_arguments,
                delta,
            )?;
            let transformation = Arc::into_raw(Arc::new(transformation));
            transformations.push(unsafe { RetainedStyleValueData::from_retained_pointer(transformation) });
            continue;
        }

        let mut arguments = Vec::with_capacity(from_arguments.len());
        for (from, to) in from_arguments.iter().zip(to_arguments) {
            if matches!(
                transform_function,
                TRANSFORM_FUNCTION_TRANSLATE | TRANSFORM_FUNCTION_TRANSLATE_3D
            ) {
                arguments.push(interpolate_translate_component(
                    property_id,
                    from.data(),
                    to.data(),
                    delta,
                )?);
                continue;
            }
            let result = interpolate_scalar_value(property_id, from.data(), to.data(), delta, &[]);
            if !result.handled || result.value.is_null() {
                return None;
            };
            arguments.push(unsafe { RetainedStyleValueData::from_retained_pointer(result.value) });
        }
        let transformation = Arc::into_raw(Arc::new(StyleValueData::Transformation {
            property: *from_property,
            transform_function,
            values: RetainedStyleValueDataList::from_retained_values(arguments),
        }));
        transformations.push(unsafe { RetainedStyleValueData::from_retained_pointer(transformation) });
    }

    Some(Some(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(transformations),
        separator,
        collapsible,
    }))
}

fn retained_legacy_color(red: u8, green: u8, blue: u8, alpha: u8) -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::ColorFunction {
        color_base: ColorBase {
            has_color_type: true,
            color_type: COLOR_TYPE_RGB,
            color_syntax: COLOR_SYNTAX_LEGACY,
        },
        channel_0: retained_number(red as f64),
        channel_1: retained_number(green as f64),
        channel_2: retained_number(blue as f64),
        alpha: retained_number(alpha as f64 / 255.0),
        has_name: false,
        name: empty_retained_fly_string(),
        origin_color: empty_retained_style_value(),
    }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn retained_transparent_legacy_color() -> RetainedStyleValueData {
    retained_legacy_color(0, 0, 0, 0)
}

fn animation_length_resolution_context(
    context: Option<&FfiAnimationContext>,
) -> Option<crate::css::style_compute::FfiLengthResolutionContext> {
    let animation_context = &context
        .filter(|context| context.has_length_resolution_context)?
        .length_resolution_context;
    let font_metrics = |metrics: &FfiAnimationFontMetrics| crate::css::style_compute::FfiFontMetrics {
        font_size: metrics.font_size,
        x_height: metrics.x_height,
        cap_height: metrics.cap_height,
        zero_advance: metrics.zero_advance,
        line_height: metrics.line_height,
    };
    Some(crate::css::style_compute::FfiLengthResolutionContext {
        viewport_width: animation_context.viewport_width,
        viewport_height: animation_context.viewport_height,
        font_metrics: font_metrics(&animation_context.font_metrics),
        root_font_metrics: font_metrics(&animation_context.root_font_metrics),
        font_metrics_depend_on_viewport_metrics: animation_context.font_metrics_depend_on_viewport_metrics,
        root_font_metrics_depend_on_viewport_metrics: animation_context.root_font_metrics_depend_on_viewport_metrics,
        has_container_width_basis: false,
        has_container_height_basis: false,
        container_width_basis: 0.0,
        container_height_basis: 0.0,
        container_width_basis_depends_on_viewport_metrics: false,
        container_height_basis_depends_on_viewport_metrics: false,
        subject_inline_axis_is_horizontal: true,
        resolved_viewport_relative_length: std::ptr::null_mut(),
    })
}

fn resolve_animation_length(context: Option<&FfiAnimationContext>, value: &StyleValueData) -> Option<f64> {
    match value {
        StyleValueData::Length { value, unit } => crate::css::style_compute::absolute_length_to_px(*value, *unit),
        StyleValueData::Calculated { .. } => crate::css::calc::resolve_calculated_length_with_context(
            value,
            &animation_length_resolution_context(context)?,
        ),
        _ => None,
    }
}

fn interpolate_animation_length(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
    range: Option<(f64, f64)>,
) -> Option<RetainedStyleValueData> {
    Some(retained_length(
        interpolate_f64(
            resolve_animation_length(context, from)?,
            resolve_animation_length(context, to)?,
            delta,
            range,
        ),
        crate::css::style_compute::px_length_unit(),
    ))
}

fn resolve_animation_angle(context: Option<&FfiAnimationContext>, value: &StyleValueData) -> Option<f64> {
    match value {
        StyleValueData::Angle { value, unit } => angle_to_degrees(*value, *unit),
        StyleValueData::Calculated { .. } => animation_length_resolution_context(context)
            .and_then(|context| crate::css::calc::resolve_calculated_angle_with_context(value, &context))
            .or_else(|| crate::css::calc::resolve_calculated_angle_without_context(value)),
        _ => None,
    }
}

fn resolve_animation_number(context: Option<&FfiAnimationContext>, value: &StyleValueData) -> Option<f64> {
    match value {
        StyleValueData::Number { value } => Some(*value),
        StyleValueData::Calculated { .. } => animation_length_resolution_context(context)
            .and_then(|context| crate::css::calc::resolve_calculated_number_with_context(value, &context))
            .or_else(|| crate::css::calc::resolve_calculated_number_without_context(value)),
        _ => None,
    }
}

fn resolve_animation_color(
    context: Option<&FfiAnimationContext>,
    color: &RetainedStyleValueData,
) -> Option<RetainedStyleValueData> {
    let use_current_color = match color.optional_data() {
        None => true,
        Some(StyleValueData::Keyword { keyword }) => *keyword == crate::css::style_compute::current_color_keyword(),
        Some(_) => false,
    };
    if !use_current_color {
        return Some(color.clone_retained());
    }
    let current_color = context?.current_color;
    if current_color.is_null() {
        return None;
    }
    let retained = unsafe { crate::css::style_value::retain_style_value(current_color) };
    Some(unsafe { RetainedStyleValueData::from_retained_pointer(retained) })
}

enum RootAnimationColor<'a> {
    Borrowed(&'a StyleValueData),
    Owned(StyleValueData),
}

impl RootAnimationColor<'_> {
    fn data(&self) -> &StyleValueData {
        match self {
            Self::Borrowed(value) => value,
            Self::Owned(value) => value,
        }
    }
}

fn resolve_root_animation_color<'a>(
    context: Option<&'a FfiAnimationContext>,
    color: &'a StyleValueData,
) -> Option<RootAnimationColor<'a>> {
    match color {
        StyleValueData::ColorFunction { .. } => Some(RootAnimationColor::Borrowed(color)),
        StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::current_color_keyword() => {
            let current_color = context?.current_color;
            if current_color.is_null() {
                return None;
            }
            let (components, missing) = legacy_srgb_components(unsafe { &*current_color })?;
            if missing.iter().any(|component| *component) {
                return None;
            }
            let to_byte = |value: f32| (value * 255.0).round().clamp(0.0, 255.0) as u8;
            let alpha = to_byte(components[3]);
            Some(RootAnimationColor::Owned(StyleValueData::ColorFunction {
                color_base: ColorBase {
                    has_color_type: true,
                    color_type: COLOR_TYPE_RGB,
                    color_syntax: COLOR_SYNTAX_LEGACY,
                },
                channel_0: retained_number(to_byte(components[0]) as f64),
                channel_1: retained_number(to_byte(components[1]) as f64),
                channel_2: retained_number(to_byte(components[2]) as f64),
                alpha: retained_number(alpha as f64 / 255.0),
                has_name: false,
                name: empty_retained_fly_string(),
                origin_color: empty_retained_style_value(),
            }))
        }
        _ => None,
    }
}

fn blank_shadow(other: &StyleValueData) -> Option<RetainedStyleValueData> {
    let StyleValueData::Shadow {
        shadow_type, placement, ..
    } = other
    else {
        return None;
    };
    let value = Arc::into_raw(Arc::new(StyleValueData::Shadow {
        shadow_type: *shadow_type,
        color: retained_transparent_legacy_color(),
        offset_x: retained_zero_px(),
        offset_y: retained_zero_px(),
        blur_radius: retained_zero_px(),
        spread_distance: retained_zero_px(),
        placement: *placement,
    }));
    Some(unsafe { RetainedStyleValueData::from_retained_pointer(value) })
}

fn interpolate_shadow(
    context: Option<&FfiAnimationContext>,
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> Option<StyleValueData> {
    let (
        StyleValueData::Shadow {
            shadow_type: from_shadow_type,
            color: from_color,
            offset_x: from_offset_x,
            offset_y: from_offset_y,
            blur_radius: from_blur_radius,
            spread_distance: from_spread_distance,
            placement: from_placement,
        },
        StyleValueData::Shadow {
            shadow_type: to_shadow_type,
            color: to_color,
            offset_x: to_offset_x,
            offset_y: to_offset_y,
            blur_radius: to_blur_radius,
            spread_distance: to_spread_distance,
            placement: to_placement,
        },
    ) = (from, to)
    else {
        return None;
    };
    if from_shadow_type != to_shadow_type {
        return None;
    }

    let from_blur_default = retained_zero_px();
    let to_blur_default = retained_zero_px();
    let from_spread_default = retained_zero_px();
    let to_spread_default = retained_zero_px();
    let from_blur_radius = if from_blur_radius.optional_data().is_some() {
        from_blur_radius
    } else {
        &from_blur_default
    };
    let to_blur_radius = if to_blur_radius.optional_data().is_some() {
        to_blur_radius
    } else {
        &to_blur_default
    };
    let from_spread_distance = if from_spread_distance.optional_data().is_some() {
        from_spread_distance
    } else {
        &from_spread_default
    };
    let to_spread_distance = if to_spread_distance.optional_data().is_some() {
        to_spread_distance
    } else {
        &to_spread_default
    };
    let from_color = resolve_animation_color(context, from_color)?;
    let to_color = resolve_animation_color(context, to_color)?;

    let interpolate = |from: &RetainedStyleValueData, to: &RetainedStyleValueData, ranges: &[NumericRangeOverride]| {
        retained_animation_result(interpolate_scalar_value(
            property_id,
            from.data(),
            to.data(),
            delta,
            ranges,
        ))
    };
    let color = interpolate(&from_color, &to_color, &[])?;
    let offset_x = interpolate_animation_length(context, from_offset_x.data(), to_offset_x.data(), delta, None)
        .or_else(|| interpolate(from_offset_x, to_offset_x, &[]))?;
    let offset_y = interpolate_animation_length(context, from_offset_y.data(), to_offset_y.data(), delta, None)
        .or_else(|| interpolate(from_offset_y, to_offset_y, &[]))?;
    let blur_radius = interpolate_animation_length(
        context,
        from_blur_radius.data(),
        to_blur_radius.data(),
        delta,
        Some((0.0, f64::INFINITY)),
    )
    .or_else(|| interpolate(from_blur_radius, to_blur_radius, NONNEGATIVE_LENGTH_RANGE))?;
    let spread_distance = interpolate_animation_length(
        context,
        from_spread_distance.data(),
        to_spread_distance.data(),
        delta,
        None,
    )
    .or_else(|| interpolate(from_spread_distance, to_spread_distance, &[]))?;

    Some(StyleValueData::Shadow {
        shadow_type: *from_shadow_type,
        color,
        offset_x,
        offset_y,
        blur_radius,
        spread_distance,
        placement: if delta >= 0.5 { *to_placement } else { *from_placement },
    })
}

fn interpolate_shadow_list(
    context: Option<&FfiAnimationContext>,
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    // https://drafts.csswg.org/css-backgrounds/#box-shadow
    // Animation type: by computed value, treating none as a zero-item list and appending blank shadows
    //                 (transparent 0 0 0 0) with a corresponding inset keyword as needed to match the longer list if
    //                 the shorter list is otherwise compatible with the longer one
    let list = |value: &StyleValueData| -> Option<(Vec<RetainedStyleValueData>, u8, bool)> {
        match value {
            StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword() => {
                Some((Vec::new(), 0, false))
            }
            StyleValueData::ValueList {
                values,
                separator,
                collapsible,
            } => Some((
                values
                    .as_slice()
                    .iter()
                    .map(RetainedStyleValueData::clone_retained)
                    .collect(),
                *separator,
                *collapsible,
            )),
            _ => None,
        }
    };
    let Some((mut from_shadows, from_separator, from_collapsible)) = list(from) else {
        return not_handled();
    };
    let Some((mut to_shadows, to_separator, to_collapsible)) = list(to) else {
        return not_handled();
    };
    let from_was_empty = from_shadows.is_empty();

    while from_shadows.len() < to_shadows.len() {
        let Some(shadow) = blank_shadow(to_shadows[from_shadows.len()].data()) else {
            return not_handled();
        };
        from_shadows.push(shadow);
    }
    while to_shadows.len() < from_shadows.len() {
        let Some(shadow) = blank_shadow(from_shadows[to_shadows.len()].data()) else {
            return not_handled();
        };
        to_shadows.push(shadow);
    }

    let mut shadows = Vec::with_capacity(from_shadows.len());
    for (from_shadow, to_shadow) in from_shadows.iter().zip(&to_shadows) {
        let Some(shadow) = interpolate_shadow(context, property_id, from_shadow.data(), to_shadow.data(), delta) else {
            return discrete_value(context, from, to, delta);
        };
        let shadow = Arc::into_raw(Arc::new(shadow));
        shadows.push(unsafe { RetainedStyleValueData::from_retained_pointer(shadow) });
    }

    let use_to_metadata = from_was_empty && !to_shadows.is_empty();
    owned(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(shadows),
        separator: if use_to_metadata { to_separator } else { from_separator },
        collapsible: if use_to_metadata {
            to_collapsible
        } else {
            from_collapsible
        },
    })
}

const FILTER_KIND_BLUR: u8 = 0;
const FILTER_KIND_DROP_SHADOW: u8 = 1;
const FILTER_KIND_HUE_ROTATE: u8 = 2;
const FILTER_KIND_COLOR: u8 = 3;

fn retained_filter(kind: u8, color_operation: u8, value: RetainedStyleValueData) -> RetainedStyleValueData {
    let value = Arc::into_raw(Arc::new(StyleValueData::Filter {
        kind,
        color_operation,
        value,
    }));
    unsafe { RetainedStyleValueData::from_retained_pointer(value) }
}

fn initial_filter_value(
    value: &StyleValueData,
    use_transparent_drop_shadow_color: bool,
) -> Option<RetainedStyleValueData> {
    let StyleValueData::Filter {
        kind,
        color_operation,
        value,
    } = value
    else {
        return None;
    };
    let initial = match *kind {
        FILTER_KIND_BLUR => retained_zero_px(),
        FILTER_KIND_DROP_SHADOW => {
            let StyleValueData::Shadow {
                shadow_type, placement, ..
            } = value.data()
            else {
                return None;
            };
            let shadow = Arc::into_raw(Arc::new(StyleValueData::Shadow {
                shadow_type: *shadow_type,
                color: if use_transparent_drop_shadow_color {
                    retained_transparent_legacy_color()
                } else {
                    empty_retained_style_value()
                },
                offset_x: retained_zero_px(),
                offset_y: retained_zero_px(),
                blur_radius: retained_zero_px(),
                spread_distance: empty_retained_style_value(),
                placement: *placement,
            }));
            unsafe { RetainedStyleValueData::from_retained_pointer(shadow) }
        }
        FILTER_KIND_HUE_ROTATE => {
            let angle = Arc::into_raw(Arc::new(StyleValueData::Angle { value: 0.0, unit: 0 }));
            unsafe { RetainedStyleValueData::from_retained_pointer(angle) }
        }
        FILTER_KIND_COLOR => retained_number(if matches!(*color_operation, 2 | 3 | 6) {
            0.0
        } else {
            1.0
        }),
        _ => return None,
    };
    Some(retained_filter(*kind, *color_operation, initial))
}

// https://drafts.fxtf.org/filter-effects/#interpolation-of-filter-functions
fn interpolate_filter_function(
    context: Option<&FfiAnimationContext>,
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> Option<RetainedStyleValueData> {
    let (
        StyleValueData::Filter {
            kind: from_kind,
            color_operation: from_color_operation,
            value: from_value,
        },
        StyleValueData::Filter {
            kind: to_kind,
            color_operation: to_color_operation,
            value: to_value,
        },
    ) = (from, to)
    else {
        return None;
    };
    if from_kind != to_kind {
        return None;
    }

    let value = match *from_kind {
        FILTER_KIND_BLUR => interpolate_animation_length(
            context,
            from_value.data(),
            to_value.data(),
            delta,
            Some((0.0, f32::MAX as f64)),
        )
        .or_else(|| {
            retained_animation_result(interpolate_scalar_value(
                property_id,
                from_value.data(),
                to_value.data(),
                delta,
                NONNEGATIVE_LENGTH_RANGE,
            ))
        })?,
        FILTER_KIND_DROP_SHADOW => {
            let mut shadow = interpolate_shadow(context, property_id, from_value.data(), to_value.data(), delta)?;
            let (
                StyleValueData::Shadow {
                    blur_radius: from_blur_radius,
                    ..
                },
                StyleValueData::Shadow {
                    blur_radius: to_blur_radius,
                    ..
                },
                StyleValueData::Shadow {
                    blur_radius,
                    spread_distance,
                    ..
                },
            ) = (from_value.data(), to_value.data(), &mut shadow)
            else {
                return None;
            };
            let selected_blur_radius = if delta >= 0.5 { to_blur_radius } else { from_blur_radius };
            if selected_blur_radius.optional_data().is_none() {
                *blur_radius = empty_retained_style_value();
            }
            *spread_distance = empty_retained_style_value();
            let shadow = Arc::into_raw(Arc::new(shadow));
            unsafe { RetainedStyleValueData::from_retained_pointer(shadow) }
        }
        FILTER_KIND_HUE_ROTATE => {
            let from = resolve_animation_angle(context, from_value.data())?;
            let to = resolve_animation_angle(context, to_value.data())?;
            let angle = Arc::into_raw(Arc::new(StyleValueData::Angle {
                value: interpolate_f64(from, to, delta, None),
                unit: 0,
            }));
            unsafe { RetainedStyleValueData::from_retained_pointer(angle) }
        }
        FILTER_KIND_COLOR if from_color_operation == to_color_operation => {
            let ranges = if matches!(*from_color_operation, 2 | 3 | 4 | 6) {
                &[NumericRangeOverride {
                    value_type: VALUE_TYPE_NUMBER,
                    min: 0.0,
                    max: 1.0,
                }][..]
            } else {
                &[NumericRangeOverride {
                    value_type: VALUE_TYPE_NUMBER,
                    min: 0.0,
                    max: f32::MAX as f64,
                }][..]
            };
            retained_animation_result(interpolate_scalar_value(
                property_id,
                from_value.data(),
                to_value.data(),
                delta,
                ranges,
            ))?
        }
        _ => return None,
    };
    Some(retained_filter(*from_kind, *from_color_operation, value))
}

fn accumulate_filter_function(
    context: &FfiAnimationContext,
    underlying: &StyleValueData,
    animated: &StyleValueData,
) -> Option<RetainedStyleValueData> {
    let (
        StyleValueData::Filter {
            kind: underlying_kind,
            color_operation: underlying_color_operation,
            value: underlying_value,
        },
        StyleValueData::Filter {
            kind: animated_kind,
            color_operation: animated_color_operation,
            value: animated_value,
        },
    ) = (underlying, animated)
    else {
        return None;
    };
    if underlying_kind != animated_kind {
        return None;
    }

    let value = match *underlying_kind {
        FILTER_KIND_BLUR => retained_length(
            resolve_animation_length(Some(context), underlying_value.data())?
                + resolve_animation_length(Some(context), animated_value.data())?,
            crate::css::style_compute::px_length_unit(),
        ),
        FILTER_KIND_HUE_ROTATE => {
            let angle = Arc::into_raw(Arc::new(StyleValueData::Angle {
                value: resolve_animation_angle(Some(context), underlying_value.data())?
                    + resolve_animation_angle(Some(context), animated_value.data())?,
                unit: 0,
            }));
            unsafe { RetainedStyleValueData::from_retained_pointer(angle) }
        }
        FILTER_KIND_COLOR if underlying_color_operation == animated_color_operation => {
            let underlying = resolve_animation_number(Some(context), underlying_value.data())?;
            let animated = resolve_animation_number(Some(context), animated_value.data())?;
            retained_number(if matches!(*underlying_color_operation, 0 | 1 | 4 | 5) {
                underlying + animated - 1.0
            } else {
                underlying + animated
            })
        }
        FILTER_KIND_DROP_SHADOW => {
            let (
                StyleValueData::Shadow {
                    shadow_type,
                    color: underlying_color,
                    offset_x: underlying_offset_x,
                    offset_y: underlying_offset_y,
                    blur_radius: underlying_blur_radius,
                    placement,
                    ..
                },
                StyleValueData::Shadow {
                    color: animated_color,
                    offset_x: animated_offset_x,
                    offset_y: animated_offset_y,
                    blur_radius: animated_blur_radius,
                    ..
                },
            ) = (underlying_value.data(), animated_value.data())
            else {
                return None;
            };
            let add_lengths = |underlying: &RetainedStyleValueData, animated: &RetainedStyleValueData| {
                Some(retained_length(
                    resolve_animation_length(Some(context), underlying.data())?
                        + resolve_animation_length(Some(context), animated.data())?,
                    crate::css::style_compute::px_length_unit(),
                ))
            };
            let offset_x = add_lengths(underlying_offset_x, animated_offset_x)?;
            let offset_y = add_lengths(underlying_offset_y, animated_offset_y)?;
            let blur_radius =
                if underlying_blur_radius.optional_data().is_some() || animated_blur_radius.optional_data().is_some() {
                    let underlying = match underlying_blur_radius.optional_data() {
                        Some(value) => resolve_animation_length(Some(context), value)?,
                        None => 0.0,
                    };
                    let animated = match animated_blur_radius.optional_data() {
                        Some(value) => resolve_animation_length(Some(context), value)?,
                        None => 0.0,
                    };
                    retained_length(underlying + animated, crate::css::style_compute::px_length_unit())
                } else {
                    empty_retained_style_value()
                };
            let color_bytes = |color: &RetainedStyleValueData| {
                let color = resolve_animation_color(Some(context), color)?;
                let (components, _) = legacy_srgb_components(color.data())?;
                let byte = |value: f32| (value * 255.0).round().clamp(0.0, 255.0) as u8;
                Some([
                    byte(components[0]),
                    byte(components[1]),
                    byte(components[2]),
                    byte(components[3]),
                ])
            };
            let underlying_color = color_bytes(underlying_color)?;
            let animated_color = color_bytes(animated_color)?;
            let add_color_component =
                |underlying: u8, animated: u8| u16::from(underlying).saturating_add(u16::from(animated)).min(255) as u8;
            let color = retained_legacy_color(
                add_color_component(underlying_color[0], animated_color[0]),
                add_color_component(underlying_color[1], animated_color[1]),
                add_color_component(underlying_color[2], animated_color[2]),
                add_color_component(underlying_color[3], animated_color[3]),
            );
            let shadow = Arc::into_raw(Arc::new(StyleValueData::Shadow {
                shadow_type: *shadow_type,
                color,
                offset_x,
                offset_y,
                blur_radius,
                spread_distance: empty_retained_style_value(),
                placement: *placement,
            }));
            unsafe { RetainedStyleValueData::from_retained_pointer(shadow) }
        }
        _ => return None,
    };
    Some(retained_filter(*underlying_kind, *underlying_color_operation, value))
}

fn composite_filter_list(
    context: &FfiAnimationContext,
    underlying: &StyleValueData,
    animated: &StyleValueData,
    operation: FfiCompositeOperation,
) -> FfiAnimationValueResult {
    if matches!(operation, FfiCompositeOperation::Replace) {
        return handled_without_value();
    }
    let list = |value: &StyleValueData| -> Option<(Vec<RetainedStyleValueData>, u8, bool)> {
        match value {
            StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword() => {
                Some((Vec::new(), 0, false))
            }
            StyleValueData::ValueList {
                values,
                separator,
                collapsible,
            } => Some((
                values
                    .as_slice()
                    .iter()
                    .map(RetainedStyleValueData::clone_retained)
                    .collect(),
                *separator,
                *collapsible,
            )),
            _ => None,
        }
    };
    let Some((mut underlying_filters, underlying_separator, underlying_collapsible)) = list(underlying) else {
        return handled_without_value();
    };
    let Some((mut animated_filters, animated_separator, animated_collapsible)) = list(animated) else {
        return handled_without_value();
    };
    let underlying_was_empty = underlying_filters.is_empty();

    // https://drafts.fxtf.org/filter-effects/#addition
    // Given two filter values representing an base value (base filter list) and a value to add (added filter list),
    // returns the concatenation of the the two lists: ‘base filter list added filter list’.
    if matches!(operation, FfiCompositeOperation::Add) {
        if underlying_filters.is_empty() && animated_filters.is_empty() {
            return handled_without_value();
        }
        let use_animated_metadata = underlying_was_empty;
        underlying_filters.append(&mut animated_filters);
        return owned(StyleValueData::ValueList {
            values: RetainedStyleValueDataList::from_retained_values(underlying_filters),
            separator: if use_animated_metadata {
                animated_separator
            } else {
                underlying_separator
            },
            collapsible: if use_animated_metadata {
                animated_collapsible
            } else {
                underlying_collapsible
            },
        });
    }

    // https://drafts.fxtf.org/filter-effects/#accumulation
    // Accumulation of <filter-value-list>s follows the same matching and extending rules as interpolation, falling
    // back to replace behavior if the lists do not match. However instead of interpolating the matching
    // <filter-function> pairs, their arguments are arithmetically added together - except in the case of
    // <filter-function>s whose initial value for interpolation is 1, which combine using one-based addition:
    // Vresult = Va + Vb - 1
    if !underlying_filters
        .iter()
        .chain(&animated_filters)
        .all(|value| matches!(value.data(), StyleValueData::Filter { .. }))
    {
        return handled_without_value();
    }
    while underlying_filters.len() < animated_filters.len() {
        let Some(value) = initial_filter_value(animated_filters[underlying_filters.len()].data(), false) else {
            return handled_without_value();
        };
        underlying_filters.push(value);
    }
    while animated_filters.len() < underlying_filters.len() {
        let Some(value) = initial_filter_value(underlying_filters[animated_filters.len()].data(), false) else {
            return handled_without_value();
        };
        animated_filters.push(value);
    }
    if underlying_filters.is_empty() {
        return handled_without_value();
    }

    let mut filters = Vec::with_capacity(underlying_filters.len());
    for (underlying, animated) in underlying_filters.iter().zip(&animated_filters) {
        let Some(filter) = accumulate_filter_function(context, underlying.data(), animated.data()) else {
            return handled_without_value();
        };
        filters.push(filter);
    }
    owned(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(filters),
        separator: if underlying_was_empty {
            animated_separator
        } else {
            underlying_separator
        },
        collapsible: if underlying_was_empty {
            animated_collapsible
        } else {
            underlying_collapsible
        },
    })
}

fn interpolate_filter_list(
    context: Option<&FfiAnimationContext>,
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    let list = |value: &StyleValueData| -> Option<(Vec<RetainedStyleValueData>, u8, bool)> {
        match value {
            StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword() => {
                Some((Vec::new(), 0, false))
            }
            StyleValueData::ValueList {
                values,
                separator,
                collapsible,
            } if values
                .as_slice()
                .iter()
                .all(|value| matches!(value.data(), StyleValueData::Filter { .. })) =>
            {
                Some((
                    values
                        .as_slice()
                        .iter()
                        .map(RetainedStyleValueData::clone_retained)
                        .collect(),
                    *separator,
                    *collapsible,
                ))
            }
            _ => None,
        }
    };
    let Some((mut from_filters, from_separator, from_collapsible)) = list(from) else {
        return discrete_value(context, from, to, delta);
    };
    let Some((mut to_filters, to_separator, to_collapsible)) = list(to) else {
        return discrete_value(context, from, to, delta);
    };
    if from_filters.is_empty() && to_filters.is_empty() {
        return discrete_value(context, from, to, delta);
    }
    let from_was_empty = from_filters.is_empty();

    // https://drafts.fxtf.org/filter-effects/#interpolation-of-filters
    // If both filters have a <filter-value-list> of same length without <url> and for each <filter-function> for which there is a corresponding item in each list
    // Interpolate each <filter-function> pair following the rules in section Interpolation of Filter Functions.

    // If both filters have a <filter-value-list> of different length without <url> and for each <filter-function> for which there is a corresponding item in each list

    // 1. Append the missing equivalent <filter-function>s from the longer list to the end of the shorter list. The new added <filter-function>s must be initialized to their initial values for interpolation.
    while from_filters.len() < to_filters.len() {
        let Some(value) = initial_filter_value(to_filters[from_filters.len()].data(), true) else {
            return discrete_value(context, from, to, delta);
        };
        from_filters.push(value);
    }
    while to_filters.len() < from_filters.len() {
        let Some(value) = initial_filter_value(from_filters[to_filters.len()].data(), true) else {
            return discrete_value(context, from, to, delta);
        };
        to_filters.push(value);
    }

    // If one filter is none and the other is a <filter-value-list> without <url>
    //
    // 1. Replace none with the corresponding <filter-value-list> of the other filter. The new <filter-function>s must be initialized to their initial values for interpolation.

    // 2. Interpolate each <filter-function> pair following the rules in section Interpolation of Filter Functions.
    let mut filters = Vec::with_capacity(from_filters.len());
    for (from_filter, to_filter) in from_filters.iter().zip(&to_filters) {
        let Some(filter) =
            interpolate_filter_function(context, property_id, from_filter.data(), to_filter.data(), delta)
        else {
            return discrete_value(context, from, to, delta);
        };
        filters.push(filter);
    }

    let use_to_metadata = from_was_empty && !to_filters.is_empty();
    owned(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(filters),
        separator: if use_to_metadata { to_separator } else { from_separator },
        collapsible: if use_to_metadata {
            to_collapsible
        } else {
            from_collapsible
        },
    })
}

pub(crate) fn interpolate_value(
    context: Option<&FfiAnimationContext>,
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    let animation_type = property_animation_type(property_id);
    if animation_type == ANIMATION_TYPE_NONE {
        // https://www.w3.org/TR/web-animations-1/#not-animatable
        // The property is not animatable. It is not processed when listed in an animation keyframe, and is not affected by transitions.
        // NB: Such values are normally filtered before evaluation. Preserve the C++ scalar API's
        //     existing endpoint behavior if one reaches this lower-level operation.
        return FfiAnimationValueResult {
            value: unsafe { crate::css::style_value::retain_style_value(to) },
            handled: true,
        };
    }
    if animation_type == ANIMATION_TYPE_DISCRETE {
        // https://www.w3.org/TR/web-animations-1/#discrete
        // The property’s values cannot be meaningfully combined, thus it is not additive and interpolation swaps from Va to Vb at 50% (p=0.5), i.e.
        return discrete_value(context, from, to, delta);
    }
    if let (Some(resolved_from), Some(resolved_to)) = (
        resolve_root_animation_color(context, from),
        resolve_root_animation_color(context, to),
    ) {
        // https://drafts.csswg.org/css-color-4/#interpolation
        // Interpolating to or from currentcolor is possible. The numerical value used for this purpose is the used value.
        let result = interpolate_scalar_value(property_id, resolved_from.data(), resolved_to.data(), delta, &[]);
        if result.handled {
            return result;
        }
    }
    if animation_type == ANIMATION_TYPE_CUSTOM
        && matches!(
            property_id,
            crate::css::property_metadata::property_id::FILTER
                | crate::css::property_metadata::property_id::BACKDROP_FILTER
        )
    {
        let result = interpolate_filter_list(context, property_id, from, to, delta);
        if result.handled {
            return result;
        }
    }
    if animation_type == ANIMATION_TYPE_CUSTOM
        && matches!(
            property_id,
            crate::css::property_metadata::property_id::BOX_SHADOW
                | crate::css::property_metadata::property_id::TEXT_SHADOW
        )
    {
        let result = interpolate_shadow_list(context, property_id, from, to, delta);
        if result.handled {
            return result;
        }
    }
    let is_stroke_dasharray = animation_type == ANIMATION_TYPE_CUSTOM
        && property_id == crate::css::property_metadata::property_id::STROKE_DASHARRAY;
    if is_stroke_dasharray
        && (!matches!(from, StyleValueData::ValueList { .. }) || !matches!(to, StyleValueData::ValueList { .. }))
    {
        // https://svgwg.org/svg2-draft/painting.html#StrokeDashing
        // If either start or end compute to none or are invalid, start or end are combined using the discrete animation type.
        return discrete_value(context, from, to, delta);
    }
    if animation_type == ANIMATION_TYPE_REPEATABLE_LIST || is_stroke_dasharray {
        // https://svgwg.org/svg2-draft/painting.html#StrokeDashing
        // Otherwise, repeat both dash patterns of start and end value list until the length of elements in
        // both value lists match. Each item is then combined by computed value.
        // https://drafts.csswg.org/web-animations-1/#repeatable-list
        // Same as by computed value except that if the two lists have differing numbers of items, they are first repeated to the least common multiple number of items.
        // Each item is then combined by computed value.
        // If a pair of values cannot be combined or if any component value uses discrete animation, then the property values combine as discrete.
        let from_list = match from {
            StyleValueData::ValueList {
                values,
                separator,
                collapsible,
            } => Some((values.as_slice(), *separator, *collapsible)),
            _ => None,
        };
        let to_list = match to {
            StyleValueData::ValueList {
                values,
                separator,
                collapsible,
            } => Some((values.as_slice(), *separator, *collapsible)),
            _ => None,
        };
        if from_list.is_none() && to_list.is_none() {
            let result = interpolate_scalar_value(property_id, from, to, delta, &[]);
            if result.handled && !result.value.is_null() {
                return result;
            }
            return discrete_value(context, from, to, delta);
        }

        let (separator, collapsible) = from_list
            .map(|(_, separator, collapsible)| (separator, collapsible))
            .or_else(|| to_list.map(|(_, separator, collapsible)| (separator, collapsible)))
            .expect("at least one repeatable value is a list");
        let from_length = from_list.map_or_else(|| to_list.unwrap().0.len(), |(values, _, _)| values.len());
        let to_length = to_list.map_or_else(|| from_list.unwrap().0.len(), |(values, _, _)| values.len());
        if from_length == 0 || to_length == 0 {
            return owned(StyleValueData::ValueList {
                values: RetainedStyleValueDataList::from_retained_values(Vec::new()),
                separator,
                collapsible,
            });
        }

        let mut a = from_length;
        let mut b = to_length;
        while b != 0 {
            (a, b) = (b, a % b);
        }
        let Some(list_size) = (from_length / a).checked_mul(to_length) else {
            return discrete_value(context, from, to, delta);
        };

        let mut values = Vec::with_capacity(list_size);
        for index in 0..list_size {
            let from_value = from_list.map_or(from, |(values, _, _)| values[index % from_length].data());
            let to_value = to_list.map_or(to, |(values, _, _)| values[index % to_length].data());
            let result = interpolate_scalar_value(property_id, from_value, to_value, delta, &[]);
            if !result.handled {
                return discrete_value(context, from, to, delta);
            }
            if result.value.is_null() {
                return discrete_value(context, from, to, delta);
            }
            values.push(unsafe { RetainedStyleValueData::from_retained_pointer(result.value) });
        }
        return owned(StyleValueData::ValueList {
            values: RetainedStyleValueDataList::from_retained_values(values),
            separator,
            collapsible,
        });
    }
    if animation_type == ANIMATION_TYPE_CUSTOM
        && property_id == crate::css::property_metadata::property_id::FONT_STYLE
        && let (
            StyleValueData::FontStyle {
                font_style: from_font_style,
                angle_value: from_angle,
            },
            StyleValueData::FontStyle {
                font_style: to_font_style,
                angle_value: to_angle,
            },
        ) = (from, to)
    {
        // https://drafts.csswg.org/css-fonts-4/#font-style-prop
        // Animation type: by computed value type; normal animates as oblique 0deg
        let normalize = |font_style: u8, angle: &RetainedStyleValueData| {
            if font_style == FONT_STYLE_NORMAL {
                return Some((FONT_STYLE_OBLIQUE, Some((0.0, 0))));
            }
            match angle.optional_data() {
                Some(StyleValueData::Angle { value, unit }) => Some((font_style, Some((*value, *unit)))),
                Some(_) => None,
                None => Some((font_style, None)),
            }
        };
        let (Some((from_font_style, from_angle)), Some((to_font_style, to_angle))) = (
            normalize(*from_font_style, from_angle),
            normalize(*to_font_style, to_angle),
        ) else {
            return discrete_value(context, from, to, delta);
        };
        let font_style = if from_font_style == to_font_style {
            from_font_style
        } else if !context.is_some_and(|context| context.allow_discrete) {
            return handled_without_value();
        } else if delta < 0.5 {
            from_font_style
        } else {
            to_font_style
        };
        let angle_value = match (from_angle, to_angle) {
            (Some((from_value, from_unit)), Some((to_value, to_unit))) => {
                let (Some(from_value), Some(to_value)) = (
                    angle_to_degrees(from_value, from_unit),
                    angle_to_degrees(to_value, to_unit),
                ) else {
                    return discrete_value(context, from, to, delta);
                };
                let angle = Arc::into_raw(Arc::new(StyleValueData::Angle {
                    value: interpolate_f64(from_value, to_value, delta, Some((-90.0, 90.0))),
                    unit: 0,
                }));
                unsafe { RetainedStyleValueData::from_retained_pointer(angle) }
            }
            _ => unsafe { RetainedStyleValueData::from_retained_optional_pointer(std::ptr::null()) },
        };
        return owned(StyleValueData::FontStyle {
            font_style,
            angle_value,
        });
    }
    if animation_type == ANIMATION_TYPE_CUSTOM && property_id == crate::css::property_metadata::property_id::VISIBILITY
    {
        let result = interpolate_visibility(context, from, to, delta);
        if result.handled {
            return result;
        }
    }
    if animation_type == ANIMATION_TYPE_CUSTOM
        && property_id == crate::css::property_metadata::property_id::CONTENT_VISIBILITY
    {
        let result = interpolate_content_visibility(context, from, to, delta);
        if result.handled {
            return result;
        }
    }
    if animation_type == ANIMATION_TYPE_CUSTOM && property_id == crate::css::property_metadata::property_id::DISPLAY {
        let result = interpolate_display(context, from, to, delta);
        if result.handled {
            return result;
        }
    }
    if animation_type == ANIMATION_TYPE_CUSTOM && property_id == crate::css::property_metadata::property_id::SCALE {
        let result = interpolate_scale(from, to, delta);
        if result.handled {
            return result;
        }
    }
    if animation_type == ANIMATION_TYPE_CUSTOM && property_id == crate::css::property_metadata::property_id::TRANSLATE {
        let result = interpolate_translate(from, to, delta);
        if result.handled {
            return result;
        }
    }
    if animation_type == ANIMATION_TYPE_CUSTOM && property_id == crate::css::property_metadata::property_id::ROTATE {
        let result = interpolate_individual_rotate(context, from, to, delta);
        if result.handled {
            return result;
        }
    }
    if animation_type == ANIMATION_TYPE_CUSTOM
        && property_id == crate::css::property_metadata::property_id::FONT_VARIATION_SETTINGS
    {
        let result = interpolate_font_variation_settings(context, property_id, from, to, delta);
        if result.handled {
            return result;
        }
    }
    if animation_type == ANIMATION_TYPE_CUSTOM
        && property_id == crate::css::property_metadata::property_id::TRANSFORM
        && let Some(value) = interpolate_transform_list(context, property_id, from, to, delta)
    {
        return value.map_or_else(
            || {
                if context.is_some_and(|context| context.allow_discrete) {
                    discrete_value(context, from, to, delta)
                } else {
                    handled_without_value()
                }
            },
            owned,
        );
    }
    if animation_type == ANIMATION_TYPE_CUSTOM
        && matches!(
            property_id,
            crate::css::property_metadata::property_id::GRID_TEMPLATE_COLUMNS
                | crate::css::property_metadata::property_id::GRID_TEMPLATE_ROWS
        )
        && let (
            StyleValueData::GridTrackSizeList {
                is_subgrid: from_is_subgrid,
                entries: from_entries,
                ..
            },
            StyleValueData::GridTrackSizeList {
                is_subgrid: to_is_subgrid,
                entries: to_entries,
                ..
            },
        ) = (from, to)
    {
        let Some(entries) = interpolate_grid_track_entries(
            property_id,
            *from_is_subgrid,
            from_entries.as_slice(),
            *to_is_subgrid,
            to_entries.as_slice(),
            delta,
        ) else {
            return discrete_value(context, from, to, delta);
        };
        return owned(StyleValueData::GridTrackSizeList {
            is_subgrid: false,
            preserve_line_name_sets: false,
            entries: RetainedGridTrackEntryList::from_retained_entries(entries),
        });
    }
    if animation_type == ANIMATION_TYPE_CUSTOM {
        // NB: C++ treats values declined by a property's specialized custom algorithm as discrete.
        return discrete_value(context, from, to, delta);
    }
    assert_eq!(animation_type, ANIMATION_TYPE_BY_COMPUTED_VALUE);
    let result = interpolate_scalar_value(property_id, from, to, delta, &[]);
    // https://drafts.csswg.org/web-animations-1/#by-computed-value
    // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
    if !result.handled || result.value.is_null() && context.is_some_and(|context| context.allow_discrete) {
        return discrete_value(context, from, to, delta);
    }
    result
}

enum BatchAnimationValue<'a> {
    Borrowed(&'a StyleValueData),
    Owned(Arc<StyleValueData>),
}

impl BatchAnimationValue<'_> {
    fn data(&self) -> &StyleValueData {
        match self {
            Self::Borrowed(value) => value,
            Self::Owned(value) => value,
        }
    }
}

fn composite_batch_value<'a>(
    context: &FfiAnimationContext,
    property_id: u16,
    underlying: &'a StyleValueData,
    animated: &'a StyleValueData,
    operation: FfiCompositeOperation,
) -> BatchAnimationValue<'a> {
    let result = if matches!(
        property_id,
        crate::css::property_metadata::property_id::FILTER
            | crate::css::property_metadata::property_id::BACKDROP_FILTER
    ) {
        composite_filter_list(context, underlying, animated, operation)
    } else {
        composite_scalar_value(underlying, animated, operation)
    };
    assert!(result.handled);
    if result.value.is_null() {
        return BatchAnimationValue::Borrowed(animated);
    }
    // SAFETY: A handled non-null composition result transfers one Arc reference.
    BatchAnimationValue::Owned(unsafe { Arc::from_raw(result.value) })
}

fn evaluate_animation_value(
    context: &FfiAnimationContext,
    input: &FfiAnimationValueInput,
    underlying_override: Option<&StyleValueData>,
) -> FfiAnimatedProperty {
    let keyframes = unsafe { std::slice::from_raw_parts(input.keyframes, input.keyframe_count) };
    assert!(keyframes.len() >= 2);

    // https://drafts.csswg.org/web-animations-1/#the-effect-value-of-a-keyframe-animation-effect
    // 10. Let interval endpoints be an empty sequence of keyframes.
    // 11. Populate interval endpoints by following the steps from the first matching condition from below:
    // Otherwise,
    // 1. Append to interval endpoints the last keyframe in property-specific keyframes whose computed keyframe offset is less than or equal
    //    to iteration progress and less than 1. If there is no such keyframe (because, for example, the iteration progress is negative),
    //    add the last keyframe whose computed keyframe offset is 0.
    // 2. Append to interval endpoints the next keyframe in property-specific keyframes after the one added in the previous step.
    let mut start_index = 0;
    let mut end_index = 1;
    for next_index in 2..keyframes.len() {
        if input.current_key < keyframes[end_index].key as f64 {
            break;
        }
        start_index = end_index;
        end_index = next_index;
    }
    let start_keyframe = &keyframes[start_index];
    let end_keyframe = &keyframes[end_index];
    let interval_progress =
        (input.current_key - start_keyframe.key as f64) / (end_keyframe.key - start_keyframe.key) as f64;
    let progress = evaluate_easing_descriptor(&start_keyframe.easing, interval_progress, false) as f32;
    if end_keyframe.value.is_null() {
        if start_keyframe.value.is_null() {
            return FfiAnimatedProperty {
                property_id: input.property_id,
                value: std::ptr::null(),
                progress,
                start_index,
                end_index,
                handled: true,
                apply: false,
            };
        }
        return FfiAnimatedProperty {
            property_id: input.property_id,
            value: unsafe { crate::css::style_value::retain_style_value(start_keyframe.value) },
            progress,
            start_index,
            end_index,
            handled: true,
            apply: true,
        };
    }
    let underlying = underlying_override.unwrap_or_else(|| unsafe { &*input.underlying });
    let start = unsafe {
        if start_keyframe.value.is_null() {
            &*input.initial
        } else {
            &*start_keyframe.value
        }
    };
    let end = unsafe { &*end_keyframe.value };
    let start = composite_batch_value(context, input.property_id, underlying, start, start_keyframe.composite);
    let end = composite_batch_value(context, input.property_id, underlying, end, end_keyframe.composite);
    let result = interpolate_value(Some(context), input.property_id, start.data(), end.data(), progress);
    assert!(result.handled);
    FfiAnimatedProperty {
        property_id: input.property_id,
        value: result.value,
        progress,
        start_index,
        end_index,
        handled: true,
        apply: true,
    }
}

/// Resolve an element's keyframe declarations into one owned batch for C++ computation.
///
/// # Safety
/// `batch` must point to a live value whose declaration and bitmap ranges remain live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_resolve_animation_declarations(
    batch: *const FfiAnimationBatch,
) -> FfiResolvedAnimationProperties {
    crate::abort_on_panic(|| {
        let batch = unsafe { &*batch };
        let declarations = unsafe { std::slice::from_raw_parts(batch.declarations, batch.declaration_count) };
        let important_property_bitmap = unsafe {
            std::slice::from_raw_parts(batch.important_property_bitmap, batch.important_property_bitmap_length)
        };
        let resolved = resolve_animation_declarations(
            declarations,
            batch.writing_mode,
            batch.direction,
            important_property_bitmap,
        );
        if resolved.properties.is_empty() {
            return FfiResolvedAnimationProperties {
                properties: std::ptr::null(),
                count: 0,
                uses_tree_counting_function: false,
                container_relative_length_unit_mask: 0,
                needs_document_base_url: false,
                unfixed_random_sharings: std::ptr::null(),
                unfixed_random_sharing_count: 0,
                storage: std::ptr::null_mut(),
            };
        }
        let resolved = Box::new(resolved);
        let properties = resolved.properties.as_ptr();
        let count = resolved.properties.len();
        FfiResolvedAnimationProperties {
            properties,
            count,
            uses_tree_counting_function: resolved.uses_tree_counting_function,
            container_relative_length_unit_mask: resolved.container_relative_length_unit_mask,
            needs_document_base_url: resolved.needs_document_base_url,
            unfixed_random_sharings: resolved.unfixed_random_sharings.as_ptr(),
            unfixed_random_sharing_count: resolved.unfixed_random_sharings.len(),
            storage: Box::into_raw(resolved).cast(),
        }
    })
}

/// # Safety
/// `storage` must be null or a live pointer returned by `rust_resolve_animation_declarations`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_resolved_animation_properties_destroy(storage: *mut std::ffi::c_void) {
    if !storage.is_null() {
        drop(unsafe { Box::from_raw(storage.cast::<ResolvedAnimationDeclarations>()) });
    }
}

/// Evaluate and compose every prepared animation interval without consulting C++ or the DOM.
/// Applied results are stored directly in the Rust-owned animated overlay.
///
/// # Safety
/// `computed` must point to a live batch whose input style values remain live for the call. Its
/// `overlay` must point at a live, uniquely owned animated overlay.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_evaluate_animations(computed: *const FfiComputedAnimationBatch) -> usize {
    crate::abort_on_panic(|| {
        crate::css::ffi_stats::rust_style_ffi_note_animation_evaluation();
        let computed = unsafe { &*computed };
        if computed.value_count == 0 {
            return 0;
        }
        let inputs = unsafe { std::slice::from_raw_parts(computed.values, computed.value_count) };
        let overlay = unsafe { &mut *computed.overlay.cast::<crate::css::animated_overlay::AnimatedOverlay>() };

        // https://www.w3.org/TR/web-animations-1/#effect-stacks
        // NB: Inputs arrive in composite order. Keep each result as the underlying value for the
        //     next effect affecting the same property.
        let mut previous_values = Vec::<(u16, *const StyleValueData)>::new();
        for input in inputs {
            let previous_value = previous_values
                .iter()
                .rev()
                .find(|(property_id, _)| *property_id == input.property_id)
                .map(|(_, value)| unsafe { &**value });
            let result = evaluate_animation_value(&computed.context, input, previous_value);
            if !result.apply {
                assert!(result.value.is_null());
                continue;
            }
            if !result.value.is_null() {
                previous_values.push((input.property_id, result.value));
                let value = unsafe { RetainedStyleValueData::from_retained_pointer(result.value) };
                overlay.set_owned(input.property_id, value, false, input.result_of_transition);
            } else {
                let value = RetainedStyleValueData::from_owned(StyleValueData::Keyword {
                    keyword: crate::css::css_enums::keyword::HIDDEN,
                });
                overlay.set_owned(
                    crate::css::property_metadata::property_id::VISIBILITY,
                    value,
                    false,
                    input.result_of_transition,
                );
            }
        }
        overlay.refresh_ffi_entries();
        inputs.len()
    })
}

/// Attempt Rust-owned style value interpolation without consulting C++ or the DOM.
///
/// # Safety
/// `context` must be null or point at a live `FfiAnimationContext`. `from` and `to` must point at live
/// `StyleValueData` allocations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_interpolate_scalar_style_value(
    context: *const FfiAnimationContext,
    property_id: u16,
    from: *const StyleValueData,
    to: *const StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    crate::abort_on_panic(|| {
        interpolate_value(
            unsafe { context.as_ref() },
            property_id,
            unsafe { &*from },
            unsafe { &*to },
            delta,
        )
    })
}

/// Test-only bridge for exercising Rust-owned style value composition without constructing an
/// animation batch. Production animation evaluation uses `rust_evaluate_animations`.
///
/// # Safety
/// `underlying` and `animated` must point at live `StyleValueData` allocations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_test_composite_style_value(
    underlying: *const StyleValueData,
    animated: *const StyleValueData,
    operation: FfiCompositeOperation,
) -> FfiAnimationValueResult {
    crate::abort_on_panic(|| composite_scalar_value(unsafe { &*underlying }, unsafe { &*animated }, operation))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resolves_keyframe_property_conflicts_as_one_batch() {
        use crate::css::property_metadata::property_id;
        let value = || {
            let pointer = Arc::into_raw(Arc::new(StyleValueData::Number { value: 1.0 }));
            unsafe { RetainedStyleValueData::from_retained_pointer(pointer) }
        };
        let candidates = [
            AnimationPropertyConflictCandidate {
                keyframe_index: 0,
                physical_property_id: property_id::BORDER_TOP_COLOR,
                source_property_id: property_id::BORDER,
                source_longhand_id: property_id::BORDER_TOP_COLOR,
                value: value(),
                style_sheet_resource_context: FfiAnimationStyleSheetResourceContext::empty(),
                use_initial: false,
                suppressed_by_important: false,
                is_transition: false,
            },
            AnimationPropertyConflictCandidate {
                keyframe_index: 0,
                physical_property_id: property_id::BORDER_TOP_COLOR,
                source_property_id: property_id::BORDER_TOP,
                source_longhand_id: property_id::BORDER_TOP_COLOR,
                value: value(),
                style_sheet_resource_context: FfiAnimationStyleSheetResourceContext::empty(),
                use_initial: false,
                suppressed_by_important: false,
                is_transition: false,
            },
            AnimationPropertyConflictCandidate {
                keyframe_index: 0,
                physical_property_id: property_id::BORDER_TOP_COLOR,
                source_property_id: property_id::BORDER_TOP_COLOR,
                source_longhand_id: property_id::BORDER_TOP_COLOR,
                value: value(),
                style_sheet_resource_context: FfiAnimationStyleSheetResourceContext::empty(),
                use_initial: false,
                suppressed_by_important: false,
                is_transition: false,
            },
            AnimationPropertyConflictCandidate {
                keyframe_index: 0,
                physical_property_id: property_id::BORDER_TOP_COLOR,
                source_property_id: property_id::BORDER_TOP_COLOR,
                source_longhand_id: property_id::BORDER_TOP_COLOR,
                value: value(),
                style_sheet_resource_context: FfiAnimationStyleSheetResourceContext::empty(),
                use_initial: true,
                suppressed_by_important: false,
                is_transition: false,
            },
            AnimationPropertyConflictCandidate {
                keyframe_index: 1,
                physical_property_id: property_id::BORDER_TOP_COLOR,
                source_property_id: property_id::BORDER,
                source_longhand_id: property_id::BORDER_TOP_COLOR,
                value: value(),
                style_sheet_resource_context: FfiAnimationStyleSheetResourceContext::empty(),
                use_initial: true,
                suppressed_by_important: false,
                is_transition: false,
            },
        ];
        let mut selected = [false; 5];
        let mut value_sources = [FfiAnimationSpecifiedValueSource::Value; 5];
        resolve_animation_property_conflicts(&candidates, &mut selected, &mut value_sources);
        assert_eq!(selected, [false, false, true, false, true]);
    }

    #[test]
    fn resolves_animation_css_wide_keywords() {
        use crate::css::property_metadata::property_id;
        use crate::css::style_compute::keyword;
        let keyword_value = |keyword| StyleValueData::Keyword { keyword };

        assert_eq!(
            animation_specified_value_source(&keyword_value(keyword::INHERIT), property_id::MARGIN_LEFT),
            FfiAnimationSpecifiedValueSource::Inherited
        );
        assert_eq!(
            animation_specified_value_source(&keyword_value(keyword::UNSET), property_id::COLOR),
            FfiAnimationSpecifiedValueSource::Inherited
        );
        assert_eq!(
            animation_specified_value_source(&keyword_value(keyword::UNSET), property_id::MARGIN_LEFT),
            FfiAnimationSpecifiedValueSource::Initial
        );
        assert_eq!(
            animation_specified_value_source(&keyword_value(keyword::INITIAL), property_id::COLOR),
            FfiAnimationSpecifiedValueSource::Initial
        );
        assert_eq!(
            animation_specified_value_source(&keyword_value(keyword::REVERT), property_id::COLOR),
            FfiAnimationSpecifiedValueSource::Underlying
        );
        assert_eq!(
            animation_specified_value_source(&keyword_value(keyword::REVERT_LAYER), property_id::COLOR),
            FfiAnimationSpecifiedValueSource::Underlying
        );
    }

    #[test]
    fn retains_synthesized_pending_substitution_values() {
        let original = Arc::into_raw(Arc::new(StyleValueData::Number { value: 1.0 }));
        let pending = Arc::new(StyleValueData::PendingSubstitution {
            original_shorthand_value: unsafe { RetainedStyleValueData::from_retained_pointer(original) },
        });
        let declaration = FfiAnimationDeclaration {
            keyframe_index: 0,
            property_id: crate::css::property_metadata::property_id::BORDER,
            value: &raw const *pending,
            style_sheet_resource_context: FfiAnimationStyleSheetResourceContext::empty(),
            use_initial: false,
            is_transition: false,
        };
        let resolved = resolve_animation_declarations(&[declaration], 0, 0, &[]);
        assert!(!resolved.properties.is_empty());
        let mut found_synthesized = false;
        for property in &resolved.properties {
            let value = unsafe { &*property.value };
            if let StyleValueData::PendingSubstitution {
                original_shorthand_value,
            } = value
                && matches!(
                    original_shorthand_value.data(),
                    StyleValueData::PendingSubstitution { .. }
                )
            {
                found_synthesized = true;
            }
        }
        assert!(found_synthesized);
    }

    #[test]
    fn reads_important_property_snapshot() {
        use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, property_id};
        let important_index = usize::from(property_id::COLOR - FIRST_LONGHAND_PROPERTY_ID);
        let mut bitmap = vec![0; important_index / 8 + 1];
        bitmap[important_index / 8] |= 1 << (important_index % 8);
        assert!(property_is_important(property_id::COLOR, &bitmap));
        assert!(!property_is_important(property_id::MARGIN_LEFT, &bitmap));
        assert!(animation_property_is_suppressed(false, property_id::COLOR, &bitmap));
        assert!(!animation_property_is_suppressed(true, property_id::COLOR, &bitmap));
        assert!(!property_is_important(FIRST_LONGHAND_PROPERTY_ID - 1, &bitmap));
    }

    fn animation_context(allow_discrete: bool) -> FfiAnimationContext {
        let font_metrics = || FfiAnimationFontMetrics {
            font_size: 0.0,
            x_height: 0.0,
            cap_height: 0.0,
            zero_advance: 0.0,
            line_height: 0.0,
        };
        FfiAnimationContext {
            allow_discrete,
            current_color: std::ptr::null(),
            has_length_resolution_context: false,
            length_resolution_context: FfiAnimationLengthResolutionContext {
                viewport_width: 0.0,
                viewport_height: 0.0,
                font_metrics: font_metrics(),
                root_font_metrics: font_metrics(),
                font_metrics_depend_on_viewport_metrics: false,
                root_font_metrics_depend_on_viewport_metrics: false,
            },
            has_transform_reference_box: false,
            transform_reference_box_width: 0.0,
            transform_reference_box_height: 0.0,
        }
    }

    fn calculated_number(value: f64) -> StyleValueData {
        StyleValueData::Calculated {
            rust_calculation: crate::css::calc::CalcNodeHandle::from_arc(Arc::new(
                crate::css::calc::CalcNode::Numeric(crate::css::calc::CalcNumericValue::Number {
                    value,
                    number_type: 0,
                }),
            )),
            resolve_as_is_number: false,
            resolve_as_base: 0,
            resolved_type: crate::css::calc::FfiNumericType::from_calc(Some(
                crate::css::calc::CalcNumericType::default(),
            )),
            has_percentages_resolve_as: false,
            percentages_resolve_as: 0,
            resolve_numbers_as_integers: false,
            accepted_ranges: RetainedNumericRangeList::empty(),
        }
    }

    #[test]
    fn evaluates_linear_easing() {
        let points = [
            FfiLinearEasingPoint {
                input: 0.0,
                output: 0.0,
            },
            FfiLinearEasingPoint {
                input: 0.0,
                output: 0.5,
            },
            FfiLinearEasingPoint {
                input: 1.0,
                output: 1.0,
            },
        ];
        assert_eq!(evaluate_linear_easing(&points, 0.0, true), 0.0);
        assert_eq!(evaluate_linear_easing(&points, 0.0, false), 0.5);
        assert_eq!(evaluate_linear_easing(&points, 0.5, false), 0.75);
    }

    #[test]
    fn evaluates_cubic_bezier_easing() {
        assert!((evaluate_cubic_bezier_easing(0.42, 0.0, 0.58, 1.0, 0.5) - 0.5).abs() < 1e-7);
        assert_eq!(evaluate_cubic_bezier_easing(0.5, 1.0, 1.0, 1.0, -0.5), -1.0);
    }

    #[test]
    fn evaluates_steps_easing() {
        assert_eq!(evaluate_steps_easing(4, 1, 0.5, false), 0.5);
        assert_eq!(evaluate_steps_easing(4, 1, 0.5, true), 0.25);
        assert_eq!(evaluate_steps_easing(4, STEP_POSITION_JUMP_START, 0.0, false), 0.25);
    }

    #[test]
    fn evaluates_composited_animation_value() {
        let underlying = StyleValueData::Number { value: 2.0 };
        let start = StyleValueData::Number { value: 3.0 };
        let middle = StyleValueData::Number { value: 5.0 };
        let end = StyleValueData::Number { value: 9.0 };
        let linear_easing = || FfiEasingDescriptor {
            kind: FfiEasingKind::CubicBezier,
            linear_points: std::ptr::null(),
            linear_point_count: 0,
            x1: 0.0,
            y1: 0.0,
            x2: 1.0,
            y2: 1.0,
            interval_count: 0,
            step_position: 0,
        };
        let keyframes = [
            FfiAnimationKeyframeValue {
                key: 0,
                value: &raw const start,
                easing: linear_easing(),
                composite: FfiCompositeOperation::Add,
            },
            FfiAnimationKeyframeValue {
                key: 50,
                value: &raw const middle,
                easing: linear_easing(),
                composite: FfiCompositeOperation::Add,
            },
            FfiAnimationKeyframeValue {
                key: 100,
                value: &raw const end,
                easing: linear_easing(),
                composite: FfiCompositeOperation::Add,
            },
        ];
        let input = FfiAnimationValueInput {
            property_id: crate::css::property_metadata::property_id::FLEX_GROW,
            result_of_transition: false,
            underlying: &raw const underlying,
            initial: &raw const underlying,
            current_key: 75.0,
            keyframes: keyframes.as_ptr(),
            keyframe_count: keyframes.len(),
        };
        let result = evaluate_animation_value(&animation_context(true), &input, None);
        assert!(result.handled);
        assert!(result.apply);
        assert!(!result.value.is_null());
        assert_eq!(result.start_index, 1);
        assert_eq!(result.end_index, 2);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Number { value } if *value == 9.0));
    }

    #[test]
    fn evaluates_missing_keyframe_values() {
        let underlying = Arc::new(StyleValueData::Number { value: 2.0 });
        let initial = Arc::new(StyleValueData::Number { value: 1.0 });
        let start = Arc::new(StyleValueData::Number { value: 3.0 });
        let end = Arc::new(StyleValueData::Number { value: 5.0 });
        let easing = || FfiEasingDescriptor {
            kind: FfiEasingKind::CubicBezier,
            linear_points: std::ptr::null(),
            linear_point_count: 0,
            x1: 0.0,
            y1: 0.0,
            x2: 1.0,
            y2: 1.0,
            interval_count: 0,
            step_position: 0,
        };
        let evaluate = |start_value, end_value| {
            let keyframes = [
                FfiAnimationKeyframeValue {
                    key: 0,
                    value: start_value,
                    easing: easing(),
                    composite: FfiCompositeOperation::Replace,
                },
                FfiAnimationKeyframeValue {
                    key: 100,
                    value: end_value,
                    easing: easing(),
                    composite: FfiCompositeOperation::Replace,
                },
            ];
            let input = FfiAnimationValueInput {
                property_id: crate::css::property_metadata::property_id::FLEX_GROW,
                result_of_transition: false,
                underlying: Arc::as_ptr(&underlying),
                initial: Arc::as_ptr(&initial),
                current_key: 50.0,
                keyframes: keyframes.as_ptr(),
                keyframe_count: keyframes.len(),
            };
            evaluate_animation_value(&animation_context(true), &input, None)
        };

        let result = evaluate(std::ptr::null(), Arc::as_ptr(&end));
        assert!(result.handled);
        assert!(result.apply);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Number { value } if *value == 3.0));

        let result = evaluate(Arc::as_ptr(&start), std::ptr::null());
        assert!(result.handled);
        assert!(result.apply);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Number { value } if *value == 3.0));

        let result = evaluate(std::ptr::null(), std::ptr::null());
        assert!(result.handled);
        assert!(!result.apply);
        assert!(result.value.is_null());
    }

    #[test]
    fn evaluates_discrete_animation_values() {
        let from = Arc::new(StyleValueData::Keyword { keyword: 1 });
        let to = Arc::new(StyleValueData::Keyword { keyword: 2 });
        let property_id = crate::css::property_metadata::property_id::ALIGN_CONTENT;

        let result = interpolate_value(Some(&animation_context(true)), property_id, &from, &to, 0.25);
        assert!(result.handled);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Keyword { keyword: 1 }));

        let result = interpolate_value(Some(&animation_context(true)), property_id, &from, &to, 0.75);
        assert!(result.handled);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Keyword { keyword: 2 }));

        let result = interpolate_value(Some(&animation_context(false)), property_id, &from, &to, 0.75);
        assert!(result.handled);
        assert!(result.value.is_null());
    }

    #[test]
    fn evaluates_incompatible_by_computed_values_as_discrete() {
        let property_id = crate::css::property_metadata::property_id::FLEX_GROW;
        let from = Arc::new(StyleValueData::Keyword { keyword: 1 });
        let to = Arc::new(StyleValueData::Keyword { keyword: 2 });

        let result = interpolate_value(Some(&animation_context(true)), property_id, &from, &to, 0.25);
        assert!(result.handled);
        assert_eq!(result.value, Arc::as_ptr(&from));
        unsafe { crate::css::style_value::release_style_value(result.value) };

        let result = interpolate_value(Some(&animation_context(true)), property_id, &from, &to, 0.75);
        assert!(result.handled);
        assert_eq!(result.value, Arc::as_ptr(&to));
        unsafe { crate::css::style_value::release_style_value(result.value) };

        let result = interpolate_value(Some(&animation_context(false)), property_id, &from, &to, 0.75);
        assert!(result.handled);
        assert!(result.value.is_null());
    }

    #[test]
    fn preserves_not_animatable_endpoint_behavior() {
        let from = Arc::new(StyleValueData::Keyword { keyword: 1 });
        let to = Arc::new(StyleValueData::Keyword { keyword: 2 });
        let result = interpolate_value(
            Some(&animation_context(true)),
            crate::css::property_metadata::property_id::ANIMATION_DURATION,
            &from,
            &to,
            0.25,
        );
        assert!(result.handled);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Keyword { keyword: 2 }));
    }

    #[test]
    fn evaluates_declined_custom_animation_values_as_discrete() {
        let from = Arc::new(StyleValueData::Keyword { keyword: 1 });
        let to = Arc::new(StyleValueData::Keyword { keyword: 2 });
        let property_id = crate::css::property_metadata::property_id::SCALE;

        let result = interpolate_value(Some(&animation_context(true)), property_id, &from, &to, 0.75);
        assert!(result.handled);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Keyword { keyword: 2 }));

        let result = interpolate_value(Some(&animation_context(false)), property_id, &from, &to, 0.75);
        assert!(result.handled);
        assert!(result.value.is_null());
    }

    #[test]
    fn normalizes_repeatable_scalar_and_list_values() {
        let property_id = crate::css::property_metadata::property_id::OBJECT_POSITION;
        let from = Arc::new(StyleValueData::Number { value: 10.0 });
        let to = Arc::new(StyleValueData::ValueList {
            values: RetainedStyleValueDataList::from_retained_values(vec![
                retained_number(20.0),
                retained_number(30.0),
            ]),
            separator: 1,
            collapsible: false,
        });

        let result = interpolate_value(Some(&animation_context(true)), property_id, &from, &to, 0.5);
        assert!(result.handled);
        let value = unsafe { Arc::from_raw(result.value) };
        let StyleValueData::ValueList { values, .. } = &*value else {
            panic!("expected a repeatable value list");
        };
        assert!(matches!(values.as_slice()[0].data(), StyleValueData::Number { value } if *value == 15.0));
        assert!(matches!(values.as_slice()[1].data(), StyleValueData::Number { value } if *value == 20.0));
    }

    #[test]
    fn interpolates_repeatable_scalar_values() {
        let from = Arc::new(StyleValueData::Number { value: 10.0 });
        let to = Arc::new(StyleValueData::Number { value: 20.0 });
        let result = interpolate_value(
            Some(&animation_context(true)),
            crate::css::property_metadata::property_id::OBJECT_POSITION,
            &from,
            &to,
            0.5,
        );
        assert!(result.handled);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Number { value } if *value == 15.0));
    }

    #[test]
    fn interpolates_mixed_length_percentage_values() {
        let from = Arc::new(StyleValueData::Length { value: 10.0, unit: 0 });
        let to = Arc::new(StyleValueData::Percentage { value: 50.0 });
        let result = interpolate_value(
            Some(&animation_context(true)),
            crate::css::property_metadata::property_id::WIDTH,
            &from,
            &to,
            0.5,
        );
        assert!(result.handled);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Calculated { .. }));
    }

    #[test]
    fn combines_lengths_with_different_units_discretely() {
        let from = Arc::new(StyleValueData::Length { value: 10.0, unit: 0 });
        let to = Arc::new(StyleValueData::Length { value: 20.0, unit: 1 });
        let result = interpolate_value(
            Some(&animation_context(true)),
            crate::css::property_metadata::property_id::WIDTH,
            &from,
            &to,
            0.75,
        );
        assert!(result.handled);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Length { value, unit } if *value == 20.0 && *unit == 1));
    }

    #[test]
    fn interpolates_rotate_3d_with_a_zero_axis() {
        let retained_angle = |value| {
            let pointer = Arc::into_raw(Arc::new(StyleValueData::Angle { value, unit: 0 }));
            unsafe { RetainedStyleValueData::from_retained_pointer(pointer) }
        };
        let from = RetainedStyleValueDataList::from_retained_values(vec![
            retained_number(0.0),
            retained_number(0.0),
            retained_number(0.0),
            retained_angle(90.0),
        ]);
        let to = RetainedStyleValueDataList::from_retained_values(vec![
            retained_number(1.0),
            retained_number(0.0),
            retained_number(0.0),
            retained_angle(180.0),
        ]);
        let result = interpolate_rotate_3d(
            None,
            crate::css::property_metadata::property_id::TRANSFORM,
            0,
            &from,
            &to,
            0.5,
        )
        .expect("rotate3d values should interpolate");
        let StyleValueData::Transformation { values, .. } = result else {
            panic!("expected a transform function");
        };
        assert!(values.as_slice().iter().all(|value| match value.data() {
            StyleValueData::Number { value } | StyleValueData::Angle { value, .. } => value.is_finite(),
            _ => false,
        }));
    }

    #[test]
    fn composes_mixed_length_percentage_values() {
        let underlying = Arc::new(StyleValueData::Length { value: 10.0, unit: 0 });
        let animated = Arc::new(StyleValueData::Percentage { value: 50.0 });
        let result = composite_scalar_value(&underlying, &animated, FfiCompositeOperation::Add);
        assert!(result.handled);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Calculated { .. }));
    }

    #[test]
    fn owns_unsupported_composition_decisions() {
        let underlying = Arc::new(StyleValueData::Keyword {
            keyword: crate::css::style_compute::none_keyword(),
        });
        let animated = Arc::new(StyleValueData::Number { value: 4.0 });
        let result = composite_scalar_value(&underlying, &animated, FfiCompositeOperation::Add);
        assert!(result.handled);
        assert!(result.value.is_null());
    }

    #[test]
    fn combines_general_calculated_numeric_values() {
        let from = Arc::new(calculated_number(2.0));
        let to = Arc::new(StyleValueData::Number { value: 4.0 });
        let result = interpolate_value(
            Some(&animation_context(true)),
            crate::css::property_metadata::property_id::FLEX_GROW,
            &from,
            &to,
            0.5,
        );
        assert!(result.handled);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Calculated { .. }));

        let result = composite_scalar_value(&from, &to, FfiCompositeOperation::Add);
        assert!(result.handled);
        let value = unsafe { Arc::from_raw(result.value) };
        assert!(matches!(&*value, StyleValueData::Calculated { .. }));
    }
}
