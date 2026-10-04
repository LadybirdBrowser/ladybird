/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host keeps of a document's render state, which the render owner holds: its name, the frame that may fly
//! beside the host, and what the host reads between the jobs it hands the owner.

use super::owner::{self, DocumentId, SharedWithHost, StateSeed};
use super::questions::Question;
use super::wait::{BegunRead, HostRead, NodeRead, ReadRight, force_read_flown_style};
use super::{
    ArenaChange, ChangeQueue, CommittedRows, ForcedRead, Landing, NoFrameInFlight, RenderState, RenderWait,
    ScriptForcedRead, on_render_side, post_to_render_side,
};
use crate::css::style::bridge::FfiDeviceClass;
use crate::css::style::flight_style_rows::FlightStyleRow;
use crate::css::style::rule_writes::{PublishedRules, RuleWrite};
use crate::css::style::style_job::StyleJobAnswer;
use crate::css::style::tree::StyleNodeID;
use crate::layout::row_reads::{RowIdentities, RowSnapshot};
use crate::layout::tree_update_marks::{LayoutTreeUpdateMarkWrite, LayoutTreeUpdateMarks};
use crate::layout::{FlownRound, HostTables, RowsVersion, SealedRound};
use crate::painting::paint_read::PaintSource;
use crate::painting::record::recorder_state::AbsoluteRectMemo;
use crate::painting::recording_slot::RecordingSlot;
use crate::painting::visual_animation::VisualAnimation;
use crate::render_state::TaskBoundary;
use crate::stage_thread::InFlight;
use std::cell::{Cell, RefCell, RefMut};
use std::rc::Rc;

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
    /// The frame that flies beside the host, until the host takes it in.
    flight: RefCell<Option<InFlight<Landing>>>,
    /// Whether the host waits for the frame: it flies, or the layout round that flew with it is not paid yet. The
    /// host's scopes of a read and its document's layout read it where it is (see [`document_host_read_scope_view`]).
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
}

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
    fn new(device_class: FfiDeviceClass) -> Self {
        let shared = SharedWithHost::new();
        Self {
            document: DocumentId::mint(),
            seed: Cell::new(Some(StateSeed {
                device_class,
                shared: shared.clone(),
            })),
            shared,
            flight: RefCell::default(),
            waits_for_frame: Cell::new(false),
            host_tables: HostTables::default(),
            recording: RefCell::default(),
            rows: RefCell::default(),
            arena_version: Cell::new(None),
            style_transaction: RefCell::default(),
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
        }
    }

    /// A host with a render state, for a unit test.
    #[cfg(test)]
    pub(crate) fn for_test() -> Self {
        Self::new(FfiDeviceClass::ForegroundDesktop)
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
        self.note_render_state_write();
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
        let ((answer, version), marks) = self.drain_queued_changes(read, |changes| {
            let (document, seed, marks) = (self.document, self.seed.take(), self.lend_marks());
            let in_job = self.in_job.replace(true);
            let answer = on_render_side(move || {
                owner::with_state(document, seed, |state| {
                    state.with_marks(marks, |state| {
                        state.apply(changes);
                        let answer = job(state);
                        (answer, state.rows_version())
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
        answer
    }

    /// Lets the frame fly beside the host, in the job `flight` submits to the owner with the document's name, until the
    /// host drains the reactions of the style transaction it flies with.
    pub(super) fn let_frame_fly(
        &self,
        flight: impl FnOnce(DocumentId, Option<StateSeed>, Option<LayoutTreeUpdateMarks>) -> InFlight<Landing>,
    ) {
        self.arena_version.set(None);
        let flight = flight(self.document, self.seed.take(), self.lend_marks());
        let previous = self.flight.borrow_mut().replace(flight);
        assert!(previous.is_none(), "one frame of a document flies at a time");
        self.note_frame_wait();
    }

    /// Takes the frame in flight in, where one flies, waiting for it to land, spending `read`: only a read waits for a
    /// frame.
    #[inline]
    fn take_frame_in(&self, read: ReadRight) -> NoFrameInFlight {
        if self.frame_flies() {
            self.land_flying_frame(read);
        }
        NoFrameInFlight(())
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
        let flight = self.flight.borrow_mut().take();
        if let Some(flight) = flight {
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
        }: Landing,
    ) {
        self.changes.give_back(changes);
        self.take_marks_back(marks);
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

    fn frame_flies(&self) -> bool {
        self.flight.borrow().is_some()
    }

    fn note_frame_wait(&self) {
        self.waits_for_frame
            .set(self.frame_flies() || self.flown_round.borrow().is_some());
    }

    /// Whether the document's frame still flies, where it has not landed: one that has is taken in. The event loop asks
    /// between two tasks, so this never waits.
    pub(crate) fn frame_still_flies(&self, boundary: &TaskBoundary) -> bool {
        let flight = self.flight.borrow_mut().take();
        let Some(flight) = flight else {
            return false;
        };
        match flight.try_take(boundary) {
            Ok(landing) => {
                self.landed(landing);
                false
            }
            Err(flight) => {
                *self.flight.borrow_mut() = Some(flight);
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

    /// Lets go of what the style transaction the host took last answered.
    pub(crate) fn end_style_transaction(&self) {
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

/// Creates the host of a new document, whose render state has a style engine for a device of class `device_class`.
/// The render owner makes the state with the host's first job, which waits for nothing more than the job does, and
/// which a document nothing renders may never send.
#[unsafe(no_mangle)]
pub extern "C" fn document_host_create(device_class: u8) -> *mut DocumentHost {
    let device_class = match device_class {
        0 => FfiDeviceClass::ForegroundDesktop,
        _ => panic!("unknown device class {device_class}"),
    };
    Box::into_raw(Box::new(DocumentHost::new(device_class)))
}

/// A document host with a render state, for a unit test, which destroys both when it is dropped.
#[cfg(test)]
pub(crate) struct TestHost(*mut DocumentHost);

#[cfg(test)]
impl TestHost {
    pub(crate) fn new() -> Self {
        Self(document_host_create(0))
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
