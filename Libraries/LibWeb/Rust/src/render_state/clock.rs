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

use super::owner::{self, DocumentId};
use super::wait::TaskStart;
use super::{DocumentHost, RenderState};
use crate::css::css_pixels::CssPixelRect;
use crate::css::style::animations::AnimationTimelineSamples;
use crate::css::style::engine_sample::{DependentRestyle, NeedsHost, TickShownRecords};
use crate::css::style::tree::StyleNodeID;
use crate::layout::node_data::NodeSlotId;
use crate::layout::tree_update_marks::{
    FfiLayoutTreeUpdateMark, LayoutTreeUpdateMarkWrite, layout_tree_update_reuse_reason,
};
use crate::layout::used_values::FfiCssPixelRect;
use crate::layout::{ClockRound, ClockRoundDeclined, HostStyle, LayoutNodeArena, LayoutRoundAnswer, build_keeps_box};
use crate::painting::ffi::FfiPresentation;
use crate::painting::paint_passes::{ClockTickVisualContexts, VisualContextsNeedHost, prepare_for_clock_tick};
use crate::painting::paintable_geometry::absolute_border_box_rect;
use crate::painting::presentation::Presentation;
use crate::painting::record::damage::PaintDamage;
use crate::painting::record::publish::{renders_vector_images, take_in_published_output, take_in_recording};
use crate::painting::record::recorder_state::RecorderState;
use crate::painting::record::{RecordingInputs, RecordingOutput};
use crate::painting::recording_slot::{FrameInputs, FrozenFrame, freeze_recording_frame, present, record_frame};
use crate::stage_thread::{InFlight, Riding, StopWord, Ticker};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicI64, Ordering};

/// What the ticks of a clock lease sample, sealed at the end of a rendering update.
pub(crate) struct ClockPlan {
    /// The elements whose running animations a tick samples.
    elements: Vec<StyleNodeID>,
    /// The monotonic time, in milliseconds, at which the document's timestamps are zero.
    time_origin: f64,
    /// The timestamp of the next event of the animations, which the host sends: a tick at or past it samples nothing.
    deadline: f64,
    /// The timestamp at which the animations a tick samples have all ended: the tick at or past it shows their ends,
    /// and the lease wants no tick after it.
    last_end: f64,
    round: ClockRound,
}

impl ClockPlan {
    pub(crate) fn new(
        elements: Vec<StyleNodeID>,
        time_origin: f64,
        deadline: f64,
        last_end: f64,
        round: ClockRound,
    ) -> Self {
        Self {
            elements,
            time_origin,
            deadline,
            last_end,
            round,
        }
    }
}

/// What a clock lease brings its host back: the recording of the frame a tick presented last, which brings the
/// recorder state and presentation the lease took, the boxes its ticks showed samples in with the styles the host
/// installed for them, the boxes they built again from records of their own, and what the ticks' rounds owe the host,
/// in the order they ran. The render state stays with the render owner, which the lease names it to.
pub(crate) struct LeaseLanding {
    document: DocumentId,
    pub(super) recording: TickRecording,
    plan: ClockPlan,
    pub(super) ticked: Vec<(NodeSlotId, HostStyle)>,
    pub(super) built: TickBuilt,
    pub(super) owed: Vec<LayoutRoundAnswer>,
    /// The border boxes of the plan's elements in the last frame a tick presented.
    pub(super) presented_border_boxes: Vec<(StyleNodeID, CssPixelRect)>,
    /// Whether a tick found the lease could sample no more: past the deadline or the animations' ends, or something only
    /// the host computes.
    parked: bool,
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
    /// Leases the render state of `document`, with `recorder` and `presentation`, to the render clock as a task begins,
    /// to tick `plan`. Answers the lease, and the ticks the render clock hands it.
    pub(super) fn begin(
        _: &TaskStart,
        document: DocumentId,
        recorder: RecorderState,
        presentation: Presentation,
        plan: ClockPlan,
    ) -> (Self, Arc<ClockTicks>) {
        let (flight, ticker) = crate::stage_thread::style_layout_thread().lease(LeaseLanding {
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
            parked: false,
        });
        let ticks = Arc::new(ClockTicks {
            ticker,
            latest: AtomicI64::new(i64::MIN),
            queued: AtomicBool::new(false),
            parked: AtomicBool::new(false),
        });
        (
            Self {
                flight,
                ticks: Arc::clone(&ticks),
            },
            ticks,
        )
    }

    /// Ends the lease: says the stop word, and waits for the tick that runs, if one does. The host's document module
    /// ends a lease only where the host waits for its render state.
    pub(super) fn end(self) -> LeaseLanding {
        self.flight.join(EndsLease(()))
    }

    /// The ticks the render clock hands the lease.
    pub(super) fn ticks(&self) -> &Arc<ClockTicks> {
        &self.ticks
    }
}

/// The display ticks the render clock hands a lease, folded into one queued tick, which runs at the latest of their times.
pub struct ClockTicks {
    ticker: Ticker<LeaseLanding>,
    /// The latest frame time of the ticks handed to the lease, in nanoseconds of the monotonic clock. It only grows, so
    /// a tick handed an earlier time than one before it, as a display tick behind the immediate first one is, never
    /// samples the animations back in time.
    latest: AtomicI64,
    /// Whether a tick is queued.
    queued: AtomicBool,
    /// Whether a tick parked the lease, which then samples nothing more.
    parked: AtomicBool,
}

impl ClockTicks {
    /// Hands the lease a tick at `frame_time_nanoseconds`: the tick already queued runs at the latest time, or a tick is
    /// queued. Answers whether the lease wants the next tick, which it does until it ends or parks.
    pub(super) fn tick(self: &Arc<Self>, frame_time_nanoseconds: i64) -> bool {
        if self.parked.load(Ordering::Relaxed) || !self.ticker.is_live() {
            return false;
        }
        self.latest.fetch_max(frame_time_nanoseconds, Ordering::AcqRel);
        if self.queued.swap(true, Ordering::AcqRel) {
            return true;
        }
        let ticks = Arc::clone(self);
        self.ticker.run(move |landing, stop| {
            // A tick handed after the flag drops queues another run, and one handed before it is in `latest`.
            ticks.queued.swap(false, Ordering::AcqRel);
            landing.tick(ticks.latest.load(Ordering::Acquire), stop);
            if landing.parked {
                ticks.parked.store(true, Ordering::Relaxed);
            }
        });
        true
    }

    /// Whether a tick parked the lease.
    pub(super) fn is_parked(&self) -> bool {
        self.parked.load(Ordering::Relaxed)
    }
}

/// What parks a lease: something of a tick only the host computes.
struct Park;

impl From<NeedsHost> for Park {
    fn from(_: NeedsHost) -> Self {
        Self
    }
}

impl From<ClockRoundDeclined> for Park {
    fn from(_: ClockRoundDeclined) -> Self {
        Self
    }
}

impl From<VisualContextsNeedHost> for Park {
    fn from(_: VisualContextsNeedHost) -> Self {
        Self
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
/// of the pseudo-elements those styles move for the round's tree build to build again. An element whose animations
/// `animated` samples composes over its host's style, which only the host restyles.
fn restyle_size_query_dependents(
    state: &mut RenderState,
    resized: &[StyleNodeID],
    animated: &[StyleNodeID],
    ticked: &mut Vec<(NodeSlotId, HostStyle)>,
    built: &mut TickBuilt,
) -> Result<(), Park> {
    let mut rebuilt: smallvec::SmallVec<[StyleNodeID; 2]> = smallvec::SmallVec::new();
    for &container in resized {
        for dependent in state.engine_mut().size_container_query_dependents(container) {
            if animated.contains(&dependent) {
                return Err(Park);
            }
            let row = state.arena.arena().bound_row(dependent);
            let restyled = match state
                .engine_mut()
                .restyle_size_query_dependent(dependent, !row.is_invalid())?
            {
                DependentRestyle::Unmoved => continue,
                DependentRestyle::InBox(restyled) => restyled,
                DependentRestyle::PseudoElementsMove(restyled) => {
                    rebuilt.push(dependent);
                    restyled
                }
            };
            let arena = state.arena.arena();
            if let Some(host_style) = arena.install_animation_sample(row, restyled)? {
                ticked.push((row, host_style));
            }
            arena.push_paint_damage_for_repaint(row, PaintDamage::ALL_PRODUCERS);
        }
    }
    // The build regenerates the pseudo-elements in their element's box, which keeps the sample the clock frame showed
    // in it.
    use layout_tree_update_reuse_reason::PSEUDO_ELEMENT_CHANGE;
    for node in rebuilt {
        if !build_keeps_box(state.arena.arena_mut(), node, PSEUDO_ELEMENT_CHANGE) {
            return Err(Park);
        }
        let arena = state.arena.arena();
        arena.mark_layout_tree_update(Some(node), style_change_mark(PSEUDO_ELEMENT_CHANGE));
        built.build_again_on_landing(arena, (node, PSEUDO_ELEMENT_CHANGE));
    }
    Ok(())
}

impl LeaseLanding {
    /// Samples the plan's animations at the timestamp of `frame_time_nanoseconds`, shows the samples, lays out what
    /// they moved and presents the frame, unless the host said the stop word: the host waits for the lease. A tick at or
    /// past the deadline, or one that needs the host, parks the lease, and so does the tick that shows the ends of the
    /// animations, after which a tick would present the same frame again.
    fn tick(&mut self, frame_time_nanoseconds: i64, stop: &StopWord) {
        if self.parked || stop.is_said() {
            return;
        }
        let timestamp = frame_time_nanoseconds as f64 / 1_000_000.0 - self.plan.time_origin;
        // The tick runs on the render owner, the one thread that reaches the state. It shows the records the frames
        // before it showed, and takes them back as it ends.
        self.parked = timestamp >= self.plan.deadline
            || owner::with_state(self.document, None, |state| {
                state
                    .engine_mut()
                    .lend_tick_shown(std::mem::take(&mut self.built.shown));
                let sampled = self.sample(state, timestamp);
                self.built.shown = state.engine_mut().take_tick_shown();
                sampled
            })
            .is_err()
            || timestamp >= self.plan.last_end;
    }

    fn sample(&mut self, state: &mut RenderState, timestamp: f64) -> Result<(), Park> {
        let Self {
            plan,
            ticked,
            built,
            owed,
            ..
        } = self;
        let samples = AnimationTimelineSamples::default().with_time(timestamp);
        for &element in &plan.elements {
            let arena = state.arena.arena();
            let row = arena.bound_row(element);
            if row.is_invalid() {
                return Err(Park);
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
        // A container the round resized restyles what its size decides, as the host's style update after a layout
        // does, and lays it out again, until the containers stand.
        for _ in 0..SIZE_QUERY_ROUND_LIMIT {
            let Some(answer) = plan.round.run(&mut state.arena, owed)? else {
                return self.present(state);
            };
            let resized: smallvec::SmallVec<[StyleNodeID; 4]> = answer.resized_size_containers().collect();
            if resized.is_empty() {
                return self.present(state);
            }
            restyle_size_query_dependents(state, &resized, &plan.elements, ticked, built)?;
        }
        // The last restyle may have left nothing to lay out again.
        match state.arena.arena().layout_is_up_to_date(false) {
            true => self.present(state),
            false => Err(Park),
        }
    }

    /// Takes in the frame the last tick presented, and has the Paint thread record the document's frame again, with the
    /// inputs of the last recording that published, and present it beside the event loop and the next tick. A frame
    /// whose visual contexts only the host settles, or whose trace the host reads, is the host's to present, and so is
    /// any frame after one that renders an SVG image.
    fn present(&mut self, state: &mut RenderState) -> Result<(), Park> {
        let mut clock_recorder = self.recording.0.take(WaitsForTickRecording(()));
        let arena = state.arena.arena_mut();
        match freeze_tick_frame(arena, &mut clock_recorder) {
            Ok((frozen, inputs, visual_contexts)) => {
                let viewport = arena.layout_root();
                self.recording = TickRecording(
                    crate::stage_thread::paint_thread()
                        .ride(move || clock_recorder.record(frozen, viewport, inputs, visual_contexts)),
                );
            }
            Err(park) => {
                self.recording = TickRecording(Riding::landed(clock_recorder));
                return Err(park);
            }
        }
        let rows = arena.paintable_rows();
        self.presented_border_boxes.clear();
        self.presented_border_boxes
            .extend(self.plan.elements.iter().filter_map(|&element| {
                let row = arena.bound_row(element);
                rows.paintable_row_is_populated(row)
                    .then(|| (element, absolute_border_box_rect(&rows, row)))
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
        TickPresented::LeftToHost => return Err(Park),
    }
    let viewport = arena.layout_root();
    if arena.paint_state().borrow().trace_recordings {
        return Err(Park);
    }
    let visual_contexts = prepare_for_clock_tick(arena, viewport, presentation)?;
    let inputs = recorder.published_inputs.take().ok_or(Park)?;
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
        return Err(Park);
    };
    Ok((frozen, inputs, visual_contexts))
}

impl ClockRecorder {
    /// Records `frozen`, the frame of the document's `viewport`, with `inputs`, and presents it with the visual contexts
    /// a tick prepared for it, on the Paint thread. Only the host renders an SVG image.
    fn record(
        mut self,
        frozen: FrozenFrame,
        viewport: NodeSlotId,
        inputs: RecordingInputs,
        visual_contexts: Option<ClockTickVisualContexts>,
    ) -> Self {
        let Self {
            recorder,
            presentation,
            presented,
        } = &mut self;
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
        let output = present(presentation, pending, recorder);
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

/// Hands the clock lease `ticks` belong to a display tick at `frame_time_nanoseconds`, and answers whether it wants the
/// next one.
///
/// # Safety
///
/// `ticks` must come from `document_host_lease_clock` and not be released yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn clock_ticks_tick(ticks: *const ClockTicks, frame_time_nanoseconds: i64) -> bool {
    // SAFETY: Guaranteed by the caller, whose reference this borrows.
    let ticks = std::mem::ManuallyDrop::new(unsafe { Arc::from_raw(ticks) });
    ticks.tick(frame_time_nanoseconds)
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
) -> *const ClockTicks {
    // SAFETY: Guaranteed by the caller.
    let presentation = unsafe { &mut *presentation };
    let start = TaskStart::at_event_loop_entry(&LEASES_CLOCK_FOR_TASK);
    // SAFETY: Guaranteed by the caller.
    let Some(taken) = (unsafe { Presentation::take(presentation) }) else {
        return std::ptr::null();
    };
    match host.lease_clock(&start, taken) {
        Ok(ticks) => Arc::into_raw(ticks),
        Err(given_back) => {
            *presentation = given_back.into_ffi();
            std::ptr::null()
        }
    }
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

/// Ends the clock lease of `host`'s document, where one runs, as a rendering update begins, and answers whether the
/// document's state was leased to the render clock since the last rendering update.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_end_clock_lease_for_rendering_update(host: *const DocumentHost) -> bool {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    host.end_clock_lease_for_rendering_update()
}

/// What a document's clock lease is doing.
#[repr(u8)]
pub enum FfiClockLeaseState {
    /// No lease runs.
    None,
    Ticking,
    /// A tick parked the lease, which samples nothing more.
    Parked,
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
        Some(ticks) if ticks.is_parked() => FfiClockLeaseState::Parked,
        Some(_) => FfiClockLeaseState::Ticking,
    }
}

/// Hands the clock lease of `host`'s document, where one runs, a tick at `frame_time_nanoseconds`, and waits until the
/// StyleLayout thread has run the jobs handed to it before, the tick among them, and the Paint thread the recording of
/// the frame the tick presents, without ending the lease. For a test, whose clock ticks only where it injects them.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_inject_clock_tick(host: &DocumentHost, frame_time_nanoseconds: i64) {
    if let Some(ticks) = host.clock_ticks() {
        ticks.tick(frame_time_nanoseconds);
        crate::stage_thread::style_layout_thread().run(|| ());
        crate::stage_thread::paint_thread().run(|| ());
    }
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
