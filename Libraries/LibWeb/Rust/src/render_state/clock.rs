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
//! and records and presents the frame with the navigable's presenter, beside the event loop. The host ends the lease
//! with any job it hands the owner, which waits for at most the one tick that runs, and takes everything back at once.
//! What a tick showed never becomes visible to script: the boxes take back the styles the host installed before any
//! job of the host reads them, so the animations' timeline moves only in a rendering update.

use super::owner::{self, DocumentId};
use super::wait::TaskStart;
use super::{DocumentHost, RenderState};
use crate::css::css_pixels::CssPixelRect;
use crate::css::style::animations::AnimationTimelineSamples;
use crate::css::style::engine_sample::NeedsHost;
use crate::css::style::tree::StyleNodeID;
use crate::layout::node_data::NodeSlotId;
use crate::layout::used_values::FfiCssPixelRect;
use crate::layout::{ClockRound, ClockRoundDeclined, HostStyle, LayoutRoundAnswer};
use crate::painting::ffi::FfiPresentation;
use crate::painting::paint_passes::{VisualContextsNeedHost, prepare_for_clock_tick};
use crate::painting::paintable_geometry::absolute_border_box_rect;
use crate::painting::presentation::Presentation;
use crate::painting::record::damage::PaintDamage;
use crate::painting::record::publish::{renders_vector_images, take_in_published_output, take_in_recording};
use crate::painting::record::recorder_state::RecorderState;
use crate::painting::recording_slot::{FrameInputs, freeze_recording_frame, present, record_frame};
use crate::stage_thread::{InFlight, StopWord, Ticker};
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

/// What a clock lease brings its host back: the recorder state and presentation it took, the boxes its ticks showed
/// samples in with the styles the host installed for them, and what the ticks' rounds owe the host, in the order they
/// ran. The render state stays with the render owner, which the lease names it to.
pub(crate) struct LeaseLanding {
    document: DocumentId,
    pub(super) recorder: RecorderState,
    pub(super) presentation: Presentation,
    plan: ClockPlan,
    pub(super) ticked: Vec<(NodeSlotId, HostStyle)>,
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
            recorder,
            presentation,
            plan,
            ticked: Vec::new(),
            owed: Vec::new(),
            presented_border_boxes: Vec::new(),
            parked: false,
        });
        let ticks = Arc::new(ClockTicks {
            ticker,
            newest: AtomicI64::new(NO_TICK),
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

/// The display ticks the render clock hands a lease, folded into one queued tick, which runs at the newest tick's time.
pub struct ClockTicks {
    ticker: Ticker<LeaseLanding>,
    /// The frame time of the newest tick not run yet, in nanoseconds of the monotonic clock, or [`NO_TICK`].
    newest: AtomicI64,
    /// Whether a tick parked the lease, which then samples nothing more.
    parked: AtomicBool,
}

const NO_TICK: i64 = i64::MIN;

impl ClockTicks {
    /// Hands the lease a tick at `frame_time_nanoseconds`: the tick already queued runs at the newest time, or a tick is
    /// queued. Answers whether the lease wants the next tick, which it does until it ends or parks.
    pub(super) fn tick(self: &Arc<Self>, frame_time_nanoseconds: i64) -> bool {
        if self.parked.load(Ordering::Relaxed) || !self.ticker.is_live() {
            return false;
        }
        if self.newest.swap(frame_time_nanoseconds, Ordering::AcqRel) != NO_TICK {
            return true;
        }
        let ticks = Arc::clone(self);
        self.ticker.run(move |landing, stop| {
            landing.tick(ticks.newest.swap(NO_TICK, Ordering::AcqRel), stop);
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

/// How many times a tick lays out again what the containers it resized restyled, as the host's layout update
/// stabilizes them.
const SIZE_QUERY_ROUND_LIMIT: usize = 8;

/// Shows in their boxes the styles of the elements whose style a size query or container-relative unit decided below
/// the containers in `resized`, against their new sizes, keeping each box's host style in `ticked`. An element whose
/// animations `animated` samples composes over its host's style, which only the host restyles.
fn restyle_size_query_dependents(
    state: &mut RenderState,
    resized: &[StyleNodeID],
    animated: &[StyleNodeID],
    ticked: &mut Vec<(NodeSlotId, HostStyle)>,
) -> Result<(), Park> {
    for &container in resized {
        for dependent in state.engine_mut().size_container_query_dependents(container) {
            if animated.contains(&dependent) {
                return Err(Park);
            }
            let row = state.arena.arena().bound_row(dependent);
            let Some(restyled) = state
                .engine_mut()
                .restyle_size_query_dependent(dependent, !row.is_invalid())?
            else {
                continue;
            };
            let arena = state.arena.arena();
            if let Some(host_style) = arena.install_animation_sample(row, restyled)? {
                ticked.push((row, host_style));
            }
            arena.push_paint_damage_for_repaint(row, PaintDamage::ALL_PRODUCERS);
        }
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
        // The tick runs on the render owner, the one thread that reaches the state.
        self.parked = timestamp >= self.plan.deadline
            || owner::with_state(self.document, None, |state| self.sample(state, timestamp)).is_err()
            || timestamp >= self.plan.last_end;
    }

    fn sample(&mut self, state: &mut RenderState, timestamp: f64) -> Result<(), Park> {
        let Self { plan, ticked, owed, .. } = self;
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
            let Some(answer) = plan.round.run(&mut state.arena)? else {
                return self.present(state);
            };
            let resized: smallvec::SmallVec<[StyleNodeID; 4]> = answer.resized_size_containers().collect();
            owed.push(answer);
            if resized.is_empty() {
                return self.present(state);
            }
            restyle_size_query_dependents(state, &resized, &plan.elements, ticked)?;
        }
        // The last restyle may have left nothing to lay out again.
        match state.arena.arena().layout_is_up_to_date(false) {
            true => self.present(state),
            false => Err(Park),
        }
    }

    /// Records the document's frame again, with the inputs of the last recording that published, and presents it
    /// beside the event loop. A frame whose visual contexts only the host settles, or that renders an SVG image,
    /// or whose trace the host reads, is the host's to present.
    fn present(&mut self, state: &mut RenderState) -> Result<(), Park> {
        let Self {
            recorder,
            presentation,
            plan,
            presented_border_boxes,
            ..
        } = self;
        let arena = state.arena.arena_mut();
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
        let (pending, _) = record_frame(frozen.frame, recorder, viewport, false, inputs);
        // Only the host renders an SVG image. The recording wrote the paint-order tree, which no longer describes the
        // recording published last.
        if renders_vector_images(&pending) {
            recorder.forget_published_recording();
            return Err(Park);
        }
        if let Some(visual_contexts) = visual_contexts {
            presentation.take_visual_context_tree(visual_contexts);
        }
        let output = present(presentation, pending, recorder);
        take_in_published_output(recorder, &mut None, output, true, |output, hit_test_list_changed| {
            take_in_recording(arena, output, hit_test_list_changed, true);
        });
        let rows = arena.paintable_rows();
        presented_border_boxes.clear();
        presented_border_boxes.extend(plan.elements.iter().filter_map(|&element| {
            let row = arena.bound_row(element);
            rows.paintable_row_is_populated(row)
                .then(|| (element, absolute_border_box_rect(&rows, row)))
        }));
        Ok(())
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
    let taken = std::mem::replace(
        presentation,
        FfiPresentation {
            presenter: std::ptr::null_mut(),
            sealed: std::ptr::null_mut(),
        },
    );
    // SAFETY: Guaranteed by the caller.
    let Some(taken) = (unsafe { Presentation::adopt(taken) }) else {
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
    let given_back = host.take_back_presentation().map_or(
        FfiPresentation {
            presenter: std::ptr::null_mut(),
            sealed: std::ptr::null_mut(),
        },
        Presentation::into_ffi,
    );
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
/// StyleLayout thread has run the jobs handed to it before, the tick among them, without ending the lease. For a test,
/// whose clock ticks only where it injects them.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_inject_clock_tick(host: &DocumentHost, frame_time_nanoseconds: i64) {
    if let Some(ticks) = host.clock_ticks() {
        ticks.tick(frame_time_nanoseconds);
        crate::stage_thread::style_layout_thread().run(|| ());
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
