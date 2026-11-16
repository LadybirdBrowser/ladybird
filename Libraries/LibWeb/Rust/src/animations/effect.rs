/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Animation effects.
//! https://www.w3.org/TR/web-animations-1/#animation-effects

use super::animation::{AnimationCell, AnimationHandle, WeakAnimationHandle};
use super::easing::CompactEasing;
use super::keyframes::{KeyframeOrigin, KeyframeSet};
use super::timeline_state::{TimelineState, time_unit};
use super::timing::{self, AnimationEffectPhase, AnimationEffectTiming, FillMode, SpecifiedEffectTiming, TimeValue};
use super::weak_record_set::WeakRecordSet;
use crate::css::animation::FfiCompositeOperation;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style::effect_descriptions::{EffectAnimation, EffectDescription};
use crate::painting::visual_animation_builder::CompositorAnimationEffectState;
use std::cell::{Cell, Ref, RefCell, RefMut};
use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

/// The identity of an animation effect, which no other effect of the process shares.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct AnimationEffectId(pub u64);

impl AnimationEffectId {
    fn mint() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(1);
        Self(NEXT.fetch_add(1, Ordering::Relaxed))
    }
}

/// What the host notes of an effect as it runs it on the compositor, throttles it, or samples it for script, by bit.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EffectFlag {
    /// A compositor animation runs the effect.
    CompositorDriven = 1 << 0,
    /// A compositor animation runs what the effect animates in its place.
    CompositorReplaced = 1 << 1,
    /// Nothing the effect animates shows, as its target is offscreen.
    OffscreenThrottled = 1 << 2,
    /// Script can observe what the compositor animation running the effect shows.
    ObservationRelevantCompositorAnimation = 1 << 3,
    /// Script asked for a sample of the effect, which the next style update takes.
    NeedsObservationSample = 1 << 4,
    /// A frame skipped its tick of the effect.
    PerFrameAnimationTickWasSkipped = 1 << 5,
}

impl EffectFlag {
    /// Whether the flag decides whether a frame can skip the style update of the effect.
    fn decides_per_frame_style_update_skip(self) -> bool {
        matches!(
            self,
            Self::CompositorDriven | Self::CompositorReplaced | Self::OffscreenThrottled
        )
    }
}

/// What the host found whether a frame can skip the style update of an effect from, besides the effect itself: the
/// generations of the style of its target and of the target's subtree, the identity of the target's layout node, and
/// whether the target was connected.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PerFrameStyleUpdateSkipKey {
    pub target_style_generation: u64,
    pub target_subtree_style_generation: u64,
    pub layout_node: usize,
    pub target_is_connected: bool,
}

/// Whether a frame can skip the style update of an effect, and what the host found that from.
/// NB: The fields of the key are kept apart, so that the decision fits in the padding the key leaves.
#[derive(Clone, Copy)]
struct RememberedPerFrameStyleUpdateSkipDecision {
    target_style_generation: u64,
    target_subtree_style_generation: u64,
    layout_node: usize,
    target_is_connected: bool,
    can_skip: bool,
}

impl RememberedPerFrameStyleUpdateSkipDecision {
    fn key(&self) -> PerFrameStyleUpdateSkipKey {
        PerFrameStyleUpdateSkipKey {
            target_style_generation: self.target_style_generation,
            target_subtree_style_generation: self.target_subtree_style_generation,
            layout_node: self.layout_node,
            target_is_connected: self.target_is_connected,
        }
    }
}

/// What an effect remembers of whether a frame can skip its style update, for a key.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PerFrameStyleUpdateSkipDecision {
    /// The effect remembers no decision.
    Unknown,
    /// The effect remembers a decision for another key.
    Stale,
    /// A frame can skip the style update of the effect.
    CanSkip,
    /// A frame cannot skip the style update of the effect.
    CannotSkip,
}

/// What became of a request that the next style update take an observation sample of an effect, at most once in a
/// task.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ObservationSampleRequest {
    /// The effect was already asked for a sample in the task.
    AlreadyRequestedInTask,
    /// The effect is asked for a sample, which it already was.
    Requested,
    /// The effect is asked for a sample, which it was not before.
    NewlyRequested,
}

/// How to reach the compositor animations of an effect.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CompositorAnimationStateAccess {
    /// Only where the effect built any before.
    Existing,
    /// Making the state where the effect has none yet.
    CreateIfMissing,
}

/// The pseudo-element an effect targets, by the host's `PseudoElement`, and its serialization.
pub(crate) struct EffectPseudoElement<'a> {
    pub(crate) kind: u8,
    pub(crate) serialization: &'a RetainedUtf16FlyString,
}

/// The state of an animation effect.
/// https://www.w3.org/TR/web-animations-1/#the-animationeffect-interface
/// https://www.w3.org/TR/web-animations-1/#the-keyframeeffect-interface
pub(crate) struct AnimationEffectRecord {
    id: AnimationEffectId,
    /// https://www.w3.org/TR/web-animations-1/#the-effecttiming-dictionaries
    specified: SpecifiedEffectTiming,
    /// The start delay, end delay and iteration duration, as normalizing the specified timing last made them.
    /// https://www.w3.org/TR/web-animations-1/#start-delay
    start_delay: TimeValue,
    /// https://www.w3.org/TR/web-animations-1/#end-delay
    end_delay: TimeValue,
    /// https://www.w3.org/TR/web-animations-1/#iteration-duration
    iteration_duration: TimeValue,
    /// The keyframes, timing function and composite operation, and what the associated animation is, which the
    /// effect keeps in its description for the style engine, whose lists of the effects of elements share it.
    description: Arc<EffectDescription>,
    /// The serialization of the value the timing function was made from, which script reads.
    timing_function_serialization: RetainedUtf16FlyString,
    /// https://www.w3.org/TR/web-animations-1/#animation-associated-effect
    /// The animation whose associated effect the effect is, if any, which keeps the effect alive.
    associated_animation: WeakAnimationHandle,
    /// https://www.w3.org/TR/web-animations-1/#effect-target-target-element
    /// The host's `DOM::Element`, which the host's effect keeps alive.
    target_element: Option<NonNull<c_void>>,
    /// The serialization of the target pseudo-selector, or none where the effect has none.
    /// https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-pseudoelement
    pseudo_element_serialization: RetainedUtf16FlyString,
    /// The current iteration the effect was at when the host last dispatched its style events.
    pub(crate) previous_current_iteration: f64,
    /// The host's `PseudoElement` of the target pseudo-selector.
    pseudo_element_kind: u8,
    /// The phase the effect was in when the host last dispatched its style events.
    pub(crate) previous_phase: AnimationEffectPhase,
    /// The `EffectFlag`s set.
    flags: u8,
    /// Whether script replaced keyframes the effect took from CSS, after which it takes no more from CSS.
    ignores_css_keyframes: bool,
    /// Whether a frame can skip the style update of the effect, as the host last found it, and what from.
    per_frame_style_update_skip_decision: Option<RememberedPerFrameStyleUpdateSkipDecision>,
    /// The generation of the task in which the host last asked for an observation sample of the effect on behalf of a
    /// read of an element.
    observation_sample_task_generation: Option<u64>,
    /// What the host built of the effect for the compositor, made when it first builds anything.
    compositor_animation_state: Option<Box<CompositorAnimationEffectState>>,
}

impl AnimationEffectRecord {
    fn new() -> Self {
        let timing = AnimationEffectTiming::default();
        let id = AnimationEffectId::mint();
        Self {
            id,
            specified: SpecifiedEffectTiming::default(),
            start_delay: timing.start_delay,
            end_delay: timing.end_delay,
            iteration_duration: timing.iteration_duration,
            description: Arc::new(EffectDescription::new(id)),
            timing_function_serialization: RetainedUtf16FlyString::from_utf16(
                &"linear".encode_utf16().collect::<Vec<_>>(),
            ),
            associated_animation: WeakAnimationHandle::default(),
            target_element: None,
            pseudo_element_serialization: RetainedUtf16FlyString::none(),
            previous_current_iteration: 0.0,
            pseudo_element_kind: 0,
            previous_phase: AnimationEffectPhase::Idle,
            flags: 0,
            ignores_css_keyframes: false,
            per_frame_style_update_skip_decision: None,
            observation_sample_task_generation: None,
            compositor_animation_state: None,
        }
    }

    #[must_use]
    pub(crate) fn id(&self) -> AnimationEffectId {
        self.id
    }

    #[must_use]
    pub(crate) fn specified_timing(&self) -> &SpecifiedEffectTiming {
        &self.specified
    }

    /// Sets the specified timing, which takes effect on the start delay, end delay and iteration duration once it is
    /// normalized.
    pub(crate) fn set_specified_timing(&mut self, specified: SpecifiedEffectTiming) {
        // NB: Whether a frame can skip the style update of the effect depends on its iteration count.
        if self.specified.iteration_count.to_bits() != specified.iteration_count.to_bits() {
            self.per_frame_style_update_skip_decision = None;
        }
        self.specified = specified;
    }

    /// The timing the timing model reads of the effect.
    #[must_use]
    pub(crate) fn timing(&self) -> AnimationEffectTiming {
        AnimationEffectTiming {
            start_delay: self.start_delay,
            end_delay: self.end_delay,
            iteration_duration: self.iteration_duration,
            iteration_start: self.specified.iteration_start,
            iteration_count: self.specified.iteration_count,
            fill_mode: self.specified.fill_mode,
            playback_direction: self.specified.playback_direction,
        }
    }

    /// https://drafts.csswg.org/web-animations-2/#normalize-specified-timing
    /// Against `timeline`, the timeline of the associated animation as the animation sees it, if any.
    pub(crate) fn normalize_specified_timing(&mut self, timeline: Option<&TimelineState>) {
        let timeline_duration = timeline.and_then(TimelineState::duration_for_attachment_range);
        let timing = timing::normalize_specified_timing(&self.specified, timeline_duration, time_unit(timeline));
        self.start_delay = timing.start_delay;
        self.end_delay = timing.end_delay;
        self.iteration_duration = timing.iteration_duration;
    }

    #[must_use]
    pub(crate) fn timing_function(&self) -> &CompactEasing {
        &self.description.timing_function
    }

    #[must_use]
    pub(crate) fn timing_function_serialization(&self) -> &RetainedUtf16FlyString {
        &self.timing_function_serialization
    }

    pub(crate) fn set_timing_function(
        &mut self,
        timing_function: CompactEasing,
        serialization: RetainedUtf16FlyString,
    ) {
        self.describe().timing_function = timing_function;
        self.timing_function_serialization = serialization;
    }

    #[must_use]
    pub(crate) fn keyframes(&self) -> Option<&Arc<KeyframeSet>> {
        self.description.keyframe_set.as_ref()
    }

    /// Whether the compositor may run all the effect animates.
    #[must_use]
    pub(crate) fn may_run_on_the_compositor(&self) -> bool {
        self.keyframes()
            .is_none_or(|keyframes| keyframes.may_run_on_the_compositor())
    }

    pub(crate) fn set_keyframes(&mut self, keyframes: Option<Arc<KeyframeSet>>) {
        self.describe().keyframe_set = keyframes;
    }

    /// Whether the effect's keyframes come from CSS: from a `@keyframes` rule, or a CSS transition.
    #[must_use]
    pub(crate) fn keyframes_from_css(&self) -> bool {
        self.keyframes()
            .is_some_and(|keyframes| keyframes.origin() != KeyframeOrigin::Script)
    }

    /// Notes that script is about to set the effect's keyframes, which keeps those it replaces of CSS from coming back.
    pub(crate) fn note_keyframes_set_by_script(&mut self) {
        self.ignores_css_keyframes |= self.keyframes_from_css();
    }

    /// Whether script replaced keyframes the effect took from CSS, after which it takes no more from CSS.
    #[must_use]
    pub(crate) fn ignores_css_keyframes(&self) -> bool {
        self.ignores_css_keyframes
    }

    #[must_use]
    pub(crate) fn composite(&self) -> FfiCompositeOperation {
        self.description.composite
    }

    pub(crate) fn set_composite(&mut self, composite: FfiCompositeOperation) {
        if self.description.composite != composite {
            self.describe().composite = composite;
        }
    }

    /// The description of the effect for the style engine.
    #[must_use]
    pub(crate) fn description(&self) -> &Arc<EffectDescription> {
        &self.description
    }

    /// Sets what the associated animation is, as far as describing the effect goes, or that it has none.
    pub(crate) fn set_described_animation(&mut self, animation: EffectAnimation) {
        self.describe().animation = animation;
    }

    /// Describes the effect anew, so that whatever the style engine kept of the description before stops applying to
    /// it: interval values it retained, and preparations for sampling it.
    pub(crate) fn describe_anew(&mut self) {
        self.describe();
    }

    /// The description to change, which is a new one wherever anything but the record holds the current one.
    fn describe(&mut self) -> &mut EffectDescription {
        Arc::make_mut(&mut self.description)
    }

    /// The animation whose associated effect the effect is, if any.
    #[must_use]
    pub(crate) fn associated_animation(&self) -> Option<AnimationHandle> {
        self.associated_animation.upgrade()
    }

    /// Notes that the effect is the associated effect of `animation`, or of none, and normalizes its specified timing
    /// against `timeline`, the timeline of that animation as the animation sees it, if any.
    /// NB: The normalization of the specified timing depends on the timeline of the associated animation.
    pub(crate) fn set_associated_animation(
        &mut self,
        animation: Option<&AnimationHandle>,
        timeline: Option<&TimelineState>,
    ) {
        self.associated_animation = animation.map(AnimationHandle::downgrade).unwrap_or_default();
        self.normalize_specified_timing(timeline);
    }

    #[must_use]
    pub(crate) fn target_element(&self) -> Option<NonNull<c_void>> {
        self.target_element
    }

    pub(crate) fn set_target_element(&mut self, element: Option<NonNull<c_void>>) {
        self.target_element = element;
        // NB: What the engine kept of the description was kept for the element the effect targeted.
        self.describe_anew();
    }

    #[must_use]
    pub(crate) fn pseudo_element(&self) -> Option<EffectPseudoElement<'_>> {
        (self.pseudo_element_serialization.raw() != 0).then_some(EffectPseudoElement {
            kind: self.pseudo_element_kind,
            serialization: &self.pseudo_element_serialization,
        })
    }

    /// Sets the target pseudo-selector to the pseudo-element of the host's `PseudoElement` `kind`, serialized as
    /// `serialization`, or to none where `serialization` is none.
    pub(crate) fn set_pseudo_element(&mut self, kind: u8, serialization: RetainedUtf16FlyString) {
        self.pseudo_element_kind = kind;
        self.pseudo_element_serialization = serialization;
        // NB: What the engine kept of the description was kept for the pseudo-element the effect targeted.
        self.describe_anew();
    }

    #[must_use]
    pub(crate) fn has_flag(&self, flag: EffectFlag) -> bool {
        self.flags & flag as u8 != 0
    }

    /// Sets or clears `flag`, answering whether that changed it.
    pub(crate) fn set_flag(&mut self, flag: EffectFlag, value: bool) -> bool {
        let flags = if value {
            self.flags | flag as u8
        } else {
            self.flags & !(flag as u8)
        };
        let changed = std::mem::replace(&mut self.flags, flags) != flags;
        if changed && flag.decides_per_frame_style_update_skip() {
            self.per_frame_style_update_skip_decision = None;
        }
        changed
    }

    /// What the effect remembers of whether a frame can skip its style update, for `key`.
    #[must_use]
    pub(crate) fn per_frame_style_update_skip_decision(
        &self,
        key: &PerFrameStyleUpdateSkipKey,
    ) -> PerFrameStyleUpdateSkipDecision {
        match &self.per_frame_style_update_skip_decision {
            None => PerFrameStyleUpdateSkipDecision::Unknown,
            Some(decision) if decision.key() != *key => PerFrameStyleUpdateSkipDecision::Stale,
            Some(decision) if decision.can_skip => PerFrameStyleUpdateSkipDecision::CanSkip,
            Some(_) => PerFrameStyleUpdateSkipDecision::CannotSkip,
        }
    }

    /// Remembers whether a frame can skip the style update of the effect, as the host found it from `key`.
    pub(crate) fn note_per_frame_style_update_skip_decision(
        &mut self,
        key: PerFrameStyleUpdateSkipKey,
        can_skip: bool,
    ) {
        self.per_frame_style_update_skip_decision = Some(RememberedPerFrameStyleUpdateSkipDecision {
            target_style_generation: key.target_style_generation,
            target_subtree_style_generation: key.target_subtree_style_generation,
            layout_node: key.layout_node,
            target_is_connected: key.target_is_connected,
            can_skip,
        });
    }

    /// Asks the next style update for an observation sample of the effect on behalf of a read of an element in the
    /// task of generation `task_generation`, unless the effect was already asked for one in that task.
    pub(crate) fn request_observation_sample_in_task(&mut self, task_generation: u64) -> ObservationSampleRequest {
        if self.observation_sample_task_generation == Some(task_generation) {
            return ObservationSampleRequest::AlreadyRequestedInTask;
        }
        self.observation_sample_task_generation = Some(task_generation);
        if self.set_flag(EffectFlag::NeedsObservationSample, true) {
            ObservationSampleRequest::NewlyRequested
        } else {
            ObservationSampleRequest::Requested
        }
    }

    /// What the host built of the effect for the compositor, reached as `access` says.
    pub(crate) fn compositor_animation_state(
        &mut self,
        access: CompositorAnimationStateAccess,
    ) -> Option<&mut CompositorAnimationEffectState> {
        if access == CompositorAnimationStateAccess::CreateIfMissing {
            self.compositor_animation_state.get_or_insert_default();
        }
        self.compositor_animation_state.as_deref_mut()
    }

    /// Forgets what the host concluded of the effect as it was, since the effect changed: what it built for the
    /// compositor from the keyframes, how it ran or throttled the effect, and whether a frame can skip its style update.
    pub(crate) fn forget_what_the_host_concluded(&mut self) {
        if let Some(state) = &mut self.compositor_animation_state {
            state.reset();
        }
        for flag in [
            EffectFlag::CompositorDriven,
            EffectFlag::CompositorReplaced,
            EffectFlag::OffscreenThrottled,
            EffectFlag::ObservationRelevantCompositorAnimation,
        ] {
            self.set_flag(flag, false);
        }
        self.per_frame_style_update_skip_decision = None;
    }
}

/// An animation effect's record, which every handle to the effect shares, and the host's `AnimationEffect` of it.
pub struct AnimationEffectCell {
    record: RefCell<AnimationEffectRecord>,
    /// The host's `AnimationEffect`, which holds a reference to the record until it is finalized and clears this as it
    /// does.
    object_cell: Cell<Option<NonNull<c_void>>>,
}

impl AnimationEffectCell {
    pub(crate) fn borrow(&self) -> Ref<'_, AnimationEffectRecord> {
        self.record.borrow()
    }

    pub(crate) fn borrow_mut(&self) -> RefMut<'_, AnimationEffectRecord> {
        self.record.borrow_mut()
    }

    /// The host's `AnimationEffect` of the effect, while it is alive.
    pub(crate) fn object_cell(&self) -> Option<NonNull<c_void>> {
        self.object_cell.get()
    }

    pub(crate) fn set_object_cell(&self, object_cell: Option<NonNull<c_void>>) {
        self.object_cell.set(object_cell);
    }

    /// Whether the host may reach the effect and the element it targets: while the object script sees of it is, or,
    /// for an effect without one, while its associated animation is.
    pub(crate) fn is_alive(&self) -> bool {
        if self.object_cell().is_some() {
            return true;
        }
        let animation = self.borrow().associated_animation();
        animation.is_some_and(|animation| animation.is_alive())
    }

    /// The bytes of the allocation every handle shares. The description the record holds counts toward the style
    /// engine's descriptions of the effects of elements, which share it.
    pub(crate) fn memory_size(&self) -> usize {
        2 * size_of::<usize>() + size_of::<Self>()
    }
}

/// A strong reference to an animation effect.
#[derive(Clone)]
pub(crate) struct EffectHandle(Rc<AnimationEffectCell>);

impl EffectHandle {
    /// A new effect with the default timing, no keyframes and no target.
    pub(crate) fn new() -> Self {
        Self(Rc::new(AnimationEffectCell {
            record: RefCell::new(AnimationEffectRecord::new()),
            object_cell: Cell::new(None),
        }))
    }

    /// Whether both handles are to the same effect.
    pub(crate) fn ptr_eq(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
    }

    /// Hands the reference to the host, which gives it back to `from_raw`.
    pub(crate) fn into_raw(self) -> *const AnimationEffectCell {
        Rc::into_raw(self.0)
    }

    /// Takes a reference of its own to an effect the host holds a reference to.
    ///
    /// # Safety
    /// `cell` must be an effect the host holds a reference to.
    pub(crate) unsafe fn retained_from_host(cell: *const AnimationEffectCell) -> Self {
        // SAFETY: Guaranteed by the caller.
        unsafe {
            Rc::increment_strong_count(cell);
            Self(Rc::from_raw(cell))
        }
    }

    /// Takes back a reference the host was handed.
    ///
    /// # Safety
    /// `cell` must be a reference `into_raw` handed out, which the host gives up.
    pub(crate) unsafe fn from_raw(cell: *const AnimationEffectCell) -> Self {
        // SAFETY: Guaranteed by the caller.
        Self(unsafe { Rc::from_raw(cell) })
    }
}

/// A set of effects that keeps none of them alive.
///
/// NB: An effect that is no longer alive to the host is dead to the set, though what else holds its record may only let
///     go of it as the host sweeps what holds it, so that the host never meets an effect it finalized.
pub struct WeakEffectSet(WeakRecordSet<AnimationEffectCell>);

impl Default for WeakEffectSet {
    fn default() -> Self {
        Self(WeakRecordSet::new(AnimationEffectCell::is_alive))
    }
}

impl WeakEffectSet {
    /// Adds `effect` to the set, answering whether the set did not hold it yet.
    pub(crate) fn insert(&mut self, effect: &EffectHandle) -> bool {
        self.0.insert(&effect.0)
    }

    /// Removes `effect` from the set, answering whether the set held it.
    pub(crate) fn remove(&mut self, effect: &AnimationEffectCell) -> bool {
        self.0.remove(effect)
    }

    /// Whether the set holds `effect`.
    pub(crate) fn contains(&self, effect: &AnimationEffectCell) -> bool {
        self.0.contains(effect)
    }

    /// Empties the set, calling `callback` with each effect alive to it, in insertion order.
    pub(crate) fn take_each(&mut self, mut callback: impl FnMut(&EffectHandle)) {
        self.0.take_each(|effect| callback(&EffectHandle(effect)));
    }
}

impl PartialEq for EffectHandle {
    fn eq(&self, other: &Self) -> bool {
        self.ptr_eq(other)
    }
}

impl std::fmt::Debug for EffectHandle {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_tuple("EffectHandle")
            .field(&self.borrow().id())
            .finish()
    }
}

impl std::ops::Deref for EffectHandle {
    type Target = AnimationEffectCell;

    fn deref(&self) -> &AnimationEffectCell {
        &self.0
    }
}

/// The timing of an effect at one local time, and what follows from its timing alone.
pub struct EffectTimingAtLocalTime {
    pub phase: AnimationEffectPhase,
    pub current_iteration: Option<f64>,
    /// Unresolved where it was not asked for.
    pub transformed_progress: Option<f64>,
    pub active_duration: TimeValue,
    pub end_time: TimeValue,
}

/// The timing of `effect` at `local_time` with `fill_mode` as its fill mode, associated with `animation`, if any, on
/// its timeline, if it has one, and with its transformed progress through its timing function where
/// `with_transformed_progress`.
pub(crate) fn resolve_effect_timing(
    effect: &AnimationEffectCell,
    local_time: Option<TimeValue>,
    fill_mode: FillMode,
    animation: Option<&AnimationCell>,
    with_transformed_progress: bool,
) -> EffectTimingAtLocalTime {
    let timeline = animation.and_then(|animation| animation.borrow().timeline().map(|timeline| timeline.sample()));
    let timeline = timeline.as_ref();
    let animation = animation.map(|animation| *animation.borrow().playback());
    let effect = effect.borrow();
    let timing = AnimationEffectTiming {
        fill_mode,
        ..effect.timing()
    };
    let resolved = timing::resolve_timing(&timing, local_time, animation.as_ref(), timeline);
    let transformed_progress = with_transformed_progress
        .then(|| timing::transformed_progress(&timing, &resolved, effect.timing_function().as_easing()))
        .flatten();
    EffectTimingAtLocalTime {
        phase: resolved.phase,
        current_iteration: timing::current_iteration(&timing, &resolved),
        transformed_progress,
        active_duration: timing::active_duration(&timing),
        end_time: timing::end_time(&timing),
    }
}
