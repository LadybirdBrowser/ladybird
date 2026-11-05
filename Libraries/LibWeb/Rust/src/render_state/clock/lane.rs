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
//! frame. A rendering update that presents no frame of its own leaves the lane of the frame before it to go on from the
//! state as the update left it: a fork the update's writes passed goes, and the state is forked again.

use super::super::owner::{self, DocumentId};
use super::{
    ClockPlan, ClockRecorder, ClockTicks, FfiClockLaneState, Lane, LaneMove, LanePublication, LaneReport, LaneState,
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
    /// Whether a tick presented a frame, and the last pointer move a tick took, since a rendering update last took the
    /// lanes in.
    presented: bool,
    last_move: Option<LaneMove>,
    /// The compositor context of the same-process iframe the lanes' hover handed the pointer to last, until it left it:
    /// the hover of the next lane hands it that the pointer left as well.
    nested: Option<u64>,
    /// The serial number of the last rendering update that took in what the lanes did, until it, or an update after it,
    /// seals its plan, which follows the frame it presents: a lane presents nothing meanwhile, as the update's frame
    /// shows what it took in, which no frame of the lane goes back from. The plan of an update before it, which seals
    /// as the later update runs, leaves it standing. The take-in and the seal both run on the owner, in the order the
    /// host made them.
    taken_in: Option<u64>,
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

    /// Whether the lane of the frame sampled last follows the pointer, or may once its pieces come together with the plan
    /// they wait for.
    fn newest_follows_pointer(&self) -> bool {
        match self {
            Self::None => false,
            Self::Newest(lane) => lane.needs_render_state() && lane.plan.follows_pointer() && !lane.hovered.is_parked(),
            Self::Coming { pieces, .. } => match &pieces.plan {
                FramePlan::Unsealed => true,
                FramePlan::None => false,
                FramePlan::Sealed(plan) => plan.needs_render_state() && plan.follows_pointer(),
            },
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

/// Lets go of `lanes`, whose forks may take long to drop: they are dropped once the owner no longer holds the render
/// states, which the host may be waiting for.
fn retire(lanes: Lanes) {
    owner::drop_after_hold(lanes);
}

/// On the render owner: drops the slot of a retired document, whose lanes it lets go of.
pub(in crate::render_state) fn retire_slot(slot: Option<LaneSlot>) {
    if let Some(mut slot) = slot {
        retire(std::mem::take(&mut slot.lanes));
    }
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
            // No lane starts from the frame, and the presenter takes no frame of the lane of an earlier one after it:
            // the moves and the animations are the host's.
            let (ticks, frame) = (Arc::clone(&self.ticks), self.frame);
            super::super::post_to_render_side(move || {
                with_slot(ticks.document, |slot| slot.forget_coming_lane(frame));
            });
            return;
        };
        let presented = match output {
            // NB: The fork took no recording in since the frame was sampled, so the hit-test list moved on.
            Some(output) => TickPresented::Frame {
                output,
                hit_test_list_changed: true,
                publishes: true,
            },
            None => TickPresented::Nothing,
        };
        let recorder = ClockRecorder {
            recorder,
            presentation,
            presented,
            presented_hit_test_items: None,
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

/// The nodes the selection in the fork of the lane that follows the presented frame of `document` highlights. For a test.
pub(in crate::render_state) fn selection_shown_for_testing(
    document: DocumentId,
) -> Option<Vec<super::selection::SelectionRowForTesting>> {
    with_slot(document, |slot| {
        match slot.lanes.current_ref().map(|lane| &lane.state) {
            Some(super::LaneState::Forked { fork, .. }) => Some(super::selection::rows_for_testing(fork)),
            _ => None,
        }
    })
    .flatten()
}

/// On the render owner: notes that it sampled a frame of the document `ticks` belong to, whose lane starts once the
/// frame is presented, and answers what hands the lane its pieces then.
pub(in crate::render_state) fn note_sampled(ticks: &Arc<ClockTicks>) -> LaneDelivery {
    owner::with_lanes(ticks.document, |slot| {
        let slot = LaneSlot::of(slot, ticks);
        slot.sampled += 1;
        let earlier = match std::mem::take(&mut slot.lanes) {
            // The state shows the new frame, which the lane before it can no longer fork.
            Lanes::Newest(mut lane) => {
                lane.state.move_on();
                Some(lane)
            }
            Lanes::Coming { pieces, earlier } => {
                retire(Lanes::Coming { pieces, earlier: None });
                earlier
            }
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
        // NB: The update has taken this lane in and holds it until its plan is sealed. If the lane sampled nothing,
        //     the update either replaces its frame or leaves the host showing the same frame. Do not copy the old
        //     state just to discard it when the update samples its replacement. A newly sampled frame's coming lane
        //     still needs its state preserved: a task may write it before its recorder and plan arrive.
        if slot.taken_in.is_some()
            && let Lanes::Newest(lane) = &mut slot.lanes
            && lane.samples_nothing()
        {
            lane.state.move_on();
            return;
        }
        // NB: Nor does a coming lane tick while the update holds it, and with no animations to sample, what it would show
        //     is what the update replaces or leaves showing, as above.
        if slot.taken_in.is_some()
            && let Lanes::Coming { pieces, .. } = &slot.lanes
            && matches!(&pieces.plan, FramePlan::Sealed(plan) if !plan.animates())
            && let Some(state) = slot.lanes.newest_state()
        {
            state.move_on();
            return;
        }
        // A lane that follows the pointer hovers beside the next task, which may begin as soon as this job is done,
        // whether or not its animations run beside the idle event loop.
        let needs_state = match &slot.lanes {
            Lanes::Newest(lane) => lane.needs_render_state(),
            Lanes::Coming { pieces, .. } => match &pieces.plan {
                FramePlan::Sealed(plan) => plan.needs_render_state(),
                _ => true,
            },
            Lanes::None => false,
        };
        let wants_fork = needs_state && (slot.ticks.wants_fork() || slot.lanes.newest_follows_pointer());
        if let Some(state) = slot.lanes.newest_state() {
            if wants_fork {
                state.fork(document);
            }
            state.move_on();
        }
    });
}

/// On the render owner: hands the lane of the frame of `document` sampled last `plan`, which the rendering update with
/// the serial number `update` that sampled it sealed, or none.
pub(in crate::render_state) fn seal_plan(document: DocumentId, plan: Option<ClockPlan>, update: u64) {
    with_slot(document, |slot| slot.deliver_plan(plan, update));
}

/// On the render owner: has the lanes of the document `ticks` belong to present nothing until the rendering update with
/// the serial number `update`, which takes them in now, seals its plan, and answers what their hover did so far. A
/// document whose owner sampled no frame yet gets its slot now, whose first lane waits for the update's plan too.
#[must_use]
pub(in crate::render_state) fn take_lanes_in(ticks: &Arc<ClockTicks>, update: u64) -> LaneReport {
    owner::with_lanes(ticks.document, |slot| {
        let slot = LaneSlot::of(slot, ticks);
        slot.taken_in = Some(update);
        let report = slot.take_report();
        slot.publish();
        report
    })
    .unwrap_or_default()
}

/// On the render owner: answers what the hover of the lanes of the document `ticks` belong to did so far, and leaves
/// them ticking, for the host to take in between two rendering updates.
#[must_use]
pub(in crate::render_state) fn take_lane_report(ticks: &Arc<ClockTicks>) -> LaneReport {
    with_slot(ticks.document, LaneSlot::take_report).unwrap_or_default()
}

/// On the render owner: runs the tick queued for the lanes `ticks` belong to.
pub(super) fn tick(ticks: &Arc<ClockTicks>) {
    ticks.queued.swap(false, Ordering::AcqRel);
    with_slot(ticks.document, LaneSlot::tick);
}

impl LaneSlot {
    /// The slot `slot` holds for the lanes of the document `ticks` belong to, which it makes where it holds none yet.
    fn of<'a>(slot: &'a mut Option<LaneSlot>, ticks: &Arc<ClockTicks>) -> &'a mut LaneSlot {
        slot.get_or_insert_with(|| LaneSlot {
            ticks: Arc::clone(ticks),
            sampled: 0,
            lanes: Lanes::None,
            presented: false,
            last_move: None,
            nested: None,
            taken_in: None,
        })
    }

    fn deliver_recorder(&mut self, frame: u64, recorder: ClockRecorder) {
        // The owner sampled a later frame since, whose lane takes this one's place.
        if frame != self.sampled {
            return;
        }
        if let Lanes::Coming { pieces, earlier } = &mut self.lanes {
            pieces.recorder = Some(recorder);
            // The frame presented, and the presenter takes no frame of the lane of an earlier one after it.
            if let Some(earlier) = earlier.take() {
                retire(Lanes::Newest(earlier));
            }
            self.come_together();
        }
    }

    /// Has the frame `frame`, from which no lane starts, end the lanes: a move waits for nothing more.
    fn forget_coming_lane(&mut self, frame: u64) {
        if frame == self.sampled && matches!(self.lanes, Lanes::Coming { .. }) {
            self.retire_lanes();
        }
    }

    fn deliver_plan(&mut self, plan: Option<ClockPlan>, update: u64) {
        self.taken_in = self.taken_in.filter(|taken_in| *taken_in > update);
        if super::hover::logs_hover() {
            eprintln!(
                "{} hover lane: plan sealed, lanes {}",
                super::hover::log_time(),
                match &self.lanes {
                    Lanes::None => "none",
                    Lanes::Newest(_) => "newest",
                    Lanes::Coming { earlier: Some(_), .. } => "coming beside an earlier",
                    Lanes::Coming { earlier: None, .. } => "coming",
                }
            );
        }
        match (&mut self.lanes, plan) {
            (Lanes::Coming { pieces, .. }, plan) => {
                // NB: The frame's own update sealed already, so this one presented no frame of its own.
                if self.taken_in.is_none() && !matches!(pieces.plan, FramePlan::Unsealed) {
                    pieces.state.rejoin_the_state();
                }
                pieces.plan = plan.map_or(FramePlan::None, FramePlan::Sealed);
                self.come_together();
            }
            // A rendering update that presented no frame of its own leaves its plan to the lane of the frame before it.
            (Lanes::Newest(lane), Some(plan)) => {
                // NB: With no replacement frame, the completed update left the host showing this lane's frame.
                //     Its next tick or the next task's first write can preserve that state then.
                if self.taken_in.is_none() {
                    lane.rejoin_the_state();
                }
                lane.replan(plan);
            }
            // One that seals no plan leaves a lane that showed no sample following nothing: a later update that presents
            // no frame of its own has it follow its plan again, which a lane that went away could not.
            (Lanes::Newest(lane), None) if lane.samples_nothing() => lane.unplan(),
            (_, _) => self.retire_lanes(),
        }
    }

    /// Lets go of the lanes.
    fn retire_lanes(&mut self) {
        retire(std::mem::take(&mut self.lanes));
    }

    /// Has the lane of the frame sampled last take the place of the lane before it, once its pieces have come with a
    /// plan. The lanes presented nothing since the rendering update that sampled the frame took them in, so the frame
    /// shows all they did, and a move that waits is the new lane's first tick's to hover.
    fn come_together(&mut self) {
        let lane = match std::mem::take(&mut self.lanes) {
            Lanes::Coming {
                pieces:
                    LanePieces {
                        state,
                        recorder: Some(recorder),
                        plan: FramePlan::Sealed(plan),
                    },
                earlier,
            } => {
                if let Some(earlier) = earlier {
                    retire(Lanes::Newest(earlier));
                }
                Lane::new(recorder, plan, state)
            }
            // A frame with no plan yet has no lane: the moves are the host's.
            Lanes::Coming {
                pieces:
                    pieces @ LanePieces {
                        recorder: Some(_),
                        plan: FramePlan::None,
                        ..
                    },
                earlier,
            } => {
                if let Some(earlier) = earlier {
                    retire(Lanes::Newest(earlier));
                }
                self.lanes = Lanes::Coming { pieces, earlier: None };
                return;
            }
            lanes => {
                self.lanes = lanes;
                return;
            }
        };
        self.lanes = Lanes::Newest(lane);
        if super::hover::logs_hover() {
            eprintln!("{} hover lane: lane came together", super::hover::log_time());
        }
        self.take_in_scroll_offsets();
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

    /// Shows the cursor of what is under the move that waits for a tick, if one waits, in the frame the screen shows,
    /// as a tick that presents nothing: the host hovers the move once it takes the lanes in.
    fn show_cursor_where_a_move_waits(&mut self) {
        let Some(pointer) = self.ticks.pointer_state().take() else {
            return;
        };
        self.show_cursor_for(pointer);
    }

    fn show_cursor_for(&mut self, pointer: PendingPointer) {
        let frame_time_nanoseconds = self.ticks.latest.load(Ordering::Acquire);
        let document = self.ticks.document;
        let nested = self.nested;
        let (hovered, nested) = match self.lanes.current() {
            // A lane that needs no render state hovers its read-only frame as it always does, which presents nothing.
            Some(lane) if !lane.needs_render_state() => {
                lane.hovered.go_on_from_nested(nested);
                let hovered = lane.hover_read_only(pointer, frame_time_nanoseconds);
                (Some(hovered), lane.hovered.nested())
            }
            // A lane without a fork, which the host's write let go of as the update took the lane in, hit tests the
            // state the host writes: what the frame the update presents next shows, which the cursor follows ahead of it.
            Some(lane) => {
                let hovered = lane
                    .show_cursor_on_fork(frame_time_nanoseconds, pointer)
                    .unwrap_or_else(|| {
                        owner::with_state(document, None, |state| {
                            lane.show_cursor_only(state, pointer, frame_time_nanoseconds)
                        })
                    });
                (Some(hovered), nested)
            }
            None => (None, nested),
        };
        self.nested = nested;
        self.last_move = Some(hovered.unwrap_or(LaneMove {
            input_event_id: pointer.input_event_id,
            target: None,
            handed: false,
        }));
    }

    fn tick(&mut self) {
        self.take_in_scroll_offsets();
        // A recording of the host's own adds to the resource storage the lane presents with: a move waits for it.
        if self.ticks.is_held() {
            self.show_cursor_where_a_move_waits();
            return;
        }
        // A rendering update took in what the lanes did, and the frame it presents shows that, at the time it took it
        // in: the lane of the frame before it presents nothing on from there, which the update's frame would go back
        // from, and the moves and animation times since wait for the lane of the update's frame.
        if self.taken_in.is_some() || matches!(self.lanes, Lanes::Coming { .. }) {
            if super::hover::logs_hover() && self.ticks.pointer_waits() {
                eprintln!(
                    "{} hover lane: a move waits for the update's frame",
                    super::hover::log_time()
                );
            }
            self.show_cursor_where_a_move_waits();
            return;
        }
        let samples_animations = !self.ticks.animations_held.load(Ordering::Relaxed);
        // NB: Beside an idle event loop the host gets to the move of a drag at once, and paints it itself. The move waits
        //     until then, as a task that begins first keeps the host from it.
        let host_paints_the_drag = !samples_animations
            && self
                .lanes
                .current_ref()
                .is_some_and(|lane| lane.selection_drag().is_some())
            && self.ticks.pointer_state().holds_the_primary_button();
        let pointer = match host_paints_the_drag {
            true => None,
            false => self.ticks.pointer_state().take(),
        };
        if let Lanes::Newest(lane) = &mut self.lanes
            && !lane.needs_render_state()
        {
            if let Some(pointer) = pointer {
                lane.hovered.go_on_from_nested(self.nested);
                self.last_move = Some(lane.hover_read_only(pointer, self.ticks.latest.load(Ordering::Acquire)));
                self.nested = lane.hovered.nested();
            }
            return;
        }
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
            if let Some(pointer) = pointer {
                self.show_cursor_for(pointer);
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
        lane.hovered.go_on_from_nested(self.nested);
        let ticked = lane.tick_on_fork(
            self.ticks.latest.load(Ordering::Acquire),
            pointer,
            samples_animations && animations_stand,
        );
        self.nested = lane.hovered.nested();
        let Some(ticked) = ticked else {
            return;
        };
        if ticked.hover_move.is_some() {
            self.last_move = ticked.hover_move;
        }
        if ticked.presented {
            self.presented = true;
            *self.ticks.presented_boxes.lock().expect("presented boxes") = super::PresentedBoxes {
                border_boxes: lane.presented_border_boxes.clone(),
                colors: lane.presented_colors.clone(),
                compositor_animations: lane.presented_compositor_animations.clone(),
            };
            // The samples the tick showed may have moved a box under the pointer at rest, or away from under it: the
            // next tick hovers where the pointer rests again, in the frame this tick presented.
            if ticked.sampled {
                let _ = self.ticks.pointer_rests_on_moving_content();
            }
        }
    }

    /// Takes what the lanes did since a rendering update last took them in.
    fn take_report(&mut self) -> LaneReport {
        LaneReport {
            last_move: self.last_move.take(),
            transitions: self
                .lanes
                .current()
                .map_or_else(Vec::new, Lane::take_started_transitions),
            presented: std::mem::take(&mut self.presented),
        }
    }

    fn publish(&self) {
        let lane = self.lanes.current_ref();
        *self.ticks.published() = LanePublication {
            state: lane.map_or(FfiClockLaneState::None, Lane::state),
            planned_animation_changes: lane.map_or(0, |lane| lane.plan.animation_changes),
            transitions_run: lane.is_some_and(Lane::transitions_run),
            follows_pointer: lane.is_some_and(|lane| lane.plan.follows_pointer() && !lane.hovered.is_parked()),
            awaits_lane: matches!(self.lanes, Lanes::Coming { .. }),
        };
    }
}
