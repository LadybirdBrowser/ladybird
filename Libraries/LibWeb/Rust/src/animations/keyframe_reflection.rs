/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The keyframes script reads of an effect whose keyframes come from CSS, a `@keyframes` rule or a CSS transition,
//! and those a copy of such an effect takes.
//! https://drafts.csswg.org/css-animations-2/#keyframes

use super::animation::AnimationKind;
use super::bindings_api::ParsedEasing;
use super::effect::AnimationEffectRecord;
use super::keyframes::{
    CompositeOperationOrAuto, KeyframeOrigin, KeyframeProperty, KeyframeSet, ResourceContext, offset_of_key,
};
use crate::css::style_compute::{FfiReflectedKeyframeProperty, FfiStyleSheetResourceContext};
use crate::css::style_value::{RetainedStyleValueData, StyleValueData, retain_style_value};
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

/// A keyframe of CSS as script reads it.
pub(crate) struct CssKeyframe {
    pub(crate) offset: f64,
    pub(crate) easing: ParsedEasing,
    pub(crate) composite: CompositeOperationOrAuto,
    pub(crate) properties: Vec<(KeyframeProperty, RetainedStyleValueData)>,
}

impl CssKeyframe {
    /// Sets the value the keyframe gives `property`, in place of what it gave it before.
    fn set(&mut self, property: KeyframeProperty, value: RetainedStyleValueData) {
        match self.properties.iter_mut().find(|(declared, _)| *declared == property) {
            Some((_, declared_value)) => *declared_value = value,
            None => self.properties.push((property, value)),
        }
    }
}

/// What reading the keyframes of CSS of an effect reads of it, which holds no borrow of its record.
pub(crate) struct ReflectedEffect {
    keyframes: Option<Arc<KeyframeSet>>,
    /// https://drafts.csswg.org/css-animations-2/#keyframes
    /// The easing a keyframe of the effect that declares none of its own runs: the `animation-timing-function` of its
    /// CSS animation, and `linear` for any other.
    default_easing: ParsedEasing,
    target: Option<NonNull<c_void>>,
    /// The host's `PseudoElement` of the target pseudo-selector, if any.
    pseudo_element: Option<u8>,
}

impl ReflectedEffect {
    pub(crate) fn of(effect: &AnimationEffectRecord) -> Self {
        let default_easing = effect
            .associated_animation()
            .map_or_else(ParsedEasing::linear, |animation| {
                let animation = animation.borrow();
                let AnimationKind::CssAnimation(state) = animation.kind() else {
                    return ParsedEasing::linear();
                };
                state
                    .applied_definition
                    .as_ref()
                    .and_then(|definition| ParsedEasing::of_timing_function(definition.timing_function()))
                    .unwrap_or_else(ParsedEasing::ease)
            });
        Self {
            keyframes: effect.keyframes().cloned(),
            default_easing,
            target: effect.target_element(),
            pseudo_element: effect.pseudo_element().map(|pseudo_element| pseudo_element.kind),
        }
    }
}

/// The keyframes of CSS the effect runs as they are declared: those of each block of its `@keyframes` rule, or those
/// of its CSS transition, with the blocks of one offset, easing and composite operation merged.
fn declared_css_keyframes(effect: &ReflectedEffect) -> Vec<CssKeyframe> {
    let mut frames: Vec<CssKeyframe> = Vec::new();
    let Some(set) = effect.keyframes.as_deref() else {
        return frames;
    };

    let default_easing = &effect.default_easing;
    let mut append_frame =
        |key: u64,
         easing: Option<&RetainedStyleValueData>,
         composite: CompositeOperationOrAuto,
         declarations: &mut dyn Iterator<Item = (KeyframeProperty, &RetainedStyleValueData)>| {
            let offset = offset_of_key(key as f64);
            // AD-HOC: Step 2.2 uses the computed timing function. Substitution-dependent keyframe easings use the
            //         animation-wide easing for web compatibility.
            let easing = easing
                .and_then(ParsedEasing::of_timing_function)
                .unwrap_or_else(|| default_easing.clone());
            let index = match frames.iter().position(|frame| {
                frame.offset == offset
                    && frame.easing.serialization() == easing.serialization()
                    && frame.composite == composite
            }) {
                Some(index) => index,
                None => {
                    frames.push(CssKeyframe {
                        offset,
                        easing,
                        composite,
                        properties: Vec::new(),
                    });
                    frames.len() - 1
                }
            };
            for (property, value) in declarations {
                frames[index].set(property, value.clone_retained());
            }
        };
    match set.declared_keyframes() {
        // 2.3.1. Cascade together all of its declaration blocks such that for each CSS property (except those that are
        //        "not animatable", which must be ignored) the last declaration among all its <keyframe-block>
        //        declarations takes precedence.
        Some(declared) => {
            for keyframe in declared.keyframes() {
                append_frame(
                    keyframe.key,
                    keyframe.easing.as_ref(),
                    keyframe.composite,
                    &mut declared
                        .declarations_of(keyframe)
                        .iter()
                        .map(|(property, value)| (property.clone(), value)),
                );
            }
        }
        None => {
            for keyframe in set.keyframes() {
                let declarations = set.declarations_of(keyframe).iter().filter_map(|declaration| {
                    Some((
                        KeyframeProperty::Id(declaration.property_id),
                        declaration.value.declared()?,
                    ))
                });
                let custom_declarations = set.custom_declarations_of(keyframe).iter().filter_map(|declaration| {
                    Some((
                        KeyframeProperty::Custom(declaration.name.clone()),
                        declaration.value.declared()?,
                    ))
                });
                append_frame(
                    keyframe.key,
                    None,
                    keyframe.composite,
                    &mut declarations.chain(custom_declarations),
                );
            }
        }
    }
    frames
}

/// The keyframes of CSS a copy of the effect takes: those it declares, by their offsets, with the values of a CSS
/// transition unscaled by the effective zoom they were computed at.
pub(crate) fn css_keyframes_of_copy(effect: &ReflectedEffect) -> Vec<CssKeyframe> {
    let mut frames = declared_css_keyframes(effect);
    frames.sort_by(|first, second| first.offset.total_cmp(&second.offset));
    if let Some(set) = effect.keyframes.as_deref()
        && set.origin() == KeyframeOrigin::Transition
    {
        unscale_by_zoom(&mut frames, f64::from(set.computed_value_zoom()));
    }
    frames
}

/// The style sheet the keyframes of the effect come from, which a copy of it resolves URLs against.
pub(crate) fn resource_context(effect: &AnimationEffectRecord) -> Option<ResourceContext> {
    effect.keyframes()?.resource_context().cloned()
}

/// https://drafts.csswg.org/css-animations-2/#keyframes
/// The keyframes of CSS the effect runs as `getKeyframes()` returns them: the values a `@keyframes` rule declares
/// computed against the target, with its implicit endpoints taking the target's own values, and those of a CSS
/// transition, each by its offset, and unscaled by the effective zoom they were computed at.
pub(crate) fn computed_css_keyframes(effect: &ReflectedEffect) -> Vec<CssKeyframe> {
    let mut frames = declared_css_keyframes(effect);
    let Some(set) = effect.keyframes.as_deref() else {
        return frames;
    };
    if set.origin() == KeyframeOrigin::Transition {
        unscale_by_zoom(&mut frames, f64::from(set.computed_value_zoom()));
        return frames;
    }
    if set.declared_keyframes().is_none() {
        return frames;
    }
    let mut has_custom_properties = false;
    for frame in &mut frames {
        frame.properties.retain(|(property, _)| {
            let is_custom = matches!(property, KeyframeProperty::Custom(_));
            has_custom_properties |= is_custom;
            !is_custom
        });
    }
    let default_easing = &effect.default_easing;
    if !has_custom_properties && frames.iter().all(|frame| frame.properties.is_empty()) {
        return Vec::new();
    }

    let (target, pseudo_element) = (effect.target, effect.pseudo_element);
    if let Some(target) = target
        && let Some(underlying_values) =
            compute_css_keyframe_values(target, pseudo_element, &mut frames, set.resource_context())
    {
        for offset in [0.0, 1.0] {
            let mut present: Vec<u16> = Vec::new();
            let mut destination = None;
            for (index, frame) in frames.iter().enumerate() {
                if frame.offset != offset {
                    continue;
                }
                present.extend(frame.properties.iter().filter_map(|(property, _)| match property {
                    KeyframeProperty::Id(property_id) => Some(*property_id),
                    KeyframeProperty::Custom(_) => None,
                }));
                // AD-HOC: Step 3.1 uses the default composite. Underlying-value endpoints use "replace", even for an
                //         additive effect, so their values aren't added to themselves.
                if destination.is_none()
                    && frame.easing.serialization() == default_easing.serialization()
                    && matches!(
                        frame.composite,
                        CompositeOperationOrAuto::Auto | CompositeOperationOrAuto::Replace
                    )
                {
                    destination = Some(index);
                }
            }
            let missing: Vec<_> = underlying_values
                .iter()
                .filter(|(property_id, _)| !present.contains(property_id))
                .map(|(property_id, value)| (KeyframeProperty::Id(*property_id), value.clone_retained()))
                .collect();
            // AD-HOC: Custom properties contribute keyframe offsets and implicit endpoints, but their values are
            //         omitted from CSS keyframe reflection for web compatibility.
            if missing.is_empty() && !has_custom_properties {
                continue;
            }
            // 3.2. For any property in animated properties that is not otherwise present in a keyframe with an offset
            //      of 0% or one that would be positioned earlier in the used keyframe order, add the computed value of
            //      that property on element to initial keyframe's keyframe values.
            match destination {
                Some(destination) => {
                    for (property, value) in missing {
                        frames[destination].set(property, value);
                    }
                }
                None => {
                    // AD-HOC: Step 3.3 prepends the initial keyframe. For web compatibility, reflected synthetic
                    //         endpoints follow explicitly declared frames at the same offset.
                    frames.push(CssKeyframe {
                        offset,
                        easing: default_easing.clone(),
                        composite: CompositeOperationOrAuto::Replace,
                        properties: missing,
                    });
                }
            }
        }
    }
    // 4. The computed order of keyframes—which is the order returned by getKeyframes()—is found by shifting any
    //    keyframes whose offset was specified as a <percentage>, from keyword, or to keyword to the front of the list
    //    (after the generated initial keyframe, if any), and performing a stable sort on these keyframes by their
    //    keyframe offsets.
    frames.sort_by(|first, second| first.offset.total_cmp(&second.offset));
    let zoom = target.map_or(1.0, |target| {
        // SAFETY: The effect keeps its target alive.
        unsafe { web_animation_effective_zoom_of_target(target, pseudo_element.is_some(), pseudo_element.unwrap_or(0)) }
    });
    unscale_by_zoom(&mut frames, zoom);
    frames
}

/// Unscales the values of the longhands `frames` give by the effective zoom `zoom`.
/// https://drafts.csswg.org/css-viewport/#zoom-om
fn unscale_by_zoom(frames: &mut [CssKeyframe], zoom: f64) {
    use crate::css::property_metadata::property_id;
    for frame in frames {
        for (property, value) in &mut frame.properties {
            if matches!(
                property,
                KeyframeProperty::Id(property_id::ZOOM) | KeyframeProperty::Custom(_)
            ) {
                continue;
            }
            let unscaled = crate::css::style_value::rust_style_value_unscaled_by_zoom(value.data(), zoom);
            if !unscaled.is_null() {
                // SAFETY: The unscaled value is retained for the caller.
                *value = unsafe { RetainedStyleValueData::from_retained_pointer(unscaled) };
            }
        }
    }
}

/// Computes over the style of the host's `DOM::Element` `target`, or its pseudo-element of the host's
/// `PseudoElement` `pseudo_element`, what the longhands `frames` declare compute to, replacing each frame's
/// declarations with those, by their physical longhands, and answers the target's values of each of them, or none
/// where the target has no style to compute them over.
fn compute_css_keyframe_values(
    target: NonNull<c_void>,
    pseudo_element: Option<u8>,
    frames: &mut [CssKeyframe],
    resource_context: Option<&ResourceContext>,
) -> Option<Vec<(u16, RetainedStyleValueData)>> {
    let properties: Vec<FfiReflectedKeyframeProperty> = frames
        .iter()
        .enumerate()
        .flat_map(|(keyframe_index, frame)| {
            frame
                .properties
                .iter()
                .filter_map(move |(property, value)| match property {
                    KeyframeProperty::Id(property_id) => Some(FfiReflectedKeyframeProperty {
                        keyframe_index,
                        property_id: *property_id,
                        value: value.pointer(),
                    }),
                    KeyframeProperty::Custom(_) => None,
                })
        })
        .collect();
    let resource_context = resource_context.map_or(
        FfiStyleSheetResourceContext {
            base_url: std::ptr::null(),
            base_url_length: 0,
            has_value: false,
            origin_clean: false,
        },
        |context| FfiStyleSheetResourceContext {
            base_url: context.base_url.as_ptr(),
            base_url_length: context.base_url.len(),
            has_value: true,
            origin_clean: context.origin_clean,
        },
    );

    struct Computed {
        frames: Vec<Vec<(u16, RetainedStyleValueData)>>,
        underlying: Vec<(u16, RetainedStyleValueData)>,
    }
    unsafe extern "C" fn emit(context: *mut c_void, index: usize, property_id: u16, value: *const StyleValueData) {
        // SAFETY: The context is the computation below, and the value is live for the call.
        let (computed, value) = unsafe {
            (
                &mut *context.cast::<Computed>(),
                RetainedStyleValueData::from_retained_pointer(retain_style_value(value)),
            )
        };
        let values = computed.frames.get_mut(index).unwrap_or(&mut computed.underlying);
        match values.iter_mut().find(|(held, _)| *held == property_id) {
            Some((_, held)) => *held = value,
            None => values.push((property_id, value)),
        }
    }
    let mut computed = Computed {
        frames: frames.iter().map(|_| Vec::new()).collect(),
        underlying: Vec::new(),
    };
    // SAFETY: The effect keeps its target alive, and the declarations and resource context are live for the call.
    let computed_over_style = unsafe {
        web_animation_compute_css_keyframe_values(
            target,
            pseudo_element.is_some(),
            pseudo_element.unwrap_or(0),
            properties.as_ptr().cast(),
            properties.len(),
            frames.len(),
            &resource_context,
            (&raw mut computed).cast(),
            emit,
        )
    };
    if !computed_over_style {
        return None;
    }
    for (frame, values) in frames.iter_mut().zip(computed.frames) {
        frame.properties = values
            .into_iter()
            .map(|(property_id, value)| (KeyframeProperty::Id(property_id), value))
            .collect();
    }
    Some(computed.underlying)
}

unsafe extern "C" {
    /// Computes over the style of the target what the declarations `properties` of `keyframe_count` keyframes, from a
    /// style sheet with `resource_context`, compute to, emitting each computed value with the index of its keyframe and
    /// each value of the target with `keyframe_count`, or answers false where the target has no style. `properties`
    /// points at `property_count` `FfiReflectedKeyframeProperty`s.
    fn web_animation_compute_css_keyframe_values(
        target: NonNull<c_void>,
        has_pseudo_element: bool,
        pseudo_element: u8,
        properties: *const c_void,
        property_count: usize,
        keyframe_count: usize,
        resource_context: &FfiStyleSheetResourceContext,
        context: *mut c_void,
        emit: unsafe extern "C" fn(*mut c_void, usize, u16, *const StyleValueData),
    ) -> bool;

    /// The effective zoom of the computed style of the target, one where it has none.
    fn web_animation_effective_zoom_of_target(
        target: NonNull<c_void>,
        has_pseudo_element: bool,
        pseudo_element: u8,
    ) -> f64;
}

#[cfg(test)]
mod ffi_test_stubs {
    use super::*;

    #[unsafe(no_mangle)]
    extern "C" fn web_animation_compute_css_keyframe_values(
        _target: NonNull<c_void>,
        _has_pseudo_element: bool,
        _pseudo_element: u8,
        _properties: *const c_void,
        _property_count: usize,
        _keyframe_count: usize,
        _resource_context: &FfiStyleSheetResourceContext,
        _context: *mut c_void,
        _emit: unsafe extern "C" fn(*mut c_void, usize, u16, *const StyleValueData),
    ) -> bool {
        false
    }

    #[unsafe(no_mangle)]
    extern "C" fn web_animation_effective_zoom_of_target(
        _target: NonNull<c_void>,
        _has_pseudo_element: bool,
        _pseudo_element: u8,
    ) -> f64 {
        1.0
    }
}
