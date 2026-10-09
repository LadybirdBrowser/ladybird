/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host keeps of a document's render state, which the render owner holds: its name, the frame that may fly
//! beside the host, and what the host reads between the jobs it hands the owner.

use super::clock::{ClockPlan, ClockTicks, CommittedFrame, LaneTransitionStarts};
use super::owner::{self, DocumentId, SharedWithHost, StateSeed};
use super::wait::{BegunRead, NodeRead};
use super::{
    ArenaChange, ArenaFacts, ChangeQueue, EngineFacts, ForcedRead, Landing, Moves, NoFrameInFlight, Owed, RenderState,
    RenderWait, RowWrite, StateFacts, on_render_side, post_to_render_side,
};
use crate::css::style::flight_style_rows::FlightStyleRow;
use crate::css::style::rule_writes::{PublishedRules, RuleWrite};
use crate::css::style::style_job::{SealedStyleInputs, StyleJobAnswer};
use crate::css::style::tree::StyleNodeID;
use crate::layout::node_data::NodeSlotId;
use crate::layout::row_reads::{RowIdentities, RowSnapshot, RowStyles};
use crate::layout::tree_update_marks::{LayoutTreeUpdateMarkWrite, LayoutTreeUpdateMarks};
use crate::layout::{FlownRound, HostTables, RowsVersion, SealedRound};
use crate::painting::paint_read::PaintSource;
use crate::painting::record::recorder_state::AbsoluteRectMemo;
use crate::painting::recording_slot::{RecordingAnswer, RecordingSlot};
use crate::painting::visual_animation::VisualAnimation;
use crate::render_state::TaskBoundary;
use crate::stage_thread::InFlight;
use libgfx_rust::FloatPoint;
use std::cell::{Cell, RefCell, RefMut};
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::Ordering;

/// The boundary events a hover a rendering update kept owes: where the pointer was, in the context's device pixels, or
/// none where it left the context, and the element the lane's hover put the hover on there, or nothing, where the lane
/// took that move.
#[derive(Clone, Copy)]
pub(super) struct HoverOwed {
    pub(super) pointer: Option<FloatPoint>,
    pub(super) target: Option<Option<StyleNodeID>>,
}

/// The host's side of one document's render state: the name the render owner holds the state by, the frame that may
/// fly beside the host, the host tables the host answers layout through, and what the document keeps of its display
/// list recordings, which are made on the host's thread from the frame the render state publishes. The host's document
/// owns it, and it lives on the host's thread.
///
/// The host's entries call into each other, so it is only ever shared: everything a call may change sits in a cell, and
/// the rows are handed out by reference count, so a call that publishes them again frees none an outer call still reads.
pub struct DocumentHost {
    /// The name the render owner holds the document's render state by.
    document: DocumentId,
    /// What the owner makes the state from, until the host's first job hands it over.
    seed: Cell<Option<StateSeed>>,
    /// The frame that flies beside the host, until the host takes it in.
    away: RefCell<Option<InFlight<Landing>>>,
    /// Whether the host waits for the frame: it flies, or a layout round that flew with it is not paid yet. The host's
    /// scopes of a read and its document's layout read it where it is (see [`document_host_read_scope_view`]).
    waits_for_frame: Cell<bool>,
    host_tables: HostTables,
    recording: RefCell<RecordingSlot>,
    /// The rows the render state published last, which the host reads between its jobs.
    rows: RefCell<Option<Rc<RowSnapshot>>>,
    /// The reactions of the style transaction the host took last, which it reads until it ends the transaction.
    style_transaction: RefCell<Option<StyleJobAnswer>>,
    /// The first sample of the transitions a step the host read from the style transaction starts, by element, which
    /// the sample the host takes next reads.
    fresh_transition_sample: Cell<Option<(u32, crate::css::transition::FreshTransitionSample)>>,
    /// The absolute rects the host's reads of the rows computed, kept for as long as the geometry they were computed
    /// from stays.
    absolute_rects: RefCell<AbsoluteRectMemo>,
    /// What the render state shares with the host.
    shared: SharedWithHost,
    /// The compositor animations the document's effects published in the current update pass, which the host hands
    /// the render state as the pass ends.
    compositor_animations: RefCell<Vec<VisualAnimation>>,
    /// What the host's scopes of a read lend the entries they call.
    begun_read: BegunRead,
    /// The writes the host queued that the render state has not applied yet, in the order the host made them.
    changes: ChangeQueue,
    /// The style transaction that flew with the frame, from its landing until the host has drained its reactions.
    flown_style: RefCell<Option<FlownStyle>>,
    /// Whether the paint and hit testing properties the document prepared last were prepared from the render state as it
    /// is: nothing was written to it since, queued, in place or by a job. Preparing them again would find nothing
    /// to do.
    paint_preparation_is_current: Cell<bool>,
    /// The first layout round of a rendering update, sealed until the frame flies with it.
    sealed_round: RefCell<Option<SealedRound>>,
    /// The layout round that flew with the frame, from its landing until the host's next layout update pays it.
    flown_round: RefCell<Option<FlownRound>>,
    /// Whether a job of the host runs, whose host callbacks see rows it freed and the host has not heard of yet.
    in_job: Cell<bool>,
    /// Whether the host streamed writes to the render state since its last job or frame, which bring back what they owe
    /// the host and the marks they made.
    streamed: Cell<bool>,
    /// The document's layout tree update marks, which the host lends the render owner's jobs.
    marks: RefCell<HostMarks>,
    /// The rules the host published to each of its document's style sheets.
    published_rules: PublishedRules,
    /// What the host learned of its document's style engine.
    engine_memo: crate::css::style::engine_calls::EngineMemo,
    /// The facts of the render state as the host's last job or frame left it, which a frame in flight forgets.
    facts: Cell<Option<StateFacts>>,
    /// The attribute names whose value text the engine's selectors read, as the host's last job or frame left them.
    selector_value_text_names: RefCell<crate::css::style::SelectorAttributeNames>,
    /// The attribute names the engine's selectors test in any way, as its last job left them.
    selector_tested_attribute_names: RefCell<crate::css::style::SelectorAttributeNames>,
    /// The ticks the render clock hands the lanes of the document's presented frames.
    ticks: Arc<ClockTicks>,
    /// The transitions the hover of the lane that follows the presented frame started: those the host's own hover starts
    /// of the same properties of the same elements run from then, as the screen showed them.
    lane_transition_starts: RefCell<LaneTransitionStarts>,
    /// Whether the frame in flight waits for a test to release it.
    frame_held_for_testing: Cell<bool>,
    /// The root and the sealed document computation inputs of the last style transaction the host took or let fly,
    /// which the hover of a lane takes its own transactions with.
    style_inputs: RefCell<Option<(StyleNodeID, Arc<SealedStyleInputs>)>>,
    /// The element the host asked the engine's hover to move to last: the one the events of the last mouse move it
    /// handled hovered, as the engine's hover follows it through tree changes.
    hover_target: Cell<Option<StyleNodeID>>,
    /// Where the compositor said the pointer went last as a rendering update took the lanes in, and the element a lane's
    /// hover put the hover on there, if one did: the boundary events of the move the host fires where it handles no
    /// mouse move first.
    hover_events_owed: Cell<Option<HoverOwed>>,
    /// The input event id of the newest pointer move the host knows of: the newest mouse event it handled, or the
    /// newest move the compositor told the render clock of as the host took in what the lanes did.
    newest_pointer_input_event: Cell<u64>,
    /// The last pointer move a tick of the lanes took that the last rendering update took in, which the mouse event of
    /// that move goes by: the host drops older moves' events.
    lane_move: Cell<Option<super::clock::LaneMove>>,
    /// The serial number of the last rendering update that took the lanes in, until it, or an update after it, seals its
    /// plan.
    lanes_taken_in: Cell<Option<u64>>,
    /// Whether the last plan the host sealed was one, which the lanes may still follow: with none, they do nothing.
    lanes_planned: Cell<bool>,
}

/// Pays what the writes a job applied owe the host, which only [`DocumentHost::pay`] makes.
pub(crate) struct OwedWorkPayment {
    _private: (),
}

const OWED_WORK_PAYMENT: OwedWorkPayment = OwedWorkPayment { _private: () };

/// The layout tree update marks of a host's document: here, between the render owner's jobs, or lent to a job.
struct HostMarks {
    /// The document's marks, unless a job or the frame in flight has them.
    here: Option<LayoutTreeUpdateMarks>,
    /// The marks the host made beside the frame in flight that has the document's, which answer the host's questions
    /// about marks meanwhile: a mark made before the frame flew reads as unset there, which at most makes a mark reach
    /// further than it had to.
    beside_flight: LayoutTreeUpdateMarks,
    /// The writes the host made beside the frame, which the document's marks take once it lands.
    written_beside_flight: Vec<LayoutTreeUpdateMarkWrite>,
}

/// A style transaction that flew with the frame and has landed, or whose reactions the host drains. The style writes the
/// host queued beside it reach the render state only behind its drain.
enum FlownStyle {
    /// What the transaction answered, and the rows of it the frame applied to the boxes itself, by element.
    Landed(StyleJobAnswer, Vec<FlightStyleRow>),
    /// The host drains the transaction's reactions, with the rows the frame applied.
    Draining(Vec<FlightStyleRow>),
}

impl DocumentHost {
    fn new() -> Self {
        let shared = SharedWithHost::new();
        let document = DocumentId::mint();
        Self {
            document,
            seed: Cell::new(Some(StateSeed { shared: shared.clone() })),
            shared,
            away: RefCell::default(),
            waits_for_frame: Cell::new(false),
            host_tables: HostTables::default(),
            recording: RefCell::default(),
            rows: RefCell::default(),
            style_transaction: RefCell::default(),
            fresh_transition_sample: Cell::default(),
            absolute_rects: RefCell::default(),
            compositor_animations: RefCell::default(),
            begun_read: BegunRead::of_host(),
            changes: ChangeQueue::default(),
            flown_style: RefCell::default(),
            paint_preparation_is_current: Cell::new(false),
            sealed_round: RefCell::default(),
            flown_round: RefCell::default(),
            in_job: Cell::new(false),
            streamed: Cell::new(false),
            marks: RefCell::new(HostMarks {
                here: Some(LayoutTreeUpdateMarks::default()),
                beside_flight: LayoutTreeUpdateMarks::default(),
                written_beside_flight: Vec::new(),
            }),
            published_rules: PublishedRules::default(),
            engine_memo: Default::default(),
            facts: Cell::new(None),
            selector_value_text_names: RefCell::default(),
            selector_tested_attribute_names: RefCell::default(),
            ticks: ClockTicks::new(document),
            lane_transition_starts: RefCell::default(),
            frame_held_for_testing: Cell::default(),
            style_inputs: RefCell::default(),
            hover_target: Cell::default(),
            hover_events_owed: Cell::default(),
            newest_pointer_input_event: Cell::default(),
            lane_move: Cell::default(),
            lanes_taken_in: Cell::default(),
            lanes_planned: Cell::default(),
        }
    }

    /// A host with a render state, for a unit test.
    #[cfg(test)]
    pub(crate) fn for_test() -> Self {
        Self::new()
    }

    /// A read of the host's document the test begins, and never ends.
    #[cfg(test)]
    pub(crate) fn read_for_test(&self) -> &BegunRead {
        &self.begun_read
    }

    pub(crate) fn host_tables(&self) -> &HostTables {
        &self.host_tables
    }

    /// The rules the host published to each of its document's style sheets, which it reads in place of the engine's.
    pub(crate) fn published_rules(&self) -> &PublishedRules {
        &self.published_rules
    }

    /// What the host learned of its document's style engine.
    pub(crate) fn engine_memo(&self) -> &crate::css::style::engine_calls::EngineMemo {
        &self.engine_memo
    }

    /// Writes `change` to the document's render state, before anything that reads what it changes: the render owner
    /// applies it ahead of the host's next job. A write never reaches the render state as the host makes it.
    pub(crate) fn queue_change(&self, change: ArenaChange) {
        match &change {
            ArenaChange::Style(change) if change.only_keeps_records_alive() => {}
            ArenaChange::Style(change) => {
                self.note_render_state_write();
                self.engine_memo.follow(change);
            }
            _ => self.note_render_state_write(),
        }
        self.changes
            .push(change, |queued| self.engine_memo.follow_queued(queued));
        self.stream_writes_if_due();
    }

    /// Whether a write the host queues now waits behind the drain of the reactions of a style transaction that flew,
    /// beside the writes the drain queues meanwhile.
    pub(crate) fn holds_style_writes(&self) -> bool {
        self.changes.holds_style.get()
    }

    /// How many writes the host queues before it streams them to the render state.
    const STREAMED_WRITES: usize = 256;

    /// Streams the writes the host queued to the render state, once a batch of them is due, where nothing keeps them
    /// queued: the render owner applies them beside the host's task, behind the jobs and frames handed to it before,
    /// rather than in the host's next job. What they owe the host and the marks they make wait for that job.
    fn stream_writes_if_due(&self) {
        if self.changes.len() < Self::STREAMED_WRITES || !self.may_stream_writes() {
            return;
        }
        let changes = self.changes.take_for_stream();
        self.streamed.set(true);
        let document = self.document;
        post_to_render_side(move || {
            super::clock::note_host_write(document);
            owner::with_state(document, None, |state| state.apply_streamed(changes));
        });
    }

    /// Commits the rendering update's frame to the render owner, spending `read`, and goes on: the owner applies the
    /// writes the host queued, samples the frame as `commit` says, and hands it to the Paint thread, which records and
    /// presents it beside the host. Answers the flight the recording's answer lands in.
    pub(crate) fn commit_rendering_update(
        &self,
        read: &BegunRead,
        commit: CommittedFrame,
    ) -> InFlight<RecordingAnswer> {
        self.take_frame_in(read);
        // Writes the host may not stream beside its task the owner applies in a job first, which the frame reads as a
        // read would.
        let changes = match self.may_stream_writes() {
            true => self.changes.take_for_stream(),
            false => {
                self.run(read, false, |_| ());
                Vec::new()
            }
        };
        if !changes.is_empty() {
            self.streamed.set(true);
        }
        let document = self.document;
        let ticks = Arc::clone(&self.ticks);
        crate::stage_thread::style_layout_thread().submit_relayed(move |relay| {
            if !changes.is_empty() {
                super::clock::note_host_write(document);
            }
            owner::with_state(document, None, |state| {
                if !changes.is_empty() {
                    state.apply_streamed(changes);
                }
                commit.sample(state, relay, &ticks);
            });
        })
    }

    /// Whether the render state may apply the writes the host queued now: a job made it, none runs, as a host callback
    /// of one does, no frame flies or waits to be taken in, and the host reads the state as of its writes.
    fn may_stream_writes(&self) -> bool {
        if self.in_job.get()
            || self.away.borrow().is_some()
            || self.waits_for_frame.get()
            || self.flown_style.borrow().is_some()
            || self.changes.sets_writes_aside()
        {
            return false;
        }
        let seed = self.seed.take();
        let made = seed.is_none();
        self.seed.set(seed);
        made && (cfg!(test) || !crate::stage_thread::style_layout_thread().is_current())
    }

    /// Notes that the document's render state is written, which leaves the paint and hit testing properties prepared
    /// from it stale.
    pub(super) fn note_render_state_write(&self) {
        self.paint_preparation_is_current.set(false);
    }

    /// Writes `write` to the document's style sheets in its style engine, behind the writes queued before it.
    pub(crate) fn write_rules(&self, write: RuleWrite) {
        self.queue_change(ArenaChange::Rule(write));
    }

    /// Takes the writes the host queued, for a frame that flies with them to the render owner. The style writes queued
    /// beside a frame that flies with a style transaction wait for its drain.
    pub(super) fn take_queued_changes_for_flight(&self, with_style: bool) -> Vec<ArenaChange> {
        debug_assert!(!self.has_flown_style(), "one frame of a document flies at a time");
        self.changes.take_for_flight(with_style)
    }

    /// Pays what the writes a job or a frame applied owe the host, now that the host has it back.
    fn pay(
        &self,
        Owed {
            work,
            deferred_inputs,
            facts,
            selector_value_text_names,
            selector_tested_attribute_names,
        }: Owed,
    ) {
        self.facts.set(Some(facts));
        *self.selector_value_text_names.borrow_mut() = selector_value_text_names;
        *self.selector_tested_attribute_names.borrow_mut() = selector_tested_attribute_names;
        self.engine_memo
            .deferred
            .borrow_mut()
            .follow_job(deferred_inputs, !self.changes.is_empty());
        // SAFETY: The host has its job or frame back, on its document's thread, or on the render owner in a host
        // callback of a job the host waits for, as every host callback runs.
        let main_thread = unsafe { crate::stage::from_ffi_entry(&OWED_WORK_PAYMENT, self) };
        crate::css::style::give_up_custom_property_data_let_go_beside_the_host(&main_thread);
        for work in work {
            work.pay(&main_thread);
        }
    }

    /// Lets the frame fly beside the host, in the job `flight` submits to the owner with the document's name, until the
    /// host drains the reactions of the style transaction it flies with. `flight` also answers whether a test holds it.
    pub(super) fn let_frame_fly(
        &self,
        flight: impl FnOnce(DocumentId, Option<StateSeed>, Option<LayoutTreeUpdateMarks>) -> (InFlight<Landing>, bool),
    ) {
        self.facts.set(None);
        self.streamed.set(false);
        let (flight, held_for_testing) = flight(self.document, self.seed.take(), self.lend_marks());
        self.frame_held_for_testing.set(held_for_testing);
        let previous = self.away.borrow_mut().replace(flight);
        assert!(previous.is_none(), "one frame of a document flies at a time");
        self.note_frame_wait();
    }

    /// Takes the frame in flight in, where one flies, waiting for it to land, spending `read`: only a read waits for a
    /// frame.
    #[inline]
    fn take_frame_in(&self, read: impl RenderWait) {
        if self.away.borrow().is_some() {
            self.land(read.into_forced_read());
        }
    }

    /// When the hover of the lane started a transition of `property` of the element `node` names, where it did. See
    /// [`Self::lane_transition_starts`].
    pub(crate) fn lane_transition_start(&self, node: StyleNodeID, property: u16) -> Option<f64> {
        self.lane_transition_starts.borrow_mut().start_of(node, property)
    }

    /// Forgets when the hover of the lane started transitions, once the host started its own.
    pub(crate) fn forget_lane_transition_starts(&self) {
        self.lane_transition_starts.borrow_mut().forget();
    }

    /// Keeps the root and the sealed inputs of a style transaction the host takes or lets fly, for the hover of a lane
    /// to take its own with.
    pub(crate) fn keep_style_inputs(&self, root: StyleNodeID, inputs: Arc<SealedStyleInputs>) {
        *self.style_inputs.borrow_mut() = Some((root, inputs));
    }

    /// The root and the sealed inputs of the last style transaction the host took or let fly.
    pub(crate) fn style_inputs(&self) -> Option<(StyleNodeID, Arc<SealedStyleInputs>)> {
        self.style_inputs.borrow().clone()
    }

    /// Takes `target` as the element the hover targets, as the events of a mouse move hover it. Answers whether the
    /// host's next input transaction moves the engine's hover to it.
    pub(crate) fn request_hover(&self, target: Option<StyleNodeID>) -> bool {
        self.hover_events_owed.set(None);
        let moves = self.hover_target.get() != target;
        self.hover_target.set(target);
        moves
    }

    /// The element the lanes' hover put the hover on at the pointer move with the input event id `input_event_id`, or
    /// nothing, where a tick of theirs took that move and hovered it, as the last rendering update took them in.
    pub(super) fn lane_hover_target_for(&self, input_event_id: u64) -> Option<Option<StyleNodeID>> {
        self.lane_move
            .get()
            .filter(|hover_move| input_event_id != 0 && hover_move.input_event_id == input_event_id)
            .and_then(|hover_move| hover_move.target)
    }

    /// Whether the lanes may hover or run transitions beside the tasks: the last plan the host sealed was one.
    pub(super) fn may_have_lanes(&self) -> bool {
        self.lanes_planned.get()
    }

    /// Whether a rendering update took the lanes in and has yet to seal its plan, until which they present nothing.
    pub(super) fn lanes_wait_for_the_update(&self) -> bool {
        self.lanes_taken_in.get().is_some()
    }

    /// Hands `plan` to the lane of the frame the host presented last, or none, for the tasks after the rendering update
    /// with the serial number `update`. The plan of an update that sealed after a later one took the lanes in leaves them
    /// waiting for that one's.
    pub(crate) fn seal_clock_plan(&self, plan: Option<ClockPlan>, update: u64) {
        if super::clock::hover::logs_hover() {
            eprintln!(
                "{} hover lane: host frame of the update hovers {:?}",
                super::clock::hover::log_time(),
                self.hover_target.get().map(StyleNodeID::raw)
            );
        }
        self.ticks
            .note_plan_animates(plan.as_ref().is_some_and(ClockPlan::animates));
        self.ticks.note_input_handled(self.newest_pointer_input_event.get());
        self.lanes_planned.set(plan.is_some());
        let plan = plan.map(|mut plan| {
            plan.animation_changes = self.ticks.animation_changes();
            plan
        });
        let document = self.document;
        self.lanes_taken_in
            .set(self.lanes_taken_in.get().filter(|taken_in| *taken_in > update));
        post_to_render_side(move || super::clock::seal_plan(document, plan, update));
    }

    /// Notes whether the event loop begins a task or goes idle: beside an idle event loop the lanes sample no animations.
    /// Answers whether a task begins whose lanes sample the animations of the last plan the host sealed.
    pub(super) fn note_event_loop_task(&self, runs_task: bool) -> bool {
        self.ticks.hold_animations(!runs_task);
        runs_task && self.ticks.plan_animates()
    }

    /// The ticks the render clock hands the lanes of the document's presented frames.
    pub(super) fn clock_ticks(&self) -> &Arc<ClockTicks> {
        &self.ticks
    }

    /// Takes the frame in flight in, where one flies, spending `read`.
    pub(crate) fn take_frame_in_with(&self, read: &BegunRead) {
        assert!(
            read.reaches(self),
            "a begun read reaches only the render state of its own document"
        );
        self.take_frame_in(read);
    }

    /// Lands the frame in flight, waiting for it, spending `read`: the style transaction that flew with it waits to be
    /// drained.
    fn land(&self, read: ForcedRead) {
        if let Some(flight) = self.away.take() {
            // A frame a test holds goes on as soon as the host waits for it.
            if self.frame_held_for_testing.take() {
                crate::painting::recording_slot::release_held_recording_for_testing();
            }
            self.landed(flight.join(read));
        }
    }

    fn landed(
        &self,
        Landing {
            style,
            applied,
            round,
            changes,
            marks,
            owed,
        }: Landing,
    ) {
        self.changes.give_back(changes);
        self.take_marks_back(marks);
        self.pay(owed);
        if let Some(style) = style {
            let previous = self
                .flown_style
                .borrow_mut()
                .replace(FlownStyle::Landed(style, applied));
            debug_assert!(
                previous.is_none(),
                "one style transaction of a document flies at a time"
            );
        }
        if let Some((round, rows)) = round {
            // The layout round that flew with the frame wrote the render state the paint properties are prepared from.
            self.note_render_state_write();
            let untaken = self.flown_round.borrow_mut().replace(round);
            assert!(
                untaken.is_none(),
                "a layout round that flew is taken in before the next one flies"
            );
            // The frame's job ended with the rows, so they read as the arena unless the host wrote it since.
            *self.rows.borrow_mut() = Some(Rc::new(rows));
        }
        self.note_frame_wait();
    }

    /// Seals `round`, the first layout round of a rendering update, for the frame the update lets fly next.
    pub(crate) fn seal_round(&self, round: SealedRound) {
        *self.sealed_round.borrow_mut() = Some(round);
    }

    /// Takes the round the host sealed, for the frame that flies with it.
    pub(crate) fn take_sealed_round(&self) -> Option<SealedRound> {
        self.sealed_round.borrow_mut().take()
    }

    /// Whether a layout round that flew waits for the host to take it in.
    pub(crate) fn has_flown_round(&self) -> bool {
        self.flown_round.borrow().is_some()
    }

    /// Takes what the layout round that flew owes the host, for the layout update that pays it.
    pub(crate) fn take_flown_round(&self) -> Option<FlownRound> {
        let round = self.flown_round.borrow_mut().take();
        self.note_frame_wait();
        round
    }

    /// Answers `read` of the document's layout tree update marks, or of the marks the host made beside the frame in
    /// flight that has them.
    pub(crate) fn read_marks<R>(&self, read: impl FnOnce(&LayoutTreeUpdateMarks) -> R) -> R {
        let marks = self.marks.borrow();
        match &marks.here {
            Some(here) => read(here),
            None => read(&self.beside_flight(&marks).beside_flight),
        }
    }

    /// Makes `write` to the document's layout tree update marks, answering what it answers, or to the marks the host
    /// made beside the frame in flight that has them, for the document's to take once the frame lands.
    pub(crate) fn write_marks(&self, write: LayoutTreeUpdateMarkWrite) -> bool {
        let mut marks = self.marks.borrow_mut();
        let marks = &mut *marks;
        if let Some(here) = &mut marks.here {
            return write.apply(here);
        }
        self.beside_flight(marks);
        marks.written_beside_flight.push(write);
        write.apply(&mut marks.beside_flight)
    }

    /// `marks`, which the frame in flight has lent: only the frame holds the document's marks while the host runs.
    fn beside_flight<'a>(&self, marks: &'a HostMarks) -> &'a HostMarks {
        assert!(
            self.frame_flies(),
            "the host reaches its layout tree update marks only between the render owner's jobs"
        );
        marks
    }

    /// Lends the document's layout tree update marks to a job of the render owner, unless a job that runs has them.
    fn lend_marks(&self) -> Option<LayoutTreeUpdateMarks> {
        self.marks.borrow_mut().here.take()
    }

    /// Takes back the marks a job was lent, if it was, and makes the writes the host made beside it to them.
    fn take_marks_back(&self, lent: Option<LayoutTreeUpdateMarks>) {
        let Some(mut lent) = lent else {
            return;
        };
        let mut marks = self.marks.borrow_mut();
        for write in marks.written_beside_flight.drain(..) {
            write.apply(&mut lent);
        }
        marks.beside_flight = LayoutTreeUpdateMarks::default();
        marks.here = Some(lent);
    }

    /// Proof that no frame flies, where the document's layout waits for none: neither a frame that flies nor a round
    /// that flew and is not paid yet. A read of the layout in place then takes no frame in, and needs no read the host
    /// began.
    pub(crate) fn layout_waits_for_no_frame(&self) -> Option<NoFrameInFlight> {
        (!self.waits_for_frame.get()).then_some(NoFrameInFlight(()))
    }

    /// The layout node C++ made for the row `id`, which the host reads without asking where it knows the row live: a
    /// row frees only in a job or a frame, whose freed rows the host destroys the layout nodes of once it has them back,
    /// and C++ makes a layout node only for a row the arena stamped.
    pub(crate) fn held_shell(&self, id: crate::layout::node_data::NodeSlotId) -> Option<*mut std::ffi::c_void> {
        if self.in_job.get() || self.waits_for_frame.get() {
            return None;
        }
        self.host_tables.shells.borrow().get(&id).map(|shell| shell.as_ptr())
    }

    /// The facts of the render state as the host's last job or frame left it, where no job runs and no write the host
    /// queued since may have moved what `holds` says they read.
    fn facts_where(&self, holds: impl FnOnce(Moves) -> bool) -> Option<StateFacts> {
        let facts = self.facts.get()?;
        (!self.in_job.get() && holds(self.changes.moves())).then_some(facts)
    }

    /// The facts of the style engine, where the host knows them: it wrote nothing that may move them since its last job
    /// or frame left them, and none runs. A write to the layout boxes alone leaves them known.
    pub(crate) fn known_engine_facts(&self) -> Option<EngineFacts> {
        self.facts_where(|moves| !moves.engine_facts).map(|facts| facts.engine)
    }

    /// The facts of the arena, where the host knows them: it wrote nothing that may move them since its last job or
    /// frame left them, and none runs.
    pub(crate) fn known_arena_facts(&self) -> Option<ArenaFacts> {
        self.facts_where(|moves| !moves.arena_facts).map(|facts| facts.arena)
    }

    /// Whether the document's layout is up to date as of every write the host made, unless its layout tree update marks
    /// ask for a build of it, where the host knows: a write only ever leaves layout staler, and only a layout round, or
    /// a frame's, lays it out, so stale layout stays stale until one runs, and fresh layout stays fresh until the host
    /// writes the rows with a write that may move the arena's facts. Writes of the paint state alone leave it fresh, as
    /// the commit of a layout makes them.
    pub(crate) fn known_layout_up_to_date_unless_built(&self) -> Option<bool> {
        let facts = self.facts_where(|_| true)?;
        let up_to_date = facts.arena.layout_is_up_to_date_unless_built;
        let moves = self.changes.moves();
        (!up_to_date || moves.rows == RowWrite::None || !moves.arena_facts).then_some(up_to_date)
    }

    /// Where the engine's selectors' requirements of attribute value text are, where the host knows: none runs, and it
    /// queued no rule since its last job or frame, which is all that compiles a selector.
    pub(crate) fn known_selector_attribute_value_text_requirements_version(&self) -> Option<u64> {
        self.facts_where(|moves| !moves.selectors)
            .map(|facts| facts.selector_attribute_value_text_requirements_version)
    }

    /// Whether the engine's selectors read the value text of an attribute that answers to any of `keys`, where the host
    /// knows: as it knows where their requirements are.
    pub(crate) fn known_selectors_read_value_text_of(
        &self,
        keys: &[crate::css::style::index::StyleAtomID],
    ) -> Option<bool> {
        self.known_selector_attribute_value_text_requirements_version()?;
        let names = self.selector_value_text_names.borrow();
        Some(keys.iter().any(|key| names.contains(key)))
    }

    /// Whether the engine's selectors test an attribute that answers to any of `keys`, where the host knows: as it knows
    /// where their requirements are.
    pub(crate) fn known_selectors_test_attribute(
        &self,
        keys: &[crate::css::style::index::StyleAtomID],
    ) -> Option<bool> {
        self.known_selector_attribute_value_text_requirements_version()?;
        let names = self.selector_tested_attribute_names.borrow();
        Some(keys.iter().any(|key| names.contains(key)))
    }

    /// Whether the host knows what its engine holds as of the writes it queued: no job of the engine runs, as a host
    /// callback of one does, and no frame flies.
    pub(crate) fn knows_engine_between_jobs(&self) -> bool {
        !self.in_job.get() && !self.frame_flies()
    }

    fn frame_flies(&self) -> bool {
        self.away.borrow().is_some()
    }

    fn note_frame_wait(&self) {
        self.waits_for_frame
            .set(self.frame_flies() || self.flown_round.borrow().is_some());
    }

    /// Takes in what the lanes did, as a rendering update begins, and answers whether a tick of a lane presented a frame
    /// since the host last took in what they did, which the screen shows in place of the host's. The update keeps what
    /// the screen shows: it hovers where the compositor said the pointer went last, as the lanes did, which owes the
    /// boundary events of the move until the host handles a mouse move.
    pub(super) fn take_clock_lanes_in(&self, update: u64) -> bool {
        // The lanes present nothing from now until the update's frame: a pointer move that waits for a tick is the
        // update's to hover, as the newest the compositor told of. The owner takes them in between ticks, so no tick
        // presents a frame the report leaves out.
        let ticks = Arc::clone(&self.ticks);
        let report = match self.lanes_taken_in.replace(Some(update)) {
            // An earlier update took the lanes in and has yet to seal its plan: they did nothing since, and wait for
            // this update's plan from now on, which the owner hears of behind what it runs for the earlier update.
            Some(_) => {
                post_to_render_side(move || {
                    let _ = super::clock::take_lanes_in(&ticks, update);
                });
                None
            }
            None => Some(on_render_side(move || super::clock::take_lanes_in(&ticks, update))),
        };
        let mut presented = false;
        if let Some(report) = report {
            presented = report.presented;
            self.lane_move.set(report.last_move);
            self.lane_transition_starts
                .borrow_mut()
                .take_in(report.transition_starts);
        }
        if super::clock::hover::logs_hover() {
            eprintln!(
                "{} hover lane: update takes the lanes in: moved {}, presented {presented}, newest pointer {:?}",
                super::clock::hover::log_time(),
                self.lane_move.get().is_some(),
                self.ticks
                    .newest_pointer()
                    .map(|pointer| (pointer.position, pointer.input_event_id))
            );
        }
        // The host hovers where the compositor said the pointer went last, which is newer than the mouse events the host
        // queued before it, and than the moves the lanes' hover took, if any: the hover never moves back to where an
        // older event says the pointer was, and the newest move's own event follows.
        // Where a tick took that move last, the host hovers the element the lane hovered, which is what the screen
        // shows: a hit test of its own, in a layout that has not moved with the lane's hover, could find another.
        // A move older than a mouse event the host handled owes nothing.
        let lane_move = self.lane_move.get();
        let handled = self.newest_pointer_input_event.get();
        let (newest, waits) = self.ticks.newest_pointer_and_waits();
        if let Some(newest) = newest
            && (newest.input_event_id == 0 || newest.input_event_id >= handled)
            && (newest.input_event_id > handled || waits || lane_move.is_some())
        {
            self.newest_pointer_input_event.set(handled.max(newest.input_event_id));
            self.hover_events_owed.set(Some(HoverOwed {
                pointer: newest.position,
                target: lane_move
                    .filter(|last| last.input_event_id == newest.input_event_id && !waits)
                    .and_then(|last| last.target),
            }));
        }
        presented
    }

    /// Whether the mouse event with the input event id `input_event_id` is older than the newest pointer move the host
    /// knows of, and notes the id of one that is not, which the host handles: the hover and the pointer never move back
    /// to where an older event says the pointer was. An event with id 0 matches no UI event, and is never outdated.
    pub(super) fn mouse_event_is_outdated(&self, input_event_id: u64) -> bool {
        if input_event_id == 0 {
            return false;
        }
        if input_event_id < self.newest_pointer_input_event.get() {
            return true;
        }
        self.newest_pointer_input_event.set(input_event_id);
        false
    }

    /// Takes the boundary events a hover a rendering update kept owes, where the host handled no mouse move since: where
    /// the pointer was, in the context's device pixels, or none where it left the context, and the element a lane's hover
    /// put the hover on there, if one did.
    pub(super) fn take_hover_events_owed(&self) -> Option<HoverOwed> {
        self.hover_events_owed.take()
    }

    /// Whether the document's frame still flies, where it has not landed: one that has is taken in. The event loop asks
    /// between two tasks, so this never waits.
    pub(crate) fn frame_still_flies(&self, boundary: &TaskBoundary) -> bool {
        let Some(flight) = self.away.take() else {
            return false;
        };
        match flight.try_take(boundary) {
            Ok(landing) => {
                self.landed(landing);
                false
            }
            Err(flight) => {
                *self.away.borrow_mut() = Some(flight);
                true
            }
        }
    }

    /// Whether the host let a style transaction fly whose reactions it has not begun to drain.
    #[inline]
    pub(crate) fn has_flown_style(&self) -> bool {
        self.frame_flies() || matches!(*self.flown_style.borrow(), Some(FlownStyle::Landed(..)))
    }

    /// Takes what the style transaction that flew answered, for the host to drain its reactions, once `read` took the
    /// frame in. The writes the host queued beside the transaction wait for the drain to end.
    pub(crate) fn begin_style_drain(&self, read: impl RenderWait) -> StyleJobAnswer {
        self.take_frame_in(read);
        let mut flown = self.flown_style.borrow_mut();
        let Some(FlownStyle::Landed(answer, applied)) = flown.take() else {
            panic!("the host drains a style transaction that flew and has landed");
        };
        *flown = Some(FlownStyle::Draining(applied));
        self.changes.stop_holding_style();
        answer
    }

    /// Whether the frame the host drains the style transaction of applied the element `style_node` names its record
    /// `style_record` ahead of the host, and marked the relayout the move asks for.
    pub(crate) fn frame_marked_relayout(&self, style_node: StyleNodeID, style_record: u64) -> bool {
        let flown = self.flown_style.borrow();
        let Some(FlownStyle::Draining(applied)) = &*flown else {
            return false;
        };
        applied
            .binary_search_by_key(&style_node, |row| row.style_node)
            .is_ok_and(|index| applied[index].new_style_record == style_record && applied[index].relayout)
    }

    /// Sets the writes the host queued aside, for it to read its render state as a frame that flew left it, until
    /// queue_writes_set_aside().
    pub(crate) fn set_writes_aside(&self) {
        self.changes.set_aside();
    }

    /// Queues the writes the host set aside behind those it queued meanwhile.
    pub(crate) fn queue_writes_set_aside(&self) {
        self.changes.queue_set_aside();
    }

    /// Ends the drain of the style transaction that flew: the writes the host queued beside it are queued again, behind
    /// what the drain wrote.
    pub(crate) fn end_style_drain(&self) {
        let Some(FlownStyle::Draining(_)) = self.flown_style.borrow_mut().take() else {
            panic!("the host ends the drain it began");
        };
        self.changes
            .queue_held_style(|queued| self.engine_memo.follow_queued(queued));
    }

    /// What the host's scopes of a read lend the entries they call.
    pub(super) fn begun_read(&self) -> &BegunRead {
        &self.begun_read
    }

    /// The read of a layout node the host holds, as the read the host began, for the entries the host calls about the
    /// node's document: the node was reached in a read, which took the frame in (see [`NodeRead`]).
    pub(crate) fn begun_read_of_held_node(&self, _: NodeRead) -> &BegunRead {
        assert!(
            !self.frame_flies(),
            "a layout node the host holds finds its document's frame taken in"
        );
        &self.begun_read
    }

    /// A fresh identity for an element-sourced declaration block's contents.
    pub(crate) fn next_declaration_block_version(&self) -> u32 {
        crate::css::style::next_declaration_block_version(&self.shared.declaration_block_versions)
    }

    /// Whether the document's style engine may keep a row's container effects for the host to take, where the host
    /// knows it without asking: no frame flies, whose style may note some beside the host. What a frame noted has
    /// reached the host once it is back.
    pub(crate) fn container_effects_may_be_held(&self) -> bool {
        self.away.borrow().is_some() || self.shared.container_effects_held.load(Ordering::Relaxed)
    }

    /// Whether some row may have enrolled an SVG paint resource, which only then has to be synced again.
    pub(crate) fn svg_paint_resources_may_be_enrolled(&self) -> bool {
        self.shared.svg_paint_resources_enrolled.load(Ordering::Relaxed)
    }

    /// Whether some element may have random base values to keep, which only then is worth asking the render state.
    pub(crate) fn element_random_base_values_may_exist(&self) -> bool {
        self.shared.element_random_base_values_exist.load(Ordering::Relaxed)
    }

    /// Whether a job of the host made the document's render state, for a unit test.
    #[cfg(test)]
    pub(crate) fn has_made_state(&self) -> bool {
        let seed = self.seed.take();
        let made = seed.is_none();
        self.seed.set(seed);
        made
    }

    /// The arena of the document's render state, for a unit test that writes it directly, whose render owner is the
    /// test's own thread. The host knows nothing of what the test writes.
    #[cfg(test)]
    pub(crate) fn arena_for_test(&self) -> *mut crate::layout::LayoutNodeArena {
        self.facts.set(None);
        owner::with_state(self.document, self.seed.take(), |state| {
            std::ptr::from_mut(state.arena.arena_mut())
        })
    }

    /// Runs `job` on the document's render state on the render owner, after the writes the host queued, and waits for
    /// it, taking the frame in flight in first with `wait`. A job that `writes` leaves the paint and hit testing
    /// properties prepared from the state stale. A host callback the job makes may reach the state again, as a layout
    /// round's read of an element's style does, which runs right there on the owner.
    pub(crate) fn run<R>(&self, wait: impl RenderWait, writes: bool, job: impl FnOnce(&mut RenderState) -> R) -> R {
        assert!(
            wait.reaches(self),
            "a begun read reaches only the render state of its own document"
        );
        if writes {
            self.note_render_state_write();
        }
        self.take_frame_in(wait);
        let job = Waited(job);
        let (answer, marks) = self.changes.drain(|changes| {
            let (document, seed, marks) = (self.document, self.seed.take(), self.lend_marks());
            let in_job = self.in_job.replace(true);
            // A job that writes the state moves it on from the frame the host presented last, which a lane no longer forks.
            let moves_state = writes
                || changes
                    .as_slice()
                    .iter()
                    .any(|change| !change.keeps_the_presented_frame());
            let answer = on_render_side(move || {
                if moves_state {
                    super::clock::note_host_write(document);
                }
                owner::with_state(document, seed, |state| {
                    state.with_marks(marks, |state| {
                        state.apply(changes);
                        Waited((job.into_inner()(state), state.take_owed()))
                    })
                })
            });
            self.in_job.set(in_job);
            answer
        });
        let (answer, owed) = answer.into_inner();
        // A job lent the marks folded in those the streamed writes made, and brought back what they owe.
        if marks.is_some() {
            self.streamed.set(false);
        }
        self.take_marks_back(marks);
        // A write a host callback of the job queued comes after the facts the job left, which the host's reads of them
        // learn from the queue.
        self.pay(owed);
        answer
    }

    /// Answers `read` of the document's render state, spending `wait`, as of every write the host queued. A read leaves
    /// the paint and hit testing properties prepared from the state current: what it brings up to date first, a
    /// preparation leaves up to date, or only a layout round reads, but for overflow it measures first, which can
    /// change what the preparation answers, and so writes the render state.
    pub(crate) fn ask<R>(&self, wait: impl RenderWait, read: impl FnOnce(&mut RenderState) -> R) -> R {
        let (answer, measured_for_preparation) = self.run(wait, false, |state| {
            let awaited = state.arena_mut().scrollable_overflow.measurement_awaits_preparation();
            let answer = read(state);
            let awaits = state.arena_mut().scrollable_overflow.measurement_awaits_preparation();
            (answer, !awaited && awaits)
        });
        if measured_for_preparation {
            self.note_render_state_write();
        }
        answer
    }

    /// The rows the render state published last, unless the host wrote them since, or none were published yet.
    #[cfg(test)]
    pub(crate) fn rows(&self) -> Option<Rc<RowSnapshot>> {
        let rows = self.rows.borrow().clone()?;
        let version = self.ask(super::ScriptForcedRead::for_test(), |state| state.rows_version());
        rows.reads_as(version).then_some(rows)
    }

    /// Whether `rows` read as the arena does as of every write the host made, which the host knows without asking only
    /// where it wrote nothing since its last job that may have moved the rows on.
    fn knows_rows_read_as_arena(&self, rows: &RowSnapshot) -> bool {
        self.changes.moves().rows == RowWrite::None
            && self.facts.get().is_some_and(|facts| rows.reads_as(facts.rows_version))
    }

    /// The rows the host has, where `reads_as` says they read as the arena does as of every write the host made, which
    /// most writes leave as they are, as far as `moved` does not reach: the host knows it without asking only where no
    /// job runs.
    fn known_rows(
        &self,
        moved: RowWrite,
        reads_as: impl FnOnce(&RowSnapshot, RowsVersion) -> bool,
    ) -> Option<Rc<RowSnapshot>> {
        let facts = self.facts_where(|moves| moves.rows < moved)?;
        self.rows
            .borrow()
            .as_ref()
            .filter(|rows| reads_as(rows, facts.rows_version))
            .cloned()
    }

    /// The rows as of every change the host queued, which the render state publishes again first where the ones the
    /// host has may be stale, spending `wait`.
    pub(crate) fn fresh_rows(&self, wait: impl RenderWait) -> Rc<RowSnapshot> {
        self.rows_as_of_writes(wait, false)
    }

    /// What each row is and the row each node is bound to, as of every change the host queued. The host reads them from
    /// the rows it has where no write since changed them, as installing a style does not, and otherwise from rows the
    /// render state publishes again first, spending `wait`.
    pub(crate) fn row_identities(&self, wait: impl RenderWait) -> RowIdentities {
        self.known_row_identities()
            .unwrap_or_else(|| RowIdentities::of(self.rows_as_of_writes(wait, false)))
    }

    /// The style record each row has, and what each row is, as of every change the host queued, for a read of the row
    /// `id`. The host reads them from the rows it has where no write since changed the style of `id`, as a write of
    /// what a row holds but its style does not, nor a write of another row's style, and otherwise from rows the render
    /// state publishes again first, spending `wait`.
    pub(crate) fn row_styles_of(&self, id: crate::layout::node_data::NodeSlotId, wait: impl RenderWait) -> RowStyles {
        RowStyles::of(
            self.known_rows(RowWrite::Styles, |rows, version| {
                rows.reads_styles_as(version) && !self.changes.may_write_style_of(id, rows)
            })
            .unwrap_or_else(|| self.rows_as_of_writes(wait, false)),
        )
    }

    /// Whether the row in `slot` is populated, as of every change the host queued, where the host knows it without
    /// asking: only a layout round or a paint pass populates a row, and only a write of what a row is resets one.
    pub(crate) fn known_paintable_row_is_populated(&self, slot: crate::layout::node_data::NodeSlotId) -> Option<bool> {
        let rows = self.known_rows(RowWrite::Identities, |rows, version| rows.reads_population_as(version))?;
        Some(rows.paintable.paintable_row_is_populated(slot))
    }

    /// What each row is and the row each node is bound to, as of every change the host queued, where the host knows
    /// them without asking.
    pub(crate) fn known_row_identities(&self) -> Option<RowIdentities> {
        let rows = self.known_rows(RowWrite::Identities, |rows, version| {
            rows.reads_identity_as(version.identity())
        })?;
        Some(RowIdentities::of(rows))
    }

    /// Like [`Self::fresh_rows`], with every row's scrollable overflow measured, as a read of overflow needs.
    pub(crate) fn fresh_measured_rows(&self, wait: impl RenderWait) -> Rc<RowSnapshot> {
        self.rows_as_of_writes(wait, true)
    }

    /// Answers `read` from the rows as of every write the host made, through the paint side's reads, spending `wait`
    /// where the rows have to be published again, with every row's overflow measured where `measure_overflow`.
    pub(crate) fn read_rows<R>(
        &self,
        wait: impl RenderWait,
        measure_overflow: bool,
        read: impl FnOnce(&PaintSource<'_>) -> R,
    ) -> R {
        let rows = self.rows_as_of_writes(wait, measure_overflow);
        read(&PaintSource::over_rows(&rows.paintable, &self.absolute_rects))
    }

    /// Like [`Self::read_rows`], without measuring overflow, where no job runs: the arena a job writes moves on from the
    /// rows the host has, and a host callback of the job reads the arena.
    pub(crate) fn read_rows_between_jobs<R>(
        &self,
        wait: impl RenderWait,
        read: impl FnOnce(&PaintSource<'_>) -> R,
    ) -> Option<R> {
        (!self.in_job.get()).then(|| self.read_rows(wait, false, read))
    }

    /// Answers `read` from the rows the host has, where it knows they read as the arena does as of every write it made,
    /// through the paint side's reads.
    pub(crate) fn read_known_rows<R>(&self, read: impl FnOnce(&RowSnapshot, &PaintSource<'_>) -> R) -> Option<R> {
        let rows = self.known_rows(RowWrite::Rows, |rows, version| rows.reads_as(version))?;
        Some(read(
            &rows,
            &PaintSource::over_rows(&rows.paintable, &self.absolute_rects),
        ))
    }

    /// Answers `read` from the rows as of every write the host made, as [`Self::read_rows`] does, handing it the text
    /// rows that do not carry the text they render as well, by slot index.
    pub(crate) fn read_rows_with_rendered_text<R>(
        &self,
        wait: impl RenderWait,
        read: impl FnOnce(&PaintSource<'_>, &[NodeSlotId]) -> R,
    ) -> R {
        let rows = self.rows_as_of_writes(wait, false);
        read(
            &PaintSource::over_rows(&rows.paintable, &self.absolute_rects),
            rows.text_awaiting_render(),
        )
    }

    /// The rows as of every write the host made: the ones the host has where it knows they still read as the arena,
    /// and otherwise those the render state answers, spending `wait`, which are the ones the host has where they still
    /// read as the arena once the owner has applied the host's writes.
    fn rows_as_of_writes(&self, wait: impl RenderWait, measure_overflow: bool) -> Rc<RowSnapshot> {
        let held = self
            .rows
            .borrow()
            .clone()
            .filter(|rows| !measure_overflow || rows.overflow_is_measured());
        if let Some(rows) = held.as_ref().filter(|rows| self.knows_rows_read_as_arena(rows)) {
            return Rc::clone(rows);
        }
        let held_version = held.as_ref().map(|rows| rows.version());
        let published = self.ask(wait, |state| {
            let arena = state.arena_mut();
            (held_version != Some(arena.rows_version())).then(|| arena.publish_row_snapshot(measure_overflow))
        });
        match published {
            Some(rows) => {
                let rows = Rc::new(rows);
                *self.rows.borrow_mut() = Some(Rc::clone(&rows));
                rows
            }
            None => held.expect("the owner keeps only rows the host holds"),
        }
    }

    /// Keeps `rows`, which the render state published as a job of the host's ended, as the rows it holds.
    pub(crate) fn keep_rows(&self, rows: RowSnapshot) {
        *self.rows.borrow_mut() = Some(Rc::new(rows));
    }

    /// Lets go of the rows the render state published last, before a job the host waits for writes them. The host
    /// reads none while the job runs, and those it reads after the job are published after it, so the job writes the
    /// chunks nothing else holds in place rather than copying them for rows nobody reads again. Rows an outer call
    /// still reads stay with it.
    pub(crate) fn let_go_of_rows(&self) {
        self.rows.borrow_mut().take();
    }

    /// Keeps what the style transaction the host took answered, until the host ends the transaction.
    pub(crate) fn keep_style_transaction(&self, answer: StyleJobAnswer) -> std::cell::Ref<'_, StyleJobAnswer> {
        // The atoms the host interned forget the ones the engine reclaimed, before the host interns a name again.
        self.engine_memo
            .atoms
            .borrow_mut()
            .forget(answer.reclaimed_style_atoms());
        *self.style_transaction.borrow_mut() = Some(answer);
        std::cell::Ref::map(self.style_transaction.borrow(), |answer| {
            answer.as_ref().expect("the answer was kept above")
        })
    }

    /// The view of `record`, one the rows of the style transaction the host took last name, and the custom-property
    /// environment it was computed in.
    pub(crate) fn transaction_record(
        &self,
        record: u64,
    ) -> Option<(crate::css::style::bridge::FfiStyleRecordView, Option<u64>)> {
        self.style_transaction.borrow().as_ref()?.record(record)
    }

    /// What the style transaction the host took last answered of the pseudo-elements of `node`, where the host composes
    /// its row.
    pub(crate) fn transaction_pseudo_styles(
        &self,
        node: u32,
    ) -> Option<crate::css::style::style_job::ComposedPseudoStyles> {
        self.style_transaction.borrow().as_ref()?.composed_pseudo_styles(node)
    }

    /// Hands `hand_over` the actions of the transition step `decision` asks of a target with the `existing` transitions,
    /// where the style transaction the host took last decided it beside the row of the step's element, `node`, over the
    /// same inputs. Answers whether it did. The transitions such a step starts are what the host samples next, which
    /// reads the transaction's first sample of them.
    pub(crate) fn answer_decided_transition_step(
        &self,
        node: u32,
        decision: &crate::css::transition::TransitionDecision,
        existing: &[crate::css::transition::FfiExistingTransition],
        hand_over: impl FnOnce(&[crate::css::transition::FfiTransitionAction]),
    ) -> bool {
        let transaction = self.style_transaction.borrow();
        let step = transaction
            .as_ref()
            .and_then(|answer| answer.decided_transition_step(node))
            .and_then(|step| Some((step, step.answer(decision, existing)?)));
        self.fresh_transition_sample
            .set(step.and_then(|(step, _)| Some((node, step.take_fresh_sample()?))));
        let Some((_, actions)) = step else {
            return false;
        };
        hand_over(actions);
        true
    }

    /// Answers the sample `input` from the transaction's first sample of the transitions the step the host read last
    /// started, where it is the host's sample of them, and has the engine keep the timing each was sampled with.
    ///
    /// # Safety
    /// As for `rust_sample_animation_effects`.
    pub(crate) unsafe fn answer_fresh_transition_sample(
        &self,
        input: &crate::css::style_compute::FfiHostAnimationSample,
    ) -> Option<crate::css::style_compute::FfiHostAnimationSampleResult> {
        let (node, sample) = self.fresh_transition_sample.take()?;
        if node != input.style_node || input.pseudo_kind != crate::css::cascaded_properties::NO_PSEUDO_ELEMENT {
            return None;
        }
        // SAFETY: Guaranteed by the caller.
        let (result, timings) = unsafe { sample.answer(input) }?;
        self.queue_change(ArenaChange::Engine(
            crate::css::style::engine_calls::EngineWrite::AnimationEffectTimings {
                node: crate::css::style::tree::StyleNodeID::from_raw(node)?,
                timings,
            },
        ));
        Some(result)
    }

    /// Lets go of what the style transaction the host took last answered.
    pub(crate) fn end_style_transaction(&self) {
        self.fresh_transition_sample.take();
        self.style_transaction.borrow_mut().take();
    }

    /// Starts an update pass of the document's compositor animations, with none published.
    pub(crate) fn begin_compositor_animation_update(&self) {
        self.compositor_animations.borrow_mut().clear();
    }

    /// Adds what an effect published to the compositor animations of the current update pass.
    pub(crate) fn publish_compositor_animations(&self, animations: impl IntoIterator<Item = VisualAnimation>) {
        self.compositor_animations.borrow_mut().extend(animations);
    }

    /// Ends the current update pass of the document's compositor animations, answering what its effects published.
    pub(crate) fn take_compositor_animations(&self) -> Vec<VisualAnimation> {
        self.compositor_animations.take()
    }

    /// What the document keeps of its recordings.
    pub(crate) fn recording(&self) -> RefMut<'_, RecordingSlot> {
        self.recording.borrow_mut()
    }
}

/// Creates the host of a new document.
/// The render owner makes the state with the host's first job, which waits for nothing more than the job does, and
/// which a document nothing renders may never send.
#[unsafe(no_mangle)]
pub extern "C" fn document_host_create() -> *mut DocumentHost {
    Box::into_raw(Box::new(DocumentHost::new()))
}

/// What a job the host waits for carries to the render owner and back.
struct Waited<T>(T);

// SAFETY: The host waits for the job, so what the job borrows of the host's stays live and unwritten until the render
// owner has answered, and the host reads what the answer names of the render state's before its next job, which is
// what may write it. The host callbacks a job makes run on the owner while the host waits, as a style transaction's do.
unsafe impl<T> Send for Waited<T> {}

impl<T> Waited<T> {
    fn into_inner(self) -> T {
        self.0
    }
}

/// A document host with a render state, for a unit test, which destroys both when it is dropped.
#[cfg(test)]
pub(crate) struct TestHost(*mut DocumentHost);

#[cfg(test)]
impl TestHost {
    pub(crate) fn new() -> Self {
        Self(document_host_create())
    }

    pub(crate) fn host(&self) -> &DocumentHost {
        // SAFETY: The host lives until the test host is dropped.
        unsafe { &*self.0 }
    }

    /// The style engine of the host's document, as of every write the host queued, which the test reaches between the
    /// host's calls.
    pub(crate) fn engine(&self) -> crate::css::style::StyleEngineHandle {
        // SAFETY: The host lives until the test host is dropped.
        let host = unsafe { &*self.0 };
        host.ask(super::ScriptForcedRead::for_test(), |state| state.engine)
    }
}

#[cfg(test)]
impl Drop for TestHost {
    fn drop(&mut self) {
        // SAFETY: The test host made the host, and destroys it once.
        unsafe { document_host_destroy(self.0) };
    }
}

/// Whether the paint and hit testing properties of `host`'s document were prepared from its render state as it is, so
/// that preparing them again would find nothing to do: the host noted them current as it began to prepare them, and
/// wrote nothing to the render state since, queued, in place or by a job.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_paint_preparation_is_current(host: &DocumentHost) -> bool {
    host.paint_preparation_is_current.get()
}

/// Notes that the paint and hit testing properties of `host`'s document are current, as the host begins to prepare them:
/// they stay current until the host writes to the render state.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_note_paint_preparation_is_current(host: &DocumentHost) {
    host.paint_preparation_is_current.set(true);
}

/// What a scope of a read of a document's render state reads of the document's host: whether the host waits for the
/// frame, which it keeps up to date, and the read the scope lends.
#[repr(C)]
pub struct FfiReadScopeView {
    pub waits_for_frame: *const bool,
    pub read: *const BegunRead,
}

/// What a scope of a read of the render state of `host`'s document reads of `host`, which stays where it is for as long
/// as `host` lives.
///
/// # Safety
///
/// `host` must come from [`document_host_create`] and not be destroyed yet, and what this answers is read on its
/// document's thread only, until the host is destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_read_scope_view(host: &DocumentHost) -> FfiReadScopeView {
    FfiReadScopeView {
        waits_for_frame: host.waits_for_frame.as_ptr(),
        read: std::ptr::from_ref(&host.begun_read),
    }
}

/// Whether the frame of `host`'s document flies, where the host has not taken it in yet, whether or not it landed.
///
/// # Safety
///
/// `host` must come from [`document_host_create`] and not be destroyed yet, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_frame_flies(host: &DocumentHost) -> bool {
    host.frame_flies()
}

/// Sets the writes `host` queued aside, for the host to read its document's render state as the frame that flew last
/// left it, until document_host_queue_writes_set_aside() queues them behind the writes it queued meanwhile.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, with no writes set aside.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_set_writes_aside(host: &DocumentHost) {
    host.set_writes_aside();
}

/// Queues the writes `host` set aside behind the writes it queued meanwhile.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_queue_writes_set_aside(host: &DocumentHost) {
    host.queue_writes_set_aside();
}

/// Destroys `host`, and has the render owner drop the render state of its document, where a job made one, without
/// waiting for it.
///
/// # Safety
///
/// `host` must come from [`document_host_create`], be destroyed once, and no entry may reach the arena of its render
/// state any more.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_destroy(host: *mut DocumentHost) {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { Box::from_raw(host) };
    // The document's teardown is the host's own read: a frame in flight lands first, and what it brought back for the
    // host goes unpaid, as the host made nothing of it yet.
    host.take_frame_in(ForcedRead::of_teardown());
    // What writes the host streamed since its last job owe it, it pays as it would after a job.
    if host.streamed.get() {
        host.ask(ForcedRead::of_teardown(), |_| ());
    }
    let DocumentHost {
        document,
        seed,
        host_tables,
        ..
    } = *host;
    assert_eq!(
        host_tables.shells.borrow().len(),
        0,
        "document host destroyed with layout nodes"
    );
    // A state no job made has nothing to drop, and the writes still queued for it go with the host.
    if seed.into_inner().is_none() {
        post_to_render_side(move || owner::retire(document));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::style::boundary::StyleChange;
    use crate::css::style::engine_calls::{EngineWrite, absorb_element_style_input, has_deferred_element_style_input};
    use crate::css::style::transaction::STYLE_REACTION_INHERITED_STYLE;
    use crate::render_state::ScriptForcedRead;

    const INHERITED_STYLE_GROUPS: u8 = 0b0010;

    fn style_nodes<const COUNT: usize>(host: &DocumentHost) -> [StyleNodeID; COUNT] {
        let raw = host.ask(ScriptForcedRead::for_test(), |state| {
            let mut raw = [0; COUNT];
            state.engine_mut().allocate_style_nodes(&mut raw);
            raw
        });
        raw.map(|raw| StyleNodeID::from_raw(raw).unwrap())
    }

    fn record_input(host: &DocumentHost, node: StyleNodeID) {
        host.queue_change(ArenaChange::Style(StyleChange::RecordDerivedElementStyleInput {
            node: Some(node),
            reaction: STYLE_REACTION_INHERITED_STYLE,
            inherited_style_groups: INHERITED_STYLE_GROUPS,
        }));
    }

    fn hold_style_writes(host: &DocumentHost) {
        assert!(host.changes.is_empty());
        host.changes.give_back(host.changes.take_for_flight(true));
    }

    fn begin_drain(host: &DocumentHost) {
        *host.flown_style.borrow_mut() = Some(FlownStyle::Draining(Vec::new()));
        host.changes.stop_holding_style();
    }

    fn absorb_input(host: &DocumentHost, node: StyleNodeID) -> u32 {
        absorb_element_style_input(
            host,
            host.read_for_test(),
            node,
            STYLE_REACTION_INHERITED_STYLE,
            INHERITED_STYLE_GROUPS,
            false,
        )
    }

    fn host_has_deferred_input(host: &DocumentHost, node: StyleNodeID) -> bool {
        has_deferred_element_style_input(host, host.read_for_test(), node)
    }

    fn engine_has_deferred_input(host: &DocumentHost, node: StyleNodeID) -> bool {
        host.ask(ScriptForcedRead::for_test(), |state| {
            state.engine_mut().has_deferred_element_style_input(node)
        })
    }

    #[test]
    fn the_plan_of_an_earlier_update_leaves_a_later_take_in_standing() {
        let host = TestHost::new();
        let host = host.host();
        host.take_clock_lanes_in(1);
        // The update before ends as the next one runs, after the next one took the lanes in.
        host.take_clock_lanes_in(2);
        host.seal_clock_plan(None, 1);
        assert!(
            host.lanes_wait_for_the_update(),
            "the lanes wait for the plan of the update that took them in last"
        );
        host.seal_clock_plan(None, 2);
        assert!(!host.lanes_wait_for_the_update());
    }

    #[test]
    fn a_drain_beside_held_style_writes_folds_style_inputs_without_a_job() {
        let host = DocumentHost::for_test();
        let [parent, child, held] = style_nodes(&host);
        hold_style_writes(&host);
        record_input(&host, held);
        begin_drain(&host);
        // The parent's applied reaction defers inputs the host cannot name, until the drain's next job.
        record_input(&host, child);
        host.queue_change(ArenaChange::Style(StyleChange::NoteStyleReactionApplied {
            node: Some(parent),
            reaction: STYLE_REACTION_INHERITED_STYLE,
            inherited_style_groups_changed: INHERITED_STYLE_GROUPS,
            facts: 0,
        }));
        host.ask(ScriptForcedRead::for_test(), |_| ());

        assert_eq!(
            absorb_input(&host, child),
            u32::from(STYLE_REACTION_INHERITED_STYLE) | (u32::from(INHERITED_STYLE_GROUPS) << 8)
        );
        assert!(
            matches!(
                host.changes.queued.borrow().last(),
                Some(ArenaChange::Engine(EngineWrite::AbsorbElementStyleInput { .. }))
            ),
            "the host folds the input without a job, and queues the fold for the engine"
        );
        assert!(!engine_has_deferred_input(&host, child));
        host.end_style_drain();
    }

    #[test]
    fn a_held_style_write_defers_an_input_once_the_drain_queues_it() {
        let host = DocumentHost::for_test();
        let [node] = style_nodes(&host);
        hold_style_writes(&host);
        record_input(&host, node);
        begin_drain(&host);
        // No job applies the held write until the drain ends.
        assert!(!host_has_deferred_input(&host, node));
        assert_eq!(absorb_input(&host, node), 0);
        assert!(!engine_has_deferred_input(&host, node));

        host.end_style_drain();
        assert!(host_has_deferred_input(&host, node));
        assert!(engine_has_deferred_input(&host, node));
    }

    fn render_state_has_deferred_inputs(host: &DocumentHost, nodes: &[StyleNodeID]) -> bool {
        owner::with_state(host.document, None, |state| {
            nodes
                .iter()
                .all(|&node| state.engine_mut().has_deferred_element_style_input(node))
        })
    }

    #[test]
    fn a_batch_of_writes_streams_to_the_render_state_before_any_job() {
        let host = DocumentHost::for_test();
        let nodes: [StyleNodeID; DocumentHost::STREAMED_WRITES] = style_nodes(&host);
        for &node in &nodes[..DocumentHost::STREAMED_WRITES - 1] {
            record_input(&host, node);
        }
        assert!(
            !render_state_has_deferred_inputs(&host, &nodes[..1]),
            "a batch is not due yet"
        );
        record_input(&host, nodes[DocumentHost::STREAMED_WRITES - 1]);
        assert!(host.changes.is_empty());
        assert!(render_state_has_deferred_inputs(&host, &nodes));
        assert!(host.streamed.get());
        host.ask(ScriptForcedRead::for_test(), |_| ());
        assert!(
            !host.streamed.get(),
            "the next job brings back what the streamed writes owe"
        );
    }

    /// A host whose last job left the document's layout up to date.
    fn host_with_fresh_layout() -> (DocumentHost, crate::layout::node_data::NodeSlotId) {
        let host = DocumentHost::for_test();
        // SAFETY: The test writes the arena on its own thread, which is the render owner of a test.
        let arena = unsafe { &mut *host.arena_for_test() };
        let viewport = arena.allocate_unbound();
        arena.set_layout_root(viewport);
        arena.reset_layout_update_flags_in_subtree(viewport);
        host.ask(ScriptForcedRead::for_test(), |_| ());
        assert_eq!(host.known_layout_up_to_date_unless_built(), Some(true));
        (host, viewport)
    }

    #[test]
    fn writes_of_the_paint_state_alone_leave_fresh_layout_known() {
        use crate::layout::layout_changes::LayoutChange;
        use crate::painting::paint_changes::PaintChange;
        let (host, viewport) = host_with_fresh_layout();
        host.queue_change(ArenaChange::Layout(LayoutChange::InvalidateSearchableText));
        host.queue_change(ArenaChange::Paint(PaintChange::NoteVisualContextBoxDirty {
            node: viewport,
            kind: crate::painting::visual_context::dirty::VisualContextBoxDirtyKind::StyleValueChange,
        }));
        assert_eq!(host.known_layout_up_to_date_unless_built(), Some(true));
        assert_eq!(host.changes.moves().rows, RowWrite::None);

        host.queue_change(ArenaChange::Layout(LayoutChange::SetNeedsLayoutUpdate {
            node: viewport,
            propagate_through_ancestors: false,
        }));
        assert_eq!(host.known_layout_up_to_date_unless_built(), None);
    }

    #[test]
    fn clearing_paint_facts_the_rows_never_held_queues_nothing() {
        let (host, viewport) = host_with_fresh_layout();
        // SAFETY: The host is live, on its document's thread.
        unsafe {
            crate::painting::paint_changes::render_state_set_layer_image_paint_facts(
                &host,
                viewport,
                std::ptr::null(),
                0,
            );
            crate::painting::paint_changes::render_state_clear_search_text(&host);
        }
        assert!(host.changes.is_empty());
        assert_eq!(host.known_layout_up_to_date_unless_built(), Some(true));
    }

    #[test]
    fn writes_beside_a_style_drain_stay_queued() {
        let host = DocumentHost::for_test();
        let nodes: [StyleNodeID; DocumentHost::STREAMED_WRITES] = style_nodes(&host);
        begin_drain(&host);
        for node in nodes {
            record_input(&host, node);
        }
        assert_eq!(host.changes.len(), DocumentHost::STREAMED_WRITES);
        assert!(!host.streamed.get());
        host.end_style_drain();
    }
}
