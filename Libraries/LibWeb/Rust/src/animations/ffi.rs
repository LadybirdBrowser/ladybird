/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ (the DOM, style computation, SVG and the event loop) reaches the animation
//! records, and the host functions the records call back into.

use super::animation::{
    AnimationCell, AnimationHandle, AnimationKind, AttachmentRangeEdge, AttachmentRangeEdgeOffset,
    CSS_ANIMATION_PLAY_STATE_PAUSED, CSS_ANIMATION_PLAY_STATE_RUNNING, CssAnimationFlag, CssAnimationState,
    CssTransitionState, HostCellKind, HostTaskId, OwningElement, SmilHolder, TransitionPhase, WeakAnimationSet,
};
use super::clock_plan::ClockPlanAnimation;
use super::composite_order::composite_order;
use super::document_animations::{DocumentAnimationSetKind, DocumentAnimations, DocumentEffectSetKind};
use super::easing::CompactEasing;
use super::effect::{AnimationEffectCell, AnimationEffectId, EffectFlag, EffectHandle, resolve_effect_timing};
use super::element_animations::{AnimationOwnerSlot, ElementAnimations, StyleEngineTarget, TransitionProperty};
use super::host_effects::{HostEffectKind, HostEffects, PlaybackEventType};
use super::host_timing::FfiAnimationKind;
use super::keyframes::{
    CompositeOperationOrAuto, KeyframeProperty, KeyframeSet, ProcessedKeyframe, ResourceContext, key_of_offset,
};
use super::pending_animation_events::{
    FfiPendingAnimationEvent, PendingAnimationEvent, PendingAnimationEventKind, TimeEventType,
};
use super::procedures::{self, AnimationError, DidSeek, Invalidate, Procedures, SynchronouslyNotify};
use super::provisional_transitions::TransitionTarget;
use super::rendering_trace::note_rendering_trace_event;
use super::timeline::{
    ScrollTimelineSource, ScrollTimelineState, TimelineCell, TimelineHandle, TimelineKind, ViewTimelineInset,
    ViewTimelineInsetEdge, ViewTimelineState,
};
use super::timeline_host::{create_timeline, default_document_timeline};
use super::timing::{AnimationPlayState, OptionalTimeValue, SpecifiedEffectTiming, TimeValue};
use super::transitions::{self, LaneAdoption, ProvisionalTransitionTiming, TransitionOrigin};
use crate::css::animation::FfiCompositeOperation;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style::animations::AppliedAnimationDefinition;
use crate::css::style::tree::StyleNodeID;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData, retain_style_value};
use crate::css::transition::{FfiExistingTransition, FfiLaneTransition, FfiTransitionAction, FfiTransitionActionKind};
use crate::render_state::DocumentHost;
use libcompositing_rust::easing::{Easing, FfiEasingDescriptor};
use smallvec::SmallVec;
use std::cmp::Ordering;
use std::ffi::c_void;
use std::num::NonZeroU64;
use std::ptr::NonNull;
use std::sync::Arc;

/// One thing a procedure asks the host to do for the animation `animation`, and what it needs to do it.
#[repr(C)]
pub struct FfiHostEffect {
    pub animation: NonNull<AnimationCell>,
    pub kind: FfiHostEffectKind,
    /// For `QueuePlaybackEvent`.
    pub playback_event_type: PlaybackEventType,
    /// For `InvalidateEffect`.
    pub changes_plan: bool,
    /// For `InvalidateUnlessThrottled`.
    pub invalidate: bool,
    /// For `QueuePlaybackEvent`.
    pub current_time: OptionalTimeValue,
    pub timeline_time: OptionalTimeValue,
    pub scheduled_timeline_time: OptionalTimeValue,
    /// For `ScheduleWakeup`, in milliseconds.
    pub delay: f64,
    /// For `CancelFinishNotificationTask`, the host's identity of the task to cancel.
    pub task: u64,
    /// For `AssociateWithTarget` and `DisassociateFromTarget`, the host's `DOM::Element`.
    pub element: *mut c_void,
    /// For `DisassociateFromTarget` and `RecomputeEffectStyle`, the effect, which lives as long as the host performs
    /// this.
    pub effect: *const AnimationEffectCell,
}

/// What a procedure asks the host to do, as `HostEffectKind` describes it.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum FfiHostEffectKind {
    ReplaceReadyPromise,
    ResolveReadyPromise,
    RejectReadyPromise,
    ReplaceFinishedPromise,
    ResolveFinishedPromise,
    RejectFinishedPromise,
    QueueFinishNotificationTask,
    CancelFinishNotificationTask,
    QueuePlaybackEvent,
    InvalidateEffect,
    InvalidateUnlessThrottled,
    AssociateWithTarget,
    DisassociateFromTarget,
    RecomputeEffectStyle,
    RequestRepaint,
    ScheduleWakeup,
}

impl FfiHostEffect {
    fn new(animation: &AnimationHandle, kind: &HostEffectKind, task: Option<HostTaskId>) -> Self {
        let mut effect = Self {
            animation: NonNull::from(&**animation),
            kind: FfiHostEffectKind::ReplaceReadyPromise,
            playback_event_type: PlaybackEventType::Finish,
            changes_plan: false,
            invalidate: false,
            current_time: OptionalTimeValue::UNRESOLVED,
            timeline_time: OptionalTimeValue::UNRESOLVED,
            scheduled_timeline_time: OptionalTimeValue::UNRESOLVED,
            delay: 0.0,
            task: task.map_or(0, |task| task.0.get()),
            element: std::ptr::null_mut(),
            effect: std::ptr::null(),
        };
        effect.kind = match *kind {
            HostEffectKind::ReplaceReadyPromise => FfiHostEffectKind::ReplaceReadyPromise,
            HostEffectKind::ResolveReadyPromise => FfiHostEffectKind::ResolveReadyPromise,
            HostEffectKind::RejectReadyPromise => FfiHostEffectKind::RejectReadyPromise,
            HostEffectKind::ReplaceFinishedPromise => FfiHostEffectKind::ReplaceFinishedPromise,
            HostEffectKind::ResolveFinishedPromise => FfiHostEffectKind::ResolveFinishedPromise,
            HostEffectKind::RejectFinishedPromise => FfiHostEffectKind::RejectFinishedPromise,
            HostEffectKind::QueueFinishNotificationTask => FfiHostEffectKind::QueueFinishNotificationTask,
            HostEffectKind::CancelFinishNotificationTask => FfiHostEffectKind::CancelFinishNotificationTask,
            HostEffectKind::QueuePlaybackEvent {
                r#type,
                current_time,
                timeline_time,
                scheduled_timeline_time,
            } => {
                effect.playback_event_type = r#type;
                effect.current_time = current_time.into();
                effect.timeline_time = timeline_time.into();
                effect.scheduled_timeline_time = scheduled_timeline_time.into();
                FfiHostEffectKind::QueuePlaybackEvent
            }
            HostEffectKind::InvalidateEffect { changes_plan } => {
                effect.changes_plan = changes_plan;
                FfiHostEffectKind::InvalidateEffect
            }
            HostEffectKind::InvalidateUnlessThrottled { invalidate } => {
                effect.invalidate = invalidate;
                FfiHostEffectKind::InvalidateUnlessThrottled
            }
            HostEffectKind::AssociateWithTarget { element } => {
                effect.element = element.as_ptr();
                FfiHostEffectKind::AssociateWithTarget
            }
            HostEffectKind::DisassociateFromTarget {
                element,
                effect: ref animation_effect,
            } => {
                effect.element = element.as_ptr();
                effect.effect = &raw const **animation_effect;
                FfiHostEffectKind::DisassociateFromTarget
            }
            HostEffectKind::RecomputeEffectStyle {
                effect: ref animation_effect,
            } => {
                effect.effect = &raw const **animation_effect;
                FfiHostEffectKind::RecomputeEffectStyle
            }
            HostEffectKind::RequestRepaint => FfiHostEffectKind::RequestRepaint,
            HostEffectKind::ScheduleWakeup { delay } => {
                effect.delay = delay;
                FfiHostEffectKind::ScheduleWakeup
            }
        };
        effect
    }
}

/// The host's function that performs what a procedure asks of it.
pub type HostEffectPerformer = extern "C" fn(effect: &FfiHostEffect);

/// Runs `procedure`, which does not throw, on `animation` and then has the host perform what it asked for through
/// `perform`, in order, holding no borrow of any record.
pub(crate) fn run_procedure(
    animation: &AnimationCell,
    perform: HostEffectPerformer,
    procedure: impl FnOnce(&mut Procedures<'_>),
) {
    run_procedure_on(animation, perform, |procedures| {
        procedure(procedures);
        Ok(())
    })
    .expect("the procedure does not throw");
}

/// Runs `procedure` on `animation` and then has the host perform what it asked for through `perform`, in order, holding
/// no borrow of any record.
pub(crate) fn run_procedure_on(
    animation: &AnimationCell,
    perform: HostEffectPerformer,
    procedure: impl FnOnce(&mut Procedures<'_>) -> Result<(), AnimationError>,
) -> Result<(), AnimationError> {
    // SAFETY: The host holds a reference to every animation it runs a procedure on.
    let animation = unsafe { AnimationHandle::retained_from_host(animation) };
    let ((), effects) = procedures::run(&animation, procedure)?;
    perform_host_effects(effects, perform);
    Ok(())
}

/// Has the host perform `effects` through `perform`, in order, holding no borrow of any record.
fn perform_host_effects(effects: HostEffects, perform: HostEffectPerformer) {
    for (animation, kind) in effects {
        // NB: A procedure may name an animation that is no longer alive to the host, whose record the host only lets go
        //     of as it sweeps.
        if !animation.is_alive() {
            continue;
        }
        // NB: The record keeps the task the host queued, which the host then neither queues again nor cancels without.
        let task = animation.borrow().finish_notification_task;
        match kind {
            HostEffectKind::QueueFinishNotificationTask if task.is_some() => continue,
            HostEffectKind::CancelFinishNotificationTask if task.is_none() => continue,
            _ => {}
        }
        perform(&FfiHostEffect::new(&animation, &kind, task));
        // NB: A canceled task is no longer queued, so a later finish notification task can be queued again.
        if matches!(kind, HostEffectKind::CancelFinishNotificationTask) {
            animation.borrow_mut().finish_notification_task = None;
        }
    }
}

/// Notes `task`, the host's identity of the task it queued to run the finish notification steps of `animation`, or to
/// dispatch its finish event.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_set_finish_notification_task(animation: &AnimationCell, task: u64) {
    let task = HostTaskId(NonZeroU64::new(task).expect("the host's tasks have nonzero identities"));
    animation.borrow_mut().finish_notification_task = Some(task);
}

pub(crate) fn invalidate(invalidate: bool) -> Invalidate {
    if invalidate { Invalidate::Yes } else { Invalidate::No }
}

/// Runs the finish notification steps of `animation` on its timeline, if it has one, from the task the host queued.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_run_finish_notification_steps(
    animation: &AnimationCell,
    perform: HostEffectPerformer,
) {
    run_procedure(animation, perform, |procedures| procedures.finish_notification_steps());
}

/// https://www.w3.org/TR/web-animations-1/#dom-animation-cancel
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_cancel(
    animation: &AnimationCell,
    should_invalidate: bool,
    perform: HostEffectPerformer,
) {
    run_procedure(animation, perform, |procedures| {
        procedures.cancel(invalidate(should_invalidate));
    });
}

/// The keyframe set `set` names, borrowed for as long as the reference the host holds.
///
/// # Safety
/// `set` must be a keyframe set the host holds a reference to.
unsafe fn borrowed_keyframe_set<'a>(set: *const KeyframeSet) -> &'a KeyframeSet {
    // SAFETY: Guaranteed by the caller.
    unsafe { &*set }
}

/// Hands the host the one reference to a new keyframe set.
fn into_host(set: KeyframeSet) -> *const KeyframeSet {
    Arc::into_raw(Arc::new(set))
}

/// Takes another reference to a keyframe set the host holds one to.
///
/// # Safety
/// `set` must be a keyframe set the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_keyframe_set_retain(set: *const KeyframeSet) {
    // SAFETY: Guaranteed by the caller.
    unsafe { Arc::increment_strong_count(set) };
}

/// Gives up one of the references the host holds to a keyframe set.
///
/// # Safety
/// `set` must be a keyframe set the host holds a reference to, which it gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_keyframe_set_release(set: *const KeyframeSet) {
    // SAFETY: Guaranteed by the caller.
    unsafe { Arc::decrement_strong_count(set) };
}

/// The keyframe set of the `@keyframes` rule `rule` views, a `ValueParserFFI::NativeRuleView`, from a style sheet
/// whose base URL, where `has_resource_context`, is `base_url`, with the reference to it the host takes over.
///
/// # Safety
/// `rule` must view a live `@keyframes` rule for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_keyframe_set_from_keyframes_rule(
    rule: *const c_void,
    has_resource_context: bool,
    base_url: ak::Span<'_, u8>,
    origin_clean: bool,
) -> *const KeyframeSet {
    // SAFETY: Guaranteed by the caller.
    let rule = unsafe { &*rule.cast::<crate::css::rule::read::NativeRuleView<'_>>() };
    let resource_context = has_resource_context.then(|| ResourceContext {
        base_url: base_url.as_slice().into(),
        origin_clean,
    });
    let mut blocks = Vec::new();
    rule.for_each_keyframe(|keys, declarations| blocks.push((keys.to_vec(), declarations)));
    into_host(KeyframeSet::from_keyframes_rule(
        blocks
            .iter()
            .map(|(keys, declarations)| (keys.as_slice(), &**declarations)),
        resource_context,
    ))
}

/// One keyframe script gave a `KeyframeEffect`, as processing the keyframes argument made it: its computed offset, its
/// composite operation, its easing where it has one, and the range of the properties it
/// declares.
#[repr(C)]
pub struct FfiProcessedKeyframe {
    pub offset: f64,
    pub composite: CompositeOperationOrAuto,
    pub has_easing: bool,
    pub easing: FfiEasingDescriptor,
    pub first_property: u32,
    pub property_count: u32,
}

/// One property a processed keyframe declares: a longhand or shorthand by its id, or, where `custom_name` is not
/// zero, the custom property of that raw fly string, with its value.
#[repr(C)]
pub struct FfiProcessedKeyframeProperty {
    pub property_id: u16,
    pub custom_name: usize,
    pub value: *const c_void,
}

/// The property a host names by its id, or by the raw fly string of a custom property's name where that is not zero.
///
/// # Safety
/// `custom_name` must be zero or a live raw fly string.
unsafe fn keyframe_property(property_id: u16, custom_name: usize) -> KeyframeProperty {
    match custom_name {
        0 => KeyframeProperty::Id(property_id),
        // SAFETY: Guaranteed by the caller.
        name => KeyframeProperty::Custom(unsafe { RetainedUtf16FlyString::from_borrowed_raw(name) }),
    }
}

/// # Safety
/// `value` must be null or a live style value.
unsafe fn retained_optional_value(value: *const c_void) -> RetainedStyleValueData {
    if value.is_null() {
        return RetainedStyleValueData::none();
    }
    // SAFETY: Guaranteed by the caller.
    unsafe { retained_value(value) }
}

/// # Safety
/// `value` must be a live style value.
unsafe fn retained_value(value: *const c_void) -> RetainedStyleValueData {
    // SAFETY: Guaranteed by the caller.
    unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(value.cast::<StyleValueData>())) }
}

/// The keyframe set of the keyframes script gave a `KeyframeEffect`, in the order it gave them, with the reference to
/// it the host takes over.
///
/// # Safety
/// Every value, custom-property name and linear easing point the keyframes name must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_keyframe_set_from_processed_keyframes(
    keyframes: ak::Span<'_, FfiProcessedKeyframe>,
    properties: ak::Span<'_, FfiProcessedKeyframeProperty>,
) -> *const KeyframeSet {
    let properties = properties.as_slice();
    let keyframes = keyframes
        .as_slice()
        .iter()
        .map(|keyframe| {
            let first = keyframe.first_property as usize;
            ProcessedKeyframe {
                key: key_of_offset(keyframe.offset),
                composite: keyframe.composite,
                // SAFETY: Guaranteed by the caller.
                easing: keyframe
                    .has_easing
                    .then(|| unsafe { Easing::from_descriptor(&keyframe.easing) }),
                declarations: properties[first..first + keyframe.property_count as usize]
                    .iter()
                    // SAFETY: Guaranteed by the caller.
                    .map(|property| unsafe {
                        (
                            keyframe_property(property.property_id, property.custom_name),
                            retained_value(property.value),
                        )
                    })
                    .collect(),
            }
        })
        .collect();
    into_host(KeyframeSet::from_processed_keyframes(keyframes, None))
}

/// What follows from a set's keyframes alone, borrowed from the set: the longhands they animate, sorted, the raw fly
/// strings of the names of the custom properties they animate, and what the set answers of them.
#[repr(C)]
pub struct FfiKeyframeSetFacts {
    pub target_properties: *const u16,
    pub target_property_count: usize,
    pub target_custom_properties: *const usize,
    pub target_custom_property_count: usize,
    pub animates_what_a_tick_cannot: bool,
    pub animates_an_inherited_property: bool,
}

/// What follows from the keyframes of a set alone.
///
/// # Safety
/// `set` must be a keyframe set the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_keyframe_set_facts(set: *const KeyframeSet) -> FfiKeyframeSetFacts {
    // SAFETY: Guaranteed by the caller.
    let set = unsafe { borrowed_keyframe_set(set) };
    FfiKeyframeSetFacts {
        target_properties: set.target_properties().as_ptr(),
        target_property_count: set.target_properties().len(),
        target_custom_properties: set.target_custom_properties().as_ptr().cast(),
        target_custom_property_count: set.target_custom_properties().len(),
        animates_what_a_tick_cannot: set.animates_what_a_tick_cannot(),
        animates_an_inherited_property: set.animates_an_inherited_property(),
    }
}

/// Notes what the `attr()`s of the values a keyframe set declares can read, answering whether they can read a name
/// none could before.
///
/// # Safety
/// `set` must be a keyframe set the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_note_attr_names_read_by_keyframe_set(
    host: &DocumentHost,
    set: *const KeyframeSet,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let set = unsafe { borrowed_keyframe_set(set) };
    let before = crate::css::style::host_atoms::attr_names_read_generation(host);
    crate::css::style::host_atoms::note_attr_names_read_by(host, set.declared_values());
    crate::css::style::host_atoms::attr_names_read_generation(host) != before
}

/// Takes a reference of its own to `effect`, which the host gives up with `animations_effect_release`.
///
/// # Safety
/// `effect` must be an effect the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_effect_retain(effect: *const AnimationEffectCell) {
    // SAFETY: Guaranteed by the caller.
    let _ = unsafe { EffectHandle::retained_from_host(effect) }.into_raw();
}

/// The host's `AnimationEffect` of `effect`, while it is alive.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_object_cell(effect: &AnimationEffectCell) -> *mut c_void {
    effect.object_cell().map_or(std::ptr::null_mut(), NonNull::as_ptr)
}

/// Gives up a reference the host holds to an effect.
///
/// # Safety
/// `effect` must be an effect the host holds a reference to, which it gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_effect_release(effect: *const AnimationEffectCell) {
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { EffectHandle::from_raw(effect) });
}

#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_id(effect: &AnimationEffectCell) -> AnimationEffectId {
    effect.borrow().id()
}

/// The keyframe set `effect` runs, borrowed until its keyframes change, or null where it has none.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_keyframe_set(effect: &AnimationEffectCell) -> *const KeyframeSet {
    effect.borrow().keyframes().map_or(std::ptr::null(), Arc::as_ptr)
}

/// Sets the keyframe set `effect` runs to `set`, which it takes a reference of its own to, or to none where `set` is
/// null.
///
/// # Safety
/// `set` must be null or a keyframe set the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_effect_set_keyframe_set(effect: &AnimationEffectCell, set: *const KeyframeSet) {
    // SAFETY: Guaranteed by the caller.
    let keyframes = (!set.is_null()).then(|| unsafe { KeyframeSet::retained_from_host(set) });
    effect.borrow_mut().set_keyframes(keyframes);
}

/// Sets the host's `DOM::Element` that `effect` targets, which the host keeps alive while the effect targets it, or
/// none where `element` is null, and has the host associate the associated animation of `effect`, if any, with it
/// through `perform`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_set_target_element(
    effect: &AnimationEffectCell,
    element: *mut c_void,
    perform: HostEffectPerformer,
) {
    // SAFETY: The host holds a reference to the effect it passes.
    let effect = unsafe { EffectHandle::retained_from_host(effect) };
    perform_host_effects(procedures::set_effect_target(&effect, NonNull::new(element)), perform);
}

/// The animation whose associated effect `effect` is, which it borrows for as long as it is, or null where it is the
/// associated effect of none.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_associated_animation(effect: &AnimationEffectCell) -> *const AnimationCell {
    effect
        .borrow()
        .associated_animation()
        .map_or(std::ptr::null(), |animation| {
            std::ptr::from_ref::<AnimationCell>(&animation)
        })
}

/// The target pseudo-selector of an effect: the host's `PseudoElement`, and the raw fly string of its serialization,
/// or zero where the effect has none.
#[repr(C)]
pub struct FfiEffectPseudoElement {
    pub kind: u8,
    pub serialization: usize,
}

/// The target of an effect: the host's `DOM::Element` it targets, or null where it targets none, and its target
/// pseudo-selector.
#[repr(C)]
pub struct FfiEffectTarget {
    pub element: *mut c_void,
    pub pseudo_element: FfiEffectPseudoElement,
}

/// The target of `effect`, whose pseudo-selector's serialization it borrows until it changes.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_target(effect: &AnimationEffectCell) -> FfiEffectTarget {
    let effect = effect.borrow();
    FfiEffectTarget {
        element: effect
            .target_element()
            .map_or(std::ptr::null_mut(), std::ptr::NonNull::as_ptr),
        pseudo_element: effect.pseudo_element().map_or(
            FfiEffectPseudoElement {
                kind: 0,
                serialization: 0,
            },
            |pseudo_element| FfiEffectPseudoElement {
                kind: pseudo_element.kind,
                serialization: pseudo_element.serialization.raw(),
            },
        ),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_has_flag(effect: &AnimationEffectCell, flag: EffectFlag) -> bool {
    effect.borrow().has_flag(flag)
}

/// Has `effect` forget what the host concluded of it as it was, since it changed: what the host built of it for the
/// compositor, how the host ran or throttled it, and whether a frame can skip its style update.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_forget_what_the_host_concluded(effect: &AnimationEffectCell) {
    effect.borrow_mut().forget_what_the_host_concluded();
}

/// Whether a frame can skip the style update of `effect`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_can_skip_per_frame_style_update(effect: &AnimationEffectCell) -> bool {
    super::throttling::can_skip_per_frame_style_update(effect)
}

/// A new idle CSS animation named `name` with no associated effect.
pub(crate) fn new_css_animation(name: RetainedUtf16FlyString) -> AnimationHandle {
    AnimationHandle::new(AnimationKind::CssAnimation(CssAnimationState {
        name,
        // https://drafts.csswg.org/css-animations-1/#animation-timing-function
        // NB: The initial value of animation-timing-function is ease.
        default_easing: CompactEasing::of(super::easing::keyword_cubic_bezier(0)),
        applied_definition: None,
        named_timeline: RetainedUtf16FlyString::none(),
        name_index: 0,
        last_css_play_state: None,
        flags: 0,
    }))
}

/// What a CSS transition is of and runs between, as the host starts one and reads it: the longhand by its id, or, where
/// `custom_name` is not zero, the custom property of that raw fly string, and the values, which a transition the host
/// reads lends it for as long as it holds the transition.
pub struct TransitionDescription {
    pub generation: u64,
    pub start_time: f64,
    pub end_time: f64,
    pub reversing_shortening_factor: f64,
    pub custom_name: usize,
    pub start_value: *const c_void,
    pub end_value: *const c_void,
    pub reversing_adjusted_start_value: *const c_void,
    pub property_id: u16,
}

/// What the host describes of the animation of an SVG animation element as it creates it.
#[repr(C)]
pub struct FfiSmilAnimation {
    /// The host's `SVGAnimationElement` whose animation it is, which holds the animation until it lets go of it.
    pub animation_element: NonNull<c_void>,
    /// The host's `DOM::Element` the effect of the animation targets.
    pub target: NonNull<c_void>,
    /// The keyframes the effect runs, which the host holds a reference to.
    pub keyframes: *const KeyframeSet,
    pub timing: SpecifiedEffectTiming,
    /// The timing function of the effect and the serialization of the value it was made from, or null for linear.
    pub timing_function: *const FfiEasingDescriptor,
    pub timing_function_serialization: usize,
    /// The time the animation begins at, which orders it among the animations of its time container.
    pub begin_time: f64,
}

/// A new animation of an SVG animation element on the default document timeline of its document, whose associated effect is a new keyframe effect as
/// `description` describes, with the one reference to it the host takes over. The host makes no `Animation` of it, and
/// its animation element holds it until it lets go of it.
///
/// # Safety
/// `description` must name live host objects and keyframes, and a timing function, if any, must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_smil_animation_create(
    description: &FfiSmilAnimation,
    perform: HostEffectPerformer,
) -> *const AnimationCell {
    let effect = EffectHandle::new();
    {
        let mut record = effect.borrow_mut();
        record.set_specified_timing(description.timing);
        // SAFETY: Guaranteed by the caller.
        if let Some(timing_function) = unsafe { description.timing_function.as_ref() } {
            // SAFETY: Guaranteed by the caller.
            let (timing_function, serialization) = unsafe {
                (
                    CompactEasing::of(Easing::from_descriptor(timing_function)),
                    RetainedUtf16FlyString::from_borrowed_raw(description.timing_function_serialization),
                )
            };
            record.set_timing_function(timing_function, serialization);
        }
        record.normalize_specified_timing(None);
        // SAFETY: Guaranteed by the caller.
        record.set_keyframes(Some(unsafe { KeyframeSet::retained_from_host(description.keyframes) }));
    }
    perform_host_effects(
        procedures::set_effect_target(&effect, Some(description.target)),
        perform,
    );

    let animation = AnimationHandle::new(AnimationKind::Smil {
        begin_time: description.begin_time,
        animation_element: description.animation_element,
        holders: SmilHolder::AnimationElement as u8,
    });
    let timeline = default_document_timeline(element_node_document(description.animation_element));
    run_procedure(&animation, perform, |procedures| {
        procedures.set_timeline(Some(timeline));
    });
    run_procedure(&animation, perform, |procedures| {
        procedures.set_the_associated_effect(Some(&effect), Invalidate::Yes);
    });
    animation.into_raw()
}

/// Notes that the SVG animation element of `animation` lets go of it, as it is done with it or as the host finalizes the
/// element.
#[unsafe(no_mangle)]
pub extern "C" fn animations_smil_animation_let_go(animation: &AnimationCell) {
    animation.note_smil_holder(SmilHolder::AnimationElement, false);
}

/// Calls `visit` with `context` and each GC cell of the host that keeps alive the elements the record of `animation`
/// names, which a holder of the record the GC traces visits.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_visit_host_cells(
    animation: &AnimationCell,
    context: *mut c_void,
    visit: unsafe extern "C" fn(context: *mut c_void, cell: *mut c_void, kind: HostCellKind),
) {
    animation.for_each_host_cell(|cell, kind| {
        // SAFETY: The host visits a cell of an animation it holds.
        unsafe { visit(context, cell.as_ptr(), kind) }
    });
}

/// The associated effect of `animation`, which it borrows for as long as the animation holds it, or null where it has
/// none.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_effect(animation: &AnimationCell) -> *const AnimationEffectCell {
    animation.borrow().effect().map_or(std::ptr::null(), |effect| {
        std::ptr::from_ref::<AnimationEffectCell>(effect)
    })
}

/// https://www.w3.org/TR/web-animations-1/#play-states
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_play_state(animation: &AnimationCell) -> AnimationPlayState {
    animation.borrow().play_state()
}

/// https://www.w3.org/TR/web-animations-1/#playback-rate
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_playback_rate(animation: &AnimationCell) -> f64 {
    animation.borrow().playback().playback_rate
}

/// The host's function that keeps `animation`, which it holds a reference to, alive to it with `context` until the call
/// that handed it the function returns, since what it performs for the animation can run the GC.
pub type FfiKeepAlive = unsafe extern "C" fn(context: *mut c_void, animation: &AnimationCell);

/// Has the host keep alive, through `keep_alive` with `context`, every animation `effects` names.
fn keep_alive_for_host_effects(effects: &HostEffects, keep_alive: FfiKeepAlive, context: *mut c_void) {
    let mut previous: Option<&AnimationHandle> = None;
    for (animation, _) in effects.iter() {
        if previous.is_some_and(|previous| previous.ptr_eq(animation)) {
            continue;
        }
        // SAFETY: The host keeps alive an animation it is handed for as long as the call that handed it lasts.
        unsafe { keep_alive(context, animation) };
        previous = Some(animation);
    }
}

/// https://www.w3.org/TR/web-animations-1/#remove-replaced-animations
/// For the host's `DOM::Document` `document`, whose animations are `animations`, comparing elements in tree order with
/// `tree_order`, and has the host keep each animation it removes alive through `keep_alive` with `context` and perform
/// what that asks of it through `perform`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_remove_replaced_animations(
    animations: &DocumentAnimations,
    document: NonNull<c_void>,
    tree_order: FfiTreeOrder,
    keep_alive: FfiKeepAlive,
    context: *mut c_void,
    perform: HostEffectPerformer,
) {
    let timelines = animations.timelines.clone();
    let effects = procedures::remove_replaced_animations(
        timelines.iter().map(|timeline| &**timeline),
        |element| element_node_document(element) == document,
        &mut host_tree_order(tree_order),
    );
    keep_alive_for_host_effects(&effects, keep_alive, context);
    perform_host_effects(effects, perform);
}

/// Keeps the animation of an SVG animation element, on its timeline, if it has one, at the time of its time container,
/// which began at `zero_time` of the document timeline and, where `is_paused`, is paused at `paused_time`, in
/// milliseconds.
#[unsafe(no_mangle)]
pub extern "C" fn animations_smil_animation_synchronize(
    animation: &AnimationCell,
    zero_time: f64,
    is_paused: bool,
    paused_time: f64,
    perform: HostEffectPerformer,
) {
    run_procedure(animation, perform, |procedures| {
        procedures.synchronize_with_smil_time_container(zero_time, is_paused.then_some(paused_time));
    });
}

/// What the render clock's plan makes of `animation`: its play is pending and a tick holds its element still, a tick
/// cannot move it, or a tick moves it.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum FfiClockPlanAnimationKind {
    Held,
    NotTicked,
    Ticked,
}

/// What the render clock's plan makes of an animation, and, where a tick moves it, the local time of its effect and,
/// on a document timeline, the time its active interval ends at.
#[repr(C)]
pub struct FfiClockPlanAnimation {
    pub kind: FfiClockPlanAnimationKind,
    pub local_time: TimeValue,
    pub active_end: TimeValue,
}

/// What the render clock's plan makes of `animation`, of a keyframe effect with a target, on its timeline.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_clock_plan(animation: &AnimationCell) -> FfiClockPlanAnimation {
    let unresolved = TimeValue::milliseconds(0.0);
    let animation = animation.borrow();
    let timeline = animation.timeline().expect("the plan runs on animations of timelines");
    let on_scroll_timeline = timeline.borrow().scroll().is_some();
    let plan = match animation.clock_plan(timeline.current_time(), on_scroll_timeline) {
        // NB: A tick moves an animation of a scroll timeline only where it follows the timeline as the plan seals it.
        ClockPlanAnimation::Ticked { .. } if on_scroll_timeline && timeline.clock_snapshot().is_none() => {
            ClockPlanAnimation::NotTicked
        }
        plan => plan,
    };
    match plan {
        ClockPlanAnimation::Held => FfiClockPlanAnimation {
            kind: FfiClockPlanAnimationKind::Held,
            local_time: unresolved,
            active_end: unresolved,
        },
        ClockPlanAnimation::NotTicked => FfiClockPlanAnimation {
            kind: FfiClockPlanAnimationKind::NotTicked,
            local_time: unresolved,
            active_end: unresolved,
        },
        ClockPlanAnimation::Ticked { local_time, active_end } => FfiClockPlanAnimation {
            kind: FfiClockPlanAnimationKind::Ticked,
            local_time,
            active_end,
        },
    }
}

/// The timing a hover of the render clock's lane runs `animation`, of a keyframe effect, with, on its timeline.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_hover_timing(
    animation: &AnimationCell,
) -> super::snapshot::EffectTimingSnapshot {
    let animation = animation.borrow();
    let timeline = animation.timeline().expect("the hover runs animations of timelines");
    animation.hover_timing(timeline.sample(), true, timeline.clock())
}

/// The owning element of a CSS animation or transition: the host's `DOM::Element`, or null where it has none, and the
/// host's `PseudoElement`, where `has_pseudo_element`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiOwningElement {
    pub element: *mut c_void,
    pub pseudo_element: u8,
    pub has_pseudo_element: bool,
}

/// https://www.w3.org/TR/css-animations-2/#owning-element-section
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_owning_element(animation: &AnimationCell) -> FfiOwningElement {
    match animation.borrow().owning_element() {
        Some(owner) => FfiOwningElement {
            element: owner.element.as_ptr(),
            pseudo_element: owner.pseudo_element.unwrap_or(0),
            has_pseudo_element: owner.pseudo_element.is_some(),
        },
        None => FfiOwningElement {
            element: std::ptr::null_mut(),
            pseudo_element: 0,
            has_pseudo_element: false,
        },
    }
}

/// Applies the timing, attachment range, easing, composite operation and play state the style computation computed in
/// `definition` to the CSS animation `animation`, whose timeline is updated, having the host perform what that asks for
/// through `perform`.
/// https://drafts.csswg.org/css-animations-2/#animations
///
/// # Safety
/// The values `definition` names must be null or live style values.
unsafe fn apply_css_animation_properties(
    animation: &AnimationCell,
    definition: &crate::css::style_compute::FfiComputedAnimation,
    perform: HostEffectPerformer,
) {
    use crate::css::css_enums::{animation_composition, animation_direction, animation_fill_mode};
    use libcompositing_rust::animation_timing::effect_timing::{
        FillMode as SpecifiedFillMode, PlaybackDirection, SpecifiedIterationDuration,
    };

    let has_flag = |flag: CssAnimationFlag| animation.borrow().css_animation().has_flag(flag);

    // NB: An edge of the range that script set is no longer the style's to change.
    let [current_start, current_end] = animation.borrow().range();
    let edge = |name, px, percentage, calculated| AttachmentRangeEdge {
        name,
        offset: AttachmentRangeEdgeOffset {
            px,
            percentage,
            // SAFETY: Guaranteed by the caller.
            calculated: unsafe { retained_optional_value(calculated) },
        },
    };
    let start = if has_flag(CssAnimationFlag::ScriptSetRangeStart) {
        current_start
    } else {
        edge(
            definition.range_start_name,
            definition.range_start_px,
            definition.range_start_percentage,
            definition.range_start_calculated,
        )
    };
    let end = if has_flag(CssAnimationFlag::ScriptSetRangeEnd) {
        current_end
    } else {
        edge(
            definition.range_end_name,
            definition.range_end_px,
            definition.range_end_percentage,
            definition.range_end_calculated,
        )
    };
    if animation.borrow_mut().set_range([start, end]) {
        run_procedure(animation, perform, |procedures| procedures.attachment_range_changed());
    }

    let effect = animation
        .borrow()
        .effect()
        .cloned()
        .expect("a CSS animation the style applies to has an associated effect");
    let mut specified = *effect.borrow().specified_timing();
    specified.iteration_duration = if definition.duration_is_auto {
        SpecifiedIterationDuration::AUTO
    } else {
        SpecifiedIterationDuration::milliseconds(definition.duration)
    };
    specified.start_delay = definition.delay;
    specified.iteration_count = definition.iteration_count;
    specified.fill_mode = match definition.fill_mode {
        animation_fill_mode::BACKWARDS => SpecifiedFillMode::Backwards,
        animation_fill_mode::BOTH => SpecifiedFillMode::Both,
        animation_fill_mode::FORWARDS => SpecifiedFillMode::Forwards,
        animation_fill_mode::NONE => SpecifiedFillMode::None,
        fill_mode => unreachable!("animation-fill-mode {fill_mode} is a keyword of the property"),
    };
    specified.playback_direction = match definition.direction {
        animation_direction::ALTERNATE => PlaybackDirection::Alternate,
        animation_direction::ALTERNATE_REVERSE => PlaybackDirection::AlternateReverse,
        animation_direction::NORMAL => PlaybackDirection::Normal,
        animation_direction::REVERSE => PlaybackDirection::Reverse,
        direction => unreachable!("animation-direction {direction} is a keyword of the property"),
    };
    effect.borrow_mut().set_specified_timing(specified);
    // https://drafts.csswg.org/web-animations-2/#updating-animationeffect-timing
    // Timing properties may also be updated due to a style change. Any change to a CSS animation property that affects
    // timing requires rerunning the procedure to normalize specified timing.
    let timeline = animation.borrow().timeline_state();
    effect.borrow_mut().normalize_specified_timing(timeline.as_ref());

    // NB: animation-timing-function is applied per-keyframe, not as the effect-level timing function. The effect-level
    //     timing function remains linear. The effect's description for the style engine gives it to every keyframe that
    //     declares no easing of its own.
    // SAFETY: Guaranteed by the caller.
    let timing_function = unsafe { definition.timing_function.cast::<StyleValueData>().as_ref() }
        .expect("a computed animation has a timing function");
    let default_easing = crate::css::style::effect_descriptions::easing_from_computed_timing_function(timing_function)
        .expect("a computed animation-timing-function describes an easing");
    animation
        .borrow_mut()
        .set_css_animation_default_easing(CompactEasing::of(default_easing));
    effect.borrow_mut().set_composite(match definition.composition {
        animation_composition::ACCUMULATE => FfiCompositeOperation::Accumulate,
        animation_composition::ADD => FfiCompositeOperation::Add,
        animation_composition::REPLACE => FfiCompositeOperation::Replace,
        composition => unreachable!("animation-composition {composition} is a keyword of the property"),
    });

    // NB: The style changed the timing of the associated effect, and its composite operation, which invalidates the
    //     style it produces.
    run_procedure(animation, perform, |procedures| {
        procedures.update_finished_state(DidSeek::No, SynchronouslyNotify::No, Invalidate::Yes, None);
    });

    if animation.borrow().css_animation().last_css_play_state == Some(definition.play_state) {
        return;
    }
    if !has_flag(CssAnimationFlag::ScriptOverrodePlayState) {
        let play_state = animation.borrow().play_state();
        // NB: Neither procedure throws for a CSS animation, which has an associated effect.
        let result = match definition.play_state {
            CSS_ANIMATION_PLAY_STATE_RUNNING if play_state != AnimationPlayState::Running => {
                run_procedure_on(animation, perform, |procedures| procedures.play_from_css())
            }
            CSS_ANIMATION_PLAY_STATE_PAUSED if play_state != AnimationPlayState::Paused => {
                run_procedure_on(animation, perform, |procedures| procedures.pause_from_css())
            }
            _ => Ok(()),
        };
        assert!(result.is_ok(), "playing or pausing a CSS animation throws nothing");
    }
    animation.borrow_mut().css_animation_mut().last_css_play_state = Some(definition.play_state);
}

/// What applying the definitions of the CSS animations of an element or pseudo-element asks of the host, which hands
/// each function `context`.
#[repr(C)]
pub struct FfiCssAnimationHost {
    pub context: *mut c_void,
    /// https://drafts.csswg.org/scroll-animations-1/#target-timeline
    /// The target timeline of the owner of the animations for the timeline name `name`, a raw fly string, which the
    /// host holds a reference to until the call returns, or null for none.
    pub named_timeline: unsafe extern "C" fn(context: *mut c_void, name: usize) -> Option<&'static TimelineCell>,
    /// Invalidates the composite order of the animations associated with the host's `DOM::Element` `element`.
    pub invalidate_composite_order: unsafe extern "C" fn(context: *mut c_void, element: NonNull<c_void>),
    pub keep_alive: FfiKeepAlive,
    pub in_execution_context: FfiInExecutionContext,
    pub perform: HostEffectPerformer,
}

impl FfiCssAnimationHost {
    /// Has the host invalidate the style the associated effect of `animation` produces.
    fn invalidate_effect(&self, animation: &AnimationHandle) {
        let mut effects = HostEffects::default();
        effects.push(animation, HostEffectKind::InvalidateEffect { changes_plan: false });
        perform_host_effects(effects, self.perform);
    }

    /// Runs `run` in the execution context the host opens for `animation`.
    fn in_execution_context(&self, animation: &AnimationCell, run: impl FnOnce()) {
        in_host_execution_context(self.in_execution_context, self.context, animation, run);
    }
}

/// Sets the keyframes of `effect` to `keyframes` of CSS, answering whether that changed them. An effect whose keyframes
/// from CSS script replaced takes none.
///
/// # Safety
/// `keyframes` must be null or a keyframe set the host holds a reference to.
unsafe fn set_effect_keyframes(effect: &EffectHandle, keyframes: *const c_void) -> bool {
    if effect.borrow().ignores_css_keyframes() {
        return false;
    }
    let keyframes = keyframes.cast::<KeyframeSet>();
    let current = effect.borrow().keyframes().map_or(std::ptr::null(), Arc::as_ptr);
    if std::ptr::eq(current, keyframes) {
        return false;
    }
    // SAFETY: Guaranteed by the caller.
    let keyframes = (!keyframes.is_null()).then(|| unsafe { KeyframeSet::retained_from_host(keyframes) });
    effect.borrow_mut().set_keyframes(keyframes);
    effect.borrow_mut().forget_what_the_host_concluded();
    true
}

/// The timeline the host finds of the name `definition` names for `owner`, through `host`.
///
/// # Safety
/// The definition must name a live CSS string.
unsafe fn named_timeline_of(
    definition: &crate::css::style_compute::FfiComputedAnimation,
    host: &FfiCssAnimationHost,
) -> Option<TimelineHandle> {
    // SAFETY: Guaranteed by the caller.
    let name = unsafe { definition.timeline_name.as_ref() }
        .map_or_else(RetainedUtf16FlyString::none, |name| name.to_fly_string());
    // NB: The timeline is retained before anything can run the GC, and the animation then keeps it alive.
    // SAFETY: The host hands out a timeline it holds a reference to, or null.
    unsafe { (host.named_timeline)(host.context, name.raw()) }
        .map(|timeline| unsafe { TimelineHandle::retained_from_host(timeline) })
}

/// The anonymous scroll timeline source `definition` describes for `owner`.
fn anonymous_scroll_source(
    definition: &crate::css::style_compute::FfiComputedAnimation,
    owner: OwningElement,
) -> ScrollTimelineSource {
    ScrollTimelineSource::Anonymous {
        scroller: definition.scroll_scroller,
        target: owner.element,
        pseudo_element: owner.pseudo_element,
    }
}

/// The inset of the view() timeline `definition` describes.
///
/// # Safety
/// The calculations of the inset must be null or live style values.
pub(crate) unsafe fn retained_view_inset(inset: &crate::css::style_compute::FfiViewTimelineInset) -> ViewTimelineInset {
    let edge = |is_auto, px, percentage, calculated| ViewTimelineInsetEdge {
        is_auto,
        px,
        percentage,
        // SAFETY: Guaranteed by the caller.
        calculated: unsafe { retained_optional_value(calculated) },
    };
    ViewTimelineInset {
        start: edge(
            inset.start_is_auto,
            inset.start_px,
            inset.start_percentage,
            inset.start_calculated,
        ),
        end: edge(
            inset.end_is_auto,
            inset.end_px,
            inset.end_percentage,
            inset.end_calculated,
        ),
    }
}

/// Whether the CSS animation `animation`, owned by `owner`, takes the timeline `definition` describes in place of the
/// one it has.
/// NB: Unrelated style changes shouldn't cause us to recreate anonymous timelines, to achieve this we drop updates
///     between two equivalent anonymous timelines. The comparison is made against the description of the requested
///     timeline, so that the timeline a definition names is only ever created when it will replace the one the
///     animation already has.
///
/// # Safety
/// The values `definition` names must be null or live.
unsafe fn should_update_timeline(
    animation: &AnimationHandle,
    definition: &crate::css::style_compute::FfiComputedAnimation,
    owner: OwningElement,
    host: &FfiCssAnimationHost,
) -> bool {
    use crate::css::style_compute::FfiAnimationTimelineKind;
    let old_timeline = animation.borrow().timeline().cloned();
    let same = |timeline: Option<TimelineHandle>| match (&old_timeline, &timeline) {
        (Some(old), Some(new)) => old.ptr_eq(new),
        (None, None) => true,
        _ => false,
    };
    match definition.timeline_kind {
        FfiAnimationTimelineKind::None => return old_timeline.is_some(),
        FfiAnimationTimelineKind::Document => {
            return !same(Some(default_document_timeline(element_node_document(owner.element))));
        }
        // SAFETY: Guaranteed by the caller.
        FfiAnimationTimelineKind::Named => return !same(unsafe { named_timeline_of(definition, host) }),
        FfiAnimationTimelineKind::View | FfiAnimationTimelineKind::Scroll => {}
    }
    let Some(old_timeline) = old_timeline else {
        return true;
    };
    let record = old_timeline.borrow();
    let Some(scroll) = record.scroll() else {
        return true;
    };
    if scroll.axis != definition.scroll_axis {
        return true;
    }
    if definition.timeline_kind == FfiAnimationTimelineKind::View {
        let Some(view) = &scroll.view else {
            return true;
        };
        let ScrollTimelineSource::Anonymous {
            target, pseudo_element, ..
        } = scroll.source
        else {
            return true;
        };
        // SAFETY: Guaranteed by the caller.
        return target != owner.element
            || pseudo_element != owner.pseudo_element
            || !view
                .inset
                .equals(&unsafe { retained_view_inset(&definition.view_inset) });
    }
    if scroll.view.is_some() {
        return true;
    }
    match (scroll.source, anonymous_scroll_source(definition, owner)) {
        (
            ScrollTimelineSource::Anonymous {
                scroller,
                target,
                pseudo_element,
            },
            ScrollTimelineSource::Anonymous {
                scroller: new_scroller,
                target: new_target,
                pseudo_element: new_pseudo_element,
            },
        ) => scroller != new_scroller || target != new_target || pseudo_element != new_pseudo_element,
        _ => true,
    }
}

/// The timeline `definition` describes for the CSS animation owned by `owner`, a new anonymous one for scroll() and
/// view(), or none.
///
/// # Safety
/// The values `definition` names must be null or live.
unsafe fn materialize_timeline(
    definition: &crate::css::style_compute::FfiComputedAnimation,
    owner: OwningElement,
    host: &FfiCssAnimationHost,
) -> Option<TimelineHandle> {
    use crate::css::style_compute::FfiAnimationTimelineKind;
    let document = element_node_document(owner.element);
    let scroll = |source, view| {
        TimelineKind::Scroll(Box::new(ScrollTimelineState {
            source,
            axis: definition.scroll_axis,
            last_max_scroll_offset: None,
            followed_scroller: None,
            view,
        }))
    };
    match definition.timeline_kind {
        FfiAnimationTimelineKind::Document => Some(default_document_timeline(document)),
        FfiAnimationTimelineKind::None => None,
        FfiAnimationTimelineKind::Scroll => Some(create_timeline(
            document,
            scroll(anonymous_scroll_source(definition, owner), None),
        )),
        FfiAnimationTimelineKind::View => Some(create_timeline(
            document,
            scroll(
                ScrollTimelineSource::Anonymous {
                    scroller: SCROLLER_NEAREST,
                    target: owner.element,
                    pseudo_element: owner.pseudo_element,
                },
                Some(ViewTimelineState {
                    // SAFETY: Guaranteed by the caller.
                    inset: unsafe { retained_view_inset(&definition.view_inset) },
                    last_metrics: None,
                    subject_style_node: None,
                }),
            ),
        )),
        // SAFETY: Guaranteed by the caller.
        FfiAnimationTimelineKind::Named => unsafe { named_timeline_of(definition, host) },
    }
}

/// The host's `CSS::Scroller::Nearest`, which a view timeline finds its source from its subject with.
pub(crate) const SCROLLER_NEAREST: u8 = 1;

/// Applies `definition` to the CSS animation `animation`, whose associated effect runs the keyframes the caller gave it.
///
/// # Safety
/// The values `definition` names must be null or live style values.
unsafe fn apply_css_animation_definition(
    animation: &AnimationHandle,
    definition: &crate::css::style_compute::FfiComputedAnimation,
    owner: OwningElement,
    host: &FfiCssAnimationHost,
) {
    use crate::css::style_compute::FfiAnimationTimelineKind;

    // FIXME: Don't apply overridden properties as defined here: https://drafts.csswg.org/css-animations-2/#animations

    // NB: Recorded before the early return below, so that the definition always says what the style computation last
    //     computed.
    // NB: An animation that names its timeline looks it up again from its owning element as the timelines its document
    //     declares change, also while the name finds no timeline. The record notes the name it follows.
    let named_timeline =
        if definition.timeline_kind == FfiAnimationTimelineKind::Named && !definition.timeline_name.is_null() {
            // SAFETY: The style computation names a live CSS string.
            unsafe { &*definition.timeline_name }.to_fly_string()
        } else {
            RetainedUtf16FlyString::none()
        };
    // SAFETY: Guaranteed by the caller.
    let applies_the_same_properties = unsafe { note_css_animation_definition(animation, definition, named_timeline) };

    let script_set_timeline = animation
        .borrow()
        .css_animation()
        .has_flag(CssAnimationFlag::ScriptSetTimeline);
    // NB: An animation that names its timeline follows it as the declarations of its document change.
    if definition.timeline_kind == FfiAnimationTimelineKind::Named {
        let document = element_node_document(owner.element);
        // SAFETY: The document of the owner of an animation is live, and the borrow ends here.
        unsafe { super::timeline_host::document_animations(document) }
            .set_mut(DocumentAnimationSetKind::FollowingNamedTimelines)
            .insert(animation);
    }
    // SAFETY: The definition lives for the call.
    let update_timeline = !script_set_timeline && unsafe { should_update_timeline(animation, definition, owner, host) };

    // NB: Style feedback can recompute an animation's owning element. Reapplying unchanged properties would invalidate
    //     the effect and turn that feedback into an endless cycle.
    if !update_timeline && applies_the_same_properties {
        return;
    }

    host.in_execution_context(animation, || {
        if update_timeline {
            // SAFETY: The definition lives for the call.
            let timeline = unsafe { materialize_timeline(definition, owner, host) };
            run_procedure(animation, host.perform, |procedures| procedures.set_timeline(timeline));
        }
        // SAFETY: Guaranteed by the caller.
        unsafe { apply_css_animation_properties(animation, definition, host.perform) };
    });
}

/// https://drafts.csswg.org/css-animations-1/#animations
/// Applies the `count` animation definitions `definitions` the style computation computed for the owner at `slot` of
/// the element whose lists `animations` are, `owner`, which is in a display: none subtree where
/// `in_display_none_subtree`: claims the CSS animation of each definition that claims one, creates one for each other
/// definition, cancels each animation no definition claims, and sets the CSS animations of the owner to those of the
/// definitions, publishing them to the style engine of `style_engine`, which knows the element as `node`, where the
/// host names one. `pseudo_element_serialization` is the raw fly string of the serialization of the pseudo-element of
/// `owner`, or zero where it has none.
///
/// # Safety
/// `animations` must be the lists of `owner`, which live for the call, and which the host may change as it performs;
/// `definitions` must point to `count` definitions whose values are live and whose keyframe sets are null or ones the
/// host holds references to; a non-null `style_engine` must be live for the call; and `pseudo_element_serialization`
/// must be zero or a live raw fly string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_element_animations_apply_css_animation_definitions(
    animations: *mut ElementAnimations,
    slot: AnimationOwnerSlot,
    owner: FfiOwningElement,
    pseudo_element_serialization: usize,
    definitions: *const crate::css::style_compute::FfiComputedAnimation,
    count: usize,
    in_display_none_subtree: bool,
    style_engine: *const DocumentHost,
    node: u32,
    host: &FfiCssAnimationHost,
) {
    let owner_element = NonNull::new(owner.element).expect("CSS animations have an owning element");
    // SAFETY: Guaranteed by the caller.
    let definitions = unsafe { ak::slice_from_raw_parts(definitions, count) };
    // NB: What the host performs can change the lists, so no borrow of them outlives a read.
    // SAFETY: Guaranteed by the caller.
    let existing_animations: SmallVec<[AnimationHandle; 1]> =
        unsafe { &*animations }.css_animations(slot).iter().cloned().collect();
    for animation in &existing_animations {
        // SAFETY: The host keeps alive an animation it is handed for as long as this call lasts.
        unsafe { (host.keep_alive)(host.context, animation) };
    }
    let mut existing_animation_was_claimed: SmallVec<[bool; 1]> = smallvec::smallvec![false; existing_animations.len()];
    let mut new_animations: SmallVec<[AnimationHandle; 1]> = SmallVec::with_capacity(definitions.len());

    // NB: Which existing animation each definition claims is decided by the style computation, from the names of the
    //     animations this element holds, which it publishes. So are the keyframes each definition runs, from the
    //     `@keyframes` every style scope publishes.
    let owning_element = OwningElement {
        element: owner_element,
        pseudo_element: owner.has_pseudo_element.then_some(owner.pseudo_element),
    };
    for (name_index, definition) in definitions.iter().enumerate().rev() {
        let name_index = u32::try_from(name_index).expect("animation-name has fewer than 2^32 entries");
        let animation = if let Ok(matched_index) = usize::try_from(definition.matched_existing_index) {
            assert!(
                !existing_animation_was_claimed[matched_index],
                "a definition claims an animation only once"
            );
            existing_animation_was_claimed[matched_index] = true;
            let animation = existing_animations[matched_index].clone();
            let effect = animation.borrow().effect().cloned();
            if let Some(effect) = effect {
                // SAFETY: Guaranteed by the caller.
                if unsafe { set_effect_keyframes(&effect, definition.keyframe_set) } {
                    host.invalidate_effect(&animation);
                }
                // SAFETY: Guaranteed by the caller.
                unsafe { apply_css_animation_definition(&animation, definition, owning_element, host) };
            }
            if set_css_animation_name_index(&animation, name_index) {
                let target = animation
                    .borrow()
                    .effect()
                    .and_then(|effect| effect.borrow().target_element());
                if let Some(target) = target {
                    // SAFETY: The effect keeps its target alive.
                    unsafe { (host.invalidate_composite_order)(host.context, target) };
                }
            }
            animation
        } else {
            if in_display_none_subtree {
                continue;
            }
            // An animation applies to an element if its name appears as one of the identifiers in the computed value
            // of the animation-name property and the animation uses a valid @keyframes rule
            // FIXME:
            // CSS Animations generated using the markup defined in this specification are not added to the global
            // animation list when they are created. Instead, these animations are appended to the global animation
            // list at the first moment when they transition out of the idle play state after being disassociated from
            // their owning element. CSS Animations that have been disassociated from their owning element but are
            // still idle do not have a defined composite order.
            // NB: Script gets the object of the animation, a CSSAnimation, as it first asks for it.
            // SAFETY: The style computation names a live CSS string.
            let name = unsafe { definition.name.as_ref() }
                .map_or_else(RetainedUtf16FlyString::none, |name| name.to_fly_string());
            let animation = new_css_animation(name);
            // SAFETY: The host keeps alive an animation it is handed for as long as this call lasts.
            unsafe { (host.keep_alive)(host.context, &animation) };
            animation.borrow_mut().set_owning_element(Some(owning_element));
            let effect = EffectHandle::new();
            // SAFETY: Guaranteed by the caller.
            unsafe { set_effect_keyframes(&effect, definition.keyframe_set) };
            run_procedure(&animation, host.perform, |procedures| {
                procedures.set_the_associated_effect(Some(&effect), Invalidate::Yes);
            });
            // SAFETY: Guaranteed by the caller.
            unsafe { apply_css_animation_definition(&animation, definition, owning_element, host) };
            set_css_animation_name_index(&animation, name_index);
            // NB: The effect targets the owning pseudo-element, if any, before it is associated with its target.
            // SAFETY: Guaranteed by the caller.
            let serialization = unsafe { RetainedUtf16FlyString::from_borrowed_raw(pseudo_element_serialization) };
            effect
                .borrow_mut()
                .set_pseudo_element(owner.pseudo_element, serialization);
            // NB: The keyframes of a CSS animation read the attributes their rule read, which the style engine noted
            //     as the rule was published.
            perform_host_effects(
                procedures::set_effect_target(&effect, Some(owner_element)),
                host.perform,
            );
            host.invalidate_effect(&animation);
            animation
        };
        new_animations.push(animation);
    }

    // Once an animation has started it continues until it ends or the animation-name is removed
    // NB: An animation no definition claimed is one whose animation-name entry has gone. Canceling it from the style
    //     computation needs no invalidation, as the effect stack is computed after this.
    for (animation, _) in existing_animations
        .iter()
        .zip(&existing_animation_was_claimed)
        .filter(|(_, claimed)| !**claimed)
    {
        cancel_animation(
            animation,
            Invalidate::No,
            host.in_execution_context,
            host.context,
            host.perform,
        );
    }

    // NB: The animations were created in reverse definition order, so flip it back.
    new_animations.reverse();
    // SAFETY: Guaranteed by the caller.
    unsafe { &mut *animations }.set_css_animations(slot, new_animations.into_iter(), unsafe {
        style_engine_target(style_engine, node)
    });
}

/// What preparing to observe the CSS animation events of a document asks of the host, which hands each function
/// `context`.
#[repr(C)]
pub struct FfiObserveCssAnimationEventsHost {
    pub context: *mut c_void,
    pub keep_alive: FfiKeepAlive,
    pub perform: HostEffectPerformer,
}

/// Has the CSS animations of a document whose animations are `animations`, whose effects skipped per-frame animation
/// ticks, take their current phase and iteration as those they last dispatched events for, as the host updates the
/// timelines of the document to now first, keeping alive and performing through `host`.
///
/// # Safety
/// `animations` must be what a live document knows of animations, which the host may reach as it performs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_prepare_to_observe_css_animation_events(
    animations: *mut DocumentAnimations,
    host: &FfiObserveCssAnimationEventsHost,
) {
    // SAFETY: Guaranteed by the caller, and the borrow ends before the host performs anything.
    let effects_to_synchronize: Vec<(AnimationHandle, EffectHandle)> = unsafe { &mut *animations }
        .set_mut(DocumentAnimationSetKind::Associated)
        .iter()
        .filter_map(|animation| {
            let effect = {
                let record = animation.borrow();
                if record.ffi_kind() != FfiAnimationKind::CssAnimation {
                    return None;
                }
                record.effect()?.clone()
            };
            let skipped = effect.borrow().has_flag(EffectFlag::PerFrameAnimationTickWasSkipped);
            skipped.then_some((animation, effect))
        })
        .collect();
    if effects_to_synchronize.is_empty() {
        return;
    }
    for (animation, _) in &effects_to_synchronize {
        // SAFETY: The host keeps alive an animation it is handed for as long as this call lasts.
        unsafe { (host.keep_alive)(host.context, animation) };
    }
    // NB: The timelines of the document take their current time as of now first.
    super::timeline_host::update_timelines_to_now(
        animations,
        &super::timeline_host::keep_alive_with(host.keep_alive, host.context),
        host.perform,
    );

    // OPTIMIZATION: Events which occurred while there were no listeners were intentionally not sampled. Establish
    //               the current phase as the baseline so a newly added listener only observes future events.
    for (_, effect) in &effects_to_synchronize {
        let animation = effect.borrow().associated_animation();
        let local_time = animation
            .as_ref()
            .and_then(|animation| animation.borrow().current_time());
        let fill_mode = effect.borrow().timing().fill_mode;
        let resolved = resolve_effect_timing(effect, local_time, fill_mode, animation.as_deref(), false);
        let mut record = effect.borrow_mut();
        record.previous_phase = resolved.phase;
        record.previous_current_iteration = resolved.current_iteration.unwrap_or(0.0);
        record.set_flag(EffectFlag::PerFrameAnimationTickWasSkipped, false);
    }
}

/// What looking up the named timelines of the CSS animations of a document again asks of the host, which hands each
/// function `context`.
#[repr(C)]
pub struct FfiNamedTimelinesHost {
    pub context: *mut c_void,
    /// The timeline the host finds of the name `name`, a raw fly string, for the element `owner`, or null.
    pub target_timeline: unsafe extern "C" fn(
        context: *mut c_void,
        owner: FfiOwningElement,
        name: usize,
    ) -> Option<&'static TimelineCell>,
    pub keep_alive: FfiKeepAlive,
    pub in_execution_context: FfiInExecutionContext,
    pub perform: HostEffectPerformer,
}

/// Looks the named timeline each CSS animation of a document whose animations are `animations` follows up again from
/// its owning element, through `host`, and sets it as the animation's timeline where it changed, answering whether that
/// changed the timeline of any.
///
/// # Safety
/// `animations` must be what a live document knows of animations, which the host may reach as it performs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_update_named_timelines(
    animations: *mut DocumentAnimations,
    host: &FfiNamedTimelinesHost,
) -> bool {
    // SAFETY: Guaranteed by the caller, and the borrow ends before the host performs anything.
    let following = unsafe { &mut *animations }
        .set_mut(DocumentAnimationSetKind::FollowingNamedTimelines)
        .alive();
    for animation in &following {
        // SAFETY: The host keeps alive an animation it is handed for as long as this call lasts.
        unsafe { (host.keep_alive)(host.context, animation) };
    }
    let mut any_timeline_changed = false;
    for animation in &following {
        if update_named_timeline(animation, host) {
            any_timeline_changed = true;
        }
    }
    any_timeline_changed
}

/// Looks the named timeline the CSS animation `animation` follows up again from its owning element through `host`, and
/// sets it as the animation's timeline, answering whether that changed its timeline.
fn update_named_timeline(animation: &AnimationCell, host: &FfiNamedTimelinesHost) -> bool {
    let (named_timeline, owner) = {
        let record = animation.borrow();
        let state = record.css_animation();
        if state.named_timeline.raw() == 0 || state.has_flag(CssAnimationFlag::ScriptSetTimeline) {
            return false;
        }
        // NB: The owning element of a CSS animation is the element whose style names the timeline, which the animation
        //     keeps alive.
        let owner = record
            .owning_element()
            .expect("a CSS animation that follows a named timeline has an owning element");
        (state.named_timeline.clone(), owner)
    };
    let owner = FfiOwningElement {
        element: owner.element.as_ptr(),
        pseudo_element: owner.pseudo_element.unwrap_or(0),
        has_pseudo_element: owner.pseudo_element.is_some(),
    };
    // SAFETY: The host finds the timeline for a live element and a live raw fly string, which it holds a reference to.
    let timeline = unsafe { (host.target_timeline)(host.context, owner, named_timeline.raw()) };
    let current = animation
        .borrow()
        .timeline()
        .map(|timeline| std::ptr::from_ref::<TimelineCell>(timeline));
    if current == timeline.map(std::ptr::from_ref) {
        return false;
    }
    // SAFETY: As above.
    let timeline = timeline.map(|timeline| unsafe { TimelineHandle::retained_from_host(timeline) });
    in_host_execution_context(host.in_execution_context, host.context, animation, || {
        run_procedure(animation, host.perform, |procedures| procedures.set_timeline(timeline));
    });
    true
}

/// Sets the position of the CSS animation `animation` in `animation-name`, answering whether that changed it.
fn set_css_animation_name_index(animation: &AnimationCell, name_index: u32) -> bool {
    std::mem::replace(&mut animation.borrow_mut().css_animation_mut().name_index, name_index) != name_index
}

/// Notes that the style computation applied `definition` to the CSS animation `animation`, which follows the named
/// timeline `named_timeline` where that is not none, answering whether that sets the same properties as the definition
/// it last applied, whatever keyframes each gives the animation's effect.
///
/// # Safety
/// The values `definition` names must be null or live style values, and its keyframe set null or one the host holds a
/// reference to.
unsafe fn note_css_animation_definition(
    animation: &AnimationCell,
    definition: &crate::css::style_compute::FfiComputedAnimation,
    named_timeline: RetainedUtf16FlyString,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let definition = unsafe { Arc::new(AppliedAnimationDefinition::retained(definition)) };
    let mut animation = animation.borrow_mut();
    let state = animation.css_animation_mut();
    state.named_timeline = named_timeline;
    let applies_the_same_properties = state
        .applied_definition
        .as_ref()
        .is_some_and(|applied| applied.applies_the_same_properties(&definition));
    state.applied_definition = Some(definition);
    // NB: The effect is described anew under a definition that sets other properties, so that the interval values the
    //     style engine retained for its custom properties under the last one are let go of.
    if !applies_the_same_properties && let Some(effect) = animation.effect() {
        effect.borrow_mut().describe_anew();
    }
    applies_the_same_properties
}

/// What a CSS transition starts with: its owning element, which its effect targets, the raw fly string of the
/// serialization of the owner's pseudo-element, or zero where it has none, the transition, whose start and end times
/// include its delay, the timing its effect runs with: its delay and duration, in milliseconds, and the computed value
/// of its timing function, and the effective zoom of the owner its values were computed at.
pub struct TransitionStart {
    pub owner: FfiOwningElement,
    pub pseudo_element_serialization: usize,
    pub transition: TransitionDescription,
    pub delay: f64,
    pub duration: f64,
    pub timing_function: *const c_void,
    pub computed_value_zoom: f32,
}

/// The easing of the computed timing function `value`, with its serialization, as `EffectTiming.easing` reads it.
fn timing_function_and_serialization(value: &StyleValueData) -> (CompactEasing, RetainedUtf16FlyString) {
    use crate::css::serialize::{SerializationMode, TextSink, serialize_style_value};
    use libcompositing_rust::easing::FfiEasingKind;
    let easing = crate::css::style::effect_descriptions::easing_from_computed_timing_function(value)
        .expect("a computed transition-timing-function describes an easing");
    let mode = if matches!(value, StyleValueData::Easing(_)) && easing.kind() == FfiEasingKind::CubicBezier {
        SerializationMode::Normal
    } else {
        SerializationMode::ResolvedValue
    };
    let mut sink = TextSink::new();
    serialize_style_value(&mut sink, value, mode);
    (
        CompactEasing::of(easing),
        RetainedUtf16FlyString::from_utf16(&sink.into_utf16()),
    )
}

/// https://drafts.csswg.org/css-transitions/#starting
/// Starts the CSS transition `start` describes on `timeline` provisionally, having the host perform what that asks for
/// through `perform`. It is associated with its target only once committed.
///
/// # Safety
/// `start` must name live host objects, style values and raw fly strings.
unsafe fn start_a_provisional_transition(
    start: &TransitionStart,
    timeline: Option<TimelineHandle>,
    perform: HostEffectPerformer,
) -> AnimationHandle {
    let description = &start.transition;
    // SAFETY: Guaranteed by the caller.
    let state = unsafe {
        CssTransitionState {
            custom_property_name: RetainedUtf16FlyString::from_borrowed_raw(description.custom_name),
            generation: description.generation,
            start_time: description.start_time,
            end_time: description.end_time,
            start_value: retained_value(description.start_value),
            end_value: retained_value(description.end_value),
            reversing_adjusted_start_value: retained_value(description.reversing_adjusted_start_value),
            reversing_shortening_factor: description.reversing_shortening_factor,
            property_id: description.property_id,
            previous_phase: TransitionPhase::Idle,
            provisional: true,
            adopted_from_lane: false,
            effect_started_with: None,
        }
    };
    // NB: Script gets the object of the transition, a CSSTransition, as it first asks for it.
    let transition = AnimationHandle::new(AnimationKind::CssTransition(state));

    // FIXME:
    // Transitions generated using the markup defined in this specification are not added to the global animation list
    // when they are created. Instead, these animations are appended to the global animation list at the first moment
    // when they transition out of the idle play state after being disassociated from their owning element. Transitions
    // that have been disassociated from their owning element but are still idle do not have a defined composite order.

    // NB: The transition's associated effect is a keyframe effect. The current style computation collects this effect
    //     before publishing its result, so scheduling a second animated style update here would evaluate the same
    //     transition twice. The record keeps the effect the transition is started with. The values of a transition are
    //     computed values, which read no attributes.
    let owner = NonNull::new(start.owner.element).expect("a transition has an owning element");
    let effect = EffectHandle::new();
    perform_host_effects(procedures::set_effect_target(&effect, Some(owner)), perform);
    // SAFETY: Guaranteed by the caller.
    unsafe {
        let mut effect = effect.borrow_mut();
        effect.set_pseudo_element(
            start.owner.pseudo_element,
            RetainedUtf16FlyString::from_borrowed_raw(start.pseudo_element_serialization),
        );
        effect.set_keyframes(Some(Arc::new(KeyframeSet::for_transition(
            keyframe_property(description.property_id, description.custom_name),
            retained_value(description.start_value),
            retained_value(description.end_value),
            start.computed_value_zoom,
        ))));
    }
    transition.borrow_mut().set_owning_element(Some(OwningElement {
        element: owner,
        pseudo_element: start.owner.has_pseudo_element.then_some(start.owner.pseudo_element),
    }));

    // NB: The record sets the timing of the effect, sets it as the transition's associated effect and plays the
    //     transition, which creates its ready promise.
    // SAFETY: Guaranteed by the caller.
    let timing_function =
        unsafe { start.timing_function.cast::<StyleValueData>().as_ref() }.expect("a transition has a timing function");
    let (timing_function, timing_function_serialization) = timing_function_and_serialization(timing_function);
    let timing = ProvisionalTransitionTiming {
        delay: start.delay,
        duration: start.duration,
        timing_function,
        timing_function_serialization,
    };
    run_procedure(&transition, perform, |procedures| {
        procedures.set_timeline(timeline);
        procedures.start_a_provisional_transition(&effect, timing);
    });
    transition
}

/// Discards the CSS transition `animation`, which was started provisionally, setting its timeline to none, having the
/// host perform what that asks for through `perform`.
fn discard_provisional_transition(animation: &AnimationHandle, perform: HostEffectPerformer) {
    run_procedure(animation, perform, |procedures| procedures.set_timeline(None));
    procedures::run(animation, |procedures| {
        procedures.discard_provisional_transition();
        Ok(())
    })
    .expect("discarding a provisional transition does not throw");
}

/// Cancels `animation`, invalidating the style of its target as `invalidate` asks, in an execution context the host
/// opens through `in_execution_context` with `context` where it is not idle, having the host perform what that asks for
/// through `perform`.
fn cancel_animation(
    animation: &AnimationHandle,
    invalidate: Invalidate,
    in_execution_context: FfiInExecutionContext,
    context: *mut c_void,
    perform: HostEffectPerformer,
) {
    let cancel = || {
        run_procedure(animation, perform, |procedures| procedures.cancel(invalidate));
    };
    // NB: Canceling an animation that is not idle settles its promises and creates its cancel event.
    if animation.borrow().play_state() == AnimationPlayState::Idle {
        cancel();
    } else {
        in_host_execution_context(in_execution_context, context, animation, cancel);
    }
}

/// The element or pseudo-element `owner` names, as the target of a transition step.
fn transition_target(owner: FfiOwningElement) -> TransitionTarget {
    TransitionTarget {
        element: NonNull::new(owner.element).expect("a transition step has a target"),
        pseudo_element: owner.has_pseudo_element.then_some(owner.pseudo_element),
    }
}

/// What a transition step decides over: the lists of its target's element, which it can change, the target, with the
/// raw fly string of the serialization of its pseudo-element, or zero, and its slot in the lists, where it can own
/// transitions, the time of the style change event on the document timeline, in milliseconds, the generation of the
/// transitions it starts, the store of the animation overlay of the target's custom properties, or null where it has
/// none, the style engine of `host`, which `read` reads, the records `before` and `after` the target's style changes
/// between, the target's style node, its pseudo-element kind, or `u8::MAX`, the slot of the row of the box of its
/// element, and the tree order of the host's elements.
#[repr(C)]
pub struct FfiTransitionStep {
    pub animations: *mut ElementAnimations,
    pub owner: FfiOwningElement,
    pub pseudo_element_serialization: usize,
    pub has_slot: bool,
    pub slot: AnimationOwnerSlot,
    pub style_change_event_time: f64,
    pub generation: u64,
    pub overlay_store: *const c_void,
    pub host: *const DocumentHost,
    pub read: *const crate::render_state::BegunRead,
    pub before: *const c_void,
    pub after: *const c_void,
    pub target_node: u32,
    pub target_pseudo_kind: u8,
    pub element_box_slot: u32,
    pub tree_order: FfiTreeOrder,
}

/// What a transition step did: whether it changed the effects that compose over the target's style, and whether it
/// started any transition.
#[repr(C)]
#[derive(Default)]
pub struct FfiTransitionStepOutcome {
    pub changes_effects: bool,
    pub started_transitions: bool,
}

/// Records the decision `action` for the provisional state at `index` of `document`, which supersedes any an earlier
/// pass of the epoch made, discarding the transition that one started, if any.
///
/// # Safety
/// `document` must be live, and no reference to it may be held across the call.
unsafe fn decide_provisional_transition(
    document: *mut DocumentAnimations,
    index: usize,
    action: FfiTransitionActionKind,
    perform: HostEffectPerformer,
) {
    let discarded = {
        // SAFETY: Guaranteed by the caller.
        let state = unsafe { &mut *document }.provisional_transitions.state_mut(index);
        note_rendering_trace_event("provisional transition decision");
        if state.has_decision {
            note_rendering_trace_event("provisional transition decision superseded");
        }
        state.has_decision = true;
        state.action = action;
        state.proposed.take()
    };
    if let Some(discarded) = discarded {
        discard_provisional_transition(&discarded, perform);
    }
}

/// https://drafts.csswg.org/css-transitions/#starting
/// Runs the transition step `step` describes in the outermost style stabilization epoch of `document`: the style engine
/// decides what it does to each property's transitions, and the transitions it starts start provisionally, on the
/// document timeline of the target. A transition action is provisional until the epoch commits; a later pass of the
/// epoch decides again, superseding the decision of this one. Where the step changes the effects that compose over the
/// target's style, the host keeps alive through `keep_alive` with `context` the animations of those effects, in
/// composite order: the relevant animations of the element that target the same element or pseudo-element as the
/// step, beside those of the SVG animation elements, which script does not see, and those whose transitions the step
/// replaces, and the transitions it started. The host performs what that asks of it through `perform`.
///
/// # Safety
/// `document` must be live for the call, and `step` must name live host objects, records and lists, which the host may
/// change as it performs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_run_transition_step(
    document: *mut DocumentAnimations,
    step: &FfiTransitionStep,
    keep_alive: FfiKeepAlive,
    context: *mut c_void,
    perform: HostEffectPerformer,
) -> FfiTransitionStepOutcome {
    let target = transition_target(step.owner);
    let animations = NonNull::new(step.animations).expect("the target's element has lists");
    let slot = step.has_slot.then_some(step.slot);
    let t = step.style_change_event_time;

    // NB: The element holds each transition, which lends what is read of it for as long as the element holds it. A
    //     running transition of a custom property holds it at what the overlay of the target's environment answers.
    //     One not sampled yet holds it at its start value.
    // SAFETY: Guaranteed by the caller.
    let overlay = unsafe {
        step.overlay_store
            .cast::<crate::css::custom_properties::CustomPropertyStore>()
            .as_ref()
    };
    // SAFETY: Guaranteed by the caller.
    let held = slot.map_or(&[][..], |slot| unsafe { animations.as_ref() }.transitions(slot));
    let existing: SmallVec<[FfiExistingTransition; 4]> = held
        .iter()
        .map(|transition| {
            let record = transition.borrow();
            let mut existing = record.existing_transition(t);
            if existing.custom_name != 0 && existing.running {
                existing.custom_current_value = overlay
                    .and_then(|store| store.get(existing.custom_name))
                    .map_or(record.css_transition().start_value.pointer(), |entry| {
                        entry.value.pointer()
                    });
            }
            existing
        })
        .collect();

    // NB: The values of the transitions the step starts are computed values of the after-change style.
    // SAFETY: Guaranteed by the caller.
    let computed_value_zoom = unsafe { crate::css::style::ViewLonghands::of_raw_record(step.after) }.effective_zoom();
    let mut outcome = FfiTransitionStepOutcome::default();
    let mut timeline = None;
    let mut started: SmallVec<[AnimationHandle; 2]> = SmallVec::new();
    let mut replaced: SmallVec<[AnimationEffectId; 2]> = SmallVec::new();
    let input = crate::css::transition::FfiTransitionInput {
        target_node: step.target_node,
        target_pseudo_kind: step.target_pseudo_kind,
        element_box_slot: step.element_box_slot,
    };
    let on_actions = |actions: &[FfiTransitionAction]| {
        let mut decided: SmallVec<[usize; 4]> = SmallVec::new();
        for action in actions {
            let property = TransitionProperty {
                property_id: action.property_id,
                custom_name: action.custom_name,
            };
            // SAFETY: Guaranteed by the caller.
            let index = unsafe {
                (*document)
                    .provisional_transitions
                    .ensure(target, animations, slot, property)
            };
            decided.push(index);
            // SAFETY: Guaranteed by the caller.
            unsafe { decide_provisional_transition(document, index, action.kind, perform) };
            if action.kind != FfiTransitionActionKind::None && action.kind != FfiTransitionActionKind::Start {
                // SAFETY: Guaranteed by the caller.
                let state = unsafe { &*document }.provisional_transitions.state(index);
                let committed = state
                    .committed
                    .as_ref()
                    .expect("an action on a transition has one to act on");
                if let Some(effect) = committed.borrow().effect() {
                    replaced.push(effect.borrow().id());
                }
            }
            if action.start_value.is_null() {
                continue;
            }
            let start = TransitionStart {
                owner: step.owner,
                pseudo_element_serialization: step.pseudo_element_serialization,
                transition: TransitionDescription {
                    generation: step.generation,
                    start_time: t + action.delay,
                    end_time: t + action.active_duration + action.delay,
                    reversing_shortening_factor: action.reversing_shortening_factor,
                    custom_name: action.custom_name,
                    start_value: action.start_value.cast(),
                    end_value: action.end_value.cast(),
                    reversing_adjusted_start_value: action.reversing_adjusted_start_value.cast(),
                    property_id: action.property_id,
                },
                delay: action.delay,
                duration: action.active_duration,
                timing_function: action.timing_function.cast(),
                computed_value_zoom,
            };
            let timeline = timeline
                .get_or_insert_with(|| default_document_timeline(element_node_document(target.element)))
                .clone();
            // SAFETY: The action names live style values and raw fly strings, and the step live host objects.
            let transition = unsafe { start_a_provisional_transition(&start, Some(timeline), perform) };
            started.push(transition.clone());
            // SAFETY: Guaranteed by the caller.
            unsafe { &mut *document }
                .provisional_transitions
                .state_mut(index)
                .proposed = Some(transition);
        }
        // NB: A property an earlier pass of the epoch decided over that this one does not leaves its transitions as
        //     they are.
        // SAFETY: Guaranteed by the caller.
        let indices = unsafe { &*document }.provisional_transitions.indices_of(target);
        for index in indices {
            if !decided.contains(&index) {
                // SAFETY: Guaranteed by the caller.
                unsafe {
                    decide_provisional_transition(document, index, FfiTransitionActionKind::None, perform);
                };
            }
        }
    };
    // SAFETY: Guaranteed by the caller.
    unsafe {
        crate::css::style::engine_calls::decide_transitions(
            &*step.host,
            &*step.read,
            step.before,
            step.after,
            &input,
            &existing,
            on_actions,
        );
    }

    // NB: A transition action is provisional until the stabilization epoch commits, but the style published by this
    //     pass must already reflect that decision. The effect stack is rebuilt without the transitions being removed,
    //     and with any proposed replacements on top.
    if replaced.is_empty() && started.is_empty() {
        return outcome;
    }
    outcome.changes_effects = true;
    outcome.started_transitions = !started.is_empty();
    // SAFETY: Guaranteed by the caller.
    let mut remaining: Vec<AnimationHandle> = unsafe { animations.as_ref() }
        .associated()
        .iter()
        .filter(|animation| {
            if matches!(animation.borrow().kind(), AnimationKind::Smil { .. }) {
                return false;
            }
            let Some(effect) = animation.borrow().effect().cloned() else {
                return false;
            };
            let effect = effect.borrow();
            if effect.pseudo_element().map(|pseudo_element| pseudo_element.kind) != target.pseudo_element
                || replaced.contains(&effect.id())
            {
                return false;
            }
            procedures::run(animation, |procedures| Ok(procedures.is_relevant()))
                .expect("reading relevance does not throw")
                .0
        })
        .cloned()
        .collect();
    remaining.extend(started);
    let mut tree_order = host_tree_order(step.tree_order);
    remaining.sort_by(|a, b| composite_order(&a.borrow(), &b.borrow(), &mut tree_order));
    for animation in &remaining {
        // SAFETY: The host keeps alive an animation it is handed.
        unsafe { keep_alive(context, animation) };
    }
    outcome
}

/// Commits the transition actions the outermost style stabilization epoch of `document` decided, as the epoch commits:
/// each removes the transition its property had, cancels it in an execution context the host opens through
/// `in_execution_context` with `context` where it is not idle, or commits the transition the epoch started, which
/// associates it with its target. The host performs what that asks of it through `perform`.
///
/// # Safety
/// `document` must be live for the call, which the host may change as it performs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_commit_provisional_transitions(
    document: *mut DocumentAnimations,
    in_execution_context: FfiInExecutionContext,
    context: *mut c_void,
    perform: HostEffectPerformer,
) {
    // NB: The states stay with the document until every action is taken, which keeps what they name alive.
    let mut index = 0;
    // SAFETY: Guaranteed by the caller, for each read below; no borrow outlives the read.
    while index < unsafe { &*document }.provisional_transitions.len() {
        let (action, animations, slot, property, committed, proposed) = {
            // SAFETY: As above.
            let state = unsafe { &*document }.provisional_transitions.state(index);
            (
                state.action,
                state.animations,
                state.slot,
                state.property,
                state.committed.clone(),
                state.proposed.clone(),
            )
        };
        index += 1;
        let remove_committed_transition = || {
            let (Some(slot), Some(committed)) = (slot, committed.as_ref()) else {
                return;
            };
            // SAFETY: The state keeps the element that owns the lists alive, and nothing else refers to them here.
            let animations = unsafe { &mut *animations.as_ptr() };
            if animations
                .transition(slot, property)
                .is_some_and(|transition| transition.ptr_eq(committed))
            {
                animations.remove_transition(slot, property);
            }
        };
        let cancel_committed_transition = || {
            let committed = committed
                .as_ref()
                .expect("a cancelled transition is one the target held");
            cancel_animation(committed, Invalidate::Yes, in_execution_context, context, perform);
        };
        let commit_proposed_transition = || {
            let proposed = proposed.clone().expect("a started transition was proposed");
            // SAFETY: The state keeps the element that owns the lists alive. Associating the transition with its
            //         target reaches the lists again, after this borrow ends.
            let effects = transitions::commit_provisional_transition(
                slot.map(|slot| (unsafe { &mut *animations.as_ptr() }, slot)),
                proposed,
                TransitionOrigin::StyleChange,
            );
            perform_host_effects(effects, perform);
            note_rendering_trace_event("transition started");
        };
        match action {
            FfiTransitionActionKind::None => continue,
            FfiTransitionActionKind::Remove => remove_committed_transition(),
            FfiTransitionActionKind::Cancel => cancel_committed_transition(),
            FfiTransitionActionKind::Start => commit_proposed_transition(),
            FfiTransitionActionKind::RemoveAndStart => {
                remove_committed_transition();
                commit_proposed_transition();
            }
            FfiTransitionActionKind::CancelRemoveAndStart => {
                cancel_committed_transition();
                remove_committed_transition();
                commit_proposed_transition();
            }
        }
        note_rendering_trace_event("transition action committed");
    }
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { &mut *document }.provisional_transitions.take());
}

/// Whether the outermost style stabilization epoch of `document` decided over the transitions of `owner`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_animations_has_provisional_transitions(
    document: &DocumentAnimations,
    owner: FfiOwningElement,
) -> bool {
    document.provisional_transitions.has_states(transition_target(owner))
}

/// The effect of the transition at `index` among those the outermost style stabilization epoch of `document` started
/// provisionally for `owner`, which the transition borrows for as long as the epoch holds it, or null past the last.
/// NB: A provisionally started transition already contributed to the style published by the pass that started it,
///     but it is not associated with its target until the stabilization epoch commits.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_animations_proposed_transition_effect(
    document: &DocumentAnimations,
    owner: FfiOwningElement,
    index: usize,
) -> *const AnimationEffectCell {
    document
        .provisional_transitions
        .proposed_effect(transition_target(owner), index)
}

/// AD-HOC: Runs the transitions the hover of a clock lane left the element whose lists `animations` are, `element`,
///         running beside a task, which the screen showed, in place of those its steps ended, as the rendering update
///         that takes them in begins: `lane_transitions` are those the lane leaves the element running, and the lane's
///         steps ended the host's transitions whose effects `cancelled` names and saw those `seen` names. A transition
///         this starts is of the generation `generation`, on the document timeline, with values computed at the
///         element's effective zoom `effective_zoom`. The host keeps each transition alive through `keep_alive` with
///         `context`, and performs what that asks of it through `perform`.
///
/// # Safety
/// `animations` must be the lists of `element`, which live for the call, and which the host may change as it performs,
/// and `lane_transitions` must name live style values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_element_animations_adopt_lane_transitions(
    animations: *mut ElementAnimations,
    element: NonNull<c_void>,
    cancelled: ak::Span<'_, AnimationEffectId>,
    seen: ak::Span<'_, AnimationEffectId>,
    lane_transitions: ak::Span<'_, FfiLaneTransition>,
    generation: u64,
    effective_zoom: f32,
    keep_alive: FfiKeepAlive,
    context: *mut c_void,
    perform: HostEffectPerformer,
) {
    let slot = AnimationOwnerSlot(0);
    let (cancelled, seen, lane_transitions) = (cancelled.as_slice(), seen.as_slice(), lane_transitions.as_slice());
    let timeline = Some(default_document_timeline(element_node_document(element)));
    // NB: What the host performs can change the lists, so no borrow of them outlives a read.
    // SAFETY: Guaranteed by the caller, for each read below.
    let lists = || unsafe { &mut *animations };
    let transition_of = |property_id: u16| {
        lists()
            .transition(
                slot,
                TransitionProperty {
                    property_id,
                    custom_name: 0,
                },
            )
            .cloned()
    };
    let run = |transition: &AnimationHandle,
               procedure: &mut dyn FnMut(&mut Procedures<'_>) -> Result<(), AnimationError>| {
        if let Ok(((), effects)) = procedures::run(transition, |procedures| procedure(procedures)) {
            perform_host_effects(effects, perform);
        }
    };
    let is_running = |transition: &AnimationHandle| {
        let transition = transition.borrow();
        transition.play_state() != AnimationPlayState::Idle && !transition.playback().is_finished
    };
    // NB: The start time of a transition is a time of the document timeline, which validating it as script sets it
    //     would only read as milliseconds.
    let set_start_time = |transition: &AnimationHandle, start_time: f64| {
        run(transition, &mut |procedures| {
            procedures.set_start_time_for_bindings(Some(TimeValue::milliseconds(start_time)));
            Ok(())
        });
    };

    // NB: The record of each running transition decides what becomes of it (see LaneAdoption). One the steps saw end by
    //     itself ends as it does, with its events, and so does one script finished meanwhile.
    let existing: SmallVec<[AnimationHandle; 4]> = lists().transitions(slot).iter().cloned().collect();
    let mut ended: SmallVec<[AnimationHandle; 4]> = SmallVec::new();
    let mut host_decides: SmallVec<[TransitionProperty; 4]> = SmallVec::new();
    for transition in &existing {
        // SAFETY: The host keeps alive an animation it is handed for as long as this call lasts.
        unsafe { keep_alive(context, transition) };
        if !is_running(transition) {
            continue;
        }
        let adoption = transition.borrow().lane_adoption(lane_transitions, cancelled, seen);
        match adoption {
            LaneAdoption::Stands => {}
            LaneAdoption::Ends => ended.push(transition.clone()),
            LaneAdoption::HostDecides => host_decides.push(TransitionProperty::of(&transition.borrow())),
        }
    }
    for transition in &ended {
        run(transition, &mut |procedures| {
            procedures.cancel(Invalidate::Yes);
            Ok(())
        });
        let property = TransitionProperty::of(&transition.borrow());
        lists().remove_transition(slot, property);
    }
    // NB: A transition of the host's whose play is still pending runs from when the lane took it to start.
    for lane_transition in lane_transitions
        .iter()
        .filter(|lane_transition| lane_transition.host_identity != AnimationEffectId::default())
    {
        let Some(transition) = transition_of(lane_transition.property_id) else {
            continue;
        };
        if !transition.borrow().is_host_transition_of(lane_transition) {
            continue;
        }
        let timing = transition.borrow().timing_state(None);
        if timing.playback.start_time.get().is_none()
            && (timing.playback.has_pending_play_task || timing.playback.has_pending_pause_task)
            && timing.play_state != AnimationPlayState::Paused
        {
            set_start_time(&transition, lane_transition.start_time);
        }
    }
    for lane_transition in lane_transitions
        .iter()
        .filter(|lane_transition| lane_transition.host_identity == AnimationEffectId::default())
    {
        let property = TransitionProperty {
            property_id: lane_transition.property_id,
            custom_name: 0,
        };
        if host_decides.contains(&property) {
            continue;
        }
        if let Some(current) = transition_of(lane_transition.property_id) {
            if current.borrow().runs_as_lane_does(lane_transitions) {
                continue;
            }
            // NB: A completed transition of the property gives way to the one the lane started. One the lane saw end,
            //     which the host's timeline has yet to reach, ends now.
            if is_running(&current) {
                run(&current, &mut |procedures| procedures.finish_an_animation());
            }
            lists().remove_transition(slot, property);
        }
        let start = TransitionStart {
            owner: FfiOwningElement {
                element: element.as_ptr(),
                pseudo_element: 0,
                has_pseudo_element: false,
            },
            pseudo_element_serialization: 0,
            transition: TransitionDescription {
                generation,
                start_time: lane_transition.start_time + lane_transition.delay,
                end_time: lane_transition.start_time + lane_transition.duration + lane_transition.delay,
                reversing_shortening_factor: lane_transition.reversing_shortening_factor,
                custom_name: 0,
                start_value: lane_transition.start_value.cast(),
                end_value: lane_transition.end_value.cast(),
                reversing_adjusted_start_value: lane_transition.reversing_adjusted_start_value.cast(),
                property_id: lane_transition.property_id,
            },
            delay: lane_transition.delay,
            duration: lane_transition.duration,
            timing_function: lane_transition.timing_function.cast(),
            computed_value_zoom: effective_zoom,
        };
        // SAFETY: Guaranteed by the caller.
        let transition = unsafe { start_a_provisional_transition(&start, timeline.clone(), perform) };
        // SAFETY: The host keeps alive an animation it is handed for as long as this call lasts.
        unsafe { keep_alive(context, &transition) };
        let effects = transitions::commit_provisional_transition(
            Some((lists(), slot)),
            transition.clone(),
            TransitionOrigin::Lane,
        );
        // NB: Associating the transition with its target reaches the lists again.
        perform_host_effects(effects, perform);
        set_start_time(&transition, lane_transition.start_time);
    }
}

/// The host's `SVGAnimationElement` whose animation the SMIL animation `animation` is.
#[unsafe(no_mangle)]
pub extern "C" fn animations_smil_animation_element(animation: &AnimationCell) -> *mut c_void {
    smil_animation_element(animation).as_ptr()
}

/// The host's `SVGAnimationElement` whose animation `animation` is.
pub(crate) fn smil_animation_element(animation: &AnimationCell) -> NonNull<c_void> {
    match animation.borrow().kind() {
        AnimationKind::Smil { animation_element, .. } => *animation_element,
        _ => unreachable!("the animation is an SVG animation element's"),
    }
}

/// Compares two of the host's `DOM::Element`s in tree order, answering a negative number where the first comes first,
/// a positive one where it comes last, and zero where neither does.
pub type FfiTreeOrder = unsafe extern "C" fn(first: *mut c_void, second: *mut c_void) -> i32;

/// The tree order `compare` gives.
pub(crate) fn host_tree_order(compare: FfiTreeOrder) -> impl FnMut(NonNull<c_void>, NonNull<c_void>) -> Ordering {
    move |first, second| {
        // SAFETY: The host compares live elements of its own.
        unsafe { compare(first.as_ptr(), second.as_ptr()) }.cmp(&0)
    }
}

/// https://www.w3.org/TR/web-animations-1/#animation-composite-order
/// The relative composite order of the animations `first` and `second`: less than zero where `first` comes first, zero
/// for the same animation, and greater than zero otherwise, with the host's elements in the tree order `tree_order`
/// gives.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_composite_order(
    first: &AnimationCell,
    second: &AnimationCell,
    tree_order: FfiTreeOrder,
) -> i32 {
    if std::ptr::eq(first, second) {
        return 0;
    }
    match super::composite_order::composite_order(&first.borrow(), &second.borrow(), &mut host_tree_order(tree_order)) {
        Ordering::Less => -1,
        Ordering::Equal => 0,
        Ordering::Greater => 1,
    }
}

/// Takes a reference of the host's own to `animation`, which the host holds a reference to.
///
/// # Safety
/// `animation` must be an animation the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_animation_retain(animation: *const AnimationCell) {
    // SAFETY: Guaranteed by the caller.
    let _ = unsafe { AnimationHandle::retained_from_host(animation) }.into_raw();
}

/// Gives up a reference the host took to `animation` with `animations_animation_retain`, or was handed.
///
/// # Safety
/// `animation` must be an animation the host holds a reference to, which it gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_animation_unretain(animation: *const AnimationCell) {
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { AnimationHandle::from_raw(animation) });
}

/// What a new document knows of animations, which the document owns.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_animations_create() -> *mut DocumentAnimations {
    Box::into_raw(Box::default())
}

/// Destroys what a document knows of animations, which keeps none of them alive.
///
/// # Safety
/// `animations` must be what `animations_document_animations_create` handed out, which the host gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_destroy(animations: *mut DocumentAnimations) {
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { Box::from_raw(animations) });
}

/// Begins the outermost style stabilization epoch of a document.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_animations_begin_style_stabilization_epoch(animations: &mut DocumentAnimations) {
    animations.begin_style_stabilization_epoch();
}

/// Commits the events a document queued during its outermost style stabilization epoch to its pending animation event
/// queue as the epoch commits, answering how many it committed.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_animations_commit_provisional_events(
    animations: &mut DocumentAnimations,
) -> usize {
    animations.commit_provisional_events()
}

/// Appends `event` of `animation` to the pending animation event queue of a document, or, where `provisional`, to the
/// events it queues during its style stabilization epoch.
/// https://www.w3.org/TR/web-animations-1/#pending-animation-event-queue
///
/// # Safety
/// `animation` must be an animation the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_append_event(
    animations: &mut DocumentAnimations,
    animation: *const AnimationCell,
    event: &FfiPendingAnimationEvent,
    provisional: bool,
) {
    // SAFETY: Guaranteed by the caller.
    let animation = unsafe { AnimationHandle::retained_from_host(animation) };
    let mut event = *event;
    // NB: A time event is scheduled at the origin-relative time of the current time of a document timeline.
    if event.kind == PendingAnimationEventKind::Time {
        let timeline = animation.borrow().timeline().cloned();
        event.scheduled_event_time = timeline
            .filter(|timeline| matches!(timeline.borrow().kind, TimelineKind::Document { .. }))
            .and_then(|timeline| timeline.convert_a_timeline_time_to_an_origin_relative_time(timeline.current_time()))
            .into();
    }
    animations
        .pending_events
        .append(PendingAnimationEvent { animation, event }, provisional);
}

/// What queueing the events of the animations of a document an animation frame established asks of the host, which
/// hands each function `context`.
#[repr(C)]
pub struct FfiQueueAnimationEventsHost {
    pub context: *mut c_void,
    pub keep_alive: FfiKeepAlive,
    /// The time stamp of an event queued for the host's `DOM::Element` `element` now.
    pub time_stamp: unsafe extern "C" fn(context: *mut c_void, element: NonNull<c_void>) -> f64,
    /// Queues the time events of the host's `SVGAnimationElement` `animation_element` the animation frame established.
    pub queue_time_events: unsafe extern "C" fn(context: *mut c_void, animation_element: NonNull<c_void>),
    pub perform: HostEffectPerformer,
}

/// https://www.w3.org/TR/web-animations-1/#update-animations-and-send-events
/// https://drafts.csswg.org/css-transitions-2/#event-dispatch
/// https://www.w3.org/TR/css-animations-2/#event-dispatch
/// For each animation associated with a document whose animations `animations` are, whether or not it has a timeline,
/// appends the events the animation frame established for a CSS animation or transition to the pending animation event
/// queue of the document, or, where `provisional`, to the events it queues during its style stabilization epoch, has
/// the host queue those of the animation of an SVG animation element, and disassociates the animation from the target
/// of its associated effect where it is inert, as a procedure scheduled. Answers how many CSS events it queued.
///
/// # Safety
/// `animations` must be what a live document knows of animations, which the host may reach as it performs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_queue_events(
    animations: *mut DocumentAnimations,
    provisional: bool,
    host: &FfiQueueAnimationEventsHost,
) -> usize {
    // SAFETY: Guaranteed by the caller, and the borrow ends before the host performs anything.
    let associated = unsafe { &mut *animations }
        .set_mut(DocumentAnimationSetKind::Associated)
        .alive();
    for animation in &associated {
        // SAFETY: The host keeps alive an animation it is handed for as long as this call lasts.
        unsafe { (host.keep_alive)(host.context, animation) };
    }
    let mut queued_count = 0;
    for animation in &associated {
        let events = animation.borrow_mut().take_css_events();
        if let AnimationKind::Smil { animation_element, .. } = *animation.borrow().kind() {
            // SAFETY: The animation keeps its animation element alive.
            unsafe { (host.queue_time_events)(host.context, animation_element) };
        } else if !events.as_slice().is_empty() {
            let owner = animation
                .borrow()
                .owning_element()
                .expect("a CSS animation or transition with events has an owning element");
            // NB: Each event takes the time stamp of when it is queued.
            // SAFETY: The animation keeps its owning element alive.
            let time_stamp = unsafe { (host.time_stamp)(host.context, owner.element) };
            for css_event in events.as_slice() {
                let event = FfiPendingAnimationEvent {
                    kind: PendingAnimationEventKind::Css,
                    css_event_type: css_event.r#type,
                    playback_event_type: PlaybackEventType::Finish,
                    time_event_type: TimeEventType::Begin,
                    detail: 0,
                    elapsed_time: css_event.elapsed_time,
                    current_time: OptionalTimeValue::UNRESOLVED,
                    timeline_time: OptionalTimeValue::UNRESOLVED,
                    scheduled_event_time: events.scheduled_event_time,
                    time_stamp,
                };
                // SAFETY: Guaranteed by the caller; the borrow ends before the host performs anything.
                unsafe { &mut *animations }.pending_events.append(
                    PendingAnimationEvent {
                        animation: animation.clone(),
                        event,
                    },
                    provisional,
                );
            }
            queued_count += events.as_slice().len();
        }
        if animation.borrow().disassociation_from_target_pending {
            run_procedure(animation, host.perform, |procedures| {
                procedures.disassociate_from_target_if_inert();
            });
        }
    }
    queued_count
}

/// Calls `visit` with `context` and each GC cell of the host that keeps alive the elements the records of the events a
/// document queued name, and the timelines associated with the document, which the document visits as the GC traces
/// it.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_animations_visit_edges(
    animations: &DocumentAnimations,
    context: *mut c_void,
    visit: unsafe extern "C" fn(context: *mut c_void, cell: *mut c_void, kind: HostCellKind),
) {
    animations.pending_events.for_each_host_cell(|cell, kind| {
        // SAFETY: The host visits a cell of an animation it queued an event of.
        unsafe { visit(context, cell.as_ptr(), kind) }
    });
    // NB: The document keeps alive the elements and transitions its provisional transition actions decide over.
    animations.provisional_transitions.for_each_host_cell(|cell, kind| {
        // SAFETY: The host visits a cell the document's provisional transition actions keep alive.
        unsafe { visit(context, cell.as_ptr(), kind) }
    });
    // NB: The document keeps the timelines associated with it alive, and the objects script sees of them.
    for timeline in &animations.timelines {
        timeline.for_each_holder_cell(|cell, kind| {
            // SAFETY: The host visits a cell of a timeline associated with the document.
            unsafe { visit(context, cell.as_ptr(), kind) }
        });
    }
}

/// Adds `effect` to the set `kind` of the effects a document knows of, answering whether the set did not hold it yet.
///
/// # Safety
/// `effect` must be an effect the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_effect_set_insert(
    animations: &mut DocumentAnimations,
    kind: DocumentEffectSetKind,
    effect: *const AnimationEffectCell,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let effect = unsafe { EffectHandle::retained_from_host(effect) };
    animations.effect_set_mut(kind).insert(&effect)
}

/// Drops the entries of the animations in `set` that died, as the host begins to enumerate it.
#[unsafe(no_mangle)]
pub extern "C" fn animations_weak_animation_set_prune(set: &mut WeakAnimationSet) {
    set.prune();
}

/// The number of entries of `set`, live or dead, which the host enumerates by index.
#[unsafe(no_mangle)]
pub extern "C" fn animations_weak_animation_set_entry_count(set: &WeakAnimationSet) -> usize {
    set.entry_count()
}

/// A reference, handed to the host, to the animation of the entry at `index` of `set`, or null where it died.
#[unsafe(no_mangle)]
pub extern "C" fn animations_weak_animation_set_get(set: &WeakAnimationSet, index: usize) -> *const AnimationCell {
    set.get(index).map_or(std::ptr::null(), AnimationHandle::into_raw)
}

/// New empty lists of the animations of an element, which the element owns.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_create() -> *mut ElementAnimations {
    Box::into_raw(Box::default())
}

/// Destroys the lists of the animations of an element, giving up the references they hold.
///
/// # Safety
/// `animations` must be lists `animations_element_animations_create` handed out, which the host gives up, and
/// `element` null or the element whose lists they are.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_element_animations_destroy(
    animations: *mut ElementAnimations,
    element: *mut c_void,
) {
    // SAFETY: Guaranteed by the caller.
    let mut animations = unsafe { Box::from_raw(animations) };
    if let Some(element) = NonNull::new(element) {
        animations.forget_element_as_target_of_disassociated(element);
    }
    drop(animations);
}

/// Calls `visit` with `context` and each GC cell of the host that keeps alive what an animation the lists hold names.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_for_each_host_cell(
    animations: &ElementAnimations,
    context: *mut c_void,
    visit: unsafe extern "C" fn(context: *mut c_void, cell: *mut c_void, kind: HostCellKind),
) {
    animations.for_each_host_cell(|cell, kind| {
        // SAFETY: The host visits a cell of an animation it holds.
        unsafe { visit(context, cell.as_ptr(), kind) }
    });
}

/// Associates `animation` with the element, and, where it was not associated yet, with its document, whose animations
/// `document` are, as one created during its style stabilization epoch where `in_style_stabilization_epoch`, answering
/// whether it was not associated yet.
///
/// # Safety
/// `animation` must be an animation the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_element_animations_associate(
    animations: &mut ElementAnimations,
    document: &mut DocumentAnimations,
    animation: *const AnimationCell,
    in_style_stabilization_epoch: bool,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let animation = unsafe { AnimationHandle::retained_from_host(animation) };
    if !animations.associate(animation.clone()) {
        return false;
    }
    animation.borrow_mut().did_associate_with_target();
    document.associate(&animation, in_style_stabilization_epoch);
    true
}

/// Disassociates `animation`, whose associated effect `effect` was, if any, from the element, whose lists `animations`
/// are where it has any, and from its document, whose animations `document` are, answering whether it was associated
/// with the element.
///
/// # Safety
/// `animations` must be null or the live lists of the element, and `effect` null or a live effect.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_element_animations_disassociate(
    animations: *mut ElementAnimations,
    document: &mut DocumentAnimations,
    animation: &AnimationCell,
    effect: *const AnimationEffectCell,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let was_associated = unsafe { animations.as_mut() }.is_some_and(|animations| animations.disassociate(animation));
    // SAFETY: Guaranteed by the caller.
    document.disassociate(animation, unsafe { effect.as_ref() });
    was_associated
}

/// Moves the animations associated with an element, whose lists `animations` are, from the document whose animations
/// `old_document` are to the one whose animations `new_document` are, as ones created during its style stabilization
/// epoch where `in_style_stabilization_epoch`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_move_to_document(
    animations: &ElementAnimations,
    old_document: &mut DocumentAnimations,
    new_document: &mut DocumentAnimations,
    in_style_stabilization_epoch: bool,
) {
    for animation in animations.associated() {
        let effect = animation.borrow().effect().cloned();
        old_document.disassociate(animation, effect.as_deref());
        new_document.associate(animation, in_style_stabilization_epoch);
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_invalidate_composite_order(animations: &mut ElementAnimations) {
    animations.invalidate_composite_order();
}

/// Sorts the animations associated with the element in composite order, with `tree_order` comparing elements in tree
/// order.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_sort_in_composite_order(
    animations: &mut ElementAnimations,
    tree_order: FfiTreeOrder,
) {
    animations.sort_in_composite_order(&mut host_tree_order(tree_order));
}

/// One of the lists of the animations of an element.
#[expect(dead_code, reason = "C++ constructs the variants")]
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum ElementAnimationListKind {
    /// The animations associated with the element.
    Associated,
    /// The CSS animations of one owner.
    CssAnimations,
    /// The CSS transitions of one owner.
    Transitions,
}

fn element_animation_list(
    animations: &ElementAnimations,
    kind: ElementAnimationListKind,
    slot: AnimationOwnerSlot,
) -> &[AnimationHandle] {
    match kind {
        ElementAnimationListKind::Associated => animations.associated(),
        ElementAnimationListKind::CssAnimations => animations.css_animations(slot),
        ElementAnimationListKind::Transitions => animations.transitions(slot),
    }
}

/// The number of animations in the list `kind`, of the owner at `slot` where it is one of an owner's.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_count(
    animations: &ElementAnimations,
    kind: ElementAnimationListKind,
    slot: AnimationOwnerSlot,
) -> usize {
    element_animation_list(animations, kind, slot).len()
}

/// The animation at `index` in the list `kind`, of the owner at `slot` where it is one of an owner's, which it borrows
/// for as long as the list holds it.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_get(
    animations: &ElementAnimations,
    kind: ElementAnimationListKind,
    slot: AnimationOwnerSlot,
    index: usize,
) -> *const AnimationCell {
    std::ptr::from_ref::<AnimationCell>(&element_animation_list(animations, kind, slot)[index])
}

/// The style engine of `host`, or none where it is null, which knows the element as `node`, or not at all where it is
/// zero.
///
/// # Safety
/// A non-null `host` must be live for the call.
unsafe fn style_engine_target<'a>(host: *const DocumentHost, node: u32) -> Option<StyleEngineTarget<'a>> {
    Some(StyleEngineTarget {
        // SAFETY: Guaranteed by the caller.
        host: unsafe { host.as_ref() }?,
        node: StyleNodeID::from_raw(node)?,
    })
}

/// Whether the element has had CSS animations since it last canceled them, or has transitions.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_has_css_animations_or_transitions(
    animations: &ElementAnimations,
) -> bool {
    animations.has_css_animations_or_transitions()
}

/// The host's function that runs `run` with `run_context` in an execution context it opens for `animation`, as its
/// caller, which hands it `context`, asks.
pub type FfiInExecutionContext = unsafe extern "C" fn(
    context: *mut c_void,
    animation: &AnimationCell,
    run: unsafe extern "C" fn(run_context: *mut c_void),
    run_context: *mut c_void,
);

/// Runs `run` in the execution context the host opens for `animation` through `in_execution_context` with `context`.
fn in_host_execution_context<F: FnOnce()>(
    in_execution_context: FfiInExecutionContext,
    context: *mut c_void,
    animation: &AnimationCell,
    run: F,
) {
    unsafe extern "C" fn run_once<F: FnOnce()>(run_context: *mut c_void) {
        // SAFETY: `run_context` is the `Option<F>` below, which outlives the host's call.
        let run = unsafe { &mut *run_context.cast::<Option<F>>() };
        (run.take().expect("the host runs the procedure once"))();
    }
    let mut run = Some(run);
    // SAFETY: The host runs `run_once` with `run_context` before it returns.
    unsafe { in_execution_context(context, animation, run_once::<F>, (&raw mut run).cast()) };
}

/// https://drafts.csswg.org/css-animations-2/#owning-element-section
/// Updating display to a value other than none will start all animations applied to the element by the animation-name
/// property.
/// Plays or pauses each CSS animation the element and its pseudo-elements own as its `animation-play-state` last
/// computed, the element's first and then each pseudo-element's in the order of the host's `PseudoElement`, each in an
/// execution context the host opens through `in_execution_context` with `context`. The host keeps each animation alive
/// through `keep_alive` with `context`, and performs what that asks of it through `perform`.
///
/// # Safety
/// `animations` must be the lists of an element, which live for the call, and which the host may change as it performs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_element_animations_play_or_pause_css_animations(
    animations: *const ElementAnimations,
    keep_alive: FfiKeepAlive,
    in_execution_context: FfiInExecutionContext,
    context: *mut c_void,
    perform: HostEffectPerformer,
) {
    let mut previous_slot = None;
    // NB: What the host performs can change the lists, which are read anew for each owner and no borrow of which
    //     outlives the read.
    // SAFETY: Guaranteed by the caller.
    while let Some((slot, css_animations)) = unsafe { &*animations }.css_animations_after(previous_slot) {
        previous_slot = Some(slot);
        for animation in &css_animations {
            // SAFETY: The host keeps alive an animation it is handed for as long as this call lasts.
            unsafe { keep_alive(context, animation) };
        }
        for animation in &css_animations {
            let play_state = animation.borrow().css_animation().last_css_play_state;
            let plays = match play_state {
                Some(CSS_ANIMATION_PLAY_STATE_RUNNING) => true,
                Some(CSS_ANIMATION_PLAY_STATE_PAUSED) => false,
                _ => unreachable!("a CSS-defined animation has the play state animation-play-state computed for it"),
            };
            in_host_execution_context(in_execution_context, context, animation, || {
                run_procedure_on(animation, perform, |procedures| {
                    if plays {
                        procedures.play_from_css()
                    } else {
                        procedures.pause_from_css()
                    }
                })
                .expect("playing or pausing an animation from CSS does not throw");
            });
        }
    }
}

/// https://drafts.csswg.org/css-animations-2/#owning-element-section
/// If the display property of an element is set to none and its display value would compute to none when ignoring the
/// Transitions and Animations cascade origins, then terminate running animations with this owning element.
/// Takes the CSS animations and transitions of the element and its pseudo-elements, publishes that the element and its
/// pseudo-elements own no CSS animations to the style engine of `host`, which knows the element as `node`, where the
/// host names one, and cancels each, in an execution context the host opens through `in_execution_context` with
/// `context` where it is not idle. The host keeps each animation alive through `keep_alive` with `context`, and
/// performs what that asks of it through `perform`.
///
/// # Safety
/// `animations` must be the lists of an element, which live for the call, and a non-null `host` must be live for the
/// call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_element_animations_cancel_css_animations_and_transitions(
    animations: *mut ElementAnimations,
    host: *const DocumentHost,
    node: u32,
    keep_alive: FfiKeepAlive,
    in_execution_context: FfiInExecutionContext,
    context: *mut c_void,
    perform: HostEffectPerformer,
) {
    // SAFETY: Guaranteed by the caller, and the borrow of the lists ends before the host performs anything.
    let taken = unsafe { &mut *animations }
        .take_css_animations_and_transitions(None, unsafe { style_engine_target(host, node) });
    cancel_taken_animations(&taken, keep_alive, in_execution_context, context, perform);
}

/// As [`animations_element_animations_cancel_css_animations_and_transitions`], of the owner at `slot` alone: a
/// pseudo-element that is no longer rendered, while its element is.
///
/// # Safety
/// As for [`animations_element_animations_cancel_css_animations_and_transitions`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_element_animations_cancel_css_animations_and_transitions_of_owner(
    animations: *mut ElementAnimations,
    slot: AnimationOwnerSlot,
    host: *const DocumentHost,
    node: u32,
    keep_alive: FfiKeepAlive,
    in_execution_context: FfiInExecutionContext,
    context: *mut c_void,
    perform: HostEffectPerformer,
) {
    // SAFETY: Guaranteed by the caller, and the borrow of the lists ends before the host performs anything.
    let taken = unsafe { &mut *animations }
        .take_css_animations_and_transitions(Some(slot), unsafe { style_engine_target(host, node) });
    cancel_taken_animations(&taken, keep_alive, in_execution_context, context, perform);
}

/// Cancels the CSS animations and transitions taken from their owners, each kept alive by the host for the call.
fn cancel_taken_animations(
    taken: &[AnimationHandle],
    keep_alive: FfiKeepAlive,
    in_execution_context: FfiInExecutionContext,
    context: *mut c_void,
    perform: HostEffectPerformer,
) {
    for animation in taken {
        // SAFETY: The host keeps alive an animation it is handed for as long as this call lasts.
        unsafe { keep_alive(context, animation) };
    }
    for animation in taken {
        cancel_animation(animation, Invalidate::No, in_execution_context, context, perform);
    }
}

/// The bytes the lists of the animations of an element hold, with the records of the animations associated with it and
/// of their effects, and the keyframe sets those run that `first_counted` with `context` answers are counted here first.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_record_memory_size(
    animations: &ElementAnimations,
    first_counted: unsafe extern "C" fn(context: *mut c_void, set: *const KeyframeSet) -> bool,
    context: *mut c_void,
) -> usize {
    let mut bytes = animations.memory_size();
    for animation in animations.associated() {
        bytes += animation.memory_size();
        let Some(effect) = animation.borrow().effect().cloned() else {
            continue;
        };
        bytes += effect.memory_size();
        let keyframes = effect.borrow().keyframes().cloned();
        // SAFETY: The host only notes the address of the set.
        if let Some(keyframes) = keyframes
            && unsafe { first_counted(context, Arc::as_ptr(&keyframes)) }
        {
            bytes += crate::heap_size::ARC_HEADER_BYTES + size_of::<KeyframeSet>() + keyframes.heap_size();
        }
    }
    bytes
}

/// Forgets the descriptions of the effects of the element `animations` belongs to that the host published to the style
/// engine, as the element's identity in the engine changes.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_forget_published_effect_descriptions(
    animations: &mut ElementAnimations,
) {
    animations.forget_published_effect_descriptions();
}

/// One effect the host samples on an element, by its record, with the record of its associated animation, or null
/// where it has none, which the host describes but does not sample.
#[repr(C)]
pub struct FfiSampledEffect {
    pub effect: *const AnimationEffectCell,
    pub animation: *const AnimationCell,
}

/// What the host samples of one element's effects: the description of each effect, in composite order, and the
/// effects it samples, each with the timing it samples it with.
pub(crate) struct SampledEffectDescriptions {
    pub(crate) descriptions: smallvec::SmallVec<[Arc<crate::css::style::effect_descriptions::EffectDescription>; 4]>,
    pub(crate) sampled: crate::css::style_compute::HostSampledEffects,
}

/// The descriptions of `effects` and the timing the host samples each with an animation with.
///
/// # Safety
/// Each effect and non-null animation `effects` names must be one the host holds a reference to.
pub(crate) unsafe fn describe_sampled_effects(effects: &[FfiSampledEffect]) -> SampledEffectDescriptions {
    let mut described = SampledEffectDescriptions {
        descriptions: smallvec::SmallVec::with_capacity(effects.len()),
        sampled: smallvec::SmallVec::with_capacity(effects.len()),
    };
    for sampled in effects {
        // SAFETY: Guaranteed by the caller.
        let (effect, animation) = unsafe { (&*sampled.effect, sampled.animation.as_ref()) };
        let effect = effect.borrow();
        let description = effect.description().clone();
        described.descriptions.push(description.clone());
        let Some(animation) = animation else {
            continue;
        };
        // NB: The style engine samples the animation's timeline as the timing model reads it now, with what moves it
        //     as a clock tick samples it.
        let animation = animation.borrow();
        let timeline = animation.timeline();
        let timing = super::snapshot::EffectTimingSnapshot {
            effect: effect.timing(),
            playback: *animation.playback(),
            timeline: timeline.map(|timeline| timeline.sample()).unwrap_or_default(),
            has_timeline: timeline.is_some(),
            clock: timeline.map(|timeline| timeline.clock()).unwrap_or_default(),
        };
        described.sampled.push(crate::css::style_compute::HostSampledEffect {
            description,
            timing: (&timing).into(),
        });
    }
    described
}

unsafe extern "C" {
    fn ladybird_animations_document_is_active(document: NonNull<c_void>) -> bool;
    fn ladybird_animations_element_node_document(element: NonNull<c_void>) -> NonNull<c_void>;
    /// Compares the host's `DOM::Element`s `first` and `second` in tree order, as composite order does.
    pub fn web_animation_compare_elements_in_tree_order(first: *mut c_void, second: *mut c_void) -> i32;
}

/// The host's `DOM::Document` that is the node document of the host's `DOM::Element` `element`, which something the
/// caller holds keeps alive.
/// https://dom.spec.whatwg.org/#concept-node-document
pub fn element_node_document(element: NonNull<c_void>) -> NonNull<c_void> {
    // SAFETY: Guaranteed by the caller, and the host only reads the element.
    unsafe { ladybird_animations_element_node_document(element) }
}

/// Whether the host's `DOM::Document` `document`, which something the caller holds keeps alive, is an active document.
/// https://html.spec.whatwg.org/multipage/document-sequences.html#active-document
pub(super) fn document_is_active(document: NonNull<c_void>) -> bool {
    // SAFETY: Guaranteed by the caller, and the host only reads the document.
    unsafe { ladybird_animations_document_is_active(document) }
}

#[cfg(test)]
mod ffi_test_stubs {
    #[unsafe(no_mangle)]
    extern "C" fn ladybird_animations_document_is_active(_document: std::ptr::NonNull<std::ffi::c_void>) -> bool {
        true
    }

    #[unsafe(no_mangle)]
    extern "C" fn ladybird_animations_element_node_document(
        element: std::ptr::NonNull<std::ffi::c_void>,
    ) -> std::ptr::NonNull<std::ffi::c_void> {
        element
    }
}

/// What kind of timeline the host makes.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiTimelineKind {
    Document,
    Scroll,
    View,
    Internal,
}

/// One side of the view progress visibility range of a view timeline, as `ViewTimelineInsetEdge` describes it, whose
/// calculation, if any, is a style value or null.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiViewTimelineInsetEdge {
    pub is_auto: bool,
    pub px: f64,
    pub percentage: f64,
    pub calculated: *const c_void,
}

/// https://drafts.csswg.org/scroll-animations-1/#view-timeline-inset
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiViewTimelineInset {
    pub start: FfiViewTimelineInsetEdge,
    pub end: FfiViewTimelineInsetEdge,
}

impl FfiViewTimelineInset {
    /// # Safety
    /// The calculations must be null or live style values.
    pub(crate) unsafe fn retained(&self) -> ViewTimelineInset {
        let edge = |edge: &FfiViewTimelineInsetEdge| ViewTimelineInsetEdge {
            is_auto: edge.is_auto,
            px: edge.px,
            percentage: edge.percentage,
            // SAFETY: Guaranteed by the caller.
            calculated: unsafe { retained_optional_value(edge.calculated) },
        };
        ViewTimelineInset {
            start: edge(&self.start),
            end: edge(&self.end),
        }
    }
}

/// https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-source
/// The host's `DOM::Element` that is the source of a scroll timeline, or null, or, where `is_anonymous`, the host's
/// `CSS::Scroller` that an anonymous timeline finds from `target`, the host's `DOM::Element`, or from its
/// pseudo-element, the host's `PseudoElement`, where `has_pseudo_element`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiScrollTimelineSource {
    pub element: *mut c_void,
    pub is_anonymous: bool,
    pub scroller: u8,
    pub target: *mut c_void,
    pub pseudo_element: u8,
    pub has_pseudo_element: bool,
}

impl FfiScrollTimelineSource {
    pub(crate) fn get(&self) -> ScrollTimelineSource {
        if !self.is_anonymous {
            return ScrollTimelineSource::Element(NonNull::new(self.element));
        }
        ScrollTimelineSource::Anonymous {
            scroller: self.scroller,
            target: NonNull::new(self.target).expect("an anonymous timeline has a target"),
            pseudo_element: self.has_pseudo_element.then_some(self.pseudo_element),
        }
    }
}

impl From<ScrollTimelineSource> for FfiScrollTimelineSource {
    fn from(source: ScrollTimelineSource) -> Self {
        let mut ffi = Self {
            element: std::ptr::null_mut(),
            is_anonymous: false,
            scroller: 0,
            target: std::ptr::null_mut(),
            pseudo_element: 0,
            has_pseudo_element: false,
        };
        match source {
            ScrollTimelineSource::Element(element) => {
                ffi.element = element.map_or(std::ptr::null_mut(), NonNull::as_ptr);
            }
            ScrollTimelineSource::Anonymous {
                scroller,
                target,
                pseudo_element,
            } => {
                ffi.is_anonymous = true;
                ffi.scroller = scroller;
                ffi.target = target.as_ptr();
                ffi.pseudo_element = pseudo_element.unwrap_or(0);
                ffi.has_pseudo_element = pseudo_element.is_some();
            }
        }
        ffi
    }
}

/// What the host makes a timeline of: a document timeline's origin time, a scroll timeline's source and axis, and a
/// view timeline's inset besides.
#[repr(C)]
pub struct FfiTimelineDescription {
    pub kind: FfiTimelineKind,
    pub origin_time: f64,
    pub source: FfiScrollTimelineSource,
    /// The host's `ScrollAxis`.
    pub axis: u8,
    pub inset: FfiViewTimelineInset,
}

/// A new timeline associated with the host's `DOM::Document` `document`, as `description` describes it, which has no
/// object script sees of it yet.
///
/// # Safety
/// The calculations of a view timeline's inset must be null or live style values.
pub(crate) unsafe fn timeline_of_description(
    document: NonNull<c_void>,
    description: &FfiTimelineDescription,
) -> TimelineHandle {
    let scroll = |view| {
        TimelineKind::Scroll(Box::new(ScrollTimelineState {
            source: description.source.get(),
            axis: description.axis,
            last_max_scroll_offset: None,
            followed_scroller: None,
            view,
        }))
    };
    let kind = match description.kind {
        FfiTimelineKind::Document => TimelineKind::Document {
            origin_time: description.origin_time,
        },
        FfiTimelineKind::Scroll => scroll(None),
        FfiTimelineKind::View => scroll(Some(ViewTimelineState {
            // SAFETY: Guaranteed by the caller.
            inset: unsafe { description.inset.retained() },
            last_metrics: None,
            subject_style_node: None,
        })),
        FfiTimelineKind::Internal => TimelineKind::Internal {
            time_for_observation: None,
        },
    };
    TimelineHandle::new(document, kind)
}

/// What the host reads of a timeline: its kind and its current time, which is unresolved where the timeline is
/// inactive.
#[repr(C)]
pub struct FfiTimeline {
    pub kind: FfiTimelineKind,
    pub current_time: OptionalTimeValue,
}

/// What the host reads of `timeline`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_timeline_read(timeline: &TimelineCell) -> FfiTimeline {
    let current_time = timeline.sample().current_time;
    let kind = match &timeline.borrow().kind {
        TimelineKind::Document { .. } => FfiTimelineKind::Document,
        TimelineKind::Scroll(scroll) if scroll.view.is_some() => FfiTimelineKind::View,
        TimelineKind::Scroll(_) => FfiTimelineKind::Scroll,
        TimelineKind::Internal { .. } => FfiTimelineKind::Internal,
    };
    FfiTimeline { kind, current_time }
}

/// The animations associated with `timeline`, which lives as long as the timeline.
#[unsafe(no_mangle)]
pub extern "C" fn animations_timeline_associated_animations(timeline: &TimelineCell) -> *mut WeakAnimationSet {
    timeline.associated_animations().as_ptr()
}
