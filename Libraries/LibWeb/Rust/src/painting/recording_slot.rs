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
use crate::painting::ffi::FfiFlightBlocker;
use crate::painting::paint_read::PaintSource;
use crate::painting::paint_state::{PendingRecording, PendingRecordingTrace};
use crate::painting::presentation::Presentation;
use crate::painting::published_frame::{HeldFrame, PublishedFrame};
use crate::painting::record::change_ticks::RowPaintChange;
use crate::painting::record::publish::HitTestItemsOnScreen;
use crate::painting::record::recorder_state::RecorderState;
use crate::painting::record::vector_images::VectorImageRenderRequest;
use crate::painting::record::{RecordingInputs, RecordingOutput};
use crate::painting::vector_image_records::VectorImageRecords;
use crate::render_state::{LockstepProof, SampledFrame, TaskBoundary};
use crate::stage_thread::InFlight;
#[cfg(not(test))]
use crate::stage_thread::{ParkedJob, StopWord};
use std::sync::atomic::{AtomicBool, Ordering};
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
    held_frame: Option<HeldFrame>,
}

/// What a recording answers: the recorder state it was handed, what it made of what it recorded at
/// which rows version, and the presentation it was handed, if any.
pub(crate) struct RecordingAnswer {
    recorder: RecorderState,
    recorded: Recorded,
    rows_version: RowsVersion,
    trace: Option<PendingRecordingTrace>,
    presentation: Option<Presentation>,
    held_frame: Option<HeldFrame>,
}

/// What a recording made of what it recorded: nothing, where the document's viewport had no box to
/// record, a recording for the host to publish, one it published and presented itself, nothing, for a
/// committed frame that keeps the display list the compositor has, which it presented, or a committed
/// frame that waits for its SVG images.
enum Recorded {
    Nothing,
    Pending(PendingRecording),
    Presented {
        output: Arc<RecordingOutput>,
        publishes_recording: bool,
    },
    PresentedUnrecorded,
    /// A recording of a committed frame that renders SVG images, which only the host renders: the frame waits for
    /// them, with the presentation.
    NeedsVectorImages(SampledFrame<VectorImageFrame>),
}

/// A committed frame whose recording renders SVG images, with the presentation that presents it once the host has
/// rendered them, and the rows version its frame was frozen at.
pub(crate) struct VectorImageFrame {
    pending: PendingRecording,
    presentation: Presentation,
    rows_version: RowsVersion,
    /// The clock lane the frame starts once it is presented.
    lane: Option<crate::render_state::LaneDelivery>,
}

impl VectorImageFrame {
    /// The SVG images the frame renders.
    pub(crate) fn requests(&self) -> &[VectorImageRenderRequest] {
        &self.pending.recording.resources.vector_image_render_requests
    }

    /// Presents the frame, recorded with `recorder`, with `vector_images`, the records of the SVG images the host
    /// rendered, those of each of the frame's requests, beside the event loop.
    pub(crate) fn present(
        self,
        recorder: RecorderState,
        vector_images: &[Arc<VectorImageRecords>],
        presenting: &mut Presenting,
    ) -> RecordingAnswer {
        let Self {
            pending,
            mut presentation,
            rows_version,
            lane,
        } = self;
        let publishes_recording = pending.publishes_recording;
        let output = present_with_vector_images(&mut presentation, pending, &recorder, presenting, vector_images);
        let recorded = Recorded::Presented {
            output,
            publishes_recording,
        };
        let mut answer = RecordingAnswer {
            recorder,
            recorded,
            rows_version,
            trace: None,
            presentation: Some(presentation),
            held_frame: None,
        };
        if let Some(lane) = lane {
            lane.deliver(&mut answer);
        }
        answer
    }
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
            held_frame: None,
        }
    }

    /// Holds a recording of a committed frame that renders SVG images on the Paint thread, as `sampled`, the frame the
    /// render owner sampled, until the host has rendered them: the answer gives back the recorder state alone.
    pub(crate) fn hold_vector_images(
        self,
        sampled: impl FnOnce(VectorImageFrame) -> SampledFrame<VectorImageFrame>,
    ) -> Self {
        let Self {
            recorder,
            recorded,
            rows_version,
            trace,
            presentation,
            held_frame,
        } = self;
        // A recording handed a presentation presented its frame, unless it renders SVG images.
        let (recorded, presentation) = match (recorded, presentation) {
            (Recorded::Pending(pending), Some(presentation)) => (
                Recorded::NeedsVectorImages(sampled(VectorImageFrame {
                    pending,
                    presentation,
                    rows_version,
                    lane: None,
                })),
                None,
            ),
            unchanged => unchanged,
        };
        // The lane of a frame that waits for its SVG images records nothing where the compositor scrolled to.
        let held_frame = held_frame.filter(|_| !matches!(recorded, Recorded::NeedsVectorImages(_)));
        Self {
            recorder,
            recorded,
            rows_version,
            trace,
            presentation,
            held_frame,
        }
    }

    /// Takes the frame this answers, held for its clock lane, if the recording was handed one.
    pub(crate) fn take_held_frame(&mut self) -> Option<HeldFrame> {
        self.held_frame.take()
    }

    /// Hands `lane` the pieces of the clock lane of the committed frame this answers, once it presented: at once, or
    /// once the frame that waits for its SVG images is.
    pub(crate) fn start_clock_lane(&mut self, lane: crate::render_state::LaneDelivery) {
        match &mut self.recorded {
            Recorded::NeedsVectorImages(frame) => frame.get_mut().lane = Some(lane),
            _ => lane.deliver(self),
        }
    }

    /// What the clock lane of the committed frame this answers records and presents with, once the frame presented: a
    /// fork of the recorder state as the recording left it, with what it published, a seal of frames that start from
    /// the frame, and the recording the lane's fork of the render state takes in, if one published. None where the
    /// frame presented nothing a lane starts from.
    pub(crate) fn clock_lane_recorder(&self) -> Option<(RecorderState, Presentation, Option<Arc<RecordingOutput>>)> {
        let fork_after = |published: Option<&Arc<RecordingOutput>>| {
            let mut recorder = self.recorder.fork();
            if let Some(published) = published {
                recorder.published_recording = Some(Arc::clone(published));
            }
            recorder
        };
        let (recorder, output) = match &self.recorded {
            Recorded::PresentedUnrecorded => (fork_after(None), None),
            Recorded::Presented {
                output,
                publishes_recording: true,
            } => (fork_after(Some(output)), Some(Arc::clone(output))),
            _ => return None,
        };
        Some((recorder, self.presentation.as_ref()?.for_clock_lane()?, output))
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
            held_frame: None,
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
            held_frame: frozen.held,
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
            held_frame,
        } = self;
        let (pending, trace) = record_frame(frame, &mut recorder, viewport, trace_recordings, inputs);
        // A recording that renders an SVG image the recording published last did not draw leaves its frame for the host
        // to present, which renders it.
        let rendered_vector_images = match crate::painting::record::publish::renders_vector_images(&pending) {
            false => Some(None),
            true => crate::painting::record::publish::published_vector_image_records(&pending, &recorder).map(Some),
        };
        let recorded = match (presentation.as_mut(), presenting, rendered_vector_images) {
            (Some(presentation), Some(presenting), Some(rendered_vector_images)) => {
                let publishes_recording = pending.publishes_recording;
                let output = match rendered_vector_images {
                    Some(vector_images) => {
                        present_with_vector_images(presentation, pending, &recorder, presenting, &vector_images)
                    }
                    None => present(presentation, pending, &recorder, presenting),
                };
                Recorded::Presented {
                    output,
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
            held_frame,
        }
    }
}

/// The test hold on recordings: a test arms it for the next recording that flies, which then reads nothing of its frame
/// until the test releases it, so that the test writes the document beside a recording that has read nothing yet.
enum RecordingHold {
    Idle,
    Armed,
    Holding,
    // The Render thread waits inside its claim of a style update that flies, until the hold goes.
    #[cfg_attr(test, allow(dead_code))]
    HoldingClaim,
    // The claim hold gave way to a job another thread waits for on the Render thread, which then leaves the style
    // update unclaimed, for the host to take back.
    #[cfg_attr(test, allow(dead_code))]
    ClaimYielded,
    // A layout held is never handed to the Render thread until it goes, so that the thread runs the jobs of other
    // documents meanwhile, as one the collector finalizes asks of it.
    #[cfg(not(test))]
    HoldingLayout {
        _parked: ParkedJob,
    },
    ArmedForStyle,
    ArmedForClaim,
}

static RECORDING_HOLD: Mutex<RecordingHold> = Mutex::new(RecordingHold::Idle);
static RECORDING_HOLD_RELEASED: Condvar = Condvar::new();
// Whether the claim hold keeps the Render thread waiting, which a thread about to wait for the Render thread reads
// without taking the lock.
static CLAIM_HOLD_BLOCKS_RENDER_THREAD: AtomicBool = AtomicBool::new(false);

fn recording_hold() -> MutexGuard<'static, RecordingHold> {
    RECORDING_HOLD.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Whether a test holds a recording that flies.
pub(crate) fn recording_is_held_for_testing() -> bool {
    matches!(
        *recording_hold(),
        RecordingHold::Holding | RecordingHold::HoldingClaim | RecordingHold::ClaimYielded
    )
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

/// Submits `job`, the layout of a frame, to the Render thread, which claims it inside `claim_under`, so that the
/// host may take it back until then, or parks it where the test hold is armed for it. Answers the flight and whether
/// the hold took it.
#[cfg(not(test))]
pub(crate) fn submit_layout<R: Send + 'static>(
    job: impl FnOnce(&StopWord) -> R + Send + 'static,
    claim_under: fn(&mut dyn FnMut()),
) -> (InFlight<R>, bool) {
    let thread = crate::stage_thread::render_thread();
    let mut hold = recording_hold();
    if !matches!(*hold, RecordingHold::Armed) {
        return (thread.submit_reclaimable(job, claim_under), false);
    }
    let (flight, parked) = thread.park(job);
    *hold = RecordingHold::HoldingLayout { _parked: parked };
    (flight, true)
}

/// Whether the style update about to fly is the one the test hold is armed for, which then parks as a frame's
/// layout does.
#[cfg(not(test))]
pub(crate) fn take_style_flight_hold_for_testing() -> bool {
    let mut hold = recording_hold();
    if !matches!(*hold, RecordingHold::ArmedForStyle) {
        return false;
    }
    *hold = RecordingHold::Armed;
    true
}

/// Whether the style update about to fly is the one the test hold is armed to keep the Render thread from claiming.
#[cfg(not(test))]
pub(crate) fn take_style_flight_claim_hold_for_testing() -> bool {
    let mut hold = recording_hold();
    if !matches!(*hold, RecordingHold::ArmedForClaim) {
        return false;
    }
    *hold = RecordingHold::HoldingClaim;
    CLAIM_HOLD_BLOCKS_RENDER_THREAD.store(true, Ordering::Release);
    true
}

pub(crate) fn wait_while_recording_is_held_for_testing() {
    let _released = RECORDING_HOLD_RELEASED
        .wait_while(recording_hold(), |hold| matches!(hold, RecordingHold::Holding))
        .unwrap_or_else(PoisonError::into_inner);
}

/// Waits while the claim hold keeps the Render thread from claiming a style update, and answers whether to claim it: A
/// hold that gave way to another job leaves the update for the host to take back, as the test holding it expects.
#[cfg(not(test))]
pub(crate) fn wait_while_claim_is_held_for_testing() -> bool {
    let mut hold = RECORDING_HOLD_RELEASED
        .wait_while(recording_hold(), |hold| matches!(hold, RecordingHold::HoldingClaim))
        .unwrap_or_else(PoisonError::into_inner);
    if !matches!(*hold, RecordingHold::ClaimYielded) {
        return true;
    }
    *hold = RecordingHold::Idle;
    false
}

/// Holds the next recording that flies before it reads its frame, until
/// [`release_held_recording_for_testing`] or a wait for it.
pub(crate) fn hold_next_recording_for_testing() {
    *recording_hold() = RecordingHold::Armed;
}

/// Holds the next style update that flies before it reads anything, until
/// [`release_held_recording_for_testing`] or a wait for it, so that a test orders what it does beside the flight ahead
/// of everything the transaction computes.
pub(crate) fn hold_next_style_flight_for_testing() {
    *recording_hold() = RecordingHold::ArmedForStyle;
}

/// Keeps the Render thread from claiming the next style update that flies until the host waits for it, which then takes
/// it back, or until [`release_held_recording_for_testing`].
pub(crate) fn hold_next_style_flight_claim_for_testing() {
    *recording_hold() = RecordingHold::ArmedForClaim;
}

/// Lets the recording held for the test go, or disarms the hold no recording has taken yet.
pub(crate) fn release_held_recording_for_testing() {
    *recording_hold() = RecordingHold::Idle;
    CLAIM_HOLD_BLOCKS_RENDER_THREAD.store(false, Ordering::Release);
    RECORDING_HOLD_RELEASED.notify_all();
}

/// Has the claim hold give way, as it keeps the Render thread from every job after the one it holds: A thread that waits
/// for one of those, as the collector finalizing a document waits for its layout to be torn down, would wait forever.
/// The Render thread then leaves the held style update unclaimed, which the host takes back as it would have.
#[cfg(not(test))]
pub(crate) fn yield_claim_hold_for_testing() {
    if !CLAIM_HOLD_BLOCKS_RENDER_THREAD.load(Ordering::Acquire) {
        return;
    }
    let mut hold = recording_hold();
    if matches!(*hold, RecordingHold::HoldingClaim) {
        *hold = RecordingHold::ClaimYielded;
        RECORDING_HOLD_RELEASED.notify_all();
    }
    CLAIM_HOLD_BLOCKS_RENDER_THREAD.store(false, Ordering::Release);
}

/// Records `frame` of the document's `viewport` with `inputs` and `recorder`, which keeps the inputs of a recording that
/// publishes, and answers what it recorded, with the trace it left where `trace_recordings` asks for one.
/// The window a recording of `frame` with `inputs` is made for: none where it records the whole document, the window
/// of `published`, the recording published last, where that still covers what the frame's viewport shows, or else one
/// placed where the frame's scrollers are.
fn window_to_record(
    frame: &PublishedFrame,
    inputs: &RecordingInputs,
    published: Option<&RecordingOutput>,
) -> Option<Arc<crate::painting::record::window::RecordedWindow>> {
    if inputs.records_whole_document || inputs.records_without_visual_contexts {
        return None;
    }
    let paint_state = frame.paint_state();
    let tree = paint_state.visual_context_tree.as_deref()?;
    Some(crate::painting::record::window::window_of_frame(
        published.and_then(|published| published.window.as_ref().zip(published.visual_context_tree.as_deref())),
        tree,
        inputs.uncaptured.device_viewport_size,
        inputs.device_pixels_per_css_pixel,
        paint_state.scroll_own_offsets(),
    ))
}

pub(crate) fn record_frame(
    frame: PublishedFrame,
    recorder: &mut RecorderState,
    viewport: NodeSlotId,
    trace_recordings: bool,
    inputs: RecordingInputs,
) -> (PendingRecording, Option<PendingRecordingTrace>) {
    let RecorderState {
        published_recording,
        published_recording_count,
        scratch,
        absolute_rects,
        published_inputs,
    } = recorder;
    let source = PaintSource::new(&frame, absolute_rects);
    let publishes_recording = published_recording_count.note_recording(inputs.publishes_recording);
    let window = window_to_record(&frame, &inputs, published_recording.as_deref());
    let mut recording = crate::painting::record::traversal::record_display_list(
        &source,
        scratch,
        viewport,
        &inputs,
        published_recording.clone(),
        window,
        trace_recordings,
    );
    let output = &mut recording.output;
    output.surface_clear_color = inputs.surface_clear_color;
    output.async_scrolling_metadata = inputs.async_scrolling_metadata.clone().map(|mut metadata| {
        metadata.has_blocking_wheel_event_listeners |= output.has_blocking_wheel_event_listeners;
        metadata
    });
    let trace = trace_recordings.then_some(PendingRecordingTrace {
        viewport,
        should_paint_overlay: inputs.should_paint_overlay,
    });
    let svg_paint_resources = frame.svg_paint_resources().clone();
    drop(frame);
    if inputs.publishes_recording {
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
) -> Arc<RecordingOutput> {
    let output = Arc::new(crate::painting::record::publish::publish_to_presenter(
        pending, recorder,
    ));
    presentation.present(&output, presenting);
    output
}

/// Presents `pending`, which renders SVG images and was recorded with `recorder`, with `presentation`, beside the event
/// loop, drawing them as `vector_images`, the records the host rendered them as, and answers its output.
pub(crate) fn present_with_vector_images(
    presentation: &mut Presentation,
    pending: PendingRecording,
    recorder: &RecorderState,
    presenting: &mut Presenting,
    vector_images: &[Arc<VectorImageRecords>],
) -> Arc<RecordingOutput> {
    let output = Arc::new(crate::painting::record::publish::publish_with_vector_images(
        pending,
        recorder,
        vector_images,
    ));
    presentation.present(&output, presenting);
    output
}

/// What the host knows that freezing a document's frame for a recording reads.
pub(crate) struct FrameInputs {
    pub(crate) viewport: NodeSlotId,
    pub(crate) css_viewport_rect: crate::css::css_pixels::CssPixelRect,
    pub(crate) publishes_recording: bool,
    /// The canvas rect the root background painted in the recording published last, if any.
    pub(crate) published_root_background_canvas_rect: Option<crate::css::css_pixels::CssPixelRect>,
}

/// A document's frame, frozen for a recording, with what the recording reads beside it, and the rows version it was
/// frozen at.
pub(crate) struct FrozenFrame {
    pub(crate) frame: crate::painting::published_frame::PublishedFrame,
    pub(crate) tree_inputs: crate::painting::host::FfiVisualContextTreeInputs,
    pub(crate) root_background_source: crate::painting::host::RootBackgroundSource,
    pub(crate) trace_recordings: bool,
    pub(crate) rows_version: crate::layout::RowsVersion,
    /// The frame again, for the clock lane of a committed frame to keep.
    pub(crate) held: Option<HeldFrame>,
}

/// Freezes the frame of the document whose arena `arena` is for a recording of its viewport, or none where the
/// viewport has no box to paint.
pub(crate) fn freeze_recording_frame(arena: &mut LayoutNodeArena, inputs: FrameInputs) -> Option<FrozenFrame> {
    // Recording reads overflow, and reading overflow never measures it.
    arena.settle_scrollable_overflow();
    if !arena.paintable_row_is_populated(inputs.viewport) || arena.stacking_context_entries(inputs.viewport).is_none() {
        return None;
    }
    // The root background paints the union of the viewport and the root's overflow, so it is the
    // one output a viewport move can change. Drop its caches before the frame is published instead
    // of treating the viewport position as a frame-wide input.
    if let Some(published_canvas_rect) = inputs.published_root_background_canvas_rect {
        let root = arena
            .paint_state()
            .root_background_source
            .expect("a recording follows paint preparation")
            .root_layout_node;
        let canvas_rect = crate::painting::record::paint::background_resolution::root_background_canvas_rect(
            &arena.paintable_rows(),
            root,
            inputs.css_viewport_rect,
        );
        if canvas_rect != published_canvas_rect {
            arena.note_paint_change(root, RowPaintChange::DRAWING);
        }
    }
    // The recording reads the document as it is now: what the host writes after this goes to the next frame.
    let frame = arena.freeze_frame();
    let rows_version = arena.rows_version();
    let paint_state = arena.paint_state();
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
        held: None,
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
    /// The committed frame's recording renders SVG images, which the host renders for the frame (see
    /// [`RecordingSlot::take_vector_image_frame`]); it keeps the presentation.
    NeedsVectorImages,
    /// The recording landed after the host wrote the document's rows. What it recorded stands as
    /// the compositor's frame, whether it presented it or the host does, but not what hit testing reads of
    /// it, as the boxes it shows may be gone.
    LandedBehindRows(Option<Presentation>),
}

/// What takes in a recording that presented itself: its output, and whether the recording publishes.
pub(crate) type TakeInPresented<'a> = &'a mut dyn FnMut(Arc<RecordingOutput>, bool);

/// A recording the host publishes, with what its publication writes of the document's recordings.
pub(crate) struct Publication<'a> {
    pub(crate) pending: PendingRecording,
    /// Whether the recording landed after the host wrote the document's rows, so that the boxes the
    /// frame shows may be gone.
    pub(crate) behind_rows: bool,
    pub(crate) recorder: &'a mut RecorderState,
    pub(crate) hit_test_items_on_screen: &'a mut HitTestItemsOnScreen,
}

/// A recording that landed for the host to publish, and whether it landed behind the rows.
struct PendingPublication {
    pending: PendingRecording,
    behind_rows: bool,
}

/// What a document keeps of its recordings: the recording its host is to publish, the trace that
/// recording left, the recorder state the next recording records with, and what the hit-test items of
/// the frame on screen are as of.
#[derive(Default)]
pub(crate) struct RecordingSlot {
    pending_recording: Option<PendingPublication>,
    /// The committed frame that waits for the host to render its SVG images.
    vector_image_frame: Option<SampledFrame<VectorImageFrame>>,
    pending_recording_trace: Option<PendingRecordingTrace>,
    recorder: Recorder,
    hit_test_items_on_screen: HitTestItemsOnScreen,
}

impl RecordingSlot {
    pub(crate) fn has_pending_recording(&self) -> bool {
        self.pending_recording.is_some()
    }

    /// The pending recording, for the host to publish, with the recorder state it recorded with and
    /// what the frame it replaces on screen placed. A recording is pending only once it has landed.
    pub(crate) fn take_publication(&mut self) -> Option<Publication<'_>> {
        let Recorder::Here(recorder) = &mut self.recorder else {
            return None;
        };
        let PendingPublication { pending, behind_rows } = self.pending_recording.take()?;
        Some(Publication {
            pending,
            behind_rows,
            recorder,
            hit_test_items_on_screen: &mut self.hit_test_items_on_screen,
        })
    }

    /// The trace the pending recording left, for the host to read.
    pub(crate) fn take_pending_recording_trace(&mut self) -> Option<PendingRecordingTrace> {
        self.pending_recording_trace.take()
    }

    /// The recording the document published last, which the next recording copies from, while no recording is in
    /// flight.
    pub(crate) fn published_recording(&self) -> Option<Arc<RecordingOutput>> {
        match &self.recorder {
            Recorder::Here(recorder) => recorder.published_recording.clone(),
            Recorder::InFlight(_) => None,
        }
    }

    /// Whether a recording of the document is in flight.
    pub(crate) fn has_recording_in_flight(&self) -> bool {
        matches!(self.recorder, Recorder::InFlight(_))
    }

    /// The generation of the hit-test items of the frame on screen, which moves on whenever a frame places other
    /// items, or zero where no frame stands.
    pub(crate) fn hit_test_items_generation(&self) -> u64 {
        self.hit_test_items_on_screen.generation()
    }

    /// The recorder state, for a recording to take until it answers. The host takes a recording in
    /// flight in before it records again.
    pub(crate) fn take_recorder(&mut self) -> RecorderState {
        match std::mem::take(&mut self.recorder) {
            Recorder::Here(recorder) => recorder,
            Recorder::InFlight(_) => panic!("the host takes its recording in flight in before it records again"),
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
            Recorded::NeedsVectorImages(frame) => self.vector_image_frame = Some(frame),
            Recorded::Pending(pending) => self.pending_recording = Some(PendingPublication { pending, behind_rows }),
            Recorded::Presented {
                output,
                publishes_recording,
            } => crate::painting::record::publish::take_in_presented_output(
                &mut recorder,
                &mut self.hit_test_items_on_screen,
                output,
                publishes_recording,
                |output| take_in(output, publishes_recording),
            ),
        }
        self.recorder = Recorder::Here(recorder);
        if behind_rows {
            self.hit_test_items_on_screen.forget();
        }
        presentation
    }

    /// Takes in what a recording the host waited for answered, leaving it pending for the host to
    /// publish.
    pub(crate) fn accept_recording_answer(&mut self, answer: RecordingAnswer) {
        let presentation = self.land(answer, false, &mut |_, _| {
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
            Recorded::NeedsVectorImages(_) => {
                let presentation = self.land(answer, false, take_in);
                debug_assert!(presentation.is_none(), "the frame keeps the presentation");
                return RecordingLanding::NeedsVectorImages;
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

    /// The committed frame that waits for the host to render its SVG images, with the recorder state, which goes with
    /// it to the Paint thread again: the host hands the frame back to the slot to fly with.
    pub(crate) fn take_vector_image_frame(&mut self) -> Option<(SampledFrame<VectorImageFrame>, RecorderState)> {
        let frame = self.vector_image_frame.take()?;
        Some((frame, self.take_recorder()))
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
            Recorder::InFlight(_) => None,
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
            held_frame: None,
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
            frame: arena.freeze_frame(),
            rows_version: RowsVersion::default(),
            recorder: slot.take_recorder(),
            viewport: NodeSlotId::INVALID,
            trace_recordings: false,
            presentation: None,
            held_frame: None,
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

    fn take_in_nothing(_: Arc<RecordingOutput>, _: bool) {
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
                output: Arc::new(RecordingOutput::default()),
                publishes_recording: true,
            },
        );
        slot.fly(flight);
        let mut taken_in = None;
        let landing = loop {
            match slot.take_finished_recording_in(
                &TaskBoundary::for_test(),
                |_| true,
                &mut |output, publishes_recording| {
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
        assert_ne!(slot.hit_test_items_generation(), 0);
    }

    #[test]
    fn a_recording_that_lands_behind_the_rows_leaves_no_hit_test_items_on_screen() {
        let mut slot = RecordingSlot::default();
        let flight = flight_of(
            &mut slot,
            Recorded::Presented {
                output: Arc::new(RecordingOutput::default()),
                publishes_recording: false,
            },
        );
        slot.fly(flight);
        let landing = slot.join_recording_in_flight(|_| false, &mut |_, _| {});
        assert!(matches!(landing, RecordingLanding::LandedBehindRows(None)));
        assert_eq!(slot.hit_test_items_generation(), 0);

        let flight = flight_of(&mut slot, Recorded::Pending(pending(false)));
        slot.fly(flight);
        let landing = slot.join_recording_in_flight(|_| false, &mut take_in_nothing);
        assert!(matches!(landing, RecordingLanding::LandedBehindRows(None)));
        let publication = slot.take_publication().expect("the host presents what it did not");
        assert!(publication.behind_rows);
    }
}
