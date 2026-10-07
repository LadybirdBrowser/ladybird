/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The render clock's lease of a document's render state.
//!
//! As a task begins, the host leases its document's render state, with the recorder state and its navigable's
//! presentation, to the render clock, which ticks at the display's ticks on the StyleLayout thread: each tick reaches
//! the state there by the document's name, as the render owner, samples the running animations of the plan the last
//! rendering update sealed at the tick's time, shows the samples in their elements' boxes, lays out what they moved,
//! and has the Paint thread record and present the frame with the navigable's presenter, beside the event loop and the
//! next tick, which waits for that recording only once it has laid out. The host ends the lease with any job it hands
//! the owner, which waits for at most the one tick that runs and takes everything back at once, but for the recorder
//! state and presentation, which the recording beside it brings back once the host needs them.
//! What a tick showed never becomes visible to script: the boxes take back the styles the host installed before any
//! job of the host reads them, so the animations' timeline moves only in a rendering update.
//!
//! A lease may also follow the pointer, which the render clock hears of from the compositor: a tick hovers what is under
//! it, for real, as the host would as it handles the move (see [`hover`]). A lease of a document with no running
//! animations only does that.

use super::owner::{self, DocumentId};
use super::wait::TaskStart;
use super::{DocumentHost, RenderState};
use crate::css::css_pixels::CssPixelRect;
use crate::css::style::animations::{AnimationTimelineSamples, ScrollProgress};
use crate::css::style::engine_sample::{DependentRestyle, NeedsHost, TickShownRecords};
use crate::css::style::tree::StyleNodeID;
use crate::layout::node_data::NodeSlotId;
use crate::layout::tree_update_marks::{
    FfiLayoutTreeUpdateMark, LayoutTreeUpdateMarkWrite, layout_tree_update_reuse_reason,
};
use crate::layout::used_values::FfiCssPixelRect;
use crate::layout::{ClockRound, ClockRoundDeclined, HostStyle, LayoutNodeArena, LayoutRoundAnswer, build_keeps_box};
use crate::paint_stage::Presenting;
use crate::painting::ffi::FfiPresentation;
use crate::painting::paint_passes::{ClockTickVisualContexts, VisualContextsNeedHost, prepare_for_clock_tick};
use crate::painting::paintable_geometry::absolute_border_box_rect;
use crate::painting::presentation::Presentation;
use crate::painting::record::damage::PaintDamage;
use crate::painting::record::inputs::UnframedRecordingInputs;
use crate::painting::record::publish::{renders_vector_images, take_in_published_output, take_in_recording};
use crate::painting::record::recorder_state::RecorderState;
use crate::painting::record::{RecordingInputs, RecordingOutput};
use crate::painting::recording_slot::{
    FrameInputs, FrozenFrame, RecordingAnswer, RecordingJob, freeze_recording_frame, present, record_frame,
    wait_while_recording_is_held_for_testing,
};
use crate::painting::visual_context::VisualContextTree;
use crate::stage_thread::{InFlight, Relay, Riding, StopWord, Ticker};
use smallvec::SmallVec;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

pub(crate) mod hover;
pub(crate) use hover::PendingPointer;

/// What the ticks of a clock lease sample, sealed at the end of a rendering update.
pub(crate) struct ClockPlan {
    /// The elements whose running animations a tick samples.
    elements: Vec<StyleNodeID>,
    /// The monotonic time, in milliseconds, at which the document's timestamps are zero.
    time_origin: f64,
    /// The timestamp of the next event of the animations, which the host sends: a tick at or past it samples nothing.
    deadline: f64,
    /// The timestamp at which the animations of the document timeline a tick samples have all ended: the tick at or
    /// past it shows their ends, and after it only a scroll moves anything.
    last_end: f64,
    /// The scroll timelines a tick samples where the compositor has scrolled their scrollers to.
    scroll_timelines: Vec<FfiPlannedScrollTimeline>,
    round: ClockRound,
    /// What a tick hovers the element under the pointer with, where the lease follows the pointer.
    hover: Option<hover::HoverPlan>,
}

/// A scroll timeline whose animations the ticks of a clock lease sample, as a rendering update sealed it.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FfiPlannedScrollTimeline {
    /// The unique node id of the element whose scroll node the timeline follows, or of the document for its viewport,
    /// and whether along the vertical axis.
    pub scroller: i64,
    pub vertical: bool,
    /// The scroll offset at 100% progress, in CSS pixels, in the layout the host sealed the plan from.
    pub max_scroll_offset: f64,
    /// The progress, in percent, at the scroll offset the host laid out.
    pub progress: f64,
    /// The progress from which, and up to which, the timeline's animations send no event the host has listeners for.
    pub progress_start: f64,
    pub progress_end: f64,
}

/// Where the compositor had scrolled the scroll node of an element, or of a document's viewport, by its unique node id,
/// to at a display tick, in CSS pixels.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FfiScrollOffset {
    pub scroller: i64,
    pub x: f64,
    pub y: f64,
}

impl ClockPlan {
    pub(crate) fn new(
        elements: Vec<StyleNodeID>,
        time_origin: f64,
        deadline: f64,
        last_end: f64,
        scroll_timelines: Vec<FfiPlannedScrollTimeline>,
        round: ClockRound,
        hover: Option<hover::HoverPlan>,
    ) -> Self {
        Self {
            elements,
            time_origin,
            deadline,
            last_end,
            scroll_timelines,
            round,
            hover,
        }
    }

    /// The progress of the scroll timelines where `scroll_offsets` scrolled their scrollers to, or where the host laid
    /// out those it has none for. Past the progress at which an animation sends an event, the host has it to send.
    fn scroll_progress(&self, scroll_offsets: &[FfiScrollOffset]) -> Result<SmallVec<[ScrollProgress; 2]>, Park> {
        self.scroll_timelines
            .iter()
            .map(|timeline| {
                let progress = scroll_offsets
                    .iter()
                    .find(|offset| offset.scroller == timeline.scroller)
                    .map_or(timeline.progress, |offset| {
                        let position = if timeline.vertical { offset.y } else { offset.x };
                        position / timeline.max_scroll_offset * 100.0
                    });
                if !(timeline.progress_start..timeline.progress_end).contains(&progress) {
                    return Err(Park("scrolled outside a scroll timeline's range"));
                }
                Ok(ScrollProgress {
                    scroller: timeline.scroller,
                    vertical: timeline.vertical,
                    progress,
                })
            })
            .collect()
    }

    /// Whether a tick samples running animations.
    pub(crate) fn animates(&self) -> bool {
        !self.elements.is_empty()
    }

    /// Whether the plan follows the pointer.
    pub(crate) fn follows_pointer(&self) -> bool {
        self.hover.is_some()
    }

    /// Has the lease's ticks follow no pointer.
    pub(crate) fn drop_hover(&mut self) {
        self.hover = None;
    }
}

/// What a clock lease brings its host back: the recording of the frame a tick presented last, which brings the
/// recorder state and presentation the lease took, the boxes its ticks showed samples in with the styles the host
/// installed for them, the boxes they built again from records of their own, and what the ticks' rounds owe the host,
/// in the order they ran. The render state stays with the render owner, which the lease names it to.
pub(crate) struct LeaseLanding {
    document: DocumentId,
    pub(super) recording: TickRecording,
    pub(super) plan: ClockPlan,
    pub(super) ticked: Vec<(NodeSlotId, HostStyle)>,
    pub(super) built: TickBuilt,
    pub(super) owed: Vec<LayoutRoundAnswer>,
    /// The border boxes of the plan's elements, and of those the hover restyled, in the last frame a tick presented.
    pub(super) presented_border_boxes: Vec<(StyleNodeID, CssPixelRect)>,
    /// The color each box the hover's transitions restyled showed in that frame, as `0xAARRGGBB`. For a test.
    pub(super) presented_colors: Vec<(StyleNodeID, u32)>,
    /// Where the compositor had scrolled to at the latest tick that said so, which the plan's scroll timelines follow,
    /// and the hit tests of its hover.
    scroll_offsets: Vec<FfiScrollOffset>,
    /// The timestamp at which the last frame presented shows the animations of the document timeline, or negative
    /// infinity for the host's frame, and the progress at which it shows the plan's scroll timelines.
    shown_at: f64,
    shown_scroll_progress: SmallVec<[ScrollProgress; 2]>,
    /// Whether a tick found the lease could sample no more: past the deadline or the animations' ends, or something only
    /// the host computes.
    parked: bool,
    /// Whether a tick presented a frame.
    pub(super) presented: bool,
    /// What the lease's hover moved, which the host takes in as it lands.
    pub(super) hovered: hover::LeaseHover,
    /// The fork of the render state the lease ticks on, where it took one: the host's state is then the host's alone,
    /// and nothing the lease does lands in it. See [`crate::fork`].
    pub(super) fork: Option<super::RenderFork>,
    /// The recorder state the host lent the lease, which it gets back as it was where the lease forked: the lease's ticks
    /// record with a fork of it.
    pub(super) host_recorder: Option<RecorderState>,
    /// Whether the lease records with the recorder state the host lent it, rather than a fork of it the host gave it.
    holds_host_recorder: bool,
    /// How many pointer moves the lease's hover took, whether it hovered them or left them to the host.
    pub(super) hover_moves: u64,
    /// Where the pointer was at the last move the hover took, or none where it left the context.
    pub(super) last_pointer: Option<Option<libgfx_rust::FloatPoint>>,
    /// What the lease and the host tell each other: beside an idle event loop the lease leaves a move to the host, which
    /// hovers it in its next rendering update as soon.
    activity: Arc<HostActivity>,
}

// A lease runs on the StyleLayout thread, and what it brings back crosses back to the host's.
const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<LeaseLanding>();
};

/// The right to wait for the one tick a clock lease runs, which only [`ClockLease::end`] mints.
pub(crate) struct EndsLease(());

impl crate::stage_thread::Flown for LeaseLanding {
    type JoinRight = EndsLease;
}

/// The recorder state and presentation of a clock lease, as the recording of a tick's frame gives them back, with what
/// it presented.
pub(crate) struct ClockRecorder {
    pub(super) recorder: RecorderState,
    pub(super) presentation: Presentation,
    pub(super) presented: TickPresented,
}

/// What the recording of a tick's frame presented.
pub(crate) enum TickPresented {
    /// No tick recorded a frame since the recorder state came back.
    Nothing,
    /// The frame, which the document takes in as its last recording.
    Frame {
        output: Arc<RecordingOutput>,
        hit_test_list_changed: bool,
    },
    /// Nothing: the frame renders an SVG image, which only the host renders, and the lease samples no more.
    LeftToHost,
}

/// The recording of the frame the last tick of a clock lease presented, which may still run on the Paint thread, and
/// which brings back the recorder state and presentation the lease took. A tick waits for it once it has laid out the
/// next frame, and the host once it needs them again: dropping it would leave the presentation to the Paint thread.
#[must_use]
pub(crate) struct TickRecording(Riding<ClockRecorder>);

impl TickRecording {
    /// Waits for the recording, and answers what it gives back.
    pub(super) fn land(mut self) -> ClockRecorder {
        self.0.take(WaitsForTickRecording(()))
    }
}

/// The right to wait for the recording of a tick's frame, which only a tick that records the next frame and
/// [`TickRecording::land`] mint.
pub(crate) struct WaitsForTickRecording(());

impl crate::stage_thread::Flown for ClockRecorder {
    type JoinRight = WaitsForTickRecording;
}

/// A document's render state, leased to the render clock. A task boundary cannot take it in, as a lease has no
/// `try_take`: only [`Self::end`] takes it back, which waits for at most one tick, and which any wait of the host for the
/// render state ends it with.
pub(crate) struct ClockLease {
    flight: InFlight<LeaseLanding>,
    ticks: Arc<ClockTicks>,
}

impl ClockLease {
    /// Leases the render state of `document`, with `recorder` and `presentation`, to the render clock as the event loop
    /// begins a task or goes idle, to tick `plan`. A lease that `holds_animations` samples none until a task begins.
    /// Answers the lease, and the ticks the render clock hands it.
    pub(super) fn begin(
        start: &TaskStart,
        document: DocumentId,
        recorder: RecorderState,
        presentation: Presentation,
        plan: ClockPlan,
        holds_animations: bool,
        activity: Arc<HostActivity>,
    ) -> (Self, Arc<ClockTicks>) {
        let follows_scrolling = !plan.scroll_timelines.is_empty() || plan.hover.is_some();
        let animates = plan.animates();
        let follows_pointer = plan.follows_pointer();
        // The pointer moving over the document is what a hover follows: a lease that may hover beside the host's tasks
        // forks the render state before the host first writes it, as the state then is the one the screen shows.
        let forks = follows_pointer && activity.pointer_is_active();
        let (flight, ticker) = Self::landing(
            start,
            document,
            recorder,
            presentation,
            plan,
            true,
            Arc::clone(&activity),
        );
        let ticks = Arc::new(ClockTicks {
            ticker: Mutex::new(ticker),
            latest: AtomicI64::new(i64::MIN),
            follows_scrolling,
            scroll_offsets: Mutex::default(),
            queued: AtomicBool::new(false),
            parked: AtomicBool::new(!animates),
            animations_held: AtomicBool::new(animates && holds_animations),
            paused: AtomicBool::new(false),
            pointer: std::sync::Mutex::new(hover::PointerState::new(follows_pointer)),
            animates,
            forked: AtomicBool::new(false),
            forks_on_host_write: forks,
            activity,
            hover_moves_taken_in: AtomicU64::new(0),
            report: Mutex::default(),
        });
        (
            Self {
                flight,
                ticks: Arc::clone(&ticks),
            },
            ticks,
        )
    }

    /// Leases the render state again to the render clock, with the ticks of the lease the host ended last, which the
    /// render clock kept handing the pointer: `plan` is that lease's, which samples no animations.
    pub(super) fn resume(
        start: &TaskStart,
        document: DocumentId,
        recorder: RecorderState,
        presentation: Presentation,
        plan: ClockPlan,
        ticks: Arc<ClockTicks>,
    ) -> Self {
        debug_assert!(!plan.animates());
        let activity = Arc::clone(&ticks.activity);
        let (flight, ticker) = Self::landing(start, document, recorder, presentation, plan, true, activity);
        *ticks.ticker.lock().expect("clock ticks ticker") = ticker;
        ticks.paused.store(false, Ordering::Relaxed);
        Self { flight, ticks }
    }

    fn landing(
        _: &TaskStart,
        document: DocumentId,
        recorder: RecorderState,
        presentation: Presentation,
        plan: ClockPlan,
        holds_host_recorder: bool,
        activity: Arc<HostActivity>,
    ) -> (InFlight<LeaseLanding>, Ticker<LeaseLanding>) {
        let shown_scroll_progress = plan.scroll_progress(&[]).unwrap_or_default();
        let parked = !plan.animates();
        crate::stage_thread::style_layout_thread().lease(LeaseLanding {
            document,
            recording: TickRecording(Riding::landed(ClockRecorder {
                recorder,
                presentation,
                presented: TickPresented::Nothing,
            })),
            plan,
            ticked: Vec::new(),
            built: TickBuilt::default(),
            owed: Vec::new(),
            presented_border_boxes: Vec::new(),
            presented_colors: Vec::new(),
            scroll_offsets: Vec::new(),
            shown_at: f64::NEG_INFINITY,
            shown_scroll_progress,
            parked,
            presented: false,
            hovered: hover::LeaseHover::default(),
            fork: None,
            host_recorder: None,
            holds_host_recorder,
            hover_moves: 0,
            last_pointer: None,
            activity,
        })
    }

    /// Ends the lease: says the stop word, and waits for the tick that runs, if one does. The host's document module
    /// ends a lease only where the host waits for its render state.
    pub(super) fn end(self) -> LeaseLanding {
        self.flight.join(EndsLease(()))
    }

    /// Forks the render state for the lease before the host first writes it, where the lease forks then and wrote
    /// nothing of the host's state yet, waiting for the tick that runs. Answers whether the lease ticks on a fork.
    pub(super) fn fork_before_host_write(&self) -> bool {
        if self.ticks.is_forked() {
            return true;
        }
        if !self.ticks.forks_on_host_write {
            return false;
        }
        let ticks = Arc::clone(&self.ticks);
        self.ticks
            .ticker
            .lock()
            .expect("clock ticks ticker")
            .run(move |landing, _| {
                if landing.fork.is_none() && !landing.writes_host_state() {
                    landing.fork();
                }
                if landing.fork.is_some() {
                    ticks.forked.store(true, Ordering::Relaxed);
                }
            });
        crate::stage_thread::style_layout_thread().run(|| ());
        self.ticks.is_forked()
    }

    /// Takes back the recorder state the host lent a lease that forked as it ticked, waiting for the tick that runs.
    pub(super) fn take_host_recorder(&self) -> Option<RecorderState> {
        let taken = Arc::new(Mutex::new(None));
        let into = Arc::clone(&taken);
        self.ticks
            .ticker
            .lock()
            .expect("clock ticks ticker")
            .run(move |landing, _| *into.lock().expect("taken recorder") = landing.host_recorder.take());
        crate::stage_thread::style_layout_thread().run(|| ());
        taken.lock().expect("taken recorder").take()
    }

    /// The ticks the render clock hands the lease.
    pub(super) fn ticks(&self) -> &Arc<ClockTicks> {
        &self.ticks
    }
}

/// The right to sample a frame of a document for the Paint thread to present. Only a tick of the render clock and the
/// commit of a rendering update mint one, in the job the StyleLayout thread runs them in, so a job the host hands the
/// render owner to ask or write its render state cannot sample a frame.
pub(crate) struct SamplingTurn(());

/// The timestamp a frame shows a document's animations at, which is never earlier than that of a frame sampled before
/// it: only [`SampleClock::next`] mints one.
#[derive(Clone, Copy)]
pub(crate) struct SampleTime(f64);

/// What hands out the times a document's frames are sampled at, on the render owner, across clock leases.
#[derive(Clone)]
pub(crate) struct SampleClock {
    last: f64,
}

impl Default for SampleClock {
    fn default() -> Self {
        Self {
            last: f64::NEG_INFINITY,
        }
    }
}

impl SampleClock {
    /// The time to sample a frame asked for at `timestamp`: that, or the time of the frame sampled last, if later.
    pub(crate) fn next(&mut self, timestamp: f64) -> SampleTime {
        self.last = self.last.max(timestamp);
        SampleTime(self.last)
    }
}

/// A frame of a document the render owner sampled, for the Paint thread to present. Only a [`SamplingTurn`] makes one,
/// and the Paint thread presents nothing else (see [`crate::paint_stage`]).
pub(crate) struct SampledFrame<T>(T);

impl<T> SampledFrame<T> {
    fn new(_: &SamplingTurn, frame: T) -> Self {
        Self(frame)
    }

    pub(crate) fn get(&self) -> &T {
        &self.0
    }

    pub(crate) fn into_inner(self) -> T {
        self.0
    }
}

/// A frame frozen for a recording, with what it is recorded with, and the visual contexts a tick prepared for it.
pub(crate) struct RecordedFrame {
    frozen: FrozenFrame,
    viewport: NodeSlotId,
    inputs: RecordingInputs,
    visual_contexts: Option<ClockTickVisualContexts>,
}

/// What a rendering update's frame presents: a recording, or the display list the compositor has, with the visual
/// context tree of the render state where the host says it changed.
#[expect(
    clippy::large_enum_variant,
    reason = "a document commits one frame at a time, which moves to the Paint thread without an allocation"
)]
pub(crate) enum CommittedSample {
    Recorded(RecordedFrame),
    Unrecorded {
        visual_context_tree: Option<Arc<VisualContextTree>>,
    },
}

impl SampledFrame<CommittedSample> {
    /// Records the frame, which a rendering update committed, with `recorder`, where it records, and presents it with
    /// `presentation`.
    fn present_committed(
        self,
        recorder: RecorderState,
        mut presentation: Presentation,
        presenting: &mut Presenting,
    ) -> RecordingAnswer {
        match self.0 {
            CommittedSample::Recorded(RecordedFrame {
                frozen,
                viewport,
                inputs,
                visual_contexts: _,
            }) => RecordingJob::new(frozen, recorder, viewport, Some(presentation))
                .run_beside_host(inputs, presenting)
                .hold_vector_images(SampledFrame),
            CommittedSample::Unrecorded { visual_context_tree } => {
                presentation.present_unrecorded(visual_context_tree, presenting);
                RecordingAnswer::presented_unrecorded(recorder, presentation)
            }
        }
    }
}

/// What a rendering update's frame shows: a new recording of the document, with what freezing it reads and the inputs
/// it is recorded with but for what the frame decides, or the display list the compositor has, with the render state's
/// visual context tree where the host says the tree changed.
#[expect(
    clippy::large_enum_variant,
    reason = "a document commits one frame at a time, which moves to the render owner without an allocation"
)]
pub(crate) enum CommittedContent {
    Recording {
        frame_inputs: FrameInputs,
        inputs: UnframedRecordingInputs,
    },
    Unrecorded {
        sends_visual_context_tree: bool,
    },
}

/// A rendering update's frame, which the host commits to the render owner: what it shows, the recorder state it is
/// recorded with, the presentation that presents it, the time the update sampled the document's animations at, and
/// whether a test holds its recording.
pub(crate) struct CommittedFrame {
    pub(crate) content: CommittedContent,
    pub(crate) recorder: RecorderState,
    pub(crate) presentation: Presentation,
    pub(crate) timestamp: f64,
    pub(crate) held_for_testing: bool,
}

impl CommittedFrame {
    /// Samples the frame from `state`, as the render owner, and hands it on with `relay` to the Paint thread, which
    /// records and presents it beside the host. The frames the render clock samples after it sample no earlier.
    pub(super) fn sample(self, state: &mut RenderState, relay: Relay<RecordingAnswer>) {
        let turn = SamplingTurn(());
        let Self {
            content,
            recorder,
            presentation,
            timestamp,
            held_for_testing,
        } = self;
        let frame = match content {
            CommittedContent::Recording { frame_inputs, inputs } => {
                let viewport = frame_inputs.viewport;
                let Some(frozen) = freeze_recording_frame(state.arena_mut(), frame_inputs) else {
                    relay.land(RecordingAnswer::nothing_recorded(recorder, Some(presentation)));
                    return;
                };
                let inputs = inputs.for_frame(frozen.tree_inputs, frozen.root_background_source);
                SampledFrame::new(
                    &turn,
                    CommittedSample::Recorded(RecordedFrame {
                        frozen,
                        viewport,
                        inputs,
                        visual_contexts: None,
                    }),
                )
            }
            CommittedContent::Unrecorded {
                sends_visual_context_tree,
            } => SampledFrame::new(
                &turn,
                CommittedSample::Unrecorded {
                    visual_context_tree: sends_visual_context_tree
                        .then(|| state.arena_mut().paint_state().borrow().visual_context.tree.clone())
                        .flatten(),
                },
            ),
        };
        state.sample_clock.next(timestamp);
        crate::paint_stage::relay_presenting(relay, frame, move |frame, presenting| {
            if held_for_testing {
                wait_while_recording_is_held_for_testing();
            }
            frame.present_committed(recorder, presentation, presenting)
        });
    }
}

/// The display ticks the render clock hands a lease, folded into one queued tick, which runs at the latest of their times.
pub struct ClockTicks {
    /// What the ticks run on: the lease that runs, or the one that ended last, until the host takes it up again.
    ticker: Mutex<Ticker<LeaseLanding>>,
    /// The latest frame time of the ticks handed to the lease, in nanoseconds of the monotonic clock. It only grows, so
    /// a tick handed an earlier time than one before it, as a display tick behind the immediate first one is, never
    /// samples the animations back in time.
    latest: AtomicI64,
    /// Whether the lease samples scroll timelines or follows the pointer, for which it keeps where the compositor had
    /// scrolled to at the latest tick handed to it, until the tick that runs next takes it.
    follows_scrolling: bool,
    scroll_offsets: Mutex<Option<Vec<FfiScrollOffset>>>,
    /// Whether a tick is queued.
    queued: AtomicBool,
    /// Whether a tick parked the lease, which then samples nothing more.
    parked: AtomicBool,
    /// Whether the lease samples no animations until a task begins: an idle event loop's rendering updates run them.
    animations_held: AtomicBool,
    /// Whether the lease ended, and the host may take it up again with these ticks: until it does, a pointer move waits
    /// for the lease, and the render clock goes on handing it the pointer.
    paused: AtomicBool,
    /// Where the render clock heard the pointer went, until a tick hovers what is there.
    pointer: std::sync::Mutex<hover::PointerState>,
    /// Whether the plan the lease began with samples running animations.
    animates: bool,
    /// Whether the lease ticks on a fork of the render state, which the host's waits for its own state leave running.
    forked: AtomicBool,
    /// Whether the lease forks the render state before the host first writes it, where the pointer moves over the
    /// document: the host's write would leave nothing of the state the screen shows for the lease to hover.
    forks_on_host_write: bool,
    /// What the lease and the document's host tell each other of the pointer and the event loop.
    activity: Arc<HostActivity>,
    /// How many of the hover's moves the host took in.
    hover_moves_taken_in: AtomicU64,
    /// What the hover of a forked lease did so far, which the host reads as a rendering update begins beside it.
    report: Mutex<LaneReport>,
}

/// What the hover of a forked lease did so far. See [`ClockTicks::report`].
#[derive(Clone, Default)]
pub(crate) struct LaneReport {
    /// Where the pointer was at the last move the hover took, or none where it left the context; nothing where it took
    /// none.
    pub(crate) hovered_pointer: Option<Option<libgfx_rust::FloatPoint>>,
    /// The elements whose transitions the hover started, and when, in the document's milliseconds.
    pub(crate) transition_starts: Vec<(StyleNodeID, f64)>,
    /// Whether a tick presented a frame.
    pub(crate) presented: bool,
    /// How many pointer moves the hover took, which tells the host whether it took one since the host last read the
    /// report.
    pub(crate) hover_moves: u64,
}

impl ClockTicks {
    /// Hands the lease a tick at `frame_time_nanoseconds`, at which the compositor had scrolled to `scroll_offsets`: the
    /// tick already queued runs at the latest time and offsets, or a tick is queued. Answers whether the lease wants the
    /// next tick, which it does until it ends or parks, or while a pointer move waits for a tick to hover it.
    pub(super) fn tick(self: &Arc<Self>, frame_time_nanoseconds: i64, scroll_offsets: &[FfiScrollOffset]) -> bool {
        let ticker = self.ticker.lock().expect("clock ticks ticker");
        if !ticker.is_live() {
            return false;
        }
        // A tick that runs nothing still says where the compositor scrolled to, which a pointer move it hovers later
        // is hit tested at.
        if self.follows_scrolling && !scroll_offsets.is_empty() {
            *self.scroll_offsets.lock().expect("clock tick scroll offsets") = Some(scroll_offsets.to_vec());
        }
        let animates = !self.parked.load(Ordering::Relaxed) && !self.animations_held.load(Ordering::Relaxed);
        if !animates && !self.pointer_state().waits() {
            return false;
        }
        self.latest.fetch_max(frame_time_nanoseconds, Ordering::AcqRel);
        if self.queued.swap(true, Ordering::AcqRel) {
            return true;
        }
        let ticks = Arc::clone(self);
        ticker.run(move |landing, stop| {
            // A tick handed after the flag drops queues another run, and one handed before it is in `latest`.
            ticks.queued.swap(false, Ordering::AcqRel);
            if let Some(scroll_offsets) = ticks.scroll_offsets.lock().expect("clock tick scroll offsets").take() {
                landing.scroll_offsets = scroll_offsets;
            }
            let pointer = ticks.pointer_state().take();
            let samples_animations = !ticks.animations_held.load(Ordering::Relaxed);
            landing.tick(ticks.latest.load(Ordering::Acquire), pointer, samples_animations, stop);
            if landing.fork.is_some() {
                ticks.forked.store(true, Ordering::Relaxed);
                *ticks.report.lock().expect("clock lease report") = landing.report();
            }
            // A transition a hover started keeps the ticks coming until it ends.
            ticks.parked.store(
                landing.parked && !landing.hovered.has_running_transitions(),
                Ordering::Relaxed,
            );
            if landing.hovered.is_parked() {
                ticks.pointer_state().park();
            }
        });
        true
    }

    /// Lets the lease sample the animations it held, as a task begins, and answers whether it should tick at once.
    pub(super) fn run_animations(&self) -> bool {
        self.animations_held.swap(false, Ordering::Relaxed) && !self.parked.load(Ordering::Relaxed)
    }

    /// Hands the lease where the pointer went, which the next tick hovers, and answers what the lease wants next.
    pub(super) fn pointer_moved(&self, pointer: PendingPointer) -> hover::PointerAnswer {
        self.activity.note_pointer_move();
        if !self.paused.load(Ordering::Relaxed) && !self.ticker.lock().expect("clock ticks ticker").is_live() {
            return hover::PointerAnswer::Disarm;
        }
        self.pointer_state().moved(pointer)
    }

    /// Keeps a pointer move for a lease the host may take up again with these ticks, or, with `paused` false, lets the
    /// render clock disarm them at the next one.
    pub(super) fn set_paused(&self, paused: bool) {
        self.paused.store(paused, Ordering::Relaxed);
    }

    /// Whether a pointer move waits for a tick to hover it.
    pub(super) fn pointer_waits(&self) -> bool {
        self.pointer_state().waits()
    }

    /// Hands the lease a pointer move, and ticks it at `frame_time_nanoseconds`. For a test.
    pub(super) fn inject_pointer(self: &Arc<Self>, pointer: PendingPointer, frame_time_nanoseconds: i64) {
        let _ = self.pointer_state().moved(pointer);
        self.tick(frame_time_nanoseconds, &[]);
    }

    fn pointer_state(&self) -> std::sync::MutexGuard<'_, hover::PointerState> {
        self.pointer.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    /// What the hover of the lease did so far, where it ticks on a fork of the render state.
    pub(super) fn report(&self) -> LaneReport {
        self.report.lock().expect("clock lease report").clone()
    }

    /// Notes that the host took in `report`, and answers whether the hover moved since the host last did.
    pub(super) fn take_in_hover_moves(&self, report: &LaneReport) -> bool {
        self.hover_moves_taken_in
            .fetch_max(report.hover_moves, Ordering::Relaxed)
            < report.hover_moves
    }

    /// Whether the lease ticks on a fork of the render state.
    pub(super) fn is_forked(&self) -> bool {
        self.forked.load(Ordering::Relaxed)
    }

    /// Whether a tick parked the lease.
    pub(super) fn is_parked(&self) -> bool {
        self.parked.load(Ordering::Relaxed)
    }
}

/// What parks a lease: something of a tick only the host computes, and what, for a developer.
struct Park(&'static str);

impl From<NeedsHost> for Park {
    fn from(_: NeedsHost) -> Self {
        Self("a sample the host computes")
    }
}

impl From<ClockRoundDeclined> for Park {
    fn from(ClockRoundDeclined(reason): ClockRoundDeclined) -> Self {
        Self(reason)
    }
}

impl From<VisualContextsNeedHost> for Park {
    fn from(VisualContextsNeedHost(reason): VisualContextsNeedHost) -> Self {
        Self(reason)
    }
}

/// The boxes the render clock built again from records of its own, which the host builds again from its own once it
/// has the state back, as it would have had its style moved them: the records they showed, and the marks the host
/// makes for its next layout tree build, in its own marks and on the boxes.
#[derive(Default)]
pub(crate) struct TickBuilt {
    pub(super) shown: TickShownRecords,
    pub(super) mark_writes: Vec<LayoutTreeUpdateMarkWrite>,
    pub(super) box_marks: Vec<(StyleNodeID, FfiLayoutTreeUpdateMark)>,
}

impl TickBuilt {
    /// Keeps the marks the host makes to build again from its own records what a clock frame built from its
    /// records at `target`.
    fn build_again_on_landing(&mut self, arena: &LayoutNodeArena, (target, reuse_reason): (StyleNodeID, u8)) {
        if self
            .box_marks
            .iter()
            .any(|(marked, made)| *marked == target && made.reuse_reason == reuse_reason)
        {
            return;
        }
        self.box_marks.push((target, style_change_mark(reuse_reason)));
        self.mark_writes
            .extend(arena.layout_tree_update_mark_writes(target, reuse_reason));
    }
}

/// What a clock frame builds again for an element whose boxes the new size of a container moved.
#[derive(Clone, Copy)]
enum TickRebuild {
    /// The boxes of its `::before` and `::after`.
    PseudoElements,
    /// Its box, which goes, and which `parent`'s box places among its children wherever the host's record gives the
    /// element one.
    TakeAway { parent: StyleNodeID },
    /// Its box and every box below it, which it gains among `parent`'s children.
    Insert { parent: StyleNodeID },
    /// The boxes a clock frame built for it and below it.
    Again,
}

/// The mark a style change makes for the next layout tree build, which may reuse a box as `reuse_reason` says.
fn style_change_mark(reuse_reason: u8) -> FfiLayoutTreeUpdateMark {
    FfiLayoutTreeUpdateMark {
        reuse_reason,
        is_child_list_insertion: false,
        is_structural_boundary_self_rebuild: false,
    }
}

/// How many times a tick lays out again what the containers it resized restyled, as the host's layout update
/// stabilizes them.
const SIZE_QUERY_ROUND_LIMIT: usize = 8;

/// Shows in their boxes the styles of the elements whose style a size query or container-relative unit decided below
/// the containers in `resized`, against their new sizes, keeping each box's host style in `ticked`, and marks the boxes
/// those styles move for the round's tree build to build again. An element whose animations `animated` samples
/// composes over its host's style, which only the host restyles.
fn restyle_size_query_dependents(
    state: &mut RenderState,
    resized: &[StyleNodeID],
    animated: &[StyleNodeID],
    ticked: &mut Vec<(NodeSlotId, HostStyle)>,
    built: &mut TickBuilt,
) -> Result<(), Park> {
    let mut seen: smallvec::SmallVec<[StyleNodeID; 8]> = smallvec::SmallVec::new();
    let mut rebuilt: smallvec::SmallVec<[(StyleNodeID, TickRebuild); 2]> = smallvec::SmallVec::new();
    for &container in resized {
        for dependent in state.engine_mut().size_container_query_dependents(container) {
            let Some(dependent) = state.engine_mut().size_query_restyle_target(dependent) else {
                continue;
            };
            if seen.contains(&dependent) {
                continue;
            }
            seen.push(dependent);
            if animated.contains(&dependent) {
                return Err(Park("a size query dependent animates"));
            }
            let row = state.arena.arena().bound_row(dependent);
            let restyled = match state
                .engine_mut()
                .restyle_size_query_dependent(dependent, !row.is_invalid())?
            {
                DependentRestyle::Unmoved => continue,
                DependentRestyle::InBox(restyled) => restyled,
                DependentRestyle::PseudoElementsMove(restyled) => {
                    rebuilt.push((dependent, TickRebuild::PseudoElements));
                    restyled
                }
                DependentRestyle::LosesItsBox { parent } => {
                    rebuilt.push((dependent, TickRebuild::TakeAway { parent }));
                    continue;
                }
                DependentRestyle::GainsABox { parent } => {
                    rebuilt.push((dependent, TickRebuild::Insert { parent }));
                    continue;
                }
                DependentRestyle::BuiltBoxesMove => {
                    rebuilt.push((dependent, TickRebuild::Again));
                    continue;
                }
            };
            let arena = state.arena.arena();
            if let Some(host_style) = arena.install_animation_sample(row, restyled)? {
                ticked.push((row, host_style));
            }
            arena.push_paint_damage_for_repaint(row, PaintDamage::ALL_PRODUCERS);
        }
    }
    for (node, rebuild) in rebuilt {
        let landing = mark_for_tree_build(state, node, rebuild, animated, ticked)?;
        built.build_again_on_landing(state.arena.arena(), landing);
    }
    Ok(())
}

/// Marks what `rebuild` builds again for `node` for the round's tree build, as the host marks it for a style change,
/// from what the clock shows, and answers the box the host marks to build it again from its own records once it has the
/// state back. The build keeps every box a sample shows in: the box whose pseudo-elements it regenerates, and the
/// parent box it inserts a box into or takes one out of, in place. Where it cannot, as where an anonymous box wraps a
/// neighbor, the build builds the parent's box again with every box below it (see [`mark_region_for_tree_build`]).
fn mark_for_tree_build(
    state: &mut RenderState,
    node: StyleNodeID,
    rebuild: TickRebuild,
    animated: &[StyleNodeID],
    ticked: &mut Vec<(NodeSlotId, HostStyle)>,
) -> Result<(StyleNodeID, u8), Park> {
    use layout_tree_update_reuse_reason::{CHILD_LIST_INSERTION, PSEUDO_ELEMENT_CHANGE};
    let arena = state.arena.arena_mut();
    let in_place = match rebuild {
        TickRebuild::PseudoElements => {
            let in_place = build_keeps_box(arena, node, PSEUDO_ELEMENT_CHANGE);
            if in_place {
                arena.mark_layout_tree_update(Some(node), style_change_mark(PSEUDO_ELEMENT_CHANGE));
            }
            in_place.then_some((node, PSEUDO_ELEMENT_CHANGE))
        }
        TickRebuild::Insert { parent } => {
            // The build reads which children of the parent it inserts boxes for from their marks.
            arena.mark_layout_tree_update(Some(node), FfiLayoutTreeUpdateMark::NODE_INSERT);
            let in_place =
                !arena.bound_row(parent).is_invalid() && build_keeps_box(arena, parent, CHILD_LIST_INSERTION);
            if in_place {
                arena.mark_layout_tree_update(Some(parent), FfiLayoutTreeUpdateMark::NODE_INSERT);
            }
            in_place.then_some((parent, 0))
        }
        TickRebuild::TakeAway { parent } => {
            let in_place = arena.can_take_box_away_in_place(parent, node);
            if in_place {
                for (box_, host_style) in sampled_boxes_in(arena, arena.bound_row(node), animated, ticked)? {
                    arena.restore_host_style(box_, host_style);
                }
                arena.mark_layout_tree_update(Some(node), style_change_mark(0));
            }
            in_place.then_some((parent, 0))
        }
        TickRebuild::Again => None,
    };
    if let Some(landing) = in_place {
        return Ok(landing);
    }
    let root = match rebuild {
        TickRebuild::PseudoElements | TickRebuild::Again => node,
        TickRebuild::Insert { parent } | TickRebuild::TakeAway { parent } => parent,
    };
    mark_region_for_tree_build(state, root, animated, ticked)?;
    Ok((root, 0))
}

/// Takes out of `ticked` the boxes in the subtree of `root` the clock showed samples in, with the styles the host
/// installed for them. A box an animated element's samples show in only the host builds again.
fn sampled_boxes_in(
    arena: &LayoutNodeArena,
    root: NodeSlotId,
    animated: &[StyleNodeID],
    ticked: &mut Vec<(NodeSlotId, HostStyle)>,
) -> Result<smallvec::SmallVec<[(NodeSlotId, HostStyle); 8]>, Park> {
    let live = |box_: NodeSlotId| (!box_.is_invalid()).then_some(box_);
    let holds = |inner: NodeSlotId| {
        std::iter::successors(live(inner), |&box_| live(arena.data(box_).parent.get())).any(|box_| box_ == root)
    };
    if animated.iter().any(|&element| holds(arena.bound_row(element))) {
        return Err(Park("a rebuilt box holds an animated element"));
    }
    Ok(ticked.extract_if(.., |(box_, _)| holds(*box_)).collect())
}

/// Marks the box of `root` for the round's tree build to build again with every box below it, from what the clock
/// shows, as the host marks a parent whose children's boxes it cannot build again in place: the samples the clock
/// showed in those boxes are shown to the build as their elements' records. The root's box is its parent's child, which
/// no anonymous box wraps with its neighbors, and neither the document's root nor its body, whose boxes the build
/// places otherwise.
fn mark_region_for_tree_build(
    state: &mut RenderState,
    root: StyleNodeID,
    animated: &[StyleNodeID],
    ticked: &mut Vec<(NodeSlotId, HostStyle)>,
) -> Result<(), Park> {
    use crate::css::style::bridge::element_adjustment_fact::{
        IS_DOCUMENT_ELEMENT, IS_HTML_BODY_ELEMENT, RENDERED_IN_TOP_LAYER,
    };
    let arena = state.arena.arena();
    let row = arena.bound_row(root);
    let parent = match row.is_invalid() {
        true => NodeSlotId::INVALID,
        false => arena.data(row).parent.get(),
    };
    let placed_otherwise = IS_DOCUMENT_ELEMENT | IS_HTML_BODY_ELEMENT | RENDERED_IN_TOP_LAYER;
    if parent.is_invalid()
        || crate::layout::node_facts::has_flag(arena.data(parent), crate::layout::node_data::NodeFlag::Anonymous)
        || arena.element_adjustment_facts(Some(root)) & placed_otherwise != 0
    {
        return Err(Park("a rebuilt box the host places"));
    }
    let sampled = sampled_boxes_in(arena, row, animated, ticked)?;
    let samples: smallvec::SmallVec<[(StyleNodeID, u64); 8]> = sampled
        .iter()
        .filter_map(|&(box_, _)| Some((arena.dom_node_style_node(box_)?, arena.node_style_record(box_))))
        .collect();
    for (element, record) in samples {
        state.engine_mut().show_sample_in_rebuilt_box(element, record);
    }
    let arena = state.arena.arena();
    for (box_, host_style) in sampled {
        arena.restore_host_style(box_, host_style);
    }
    arena.mark_layout_tree_update(Some(root), style_change_mark(0));
    Ok(())
}

impl LeaseLanding {
    /// Samples the plan's animations at the timestamp of `frame_time_nanoseconds` and where the compositor has scrolled
    /// to, shows the samples, lays out what they moved and presents the frame, unless the host said the stop word: the
    /// host waits for the lease. A tick at or past the deadline, or one that needs the host, parks the lease, and so
    /// does the tick that shows the ends of the animations where no scroll moves anything after them. A tick handed
    /// `pointer`, where the pointer moved since the last tick, hovers what is under it first (see [`hover`]).
    fn tick(
        &mut self,
        frame_time_nanoseconds: i64,
        pointer: Option<PendingPointer>,
        samples_animations: bool,
        stop: &StopWord,
    ) {
        if stop.is_said() {
            return;
        }
        let timestamp = frame_time_nanoseconds as f64 / 1_000_000.0 - self.plan.time_origin;
        let mut hovers = pointer.filter(|_| !self.hovered.is_parked());
        let transitions = self.hovered.has_running_transitions();
        let samples_animations = samples_animations && !self.parked;
        if !samples_animations && hovers.is_none() && !transitions {
            return;
        }
        // A hover runs on a fork of the render state, which the host's state never sees. A lease that wrote the host's
        // state already leaves the move to the host, and so does one beside an idle event loop, which hovers it in its
        // next rendering update as soon.
        if hovers.is_some() && self.fork.is_none() {
            match !self.writes_host_state() && self.activity.runs_task() {
                true => self.fork(),
                false => hovers = None,
            }
        }
        if !samples_animations && hovers.is_none() && !transitions {
            return;
        }
        let turn = SamplingTurn(());
        // The tick runs on the render owner, the one thread that reaches the state. It shows the records the frames
        // before it showed, and takes them back as it ends.
        let document = self.document;
        let mut fork = self.fork.take();
        let tick = |landing: &mut Self, state: &mut RenderState| {
            let sampled_at = state.sample_clock.next(timestamp);
            // What the plan's animations show at the tick, where the lease still samples them.
            let progress = match samples_animations {
                false => None,
                true => landing.sample_progress(sampled_at).unwrap_or_else(|_| {
                    landing.parked = true;
                    None
                }),
            };
            if progress.is_none() && hovers.is_none() && !transitions {
                return;
            }
            state
                .engine_mut()
                .lend_tick_shown(std::mem::take(&mut landing.built.shown));
            let mut moved = false;
            if transitions {
                moved |= landing.sample_hover_transitions(state, sampled_at.0);
            }
            if let Some(pointer) = hovers {
                landing.hover_moves += 1;
                landing.last_pointer = Some(pointer.position);
                moved |= landing.hover(state, pointer, sampled_at.0);
            }
            if let Some(progress) = progress {
                match landing.sample(state, sampled_at, &progress) {
                    Ok(()) => {
                        moved = true;
                        landing.shown_at = sampled_at.0;
                        landing.shown_scroll_progress = progress;
                    }
                    Err(_) => landing.parked = true,
                }
                if sampled_at.0 >= landing.plan.last_end && landing.plan.scroll_timelines.is_empty() {
                    landing.parked = true;
                }
            }
            if moved {
                match landing.lay_out_and_present(state, &turn) {
                    Ok(()) => {
                        if hover::logs_hover() {
                            eprintln!("hover lane: tick presented a frame");
                        }
                        landing.presented = true;
                    }
                    Err(Park(reason)) => {
                        if hover::logs_hover() {
                            eprintln!("hover lane: frame left to the host: {reason}");
                        }
                        landing.parked = true;
                        landing.hovered.park();
                    }
                }
            }
            landing.built.shown = state.engine_mut().take_tick_shown();
        };
        match &mut fork {
            Some(fork) => tick(self, fork),
            None => owner::with_state(document, None, |state| tick(self, state)),
        }
        self.fork = fork;
    }

    /// What the lease's hover did so far.
    fn report(&self) -> LaneReport {
        LaneReport {
            hovered_pointer: self.last_pointer,
            transition_starts: self
                .hovered
                .transitions
                .iter()
                .map(|started| (started.transitions.node, started.start_time))
                .collect(),
            presented: self.presented,
            hover_moves: self.hover_moves,
        }
    }

    /// Whether a tick of the lease wrote the host's state, which only the lease's landing takes back.
    fn writes_host_state(&self) -> bool {
        self.presented || !self.ticked.is_empty() || !self.built.shown.is_empty() || !self.owed.is_empty()
    }

    /// Forks the render state, as the frame the lease began with left it, for the lease's ticks to write and present: the
    /// host gets back the recorder state it lent the lease as it is, and the ticks record with a fork of it.
    fn fork(&mut self) {
        debug_assert!(self.fork.is_none() && !self.writes_host_state());
        self.fork = Some(owner::with_state(self.document, None, |state| state.fork()));
        if !self.holds_host_recorder {
            return;
        }
        self.holds_host_recorder = false;
        let ClockRecorder {
            recorder,
            presentation,
            presented,
        } = self.recording.0.take(WaitsForTickRecording(()));
        self.recording = TickRecording(Riding::landed(ClockRecorder {
            recorder: recorder.fork(),
            presentation,
            presented,
        }));
        self.host_recorder = Some(recorder);
    }

    /// The progress of the scroll timelines a tick sampled at `sampled_at` samples the plan's animations at, or none where
    /// a frame that showed the document timeline's animations ended shows them already and nothing scrolled since. A tick
    /// at or past the deadline, or past what the scroll timelines plan for, parks the lease.
    fn sample_progress(&self, sampled_at: SampleTime) -> Result<Option<SmallVec<[ScrollProgress; 2]>>, Park> {
        if sampled_at.0 >= self.plan.deadline {
            return Err(Park("past the deadline"));
        }
        let scroll_progress = self.plan.scroll_progress(&self.scroll_offsets)?;
        if self.shown_at >= self.plan.last_end && scroll_progress == self.shown_scroll_progress {
            return Ok(None);
        }
        Ok(Some(scroll_progress))
    }

    fn sample(
        &mut self,
        state: &mut RenderState,
        sampled_at: SampleTime,
        scroll_progress: &[ScrollProgress],
    ) -> Result<(), Park> {
        let Self {
            plan, ticked, hovered, ..
        } = self;
        let samples = AnimationTimelineSamples::at_tick(sampled_at.0, scroll_progress);
        for &element in &plan.elements {
            // The transitions a hover started in place of those the element ran are the hover's to sample.
            if hovered.owns_transitions_of(element) {
                continue;
            }
            let arena = state.arena.arena();
            let row = arena.bound_row(element);
            if row.is_invalid() {
                return Err(Park("an animated element has no box"));
            }
            // Every tick samples over the record the host installed.
            let host_record = ticked
                .iter()
                .find(|(ticked_row, _)| *ticked_row == row)
                .map_or_else(|| arena.node_style_record(row), |(_, host_style)| host_style.record());
            let reference_box = crate::painting::ffi::committed_transform_reference_box(&arena.paintable_rows(), row);
            let sample = state
                .engine_mut()
                .sample_at(element, host_record, samples, reference_box)?;
            if let Some(host_style) = state.arena.arena().install_animation_sample(row, sample)? {
                ticked.push((row, host_style));
            }
            state
                .arena
                .arena()
                .push_paint_damage_for_repaint(row, PaintDamage::ALL_PRODUCERS);
        }
        Ok(())
    }

    /// Lays out what the tick moved and presents the frame.
    fn lay_out_and_present(&mut self, state: &mut RenderState, turn: &SamplingTurn) -> Result<(), Park> {
        let Self {
            plan,
            ticked,
            built,
            owed,
            ..
        } = self;
        // A container the round resized restyles what its size decides, as the host's style update after a layout
        // does, and lays it out again, until the containers stand.
        for _ in 0..SIZE_QUERY_ROUND_LIMIT {
            let Some(answer) = plan.round.run(&mut state.arena, owed)? else {
                return self.present(state, turn);
            };
            let resized: smallvec::SmallVec<[StyleNodeID; 4]> = answer.resized_size_containers().collect();
            if resized.is_empty() {
                return self.present(state, turn);
            }
            restyle_size_query_dependents(state, &resized, &plan.elements, ticked, built)?;
        }
        // The last restyle may have left nothing to lay out again.
        match state.arena.arena().layout_is_up_to_date(false) {
            true => self.present(state, turn),
            false => Err(Park("size containers did not settle")),
        }
    }

    /// Takes in the frame the last tick presented, and has the Paint thread record the document's frame again, with the
    /// inputs of the last recording that published, and present it beside the event loop and the next tick. A frame
    /// whose visual contexts only the host settles, or whose trace the host reads, is the host's to present, and so is
    /// any frame after one that renders an SVG image.
    fn present(&mut self, state: &mut RenderState, turn: &SamplingTurn) -> Result<(), Park> {
        let mut clock_recorder = self.recording.0.take(WaitsForTickRecording(()));
        let arena = state.arena.arena_mut();
        match freeze_tick_frame(arena, &mut clock_recorder) {
            Ok((frozen, inputs, visual_contexts)) => {
                let frame = SampledFrame::new(
                    turn,
                    RecordedFrame {
                        frozen,
                        viewport: arena.layout_root(),
                        inputs,
                        visual_contexts,
                    },
                );
                self.recording = TickRecording(crate::paint_stage::ride_presenting(frame, move |frame, presenting| {
                    clock_recorder.record(frame, presenting)
                }));
            }
            Err(park) => {
                self.recording = TickRecording(Riding::landed(clock_recorder));
                return Err(park);
            }
        }
        let rows = arena.paintable_rows();
        self.presented_border_boxes.clear();
        let installed = self
            .hovered
            .installs
            .iter()
            .filter_map(|install| StyleNodeID::from_raw(install.element.style_node));
        self.presented_border_boxes.extend(
            self.plan
                .elements
                .iter()
                .copied()
                .chain(installed)
                .filter_map(|element| {
                    let row = arena.bound_row(element);
                    rows.paintable_row_is_populated(row)
                        .then(|| (element, absolute_border_box_rect(&rows, row)))
                }),
        );
        self.presented_colors.clear();
        let restyled = self.hovered.transitions.iter().flat_map(|started| {
            std::iter::once(started.transitions.node)
                .chain(started.transitions.inheriting.iter().map(|(descendant, _)| *descendant))
        });
        // NB: A descendant that inherits what the transitions animate may have no box, as under `display: none`.
        self.presented_colors.extend(restyled.filter_map(|element| {
            let row = arena.bound_row(element);
            if !arena.slot_is_live(row) {
                return None;
            }
            let payloads = arena.style_payloads(row)?;
            let color = crate::layout::ComputedValuesView::new(&payloads.groups)
                .inherited_text()
                .color;
            Some((element, color))
        }));
        Ok(())
    }
}

/// Takes the frame the last tick presented in as the document's last recording, and prepares and freezes the
/// document's frame for the next recording, with the inputs of the last recording that published.
fn freeze_tick_frame(
    arena: &mut LayoutNodeArena,
    clock_recorder: &mut ClockRecorder,
) -> Result<(FrozenFrame, RecordingInputs, Option<ClockTickVisualContexts>), Park> {
    let ClockRecorder {
        recorder,
        presentation,
        presented,
    } = clock_recorder;
    match std::mem::replace(presented, TickPresented::Nothing) {
        TickPresented::Nothing => {}
        TickPresented::Frame {
            output,
            hit_test_list_changed,
        } => {
            take_in_recording(arena, output, hit_test_list_changed, true);
        }
        TickPresented::LeftToHost => return Err(Park("the host presents the frame")),
    }
    let viewport = arena.layout_root();
    if arena.paint_state().borrow().trace_recordings {
        return Err(Park("a trace reads the recording"));
    }
    let visual_contexts = prepare_for_clock_tick(arena, viewport, presentation)?;
    let inputs = recorder
        .published_inputs
        .take()
        .ok_or(Park("no published recording inputs"))?;
    let frame_inputs = FrameInputs {
        viewport,
        css_viewport_rect: inputs.css_viewport_rect,
        publishes_recording: true,
        published_root_background_canvas_rect: recorder
            .published_recording
            .as_ref()
            .map(|recording| recording.root_background_canvas_rect),
        hit_test_item_capacity_hint: recorder
            .published_hit_test_items
            .as_ref()
            .map_or(0, |published| published.items.len()),
    };
    let Some(frozen) = freeze_recording_frame(arena, frame_inputs) else {
        recorder.published_inputs = Some(inputs);
        return Err(Park("the frame does not freeze"));
    };
    Ok((frozen, inputs, visual_contexts))
}

impl ClockRecorder {
    /// Records `frame`, which a tick sampled, and presents it with the visual contexts the tick prepared for it, on the
    /// Paint thread. Only the host renders an SVG image.
    fn record(mut self, frame: SampledFrame<RecordedFrame>, presenting: &mut Presenting) -> Self {
        let Self {
            recorder,
            presentation,
            presented,
        } = &mut self;
        let SampledFrame(RecordedFrame {
            frozen,
            viewport,
            inputs,
            visual_contexts,
        }) = frame;
        let (pending, _) = record_frame(frozen.frame, recorder, viewport, false, inputs);
        // The recording wrote the paint-order tree, which no longer describes the recording published last.
        if renders_vector_images(&pending) {
            recorder.forget_published_recording();
            *presented = TickPresented::LeftToHost;
            return self;
        }
        if let Some(visual_contexts) = visual_contexts {
            presentation.take_visual_context_tree(visual_contexts);
        }
        let output = present(presentation, pending, recorder, presenting);
        take_in_published_output(recorder, &mut None, output, true, |output, hit_test_list_changed| {
            *presented = TickPresented::Frame {
                output,
                hit_test_list_changed,
            };
        });
        self
    }
}

/// The marker of the host entry the event loop calls as a task begins, which begins a clock lease.
pub(crate) struct LeasesClockForTask {
    _private: (),
}

pub(super) const LEASES_CLOCK_FOR_TASK: LeasesClockForTask = LeasesClockForTask { _private: () };

/// Hands the clock lease `ticks` belong to a display tick at `frame_time_nanoseconds`, at which the compositor had
/// scrolled to the `scroll_offset_count` offsets at `scroll_offsets`, and answers whether it wants the next one.
///
/// # Safety
///
/// `ticks` must come from `document_host_lease_clock` and not be released yet, and `scroll_offsets` must hold
/// `scroll_offset_count` offsets.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn clock_ticks_tick(
    ticks: *const ClockTicks,
    frame_time_nanoseconds: i64,
    scroll_offsets: *const FfiScrollOffset,
    scroll_offset_count: usize,
) -> bool {
    // SAFETY: Guaranteed by the caller, whose reference this borrows.
    let ticks = std::mem::ManuallyDrop::new(unsafe { Arc::from_raw(ticks) });
    // SAFETY: Guaranteed by the caller.
    let scroll_offsets = unsafe { crate::css::custom_properties::ffi_slice(scroll_offsets, scroll_offset_count) };
    ticks.tick(frame_time_nanoseconds, scroll_offsets)
}

/// Gives up the reference `ticks` holds.
///
/// # Safety
///
/// `ticks` must come from `document_host_lease_clock`, and be released once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn clock_ticks_release(ticks: *const ClockTicks) {
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { Arc::from_raw(ticks) });
}

/// Whether the last rendering update left `host`'s document a plan for a clock lease no task has taken yet.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_has_clock_plan(host: &DocumentHost) -> bool {
    host.has_clock_plan()
}

/// Whether the plan for a clock lease of `host`'s document that no task has taken yet samples running animations, rather
/// than only following the pointer.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_clock_plan_animates(host: &DocumentHost) -> bool {
    host.clock_plan_animates()
}

/// Drops the plan for a clock lease of `host`'s document, for a rendering update that leaves it none. The plan is the
/// host's, so this reads nothing of the render state.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_drop_clock_plan(host: &DocumentHost) {
    host.seal_clock_plan(None);
}

/// Leases the render state of `host`'s document to the render clock as a task begins, with the recorder state and the
/// presentation `presentation` names, where the last rendering update left a plan for it, no frame flies and the
/// recorder state is here: the lease takes the presentation, and nulls it there. Answers the ticks the render clock
/// hands the lease, which the caller releases with `clock_ticks_release`, or null where no lease began, leaving the
/// presentation with the caller.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread, as a task of its
/// event loop begins. `presentation` must be valid for reads and writes, and name a presentation the caller gives up
/// where the lease takes it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_lease_clock(
    host: &DocumentHost,
    presentation: *mut FfiPresentation,
    holds_animations: bool,
) -> *const ClockTicks {
    // SAFETY: Guaranteed by the caller.
    let presentation = unsafe { &mut *presentation };
    let start = TaskStart::at_event_loop_entry(&LEASES_CLOCK_FOR_TASK);
    // SAFETY: Guaranteed by the caller.
    let Some(taken) = (unsafe { Presentation::take(presentation) }) else {
        return std::ptr::null();
    };
    match host.lease_clock(&start, taken, holds_animations) {
        Ok(ticks) => Arc::into_raw(ticks),
        Err(given_back) => {
            *presentation = given_back.into_ffi();
            std::ptr::null()
        }
    }
}

/// Leases the render state of `host`'s document to the render clock again as a task begins, where the clock lease the
/// host ended last beside the tasks moved and presented nothing, and the host wrote nothing since: the lease follows the
/// pointer with what that lease ended with. Answers the ticks the render clock hands the lease, which the caller
/// releases with `clock_ticks_release`, or null where no lease began.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread, as a task of its
/// event loop begins.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_resume_clock_lease(host: &DocumentHost) -> *const ClockTicks {
    let start = TaskStart::at_event_loop_entry(&LEASES_CLOCK_FOR_TASK);
    host.resume_clock_lease(&start).map_or(std::ptr::null(), Arc::into_raw)
}

/// Ends the clock lease of `host`'s document, where one runs, for the presenter of its navigable, and writes the
/// presentation a lease brought back to `presentation`, or nulls where none did.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread, and
/// `presentation` must be valid for writes; the caller takes over what it names.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_take_back_presentation(host: &DocumentHost, presentation: *mut FfiPresentation) {
    let given_back = host
        .take_back_presentation()
        .map_or_else(FfiPresentation::default, Presentation::into_ffi);
    // SAFETY: Guaranteed by the caller.
    unsafe { presentation.write(given_back) };
}

/// Ends the clock lease of `host`'s document, where one runs, as script changes an animation of the document, and drops
/// the plan for the next one, which no longer stands.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_end_clock_lease_for_animation(host: &DocumentHost) {
    host.end_clock_lease_and_plan();
}

/// Ends the clock lease of `host`'s document, where one runs, as a rendering update begins at `frame_time_nanoseconds`,
/// and answers whether a tick of a clock lease presented a frame of the document since the last rendering update. A
/// pointer move the lease's next tick would hover hovers first. The update keeps the hover a lease moved, which owes the
/// boundary events of the move until the host handles a mouse move.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_end_clock_lease_for_rendering_update(
    host: &DocumentHost,
    frame_time_nanoseconds: i64,
) -> bool {
    host.end_clock_lease_for_rendering_update(frame_time_nanoseconds)
}

/// Notes whether the event loop of `host`'s document runs a task as it leases the document's render state to the render
/// clock, or goes idle: a lease's hover follows the pointer only beside a task.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_note_event_loop_runs_task(host: &DocumentHost, runs_task: bool) {
    host.note_event_loop_runs_task(runs_task);
}

/// Whether the pointer moved over `host`'s document lately, which a hover beside the host's tasks follows: a task waits
/// for the frame in flight to finish recording before it begins, so that a lease ticks beside it.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_pointer_is_active(host: &DocumentHost) -> bool {
    host.pointer_is_active()
}

/// Whether a clock lease of `host`'s document that ticked on a fork of its render state presented a frame since the
/// host last asked: the screen shows the fork's frame, in place of the host's, until the host presents again.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_take_fork_presented(host: &DocumentHost) -> bool {
    host.take_fork_presented()
}

/// Takes the boundary events a hover kept by a rendering update owes, where the host handled no mouse move since, and
/// answers whether it owes any: `has_pointer` is set with `pointer_x`, `pointer_y` where the pointer was, in the
/// context's device pixels, and not where it left the context.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread, and the pointers
/// must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_take_hover_events_owed(
    host: &DocumentHost,
    has_pointer: &mut bool,
    pointer_x: &mut f32,
    pointer_y: &mut f32,
) -> bool {
    let Some(pointer) = host.take_hover_events_owed() else {
        return false;
    };
    *has_pointer = pointer.is_some();
    if let Some(pointer) = pointer {
        *pointer_x = pointer.x;
        *pointer_y = pointer.y;
    }
    true
}

/// Whether the render clock holds a lease of `host`'s document, or the event loop may give it one: from the plan the
/// last rendering update left, or by taking up the lease the host ended last again.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_may_lease_clock(host: &DocumentHost) -> bool {
    host.may_lease_clock()
}

/// Whether the render clock holds a lease of `host`'s document.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_clock_lease_runs(host: &DocumentHost) -> bool {
    host.clock_ticks().is_some()
}

/// Lets the clock lease of `host`'s document sample the animations it held while the event loop was idle, as a task
/// begins. Answers its ticks where it should tick at once, which the caller releases with `clock_ticks_release`, or
/// null.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_run_clock_animations(host: &DocumentHost) -> *const ClockTicks {
    host.clock_ticks()
        .filter(|ticks| ticks.run_animations())
        .map_or(std::ptr::null(), Arc::into_raw)
}

/// Whether a pointer move waits for a tick of the clock lease `ticks` belong to to hover it, which the render clock
/// ticks for at once.
///
/// # Safety
///
/// `ticks` must come from `document_host_lease_clock` and not be released yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn clock_ticks_pointer_waits(ticks: *const ClockTicks) -> bool {
    // SAFETY: Guaranteed by the caller, whose reference this borrows.
    let ticks = std::mem::ManuallyDrop::new(unsafe { Arc::from_raw(ticks) });
    ticks.pointer_state().waits()
}

/// What a document's clock lease is doing.
#[repr(u8)]
pub enum FfiClockLeaseState {
    /// No lease runs.
    None,
    Ticking,
    /// A tick parked the lease, which samples nothing more.
    Parked,
    /// The lease samples no animation, and only follows the pointer.
    Hovering,
}

/// What the clock lease of `host`'s document is doing.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_clock_lease_state(host: &DocumentHost) -> FfiClockLeaseState {
    match host.clock_ticks() {
        None => FfiClockLeaseState::None,
        Some(ticks) if !ticks.animates => FfiClockLeaseState::Hovering,
        Some(ticks) if ticks.is_parked() => FfiClockLeaseState::Parked,
        Some(_) => FfiClockLeaseState::Ticking,
    }
}

/// Hands the clock lease of `host`'s document, where one runs, a tick at `frame_time_nanoseconds`, at which the
/// compositor had scrolled to `scroll_offset`, if not null, and waits until the StyleLayout thread has run the jobs
/// handed to it before, the tick among them, and the Paint thread the recording of the frame the tick presents, without
/// ending the lease. For a test, whose clock ticks only where it injects them.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread, and
/// `scroll_offset` must be null or valid for reads.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_inject_clock_tick(
    host: &DocumentHost,
    frame_time_nanoseconds: i64,
    scroll_offset: *const FfiScrollOffset,
) {
    if let Some(ticks) = host.clock_ticks() {
        // SAFETY: Guaranteed by the caller.
        let scroll_offsets = unsafe { scroll_offset.as_ref() }.map_or(&[][..], std::slice::from_ref);
        ticks.tick(frame_time_nanoseconds, scroll_offsets);
        crate::stage_thread::style_layout_thread().run(|| ());
        crate::paint_stage::paint_thread().run(|| ());
    }
}

/// Writes the color the box of `element` showed in the last frame a tick of a clock lease of `host`'s document
/// presented to `color`, as `0xAARRGGBB`, ending the lease that runs, and answers whether a hover's transitions
/// restyled the box in it. For a test.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread, and `color` must be
/// valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_presented_color(host: &DocumentHost, element: u32, color: *mut u32) -> bool {
    let Some(presented) = StyleNodeID::from_raw(element).and_then(|element| host.presented_color(element)) else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { color.write(presented) };
    true
}

/// Writes the border box of `element` in the last frame a tick of a clock lease of `host`'s document presented to
/// `rect`, ending the lease that runs, and answers whether a tick presented one. For a test.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread, and `rect` must be
/// valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_presented_border_box(
    host: &DocumentHost,
    element: u32,
    rect: *mut FfiCssPixelRect,
) -> bool {
    let Some(presented) = StyleNodeID::from_raw(element).and_then(|element| host.presented_border_box(element)) else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { rect.write(presented.into()) };
    true
}

/// What a document's host and its clock leases tell each other of the pointer and the event loop.
pub(crate) struct HostActivity {
    epoch: std::time::Instant,
    /// How long after `epoch` the pointer last moved over the document, in nanoseconds, or 0 where it never moved.
    pointer_moved_at: AtomicU64,
    /// Whether the event loop runs a task, beside which a lease's hover follows the pointer.
    runs_task: AtomicBool,
}

impl Default for HostActivity {
    fn default() -> Self {
        Self {
            epoch: std::time::Instant::now(),
            pointer_moved_at: AtomicU64::new(0),
            runs_task: AtomicBool::new(false),
        }
    }
}

impl HostActivity {
    /// How long after the pointer last moved over the document a lease forks the render state as it begins.
    const POINTER_ACTIVITY: std::time::Duration = std::time::Duration::from_secs(2);

    fn now(&self) -> u64 {
        u64::try_from(self.epoch.elapsed().as_nanos())
            .unwrap_or(u64::MAX)
            .max(1)
    }

    /// Notes that the pointer moved over the document now.
    fn note_pointer_move(&self) {
        self.pointer_moved_at.store(self.now(), Ordering::Relaxed);
    }

    /// Whether the pointer moved over the document lately. See [`Self::POINTER_ACTIVITY`].
    pub(crate) fn pointer_is_active(&self) -> bool {
        let moved_at = self.pointer_moved_at.load(Ordering::Relaxed);
        moved_at != 0 && u128::from(self.now().saturating_sub(moved_at)) < Self::POINTER_ACTIVITY.as_nanos()
    }

    /// Notes whether the event loop runs a task.
    pub(crate) fn set_runs_task(&self, runs_task: bool) {
        self.runs_task.store(runs_task, Ordering::Relaxed);
    }

    fn runs_task(&self) -> bool {
        self.runs_task.load(Ordering::Relaxed)
    }
}
