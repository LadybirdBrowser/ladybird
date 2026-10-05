/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host keeps of a document's render state, which the render owner holds: its name, the frame that may fly
//! beside the host, and what the host reads between the jobs it hands the owner.

use super::clock::{ClockLease, ClockPlan, ClockTicks, LeaseLanding};
use super::owner::{self, DocumentId, SharedWithHost, StateSeed};
use super::questions::Question;
use super::wait::{BegunRead, HostRead, NodeRead, ReadRight, TaskStart, force_read_flown_style};
use super::{
    ArenaChange, ChangeQueue, CommittedRows, ForcedRead, Landing, NoFrameInFlight, Owed, RenderState, RenderWait,
    ScriptForcedRead, StateFacts, on_render_side, post_to_render_side,
};
use crate::css::css_pixels::CssPixelRect;
use crate::css::style::flight_style_rows::FlightStyleRow;
use crate::css::style::rule_writes::{PublishedRules, RuleWrite};
use crate::css::style::style_job::StyleJobAnswer;
use crate::css::style::tree::StyleNodeID;
use crate::layout::node_data::NodeSlotId;
use crate::layout::row_reads::{RowIdentities, RowSnapshot, RowStyles};
use crate::layout::tree_update_marks::{LayoutTreeUpdateMarkWrite, LayoutTreeUpdateMarks};
use crate::layout::{FlownRound, HostTables, LayoutRoundAnswer, RowsVersion, SealedRound};
use crate::painting::paint_read::PaintSource;
use crate::painting::presentation::Presentation;
use crate::painting::record::recorder_state::AbsoluteRectMemo;
use crate::painting::recording_slot::RecordingSlot;
use crate::painting::visual_animation::VisualAnimation;
use crate::render_state::TaskBoundary;
use crate::stage_thread::InFlight;
use std::cell::{Cell, RefCell, RefMut};
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::Ordering;

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
    /// The frame that flies beside the host, until the host takes it in, or the render clock's lease of the render state
    /// while a task runs, until any job of the host ends it.
    away: RefCell<Option<Away>>,
    /// Whether the host waits for the frame: it flies, or a layout round that flew with it or that the ticks of a clock
    /// lease ran is not paid yet. The host's scopes of a read and its document's layout read it where it is (see
    /// [`document_host_read_scope_view`]).
    waits_for_frame: Cell<bool>,
    host_tables: HostTables,
    recording: RefCell<RecordingSlot>,
    /// The rows the render state published last, which the host reads between messages.
    rows: RefCell<Option<Rc<RowSnapshot>>>,
    /// How far the arena's rows had been written after the last job the host handed the owner, where no write the host
    /// made since may have moved them on.
    arena_version: Cell<Option<RowsVersion>>,
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
    /// The read of the render state the host began and has not ended, if any.
    forced_read: RefCell<ReadScopes>,
    /// What the host's scopes of a read lend the entries they call.
    begun_read: BegunRead,
    /// The writes the host queued that the render state has not applied yet, in the order the host made them.
    changes: ChangeQueue,
    /// The style transaction that flew with the frame, from its landing until the host has drained its reactions.
    flown_style: RefCell<Option<FlownStyle>>,
    /// Whether the paint and hit testing properties the document prepared last were prepared from the render state as it
    /// is: nothing was written to it since, queued, in place or by a message. Preparing them again would find nothing
    /// to do.
    paint_preparation_is_current: Cell<bool>,
    /// The first layout round of a rendering update, sealed until the frame flies with it.
    sealed_round: RefCell<Option<SealedRound>>,
    /// The layout round that flew with the frame, from its landing until the host's next layout update pays it.
    flown_round: RefCell<Option<FlownRound>>,
    /// Whether the document's layout is up to date, where the host knows: a write only ever leaves layout staler, and
    /// only a layout round, or a frame's, lays it out, so stale layout stays stale until one runs, and fresh layout
    /// stays fresh until a job runs or the host writes the rows or the marks.
    layout_up_to_date: Cell<Option<bool>>,
    /// Whether a job of the host runs, whose host callbacks see rows it freed and the host has not heard of yet.
    in_job: Cell<bool>,
    /// The document's layout tree update marks, which the host lends the render owner's jobs.
    marks: RefCell<HostMarks>,
    /// The rules the host published to each of its document's style sheets.
    published_rules: PublishedRules,
    /// What the host learned of its document's style engine.
    engine_memo: crate::css::style::engine_calls::EngineMemo,
    /// The facts of the render state as the host's last job or frame left it, which a clock lease forgets.
    facts: Cell<Option<StateFacts>>,
    /// The attribute names whose value text the engine's selectors read, as the host's last job or frame left them.
    selector_value_text_names: RefCell<crate::css::style::SelectorValueTextNames>,
    /// What the clock leases the tasks after the last rendering update tick, until one takes it.
    clock_plan: RefCell<Option<ClockPlan>>,
    /// What the rounds of the ticks of the clock lease that landed last owe, until the host's next layout update pays it.
    clock_rounds: RefCell<Vec<LayoutRoundAnswer>>,
    /// The presentation of the document's navigable a clock lease brought back, until the navigable takes it again.
    presentation: RefCell<Option<Presentation>>,
    /// The border boxes of the elements a clock lease sampled in the last frame one of its ticks presented.
    presented_border_boxes: RefCell<Vec<(StyleNodeID, CssPixelRect)>>,
}

/// What runs on the render owner beside the host, which the host takes back before it hands the owner a job: the frame
/// that flies, or a clock lease, never both.
enum Away {
    Flying(InFlight<Landing>),
    Leased(ClockLease),
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
    /// The host drains the transaction's reactions, holding the writes it queued beside the transaction until the drain
    /// ends, and the rows the frame applied.
    Draining(Vec<ArenaChange>, Vec<FlightStyleRow>),
}

/// A read of a document's render state the host began: how many of the read's scopes are open, and the read itself
/// until its first job takes it.
#[derive(Default)]
struct ReadScopes {
    scopes: u32,
    read: Option<ForcedRead>,
}

impl DocumentHost {
    fn new() -> Self {
        let shared = SharedWithHost::new();
        Self {
            document: DocumentId::mint(),
            seed: Cell::new(Some(StateSeed { shared: shared.clone() })),
            shared,
            away: RefCell::default(),
            waits_for_frame: Cell::new(false),
            host_tables: HostTables::default(),
            recording: RefCell::default(),
            rows: RefCell::default(),
            arena_version: Cell::new(None),
            style_transaction: RefCell::default(),
            fresh_transition_sample: Cell::default(),
            absolute_rects: RefCell::default(),
            compositor_animations: RefCell::default(),
            forced_read: RefCell::default(),
            begun_read: BegunRead::of_host(),
            changes: ChangeQueue::default(),
            flown_style: RefCell::default(),
            paint_preparation_is_current: Cell::new(false),
            sealed_round: RefCell::default(),
            flown_round: RefCell::default(),
            layout_up_to_date: Cell::new(None),
            in_job: Cell::new(false),
            marks: RefCell::new(HostMarks {
                here: Some(LayoutTreeUpdateMarks::default()),
                beside_flight: LayoutTreeUpdateMarks::default(),
                written_beside_flight: Vec::new(),
            }),
            published_rules: PublishedRules::default(),
            engine_memo: Default::default(),
            facts: Cell::new(None),
            selector_value_text_names: RefCell::default(),
            clock_plan: RefCell::default(),
            clock_rounds: RefCell::default(),
            presentation: RefCell::default(),
            presented_border_boxes: RefCell::default(),
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
        self.begin_forced_read(false)
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
        if change.may_write_rows() {
            self.forget_fresh_layout();
        }
        self.changes.push(change);
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

    /// Takes the frame in flight in with `read`, and lends the writes the host queued to `apply`, for the render state
    /// to apply ahead of the host's next job. A style write queued beside a style transaction that flew stays queued,
    /// behind the drain of the transaction's reactions, which it is the next transaction's input to.
    fn drain_queued_changes<R>(&self, read: ReadRight, apply: impl FnOnce(std::vec::Drain<'_, ArenaChange>) -> R) -> R {
        self.take_frame_in(read);
        self.changes.drain(self.has_flown_style(), apply)
    }

    /// Takes the writes the host queued, for a style transaction that flies with them to the render owner.
    pub(super) fn take_queued_changes_for_flight(&self) -> Vec<ArenaChange> {
        // A clock lease ends before the frame flies, and the styles it gives back go first.
        self.end_clock_lease();
        debug_assert!(
            !self.has_flown_style(),
            "one style transaction of a document flies at a time"
        );
        self.changes.take()
    }

    /// Runs `job` on the document's render state on the render owner, after the writes the host queued, and waits for
    /// it, taking the frame in flight in first with `read`. A host callback the job makes may reach the state again, as
    /// a layout round's read of an element's style does, which runs right there on the owner.
    pub(super) fn reach<R: Send>(&self, read: ReadRight, job: impl FnOnce(&mut RenderState) -> R + Send) -> R {
        let ((answer, version, owed), marks) = self.drain_queued_changes(read, |changes| {
            let (document, seed, marks) = (self.document, self.seed.take(), self.lend_marks());
            let in_job = self.in_job.replace(true);
            let answer = on_render_side(move || {
                owner::with_state(document, seed, |state| {
                    state.with_marks(marks, |state| {
                        state.apply(changes);
                        let answer = job(state);
                        (answer, state.rows_version(), state.take_owed())
                    })
                })
            });
            self.in_job.set(in_job);
            answer
        });
        self.take_marks_back(marks);
        self.forget_fresh_layout();
        // A write a host callback of the job queued comes after the version the job answered, which the host's reads of
        // the rows learn from the queue.
        self.arena_version.set(Some(version));
        self.pay(owed);
        answer
    }

    /// Pays what the writes a job or a frame applied owe the host, now that the host has it back.
    fn pay(
        &self,
        Owed {
            work,
            deferred_inputs,
            facts,
            selector_value_text_names,
        }: Owed,
    ) {
        self.facts.set(Some(facts));
        *self.selector_value_text_names.borrow_mut() = selector_value_text_names;
        self.engine_memo
            .deferred
            .borrow_mut()
            .follow_job(deferred_inputs, !self.changes.is_empty());
        if work.is_empty() {
            return;
        }
        // SAFETY: The host has its job or frame back, on its document's thread, or on the render owner in a host
        // callback of a job the host waits for, as every host callback runs.
        let main_thread = unsafe { crate::stage::from_ffi_entry(&OWED_WORK_PAYMENT, self) };
        for work in work {
            work.pay(&main_thread);
        }
    }

    /// Lets the frame fly beside the host, in the job `flight` submits to the owner with the document's name, until the
    /// host drains the reactions of the style transaction it flies with.
    pub(super) fn let_frame_fly(
        &self,
        flight: impl FnOnce(DocumentId, Option<StateSeed>, Option<LayoutTreeUpdateMarks>) -> InFlight<Landing>,
    ) {
        self.arena_version.set(None);
        let flight = flight(self.document, self.seed.take(), self.lend_marks());
        let previous = self.away.borrow_mut().replace(Away::Flying(flight));
        assert!(previous.is_none(), "one frame of a document flies at a time");
        self.note_frame_wait();
    }

    /// Takes the frame in flight in, where one flies, waiting for it to land, spending `read`: only a read waits for a
    /// frame. A lease ends, which waits for at most one tick and spends no read.
    #[inline]
    fn take_frame_in(&self, read: ReadRight) -> NoFrameInFlight {
        if self.away.borrow().is_some() {
            self.take_frame_back(read);
        }
        NoFrameInFlight(())
    }

    /// Takes back what runs beside the host: lands the frame in flight, or ends the lease.
    #[cold]
    fn take_frame_back(&self, read: ReadRight) {
        if self.frame_flies() {
            self.land_flying_frame(read);
        }
        self.end_clock_lease();
    }

    /// Ends the document's clock lease, where one runs: the boxes take back the styles the host installed before
    /// anything reads them, the host's next layout update pays what the ticks laid out, and the recorder state and
    /// presentation come back.
    fn end_clock_lease(&self) {
        match self.away.take() {
            Some(Away::Leased(lease)) => self.lease_landed(lease.end()),
            other => *self.away.borrow_mut() = other,
        }
    }

    fn lease_landed(
        &self,
        LeaseLanding {
            recorder,
            presentation,
            ticked,
            owed,
            presented_border_boxes,
            ..
        }: LeaseLanding,
    ) {
        if !presented_border_boxes.is_empty() {
            *self.presented_border_boxes.borrow_mut() = presented_border_boxes;
        }
        if !ticked.is_empty() {
            self.changes.push_front(ArenaChange::Layout(
                crate::layout::layout_changes::LayoutChange::RestoreHostStyles(ticked),
            ));
        }
        self.clock_rounds.borrow_mut().extend(owed);
        self.note_frame_wait();
        self.recording().give_back_recorder(recorder);
        *self.presentation.borrow_mut() = Some(presentation);
    }

    /// Keeps `plan` for the clock lease of the tasks after a rendering update, in place of the last one, or none.
    pub(crate) fn seal_clock_plan(&self, plan: Option<ClockPlan>) {
        *self.clock_plan.borrow_mut() = plan;
    }

    /// Leases the render state, with the recorder state and `presentation`, to the render clock as `start` begins a
    /// task, where the host has a plan, no frame flies and the recorder state is here. The render state stays with the
    /// render owner, whose ticks reach it by the document's name. Answers the ticks the render clock hands the lease, or
    /// gives `presentation` back.
    pub(super) fn lease_clock(
        &self,
        start: &TaskStart,
        presentation: Presentation,
    ) -> Result<Arc<ClockTicks>, Presentation> {
        if self.away.borrow().is_some() {
            return Err(presentation);
        }
        let Some(plan) = self.clock_plan.take() else {
            return Err(presentation);
        };
        let Some(recorder) = self.recording().take_recorder_for_clock() else {
            *self.clock_plan.borrow_mut() = Some(plan);
            return Err(presentation);
        };
        // The ticks write the rows, what the paint properties are prepared from and the facts of the state, so the
        // host's next read asks the owner, and ends the lease first.
        self.arena_version.set(None);
        self.facts.set(None);
        self.note_render_state_write();
        let (lease, ticks) = ClockLease::begin(start, self.document, recorder, presentation, plan);
        *self.away.borrow_mut() = Some(Away::Leased(lease));
        Ok(ticks)
    }

    /// Hands `pay` what each round the ticks of the clock lease that landed last ran owes, for the layout update that
    /// pays it. The host waits for the frame while any is owed, so a layout update that owes none pays a load.
    #[inline]
    pub(crate) fn pay_clock_rounds(&self, pay: impl FnMut(LayoutRoundAnswer)) {
        if self.waits_for_frame.get() {
            self.pay_owed_clock_rounds(pay);
        }
    }

    #[cold]
    fn pay_owed_clock_rounds(&self, pay: impl FnMut(LayoutRoundAnswer)) {
        let rounds = self.clock_rounds.take();
        self.note_frame_wait();
        rounds.into_iter().for_each(pay);
    }

    /// Lands the frame in flight with `read`. A read the host began spends itself where no job took it yet, and is the
    /// host's own read otherwise, as is a read begun where no frame flew, which the host knows nothing of: the frame
    /// flew from inside it.
    #[cold]
    fn land_flying_frame(&self, read: ReadRight) {
        let read = match read {
            ReadRight::Forced(read) => read,
            ReadRight::Here(_) => unreachable!("a frame flies only from a rendering update, which no reach runs"),
            ReadRight::Begun(_) => self
                .take_forced_read()
                .unwrap_or_else(|| ForcedRead::Host(HostRead::begun())),
        };
        force_read_flown_style(read, self);
    }

    /// Takes the frame in flight in, where one flies, spending `read`.
    pub(crate) fn take_frame_in_with(&self, read: &BegunRead) {
        assert!(
            read.reaches(self),
            "a begun read reaches only the render state of its own document"
        );
        self.take_frame_in(read.into_read_right());
    }

    /// Lands the frame in flight, waiting for it, spending `read`: the style transaction that flew with it waits to be
    /// drained.
    pub(super) fn land(&self, read: ForcedRead) {
        match self.away.take() {
            Some(Away::Flying(flight)) => self.landed(flight.join(read)),
            other => *self.away.borrow_mut() = other,
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
        self.layout_up_to_date.set(None);
        let previous = self
            .flown_style
            .borrow_mut()
            .replace(FlownStyle::Landed(style, applied));
        debug_assert!(
            previous.is_none(),
            "one style transaction of a document flies at a time"
        );
        if let Some((round, rows)) = round {
            // The layout round that flew with the frame wrote the render state the paint properties are prepared from.
            self.note_render_state_write();
            *self.flown_round.borrow_mut() = Some(round);
            // The frame's job ended with the rows, so they read as the arena unless the host wrote it since.
            self.arena_version.set(Some(rows.version()));
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
        self.forget_fresh_layout();
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
    /// that flew or a clock lease ran and is not paid yet. A read of the layout in place then takes no frame in, and
    /// needs no read the host began.
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

    /// Whether the document's layout is up to date as of every write the host made, where the host knows without
    /// asking.
    pub(crate) fn known_layout_up_to_date(&self) -> Option<bool> {
        self.layout_up_to_date.get()
    }

    /// Notes what a job answered of whether the document's layout is up to date.
    pub(crate) fn note_layout_up_to_date(&self, up_to_date: bool) {
        self.layout_up_to_date.set(Some(up_to_date));
    }

    /// Forgets what the host knew of the document's layout, before a layout round may lay it out.
    pub(crate) fn forget_layout_up_to_date(&self) {
        self.layout_up_to_date.set(None);
    }

    fn forget_fresh_layout(&self) {
        if self.layout_up_to_date.get() == Some(true) {
            self.layout_up_to_date.set(None);
        }
    }

    /// The facts of the render state, where the host knows them: it wrote nothing that may move them since its last job
    /// or frame left them, and none runs.
    pub(crate) fn known_facts(&self) -> Option<StateFacts> {
        if !self.knows_engine_between_jobs() || self.changes.may_move_facts() {
            return None;
        }
        self.facts.get()
    }

    /// Where the engine's selectors' requirements of attribute value text are, where the host knows: none runs, and it
    /// queued no rule since its last job or frame, which is all that compiles a selector.
    pub(crate) fn known_selector_attribute_value_text_requirements_version(&self) -> Option<u64> {
        if !self.knows_engine_between_jobs() || self.changes.may_compile_selectors() {
            return None;
        }
        self.facts
            .get()
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

    /// Whether the layout tree builds owe the host image resources, where the host knows: none runs. Only a build of a
    /// job or a frame comes to owe some, never a write, so the fact its last job or frame left holds whatever it queued.
    pub(crate) fn known_owed_image_resources(&self) -> Option<bool> {
        if !self.knows_engine_between_jobs() {
            return None;
        }
        self.facts.get().map(|facts| facts.owes_image_resources)
    }

    /// Whether the host knows what its engine holds as of the writes it queued: no job of the engine runs, as a host
    /// callback of one does, and no frame flies.
    pub(crate) fn knows_engine_between_jobs(&self) -> bool {
        !self.in_job.get() && !self.frame_flies()
    }

    fn frame_flies(&self) -> bool {
        matches!(*self.away.borrow(), Some(Away::Flying(_)))
    }

    fn note_frame_wait(&self) {
        self.waits_for_frame
            .set(self.frame_flies() || self.flown_round.borrow().is_some() || !self.clock_rounds.borrow().is_empty());
    }

    /// Ends the document's clock lease, where one runs, spending `_wait`.
    pub(crate) fn end_clock_lease_waiting(&self, _wait: impl RenderWait) {
        self.end_clock_lease();
    }

    /// Whether the last rendering update left a plan for a clock lease no task has taken yet.
    pub(super) fn has_clock_plan(&self) -> bool {
        self.clock_plan.borrow().is_some()
    }

    /// Ends the clock lease, where one runs, and takes the presentation a lease brought back, if any.
    pub(super) fn take_back_presentation(&self) -> Option<Presentation> {
        self.end_clock_lease();
        self.presentation.take()
    }

    /// Ends the clock lease, where one runs, and drops the plan for the next one, which no longer stands.
    pub(super) fn end_clock_lease_and_plan(&self) {
        self.end_clock_lease();
        self.clock_plan.take();
    }

    /// Ends the clock lease, where one runs, and answers the border box of `element` in the last frame a tick of a clock
    /// lease presented, if it presented one.
    pub(super) fn presented_border_box(&self, element: StyleNodeID) -> Option<CssPixelRect> {
        self.end_clock_lease();
        self.presented_border_boxes
            .borrow()
            .iter()
            .find_map(|&(presented, rect)| (presented == element).then_some(rect))
    }

    /// The ticks of the document's clock lease, where one runs.
    pub(super) fn clock_ticks(&self) -> Option<Arc<ClockTicks>> {
        match &*self.away.borrow() {
            Some(Away::Leased(lease)) => Some(Arc::clone(lease.ticks())),
            Some(Away::Flying(_)) | None => None,
        }
    }

    /// Whether the document's frame still flies, where it has not landed: one that has is taken in. The event loop asks
    /// between two tasks, so this never waits.
    pub(crate) fn frame_still_flies(&self, boundary: &TaskBoundary) -> bool {
        let flight = match self.away.take() {
            Some(Away::Flying(flight)) => flight,
            other => {
                *self.away.borrow_mut() = other;
                return false;
            }
        };
        match flight.try_take(boundary) {
            Ok(landing) => {
                self.landed(landing);
                false
            }
            Err(flight) => {
                *self.away.borrow_mut() = Some(Away::Flying(flight));
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
    pub(crate) fn begin_style_drain(&self, read: ReadRight) -> StyleJobAnswer {
        self.take_frame_in(read);
        let mut flown = self.flown_style.borrow_mut();
        let Some(FlownStyle::Landed(answer, applied)) = flown.take() else {
            panic!("the host drains a style transaction that flew and has landed");
        };
        *flown = Some(FlownStyle::Draining(self.changes.hold_style_writes(), applied));
        answer
    }

    /// Whether the frame the host drains the style transaction of applied the element `style_node` names its record
    /// `style_record` ahead of the host, and marked the relayout the move asks for.
    pub(crate) fn frame_marked_relayout(&self, style_node: StyleNodeID, style_record: u64) -> bool {
        let flown = self.flown_style.borrow();
        let Some(FlownStyle::Draining(_, applied)) = &*flown else {
            return false;
        };
        applied
            .binary_search_by_key(&style_node, |row| row.style_node)
            .is_ok_and(|index| applied[index].new_style_record == style_record && applied[index].relayout)
    }

    /// Ends the drain of the style transaction that flew: the writes the host queued beside it are queued again, behind
    /// what the drain wrote.
    pub(crate) fn end_style_drain(&self) {
        let Some(FlownStyle::Draining(beside, _)) = self.flown_style.borrow_mut().take() else {
            panic!("the host ends the drain it began");
        };
        self.changes.requeue(beside);
    }

    /// Begins a read of the document's render state that the host waits for, for a script API call where `by_script`
    /// and for the host's own read otherwise, and answers the read, which the scope lends the entries it calls. A scope
    /// begun inside the document's open read belongs to that read. A scope begins a read only where the host waits for
    /// the frame.
    pub(super) fn begin_forced_read(&self, by_script: bool) -> &BegunRead {
        let mut begun = self.forced_read.borrow_mut();
        begun.scopes += 1;
        if begun.scopes == 1 {
            begun.read = Some(if by_script {
                ForcedRead::Script(ScriptForcedRead::at_script_entry(&FORCED_READ_SCOPE))
            } else {
                ForcedRead::Host(HostRead::begun())
            });
        }
        &self.begun_read
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

    /// Ends a scope of the read the host began. The outermost drops the read where no job took it.
    pub(super) fn end_forced_read(&self) {
        let mut begun = self.forced_read.borrow_mut();
        assert!(begun.scopes > 0, "a forced read ends where it began");
        begun.scopes -= 1;
        if begun.scopes == 0 {
            begun.read = None;
        }
    }

    /// Takes the read the host began, where no job took it yet, for the read's first layout round.
    pub(crate) fn take_forced_read(&self) -> Option<ForcedRead> {
        self.forced_read.borrow_mut().read.take()
    }

    /// Takes the read the host began, where no job took it yet, for the read's style transaction. A read whose first
    /// job was a style transaction keeps what that left its first layout round.
    pub(crate) fn take_unstyled_read(&self) -> Option<ForcedRead> {
        let mut begun = self.forced_read.borrow_mut();
        match begun.read {
            Some(ForcedRead::Script(_) | ForcedRead::Host(_)) => begun.read.take(),
            Some(ForcedRead::AfterStyle(_)) | None => None,
        }
    }

    /// Leaves `read` to the next job of the read the host began, where one is open.
    pub(super) fn leave_forced_read(&self, read: ForcedRead) {
        let mut begun = self.forced_read.borrow_mut();
        if begun.scopes > 0 {
            begun.read = Some(read);
        }
    }

    /// A fresh identity for an element-sourced declaration block's contents.
    pub(crate) fn next_declaration_block_version(&self) -> u32 {
        crate::css::style::next_declaration_block_version(&self.shared.declaration_block_versions)
    }

    /// Whether the document's style engine may keep a row's container effects for the host to take, where the host
    /// knows it without asking: no frame flies, whose style may note some.
    pub(crate) fn container_effects_may_be_held(&self) -> bool {
        self.waits_for_frame.get() || self.shared.container_effects_held.load(Ordering::Relaxed)
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
        self.arena_version.set(None);
        self.facts.set(None);
        owner::with_state(self.document, self.seed.take(), |state| {
            std::ptr::from_mut(state.arena.arena_mut())
        })
    }

    /// Asks the document's render state `question`, spending `wait`, and answers what it answered as of every write the
    /// host queued.
    pub(super) fn ask<Q: Question + Send>(&self, wait: impl RenderWait, question: Q) -> Q::Answer
    where
        Q::Answer: Send,
    {
        assert!(
            wait.reaches(self),
            "a begun read reaches only the render state of its own document"
        );
        if !Q::LEAVES_PAINT_PREPARATION_CURRENT {
            self.note_render_state_write();
        }
        // A question that reads overflow first measures what it finds unmeasured, which can change what the preparation
        // answers: such a measurement writes the render state, whatever the question.
        let (answer, measured_for_preparation) = self.reach(wait.into_read_right(), move |state| {
            let awaited = state.arena.arena().scrollable_overflow.measurement_awaits_preparation();
            let answer = state.answer(question);
            let awaits = state.arena.arena().scrollable_overflow.measurement_awaits_preparation();
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
        let version = self
            .ask(
                ScriptForcedRead::for_test(),
                super::questions::ArenaRead::new((), |arena, ()| arena.rows_version()),
            )
            .0;
        rows.reads_as(version).then_some(rows)
    }

    /// Whether `rows` read as the arena does as of every write the host made, which the host knows without asking only
    /// where it wrote nothing since its last job that may have moved the rows on.
    fn knows_rows_read_as_arena(&self, rows: &RowSnapshot) -> bool {
        !self.changes.may_write_rows() && self.arena_version.get().is_some_and(|version| rows.reads_as(version))
    }

    /// Like [`Self::knows_rows_read_as_arena`], for what each row is and the row each node is bound to alone, which
    /// most writes leave as they are.
    fn knows_identities_read_as_arena(&self, rows: &RowSnapshot) -> bool {
        !self.in_job.get()
            && !self.changes.may_write_row_identities()
            && self
                .arena_version
                .get()
                .is_some_and(|version| rows.reads_identity_as(version.identity()))
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

    /// The style record each row has, and what each row is, as of every change the host queued. The host reads them
    /// from the rows it has where no write since changed them, as a write of what a row holds but its style does not,
    /// and otherwise from rows the render state publishes again first, spending `wait`.
    pub(crate) fn row_styles(&self, wait: impl RenderWait) -> RowStyles {
        let rows = self.rows.borrow();
        if let Some(rows) = rows.as_ref().filter(|rows| {
            !self.in_job.get()
                && !self.changes.may_write_row_styles()
                && self
                    .arena_version
                    .get()
                    .is_some_and(|version| rows.reads_styles_as(version))
        }) {
            return RowStyles::of(Rc::clone(rows));
        }
        drop(rows);
        RowStyles::of(self.rows_as_of_writes(wait, false))
    }

    /// Whether the row in `slot` is populated, as of every change the host queued, where the host knows it without
    /// asking: only a layout round or a paint pass populates a row, and only a write of what a row is resets one.
    pub(crate) fn known_paintable_row_is_populated(&self, slot: crate::layout::node_data::NodeSlotId) -> Option<bool> {
        let rows = self.rows.borrow();
        let rows = rows.as_ref().filter(|rows| {
            !self.in_job.get()
                && !self.changes.may_write_row_identities()
                && self
                    .arena_version
                    .get()
                    .is_some_and(|version| rows.reads_population_as(version))
        })?;
        Some(rows.paintable.paintable_row_is_populated(slot))
    }

    /// What each row is and the row each node is bound to, as of every change the host queued, where the host knows
    /// them without asking.
    pub(crate) fn known_row_identities(&self) -> Option<RowIdentities> {
        let rows = self.rows.borrow();
        let rows = rows.as_ref().filter(|rows| self.knows_identities_read_as_arena(rows))?;
        Some(RowIdentities::of(Rc::clone(rows)))
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
        let rows = self.rows.borrow();
        let rows = rows
            .as_ref()
            .filter(|rows| !self.in_job.get() && self.knows_rows_read_as_arena(rows))?;
        Some(read(
            rows,
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
        let question = CommittedRows {
            measure_overflow,
            held: held.as_ref().map(|rows| rows.version()),
        };
        match self.ask(wait, question) {
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

    /// Answers the transition step `decision` asks of `properties`, writing the values each transition compared and the
    /// decisions into `actions`, where the style transaction the host took last decided it beside the row of the
    /// step's element, `node`, over the same inputs. Answers whether it did. The transitions such a step starts are what
    /// the host samples next, which reads the transaction's first sample of them.
    pub(crate) fn answer_decided_transition_step(
        &self,
        node: u32,
        decision: &crate::css::transition::TransitionDecision,
        properties: &mut [crate::css::transition::FfiTransitionPropertyInput],
        actions: &mut [crate::css::transition::FfiTransitionAction],
    ) -> bool {
        let transaction = self.style_transaction.borrow();
        let step = transaction
            .as_ref()
            .and_then(|answer| answer.decided_transition_step(node))
            .filter(|step| step.answer(decision, properties, actions));
        self.fresh_transition_sample
            .set(step.and_then(|step| Some((node, step.take_fresh_sample()?))));
        step.is_some()
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

/// A document host with a render state, for a unit test, which destroys both when it is dropped.
#[cfg(test)]
pub(crate) struct TestHost(*mut DocumentHost);

#[cfg(test)]
impl TestHost {
    pub(crate) fn new() -> Self {
        Self(document_host_create())
    }

    pub(crate) fn host(&self) -> *const DocumentHost {
        self.0
    }

    /// The style engine of the host's document, as of every write the host queued, which the test reaches between the
    /// host's calls.
    pub(crate) fn engine(&self) -> crate::css::style::StyleEngineHandle {
        // SAFETY: The host lives until the test host is dropped.
        let host = unsafe { &*self.0 };
        host.reach(ScriptForcedRead::for_test().into_read_right(), |state| state.engine)
    }
}

#[cfg(test)]
impl Drop for TestHost {
    fn drop(&mut self) {
        // SAFETY: The test host made the host, and destroys it once.
        unsafe { document_host_destroy(self.0) };
    }
}

/// Marks the scope of a read of a document's render state that a script API call begins, which mints the call's
/// forced read.
pub(crate) struct ForcedReadScope {
    _private: (),
}

const FORCED_READ_SCOPE: ForcedReadScope = ForcedReadScope { _private: () };

/// Begins a read of the render state of `host`'s document that the host waits for, for a script API call where
/// `by_script` and for the host's own read otherwise, which the scope lends the entries that reach the render state
/// where the host is until it ends the read (see [`document_host_read_scope_view`]). The read's first style or layout
/// job spends it. A scope begins one only where the host waits for the frame.
///
/// # Safety
///
/// `host` must come from [`document_host_create`] and not be destroyed yet, on its document's thread, with a
/// [`document_host_end_forced_read`] for each call. `by_script` only for a scope a script API call opens.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_begin_forced_read(host: *const DocumentHost, by_script: bool) {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.begin_forced_read(by_script);
}

/// Ends a scope of the read of the render state of `host`'s document that the host began.
///
/// # Safety
///
/// As for [`document_host_begin_forced_read`], once for each of its calls.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_end_forced_read(host: *const DocumentHost) {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.end_forced_read();
}

/// Whether the paint and hit testing properties of `host`'s document were prepared from its render state as it is, so
/// that preparing them again would find nothing to do: the host noted them current as it began to prepare them, and
/// wrote nothing to the render state since, queued, in place or by a message.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_paint_preparation_is_current(host: *const DocumentHost) -> bool {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.paint_preparation_is_current.get()
}

/// Notes that the paint and hit testing properties of `host`'s document are current, as the host begins to prepare them:
/// they stay current until the host writes to the render state.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_note_paint_preparation_is_current(host: *const DocumentHost) {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.paint_preparation_is_current.set(true);
}

/// What a scope of a read of a document's render state reads of the document's host: whether the host waits for the
/// frame, which it keeps up to date, and the read the scope lends. Only a read begun where the frame flies may take
/// the frame in, so a scope begins one with [`document_host_begin_forced_read`] only where the host waits for it, and
/// otherwise lends the read and asks the host nothing.
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
pub unsafe extern "C" fn document_host_read_scope_view(host: *const DocumentHost) -> FfiReadScopeView {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
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
pub unsafe extern "C" fn document_host_frame_flies(host: *const DocumentHost) -> bool {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.frame_flies()
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
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { Box::from_raw(host) };
    // The document's teardown is the host's own read: a frame in flight lands first, and what it brought back for the
    // host goes unpaid, as the host made nothing of it yet.
    host.take_frame_in(ReadRight::Forced(ForcedRead::Host(HostRead::begun())));
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
