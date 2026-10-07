/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where the lanes of each document come together, on the render owner, which keeps them beside the document's render
//! state.
//!
//! The owner samples a frame of a document, and the frame's lane takes the place of the lane before it once its pieces
//! have come: the recorder state and the seal its ticks present with, from the Paint thread once it presented the
//! frame, and a plan, from the end of the frame's rendering update, or of a later one that presents no frame of its
//! own. The fork of the render state the lane writes is taken while no job of the host wrote the state since the frame
//! was sampled, which the state then still shows: as the first tick that needs it runs, or before the host first writes
//! the state, where the pointer moves over the document or a task runs the plan's animations. Until a lane comes
//! together, the one before it follows the pointer, whose frames the presenter drops once it presented the newer
//! frame.

use super::super::owner::{self, DocumentId};
use super::{
    ClockPlan, ClockRecorder, ClockTicks, FfiClockLaneState, Lane, LanePublication, LaneReport, LaneState,
    PendingPointer, TickPresented,
};
use crate::painting::recording_slot::RecordingAnswer;
use std::sync::Arc;
use std::sync::atomic::Ordering;

/// What the render owner keeps of a document's lanes, beside its render state.
pub(crate) struct LaneSlot {
    ticks: Arc<ClockTicks>,
    /// How many frames of the document the owner sampled: the last one is the frame the next lane starts from.
    sampled: u64,
    lanes: Lanes,
    /// How many frames the lanes' ticks presented, and how many pointer moves their hover took that the host has to
    /// hover, and where the pointer was at the last one.
    presented_frames: u64,
    hover_moves: u64,
    hovered_pointer: Option<Option<libgfx_rust::FloatPoint>>,
}

impl Drop for LaneSlot {
    /// A retired document has no lanes: the render clock hears no lane follows the pointer or ticks.
    fn drop(&mut self) {
        *self.ticks.published() = LanePublication::default();
    }
}

/// The lanes of a document.
#[derive(Default)]
#[expect(
    clippy::large_enum_variant,
    reason = "a document has one set of lanes, which a lane moves between without an allocation"
)]
enum Lanes {
    #[default]
    None,
    /// The lane of the frame sampled last.
    Newest(Lane),
    /// The pieces of the lane of the frame sampled last, until they come together with a plan, and the lane of an
    /// earlier frame, which follows the presented frame meanwhile, until the frame sampled last presents.
    Coming { pieces: LanePieces, earlier: Option<Lane> },
}

impl Lanes {
    /// The lane that follows the presented frame.
    fn current(&mut self) -> Option<&mut Lane> {
        match self {
            Self::None => None,
            Self::Newest(lane) => Some(lane),
            Self::Coming { earlier, .. } => earlier.as_mut(),
        }
    }

    fn current_ref(&self) -> Option<&Lane> {
        match self {
            Self::None => None,
            Self::Newest(lane) => Some(lane),
            Self::Coming { earlier, .. } => earlier.as_ref(),
        }
    }

    /// What the lane of the frame sampled last writes, whether or not its pieces came together yet.
    fn newest_state(&mut self) -> Option<&mut LaneState> {
        match self {
            Self::None => None,
            Self::Newest(lane) => Some(&mut lane.state),
            Self::Coming { pieces, .. } => Some(&mut pieces.state),
        }
    }
}

/// The pieces of a lane, as they come.
#[derive(Default)]
struct LanePieces {
    state: LaneState,
    recorder: Option<ClockRecorder>,
    plan: FramePlan,
}

/// The plan of a lane's frame. A rendering update may leave the frame no plan, and a later one that presents no frame of
/// its own seal one for it.
#[derive(Default)]
#[expect(
    clippy::large_enum_variant,
    reason = "a document has the pieces of one lane at a time, which a plan moves into without an allocation"
)]
enum FramePlan {
    #[default]
    Unsealed,
    None,
    Sealed(ClockPlan),
}

/// What hands the lane of a frame the pieces the Paint thread makes, once it presented the frame.
pub(crate) struct LaneDelivery {
    ticks: Arc<ClockTicks>,
    frame: u64,
}

impl LaneDelivery {
    /// Hands the lane the recorder state and the seal its ticks present with, from what the recording of its frame
    /// answered. On the Paint thread, once the frame is presented.
    pub(crate) fn deliver(&self, answer: &RecordingAnswer) {
        let Some((recorder, presentation, output)) = answer.clock_lane_recorder() else {
            return;
        };
        let presented = match output {
            // NB: The fork took no recording in since the frame was sampled, so the hit-test list moved on.
            Some(output) => TickPresented::Frame {
                output,
                hit_test_list_changed: true,
            },
            None => TickPresented::Nothing,
        };
        let recorder = ClockRecorder {
            recorder,
            presentation,
            presented,
        };
        let (ticks, frame) = (Arc::clone(&self.ticks), self.frame);
        super::super::post_to_render_side(move || {
            with_slot(ticks.document, |slot| slot.deliver_recorder(frame, recorder));
        });
    }
}

/// Runs `job` on the slot of `document`, where the owner sampled a frame of it, and publishes what the lanes do then.
fn with_slot<R>(document: DocumentId, job: impl FnOnce(&mut LaneSlot) -> R) -> Option<R> {
    owner::with_lanes(document, |slot| {
        let slot = slot.as_mut()?;
        let answer = job(slot);
        slot.publish();
        Some(answer)
    })
    .flatten()
}

/// On the render owner: notes that it sampled a frame of the document `ticks` belong to, whose lane starts once the
/// frame is presented, and answers what hands the lane its pieces then.
pub(in crate::render_state) fn note_sampled(ticks: &Arc<ClockTicks>) -> LaneDelivery {
    owner::with_lanes(ticks.document, |slot| {
        let slot = slot.get_or_insert_with(|| LaneSlot {
            ticks: Arc::clone(ticks),
            sampled: 0,
            lanes: Lanes::None,
            presented_frames: 0,
            hover_moves: 0,
            hovered_pointer: None,
        });
        slot.sampled += 1;
        let earlier = match std::mem::take(&mut slot.lanes) {
            // The state shows the new frame, which the lane before it can no longer fork.
            Lanes::Newest(mut lane) => {
                lane.state.move_on();
                Some(lane)
            }
            Lanes::Coming { earlier, .. } => earlier,
            Lanes::None => None,
        };
        slot.lanes = Lanes::Coming {
            pieces: LanePieces::default(),
            earlier,
        };
        slot.publish();
        LaneDelivery {
            ticks: Arc::clone(ticks),
            frame: slot.sampled,
        }
    })
    .expect("the owner samples a frame of a render state it holds")
}

/// On the render owner: notes that a job of the host is about to write the render state of `document`, which then no
/// longer is the one the frame sampled last shows. A lane that may want it forks the state first.
pub(in crate::render_state) fn note_host_write(document: DocumentId) {
    with_slot(document, |slot| {
        let wants_fork = slot.ticks.wants_fork();
        if let Some(state) = slot.lanes.newest_state() {
            if wants_fork {
                state.fork(document);
            }
            state.move_on();
        }
    });
}

/// On the render owner: hands the lane of the frame of `document` sampled last `plan`, which the rendering update that
/// sampled it sealed, or none.
pub(in crate::render_state) fn seal_plan(document: DocumentId, plan: Option<ClockPlan>) {
    with_slot(document, |slot| slot.deliver_plan(plan));
}

/// On the render owner: runs the tick queued for the lanes `ticks` belong to.
pub(super) fn tick(ticks: &Arc<ClockTicks>) {
    ticks.queued.swap(false, Ordering::AcqRel);
    with_slot(ticks.document, LaneSlot::tick);
}

impl LaneSlot {
    fn deliver_recorder(&mut self, frame: u64, recorder: ClockRecorder) {
        // The owner sampled a later frame since, whose lane takes this one's place.
        if frame != self.sampled {
            return;
        }
        if let Lanes::Coming { pieces, earlier } = &mut self.lanes {
            pieces.recorder = Some(recorder);
            // The frame presented, and the presenter takes no frame of the lane of an earlier one after it.
            *earlier = None;
            self.come_together();
        }
    }

    fn deliver_plan(&mut self, plan: Option<ClockPlan>) {
        match (&mut self.lanes, plan) {
            (Lanes::Coming { pieces, .. }, plan) => {
                pieces.plan = plan.map_or(FramePlan::None, FramePlan::Sealed);
                self.come_together();
            }
            // A rendering update that presented no frame of its own leaves its plan to the lane of the frame before it.
            (Lanes::Newest(lane), Some(plan)) => lane.replan(plan),
            (_, _) => self.lanes = Lanes::None,
        }
    }

    /// Has the lane of the frame sampled last take the place of the lane before it, once its pieces have come with a
    /// plan. Where the lanes hovered moves the host has not taken in yet, the lane hovers where the pointer went last at
    /// once: the frame shows the hover of the moves the host handled, and the presenter dropped the frames the lane
    /// before it presented once the frame was presented.
    fn come_together(&mut self) {
        let lane = match std::mem::take(&mut self.lanes) {
            Lanes::Coming {
                pieces:
                    LanePieces {
                        state,
                        recorder: Some(recorder),
                        plan: FramePlan::Sealed(plan),
                    },
                ..
            } => Lane::new(recorder, plan, state),
            // A frame with no plan yet has no lane: the moves are the host's.
            Lanes::Coming {
                pieces:
                    pieces @ LanePieces {
                        recorder: Some(_),
                        plan: FramePlan::None,
                        ..
                    },
                ..
            } => {
                self.lanes = Lanes::Coming { pieces, earlier: None };
                return;
            }
            lanes => {
                self.lanes = lanes;
                return;
            }
        };
        let follows_pointer = lane.plan.follows_pointer();
        self.lanes = Lanes::Newest(lane);
        self.take_in_scroll_offsets();
        let rehover = self.ticks.pointer_state().last().filter(|_| follows_pointer);
        let hovered_beyond_the_host = self.hover_moves > self.ticks.hover_moves_taken_in.load(Ordering::Relaxed);
        if let Some(pointer) = rehover
            && hovered_beyond_the_host
            && !self.ticks.is_held()
        {
            if let Some(state) = self.lanes.newest_state() {
                state.fork(self.ticks.document);
            }
            self.run_tick(Some(pointer), false);
        }
    }

    /// Hands the current lane where the compositor scrolled to at the latest tick that said so. Where no lane came
    /// together yet, the offsets wait for the one that does.
    fn take_in_scroll_offsets(&mut self) {
        let Some(lane) = self.lanes.current() else {
            return;
        };
        if let Some(offsets) = self
            .ticks
            .scroll_offsets
            .lock()
            .expect("clock tick scroll offsets")
            .take()
        {
            lane.scroll_offsets = offsets;
        }
    }

    fn tick(&mut self) {
        self.take_in_scroll_offsets();
        // A recording of the host's own adds to the resource storage the lane presents with: a move waits for it.
        if self.ticks.is_held() {
            return;
        }
        let samples_animations = !self.ticks.animations_held.load(Ordering::Relaxed);
        let pointer = self.ticks.pointer_state().take();
        // The state still shows the frame sampled last, whose lane forks it now, whether or not it came together yet.
        if let Some(state) = self.lanes.newest_state() {
            state.fork(self.ticks.document);
        }
        let newest = matches!(self.lanes, Lanes::Newest(_));
        if let Some(lane) = self.lanes.current()
            && matches!(lane.state, LaneState::MovedOn)
        {
            // The host wrote the state before the lane of the frame sampled last forked it: the host shows what the
            // pointer and the animations move from now on. A lane of an earlier frame waits for the next one.
            if newest {
                lane.parked = true;
                lane.hovered.park();
            }
            return;
        }
        self.run_tick(pointer, samples_animations);
    }

    /// Ticks the lane, which has a fork, with `pointer`, and sampling its animations where `samples_animations`.
    fn run_tick(&mut self, pointer: Option<PendingPointer>, samples_animations: bool) {
        let Some(lane) = self.lanes.current() else {
            return;
        };
        let animations_stand = lane.plan.animation_changes == self.ticks.animation_changes();
        if !animations_stand {
            lane.parked = true;
        }
        let Some(ticked) = lane.tick_on_fork(
            self.ticks.latest.load(Ordering::Acquire),
            pointer,
            samples_animations && animations_stand,
        ) else {
            return;
        };
        if let Some(position) = ticked.hovered {
            self.hover_moves += 1;
            self.hovered_pointer = Some(position);
        }
        if ticked.presented {
            self.presented_frames += 1;
            *self.ticks.presented_boxes.lock().expect("presented boxes") = super::PresentedBoxes {
                border_boxes: lane.presented_border_boxes.clone(),
                colors: lane.presented_colors.clone(),
            };
        }
    }

    /// Publishes what the lanes do for the host and the render clock.
    fn publish(&self) {
        let lane = self.lanes.current_ref();
        *self.ticks.published() = LanePublication {
            state: lane.map_or(FfiClockLaneState::None, Lane::state),
            planned_animation_changes: lane.map_or(0, |lane| lane.plan.animation_changes),
            transitions_run: lane.is_some_and(Lane::transitions_run),
            follows_pointer: lane.is_some_and(|lane| lane.plan.follows_pointer() && !lane.hovered.is_parked()),
            awaits_lane: matches!(self.lanes, Lanes::Coming { .. }),
            report: LaneReport {
                hovered_pointer: self.hovered_pointer,
                transition_starts: lane.map_or_else(Vec::new, |lane| {
                    lane.started_transitions()
                        .map(|(transitions, start_time)| super::LaneTransitionStart {
                            node: transitions.node,
                            properties: transitions.properties().iter().copied().collect(),
                            start_time,
                        })
                        .collect()
                }),
                presented_frames: self.presented_frames,
                hover_moves: self.hover_moves,
            },
        };
    }
}
