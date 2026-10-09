/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A clock lane's hover: the element under the pointer, which the render clock hears of from the compositor beside the
//! mouse event the host takes, matches `:hover` in the frames the lane presents once a tick of the lane finds it there,
//! while the host runs a task.
//!
//! The pointer reaches the lane on the render clock's thread. A tick hit tests where it went in the frame the lane
//! presented last, with the scroll offsets and metrics the host sealed with the lane's plan, and moves the hover facts
//! of the lane's fork of the style engine to the element it finds and its shadow-including ancestors, as the host would
//! as it handles the move. The host hovers the move in its own state as it handles it, or as the lanes' report says.

use super::{Lane, RenderState, TickRecording, WaitsForTickRecording};
use crate::css::css_pixels::{CssPixelPoint, CssPixels};
use crate::css::style::hover_lane::{HoverBoxRebuild, HoverInstall, hover_row_generated_for};
use crate::css::style::style_job::SealedStyleInputs;
use crate::css::style::tree::StyleNodeID;
use crate::css::style_compute::FfiEffectTiming;
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::layout::tree_mutation::{HostCalls, OwedHostWork};
use crate::painting::ffi::FfiChromeMetrics;
use crate::painting::hit_test::{HitTestItemKind, HitTestList};
use crate::painting::host::FfiHitTestQueryCallbacks;
use crate::stage_thread::Riding;
use libgfx_rust::FloatPoint;
use std::sync::Arc;

/// What the host seals for a clock lane's hover with the plan, at the end of a rendering update.
#[repr(C)]
pub struct FfiHoverPlanInputs {
    pub device_pixels_per_css_pixel: f64,
    /// The scroll offsets of the frame the rendering update presented, in device pixels, by scroll frame.
    pub scroll_offsets: *const libgfx_rust::FloatPoint,
    pub scroll_offset_count: usize,
    pub chrome_metrics: FfiChromeMetrics,
    /// The cursor of the document's page, which the hover asks to show the cursor of what it hovers.
    pub page_cursor: FfiPageCursor,
    /// The timings the host runs the effects of elements on without sampling them, as the compositor runs them: the
    /// element's style node, the effect's identity, and the timing, an `FfiEffectTiming`, by effect.
    pub effect_timing_nodes: *const u32,
    pub effect_timing_identities: *const u64,
    pub effect_timings: *const std::ffi::c_void,
    pub effect_timing_count: usize,
}

/// The cursor of a page, which the host hands a hover with its plan: what the page asks its client to show, and the
/// calls that reach it from any thread. The cursor is retained for as long as a plan holds it.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiPageCursor {
    pub cursor: *const std::ffi::c_void,
    pub retain: Option<unsafe extern "C" fn(*const std::ffi::c_void)>,
    pub release: Option<unsafe extern "C" fn(*const std::ffi::c_void)>,
    /// Asks the page to show the cursor of a CSS predefined cursor, never `auto`.
    pub request: Option<unsafe extern "C" fn(*const std::ffi::c_void, u8)>,
}

/// The cursor of a page a plan holds a reference to.
struct PageCursor(FfiPageCursor);

// SAFETY: The page's cursor takes requests from any thread, and is retained and released atomically.
unsafe impl Send for PageCursor {}
// SAFETY: As above.
unsafe impl Sync for PageCursor {}

impl PageCursor {
    /// A reference to the cursor `ffi` names, or none where it names none.
    ///
    /// # Safety
    ///
    /// `ffi.cursor` must point at a live page cursor, which `ffi.retain` and `ffi.release` reference, or be null.
    unsafe fn retained(ffi: FfiPageCursor) -> Option<Self> {
        if ffi.cursor.is_null() || ffi.release.is_none() || ffi.request.is_none() {
            return None;
        }
        let retain = ffi.retain?;
        // SAFETY: Guaranteed by the caller.
        unsafe { retain(ffi.cursor) };
        Some(Self(ffi))
    }

    /// Asks the page to show the CSS predefined cursor `cursor`.
    fn request(&self, cursor: u8) {
        if let Some(request) = self.0.request {
            // SAFETY: The reference keeps the cursor live.
            unsafe { request(self.0.cursor, cursor) };
        }
    }
}

impl Drop for PageCursor {
    fn drop(&mut self) {
        if let Some(release) = self.0.release {
            // SAFETY: The reference was retained in `retained`, and is released once.
            unsafe { release(self.0.cursor) };
        }
    }
}

/// What a lane's ticks hit test and restyle with, sealed with the clock plan at the end of a rendering update.
pub(crate) struct HoverPlan {
    device_pixels_per_css_pixel: f64,
    /// The scroll offsets of the frame the rendering update presented, in device pixels, by scroll frame.
    scroll_offsets: Vec<FloatPoint>,
    chrome_metrics: FfiChromeMetrics,
    /// What the hover's style transactions take: the root and the document computation inputs of the host's last.
    style: Option<(StyleNodeID, Arc<SealedStyleInputs>)>,
    /// The cursor of the document's page.
    page_cursor: Option<PageCursor>,
    /// The timings the host runs effects on without sampling them, which the descriptions of their elements' effects
    /// hold from before: the step a hover decides over the transitions they run reads them.
    effect_timings: Vec<(StyleNodeID, u64, FfiEffectTiming)>,
}

impl HoverPlan {
    /// The plan `inputs` describes.
    ///
    /// # Safety
    ///
    /// `inputs.scroll_offsets` must point at `inputs.scroll_offset_count` offsets, or be null, the effect timing
    /// arrays at `inputs.effect_timing_count` entries each, or be null, and `inputs.page_cursor` must name a live page
    /// cursor, or none.
    pub(crate) unsafe fn from_ffi(inputs: &FfiHoverPlanInputs) -> Self {
        let scroll_offsets = if inputs.scroll_offsets.is_null() {
            Vec::new()
        } else {
            // SAFETY: Guaranteed by the caller.
            unsafe { std::slice::from_raw_parts(inputs.scroll_offsets, inputs.scroll_offset_count) }.to_vec()
        };
        let effect_timings = if inputs.effect_timing_count == 0 {
            Vec::new()
        } else {
            // SAFETY: Guaranteed by the caller.
            let (nodes, identities, timings) = unsafe {
                (
                    std::slice::from_raw_parts(inputs.effect_timing_nodes, inputs.effect_timing_count),
                    std::slice::from_raw_parts(inputs.effect_timing_identities, inputs.effect_timing_count),
                    std::slice::from_raw_parts(
                        inputs.effect_timings.cast::<FfiEffectTiming>(),
                        inputs.effect_timing_count,
                    ),
                )
            };
            nodes
                .iter()
                .zip(identities)
                .zip(timings)
                .filter_map(|((&node, &identity), timing)| Some((StyleNodeID::from_raw(node)?, identity, *timing)))
                .collect()
        };
        Self {
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
            scroll_offsets,
            chrome_metrics: inputs.chrome_metrics,
            style: None,
            // SAFETY: Guaranteed by the caller.
            page_cursor: unsafe { PageCursor::retained(inputs.page_cursor) },
            effect_timings,
        }
    }

    /// Has the hover's style transactions take `inputs`, the document computation inputs the host sealed for its last
    /// transaction under `root`.
    pub(crate) fn with_style_inputs(self, style: Option<(StyleNodeID, Arc<SealedStyleInputs>)>) -> Self {
        Self { style, ..self }
    }
}

/// Where the render clock heard the pointer went.
#[derive(Clone, Copy, Debug)]
pub(crate) struct PendingPointer {
    /// In the context's device pixels, or none where the pointer left the context.
    pub(crate) position: Option<FloatPoint>,
    /// The mouse buttons held.
    pub(crate) buttons: u32,
    /// Whether the compositor scrolled since the frame the lane presented last.
    pub(crate) scrolled_since_frame: bool,
    /// The input event id of the mouse event the host takes beside the move, or 0 where there is none.
    pub(crate) input_event_id: u64,
}

/// What the lanes want once they heard where the pointer went, as the render clock reads it.
#[repr(u8)]
pub(crate) enum PointerAnswer {
    /// The moves that follow, but no display tick: no lane hovers the move, which the host does.
    Moves = 1,
    /// Display ticks, the next of which may hover where the pointer went.
    Ticks = 2,
}

/// The pointer as the render clock's thread hands it to the lanes, until a tick takes it.
#[derive(Default)]
pub(crate) struct PointerState {
    pending: Option<PendingPointer>,
    /// Where the pointer went last, which the next lane hovers as it comes together.
    last: Option<PendingPointer>,
    /// The input event id of the newest mouse event the host handled.
    handled: u64,
}

impl PointerState {
    /// Has the next tick hover where the pointer went, where a lane `follows` the pointer, and answers what the lanes
    /// want next. A move older than a mouse event the host handled already is nobody's to hover: the hover never moves
    /// back to where an older event says the pointer was.
    pub(super) fn moved(&mut self, pointer: PendingPointer, follows: bool) -> PointerAnswer {
        if self.is_older_than_handled(pointer) {
            return PointerAnswer::Moves;
        }
        self.last = Some(pointer);
        // A move no lane follows is the host's, and so is the one an earlier move left waiting.
        if !follows {
            self.pending = None;
            return PointerAnswer::Moves;
        }
        self.pending = Some(pointer);
        PointerAnswer::Ticks
    }

    /// Where the pointer went last.
    pub(super) fn last(&self) -> Option<PendingPointer> {
        self.last
    }

    /// Takes the move that waits for a tick to hover it.
    pub(super) fn take(&mut self) -> Option<PendingPointer> {
        self.pending.take()
    }

    /// Whether a move waits for a tick to hover it.
    pub(super) fn waits(&self) -> bool {
        self.pending.is_some()
    }

    /// Notes that the host handled the mouse event with the input event id `input_event_id`, and forgets the move that
    /// waits for a tick, where it is older.
    pub(super) fn note_handled(&mut self, input_event_id: u64) {
        self.handled = self.handled.max(input_event_id);
        if self.pending.is_some_and(|pending| self.is_older_than_handled(pending)) {
            self.pending = None;
        }
    }

    /// Whether `pointer` is older than a mouse event the host handled. A move with id 0 matches no UI event.
    fn is_older_than_handled(&self, pointer: PendingPointer) -> bool {
        pointer.input_event_id != 0 && pointer.input_event_id < self.handled
    }
}

/// What a lane's hover did.
#[derive(Default)]
pub(crate) struct LaneHover {
    parked: bool,
    /// The element the hover moved the style's hover to, or nothing; none where it moved nothing.
    pub(crate) target: Option<Option<StyleNodeID>>,
    /// Where the pointer was as the hover moved it last, in the context's device pixels, or none where it left the
    /// context.
    pub(crate) pointer: Option<FloatPoint>,
    /// The rows the hover's transactions installed, the last one for each element, in the order the elements were
    /// first installed, whose records the engine keeps for the boxes that show them.
    pub(crate) installs: Vec<HoverInstall>,
    /// The element the pointer moved to last whose hover the lane left to the host, or none for the pointer leaving
    /// the document: the lane hovers nothing until the pointer moves to another.
    left_to_host: Option<Option<StyleNodeID>>,
}

impl LaneHover {
    pub(super) fn is_parked(&self) -> bool {
        self.parked
    }

    pub(super) fn park(&mut self) {
        self.parked = true;
    }

    /// Keeps `installs`, the last install of each element in place of an earlier one, whose records the engine lets go.
    fn keep_installs(&mut self, engine: &mut crate::css::style::StyleEngine, installs: Vec<HoverInstall>) {
        for install in installs {
            match self
                .installs
                .iter_mut()
                .find(|kept| kept.element.style_node == install.element.style_node)
            {
                Some(kept) => {
                    engine.release_hover_install(kept);
                    *kept = install;
                }
                None => self.installs.push(install),
            }
        }
    }
}

/// Where a hit test landed, as the host's handling of a mouse move finds its target.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum HoverTarget {
    /// Where the hit landed, on an element the host's handling would hover.
    Element(CursorHit),
    /// Nothing the host would hover, so the hover stays where it is.
    Unchanged,
}

/// What a hit shows the cursor of, as `EventHandler::update_cursor` reads it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct CursorHit {
    /// The box whose style admitted the hit, whose `cursor` shows.
    hit_node: NodeSlotId,
    /// The element the hit dispatches to.
    element: StyleNodeID,
    /// Whether the hit is in a text fragment.
    in_text: bool,
}

/// Whose style inputs a hover's transaction may take with it.
#[derive(Clone, Copy, PartialEq, Eq)]
enum HoverInputs {
    /// None but the hover's: the host left nothing pending.
    TheHostsAlone,
    /// Those a move the lane moved back left pending too.
    TheLanesToo,
}

/// What a move of the hover made, before it needed the host, if it did.
#[derive(Default)]
pub(super) struct MoveMade {
    /// Whether it installed anything in the boxes.
    installed: bool,
    /// The elements it started transitions on.
    started: smallvec::SmallVec<[StyleNodeID; 2]>,
    /// The transitions the hover started before that those it started replaced.
    replaced: Vec<super::effects::ElementEffects>,
}

/// What a lane's hover made of a pointer move.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum Hovered {
    /// It moved the hover, which the lane lays out and presents.
    Moved,
    /// The pointer stays over the element the hover is on: nothing is anybody's to do.
    Same,
    /// It moved nothing the lane presents, and the host hovers what is under the pointer as it handles the move.
    LeftToHost,
}

/// Why a hover moved nothing the lane presents.
enum HoverDeclined {
    /// The move needs the host, which hovers it as it handles the move: the lane hovers nothing more.
    Park(&'static str),
    /// The move hovers nothing new.
    Unmoved(&'static str),
    /// The move hovers the element the hover is on already.
    Same,
}

/// Whether a lane's hover tells what it did on the standard error, for a developer: LIBWEB_HOVER_LANE_LOG=1.
pub(in crate::render_state) fn logs_hover() -> bool {
    static LOGS: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *LOGS.get_or_init(|| std::env::var_os("LIBWEB_HOVER_LANE_LOG").is_some_and(|value| value == "1"))
}

/// The time a line of the hover's log tells, in milliseconds of the monotonic clock the compositor's frame times and the
/// host's shared current time read, within the last ten thousand seconds.
pub(in crate::render_state) fn log_time() -> String {
    format!("{:.3}", monotonic_milliseconds() % 10_000_000.0)
}

/// The monotonic clock's time, in milliseconds.
#[cfg(unix)]
fn monotonic_milliseconds() -> f64 {
    #[repr(C)]
    struct Timespec {
        seconds: i64,
        nanoseconds: i64,
    }
    unsafe extern "C" {
        fn clock_gettime(clock: i32, time: *mut Timespec) -> i32;
    }
    #[cfg(target_os = "macos")]
    const CLOCK_MONOTONIC: i32 = 6;
    #[cfg(not(target_os = "macos"))]
    const CLOCK_MONOTONIC: i32 = 1;
    let mut time = Timespec {
        seconds: 0,
        nanoseconds: 0,
    };
    // SAFETY: `time` is valid for writes.
    unsafe { clock_gettime(CLOCK_MONOTONIC, &raw mut time) };
    time.seconds as f64 * 1000.0 + time.nanoseconds as f64 / 1_000_000.0
}

/// The wall clock's time, in milliseconds, where no clock_gettime reads the monotonic clock.
#[cfg(not(unix))]
fn monotonic_milliseconds() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0.0, |time| time.as_secs_f64() * 1000.0)
}

/// The transform reference box of the box of the element `node` names, as the frame the lane presented last laid it
/// out, if it has one.
pub(super) fn transform_reference_box(
    arena: &crate::layout::LayoutNodeArena,
    node: StyleNodeID,
) -> Option<crate::css::css_pixels::CssPixelRect> {
    let row = arena.bound_row(node);
    if row.is_invalid() {
        return None;
    }
    crate::painting::ffi::committed_transform_reference_box(&arena.paintable_rows(), row)
}

impl Lane {
    /// Has the engine hold the timings of the animations the host runs as the host runs them, where it runs them without
    /// sampling them, as on the compositor: a tick samples them, and a hover's step decides over them, as they run.
    pub(super) fn refresh_host_effect_timings(&self, state: &mut RenderState) {
        let Some(plan) = self.plan.hover.as_ref() else {
            return;
        };
        let engine = state.engine_mut();
        for (node, identity, timing) in &plan.effect_timings {
            engine.refresh_element_animation_effect_timing(*node, *identity, timing);
        }
    }

    /// Hovers the element under `pointer`, and answers whether that moved anything to lay out and present.
    pub(super) fn hover(&mut self, state: &mut RenderState, pointer: PendingPointer, timestamp: f64) -> Hovered {
        // A scroll the host has not laid out moved what is under the pointer, and a held button drags or selects,
        // which only the host follows.
        let Some(plan) = self.plan.hover.take() else {
            return Hovered::LeftToHost;
        };
        let started = std::time::Instant::now();
        // A scroll of the compositor's since the frame moved what is under the pointer, which only a tick that said where
        // it scrolled to tells.
        let mut cursor_hit = None;
        let hovered = if pointer.scrolled_since_frame && self.scroll_offsets.is_empty() {
            Err(HoverDeclined::Unmoved("scrolled since the frame"))
        } else if pointer.buttons != 0 {
            Err(HoverDeclined::Unmoved("buttons held"))
        } else {
            self.hover_with(state, &plan, pointer, timestamp, &mut cursor_hit)
        };
        // The cursor shows what the pointer is over once the element the hover is on is the one under it, as the host
        // shows it as it handles the move. A move the host hovers leaves the cursor to it too.
        if matches!(hovered, Ok(()) | Err(HoverDeclined::Same))
            && let Some(page_cursor) = &plan.page_cursor
            && let Some(hit) = cursor_hit
            && let Some(cursor) = cursor_for_hit(state, hit)
        {
            page_cursor.request(cursor);
        }
        self.plan.hover = Some(plan);
        if logs_hover() {
            let outcome = match &hovered {
                Ok(()) => format!("hovered {:?}", self.hovered.target.flatten().map(StyleNodeID::raw)),
                Err(HoverDeclined::Unmoved(reason)) => format!("unmoved: {reason}"),
                Err(HoverDeclined::Same) => "unmoved: same element".to_string(),
                Err(HoverDeclined::Park(reason)) => format!("parked: {reason}"),
            };
            eprintln!(
                "{} hover lane: pointer {:?}: {outcome} in {} us",
                log_time(),
                pointer.position,
                started.elapsed().as_micros()
            );
        }
        match hovered {
            Ok(()) => Hovered::Moved,
            Err(HoverDeclined::Same) => Hovered::Same,
            Err(HoverDeclined::Unmoved(_)) => Hovered::LeftToHost,
            Err(HoverDeclined::Park(_)) => {
                self.hovered.park();
                Hovered::LeftToHost
            }
        }
    }

    fn hover_with(
        &mut self,
        state: &mut RenderState,
        plan: &HoverPlan,
        pointer: PendingPointer,
        timestamp: f64,
        cursor_hit: &mut Option<CursorHit>,
    ) -> Result<(), HoverDeclined> {
        let target = match pointer.position {
            Some(position) => match self.hit_test(state, plan, position)? {
                HoverTarget::Element(hit) => {
                    *cursor_hit = Some(hit);
                    Some(hit.element)
                }
                HoverTarget::Unchanged => return Err(HoverDeclined::Unmoved("nothing the host hovers")),
            },
            // The pointer left the document, which the host hovers nothing in once it handles the leave.
            None => None,
        };
        // A move to the element whose hover the lane left to the host leaves the hover where it is.
        if self.hovered.left_to_host == Some(target) {
            return Err(HoverDeclined::Unmoved("left to the host"));
        }
        self.hovered.left_to_host = None;
        let previous = (
            state.engine_mut().hover_target(),
            self.hovered.target,
            self.hovered.pointer,
        );
        let parked = self.parked;
        match self.move_hover_to(
            state,
            plan,
            target,
            pointer.position,
            timestamp,
            HoverInputs::TheHostsAlone,
        ) {
            // A move half made leaves nothing to move back.
            Err(HoverDeclined::Park(reason)) if self.frame_left_to_host => Err(HoverDeclined::Park(reason)),
            Err(HoverDeclined::Park(reason)) => {
                // The move needs the host, which hovers it as it handles the move. Moving the hover back to where it
                // was leaves the boxes as the screen shows them, and the lane hovers the moves after it.
                let (engine_target, target_before, pointer_before) = previous;
                if self
                    .move_hover_to(
                        state,
                        plan,
                        engine_target,
                        pointer_before,
                        timestamp,
                        HoverInputs::TheLanesToo,
                    )
                    .is_err()
                {
                    return Err(HoverDeclined::Park(reason));
                }
                if logs_hover() {
                    eprintln!("{} hover lane: move left to the host: {reason}", log_time());
                }
                self.parked = parked;
                self.hovered.target = target_before;
                self.hovered.left_to_host = Some(target);
                Err(HoverDeclined::Unmoved("left to the host"))
            }
            moved => moved,
        }
    }

    /// Moves the style's hover to `target`, as the pointer at `position` hovers it, and installs what that moves in the
    /// boxes. `inputs` says whether the style inputs pending may be the lane's own, which a hover it moved back left.
    /// A move that needs the host after it installed something leaves the fork holding a move half made, which no
    /// frame shows: the lane leaves its frame to the host, and forgets the transitions the move started.
    fn move_hover_to(
        &mut self,
        state: &mut RenderState,
        plan: &HoverPlan,
        target: Option<StyleNodeID>,
        position: Option<FloatPoint>,
        timestamp: f64,
        inputs: HoverInputs,
    ) -> Result<(), HoverDeclined> {
        let mut made = MoveMade::default();
        let moved = self.make_hover_move(state, plan, target, position, timestamp, inputs, &mut made);
        if matches!(moved, Err(HoverDeclined::Park(_))) && made.installed {
            self.frame_left_to_host = true;
            self.forget_transitions_started_at(&made.started, timestamp, std::mem::take(&mut made.replaced));
        } else if moved.is_ok() {
            self.unshown_move = Some((made, timestamp));
        }
        moved
    }

    /// Forgets the transitions the move the tick's hover made started, which the frame left to the host shows nothing
    /// of, and has the ticks sample those they replaced again: the host decides the move's transitions as it takes the
    /// hover in.
    pub(super) fn forget_unshown_move(&mut self) {
        if let Some((made, timestamp)) = self.unshown_move.take() {
            self.forget_transitions_started_at(&made.started, timestamp, made.replaced);
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn make_hover_move(
        &mut self,
        state: &mut RenderState,
        plan: &HoverPlan,
        target: Option<StyleNodeID>,
        position: Option<FloatPoint>,
        timestamp: f64,
        inputs: HoverInputs,
        made: &mut MoveMade,
    ) -> Result<(), HoverDeclined> {
        let Some((root, sealed_inputs)) = plan.style.as_ref() else {
            return Err(HoverDeclined::Park("no style inputs"));
        };
        let engine = state.engine_mut();
        if engine.hover_target() == target {
            return Err(HoverDeclined::Same);
        }
        // The hover moves with nothing else of the host's pending, which a transaction of the hover would take with it.
        if inputs == HoverInputs::TheHostsAlone && !engine.takes_hover_transactions() {
            return Err(HoverDeclined::Park("style work pending"));
        }
        engine.set_hover(target);
        self.hovered.target = Some(target);
        self.hovered.pointer = position;
        // The elements whose transitions the hover composes, which the waves after theirs settle the children of.
        let mut composed_beside = Default::default();
        let mut started_transitions = false;
        // The elements whose boxes the round builds again, across every wave.
        let mut rebuilt_elements: smallvec::SmallVec<[(StyleNodeID, HoverBoxRebuild); 2]> = smallvec::SmallVec::new();
        loop {
            let (engine, arena) = state.engine_and_arena();
            // SAFETY: The plan keeps the sealed inputs live.
            let mut wave = unsafe {
                engine.take_hover_wave(
                    *root,
                    sealed_inputs.inputs(),
                    &mut composed_beside,
                    arena.may_have_scroll_snap_areas(),
                    timestamp,
                    |node| transform_reference_box(arena, node),
                    |node| self.lane_transitions(node),
                )
            };
            if wave.rows.is_empty() {
                break;
            }
            // The box of an element whose step the wave decided over the transitions it ran shows the record the host
            // or the hover installed again, which its row moves it from, in place of a tick's sample of those
            // transitions.
            // So do the boxes of the descendants that inherited what those transitions animate, and the anonymous boxes
            // below all of them: the samples after the move are those of the transitions the step decides.
            let decided_over_running: smallvec::SmallVec<[StyleNodeID; 2]> = wave
                .transitions
                .iter()
                .filter(|(transitions, _)| transitions.decided_over_running)
                .map(|(transitions, _)| transitions.node)
                .collect();
            for node in decided_over_running {
                self.samples_restored |= self.show_host_styles_of_started(state, node);
            }
            // So does the box of an element that shows what it inherits of a sample of an element above it, with the
            // anonymous boxes below it: the samples after the move compose over the record the row installs.
            for (row, _) in wave.element_rows() {
                let Some(node) = StyleNodeID::from_raw(row.style_node) else {
                    continue;
                };
                let slot = state.arena.arena().bound_row(node);
                let shows_sample_over_old_record = !slot.is_invalid()
                    && self
                        .ticked
                        .iter()
                        .any(|(ticked, host_style)| *ticked == slot && host_style.record() == row.old_style_record);
                if shows_sample_over_old_record {
                    self.samples_restored |= self.show_host_style_of_inheriting(state, node);
                }
            }
            // An element whose move builds its boxes again is built again by the round, where a build of the boxes
            // around it builds them by itself.
            let mut rebuilds: smallvec::SmallVec<[(StyleNodeID, HoverBoxRebuild); 2]> = smallvec::SmallVec::new();
            let mut rebuilds_need_host = false;
            {
                let (engine, arena) = state.engine_and_arena();
                for (row, _) in wave.element_rows() {
                    if !arena.hover_row_builds_boxes_again(&row) {
                        continue;
                    }
                    let Some(node) = StyleNodeID::from_raw(row.style_node) else {
                        continue;
                    };
                    match engine.hover_box_rebuild(&row, !arena.bound_row(node).is_invalid()) {
                        Some(rebuild) => rebuilds.push((node, rebuild)),
                        None => rebuilds_need_host = true,
                    }
                }
            }
            let rebuilt = |node: u32| rebuilds.iter().any(|(rebuilt, _)| rebuilt.raw() == node);
            let arena = state.arena.arena();
            let boxes_take_wave = !rebuilds_need_host
                && wave.element_rows().all(|(row, pseudo_rows)| {
                    (rebuilt(row.style_node) || arena.takes_hover_row(&row, 0))
                        && pseudo_rows.iter().all(|pseudo| {
                            hover_row_generated_for(pseudo)
                                .is_some_and(|generated_for| arena.takes_hover_row(pseudo, generated_for))
                        })
                });
            // The host settles the `::before` and `::after` of an element, and of the descendants that inherit from it,
            // over the composition of its transitions, which their boxes show where they take an animated value.
            let pseudo_elements_take_transitions = {
                let (engine, arena) = state.engine_and_arena();
                wave.transitions.iter().any(|(transitions, _)| {
                    super::effects::pseudo_element_boxes_inherit(
                        engine,
                        arena,
                        transitions.node,
                        transitions.properties(),
                        &transitions.inheriting,
                        &wave.rows,
                    )
                })
            };
            let boxes_take_wave = boxes_take_wave && !pseudo_elements_take_transitions;
            // A row of an element whose transitions the hover started that owes no transition step leaves them to no
            // step the hover decides, which only the host decides then.
            let moves_transitions = wave.rows.iter().any(|row| {
                !row.owes_a_transition_step
                    && StyleNodeID::from_raw(row.style_node).is_some_and(|node| self.transitions_run_on(node))
            });
            // A move back to where a hover left to the host found the hover moves each element back to the record the
            // host holds, which its boxes show: nothing is the host's to do, and the boxes stay as they are.
            let settles_back = inputs == HoverInputs::TheLanesToo && {
                let (engine, arena) = state.engine_and_arena();
                wave.element_rows().all(|(row, pseudo_rows)| {
                    (engine.hover_row_returns_to_held_record(&row) || arena.hover_row_box_shows_its_record(&row, 0))
                        && pseudo_rows.iter().all(|pseudo| {
                            hover_row_generated_for(pseudo)
                                .is_none_or(|generated_for| arena.hover_row_box_shows_its_record(pseudo, generated_for))
                        })
                })
            };
            // The compositor runs the host's transitions of some properties itself, whose animations the lane stops
            // where a step ends the transitions, but not where a transition the step leaves running drives one alike.
            let compositor_animations_stop = wave
                .transitions
                .iter()
                .all(|(transitions, _)| super::effects::ended_compositor_animation_kinds(transitions).is_some());
            // A box shows the samples of one element's effects alone.
            let samples_overlap = {
                let started: smallvec::SmallVec<[&crate::css::transition::HoverTransitions; 2]> =
                    wave.transitions.iter().map(|(transitions, _)| transitions).collect();
                !started.is_empty() && self.samples_overlap(state, &started)
            };
            if !settles_back
                && (!wave.installable
                    || !boxes_take_wave
                    || moves_transitions
                    || !compositor_animations_stop
                    || samples_overlap)
            {
                if logs_hover() {
                    if let Some(refusal) = wave.refusal {
                        eprintln!("{} hover lane: wave refused: {refusal}", log_time());
                    }
                    if !compositor_animations_stop {
                        eprintln!(
                            "{} hover lane: wave refused: compositor animations a step ends and keeps alike",
                            log_time()
                        );
                    }
                    if samples_overlap {
                        eprintln!(
                            "{} hover lane: wave refused: an element inherits what another's samples animate",
                            log_time()
                        );
                    }
                    let engine = state.engine_mut();
                    for row in &wave.rows {
                        if let Some(refusal) = engine.hover_row_refusal(row) {
                            eprintln!(
                                "{} hover lane: row of style node {} (reaction {:#x}, plan {}, step {}, composed {}, \
                                 records {}->{}) refused: {refusal}",
                                log_time(),
                                row.style_node,
                                row.reaction,
                                row.owes_an_animation_plan,
                                row.owes_a_transition_step,
                                row.composed_by_the_host,
                                row.old_style_record,
                                row.new_style_record
                            );
                        }
                    }
                    let arena = state.arena.arena();
                    for row in wave.rows.iter().filter(|row| !rebuilt(row.style_node)) {
                        let refusal = match hover_row_generated_for(row) {
                            Some(generated_for) => arena.hover_row_box_refusal(row, generated_for),
                            None => None,
                        };
                        if let Some(refusal) = refusal {
                            eprintln!(
                                "{} hover lane: box of style node {} refused: {refusal}",
                                log_time(),
                                row.style_node
                            );
                        }
                    }
                }
                // What the wave answered is the host's to install, as is everything that follows from it: the boxes
                // show a move half made, which no tick presents until the host has made the rest.
                state.engine_mut().abandon_hover_wave(&mut wave);
                self.parked = true;
                return Err(HoverDeclined::Park(if wave.installable {
                    "a box the host styles"
                } else {
                    "a row the host installs"
                }));
            }
            let installs = state.engine_mut().install_hover_wave(&wave);
            made.installed |= !settles_back;
            let arena = state.arena.arena();
            let work = OwedHostWork::default();
            // The installs cannot call the host, which never hears of the boxes of the lane's fork they touched.
            arena.queue_box_presence();
            for install in installs.iter().filter(|_| !settles_back) {
                // The build of a box built again takes the record.
                if rebuilt(install.element.style_node) {
                    continue;
                }
                arena.install_hover_row(HostCalls(&work), &install.element, 0);
                for pseudo in &install.pseudo_elements {
                    if let Some(generated_for) = hover_row_generated_for(pseudo) {
                        arena.install_hover_row(HostCalls(&work), pseudo, generated_for);
                    }
                }
            }
            // NB: What the installs owe is the fork's, which no host pays: resolving it ends the queue of box presence.
            drop(work.resolve(arena));
            // The boxes show what the rows moved in place of the compositor animations the host published for them.
            if !settles_back {
                let (engine, arena) = state.engine_and_arena();
                for install in installs.iter().filter(|install| !rebuilt(install.element.style_node)) {
                    super::effects::stop_compositor_animations_a_row_moves(engine, arena, &install.element);
                }
            }
            if !settles_back {
                rebuilt_elements.extend(rebuilds);
            }
            // The transitions the rows start show as they start once every wave is in place, over the after-change
            // styles of the elements and of the descendants that inherit from them.
            for (transitions, sample) in std::mem::take(&mut wave.transitions) {
                state.engine_mut().unpin_layout_style_record(sample.record);
                if settles_back {
                    continue;
                }
                started_transitions = true;
                made.started.push(transitions.node);
                super::effects::stop_ended_compositor_animations(state.arena.arena(), &transitions);
                made.replaced.extend(self.start_transitions(transitions, timestamp));
            }
            let engine = state.engine_mut();
            self.hovered.keep_installs(engine, installs);
            if !engine.has_pending_transaction() {
                break;
            }
        }
        state.engine_mut().end_hover_transaction();
        if made.installed
            && let Err(super::Park(reason)) = self.find_inheriting_again(state)
        {
            if logs_hover() {
                eprintln!("{} hover lane: move left to the host: {reason}", log_time());
            }
            self.parked = true;
            return Err(HoverDeclined::Park(reason));
        }
        // The build of an element built again reads the records the hover moved it and the elements below it to, in
        // place of those the host holds, which the engine derives once the transactions have settled. What only the host
        // builds leaves the move for it to finish, and no tick presents it until then.
        let mut marks: smallvec::SmallVec<[(StyleNodeID, super::TickRebuild); 2]> = smallvec::SmallVec::new();
        for (node, rebuild) in rebuilt_elements {
            if state.engine_mut().show_boxes_a_hover_builds(node, rebuild).is_err() {
                if logs_hover() {
                    eprintln!(
                        "{} hover lane: boxes of style node {} left to the host: boxes only the host builds",
                        log_time(),
                        node.raw()
                    );
                }
                self.parked = true;
                return Err(HoverDeclined::Park("a box the host builds"));
            }
            let (root, rebuild) = match rebuild {
                HoverBoxRebuild::GainsABox { parent } => (node, super::TickRebuild::Insert { parent }),
                HoverBoxRebuild::LosesItsBox { parent } => (node, super::TickRebuild::TakeAway { parent }),
                HoverBoxRebuild::BoxesMove => (node, super::TickRebuild::Again),
                // The parent's box lays its children out in other runs, which only a build of them all makes.
                HoverBoxRebuild::PlaceMoves { parent } => (parent, super::TickRebuild::Again),
            };
            marks.push((root, rebuild));
        }
        let builds_boxes = !marks.is_empty();
        let marked = self.mark_all_for_tree_build(state, marks);
        if let Err(super::Park(reason)) = marked {
            if logs_hover() {
                eprintln!("{} hover lane: boxes left to the host: {reason}", log_time());
            }
            self.parked = true;
            return Err(HoverDeclined::Park("a box the host builds"));
        }
        // A transition the hover cannot show as it starts is the host's, with the move that starts it. A move that builds
        // boxes again shows its transitions once the tick built them.
        if started_transitions
            && !builds_boxes
            && let Err(super::Park(reason)) = self.sample_started_transitions(state, timestamp)
        {
            if logs_hover() {
                eprintln!("{} hover lane: transitions left to the host: {reason}", log_time());
            }
            self.parked = true;
            return Err(HoverDeclined::Park(reason));
        }
        Ok(())
    }

    /// Hit tests `position`, in device pixels, in the frame the lane presented last.
    fn hit_test(
        &mut self,
        state: &mut RenderState,
        plan: &HoverPlan,
        position: FloatPoint,
    ) -> Result<HoverTarget, HoverDeclined> {
        // The hit-test list is the one of the frame the lane presented last, which comes back with its recording.
        let recorder = self.recording.0.take(WaitsForTickRecording(()));
        let published = recorder.recorder.published_hit_test_items.clone();
        self.recording = TickRecording(Riding::landed(recorder));
        let published = published.ok_or(HoverDeclined::Unmoved("no hit-test list"))?;
        let arena = state.arena.arena();
        let tree = arena
            .paint_state()
            .borrow()
            .visual_context
            .tree
            .clone()
            .ok_or(HoverDeclined::Unmoved("no visual context tree"))?;
        // The items name the visual contexts of the tree they were recorded against, which a hit test of the host's may
        // have built anew since without presenting it.
        if published.structural_epoch != tree.structural_epoch {
            return Err(HoverDeclined::Unmoved("visual contexts the frame does not show"));
        }
        let mut list = HitTestList {
            items: std::sync::Arc::clone(&published.items),
            ..HitTestList::default()
        };
        list.build_spatial_indexes_if_needed();
        let scroll_offsets = self.scroll_offsets_on_screen(arena, plan);
        let callbacks =
            FfiHitTestQueryCallbacks::sealed(plan.device_pixels_per_css_pixel, &scroll_offsets, plan.chrome_metrics);
        let pixel_ratio = plan.device_pixels_per_css_pixel as f32;
        let point = CssPixelPoint::new(
            CssPixels::nearest_value_for_f32(position.x / pixel_ratio),
            CssPixels::nearest_value_for_f32(position.y / pixel_ratio),
        );
        let Some(topmost) = list.find_topmost_item(arena, &tree, &callbacks, point) else {
            return Ok(HoverTarget::Unchanged);
        };
        let item = &list.items[topmost.index];
        // A scrollbar or a resizer hovers nothing the host would hover without asking it.
        if item.kind == HitTestItemKind::ChromeWidget {
            return Err(HoverDeclined::Unmoved("a scrollbar or resizer"));
        }
        use crate::painting::paint_read::GeometryRead;
        let paintable = item.paintable;
        let paintable_kind = arena
            .node_kind_if_live(paintable)
            .ok_or(HoverDeclined::Unmoved("a box that is gone"))?;
        // Content of another navigable takes the move, which leaves this document's hover where it is.
        if paintable_kind == NodeKind::NavigableContainerViewport {
            return Ok(HoverTarget::Unchanged);
        }
        // An image map's areas are the host's to hit test.
        if paintable_kind == NodeKind::ImageBox && arena.image_map_areas().has_areas(paintable) {
            return Err(HoverDeclined::Unmoved("an image map"));
        }
        let resolved = list.resolve_hit(arena, topmost.index, topmost.local_point);
        let identity = if paintable_kind == NodeKind::Viewport {
            // The viewport stands for the document, and a hit that falls through to it hits the root element, the root
            // of the style transactions the hover takes its own with. The cursor is the viewport's, as the host's is.
            let &(root, _) = plan.style.as_ref().ok_or(HoverDeclined::Unmoved("no root element"))?;
            return Ok(HoverTarget::Element(CursorHit {
                hit_node: paintable,
                element: root,
                in_text: false,
            }));
        } else if !resolved.dispatch.is_none() {
            resolved.dispatch
        } else {
            resolved.fallback_dispatch
        };
        if identity.is_none() || identity.is_document {
            return Ok(HoverTarget::Unchanged);
        }
        let node = StyleNodeID::from_raw(identity.style_node).ok_or(HoverDeclined::Unmoved("no style node"))?;
        // The host walks from the box whose style admitted the hit: a text fragment's parent box, or the box hit.
        let hit_node = item.hit_node;
        let in_text = resolved.is_text_fragment;
        self.element_for_dispatch(state, hit_node, node)
            .map(|element| {
                HoverTarget::Element(CursorHit {
                    hit_node,
                    element,
                    in_text,
                })
            })
            .ok_or(HoverDeclined::Unmoved("no element the host dispatches to"))
    }

    /// The device scroll offsets of the visual context tree's nodes as the screen shows them: those of the frame the
    /// host laid out, where the compositor scrolled the scroll containers to at the latest tick, as the hit test reads
    /// them.
    fn scroll_offsets_on_screen(&self, arena: &crate::layout::LayoutNodeArena, plan: &HoverPlan) -> Vec<FloatPoint> {
        let mut offsets = plan.scroll_offsets.clone();
        if self.scroll_offsets.is_empty() {
            return offsets;
        }
        let paint_state = arena.paint_state().borrow();
        let visual_context = &paint_state.visual_context;
        let Some(tree) = visual_context.tree.as_deref() else {
            return offsets;
        };
        let scale = plan.device_pixels_per_css_pixel as f32;
        for slot in 0..visual_context.scroll_state.slot_count() {
            let scroller = visual_context.scroll_state.state_at_slot(slot);
            if scroller.is_sticky || !arena.paintable_row_is_populated(scroller.paintable) {
                continue;
            }
            // The compositor names a scroll container by the identity of its node, as the recording told it.
            let identity = arena.live_paintable_data(scroller.paintable).node_identity;
            let Some(scrolled) = self.scroll_offsets.iter().find(|offset| offset.scroller == identity) else {
                continue;
            };
            let index = scroller.node_index.0 as usize;
            if offsets.len() <= index {
                offsets.resize(index + 1, FloatPoint::default());
            }
            offsets[index] = FloatPoint {
                x: -(scrolled.x as f32) * scale,
                y: -(scrolled.y as f32) * scale,
            };
        }
        // https://drafts.csswg.org/css-position/#sticky-pos
        tree.resolve_sticky_offsets_in_place(&mut offsets);
        offsets
    }

    /// The element the host dispatches a move at `node`, hit in `paintable`, to: the generator of a pseudo-element's
    /// box, and the nearest element for a text node. None where the host dispatches nothing, under a disabled form
    /// control.
    fn element_for_dispatch(
        &self,
        state: &mut RenderState,
        paintable: NodeSlotId,
        node: StyleNodeID,
    ) -> Option<StyleNodeID> {
        use crate::painting::paint_read::{GeometryRead, PaintRow};
        let arena = state.arena.arena();
        let is_anonymous = |row: NodeSlotId| arena.node_flags_if_live(row) & NodeFlag::Anonymous as u32 != 0;
        let mut row = paintable;
        let mut node = node;
        // A box generated for `::before`, `::after` or `::backdrop` stands for the element that generates it, whose
        // style node it carries.
        if is_anonymous(row)
            && arena.node(row).is_some_and(|data| data.generated_for() != 0)
            && let Some(generator) = arena.node_style_node(row)
        {
            node = generator;
        }
        // A text node stands for the nearest element its box is in, starting with the box that admitted the hit.
        while node.text_index().is_some() {
            if !is_anonymous(row)
                && let Some(row_node) = arena.dom_node_style_node(row)
                && row_node != node
            {
                node = row_node;
                continue;
            }
            row = arena.node_parent_if_live(row)?;
        }
        state.engine_mut().element_for_hover_dispatch(node)
    }
}

/// The CSS predefined cursor the host shows over `hit` as it handles a move there (see `EventHandler::update_cursor`):
/// the first of the hit box's `cursor` values, where `auto` shows the text cursor over the editable element the hit
/// dispatches to and over text that may be selected, and the default cursor otherwise. None where only the host
/// resolves the cursor: an image cursor, or text whose selection only the host decides.
fn cursor_for_hit(state: &mut RenderState, hit: CursorHit) -> Option<u8> {
    use crate::css::css_enums::cursor_predefined;
    use crate::painting::paint_read::{GeometryRead, PaintRow};
    let (engine, arena) = state.engine_and_arena();
    let boxed = !arena.bound_row(hit.element).is_invalid();
    let style = arena.node(hit.hit_node)?.style()?;
    let cursor = style.inherited_ui().cursor.as_slice().first()?;
    // An image cursor shows the image the host decoded.
    if cursor.is_cursor_value {
        return None;
    }
    let shows_text = if engine.hover_target_is_editable(hit.element) {
        true
    } else if !hit.in_text || !boxed {
        false
    } else if arena.node_flags_if_live(hit.hit_node) & NodeFlag::IsInUserAgentShadowTree as u32 != 0 {
        // A text of a user agent shadow tree may be selected as the form control it is in decides, which only the
        // host reads.
        return None;
    } else {
        engine.hover_text_may_be_selected(hit.element)?
    };
    // An element without a box shows the default cursor, unless it shows the text cursor.
    if !shows_text && !boxed {
        return Some(cursor_predefined::DEFAULT);
    }
    Some(match cursor.predefined {
        cursor_predefined::AUTO if shows_text => cursor_predefined::TEXT,
        cursor_predefined::AUTO => cursor_predefined::DEFAULT,
        predefined => predefined,
    })
}

/// Hands the clock lane `ticks` belong to where the pointer went: to `x`, `y` in device pixels where `has_position`,
/// or out of the context, beside the mouse event with the input event id `input_event_id`, and answers what the lane
/// wants next, as a `PointerAnswer`.
///
/// # Safety
///
/// `ticks` must come from `document_host_lease_clock` and not be released yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn clock_ticks_pointer_moved(
    ticks: *const super::ClockTicks,
    has_position: bool,
    x: f32,
    y: f32,
    buttons: u32,
    scrolled_since_frame: bool,
    input_event_id: u64,
) -> u8 {
    // SAFETY: Guaranteed by the caller, whose reference this borrows.
    let ticks = std::mem::ManuallyDrop::new(unsafe { std::sync::Arc::from_raw(ticks) });
    ticks.pointer_moved(PendingPointer {
        position: has_position.then_some(FloatPoint { x, y }),
        buttons,
        scrolled_since_frame,
        input_event_id,
    }) as u8
}

/// Hands the clock lanes of `host`'s document a pointer move to `x`, `y` in device pixels, as the compositor would,
/// beside the mouse event with the input event id `input_event_id`, or none for 0, which the next tick hovers. For a
/// test, whose clock ticks only where it injects them.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_move_pointer(
    host: &crate::render_state::DocumentHost,
    x: f32,
    y: f32,
    input_event_id: u64,
) {
    let _ = host.clock_ticks().pointer_moved(PendingPointer {
        position: Some(FloatPoint { x, y }),
        buttons: 0,
        scrolled_since_frame: false,
        input_event_id,
    });
}

/// Hands the clock lanes of `host`'s document a pointer move to `x`, `y` in device pixels, beside the mouse event with
/// the input event id `input_event_id`, or none for 0, ticks them at `frame_time_nanoseconds`, and waits until the
/// StyleLayout thread has run the tick and the Paint thread the recording of the frame it presents. For a test.
///
/// # Safety
///
/// `host` must come from `document_host_create` and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_inject_pointer(
    host: &crate::render_state::DocumentHost,
    x: f32,
    y: f32,
    frame_time_nanoseconds: i64,
    input_event_id: u64,
) {
    super::settle_lanes_for_testing();
    host.clock_ticks().inject_pointer(
        PendingPointer {
            position: Some(FloatPoint { x, y }),
            buttons: 0,
            scrolled_since_frame: false,
            input_event_id,
        },
        frame_time_nanoseconds,
    );
    super::settle_lanes_for_testing();
}
