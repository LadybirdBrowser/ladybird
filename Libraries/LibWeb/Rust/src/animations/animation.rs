/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Animations.
//! https://www.w3.org/TR/web-animations-1/#animations

use super::easing::CompactEasing;
use super::effect::EffectHandle;
use super::timeline::TimelineHandle;
use super::timeline_state::TimelineState;
use super::timing::{AnimationPlayState, AnimationPlayback, TimeValue};
use super::weak_record_set::WeakRecordSet;
use crate::css::property_metadata::property_name;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style::animations::AppliedAnimationDefinition;
use crate::css::style::effect_descriptions::{EffectAnimation, TransitionReversing};
use crate::css::style_value::{RetainedStyleValueData, optional_style_values_equal};
use std::borrow::Cow;
use std::cell::{Cell, Ref, RefCell, RefMut};
use std::ffi::c_void;
use std::num::NonZeroU64;
use std::ptr::NonNull;
use std::rc::{Rc, Weak};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

/// The position of an animation in the global animation list, which orders animations by when they were created.
/// https://www.w3.org/TR/web-animations-1/#global-animation-list
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct GlobalAnimationListOrder(pub u64);

impl GlobalAnimationListOrder {
    fn mint() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        Self(NEXT.fetch_add(1, Ordering::Relaxed))
    }
}

/// The host's identity of a task it queued for an animation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct HostTaskId(pub(crate) NonZeroU64);

/// The offset of one edge of an attachment range from the start of its named timeline range: a length plus a
/// percentage of the range, or a calculation only the size of the range resolves.
/// https://drafts.csswg.org/scroll-animations-1/#animation-range-start
#[derive(Clone)]
pub(crate) struct AttachmentRangeEdgeOffset {
    pub(crate) px: f64,
    pub(crate) percentage: f64,
    /// A calculation the offset resolves against the size of its range, or none, which `px` and `percentage` then
    /// describe as it resolves against 0px and 100px.
    pub(crate) calculated: RetainedStyleValueData,
}

impl AttachmentRangeEdgeOffset {
    fn equals(&self, other: &Self) -> bool {
        self.px == other.px
            && self.percentage == other.percentage
            && optional_style_values_equal(self.calculated.optional_data(), other.calculated.optional_data())
    }
}

/// The edge of an attachment range: the host's `TimelineRangeName` it is measured from, and its offset.
pub(crate) struct AttachmentRangeEdge {
    pub(crate) name: u8,
    pub(crate) offset: AttachmentRangeEdgeOffset,
}

/// The owning element of a CSS animation or transition: the host's `DOM::Element`, which the host's animation keeps
/// alive, and the host's `PseudoElement` where the owner is one of its pseudo-elements.
/// https://www.w3.org/TR/css-animations-2/#owning-element-section
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) struct OwningElement {
    pub(crate) element: NonNull<c_void>,
    pub(crate) pseudo_element: Option<u8>,
}

/// What the host's `CSSAnimation` notes of what script did to it and what it is doing, by bit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CssAnimationFlag {
    /// Script changed the play state, which `animation-play-state` no longer changes.
    ScriptOverrodePlayState = 1 << 0,
    /// Script set the timeline, which `animation-timeline` no longer changes.
    ScriptSetTimeline = 1 << 1,
    /// Script set the start of the attachment range, which `animation-range-start` no longer changes.
    ScriptSetRangeStart = 1 << 2,
    /// Script set the end of the attachment range, which `animation-range-end` no longer changes.
    ScriptSetRangeEnd = 1 << 3,
}

/// The state of a CSS animation.
/// https://drafts.csswg.org/css-animations-2/#the-CSSAnimation-interface
pub(crate) struct CssAnimationState {
    /// https://drafts.csswg.org/css-animations-2/#dom-cssanimation-animationname
    pub(crate) name: RetainedUtf16FlyString,
    /// https://drafts.csswg.org/css-animations-1/#animation-timing-function
    /// The easing a keyframe that declares none of its own runs, from the `animation-timing-function` of the owning
    /// element.
    pub(crate) default_easing: CompactEasing,
    /// The definition the style computation last applied to the animation, which the style engine shares, or none where
    /// none has been applied yet.
    pub(crate) applied_definition: Option<Arc<AppliedAnimationDefinition>>,
    /// The name of the named timeline the animation follows, which it looks up from its owning element again as the
    /// timelines its document declares change, or none where its `animation-timeline` names none or it was removed
    /// from its owning element.
    /// https://drafts.csswg.org/scroll-animations-1/#timeline-scoping
    pub(crate) named_timeline: RetainedUtf16FlyString,
    /// The position of the animation in the computed value of `animation-name` of its owning element.
    pub(crate) name_index: u32,
    /// The host's `AnimationPlayState` that `animation-play-state` last computed for the animation, if any.
    pub(crate) last_css_play_state: Option<u8>,
    /// The `CssAnimationFlag`s set.
    pub(crate) flags: u8,
}

impl CssAnimationState {
    pub(crate) fn has_flag(&self, flag: CssAnimationFlag) -> bool {
        self.flags & flag as u8 != 0
    }

    pub(crate) fn set_flag(&mut self, flag: CssAnimationFlag) {
        self.flags |= flag as u8;
    }
}

/// The host's `CSS::AnimationPlayState::Paused` and `CSS::AnimationPlayState::Running`.
pub(crate) const CSS_ANIMATION_PLAY_STATE_PAUSED: u8 = 0;
pub(crate) const CSS_ANIMATION_PLAY_STATE_RUNNING: u8 = 1;

/// https://drafts.csswg.org/css-transitions-2/#transition-phase
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum TransitionPhase {
    Before,
    Active,
    After,
    Idle,
    Pending,
}

/// The state of a CSS transition.
/// https://drafts.csswg.org/css-transitions-2/#the-CSSTransition-interface
pub(crate) struct CssTransitionState {
    /// The name of the custom property the transition runs, or none where it runs the longhand `property_id` names.
    pub(crate) custom_property_name: RetainedUtf16FlyString,
    /// https://drafts.csswg.org/css-transitions-2/#transition-generation
    pub(crate) generation: u64,
    /// https://drafts.csswg.org/css-transitions/#transition-start-time
    pub(crate) start_time: f64,
    /// https://drafts.csswg.org/css-transitions/#transition-end-time
    pub(crate) end_time: f64,
    /// https://drafts.csswg.org/css-transitions/#transition-start-value
    pub(crate) start_value: RetainedStyleValueData,
    /// https://drafts.csswg.org/css-transitions/#transition-end-value
    pub(crate) end_value: RetainedStyleValueData,
    /// https://drafts.csswg.org/css-transitions/#transition-reversing-adjusted-start-value
    pub(crate) reversing_adjusted_start_value: RetainedStyleValueData,
    /// https://drafts.csswg.org/css-transitions/#transition-reversing-shortening-factor
    pub(crate) reversing_shortening_factor: f64,
    /// The host's `PropertyID` of the property the transition runs.
    pub(crate) property_id: u16,
    /// The phase the transition was in when its events were last established.
    pub(crate) previous_phase: TransitionPhase,
    /// Whether the transition was started provisionally and is neither committed nor discarded yet.
    pub(crate) provisional: bool,
    /// Whether the transition is one the hover of a clock lane started, which the host took in.
    pub(crate) adopted_from_lane: bool,
    /// The effect the transition was started with, which it keeps whatever script sets as its associated effect. Its
    /// timing function shapes the transition's interval.
    pub(crate) effect_started_with: Option<EffectHandle>,
}

impl CssTransitionState {
    /// The expanded name of the property the transition runs.
    pub(crate) fn property_name(&self) -> Cow<'_, [u16]> {
        if self.custom_property_name.raw() != 0 {
            return self.custom_property_name.to_utf16();
        }
        Cow::Owned(property_name(self.property_id).encode_utf16().collect())
    }
}

/// What kind of animation an animation is, with the state of its kind.
pub(crate) enum AnimationKind {
    /// An animation script created.
    Script,
    CssAnimation(CssAnimationState),
    CssTransition(CssTransitionState),
    /// The animation of an SVG animation element, which has no `Animation` of the host.
    /// https://www.w3.org/TR/smil-animation/#AnimationSandwichModel
    Smil {
        /// The time the animation begins at, which orders it among the animations of its time container.
        begin_time: f64,
        /// The host's `SVGAnimationElement` whose animation it is, which whatever the GC traces of the host that holds
        /// the record keeps alive.
        animation_element: NonNull<c_void>,
        /// The `SmilHolder` bits of what holds the animation.
        holders: u8,
    },
}

/// What a pointer to a GC cell of the host points at.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum HostCellKind {
    /// A `GC::Cell`.
    Cell,
    /// A `DOM::Element`.
    Element,
}

/// What of the host holds the animation of an SVG animation element, by bit. The animation is alive to the host while
/// one does, as each lets go of it when it is done with it or the host finalizes it.
#[derive(Clone, Copy)]
pub(crate) enum SmilHolder {
    /// Its animation element, until it lets go of the animation or is finalized.
    AnimationElement = 1 << 0,
    /// The list of the animations of the target of its effect, until the animation leaves it or the target is
    /// finalized.
    TargetList = 1 << 1,
}

/// The timeline times of the start and the end of an attachment range.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct AttachmentInterval {
    pub(crate) start: TimeValue,
    pub(crate) end: TimeValue,
}

/// The state of an animation.
/// https://www.w3.org/TR/web-animations-1/#the-animation-interface
pub(crate) struct AnimationRecord {
    /// https://www.w3.org/TR/web-animations-1/#animation-associated-effect
    effect: Option<EffectHandle>,
    /// https://www.w3.org/TR/web-animations-1/#animation-timeline
    timeline: Option<TimelineHandle>,
    /// The start time, hold time, playback rate, pending playback rate, pending tasks, whether the animation was last
    /// found finished, and the replace state.
    playback: AnimationPlayback,
    /// https://www.w3.org/TR/web-animations-1/#previous-current-time
    pub(crate) previous_current_time: Option<TimeValue>,
    /// The current time of the associated effect where the animation was last canceled, until the host takes it.
    pub(crate) saved_cancel_time: Option<TimeValue>,
    /// The attachment interval the effect's timing was last normalized against.
    pub(crate) normalized_attachment_interval: Option<AttachmentInterval>,
    /// The play state the animation was in when it was disassociated from its target as inert.
    pub(crate) play_state_when_disassociated_from_target: Option<AnimationPlayState>,
    /// https://drafts.csswg.org/web-animations-2/#auto-aligning-start-time
    pub(crate) auto_align_start_time: bool,
    /// Whether the animation is disassociated from its target once it is inert.
    pub(crate) disassociation_from_target_pending: bool,
    /// https://www.w3.org/TR/web-animations-1/#dom-animation-id
    /// None where it is the empty string.
    pub(crate) dom_id: RetainedUtf16FlyString,
    /// https://www.w3.org/TR/web-animations-1/#global-animation-list
    global_animation_list_order: GlobalAnimationListOrder,
    /// https://drafts.csswg.org/scroll-animations-1/#animation-attachment-range
    /// The offsets of the start and the end edge, and the host's `TimelineRangeName` of each.
    range_offsets: [AttachmentRangeEdgeOffset; 2],
    range_names: [u8; 2],
    /// https://www.w3.org/TR/css-animations-2/#owning-element-section
    owning_element: Option<NonNull<c_void>>,
    owning_pseudo_element: Option<u8>,
    /// The task the host last queued to run the finish notification steps of the animation, or to dispatch its finish
    /// event, if any.
    /// NB: Canceling the task forgets it. Nothing forgets the task once it ran, which keeps a later finish
    ///     notification task from being queued until one is canceled.
    pub(crate) finish_notification_task: Option<HostTaskId>,
    kind: AnimationKind,
}

impl AnimationRecord {
    fn new(kind: AnimationKind) -> Self {
        Self {
            effect: None,
            timeline: None,
            playback: AnimationPlayback::default(),
            previous_current_time: None,
            saved_cancel_time: None,
            normalized_attachment_interval: None,
            play_state_when_disassociated_from_target: None,
            auto_align_start_time: false,
            disassociation_from_target_pending: false,
            dom_id: RetainedUtf16FlyString::none(),
            global_animation_list_order: GlobalAnimationListOrder::mint(),
            // NB: The initial edges, normal, are the start and the end of the entire timeline.
            range_offsets: [
                AttachmentRangeEdgeOffset {
                    px: 0.0,
                    percentage: 0.0,
                    calculated: RetainedStyleValueData::none(),
                },
                AttachmentRangeEdgeOffset {
                    px: 0.0,
                    percentage: 100.0,
                    calculated: RetainedStyleValueData::none(),
                },
            ],
            range_names: [0, 0],
            owning_element: None,
            owning_pseudo_element: None,
            finish_notification_task: None,
            kind,
        }
    }

    #[must_use]
    pub(crate) fn effect(&self) -> Option<&EffectHandle> {
        self.effect.as_ref()
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-timeline
    #[must_use]
    pub(crate) fn timeline(&self) -> Option<&TimelineHandle> {
        self.timeline.as_ref()
    }

    /// Sets the timeline, handing back the old one.
    pub(crate) fn replace_timeline(&mut self, timeline: Option<TimelineHandle>) -> Option<TimelineHandle> {
        std::mem::replace(&mut self.timeline, timeline)
    }

    /// The timeline, if any, as the animation, attached to its attachment range on it, reads it now.
    #[must_use]
    pub(crate) fn timeline_state(&self) -> Option<TimelineState> {
        self.timeline.as_ref().map(|timeline| timeline.state_for(&self.range()))
    }

    /// Sets the associated effect, describing the old one as associated with no animation and the new one as
    /// associated with this one.
    pub(crate) fn set_effect(&mut self, effect: Option<EffectHandle>) {
        if let Some(old_effect) = &self.effect {
            old_effect
                .borrow_mut()
                .set_described_animation(EffectAnimation::Unassociated);
        }
        if let Some(effect) = &effect {
            effect.borrow_mut().set_described_animation(self.described_animation());
        }
        self.effect = effect;
    }

    /// What the animation is, as far as describing its effect for the style engine goes.
    fn described_animation(&self) -> EffectAnimation {
        match &self.kind {
            AnimationKind::CssAnimation(state) => EffectAnimation::Animation(state.default_easing.clone()),
            AnimationKind::CssTransition(state) => EffectAnimation::Transition(TransitionReversing {
                adjusted_start_value: state.reversing_adjusted_start_value.clone(),
                shortening_factor: state.reversing_shortening_factor,
            }),
            AnimationKind::Script | AnimationKind::Smil { .. } => EffectAnimation::Animation(CompactEasing::Linear),
        }
    }

    /// Sets the easing a keyframe of a CSS animation's effect that declares none runs.
    pub(crate) fn set_css_animation_default_easing(&mut self, easing: CompactEasing) {
        let state = self.css_animation_mut();
        if state.default_easing == easing {
            return;
        }
        state.default_easing = easing;
        if let Some(effect) = &self.effect {
            effect.borrow_mut().set_described_animation(self.described_animation());
        }
    }

    #[must_use]
    pub(crate) fn playback(&self) -> &AnimationPlayback {
        &self.playback
    }

    pub(crate) fn playback_mut(&mut self) -> &mut AnimationPlayback {
        &mut self.playback
    }

    /// Notes that the animation was associated with the target of its associated effect again. Joining a target does
    /// not make an animation that was disassociated from it as inert relevant, so whether it is inert is checked again.
    pub(crate) fn did_associate_with_target(&mut self) {
        if self.play_state_when_disassociated_from_target.take().is_some() {
            self.disassociation_from_target_pending = true;
        }
    }

    #[must_use]
    pub(crate) fn global_animation_list_order(&self) -> GlobalAnimationListOrder {
        self.global_animation_list_order
    }

    /// The start and the end edge of the attachment range.
    #[must_use]
    pub(crate) fn range(&self) -> [AttachmentRangeEdge; 2] {
        [0, 1].map(|index| AttachmentRangeEdge {
            name: self.range_names[index],
            offset: self.range_offsets[index].clone(),
        })
    }

    /// Sets the attachment range to the start and the end edge `range`, answering whether that changed it.
    pub(crate) fn set_range(&mut self, range: [AttachmentRangeEdge; 2]) -> bool {
        let changed = range.iter().enumerate().any(|(index, edge)| {
            edge.name != self.range_names[index] || !edge.offset.equals(&self.range_offsets[index])
        });
        if changed {
            for (index, edge) in range.into_iter().enumerate() {
                self.range_names[index] = edge.name;
                self.range_offsets[index] = edge.offset;
            }
        }
        changed
    }

    #[must_use]
    pub(crate) fn owning_element(&self) -> Option<OwningElement> {
        self.owning_element.map(|element| OwningElement {
            element,
            pseudo_element: self.owning_pseudo_element,
        })
    }

    pub(crate) fn set_owning_element(&mut self, owning_element: Option<OwningElement>) {
        self.owning_element = owning_element.map(|owner| owner.element);
        self.owning_pseudo_element = owning_element.and_then(|owner| owner.pseudo_element);
    }

    #[must_use]
    pub(crate) fn kind(&self) -> &AnimationKind {
        &self.kind
    }

    /// The state of the CSS animation the record is of.
    #[must_use]
    pub(crate) fn css_animation(&self) -> &CssAnimationState {
        match &self.kind {
            AnimationKind::CssAnimation(state) => state,
            _ => unreachable!("the animation is a CSS animation"),
        }
    }

    pub(crate) fn css_animation_mut(&mut self) -> &mut CssAnimationState {
        match &mut self.kind {
            AnimationKind::CssAnimation(state) => state,
            _ => unreachable!("the animation is a CSS animation"),
        }
    }

    /// The state of the CSS transition the record is of.
    #[must_use]
    pub(crate) fn css_transition(&self) -> &CssTransitionState {
        match &self.kind {
            AnimationKind::CssTransition(state) => state,
            _ => unreachable!("the animation is a CSS transition"),
        }
    }

    /// The time of the style change event that started the CSS transition the record is of.
    /// https://drafts.csswg.org/css-transitions/#style-change-event
    #[must_use]
    pub(crate) fn css_transition_style_change_event_time(&self) -> f64 {
        let start_delay = self
            .effect
            .as_ref()
            .map_or(0.0, |effect| effect.borrow().timing().start_delay.value);
        self.css_transition().start_time - start_delay
    }

    pub(crate) fn css_transition_mut(&mut self) -> &mut CssTransitionState {
        match &mut self.kind {
            AnimationKind::CssTransition(state) => state,
            _ => unreachable!("the animation is a CSS transition"),
        }
    }

    /// The bytes the record holds beyond itself.
    fn heap_size(&self) -> usize {
        match &self.kind {
            AnimationKind::CssAnimation(state) => {
                state.default_easing.heap_size()
                    + state.applied_definition.as_ref().map_or(0, |definition| {
                        crate::heap_size::ARC_HEADER_BYTES + size_of_val::<AppliedAnimationDefinition>(definition)
                    })
            }
            AnimationKind::Script | AnimationKind::CssTransition(_) | AnimationKind::Smil { .. } => 0,
        }
    }
}

/// An animation's record, which every handle to the animation shares, and the host's `Animation` of it.
pub struct AnimationCell {
    record: RefCell<AnimationRecord>,
    /// The GC cell of the object script sees of the animation, which holds a reference to the record until it is
    /// finalized and clears this as it does.
    object_cell: Cell<Option<NonNull<c_void>>>,
}

impl AnimationCell {
    pub(crate) fn borrow(&self) -> Ref<'_, AnimationRecord> {
        self.record.borrow()
    }

    pub(crate) fn borrow_mut(&self) -> RefMut<'_, AnimationRecord> {
        self.record.borrow_mut()
    }

    /// The host's `Animation` of the animation, while it is alive.
    pub(crate) fn object_cell(&self) -> Option<NonNull<c_void>> {
        self.object_cell.get()
    }

    /// Whether the host may reach the animation and the elements its record names. One with an object script sees is
    /// alive until the host finalizes that object. Something that holds the record of a dead animation lets go of it
    /// only as the host sweeps it.
    /// NB: A CSS animation or transition gets an object only once script asks for one. Until then it is alive while it
    ///     has its owning element, which whatever holds the record keeps alive. The animation of an SVG animation
    ///     element never has an object. It is alive while a holder its record notes holds it, each of which keeps the
    ///     elements the record names alive while it does.
    pub(crate) fn is_alive(&self) -> bool {
        if self.object_cell().is_some() {
            return true;
        }
        let record = self.borrow();
        match record.kind() {
            AnimationKind::Smil { holders, .. } => *holders != 0,
            AnimationKind::CssAnimation(_) | AnimationKind::CssTransition(_) => record.owning_element.is_some(),
            AnimationKind::Script => false,
        }
    }

    /// Notes whether `holder` holds the animation, where it is the animation of an SVG animation element.
    pub(crate) fn note_smil_holder(&self, holder: SmilHolder, holds: bool) {
        if let AnimationKind::Smil { holders, .. } = &mut self.borrow_mut().kind {
            if holds {
                *holders |= holder as u8;
            } else {
                *holders &= !(holder as u8);
            }
        }
    }

    /// Calls `visit` with each GC cell of the host that keeps alive the elements the record names: the animation's
    /// object, or, for an animation without one, the cells the record names.
    pub(crate) fn for_each_host_cell(&self, mut visit: impl FnMut(NonNull<c_void>, HostCellKind)) {
        if let Some(object_cell) = self.object_cell() {
            visit(object_cell, HostCellKind::Cell);
            return;
        }
        let record = self.borrow();
        if let AnimationKind::Smil { animation_element, .. } = record.kind() {
            visit(*animation_element, HostCellKind::Element);
            if let Some(target) = record.effect().and_then(|effect| effect.borrow().target_element()) {
                visit(target, HostCellKind::Element);
            }
            return;
        }
        drop(record);
        self.for_each_named_cell(visit);
    }

    /// Calls `visit` with each GC cell of the host the record names, which the animation's object keeps alive: its
    /// owning element, the object of its associated effect, or the target of an effect without one, and the object of
    /// its timeline.
    pub(crate) fn for_each_named_cell(&self, mut visit: impl FnMut(NonNull<c_void>, HostCellKind)) {
        let record = self.borrow();
        if let Some(owner) = record.owning_element {
            visit(owner, HostCellKind::Element);
        }
        if let Some(effect) = record.effect() {
            match effect.object_cell() {
                Some(object_cell) => visit(object_cell, HostCellKind::Cell),
                None => {
                    if let Some(target) = effect.borrow().target_element() {
                        visit(target, HostCellKind::Element);
                    }
                }
            }
        }
        if let Some(timeline) = record.timeline() {
            timeline.for_each_holder_cell(&mut visit);
        }
    }

    pub(crate) fn set_object_cell(&self, object_cell: Option<NonNull<c_void>>) {
        self.object_cell.set(object_cell);
    }

    /// The bytes of the allocation every handle shares, and those the record holds beyond it.
    pub(crate) fn memory_size(&self) -> usize {
        2 * size_of::<usize>() + size_of::<Self>() + self.borrow().heap_size()
    }
}

/// A strong reference to an animation, as what libweb_bindings_rust holds of one.
#[derive(Clone)]
pub struct AnimationRecordRef(AnimationHandle);

impl AnimationRecordRef {
    pub(crate) fn from_handle(animation: AnimationHandle) -> Self {
        Self(animation)
    }

    /// Takes a reference of its own to an animation the host holds a reference to.
    ///
    /// # Safety
    /// `animation` must be an animation the host holds a reference to.
    pub unsafe fn retained_from_host(animation: *const c_void) -> Self {
        // SAFETY: Guaranteed by the caller.
        Self(unsafe { AnimationHandle::retained_from_host(animation.cast()) })
    }

    /// The animation, as the host names it.
    #[must_use]
    pub fn as_host_ptr(&self) -> *const c_void {
        Rc::as_ptr(&self.0.0).cast()
    }

    pub(crate) fn cell(&self) -> &AnimationCell {
        &self.0.0
    }
}

/// A strong reference to an animation.
#[derive(Clone)]
pub(crate) struct AnimationHandle(Rc<AnimationCell>);

impl AnimationHandle {
    /// A new idle animation of `kind` with no associated effect.
    pub(crate) fn new(kind: AnimationKind) -> Self {
        Self(Rc::new(AnimationCell {
            record: RefCell::new(AnimationRecord::new(kind)),
            object_cell: Cell::new(None),
        }))
    }

    /// Whether both handles are to the same animation.
    pub(crate) fn ptr_eq(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
    }

    /// A reference to the animation that does not keep it alive.
    pub(crate) fn downgrade(&self) -> WeakAnimationHandle {
        WeakAnimationHandle(Rc::downgrade(&self.0))
    }

    /// Whether the handle is to `animation`.
    pub(crate) fn is(&self, animation: &AnimationCell) -> bool {
        std::ptr::eq(Rc::as_ptr(&self.0), animation)
    }

    /// Takes a reference of its own to an animation the host holds a reference to.
    ///
    /// # Safety
    /// `cell` must be an animation the host holds a reference to.
    pub(crate) unsafe fn retained_from_host(cell: *const AnimationCell) -> Self {
        // SAFETY: Guaranteed by the caller.
        unsafe {
            Rc::increment_strong_count(cell);
            Self(Rc::from_raw(cell))
        }
    }

    /// Hands the reference to the host, which gives it back to `from_raw`.
    pub(crate) fn into_raw(self) -> *const AnimationCell {
        Rc::into_raw(self.0)
    }

    /// Takes back a reference the host was handed.
    ///
    /// # Safety
    /// `cell` must be a reference `into_raw` handed out, which the host gives up.
    pub(crate) unsafe fn from_raw(cell: *const AnimationCell) -> Self {
        // SAFETY: Guaranteed by the caller.
        Self(unsafe { Rc::from_raw(cell) })
    }
}

impl std::fmt::Debug for AnimationHandle {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_tuple("AnimationHandle")
            .field(&self.borrow().global_animation_list_order())
            .finish()
    }
}

/// A reference to an animation that does not keep it alive, or to none.
#[derive(Clone, Default)]
pub(crate) struct WeakAnimationHandle(Weak<AnimationCell>);

impl WeakAnimationHandle {
    /// The animation, while it is alive.
    pub(crate) fn upgrade(&self) -> Option<AnimationHandle> {
        self.0.upgrade().map(AnimationHandle)
    }
}

/// A set of animations that keeps none of them alive.
///
/// NB: An animation that is no longer alive to the host is dead to the set, though what else holds its record may only
///     let go of it as the host sweeps what holds it, so that the host never meets an animation it finalized.
pub struct WeakAnimationSet(WeakRecordSet<AnimationCell>);

impl Default for WeakAnimationSet {
    fn default() -> Self {
        Self(WeakRecordSet::new(AnimationCell::is_alive))
    }
}

impl WeakAnimationSet {
    /// Adds `animation` to the set, answering whether the set did not hold it yet.
    pub(crate) fn insert(&mut self, animation: &AnimationHandle) -> bool {
        self.0.insert(&animation.0)
    }

    /// Removes `animation` from the set, answering whether the set held it.
    pub(crate) fn remove(&mut self, animation: &AnimationCell) -> bool {
        self.0.remove(animation)
    }

    /// Whether the set holds `animation`.
    pub(crate) fn contains(&self, animation: &AnimationCell) -> bool {
        self.0.contains(animation)
    }

    /// The index of the entry of `animation` in the set's order, if the set holds it.
    pub(crate) fn position(&self, animation: &AnimationCell) -> Option<u32> {
        self.0.position(animation)
    }

    /// Drops the entries of animations that died.
    pub(crate) fn prune(&mut self) {
        self.0.prune();
    }

    /// The number of entries, live or dead, which enumeration runs over.
    pub(crate) fn entry_count(&self) -> usize {
        self.0.entry_count()
    }

    /// The animation of the entry at `index`, where it is alive.
    pub(crate) fn get(&self, index: usize) -> Option<AnimationHandle> {
        self.0.get(index).map(AnimationHandle)
    }

    /// The animations of the set that are alive, in insertion order, as enumerating the entries in place finds them.
    pub(crate) fn iter(&self) -> impl Iterator<Item = AnimationHandle> + '_ {
        (0..self.entry_count()).filter_map(|index| self.get(index))
    }

    /// The animations of the set that are alive.
    pub(crate) fn alive(&mut self) -> Vec<AnimationHandle> {
        self.prune();
        self.iter().collect()
    }

    /// Removes every animation.
    pub(crate) fn clear(&mut self) {
        self.0.clear();
    }
}

impl std::ops::Deref for AnimationHandle {
    type Target = AnimationCell;

    fn deref(&self) -> &AnimationCell {
        &self.0
    }
}
