/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! DocumentTimeline, https://drafts.csswg.org/web-animations-1/#the-documenttimeline-interface

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, Realm, Trace, Visitor, Vm};
use libweb_rust::web_animations::{TimelineDescription, TimelineRecordRef};

use super::animation_timeline::{AnimationTimeline, did_create_timeline, finalize_timeline};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_css_realm_document(realm: *mut c_void) -> *mut c_void;
}

/// The dictionary DocumentTimelineOptions of Animations/DocumentTimeline.idl.
pub(crate) struct DocumentTimelineOptions {
    pub(crate) origin_time: f64,
}

/// The host's `DOM::Document` associated with the Window that is the global object of the realm.
pub(crate) fn document_of_realm(realm: Gc<Realm>) -> NonNull<c_void> {
    // SAFETY: The realm is live.
    NonNull::new(unsafe { web_css_realm_document(realm.as_ptr().cast()) })
        .expect("the realm of a Window has an associated document")
}

// https://drafts.csswg.org/web-animations-1/#the-documenttimeline-interface
pub(crate) struct DocumentTimeline {
    timeline: AnimationTimeline,
}

// SAFETY: The timeline visits what it reaches.
unsafe impl Trace for DocumentTimeline {
    fn trace(&self, visitor: &mut Visitor) {
        self.timeline.trace(visitor);
    }
}

impl Inherits<AnimationTimeline> for DocumentTimeline {
    fn parent(&self) -> &AnimationTimeline {
        &self.timeline
    }
}

impl Wrappable for DocumentTimeline {
    const TRACED_ON_OWNER_THREAD_ONLY: bool = true;
    const FINALIZES: bool = true;

    fn finalize(object: Ref<Self>) {
        finalize_timeline(&object.timeline);
    }
}

impl DocumentTimeline {
    pub(crate) fn from_timeline(timeline: AnimationTimeline) -> Self {
        Self { timeline }
    }

    pub(crate) fn timeline(&self) -> &AnimationTimeline {
        &self.timeline
    }

    /// A new document timeline associated with the host's `DOM::Document` `document`, with the origin time.
    pub(crate) fn create(vm: &Vm, document: NonNull<c_void>, origin_time: f64) -> Ref<Self> {
        let record = TimelineRecordRef::new(document, TimelineDescription::Document { origin_time });
        let object = allocate(vm, |_| Self::from_timeline(AnimationTimeline::new(record)));
        did_create_timeline(cell_into_abi(object));
        object
    }

    // https://drafts.csswg.org/web-animations-1/#dom-documenttimeline-documenttimeline
    pub(crate) fn construct(cx: &mut CallContext, options: DocumentTimelineOptions) -> WebIdlResult<Ref<Self>> {
        // Creates a new DocumentTimeline. The Document with which the timeline is associated is the Document associated
        // with the Window that is the current global object.
        let document = document_of_realm(cx.realm());
        Ok(Self::create(cx.vm(), document, options.origin_time))
    }
}
