/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The pending animation event queue of a document, which holds what each event is, not the event: the host creates
//! each event as it dispatches it.
//! https://www.w3.org/TR/web-animations-1/#pending-animation-event-queue

use super::animation::{AnimationCell, AnimationHandle, HostCellKind};
use super::composite_order::{TreeOrder, composite_order};
use super::css_events::FfiCssEventType;
use super::host_effects::PlaybackEventType;
use super::timing::{OptionalNumber, OptionalTimeValue};
use std::cmp::Ordering;
use std::ffi::c_void;
use std::ptr::NonNull;

/// The type of a `TimeEvent` an SVG animation element dispatches.
/// https://svgwg.org/specs/animations/#InterfaceTimeEvent
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TimeEventType {
    Begin,
    End,
    Repeat,
}

/// Which interface an event of the pending animation event queue is of.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PendingAnimationEventKind {
    /// An `AnimationEvent` or a `TransitionEvent` of a CSS animation or transition, dispatched at its owning element.
    Css,
    /// An `AnimationPlaybackEvent`, dispatched at the animation.
    Playback,
    /// A `TimeEvent` of the animation of an SVG animation element, dispatched at the animation element.
    Time,
}

/// An event of the pending animation event queue, as the host creates it as it dispatches it, the fields of its kind
/// set.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FfiPendingAnimationEvent {
    pub kind: PendingAnimationEventKind,
    /// For `Css`.
    pub css_event_type: FfiCssEventType,
    /// For `Playback`.
    pub playback_event_type: PlaybackEventType,
    /// For `Time`.
    pub time_event_type: TimeEventType,
    /// For `Time`, the iteration a repeat event is of.
    pub detail: i32,
    /// For `Css`, in seconds.
    pub elapsed_time: f64,
    /// For `Playback`.
    pub current_time: OptionalTimeValue,
    pub timeline_time: OptionalTimeValue,
    /// https://www.w3.org/TR/web-animations-1/#scheduled-event-time
    pub scheduled_event_time: OptionalNumber,
    /// The time the event was queued at, which is its time stamp.
    pub time_stamp: f64,
}

/// An event of the queue and the animation it is of, which the queue holds.
pub(crate) struct PendingAnimationEvent {
    pub(crate) animation: AnimationHandle,
    pub(crate) event: FfiPendingAnimationEvent,
}

/// The pending animation event queue of a document, and the events queued while style and layout feedback is
/// provisional, which join it only as the outermost style stabilization epoch commits.
#[derive(Default)]
pub(crate) struct PendingAnimationEvents {
    pending: Vec<PendingAnimationEvent>,
    provisional: Vec<PendingAnimationEvent>,
}

impl PendingAnimationEvents {
    /// Appends an event to the pending animation event queue, or, where `provisional`, to the provisional events.
    pub(crate) fn append(&mut self, event: PendingAnimationEvent, provisional: bool) {
        if provisional {
            self.provisional.push(event);
        } else {
            self.pending.push(event);
        }
    }

    pub(crate) fn has_provisional_events(&self) -> bool {
        !self.provisional.is_empty()
    }

    /// Appends the provisional events to the pending animation event queue but those `drops` says to drop, answering how
    /// many it appended.
    pub(crate) fn commit_provisional_events(&mut self, mut drops: impl FnMut(&AnimationCell) -> bool) -> usize {
        let before = self.pending.len();
        for event in std::mem::take(&mut self.provisional) {
            if !drops(&event.animation) {
                self.pending.push(event);
            }
        }
        self.pending.len() - before
    }

    /// Takes the events of the pending animation event queue in the order to dispatch them in.
    /// https://www.w3.org/TR/web-animations-1/#update-animations-and-send-events
    pub(crate) fn take_events_to_dispatch(&mut self, tree_order: &mut impl TreeOrder) -> Vec<PendingAnimationEvent> {
        // 4. Let events to dispatch be a copy of doc’s pending animation event queue.
        // 5. Clear doc’s pending animation event queue.
        let mut events_to_dispatch = std::mem::take(&mut self.pending);

        // 6. Perform a stable sort of the animation events in events to dispatch as follows:
        //    1. Sort the events by their scheduled event time such that events that were scheduled to occur earlier
        //       sort before events scheduled to occur later, and events whose scheduled event time is unresolved sort
        //       before events with a resolved scheduled event time.
        //    2. Within events with equal scheduled event times, sort by their composite order.
        // NB: An event of an animation without an associated effect sorts before those of animations with one.
        events_to_dispatch.sort_by(|a, b| {
            let (a_time, b_time) = (a.event.scheduled_event_time, b.event.scheduled_event_time);
            match (a_time.has_value, b_time.has_value) {
                (false, true) => return Ordering::Less,
                (true, false) => return Ordering::Greater,
                (true, true) => {
                    // NB: A total order keeps the sort well defined should a time be NaN, and adding zero makes
                    //     -0 and +0 equal times, as they compare.
                    let ordering = (a_time.value + 0.0).total_cmp(&(b_time.value + 0.0));
                    if ordering != Ordering::Equal {
                        return ordering;
                    }
                }
                (false, false) => {}
            }
            let (a, b) = (a.animation.borrow(), b.animation.borrow());
            match (a.effect().is_some(), b.effect().is_some()) {
                (true, true) => composite_order(&a, &b, tree_order),
                (a_has_effect, b_has_effect) => a_has_effect.cmp(&b_has_effect),
            }
        });
        events_to_dispatch
    }

    /// Calls `visit` with each GC cell of the host that keeps alive the elements the records of the queued events
    /// name.
    pub(crate) fn for_each_host_cell(&self, mut visit: impl FnMut(NonNull<c_void>, HostCellKind)) {
        for event in self.pending.iter().chain(&self.provisional) {
            event.animation.for_each_host_cell(&mut visit);
        }
    }
}
