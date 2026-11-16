/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which the C++ animation classes reach the timing model, until they move to Rust.

use super::animation::{
    AnimationCell, AnimationHandle, AnimationKind, AttachmentRangeEdge, AttachmentRangeEdgeOffset,
    CSS_ANIMATION_PLAY_STATE_PAUSED, CSS_ANIMATION_PLAY_STATE_RUNNING, CSS_TRANSITION_PHASE_IDLE, CssAnimationFlag,
    CssAnimationState, CssTransitionState, HostCellKind, HostTaskId, OwningElement, SmilHolder, WeakAnimationSet,
};
use super::clock_plan::ClockPlanAnimation;
use super::composite_order::composite_order;
use super::document_animations::{DocumentAnimationSetKind, DocumentAnimations, DocumentEffectSetKind};
use super::easing::CompactEasing;
use super::effect::{
    AnimationEffectCell, AnimationEffectId, CompositorAnimationStateAccess, EffectFlag, EffectHandle,
    ObservationSampleRequest, PerFrameStyleUpdateSkipDecision, PerFrameStyleUpdateSkipKey,
};
use super::element_animations::{AnimationOwnerSlot, ElementAnimations, StyleEngineTarget, TransitionProperty};
use super::host_effects::{HostEffectKind, HostEffects, PlaybackEventType};
use super::host_timing::{FfiAnimationKind, FfiAnimationTimingState};
use super::keyframes::{
    CompositeOperationOrAuto, KeyframeOrigin, KeyframeProperty, KeyframeSet, KeyframeValue, ProcessedKeyframe,
    ResourceContext,
};
use super::pending_animation_events::{
    FfiPendingAnimationEvent, PendingAnimationEvent, PendingAnimationEventKind, TimeEventType,
};
use super::procedures::{self, AnimationError, DidSeek, Invalidate, Procedures, SynchronouslyNotify};
use super::snapshot::TimelineClock;
use super::timeline::{
    AnimationTimelineId, ScrollNodeStableId, ScrollTimelineBoxes, ScrollTimelineSource, ScrollTimelineState,
    TimelineCell, TimelineHandle, TimelineKind, TimelineUpdate, ViewTimelineInset, ViewTimelineInsetEdge,
    ViewTimelineState,
};
use super::timing::{
    self, AnimationEffectPhase, AnimationEffectTiming, AnimationPlayState, AnimationPlayback, FillMode, OptionalNumber,
    OptionalTimeValue, SpecifiedEffectTiming, TimeValue, TimelineSample,
};
use super::transitions::{self, LaneAdoption, ProvisionalTransitionTiming, TransitionOrigin};
use crate::css::animation::FfiCompositeOperation;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style::animations::AppliedAnimationDefinition;
use crate::css::style::tree::StyleNodeID;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData, retain_style_value};
use crate::css::transition::{FfiExistingTransition, FfiLaneTransition};
use crate::layout::node_data::NodeSlotId;
use crate::painting::visual_animation_builder::CompositorAnimationEffectState;
use crate::render_state::{BegunRead, DocumentHost};
use libcompositing_rust::easing::{Easing, FfiEasingDescriptor, evaluate_easing_descriptor};
use std::cmp::Ordering;
use std::ffi::c_void;
use std::num::NonZeroU64;
use std::ptr::NonNull;
use std::sync::Arc;

/// The timing of an effect at one local time, and what follows from its timing alone.
#[repr(C)]
pub struct FfiResolvedEffectTiming {
    pub phase: AnimationEffectPhase,
    pub local_time: OptionalTimeValue,
    pub active_time: OptionalTimeValue,
    pub overall_progress: OptionalNumber,
    pub current_iteration: OptionalNumber,
    /// Unresolved where it was not asked for.
    pub transformed_progress: OptionalNumber,
    pub is_current: bool,
    pub is_in_effect: bool,
    pub active_duration: TimeValue,
    pub end_time: TimeValue,
    pub before_active_boundary_time: TimeValue,
    pub after_active_boundary_time: TimeValue,
}

/// The timing of `effect` at `local_time` with `fill_mode` as its fill mode, associated with `animation`, if any, on
/// its timeline, if it has one, and with its transformed progress through its timing function where
/// `with_transformed_progress`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_resolve_timing(
    effect: &AnimationEffectCell,
    local_time: OptionalTimeValue,
    fill_mode: FillMode,
    animation: Option<&AnimationCell>,
    with_transformed_progress: bool,
) -> FfiResolvedEffectTiming {
    let timeline = animation.and_then(timeline_sample_of);
    let timeline = timeline.as_ref();
    let animation = animation.map(|animation| *animation.borrow().playback());
    let animation = animation.as_ref();
    let effect = effect.borrow();
    let timing = AnimationEffectTiming {
        fill_mode,
        ..effect.timing()
    };
    let resolved = timing::resolve_timing(&timing, local_time.get(), animation, timeline);
    let transformed_progress = with_transformed_progress
        .then(|| {
            let timing_function = effect.timing_function().descriptor();
            timing::transformed_progress_with(&timing, &resolved, |input_progress, before_flag| {
                evaluate_easing_descriptor(&timing_function, input_progress, before_flag)
            })
        })
        .flatten();
    FfiResolvedEffectTiming {
        phase: resolved.phase,
        local_time: resolved.local_time.into(),
        active_time: resolved.active_time.into(),
        overall_progress: resolved.overall_progress.into(),
        current_iteration: timing::current_iteration(&timing, &resolved).into(),
        transformed_progress: transformed_progress.into(),
        is_current: timing::is_current(&timing, resolved.phase, animation, timeline),
        is_in_effect: timing::is_in_effect(&resolved),
        active_duration: timing::active_duration(&timing),
        end_time: timing::end_time(&timing),
        before_active_boundary_time: timing::before_active_boundary_time(&timing),
        after_active_boundary_time: timing::after_active_boundary_time(&timing),
    }
}

/// What the timing model reads of the timeline of `animation` now, if it has one.
fn timeline_sample_of(animation: &AnimationCell) -> Option<TimelineSample> {
    animation.borrow().timeline().map(|timeline| timeline.sample())
}

/// Whether `animation`, on its timeline, if it has one, is relevant, with its associated effect, if any, at the local
/// time `local_time`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_is_relevant(animation: &AnimationCell, local_time: OptionalTimeValue) -> bool {
    let timeline = timeline_sample_of(animation);
    let timeline = timeline.as_ref();
    let animation = animation.borrow();
    let effect_timing = animation.effect().map(|effect| effect.borrow().timing());
    timing::is_relevant(animation.playback(), timeline, effect_timing.as_ref(), local_time.get())
}

/// What a procedure threw, as the host throws it: nothing, an "InvalidStateError" DOMException, or a TypeError, with
/// its message.
#[repr(C)]
pub struct FfiAnimationError {
    pub kind: FfiAnimationErrorKind,
    pub message: *const u8,
    pub message_length: usize,
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiAnimationErrorKind {
    None,
    InvalidState,
    TypeError,
}

impl From<Result<(), AnimationError>> for FfiAnimationError {
    fn from(result: Result<(), AnimationError>) -> Self {
        let (kind, message) = match result {
            Ok(()) => (FfiAnimationErrorKind::None, ""),
            Err(AnimationError::InvalidState(message)) => (FfiAnimationErrorKind::InvalidState, message),
            Err(AnimationError::TypeError(message)) => (FfiAnimationErrorKind::TypeError, message),
        };
        Self {
            kind,
            message: message.as_ptr(),
            message_length: message.len(),
        }
    }
}

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

/// Runs `procedure` on `animation` and then has the host perform what it asked for through `perform`, in order, holding
/// no borrow of any record.
fn run_procedure(
    animation: &AnimationCell,
    perform: HostEffectPerformer,
    procedure: impl FnOnce(&mut Procedures<'_>) -> Result<(), AnimationError>,
) -> FfiAnimationError {
    run_procedure_on(animation, perform, procedure).into()
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
        let task = animation.borrow().finish_notification_task();
        match kind {
            HostEffectKind::QueueFinishNotificationTask if task.is_some() => continue,
            HostEffectKind::CancelFinishNotificationTask if task.is_none() => continue,
            _ => {}
        }
        perform(&FfiHostEffect::new(&animation, &kind, task));
    }
}

/// Notes `task`, the host's identity of the task it queued to run the finish notification steps of `animation`, or to
/// dispatch its finish event.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_set_finish_notification_task(animation: &AnimationCell, task: u64) {
    let task = HostTaskId(NonZeroU64::new(task).expect("the host's tasks have nonzero identities"));
    animation.borrow_mut().set_finish_notification_task(task);
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
    run_procedure(animation, perform, |procedures| {
        procedures.finish_notification_steps();
        Ok(())
    });
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
        Ok(())
    });
}

/// https://www.w3.org/TR/web-animations-1/#dom-animation-finish
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_finish(
    animation: &AnimationCell,
    perform: HostEffectPerformer,
) -> FfiAnimationError {
    run_procedure(animation, perform, |procedures| procedures.finish_an_animation())
}

/// https://www.w3.org/TR/web-animations-1/#dom-animation-starttime
/// To the validated `start_time`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_set_start_time(
    animation: &AnimationCell,
    start_time: OptionalTimeValue,
    perform: HostEffectPerformer,
) {
    run_procedure(animation, perform, |procedures| {
        procedures.set_start_time_for_bindings(start_time.get());
        Ok(())
    });
}

/// https://www.w3.org/TR/web-animations-1/#animation-set-the-timeline-of-an-animation
/// Of `animation` to `timeline`, or to none where it is null.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_set_timeline(
    animation: &AnimationCell,
    timeline: Option<&TimelineCell>,
    perform: HostEffectPerformer,
) {
    // SAFETY: The host holds a reference to the timeline it passes.
    let timeline = timeline.map(|timeline| unsafe { TimelineHandle::retained_from_host(timeline) });
    run_procedure(animation, perform, |procedures| {
        procedures.set_timeline(timeline);
        Ok(())
    });
}

/// The host's timeline of `animation`, or null where it has none.
/// https://www.w3.org/TR/web-animations-1/#animation-timeline
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_timeline(animation: &AnimationCell) -> *mut c_void {
    animation
        .borrow()
        .timeline()
        .and_then(|timeline| timeline.facade())
        .map_or(std::ptr::null_mut(), NonNull::as_ptr)
}

/// Updates `animation` as its timeline updated its current time.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_update(animation: &AnimationCell, perform: HostEffectPerformer) {
    run_procedure(animation, perform, |procedures| {
        procedures.update();
        Ok(())
    });
}

/// Runs the pending play task of `animation` where it has one and is ready.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_run_pending_play_task_if_ready(
    animation: &AnimationCell,
    perform: HostEffectPerformer,
) {
    run_procedure(animation, perform, |procedures| {
        procedures.run_pending_play_task_if_ready();
        Ok(())
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

/// One keyframe script gave a `KeyframeEffect`, as processing the keyframes argument made it: its computed offset,
/// scaled to a key, its composite operation, its easing where it has one, and the range of the properties it
/// declares.
#[repr(C)]
pub struct FfiProcessedKeyframe {
    pub key: u64,
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
                key: keyframe.key,
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

/// The keyframe set of a CSS transition of the property `property_id`, or of the custom property whose name is the
/// raw fly string `custom_name` where that is not zero, from `start` to `end`, computed at the effective zoom
/// `computed_value_zoom`, with the reference to it the host takes over.
///
/// # Safety
/// `custom_name` must be zero or a live raw fly string, and `start` and `end` live style values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_keyframe_set_for_transition(
    property_id: u16,
    custom_name: usize,
    start: *const c_void,
    end: *const c_void,
    computed_value_zoom: f32,
) -> *const KeyframeSet {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        into_host(KeyframeSet::for_transition(
            keyframe_property(property_id, custom_name),
            retained_value(start),
            retained_value(end),
            computed_value_zoom,
        ))
    }
}

/// What a keyframe declares for a property, as the host reads it.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum FfiKeyframeValueState {
    Absent,
    /// The element's own value, which a generated initial or final keyframe holds.
    ElementValue,
    Declared,
}

/// What a keyframe declares for a property, with the value it declares, borrowed from the set, where it declares one.
#[repr(C)]
pub struct FfiKeyframeValue {
    pub state: FfiKeyframeValueState,
    pub value: *const c_void,
}

/// What the keyframe of a set at `index` declares for the property `property_id`.
///
/// # Safety
/// `set` must be a keyframe set the host holds a reference to, and `index` one of its keyframes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_keyframe_set_value(
    set: *const KeyframeSet,
    index: usize,
    property_id: u16,
) -> FfiKeyframeValue {
    // SAFETY: Guaranteed by the caller.
    let set = unsafe { borrowed_keyframe_set(set) };
    let declaration = set
        .declarations_of(&set.keyframes()[index])
        .iter()
        .find(|declaration| declaration.property_id == property_id);
    match declaration.map(|declaration| &declaration.value) {
        None => FfiKeyframeValue {
            state: FfiKeyframeValueState::Absent,
            value: std::ptr::null(),
        },
        Some(KeyframeValue::ElementValue) => FfiKeyframeValue {
            state: FfiKeyframeValueState::ElementValue,
            value: std::ptr::null(),
        },
        Some(KeyframeValue::Declared(value)) => FfiKeyframeValue {
            state: FfiKeyframeValueState::Declared,
            value: value.pointer().cast(),
        },
    }
}

/// What follows from a set's keyframes alone, borrowed from the set: how many there are, the longhands they animate,
/// sorted, the raw fly strings of the names of the custom properties they animate, and what the set answers of them.
#[repr(C)]
pub struct FfiKeyframeSetFacts {
    pub keyframe_count: usize,
    pub target_properties: *const u16,
    pub target_property_count: usize,
    pub target_custom_properties: *const usize,
    pub target_custom_property_count: usize,
    pub animates_what_a_tick_cannot: bool,
    pub animates_an_inherited_property: bool,
    pub has_additive_keyframe: bool,
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
        keyframe_count: set.keyframes().len(),
        target_properties: set.target_properties().as_ptr(),
        target_property_count: set.target_properties().len(),
        target_custom_properties: set.target_custom_properties().as_ptr().cast::<usize>(),
        target_custom_property_count: set.target_custom_properties().len(),
        animates_what_a_tick_cannot: set.animates_what_a_tick_cannot(),
        animates_an_inherited_property: set.animates_an_inherited_property(),
        has_additive_keyframe: set.has_additive_keyframe(),
    }
}

/// The bytes a keyframe set holds, with the allocation the host's references share.
///
/// # Safety
/// `set` must be a keyframe set the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_keyframe_set_memory_size(set: *const KeyframeSet) -> usize {
    // SAFETY: Guaranteed by the caller.
    let set = unsafe { borrowed_keyframe_set(set) };
    crate::heap_size::ARC_HEADER_BYTES + size_of::<KeyframeSet>() + set.heap_size()
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
pub extern "C" fn animations_effect_facade(effect: &AnimationEffectCell) -> *mut c_void {
    effect.facade().map_or(std::ptr::null_mut(), NonNull::as_ptr)
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

/// The normalized timing the timing model reads of `effect`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_timing(effect: &AnimationEffectCell) -> AnimationEffectTiming {
    effect.borrow().timing()
}

/// The timing function of an effect, whose control points it borrows, and the raw fly string of its serialization,
/// which it borrows, until the timing function changes.
#[repr(C)]
pub struct FfiEffectTimingFunction {
    pub descriptor: FfiEasingDescriptor,
    pub serialization: usize,
}

/// The timing function of `effect`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_timing_function(effect: &AnimationEffectCell) -> FfiEffectTimingFunction {
    let effect = effect.borrow();
    FfiEffectTimingFunction {
        descriptor: effect.timing_function().descriptor(),
        serialization: effect.timing_function_serialization().raw(),
    }
}

/// The keyframe set `effect` runs, borrowed until its keyframes change, or null where it has none.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_keyframe_set(effect: &AnimationEffectCell) -> *const KeyframeSet {
    effect.borrow().keyframes().map_or(std::ptr::null(), Arc::as_ptr)
}

/// Whether the compositor may run all `effect` animates.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_may_run_on_the_compositor(effect: &AnimationEffectCell) -> bool {
    effect.borrow().may_run_on_the_compositor()
}

/// Sets the keyframe set `effect` runs to `set`, which it takes a reference of its own to, or to none where `set` is
/// null, answering whether it did. An effect whose keyframes from CSS script replaced takes no keyframes of CSS.
///
/// # Safety
/// `set` must be null or a keyframe set the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_effect_set_keyframe_set(
    effect: &AnimationEffectCell,
    set: *const KeyframeSet,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let keyframes = (!set.is_null()).then(|| unsafe { KeyframeSet::retained_from_host(set) });
    let mut effect = effect.borrow_mut();
    if effect.ignores_css_keyframes()
        && keyframes
            .as_ref()
            .is_some_and(|keyframes| keyframes.origin() != KeyframeOrigin::Script)
    {
        return false;
    }
    effect.set_keyframes(keyframes);
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_composite(effect: &AnimationEffectCell) -> FfiCompositeOperation {
    effect.borrow().composite()
}

#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_set_composite(effect: &AnimationEffectCell, composite: FfiCompositeOperation) {
    effect.borrow_mut().set_composite(composite);
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

/// Sets the target pseudo-selector of `effect`.
///
/// # Safety
/// The serialization of `pseudo_element` must be zero or a live raw fly string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_effect_set_pseudo_element(
    effect: &AnimationEffectCell,
    pseudo_element: FfiEffectPseudoElement,
) {
    // SAFETY: Guaranteed by the caller.
    let serialization = unsafe { RetainedUtf16FlyString::from_borrowed_raw(pseudo_element.serialization) };
    effect
        .borrow_mut()
        .set_pseudo_element(pseudo_element.kind, serialization);
}

/// Sets the phase and the current iteration `effect` had when the host last dispatched its events.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_set_previous_phase_and_current_iteration(
    effect: &AnimationEffectCell,
    phase: AnimationEffectPhase,
    current_iteration: f64,
) {
    let mut effect = effect.borrow_mut();
    effect.set_previous_phase(phase);
    effect.set_previous_current_iteration(current_iteration);
}

#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_has_flag(effect: &AnimationEffectCell, flag: EffectFlag) -> bool {
    effect.borrow().has_flag(flag)
}

/// Sets or clears `flag` of `effect`, answering whether that changed it.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_set_flag(effect: &AnimationEffectCell, flag: EffectFlag, value: bool) -> bool {
    effect.borrow_mut().set_flag(flag, value)
}

/// Has `effect` forget what the host concluded of it as it was, since it changed: what the host built of it for the
/// compositor, how the host ran or throttled it, and whether a frame can skip its style update.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_forget_what_the_host_concluded(effect: &AnimationEffectCell) {
    effect.borrow_mut().forget_what_the_host_concluded();
}

/// What `effect` remembers of whether a frame can skip its style update, for `key`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_per_frame_style_update_skip_decision(
    effect: &AnimationEffectCell,
    key: &PerFrameStyleUpdateSkipKey,
) -> PerFrameStyleUpdateSkipDecision {
    effect.borrow().per_frame_style_update_skip_decision(key)
}

/// Has `effect` remember whether a frame can skip its style update, as the host found it from `key`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_note_per_frame_style_update_skip_decision(
    effect: &AnimationEffectCell,
    key: &PerFrameStyleUpdateSkipKey,
    can_skip: bool,
) {
    effect
        .borrow_mut()
        .note_per_frame_style_update_skip_decision(*key, can_skip);
}

/// Asks the next style update for an observation sample of `effect` on behalf of a read of an element in the task of
/// generation `task_generation`, unless it was already asked for one in that task.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_request_observation_sample_in_task(
    effect: &AnimationEffectCell,
    task_generation: u64,
) -> ObservationSampleRequest {
    effect.borrow_mut().request_observation_sample_in_task(task_generation)
}

/// What the host built of `effect` for the compositor, reached as `access` says, or null where it built nothing and
/// `access` makes nothing. The state lives as long as the effect.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_compositor_animation_state(
    effect: &AnimationEffectCell,
    access: CompositorAnimationStateAccess,
) -> *mut CompositorAnimationEffectState {
    effect
        .borrow_mut()
        .compositor_animation_state(access)
        .map_or(std::ptr::null_mut(), std::ptr::from_mut)
}

/// The bytes the record of `effect` holds, without the keyframe set, which others share.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_memory_size(effect: &AnimationEffectCell) -> usize {
    effect.memory_size()
}

/// A new idle CSS animation with no associated effect, with the one reference to it the host takes over.
#[unsafe(no_mangle)]
pub extern "C" fn animations_css_animation_create() -> *const AnimationCell {
    AnimationHandle::new(AnimationKind::CssAnimation(CssAnimationState {
        name: RetainedUtf16FlyString::none(),
        // https://drafts.csswg.org/css-animations-1/#animation-timing-function
        // The initial value of animation-timing-function is ease.
        default_easing: CompactEasing::of(super::easing::keyword_cubic_bezier(0)),
        applied_definition: None,
        named_timeline: RetainedUtf16FlyString::none(),
        name_index: 0,
        last_css_play_state: None,
        flags: 0,
    }))
    .into_raw()
}

/// What a CSS transition is of and runs between, as the host starts one and reads it: the longhand by its id, or, where
/// `custom_name` is not zero, the custom property of that raw fly string, and the values, which a transition the host
/// reads lends it for as long as it holds the transition.
#[repr(C)]
pub struct FfiCssTransition {
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

/// A new idle CSS transition `transition` describes, with no associated effect, with the one reference to it the host
/// takes over. It is provisional until the host commits or discards it.
///
/// # Safety
/// `transition` must name live style values and a live raw fly string or zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_css_transition_create(transition: &FfiCssTransition) -> *const AnimationCell {
    // SAFETY: Guaranteed by the caller.
    let state = unsafe {
        CssTransitionState {
            custom_property_name: RetainedUtf16FlyString::from_borrowed_raw(transition.custom_name),
            generation: transition.generation,
            start_time: transition.start_time,
            end_time: transition.end_time,
            start_value: retained_value(transition.start_value),
            end_value: retained_value(transition.end_value),
            reversing_adjusted_start_value: retained_value(transition.reversing_adjusted_start_value),
            reversing_shortening_factor: transition.reversing_shortening_factor,
            property_id: transition.property_id,
            previous_phase: CSS_TRANSITION_PHASE_IDLE,
            provisional: true,
            adopted_from_lane: false,
            effect_started_with: None,
        }
    };
    AnimationHandle::new(AnimationKind::CssTransition(state)).into_raw()
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

/// A new animation of an SVG animation element on `timeline`, whose associated effect is a new keyframe effect as
/// `description` describes, with the one reference to it the host takes over. The host makes no `Animation` of it, and
/// its animation element holds it until it lets go of it.
///
/// # Safety
/// `description` must name live host objects and keyframes, `timeline` a timeline the host holds a reference to, and a
/// timing function, if any, must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_smil_animation_create(
    description: &FfiSmilAnimation,
    timeline: &TimelineCell,
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
    // SAFETY: The host holds a reference to the timeline it passes.
    let timeline = unsafe { TimelineHandle::retained_from_host(timeline) };
    run_procedure(&animation, perform, |procedures| {
        procedures.set_timeline(Some(timeline));
        Ok(())
    });
    run_procedure(&animation, perform, |procedures| {
        procedures.set_the_associated_effect(Some(&effect), Invalidate::Yes);
        Ok(())
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

/// Calls `visit` with `context` and each GC cell of the host the record of `animation` names, which the object of the
/// animation visits.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_visit_named_cells(
    animation: &AnimationCell,
    context: *mut c_void,
    visit: unsafe extern "C" fn(context: *mut c_void, cell: *mut c_void, kind: HostCellKind),
) {
    animation.for_each_named_cell(|cell, kind| {
        // SAFETY: The host visits a cell of an animation its object holds.
        unsafe { visit(context, cell.as_ptr(), kind) }
    });
}

/// A new effect with the default timing, no keyframes and no target, with the one reference to it the host takes over,
/// as a CSS animation or transition creates its associated effect.
#[unsafe(no_mangle)]
pub extern "C" fn animations_effect_create() -> *const AnimationEffectCell {
    EffectHandle::new().into_raw()
}

/// The host's effect of an effect the host passes, which is null or one it holds a reference to.
///
/// # Safety
/// `effect` must be null or an effect the host holds a reference to.
unsafe fn effect_from_host(effect: *const AnimationEffectCell) -> Option<EffectHandle> {
    // SAFETY: Guaranteed by the caller.
    (!effect.is_null()).then(|| unsafe { EffectHandle::retained_from_host(effect) })
}

/// https://www.w3.org/TR/web-animations-1/#setting-the-associated-effect
/// Sets the associated effect of `animation` to `effect`, or to none where it is null.
///
/// # Safety
/// `effect` must be null or an effect the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_animation_set_effect(
    animation: &AnimationCell,
    effect: *const AnimationEffectCell,
    should_invalidate: bool,
    perform: HostEffectPerformer,
) {
    // SAFETY: Guaranteed by the caller.
    let effect = unsafe { effect_from_host(effect) };
    run_procedure(animation, perform, |procedures| {
        procedures.set_the_associated_effect(effect.as_ref(), invalidate(should_invalidate));
        Ok(())
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

/// What the host's per-frame decisions read of `animation` now, with its current time at `timestamp` where
/// `has_timestamp`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_timing_state(
    animation: &AnimationCell,
    has_timestamp: bool,
    timestamp: f64,
) -> FfiAnimationTimingState {
    animation.borrow().timing_state(has_timestamp.then_some(timestamp))
}

/// The playback state of `animation`, which the timing model reads.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_playback(animation: &AnimationCell) -> AnimationPlayback {
    *animation.borrow().playback()
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
/// For the host's `DOM::Document` `document`, whose associated timelines are the `timeline_count` of `timelines`,
/// comparing elements in tree order with `tree_order`, and has the host keep each animation it removes alive through
/// `keep_alive` with `context` and perform what that asks of it through `perform`.
///
/// # Safety
/// `timelines` must point to `timeline_count` timelines the host holds references to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_remove_replaced_animations(
    document: NonNull<c_void>,
    timelines: *const &TimelineCell,
    timeline_count: usize,
    tree_order: FfiTreeOrder,
    keep_alive: FfiKeepAlive,
    context: *mut c_void,
    perform: HostEffectPerformer,
) {
    // SAFETY: Guaranteed by the caller.
    let timelines = unsafe { ak::slice_from_raw_parts(timelines, timeline_count) };
    let effects = procedures::remove_replaced_animations(
        timelines.iter().copied(),
        |element| element_node_document(element) == document,
        &mut host_tree_order(tree_order),
    );
    keep_alive_for_host_effects(&effects, keep_alive, context);
    perform_host_effects(effects, perform);
}

/// Disassociates `animation`, on its timeline, if it has one, from the target of its associated effect where it is
/// inert, where a procedure scheduled that.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_disassociate_from_target_if_inert(
    animation: &AnimationCell,
    perform: HostEffectPerformer,
) {
    if !animation.borrow().disassociation_from_target_pending() {
        return;
    }
    run_procedure(animation, perform, |procedures| {
        procedures.disassociate_from_target_if_inert();
        Ok(())
    });
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
        Ok(())
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

/// Sets the owning element of `animation`, which the host keeps alive while the animation holds it, or to none where
/// its element is null.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_set_owning_element(animation: &AnimationCell, owner: FfiOwningElement) {
    animation
        .borrow_mut()
        .set_owning_element(std::ptr::NonNull::new(owner.element).map(|element| OwningElement {
            element,
            pseudo_element: owner.has_pseudo_element.then_some(owner.pseudo_element),
        }));
}

/// The state of a CSS animation as the host reads it: the raw fly string of its name, borrowed until it changes, or
/// zero.
#[repr(C)]
pub struct FfiCssAnimationState {
    pub name: usize,
    /// The raw fly string of the name of the named timeline the animation follows, or zero.
    pub named_timeline: usize,
    /// The easing a keyframe of the animation that declares none of its own runs, whose control points it borrows
    /// until the easing changes.
    pub default_easing: FfiEasingDescriptor,
}

/// The state of the CSS animation `animation`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_css_animation_state(animation: &AnimationCell) -> FfiCssAnimationState {
    let animation = animation.borrow();
    let state = animation.css_animation();
    FfiCssAnimationState {
        name: state.name.raw(),
        named_timeline: state.named_timeline.raw(),
        default_easing: state.default_easing.descriptor(),
    }
}

/// Applies the timing, attachment range, easing, composite operation and play state the style computation computed in
/// `definition` to the CSS animation `animation`, whose timeline the host updated, having the host perform what that
/// asks for through `perform`.
/// https://drafts.csswg.org/css-animations-2/#animations
///
/// # Safety
/// The values `definition` names must be null or live style values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_css_animation_apply_properties(
    animation: &AnimationCell,
    definition: &crate::css::style_compute::FfiComputedAnimation,
    perform: HostEffectPerformer,
) {
    use crate::css::css_enums::{animation_composition, animation_direction, animation_fill_mode};
    use libcompositing_rust::animation_timing::effect_timing::{
        FillMode as SpecifiedFillMode, PlaybackDirection, SpecifiedIterationDuration,
    };

    let has_flag = |flag: CssAnimationFlag| animation.borrow().css_animation().flags & flag as u8 != 0;

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
        run_procedure(animation, perform, |procedures| {
            procedures.attachment_range_changed();
            Ok(())
        });
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
        Ok(())
    });

    if animation.borrow().css_animation().last_css_play_state == Some(definition.play_state) {
        return;
    }
    if !has_flag(CssAnimationFlag::ScriptOverrodePlayState) {
        let play_state = animation.borrow().timing_state(None).play_state;
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

/// Whether `flag` of the CSS animation `animation` is set.
#[unsafe(no_mangle)]
pub extern "C" fn animations_css_animation_has_flag(animation: &AnimationCell, flag: CssAnimationFlag) -> bool {
    animation.borrow().css_animation().flags & flag as u8 != 0
}

/// Sets the name of the CSS animation `animation` to the raw fly string `name`.
///
/// # Safety
/// `name` must be a live raw fly string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_css_animation_set_name(animation: &AnimationCell, name: usize) {
    // SAFETY: Guaranteed by the caller.
    let name = unsafe { RetainedUtf16FlyString::from_borrowed_raw(name) };
    animation.borrow_mut().css_animation_mut().name = name;
}

/// Sets the position of the CSS animation `animation` in `animation-name`, answering whether that changed it.
#[unsafe(no_mangle)]
pub extern "C" fn animations_css_animation_set_name_index(animation: &AnimationCell, name_index: u32) -> bool {
    std::mem::replace(&mut animation.borrow_mut().css_animation_mut().name_index, name_index) != name_index
}

/// Notes that the style computation applied `definition` to the CSS animation `animation`, which follows the named
/// timeline of the raw fly string `named_timeline` where that is not zero, answering whether that sets the same
/// properties as the definition it last applied, whatever keyframes each gives the animation's effect.
///
/// # Safety
/// The values `definition` names must be null or live style values, its keyframe set null or one the host holds a
/// reference to, and `named_timeline` zero or a live raw fly string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_css_animation_apply_definition(
    animation: &AnimationCell,
    definition: &crate::css::style_compute::FfiComputedAnimation,
    named_timeline: usize,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let (definition, named_timeline) = unsafe {
        (
            Arc::new(AppliedAnimationDefinition::retained(definition)),
            RetainedUtf16FlyString::from_borrowed_raw(named_timeline),
        )
    };
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

/// The CSS transition `animation`, which lends the host its values and the name of its custom property for as long as
/// the host holds it.
#[unsafe(no_mangle)]
pub extern "C" fn animations_css_transition(animation: &AnimationCell) -> FfiCssTransition {
    let animation = animation.borrow();
    let state = animation.css_transition();
    FfiCssTransition {
        generation: state.generation,
        start_time: state.start_time,
        end_time: state.end_time,
        reversing_shortening_factor: state.reversing_shortening_factor,
        custom_name: state.custom_property_name.raw(),
        start_value: state.start_value.pointer().cast(),
        end_value: state.end_value.pointer().cast(),
        reversing_adjusted_start_value: state.reversing_adjusted_start_value.pointer().cast(),
        property_id: state.property_id,
    }
}

/// The timing a CSS transition starts with: its delay and duration, in milliseconds, its timing function, and the raw
/// fly string of the serialization of the value that was made from.
#[repr(C)]
pub struct FfiProvisionalTransitionTiming {
    pub delay: f64,
    pub duration: f64,
    pub timing_function: FfiEasingDescriptor,
    pub timing_function_serialization: usize,
}

/// https://drafts.csswg.org/css-transitions/#starting
/// Starts the CSS transition `animation`, on its timeline, provisionally, with `effect`, which targets its owning
/// element and runs its keyframes, and the timing `timing`.
///
/// # Safety
/// The serialization `timing` names must be a live raw fly string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_css_transition_start_provisionally(
    animation: &AnimationCell,
    effect: &AnimationEffectCell,
    timing: &FfiProvisionalTransitionTiming,
    perform: HostEffectPerformer,
) {
    // SAFETY: The host holds a reference to the effect it passes, and the caller guarantees the serialization.
    let (effect, timing) = unsafe {
        (
            EffectHandle::retained_from_host(effect),
            ProvisionalTransitionTiming {
                delay: timing.delay,
                duration: timing.duration,
                timing_function: CompactEasing::of(Easing::from_descriptor(&timing.timing_function)),
                timing_function_serialization: RetainedUtf16FlyString::from_borrowed_raw(
                    timing.timing_function_serialization,
                ),
            },
        )
    };
    run_procedure(animation, perform, |procedures| {
        procedures.start_a_provisional_transition(&effect, timing);
        Ok(())
    });
}

/// Discards the CSS transition `animation`, which was started provisionally and has no timeline any more.
#[unsafe(no_mangle)]
pub extern "C" fn animations_css_transition_discard_provisional(animation: &AnimationCell) {
    // SAFETY: The host holds a reference to the animation it passes.
    let animation = unsafe { AnimationHandle::retained_from_host(animation) };
    procedures::run(&animation, |procedures| {
        procedures.discard_provisional_transition();
        Ok(())
    })
    .expect("discarding a provisional transition does not throw");
}

/// Commits the CSS transition `animation`, which was started provisionally, as the running transition of its property
/// the owner at `slot` of `animations` holds, where `animations` is not null, which `origin` started, and has the host
/// associate it with its target through `perform`.
///
/// # Safety
/// A non-null `animations` must be lists the host holds, which nothing else refers to for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_css_transition_commit_provisional(
    animation: &AnimationCell,
    animations: *mut ElementAnimations,
    slot: AnimationOwnerSlot,
    origin: TransitionOrigin,
    perform: HostEffectPerformer,
) {
    // SAFETY: The host holds a reference to the animation it passes.
    let animation = unsafe { AnimationHandle::retained_from_host(animation) };
    let effects = {
        // SAFETY: Guaranteed by the caller.
        let animations = unsafe { animations.as_mut() };
        transitions::commit_provisional_transition(animations.map(|animations| (animations, slot)), animation, origin)
    };
    // NB: Associating the transition with its target reaches the lists again.
    perform_host_effects(effects, perform);
}

/// Writes to `existing` what a style change event at time `t` of the document timeline reads of the CSS transition
/// `animation`, which its target holds, whose values it lends for as long as the host holds the transition. The host
/// gives the current value of a running transition of a custom property.
#[unsafe(no_mangle)]
pub extern "C" fn animations_css_transition_existing(
    animation: &AnimationCell,
    t: f64,
    existing: &mut FfiExistingTransition,
) {
    *existing = animation.borrow().existing_transition(t);
}

/// Whether the CSS transition `animation` already runs from when the hover of a clock lane started it, as one of the
/// `lane_transitions` the lane leaves its element running does.
#[unsafe(no_mangle)]
pub extern "C" fn animations_css_transition_runs_as_lane_does(
    animation: &AnimationCell,
    lane_transitions: ak::Span<'_, FfiLaneTransition>,
) -> bool {
    animation.borrow().runs_as_lane_does(lane_transitions.as_slice())
}

/// Whether `lane_transition` is the run of the host's CSS transition `animation` by the hover of a clock lane.
#[unsafe(no_mangle)]
pub extern "C" fn animations_css_transition_is_host_transition_of(
    animation: &AnimationCell,
    lane_transition: &FfiLaneTransition,
) -> bool {
    animation.borrow().is_host_transition_of(lane_transition)
}

/// What becomes of the running CSS transition `animation` as the host takes in `lane_transitions`, which the hover of a
/// clock lane leaves its element running, where the lane's steps ended the transitions whose effects `cancelled` names
/// and saw those `seen` names.
#[unsafe(no_mangle)]
pub extern "C" fn animations_css_transition_lane_adoption(
    animation: &AnimationCell,
    lane_transitions: ak::Span<'_, FfiLaneTransition>,
    cancelled: ak::Span<'_, AnimationEffectId>,
    seen: ak::Span<'_, AnimationEffectId>,
) -> LaneAdoption {
    animation
        .borrow()
        .lane_adoption(lane_transitions.as_slice(), cancelled.as_slice(), seen.as_slice())
}

/// The host's `SVGAnimationElement` whose animation the SMIL animation `animation` is.
#[unsafe(no_mangle)]
pub extern "C" fn animations_smil_animation_element(animation: &AnimationCell) -> *mut c_void {
    match animation.borrow().kind() {
        AnimationKind::Smil { animation_element, .. } => animation_element.as_ptr(),
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
/// The relative composite order of `a` and `b`, as a negative number where `a` sorts first, a positive one where it
/// sorts last, and zero where they are the same animation, with `tree_order` comparing elements in tree order.
#[unsafe(no_mangle)]
pub extern "C" fn animations_composite_order(a: &AnimationCell, b: &AnimationCell, tree_order: FfiTreeOrder) -> i32 {
    if std::ptr::eq(a, b) {
        return 0;
    }
    composite_order(&a.borrow(), &b.borrow(), &mut host_tree_order(tree_order)) as i32
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

/// The set `kind` of the animations a document knows of, which lives as long as `animations`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_animations_set(
    animations: &mut DocumentAnimations,
    kind: DocumentAnimationSetKind,
) -> *mut WeakAnimationSet {
    animations.set_mut(kind)
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
    animations.pending_events.append(
        PendingAnimationEvent {
            animation,
            event: *event,
        },
        provisional,
    );
}

/// What queueing the events an animation frame established for an animation did: what made the animation, which only
/// has events where it is a CSS animation or a CSS transition, and how many it queued.
#[repr(C)]
pub struct FfiQueuedCssEvents {
    pub kind: FfiAnimationKind,
    pub count: u8,
}

/// https://drafts.csswg.org/css-transitions-2/#event-dispatch
/// https://www.w3.org/TR/css-animations-2/#event-dispatch
/// Appends the events the animation frame just established for `animation` to the pending animation event queue of a
/// document, or, where `provisional`, to the events it queues during its style stabilization epoch, each with the time
/// stamp `time_stamp` answers for the owning element of the animation.
///
/// # Safety
/// `animation` must be an animation the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_queue_css_events(
    animations: &mut DocumentAnimations,
    animation: *const AnimationCell,
    provisional: bool,
    time_stamp: unsafe extern "C" fn(element: NonNull<c_void>) -> f64,
) -> FfiQueuedCssEvents {
    // SAFETY: Guaranteed by the caller.
    let animation = unsafe { AnimationHandle::retained_from_host(animation) };
    let events = animation.borrow_mut().take_css_events();
    let queued = FfiQueuedCssEvents {
        kind: events.kind,
        count: events.count,
    };
    if events.count == 0 {
        return queued;
    }
    let owner = animation
        .borrow()
        .owning_element()
        .expect("a CSS animation or transition with events has an owning element");
    // SAFETY: The animation keeps its owning element alive, and the host answers the time stamp of a live element.
    let time_stamp = unsafe { time_stamp(owner.element) };
    for css_event in &events.events[..usize::from(events.count)] {
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
        animations.pending_events.append(
            PendingAnimationEvent {
                animation: animation.clone(),
                event,
            },
            provisional,
        );
    }
    queued
}

/// https://www.w3.org/TR/web-animations-1/#update-animations-and-send-events
/// Takes the events of the pending animation event queue of a document in the order to dispatch them in, comparing
/// elements in tree order with `tree_order`, has the host keep the animation of each alive through `keep_alive` with
/// `context`, and has it dispatch each through `dispatch` with `context`.
///
/// # Safety
/// `animations` must be what a live document knows of animations, which `dispatch` may reach again, and `context` must
/// be what `keep_alive` and `dispatch` expect.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_dispatch_events(
    animations: *mut DocumentAnimations,
    tree_order: FfiTreeOrder,
    keep_alive: FfiKeepAlive,
    dispatch: unsafe extern "C" fn(context: *mut c_void, animation: &AnimationCell, event: &FfiPendingAnimationEvent),
    context: *mut c_void,
) {
    // NB: Steps 4 to 6 take and sort the events, which dispatching them leaves to the host.
    let events_to_dispatch = {
        // SAFETY: Guaranteed by the caller; nothing else reaches the queue until the events are taken.
        let animations = unsafe { &mut *animations };
        animations
            .pending_events
            .take_events_to_dispatch(&mut host_tree_order(tree_order))
    };
    // NB: Dispatching an event runs script, which can run the GC, which must not collect the animation of an event
    //     yet to be dispatched.
    for event in &events_to_dispatch {
        // SAFETY: The host keeps alive an animation it is handed for as long as the call that handed it lasts.
        unsafe { keep_alive(context, &event.animation) };
    }
    // 7. Dispatch each of the events in events to dispatch at their corresponding target using the order established in
    //    the previous step.
    for event in &events_to_dispatch {
        // SAFETY: Guaranteed by the caller.
        unsafe { dispatch(context, &event.animation, &event.event) };
    }
}

/// Calls `visit` with `context` and each GC cell of the host that keeps alive the elements the records of the events a
/// document queued name, which the document visits as the GC traces it.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_animations_visit_pending_events(
    animations: &DocumentAnimations,
    context: *mut c_void,
    visit: unsafe extern "C" fn(context: *mut c_void, cell: *mut c_void, kind: HostCellKind),
) {
    animations.pending_events.for_each_host_cell(|cell, kind| {
        // SAFETY: The host visits a cell of an animation it queued an event of.
        unsafe { visit(context, cell.as_ptr(), kind) }
    });
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

/// Removes `effect` from the set `kind` of the effects a document knows of, answering whether the set held it.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_effect_set_remove(
    animations: &mut DocumentAnimations,
    kind: DocumentEffectSetKind,
    effect: &AnimationEffectCell,
) -> bool {
    animations.effect_set_mut(kind).remove(effect)
}

/// Whether the set `kind` of the effects a document knows of holds `effect`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_effect_set_contains(
    animations: &mut DocumentAnimations,
    kind: DocumentEffectSetKind,
    effect: &AnimationEffectCell,
) -> bool {
    animations.effect_set_mut(kind).contains(effect)
}

/// Empties the set `kind` of the effects a document knows of, calling `callback` with `context` and each effect alive
/// to it, which the effect stays alive for the call of.
///
/// # Safety
/// `callback` must not reach the effects the document knows of, and `context` must be what it expects.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_effect_set_take_each(
    animations: &mut DocumentAnimations,
    kind: DocumentEffectSetKind,
    context: *mut c_void,
    callback: unsafe extern "C" fn(*mut c_void, &AnimationEffectCell),
) {
    animations.effect_set_mut(kind).take_each(|effect| {
        // SAFETY: Guaranteed by the caller.
        unsafe { callback(context, effect) };
    });
}

/// Adds `animation` to `set`, answering whether `set` did not hold it yet.
///
/// # Safety
/// `animation` must be an animation the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_weak_animation_set_insert(
    set: &mut WeakAnimationSet,
    animation: *const AnimationCell,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    set.insert(&unsafe { AnimationHandle::retained_from_host(animation) })
}

/// Removes `animation` from `set`, answering whether `set` held it.
#[unsafe(no_mangle)]
pub extern "C" fn animations_weak_animation_set_remove(set: &mut WeakAnimationSet, animation: &AnimationCell) -> bool {
    set.remove(animation)
}

/// Whether `set` holds `animation`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_weak_animation_set_contains(set: &WeakAnimationSet, animation: &AnimationCell) -> bool {
    set.contains(animation)
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

/// The bytes the record of `animation` holds, without its associated effect's.
#[unsafe(no_mangle)]
pub extern "C" fn animations_animation_memory_size(animation: &AnimationCell) -> usize {
    animation.memory_size()
}

/// New empty lists of the animations of an element, which the element owns.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_create() -> *mut ElementAnimations {
    Box::into_raw(Box::default())
}

/// Destroys the lists of the animations of an element, giving up the references they hold.
///
/// # Safety
/// `animations` must be lists `animations_element_animations_create` handed out, which the host gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_element_animations_destroy(animations: *mut ElementAnimations) {
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { Box::from_raw(animations) });
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

/// Associates `animation` with the element, answering whether it was not associated yet.
///
/// # Safety
/// `animation` must be an animation the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_element_animations_associate(
    animations: &mut ElementAnimations,
    animation: *const AnimationCell,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let animation = unsafe { AnimationHandle::retained_from_host(animation) };
    if !animations.associate(animation.clone()) {
        return false;
    }
    animation.borrow_mut().did_associate_with_target();
    true
}

/// Disassociates `animation` from the element, answering whether it was associated.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_disassociate(
    animations: &mut ElementAnimations,
    animation: &AnimationCell,
) -> bool {
    animations.disassociate(animation)
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

/// Sets the CSS animations of the owner at `slot` to `records`, which it takes references of its own to, and publishes
/// them to the style engine of `host`, which knows the element as `node`, where the host names one.
///
/// # Safety
/// Each of `records` must be an animation the host holds a reference to, and a non-null `host` must be live for the
/// call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_element_animations_set_css_animations(
    animations: &mut ElementAnimations,
    slot: AnimationOwnerSlot,
    records: ak::Span<'_, *const AnimationCell>,
    host: *const DocumentHost,
    node: u32,
) {
    animations.set_css_animations(
        slot,
        records
            .as_slice()
            .iter()
            // SAFETY: Guaranteed by the caller.
            .map(|&record| unsafe { AnimationHandle::retained_from_host(record) }),
        // SAFETY: Guaranteed by the caller.
        unsafe { style_engine_target(host, node) },
    );
}

/// Whether the element has had CSS animations since it last cancelled them, or has transitions.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_has_css_animations_or_transitions(
    animations: &ElementAnimations,
) -> bool {
    animations.has_css_animations_or_transitions()
}

/// The property a CSS transition runs: the host's `PropertyID`, and the raw fly string of the name of a custom property,
/// or zero.
#[repr(C)]
pub struct FfiTransitionProperty {
    pub property_id: u16,
    pub custom_name: usize,
}

impl From<FfiTransitionProperty> for TransitionProperty {
    fn from(property: FfiTransitionProperty) -> Self {
        Self {
            property_id: property.property_id,
            custom_name: property.custom_name,
        }
    }
}

/// The record of the transition of `property` the owner at `slot` has, which the lists hold, or null.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_transition(
    animations: &ElementAnimations,
    slot: AnimationOwnerSlot,
    property: FfiTransitionProperty,
) -> *const AnimationCell {
    animations
        .transition(slot, property.into())
        .map_or(std::ptr::null(), |transition| &raw const **transition)
}

/// Removes the transition of `property` the owner at `slot` has, which is disassociated from its target once it is
/// inert.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_remove_transition(
    animations: &mut ElementAnimations,
    slot: AnimationOwnerSlot,
    property: FfiTransitionProperty,
) {
    animations.remove_transition(slot, property.into());
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
                let ((), effects) = procedures::run(animation, |procedures| {
                    if plays {
                        procedures.play_from_css()
                    } else {
                        procedures.pause_from_css()
                    }
                })
                .expect("playing or pausing an animation from CSS does not throw");
                perform_host_effects(effects, perform);
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
    let taken =
        unsafe { &mut *animations }.take_css_animations_and_transitions(unsafe { style_engine_target(host, node) });
    for animation in &taken {
        // SAFETY: The host keeps alive an animation it is handed for as long as this call lasts.
        unsafe { keep_alive(context, animation) };
    }
    for animation in &taken {
        let cancel = || {
            let ((), effects) = procedures::run(animation, |procedures| {
                procedures.cancel(Invalidate::No);
                Ok(())
            })
            .expect("cancelling an animation does not throw");
            perform_host_effects(effects, perform);
        };
        // NB: Cancelling an animation that is not idle settles its promises and creates its cancel event.
        let (play_state, _) = procedures::run(animation, |procedures| Ok(procedures.play_state()))
            .expect("reading the play state does not throw");
        if play_state == AnimationPlayState::Idle {
            cancel();
        } else {
            in_host_execution_context(in_execution_context, context, animation, cancel);
        }
    }
}

/// The bytes the lists of the animations of an element hold, without the animations.
#[unsafe(no_mangle)]
pub extern "C" fn animations_element_animations_memory_size(animations: &ElementAnimations) -> usize {
    animations.memory_size()
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
}

/// The host's `DOM::Document` that is the node document of the host's `DOM::Element` `element`, which something the
/// caller holds keeps alive.
/// https://dom.spec.whatwg.org/#concept-node-document
fn element_node_document(element: NonNull<c_void>) -> NonNull<c_void> {
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

    /// The inset `inset` describes, whose calculations it borrows.
    fn borrowing(inset: &ViewTimelineInset) -> Self {
        let edge = |edge: &ViewTimelineInsetEdge| FfiViewTimelineInsetEdge {
            is_auto: edge.is_auto,
            px: edge.px,
            percentage: edge.percentage,
            calculated: edge.calculated.pointer().cast(),
        };
        Self {
            start: edge(&inset.start),
            end: edge(&inset.end),
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
pub(crate) unsafe fn create_timeline(
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

/// The host's `DOM::Document` that `timeline` is associated with.
#[unsafe(no_mangle)]
pub extern "C" fn animations_timeline_document(timeline: &TimelineCell) -> *mut c_void {
    timeline.document().as_ptr()
}

/// The scroll offsets at the start and at the end of the cover range of a view timeline, in CSS pixels.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FfiViewTimelineCoverRange {
    pub start: f64,
    pub end: f64,
}

/// What the host reads of a timeline: what the timing model reads of it, with its duration and what moves it as a
/// clock tick samples it, what script observed of it, and the state of a scroll or view timeline.
#[repr(C)]
pub struct FfiTimeline {
    pub id: AnimationTimelineId,
    pub kind: FfiTimelineKind,
    /// Its current time is unresolved where the timeline is inactive.
    pub sample: TimelineSample,
    pub is_inactive: bool,
    pub duration: OptionalTimeValue,
    pub clock: TimelineClock,
    /// The current time a style sampling reads instead, where `has_style_sampling_override`.
    pub has_style_sampling_override: bool,
    pub style_sampling_override: OptionalTimeValue,
    pub can_sample_current_time_at_timestamp: bool,
    /// The task generation of the event loop in which script last observed the timeline, where
    /// `has_observation_task_generation`, and the current time it observed then.
    pub has_observation_task_generation: bool,
    pub observation_task_generation: u64,
    pub observed_current_time: OptionalTimeValue,
    /// The task generation in which script observed the timeline at a time ahead of its current time, where
    /// `was_observed_ahead`.
    pub was_observed_ahead: bool,
    pub observed_ahead_task_generation: u64,
    pub source: FfiScrollTimelineSource,
    pub axis: u8,
    /// The inset of a view timeline, whose calculations it borrows until the inset changes.
    pub inset: FfiViewTimelineInset,
    /// The cover range of a view timeline as its progress was last calculated, where `has_cover_range`.
    pub has_cover_range: bool,
    pub cover_range: FfiViewTimelineCoverRange,
}

/// What the host reads of `timeline`.
#[unsafe(no_mangle)]
pub extern "C" fn animations_timeline_read(timeline: &TimelineCell) -> FfiTimeline {
    let sample = timeline.sample();
    let is_inactive = timeline.is_inactive();
    let clock = timeline.clock();
    let record = timeline.borrow();
    let observation = record.observed_in_task_generation();
    let observed_ahead = record.observed_ahead_in_task_generation();
    let style_sampling_override = record.style_sampling_override();
    let no_edge = FfiViewTimelineInsetEdge {
        is_auto: true,
        px: 0.0,
        percentage: 0.0,
        calculated: std::ptr::null(),
    };
    let mut read = FfiTimeline {
        id: record.id(),
        kind: match &record.kind {
            TimelineKind::Document { .. } => FfiTimelineKind::Document,
            TimelineKind::Scroll(scroll) if scroll.view.is_some() => FfiTimelineKind::View,
            TimelineKind::Scroll(_) => FfiTimelineKind::Scroll,
            TimelineKind::Internal { .. } => FfiTimelineKind::Internal,
        },
        sample,
        is_inactive,
        duration: record.duration().into(),
        clock,
        has_style_sampling_override: style_sampling_override.is_some(),
        style_sampling_override: style_sampling_override.flatten().into(),
        can_sample_current_time_at_timestamp: record.can_sample_current_time_at_timestamp(),
        has_observation_task_generation: observation.0.is_some(),
        observation_task_generation: observation.0.unwrap_or(0),
        observed_current_time: observation.1.into(),
        was_observed_ahead: observed_ahead.is_some(),
        observed_ahead_task_generation: observed_ahead.unwrap_or(0),
        source: ScrollTimelineSource::Element(None).into(),
        axis: 0,
        inset: FfiViewTimelineInset {
            start: no_edge,
            end: no_edge,
        },
        has_cover_range: false,
        cover_range: FfiViewTimelineCoverRange::default(),
    };
    if let Some(scroll) = record.scroll() {
        read.source = scroll.source.into();
        read.axis = scroll.axis;
        if let Some(view) = &scroll.view {
            read.inset = FfiViewTimelineInset::borrowing(&view.inset);
            if let Some(metrics) = view.last_metrics {
                read.has_cover_range = true;
                read.cover_range = FfiViewTimelineCoverRange {
                    start: metrics.cover_start(),
                    end: metrics.cover_end(),
                };
            }
        }
    }
    read
}

/// The boxes a scroll timeline follows, as the host found them from its source in the render state of `host`'s
/// document, which it reads in `read`: the box of the scroll container, if any, the viewport's where the source is the
/// root element, its scroll node, where `has_scroll_node`, and the style node of the element whose box scrolls, or 0
/// for the viewport. For a view timeline, the box of its subject, if any, and the style node by which a clock tick finds
/// it, or 0 where the subject is a pseudo-element. `host` and `read` are null where the source has no box.
#[repr(C)]
pub struct FfiScrollTimelineBoxes {
    pub host: *const DocumentHost,
    pub read: *const BegunRead,
    pub scroller: NodeSlotId,
    pub has_scroll_node: bool,
    pub scroll_node: ScrollNodeStableId,
    pub scroller_style_node: u32,
    pub subject: NodeSlotId,
    pub subject_style_node: u32,
}

/// The host's function that updates the animations associated with its timeline `facade`.
pub type TimelineAnimationsUpdater = extern "C" fn(facade: *mut c_void);

/// Updates the current time of `timeline` in the task of `task_generation`: a document timeline's from the now
/// timestamp `timestamp`, and a scroll timeline's from where the render state lays out the boxes it follows, `boxes`,
/// unless that leaves it as it was where `only_if_stale`. Has `update_associated_animations` update the animations
/// associated with the timeline as its current time changes, and answers whether the timeline was updated.
///
/// # Safety
/// The host and the read of `boxes` must be null or a live document host and a read the host began of its render
/// state, in which the slots of `boxes` name its rows.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_timeline_update_current_time(
    timeline: &TimelineCell,
    timestamp: f64,
    boxes: Option<&FfiScrollTimelineBoxes>,
    task_generation: u64,
    only_if_stale: bool,
    update_associated_animations: TimelineAnimationsUpdater,
) -> bool {
    let facade = timeline.facade().expect("the host updates a live timeline");
    let update = || update_associated_animations(facade.as_ptr());
    let Some(boxes) = boxes else {
        timeline.update_current_time(TimelineUpdate::Timestamp(timestamp), task_generation, false, update);
        return true;
    };
    let subject_style_node = StyleNodeID::from_raw(boxes.subject_style_node);
    // SAFETY: Guaranteed by the caller.
    let (scroller, subject) = match unsafe { (boxes.host.as_ref(), boxes.read.as_ref()) } {
        (Some(host), Some(read)) => {
            let found = ScrollTimelineBoxes {
                scroller: boxes.scroller,
                scroll_node: boxes.has_scroll_node.then_some(boxes.scroll_node),
                scroller_style_node: boxes.scroller_style_node,
                subject: boxes.subject,
            };
            let record = timeline.borrow();
            let scroll = record.scroll().expect("the host lays out scroll timelines");
            host.ask(read, |state| {
                let arena = state.arena_mut();
                arena.settle_scrollable_overflow();
                scroll.measure(arena, &found)
            })
        }
        _ => (None, None),
    };
    timeline.update_current_time(
        TimelineUpdate::Layout {
            scroller: scroller.as_ref(),
            subject: subject.as_ref(),
            subject_style_node,
        },
        task_generation,
        only_if_stale,
        update,
    )
}

/// Sets the current time of the Internals interface's timeline `timeline` to `time`, in milliseconds, in the task of
/// `task_generation`, having `update_associated_animations` update its animations, or, where `for_observation`, the
/// time a read of it observes.
#[unsafe(no_mangle)]
pub extern "C" fn animations_internal_timeline_set_time(
    timeline: &TimelineCell,
    time: OptionalNumber,
    for_observation: bool,
    task_generation: u64,
    update_associated_animations: TimelineAnimationsUpdater,
) {
    let facade = timeline.facade().expect("the host sets the time of a live timeline");
    timeline.set_internal_time(time.get(), for_observation, task_generation, || {
        update_associated_animations(facade.as_ptr());
    });
}

/// The current time at the timestamp `timestamp` of `timeline`, where it can sample it there, and its current time
/// otherwise. Where `observed`, script observes it in the task of `task_generation`, which the timeline keeps.
#[unsafe(no_mangle)]
pub extern "C" fn animations_timeline_current_time_at_timestamp(
    timeline: &TimelineCell,
    timestamp: f64,
    observed: bool,
    task_generation: u64,
) -> OptionalTimeValue {
    timeline
        .current_time_at_timestamp(timestamp, observed.then_some(task_generation))
        .into()
}

/// Has a style sampling read `current_time` as the current time of `timeline` where `has_override`, and its current
/// time otherwise.
#[unsafe(no_mangle)]
pub extern "C" fn animations_timeline_set_style_sampling_override(
    timeline: &TimelineCell,
    has_override: bool,
    current_time: OptionalTimeValue,
) {
    timeline
        .borrow_mut()
        .set_style_sampling_override(has_override.then(|| current_time.get()));
}

/// The animations associated with `timeline`, which lives as long as the timeline.
#[unsafe(no_mangle)]
pub extern "C" fn animations_timeline_associated_animations(timeline: &TimelineCell) -> *mut WeakAnimationSet {
    timeline.associated_animations().as_ptr()
}

/// Sets the axis of the scroll timeline `timeline` to `axis`, the host's `ScrollAxis`, and the inset of a view
/// timeline to `inset`, answering whether that changed either.
///
/// # Safety
/// The calculations of `inset` must be null or live style values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_scroll_timeline_set_axis_and_inset(
    timeline: &TimelineCell,
    axis: u8,
    inset: Option<&FfiViewTimelineInset>,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let inset = inset.map(|inset| unsafe { inset.retained() });
    timeline.set_axis_and_inset(axis, inset)
}

/// https://www.w3.org/TR/web-animations-1/#timeline-time-to-origin-relative-time
/// The origin-relative time of the timeline time `timeline_time` of `timeline`, which is unresolved where the timeline
/// defines no procedure to convert it.
#[unsafe(no_mangle)]
pub extern "C" fn animations_timeline_convert_to_origin_relative_time(
    timeline: &TimelineCell,
    timeline_time: OptionalTimeValue,
) -> OptionalNumber {
    timeline
        .convert_a_timeline_time_to_an_origin_relative_time(timeline_time.get())
        .into()
}
