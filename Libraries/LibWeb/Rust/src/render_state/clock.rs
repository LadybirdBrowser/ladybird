/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The render clock's lanes of a document's presented frames.
//!
//! Each frame of a document the host presents has a lane, which the render clock's ticks follow on the StyleLayout
//! thread, as the render owner, from the frame on, until the lane of the host's next frame takes its place: a tick
//! hovers what is under the pointer, for real, as the host would as it handles the move (see [`hover`]), and samples
//! the running animations of the plan the frame's rendering update sealed at the tick's time beside a task, shows the
//! samples in their elements' boxes, lays out what they moved, and has the Paint thread record and present the frame.
//! A lane's ticks write a fork of the render state as the frame left it (see [`crate::fork`]), which the host's state
//! never sees, and record with a fork of the recorder state the frame recorded with: nothing on the host's thread
//! begins, waits for or ends a lane. What a tick presents starts from the lane's frame, and the presenter drops it once
//! the host presented a later frame.
//!
//! The lane's pieces come together on the render owner (see [`lane`]): the fork, before the host first writes the state
//! after the frame or at the first tick that needs it, the recorder state and the seal its frames are presented with,
//! from the Paint thread once it presented the frame, and the plan, from the end of the frame's rendering update.
//!
//! The types keep the rules: a lane ticks only with its fork in hand ([`Lane::tick_on_fork`]), and only the lane of the
//! frame sampled last forks, while the state still shows that frame ([`LaneState`]); a document has no lane, the lane
//! of the frame sampled last, or that lane's pieces as they come beside an earlier lane, never more; and what the
//! render clock and the host read of the lanes is one publication the owner writes after each job on them
//! ([`LanePublication`]).

use super::owner::DocumentId;
use super::{DocumentHost, RenderState};
use crate::css::css_pixels::CssPixelRect;
use crate::css::style::animations::{AnimationTimelineSamples, ScrollProgress};
use crate::css::style::engine_sample::{DependentRestyle, NeedsHost, TickShownRecords};
use crate::css::style::tree::StyleNodeID;
use crate::layout::node_data::NodeSlotId;
use crate::layout::tree_update_marks::{FfiLayoutTreeUpdateMark, layout_tree_update_reuse_reason};
use crate::layout::used_values::FfiCssPixelRect;
use crate::layout::{ClockRound, ClockRoundDeclined, HostStyle, LayoutNodeArena, build_keeps_box};
use crate::paint_stage::Presenting;
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
use crate::stage_thread::{Relay, Riding};
use smallvec::SmallVec;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

mod effects;
pub(crate) mod hover;
mod lane;
pub(crate) use hover::PendingPointer;
pub(crate) use lane::LaneDelivery;
pub(super) use lane::{LaneSlot, note_host_write, seal_plan, take_lanes_in};

/// What the ticks of a lane sample, sealed at the end of the lane's frame's rendering update.
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
    /// What a tick hovers the element under the pointer with, where the lane follows the pointer.
    hover: Option<hover::HoverPlan>,
    /// How many times script had changed the document's animations as the host sealed the plan: once it changes them
    /// again, the plan's animations no longer stand.
    pub(super) animation_changes: u64,
}

/// A scroll timeline whose animations the ticks of a lane sample, as a rendering update sealed it.
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
            animation_changes: 0,
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
}

/// The lane of a frame of a document the host presented, on the render owner: the fork of the render state its ticks
/// write, the recording of the frame its last tick presented, which brings back the recorder state and presentation it
/// records and presents with, and what its ticks showed.
pub(crate) struct Lane {
    recording: TickRecording,
    plan: ClockPlan,
    /// What the ticks write.
    state: LaneState,
    /// What the ticks sample of each element: the animations the plan names, and the transitions the hover started.
    effects: Vec<effects::ElementEffects>,
    /// The boxes the ticks showed samples in, with the styles the frame showed in them.
    ticked: Vec<(NodeSlotId, HostStyle)>,
    /// The records the boxes the ticks built again show, which the engine keeps for them.
    shown: TickShownRecords,
    /// The border boxes of the plan's elements, and of those the hover restyled, in the last frame a tick presented.
    presented_border_boxes: Vec<(StyleNodeID, CssPixelRect)>,
    /// The color each box the ticks showed samples in showed in that frame, as `0xAARRGGBB`. For a test.
    presented_colors: Vec<(StyleNodeID, u32)>,
    /// Where the compositor had scrolled to at the latest tick that said so, which the plan's scroll timelines follow,
    /// and the hit tests of its hover.
    scroll_offsets: Vec<FfiScrollOffset>,
    /// The timestamp at which the last frame presented shows the animations of the document timeline, or negative
    /// infinity for the lane's frame, and the progress at which it shows the plan's scroll timelines.
    shown_at: f64,
    shown_scroll_progress: SmallVec<[ScrollProgress; 2]>,
    /// Whether a tick found the lane could sample no more: past the deadline or the animations' ends, or something only
    /// the host computes.
    parked: bool,
    /// Whether a tick left its frame to the host, after which the fork may hold what no frame the lane presented shows,
    /// such as visual contexts of another structure than the compositor has: the lane presents no other frame.
    frame_left_to_host: bool,
    /// What the lane's hover moved.
    hovered: hover::LaneHover,
}

/// What the ticks of a lane write: the render state of the render owner while it still shows the lane's frame, which
/// the lane forks, the lane's fork of it, or nothing, where the state moved on before the lane forked it. Only a lane
/// with a fork ticks (see [`Lane::tick_on_fork`]).
#[derive(Default)]
pub(crate) enum LaneState {
    #[default]
    Shown,
    Forked(super::RenderFork),
    MovedOn,
}

impl LaneState {
    /// Forks the render state of `document`, where it still shows the lane's frame.
    pub(super) fn fork(&mut self, document: DocumentId) {
        if matches!(self, Self::Shown) {
            *self = Self::Forked(super::owner::with_state(document, None, |state| state.fork()));
        }
    }

    /// Notes that the render state no longer shows the lane's frame, where the lane did not fork it.
    pub(super) fn move_on(&mut self) {
        if matches!(self, Self::Shown) {
            *self = Self::MovedOn;
        }
    }
}

/// What a tick of a lane did: the pointer move its hover took, if it took one, and whether it presented a frame.
struct Ticked {
    hover_move: Option<LaneMove>,
    presented: bool,
}

/// A pointer move a tick of a lane took, which the host takes in.
#[derive(Clone, Copy)]
pub(crate) struct LaneMove {
    pub(crate) input_event_id: u64,
    /// The element the hover is on after the move, or nothing; none where the lane left the move to the host, which
    /// hovers what is under the pointer itself.
    pub(crate) target: Option<Option<StyleNodeID>>,
}

// A lane is assembled from pieces made on the Paint thread and the host's.
const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<ClockRecorder>();
    assert_send::<ClockPlan>();
};

/// The recorder state and presentation of a lane, as the recording of a tick's frame gives them back, with what it
/// presented.
pub(crate) struct ClockRecorder {
    pub(super) recorder: RecorderState,
    pub(super) presentation: Presentation,
    pub(super) presented: TickPresented,
}

/// What the recording of a tick's frame presented.
pub(crate) enum TickPresented {
    /// No tick recorded a frame since the recorder state came back.
    Nothing,
    /// The frame, which the fork takes in as its last recording.
    Frame {
        output: Arc<RecordingOutput>,
        hit_test_list_changed: bool,
    },
    /// Nothing: the frame renders an SVG image, which only the host renders, and the lane samples no more.
    LeftToHost,
}

/// The recording of the frame the last tick of a lane presented, which may still run on the Paint thread, and which
/// brings back the recorder state and presentation the lane records and presents with. A tick waits for it once it
/// has laid out the next frame.
pub(crate) struct TickRecording(Riding<ClockRecorder>);

/// The right to wait for the recording of a tick's frame, which only a tick that records the next frame mints, and a
/// lane as it goes.
pub(crate) struct WaitsForTickRecording(());

impl crate::stage_thread::Flown for ClockRecorder {
    type JoinRight = WaitsForTickRecording;
}

/// The right to sample a frame of a document for the Paint thread to present. Only a tick of the render clock and the
/// commit of a rendering update mint one, in the job the StyleLayout thread runs them in, so a job the host hands the
/// render owner to ask or write its render state cannot sample a frame.
pub(crate) struct SamplingTurn(());

/// The timestamp a frame shows a document's animations at, which is never earlier than that of a frame sampled before
/// it: only [`SampleClock::next`] mints one.
#[derive(Clone, Copy)]
pub(crate) struct SampleTime(f64);

/// What hands out the times a document's frames are sampled at, on the render owner, across clock lanes.
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

    pub(crate) fn get_mut(&mut self) -> &mut T {
        &mut self.0
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
    /// records and presents it beside the host, and starts its lane, whose ticks `ticks` hands it, once it presented
    /// it. The frames the render clock samples after it sample no earlier.
    pub(super) fn sample(self, state: &mut RenderState, relay: Relay<RecordingAnswer>, ticks: &Arc<ClockTicks>) {
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
        let lane = lane::note_sampled(ticks);
        crate::paint_stage::relay_presenting(relay, frame, move |frame, presenting| {
            if held_for_testing {
                wait_while_recording_is_held_for_testing();
            }
            let mut answer = frame.present_committed(recorder, presentation, presenting);
            answer.start_clock_lane(lane);
            answer
        });
    }
}

/// What the lane of a document's presented frame does, as the host reads it.
#[repr(u8)]
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub enum FfiClockLaneState {
    /// No lane follows the presented frame.
    #[default]
    None,
    Ticking,
    /// A tick parked the lane, which samples nothing more.
    Parked,
    /// The lane samples no animation, and only follows the pointer.
    Hovering,
}

/// The display ticks and pointer moves the render clock hands a document's lanes, from the document's first frame on:
/// a tick of the lane that follows the presented frame takes them on the render owner, folded into one queued tick,
/// which runs at the latest of their times. The document's host makes them, and the render clock holds them as long as
/// it hands them ticks.
pub struct ClockTicks {
    document: DocumentId,
    /// The latest frame time of the ticks handed, in nanoseconds of the monotonic clock. It only grows, so a tick handed
    /// an earlier time than one before it, as a display tick behind the immediate first one is, never samples the
    /// animations back in time.
    latest: AtomicI64,
    /// Where the compositor had scrolled to at the latest tick handed, until the tick that runs next takes it.
    scroll_offsets: Mutex<Option<Vec<FfiScrollOffset>>>,
    /// Whether a tick is queued.
    queued: AtomicBool,
    /// What the lanes do, as the render owner published it last.
    published: Mutex<LanePublication>,
    /// Whether the lanes sample no animations until a task begins: an idle event loop's rendering updates run them.
    animations_held: AtomicBool,
    /// Whether the plan the host sealed last samples running animations.
    plan_animates: AtomicBool,
    /// How many times script changed the document's animations, after which the animations a plan sealed before no
    /// longer stand.
    animation_changes: AtomicU64,
    /// How many recordings of the host's own add to the resource storage the lanes present with: none presents
    /// meanwhile.
    holds: AtomicU32,
    /// Where the render clock heard the pointer went, until a tick hovers what is there.
    pointer: Mutex<hover::PointerState>,
    /// The border boxes of the elements the lane sampled or its hover restyled in the last frame a tick presented, and
    /// the colors the boxes its hover's transitions restyled showed. For a test.
    presented_boxes: Mutex<PresentedBoxes>,
}

/// The border boxes and colors of the last frame a tick of a lane presented. See [`ClockTicks::presented_boxes`].
#[derive(Default)]
struct PresentedBoxes {
    border_boxes: Vec<(StyleNodeID, CssPixelRect)>,
    colors: Vec<(StyleNodeID, u32)>,
}

/// What the lanes of a document do, as the render owner publishes it after every job on them: the render clock reads it
/// to decide whether to tick, and the host as a rendering update begins. Only the owner's lane slot writes it.
#[derive(Default)]
pub(crate) struct LanePublication {
    /// What the lane that follows the presented frame does.
    pub(super) state: FfiClockLaneState,
    /// How many times script had changed the document's animations as the host sealed that lane's plan.
    pub(super) planned_animation_changes: u64,
    /// Whether a transition the lane's hover started has yet to end, which the ticks sample until it does.
    pub(super) transitions_run: bool,
    /// Whether the lane's hover follows the pointer: it has a plan that does, and left no move to the host.
    pub(super) follows_pointer: bool,
    /// Whether the owner sampled a frame whose lane has yet to come together.
    pub(super) awaits_lane: bool,
}

/// Transitions the hover of a lane started on an element.
#[derive(Clone)]
pub(crate) struct LaneTransitionStart {
    pub(crate) node: StyleNodeID,
    /// The properties the transitions animate.
    pub(crate) properties: SmallVec<[u16; 2]>,
    /// When the hover started them, in the document's milliseconds.
    pub(crate) start_time: f64,
}

impl LaneTransitionStart {
    /// Whether `other` reports the same start.
    fn same_start_as(&self, other: &Self) -> bool {
        self.node == other.node && self.start_time == other.start_time
    }
}

/// The transitions the hover of the lane that follows the presented frame started, as the host takes in what the lanes
/// report: those the host's own hover starts of the same properties of the same elements run from then, and those it
/// started its own of stay its own however long the lane still reports them.
#[derive(Default)]
pub(crate) struct LaneTransitionStarts {
    /// The starts the host has yet to start its own transitions of, each with whether a transition the host starts
    /// runs from it since the host last forgot the ones it started.
    pending: Vec<(LaneTransitionStart, bool)>,
    /// The starts the lane still reports that the host started its own transitions of.
    taken: Vec<LaneTransitionStart>,
}

impl LaneTransitionStarts {
    /// Takes in `reported`, the starts the lanes report now, in place of those they reported before.
    pub(crate) fn take_in(&mut self, reported: Vec<LaneTransitionStart>) {
        self.taken
            .retain(|taken| reported.iter().any(|start| start.same_start_as(taken)));
        self.pending = reported
            .into_iter()
            .filter(|start| !self.taken.iter().any(|taken| taken.same_start_as(start)))
            .map(|start| (start, false))
            .collect();
    }

    /// When the hover started a transition of `property` of the element `node` names that the host has yet to start its
    /// own of, which the host's transition runs from.
    pub(crate) fn start_of(&mut self, node: StyleNodeID, property: u16) -> Option<f64> {
        let (start, runs_a_host_transition) = self
            .pending
            .iter_mut()
            .find(|(start, _)| start.node == node && start.properties.contains(&property))?;
        *runs_a_host_transition = true;
        Some(start.start_time)
    }

    /// Notes that the host started its own transitions of the starts its transitions ran from.
    pub(crate) fn forget(&mut self) {
        let (taken, pending): (Vec<_>, Vec<_>) = std::mem::take(&mut self.pending)
            .into_iter()
            .partition(|(_, runs_a_host_transition)| *runs_a_host_transition);
        self.pending = pending;
        self.taken.extend(taken.into_iter().map(|(start, _)| start));
    }
}

/// What the lanes did since a rendering update last took them in, as the next one takes them in.
#[derive(Clone, Default)]
pub(crate) struct LaneReport {
    /// The last pointer move a tick took.
    pub(crate) last_move: Option<LaneMove>,
    /// The transitions the hover of the lane that follows the presented frame started.
    pub(crate) transition_starts: Vec<LaneTransitionStart>,
    /// Whether a tick presented a frame, which the screen shows in place of the host's.
    pub(crate) presented: bool,
}

impl ClockTicks {
    pub(super) fn new(document: DocumentId) -> Arc<Self> {
        Arc::new(Self {
            document,
            latest: AtomicI64::new(i64::MIN),
            scroll_offsets: Mutex::default(),
            queued: AtomicBool::new(false),
            published: Mutex::default(),
            animations_held: AtomicBool::new(true),
            plan_animates: AtomicBool::new(false),
            animation_changes: AtomicU64::new(0),
            holds: AtomicU32::new(0),
            pointer: Mutex::new(hover::PointerState::default()),
            presented_boxes: Mutex::default(),
        })
    }

    /// Hands the lane a tick at `frame_time_nanoseconds`, at which the compositor had scrolled to `scroll_offsets`: the
    /// tick already queued runs at the latest time and offsets, or a tick is queued. Answers whether the lane wants the
    /// next tick, which it does while it samples animations or transitions, or while a pointer move waits for a tick to
    /// hover it.
    pub(super) fn tick(self: &Arc<Self>, frame_time_nanoseconds: i64, scroll_offsets: &[FfiScrollOffset]) -> bool {
        let (state, transitions_run, awaits_lane) = {
            let published = self.published();
            (
                self.lane_state_in(&published),
                published.transitions_run,
                published.awaits_lane,
            )
        };
        // The lane of a frame whose plan samples animations may still come together, and ticks them then.
        let animations_run = !self.animations_held.load(Ordering::Relaxed);
        let awaits_animating_lane = awaits_lane && self.plan_animates();
        if state == FfiClockLaneState::None && !(animations_run && awaits_animating_lane) {
            return false;
        }
        // A tick that runs nothing still says where the compositor scrolled to, which a pointer move it hovers later
        // is hit tested at.
        if !scroll_offsets.is_empty() {
            *self.scroll_offsets.lock().expect("clock tick scroll offsets") = Some(scroll_offsets.to_vec());
        }
        let animates = animations_run && (state == FfiClockLaneState::Ticking || awaits_animating_lane);
        if !animates && !transitions_run && !self.pointer_waits() {
            return false;
        }
        self.latest.fetch_max(frame_time_nanoseconds, Ordering::AcqRel);
        if self.queued.swap(true, Ordering::AcqRel) {
            return true;
        }
        let ticks = Arc::clone(self);
        super::post_to_render_side(move || lane::tick(&ticks));
        true
    }

    /// Hands the lane where the pointer went, which the next tick hovers, and answers what the lane wants next.
    pub(super) fn pointer_moved(&self, pointer: PendingPointer) -> hover::PointerAnswer {
        let follows = self.published().follows_pointer;
        self.pointer_state().moved(pointer, follows)
    }

    /// Where the compositor said the pointer went last, beside the mouse event the host takes, if it said so.
    pub(super) fn newest_pointer(&self) -> Option<PendingPointer> {
        self.pointer_state().last()
    }

    /// Whether a pointer move waits for a tick to hover it.
    pub(super) fn pointer_waits(&self) -> bool {
        self.pointer_state().waits()
    }

    /// Where the compositor said the pointer went last, and whether that move waits for a tick to hover it, as one.
    pub(super) fn newest_pointer_and_waits(&self) -> (Option<PendingPointer>, bool) {
        let pointer = self.pointer_state();
        (pointer.last(), pointer.waits())
    }

    /// Notes that the host handled the mouse event with the input event id `input_event_id`: no tick hovers an older
    /// move.
    pub(super) fn note_input_handled(&self, input_event_id: u64) {
        self.pointer_state().note_handled(input_event_id);
    }

    /// Hands the lane a pointer move, and ticks it at `frame_time_nanoseconds`. For a test.
    pub(super) fn inject_pointer(self: &Arc<Self>, pointer: PendingPointer, frame_time_nanoseconds: i64) {
        let _ = self.pointer_moved(pointer);
        self.tick(frame_time_nanoseconds, &[]);
    }

    fn pointer_state(&self) -> std::sync::MutexGuard<'_, hover::PointerState> {
        self.pointer.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    /// What the lanes do, as the render owner published it last.
    pub(super) fn published(&self) -> std::sync::MutexGuard<'_, LanePublication> {
        self.published.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
    }

    /// What the lane that follows the presented frame does.
    pub(super) fn lane_state(&self) -> FfiClockLaneState {
        self.lane_state_in(&self.published())
    }

    fn lane_state_in(&self, published: &LanePublication) -> FfiClockLaneState {
        match published.state {
            // Script changed the animations the lane samples since, which no longer stand.
            FfiClockLaneState::Ticking if published.planned_animation_changes != self.animation_changes() => {
                FfiClockLaneState::Parked
            }
            state => state,
        }
    }

    /// Has the lanes sample no animations until a task begins, or lets them, as the event loop goes idle or begins one.
    pub(super) fn hold_animations(&self, held: bool) {
        self.animations_held.store(held, Ordering::Relaxed);
    }

    /// Notes whether the plan the host sealed last samples running animations.
    pub(super) fn note_plan_animates(&self, animates: bool) {
        self.plan_animates.store(animates, Ordering::Relaxed);
    }

    pub(super) fn plan_animates(&self) -> bool {
        self.plan_animates.load(Ordering::Relaxed)
    }

    /// Whether a lane wants a fork of the render state as the frame sampled last left it, before the host writes it in a
    /// task: one hovers and samples the plan's animations beside the task. Beside an idle event loop, the host renders
    /// both itself.
    fn wants_fork(&self) -> bool {
        !self.animations_held.load(Ordering::Relaxed)
    }

    /// Notes that script changed the document's animations: those the lanes sample no longer stand.
    pub(super) fn note_animation_change(&self) {
        self.animation_changes.fetch_add(1, Ordering::Relaxed);
    }

    /// How many times script changed the document's animations so far.
    pub(super) fn animation_changes(&self) -> u64 {
        self.animation_changes.load(Ordering::Relaxed)
    }

    /// Holds the lanes for a recording of the host's own that adds to the resource storage they present with, waiting
    /// for what they present meanwhile: they present nothing until [`Self::release`].
    pub(super) fn hold(&self) {
        self.holds.fetch_add(1, Ordering::AcqRel);
        if cfg!(test) {
            return;
        }
        crate::stage_thread::style_layout_thread().run(|| ());
        crate::paint_stage::paint_thread().run(|| ());
    }

    pub(super) fn release(&self) {
        let previous = self.holds.fetch_sub(1, Ordering::AcqRel);
        debug_assert!(previous > 0, "a hold of the lanes is released once");
    }

    fn is_held(&self) -> bool {
        self.holds.load(Ordering::Acquire) > 0
    }

    /// The border box of `element` in the last frame a tick presented, if it presented one.
    pub(super) fn presented_border_box(&self, element: StyleNodeID) -> Option<CssPixelRect> {
        let presented = self.presented_boxes.lock().expect("presented boxes");
        presented
            .border_boxes
            .iter()
            .find_map(|&(presented, rect)| (presented == element).then_some(rect))
    }

    /// The color the box of `element` showed in the last frame a tick presented, where a tick showed a sample in it.
    pub(super) fn presented_color(&self, element: StyleNodeID) -> Option<u32> {
        let presented = self.presented_boxes.lock().expect("presented boxes");
        presented
            .colors
            .iter()
            .find_map(|&(presented, color)| (presented == element).then_some(color))
    }
}

/// What parks a lane: something of a tick only the host computes, and what, for a developer.
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
            if let Some(host_style) = arena.install_sample(row, restyled, crate::layout::SampleKind::Animation)? {
                ticked.push((row, host_style));
            }
            arena.push_paint_damage_for_repaint(row, PaintDamage::ALL_PRODUCERS);
        }
    }
    for (node, rebuild) in rebuilt {
        mark_for_tree_build(state, node, rebuild, animated, ticked)?;
    }
    Ok(())
}

/// Marks what `rebuild` builds again for `node` for the round's tree build, as the host marks it for a style change,
/// from what the clock shows. The build keeps every box a sample shows in: the box whose pseudo-elements it
/// regenerates, and the parent box it inserts a box into or takes one out of, in place. Where it cannot, as where an
/// anonymous box wraps a neighbor, the build builds the parent's box again with every box below it (see
/// [`mark_region_for_tree_build`]).
fn mark_for_tree_build(
    state: &mut RenderState,
    node: StyleNodeID,
    rebuild: TickRebuild,
    animated: &[StyleNodeID],
    ticked: &mut Vec<(NodeSlotId, HostStyle)>,
) -> Result<(), Park> {
    use layout_tree_update_reuse_reason::{CHILD_LIST_INSERTION, PSEUDO_ELEMENT_CHANGE};
    let arena = state.arena.arena_mut();
    let in_place = match rebuild {
        TickRebuild::PseudoElements => {
            let in_place = build_keeps_box(arena, node, PSEUDO_ELEMENT_CHANGE);
            if in_place {
                arena.mark_layout_tree_update(Some(node), style_change_mark(PSEUDO_ELEMENT_CHANGE));
            }
            in_place
        }
        TickRebuild::Insert { parent } => {
            // The build reads which children of the parent it inserts boxes for from their marks.
            arena.mark_layout_tree_update(Some(node), FfiLayoutTreeUpdateMark::NODE_INSERT);
            let in_place =
                !arena.bound_row(parent).is_invalid() && build_keeps_box(arena, parent, CHILD_LIST_INSERTION);
            if in_place {
                arena.mark_layout_tree_update(Some(parent), FfiLayoutTreeUpdateMark::NODE_INSERT);
            }
            in_place
        }
        TickRebuild::TakeAway { parent } => {
            let in_place = arena.can_take_box_away_in_place(parent, node);
            if in_place {
                for (box_, host_style) in sampled_boxes_in(arena, arena.bound_row(node), animated, ticked)? {
                    arena.restore_host_style(box_, host_style);
                }
                arena.mark_layout_tree_update(Some(node), style_change_mark(0));
            }
            in_place
        }
        TickRebuild::Again => false,
    };
    if in_place {
        return Ok(());
    }
    let root = match rebuild {
        TickRebuild::PseudoElements | TickRebuild::Again => node,
        TickRebuild::Insert { parent } | TickRebuild::TakeAway { parent } => parent,
    };
    mark_region_for_tree_build(state, root, animated, ticked)
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

impl Drop for Lane {
    /// The recording of the frame the last tick presented reads the records the ticks sampled, which go with the fork:
    /// the lane goes once the recording is done.
    fn drop(&mut self) {
        let _ = self.recording.0.take(WaitsForTickRecording(()));
    }
}

impl Lane {
    /// A lane whose ticks record and present with `recorder`, follow `plan`, and write what `state` says.
    fn new(recorder: ClockRecorder, plan: ClockPlan, state: LaneState) -> Self {
        let shown_scroll_progress = plan.scroll_progress(&[]).unwrap_or_default();
        let parked = !plan.animates();
        Self {
            recording: TickRecording(Riding::landed(recorder)),
            effects: Self::host_effects(&plan.elements),
            plan,
            state,
            ticked: Vec::new(),
            shown: TickShownRecords::default(),
            presented_border_boxes: Vec::new(),
            presented_colors: Vec::new(),
            scroll_offsets: Vec::new(),
            shown_at: f64::NEG_INFINITY,
            shown_scroll_progress,
            parked,
            frame_left_to_host: false,
            hovered: hover::LaneHover::default(),
        }
    }

    /// Whether no tick of the lane showed a sample in a box, nor did its hover start a transition: its fork shows the
    /// frame it was presented with, and the hovers the host took in.
    fn samples_nothing(&self) -> bool {
        self.ticked.is_empty() && self.effects.iter().all(effects::ElementEffects::is_host)
    }

    /// Has the lane follow no plan, as a rendering update that sealed none leaves it: it ticks nothing until the plan of
    /// a later update that presents no frame of its own.
    fn unplan(&mut self) {
        self.plan.hover = None;
        self.plan.elements.clear();
        self.plan.scroll_timelines.clear();
        self.effects.clear();
        self.parked = true;
    }

    /// Has the lane follow `plan`, which a rendering update that presented no frame of its own sealed, in place of the
    /// one it followed.
    fn replan(&mut self, plan: ClockPlan) {
        self.parked = !plan.animates();
        self.shown_scroll_progress = plan.scroll_progress(&[]).unwrap_or_default();
        self.shown_at = f64::NEG_INFINITY;
        self.replan_host_effects(&plan.elements);
        self.plan = plan;
    }

    /// What the lane does, as the host reads it.
    fn state(&self) -> FfiClockLaneState {
        if !self.plan.animates() {
            FfiClockLaneState::Hovering
        } else if self.parked && !self.transitions_run() {
            FfiClockLaneState::Parked
        } else {
            FfiClockLaneState::Ticking
        }
    }

    /// Samples the plan's animations at the timestamp of `frame_time_nanoseconds` and where the compositor has scrolled
    /// to, where `samples_animations`, shows the samples, lays out what they moved and presents the frame. A tick at or
    /// past the deadline, or one that needs the host, parks the lane, and so does the tick that shows the ends of the
    /// animations where no scroll moves anything after them. A tick handed `pointer`, where the pointer moved since
    /// the last tick, hovers what is under it first (see [`hover`]). Answers none where the lane has no fork to tick on.
    fn tick_on_fork(
        &mut self,
        frame_time_nanoseconds: i64,
        pointer: Option<PendingPointer>,
        samples_animations: bool,
    ) -> Option<Ticked> {
        let mut fork = match std::mem::take(&mut self.state) {
            LaneState::Forked(fork) => fork,
            state => {
                self.state = state;
                return None;
            }
        };
        let ticked = self.tick(&mut fork, frame_time_nanoseconds, pointer, samples_animations);
        self.state = LaneState::Forked(fork);
        Some(ticked)
    }

    fn tick(
        &mut self,
        state: &mut RenderState,
        frame_time_nanoseconds: i64,
        pointer: Option<PendingPointer>,
        samples_animations: bool,
    ) -> Ticked {
        let timestamp = frame_time_nanoseconds as f64 / 1_000_000.0 - self.plan.time_origin;
        let hovers = pointer.filter(|_| !self.hovered.is_parked());
        let transitions = self.transitions_run();
        let samples_animations = samples_animations && !self.parked;
        let mut ticked = Ticked {
            hover_move: None,
            presented: false,
        };
        if self.frame_left_to_host || (!samples_animations && hovers.is_none() && !transitions) {
            return ticked;
        }
        let turn = SamplingTurn(());
        let sampled_at = state.sample_clock.next(timestamp);
        // What the plan's animations show at the tick, where the lane still samples them.
        let progress = match samples_animations {
            false => None,
            true => self.sample_progress(sampled_at).unwrap_or_else(|_| {
                self.parked = true;
                None
            }),
        };
        if progress.is_none() && hovers.is_none() && !transitions {
            return ticked;
        }
        state.engine_mut().lend_tick_shown(std::mem::take(&mut self.shown));
        let mut moved = false;
        if transitions {
            // NB: Transitions a tick left to the host keep what their boxes showed, beside what the others moved.
            moved |= self.sample_started_transitions(state, sampled_at.0).unwrap_or(true);
        }
        if let Some(pointer) = hovers {
            let hovered = self.hover(state, pointer, sampled_at.0);
            ticked.hover_move = Some(LaneMove {
                input_event_id: pointer.input_event_id,
                target: match hovered {
                    hover::Hovered::LeftToHost => None,
                    _ => self.hovered.target,
                },
            });
            moved |= hovered == hover::Hovered::Moved;
        }
        if let Some(progress) = progress {
            let samples = AnimationTimelineSamples::at_tick(sampled_at.0, &progress);
            match self.sample_host_animations(state, samples) {
                Ok(()) => {
                    moved = true;
                    self.shown_at = sampled_at.0;
                    self.shown_scroll_progress = progress;
                }
                Err(_) => self.parked = true,
            }
            if sampled_at.0 >= self.plan.last_end && self.plan.scroll_timelines.is_empty() {
                self.parked = true;
            }
        }
        if moved {
            match self.lay_out_and_present(state, &turn) {
                Ok(()) => {
                    if hover::logs_hover() {
                        eprintln!(
                            "{} hover lane: tick presented a frame hovering {:?} at {:?}, sampled at {:.3}",
                            hover::log_time(),
                            self.hovered.target.flatten().map(StyleNodeID::raw),
                            self.hovered.pointer,
                            (sampled_at.0 + self.plan.time_origin) % 10_000_000.0
                        );
                    }
                    ticked.presented = true;
                }
                Err(Park(reason)) => {
                    if hover::logs_hover() {
                        eprintln!("{} hover lane: frame left to the host: {reason}", hover::log_time());
                    }
                    self.parked = true;
                    self.frame_left_to_host = true;
                    self.hovered.park();
                }
            }
        }
        self.shown = state.engine_mut().take_tick_shown();
        ticked
    }

    /// The progress of the scroll timelines a tick sampled at `sampled_at` samples the plan's animations at, or none where
    /// a frame that showed the document timeline's animations ended shows them already and nothing scrolled since. A tick
    /// at or past the deadline, or past what the scroll timelines plan for, parks the lane.
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

    /// Lays out what the tick moved and presents the frame.
    fn lay_out_and_present(&mut self, state: &mut RenderState, turn: &SamplingTurn) -> Result<(), Park> {
        let Self { plan, ticked, .. } = self;
        // What the rounds owe goes with the fork, which no host takes back.
        let mut owed = Vec::new();
        // A container the round resized restyles what its size decides, as the host's style update after a layout
        // does, and lays it out again, until the containers stand.
        for _ in 0..SIZE_QUERY_ROUND_LIMIT {
            let Some(answer) = plan.round.run(&mut state.arena, &mut owed)? else {
                return self.present(state, turn);
            };
            let resized: smallvec::SmallVec<[StyleNodeID; 4]> = answer.resized_size_containers().collect();
            if resized.is_empty() {
                return self.present(state, turn);
            }
            restyle_size_query_dependents(state, &resized, &plan.elements, ticked)?;
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
        let restyled: SmallVec<[StyleNodeID; 4]> = self.sampled_elements().collect();
        // NB: A descendant that inherits what the effects animate may have no box, as under `display: none`.
        self.presented_colors.extend(restyled.into_iter().filter_map(|element| {
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
        recorder, presented, ..
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
    let visual_contexts = prepare_for_clock_tick(arena, viewport)?;
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

/// Hands the lanes `ticks` belong to a display tick at `frame_time_nanoseconds`, at which the compositor had scrolled to
/// the `scroll_offset_count` offsets at `scroll_offsets`, and answers whether they want the next one.
///
/// # Safety
///
/// `ticks` must come from `document_host_clock_ticks` and not be released yet, and `scroll_offsets` must hold
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
/// `ticks` must come from `document_host_clock_ticks`, and be released once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn clock_ticks_release(ticks: *const ClockTicks) {
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { Arc::from_raw(ticks) });
}

/// A reference to the ticks the render clock hands the lanes of `host`'s document, which the caller releases with
/// `clock_ticks_release`.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_clock_ticks(host: &DocumentHost) -> *const ClockTicks {
    Arc::into_raw(Arc::clone(host.clock_ticks()))
}

/// Drops the plan of the lane of the frame `host`'s document presented last, for the rendering update with the serial
/// number `update`, which leaves it none. The plan is the host's, so this reads nothing of the render state.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_drop_clock_plan(host: &DocumentHost, update: u64) {
    host.seal_clock_plan(None, update);
}

/// Notes that script changed an animation of `host`'s document: the animations of the plans sealed before no longer
/// stand, and the lanes' hover goes on.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_note_animation_change(host: &DocumentHost) {
    host.clock_ticks().note_animation_change();
}

/// Takes in what the lanes of `host`'s document did, as a rendering update begins, and answers whether a tick of a lane
/// presented a frame since the last rendering update, which the screen shows in place of the host's. The update keeps
/// the hover the lanes moved, and hovers the newest pointer move the compositor told of, which owe the boundary events
/// of the move until the host handles a mouse move. The lanes present nothing more until the update's frame: `update` is
/// the update's serial number, which its plan seals with.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_take_clock_lanes_in(host: &DocumentHost, update: u64) -> bool {
    host.take_clock_lanes_in(update)
}

/// Whether the lanes of `host`'s document may hover or run transitions beside the tasks: the last plan the host sealed for
/// them was one.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_may_have_lanes(host: &DocumentHost) -> bool {
    host.may_have_lanes()
}

/// Whether the lanes of `host`'s document wait for the rendering update that took them in to seal its plan, presenting
/// nothing until then, where they would present frames beside a task: they sample running animations, or the pointer
/// is over the document, which they hover as it moves.
///
/// # Safety
///
/// `host` must name a live document host.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_lanes_wait_for_the_update(host: &DocumentHost) -> bool {
    let ticks = host.clock_ticks();
    host.lanes_wait_for_the_update()
        && (ticks.plan_animates() || ticks.newest_pointer().is_some_and(|pointer| pointer.position.is_some()))
}

/// Notes whether the event loop of `host`'s document begins a task or goes idle: beside an idle event loop the lanes
/// sample no animations, which its rendering updates run. Answers whether a task begins whose lanes sample the
/// animations of the last plan the host sealed, for the caller to tick them at once.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_note_event_loop_task(host: &DocumentHost, runs_task: bool) -> bool {
    host.note_event_loop_task(runs_task)
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
    has_target: &mut bool,
    target: &mut u32,
) -> bool {
    let Some(owed) = host.take_hover_events_owed() else {
        return false;
    };
    *has_pointer = owed.pointer.is_some();
    if let Some(pointer) = owed.pointer {
        *pointer_x = pointer.x;
        *pointer_y = pointer.y;
    }
    *has_target = owed.target.is_some();
    *target = owed.target.flatten().map_or(0, StyleNodeID::raw);
    true
}

/// Answers whether a tick of the lanes of `host`'s document took the pointer move of the mouse event with the input event
/// id `input_event_id`, as the rendering update handling it took the lanes in, and writes the style node id of the
/// element the lane hovered there to `target`, or 0 for none: that is the element the screen shows the hover on, which
/// the host's events hover in place of the one its own layout has under the pointer.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_lane_hover_target_for(
    host: &DocumentHost,
    input_event_id: u64,
    target: &mut u32,
) -> bool {
    let Some(hovered) = host.lane_hover_target_for(input_event_id) else {
        return false;
    };
    *target = hovered.map_or(0, StyleNodeID::raw);
    true
}

/// Holds the lanes of `host`'s document for a recording of the host's own that adds to the resource storage they
/// present with, until `document_host_release_clock_lane`: they present nothing meanwhile.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_hold_clock_lane(host: &DocumentHost) {
    host.clock_ticks().hold();
}

/// Releases a hold of `document_host_hold_clock_lane`.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_release_clock_lane(host: &DocumentHost) {
    host.clock_ticks().release();
}

/// What the lane of the frame `host`'s document presented last does, once the lane of a frame presented by then has
/// come together. For a test.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_clock_lane_state(host: &DocumentHost) -> FfiClockLaneState {
    settle_lanes_for_testing();
    host.clock_ticks().lane_state()
}

/// Whether the lane of a frame `host`'s document sampled after the one it presented last has yet to come together, once
/// the lane of a frame presented by then has come together. For a test.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_clock_lane_is_coming(host: &DocumentHost) -> bool {
    settle_lanes_for_testing();
    host.clock_ticks().published().awaits_lane
}

/// Hands the lanes of `host`'s document a tick at `frame_time_nanoseconds`, at which the compositor had scrolled to
/// `scroll_offset`, if not null, and waits until the StyleLayout thread has run the jobs handed to it before, the tick
/// among them, and the Paint thread the recording of the frame the tick presents. For a test, whose clock ticks only
/// where it injects them.
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
    settle_lanes_for_testing();
    // SAFETY: Guaranteed by the caller.
    let scroll_offsets = unsafe { scroll_offset.as_ref() }.map_or(&[][..], std::slice::from_ref);
    host.clock_ticks().tick(frame_time_nanoseconds, scroll_offsets);
    settle_lanes_for_testing();
}

/// Waits until the Paint thread has run the jobs handed to it before, where a test holds no recording on it, then the
/// StyleLayout thread, and the Paint thread again: the lane of a frame presented by then has come together, and the
/// frames ticks recorded were presented. For a test.
pub(super) fn settle_lanes_for_testing() {
    let settle_paint = || {
        if !crate::painting::recording_slot::recording_is_held_for_testing() {
            crate::paint_stage::paint_thread().run(|| ());
        }
    };
    settle_paint();
    crate::stage_thread::style_layout_thread().run(|| ());
    settle_paint();
}

/// Writes the color the box of `element` showed in the last frame a tick of a lane of `host`'s document presented to
/// `color`, as `0xAARRGGBB`, and answers whether a hover's transitions restyled the box in it. For a test.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread, and `color` must be
/// valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_presented_color(host: &DocumentHost, element: u32, color: *mut u32) -> bool {
    crate::stage_thread::style_layout_thread().run(|| ());
    let Some(presented) =
        StyleNodeID::from_raw(element).and_then(|element| host.clock_ticks().presented_color(element))
    else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { color.write(presented) };
    true
}

/// Whether the mouse event with the input event id `input_event_id` is older than the newest pointer move `host`'s
/// document knows of, and notes the id of one that is not, which the host handles: an older event moves neither the
/// hover nor the pointer.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_mouse_event_is_outdated(host: &DocumentHost, input_event_id: u64) -> bool {
    let outdated = host.mouse_event_is_outdated(input_event_id);
    if outdated && hover::logs_hover() {
        eprintln!(
            "{} hover lane: mouse event {input_event_id} is older than the newest pointer move",
            hover::log_time()
        );
    }
    outdated
}

/// Writes the border box of `element` in the last frame a tick of a lane of `host`'s document presented to `rect`, and
/// answers whether a tick presented one. For a test.
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
    crate::stage_thread::style_layout_thread().run(|| ());
    let Some(presented) =
        StyleNodeID::from_raw(element).and_then(|element| host.clock_ticks().presented_border_box(element))
    else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { rect.write(presented.into()) };
    true
}

#[cfg(test)]
mod tests {
    use super::{LaneTransitionStart, LaneTransitionStarts};
    use crate::css::style::tree::StyleNodeID;

    const COLOR: u16 = 1;
    const WIDTH: u16 = 2;

    fn start(node: u32, properties: &[u16], start_time: f64) -> LaneTransitionStart {
        LaneTransitionStart {
            node: StyleNodeID::from_raw(node).unwrap(),
            properties: properties.iter().copied().collect(),
            start_time,
        }
    }

    fn node(raw: u32) -> StyleNodeID {
        StyleNodeID::from_raw(raw).unwrap()
    }

    #[test]
    fn a_lane_start_backdates_only_the_properties_it_started() {
        let mut starts = LaneTransitionStarts::default();
        starts.take_in(vec![start(1, &[COLOR], 1000.0)]);
        assert_eq!(starts.start_of(node(1), COLOR), Some(1000.0));
        assert_eq!(starts.start_of(node(1), WIDTH), None);
        assert_eq!(starts.start_of(node(2), COLOR), None);
    }

    #[test]
    fn a_lane_start_the_host_took_over_backdates_nothing_after() {
        let mut starts = LaneTransitionStarts::default();
        starts.take_in(vec![start(1, &[COLOR], 1000.0)]);
        assert_eq!(starts.start_of(node(1), COLOR), Some(1000.0));
        starts.forget();
        // The lane still reports the start until the frame it follows is replaced.
        starts.take_in(vec![start(1, &[COLOR], 1000.0)]);
        assert_eq!(starts.start_of(node(1), COLOR), None);
        // A later hover of the lane starts the transition again.
        starts.take_in(vec![start(1, &[COLOR], 1000.0), start(1, &[COLOR], 5000.0)]);
        assert_eq!(starts.start_of(node(1), COLOR), Some(5000.0));
    }

    #[test]
    fn a_lane_start_the_host_did_not_take_over_backdates_later() {
        let mut starts = LaneTransitionStarts::default();
        starts.take_in(vec![start(1, &[COLOR], 1000.0), start(2, &[COLOR], 1000.0)]);
        assert_eq!(starts.start_of(node(1), COLOR), Some(1000.0));
        starts.forget();
        starts.take_in(vec![start(1, &[COLOR], 1000.0), start(2, &[COLOR], 1000.0)]);
        assert_eq!(starts.start_of(node(1), COLOR), None);
        assert_eq!(starts.start_of(node(2), COLOR), Some(1000.0));
    }

    #[test]
    fn a_lane_start_no_lane_reports_backdates_nothing() {
        let mut starts = LaneTransitionStarts::default();
        starts.take_in(vec![start(1, &[COLOR], 1000.0)]);
        starts.take_in(Vec::new());
        assert_eq!(starts.start_of(node(1), COLOR), None);
    }
}
