/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! InternalAnimationTimeline, the timeline whose time tests set.

use core::ffi::c_void;

use libcompositing_rust::animation_timing::OptionalNumber;
use libjs_rust::api::{Trace, Visitor};

use crate::animations::animation_timeline::{AnimationTimeline, finalize_timeline};
use crate::bindings::interface::{Inherits, Ref, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_internal_animation_timeline_set_time(timeline: *mut c_void, time: OptionalNumber, for_observation: bool);
}

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

    pub(crate) fn set_time(&self, _: &mut CallContext, time: Option<f64>) -> WebIdlResult<()> {
        // SAFETY: The cell is that of this live timeline.
        unsafe { web_internal_animation_timeline_set_time(self.timeline.cell(), time.into(), false) };
        Ok(())
    }

    pub(crate) fn set_time_for_observation(&self, _: &mut CallContext, time: f64) -> WebIdlResult<()> {
        // SAFETY: The cell is that of this live timeline.
        unsafe { web_internal_animation_timeline_set_time(self.timeline.cell(), Some(time).into(), true) };
        Ok(())
    }
}
