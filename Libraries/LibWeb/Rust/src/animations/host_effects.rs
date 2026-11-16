/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the procedures of animations ask the host to do: settle promises, queue tasks and events, and invalidate
//! style. A procedure only records these, in the order its steps take them, and the host performs them once the
//! procedure has returned, holding no borrow of any record.

use super::animation::AnimationHandle;
use super::effect::EffectHandle;
use super::timing::TimeValue;
use smallvec::SmallVec;
use std::ffi::c_void;
use std::ptr::NonNull;

/// The type of an `AnimationPlaybackEvent` a procedure queues.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlaybackEventType {
    Finish,
    Cancel,
    Remove,
}

/// One thing a procedure asks the host to do for an animation.
#[derive(Clone, Debug, PartialEq)]
pub(crate) enum HostEffectKind {
    /// https://www.w3.org/TR/web-animations-1/#current-ready-promise
    /// Let the current ready promise be a new promise in the relevant Realm of the animation.
    ReplaceReadyPromise,
    /// Resolve the current ready promise with the animation.
    ResolveReadyPromise,
    /// Reject the current ready promise with a DOMException named "AbortError", set its [[PromiseIsHandled]] internal
    /// slot to true, and let the current ready promise be a new resolved promise with the animation.
    RejectReadyPromise,
    /// https://www.w3.org/TR/web-animations-1/#current-finished-promise
    /// Let the current finished promise be a new promise in the relevant Realm of the animation.
    ReplaceFinishedPromise,
    /// Resolve the current finished promise with the animation.
    ResolveFinishedPromise,
    /// Reject the current finished promise with a DOMException named "AbortError", set its [[PromiseIsHandled]]
    /// internal slot to true, and let the current finished promise be a new promise.
    RejectFinishedPromise,
    /// Queue a task to run the finish notification steps of the animation, unless there is one queued already.
    QueueFinishNotificationTask,
    /// Cancel the queued task to run the finish notification steps of the animation, if any.
    CancelFinishNotificationTask,
    /// Create an `AnimationPlaybackEvent` and append it to the pending animation event queue of the animation's
    /// document for timing, or queue a task to dispatch it at the animation without one. Its scheduled event time is
    /// `scheduled_timeline_time` converted to an origin-relative time, where the timeline can convert it.
    QueuePlaybackEvent {
        r#type: PlaybackEventType,
        current_time: Option<TimeValue>,
        timeline_time: Option<TimeValue>,
        scheduled_timeline_time: Option<TimeValue>,
    },
    /// Invalidate the style the associated effect produces. `changes_plan` is whether this changes the animation as
    /// the last rendering update planned it for the render clock, rather than move it on in time, as a rendering
    /// update does.
    InvalidateEffect { changes_plan: bool },
    /// Unless the style the associated effect produces can skip its update for this frame, which the host notes
    /// then, invalidate it as `InvalidateEffect` does without changing the plan, where `invalidate`.
    InvalidateUnlessThrottled { invalidate: bool },
    /// Associate the animation with `element`, the host's `DOM::Element` the animation's associated effect targets.
    AssociateWithTarget { element: NonNull<c_void> },
    /// Disassociate the animation from `element`, the host's `DOM::Element` that `effect`, the animation's associated
    /// effect, targeted.
    DisassociateFromTarget {
        element: NonNull<c_void>,
        effect: EffectHandle,
    },
    /// Recompute the style of the target of `effect`, which no longer applies as it did.
    RecomputeEffectStyle { effect: EffectHandle },
    /// Ask for a repaint of the target of the associated effect.
    RequestRepaint,
    /// Ask the document of the target of the associated effect for a rendering update in `delay` milliseconds, as
    /// the effect begins.
    ScheduleWakeup { delay: f64 },
}

/// What one procedure asks the host to do, in order. Most procedures ask for little or nothing, which needs no
/// allocation.
#[derive(Default)]
pub(crate) struct HostEffects(SmallVec<[(AnimationHandle, HostEffectKind); 4]>);

impl HostEffects {
    pub(crate) fn push(&mut self, animation: &AnimationHandle, kind: HostEffectKind) {
        self.0.push((animation.clone(), kind));
    }

    /// Appends what another procedure asks the host to do after this.
    pub(crate) fn extend(&mut self, other: HostEffects) {
        self.0.extend(other.0);
    }

    /// What the procedures asked for, in order.
    pub(crate) fn iter(&self) -> impl Iterator<Item = &(AnimationHandle, HostEffectKind)> {
        self.0.iter()
    }

    #[cfg(test)]
    pub(crate) fn kinds(&self) -> Vec<HostEffectKind> {
        self.0.iter().map(|(_, kind)| kind.clone()).collect()
    }
}

impl IntoIterator for HostEffects {
    type Item = (AnimationHandle, HostEffectKind);
    type IntoIter = smallvec::IntoIter<[(AnimationHandle, HostEffectKind); 4]>;

    fn into_iter(self) -> Self::IntoIter {
        self.0.into_iter()
    }
}
