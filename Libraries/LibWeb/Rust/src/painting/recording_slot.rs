/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a document keeps of its display list recordings from one to the next, and the recording
//! that runs on it.
//!
//! A [`RecordingJob`] owns the frame it records, the inputs it records with and the recorder state
//! it records with, and returns what it recorded with that state in a [`RecordingAnswer`]. None of
//! them names the layout arena, and the job runs on the Paint thread: the host either waits for it,
//! or lets it fly beside the event loop and takes its answer in once it has finished. The slot is
//! the document host's, on the host's thread: only the recording entries, the publication and the
//! trace reach it, through [`crate::render_state::DocumentHost::recording`].

use crate::layout::RowsVersion;
use crate::layout::node_data::NodeSlotId;
use crate::painting::ffi::{FfiFlightBlocker, FfiRecordingLanding};
use crate::painting::hit_test::HitTestList;
use crate::painting::paint_read::PaintSource;
use crate::painting::paint_state::{PendingRecording, PendingRecordingTrace};
use crate::painting::published_frame::PublishedFrame;
use crate::painting::record::RecordingInputs;
use crate::painting::record::recorder_state::RecorderState;
use crate::render_state::{LockstepProof, TaskBoundary};
use crate::stage_thread::InFlight;
use std::sync::{Condvar, Mutex, MutexGuard, PoisonError};

/// A display list recording: the frame it records, which it drops before it returns, and the
/// recorder state it records with, which it returns in its answer.
pub(crate) struct RecordingJob {
    frame: PublishedFrame,
    recorder: RecorderState,
    viewport: NodeSlotId,
    trace_recordings: bool,
}

/// What a recording answers: the recorder state it was handed, and what it recorded.
pub(crate) struct RecordingAnswer {
    recorder: RecorderState,
    pending: PendingRecording,
    trace: Option<PendingRecordingTrace>,
}

// The host waits for a recording only where it needs the recorder back.
impl crate::stage_thread::Flown for RecordingAnswer {
    type JoinRight = LockstepProof;
}

// A job reads nothing the document goes on writing, and an answer holds nothing of the document.
const _: () = {
    const fn assert_send<T: Send + 'static>() {}
    assert_send::<RecordingJob>();
    assert_send::<RecordingAnswer>();
};

impl RecordingJob {
    pub(crate) fn new(
        frame: PublishedFrame,
        recorder: RecorderState,
        viewport: NodeSlotId,
        trace_recordings: bool,
    ) -> Self {
        Self {
            frame,
            recorder,
            viewport,
            trace_recordings,
        }
    }

    /// Records the frame with `inputs` on the Paint thread, and waits for what it recorded.
    pub(crate) fn run_on_paint_thread(self, inputs: &RecordingInputs) -> RecordingAnswer {
        if cfg!(test) {
            return self.run(inputs);
        }
        release_held_recording_for_testing();
        crate::stage_thread::paint_thread().run(|| self.run(inputs))
    }

    /// Records the frame with `inputs` on the Paint thread beside the host, which `_license` shows nothing needs
    /// before the event loop's next task.
    pub(crate) fn fly(self, inputs: RecordingInputs, _license: FlightLicense) -> InFlight<RecordingAnswer> {
        let held = take_recording_hold_for_testing();
        crate::stage_thread::paint_thread().submit(move || {
            if held {
                wait_while_recording_is_held_for_testing();
            }
            self.run(&inputs)
        })
    }

    /// Records the frame with `inputs`. It takes no main thread token, so nothing it calls can reach
    /// the host.
    fn run(self, inputs: &RecordingInputs) -> RecordingAnswer {
        let Self {
            frame,
            mut recorder,
            viewport,
            trace_recordings,
        } = self;
        let RecorderState {
            published_recording,
            published_hit_test_items,
            paint_order_tree,
            scratch,
            absolute_rects,
        } = &mut recorder;
        let source = PaintSource::new(&frame, absolute_rects);
        // The retained tree describes the published tape and is written in place while a frame
        // is assembled, so only a recording that publishes may copy from that frame or touch
        // the tree; any other recording records from scratch into a tree of its own.
        let mut throwaway_tree = crate::painting::record::order_tree::PaintOrderTree::default();
        let (tree, source_recording, source_items) = if inputs.publishes_recording {
            (
                paint_order_tree,
                published_recording.clone(),
                published_hit_test_items.clone(),
            )
        } else {
            (&mut throwaway_tree, None, None)
        };
        let copies_from_published_recording = source_recording.is_some();
        let recording = crate::painting::record::traversal::record_display_list(
            &source,
            scratch,
            tree,
            viewport,
            inputs,
            source_recording,
            source_items,
            true,
            trace_recordings || crate::painting::record::verify::enabled_by_environment(),
        );
        // The oracle records the same frame from scratch into a throwaway tree whenever the
        // published recording could have been copied from.
        let recording_from_scratch =
            (crate::painting::record::verify::enabled_by_environment() && copies_from_published_recording).then(|| {
                let mut inputs_for_recording_from_scratch = inputs.clone();
                inputs_for_recording_from_scratch.publishes_recording = false;
                let mut tree_for_recording_from_scratch =
                    crate::painting::record::order_tree::PaintOrderTree::default();
                crate::painting::record::traversal::record_display_list(
                    &source,
                    scratch,
                    &mut tree_for_recording_from_scratch,
                    viewport,
                    &inputs_for_recording_from_scratch,
                    None,
                    None,
                    false,
                    false,
                )
            });
        let trace = (trace_recordings && recording.output.capture_log_for_verification.is_some()).then_some(
            PendingRecordingTrace {
                viewport,
                should_paint_overlay: inputs.should_paint_overlay,
            },
        );
        let svg_paint_resources = frame.svg_paint_resources().clone();
        drop(frame);
        RecordingAnswer {
            recorder,
            pending: PendingRecording {
                recording,
                recording_from_scratch,
                publishes_recording: inputs.publishes_recording,
                svg_paint_resources,
            },
            trace,
        }
    }
}

/// The test hold on recordings: a test arms it for the next recording that flies, which then reads nothing of its frame
/// until the test releases it, so that the test writes the document beside a recording that has read nothing yet.
#[derive(Clone, Copy, PartialEq, Eq)]
enum RecordingHold {
    Idle,
    Armed,
    Holding,
}

static RECORDING_HOLD: Mutex<RecordingHold> = Mutex::new(RecordingHold::Idle);
static RECORDING_HOLD_RELEASED: Condvar = Condvar::new();

fn recording_hold() -> MutexGuard<'static, RecordingHold> {
    RECORDING_HOLD.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Whether the recording about to fly is the one the test hold is armed for.
fn take_recording_hold_for_testing() -> bool {
    let mut hold = recording_hold();
    if *hold != RecordingHold::Armed {
        return false;
    }
    *hold = RecordingHold::Holding;
    true
}

fn wait_while_recording_is_held_for_testing() {
    let _released = RECORDING_HOLD_RELEASED
        .wait_while(recording_hold(), |hold| *hold == RecordingHold::Holding)
        .unwrap_or_else(PoisonError::into_inner);
}

/// Holds the next recording that flies before it reads its frame, until
/// [`release_held_recording_for_testing`] or a wait for it.
pub(crate) fn hold_next_recording_for_testing() {
    *recording_hold() = RecordingHold::Armed;
}

/// Lets the recording held for the test go, or disarms the hold no recording has taken yet.
pub(crate) fn release_held_recording_for_testing() {
    *recording_hold() = RecordingHold::Idle;
    RECORDING_HOLD_RELEASED.notify_all();
}

/// The right of a rendering update's recording to fly beside the event loop: the document had no
/// flight blocker. Only [`FlightLicense::for_blocker`] mints one.
pub(crate) struct FlightLicense {
    _private: (),
}

impl FlightLicense {
    /// The license of a recording whose document has `blocker`, if it has none.
    pub(crate) fn for_blocker(blocker: FfiFlightBlocker) -> Option<Self> {
        (blocker == FfiFlightBlocker::None).then_some(Self { _private: () })
    }
}

/// The reason the host waits for its document's recording in flight: a recording it starts, or the
/// recording it reads, needs the recorder state the one in flight took.
pub(crate) struct RecordingNeedsItsRecorder {
    _private: (),
}

const RECORDING_NEEDS_ITS_RECORDER: RecordingNeedsItsRecorder = RecordingNeedsItsRecorder { _private: () };

/// Where the recorder state a document's recordings record with is.
#[expect(
    clippy::large_enum_variant,
    reason = "a document has one slot, and its recorder state moves to and from its recordings without an allocation"
)]
enum Recorder {
    /// With the document, for its next recording.
    Here(RecorderState),
    /// With the recording in flight, whose answer brings it back.
    InFlight(RecordingFlight),
}

impl Default for Recorder {
    fn default() -> Self {
        Self::Here(RecorderState::default())
    }
}

/// A recording in flight, and the rows version of the frame it records, which tells whether the
/// recording still stands for the document once it lands.
struct RecordingFlight {
    flight: InFlight<RecordingAnswer>,
    rows_version: RowsVersion,
}

/// A recording the host publishes, with what its publication writes of the document's recordings.
pub(crate) struct Publication<'a> {
    pub(crate) pending: PendingRecording,
    pub(crate) recorder: &'a mut RecorderState,
    pub(crate) hit_test_list: &'a mut Option<HitTestList>,
}

/// What a document keeps of its recordings: the recording its host is to publish, the trace that
/// recording left, the recorder state the next recording records with, and the hit-test list of the
/// last recording published, which hit testing reads.
#[derive(Default)]
pub(crate) struct RecordingSlot {
    pending_recording: Option<PendingRecording>,
    pending_recording_trace: Option<PendingRecordingTrace>,
    recorder: Recorder,
    hit_test_list: Option<HitTestList>,
}

impl RecordingSlot {
    pub(crate) fn has_pending_recording(&self) -> bool {
        self.pending_recording.is_some()
    }

    /// The pending recording, for the host to publish, with the recorder state it recorded with and
    /// the hit-test list it replaces. A recording is pending only once it has landed.
    pub(crate) fn take_publication(&mut self) -> Option<Publication<'_>> {
        let Recorder::Here(recorder) = &mut self.recorder else {
            return None;
        };
        Some(Publication {
            pending: self.pending_recording.take()?,
            recorder,
            hit_test_list: &mut self.hit_test_list,
        })
    }

    /// The trace the pending recording left, for the host to read.
    pub(crate) fn take_pending_recording_trace(&mut self) -> Option<PendingRecordingTrace> {
        self.pending_recording_trace.take()
    }

    /// Whether a recording of the document is in flight.
    pub(crate) fn has_recording_in_flight(&self) -> bool {
        matches!(self.recorder, Recorder::InFlight(_))
    }

    /// The hit-test list of the last recording published, if any was.
    pub(crate) fn hit_test_list(&mut self) -> &mut Option<HitTestList> {
        &mut self.hit_test_list
    }

    /// How many items the next recording's hit-test list may hold, going by the last one's.
    pub(crate) fn hit_test_item_capacity_hint(&self) -> usize {
        self.hit_test_list.as_ref().map_or(0, |list| list.items.len())
    }

    /// The recorder state, for a recording to take until it answers. The host takes a recording in
    /// flight in before it records again: one it did not is waited for and dropped unpublished.
    pub(crate) fn take_recorder(&mut self) -> RecorderState {
        match std::mem::take(&mut self.recorder) {
            Recorder::Here(recorder) => recorder,
            Recorder::InFlight(in_flight) => {
                let RecordingAnswer {
                    mut recorder, pending, ..
                } = in_flight
                    .flight
                    .join(LockstepProof::for_reason(&RECORDING_NEEDS_ITS_RECORDER));
                if pending.publishes_recording {
                    recorder.forget_published_recording();
                }
                recorder
            }
        }
    }

    /// Gives back the recorder state a recording took, where it recorded nothing.
    pub(crate) fn give_back_recorder(&mut self, recorder: RecorderState) {
        self.recorder = Recorder::Here(recorder);
    }

    /// Takes in what a recording answered, leaving the recording pending for the host to publish.
    pub(crate) fn accept_recording_answer(&mut self, answer: RecordingAnswer) {
        self.recorder = Recorder::Here(answer.recorder);
        self.pending_recording_trace = answer.trace;
        self.pending_recording = Some(answer.pending);
    }

    /// Lets the recording `flight` fly with the recorder state, recording a frame of the rows at
    /// `rows_version`.
    pub(crate) fn fly(&mut self, flight: InFlight<RecordingAnswer>, rows_version: RowsVersion) {
        debug_assert!(
            !self.has_recording_in_flight() && !self.has_pending_recording(),
            "a document records one frame at a time"
        );
        self.recorder = Recorder::InFlight(RecordingFlight { flight, rows_version });
    }

    /// Takes the recording in flight in at `boundary`, where it has finished, and answers how it
    /// landed: it stands where `rows_version` answers the version of the frame it recorded.
    pub(crate) fn take_finished_recording_in(
        &mut self,
        boundary: &TaskBoundary,
        stands: impl FnOnce(RowsVersion) -> bool,
    ) -> FfiRecordingLanding {
        let Recorder::InFlight(in_flight) = std::mem::take(&mut self.recorder) else {
            return FfiRecordingLanding::NoneInFlight;
        };
        let RecordingFlight { flight, rows_version } = in_flight;
        match flight.try_take(boundary) {
            Ok(answer) => self.land(answer, stands(rows_version)),
            Err(flight) => {
                self.recorder = Recorder::InFlight(RecordingFlight { flight, rows_version });
                FfiRecordingLanding::StillInFlight
            }
        }
    }

    /// Waits for the recording in flight and takes it in, and answers how it landed (see
    /// [`Self::take_finished_recording_in`]).
    pub(crate) fn join_recording_in_flight(&mut self, stands: impl FnOnce(RowsVersion) -> bool) -> FfiRecordingLanding {
        let Recorder::InFlight(in_flight) = std::mem::take(&mut self.recorder) else {
            return FfiRecordingLanding::NoneInFlight;
        };
        release_held_recording_for_testing();
        let answer = in_flight
            .flight
            .join(LockstepProof::for_reason(&RECORDING_NEEDS_ITS_RECORDER));
        self.land(answer, stands(in_flight.rows_version))
    }

    fn land(&mut self, answer: RecordingAnswer, stands: bool) -> FfiRecordingLanding {
        self.accept_recording_answer(answer);
        if !stands {
            self.discard_pending_recording();
            return FfiRecordingLanding::DidNotStand;
        }
        FfiRecordingLanding::Stands
    }

    /// Drops the pending recording unpublished. A recording that publishes wrote the retained
    /// paint-order tree, so the next one copies nothing from the published recording.
    pub(crate) fn discard_pending_recording(&mut self) {
        self.pending_recording_trace = None;
        if let Some(pending) = self.pending_recording.take()
            && pending.publishes_recording
            && let Recorder::Here(recorder) = &mut self.recorder
        {
            recorder.forget_published_recording();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::LayoutNodeArena;
    use crate::painting::record::{RecordingOutput, RecordingResult};
    use std::sync::Arc;

    /// The slot's recorder state, or none while a recording in flight has it.
    fn recorder_of(slot: &mut RecordingSlot) -> Option<&mut RecorderState> {
        match &mut slot.recorder {
            Recorder::Here(recorder) => Some(recorder),
            Recorder::InFlight(_) => None,
        }
    }

    fn pending(publishes_recording: bool) -> PendingRecording {
        PendingRecording {
            recording: RecordingResult {
                output: RecordingOutput::default(),
                resources: Default::default(),
            },
            recording_from_scratch: None,
            publishes_recording,
            svg_paint_resources: Default::default(),
        }
    }

    /// Leaves `pending` in the slot as a recording's answer would.
    fn accept_pending(slot: &mut RecordingSlot, pending: PendingRecording) {
        let recorder = slot.take_recorder();
        slot.accept_recording_answer(RecordingAnswer {
            recorder,
            pending,
            trace: None,
        });
    }

    #[test]
    fn the_slot_lends_its_recorder_state_to_a_job_and_takes_it_back_with_the_answer() {
        let mut engine = crate::css::style::StyleEngine::new(crate::css::style::memory::DeviceClass::ForegroundDesktop);
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(crate::css::style::StyleEngineHandle::from_raw(&raw mut engine));
        let mut slot = RecordingSlot::default();
        let published = Arc::new(RecordingOutput::default());
        recorder_of(&mut slot).unwrap().published_recording = Some(published.clone());

        let job = RecordingJob::new(arena.freeze_frame(0), slot.take_recorder(), NodeSlotId::INVALID, false);
        assert!(recorder_of(&mut slot).unwrap().published_recording.is_none());
        let RecordingJob { recorder, .. } = job;
        slot.accept_recording_answer(RecordingAnswer {
            recorder,
            pending: pending(true),
            trace: None,
        });
        assert!(Arc::ptr_eq(
            recorder_of(&mut slot).unwrap().published_recording.as_ref().unwrap(),
            &published
        ));
        assert!(slot.has_pending_recording());
    }

    #[test]
    fn a_discarded_publishing_recording_forgets_its_source() {
        let mut slot = RecordingSlot::default();
        let published = Arc::new(RecordingOutput::default());
        recorder_of(&mut slot).unwrap().published_recording = Some(published.clone());

        accept_pending(&mut slot, pending(false));
        slot.discard_pending_recording();
        assert!(!slot.has_pending_recording());
        assert!(recorder_of(&mut slot).unwrap().published_recording.is_some());

        accept_pending(&mut slot, pending(true));
        slot.discard_pending_recording();
        assert!(recorder_of(&mut slot).unwrap().published_recording.is_none());
    }

    /// A flight of the slot's recorder state that answers it back with a recording pending.
    fn flight_of(slot: &mut RecordingSlot, publishes_recording: bool) -> InFlight<RecordingAnswer> {
        let recorder = slot.take_recorder();
        crate::stage_thread::paint_thread().submit(move || RecordingAnswer {
            recorder,
            pending: pending(publishes_recording),
            trace: None,
        })
    }

    #[test]
    fn a_recording_in_flight_has_the_recorder_state_until_it_lands() {
        let mut slot = RecordingSlot::default();
        let published = Arc::new(RecordingOutput::default());
        recorder_of(&mut slot).unwrap().published_recording = Some(published.clone());
        let flight = flight_of(&mut slot, true);
        slot.fly(flight, RowsVersion::default());
        assert!(slot.has_recording_in_flight());
        assert!(recorder_of(&mut slot).is_none());
        assert!(
            slot.take_publication().is_none(),
            "nothing is pending before the recording lands"
        );

        assert_eq!(slot.join_recording_in_flight(|_| true), FfiRecordingLanding::Stands);
        assert!(!slot.has_recording_in_flight());
        let publication = slot.take_publication().unwrap();
        assert!(Arc::ptr_eq(
            publication.recorder.published_recording.as_ref().unwrap(),
            &published
        ));
    }

    #[test]
    fn a_recording_that_lands_after_a_write_to_the_rows_is_dropped_unpublished() {
        let mut slot = RecordingSlot::default();
        recorder_of(&mut slot).unwrap().published_recording = Some(Arc::new(RecordingOutput::default()));
        let flight = flight_of(&mut slot, true);
        slot.fly(flight, RowsVersion::default());
        let landing = loop {
            match slot.take_finished_recording_in(&TaskBoundary::for_test(), |_| false) {
                FfiRecordingLanding::StillInFlight => std::thread::yield_now(),
                landing => break landing,
            }
        };
        assert_eq!(landing, FfiRecordingLanding::DidNotStand);
        assert!(!slot.has_pending_recording());
        assert!(
            recorder_of(&mut slot).unwrap().published_recording.is_none(),
            "the next recording copies nothing from what the dropped one wrote over"
        );
    }

    #[test]
    fn a_recording_started_beside_one_in_flight_takes_the_recorder_state_back_first() {
        let mut slot = RecordingSlot::default();
        let flight = flight_of(&mut slot, false);
        slot.fly(flight, RowsVersion::default());
        let _recorder = slot.take_recorder();
        assert!(!slot.has_recording_in_flight());
        assert!(!slot.has_pending_recording());
    }
}
