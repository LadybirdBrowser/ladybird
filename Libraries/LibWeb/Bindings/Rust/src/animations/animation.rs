/*
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>.
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Animation, https://www.w3.org/TR/web-animations-1/#the-animation-interface

use core::cell::Cell;
use core::ffi::c_void;
use core::ops::Deref;
use core::ptr::NonNull;

use ak::Utf16String;
pub(crate) use libcompositing_rust::animation_timing::{AnimationPlayState, AnimationReplaceState};
use libcompositing_rust::animation_timing::{OptionalNumber, OptionalTimeValue, TimeValue, TimeValueType};
use libjs_rust::api::{Gc, Realm, ThrowCompletionOr, Trace, Value, Visitor, Vm};
use libweb_rust::AnimationRecordRef;
use libweb_rust::web_animations::{
    AnimationException, AnimationInterfaceKind, AnimationProcedure, AttachmentRangeEdgeValue, CssAnimationFlag,
    EffectRecordRef, HostEffect, HostEffectKind, PROPERTY_ID_ANIMATION_RANGE_END, PROPERTY_ID_ANIMATION_RANGE_START,
    PendingAnimationEvent, PlaybackEventType, StyleValue, attachment_range_edge_from_computed_value,
    numeric_type_matches_time, parse_property_value, style_value_as_host_ptr, style_value_from_host,
};

use super::animation_effect::{AnyAnimationEffect, effect_object};
use super::animation_playback_event::{
    AnimationPlaybackEvent, AnimationPlaybackEventInit, time_value_as_css_numberish,
};
use super::animation_timeline::{AnyAnimationTimeline, timeline_object_of_record};
use super::document_timeline::document_of_realm;
use crate::bindings::conversions::ToJs;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{
    Inherits, Ref, Wrappable, allocate, cell_from_abi, cell_into_abi, with_temporary_execution_context,
};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, create_promise, mark_promise_as_handled, queue_a_global_task,
    reject_promise_with_exception, resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::css::css_animation::CSSAnimation;
use crate::css::css_keyword_value::CSSKeywordValue;
use crate::css::css_numeric_value::{AnyCSSNumericValue, CSSNumberish};
use crate::css::css_transition::CSSTransition;
use crate::css::typed_om::reify_a_numeric_value;
use crate::dom::event::EventInit;

unsafe extern "C" {
    fn web_animation_perform_host_effect(effect: *const c_void);
    fn web_animation_update_style_of_element(
        element: *mut c_void,
        has_pseudo_element: bool,
        pseudo_element: u8,
        scope: StyleUpdateScope,
    );
    fn web_animation_realm_of_element(element: *mut c_void) -> *mut c_void;
    fn web_animation_absolutize_style_value(document: *mut c_void, value: *const c_void) -> *const c_void;
    fn web_animation_realm_of_document(document: *mut c_void) -> *mut c_void;
    fn web_animation_current_high_resolution_time(realm: *mut c_void) -> f64;
    fn web_animation_append_pending_playback_event(
        document: *mut c_void,
        animation: *const c_void,
        r#type: PlaybackEventType,
        current_time: OptionalTimeValue,
        timeline_time: OptionalTimeValue,
        scheduled_event_time: OptionalNumber,
        time_stamp: f64,
    );
    fn web_animation_event_set_time_stamp(event: *mut c_void, time_stamp: f64);
    fn web_animation_commit_styles(animation: *const c_void) -> CommitStylesResult;
}

/// What committing the computed styles of an animation to the style attribute of its target answers.
#[repr(u8)]
#[allow(dead_code, reason = "The host constructs the outcomes")]
enum CommitStylesResult {
    Committed,
    TargetCannotHaveInlineStyle,
    TargetIsNotBeingRendered,
}

// https://drafts.csswg.org/web-animations-2/#dictdef-timelinerangeoffset
pub(crate) struct TimelineRangeOffset {
    pub(crate) range_name: Option<Option<Utf16String>>,
    pub(crate) offset: Option<AnyCSSNumericValue>,
}

/// The union (TimelineRangeOffset or CSSNumericValue or CSSKeywordValue or DOMString) of an attachment range edge.
pub(crate) enum TimelineRangeValue {
    TimelineRangeOffset(TimelineRangeOffset),
    CSSNumericValue(AnyCSSNumericValue),
    CSSKeywordValue(Ref<CSSKeywordValue>),
    DOMString(Utf16String),
}

/// Has the host perform what a procedure asked for.
pub(crate) extern "C" fn perform_host_effect(effect: &HostEffect) {
    // SAFETY: The host performs the effects of the procedures of its animations.
    unsafe { web_animation_perform_host_effect(core::ptr::from_ref(effect).cast()) };
}

fn exception_of(exception: AnimationException) -> Exception {
    match exception {
        AnimationException::InvalidState(message) => {
            Exception::Dom("InvalidStateError", Utf16String::from_utf8(message))
        }
        AnimationException::TypeError(message) => Exception::type_error(message),
    }
}

fn run(animation: &AnimationRecordRef, procedure: AnimationProcedure) -> WebIdlResult<()> {
    animation.run(procedure, perform_host_effect).map_err(exception_of)
}

/// What the host computes the style of as script observes an animation.
#[repr(u8)]
#[derive(Clone, Copy)]
enum StyleUpdateScope {
    /// The style of the element, where it changed.
    Element,
    /// The style of the document of the element.
    Document,
}

/// Has the host compute the style `scope` names of the owning element of the CSS animation or transition `animation`,
/// where it has one.
fn update_style_of_owning_element(animation: &AnimationRecordRef, scope: StyleUpdateScope) {
    let Some((element, pseudo_element)) = animation.owning_element() else {
        return;
    };
    // SAFETY: The animation keeps its owning element alive.
    unsafe {
        web_animation_update_style_of_element(
            element.as_ptr(),
            pseudo_element.is_some(),
            pseudo_element.unwrap_or(0),
            scope,
        );
    }
}

/// Has the owning element of a CSS animation or transition compute its style, where the style changed.
pub(crate) fn update_style_if_needed(animation: &AnimationRecordRef) {
    // NB: Script observes a CSS animation or transition with the style of its owning element computed.
    update_style_of_owning_element(animation, StyleUpdateScope::Element);
}

/// The current time of the timeline of the animation as script observes it, which is unresolved where it has none.
fn timeline_time_for_observation(animation: &AnimationRecordRef) -> Option<TimeValue> {
    animation.timeline()?.current_time_for_observation()
}

/// https://www.w3.org/TR/web-animations-1/#animation-current-time
/// As script observes it.
pub(crate) fn current_time_for_observation(animation: &AnimationRecordRef) -> Option<TimeValue> {
    animation.current_time_at(timeline_time_for_observation(animation))
}

/// AD-HOC: What an update of the timing of the associated effect tells the animation.
pub(crate) fn effect_timing_changed(animation: &AnimationRecordRef) {
    animation.update_finished_state(true, None, perform_host_effect);
}

/// The realm of the host's `DOM::Element`'s node document's relevant global object.
pub(crate) fn realm_of_element(element: NonNull<c_void>) -> Gc<Realm> {
    // SAFETY: The element is live, and the host returns the live realm of its document.
    unsafe {
        Gc::from_non_null(
            NonNull::new(web_animation_realm_of_element(element.as_ptr()).cast::<Realm>())
                .expect("an element's document has a relevant realm"),
        )
    }
}

fn realm_of_document(document: NonNull<c_void>) -> Gc<Realm> {
    // SAFETY: The document is live, and the host returns its live relevant realm.
    unsafe {
        Gc::from_non_null(
            NonNull::new(web_animation_realm_of_document(document.as_ptr()).cast::<Realm>())
                .expect("a document has a relevant realm"),
        )
    }
}

/// https://w3c.github.io/hr-time/#dfn-current-high-resolution-time
/// Of the global object of the realm.
fn current_high_resolution_time(realm: Gc<Realm>) -> f64 {
    // SAFETY: The realm is live.
    unsafe { web_animation_current_high_resolution_time(realm.as_ptr().cast()) }
}

// NB: A remove event is created in the realm of the document whose replaced animations are removed, which is the
//     document for timing of the animation, and whose pending animation event queue it joins.
fn playback_event_realm(
    object_realm: Gc<Realm>,
    r#type: PlaybackEventType,
    document_for_timing: Option<NonNull<c_void>>,
) -> Gc<Realm> {
    match document_for_timing {
        Some(document) if r#type == PlaybackEventType::Remove => realm_of_document(document),
        _ => object_realm,
    }
}

// https://www.w3.org/TR/web-animations-1/#update-an-animations-finished-state
// https://www.w3.org/TR/web-animations-1/#cancel-an-animation
// https://www.w3.org/TR/web-animations-1/#remove-replaced-animations
fn playback_event_type_name(r#type: PlaybackEventType) -> &'static [u8] {
    match r#type {
        PlaybackEventType::Finish => b"finish",
        PlaybackEventType::Cancel => b"cancel",
        PlaybackEventType::Remove => b"remove",
    }
}

/// A new AnimationPlaybackEvent of the type, created in the realm, that neither bubbles nor can be canceled, and that the
/// user agent dispatches.
/// https://drafts.csswg.org/web-animations-2/#animationplaybackevent
fn create_animation_playback_event(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: PlaybackEventType,
    current_time: Option<TimeValue>,
    timeline_time: Option<TimeValue>,
) -> Ref<AnimationPlaybackEvent> {
    let event_init = AnimationPlaybackEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
        current_time: current_time.map(|time| time_value_as_css_numberish(vm, time)),
        timeline_time: timeline_time.map(|time| time_value_as_css_numberish(vm, time)),
    };
    AnimationPlaybackEvent::create(
        vm,
        realm,
        &Utf16String::from_ascii(playback_event_type_name(r#type)),
        event_init,
    )
}

/// https://drafts.csswg.org/web-animations-1/#default-document-timeline
/// Of the host's `DOM::Document`.
/// https://html.spec.whatwg.org/multipage/webappapis.html#concept-relevant-realm
/// That of the object of the animation, or for a CSS animation or transition without one, of the document of its
/// owning element, and for the animation of an SVG animation element, of the document of that element.
pub(crate) fn animation_relevant_realm(animation: &AnimationRecordRef) -> Option<Gc<Realm>> {
    if let Some(object) = existing_animation_object(animation) {
        return Some(object.realm);
    }
    let element = animation
        .owning_element()
        .map(|(element, _)| element)
        .or_else(|| animation.smil_animation_element())?;
    Some(realm_of_element(element))
}

// https://www.w3.org/TR/web-animations-1/#the-animation-interface
pub(crate) struct Animation {
    event_target: CppEventTarget,
    realm: Gc<Realm>,
    // NB: The animation's state, which lives as long as the animation.
    record: AnimationRecordRef,

    // https://www.w3.org/TR/web-animations-1/#current-ready-promise
    // NB: Created as script first asks for it. Until then, it is pending while the animation has a pending play or
    //     pause task, and resolved otherwise.
    current_ready_promise: Cell<Option<WebIdlPromise>>,

    // https://www.w3.org/TR/web-animations-1/#current-finished-promise
    // NB: Created as script first asks for it. Until then, it is resolved while the animation is finished, and pending
    //     otherwise.
    current_finished_promise: Cell<Option<WebIdlPromise>>,
}

// SAFETY: Visits the event target, the realm, the promises and the cells of the host the record names.
unsafe impl Trace for Animation {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        self.event_target.trace(visitor);
        visitor.visit(self.realm);
        if let Some(promise) = self.current_ready_promise.get() {
            visitor.visit(promise);
        }
        if let Some(promise) = self.current_finished_promise.get() {
            visitor.visit(promise);
        }
        self.record.for_each_named_cell(|cell, _| {
            // SAFETY: The record names live cells of the host, which the animation keeps alive.
            visitor.visit(unsafe { Gc::from_non_null(cell) });
        });
    }
}

impl Wrappable for Animation {
    const TRACED_ON_OWNER_THREAD_ONLY: bool = true;
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }

    const FINALIZES: bool = true;

    fn finalize(object: Ref<Self>) {
        object.finalize_animation();
    }
}

impl GetTheParent for Animation {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

/// An object script sees of an animation, of the interface its kind picks.
#[derive(Clone, Copy, PartialEq, Trace)]
#[gc(crate = libjs_rust)]
#[allow(clippy::enum_variant_names, reason = "the variants have the interfaces’ names")]
pub(crate) enum AnyAnimation {
    Animation(Ref<Animation>),
    CSSAnimation(Ref<CSSAnimation>),
    CSSTransition(Ref<CSSTransition>),
}

impl Deref for AnyAnimation {
    type Target = Animation;

    fn deref(&self) -> &Animation {
        match self {
            Self::Animation(animation) => animation,
            Self::CSSAnimation(animation) => animation.parent(),
            Self::CSSTransition(animation) => animation.parent(),
        }
    }
}

impl ToJs for AnyAnimation {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        match self {
            Self::Animation(animation) => animation.to_js(cx),
            Self::CSSAnimation(animation) => animation.to_js(cx),
            Self::CSSTransition(animation) => animation.to_js(cx),
        }
    }
}

impl AnyAnimation {
    fn cell(self) -> *mut c_void {
        match self {
            Self::Animation(animation) => cell_into_abi(animation),
            Self::CSSAnimation(animation) => cell_into_abi(animation),
            Self::CSSTransition(animation) => cell_into_abi(animation),
        }
    }

    fn event_target_owner(self) {
        match self {
            Self::Animation(animation) => animation.event_target.set_owner(animation),
            Self::CSSAnimation(animation) => animation.parent().event_target.set_owner(animation),
            Self::CSSTransition(animation) => animation.parent().event_target.set_owner(animation),
        }
    }

    fn value_in(self, vm: &Vm, realm: Gc<Realm>) -> Value {
        self.to_js(&CallContext::new(vm, realm))
            .expect("wrapping an animation does not throw")
    }
}

/// The object of the animation, if it has one.
pub(crate) fn existing_animation_object(animation: &AnimationRecordRef) -> Option<AnyAnimation> {
    let cell = animation.object_cell()?.as_ptr();
    // SAFETY: The object cell of an animation is the live cell of its object, of the interface its kind picks.
    Some(unsafe {
        match animation.kind() {
            AnimationInterfaceKind::Script => AnyAnimation::Animation(cell_from_abi(cell)),
            AnimationInterfaceKind::CssAnimation => AnyAnimation::CSSAnimation(cell_from_abi(cell)),
            AnimationInterfaceKind::CssTransition => AnyAnimation::CSSTransition(cell_from_abi(cell)),
            AnimationInterfaceKind::Smil => unreachable!("script never sees the animation of an SVG animation element"),
        }
    })
}

/// The object script sees of the animation, a CSS animation or transition or one script created, which a CSS animation
/// or transition gets as script first asks for it.
pub(crate) fn animation_object(vm: &Vm, animation: &AnimationRecordRef) -> AnyAnimation {
    if let Some(object) = existing_animation_object(animation) {
        return object;
    }
    let (owner, _) = animation
        .owning_element()
        .expect("an animation without an object is a CSS animation or transition, which has an owning element");
    // NB: The realm is that of the document of the owning element as script first asks for the object.
    let realm = realm_of_element(owner);
    let base = Animation::new(realm, animation.clone());
    let object = match animation.kind() {
        AnimationInterfaceKind::CssAnimation => AnyAnimation::CSSAnimation(CSSAnimation::allocate(vm, base)),
        AnimationInterfaceKind::CssTransition => AnyAnimation::CSSTransition(CSSTransition::allocate(vm, base)),
        AnimationInterfaceKind::Script | AnimationInterfaceKind::Smil => {
            unreachable!("only CSS animations and transitions get their objects lazily")
        }
    };
    object.event_target_owner();
    animation.set_object_cell(NonNull::new(object.cell()));
    object
}

impl Animation {
    pub(crate) fn new(realm: Gc<Realm>, record: AnimationRecordRef) -> Self {
        Self {
            event_target: CppEventTarget::new(),
            realm,
            record,
            current_ready_promise: Cell::new(None),
            current_finished_promise: Cell::new(None),
        }
    }

    /// The animation's state, which lives as long as the animation.
    pub(crate) fn record(&self) -> &AnimationRecordRef {
        &self.record
    }

    pub(crate) fn finalize_animation(&self) {
        // NB: The record can outlive the animation until what holds the record is swept.
        self.record.did_finalize_object();
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-animation
    pub(crate) fn construct(
        cx: &mut CallContext,
        effect: Option<AnyAnimationEffect>,
        timeline: Option<Option<AnyAnimationTimeline>>,
    ) -> WebIdlResult<Ref<Self>> {
        let vm = cx.vm();
        let realm = cx.realm();
        // NB: A missing timeline is the default document timeline of the Document associated with the Window that is
        //     the current global object.
        let timeline = match timeline {
            Some(timeline) => timeline.map(|timeline| timeline.record().clone()),
            None => Some(libweb_rust::web_animations::TimelineRecordRef::default_of_document(
                document_of_realm(realm),
            )),
        };

        Ok(Self::create(
            vm,
            realm,
            effect.as_ref().map(|effect| effect.record()),
            timeline.as_ref(),
        ))
    }

    /// The procedure of the Animation() constructor, in the realm.
    pub(crate) fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        effect: Option<&EffectRecordRef>,
        timeline: Option<&libweb_rust::web_animations::TimelineRecordRef>,
    ) -> Ref<Self> {
        // 1. Let animation be a new Animation object.
        let animation = allocate(vm, |_| Self::new(realm, AnimationRecordRef::new_script_animation()));
        animation.event_target.set_owner(animation);
        animation.record.set_object_cell(NonNull::new(cell_into_abi(animation)));

        // 2. Run the procedure to set the timeline of an animation on animation passing timeline as the new timeline;
        //    or, if the timeline argument is missing, passing the default document timeline of the Document associated
        //    with the Window that is the current global object.
        animation.record.set_timeline(timeline, perform_host_effect);

        // 3. Run the procedure to set the associated effect of an animation on animation passing source as the new
        //    effect.
        animation.record.set_effect(effect, perform_host_effect);

        animation
    }

    /// The object of the animation in the realm of its record, with its promises settled in that realm.
    fn object(&self) -> AnyAnimation {
        existing_animation_object(&self.record).expect("an animation is the object of its record while it is alive")
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-id
    pub(crate) fn id(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.record.dom_id())
    }

    pub(crate) fn set_id(&self, _: &mut CallContext, id: Utf16String) -> WebIdlResult<()> {
        self.record.set_dom_id(&id);
        Ok(())
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-effect
    pub(crate) fn effect(&self, cx: &mut CallContext) -> WebIdlResult<Option<AnyAnimationEffect>> {
        Ok(self.record.effect().map(|effect| effect_object(cx.vm(), &effect)))
    }

    pub(crate) fn set_effect(&self, _: &mut CallContext, effect: Option<AnyAnimationEffect>) -> WebIdlResult<()> {
        // Setting this attribute updates the object’s associated effect using the procedure to set the associated
        // effect of an animation.
        // NB: The procedure runs in the record. Where new effect is the associated effect of another animation, it
        //     sets that one's associated effect too, on that one's timeline. The old effect stays alive while the host
        //     performs what the procedure asks, which recomputes the style of its target.
        let _old_effect: Option<EffectRecordRef> = self.record.effect();
        self.record
            .set_effect(effect.as_ref().map(|effect| effect.record()), perform_host_effect);
        Ok(())
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-timeline
    pub(crate) fn timeline(&self, cx: &mut CallContext) -> WebIdlResult<Option<AnyAnimationTimeline>> {
        update_style_if_needed(&self.record);
        Ok(self
            .record
            .timeline()
            .map(|timeline| timeline_object_of_record(cx.vm(), &timeline)))
    }

    // https://www.w3.org/TR/web-animations-1/#animation-set-the-timeline-of-an-animation
    // https://drafts.csswg.org/web-animations-2/#setting-the-timeline
    pub(crate) fn set_timeline(&self, _: &mut CallContext, timeline: Option<AnyAnimationTimeline>) -> WebIdlResult<()> {
        // AD-HOC: When the timeline of a CSS animation is modified by the author from JS we should no longer apply
        //         changes to the `animation-timeline` property. See https://github.com/w3c/csswg-drafts/issues/13472
        if self.record.kind() == AnimationInterfaceKind::CssAnimation {
            self.record.set_css_animation_flag(CssAnimationFlag::ScriptSetTimeline);
        }
        // NB: The procedure runs in the record.
        self.record
            .set_timeline(timeline.as_ref().map(|timeline| timeline.record()), perform_host_effect);
        Ok(())
    }

    // https://drafts.csswg.org/web-animations-2/#validating-a-css-numberish-time
    fn validate_a_css_numberish_time(&self, time: Option<CSSNumberish>) -> WebIdlResult<Option<TimeValue>> {
        // The procedure to validate a CSSNumberish time for an input value of time is based on the first condition
        // that matches:
        let progress_based = self.record.has_progress_based_timeline();
        let numeric_type = match &time {
            Some(CSSNumberish::CSSNumericValue(value)) => Some(value.numeric_type()),
            _ => None,
        };

        // If all of the following conditions are true:
        //  - The animation is associated with a progress-based timeline, and
        //  - time is not a CSSNumericValue with percent units:
        if progress_based
            && !numeric_type
                .as_ref()
                .is_some_and(|numeric_type| numeric_type.matches_percentage())
        {
            // throw a TypeError.
            // return false;
            return Err(Exception::type_error(
                "CSSNumberish must be a percentage for progress-based animations",
            ));
        }

        // If all of the following conditions are true:
        //  - The animation is not associated with a progress-based timeline, and
        //  - time is a CSSNumericValue, and
        //  - the units of time are not duration units:
        // AD-HOC: While it's not mentioned in the spec WPT also expects us to support CSSNumericValue number value, see
        //         https://github.com/w3c/csswg-drafts/issues/13196
        if !progress_based
            && let Some(numeric_type) = &numeric_type
            && !numeric_type_matches_time(numeric_type)
            && !numeric_type.matches_number(None)
        {
            // throw a TypeError.
            // return false.
            return Err(Exception::type_error(
                "CSSNumericValue must be a time for non-progress based animations",
            ));
        }

        // Otherwise
        // return true.

        // AD-HOC: The spec doesn't say when we should absolutize the validated value so we do it here and return the
        //         absolutized value instead of a boolean
        let Some(time) = time else {
            return Ok(None);
        };
        Ok(Some(match time {
            CSSNumberish::Double(milliseconds) => TimeValue {
                r#type: TimeValueType::Milliseconds,
                value: milliseconds,
            },
            CSSNumberish::CSSNumericValue(value) => {
                let calculation = value.create_calculation_node()?;
                libweb_rust::web_animations::time_value_of_calculation(calculation, value.numeric_type())
                    .ok_or_else(|| Exception::type_error("Invalid CSSNumberish time"))?
            }
        }))
    }

    // https://drafts.csswg.org/web-animations-2/#dom-animation-starttime
    pub(crate) fn start_time(&self, cx: &mut CallContext) -> WebIdlResult<Option<CSSNumberish>> {
        update_style_if_needed(&self.record);
        Ok(self
            .record
            .playback()
            .start_time
            .get()
            .map(|time| time_value_as_css_numberish(cx.vm(), time)))
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-starttime
    // https://drafts.csswg.org/web-animations-1/#set-the-start-time
    // https://drafts.csswg.org/web-animations-2/#set-the-start-time
    pub(crate) fn set_start_time(&self, _: &mut CallContext, new_start_time: Option<CSSNumberish>) -> WebIdlResult<()> {
        // Setting this attribute updates the start time using the procedure to set the start time of this object to
        // the new value.

        // 1. Let valid start time be the result of running the validate a CSSNumberish time procedure with new start
        //    time as the input.
        // 2. If valid start time is false, abort this procedure.
        // AD-HOC: The validate_a_css_numberish_time throws on validation failure which is handled by the ? operator so
        //         there is no need to assign the `valid start time` variable here.
        let new_start_time = self.validate_a_css_numberish_time(new_start_time)?;

        // NB: The other steps run in the record.
        self.record.set_start_time(new_start_time, perform_host_effect);
        Ok(())
    }

    // https://drafts.csswg.org/web-animations-2/#dom-animation-currenttime
    pub(crate) fn current_time(&self, cx: &mut CallContext) -> WebIdlResult<Option<CSSNumberish>> {
        update_style_if_needed(&self.record);
        Ok(current_time_for_observation(&self.record).map(|time| time_value_as_css_numberish(cx.vm(), time)))
    }

    // https://drafts.csswg.org/web-animations-1/#animation-set-the-current-time
    // https://drafts.csswg.org/web-animations-2/#set-the-current-time
    pub(crate) fn set_current_time(&self, _: &mut CallContext, seek_time: Option<CSSNumberish>) -> WebIdlResult<()> {
        // AD-HOC: We validate here instead of within silently_set_current_time so we have access to the `TimeValue`
        //         value within this function.
        let seek_time = self.validate_a_css_numberish_time(seek_time)?;

        // NB: The procedure runs in the record.
        self.record
            .set_current_time(seek_time, perform_host_effect)
            .map_err(exception_of)
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-playbackrate
    pub(crate) fn playback_rate(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.record.playback_rate())
    }

    pub(crate) fn set_playback_rate(&self, _: &mut CallContext, playback_rate: f64) -> WebIdlResult<()> {
        // Setting this attribute follows the procedure to set the playback rate of this object to the new value.
        run(&self.record, AnimationProcedure::SetPlaybackRate(playback_rate))
    }

    // https://drafts.csswg.org/web-animations-2/#dom-animation-rangestart
    pub(crate) fn range_start(&self, cx: &mut CallContext) -> WebIdlResult<TimelineRangeValue> {
        update_style_if_needed(&self.record);
        let [start, _] = self.record.range();
        Ok(range_edge_to_bindings(cx.vm(), start, false))
    }

    pub(crate) fn set_range_start(&self, cx: &mut CallContext, value: TimelineRangeValue) -> WebIdlResult<()> {
        self.set_range_edge(cx, value, false)
    }

    // https://drafts.csswg.org/web-animations-2/#dom-animation-rangeend
    pub(crate) fn range_end(&self, cx: &mut CallContext) -> WebIdlResult<TimelineRangeValue> {
        update_style_if_needed(&self.record);
        let [_, end] = self.record.range();
        Ok(range_edge_to_bindings(cx.vm(), end, true))
    }

    pub(crate) fn set_range_end(&self, cx: &mut CallContext, value: TimelineRangeValue) -> WebIdlResult<()> {
        self.set_range_edge(cx, value, true)
    }

    fn set_range_edge(&self, _: &mut CallContext, value: TimelineRangeValue, is_end: bool) -> WebIdlResult<()> {
        let edge = parse_timeline_range_edge(document_of_realm(self.realm), value, is_end)?;
        self.record.set_range_edge(is_end, edge, perform_host_effect);
        // AD-HOC: Like the timeline, a range edge script sets takes precedence over the animation-range-start or
        //         animation-range-end property from then on, as WPT expects
        //         (scroll-animations/css/animation-range-ignored.html).
        if self.record.kind() == AnimationInterfaceKind::CssAnimation {
            self.record.set_css_animation_flag(if is_end {
                CssAnimationFlag::ScriptSetRangeEnd
            } else {
                CssAnimationFlag::ScriptSetRangeStart
            });
        }
        Ok(())
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-playstate
    pub(crate) fn play_state(&self, _: &mut CallContext) -> WebIdlResult<AnimationPlayState> {
        // NB: Script observes the play state of an animation with the style of the document of its owning element
        //     computed, at the current time it observes.
        update_style_of_owning_element(&self.record, StyleUpdateScope::Document);
        let current_time = current_time_for_observation(&self.record);
        Ok(self.record.play_state_at(current_time))
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-replacestate
    pub(crate) fn replace_state(&self, _: &mut CallContext) -> WebIdlResult<AnimationReplaceState> {
        Ok(self.record.replace_state())
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-pending
    pub(crate) fn pending(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        update_style_if_needed(&self.record);
        Ok(self.record.pending())
    }

    // https://www.w3.org/TR/web-animations-1/#current-ready-promise
    fn current_ready_promise(&self, vm: &Vm) -> WebIdlPromise {
        if let Some(promise) = self.current_ready_promise.get() {
            return promise;
        }
        // The current ready promise is initially a resolved Promise created using the procedure to create a new
        // resolved Promise with the animation itself as its value and created in the relevant Realm of the animation.
        // NB: A promise the procedures replaced, which script did not ask for yet, is pending while the animation has
        //     a pending play or pause task.
        let promise = create_promise(vm, self.realm);
        self.current_ready_promise.set(Some(promise));
        if !self.record.pending() {
            resolve_promise(vm, promise, self.object().value_in(vm, self.realm));
        }
        promise
    }

    // https://www.w3.org/TR/web-animations-1/#current-finished-promise
    fn current_finished_promise(&self, vm: &Vm) -> WebIdlPromise {
        if let Some(promise) = self.current_finished_promise.get() {
            return promise;
        }
        // The current finished promise is initially a pending Promise object.
        // NB: A promise the procedures resolved, which script did not ask for yet, is resolved while the animation is
        //     finished.
        let promise = create_promise(vm, self.realm);
        self.current_finished_promise.set(Some(promise));
        if self.record.playback().is_finished {
            resolve_promise(vm, promise, self.object().value_in(vm, self.realm));
        }
        promise
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-ready
    pub(crate) fn ready(&self, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        update_style_if_needed(&self.record);
        Ok(self.current_ready_promise(cx.vm()))
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-finished
    pub(crate) fn finished(&self, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        update_style_if_needed(&self.record);
        // NB: An animation that script observes finished ahead of the timeline's own current time settles its finished
        //     promise as it reads it, at the time it observes.
        if let Some(timeline) = self.record.timeline() {
            let observed_timeline_time = timeline_time_for_observation(&self.record);
            let observed_current_time = self.record.current_time_at(observed_timeline_time);
            let current_time = self.record.current_time_at(timeline.current_time());
            if !self.record.playback().is_finished
                && self.record.play_state_at(current_time) != AnimationPlayState::Finished
                && self.record.play_state_at(observed_current_time) == AnimationPlayState::Finished
            {
                let previous_override = timeline.style_sampling_override();
                timeline.set_style_sampling_override(Some(observed_timeline_time));
                self.record
                    .update_finished_state(false, observed_current_time, perform_host_effect);
                timeline.set_style_sampling_override(previous_override);
            }
        }
        Ok(self.current_finished_promise(cx.vm()))
    }

    /// Settles a promise of the animation, as a procedure asked.
    fn perform_promise_effect(&self, vm: &Vm, kind: HostEffectKind) {
        let reject = |promise: &Cell<Option<WebIdlPromise>>| {
            // OPTIMIZATION: A promise that was never created is one nothing can observe. Rejecting it does nothing,
            //               and the new promise the record then implies is just like it, so this only matters for a
            //               created one.
            let Some(promise) = promise.take() else {
                return;
            };
            reject_promise_with_exception(
                vm,
                promise,
                Exception::Dom("AbortError", Utf16String::from_ascii(b"Animation was cancelled")),
            );
            mark_promise_as_handled(vm, promise);
        };
        let resolve = |promise: &Cell<Option<WebIdlPromise>>| {
            // NB: Resolving a promise with the animation reads its "then" property, which script can observe, so a
            //     promise that was never created is created to be resolved.
            let promise = match promise.get() {
                Some(promise) => promise,
                None => {
                    let created = create_promise(vm, self.realm);
                    promise.set(Some(created));
                    created
                }
            };
            resolve_promise(vm, promise, self.object().value_in(vm, self.realm));
        };
        match kind {
            // NB: The new promise is the pending one the record now implies, which script creates as it asks for it.
            HostEffectKind::ReplaceReadyPromise => self.current_ready_promise.set(None),
            HostEffectKind::ResolveReadyPromise => resolve(&self.current_ready_promise),
            HostEffectKind::RejectReadyPromise => reject(&self.current_ready_promise),
            HostEffectKind::ReplaceFinishedPromise => self.current_finished_promise.set(None),
            HostEffectKind::ResolveFinishedPromise => resolve(&self.current_finished_promise),
            HostEffectKind::RejectFinishedPromise => reject(&self.current_finished_promise),
            _ => unreachable!("the effect is one of the promises of the animation"),
        }
    }

    /// Creates an AnimationPlaybackEvent of the animation and appends it to the pending animation event queue of the
    /// animation's document for timing, or queues a task to dispatch it at the animation without one, as a procedure
    /// asked.
    fn queue_playback_event(&self, vm: &Vm, effect: &HostEffect) {
        let r#type = effect.playback_event_type;
        let timeline = self.record.timeline();
        let document_for_timing = timeline.as_ref().map(|timeline| timeline.document());
        let event_realm = playback_event_realm(self.realm, r#type, document_for_timing);

        // If animation has a document for timing, then append finishEvent to its document for timing's pending
        // animation event queue along with its target, animation.
        // NB: The same steps apply to cancelEvent and removeEvent.
        if let (Some(timeline), Some(document)) = (timeline, document_for_timing) {
            // https://www.w3.org/TR/web-animations-1/#animation-time-to-origin-relative-time
            // NB: The procedure converted the time to a timeline time.
            let scheduled_event_time = effect
                .scheduled_timeline_time
                .get()
                .and_then(|time| timeline.convert_a_timeline_time_to_an_origin_relative_time(time));
            // NB: The queue holds what the event is, which the document creates as it dispatches it, with the time
            //     stamp of now, when the event is created in the spec.
            // SAFETY: The document and the animation are live.
            unsafe {
                web_animation_append_pending_playback_event(
                    document.as_ptr(),
                    self.record.as_host_ptr(),
                    r#type,
                    effect.current_time,
                    effect.timeline_time,
                    scheduled_event_time.into(),
                    current_high_resolution_time(event_realm),
                );
            }
            return;
        }

        let event = create_animation_playback_event(
            vm,
            event_realm,
            r#type,
            effect.current_time.get(),
            effect.timeline_time.get(),
        );

        // Otherwise, queue a task to dispatch finishEvent at animation. The task source for this task is the DOM
        // manipulation task source.
        let task = queue_a_global_task(
            vm,
            self.realm.global_object(),
            TaskSource::DOMManipulation,
            (self.object(), event),
            |_, (animation, event)| {
                animation.event_target.dispatch_event(event.event());
            },
        );
        // NB: The task that dispatches a finish event is the one the record notes as its finish notification task.
        if r#type == PlaybackEventType::Finish {
            self.record.set_finish_notification_task(task.host_id());
        }
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-cancel
    pub(crate) fn cancel(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // NB: Canceling an animation that is not idle settles its promises and creates its cancel event.
        if self.record.is_idle() {
            return run(&self.record, AnimationProcedure::Cancel);
        }
        with_temporary_execution_context(self.realm, || run(&self.record, AnimationProcedure::Cancel))
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-finish
    pub(crate) fn finish(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // NB: Finishing an animation can resolve its current ready promise.
        with_temporary_execution_context(self.realm, || run(&self.record, AnimationProcedure::Finish))
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-play
    pub(crate) fn play(&self, _: &mut CallContext) -> WebIdlResult<()> {
        run(&self.record, AnimationProcedure::Play)
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-pause
    pub(crate) fn pause(&self, _: &mut CallContext) -> WebIdlResult<()> {
        run(&self.record, AnimationProcedure::Pause)
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-updateplaybackrate
    pub(crate) fn update_playback_rate(&self, _: &mut CallContext, playback_rate: f64) -> WebIdlResult<()> {
        run(&self.record, AnimationProcedure::UpdatePlaybackRate(playback_rate))
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-reverse
    pub(crate) fn reverse(&self, _: &mut CallContext) -> WebIdlResult<()> {
        run(&self.record, AnimationProcedure::Reverse)
    }

    // https://drafts.csswg.org/web-animations-1/#dom-animation-commitstyles
    pub(crate) fn commit_styles(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // SAFETY: The animation is live for the call, and the host commits its styles without retaining it.
        match unsafe { web_animation_commit_styles(self.record.as_host_ptr()) } {
            CommitStylesResult::Committed => Ok(()),
            CommitStylesResult::TargetCannotHaveInlineStyle => Err(Exception::Dom(
                "NoModificationAllowedError",
                Utf16String::from_ascii(b"The animation target cannot have an inline style"),
            )),
            CommitStylesResult::TargetIsNotBeingRendered => Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_ascii(b"The animation target is not being rendered"),
            )),
        }
    }

    // https://www.w3.org/TR/web-animations-1/#dom-animation-persist
    pub(crate) fn persist(&self, _: &mut CallContext) -> WebIdlResult<()> {
        run(&self.record, AnimationProcedure::Persist)
    }

    /// The bytes of the object, and those of the object of its associated effect.
    pub(crate) fn memory_size(object: AnyAnimation) -> usize {
        let object_size = match object {
            AnyAnimation::Animation(_) => size_of::<Animation>(),
            AnyAnimation::CSSAnimation(_) => size_of::<CSSAnimation>(),
            AnyAnimation::CSSTransition(_) => size_of::<CSSTransition>(),
        };
        let effect_size = object
            .record
            .effect()
            .and_then(|effect| super::animation_effect::existing_effect_object(&effect))
            .map_or(0, |effect| effect.memory_size());
        object_size + effect_size
    }
}

/// The value the attribute rangeStart, or rangeEnd where `is_end`, reads of an edge: either a TimelineRangeOffset or the
/// DOMString "normal".
fn range_edge_to_bindings(vm: &Vm, edge: AttachmentRangeEdgeValue, is_end: bool) -> TimelineRangeValue {
    use crate::css::css_unit_value::CSSUnitValue;

    if edge.is_normal(is_end) {
        return TimelineRangeValue::DOMString(Utf16String::from_ascii(b"normal"));
    }

    let px = AnyCSSNumericValue::CSSUnitValue(CSSUnitValue::create(vm, edge.px, "px"));
    let percentage = AnyCSSNumericValue::CSSUnitValue(CSSUnitValue::create(vm, edge.percentage, "percent"));
    let mut offset = if edge.px == 0.0 { percentage } else { px };
    if edge.px != 0.0 && edge.percentage != 0.0 {
        let values = libjs_rust::api::MarkedVec::new(vm);
        values.push(CSSNumberish::CSSNumericValue(percentage));
        if let Ok(sum) = px.add(vm, &values) {
            offset = sum;
        }
    }
    if let Some(calculated) = &edge.calculated
        && let Some(reified) = reify_a_numeric_value(vm, calculated)
    {
        offset = reified;
    }

    let range_name = libweb_rust::web_animations::timeline_range_name_to_string(edge.name)
        .map(|name| Utf16String::from_ascii(name.as_bytes()));
    TimelineRangeValue::TimelineRangeOffset(TimelineRangeOffset {
        range_name: Some(range_name),
        offset: Some(offset),
    })
}

fn invalid_timeline_range() -> Exception {
    Exception::type_error("Invalid timeline range")
}

/// https://drafts.csswg.org/web-animations-2/#dom-keyframeanimationoptions-rangestart
/// The edge `value` gives as the start of an attachment range, or as its end where `is_end`, for the host's
/// `DOM::Document` `document`.
pub(crate) fn parse_timeline_range_edge(
    document: NonNull<c_void>,
    value: TimelineRangeValue,
    is_end: bool,
) -> WebIdlResult<AttachmentRangeEdgeValue> {
    let property = if is_end {
        PROPERTY_ID_ANIMATION_RANGE_END
    } else {
        PROPERTY_ID_ANIMATION_RANGE_START
    };
    // A DOMString value is parsed as an 'animation-range-end' value to yield a TimelineRangeOffset rangeName and offset
    // or alternatively a CSSKeywordValue with the value of "normal"; a CSSNumericValue is interpreted as a
    // TimelineRangeOffset with that offset and a null rangeName.
    // NB: The spec says 'animation-range-end' for rangeStart too; a start edge is parsed as an
    //     'animation-range-start' value.
    let parse = |text: &Utf16String| -> WebIdlResult<AttachmentRangeEdgeValue> {
        let parsed = parse_property_value(property, text).ok_or_else(invalid_timeline_range)?;
        // NB: Arbitrary substitution functions and CSS-wide keywords have nothing to substitute or inherit from here.
        if parsed.is_css_wide_keyword() || parsed.is_unresolved() || parsed.is_pending_substitution() {
            return Err(invalid_timeline_range());
        }
        // SAFETY: The document and the value are live, and the host hands over a reference to the absolutized value.
        let computed: StyleValue = unsafe {
            style_value_from_host(web_animation_absolutize_style_value(
                document.as_ptr(),
                style_value_as_host_ptr(&parsed),
            ))
        };
        attachment_range_edge_from_computed_value(&computed, is_end).ok_or_else(invalid_timeline_range)
    };
    let normal = || Utf16String::from_ascii(b"normal");
    match value {
        TimelineRangeValue::TimelineRangeOffset(offset) => {
            let mut text = String::new();
            if let Some(Some(range_name)) = &offset.range_name {
                text.push_str(&String::from_utf16_lossy(&range_name.to_utf16()));
            }
            if let Some(offset) = offset.offset {
                text.push(' ');
                text.push_str(&String::from_utf16_lossy(&offset.to_css_string().to_utf16()));
            }
            // NB: A TimelineRangeOffset that gives neither a range name nor an offset is the default edge.
            if text.is_empty() {
                return parse(&normal());
            }
            parse(&Utf16String::from_utf8(&text))
        }
        TimelineRangeValue::CSSNumericValue(offset) => parse(&offset.to_css_string()),
        // Passing a CSSKeywordValue with a value other than "normal" as the rangeStart or rangeEnd value to any API
        // defined to accept KeyframeAnimationOptions throws a TypeError.
        TimelineRangeValue::CSSKeywordValue(keyword) => {
            if !String::from_utf16_lossy(&keyword.raw_value().to_utf16()).eq_ignore_ascii_case("normal") {
                return Err(invalid_timeline_range());
            }
            parse(&normal())
        }
        TimelineRangeValue::DOMString(text) => parse(&text),
    }
}

/// The realm of the object of the animation, or null where it has none.
///
/// # Safety
///
/// `animation` must be an animation the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_animation_object_realm(animation: *const c_void) -> *mut c_void {
    // SAFETY: Guaranteed by the caller.
    let animation = unsafe { AnimationRecordRef::retained_from_host(animation) };
    existing_animation_object(&animation).map_or(core::ptr::null_mut(), |object| object.realm.as_ptr().cast())
}

/// The bytes of the object of the animation and of the object of its associated effect, or zero where the animation has
/// no object.
///
/// # Safety
///
/// `animation` must be an animation the host holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_animation_object_memory_size(animation: *const c_void) -> usize {
    // SAFETY: Guaranteed by the caller.
    let animation = unsafe { AnimationRecordRef::retained_from_host(animation) };
    existing_animation_object(&animation).map_or(0, Animation::memory_size)
}

/// Performs what a procedure asked of the object of the animation `effect` names, where it has one: settles one of its
/// promises, or queues one of its playback events.
///
/// # Safety
///
/// `vm` must be the live VM, and `effect` an effect the host performs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_animation_perform_object_effect(vm: &Vm, effect: &HostEffect) {
    // SAFETY: The effect names an animation the host holds a reference to.
    let animation = unsafe { AnimationRecordRef::retained_from_host(effect.animation.as_ptr().cast()) };
    // NB: Only an animation with an object has promises script can observe, and hears its playback events.
    let Some(object) = existing_animation_object(&animation) else {
        return;
    };
    match effect.kind {
        HostEffectKind::QueuePlaybackEvent => object.queue_playback_event(vm, effect),
        kind => object.perform_promise_effect(vm, kind),
    }
}

/// Dispatches the AnimationPlaybackEvent the pending animation event queue of the host's `DOM::Document` `document`
/// held for the animation, which the document created with the time stamp `time_stamp` as it was queued.
fn dispatch_pending_playback_event(
    vm: &Vm,
    animation: &AnimationRecordRef,
    document: NonNull<c_void>,
    event: &PendingAnimationEvent,
) {
    let object = existing_animation_object(animation).expect("only an animation with an object queues playback events");
    let realm = playback_event_realm(object.realm, event.playback_event_type, Some(document));
    let playback_event = create_animation_playback_event(
        vm,
        realm,
        event.playback_event_type,
        event.current_time.get(),
        event.timeline_time.get(),
    );
    // SAFETY: The event is live.
    unsafe { web_animation_event_set_time_stamp(playback_event.event().as_ptr(), event.time_stamp) };
    object.event_target.dispatch_event(playback_event.event());
}

/// Creates the event of a CSS animation or transition the pending animation event queue held, and dispatches it at the
/// owning element of the animation through `host`.
fn dispatch_pending_css_event(
    vm: &Vm,
    animation: &AnimationRecordRef,
    event: &PendingAnimationEvent,
    host: &PendingAnimationEventHost,
) {
    use crate::css::animation_event::{AnimationEvent, AnimationEventInit, EventAnimation};
    use crate::css::transition_event::{TransitionEvent, TransitionEventInit};
    use libweb_rust::web_animations::{CssEventType, pseudo_element_name};

    let (owning_element, pseudo_element) = animation
        .owning_element()
        .expect("a CSS animation or transition with events has an owning element");
    let pseudo_element = pseudo_element.map_or_else(Utf16String::default, |pseudo_element| {
        Utf16String::from_utf8(&format!("::{}", pseudo_element_name(pseudo_element)))
    });
    let event_name = Utf16String::from_utf8(match event.css_event_type {
        CssEventType::AnimationStart => "animationstart",
        CssEventType::AnimationEnd => "animationend",
        CssEventType::AnimationIteration => "animationiteration",
        CssEventType::AnimationCancel => "animationcancel",
        CssEventType::TransitionRun => "transitionrun",
        CssEventType::TransitionStart => "transitionstart",
        CssEventType::TransitionEnd => "transitionend",
        CssEventType::TransitionCancel => "transitioncancel",
    });
    let realm = realm_of_element(owning_element);
    let parent = EventInit {
        bubbles: true,
        cancelable: false,
        composed: false,
    };
    let event_animation = Some(EventAnimation::of_record(animation.clone()));
    let css_event = if animation.kind() == AnimationInterfaceKind::CssTransition {
        let init = TransitionEventInit {
            parent,
            animation: None,
            elapsed_time: event.elapsed_time,
            property_name: animation.css_transition_property(),
            pseudo_element,
        };
        TransitionEvent::create_for_animation(vm, realm, &event_name, init, event_animation)
            .event()
            .clone()
    } else {
        let init = AnimationEventInit {
            parent,
            animation: None,
            animation_name: animation.css_animation_name(),
            elapsed_time: event.elapsed_time,
            pseudo_element,
        };
        AnimationEvent::create_for_animation(vm, realm, &event_name, init, event_animation)
            .event()
            .clone()
    };
    // SAFETY: The event is live.
    unsafe { web_animation_event_set_time_stamp(css_event.as_ptr(), event.time_stamp) };
    // SAFETY: The owning element is live, as the animation keeps it alive, and so is the event.
    unsafe { (host.dispatch_at_element)(host.context, owning_element, css_event.as_ptr()) };
}

/// What dispatching the events of the pending animation event queue of a document asks of the host, which hands each
/// function `context`.
#[repr(C)]
pub struct PendingAnimationEventHost {
    pub context: *mut c_void,
    /// Keeps the animation, which the host holds a reference to, alive until the call that handed it the function
    /// returns, since dispatching an event runs script, which can run the GC.
    pub keep_alive: unsafe extern "C" fn(context: *mut c_void, animation: *const c_void),
    /// Dispatches the host's `DOM::Event` `event` at the host's `DOM::Element` `element`.
    pub dispatch_at_element: unsafe extern "C" fn(context: *mut c_void, element: NonNull<c_void>, event: *mut c_void),
    /// Dispatches the time event of the SMIL animation `animation` the queue held.
    pub dispatch_time_event:
        unsafe extern "C" fn(context: *mut c_void, animation: *const c_void, event: &PendingAnimationEvent),
}

/// https://www.w3.org/TR/web-animations-1/#update-animations-and-send-events
/// Steps 4 to 7 for the host's `DOM::Document` `document`, whose animations the host's `DocumentAnimations`
/// `animations` are, comparing elements in tree order with `tree_order`.
///
/// # Safety
///
/// `vm` must be the live VM, `document` a live document and `animations` what it knows of animations, which only
/// dispatching an event may reach again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_animation_dispatch_pending_animation_events(
    vm: &Vm,
    document: NonNull<c_void>,
    animations: NonNull<c_void>,
    tree_order: unsafe extern "C" fn(first: *mut c_void, second: *mut c_void) -> i32,
    host: &PendingAnimationEventHost,
) {
    use libweb_rust::web_animations::{PendingAnimationEventKind, take_pending_animation_events_to_dispatch};
    // 4. Let events to dispatch be a copy of doc’s pending animation event queue.
    // 5. Clear doc’s pending animation event queue.
    // 6. Perform a stable sort of the animation events in events to dispatch as follows:
    // SAFETY: Guaranteed by the caller; nothing else reaches the queue until the events are taken.
    let events_to_dispatch = unsafe { take_pending_animation_events_to_dispatch(animations, tree_order) };
    // NB: Dispatching an event runs script, which can run the GC, which must not collect the animation of an event yet to
    //     be dispatched.
    for (animation, _) in &events_to_dispatch {
        // SAFETY: The host keeps alive an animation it holds a reference to.
        unsafe { (host.keep_alive)(host.context, animation.as_host_ptr()) };
    }
    // 7. Dispatch each of the events in events to dispatch at their corresponding target using the order established in
    //    the previous step.
    for (animation, event) in &events_to_dispatch {
        match event.kind {
            PendingAnimationEventKind::Css => dispatch_pending_css_event(vm, animation, event, host),
            PendingAnimationEventKind::Playback => dispatch_pending_playback_event(vm, animation, document, event),
            // SAFETY: The host dispatches the time event of an animation it keeps alive.
            PendingAnimationEventKind::Time => unsafe {
                (host.dispatch_time_event)(host.context, animation.as_host_ptr(), event);
            },
        }
    }
}
