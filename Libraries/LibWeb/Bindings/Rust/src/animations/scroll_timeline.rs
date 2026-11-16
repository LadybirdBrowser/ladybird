/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ScrollTimeline, https://drafts.csswg.org/scroll-animations-1/#scrolltimeline

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Trace, Visitor, Vm};
use libweb_rust::web_animations::{TimelineDescription, TimelineRecordRef};

use super::animation_timeline::{
    AnimationTimeline, did_create_timeline, element_of_wrapper, element_wrapper, finalize_timeline,
};
use super::document_timeline::document_of_realm;
use crate::bindings::conversions::CppInterface;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_animation_document_scrolling_element(document: *mut c_void) -> *mut c_void;
}

// https://drafts.csswg.org/scroll-animations-1/#enumdef-scrollaxis
// NB: The numeric values are those of the host's ScrollAxis.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum ScrollAxis {
    Block,
    Inline,
    X,
    Y,
}

impl ScrollAxis {
    fn from_host(axis: u8) -> Self {
        match axis {
            0 => Self::Block,
            1 => Self::Inline,
            2 => Self::X,
            3 => Self::Y,
            _ => unreachable!("a scroll axis is one of four"),
        }
    }
}

/// The dictionary ScrollTimelineOptions of Animations/ScrollTimeline.idl.
pub(crate) struct ScrollTimelineOptions {
    pub(crate) source: Option<Option<CppInterface>>,
    pub(crate) axis: ScrollAxis,
}

// https://drafts.csswg.org/scroll-animations-1/#scrolltimeline
pub(crate) struct ScrollTimeline {
    timeline: AnimationTimeline,
}

// SAFETY: The timeline visits what it reaches.
unsafe impl Trace for ScrollTimeline {
    fn trace(&self, visitor: &mut Visitor) {
        self.timeline.trace(visitor);
    }
}

impl Inherits<AnimationTimeline> for ScrollTimeline {
    fn parent(&self) -> &AnimationTimeline {
        &self.timeline
    }
}

impl Wrappable for ScrollTimeline {
    const FINALIZES: bool = true;

    fn finalize(object: Ref<Self>) {
        finalize_timeline(&object.timeline);
    }
}

impl ScrollTimeline {
    pub(crate) fn from_timeline(timeline: AnimationTimeline) -> Self {
        Self { timeline }
    }

    pub(crate) fn timeline(&self) -> &AnimationTimeline {
        &self.timeline
    }

    // https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-scrolltimeline
    pub(crate) fn construct(cx: &mut CallContext, options: ScrollTimelineOptions) -> WebIdlResult<Ref<Self>> {
        let document = document_of_realm(cx.realm());

        // 1. Let timeline be the new ScrollTimeline object.
        // 2. Set the source of timeline to:
        let source = match options.source {
            // If the source member of options is present,
            //     The source member of options.
            Some(source) => source.map(element_of_wrapper),
            // Otherwise,
            //     The scrollingElement of the Document associated with the Window that is the current global object.
            // SAFETY: The document is live.
            None => NonNull::new(unsafe { web_animation_document_scrolling_element(document.as_ptr()) }),
        };

        // 3. Set the axis property of timeline to the corresponding value from options.
        let record = TimelineRecordRef::new(
            document,
            TimelineDescription::Scroll {
                source,
                axis: options.axis as u8,
            },
        );
        Ok(Self::create(cx.vm(), record))
    }

    fn create(vm: &Vm, record: TimelineRecordRef) -> Ref<Self> {
        let object = allocate(vm, |_| Self::from_timeline(AnimationTimeline::new(record)));
        did_create_timeline(cell_into_abi(object));
        object
    }

    // https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-source
    pub(crate) fn source(&self, cx: &mut CallContext) -> WebIdlResult<Option<CppInterface>> {
        // NB: The host finds the scroll container element an anonymous source names from its target, and the source of a
        //     view timeline from its subject.
        let source = self.timeline.record().source_element();
        Ok(source.map(|source| element_wrapper(cx, source)))
    }

    // https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-axis
    pub(crate) fn axis(&self, _: &mut CallContext) -> WebIdlResult<ScrollAxis> {
        Ok(ScrollAxis::from_host(self.timeline.record().axis()))
    }
}
