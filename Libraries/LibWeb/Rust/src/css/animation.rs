/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS animation value interpolation.

// Animation values use the same thread-confined shared graph as the rest of the style core.
#![allow(clippy::arc_with_non_send_sync)]

use std::collections::BTreeMap;
use std::sync::Arc;

use crate::css::easing::{Easing, FfiEasingDescriptor, FfiLinearEasingPoint, evaluate_easing_descriptor};
use crate::css::property_metadata::{property_animation_type, property_numeric_ranges};
use crate::css::style::effect_descriptions::{PublishedEffect, PublishedValue};
use crate::css::style_value::{
    BasicShapeData, ColorBase, CssString, CssStringList, FILTER_KIND_BLUR, FILTER_KIND_COLOR, FILTER_KIND_DROP_SHADOW,
    FILTER_KIND_HUE_ROTATE, GridTrackEntryKind, OwnedBasicShapeData, RetainedGridTrackEntry,
    RetainedGridTrackEntryList, RetainedNumericRangeList, RetainedShapePoint, RetainedShapePointList,
    RetainedStyleValueData, RetainedStyleValueDataList, StyleValueData,
};
use crate::css::value_codes::*;

pub(crate) const ANIMATION_TYPE_DISCRETE: u8 = 0;
pub(crate) const ANIMATION_TYPE_BY_COMPUTED_VALUE: u8 = 1;
const ANIMATION_TYPE_REPEATABLE_LIST: u8 = 2;
const ANIMATION_TYPE_CUSTOM: u8 = 3;
pub(crate) const ANIMATION_TYPE_NONE: u8 = 4;
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
const GRID_REPEAT_FIXED: u8 = 2;
const BASIC_SHAPE_INSET: u8 = 0;
const BASIC_SHAPE_CIRCLE: u8 = 3;
const BASIC_SHAPE_ELLIPSE: u8 = 4;
const BASIC_SHAPE_POLYGON: u8 = 5;
const BASIC_SHAPE_PATH: u8 = 6;
const BASIC_SHAPE_SHAPE: u8 = 7;

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

const RADIAL_SIZE_RANGES: &[NumericRangeOverride] = &[
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

/// The Rust-side form of `FfiAnimationValueResult`, which owns its value.
pub(crate) struct AnimationValueResult {
    pub(crate) value: Option<RetainedStyleValueData>,
    pub(crate) handled: bool,
}

impl AnimationValueResult {
    /// The value of a handled result.
    pub(crate) fn into_value(self) -> Option<RetainedStyleValueData> {
        self.value.filter(|_| self.handled)
    }

    /// Transfers the value's reference to the C++ caller.
    fn into_ffi(self) -> FfiAnimationValueResult {
        FfiAnimationValueResult {
            value: self
                .value
                .map_or(std::ptr::null(), |value| Arc::into_raw(value.into_arc())),
            handled: self.handled,
        }
    }
}

/// Construct a CSS value from resolved animation easing parameters.
///
/// # Safety
/// The descriptor and its control-point slice must remain valid during the call.
#[cfg(test)]
pub(crate) unsafe fn style_value_from_easing(descriptor: &FfiEasingDescriptor) -> *const StyleValueData {
    use crate::css::easing::FfiEasingKind;
    use crate::css::style_value::{RetainedLinearEasingStop, RetainedLinearEasingStopList};
    let retain = |value| RetainedStyleValueData::from_arc(Arc::new(value));
    let mut stops = Vec::new();
    let mut x1 = empty_retained_style_value();
    let mut y1 = empty_retained_style_value();
    let mut x2 = empty_retained_style_value();
    let mut y2 = empty_retained_style_value();
    let mut number_of_intervals = empty_retained_style_value();
    match descriptor.kind {
        FfiEasingKind::Linear => {
            assert!(descriptor.linear_point_count > 0);
            let points = unsafe { ak::slice_from_raw_parts(descriptor.linear_points, descriptor.linear_point_count) };
            stops = points
                .iter()
                .map(|point| {
                    RetainedLinearEasingStop::from_retained_values(
                        retain(StyleValueData::Number { value: point.output }),
                        retain(StyleValueData::Percentage {
                            value: point.input * 100.0,
                        }),
                    )
                })
                .collect();
        }
        FfiEasingKind::CubicBezier => {
            x1 = retain(StyleValueData::Number { value: descriptor.x1 });
            y1 = retain(StyleValueData::Number { value: descriptor.y1 });
            x2 = retain(StyleValueData::Number { value: descriptor.x2 });
            y2 = retain(StyleValueData::Number { value: descriptor.y2 });
        }
        FfiEasingKind::Steps => {
            number_of_intervals = retain(StyleValueData::Integer {
                value: descriptor.interval_count,
            });
        }
    }
    Arc::into_raw(Arc::new(StyleValueData::Easing {
        kind: descriptor.kind as u8,
        linear_stops: RetainedLinearEasingStopList::from_retained_elements(stops),
        x1,
        y1,
        x2,
        y2,
        number_of_intervals,
        step_position: descriptor.step_position,
    }))
}

/// Evaluate a resolved easing descriptor without consulting C++.
///
/// # Safety
/// `descriptor` must point to a live descriptor. Its linear point range must be live when the
/// descriptor kind is linear.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_evaluate_easing(
    descriptor: &FfiEasingDescriptor,
    input_progress: f64,
    before_flag: bool,
) -> f64 {
    evaluate_easing_descriptor(descriptor, input_progress, before_flag)
}

/// Describes the easing a computed timing function value names, appending a linear easing's
/// control points through `append_point`. Returns false for a value that names none.
///
/// # Safety
/// `append_point` must accept `points` for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_easing_from_style_value(
    value: &StyleValueData,
    descriptor: &mut FfiEasingDescriptor,
    points: *mut std::ffi::c_void,
    append_point: unsafe extern "C" fn(*mut std::ffi::c_void, FfiLinearEasingPoint),
) -> bool {
    let Some(easing) = crate::css::style::effect_descriptions::easing_from_computed_timing_function(value) else {
        return false;
    };
    descriptor.kind = easing.kind();
    match easing {
        Easing::Linear(linear_points) => {
            for point in linear_points {
                unsafe { append_point(points, point) };
            }
        }
        Easing::CubicBezier { x1, y1, x2, y2 } => {
            (descriptor.x1, descriptor.y1, descriptor.x2, descriptor.y2) = (x1, y1, x2, y2);
        }
        Easing::Steps {
            interval_count,
            position,
        } => {
            (descriptor.interval_count, descriptor.step_position) = (interval_count, position);
        }
    }
    true
}

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct FfiAnimationFontMetrics {
    pub font_size: f64,
    pub x_height: f64,
    pub cap_height: f64,
    pub zero_advance: f64,
    pub line_height: f64,
}

#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct FfiAnimationLengthResolutionContext {
    pub viewport_width: f64,
    pub viewport_height: f64,
    pub font_metrics: FfiAnimationFontMetrics,
    pub root_font_metrics: FfiAnimationFontMetrics,
    pub font_metrics_depend_on_viewport_metrics: bool,
    pub root_font_metrics_depend_on_viewport_metrics: bool,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiAnimationContext<'a> {
    pub allow_discrete: bool,
    /// Whether the element's interpolate-size lets sizing keywords interpolate with lengths.
    /// https://drafts.csswg.org/css-values-5/#interpolate-size
    pub allow_keyword_size_interpolation: bool,
    pub current_color: Option<&'a RetainedStyleValueData>,
    pub has_length_resolution_context: bool,
    pub length_resolution_context: FfiAnimationLengthResolutionContext,
    pub has_transform_reference_box: bool,
    pub transform_reference_box_width: f64,
    pub transform_reference_box_height: f64,
}

/// Whether the computed interpolate-size of a style lets sizing keywords interpolate with lengths.
/// https://drafts.csswg.org/css-values-5/#valdef-interpolate-size-allow-keywords
pub(crate) fn allows_keyword_size_interpolation(
    table: &crate::css::computed_longhand_table::ComputedLonghandTable,
    overlay: Option<&crate::css::animated_overlay::AnimatedOverlay>,
) -> bool {
    matches!(
        table
            .effective_value_handle(
                overlay,
                crate::css::property_metadata::property_id::INTERPOLATE_SIZE,
                true
            )
            .0
            .optional_data(),
        Some(StyleValueData::Keyword {
            keyword: crate::css::css_enums::keyword::ALLOW_KEYWORDS
        })
    )
}

impl FfiAnimationContext<'_> {
    /// Has transforms interpolate against `reference_box`, where there is one.
    pub(crate) fn set_transform_reference_box(&mut self, reference_box: Option<crate::css::css_pixels::CssPixelRect>) {
        if let Some(reference_box) = reference_box {
            self.has_transform_reference_box = true;
            self.transform_reference_box_width = reference_box.width.to_double();
            self.transform_reference_box_height = reference_box.height.to_double();
        }
    }
}

/// One of the property-specific keyframes an animated value interpolates between.
struct AnimationKeyframeValue {
    /// The keyframe, by its index among the keyframe plans.
    keyframe: usize,
    /// The keyframe's computed value, or none where the keyframe leaves the property out.
    value: RetainedStyleValueData,
}

struct AnimationValueInput<'a> {
    property_id: u16,
    custom_name_id: u32,
    result_of_transition: bool,
    underlying: Option<&'a StyleValueData>,
    initial: Option<&'a StyleValueData>,
    current_key: f64,
    keyframes: &'a [AnimationKeyframeValue],
    /// The keyframes the values' indices name.
    keyframe_plans: &'a [AnimationKeyframePlan],
}

/// The version of an effect a preparation was made from.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub struct FfiAnimationPreparationEffect {
    pub identity: u64,
    pub generation: u64,
}

/// One effect a sample composes, in composite order: the version a preparation is keyed by, and
/// where the effect is.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSampledAnimationEffect {
    pub effect: FfiAnimationPreparationEffect,
    pub current_key: f64,
}

/// The declarations an element's effects resolved to, and the computed value of each, which a
/// preparation is made from.
pub(crate) struct AnimationPreparationInputs {
    pub(crate) resolved: Box<ResolvedAnimationDeclarations>,
    /// One computed value per resolved property.
    pub(crate) computed_keyframe_values: Vec<RetainedStyleValueData>,
}

pub(crate) struct ComputedAnimationBatch<'a> {
    pub(crate) context: FfiAnimationContext<'a>,
    pub(crate) sampled_effects: &'a [FfiSampledAnimationEffect],
    /// The identities of the custom-property environments of the element and of the one it
    /// inherits from, which a preparation that substituted keyframe values against the element was
    /// made under.
    pub(crate) custom_property_environments: [u64; 2],
    pub(crate) cache_preparation: bool,
    /// Whether the preparation substituted keyframe values against the element, so that it holds
    /// only under the custom-property environments its key names.
    pub(crate) preparation_reads_custom_property_environments: bool,
    /// What a preparation is made from where the overlay holds none for the sampled effects.
    pub(crate) preparation_inputs: Option<AnimationPreparationInputs>,
    pub(crate) underlying_longhand_table: &'a crate::css::computed_longhand_table::ComputedLonghandTable,
    pub(crate) overlay: &'a mut crate::css::animated_overlay::AnimatedOverlay,
    /// What an animation of each custom property composes over, and what a keyframe saying
    /// `initial` takes, indexed by the number the property animates under, less one.
    pub(crate) custom_underlying_values: &'a [RetainedStyleValueData],
    pub(crate) custom_initial_values: &'a [RetainedStyleValueData],
}

#[repr(C)]
pub struct FfiAnimationEffect {
    pub first_keyframe_index: usize,
    pub keyframe_count: usize,
    pub current_key: f64,
    pub result_of_transition: bool,
}

/// One animated property's effect value, and whether it applies at all.
struct AnimatedProperty {
    value: Option<RetainedStyleValueData>,
    #[cfg(test)]
    start_index: usize,
    #[cfg(test)]
    end_index: usize,
    apply: bool,
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

struct AnimationDeclaration {
    keyframe_index: usize,
    property_id: u16,
    custom_name_id: u32,
    custom_is_inherited: bool,
    custom_is_important: bool,
    value: RetainedStyleValueData,
    style_sheet_resource_context: FfiAnimationStyleSheetResourceContext,
    use_initial: bool,
    is_transition: bool,
}

struct AnimationPropertyConflictCandidate {
    keyframe_index: usize,
    physical_property_id: u16,
    custom_name_id: u32,
    custom_is_inherited: bool,
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

fn animation_specified_value_source(value: &StyleValueData, is_inherited: bool) -> FfiAnimationSpecifiedValueSource {
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
        return if is_inherited {
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
        let is_inherited = if candidate.custom_name_id != 0 {
            candidate.custom_is_inherited
        } else {
            crate::css::property_metadata::property_is_inherited(candidate.physical_property_id)
        };
        *source = animation_specified_value_source(candidate.value.data(), is_inherited);
    }
    let mut winners = std::collections::HashMap::<(usize, u16, u32), usize>::new();
    for (candidate_index, candidate) in candidates.iter().enumerate() {
        let key = (
            candidate.keyframe_index,
            candidate.physical_property_id,
            candidate.custom_name_id,
        );
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

pub(crate) struct ResolvedAnimationProperty {
    pub(crate) keyframe_index: usize,
    pub(crate) physical_property_id: u16,
    pub(crate) custom_name_id: u32,
    pub(crate) source_longhand_id: u16,
    pub(crate) value: RetainedStyleValueData,
    pub(crate) value_source: FfiAnimationSpecifiedValueSource,
    pub(crate) style_sheet_resource_context: FfiAnimationStyleSheetResourceContext,
    pub(crate) is_transition: bool,
}

/// The `SUBSTITUTION_MARK_*` bits of the arbitrary-substitution functions a written value holds.
fn substitution_marks_of(written: &StyleValueData) -> u8 {
    use crate::css::style_compute::{
        SUBSTITUTION_MARK_ATTR, SUBSTITUTION_MARK_CUSTOM_FUNCTION, SUBSTITUTION_MARK_IF, SUBSTITUTION_MARK_INHERIT,
        SUBSTITUTION_MARK_VAR,
    };
    let StyleValueData::Unresolved {
        presence_var,
        presence_attr,
        presence_if,
        presence_inherit,
        presence_dashed_function,
        ..
    } = written
    else {
        return 0;
    };
    [
        (*presence_var, SUBSTITUTION_MARK_VAR),
        (*presence_attr, SUBSTITUTION_MARK_ATTR),
        (*presence_if, SUBSTITUTION_MARK_IF),
        (*presence_inherit, SUBSTITUTION_MARK_INHERIT),
        (*presence_dashed_function, SUBSTITUTION_MARK_CUSTOM_FUNCTION),
    ]
    .into_iter()
    .filter(|&(present, _)| present)
    .fold(0, |marks, (_, mark)| marks | mark)
}

/// A random function's sharing value whose base value the element or document draws.
pub(crate) struct AnimationUnfixedRandomSharing {
    /// The sharing value, which keys its base value.
    pub(crate) source: *const StyleValueData,
    pub(crate) name: Vec<u16>,
    pub(crate) element_shared: bool,
}

fn resolve_animation_declarations(
    declarations: &[AnimationDeclaration],
    effects: &[FfiAnimationEffect],
    keyframes: Vec<AnimationKeyframePlan>,
    writing_mode: u8,
    direction: u8,
    important_property_bitmap: &[u8],
) -> ResolvedAnimationDeclarations {
    let mut candidates = Vec::new();
    for declaration in declarations {
        assert!(
            declaration.value.optional_data().is_some(),
            "animation declaration value must not be null"
        );
        if declaration.custom_name_id != 0 {
            candidates.push(AnimationPropertyConflictCandidate {
                keyframe_index: declaration.keyframe_index,
                physical_property_id: declaration.property_id,
                custom_name_id: declaration.custom_name_id,
                custom_is_inherited: declaration.custom_is_inherited,
                source_property_id: declaration.property_id,
                source_longhand_id: declaration.property_id,
                value: declaration.value.clone(),
                style_sheet_resource_context: declaration.style_sheet_resource_context,
                use_initial: declaration.use_initial,
                is_transition: declaration.is_transition,
                suppressed_by_important: !declaration.is_transition && declaration.custom_is_important,
            });
            continue;
        }
        crate::css::style_compute::expand_shorthands_with(
            declaration.property_id,
            &declaration.value,
            false,
            &mut |longhand_id, data, _| {
                let physical_property_id =
                    crate::css::style_compute::map_logical_alias_to_physical(longhand_id, writing_mode, direction);
                candidates.push(AnimationPropertyConflictCandidate {
                    keyframe_index: declaration.keyframe_index,
                    physical_property_id,
                    custom_name_id: 0,
                    custom_is_inherited: false,
                    source_property_id: declaration.property_id,
                    source_longhand_id: longhand_id,
                    value: data.clone(),
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
    for ((candidate, selected), value_source) in candidates.into_iter().zip(selected).zip(value_sources) {
        if selected && !candidate.suppressed_by_important {
            properties.push(ResolvedAnimationProperty {
                keyframe_index: candidate.keyframe_index,
                physical_property_id: candidate.physical_property_id,
                custom_name_id: candidate.custom_name_id,
                source_longhand_id: candidate.source_longhand_id,
                value: candidate.value,
                value_source,
                style_sheet_resource_context: candidate.style_sheet_resource_context,
                is_transition: candidate.is_transition,
            });
        }
    }
    let mut value_plans = Vec::new();
    for (effect_index, effect) in effects.iter().enumerate() {
        let keyframe_end = effect
            .first_keyframe_index
            .checked_add(effect.keyframe_count)
            .expect("animation keyframe range must not overflow");
        assert!(keyframe_end <= keyframes.len());
        let mut properties_by_id = std::collections::BTreeMap::<(u16, u32), Vec<usize>>::new();
        for (index, property) in properties.iter().enumerate() {
            if (effect.first_keyframe_index..keyframe_end).contains(&property.keyframe_index) {
                properties_by_id
                    .entry((property.physical_property_id, property.custom_name_id))
                    .or_default()
                    .push(index);
            }
        }
        for ((property_id, custom_name_id), mut property_indices) in properties_by_id {
            if property_indices.len() < 2
                || (custom_name_id == 0
                    && !(crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
                        ..=crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID)
                        .contains(&property_id))
            {
                continue;
            }
            property_indices.sort_by_key(|index| properties[*index].keyframe_index);
            value_plans.push(AnimationValuePlan {
                property_id,
                custom_name_id,
                result_of_transition: effect.result_of_transition,
                effect_index,
                property_indices,
            });
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
        if property.custom_name_id != 0 {
            continue;
        }
        let value = property.value.data();
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
            let sharing = unsafe { &*source };
            let StyleValueData::RandomValueSharing { has_name, name, .. } = sharing else {
                unreachable!();
            };
            AnimationUnfixedRandomSharing {
                source,
                name: if *has_name { name.units().to_vec() } else { Vec::new() },
                element_shared: crate::css::style_compute::random_caching_key(sharing).1,
            }
        })
        .collect();
    ResolvedAnimationDeclarations {
        properties,
        keyframes,
        value_plans,
        uses_tree_counting_function,
        container_relative_length_unit_mask,
        needs_document_base_url,
        unfixed_random_sharings,
        ..ResolvedAnimationDeclarations::default()
    }
}

/// The declarations of the effects an element samples, resolved into what the animation core
/// interpolates.
#[derive(Default)]
pub(crate) struct ResolvedAnimationDeclarations {
    pub(crate) properties: Vec<ResolvedAnimationProperty>,
    /// The base URLs the properties' resource contexts point into.
    _base_urls: Vec<std::sync::Arc<[u8]>>,
    keyframes: Vec<AnimationKeyframePlan>,
    value_plans: Vec<AnimationValuePlan>,
    pub(crate) uses_tree_counting_function: bool,
    pub(crate) container_relative_length_unit_mask: u8,
    pub(crate) needs_document_base_url: bool,
    pub(crate) unfixed_random_sharings: Vec<AnimationUnfixedRandomSharing>,
}

struct AnimationValuePlan {
    property_id: u16,
    custom_name_id: u32,
    result_of_transition: bool,
    effect_index: usize,
    property_indices: Vec<usize>,
}

struct AnimationKeyframePlan {
    key: i64,
    easing: Easing,
    composite: FfiCompositeOperation,
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiCompositeOperation {
    Replace,
    Add,
    Accumulate,
}

fn accepted_range(property_id: u16, value_type: u8, range_overrides: &[NumericRangeOverride]) -> Option<(f64, f64)> {
    if let Some(range) = range_overrides.iter().find(|range| range.value_type == value_type) {
        return Some((range.min, range.max));
    }
    if property_id == crate::css::property_metadata::property_id::CUSTOM {
        return None;
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

fn owned(value: StyleValueData) -> AnimationValueResult {
    handled_retained_value(RetainedStyleValueData::from_arc(Arc::new(value)))
}

fn not_handled() -> AnimationValueResult {
    AnimationValueResult {
        value: None,
        handled: false,
    }
}

fn handled_without_value() -> AnimationValueResult {
    AnimationValueResult {
        value: None,
        handled: true,
    }
}

fn handled_retained_value(value: RetainedStyleValueData) -> AnimationValueResult {
    AnimationValueResult {
        value: Some(value),
        handled: true,
    }
}

/// A handled result holding another reference to `value`.
///
/// # Safety
/// `value` must live in a shared style value allocation.
unsafe fn handled_borrowed_value(value: &StyleValueData) -> AnimationValueResult {
    // SAFETY: The caller guarantees a shared allocation, which the new reference keeps alive.
    handled_retained_value(unsafe {
        RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(value))
    })
}

/// A handled result holding a copy of `value`, a keyword or display value, which is cheap to copy.
fn handled_copied_value(value: &StyleValueData) -> AnimationValueResult {
    handled_retained_value(RetainedStyleValueData::from_owned(value.clone()))
}

fn discrete_value(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> AnimationValueResult {
    if !context.is_some_and(|context| context.allow_discrete) {
        return handled_without_value();
    }
    let value = if delta < 0.5 { from } else { to };
    unsafe { handled_borrowed_value(value) }
}

fn interpolate_visibility(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> AnimationValueResult {
    let (StyleValueData::Keyword { keyword: from_keyword }, StyleValueData::Keyword { keyword: to_keyword }) =
        (from, to)
    else {
        return not_handled();
    };

    if from_keyword == to_keyword {
        return handled_copied_value(from);
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
        return handled_copied_value(value);
    }

    discrete_value(context, from, to, delta)
}

fn interpolate_content_visibility(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> AnimationValueResult {
    let (StyleValueData::Keyword { keyword: from_keyword }, StyleValueData::Keyword { keyword: to_keyword }) =
        (from, to)
    else {
        return not_handled();
    };

    if from_keyword == to_keyword {
        return handled_copied_value(from);
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
        return handled_copied_value(value);
    }

    discrete_value(context, from, to, delta)
}

fn interpolate_display(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> AnimationValueResult {
    let (StyleValueData::Display { raw: from_raw }, StyleValueData::Display { raw: to_raw }) = (from, to) else {
        return not_handled();
    };

    if from_raw == to_raw {
        return handled_copied_value(from);
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
        return handled_copied_value(value);
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

fn interpolate_scale(from: &StyleValueData, to: &StyleValueData, delta: f32) -> AnimationValueResult {
    let none = crate::css::style_compute::none_keyword();
    if matches!(from, StyleValueData::Keyword { keyword } if *keyword == none)
        && matches!(to, StyleValueData::Keyword { keyword } if *keyword == none)
    {
        return handled_copied_value(from);
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

/// https://drafts.csswg.org/css-values-5/#sizing-property
fn is_sizing_property(property_id: u16) -> bool {
    use crate::css::property_metadata::property_id;
    matches!(
        property_id,
        property_id::WIDTH
            | property_id::HEIGHT
            | property_id::MIN_WIDTH
            | property_id::MIN_HEIGHT
            | property_id::MAX_WIDTH
            | property_id::MAX_HEIGHT
            | property_id::INLINE_SIZE
            | property_id::BLOCK_SIZE
            | property_id::MIN_INLINE_SIZE
            | property_id::MIN_BLOCK_SIZE
            | property_id::MAX_INLINE_SIZE
            | property_id::MAX_BLOCK_SIZE
            | property_id::FLEX_BASIS
    )
}

/// Interpolates two sizes where one is a sizing keyword or a calc-size(), as calc-size() values, or returns None
/// where they do not interpolate that way.
/// https://drafts.csswg.org/css-values-5/#interpolate-calc-size
fn interpolate_calc_size(
    context: Option<&FfiAnimationContext>,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> Option<StyleValueData> {
    use crate::css::css_enums::keyword;

    #[derive(PartialEq)]
    enum Kind {
        Keyword,
        CalcSize,
        LengthPercentage,
    }
    let kind = |value: &StyleValueData| match value {
        StyleValueData::Keyword {
            keyword:
                keyword::AUTO
                | keyword::MIN_CONTENT
                | keyword::MAX_CONTENT
                | keyword::FIT_CONTENT
                | keyword::STRETCH
                | keyword::CONTENT,
        } => Some(Kind::Keyword),
        value if crate::css::calc_size::calc_size_arguments(value).is_some() => Some(Kind::CalcSize),
        StyleValueData::Length { .. } | StyleValueData::Percentage { .. } | StyleValueData::Calculated { .. } => {
            Some(Kind::LengthPercentage)
        }
        _ => None,
    };
    let (from_kind, to_kind) = (kind(from)?, kind(to)?);
    // NB: Two keywords never interpolate: the same keyword stays itself, and different keywords are not compatible.
    if from_kind == to_kind && from_kind != Kind::CalcSize {
        return None;
    }
    // https://drafts.csswg.org/css-values-5/#valdef-interpolate-size-allow-keywords
    // Sizing values can also be combined if one of them is an <size-keyword> and the other is a <length-percentage>.
    let keyword_and_length =
        [&from_kind, &to_kind].contains(&&Kind::Keyword) && [&from_kind, &to_kind].contains(&&Kind::LengthPercentage);
    if keyword_and_length && !context.is_some_and(|context| context.allow_keyword_size_interpolation) {
        return None;
    }

    // Treat the non-calc-size() value as calc-size(any, value) if the value is a <calc-sum> or as
    // calc-size(value, size) otherwise.
    let any = StyleValueData::Keyword { keyword: keyword::ANY };
    let prepared = |value: &StyleValueData, kind: &Kind| -> Option<(StyleValueData, Arc<crate::css::calc::CalcNode>)> {
        match kind {
            Kind::Keyword => Some((value.clone(), Arc::new(crate::css::calc::CalcNode::SizeKeyword))),
            Kind::LengthPercentage => Some((any.clone(), length_percentage_calculation_node(value)?)),
            Kind::CalcSize => prepared_calc_size(value),
        }
    };
    let (from_basis, from_calculation) = prepared(from, &from_kind)?;
    let (to_basis, to_calculation) = prepared(to, &to_kind)?;
    let basis = if from_basis == to_basis || to_basis == any {
        from_basis
    } else if from_basis == any {
        to_basis
    } else {
        return None;
    };
    // The result's calc-size calculation is the interpolation of the two input calc-size calculations.
    let (calculation, resolved_type) =
        crate::css::calc::interpolate_calc_size_calculations(from_calculation, to_calculation, delta)?;
    let calculation = crate::css::calc::length_percentage_calculated_style_value(calculation, resolved_type);
    Some(crate::css::calc_size::calc_size_function(basis, calculation))
}

/// A calc-size() prepared for interpolation, as its basis and its calculation, with any basis other than a sizing
/// keyword or any substituted into the calculation.
/// https://drafts.csswg.org/css-values-5/#calc-size-prepared-for-interpolation
fn prepared_calc_size(value: &StyleValueData) -> Option<(StyleValueData, Arc<crate::css::calc::CalcNode>)> {
    let (basis, calculation) = crate::css::calc_size::calc_size_arguments(value)?;
    let calculation = length_percentage_calculation_node(calculation)?;
    match basis {
        StyleValueData::Keyword { .. } => Some((basis.clone(), calculation)),
        // A nested calc-size() stands for its own calculation, against its own basis.
        StyleValueData::Function { .. } => {
            let (inner_basis, inner_calculation) = prepared_calc_size(basis)?;
            Some((
                inner_basis,
                crate::css::calc::replace_size_keyword(&calculation, &inner_calculation)?,
            ))
        }
        // A <calc-sum> basis with no percentage present is substituted into the calculation, leaving a basis of
        // any. Any other <calc-sum> basis is replaced with 100%, after being de-percentified and substituted.
        basis => {
            let basis_calculation = length_percentage_calculation_node(basis)?;
            if !basis_calculation.contains_percentage() {
                return Some((
                    StyleValueData::Keyword {
                        keyword: crate::css::css_enums::keyword::ANY,
                    },
                    crate::css::calc::replace_size_keyword(&calculation, &basis_calculation)?,
                ));
            }
            let de_percentified = crate::css::calc::de_percentify_calc_size_calculation(&basis_calculation)?;
            Some((
                StyleValueData::Percentage { value: 100.0 },
                crate::css::calc::replace_size_keyword(&calculation, &de_percentified)?,
            ))
        }
    }
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
    RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Calculated {
        rust_calculation: crate::css::calc::CalcNodeHandle::from_arc(calculation),
        resolve_as_is_number: context.resolve_as_is_number,
        resolve_as_base: context.resolve_as_base,
        resolved_type,
        has_percentages_resolve_as: context.has_percentages_resolve_as,
        percentages_resolve_as: context.percentages_resolve_as,
        resolve_numbers_as_integers: context.resolve_numbers_as_integers,
        accepted_ranges: context.accepted_ranges.clone_owned(),
    }))
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
    RetainedStyleValueData::from_arc(Arc::new(value))
}

fn interpolate_translate_component(
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> Option<RetainedStyleValueData> {
    let direct = interpolate_scalar_value(property_id, from, to, delta, &[]);
    if let Some(value) = direct.into_value() {
        return Some(value);
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
        let value = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Percentage { value: percentage }));
        return Some(value);
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

fn interpolate_translate(from: &StyleValueData, to: &StyleValueData, delta: f32) -> AnimationValueResult {
    let none = crate::css::style_compute::none_keyword();
    if matches!(from, StyleValueData::Keyword { keyword } if *keyword == none)
        && matches!(to, StyleValueData::Keyword { keyword } if *keyword == none)
    {
        return handled_copied_value(from);
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
        RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Length {
            value: 0.0,
            unit: crate::css::calc::canonical_pixel_unit(),
        }))
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
) -> AnimationValueResult {
    let none = crate::css::style_compute::none_keyword();
    if matches!(from, StyleValueData::Keyword { keyword } if *keyword == none)
        && matches!(to, StyleValueData::Keyword { keyword } if *keyword == none)
    {
        return handled_copied_value(from);
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
        let angle = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Angle {
            value: interpolate_f64(from, to, delta, None),
            unit: 0,
        }));
        return owned(StyleValueData::Transformation {
            property: crate::css::property_metadata::property_id::ROTATE,
            transform_function: TRANSFORM_FUNCTION_ROTATE,
            values: RetainedStyleValueDataList::from_retained_values(vec![angle]),
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
            let angle = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Angle { value: 0.0, unit: 0 }));
            return Some(RetainedStyleValueDataList::from_retained_values(vec![
                retained_number(0.0),
                retained_number(0.0),
                retained_number(1.0),
                angle,
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
) -> AnimationValueResult {
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
    if !result.handled || result.value.is_none() {
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
    line_names: Option<&'a CssStringList>,
}

fn empty_retained_style_value() -> RetainedStyleValueData {
    RetainedStyleValueData::none()
}

fn empty_grid_line_names() -> CssStringList {
    CssStringList::from_strings(Vec::new())
}

fn grid_line_names_entry(names: &CssStringList) -> RetainedGridTrackEntry {
    RetainedGridTrackEntry {
        kind: GridTrackEntryKind::LineNames,
        names: names.clone(),
        size_value: empty_retained_style_value(),
        min_value: empty_retained_style_value(),
        max_value: empty_retained_style_value(),
        repeat_type: 0,
        repeat_count: empty_retained_style_value(),
        repeat_is_subgrid: false,
        repeat_preserve_line_name_sets: false,
        repeat_entries: RetainedGridTrackEntryList::from_retained_entries(Vec::new()),
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
    line_names: Option<&CssStringList>,
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
                    from.track.repeat_entries(),
                    to.track.repeat_is_subgrid,
                    to.track.repeat_entries(),
                    delta,
                )?;
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
                    repeat_entries: RetainedGridTrackEntryList::from_retained_entries(nested),
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
                repeat_entries: RetainedGridTrackEntryList::from_retained_entries(Vec::new()),
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
                repeat_entries: RetainedGridTrackEntryList::from_retained_entries(Vec::new()),
            },
            _ => {
                if delta < 0.5 {
                    from.track.clone()
                } else {
                    to.track.clone()
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
    if let Some(value) = result.into_value() {
        return value;
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
                    underlying.track.repeat_entries(),
                    animated.track.repeat_is_subgrid,
                    animated.track.repeat_entries(),
                    operation,
                )?;
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
                    repeat_entries: RetainedGridTrackEntryList::from_retained_entries(nested),
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
                repeat_entries: RetainedGridTrackEntryList::from_retained_entries(Vec::new()),
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
                repeat_entries: RetainedGridTrackEntryList::from_retained_entries(Vec::new()),
            },
            _ => animated.track.clone(),
        };
        append_grid_track_with_line_names(&mut result, track, animated.line_names);
    }
    Some(result)
}

fn retained_percentage(value: f64) -> RetainedStyleValueData {
    RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Percentage { value }))
}

fn retained_computed_center_position() -> RetainedStyleValueData {
    let edge = || {
        RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Edge {
            has_edge: false,
            edge: 0,
            offset: retained_percentage(50.0),
        }))
    };
    RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Position {
        edge_x: edge(),
        edge_y: edge(),
    }))
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

fn empty_retained_fly_string() -> CssString {
    CssString::none()
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
        if let Some(value) = direct.into_value() {
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
        Some(BasicShapeData {
            kind: from_kind,
            v0: from_v0,
            v1: from_v1,
            v2: from_v2,
            v3: from_v3,
            v4: from_v4,
            fill_rule: from_fill_rule,
            points: from_points,
            path: from_path,
            commands: from_commands,
        }),
        Some(BasicShapeData {
            kind: to_kind,
            v0: to_v0,
            v1: to_v1,
            v2: to_v2,
            v3: to_v3,
            v4: to_v4,
            fill_rule: to_fill_rule,
            points: to_points,
            path: to_path,
            commands: to_commands,
        }),
    ) = (from.basic_shape(), to.basic_shape())
    else {
        return None;
    };

    // https://drafts.csswg.org/css-shapes-1/#interpolating-shape
    // shape() and path() functions can be interpolated with each other if their associated list of path data commands
    // is the same length and has the same commands, in order, with the first command of the path() function
    // interpolating with the initial <coordinate-pair> in the shape() function.
    // If the starting and ending values are both path() functions, the interpolated value is a path() function;
    // otherwise it's a shape() function.
    if matches!(
        (*from_kind, *to_kind),
        (BASIC_SHAPE_PATH, BASIC_SHAPE_SHAPE) | (BASIC_SHAPE_SHAPE, BASIC_SHAPE_PATH)
    ) {
        let from_converted = if *from_kind == BASIC_SHAPE_PATH {
            Some(from_path.parsed()?.to_shape_commands()?)
        } else {
            None
        };
        let to_converted = if *to_kind == BASIC_SHAPE_PATH {
            Some(to_path.parsed()?.to_shape_commands()?)
        } else {
            None
        };
        let from_parts = from_converted.as_ref().map_or(
            (*from_fill_rule, from_commands.as_slice(), from_points.as_slice()),
            |(commands, points)| (*from_fill_rule, commands.as_slice(), points.as_slice()),
        );
        let to_parts = to_converted.as_ref().map_or(
            (*to_fill_rule, to_commands.as_slice(), to_points.as_slice()),
            |(commands, points)| (*to_fill_rule, commands.as_slice(), points.as_slice()),
        );
        return interpolate_shape_commands(property_id, from_parts, to_parts, delta);
    }

    // https://drafts.csswg.org/css-shapes-1/#basic-shape-interpolation
    if from_kind != to_kind {
        return None;
    }

    let empty = empty_retained_style_value;
    match *from_kind {
        BASIC_SHAPE_INSET => {
            // If both shapes are of type inset(), interpolate between each value in the shape functions.
            Some(StyleValueData::BasicShape {
                shape: OwnedBasicShapeData::new(BasicShapeData {
                    kind: *from_kind,
                    v0: interpolate_basic_shape_component(property_id, from_v0, to_v0, delta),
                    v1: interpolate_basic_shape_component(property_id, from_v1, to_v1, delta),
                    v2: interpolate_basic_shape_component(property_id, from_v2, to_v2, delta),
                    v3: interpolate_basic_shape_component(property_id, from_v3, to_v3, delta),
                    // NB: The border radii of the inset cannot become negative.
                    v4: interpolate_scalar_value(property_id, from_v4.data(), to_v4.data(), delta, &[])
                        .into_value()
                        .unwrap_or_else(|| interpolate_basic_shape_component(property_id, from_v4, to_v4, delta)),
                    fill_rule: 0,
                    points: empty_shape_points(),
                    path: crate::css::css_path::CssPath::none(),
                    commands: crate::css::style_value::ShapeCommands::default(),
                }),
            })
        }
        BASIC_SHAPE_CIRCLE | BASIC_SHAPE_ELLIPSE => {
            // If both shapes are the same type, that type is ellipse() or circle(), and the radiuses are specified
            // as <length-percentage> (rather than keywords), interpolate between each value in the shape functions.
            let radius =
                interpolate_scalar_value(property_id, from_v0.data(), to_v0.data(), delta, &[]).into_value()?;
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
                    interpolate_scalar_value(property_id, from.data(), to.data(), delta, &[]).into_value()?
                }
            };
            Some(StyleValueData::BasicShape {
                shape: OwnedBasicShapeData::new(BasicShapeData {
                    kind: *from_kind,
                    v0: radius,
                    v1: position,
                    v2: empty(),
                    v3: empty(),
                    v4: empty(),
                    fill_rule: 0,
                    points: empty_shape_points(),
                    path: crate::css::css_path::CssPath::none(),
                    commands: crate::css::style_value::ShapeCommands::default(),
                }),
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
                shape: OwnedBasicShapeData::new(BasicShapeData {
                    kind: *from_kind,
                    v0: empty(),
                    v1: empty(),
                    v2: empty(),
                    v3: empty(),
                    v4: empty(),
                    fill_rule: *from_fill_rule,
                    points: RetainedShapePointList::from_retained_points(points),
                    path: crate::css::css_path::CssPath::none(),
                    commands: crate::css::style_value::ShapeCommands::default(),
                }),
            })
        }
        BASIC_SHAPE_PATH => {
            // https://drafts.csswg.org/css-shapes-1/#interpolating-shape
            // If the starting and ending values are both path() functions, the interpolated value is a path() function;
            if from_fill_rule != to_fill_rule {
                return None;
            }
            // NB: Like other engines, paths interpolate as absolute commands, so that paths with relative and absolute
            //     commands interpolate with each other.
            let (from_path, to_path) = (from_path.parsed()?, to_path.parsed()?);
            let (from_absolute, to_absolute) = (from_path.to_absolute(), to_path.to_absolute());
            let path = from_absolute
                .as_ref()
                .unwrap_or(from_path)
                .interpolate(to_absolute.as_ref().unwrap_or(to_path), delta)?;
            Some(StyleValueData::BasicShape {
                shape: OwnedBasicShapeData::new(BasicShapeData {
                    kind: *from_kind,
                    v0: empty(),
                    v1: empty(),
                    v2: empty(),
                    v3: empty(),
                    v4: empty(),
                    fill_rule: *from_fill_rule,
                    points: empty_shape_points(),
                    path: crate::css::css_path::CssPath::new(path),
                    commands: crate::css::style_value::ShapeCommands::default(),
                }),
            })
        }
        BASIC_SHAPE_SHAPE => interpolate_shape_commands(
            property_id,
            (*from_fill_rule, from_commands.as_slice(), from_points.as_slice()),
            (*to_fill_rule, to_commands.as_slice(), to_points.as_slice()),
            delta,
        ),
        _ => None,
    }
}

/// https://drafts.csswg.org/css-shapes-1/#interpolating-shape
/// Interpolates the commands of two shape() functions, or of a shape() and a path() function, into a shape().
fn interpolate_shape_commands(
    property_id: u16,
    (from_fill_rule, from_commands, from_points): (u8, &[crate::css::style_value::ShapeCommand], &[RetainedShapePoint]),
    (to_fill_rule, to_commands, to_points): (u8, &[crate::css::style_value::ShapeCommand], &[RetainedShapePoint]),
    delta: f32,
) -> Option<StyleValueData> {
    let empty = empty_retained_style_value;
    // https://drafts.csswg.org/css-shapes-1/#interpolating-shape
    // For this purpose, commands are "the same" if they use the same command keyword, and use the same
    // <by-to> keyword. For curve and smooth, they also must have the same number of control points.
    if from_fill_rule != to_fill_rule
        || from_commands.len() != to_commands.len()
        || from_points.len() != to_points.len()
    {
        return None;
    }
    // If an arc command has different <arc-sweep> between its starting and ending list, then the
    // interpolated result uses cw for any progress value between 0 and 1. If it has different <arc-size>
    // keywords, then the interpolated result uses large for any progress value between 0 and 1.
    let flag = |from: bool, to: bool| {
        if from == to || delta <= 0.0 {
            from
        } else if delta >= 1.0 {
            to
        } else {
            true
        }
    };
    let mut commands = Vec::with_capacity(from_commands.len());
    for (from, to) in from_commands.iter().zip(to_commands) {
        if from.kind != to.kind
            || from.relative != to.relative
            || from.control_point_count != to.control_point_count
            || from.control_points != to.control_points
        {
            return None;
        }
        let mut command = *from;
        command.arc_has_one_radius = from.arc_has_one_radius && to.arc_has_one_radius;
        command.arc_clockwise = flag(from.arc_clockwise, to.arc_clockwise);
        command.arc_large = flag(from.arc_large, to.arc_large);
        command.arc_has_rotation = from.arc_has_rotation || to.arc_has_rotation;
        commands.push(command);
    }
    let mut points = Vec::with_capacity(from_points.len());
    for (from, to) in from_points.iter().zip(to_points) {
        let [from_x, from_y] = from.values();
        let [to_x, to_y] = to.values();
        let interpolate = |from: &RetainedStyleValueData, to: &RetainedStyleValueData| match (
            from.optional_data(),
            to.optional_data(),
        ) {
            (None, None) => Some(empty()),
            (Some(from), Some(to)) => interpolate_scalar_value(property_id, from, to, delta, &[]).into_value(),
            _ => None,
        };
        points.push(RetainedShapePoint::from_retained_values(
            interpolate(from_x, to_x)?,
            interpolate(from_y, to_y)?,
        ));
    }
    Some(StyleValueData::BasicShape {
        shape: OwnedBasicShapeData::new(BasicShapeData {
            kind: BASIC_SHAPE_SHAPE,
            v0: empty(),
            v1: empty(),
            v2: empty(),
            v3: empty(),
            v4: empty(),
            fill_rule: from_fill_rule,
            points: RetainedShapePointList::from_retained_points(points),
            path: crate::css::css_path::CssPath::none(),
            commands: crate::css::style_value::ShapeCommands::new(commands),
        }),
    })
}

fn composite_retained_value(
    underlying: &RetainedStyleValueData,
    animated: &RetainedStyleValueData,
    operation: FfiCompositeOperation,
) -> Option<RetainedStyleValueData> {
    composite_scalar_value(underlying.data(), animated.data(), operation).into_value()
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
        Some(BasicShapeData {
            kind: underlying_kind,
            v0: underlying_v0,
            v1: underlying_v1,
            v2: underlying_v2,
            v3: underlying_v3,
            v4: underlying_v4,
            fill_rule: underlying_fill_rule,
            points: underlying_points,
            ..
        }),
        Some(BasicShapeData {
            kind: animated_kind,
            v0: animated_v0,
            v1: animated_v1,
            v2: animated_v2,
            v3: animated_v3,
            v4: animated_v4,
            fill_rule: animated_fill_rule,
            points: animated_points,
            ..
        }),
    ) = (underlying.basic_shape(), animated.basic_shape())
    else {
        return None;
    };
    if underlying_kind != animated_kind {
        return None;
    }

    let empty = empty_retained_style_value;
    match *underlying_kind {
        BASIC_SHAPE_INSET => Some(StyleValueData::BasicShape {
            shape: OwnedBasicShapeData::new(BasicShapeData {
                kind: *underlying_kind,
                v0: composite_retained_value(underlying_v0, animated_v0, operation)?,
                v1: composite_retained_value(underlying_v1, animated_v1, operation)?,
                v2: composite_retained_value(underlying_v2, animated_v2, operation)?,
                v3: composite_retained_value(underlying_v3, animated_v3, operation)?,
                v4: composite_retained_value(underlying_v4, animated_v4, operation)?,
                fill_rule: 0,
                points: empty_shape_points(),
                path: crate::css::css_path::CssPath::none(),
                commands: crate::css::style_value::ShapeCommands::default(),
            }),
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
                shape: OwnedBasicShapeData::new(BasicShapeData {
                    kind: *underlying_kind,
                    v0: composite_retained_value(underlying_v0, animated_v0, operation)?,
                    v1: position,
                    v2: empty(),
                    v3: empty(),
                    v4: empty(),
                    fill_rule: 0,
                    points: empty_shape_points(),
                    path: crate::css::css_path::CssPath::none(),
                    commands: crate::css::style_value::ShapeCommands::default(),
                }),
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
                shape: OwnedBasicShapeData::new(BasicShapeData {
                    kind: *underlying_kind,
                    v0: empty(),
                    v1: empty(),
                    v2: empty(),
                    v3: empty(),
                    v4: empty(),
                    fill_rule: *underlying_fill_rule,
                    points: RetainedShapePointList::from_retained_points(points),
                    path: crate::css::css_path::CssPath::none(),
                    commands: crate::css::style_value::ShapeCommands::default(),
                }),
            })
        }
        _ => None,
    }
}

/// An offset-path value split into its path and its <coord-box>, which defaults to border-box.
fn split_offset_path(value: &StyleValueData) -> Option<(Option<&StyleValueData>, u16)> {
    use crate::css::css_enums::keyword;
    match value {
        StyleValueData::Keyword { keyword: keyword::NONE } => None,
        StyleValueData::Keyword { keyword } => Some((None, *keyword)),
        StyleValueData::ValueList { values, .. } => match values.as_slice() {
            [path, box_value] => match box_value.data() {
                StyleValueData::Keyword { keyword } => Some((Some(path.data()), *keyword)),
                _ => None,
            },
            _ => None,
        },
        path => Some((Some(path), keyword::BORDER_BOX)),
    }
}

fn join_offset_path(path: StyleValueData, coord_box: u16) -> StyleValueData {
    if coord_box == crate::css::css_enums::keyword::BORDER_BOX {
        return path;
    }
    StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(vec![
            RetainedStyleValueData::from_arc(Arc::new(path)),
            RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Keyword { keyword: coord_box })),
        ]),
        separator: 0,
        collapsible: false,
    }
}

/// https://drafts.csswg.org/motion-1/#offset-path-property
/// Animation type: by computed value
fn interpolate_offset_path(
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> Option<StyleValueData> {
    let (Some((Some(from_path), from_box)), Some((Some(to_path), to_box))) =
        (split_offset_path(from), split_offset_path(to))
    else {
        return None;
    };
    // NB: Paths against different reference boxes do not interpolate, like basic shapes with different reference boxes.
    if from_box != to_box {
        return None;
    }
    let path = match (from_path, to_path) {
        (StyleValueData::Ray { .. }, StyleValueData::Ray { .. }) => {
            interpolate_ray(property_id, from_path, to_path, delta)?
        }
        (StyleValueData::BasicShape { .. }, StyleValueData::BasicShape { .. }) => {
            interpolate_basic_shape(property_id, from_path, to_path, delta)?
        }
        _ => return None,
    };
    Some(join_offset_path(path, from_box))
}

/// https://drafts.csswg.org/motion-1/#ray-function
/// NB: Rays interpolate their angles and positions when they have the same size and containment.
fn interpolate_ray(property_id: u16, from: &StyleValueData, to: &StyleValueData, delta: f32) -> Option<StyleValueData> {
    let (
        StyleValueData::Ray {
            size: from_size,
            contain: from_contain,
            angle: from_angle,
            position: from_position,
        },
        StyleValueData::Ray {
            size: to_size,
            contain: to_contain,
            angle: to_angle,
            position: to_position,
        },
    ) = (from, to)
    else {
        return None;
    };
    if from_size != to_size || from_contain != to_contain {
        return None;
    }
    let angle = interpolate_scalar_value(property_id, from_angle.data(), to_angle.data(), delta, &[]).into_value()?;
    let position = match (from_position.optional_data(), to_position.optional_data()) {
        (None, None) => empty_retained_style_value(),
        (Some(from_position), Some(to_position)) => {
            interpolate_scalar_value(property_id, from_position, to_position, delta, &[]).into_value()?
        }
        _ => return None,
    };
    Some(StyleValueData::Ray {
        size: *from_size,
        contain: *from_contain,
        angle,
        position,
    })
}

fn composite_offset_path(
    underlying: &StyleValueData,
    animated: &StyleValueData,
    operation: FfiCompositeOperation,
) -> Option<StyleValueData> {
    let (Some((Some(underlying_path), underlying_box)), Some((Some(animated_path), animated_box))) =
        (split_offset_path(underlying), split_offset_path(animated))
    else {
        return None;
    };
    if underlying_box != animated_box {
        return None;
    }
    let path = match (underlying_path, animated_path) {
        (
            StyleValueData::Ray {
                size: underlying_size,
                contain: underlying_contain,
                angle: underlying_angle,
                position: underlying_position,
            },
            StyleValueData::Ray {
                size,
                contain,
                angle,
                position,
            },
        ) => {
            if underlying_size != size || underlying_contain != contain {
                return None;
            }
            let position = match (underlying_position.optional_data(), position.optional_data()) {
                (None, None) => empty_retained_style_value(),
                (Some(_), Some(_)) => composite_retained_value(underlying_position, position, operation)?,
                _ => return None,
            };
            StyleValueData::Ray {
                size: *size,
                contain: *contain,
                angle: composite_retained_value(underlying_angle, angle, operation)?,
                position,
            }
        }
        (StyleValueData::BasicShape { .. }, StyleValueData::BasicShape { .. }) => {
            composite_basic_shape(underlying_path, animated_path, operation)?
        }
        _ => return None,
    };
    Some(join_offset_path(path, animated_box))
}

/// An offset-rotate value as whether it has auto, and its angle in degrees.
fn split_offset_rotate(value: &StyleValueData) -> Option<(bool, f64)> {
    use crate::css::css_enums::keyword;
    match value {
        StyleValueData::Keyword { keyword: keyword::AUTO } => Some((true, 0.0)),
        StyleValueData::Angle { value, unit } => Some((false, angle_to_degrees(*value, *unit)?)),
        StyleValueData::ValueList { values, .. } => match values.as_slice() {
            [first, angle] if matches!(first.data(), StyleValueData::Keyword { keyword: keyword::AUTO }) => {
                let StyleValueData::Angle { value, unit } = angle.data() else {
                    return None;
                };
                Some((true, angle_to_degrees(*value, *unit)?))
            }
            _ => None,
        },
        _ => None,
    }
}

fn join_offset_rotate(auto: bool, degrees: f64) -> StyleValueData {
    let angle = StyleValueData::Angle {
        value: degrees,
        unit: 0,
    };
    if !auto {
        return angle;
    }
    StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(vec![
            RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Keyword {
                keyword: crate::css::css_enums::keyword::AUTO,
            })),
            RetainedStyleValueData::from_arc(Arc::new(angle)),
        ]),
        separator: 0,
        collapsible: false,
    }
}

/// https://drafts.csswg.org/motion-1/#offset-rotate-property
/// Animation type: by computed value
/// NB: Values interpolate their angles when both or neither have auto.
fn interpolate_offset_rotate(from: &StyleValueData, to: &StyleValueData, delta: f32) -> Option<StyleValueData> {
    let ((from_auto, from_degrees), (to_auto, to_degrees)) = (split_offset_rotate(from)?, split_offset_rotate(to)?);
    if from_auto != to_auto {
        return None;
    }
    Some(join_offset_rotate(
        from_auto,
        interpolate_f64(from_degrees, to_degrees, delta, None),
    ))
}

fn composite_offset_rotate(underlying: &StyleValueData, animated: &StyleValueData) -> Option<StyleValueData> {
    let ((underlying_auto, underlying_degrees), (animated_auto, animated_degrees)) =
        (split_offset_rotate(underlying)?, split_offset_rotate(animated)?);
    if underlying_auto != animated_auto {
        return None;
    }
    Some(join_offset_rotate(animated_auto, underlying_degrees + animated_degrees))
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
    let angle = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Angle {
        value: angle_degrees,
        unit: 0,
    }));
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
            composite_scalar_value(underlying.data(), animated.data(), operation).into_value()
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

/// Combines two tuples component by component: an absent or keyword component combines only with
/// an equal one, which it keeps, and every other pair combines through `combine`. None where the
/// tuples do not match.
fn combine_tuples(
    from: &RetainedStyleValueDataList,
    to: &RetainedStyleValueDataList,
    combine: impl Fn(&StyleValueData, &StyleValueData) -> AnimationValueResult,
) -> Option<StyleValueData> {
    let (from, to) = (from.as_slice(), to.as_slice());
    if from.len() != to.len() {
        return None;
    }
    let values = from
        .iter()
        .zip(to)
        .map(
            |(from_value, to_value)| match (from_value.optional_data(), to_value.optional_data()) {
                (None, None) => Some(RetainedStyleValueData::none()),
                (Some(StyleValueData::Keyword { keyword: from }), Some(StyleValueData::Keyword { keyword: to })) => {
                    (from == to).then(|| from_value.clone())
                }
                (Some(StyleValueData::Keyword { .. }), _) | (_, Some(StyleValueData::Keyword { .. })) => None,
                (Some(from), Some(to)) => combine(from, to).into_value(),
                _ => None,
            },
        )
        .collect::<Option<Vec<_>>>()?;
    Some(StyleValueData::Tuple {
        values: RetainedStyleValueDataList::from_retained_values(values),
    })
}

fn composite_scalar_value(
    underlying: &StyleValueData,
    animated: &StyleValueData,
    operation: FfiCompositeOperation,
) -> AnimationValueResult {
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
            let number = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number {
                value: (underlying + animated).clamp(0.0, 1.0),
            }));
            owned(StyleValueData::OpacityValue { value: number })
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
            let (Some(x_value), Some(y_value)) = (x.value, y.value) else {
                return handled_without_value();
            };
            owned(StyleValueData::BackgroundSize {
                size_x: x_value,
                size_y: y_value,
            })
        }
        (StyleValueData::Edge { offset: underlying, .. }, StyleValueData::Edge { offset: animated, .. }) => {
            let (Some(underlying), Some(animated)) = (underlying.optional_data(), animated.optional_data()) else {
                return handled_without_value();
            };
            let result = composite_scalar_value(underlying, animated, operation);
            let Some(result_value) = result.into_value() else {
                return handled_without_value();
            };
            owned(StyleValueData::Edge {
                has_edge: false,
                edge: 0,
                offset: result_value,
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
            let (Some(x_value), Some(y_value)) = (x.value, y.value) else {
                return handled_without_value();
            };
            owned(StyleValueData::Position {
                edge_x: x_value,
                edge_y: y_value,
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
                // NB: An auto offset has no amount to add, so combining it with another auto keeps it auto.
                if let (
                    StyleValueData::Keyword {
                        keyword: underlying_keyword,
                    },
                    StyleValueData::Keyword {
                        keyword: animated_keyword,
                    },
                ) = (underlying.data(), animated.data())
                    && underlying_keyword == animated_keyword
                {
                    return Ok(Some(animated.clone_retained()));
                }
                let result = composite_scalar_value(underlying.data(), animated.data(), operation);
                if !result.handled {
                    return Err(());
                }
                let Some(result_value) = result.value else {
                    return Ok(None);
                };
                Ok(Some(result_value))
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
            let Some(horizontal) = horizontal.into_value() else {
                return handled_without_value();
            };
            let vertical = composite_scalar_value(underlying_vertical.data(), animated_vertical.data(), operation);
            let Some(vertical) = vertical.into_value() else {
                return handled_without_value();
            };
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
                let Some(result_value) = result.value else {
                    return Ok(None);
                };
                Ok(Some(result_value))
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
                let Some(result_value) = result.value else {
                    return Ok(None);
                };
                Ok(Some(result_value))
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
                tag_name: underlying_tag,
                packed_tag: underlying_packed_tag,
                value: underlying_value,
                ..
            },
            StyleValueData::OpenTypeTagged {
                tag_name: animated_tag,
                value: animated_value,
                ..
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if underlying_tag != animated_tag {
                return handled_without_value();
            }
            let value = composite_scalar_value(underlying_value.data(), animated_value.data(), operation);
            let Some(retained_value) = value.into_value() else {
                return handled_without_value();
            };
            owned(StyleValueData::OpenTypeTagged {
                mode: OPEN_TYPE_MODE_FONT_VARIATION_SETTINGS,
                tag_name: underlying_tag.clone(),
                packed_tag: *underlying_packed_tag,
                value: retained_value,
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
            if underlying_name != animated_name {
                return handled_without_value();
            }
            let value = composite_scalar_value(underlying_value.data(), animated_value.data(), operation);
            let Some(retained_value) = value.into_value() else {
                return handled_without_value();
            };
            owned(StyleValueData::Function {
                name: underlying_name.clone(),
                value: retained_value,
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
            let Some(result_value) = result.into_value() else {
                return handled_without_value();
            };
            owned(StyleValueData::TextIndent {
                length_percentage: result_value,
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
            StyleValueData::Tuple {
                values: underlying_values,
            },
            StyleValueData::Tuple {
                values: animated_values,
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            combine_tuples(underlying_values, animated_values, |underlying, animated| {
                composite_scalar_value(underlying, animated, operation)
            })
            .map_or_else(handled_without_value, owned)
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
                let Some(result_value) = result.into_value() else {
                    return handled_without_value();
                };
                values.push(result_value);
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
        unsafe { handled_borrowed_value(animated) },
        (StyleValueData::Transformation { property, .. }, StyleValueData::Keyword { keyword })
            if *keyword == crate::css::style_compute::none_keyword() && is_individual_transform_property(*property) =>
        unsafe { handled_borrowed_value(underlying) },
        _ => handled_without_value(),
    }
}

fn interpolate_scalar_value(
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
    range_overrides: &[NumericRangeOverride],
) -> AnimationValueResult {
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
        let range = accepted_range(property_id, VALUE_TYPE_PERCENTAGE, range_overrides);
        return owned(StyleValueData::Percentage {
            value: clamp_to_range(percentage, range),
        });
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
        unsafe { handled_borrowed_value(from_value) },
        (StyleValueData::Tuple { values: from_values }, StyleValueData::Tuple { values: to_values }) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            combine_tuples(from_values, to_values, |from, to| {
                interpolate_scalar_value(property_id, from, to, delta, range_overrides)
            })
            .map_or_else(not_handled, owned)
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
            let number = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number {
                value: interpolate_f64(*from, *to, delta, Some((0.0, 1.0))),
            }));
            owned(StyleValueData::OpacityValue { value: number })
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
            let parameter = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number {
                value: interpolation_value_to_super_ellipse_parameter(interpolated_value),
            }));
            owned(StyleValueData::Superellipse { parameter })
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
            let (Some(x_value), Some(y_value)) = (x.value, y.value) else {
                return handled_without_value();
            };
            owned(StyleValueData::BackgroundSize {
                size_x: x_value,
                size_y: y_value,
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
            let (Some(x_value), Some(y_value)) = (x.value, y.value) else {
                return handled_without_value();
            };
            owned(StyleValueData::Position {
                edge_x: x_value,
                edge_y: y_value,
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
                let Some(result_value) = result.value else {
                    return Ok(None);
                };
                Ok(Some(result_value))
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
            let Some(horizontal) = horizontal.value else {
                return handled_without_value();
            };
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
            let Some(vertical) = vertical.value else {
                return handled_without_value();
            };
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
                let Some(result_value) = result.value else {
                    return Ok(None);
                };
                Ok(Some(result_value))
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
                let Some(result_value) = result.value else {
                    return Ok(None);
                };
                Ok(Some(result_value))
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
                tag_name: from_tag,
                packed_tag: from_packed_tag,
                value: from_value,
                ..
            },
            StyleValueData::OpenTypeTagged {
                tag_name: to_tag,
                value: to_value,
                ..
            },
        ) => {
            // https://drafts.csswg.org/web-animations-1/#animating-properties
            // Corresponding individual components of the computed values are combined (interpolated, added, or accumulated) using the indicated procedure for that value type (see CSS Values 4 § 3 Combining Values: Interpolation, Addition, and Accumulation).
            // If the number of components or the types of corresponding components do not match, or if any component value uses discrete animation and the two corresponding values do not match, then the property values combine as discrete.
            if from_tag != to_tag {
                return handled_without_value();
            }
            let value =
                interpolate_scalar_value(property_id, from_value.data(), to_value.data(), delta, range_overrides);
            if !value.handled {
                return not_handled();
            }
            let Some(retained_value) = value.value else {
                return handled_without_value();
            };
            owned(StyleValueData::OpenTypeTagged {
                mode: OPEN_TYPE_MODE_FONT_VARIATION_SETTINGS,
                tag_name: from_tag.clone(),
                packed_tag: *from_packed_tag,
                value: retained_value,
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
            if from_name != to_name {
                return handled_without_value();
            }
            let value =
                interpolate_scalar_value(property_id, from_value.data(), to_value.data(), delta, range_overrides);
            if !value.handled {
                return not_handled();
            }
            let Some(retained_value) = value.value else {
                return handled_without_value();
            };
            owned(StyleValueData::Function {
                name: from_name.clone(),
                value: retained_value,
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
            let Some(result_value) = result.value else {
                return handled_without_value();
            };
            owned(StyleValueData::TextIndent {
                length_percentage: result_value,
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
            owned(StyleValueData::Ratio {
                numerator: RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number { value })),
                denominator: RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number { value: 1.0 })),
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
                let Some(result_value) = result.value else {
                    return handled_without_value();
                };
                values.push(result_value);
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
        let argument = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number { value }));
        arguments.push(argument);
    }
    let angle = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Angle {
        value: result_angle.to_degrees(),
        unit: 0,
    }));
    arguments.push(angle);

    Some(StyleValueData::Transformation {
        property,
        transform_function,
        values: RetainedStyleValueDataList::from_retained_values(arguments),
    })
}

fn retained_number(value: f64) -> RetainedStyleValueData {
    RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number { value }))
}

fn retained_zero_px() -> RetainedStyleValueData {
    RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Length {
        value: 0.0,
        unit: crate::css::style_compute::px_length_unit(),
    }))
}

fn retained_length(value: f64, unit: u8) -> RetainedStyleValueData {
    RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Length { value, unit }))
}

fn retained_none_keyword() -> RetainedStyleValueData {
    RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Keyword {
        keyword: crate::css::style_compute::none_keyword(),
    }))
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
            let angle = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Angle { value: 0.0, unit: 0 }));
            vec![angle]
        }
        TRANSFORM_FUNCTION_ROTATE_3D => vec![
            retained_number(1.0),
            retained_number(1.0),
            retained_number(1.0),
            RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Angle { value: 0.0, unit: 0 })),
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
    let transformation = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Transformation {
        property,
        transform_function: function,
        values: RetainedStyleValueDataList::from_retained_values(arguments),
    }));
    Some(transformation)
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
    // NB: Unlike the other matrices in the pseudo-code, rotationMatrix is indexed by row and then column. Read the
    //     other way, it would be the inverse of the rotation that decomposing a matrix extracts.
    let mut rotation_matrix = identity_matrix();
    rotation_matrix[0][0] = 1.0 - 2.0 * (y * y + z * z);
    rotation_matrix[0][1] = 2.0 * (x * y - z * w);
    rotation_matrix[0][2] = 2.0 * (x * z + y * w);
    rotation_matrix[1][0] = 2.0 * (x * y + z * w);
    rotation_matrix[1][1] = 1.0 - 2.0 * (x * x + z * z);
    rotation_matrix[1][2] = 2.0 * (y * z - x * w);
    rotation_matrix[2][0] = 2.0 * (x * z - y * w);
    rotation_matrix[2][1] = 2.0 * (y * z + x * w);
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
    let from = post_multiply_transformations(context, from).ok_or(TransformMatrixInterpolationError::NotConvertible)?;
    let to = post_multiply_transformations(context, to).ok_or(TransformMatrixInterpolationError::NotConvertible)?;
    let matrix = interpolate_matrices(from, to, delta).ok_or(TransformMatrixInterpolationError::NonInvertible)?;
    let transformation = RetainedStyleValueData::from_arc(Arc::new(matrix_transformation(property, matrix)));
    Ok(transformation)
}

fn pad_transform_lists(
    from_values: &mut Vec<RetainedStyleValueData>,
    to_values: &mut Vec<RetainedStyleValueData>,
) -> Option<()> {
    // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
    // * Treating none as a list of zero length, if Va or Vb differ in length:
    //   * extend the shorter list to the length of the longer list, setting the function at each additional
    //     position to the identity transform function matching the function at the corresponding position in the
    //     longer list. Both transform function lists are then interpolated following the next rule.
    if from_values.len() != to_values.len() {
        let (shorter, longer) = if from_values.len() < to_values.len() {
            (from_values, &*to_values)
        } else {
            (to_values, &*from_values)
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
    Some(())
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

    let (from_values, from_separator, from_collapsible) = transform_list_values(from)?;
    let (to_values, to_separator, to_collapsible) = transform_list_values(to)?;
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
    let mut from_values: Vec<_> = from_values.iter().map(RetainedStyleValueData::clone_retained).collect();
    let mut to_values: Vec<_> = to_values.iter().map(RetainedStyleValueData::clone_retained).collect();
    pad_transform_lists(&mut from_values, &mut to_values)?;

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
        if from_function == to_function && *from_function == TRANSFORM_FUNCTION_PERSPECTIVE {
            let ([from_argument], [to_argument]) = (from_arguments.as_slice(), to_arguments.as_slice()) else {
                return None;
            };
            let (Some((from_reciprocal_depth, from_unit)), Some((to_reciprocal_depth, to_unit))) = (
                reciprocal_perspective_depth(from_argument),
                reciprocal_perspective_depth(to_argument),
            ) else {
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
            let argument = perspective_from_reciprocal_depth(reciprocal_depth, from_unit.or(to_unit));
            let transformation = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Transformation {
                property: *from_property,
                transform_function: *from_function,
                values: RetainedStyleValueDataList::from_retained_values(vec![argument]),
            }));
            transformations.push(transformation);
            continue;
        }
        let Some((transform_function, from_arguments, to_arguments)) =
            convert_transform_pair_to_common_primitive(*from_function, from_arguments, *to_function, to_arguments)
        else {
            // https://drafts.csswg.org/css-transforms-2/#interpolation-of-transform-functions
            // The transform functions <matrix()>, matrix3d() and perspective() get converted into 4x4 matrices first and
            // interpolated as defined in section Interpolation of Matrices afterwards.
            let is_matrix_pair = from_function == to_function
                && matches!(*from_function, TRANSFORM_FUNCTION_MATRIX | TRANSFORM_FUNCTION_MATRIX_3D);
            // https://drafts.csswg.org/css-transforms-1/#interpolation-of-transforms
            //   * If the pair do not have a common name or primitive transform function, post-multiply the remaining
            //     transform functions in each of Va and Vb respectively to produce two 4x4 matrices. Interpolate these two
            //     matrices as described in § 11 Interpolation of Matrices, append the result to Vresult, and cease
            //     iterating over Va and Vb.
            let end = if is_matrix_pair { index + 1 } else { from_values.len() };
            let transformation = interpolate_transform_matrix_suffix(
                context,
                *from_property,
                &from_values[index..end],
                &to_values[index..end],
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
            if is_matrix_pair {
                continue;
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
            let transformation = RetainedStyleValueData::from_arc(Arc::new(transformation));
            transformations.push(transformation);
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
            arguments.push(interpolate_scalar_value(property_id, from.data(), to.data(), delta, &[]).into_value()?);
        }
        let transformation = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Transformation {
            property: *from_property,
            transform_function,
            values: RetainedStyleValueDataList::from_retained_values(arguments),
        }));
        transformations.push(transformation);
    }

    Some(Some(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(transformations),
        separator,
        collapsible,
    }))
}

fn transform_list_values(value: &StyleValueData) -> Option<(&[RetainedStyleValueData], u8, bool)> {
    match value {
        StyleValueData::ValueList {
            values,
            separator,
            collapsible,
        } => Some((values.as_slice(), *separator, *collapsible)),
        StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword() => {
            Some((&[], 0, false))
        }
        _ => None,
    }
}

fn transform_list_from_values(values: Vec<RetainedStyleValueData>, separator: u8, collapsible: bool) -> StyleValueData {
    if values.is_empty() {
        return StyleValueData::Keyword {
            keyword: crate::css::style_compute::none_keyword(),
        };
    }
    StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(values),
        separator,
        collapsible,
    }
}

fn post_multiply_transformations(
    context: Option<&FfiAnimationContext>,
    transformations: &[RetainedStyleValueData],
) -> Option<Matrix4> {
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
}

// https://drafts.csswg.org/css-transforms-2/#combining-transform-lists
// Instead of interpolating the individual parameters, they are combined using arithmetic addition-- except in the
// case of parameters whose value is one in the identity transform function (e.g. scale parameters and matrix elements
// m11, m22, m33, and m44), which combine using accumulation for one-based values as follows:
//     Vresult = Va + Vb - 1
fn accumulate_decomposed_matrices(
    underlying: DecomposedMatrix,
    animated: DecomposedMatrix,
) -> Option<DecomposedMatrix> {
    let add =
        |underlying: [f64; 3], animated: [f64; 3]| std::array::from_fn(|index| underlying[index] + animated[index]);
    let mut perspective: [f64; 4] =
        std::array::from_fn(|index| underlying.perspective[index] + animated.perspective[index]);
    perspective[3] -= 1.0;

    // INTEROP: A quaternion has no meaningful per-component addition, so the rotations accumulate by composing them,
    //          which is what the rotations of an accumulated transform list do.
    let rotation_matrix = |rotation: [f64; 4]| {
        recompose_matrix(DecomposedMatrix {
            translation: [0.0; 3],
            scale: [1.0; 3],
            skew: [0.0; 3],
            rotation,
            perspective: [0.0, 0.0, 0.0, 1.0],
        })
    };
    let rotation = decompose_matrix(multiply_matrices(
        rotation_matrix(underlying.rotation),
        rotation_matrix(animated.rotation),
    ))?
    .rotation;

    Some(DecomposedMatrix {
        translation: add(underlying.translation, animated.translation),
        scale: std::array::from_fn(|index| underlying.scale[index] + animated.scale[index] - 1.0),
        skew: add(underlying.skew, animated.skew),
        rotation,
        perspective,
    })
}

fn accumulate_matrices(underlying: Matrix4, animated: Matrix4) -> Option<Matrix4> {
    let underlying = decompose_matrix(underlying)?;
    let animated = decompose_matrix(animated)?;
    Some(recompose_matrix(accumulate_decomposed_matrices(underlying, animated)?))
}

fn rotation_quaternion(axis: [f64; 3], angle: f64) -> Option<[f64; 4]> {
    let length = vector_length(axis);
    if length < 1e-5 {
        return (angle == 0.0).then_some([0.0, 0.0, 0.0, 1.0]);
    }
    let sin_half_angle = (angle / 2.0).sin();
    Some([
        axis[0] / length * sin_half_angle,
        axis[1] / length * sin_half_angle,
        axis[2] / length * sin_half_angle,
        (angle / 2.0).cos(),
    ])
}

fn accumulate_rotate_3d(
    context: Option<&FfiAnimationContext>,
    underlying: &[RetainedStyleValueData],
    animated: &[RetainedStyleValueData],
) -> Option<Vec<RetainedStyleValueData>> {
    let decode = |arguments: &[RetainedStyleValueData]| {
        let [x, y, z, angle] = arguments else {
            return None;
        };
        Some((
            [
                resolve_animation_number(context, x.data())?,
                resolve_animation_number(context, y.data())?,
                resolve_animation_number(context, z.data())?,
            ],
            resolve_animation_angle(context, angle.data())?,
        ))
    };
    let (underlying_axis, underlying_angle) = decode(underlying)?;
    let (animated_axis, animated_angle) = decode(animated)?;
    let rotation = |axis: [f64; 3], angle: f64| {
        let mut arguments: Vec<_> = axis.into_iter().map(retained_number).collect();
        arguments.push(RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Angle {
            value: angle,
            unit: 0,
        })));
        arguments
    };

    let normalize = |axis: [f64; 3]| {
        let length = vector_length(axis);
        (length >= 1e-5).then(|| axis.map(|component| component / length))
    };
    let (underlying_normalized, animated_normalized) = (normalize(underlying_axis), normalize(animated_axis));
    if underlying_angle == 0.0 {
        return Some(rotation(animated_axis, animated_angle));
    }
    if animated_angle == 0.0 {
        return Some(rotation(underlying_axis, underlying_angle));
    }
    if let (Some(underlying_normalized), Some(animated_normalized)) = (underlying_normalized, animated_normalized)
        && vector_length(combine_vectors(underlying_normalized, animated_normalized, 1.0, -1.0)) < 1e-5
    {
        return Some(rotation(underlying_axis, underlying_angle + animated_angle));
    }

    // NB: Rotations about different axes accumulate by composing them, which multiplying their quaternions does.
    let [ax, ay, az, aw] = rotation_quaternion(underlying_axis, underlying_angle.to_radians())?;
    let [bx, by, bz, bw] = rotation_quaternion(animated_axis, animated_angle.to_radians())?;
    let quaternion = [
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ];
    let axis = [quaternion[0], quaternion[1], quaternion[2]];
    let sin_half_angle = vector_length(axis);
    if sin_half_angle < 1e-9 {
        return Some(rotation([0.0, 0.0, 1.0], 0.0));
    }
    let angle = 2.0 * sin_half_angle.atan2(quaternion[3]);
    Some(rotation(
        axis.map(|component| component / sin_half_angle),
        angle.to_degrees(),
    ))
}

fn reciprocal_perspective_depth(argument: &RetainedStyleValueData) -> Option<(f64, Option<u8>)> {
    match argument.data() {
        StyleValueData::Length { value, unit } => Some((1.0 / value.max(1.0), Some(*unit))),
        StyleValueData::Keyword { keyword } if *keyword == crate::css::style_compute::none_keyword() => {
            Some((0.0, None))
        }
        _ => None,
    }
}

fn perspective_from_reciprocal_depth(reciprocal_depth: f64, unit: Option<u8>) -> RetainedStyleValueData {
    // NB: A depth that is not positive has no perspective effect, which perspective(none) expresses.
    if reciprocal_depth <= 0.0 {
        return retained_none_keyword();
    }
    RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Length {
        value: 1.0 / reciprocal_depth,
        unit: unit.unwrap_or_else(crate::css::style_compute::px_length_unit),
    }))
}

// Accumulates a pair of transform functions that share a name or primitive, or returns None where only their
// matrices can be accumulated.
fn accumulate_transform_functions(
    context: Option<&FfiAnimationContext>,
    property: u16,
    underlying: &StyleValueData,
    animated: &StyleValueData,
) -> Option<RetainedStyleValueData> {
    let (
        StyleValueData::Transformation {
            transform_function: underlying_function,
            values: underlying_arguments,
            ..
        },
        StyleValueData::Transformation {
            transform_function: animated_function,
            values: animated_arguments,
            ..
        },
    ) = (underlying, animated)
    else {
        return None;
    };
    let transformation = |transform_function: u8, arguments: Vec<RetainedStyleValueData>| {
        RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Transformation {
            property,
            transform_function,
            values: RetainedStyleValueDataList::from_retained_values(arguments),
        }))
    };

    if underlying_function == animated_function && *underlying_function == TRANSFORM_FUNCTION_PERSPECTIVE {
        let ([underlying_argument], [animated_argument]) =
            (underlying_arguments.as_slice(), animated_arguments.as_slice())
        else {
            return None;
        };
        let (underlying_reciprocal_depth, underlying_unit) = reciprocal_perspective_depth(underlying_argument)?;
        let (animated_reciprocal_depth, animated_unit) = reciprocal_perspective_depth(animated_argument)?;
        if underlying_unit.is_some() && animated_unit.is_some() && underlying_unit != animated_unit {
            return None;
        }
        // NB: The varying component of a perspective matrix is the negative reciprocal of its depth, which is zero in
        //     the identity transform function, so accumulating it adds the reciprocals.
        let argument = perspective_from_reciprocal_depth(
            underlying_reciprocal_depth + animated_reciprocal_depth,
            underlying_unit.or(animated_unit),
        );
        return Some(transformation(TRANSFORM_FUNCTION_PERSPECTIVE, vec![argument]));
    }

    let (transform_function, underlying_arguments, animated_arguments) = convert_transform_pair_to_common_primitive(
        *underlying_function,
        underlying_arguments,
        *animated_function,
        animated_arguments,
    )?;
    let arguments = match transform_function {
        TRANSFORM_FUNCTION_ROTATE_3D => accumulate_rotate_3d(context, &underlying_arguments, &animated_arguments)?,
        TRANSFORM_FUNCTION_SCALE | TRANSFORM_FUNCTION_SCALE_3D => underlying_arguments
            .iter()
            .zip(&animated_arguments)
            .map(|(underlying, animated)| {
                Some(retained_number(
                    resolve_animation_number(context, underlying.data())?
                        + resolve_animation_number(context, animated.data())?
                        - 1.0,
                ))
            })
            .collect::<Option<Vec<_>>>()?,
        TRANSFORM_FUNCTION_ROTATE
        | TRANSFORM_FUNCTION_ROTATE_X
        | TRANSFORM_FUNCTION_ROTATE_Y
        | TRANSFORM_FUNCTION_ROTATE_Z
        | TRANSFORM_FUNCTION_SKEW
        | TRANSFORM_FUNCTION_SKEW_X
        | TRANSFORM_FUNCTION_SKEW_Y
        | TRANSFORM_FUNCTION_TRANSLATE
        | TRANSFORM_FUNCTION_TRANSLATE_3D
        | TRANSFORM_FUNCTION_TRANSLATE_X
        | TRANSFORM_FUNCTION_TRANSLATE_Y
        | TRANSFORM_FUNCTION_TRANSLATE_Z => underlying_arguments
            .iter()
            .zip(&animated_arguments)
            .map(|(underlying, animated)| {
                composite_scalar_value(underlying.data(), animated.data(), FfiCompositeOperation::Add).into_value()
            })
            .collect::<Option<Vec<_>>>()?,
        _ => return None,
    };
    Some(transformation(transform_function, arguments))
}

// https://drafts.csswg.org/css-transforms-2/#combining-transform-lists
// Returns None where the lists do not combine and the animated value replaces the underlying one.
fn composite_transform_list(
    context: &FfiAnimationContext,
    underlying: &StyleValueData,
    animated: &StyleValueData,
    operation: FfiCompositeOperation,
) -> Option<StyleValueData> {
    if matches!(operation, FfiCompositeOperation::Replace) {
        return None;
    }
    let (underlying_values, underlying_separator, underlying_collapsible) = transform_list_values(underlying)?;
    let (animated_values, animated_separator, animated_collapsible) = transform_list_values(animated)?;
    let (separator, collapsible) = if underlying_values.is_empty() {
        (animated_separator, animated_collapsible)
    } else {
        (underlying_separator, underlying_collapsible)
    };

    match operation {
        FfiCompositeOperation::Replace => unreachable!(),
        FfiCompositeOperation::Add => {
            // Addition of two transform lists Va and Vb is defined as list concatenation such that Vresult is equal to
            // Vb appended to Va.
            let values = underlying_values
                .iter()
                .chain(animated_values)
                .map(RetainedStyleValueData::clone_retained)
                .collect();
            Some(transform_list_from_values(values, separator, collapsible))
        }
        FfiCompositeOperation::Accumulate => {
            // Accumulation of two transform lists Va and Vb follows the same steps as interpolation with regards to
            // matching transform functions including padding lists with identity transform functions, converting
            // none to an identity transform function, and converting both arguments to matrices as necessary (see
            // CSS Transforms 1 § 9 Interpolation of Transforms).
            let mut underlying_values: Vec<_> = underlying_values
                .iter()
                .map(RetainedStyleValueData::clone_retained)
                .collect();
            let mut animated_values: Vec<_> = animated_values
                .iter()
                .map(RetainedStyleValueData::clone_retained)
                .collect();
            pad_transform_lists(&mut underlying_values, &mut animated_values)?;

            let mut values = Vec::with_capacity(underlying_values.len());
            for (index, (underlying_value, animated_value)) in
                underlying_values.iter().zip(&animated_values).enumerate()
            {
                let StyleValueData::Transformation { property, .. } = underlying_value.data() else {
                    return None;
                };
                if let Some(value) = accumulate_transform_functions(
                    Some(context),
                    *property,
                    underlying_value.data(),
                    animated_value.data(),
                ) {
                    values.push(value);
                    continue;
                }
                let is_matrix_pair = matches!(
                    (underlying_value.data(), animated_value.data()),
                    (
                        StyleValueData::Transformation { transform_function: underlying_function, .. },
                        StyleValueData::Transformation { transform_function: animated_function, .. },
                    ) if underlying_function == animated_function
                        && matches!(*underlying_function, TRANSFORM_FUNCTION_MATRIX | TRANSFORM_FUNCTION_MATRIX_3D)
                );
                let end = if is_matrix_pair {
                    index + 1
                } else {
                    underlying_values.len()
                };
                let underlying_matrix = post_multiply_transformations(Some(context), &underlying_values[index..end])?;
                let animated_matrix = post_multiply_transformations(Some(context), &animated_values[index..end])?;
                // NB: Accumulating a non-invertible matrix falls back to replacing the underlying value.
                let matrix = accumulate_matrices(underlying_matrix, animated_matrix)?;
                values.push(RetainedStyleValueData::from_arc(Arc::new(matrix_transformation(
                    *property, matrix,
                ))));
                if !is_matrix_pair {
                    break;
                }
            }
            Some(transform_list_from_values(values, separator, collapsible))
        }
    }
}

fn retained_legacy_color(red: u8, green: u8, blue: u8, alpha: u8) -> RetainedStyleValueData {
    RetainedStyleValueData::from_arc(Arc::new(StyleValueData::ColorFunction {
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
    }))
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
    let current_color = context?.current_color?;
    current_color.optional_data()?;
    Some(current_color.clone())
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
            let current_color = context?.current_color?.optional_data()?;
            let (components, missing) = legacy_srgb_components(current_color)?;
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
    let value = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Shadow {
        shadow_type: *shadow_type,
        color: retained_transparent_legacy_color(),
        offset_x: retained_zero_px(),
        offset_y: retained_zero_px(),
        blur_radius: retained_zero_px(),
        spread_distance: retained_zero_px(),
        placement: *placement,
    }));
    Some(value)
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
    // https://drafts.csswg.org/web-animations-1/#animating-shadow-lists
    // However, if both input shadows are inset or both input shadows are not inset, then the interpolated shadow must
    // match the input shadows in that regard. If any pair of input shadows has one inset and the other not inset, the
    // entire shadow-list uses discrete animation.
    if from_shadow_type != to_shadow_type || from_placement != to_placement {
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
        interpolate_scalar_value(property_id, from.data(), to.data(), delta, ranges).into_value()
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
        placement: *from_placement,
    })
}

fn shadow_list_entries(value: &StyleValueData) -> Option<(Vec<RetainedStyleValueData>, u8, bool)> {
    // https://drafts.csswg.org/web-animations-1/#animating-shadow-lists
    // Each shadow in the list (treating none as a 0-length list) is interpolated component-wise as with by computed
    // value behavior.
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
}

fn pad_shadow_lists(first: &mut Vec<RetainedStyleValueData>, second: &mut Vec<RetainedStyleValueData>) -> bool {
    // https://drafts.csswg.org/web-animations-1/#animating-shadow-lists
    // If the lists of shadows have different lengths, then the shorter list is padded at the end with shadows whose
    // color is transparent, whose lengths are all 0, and whose inset (or not) matches the longer list.
    while first.len() < second.len() {
        let Some(shadow) = blank_shadow(second[first.len()].data()) else {
            return false;
        };
        first.push(shadow);
    }
    while second.len() < first.len() {
        let Some(shadow) = blank_shadow(first[second.len()].data()) else {
            return false;
        };
        second.push(shadow);
    }
    true
}

fn accumulate_shadow(
    context: &FfiAnimationContext,
    underlying: &StyleValueData,
    animated: &StyleValueData,
) -> Option<StyleValueData> {
    let (
        StyleValueData::Shadow {
            shadow_type: underlying_shadow_type,
            color: underlying_color,
            offset_x: underlying_offset_x,
            offset_y: underlying_offset_y,
            blur_radius: underlying_blur_radius,
            spread_distance: underlying_spread_distance,
            placement: underlying_placement,
        },
        StyleValueData::Shadow {
            shadow_type: animated_shadow_type,
            color: animated_color,
            offset_x: animated_offset_x,
            offset_y: animated_offset_y,
            blur_radius: animated_blur_radius,
            spread_distance: animated_spread_distance,
            placement: animated_placement,
        },
    ) = (underlying, animated)
    else {
        return None;
    };
    if underlying_shadow_type != animated_shadow_type || underlying_placement != animated_placement {
        return None;
    }

    let resolve_length = |value: &RetainedStyleValueData| match value.optional_data() {
        Some(value) => resolve_animation_length(Some(context), value),
        None => Some(0.0),
    };
    let add_lengths = |underlying: &RetainedStyleValueData, animated: &RetainedStyleValueData| {
        Some(retained_length(
            resolve_length(underlying)? + resolve_length(animated)?,
            crate::css::style_compute::px_length_unit(),
        ))
    };
    let underlying_color = resolve_animation_color(Some(context), underlying_color)?;
    let animated_color = resolve_animation_color(Some(context), animated_color)?;
    let color = composite_scalar_value(
        underlying_color.data(),
        animated_color.data(),
        FfiCompositeOperation::Accumulate,
    )
    .into_value()?;

    Some(StyleValueData::Shadow {
        shadow_type: *underlying_shadow_type,
        color,
        offset_x: add_lengths(underlying_offset_x, animated_offset_x)?,
        offset_y: add_lengths(underlying_offset_y, animated_offset_y)?,
        blur_radius: add_lengths(underlying_blur_radius, animated_blur_radius)?,
        spread_distance: if underlying_spread_distance.optional_data().is_some()
            || animated_spread_distance.optional_data().is_some()
        {
            add_lengths(underlying_spread_distance, animated_spread_distance)?
        } else {
            empty_retained_style_value()
        },
        placement: *underlying_placement,
    })
}

fn composite_shadow_list(
    context: &FfiAnimationContext,
    underlying: &StyleValueData,
    animated: &StyleValueData,
    operation: FfiCompositeOperation,
) -> AnimationValueResult {
    let (
        Some((mut underlying_shadows, underlying_separator, underlying_collapsible)),
        Some((mut animated_shadows, animated_separator, animated_collapsible)),
    ) = (shadow_list_entries(underlying), shadow_list_entries(animated))
    else {
        return handled_without_value();
    };
    let (separator, collapsible) = if underlying_shadows.is_empty() {
        (animated_separator, animated_collapsible)
    } else {
        (underlying_separator, underlying_collapsible)
    };

    let shadows = match operation {
        FfiCompositeOperation::Replace => return handled_without_value(),
        FfiCompositeOperation::Add => {
            // https://drafts.csswg.org/web-animations-1/#animating-shadow-lists
            // Addition of two shadow lists Va and Vb is defined as list concatenation such that Vresult is equal to Va
            // extended with Vb.
            underlying_shadows.extend(animated_shadows);
            underlying_shadows
        }
        FfiCompositeOperation::Accumulate => {
            // https://drafts.csswg.org/web-animations-1/#animating-shadow-lists
            // Accumulation of shadow lists follows the matching rules for interpolation above, performing addition on
            // each component according to its type, or falling back to discrete animation if the inset values do not
            // match.
            if !pad_shadow_lists(&mut underlying_shadows, &mut animated_shadows) {
                return handled_without_value();
            }
            let mut shadows = Vec::with_capacity(underlying_shadows.len());
            for (underlying_shadow, animated_shadow) in underlying_shadows.iter().zip(&animated_shadows) {
                let Some(shadow) = accumulate_shadow(context, underlying_shadow.data(), animated_shadow.data()) else {
                    return handled_without_value();
                };
                shadows.push(RetainedStyleValueData::from_arc(Arc::new(shadow)));
            }
            shadows
        }
    };
    if shadows.is_empty() {
        return handled_retained_value(retained_none_keyword());
    }
    owned(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(shadows),
        separator,
        collapsible,
    })
}

fn interpolate_shadow_list(
    context: Option<&FfiAnimationContext>,
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> AnimationValueResult {
    // https://drafts.csswg.org/css-backgrounds/#box-shadow
    // Animation type: by computed value, treating none as a zero-item list and appending blank shadows
    //                 (transparent 0 0 0 0) with a corresponding inset keyword as needed to match the longer list if
    //                 the shorter list is otherwise compatible with the longer one
    let Some((mut from_shadows, from_separator, from_collapsible)) = shadow_list_entries(from) else {
        return not_handled();
    };
    let Some((mut to_shadows, to_separator, to_collapsible)) = shadow_list_entries(to) else {
        return not_handled();
    };
    let from_was_empty = from_shadows.is_empty();

    if !pad_shadow_lists(&mut from_shadows, &mut to_shadows) {
        return not_handled();
    }

    let mut shadows = Vec::with_capacity(from_shadows.len());
    for (from_shadow, to_shadow) in from_shadows.iter().zip(&to_shadows) {
        let Some(shadow) = interpolate_shadow(context, property_id, from_shadow.data(), to_shadow.data(), delta) else {
            return discrete_value(context, from, to, delta);
        };
        let shadow = RetainedStyleValueData::from_arc(Arc::new(shadow));
        shadows.push(shadow);
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

fn retained_filter(kind: u8, color_operation: u8, value: RetainedStyleValueData) -> RetainedStyleValueData {
    RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Filter {
        kind,
        color_operation,
        value,
    }))
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
            RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Shadow {
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
            }))
        }
        FILTER_KIND_HUE_ROTATE => {
            RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Angle { value: 0.0, unit: 0 }))
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
            interpolate_scalar_value(
                property_id,
                from_value.data(),
                to_value.data(),
                delta,
                NONNEGATIVE_LENGTH_RANGE,
            )
            .into_value()
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
            RetainedStyleValueData::from_arc(Arc::new(shadow))
        }
        FILTER_KIND_HUE_ROTATE => {
            let from = resolve_animation_angle(context, from_value.data())?;
            let to = resolve_animation_angle(context, to_value.data())?;
            RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Angle {
                value: interpolate_f64(from, to, delta, None),
                unit: 0,
            }))
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
            interpolate_scalar_value(property_id, from_value.data(), to_value.data(), delta, ranges).into_value()?
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
        FILTER_KIND_HUE_ROTATE => RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Angle {
            value: resolve_animation_angle(Some(context), underlying_value.data())?
                + resolve_animation_angle(Some(context), animated_value.data())?,
            unit: 0,
        })),
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
            RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Shadow {
                shadow_type: *shadow_type,
                color,
                offset_x,
                offset_y,
                blur_radius,
                spread_distance: empty_retained_style_value(),
                placement: *placement,
            }))
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
) -> AnimationValueResult {
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
) -> AnimationValueResult {
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
) -> AnimationValueResult {
    if property_id == crate::css::property_metadata::property_id::CUSTOM {
        // https://drafts.css-houdini.org/css-properties-values-api/#animation-behavior-of-custom-properties
        // When referenced by animations and transitions, custom property values interpolate by computed value, in accordance with the type that they parsed as.
        let result = interpolate_scalar_value(property_id, from, to, delta, &[]);
        if !result.handled || result.value.is_none() && context.is_some_and(|context| context.allow_discrete) {
            return discrete_value(context, from, to, delta);
        }
        return result;
    }
    let animation_type = property_animation_type(property_id);
    if animation_type == ANIMATION_TYPE_NONE {
        // https://www.w3.org/TR/web-animations-1/#not-animatable
        // The property is not animatable. It is not processed when listed in an animation keyframe, and is not affected by transitions.
        // NB: Such values are normally filtered before evaluation. Preserve the C++ scalar API's
        //     existing endpoint behavior if one reaches this lower-level operation.
        return unsafe { handled_borrowed_value(to) };
    }
    if animation_type == ANIMATION_TYPE_DISCRETE {
        // https://www.w3.org/TR/web-animations-1/#discrete
        // The property’s values cannot be meaningfully combined, thus it is not additive and interpolation swaps from Va to Vb at 50% (p=0.5), i.e.
        return discrete_value(context, from, to, delta);
    }
    if is_sizing_property(property_id)
        && let Some(value) = interpolate_calc_size(context, from, to, delta)
    {
        return owned(value);
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
            if result.handled && result.value.is_some() {
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
            let Some(result_value) = result.into_value() else {
                return discrete_value(context, from, to, delta);
            };
            values.push(result_value);
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
                RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Angle {
                    value: interpolate_f64(from_value, to_value, delta, Some((-90.0, 90.0))),
                    unit: 0,
                }))
            }
            _ => RetainedStyleValueData::none(),
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
    if animation_type == ANIMATION_TYPE_CUSTOM && property_id == crate::css::property_metadata::property_id::OFFSET_PATH
    {
        return interpolate_offset_path(property_id, from, to, delta)
            .map_or_else(|| discrete_value(context, from, to, delta), owned);
    }
    if animation_type == ANIMATION_TYPE_CUSTOM
        && property_id == crate::css::property_metadata::property_id::OFFSET_ROTATE
    {
        return interpolate_offset_rotate(from, to, delta)
            .map_or_else(|| discrete_value(context, from, to, delta), owned);
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
    if !result.handled || result.value.is_none() && context.is_some_and(|context| context.allow_discrete) {
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
    } else if matches!(operation, FfiCompositeOperation::Replace) {
        handled_without_value()
    } else if matches!(
        property_id,
        crate::css::property_metadata::property_id::BOX_SHADOW
            | crate::css::property_metadata::property_id::TEXT_SHADOW
    ) {
        composite_shadow_list(context, underlying, animated, operation)
    } else if property_id == crate::css::property_metadata::property_id::OFFSET_PATH {
        composite_offset_path(underlying, animated, operation).map_or_else(handled_without_value, owned)
    } else if property_id == crate::css::property_metadata::property_id::OFFSET_ROTATE {
        composite_offset_rotate(underlying, animated).map_or_else(handled_without_value, owned)
    } else if property_id == crate::css::property_metadata::property_id::TRANSFORM {
        composite_transform_list(context, underlying, animated, operation).map_or_else(handled_without_value, owned)
    } else {
        composite_scalar_value(underlying, animated, operation)
    };
    assert!(result.handled);
    let Some(value) = result.value else {
        return BatchAnimationValue::Borrowed(animated);
    };
    BatchAnimationValue::Owned(value.into_arc())
}

/// The endpoint indices in a property's keyframe sequence for the current sample.
fn keyframe_interval(keyframe_count: usize, current_key: f64, key_at: impl Fn(usize) -> i64) -> (usize, usize) {
    assert!(keyframe_count >= 2);
    // https://drafts.csswg.org/web-animations-1/#the-effect-value-of-a-keyframe-animation-effect
    // 1. Let interval endpoints be an empty sequence of keyframes.
    // 2. Populate interval endpoints by following the steps from the first matching condition from below:
    // NB: The first and the last property-specific keyframes are at the computed keyframe offsets 0 and 1.
    let last_index = keyframe_count - 1;

    let mut start_index = 0;
    let mut end_index = 1;
    if current_key < key_at(0) as f64 && key_at(1) == key_at(0) {
        // If iteration progress < 0 and there is more than one keyframe in property-specific keyframes with a computed
        // keyframe offset of 0,
        //     Add the first keyframe in property-specific keyframes to interval endpoints.
        end_index = start_index;
    } else if current_key >= key_at(last_index) as f64 && key_at(last_index - 1) == key_at(last_index) {
        // If iteration progress ≥ 1 and there is more than one keyframe in property-specific keyframes with a computed
        // keyframe offset of 1,
        //     Add the last keyframe in property-specific keyframes to interval endpoints.
        start_index = last_index;
        end_index = last_index;
    } else {
        // Otherwise,
        // 1. Append to interval endpoints the last keyframe in property-specific keyframes whose computed keyframe offset is less than or equal
        //    to iteration progress and less than 1. If there is no such keyframe (because, for example, the iteration progress is negative),
        //    add the last keyframe whose computed keyframe offset is 0.
        // 2. Append to interval endpoints the next keyframe in property-specific keyframes after the one added in the previous step.
        for next_index in 2..keyframe_count {
            if current_key < key_at(end_index) as f64 {
                break;
            }
            start_index = end_index;
            end_index = next_index;
        }
    }

    (start_index, end_index)
}

fn evaluate_animation_value(
    context: &FfiAnimationContext,
    input: &AnimationValueInput,
    underlying_override: Option<&StyleValueData>,
) -> AnimatedProperty {
    let keyframes = input.keyframes;
    let plan = |index: usize| &input.keyframe_plans[keyframes[index].keyframe];
    let (start_index, end_index) = keyframe_interval(keyframes.len(), input.current_key, |index| plan(index).key);
    let start_keyframe = &keyframes[start_index];
    let end_keyframe = &keyframes[end_index];
    let (start_plan, end_plan) = (plan(start_index), plan(end_index));

    // 13. If there is only one keyframe in interval endpoints return the property value of target property on that
    //     keyframe.
    // NB: Interpolating from that keyframe to itself returns its value, as step 12 composites it.
    let progress = if start_index == end_index {
        0.0
    } else {
        let interval_progress = (input.current_key - start_plan.key as f64) / (end_plan.key - start_plan.key) as f64;
        start_plan.easing.evaluate_at(interval_progress, false) as f32
    };
    let Some(end) = end_keyframe.value.optional_data() else {
        if start_keyframe.value.optional_data().is_none() {
            return AnimatedProperty {
                value: None,
                #[cfg(test)]
                start_index,
                #[cfg(test)]
                end_index,
                apply: false,
            };
        }
        return AnimatedProperty {
            value: Some(start_keyframe.value.clone()),
            #[cfg(test)]
            start_index,
            #[cfg(test)]
            end_index,
            apply: true,
        };
    };
    let underlying = underlying_override
        .or(input.underlying)
        .expect("an animated property must have an underlying value");
    let start = start_keyframe
        .value
        .optional_data()
        .or(input.initial)
        .expect("an animated property must have an initial value");
    let start = composite_batch_value(context, input.property_id, underlying, start, start_plan.composite);
    let end = composite_batch_value(context, input.property_id, underlying, end, end_plan.composite);
    let result = interpolate_value(Some(context), input.property_id, start.data(), end.data(), progress);
    assert!(result.handled);
    AnimatedProperty {
        value: result.value,
        #[cfg(test)]
        start_index,
        #[cfg(test)]
        end_index,
        apply: true,
    }
}

/// One effect an element samples: its description, and how far along it is on the scale the host
/// keys keyframes by.
pub(crate) struct SelectedEffect<'a> {
    pub(crate) effect: &'a crate::css::style::effect_descriptions::PublishedEffect,
    pub(crate) current_key: f64,
}

/// The custom-property stores the keyframes of an element being sampled read.
#[derive(Clone, Copy)]
pub(crate) struct KeyframeStores {
    /// The store the element holds now, which a keyframe's substitution reads, as the host's
    /// resolver read it.
    pub(crate) substitution: *const std::ffi::c_void,
    /// The same beneath this sample's animations, which an animation composes over.
    pub(crate) base: *const std::ffi::c_void,
    /// The store of the element the sampled one inherits from, which a keyframe saying `inherit`
    /// takes from.
    pub(crate) inheritance: *const std::ffi::c_void,
    /// Whether the element declares custom properties of its own, which may be important.
    pub(crate) element_declares_custom_properties: bool,
}

impl KeyframeStores {
    /// # Safety
    /// `store` must be null or a live custom-property store.
    unsafe fn store<'a>(
        store: *const std::ffi::c_void,
    ) -> Option<&'a crate::css::custom_properties::CustomPropertyStore> {
        unsafe {
            store
                .cast::<crate::css::custom_properties::CustomPropertyStore>()
                .as_ref()
        }
    }
}

/// The custom properties the effects an element samples declare, each minted into the number the
/// animation core keys it by, with what the element's environment and the document's registry say
/// of it.
pub(crate) struct AnimatedCustomProperties {
    stores: KeyframeStores,
    names: Vec<crate::css::retained_fly_string::RetainedUtf16FlyString>,
    units: Vec<Vec<u16>>,
}

type Registry = crate::css::custom_properties::CustomPropertyRegistry;

impl AnimatedCustomProperties {
    /// The number a name animates under, minting one where it is new.
    fn mint(&mut self, name: &crate::css::retained_fly_string::RetainedUtf16FlyString) -> u32 {
        if let Some(index) = self.names.iter().position(|minted| minted == name) {
            return index as u32 + 1;
        }
        // SAFETY: the name owns one reference to a live fly string.
        self.units
            .push(match unsafe { ak::utf16_string_units(name.raw_word()) } {
                ak::Utf16StringUnits::Ascii(bytes) => bytes.iter().map(|&unit| u16::from(unit)).collect(),
                ak::Utf16StringUnits::Utf16(units) => units.to_vec(),
            });
        self.names.push(name.clone());
        self.names.len() as u32
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.names.is_empty()
    }

    pub(crate) fn name(&self, name_id: u32) -> &crate::css::retained_fly_string::RetainedUtf16FlyString {
        &self.names[name_id as usize - 1]
    }

    fn units(&self, name_id: u32) -> &[u16] {
        &self.units[name_id as usize - 1]
    }

    /// Whether the element declares the name `!important` itself, which keeps an animation from
    /// animating it.
    fn is_important(&self, name_id: u32) -> bool {
        let name_raw = self.name(name_id).raw();
        self.stores.element_declares_custom_properties
            && unsafe { KeyframeStores::store(self.stores.base) }.is_some_and(|store| {
                store.declared_names.contains(&name_raw)
                    && store.own_values.get(&name_raw).is_some_and(|entry| entry.important)
            })
    }

    fn initial_value(&self, registry: &Registry, name_id: u32) -> RetainedStyleValueData {
        registry.initial_value(self.units(name_id))
    }

    /// The value an animation of a name composes over: the element's own beneath this sample's
    /// animations, and otherwise the initial value.
    fn underlying_value(&self, registry: &Registry, name_id: u32) -> RetainedStyleValueData {
        unsafe { KeyframeStores::store(self.stores.base) }
            .and_then(|store| store.get(self.name(name_id).raw()))
            .map_or_else(|| self.initial_value(registry, name_id), |entry| entry.value.clone())
    }

    /// What a keyframe saying `inherit` takes: the whole environment the element inherits from,
    /// which makes `inherit` on a name registered `inherits: false` take the parent's value, and
    /// otherwise the initial value.
    fn inherited_value(&self, registry: &Registry, name_id: u32) -> RetainedStyleValueData {
        unsafe { KeyframeStores::store(self.stores.inheritance) }
            .and_then(|store| store.get(self.name(name_id).raw()))
            .map_or_else(|| self.initial_value(registry, name_id), |entry| entry.value.clone())
    }

    /// The buffers the evaluation indexes by name number: what an animation of each name composes
    /// over, and what a keyframe saying `initial` takes.
    pub(crate) fn underlying_and_initial_values(
        &self,
        registry: &Registry,
    ) -> (Vec<RetainedStyleValueData>, Vec<RetainedStyleValueData>) {
        (1..=self.names.len() as u32)
            .map(|name_id| {
                (
                    self.underlying_value(registry, name_id),
                    self.initial_value(registry, name_id),
                )
            })
            .unzip()
    }

    /// The computed value of one resolved custom-property declaration: the value its source names,
    /// computed against a registration's syntax in the element's computation context, which is
    /// what gives a registered name its typed interpolation, and as written otherwise.
    pub(crate) fn computed_value(
        &self,
        registry: &Registry,
        property: &ResolvedAnimationProperty,
        finalization: &crate::css::custom_properties::CustomPropertyFinalization<'_>,
    ) -> RetainedStyleValueData {
        let name_id = property.custom_name_id;
        let value = match property.value_source {
            FfiAnimationSpecifiedValueSource::Inherited => self.inherited_value(registry, name_id),
            FfiAnimationSpecifiedValueSource::Initial => self.initial_value(registry, name_id),
            FfiAnimationSpecifiedValueSource::Underlying => self.underlying_value(registry, name_id),
            FfiAnimationSpecifiedValueSource::Value => property.value.clone(),
        };
        if !registry.name_has_syntax(self.units(name_id)) {
            return value;
        }
        registry
            .finalize_custom_property_value(
                unsafe { KeyframeStores::store(self.stores.inheritance) },
                self.name(name_id).raw(),
                self.units(name_id),
                value,
                finalization,
            )
            .map_or_else(|fallback| fallback, |(value, _)| value)
    }
}

/// The declarations of the effects an element samples, resolved, and what resolving them read.
pub(crate) struct PublishedAnimationDeclarations {
    pub(crate) resolved: Box<ResolvedAnimationDeclarations>,
    pub(crate) custom_properties: AnimatedCustomProperties,
    /// The `SUBSTITUTION_MARK_*` bits of what the values and easings substituted against the
    /// element held.
    pub(crate) substitution_marks: u8,
    /// The custom properties a substituted `style()` query read.
    pub(crate) style_query_dependencies: Option<Box<crate::css::custom_properties::StyleQueryDependencies>>,
}

/// Substitute a keyframe value written as a token stream against an element, for the compositor,
/// which offloads an animation only where the value is what a sample on the main thread would
/// substitute it to: against the custom-property store the element holds now and the one it
/// inherits from. Returns a retained value, guaranteed-invalid where the value does not
/// substitute, or null where the engine knows no such element.
///
/// # Safety
/// `host` must be a live document host, on its document's thread, each store must be null or live,
/// and `value` must be a live style value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_substitute_compositor_keyframe_value(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    style_node: u32,
    pseudo_kind: u8,
    custom_property_store: *const std::ffi::c_void,
    inheritance_custom_property_store: *const std::ffi::c_void,
    property_id: u16,
    value: *const StyleValueData,
) -> *const StyleValueData {
    // SAFETY: Guaranteed by the caller.
    crate::css::style::engine_calls::with_engine(read, host, |engine| {
        let Some(node) = crate::css::style::tree::StyleNodeID::from_raw(style_node) else {
            return std::ptr::null();
        };
        let pseudo = (pseudo_kind != crate::css::cascaded_properties::NO_PSEUDO_ELEMENT).then_some(pseudo_kind);
        let written = unsafe {
            RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(value))
        };
        // What a `style()` query reads is recorded when the main thread samples the same value.
        let style_query_references = std::cell::RefCell::new(None);
        let substituted = engine.substitute_keyframe_value(
            node,
            pseudo,
            custom_property_store,
            inheritance_custom_property_store,
            property_id,
            &[],
            &written,
            &style_query_references,
        );
        std::sync::Arc::into_raw(substituted.into_arc())
    })
}

#[derive(Clone, Copy)]
struct CustomPropertyKeyframeLookup {
    keyframe_index: usize,
    declaration_index: usize,
}

/// Build a sequence of lookups for each custom property, in keyframe order.
fn build_custom_property_keyframe_lookups(
    effect: &PublishedEffect,
    custom_properties: &mut AnimatedCustomProperties,
) -> BTreeMap<u32, Vec<CustomPropertyKeyframeLookup>> {
    let mut property_lookups = BTreeMap::<_, Vec<_>>::new();
    let mut declaration_index = 0;
    for (keyframe_index, keyframe) in effect.keyframes.iter().enumerate() {
        for declaration in effect.custom_declarations_of(keyframe) {
            let name_id = custom_properties.mint(&declaration.name);

            property_lookups
                .entry(name_id)
                .or_default()
                .push(CustomPropertyKeyframeLookup {
                    keyframe_index,
                    declaration_index,
                });

            declaration_index += 1;
        }
    }
    property_lookups
}

fn select_custom_property_interval_span(
    effect: &PublishedEffect,
    keyframe_lookups: &[CustomPropertyKeyframeLookup],
    current_key: f64,
) -> [CustomPropertyKeyframeLookup; 2] {
    let first_lookup = keyframe_lookups[0];
    let first_composite = effect.keyframes[first_lookup.keyframe_index].composite;
    let first_value = &effect.custom_declaration_at(first_lookup.declaration_index).value;

    let can_use_constant_interpolation = !matches!(first_value, PublishedValue::ElementValue)
        && keyframe_lookups.iter().all(|lookup| {
            let composite = effect.keyframes[lookup.keyframe_index].composite;
            let value = &effect.custom_declaration_at(lookup.declaration_index).value;
            composite == first_composite && value == first_value
        });

    // When declared values and composite operations are equal, use the same endpoints
    // across all intervals so discrete properties reuse their retained values.
    let (start, end) = if can_use_constant_interpolation {
        (0, keyframe_lookups.len() - 1)
    } else {
        keyframe_interval(keyframe_lookups.len(), current_key, |index| {
            effect.keyframes[keyframe_lookups[index].keyframe_index].key
        })
    };

    [keyframe_lookups[start], keyframe_lookups[end]]
}

/// Resolve the declarations of the effects an element samples from their descriptions: each
/// keyframe's easing and each value written as a token stream substituted against the element, a
/// keyframe holding the element's own value given it, and every custom property minted into a
/// number.
#[allow(clippy::too_many_arguments)]
pub(crate) fn resolve_published_animation_declarations(
    engine: &crate::css::style::StyleEngine,
    node: crate::css::style::tree::StyleNodeID,
    pseudo: Option<u8>,
    selected: &[SelectedEffect<'_>],
    table: &crate::css::computed_longhand_table::ComputedLonghandTable,
    stores: KeyframeStores,
    writing_mode: u8,
    direction: u8,
    important_property_bitmap: &[u8],
) -> PublishedAnimationDeclarations {
    use crate::css::style::effect_descriptions::{PublishedValue, easing_from_computed_timing_function};
    let registry = engine.custom_property_registry();
    let mut custom_properties = AnimatedCustomProperties {
        stores,
        names: Vec::new(),
        units: Vec::new(),
    };
    let substitution_marks = std::cell::Cell::new(0_u8);
    let style_query_references = std::cell::RefCell::new(None);
    let substitute = |property, root_name: &[u16], written: &RetainedStyleValueData| {
        substitution_marks.set(substitution_marks.get() | substitution_marks_of(written.data()));
        engine.substitute_keyframe_value(
            node,
            pseudo,
            stores.substitution,
            stores.inheritance,
            property,
            root_name,
            written,
            &style_query_references,
        )
    };
    let mut keyframes = Vec::new();
    let mut easings = Vec::new();
    let mut effects = Vec::new();
    let mut declarations = Vec::new();
    let mut base_urls = Vec::new();
    for selection in selected {
        let effect = selection.effect;
        let is_transition = effect.is_transition;
        let style_sheet_resource_context = match &effect.resource_context {
            None => FfiAnimationStyleSheetResourceContext {
                base_url: std::ptr::null(),
                base_url_length: 0,
                has_value: false,
                origin_clean: false,
            },
            Some(resource_context) => {
                base_urls.push(std::sync::Arc::clone(&resource_context.base_url));
                FfiAnimationStyleSheetResourceContext {
                    base_url: resource_context.base_url.as_ptr(),
                    base_url_length: resource_context.base_url.len(),
                    has_value: true,
                    origin_clean: resource_context.origin_clean,
                }
            }
        };
        let first_keyframe_index = keyframes.len();
        for keyframe in &effect.keyframes {
            let keyframe_index = keyframes.len();
            keyframes.push((keyframe.key, keyframe.composite));
            // A keyframe's own easing written as a value substitutes against the element; one that
            // resolves to no easing runs its animation's.
            let substituted_easing = keyframe.easing_value.as_ref().and_then(|written| {
                let substituted;
                let mut value = written.data();
                if matches!(value, StyleValueData::Unresolved { .. }) {
                    substituted = substitute(
                        crate::css::property_metadata::property_id::ANIMATION_TIMING_FUNCTION,
                        &[],
                        written,
                    );
                    value = substituted.data();
                }
                if let StyleValueData::ValueList { values, .. } = value {
                    value = values.as_slice().first()?.data();
                }
                easing_from_computed_timing_function(value)
            });
            easings.push(substituted_easing.unwrap_or_else(|| keyframe.easing.clone()));
            let keyframe_declarations = effect.declarations_of(keyframe);
            // The values of the shorthands written with arbitrary substitution functions that this keyframe's
            // longhands pend the substitution of, each already standing for its longhands.
            let mut substituted_shorthand_values: Vec<*const StyleValueData> = Vec::new();
            for declaration in keyframe_declarations {
                let (value, use_initial) = match &declaration.value {
                    PublishedValue::ElementValue => {
                        let Some(value) = table.get(declaration.property_id) else {
                            continue;
                        };
                        (value.clone_retained(), true)
                    }
                    PublishedValue::Declared(value) => match value.data() {
                        // https://drafts.csswg.org/css-values-5/#pending-substitution-value
                        // The longhands of a shorthand written with an arbitrary substitution function take their
                        // parts of its value once it substitutes, so the shorthand stands for them.
                        StyleValueData::PendingSubstitution { .. } => {
                            // NB: The longhands of a shorthand nested in the one written (`background-position` in
                            //     `background`) pend the nested shorthand's pending value in turn.
                            fn written_shorthand_value(value: &RetainedStyleValueData) -> &RetainedStyleValueData {
                                match value.data() {
                                    StyleValueData::PendingSubstitution {
                                        original_shorthand_value,
                                    } => written_shorthand_value(original_shorthand_value),
                                    _ => value,
                                }
                            }
                            let original_shorthand_value = written_shorthand_value(value);
                            let original = original_shorthand_value.pointer();
                            if substituted_shorthand_values.contains(&original) {
                                continue;
                            }
                            substituted_shorthand_values.push(original);
                            let pending_longhands = keyframe_declarations
                                .iter()
                                .filter(|candidate| {
                                    matches!(&candidate.value, PublishedValue::Declared(written)
                                        if matches!(written.data(), StyleValueData::PendingSubstitution { .. })
                                            && written_shorthand_value(written).pointer() == original)
                                })
                                .map(|candidate| candidate.property_id)
                                .collect::<Vec<_>>();
                            // NB: A later declaration may have replaced some of the written shorthand's longhands,
                            //     so this takes the smallest shorthand with the pending longhands whose grammar
                            //     parses the substituted value (`border` rather than `border-width` for
                            //     `border: 2px solid` where only the widths still pend).
                            // https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
                            let Some((shorthand, value)) =
                                crate::css::property_metadata::shorthands_with_leaf_longhands(&pending_longhands)
                                    .into_iter()
                                    .map(|shorthand| (shorthand, substitute(shorthand, &[], original_shorthand_value)))
                                    .find(|(_, value)| !matches!(value.data(), StyleValueData::GuaranteedInvalid))
                            else {
                                continue;
                            };
                            // NB: Only the longhands that still pend the shorthand's value take their parts of it; a
                            //     later declaration in the keyframe replaced the others.
                            crate::css::style_compute::expand_shorthands_with(
                                shorthand,
                                &value,
                                false,
                                &mut |longhand, data, _| {
                                    if pending_longhands.contains(&longhand) {
                                        declarations.push(AnimationDeclaration {
                                            keyframe_index,
                                            property_id: longhand,
                                            custom_name_id: 0,
                                            custom_is_inherited: false,
                                            custom_is_important: false,
                                            value: data.clone_retained(),
                                            style_sheet_resource_context,
                                            use_initial: false,
                                            is_transition,
                                        });
                                    }
                                },
                            );
                            continue;
                        }
                        StyleValueData::Unresolved { .. } => {
                            let value = substitute(declaration.property_id, &[], value);
                            // https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
                            if matches!(value.data(), StyleValueData::GuaranteedInvalid) {
                                continue;
                            }
                            (value, false)
                        }
                        _ => (value.clone_retained(), false),
                    },
                };
                declarations.push(AnimationDeclaration {
                    keyframe_index,
                    property_id: declaration.property_id,
                    custom_name_id: 0,
                    custom_is_inherited: false,
                    custom_is_important: false,
                    value,
                    style_sheet_resource_context,
                    use_initial,
                    is_transition,
                });
            }
        }
        for (name_id, keyframe_lookups) in build_custom_property_keyframe_lookups(effect, &mut custom_properties) {
            let important = custom_properties.is_important(name_id);
            if keyframe_lookups.len() < 2 || important && !is_transition {
                continue;
            }
            let name = custom_properties.units(name_id);
            let interval_span = select_custom_property_interval_span(effect, &keyframe_lookups, selection.current_key);
            let interval_values = effect.resolve_custom_property_interval_values(
                interval_span.map(|lookup| lookup.declaration_index),
                registry.name_has_interpolable_syntax(name),
                || {
                    interval_span.map(|lookup| {
                        let declaration = effect.custom_declaration_at(lookup.declaration_index);
                        match &declaration.value {
                            PublishedValue::ElementValue => custom_properties.underlying_value(registry, name_id),
                            // A custom property's value is a token stream; only the substitution functions
                            // in it resolve against the element.
                            PublishedValue::Declared(value) if holds_arbitrary_substitution(value.data()) => {
                                substitute(crate::css::property_metadata::property_id::CUSTOM, name, value)
                            }
                            PublishedValue::Declared(value) => value.clone_retained(),
                        }
                    })
                },
            );
            for (lookup, value) in interval_span.into_iter().zip(interval_values) {
                let declaration = effect.custom_declaration_at(lookup.declaration_index);
                declarations.push(AnimationDeclaration {
                    keyframe_index: first_keyframe_index + lookup.keyframe_index,
                    property_id: crate::css::property_metadata::property_id::CUSTOM,
                    custom_name_id: name_id,
                    custom_is_inherited: registry.name_inherits(name),
                    custom_is_important: important,
                    value,
                    style_sheet_resource_context,
                    use_initial: matches!(declaration.value, PublishedValue::ElementValue),
                    is_transition,
                });
            }
        }
        effects.push(FfiAnimationEffect {
            first_keyframe_index,
            keyframe_count: keyframes.len() - first_keyframe_index,
            current_key: selection.current_key,
            result_of_transition: is_transition,
        });
    }
    let keyframes = keyframes
        .into_iter()
        .zip(easings)
        .map(|((key, composite), easing)| AnimationKeyframePlan { key, easing, composite })
        .collect();
    let mut resolved = resolve_animation_declarations(
        &declarations,
        &effects,
        keyframes,
        writing_mode,
        direction,
        important_property_bitmap,
    );
    let style_query_dependencies = style_query_references.into_inner();
    resolved.uses_tree_counting_function |= style_query_dependencies
        .as_ref()
        .is_some_and(|dependencies| dependencies.sibling_position);
    resolved._base_urls = base_urls;
    PublishedAnimationDeclarations {
        resolved: Box::new(resolved),
        custom_properties,
        substitution_marks: substitution_marks.get(),
        style_query_dependencies,
    }
}

/// Whether a token stream holds an arbitrary-substitution function, which resolves against the
/// element: the host's `UnresolvedStyleValue::contains_arbitrary_substitution_function`.
fn holds_arbitrary_substitution(value: &StyleValueData) -> bool {
    matches!(
        value,
        StyleValueData::Unresolved {
            presence_attr,
            presence_dashed_function,
            presence_env,
            presence_if,
            presence_inherit,
            presence_var,
            ..
        } if *presence_attr || *presence_dashed_function || *presence_env || *presence_if || *presence_inherit || *presence_var
    )
}

struct AnimationPreparationKey {
    effects: Box<[FfiAnimationPreparationEffect]>,
    /// The custom-property environments a preparation that substituted keyframe values against the
    /// element was made under, which it holds for only as long as they stand.
    custom_property_environments: Option<[u64; 2]>,
}

impl AnimationPreparationKey {
    fn matches(&self, sampled: &[FfiSampledAnimationEffect], custom_property_environments: [u64; 2]) -> bool {
        self.effects.len() == sampled.len()
            && self
                .effects
                .iter()
                .zip(sampled)
                .all(|(effect, sampled)| *effect == sampled.effect)
            && self
                .custom_property_environments
                .is_none_or(|environments| environments == custom_property_environments)
    }
}

pub(crate) struct PreparedAnimationBatch {
    key: AnimationPreparationKey,
    resolved: ResolvedAnimationDeclarations,
    keyframes_by_value: Vec<Vec<AnimationKeyframeValue>>,
    /// The context the preparation was made in, without the current color, which each sample
    /// reads from its own working set.
    context: FfiAnimationContext<'static>,
}

// SAFETY: A prepared batch is built once and never mutated: sampling reads its plans and copies
// its context. The raw pointers its resolved declarations hold name the base URLs and random
// sharing values those declarations retain for the batch's lifetime. An animated overlay caches
// one behind an `Arc`, and an installed overlay is reachable from the computed catalog that every
// evaluation worker borrows; nothing but the animation round, which runs on the engine's thread
// between evaluation passes, ever constructs, replaces or reads one.
unsafe impl Send for PreparedAnimationBatch {}
unsafe impl Sync for PreparedAnimationBatch {}

/// Whether an animated overlay already holds the endpoint preparation for the `sampled` effects
/// under the custom-property environments a sample reads.
pub(crate) fn animation_preparation_matches(
    overlay: Option<&crate::css::animated_overlay::AnimatedOverlay>,
    sampled: &[FfiSampledAnimationEffect],
    custom_property_environments: [u64; 2],
) -> bool {
    overlay
        .and_then(|overlay| overlay.animation_preparation.as_ref())
        .is_some_and(|preparation| preparation.key.matches(sampled, custom_property_environments))
}

/// Complete the Rust-owned animation plan and compose every interval without
/// consulting C++ or the DOM. On the first sample this consumes the declarations and keyframe
/// values the batch carries. Later samples can reuse that preparation from the animated overlay.
/// Answers the final value of each custom property the effects animate, by the number it animates
/// under, in that order.
pub(crate) fn evaluate_animations(batch: ComputedAnimationBatch<'_>) -> Vec<(u32, RetainedStyleValueData)> {
    crate::css::ffi_stats::note_animation_evaluation();
    let ComputedAnimationBatch {
        context: sample_context,
        sampled_effects: sampled,
        custom_property_environments,
        cache_preparation,
        preparation_reads_custom_property_environments,
        preparation_inputs,
        underlying_longhand_table,
        overlay,
        custom_underlying_values,
        custom_initial_values,
    } = batch;
    let preparation = overlay
        .animation_preparation
        .as_ref()
        .filter(|preparation| preparation.key.matches(sampled, custom_property_environments))
        .cloned()
        .unwrap_or_else(|| {
            let AnimationPreparationInputs {
                resolved,
                computed_keyframe_values,
            } = preparation_inputs.expect("a sample without a matching preparation carries its inputs");
            assert_eq!(computed_keyframe_values.len(), resolved.properties.len());
            let keyframes_by_value = resolved
                .value_plans
                .iter()
                .map(|plan| {
                    plan.property_indices
                        .iter()
                        .map(|&property_index| AnimationKeyframeValue {
                            keyframe: resolved.properties[property_index].keyframe_index,
                            value: computed_keyframe_values[property_index].clone(),
                        })
                        .collect()
                })
                .collect();
            std::sync::Arc::new(PreparedAnimationBatch {
                key: AnimationPreparationKey {
                    effects: sampled.iter().map(|sampled| sampled.effect).collect(),
                    custom_property_environments: preparation_reads_custom_property_environments
                        .then_some(custom_property_environments),
                },
                resolved: *resolved,
                keyframes_by_value,
                context: FfiAnimationContext {
                    allow_discrete: sample_context.allow_discrete,
                    allow_keyword_size_interpolation: sample_context.allow_keyword_size_interpolation,
                    current_color: None,
                    has_length_resolution_context: sample_context.has_length_resolution_context,
                    length_resolution_context: sample_context.length_resolution_context,
                    has_transform_reference_box: sample_context.has_transform_reference_box,
                    transform_reference_box_width: sample_context.transform_reference_box_width,
                    transform_reference_box_height: sample_context.transform_reference_box_height,
                },
            })
        });
    if cache_preparation {
        overlay.animation_preparation = Some(preparation.clone());
    } else {
        overlay.animation_preparation = None;
    }
    let resolved = &preparation.resolved;
    let context = FfiAnimationContext {
        current_color: sample_context.current_color,
        has_transform_reference_box: sample_context.has_transform_reference_box,
        transform_reference_box_width: sample_context.transform_reference_box_width,
        transform_reference_box_height: sample_context.transform_reference_box_height,
        ..preparation.context
    };

    // https://www.w3.org/TR/web-animations-1/#effect-stacks
    // NB: Inputs arrive in composite order. Keep each result as the underlying value for the
    //     next effect affecting the same property.
    let mut custom_final_values = std::collections::BTreeMap::<u32, RetainedStyleValueData>::new();
    let mut previous_values = Vec::<(u16, u32, RetainedStyleValueData)>::with_capacity(resolved.value_plans.len());
    for (plan, keyframes) in resolved.value_plans.iter().zip(&preparation.keyframes_by_value) {
        assert!(plan.effect_index < sampled.len());
        let (underlying, initial) = if plan.custom_name_id != 0 {
            let custom_index = (plan.custom_name_id - 1) as usize;
            (
                custom_underlying_values[custom_index].optional_data(),
                custom_initial_values[custom_index].optional_data(),
            )
        } else {
            let underlying = underlying_longhand_table
                .get(plan.property_id)
                .expect("an animated longhand must have an underlying computed value");
            (
                underlying.optional_data(),
                crate::css::style_compute::initial_value(plan.property_id),
            )
        };
        let input = AnimationValueInput {
            property_id: plan.property_id,
            custom_name_id: plan.custom_name_id,
            result_of_transition: plan.result_of_transition,
            underlying,
            initial,
            current_key: sampled[plan.effect_index].current_key,
            keyframes,
            keyframe_plans: &resolved.keyframes,
        };
        let previous_value = previous_values
            .iter()
            .rev()
            .find(|(property_id, custom_name_id, _)| {
                *property_id == input.property_id && *custom_name_id == input.custom_name_id
            })
            .and_then(|(_, _, value)| value.optional_data());
        let result = evaluate_animation_value(&context, &input, previous_value);
        if !result.apply {
            assert!(result.value.is_none());
            continue;
        }
        if input.custom_name_id != 0 {
            if let Some(value) = result.value {
                previous_values.push((input.property_id, input.custom_name_id, value.clone()));
                custom_final_values.insert(input.custom_name_id, value);
            }
            continue;
        }
        if let Some(value) = result.value {
            previous_values.push((input.property_id, input.custom_name_id, value.clone()));
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
    custom_final_values.into_iter().collect()
}

/// Attempt Rust-owned style value interpolation without consulting C++ or the DOM.
#[unsafe(no_mangle)]
pub extern "C" fn rust_interpolate_scalar_style_value(
    context: Option<&FfiAnimationContext>,
    property_id: u16,
    from: &StyleValueData,
    to: &StyleValueData,
    delta: f32,
) -> FfiAnimationValueResult {
    interpolate_value(context, property_id, from, to, delta).into_ffi()
}

/// Test-only bridge for exercising Rust-owned style value composition without constructing an
/// animation batch. Production animation evaluation uses `evaluate_animations`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_test_composite_style_value(
    underlying: &StyleValueData,
    animated: &StyleValueData,
    operation: FfiCompositeOperation,
) -> FfiAnimationValueResult {
    composite_scalar_value(underlying, animated, operation).into_ffi()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::easing::FfiEasingKind;

    #[test]
    fn resolves_keyframe_property_conflicts_as_one_batch() {
        use crate::css::property_metadata::property_id;
        let value = || RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number { value: 1.0 }));
        let candidates = [
            AnimationPropertyConflictCandidate {
                custom_name_id: 0,
                custom_is_inherited: false,
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
                custom_name_id: 0,
                custom_is_inherited: false,
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
                custom_name_id: 0,
                custom_is_inherited: false,
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
                custom_name_id: 0,
                custom_is_inherited: false,
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
                custom_name_id: 0,
                custom_is_inherited: false,
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
            animation_specified_value_source(
                &keyword_value(keyword::INHERIT),
                crate::css::property_metadata::property_is_inherited(property_id::MARGIN_LEFT)
            ),
            FfiAnimationSpecifiedValueSource::Inherited
        );
        assert_eq!(
            animation_specified_value_source(
                &keyword_value(keyword::UNSET),
                crate::css::property_metadata::property_is_inherited(property_id::COLOR)
            ),
            FfiAnimationSpecifiedValueSource::Inherited
        );
        assert_eq!(
            animation_specified_value_source(
                &keyword_value(keyword::UNSET),
                crate::css::property_metadata::property_is_inherited(property_id::MARGIN_LEFT)
            ),
            FfiAnimationSpecifiedValueSource::Initial
        );
        assert_eq!(
            animation_specified_value_source(
                &keyword_value(keyword::INITIAL),
                crate::css::property_metadata::property_is_inherited(property_id::COLOR)
            ),
            FfiAnimationSpecifiedValueSource::Initial
        );
        assert_eq!(
            animation_specified_value_source(
                &keyword_value(keyword::REVERT),
                crate::css::property_metadata::property_is_inherited(property_id::COLOR)
            ),
            FfiAnimationSpecifiedValueSource::Underlying
        );
        assert_eq!(
            animation_specified_value_source(
                &keyword_value(keyword::REVERT_LAYER),
                crate::css::property_metadata::property_is_inherited(property_id::COLOR)
            ),
            FfiAnimationSpecifiedValueSource::Underlying
        );
    }

    #[test]
    fn builds_property_specific_animation_plans() {
        use crate::css::property_metadata::property_id;

        let values = [
            Arc::new(StyleValueData::Number { value: 0.0 }),
            Arc::new(StyleValueData::Number { value: 0.5 }),
            Arc::new(StyleValueData::Number { value: 1.0 }),
        ];
        let declarations = [
            AnimationDeclaration {
                custom_name_id: 0,
                custom_is_inherited: false,
                custom_is_important: false,
                keyframe_index: 0,
                property_id: property_id::OPACITY,
                value: RetainedStyleValueData::from_arc(values[0].clone()),
                style_sheet_resource_context: FfiAnimationStyleSheetResourceContext::empty(),
                use_initial: false,
                is_transition: false,
            },
            AnimationDeclaration {
                custom_name_id: 0,
                custom_is_inherited: false,
                custom_is_important: false,
                keyframe_index: 1,
                property_id: property_id::COLOR,
                value: RetainedStyleValueData::from_arc(values[1].clone()),
                style_sheet_resource_context: FfiAnimationStyleSheetResourceContext::empty(),
                use_initial: false,
                is_transition: false,
            },
            AnimationDeclaration {
                custom_name_id: 0,
                custom_is_inherited: false,
                custom_is_important: false,
                keyframe_index: 2,
                property_id: property_id::OPACITY,
                value: RetainedStyleValueData::from_arc(values[2].clone()),
                style_sheet_resource_context: FfiAnimationStyleSheetResourceContext::empty(),
                use_initial: false,
                is_transition: false,
            },
        ];
        let keyframes = [0, 50, 100]
            .map(|key| AnimationKeyframePlan {
                key,
                easing: Easing::default(),
                composite: FfiCompositeOperation::Replace,
            })
            .into();
        let effects = [FfiAnimationEffect {
            first_keyframe_index: 0,
            keyframe_count: 3,
            current_key: 50.0,
            result_of_transition: false,
        }];

        let resolved = resolve_animation_declarations(&declarations, &effects, keyframes, 0, 0, &[]);
        assert_eq!(resolved.value_plans.len(), 1);
        let plan = &resolved.value_plans[0];
        assert_eq!(plan.property_id, property_id::OPACITY);
        assert_eq!(plan.property_indices.len(), 2);
        assert_eq!(resolved.properties[plan.property_indices[0]].keyframe_index, 0);
        assert_eq!(resolved.properties[plan.property_indices[1]].keyframe_index, 2);
    }

    #[test]
    fn retains_synthesized_pending_substitution_values() {
        let original = RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number { value: 1.0 }));
        let pending = Arc::new(StyleValueData::PendingSubstitution {
            original_shorthand_value: original,
        });
        let declaration = AnimationDeclaration {
            keyframe_index: 0,
            property_id: crate::css::property_metadata::property_id::BORDER,
            custom_name_id: 0,
            custom_is_inherited: false,
            custom_is_important: false,
            value: RetainedStyleValueData::from_arc(pending),
            style_sheet_resource_context: FfiAnimationStyleSheetResourceContext::empty(),
            use_initial: false,
            is_transition: false,
        };
        let resolved = resolve_animation_declarations(&[declaration], &[], Vec::new(), 0, 0, &[]);
        assert!(!resolved.properties.is_empty());
        let mut found_synthesized = false;
        for property in &resolved.properties {
            let value = property.value.data();
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

    fn animation_context(allow_discrete: bool) -> FfiAnimationContext<'static> {
        let font_metrics = || FfiAnimationFontMetrics {
            font_size: 0.0,
            x_height: 0.0,
            cap_height: 0.0,
            zero_advance: 0.0,
            line_height: 0.0,
        };
        FfiAnimationContext {
            allow_discrete,
            allow_keyword_size_interpolation: false,
            current_color: None,
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
    fn constructs_easing_values_without_child_wrappers() {
        let points = [
            FfiLinearEasingPoint {
                input: 0.0,
                output: 0.0,
            },
            FfiLinearEasingPoint {
                input: 1.0,
                output: 1.0,
            },
        ];
        for (kind, expected) in [
            (FfiEasingKind::Linear, "linear(0 0%, 1 100%)"),
            (FfiEasingKind::CubicBezier, "cubic-bezier(0, 0, 1, 1)"),
            (FfiEasingKind::Steps, "steps(4)"),
        ] {
            let descriptor = FfiEasingDescriptor {
                kind,
                linear_points: points.as_ptr(),
                linear_point_count: points.len(),
                x1: 0.0,
                y1: 0.0,
                x2: 1.0,
                y2: 1.0,
                interval_count: 4,
                step_position: 1,
            };
            let value = unsafe { Arc::from_raw(style_value_from_easing(&descriptor)) };
            assert_eq!(
                crate::css::serialize::serialize_style_value_to_utf16(&value),
                expected.encode_utf16().collect::<Vec<_>>()
            );
        }
    }

    #[test]
    fn evaluates_composited_animation_value() {
        let underlying = StyleValueData::Number { value: 2.0 };
        let number = |value| RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number { value }));
        let linear_easing = || Easing::CubicBezier {
            x1: 0.0,
            y1: 0.0,
            x2: 1.0,
            y2: 1.0,
        };
        let plans = [0, 50, 100].map(|key| AnimationKeyframePlan {
            key,
            easing: linear_easing(),
            composite: FfiCompositeOperation::Add,
        });
        let keyframes = [3.0, 5.0, 9.0]
            .into_iter()
            .enumerate()
            .map(|(keyframe, value)| AnimationKeyframeValue {
                keyframe,
                value: number(value),
            })
            .collect::<Vec<_>>();
        let input = AnimationValueInput {
            property_id: crate::css::property_metadata::property_id::FLEX_GROW,
            custom_name_id: 0,
            result_of_transition: false,
            underlying: Some(&underlying),
            initial: Some(&underlying),
            current_key: 75.0,
            keyframes: &keyframes,
            keyframe_plans: &plans,
        };
        let result = evaluate_animation_value(&animation_context(true), &input, None);
        assert!(result.apply);
        assert!(result.value.is_some());
        assert_eq!(result.start_index, 1);
        assert_eq!(result.end_index, 2);
        let value = result.value.unwrap().into_arc();
        assert!(matches!(&*value, StyleValueData::Number { value } if *value == 9.0));
    }

    #[test]
    fn evaluates_overlapping_keyframes_at_the_ends() {
        let underlying = StyleValueData::Number { value: 0.0 };
        let plans = [0, 0, 100, 100].map(|key| AnimationKeyframePlan {
            key,
            easing: Easing::default(),
            composite: FfiCompositeOperation::Replace,
        });
        let keyframes = [1.0, 2.0, 3.0, 4.0]
            .into_iter()
            .enumerate()
            .map(|(keyframe, value)| AnimationKeyframeValue {
                keyframe,
                value: RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number { value })),
            })
            .collect::<Vec<_>>();
        let evaluate = |current_key| {
            let input = AnimationValueInput {
                property_id: crate::css::property_metadata::property_id::FLEX_GROW,
                custom_name_id: 0,
                result_of_transition: false,
                underlying: Some(&underlying),
                initial: Some(&underlying),
                current_key,
                keyframes: &keyframes,
                keyframe_plans: &plans,
            };
            let result = evaluate_animation_value(&animation_context(true), &input, None);
            assert!(result.apply);
            match &*result.value.unwrap().into_arc() {
                StyleValueData::Number { value } => *value,
                _ => panic!("expected a number"),
            }
        };

        assert_eq!(evaluate(-10.0), 1.0);
        assert_eq!(evaluate(0.0), 2.0);
        assert_eq!(evaluate(50.0), 2.5);
        assert_eq!(evaluate(100.0), 4.0);
        assert_eq!(evaluate(110.0), 4.0);
    }

    #[test]
    fn evaluates_missing_keyframe_values() {
        let underlying = StyleValueData::Number { value: 2.0 };
        let initial = StyleValueData::Number { value: 1.0 };
        let start = || RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number { value: 3.0 }));
        let end = || RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Number { value: 5.0 }));
        let easing = || Easing::CubicBezier {
            x1: 0.0,
            y1: 0.0,
            x2: 1.0,
            y2: 1.0,
        };
        let plans = [0, 100].map(|key| AnimationKeyframePlan {
            key,
            easing: easing(),
            composite: FfiCompositeOperation::Replace,
        });
        let evaluate = |start_value, end_value| {
            let keyframes = [
                AnimationKeyframeValue {
                    keyframe: 0,
                    value: start_value,
                },
                AnimationKeyframeValue {
                    keyframe: 1,
                    value: end_value,
                },
            ];
            let input = AnimationValueInput {
                property_id: crate::css::property_metadata::property_id::FLEX_GROW,
                custom_name_id: 0,
                result_of_transition: false,
                underlying: Some(&underlying),
                initial: Some(&initial),
                current_key: 50.0,
                keyframes: &keyframes,
                keyframe_plans: &plans,
            };
            evaluate_animation_value(&animation_context(true), &input, None)
        };

        let result = evaluate(RetainedStyleValueData::none(), end());
        assert!(result.apply);
        let value = result.value.unwrap().into_arc();
        assert!(matches!(&*value, StyleValueData::Number { value } if *value == 3.0));

        let result = evaluate(start(), RetainedStyleValueData::none());
        assert!(result.apply);
        let value = result.value.unwrap().into_arc();
        assert!(matches!(&*value, StyleValueData::Number { value } if *value == 3.0));

        let result = evaluate(RetainedStyleValueData::none(), RetainedStyleValueData::none());
        assert!(!result.apply);
        assert!(result.value.is_none());
    }

    #[test]
    fn evaluates_discrete_animation_values() {
        let from = Arc::new(StyleValueData::Keyword { keyword: 1 });
        let to = Arc::new(StyleValueData::Keyword { keyword: 2 });
        let property_id = crate::css::property_metadata::property_id::ALIGN_CONTENT;

        let result = interpolate_value(Some(&animation_context(true)), property_id, &from, &to, 0.25);
        assert!(result.handled);
        let value = result.value.unwrap().into_arc();
        assert!(matches!(&*value, StyleValueData::Keyword { keyword: 1 }));

        let result = interpolate_value(Some(&animation_context(true)), property_id, &from, &to, 0.75);
        assert!(result.handled);
        let value = result.value.unwrap().into_arc();
        assert!(matches!(&*value, StyleValueData::Keyword { keyword: 2 }));

        let result = interpolate_value(Some(&animation_context(false)), property_id, &from, &to, 0.75);
        assert!(result.handled);
        assert!(result.value.is_none());
    }

    #[test]
    fn evaluates_incompatible_by_computed_values_as_discrete() {
        let property_id = crate::css::property_metadata::property_id::FLEX_GROW;
        let from = Arc::new(StyleValueData::Keyword { keyword: 1 });
        let to = Arc::new(StyleValueData::Keyword { keyword: 2 });

        let result = interpolate_value(Some(&animation_context(true)), property_id, &from, &to, 0.25);
        assert!(result.handled);
        assert_eq!(
            result.value.as_ref().map(RetainedStyleValueData::pointer),
            Some(Arc::as_ptr(&from))
        );

        let result = interpolate_value(Some(&animation_context(true)), property_id, &from, &to, 0.75);
        assert!(result.handled);
        assert_eq!(
            result.value.as_ref().map(RetainedStyleValueData::pointer),
            Some(Arc::as_ptr(&to))
        );

        let result = interpolate_value(Some(&animation_context(false)), property_id, &from, &to, 0.75);
        assert!(result.handled);
        assert!(result.value.is_none());
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
        let value = result.value.unwrap().into_arc();
        assert!(matches!(&*value, StyleValueData::Keyword { keyword: 2 }));
    }

    #[test]
    fn evaluates_declined_custom_animation_values_as_discrete() {
        let from = Arc::new(StyleValueData::Keyword { keyword: 1 });
        let to = Arc::new(StyleValueData::Keyword { keyword: 2 });
        let property_id = crate::css::property_metadata::property_id::SCALE;

        let result = interpolate_value(Some(&animation_context(true)), property_id, &from, &to, 0.75);
        assert!(result.handled);
        let value = result.value.unwrap().into_arc();
        assert!(matches!(&*value, StyleValueData::Keyword { keyword: 2 }));

        let result = interpolate_value(Some(&animation_context(false)), property_id, &from, &to, 0.75);
        assert!(result.handled);
        assert!(result.value.is_none());
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
        let value = result.value.unwrap().into_arc();
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
        let value = result.value.unwrap().into_arc();
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
        let value = result.value.unwrap().into_arc();
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
        let value = result.value.unwrap().into_arc();
        assert!(matches!(&*value, StyleValueData::Length { value, unit } if *value == 20.0 && *unit == 1));
    }

    #[test]
    fn interpolates_rotate_3d_with_a_zero_axis() {
        let retained_angle =
            |value| RetainedStyleValueData::from_arc(Arc::new(StyleValueData::Angle { value, unit: 0 }));
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
        let value = result.value.unwrap().into_arc();
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
        assert!(result.value.is_none());
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
        let value = result.value.unwrap().into_arc();
        assert!(matches!(&*value, StyleValueData::Calculated { .. }));

        let result = composite_scalar_value(&from, &to, FfiCompositeOperation::Add);
        assert!(result.handled);
        let value = result.value.unwrap().into_arc();
        assert!(matches!(&*value, StyleValueData::Calculated { .. }));
    }

    fn parsed(property: u16, source: &str) -> Arc<StyleValueData> {
        use crate::css::css_tokenizer::{TokenizerInput, tokenize_for_parser};
        use crate::css::parser::component_value::consume_a_list_of_component_values;
        use crate::css::parser::value_parser::{ParseContext, ParseOutcome, parse_css_value};
        let units: Vec<_> = source.encode_utf16().collect();
        let values = consume_a_list_of_component_values(tokenize_for_parser(TokenizerInput::Utf16(&units))).unwrap();
        // All fields are scalars or nullable pointers.
        let context = ParseContext::default();
        let ParseOutcome::Parsed(value) = parse_css_value(&context, property, &values) else {
            panic!("invalid test value {source}");
        };
        value
    }

    // The value a handled interpolation or composition produced, serialized; nothing where it declined to combine.
    fn serialized(result: AnimationValueResult) -> Option<String> {
        assert!(result.handled);
        (result.value.is_some()).then(|| {
            let value = result.value.unwrap().into_arc();
            String::from_utf16(&crate::css::serialize::serialize_style_value_to_utf16(&value)).unwrap()
        })
    }

    #[test]
    fn interpolates_and_adds_structured_values() {
        use crate::css::property_metadata::property_id;
        for (property, from, to, halfway, sum) in [
            (
                property_id::TEXT_INDENT,
                "2px hanging",
                "6px hanging",
                "4px hanging",
                "8px hanging",
            ),
            (property_id::BACKGROUND_POSITION_X, "4px", "8px", "6px", "12px"),
            (
                property_id::OBJECT_POSITION,
                "4px 8px",
                "8px 16px",
                "6px 12px",
                "12px 24px",
            ),
            (
                property_id::CLIP,
                "rect(1px, 2px, 3px, 4px)",
                "rect(3px, 6px, 9px, 12px)",
                "rect(2px, 4px, 6px, 8px)",
                "rect(4px, 8px, 12px, 16px)",
            ),
            (
                property_id::BORDER_IMAGE_SLICE,
                "1 2 3 4 fill",
                "3 6 9 12 fill",
                "2 4 6 8 fill",
                "4 8 12 16 fill",
            ),
            (
                property_id::BORDER_TOP_LEFT_RADIUS,
                "2px 4px",
                "6px 8px",
                "4px 6px",
                "8px 12px",
            ),
            (
                property_id::BACKGROUND_SIZE,
                "4px 8px",
                "8px 16px",
                "6px 12px",
                "12px 24px",
            ),
            (
                property_id::FONT_VARIATION_SETTINGS,
                "\"wght\" 100",
                "\"wght\" 300",
                "\"wght\" 200",
                "\"wght\" 400",
            ),
        ] {
            let (from, to) = (parsed(property, from), parsed(property, to));
            assert_eq!(
                serialized(interpolate_value(None, property, &from, &to, 0.5)).as_deref(),
                Some(halfway)
            );
            assert_eq!(
                serialized(composite_scalar_value(&from, &to, FfiCompositeOperation::Add)).as_deref(),
                Some(sum)
            );
        }
    }

    #[test]
    fn declines_to_interpolate_unlike_structured_values() {
        use crate::css::property_metadata::property_id;
        for (property, from, to) in [
            (property_id::BORDER_IMAGE_SLICE, "1 2 3 4 fill", "3 6 9 12"),
            (property_id::FONT_VARIATION_SETTINGS, "\"wght\" 100", "\"slnt\" 300"),
        ] {
            let (from, to) = (parsed(property, from), parsed(property, to));
            assert_eq!(serialized(interpolate_value(None, property, &from, &to, 0.5)), None);
        }
    }

    #[test]
    fn clamps_extrapolated_structured_values() {
        use crate::css::property_metadata::property_id;
        let property = property_id::BORDER_IMAGE_SLICE;
        let (from, to) = (parsed(property, "1 2 3 4 fill"), parsed(property, "3 6 9 12 fill"));
        assert_eq!(
            serialized(interpolate_value(None, property, &from, &to, -1.0)).as_deref(),
            Some("0 fill")
        );

        let result = interpolate_value(
            None,
            property_id::MARGIN_TOP,
            &border_radius_rect([(1.0, 1.0); 4]),
            &border_radius_rect([(3.0, 3.0); 4]),
            -1.0,
        );
        assert_eq!(serialized(result).as_deref(), Some("0px"));
    }

    // Corner radii, horizontal and vertical, from the top left corner clockwise.
    fn border_radius_rect(corners: [(f64, f64); 4]) -> Arc<StyleValueData> {
        let px = |value| {
            RetainedStyleValueData::from_owned(StyleValueData::Length {
                value,
                unit: crate::css::style_compute::px_length_unit(),
            })
        };
        let [top_left, top_right, bottom_right, bottom_left] = corners.map(|(horizontal, vertical)| {
            RetainedStyleValueData::from_owned(StyleValueData::BorderRadius {
                is_elliptical: horizontal != vertical,
                horizontal_radius: px(horizontal),
                vertical_radius: px(vertical),
            })
        });
        Arc::new(StyleValueData::BorderRadiusRect {
            top_left,
            top_right,
            bottom_right,
            bottom_left,
        })
    }

    #[test]
    fn interpolates_and_adds_border_radius_rects() {
        let from = border_radius_rect([(1.0, 2.0), (2.0, 3.0), (3.0, 4.0), (4.0, 5.0)]);
        let to = border_radius_rect([(3.0, 4.0), (4.0, 5.0), (5.0, 6.0), (6.0, 7.0)]);
        let property = crate::css::property_metadata::property_id::MARGIN_TOP;
        assert_eq!(
            serialized(interpolate_value(None, property, &from, &to, 0.5)).as_deref(),
            Some("2px 3px 4px 5px / 3px 4px 5px 6px")
        );
        assert_eq!(
            serialized(composite_scalar_value(&from, &to, FfiCompositeOperation::Add)).as_deref(),
            Some("4px 6px 8px 10px / 6px 8px 10px 12px")
        );
    }

    #[test]
    fn compares_gradients_by_value() {
        use crate::css::property_metadata::property_id::BACKGROUND_IMAGE;
        let radial = "radial-gradient(farthest-corner, currentcolor, red)";
        assert!(parsed(BACKGROUND_IMAGE, radial) == parsed(BACKGROUND_IMAGE, radial));
        assert!(
            parsed(BACKGROUND_IMAGE, "radial-gradient(50px 30px, red, blue)")
                != parsed(BACKGROUND_IMAGE, "radial-gradient(50px 40px, red, blue)")
        );
        let legacy = "conic-gradient(rgb(255, 0, 0), rgb(0, 0, 255))";
        assert!(parsed(BACKGROUND_IMAGE, legacy) == parsed(BACKGROUND_IMAGE, legacy));
        assert!(
            parsed(BACKGROUND_IMAGE, legacy) != parsed(BACKGROUND_IMAGE, "conic-gradient(rgb(255 0 0), rgb(0 0 255))")
        );
    }

    #[test]
    fn resolves_halfway_superellipses_to_superellipse_zero() {
        let property = crate::css::property_metadata::property_id::CORNER_TOP_LEFT_SHAPE;
        let (from, to) = (
            parsed(property, "superellipse(-infinity)"),
            parsed(property, "superellipse(infinity)"),
        );
        let result = interpolate_value(None, property, &from, &to, 0.5);
        assert!(result.handled);
        let value = result.value.unwrap().into_arc();
        let text = crate::css::serialize::serialize_resolved_style_value_to_utf16(&value);
        assert_eq!(String::from_utf16(&text).unwrap(), "superellipse(0)");
    }

    #[test]
    fn adds_opacities_up_to_one() {
        let opacity = || {
            Arc::new(StyleValueData::OpacityValue {
                value: retained_number(0.75),
            })
        };
        let result = composite_scalar_value(&opacity(), &opacity(), FfiCompositeOperation::Add);
        assert_eq!(serialized(result).as_deref(), Some("1"));
    }
}
