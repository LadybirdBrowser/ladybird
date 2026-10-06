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
//! or commits a rendering update, whose frame the render owner samples and hands the Paint thread
//! beside the event loop, and takes its answer in once it has finished. The recording of a
//! committed frame owns its navigable's presenter, and presents the frame it recorded itself. The slot is
//! the document host's, on the host's thread: only the recording entries, the publication and the
//! trace reach it, through [`crate::render_state::DocumentHost::recording`].

use crate::layout::node_data::NodeSlotId;
use crate::layout::{LayoutNodeArena, RowsVersion};
use crate::paint_stage::Presenting;
use crate::painting::ffi::{FfiFlightBlocker, FfiPresentedRecording};
use crate::painting::hit_test::HitTestList;
use crate::painting::paint_read::PaintSource;
use crate::painting::paint_state::{PendingRecording, PendingRecordingTrace};
use crate::painting::presentation::Presentation;
use crate::painting::published_frame::PublishedFrame;
use crate::painting::record::recorder_state::RecorderState;
use crate::painting::record::{RecordingInputs, RecordingOutput};
use crate::render_state::{LockstepProof, TaskBoundary};
use crate::stage_thread::{InFlight, ParkedJob, StopWord};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};

/// A display list recording: the frame it records, which it drops before it returns, the rows
/// version that frame was frozen at, the recorder state it records with, which it returns in its
/// answer, and for the recording of a committed frame, what presents the frame it recorded.
pub(crate) struct RecordingJob {
    frame: PublishedFrame,
    rows_version: RowsVersion,
    recorder: RecorderState,
    viewport: NodeSlotId,
    trace_recordings: bool,
    presentation: Option<Presentation>,
}

/// What a recording answers: the recorder state it was handed, what it made of what it recorded at
/// which rows version, and the presentation it was handed, if any.
pub(crate) struct RecordingAnswer {
    recorder: RecorderState,
    recorded: Recorded,
    rows_version: RowsVersion,
    trace: Option<PendingRecordingTrace>,
    presentation: Option<Presentation>,
}

/// What a recording made of what it recorded: nothing, where the document's viewport had no box to
/// record, a recording for the host to publish and present, one it published and presented itself, or
/// nothing, for a committed frame that keeps the display list the compositor has, which it presented.
#[expect(
    clippy::large_enum_variant,
    reason = "a recording answers once a frame, and moves what it recorded to the host without an allocation"
)]
enum Recorded {
    Nothing,
    Pending(PendingRecording),
    Presented {
        output: RecordingOutput,
        publishes_recording: bool,
    },
    PresentedUnrecorded,
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

impl RecordingAnswer {
    /// The answer of a committed frame the render owner found nothing to record of, which gives back
    /// the recorder state and presentation the commit took.
    pub(crate) fn nothing_recorded(recorder: RecorderState, presentation: Option<Presentation>) -> Self {
        Self {
            recorder,
            recorded: Recorded::Nothing,
            rows_version: RowsVersion::default(),
            trace: None,
            presentation,
        }
    }

    /// The answer of a committed frame that records nothing, which it presented, and gives back the recorder state and
    /// presentation the commit took.
    pub(crate) fn presented_unrecorded(recorder: RecorderState, presentation: Presentation) -> Self {
        Self {
            recorder,
            recorded: Recorded::PresentedUnrecorded,
            rows_version: RowsVersion::default(),
            trace: None,
            presentation: Some(presentation),
        }
    }
}

impl RecordingJob {
    /// The recording of `frozen`, the frame of `viewport` frozen for it, with `recorder`, which presents what it
    /// recorded with `presentation`, if any.
    pub(crate) fn new(
        frozen: FrozenFrame,
        recorder: RecorderState,
        viewport: NodeSlotId,
        presentation: Option<Presentation>,
    ) -> Self {
        Self {
            frame: frozen.frame,
            rows_version: frozen.rows_version,
            recorder,
            viewport,
            trace_recordings: frozen.trace_recordings,
            presentation,
        }
    }

    /// Records the frame with `inputs` on the Paint thread, and waits for what it recorded.
    pub(crate) fn run_on_paint_thread(self, inputs: RecordingInputs) -> RecordingAnswer {
        if cfg!(test) {
            return self.run(inputs, None);
        }
        release_held_recording_for_testing();
        crate::paint_stage::paint_thread().run(|| self.run(inputs, None))
    }

    /// Records the frame with `inputs` on the Paint thread beside the host, and presents it with the job's
    /// presentation, if any.
    pub(crate) fn run_beside_host(self, inputs: RecordingInputs, presenting: &mut Presenting) -> RecordingAnswer {
        self.run(inputs, Some(presenting))
    }

    /// Records the frame with `inputs`, and presents it where the job has a presentation and runs beside the host
    /// with `presenting`. It takes no main thread token, so nothing it calls can reach the host.
    fn run(self, inputs: RecordingInputs, presenting: Option<&mut Presenting>) -> RecordingAnswer {
        let Self {
            frame,
            rows_version,
            mut recorder,
            viewport,
            trace_recordings,
            mut presentation,
        } = self;
        let (pending, trace) = record_frame(frame, &mut recorder, viewport, trace_recordings, inputs);
        // A recording that renders an SVG image leaves its frame for the host to present.
        let recorded = match (presentation.as_mut(), presenting) {
            (Some(presentation), Some(presenting))
                if !crate::painting::record::publish::renders_vector_images(&pending) =>
            {
                let publishes_recording = pending.publishes_recording;
                Recorded::Presented {
                    output: present(presentation, pending, &recorder, presenting),
                    publishes_recording,
                }
            }
            _ => Recorded::Pending(pending),
        };
        RecordingAnswer {
            recorder,
            recorded,
            rows_version,
            trace,
            presentation,
        }
    }
}

/// The test hold on recordings: a test arms it for the next recording that flies, which then reads nothing of its frame
/// until the test releases it, so that the test writes the document beside a recording that has read nothing yet.
enum RecordingHold {
    Idle,
    Armed,
    Holding,
    // A layout held is never handed to the StyleLayout thread until it goes, so that the thread runs the jobs of other
    // documents meanwhile, as one the collector finalizes asks of it.
    HoldingLayout { _parked: ParkedJob },
}

static RECORDING_HOLD: Mutex<RecordingHold> = Mutex::new(RecordingHold::Idle);
static RECORDING_HOLD_RELEASED: Condvar = Condvar::new();

fn recording_hold() -> MutexGuard<'static, RecordingHold> {
    RECORDING_HOLD.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Whether the recording about to fly is the one the test hold is armed for.
pub(crate) fn take_recording_hold_for_testing() -> bool {
    let mut hold = recording_hold();
    if !matches!(*hold, RecordingHold::Armed) {
        return false;
    }
    *hold = RecordingHold::Holding;
    true
}

/// Submits `job`, the layout of a frame, to the StyleLayout thread, or parks it where the test hold is armed for it.
/// Answers the flight and whether the hold took it.
pub(crate) fn submit_layout<R: Send + 'static>(
    job: impl FnOnce(&StopWord) -> R + Send + 'static,
) -> (InFlight<R>, bool) {
    let thread = crate::stage_thread::style_layout_thread();
    let mut hold = recording_hold();
    if !matches!(*hold, RecordingHold::Armed) {
        return (thread.submit(job), false);
    }
    let (flight, parked) = thread.park(job);
    *hold = RecordingHold::HoldingLayout { _parked: parked };
    (flight, true)
}

pub(crate) fn wait_while_recording_is_held_for_testing() {
    let _released = RECORDING_HOLD_RELEASED
        .wait_while(recording_hold(), |hold| matches!(hold, RecordingHold::Holding))
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

/// Records `frame` of the document's `viewport` with `inputs` and `recorder`, which keeps the inputs of a recording that
/// publishes, and answers what it recorded, with the trace it left where `trace_recordings` asks for one.
pub(crate) fn record_frame(
    frame: PublishedFrame,
    recorder: &mut RecorderState,
    viewport: NodeSlotId,
    trace_recordings: bool,
    inputs: RecordingInputs,
) -> (PendingRecording, Option<PendingRecordingTrace>) {
    let RecorderState {
        published_recording,
        published_hit_test_items,
        paint_order_tree,
        scratch,
        absolute_rects,
        published_inputs,
    } = recorder;
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
    let recording = crate::painting::record::traversal::record_display_list(
        &source,
        scratch,
        tree,
        viewport,
        &inputs,
        source_recording,
        source_items,
        trace_recordings,
    );
    let trace = trace_recordings.then_some(PendingRecordingTrace {
        viewport,
        should_paint_overlay: inputs.should_paint_overlay,
    });
    let svg_paint_resources = frame.svg_paint_resources().clone();
    drop(frame);
    let publishes_recording = inputs.publishes_recording;
    if publishes_recording {
        *published_inputs = Some(inputs);
    }
    (
        PendingRecording {
            recording,
            publishes_recording,
            svg_paint_resources,
        },
        trace,
    )
}

/// Presents `pending`, which renders no SVG image and was recorded with `recorder`, with `presentation`, beside the event
/// loop, and answers its output.
pub(crate) fn present(
    presentation: &mut Presentation,
    pending: PendingRecording,
    recorder: &RecorderState,
    presenting: &mut Presenting,
) -> RecordingOutput {
    let output = crate::painting::record::publish::publish_to_presenter(pending, recorder, &mut presentation.presenter);
    presentation.present(&FfiPresentedRecording::of_output(&output), presenting);
    output
}

/// What the host knows that freezing a document's frame for a recording reads.
pub(crate) struct FrameInputs {
    pub(crate) viewport: NodeSlotId,
    pub(crate) css_viewport_rect: crate::css::css_pixels::CssPixelRect,
    pub(crate) publishes_recording: bool,
    /// The canvas rect the root background painted in the recording published last, if any.
    pub(crate) published_root_background_canvas_rect: Option<crate::css::css_pixels::CssPixelRect>,
    pub(crate) hit_test_item_capacity_hint: usize,
}

/// A document's frame, frozen for a recording, with what the recording reads beside it, and the rows version it was
/// frozen at.
pub(crate) struct FrozenFrame {
    pub(crate) frame: crate::painting::published_frame::PublishedFrame,
    pub(crate) tree_inputs: crate::painting::host::FfiVisualContextTreeInputs,
    pub(crate) root_background_source: crate::painting::host::RootBackgroundSource,
    pub(crate) trace_recordings: bool,
    pub(crate) rows_version: crate::layout::RowsVersion,
}

/// Freezes the frame of the document whose arena `arena` is for a recording of its viewport, or none where the
/// viewport has no box to paint.
pub(crate) fn freeze_recording_frame(arena: &mut LayoutNodeArena, inputs: FrameInputs) -> Option<FrozenFrame> {
    // Recording reads overflow, and reading overflow never measures it.
    arena.measure_scrollable_overflow();
    if !arena.paintable_row_is_populated(inputs.viewport) || arena.stacking_context_entries(inputs.viewport).is_none() {
        return None;
    }
    // The root background paints the union of the viewport and the root's overflow, so it is the
    // one output a viewport move can change. Drop its caches before the frame is published instead
    // of treating the viewport position as a frame-wide input.
    if let Some(published_canvas_rect) = inputs.published_root_background_canvas_rect {
        let root = arena
            .paint_state()
            .borrow()
            .root_background_source
            .expect("a recording follows paint preparation")
            .root_layout_node;
        let canvas_rect = crate::painting::record::paint::background_resolution::root_background_canvas_rect(
            &arena.paintable_rows(),
            root,
            inputs.css_viewport_rect,
        );
        if canvas_rect != published_canvas_rect {
            arena.push_paint_damage(root, crate::painting::record::damage::PaintDamage::DRAW_BACKGROUND);
        }
    }
    if inputs.publishes_recording {
        arena.note_publishing_paint_recording_started();
    }
    // The recording reads the document as it is now: what the host writes after this goes to the next frame.
    let frame = arena.freeze_frame(inputs.hit_test_item_capacity_hint);
    let rows_version = arena.rows_version();
    let paint_state = arena.paint_state().borrow();
    Some(FrozenFrame {
        frame,
        rows_version,
        tree_inputs: paint_state
            .visual_context
            .last_tree_inputs
            .expect("a recording follows a visual context update"),
        root_background_source: paint_state
            .root_background_source
            .expect("a recording follows paint preparation"),
        trace_recordings: paint_state.trace_recordings,
    })
}

/// The right of a rendering update's frame to fly beside the event loop: the document had no
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

pub(crate) const RECORDING_NEEDS_ITS_RECORDER: RecordingNeedsItsRecorder = RecordingNeedsItsRecorder { _private: () };

/// Where the recorder state a document's recordings record with is.
#[expect(
    clippy::large_enum_variant,
    reason = "a document has one slot, and its recorder state moves to and from its recordings without an allocation"
)]
enum Recorder {
    /// With the document, for its next recording.
    Here(RecorderState),
    /// With the recording in flight, whose answer brings it back.
    InFlight(InFlight<RecordingAnswer>),
    /// With the document's clock lease, whose landing brings it back.
    WithClock,
}

impl Default for Recorder {
    fn default() -> Self {
        Self::Here(RecorderState::default())
    }
}

/// How the recording in flight of a document landed.
pub(crate) enum RecordingLanding {
    NoneInFlight,
    StillInFlight,
    /// The recording landed: what it recorded is pending for the host to publish and present, or the
    /// recording presented it itself. It gives back the presentation it was handed, if any.
    Landed(Option<Presentation>),
    /// The recording found the document's viewport had no box to record, and gives back the presentation
    /// it was handed, if any.
    NothingRecorded(Option<Presentation>),
    /// The committed frame recorded nothing, and presented the display list the compositor has; it gives
    /// back the presentation it was handed.
    PresentedUnrecorded(Option<Presentation>),
    /// The recording landed after the host wrote the document's rows. What it recorded stands as
    /// the compositor's frame, whether it presented it or the host does, but not its hit-test list,
    /// which names boxes that may be gone: the document keeps none.
    LandedBehindRows(Option<Presentation>),
}

/// What takes in a recording that presented itself: its output, whether the document's hit-test list
/// changed, and whether the recording publishes.
pub(crate) type TakeInPresented<'a> = &'a mut dyn FnMut(Arc<RecordingOutput>, bool, bool);

/// A recording the host publishes, with what its publication writes of the document's recordings.
pub(crate) struct Publication<'a> {
    pub(crate) pending: PendingRecording,
    /// Whether the recording landed after the host wrote the document's rows, so that the hit-test
    /// list it made names boxes that may be gone.
    pub(crate) behind_rows: bool,
    pub(crate) recorder: &'a mut RecorderState,
    pub(crate) hit_test_list: &'a mut Option<HitTestList>,
}

/// A recording that landed for the host to publish, and whether it landed behind the rows.
struct PendingPublication {
    pending: PendingRecording,
    behind_rows: bool,
}

/// What a document keeps of its recordings: the recording its host is to publish, the trace that
/// recording left, the recorder state the next recording records with, and the hit-test list of the
/// last recording published, which hit testing reads.
#[derive(Default)]
pub(crate) struct RecordingSlot {
    pending_recording: Option<PendingPublication>,
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
        let PendingPublication { pending, behind_rows } = self.pending_recording.take()?;
        Some(Publication {
            pending,
            behind_rows,
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
    /// flight in, and the presenter it holds back, and ends its clock lease, before it records again.
    pub(crate) fn take_recorder(&mut self) -> RecorderState {
        match std::mem::take(&mut self.recorder) {
            Recorder::Here(recorder) => recorder,
            Recorder::InFlight(_) => panic!("the host takes its recording in flight in before it records again"),
            Recorder::WithClock => panic!("the host ends its clock lease before it records again"),
        }
    }

    /// The recorder state, for a clock lease to take until it lands, where it is here.
    pub(crate) fn take_recorder_for_clock(&mut self) -> Option<RecorderState> {
        match std::mem::replace(&mut self.recorder, Recorder::WithClock) {
            Recorder::Here(recorder) => Some(recorder),
            elsewhere => {
                self.recorder = elsewhere;
                None
            }
        }
    }

    /// Gives back the recorder state a recording took, where it recorded nothing.
    pub(crate) fn give_back_recorder(&mut self, recorder: RecorderState) {
        self.recorder = Recorder::Here(recorder);
    }

    /// Takes in what a recording answered: a recording pending for the host to publish, or one the
    /// recording presented itself, which `take_in` takes in. `behind_rows` says the host wrote the
    /// document's rows since the frame it recorded. Answers the presentation the recording gives back.
    fn land(
        &mut self,
        answer: RecordingAnswer,
        behind_rows: bool,
        take_in: TakeInPresented<'_>,
    ) -> Option<Presentation> {
        let RecordingAnswer {
            mut recorder,
            recorded,
            trace,
            presentation,
            ..
        } = answer;
        self.pending_recording_trace = trace;
        match recorded {
            Recorded::Nothing | Recorded::PresentedUnrecorded => {}
            Recorded::Pending(pending) => self.pending_recording = Some(PendingPublication { pending, behind_rows }),
            Recorded::Presented {
                output,
                publishes_recording,
            } => crate::painting::record::publish::take_in_published_output(
                &mut recorder,
                &mut self.hit_test_list,
                output,
                publishes_recording,
                |output, hit_test_list_changed| {
                    take_in(output, hit_test_list_changed || behind_rows, publishes_recording);
                },
            ),
        }
        self.recorder = Recorder::Here(recorder);
        if behind_rows {
            self.hit_test_list = None;
        }
        presentation
    }

    /// Takes in what a recording the host waited for answered, leaving it pending for the host to
    /// publish.
    pub(crate) fn accept_recording_answer(&mut self, answer: RecordingAnswer) {
        let presentation = self.land(answer, false, &mut |_, _, _| {
            unreachable!("a recording the host waits for presents nothing itself")
        });
        debug_assert!(
            presentation.is_none(),
            "a recording the host waits for presents nothing"
        );
    }

    /// Lets the recording `flight` fly with the recorder state.
    pub(crate) fn fly(&mut self, flight: InFlight<RecordingAnswer>) {
        debug_assert!(
            !self.has_recording_in_flight() && !self.has_pending_recording(),
            "a document records one frame at a time"
        );
        self.recorder = Recorder::InFlight(flight);
    }

    /// Takes the recording in flight in at `boundary`, where it has finished. `rows_stand` answers
    /// whether the document's rows are still at the version of the frame it recorded.
    pub(crate) fn take_finished_recording_in(
        &mut self,
        boundary: &TaskBoundary,
        rows_stand: impl FnOnce(RowsVersion) -> bool,
        take_in: TakeInPresented<'_>,
    ) -> RecordingLanding {
        let Recorder::InFlight(flight) = std::mem::take(&mut self.recorder) else {
            return RecordingLanding::NoneInFlight;
        };
        match flight.try_take(boundary) {
            Ok(answer) => self.landing(answer, rows_stand, take_in),
            Err(flight) => {
                self.recorder = Recorder::InFlight(flight);
                RecordingLanding::StillInFlight
            }
        }
    }

    /// Waits for the recording in flight, and the frame it presents, and takes it in (see
    /// [`Self::take_finished_recording_in`]).
    pub(crate) fn join_recording_in_flight(
        &mut self,
        rows_stand: impl FnOnce(RowsVersion) -> bool,
        take_in: TakeInPresented<'_>,
    ) -> RecordingLanding {
        let Recorder::InFlight(flight) = std::mem::take(&mut self.recorder) else {
            return RecordingLanding::NoneInFlight;
        };
        release_held_recording_for_testing();
        let answer = flight.join(LockstepProof::for_reason(&RECORDING_NEEDS_ITS_RECORDER));
        self.landing(answer, rows_stand, take_in)
    }

    fn landing(
        &mut self,
        answer: RecordingAnswer,
        rows_stand: impl FnOnce(RowsVersion) -> bool,
        take_in: TakeInPresented<'_>,
    ) -> RecordingLanding {
        match answer.recorded {
            Recorded::Nothing => return RecordingLanding::NothingRecorded(self.land(answer, false, take_in)),
            Recorded::PresentedUnrecorded => {
                return RecordingLanding::PresentedUnrecorded(self.land(answer, false, take_in));
            }
            Recorded::Pending(_) | Recorded::Presented { .. } => {}
        }
        let rows_stand = rows_stand(answer.rows_version);
        let presentation = self.land(answer, !rows_stand, take_in);
        if rows_stand {
            RecordingLanding::Landed(presentation)
        } else {
            RecordingLanding::LandedBehindRows(presentation)
        }
    }

    /// Drops the pending recording unpublished. A recording that publishes wrote the retained
    /// paint-order tree, so the next one copies nothing from the published recording.
    pub(crate) fn discard_pending_recording(&mut self) {
        self.pending_recording_trace = None;
        if let Some(PendingPublication { pending, .. }) = self.pending_recording.take()
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
            Recorder::InFlight(_) | Recorder::WithClock => None,
        }
    }

    fn pending(publishes_recording: bool) -> PendingRecording {
        PendingRecording {
            recording: RecordingResult {
                output: RecordingOutput::default(),
                resources: Default::default(),
            },
            publishes_recording,
            svg_paint_resources: Default::default(),
        }
    }

    fn answer(recorder: RecorderState, recorded: Recorded) -> RecordingAnswer {
        RecordingAnswer {
            recorder,
            recorded,
            rows_version: RowsVersion::default(),
            trace: None,
            presentation: None,
        }
    }

    /// Leaves `pending` in the slot as a recording's answer would.
    fn accept_pending(slot: &mut RecordingSlot, pending: PendingRecording) {
        let recorder = slot.take_recorder();
        slot.accept_recording_answer(answer(recorder, Recorded::Pending(pending)));
    }

    #[test]
    fn the_slot_lends_its_recorder_state_to_a_job_and_takes_it_back_with_the_answer() {
        let mut engine = crate::css::style::StyleEngine::new();
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(crate::css::style::StyleEngineHandle::from_raw(&raw mut engine));
        let mut slot = RecordingSlot::default();
        let published = Arc::new(RecordingOutput::default());
        recorder_of(&mut slot).unwrap().published_recording = Some(published.clone());

        let job = RecordingJob {
            frame: arena.freeze_frame(0),
            rows_version: RowsVersion::default(),
            recorder: slot.take_recorder(),
            viewport: NodeSlotId::INVALID,
            trace_recordings: false,
            presentation: None,
        };
        assert!(recorder_of(&mut slot).unwrap().published_recording.is_none());
        let RecordingJob { recorder, .. } = job;
        slot.accept_recording_answer(answer(recorder, Recorded::Pending(pending(true))));
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

    /// A flight of the slot's recorder state that answers it back with `recorded`.
    fn flight_of(slot: &mut RecordingSlot, recorded: Recorded) -> InFlight<RecordingAnswer> {
        let recorder = slot.take_recorder();
        crate::paint_stage::paint_thread().submit(move |_| answer(recorder, recorded))
    }

    fn take_in_nothing(_: Arc<RecordingOutput>, _: bool, _: bool) {
        panic!("the recording presented nothing itself");
    }

    #[test]
    fn a_recording_in_flight_has_the_recorder_state_until_it_lands() {
        let mut slot = RecordingSlot::default();
        let published = Arc::new(RecordingOutput::default());
        recorder_of(&mut slot).unwrap().published_recording = Some(published.clone());
        let flight = flight_of(&mut slot, Recorded::Pending(pending(true)));
        slot.fly(flight);
        assert!(slot.has_recording_in_flight());
        assert!(recorder_of(&mut slot).is_none());
        assert!(
            slot.take_publication().is_none(),
            "nothing is pending before the recording lands"
        );

        assert!(matches!(
            slot.join_recording_in_flight(|_| true, &mut take_in_nothing),
            RecordingLanding::Landed(None)
        ));
        assert!(!slot.has_recording_in_flight());
        let publication = slot.take_publication().unwrap();
        assert!(Arc::ptr_eq(
            publication.recorder.published_recording.as_ref().unwrap(),
            &published
        ));
    }

    #[test]
    fn a_recording_that_presented_itself_lands_its_output_as_the_published_one() {
        let mut slot = RecordingSlot::default();
        let flight = flight_of(
            &mut slot,
            Recorded::Presented {
                output: RecordingOutput::default(),
                publishes_recording: true,
            },
        );
        slot.fly(flight);
        let mut taken_in = None;
        let landing = loop {
            match slot.take_finished_recording_in(
                &TaskBoundary::for_test(),
                |_| true,
                &mut |output, _, publishes_recording| {
                    taken_in = Some((output, publishes_recording));
                },
            ) {
                RecordingLanding::StillInFlight => crate::paint_stage::paint_thread().run(|| ()),
                landing => break landing,
            }
        };
        assert!(matches!(landing, RecordingLanding::Landed(None)));
        assert!(!slot.has_pending_recording(), "the host publishes nothing more");
        let (output, publishes_recording) = taken_in.expect("the output is taken in");
        assert!(publishes_recording);
        assert!(Arc::ptr_eq(
            recorder_of(&mut slot).unwrap().published_recording.as_ref().unwrap(),
            &output
        ));
        assert!(slot.hit_test_list().is_some());
    }

    #[test]
    fn a_recording_that_lands_behind_the_rows_keeps_no_hit_test_list() {
        let mut slot = RecordingSlot::default();
        let flight = flight_of(
            &mut slot,
            Recorded::Presented {
                output: RecordingOutput::default(),
                publishes_recording: false,
            },
        );
        slot.fly(flight);
        let mut hit_test_list_changed = false;
        let landing = slot.join_recording_in_flight(|_| false, &mut |_, changed, _| hit_test_list_changed = changed);
        assert!(matches!(landing, RecordingLanding::LandedBehindRows(None)));
        assert!(
            hit_test_list_changed,
            "the hit-test list the document read is no longer current"
        );
        assert!(slot.hit_test_list().is_none());

        let flight = flight_of(&mut slot, Recorded::Pending(pending(false)));
        slot.fly(flight);
        let landing = slot.join_recording_in_flight(|_| false, &mut take_in_nothing);
        assert!(matches!(landing, RecordingLanding::LandedBehindRows(None)));
        let publication = slot.take_publication().expect("the host presents what it did not");
        assert!(publication.behind_rows);
    }
}
