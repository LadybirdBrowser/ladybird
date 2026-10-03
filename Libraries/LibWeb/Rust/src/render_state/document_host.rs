/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host keeps of a document's render state, and the frame that owns the state.

use super::questions::Question;
use super::wait::{BegunRead, HostRead, NodeRead, ReadRight, force_read_flown_style};
use super::{
    ArenaChange, ChangeQueue, CommittedRows, ForcedRead, Landing, NoFrameInFlight, RenderState, RenderWait,
    ScriptForcedRead, on_render_side,
};
use crate::css::style::bridge::FfiDeviceClass;
use crate::css::style::style_job::{FfiFlownStyleDrain, StyleJobAnswer};
use crate::layout::HostTables;
use crate::layout::row_reads::{RowIdentities, RowSnapshot};
use crate::painting::paint_read::PaintSource;
use crate::painting::record::recorder_state::AbsoluteRectMemo;
use crate::painting::recording_slot::RecordingSlot;
use crate::painting::visual_animation::VisualAnimation;
use crate::render_state::TaskBoundary;
use crate::stage_thread::InFlight;
use std::cell::{Cell, RefCell, RefMut, UnsafeCell};
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

/// The host's side of one document's render state: the frame that owns the state, the host tables the host answers
/// layout through, and what the document keeps of its display list recordings, which are made on the host's thread
/// from the frame the render state publishes. The host's document owns it, and it lives on the host's thread.
///
/// The host's entries call into each other, so it is only ever shared: everything a call may change sits in a cell, and
/// the rows are handed out by reference count, so a call that publishes them again frees none an outer call still reads.
pub struct DocumentHost {
    /// The document's render state, here or flying.
    frame: RefCell<Frame>,
    /// Whether the host waits for the frame: it flies, or the layout round that flew with it is not paid yet. The
    /// host's scopes of a read and its document's layout read it where it is (see [`document_host_read_scope_view`]).
    waits_for_frame: Cell<bool>,
    host_tables: HostTables,
    recording: RefCell<RecordingSlot>,
    /// The rows the render state published last, which the host reads between messages.
    rows: RefCell<Option<Rc<RowSnapshot>>>,
    /// The reactions of the style transaction the host took last, which it reads until it ends the transaction.
    style_transaction: RefCell<Option<StyleJobAnswer>>,
    /// The absolute rects the host's reads of the rows computed, kept for as long as the geometry they were computed
    /// from stays.
    absolute_rects: RefCell<AbsoluteRectMemo>,
    /// The flag the render state raises once any element has random base values, and never lowers.
    element_random_base_values_exist: Arc<AtomicBool>,
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
    /// How the host's document drains the style transaction that flew, until a drain begins. The document drains it
    /// only where it has not begun to.
    flown_style_drain: Cell<Option<FfiFlownStyleDrain>>,
    /// Whether the paint and hit testing properties the document prepared last were prepared from the render state as it
    /// is: nothing was written to it since, queued, in place or by a message. Preparing them again would find nothing
    /// to do.
    paint_preparation_is_current: Cell<bool>,
}

/// Where a document's render state is: here, where the host lends it to the messages it waits for, or flying, moved
/// into the job of a frame that runs beside the host. Only a forced read, which may wait, or a task boundary, which
/// waits for nothing, takes a flying frame in again.
enum Frame {
    /// The state is here, lent to the host's questions and messages (see [`DocumentHost::with_state`]).
    Here(UnsafeCell<RenderState>),
    Flying(InFlight<Landing>),
}

/// A style transaction that flew with the frame and has landed, or whose reactions the host drains. The style writes the
/// host queued beside it reach the render state only behind its drain.
enum FlownStyle {
    Landed(StyleJobAnswer),
    /// The host drains the transaction's reactions, holding the writes it queued beside the transaction until the drain
    /// ends.
    Draining(Vec<ArenaChange>),
}

/// A read of a document's render state the host began: how many of the read's scopes are open, and the read itself
/// until its first job takes it.
#[derive(Default)]
struct ReadScopes {
    scopes: u32,
    read: Option<ForcedRead>,
}

impl DocumentHost {
    fn new(state: RenderState) -> Self {
        Self {
            element_random_base_values_exist: state.engine_ref().element_random_base_values_exist(),
            frame: RefCell::new(Frame::Here(UnsafeCell::new(state))),
            waits_for_frame: Cell::new(false),
            host_tables: HostTables::default(),
            recording: RefCell::default(),
            rows: RefCell::default(),
            style_transaction: RefCell::default(),
            absolute_rects: RefCell::default(),
            compositor_animations: RefCell::default(),
            forced_read: RefCell::default(),
            begun_read: BegunRead::of_host(),
            changes: ChangeQueue::default(),
            flown_style: RefCell::default(),
            flown_style_drain: Cell::new(None),
            paint_preparation_is_current: Cell::new(false),
        }
    }

    /// A host with a render state, for a unit test.
    #[cfg(test)]
    pub(crate) fn for_test() -> Self {
        Self::new(RenderState::new(FfiDeviceClass::ForegroundDesktop))
    }

    /// A read of the host's document the test begins, and never ends.
    #[cfg(test)]
    pub(crate) fn read_for_test(&self) -> &BegunRead {
        self.begin_forced_read(false)
    }

    pub(crate) fn host_tables(&self) -> &HostTables {
        &self.host_tables
    }

    /// Writes `change` to the document's render state, before anything that reads what it changes: the render side
    /// applies it ahead of the host's next message, and the host ahead of the next question it answers where it is. A
    /// write never reaches the render state as the host makes it.
    pub(crate) fn queue_change(&self, change: ArenaChange) {
        self.note_render_state_write();
        self.changes.push(change);
    }

    /// Notes that the document's render state is written, which leaves the paint and hit testing properties prepared
    /// from it stale.
    pub(super) fn note_render_state_write(&self) {
        self.paint_preparation_is_current.set(false);
    }

    /// Takes the frame in flight in with `read`, and lends the writes the host queued to `apply`, for the render state
    /// to apply ahead of the host's next message or question. A style write queued beside a style transaction that flew
    /// stays queued, behind the drain of the transaction's reactions, which it is the next transaction's input to.
    pub(super) fn drain_queued_changes<R>(
        &self,
        read: ReadRight,
        apply: impl FnOnce(std::vec::Drain<'_, ArenaChange>) -> R,
    ) -> R {
        let landed = self.take_frame_in(read);
        self.changes.drain(landed, self.has_flown_style(), apply)
    }

    /// Takes the writes the host queued, for a style transaction that flies with them to the render side.
    pub(super) fn take_queued_changes_for_flight(&self) -> Vec<ArenaChange> {
        debug_assert!(
            !self.has_flown_style(),
            "one style transaction of a document flies at a time"
        );
        self.changes.take()
    }

    /// Runs `reach` on the document's render state, which the host has here: only [`Self::drain_queued_changes`] takes a
    /// flying frame in, so every reach of the state comes after it. The frame neither flies nor lands while the reach
    /// runs. A host callback the reach makes may reach the state again, as a layout round's read of an element's style
    /// does, which reaches the same state.
    pub(super) fn with_state<R>(&self, reach: impl FnOnce(&mut RenderState) -> R) -> R {
        let frame = self.frame.borrow();
        let Frame::Here(state) = &*frame else {
            panic!("the host reaches its document's render state only once the frame is taken in");
        };
        // SAFETY: The state stays here while the frame is borrowed. A reach inside this one comes from a host callback
        // this reach makes, which reaches the state only between this reach's own uses of it.
        reach(unsafe { &mut *state.get() })
    }

    /// Moves the document's render state into the job `flight` submits with it, and lets the frame fly beside the host
    /// until the host drains the reactions of the style transaction it flies with, with `drain`.
    pub(super) fn let_frame_fly(
        &self,
        drain: FfiFlownStyleDrain,
        flight: impl FnOnce(RenderState) -> InFlight<Landing>,
    ) {
        self.flown_style_drain.set(Some(drain));
        self.turn_frame(|frame| match frame {
            Frame::Here(state) => Frame::Flying(flight(state.into_inner())),
            Frame::Flying(_) => panic!("one frame of a document flies at a time"),
        });
    }

    /// Takes the frame in flight in, where one flies, waiting for it to land, spending `read`: only a read waits for a
    /// frame. A frame in flight unsettles the host's queue, so a question the host answers in place comes here only
    /// where the queue is not settled.
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

    /// Lands the frame in flight, waiting for it, spending `read`: the render state is here again, and the style
    /// transaction that flew with it waits to be drained.
    pub(super) fn land(&self, read: ForcedRead) {
        self.turn_frame(|frame| match frame {
            Frame::Flying(flight) => self.landed(flight.join(read)),
            here @ Frame::Here(_) => here,
        });
    }

    fn landed(&self, Landing { state, style, changes }: Landing) -> Frame {
        self.changes.give_back(changes);
        let previous = self.flown_style.borrow_mut().replace(FlownStyle::Landed(style));
        debug_assert!(
            previous.is_none(),
            "one style transaction of a document flies at a time"
        );
        Frame::Here(UnsafeCell::new(state))
    }

    /// Proof that no frame flies, where none does. A read of the layout in place then takes no frame in, and needs no
    /// read the host began.
    pub(crate) fn layout_waits_for_no_frame(&self) -> Option<NoFrameInFlight> {
        (!self.frame_flies()).then_some(NoFrameInFlight(()))
    }

    fn frame_flies(&self) -> bool {
        matches!(*self.frame.borrow(), Frame::Flying(_))
    }

    /// Turns the frame into what `turn` makes of it.
    fn turn_frame(&self, turn: impl FnOnce(Frame) -> Frame) {
        replace_frame(&mut self.frame.borrow_mut(), turn);
        self.note_frame_wait();
    }

    fn note_frame_wait(&self) {
        self.waits_for_frame.set(self.frame_flies());
    }

    /// Whether the document's frame still flies, where it has not landed: one that has is taken in. The event loop asks
    /// between two tasks, so this never waits.
    pub(crate) fn frame_still_flies(&self, boundary: &TaskBoundary) -> bool {
        self.turn_frame(|frame| match frame {
            Frame::Flying(flight) => match flight.try_take(boundary) {
                Ok(landing) => self.landed(landing),
                Err(flight) => Frame::Flying(flight),
            },
            here @ Frame::Here(_) => here,
        });
        self.frame_flies()
    }

    /// Whether the host let a style transaction fly whose reactions it has not begun to drain.
    #[inline]
    pub(crate) fn has_flown_style(&self) -> bool {
        self.frame_flies() || matches!(*self.flown_style.borrow(), Some(FlownStyle::Landed(_)))
    }

    /// Has the document drain the style transaction that flew in `read`, where it has not begun to, for a write to the
    /// document's style sheets the host makes in place: the write lands behind the sheet writes the host queued beside
    /// the transaction, which wait for its drain, in the order the host made them.
    pub(crate) fn drain_flown_style(&self, read: &BegunRead) {
        if let Some(drain) = self.flown_style_drain.get() {
            // SAFETY: The document drains on the host's thread, which this is, and outlives its host.
            unsafe { (drain.drain)(drain.document, read) };
        }
    }

    /// Takes what the style transaction that flew answered, for the host to drain its reactions, once `read` took the
    /// frame in. The writes the host queued beside the transaction wait for the drain to end.
    pub(crate) fn begin_style_drain(&self, read: ReadRight) -> StyleJobAnswer {
        self.flown_style_drain.set(None);
        self.take_frame_in(read);
        let mut flown = self.flown_style.borrow_mut();
        let Some(FlownStyle::Landed(answer)) = flown.take() else {
            panic!("the host drains a style transaction that flew and has landed");
        };
        *flown = Some(FlownStyle::Draining(self.changes.hold_style_writes()));
        answer
    }

    /// Ends the drain of the style transaction that flew: the writes the host queued beside it are queued again, behind
    /// what the drain wrote.
    pub(crate) fn end_style_drain(&self) {
        let Some(FlownStyle::Draining(beside)) = self.flown_style.borrow_mut().take() else {
            panic!("the host ends the drain it began");
        };
        self.changes.requeue(beside);
    }

    /// Applies the writes the host queued to the document's render state, where the host is, for a read the host
    /// answers itself, taking the frame in flight in first with `wait`. A settled queue has neither, which is all the
    /// read tests.
    #[inline]
    fn apply_queued_changes(&self, wait: impl RenderWait) {
        assert!(
            wait.reaches(self),
            "a begun read reaches only the render state of its own document"
        );
        if !self.changes.is_settled() {
            self.settle_queued_changes(wait.into_read_right());
        }
    }

    #[inline(never)]
    fn settle_queued_changes(&self, read: ReadRight) {
        self.drain_queued_changes(read, |changes| self.with_state(|state| state.apply(changes)));
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

    /// Whether some element may have random base values to keep, which only then is worth asking the render state.
    pub(crate) fn element_random_base_values_may_exist(&self) -> bool {
        self.element_random_base_values_exist.load(Ordering::Relaxed)
    }

    /// The arena of the document's render state, for a unit test that writes it directly.
    #[cfg(test)]
    pub(crate) fn arena_for_test(&self) -> *mut crate::layout::LayoutNodeArena {
        self.with_state(|state| std::ptr::from_mut(state.arena.arena_mut()))
    }

    /// Answers `question` from the document's render state as of every write the host queued, where the host is,
    /// spending `wait`.
    pub(super) fn answer_in_place<Q: Question>(&self, wait: impl RenderWait, question: Q) -> Q::Answer {
        if !Q::LEAVES_PAINT_PREPARATION_CURRENT {
            self.note_render_state_write();
        }
        self.apply_queued_changes(wait);
        // A question that reads overflow first measures what it finds unmeasured, which can change what the preparation
        // answers: such a measurement writes the render state, whatever the question.
        let (answer, measured_for_preparation) = self.with_state(|state| {
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
        self.apply_queued_changes(ScriptForcedRead::for_test());
        self.rows
            .borrow()
            .clone()
            .filter(|rows| self.still_reads_as_arena(rows))
    }

    /// Whether `rows` read as the arena does now, once the writes the host queued are applied. The arena's rows version
    /// moves with every write to the rows, queued or direct, and with nothing else.
    fn still_reads_as_arena(&self, rows: &RowSnapshot) -> bool {
        self.with_state(|state| rows.reads_as(state.arena.arena().rows_version()))
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
        self.apply_queued_changes(wait);
        if let Some(rows) = self
            .rows
            .borrow()
            .as_ref()
            .filter(|rows| self.identities_still_read_as_arena(rows))
        {
            return RowIdentities::of(Rc::clone(rows));
        }
        RowIdentities::of(self.rows_here(false))
    }

    /// Whether what each row of `rows` is, and the row each node is bound to, read as the arena's do now (see
    /// [`Self::still_reads_as_arena`]).
    fn identities_still_read_as_arena(&self, rows: &RowSnapshot) -> bool {
        self.with_state(|state| rows.reads_identity_as(state.arena.arena().rows_identity_version()))
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

    fn rows_as_of_writes(&self, wait: impl RenderWait, measure_overflow: bool) -> Rc<RowSnapshot> {
        self.apply_queued_changes(wait);
        self.rows_here(measure_overflow)
    }

    /// The rows as of the render state here, which has applied every write the host queued.
    fn rows_here(&self, measure_overflow: bool) -> Rc<RowSnapshot> {
        let usable =
            |rows: &RowSnapshot| self.still_reads_as_arena(rows) && (!measure_overflow || rows.overflow_is_measured());
        if let Some(rows) = self.rows.borrow().as_ref().filter(|rows| usable(rows)) {
            return Rc::clone(rows);
        }
        let rows = Rc::new(self.with_state(|state| state.answer(CommittedRows { measure_overflow })));
        *self.rows.borrow_mut() = Some(Rc::clone(&rows));
        rows
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

/// Creates the host of a new document and its render state, with a style engine for a device of class
/// `device_class`.
#[unsafe(no_mangle)]
pub extern "C" fn document_host_create(device_class: u8) -> *mut DocumentHost {
    let device_class = match device_class {
        0 => FfiDeviceClass::ForegroundDesktop,
        _ => panic!("unknown device class {device_class}"),
    };
    let state = on_render_side(move || RenderState::new(device_class));
    Box::into_raw(Box::new(DocumentHost::new(state)))
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

    /// A read of the host's document the test begins, and never ends.
    pub(crate) fn read(&self) -> &BegunRead {
        // SAFETY: The host lives until the test host is dropped.
        unsafe { &*self.0 }.read_for_test()
    }

    /// The style engine of the host's document, which the test reaches between the host's calls.
    pub(crate) fn engine(&self) -> crate::css::style::StyleEngineHandle {
        // SAFETY: The host lives until the test host is dropped.
        unsafe { &*self.0 }.with_state(|state| state.engine)
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

/// Destroys `host` and the render state of its document.
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
    let DocumentHost { frame, host_tables, .. } = *host;
    let Frame::Here(state) = frame.into_inner() else {
        unreachable!("the frame was taken in above");
    };
    let state = state.into_inner();
    on_render_side(move || state.retire());
    assert_eq!(
        host_tables.shells.borrow().len(),
        0,
        "document host destroyed with layout nodes"
    );
}

/// Replaces `frame` with what `turn` makes of it. A frame that cannot be turned leaves the document without a render
/// state, which nothing can go on over.
fn replace_frame(frame: &mut Frame, turn: impl FnOnce(Frame) -> Frame) {
    struct Abort;
    impl Drop for Abort {
        fn drop(&mut self) {
            super::render_state_died();
        }
    }
    let abort = Abort;
    // SAFETY: The frame is read out once, and written back before anything reads it again: a panic in between aborts.
    unsafe {
        let turned = turn(std::ptr::read(frame));
        std::ptr::write(frame, turned);
    }
    std::mem::forget(abort);
}
