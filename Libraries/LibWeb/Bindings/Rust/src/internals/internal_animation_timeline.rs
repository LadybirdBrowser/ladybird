/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! InternalAnimationTimeline, the timeline whose time tests set.

use core::cell::RefCell;
use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{CellRoot, Gc, Trace, Visitor, Vm};
use libweb_rust::web_animations::{TimelineDescription, TimelineRecordRef};

use crate::animations::animation::perform_host_effect;
use crate::animations::animation_timeline::{AnimationTimeline, finalize_timeline, timeline_object_of_record};
use crate::bindings::interface::{Inherits, Ref, Wrappable, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};

pub(crate) struct InternalAnimationTimeline {
    timeline: AnimationTimeline,
}

// SAFETY: The timeline visits what it reaches.
unsafe impl Trace for InternalAnimationTimeline {
    fn trace(&self, visitor: &mut Visitor) {
        self.timeline.trace(visitor);
    }
}

impl Inherits<AnimationTimeline> for InternalAnimationTimeline {
    fn parent(&self) -> &AnimationTimeline {
        &self.timeline
    }
}

impl Wrappable for InternalAnimationTimeline {
    const TRACED_ON_OWNER_THREAD_ONLY: bool = true;
    const FINALIZES: bool = true;

    fn finalize(object: Ref<Self>) {
        finalize_timeline(&object.timeline);
    }
}

impl InternalAnimationTimeline {
    pub(crate) fn from_timeline(timeline: AnimationTimeline) -> Self {
        Self { timeline }
    }

    pub(crate) fn timeline(&self) -> &AnimationTimeline {
        &self.timeline
    }

    /// Sets the time of the timeline, or the time a read of it observes, where `for_observation`.
    fn set_internal_time(&self, time: Option<f64>, for_observation: bool) {
        // NB: The animations the timeline updates stay alive through it, which can run script.
        let roots: RefCell<Vec<CellRoot<c_void>>> = RefCell::default();
        let keep_alive = |cell: NonNull<c_void>| {
            // SAFETY: The record names a live cell of the host.
            roots
                .borrow_mut()
                .push(CellRoot::new(unsafe { Gc::from_non_null(cell) }));
        };
        self.timeline
            .record()
            .set_internal_time(time, for_observation, &keep_alive, perform_host_effect);
    }

    pub(crate) fn set_time(&self, _: &mut CallContext, time: Option<f64>) -> WebIdlResult<()> {
        self.set_internal_time(time, false);
        Ok(())
    }

    pub(crate) fn set_time_for_observation(&self, _: &mut CallContext, time: f64) -> WebIdlResult<()> {
        self.set_internal_time(Some(time), true);
        Ok(())
    }
}

/// A new timeline of the Internals interface associated with the host's `DOM::Document` `document`.
///
/// # Safety
///
/// `vm` must be the live VM, and `document` a live document.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_internal_animation_timeline_create(vm: *mut c_void, document: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    let document = NonNull::new(document).expect("a timeline is associated with a document");
    let record = TimelineRecordRef::new(document, TimelineDescription::Internal);
    let crate::animations::animation_timeline::AnyAnimationTimeline::InternalAnimationTimeline(object) =
        timeline_object_of_record(vm, &record)
    else {
        unreachable!("the record is of an internal timeline");
    };
    cell_into_abi(object)
}
