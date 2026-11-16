/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AnimationTimeline, https://drafts.csswg.org/web-animations-2/#the-animationtimeline-interface

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, Object, Trace, Visitor, Vm};
use libweb_rust::web_animations::{TimelineInterfaceKind, TimelineRecordRef};

use super::animation_playback_event::time_value_as_css_numberish;
use super::document_timeline::DocumentTimeline;
use super::scroll_timeline::ScrollTimeline;
use super::view_timeline::ViewTimeline;
use crate::bindings::conversions::CppInterface;
use core::ops::Deref;

use libjs_rust::api::{ThrowCompletionOr, Value};

use crate::bindings::conversions::ToJs;
use crate::bindings::interface::{
    Inherits, Interface, Ref, Wrappable, allocate, cell_from_abi, cell_if_abi, cell_into_abi,
};
use crate::bindings::{CallContext, WebIdlResult};
use crate::css::css_numeric_value::CSSNumberish;
use crate::internals::internal_animation_timeline::InternalAnimationTimeline;

unsafe extern "C" {
    fn web_animation_wrap_element(realm: *mut c_void, element: *mut c_void) -> *mut c_void;
    fn web_animation_element_of_wrapper(wrapper: *mut c_void) -> *mut c_void;
}

// https://drafts.csswg.org/web-animations-2/#the-animationtimeline-interface
pub(crate) struct AnimationTimeline {
    record: TimelineRecordRef,
}

// SAFETY: Visits the cells of the host that the record names: the document the timeline is associated with, and the
//         element a scroll timeline finds its scroller from.
unsafe impl Trace for AnimationTimeline {
    fn trace(&self, visitor: &mut Visitor) {
        self.record.for_each_host_cell(|cell| {
            // SAFETY: The record names live cells of the host, which this keeps alive.
            visitor.visit(unsafe { Gc::from_non_null(cell) });
        });
    }
}

impl Wrappable for AnimationTimeline {}

impl AnimationTimeline {
    /// The timeline's state, which lives as long as the timeline.
    pub(crate) fn record(&self) -> &TimelineRecordRef {
        &self.record
    }

    // https://drafts.csswg.org/web-animations-2/#dom-animationtimeline-currenttime
    pub(crate) fn current_time(&self, cx: &mut CallContext) -> WebIdlResult<Option<CSSNumberish>> {
        // Returns the current time for this timeline or null if this timeline is inactive. The value is expressed as a
        // percentage for a progress-based timeline, or as a double in milliseconds otherwise.
        // NB: Script observes the current time as the timeline is sampled for observation.
        Ok(self
            .record
            .current_time_for_observation()
            .map(|time| time_value_as_css_numberish(cx.vm(), time)))
    }

    // https://drafts.csswg.org/web-animations-2/#dom-animationtimeline-duration
    pub(crate) fn duration(&self, cx: &mut CallContext) -> WebIdlResult<Option<CSSNumberish>> {
        // Returns the duration for this timeline.
        Ok(self
            .record
            .duration()
            .map(|time| time_value_as_css_numberish(cx.vm(), time)))
    }
}

/// Finalizes a timeline: the record can outlive the timeline until what holds the record is swept.
pub(crate) fn finalize_timeline(timeline: &AnimationTimeline) {
    timeline.record.set_object_cell(None);
}

impl AnimationTimeline {
    pub(crate) fn new(record: TimelineRecordRef) -> Self {
        Self { record }
    }
}

/// Notes the cell of a new object of a timeline, which its record then reaches it through.
pub(crate) fn did_create_timeline(cell: *mut c_void) {
    // SAFETY: The cell is that of a new timeline.
    let timeline = unsafe { timeline_of_cell(cell) }.expect("the cell is that of a timeline");
    timeline.record.set_object_cell(NonNull::new(cell));
}

/// The object script sees of the timeline of `record`, which is made as script first asks for it, of the interface the
/// kind of the record picks.
pub(crate) fn timeline_object_of_record(vm: &Vm, record: &TimelineRecordRef) -> AnyAnimationTimeline {
    if let Some(cell) = record.object_cell() {
        // SAFETY: The object cell of a record is the live cell of its object.
        return unsafe { timeline_object(cell.as_ptr()) };
    }
    let timeline = AnimationTimeline::new(record.clone());
    let cell = match record.kind() {
        TimelineInterfaceKind::Document => cell_into_abi(allocate(vm, |_| DocumentTimeline::from_timeline(timeline))),
        TimelineInterfaceKind::Scroll => cell_into_abi(allocate(vm, |_| ScrollTimeline::from_timeline(timeline))),
        TimelineInterfaceKind::View => cell_into_abi(allocate(vm, |_| ViewTimeline::from_timeline(timeline))),
        TimelineInterfaceKind::Internal => {
            cell_into_abi(allocate(vm, |_| InternalAnimationTimeline::from_timeline(timeline)))
        }
    };
    did_create_timeline(cell);
    // SAFETY: The cell is that of the new object.
    unsafe { timeline_object(cell) }
}

/// The part as an AnimationTimeline of the timeline whose cell the host names.
///
/// # Safety
///
/// `cell` must be a live cell.
pub(crate) unsafe fn timeline_of_cell<'a>(cell: *mut c_void) -> Option<&'a AnimationTimeline> {
    // NB: The cells live while the host names them, which outlives the reference the caller uses.
    fn extend<'a, T: Interface>(object: Ref<T>, timeline: impl Fn(&T) -> &AnimationTimeline) -> &'a AnimationTimeline {
        let timeline: *const AnimationTimeline = timeline(&object);
        // SAFETY: The cell lives while the host names it.
        unsafe { &*timeline }
    }
    // SAFETY: Guaranteed by the caller.
    unsafe {
        if let Some(object) = cell_if_abi::<DocumentTimeline>(cell) {
            return Some(extend(object, |object| object.timeline()));
        }
        if let Some(object) = cell_if_abi::<ScrollTimeline>(cell) {
            return Some(extend(object, |object| object.timeline()));
        }
        if let Some(object) = cell_if_abi::<ViewTimeline>(cell) {
            return Some(extend(object, |object| object.scroll_timeline().timeline()));
        }
        if let Some(object) = cell_if_abi::<InternalAnimationTimeline>(cell) {
            return Some(extend(object, |object| object.timeline()));
        }
        if let Some(object) = cell_if_abi::<AnimationTimeline>(cell) {
            return Some(extend(object, |object| object));
        }
    }
    None
}

/// The wrapper of the host's `DOM::Element` in the realm of the call.
pub(crate) fn element_wrapper(cx: &CallContext, element: NonNull<c_void>) -> CppInterface {
    // SAFETY: The realm is live, and the element is a live element.
    let wrapper = unsafe { web_animation_wrap_element(cx.realm().as_ptr().cast(), element.as_ptr()) };
    let wrapper = NonNull::new(wrapper.cast::<Object>()).expect("LibWeb returns the wrapper of the element");
    // SAFETY: LibWeb returns a live wrapper of an Element.
    unsafe { CppInterface::from_wrapper(Gc::from_non_null(wrapper)) }
}

/// The host's `DOM::Element` of a wrapper of an Element.
pub(crate) fn element_of_wrapper(element: CppInterface) -> NonNull<c_void> {
    // SAFETY: The wrapper is that of a live Element.
    NonNull::new(unsafe { web_animation_element_of_wrapper(element.wrapper().as_ptr().cast()) })
        .expect("a wrapper of an Element wraps an element")
}

/// An object script sees of a timeline, of the interface that inherits from AnimationTimeline it is of.
#[derive(Clone, Copy, PartialEq, Trace)]
#[gc(crate = libjs_rust)]
#[allow(clippy::enum_variant_names, reason = "the variants have the interfaces’ names")]
pub(crate) enum AnyAnimationTimeline {
    AnimationTimeline(Ref<AnimationTimeline>),
    DocumentTimeline(Ref<DocumentTimeline>),
    ScrollTimeline(Ref<ScrollTimeline>),
    ViewTimeline(Ref<ViewTimeline>),
    InternalAnimationTimeline(Ref<InternalAnimationTimeline>),
}

impl Deref for AnyAnimationTimeline {
    type Target = AnimationTimeline;

    fn deref(&self) -> &AnimationTimeline {
        match self {
            Self::AnimationTimeline(timeline) => timeline,
            Self::DocumentTimeline(timeline) => timeline.parent(),
            Self::ScrollTimeline(timeline) => timeline.parent(),
            Self::ViewTimeline(timeline) => timeline.parent().parent(),
            Self::InternalAnimationTimeline(timeline) => timeline.parent(),
        }
    }
}

impl ToJs for AnyAnimationTimeline {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        match self {
            Self::AnimationTimeline(timeline) => timeline.to_js(cx),
            Self::DocumentTimeline(timeline) => timeline.to_js(cx),
            Self::ScrollTimeline(timeline) => timeline.to_js(cx),
            Self::ViewTimeline(timeline) => timeline.to_js(cx),
            Self::InternalAnimationTimeline(timeline) => timeline.to_js(cx),
        }
    }
}

/// The object of the timeline whose cell the host names.
///
/// # Safety
///
/// `cell` must be the live cell of a timeline.
pub(crate) unsafe fn timeline_object(cell: *mut c_void) -> AnyAnimationTimeline {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        if let Some(object) = cell_if_abi::<DocumentTimeline>(cell) {
            return AnyAnimationTimeline::DocumentTimeline(object);
        }
        if let Some(object) = cell_if_abi::<ViewTimeline>(cell) {
            return AnyAnimationTimeline::ViewTimeline(object);
        }
        if let Some(object) = cell_if_abi::<ScrollTimeline>(cell) {
            return AnyAnimationTimeline::ScrollTimeline(object);
        }
        if let Some(object) = cell_if_abi::<InternalAnimationTimeline>(cell) {
            return AnyAnimationTimeline::InternalAnimationTimeline(object);
        }
        AnyAnimationTimeline::AnimationTimeline(cell_from_abi::<AnimationTimeline>(cell))
    }
}
