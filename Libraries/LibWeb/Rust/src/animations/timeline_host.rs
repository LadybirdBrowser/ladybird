/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The timelines associated with a document: what the document keeps of them, what moves them, and what they ask of
//! the document and its layout.

use super::animation::AnimationCell;
use super::document_animations::DocumentAnimations;
use super::effect::EffectFlag;
use super::ffi::{FfiKeepAlive, FfiScrollTimelineSource, FfiTimelineDescription, HostEffectPerformer, run_procedure};
use super::procedures::update_leaves_animation_as_it_is;
use super::rendering_trace::note_rendering_trace_event;
use super::throttling::can_skip_per_frame_animation_tick;
use super::timeline::{ScrollTimelineBoxes, TimelineCell, TimelineHandle, TimelineKind, TimelineUpdate};
use super::timing::TimeValue;
use crate::css::style::tree::StyleNodeID;
use crate::layout::node_data::NodeSlotId;
use crate::render_state::{BegunRead, DocumentHost};
use std::ffi::c_void;
use std::ptr::NonNull;

/// The now timestamp of a document, the task generation of its event loop, and the time a new document timeline of
/// the document starts at: the timestamp of its last animation frame, or before it has one, the time navigation to it
/// started.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiDocumentClock {
    pub now: f64,
    pub task_generation: u64,
    pub timeline_start_time: f64,
}

/// The boxes a scroll timeline follows, as the host found them from its source in the render state of `host`'s
/// document, which it reads in `read`: the box of the scroll container, if any, the viewport's where the source is the
/// root element, its scroll node, where `has_scroll_node`, and the style node of the element whose box scrolls, or 0
/// for the viewport. For a view timeline, the box of its subject, if any, and the style node by which a clock tick finds
/// it, or 0 where the subject is a pseudo-element. `host` and `read` are null where the source has no box.
#[repr(C)]
pub struct FfiScrollTimelineBoxes {
    pub host: *const DocumentHost,
    pub read: *const BegunRead,
    pub scroller: NodeSlotId,
    pub has_scroll_node: bool,
    pub scroll_node: super::timeline::ScrollNodeStableId,
    pub scroller_style_node: u32,
    pub subject: NodeSlotId,
    pub subject_style_node: u32,
}

unsafe extern "C" {
    /// What the host's `DOM::Document` `document` knows of animations.
    fn web_animation_document_animations(document: NonNull<c_void>) -> NonNull<c_void>;
    /// The clock of the host's `DOM::Document` `document`.
    fn web_animation_document_clock(document: NonNull<c_void>) -> FfiDocumentClock;
    /// Finds the boxes the scroll timeline of `source`, a view timeline's where `is_view`, follows, and calls `measure`
    /// with `context` and them, while it reads the render state they are in.
    fn web_animation_find_scroll_timeline_boxes(
        source: &FfiScrollTimelineSource,
        is_view: bool,
        context: *mut c_void,
        measure: unsafe extern "C" fn(context: *mut c_void, boxes: &FfiScrollTimelineBoxes),
    );
    /// The host's `DOM::Element` that is the source of the scroll timeline of `source`, as script observes it, a view
    /// timeline's where `is_view`, or null.
    fn web_animation_scroll_timeline_source(source: &FfiScrollTimelineSource, is_view: bool) -> *mut c_void;
}

/// What the host's `DOM::Document` `document` knows of animations, which lives as long as the document.
///
/// # Safety
/// The document must be live, and the caller must not hold the reference across anything that reaches it again.
pub(crate) unsafe fn document_animations<'a>(document: NonNull<c_void>) -> &'a mut DocumentAnimations {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        web_animation_document_animations(document)
            .cast::<DocumentAnimations>()
            .as_mut()
    }
}

fn document_clock(document: NonNull<c_void>) -> FfiDocumentClock {
    // SAFETY: The document of a timeline that is updated or observed is live.
    unsafe { web_animation_document_clock(document) }
}

/// What updating the animations associated with a timeline asks of the host: to keep each animation alive while the
/// update lasts, and to perform what the procedures that update it ask for.
pub(crate) struct TimelineUpdateHost<'a> {
    pub(crate) keep_alive: &'a dyn Fn(&AnimationCell),
    pub(crate) perform: HostEffectPerformer,
}

/// The host's side of an update of the animations associated with a timeline, which hands `keep_alive` `context`.
#[repr(C)]
pub struct FfiTimelineAnimationsHost {
    pub context: *mut c_void,
    pub keep_alive: FfiKeepAlive,
    pub perform: HostEffectPerformer,
}

impl FfiTimelineAnimationsHost {
    fn with<R>(&self, run: impl FnOnce(&TimelineUpdateHost<'_>) -> R) -> R {
        let keep_alive = |animation: &AnimationCell| {
            // SAFETY: The host keeps alive an animation it is handed for as long as the call that handed it lasts.
            unsafe { (self.keep_alive)(self.context, animation) };
        };
        run(&TimelineUpdateHost {
            keep_alive: &keep_alive,
            perform: self.perform,
        })
    }
}

extern "C" fn perform_nothing(_: &super::ffi::FfiHostEffect) {
    unreachable!("a new timeline has no associated animations to update");
}

/// https://drafts.csswg.org/web-animations-1/#animation-frame-loop
/// Updates the animations associated with `timeline` as its current time changed.
fn update_associated_animations(timeline: &TimelineCell, host: &TimelineUpdateHost<'_>) {
    // Note: Due to the hierarchical nature of the timing model, updating the current time of a timeline also involves:
    // - Updating the current time of any animations associated with the timeline.
    // - Running the update an animation's finished state procedure for any animations whose current time has been
    //   updated.
    // - Queueing animation events for any such animations.
    // NB: Since we dispatch events for all animations regardless of whether they have a timeline we handle them all
    //     together in the host's update of the animations of its document.
    note_rendering_trace_event("animation timeline updated its animations");
    // NB: Updating an animation can run script that changes which animations are associated with this timeline. Skip
    //     the ones it disassociates. The host keeps the animations alive through it.
    // OPTIMIZATION: Only the animations an update may change are updated, in the order they are associated with the
    //               timeline. The others stay as they are until a procedure runs on them, which notes that they need
    //               updates again.
    let animations = timeline.associated_animations_needing_update();
    for animation in &animations {
        (host.keep_alive)(animation);
    }
    for animation in &animations {
        let is_associated = timeline.associated_animations().borrow().contains(animation);
        if is_associated {
            run_procedure(animation, host.perform, |procedures| procedures.update());
        }
        if !is_associated || update_leaves_animation_as_it_is(animation) {
            timeline.animations_needing_update().borrow_mut().remove(animation);
        }
    }
}

/// What measuring the boxes a scroll timeline follows hands back.
struct LayoutUpdate<'a> {
    timeline: &'a TimelineCell,
    task_generation: u64,
    only_if_stale: bool,
    host: &'a TimelineUpdateHost<'a>,
    updated: bool,
}

unsafe extern "C" fn update_from_boxes(context: *mut c_void, boxes: &FfiScrollTimelineBoxes) {
    // SAFETY: The context is the update that asked the host to find the boxes.
    let update = unsafe { &mut *context.cast::<LayoutUpdate<'_>>() };
    let timeline = update.timeline;
    let subject_style_node = StyleNodeID::from_raw(boxes.subject_style_node);
    // SAFETY: The host hands a live document host and a read it began of its render state, or nulls.
    let (scroller, subject) = match unsafe { (boxes.host.as_ref(), boxes.read.as_ref()) } {
        (Some(host), Some(read)) => {
            let found = ScrollTimelineBoxes {
                scroller: boxes.scroller,
                scroll_node: boxes.has_scroll_node.then_some(boxes.scroll_node),
                scroller_style_node: boxes.scroller_style_node,
                subject: boxes.subject,
            };
            let record = timeline.borrow();
            let scroll = record.scroll().expect("the host lays out scroll timelines");
            host.ask(read, |state| {
                let arena = state.arena_mut();
                arena.settle_scrollable_overflow();
                scroll.measure(arena, &found)
            })
        }
        _ => (None, None),
    };
    update.updated = timeline.update_current_time(
        TimelineUpdate::Layout {
            scroller: scroller.as_ref(),
            subject: subject.as_ref(),
            subject_style_node,
        },
        update.task_generation,
        update.only_if_stale,
        || update_associated_animations(timeline, update.host),
    );
}

/// The source of the scroll timeline `timeline` as the host names it, and whether it is a view timeline.
fn host_source(timeline: &TimelineCell) -> (FfiScrollTimelineSource, bool) {
    let record = timeline.borrow();
    let scroll = record.scroll().expect("a scroll timeline has a source");
    (scroll.source.into(), scroll.view.is_some())
}

/// Updates the current time of the scroll timeline `timeline` from where the render state lays out the boxes it
/// follows, unless that leaves it as it was where `only_if_stale`, answering whether it updated it.
fn update_current_time_from_layout(
    timeline: &TimelineCell,
    only_if_stale: bool,
    host: &TimelineUpdateHost<'_>,
) -> bool {
    let task_generation = document_clock(timeline.document()).task_generation;
    let (source, is_view) = host_source(timeline);
    let mut update = LayoutUpdate {
        timeline,
        task_generation,
        only_if_stale,
        host,
        updated: false,
    };
    // NB: The host measures the boxes the timeline follows where the render state of the document whose viewport or
    //     element the source propagates to lays them out.
    // SAFETY: The host calls back synchronously with the update.
    unsafe {
        web_animation_find_scroll_timeline_boxes(&source, is_view, (&raw mut update).cast(), update_from_boxes);
    }
    update.updated
}

/// Updates the current time of `timeline`: a document timeline's from the now timestamp `timestamp`, and a scroll
/// timeline's from the layout of its scroller, which ignores the timestamp, unless that leaves it as it was where
/// `only_if_stale`. Answers whether the timeline was updated.
pub(crate) fn update_current_time(
    timeline: &TimelineCell,
    timestamp: f64,
    only_if_stale: bool,
    host: &TimelineUpdateHost<'_>,
) -> bool {
    // https://drafts.csswg.org/scroll-animations-1/#ref-for-dom-animationtimeline-currenttime
    // NB: The scroll progress is calculated from the layout of the scroller. The passed timestamp is ignored.
    if timeline.borrow().scroll().is_some() {
        return update_current_time_from_layout(timeline, only_if_stale, host);
    }
    let task_generation = document_clock(timeline.document()).task_generation;
    timeline.update_current_time(TimelineUpdate::Timestamp(timestamp), task_generation, false, || {
        update_associated_animations(timeline, host);
    })
}

/// Whether the associated effect of an animation associated with `timeline` goes without a per-frame tick.
fn has_animation_without_per_frame_tick(timeline: &TimelineCell) -> bool {
    // NB: Script reads the current time often, so stop at the first one.
    timeline.associated_animations().borrow().iter().any(|animation| {
        let effect = animation.borrow().effect().cloned();
        effect.is_some_and(|effect| {
            effect.borrow().has_flag(EffectFlag::PerFrameAnimationTickWasSkipped)
                || can_skip_per_frame_animation_tick(&effect)
        })
    })
}

/// https://drafts.csswg.org/web-animations-1/#dom-animationtimeline-currenttime
/// The current time of `timeline` as script observes it.
pub(crate) fn current_time_for_observation(timeline: &TimelineCell) -> Option<TimeValue> {
    let can_sample = {
        let record = timeline.borrow();
        record.is_monotonically_increasing() && record.can_sample_current_time_at_timestamp()
    };
    if !can_sample || !has_animation_without_per_frame_tick(timeline) {
        return timeline.current_time();
    }
    let clock = document_clock(timeline.document());
    let (observed_in, observed_current_time) = timeline.borrow().observed_in_task_generation();
    if observed_in == Some(clock.task_generation) {
        return observed_current_time;
    }

    // OPTIMIZATION: An animation skipped by the per-frame sampler does not keep the rendering update loop alive.
    //               Sample document timelines only when script observes them, and at most once per event-loop task
    //               so all reads within a stable state agree.
    let current_time = timeline.current_time_at_timestamp(clock.now, Some(clock.task_generation));
    note_rendering_trace_event("animation timeline synchronized");
    current_time
}

/// The host's `DOM::Element` that is the source of the scroll timeline `timeline` as script observes it, or none.
pub(crate) fn scroll_timeline_source(timeline: &TimelineCell) -> Option<NonNull<c_void>> {
    let (source, is_view) = host_source(timeline);
    // SAFETY: The elements the source names are live while the timeline's object is.
    NonNull::new(unsafe { web_animation_scroll_timeline_source(&source, is_view) })
}

/// Associates the new timeline `timeline` with its document, and gives it its first current time.
pub(crate) fn associate_new_timeline(timeline: &TimelineHandle) {
    // SAFETY: The document of a new timeline is live, and the borrow ends here.
    unsafe { document_animations(timeline.document()) }
        .timelines
        .push(timeline.clone());
    let keep_alive = |_: &AnimationCell| {};
    let host = TimelineUpdateHost {
        keep_alive: &keep_alive,
        perform: perform_nothing,
    };
    let timestamp = match timeline.borrow().kind {
        // NB: The document hasn't processed an animation frame yet, we use the navigation start time, which is either
        //     the time that the previous document started to be unloaded or the creation time of the current document.
        TimelineKind::Document { .. } => document_clock(timeline.document()).timeline_start_time,
        // NB: The passed timestamp is ignored for ScrollTimelines so we can just pass 0 here.
        TimelineKind::Scroll(_) => 0.0,
        TimelineKind::Internal { .. } => return,
    };
    update_current_time(timeline, timestamp, false, &host);
}

/// Sets the current time of the Internals interface's timeline `timeline` to `time`, in milliseconds, or, where
/// `for_observation`, the time a read of it observes, updating its associated animations, which `keep_alive` keeps
/// alive.
pub(crate) fn set_internal_time(
    timeline: &TimelineCell,
    time: Option<f64>,
    for_observation: bool,
    keep_alive: &dyn Fn(&AnimationCell),
    perform: HostEffectPerformer,
) {
    let task_generation = document_clock(timeline.document()).task_generation;
    let host = TimelineUpdateHost { keep_alive, perform };
    timeline.set_internal_time(time, for_observation, task_generation, || {
        update_associated_animations(timeline, &host);
    });
}

/// https://www.w3.org/TR/web-animations-1/#document-default-document-timeline
/// The default document timeline of the host's `DOM::Document` `document`.
pub(crate) fn default_document_timeline(document: NonNull<c_void>) -> TimelineHandle {
    // SAFETY: The document is live, and the borrow ends before the timeline is associated with it.
    if let Some(timeline) = &unsafe { document_animations(document) }.default_timeline {
        return timeline.clone();
    }
    // The DocumentTimeline object representing the default document timeline. The default document timeline has an
    // origin time of zero.
    let timeline = TimelineHandle::new(document, TimelineKind::Document { origin_time: 0.0 });
    // SAFETY: As above.
    unsafe { document_animations(document) }.default_timeline = Some(timeline.clone());
    associate_new_timeline(&timeline);
    timeline
}

/// A new timeline associated with the host's `DOM::Document` `document`, of `kind`.
pub(crate) fn create_timeline(document: NonNull<c_void>, kind: TimelineKind) -> TimelineHandle {
    let timeline = TimelineHandle::new(document, kind);
    associate_new_timeline(&timeline);
    timeline
}

impl DocumentAnimations {
    /// Runs `run` with the timelines associated with the document at the times script observed of them: all of them
    /// where `all`, or else those script observed ahead of their current time in this task.
    pub(crate) fn at_observed_times<R>(this: *mut Self, all: bool, run: impl FnOnce() -> R) -> R {
        // SAFETY: The caller passes what a live document knows of animations, and the borrow ends here.
        let timelines = unsafe { &*this }.timelines.clone();
        let Some(first) = timelines.first() else {
            return run();
        };
        let task_generation = document_clock(first.document()).task_generation;
        let mut overridden = Vec::new();
        for timeline in &timelines {
            if !all && timeline.borrow().observed_ahead_in_task_generation() != Some(task_generation) {
                continue;
            }
            let previous = timeline.borrow().style_sampling_override();
            let current_time = current_time_for_observation(timeline);
            timeline.borrow_mut().set_style_sampling_override(Some(current_time));
            overridden.push((timeline, previous));
        }
        let result = run();
        for (timeline, previous) in overridden.into_iter().rev() {
            timeline.borrow_mut().set_style_sampling_override(previous);
        }
        result
    }
}

/// Which of the timelines of a document an update moves.
#[expect(dead_code, reason = "C++ constructs the variants")]
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiTimelinesUpdate {
    /// All of them, to the timestamp.
    All,
    /// The scroll timelines, from the layout of their scrollers.
    ScrollTimelines,
    /// The scroll timelines whose scrollers' layout changed them since their last update.
    StaleScrollTimelines,
}

/// Updates the current time of the timelines associated with a document whose animations are `animations`, as `update`
/// has it, a document timeline's to the now timestamp `timestamp`, updating their associated animations through
/// `host`, and answers whether that updated any.
///
/// # Safety
/// `animations` must be what a live document knows of animations, which the update may reach.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_update_timelines(
    animations: *mut DocumentAnimations,
    update: FfiTimelinesUpdate,
    timestamp: f64,
    host: &FfiTimelineAnimationsHost,
) -> bool {
    // NB: Updating a timeline can run script, which can associate new timelines with the document.
    // SAFETY: Guaranteed by the caller, and the borrow ends here.
    let timelines = unsafe { &*animations }.timelines.clone();
    host.with(|host| {
        let mut updated = false;
        for timeline in &timelines {
            let is_scroll = timeline.borrow().scroll().is_some();
            updated |= match update {
                FfiTimelinesUpdate::All => update_current_time(timeline, timestamp, false, host),
                FfiTimelinesUpdate::ScrollTimelines if is_scroll => {
                    update_current_time_from_layout(timeline, false, host)
                }
                FfiTimelinesUpdate::StaleScrollTimelines if is_scroll => {
                    update_current_time_from_layout(timeline, true, host)
                }
                FfiTimelinesUpdate::ScrollTimelines | FfiTimelinesUpdate::StaleScrollTimelines => false,
            };
        }
        updated
    })
}

/// Updates the current time of every timeline associated with the host's `DOM::Document` `document` to now, keeping
/// the animations it updates alive through `keep_alive` and performing what they ask for through `perform`.
pub(crate) fn update_timelines_to_now(
    animations: *mut DocumentAnimations,
    keep_alive: &dyn Fn(&AnimationCell),
    perform: HostEffectPerformer,
) {
    // SAFETY: The caller passes what a live document knows of animations, and the borrow ends here.
    let timelines = unsafe { &*animations }.timelines.clone();
    let Some(first) = timelines.first() else {
        return;
    };
    let now = document_clock(first.document()).now;
    let host = TimelineUpdateHost { keep_alive, perform };
    for timeline in &timelines {
        update_current_time(timeline, now, false, &host);
    }
}

/// The default document timeline of the host's `DOM::Document` `document`, which the document keeps alive.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_timeline(document: NonNull<c_void>) -> *const TimelineCell {
    // NB: The document keeps the timeline alive.
    let timeline = default_document_timeline(document);
    std::ptr::from_ref::<TimelineCell>(&*timeline)
}

/// The timeline associated with a document whose animations are `animations` at `index`, in the order they were
/// associated with it, or null past the last. The document keeps each alive.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_animations_timeline_at(
    animations: &DocumentAnimations,
    index: usize,
) -> *const TimelineCell {
    animations.timelines.get(index).map_or(std::ptr::null(), |timeline| {
        std::ptr::from_ref::<TimelineCell>(timeline)
    })
}

/// A new timeline associated with the host's `DOM::Document` `document`, as `description` describes it, which the
/// document keeps alive.
///
/// # Safety
/// The calculations of a view timeline's inset must be null or live style values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_create_timeline(
    document: NonNull<c_void>,
    description: &FfiTimelineDescription,
) -> *const TimelineCell {
    // SAFETY: Guaranteed by the caller.
    let timeline = unsafe { super::ffi::timeline_of_description(document, description) };
    associate_new_timeline(&timeline);
    // NB: The document keeps the timeline alive.
    std::ptr::from_ref::<TimelineCell>(&*timeline)
}

/// Sets the axis of the scroll timeline `timeline` to `axis`, the host's `ScrollAxis`, and the inset of a view
/// timeline to `inset`, updating its current time through `host` where that changed either.
///
/// # Safety
/// The calculations of `inset` must be null or live style values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_scroll_timeline_set_axis_and_inset(
    timeline: &TimelineCell,
    axis: u8,
    inset: Option<&super::ffi::FfiViewTimelineInset>,
    host: &FfiTimelineAnimationsHost,
) {
    // SAFETY: Guaranteed by the caller.
    let inset = inset.map(|inset| unsafe { inset.retained() });
    if timeline.set_axis_and_inset(axis, inset) {
        host.with(|host| update_current_time_from_layout(timeline, false, host));
    }
}

/// Runs `run` with `context`, with the timelines associated with a document whose animations are `animations` that
/// script observed ahead of their current time in this task at the times it observed.
///
/// # Safety
/// `animations` must be what a live document knows of animations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_at_times_observed_ahead(
    animations: *mut DocumentAnimations,
    context: *mut c_void,
    run: unsafe extern "C" fn(context: *mut c_void),
) {
    // SAFETY: The host runs synchronously.
    DocumentAnimations::at_observed_times(animations, false, || unsafe { run(context) });
}

/// Keeps alive the animations a procedure hands the host, which `keep_alive` keeps with `context`.
pub(crate) fn keep_alive_with(keep_alive: FfiKeepAlive, context: *mut c_void) -> impl Fn(&AnimationCell) {
    move |animation| {
        // SAFETY: The host keeps alive an animation it is handed for as long as the call that handed it lasts.
        unsafe { keep_alive(context, animation) };
    }
}

#[cfg(test)]
mod ffi_test_stubs {
    use super::*;

    #[unsafe(no_mangle)]
    extern "C" fn web_animation_document_animations(_document: NonNull<c_void>) -> NonNull<c_void> {
        unreachable!("the tests associate no timelines with documents")
    }

    #[unsafe(no_mangle)]
    extern "C" fn web_animation_document_clock(_document: NonNull<c_void>) -> FfiDocumentClock {
        unreachable!("the tests observe no document clocks")
    }

    #[unsafe(no_mangle)]
    extern "C" fn web_animation_find_scroll_timeline_boxes(
        _source: &FfiScrollTimelineSource,
        _is_view: bool,
        _context: *mut c_void,
        _measure: unsafe extern "C" fn(context: *mut c_void, boxes: &FfiScrollTimelineBoxes),
    ) {
        unreachable!("the tests lay out no scroll timelines")
    }

    #[unsafe(no_mangle)]
    extern "C" fn web_animation_scroll_timeline_source(
        _source: &FfiScrollTimelineSource,
        _is_view: bool,
    ) -> *mut c_void {
        unreachable!("the tests observe no scroll timeline sources")
    }
}
