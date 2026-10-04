/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host keeps of a document's render state.

use super::questions::Question;
use super::{
    ArenaChange, ChangeQueue, CommittedRows, CreatedState, DocumentId, ForcedRead, LockstepProof, NoFrameInFlight,
    QueuedChanges, RenderMessage, RenderWait, ScriptForcedRead, ask, send, wait_for_render_state,
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
use std::cell::{Cell, OnceCell, RefCell, RefMut};
use std::rc::Rc;
use std::sync::atomic::Ordering;

/// The host's side of one document's render state: the document's name, the host tables the host answers layout
/// through, and what the document keeps of its display list recordings, which are made on the host's thread from the
/// frame the render state publishes. The host's document owns it, and it lives on the host's thread.
///
/// The host's entries call into each other, so it is only ever shared: everything a call may change sits in a cell, and
/// the rows are handed out by reference count, so a call that publishes them again frees none an outer call still reads.
pub struct DocumentHost {
    document: DocumentId,
    host_tables: HostTables,
    recording: RefCell<RecordingSlot>,
    /// The rows the render state published last, which the host reads between messages.
    rows: RefCell<Option<Rc<RowSnapshot>>>,
    /// The reactions of the style transaction the host took last, which it reads until it ends the transaction.
    style_transaction: RefCell<Option<StyleJobAnswer>>,
    /// The absolute rects the host's reads of the rows computed, kept for as long as the geometry they were computed
    /// from stays.
    absolute_rects: RefCell<AbsoluteRectMemo>,
    /// What the host keeps of the document's render state once it is made: the arena, whose rows version tells the host
    /// whether the rows it has still read as the arena's after a change the host applied where it is, the style engine
    /// the host's style entries reach, and whether any element has had random base values.
    state: OnceCell<CreatedState>,
    /// The compositor animations the document's effects published in the current update pass, which the host hands
    /// the render state as the pass ends.
    compositor_animations: RefCell<Vec<VisualAnimation>>,
    /// The read of the render state the host began and has not ended, if any.
    forced_read: RefCell<BegunRead>,
    /// The writes the host queued that the render state has not applied yet, in the order the host made them.
    changes: ChangeQueue,
    /// The style transaction the host let fly beside it, until the host has drained its reactions.
    style_flight: RefCell<Option<StyleFlight>>,
    /// How the host's document drains the style transaction that flew, until a drain begins. The document drains it
    /// only where it has not begun to.
    flown_style_drain: Cell<Option<FfiFlownStyleDrain>>,
    /// Whether the paint and hit testing properties the document prepared last were prepared from the render state as it
    /// is: nothing was written to it since, queued, in place or by a message. Preparing them again would find nothing
    /// to do.
    paint_preparation_is_current: Cell<bool>,
}

/// A style transaction of the host's document that runs beside the host, has landed, or whose reactions the host
/// drains. The style writes the host queued beside it reach the render state only behind its drain.
enum StyleFlight {
    /// The transaction flies with the buffer of the writes it took, which the host's queue gets back once it lands.
    Flying(InFlight<(StyleJobAnswer, Vec<ArenaChange>)>),
    Landed(StyleJobAnswer),
    /// The host drains the transaction's reactions, holding the writes it queued beside the transaction until the drain
    /// ends.
    Draining(Vec<ArenaChange>),
}

/// A read of a document's render state the host began: how many of the read's scopes are open, and the read itself
/// until its first job takes it.
#[derive(Default)]
struct BegunRead {
    scopes: u32,
    read: Option<ForcedRead>,
}

impl DocumentHost {
    fn new(document: DocumentId) -> Self {
        Self {
            document,
            host_tables: HostTables::default(),
            recording: RefCell::default(),
            rows: RefCell::default(),
            style_transaction: RefCell::default(),
            absolute_rects: RefCell::default(),
            state: OnceCell::new(),
            compositor_animations: RefCell::default(),
            forced_read: RefCell::default(),
            changes: ChangeQueue::default(),
            style_flight: RefCell::default(),
            flown_style_drain: Cell::new(None),
            paint_preparation_is_current: Cell::new(false),
        }
    }

    /// A host with no render state, for a unit test.
    #[cfg(test)]
    pub(crate) fn for_test() -> Self {
        Self::new(DocumentId::default())
    }

    pub(crate) fn document(&self) -> DocumentId {
        self.document
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

    /// Lends the writes the host queued to `apply`, for the render side to apply ahead of the host's next message, once
    /// the frame in flight has landed. A style write queued beside a style transaction that flew stays queued, behind the
    /// drain of the transaction's reactions, which it is the next transaction's input to.
    pub(super) fn drain_queued_changes(&self, apply: impl FnOnce(QueuedChanges<'_>)) {
        let landed = self.take_frame_in();
        self.changes.drain(landed, self.document, self.has_flown_style(), apply);
    }

    /// Takes the writes the host queued, for a style transaction that flies with them to the render side.
    pub(super) fn take_queued_changes_for_flight(&self) -> Vec<ArenaChange> {
        debug_assert!(
            !self.has_flown_style(),
            "one style transaction of a document flies at a time"
        );
        self.changes.take()
    }

    /// Lets `flight`, a style transaction of the document's, fly beside the host until the host drains its reactions with
    /// `drain`.
    pub(super) fn let_style_fly(
        &self,
        flight: InFlight<(StyleJobAnswer, Vec<ArenaChange>)>,
        drain: FfiFlownStyleDrain,
    ) {
        self.flown_style_drain.set(Some(drain));
        let previous = self.style_flight.borrow_mut().replace(StyleFlight::Flying(flight));
        debug_assert!(
            previous.is_none(),
            "one style transaction of a document flies at a time"
        );
    }

    /// Takes the frame in flight in, waiting for it to land: the host reaches the document's render state, where it
    /// is or with a message, only behind it. A frame in flight unsettles the host's queue, so a question the host
    /// answers in place comes here only where the queue is not settled.
    #[inline]
    fn take_frame_in(&self) -> NoFrameInFlight {
        if matches!(*self.style_flight.borrow(), Some(StyleFlight::Flying(_))) {
            self.land_flying_style();
        }
        NoFrameInFlight(())
    }

    /// Waits for the style transaction that flies to land, and takes it in.
    #[cold]
    fn land_flying_style(&self) {
        let mut flight = self.style_flight.borrow_mut();
        let Some(StyleFlight::Flying(flying)) = flight.take() else {
            unreachable!("a style transaction flies");
        };
        let (answer, buffer) = flying.join(LockstepProof::for_reason(&HOST_REACHES_FRAME_IN_FLIGHT));
        self.changes.give_back(buffer);
        *flight = Some(StyleFlight::Landed(answer));
    }

    /// Whether the document's style transaction still flies, where it has not landed: one that has is taken in. The
    /// event loop asks between two tasks, so this never waits.
    pub(crate) fn style_flies(&self, boundary: &TaskBoundary) -> bool {
        let mut flight = self.style_flight.borrow_mut();
        let Some(StyleFlight::Flying(flying)) = flight.take_if(|flight| matches!(flight, StyleFlight::Flying(_)))
        else {
            return false;
        };
        let (landed, flies) = match flying.try_take(boundary) {
            Ok((answer, buffer)) => {
                self.changes.give_back(buffer);
                (StyleFlight::Landed(answer), false)
            }
            Err(flying) => (StyleFlight::Flying(flying), true),
        };
        *flight = Some(landed);
        flies
    }

    /// Whether the host let a style transaction fly whose reactions it has not begun to drain.
    #[inline]
    pub(crate) fn has_flown_style(&self) -> bool {
        matches!(
            *self.style_flight.borrow(),
            Some(StyleFlight::Flying(_) | StyleFlight::Landed(_))
        )
    }

    /// Has the document drain the style transaction that flew, where it has not begun to, for a write to the document's
    /// style sheets the host makes in place: the write lands behind the sheet writes the host queued beside the
    /// transaction, which wait for its drain, in the order the host made them.
    pub(crate) fn drain_flown_style(&self) {
        if let Some(drain) = self.flown_style_drain.get() {
            // SAFETY: The document drains on the host's thread, which this is, and outlives its host.
            unsafe { (drain.drain)(drain.document) };
        }
    }

    /// Takes what the style transaction the host let fly answered, waiting for it to land, for the host to drain its
    /// reactions. The writes the host queued beside the transaction wait for the drain to end.
    pub(crate) fn begin_style_drain(&self) -> StyleJobAnswer {
        self.flown_style_drain.set(None);
        self.take_frame_in();
        let mut flight = self.style_flight.borrow_mut();
        let Some(StyleFlight::Landed(answer)) = flight.take() else {
            panic!("the host drains a style transaction that flew and has landed");
        };
        *flight = Some(StyleFlight::Draining(self.changes.hold_style_writes()));
        answer
    }

    /// Ends the drain of the style transaction that flew: the writes the host queued beside it are queued again, behind
    /// what the drain wrote.
    pub(crate) fn end_style_drain(&self) {
        let Some(StyleFlight::Draining(beside)) = self.style_flight.borrow_mut().take() else {
            panic!("the host ends the drain it began");
        };
        self.changes.requeue(beside);
    }

    /// Applies the writes the host queued to the document's render state, where the host is, for a read the host
    /// answers itself, once the frame in flight has landed. Only a read that spends a wait may: the render side waits
    /// for the host meanwhile. A settled queue has neither, which is all the read tests.
    #[inline]
    fn apply_queued_changes(&self, _wait: &impl RenderWait) {
        if !self.changes.is_settled() {
            self.settle_queued_changes();
        }
    }

    #[inline(never)]
    fn settle_queued_changes(&self) {
        let state = self.created_state();
        self.drain_queued_changes(|changes| {
            // SAFETY: The state keeps its arena and engine where they are until it is destroyed, and nothing on the
            // render side reaches them while the host waits.
            unsafe { changes.apply((*state.arena.as_ptr()).arena_mut(), state.engine) };
        });
    }

    /// Begins a read of the document's render state that the host waits for, for a script API call where `by_script`
    /// and for the host's own read otherwise. A scope begun inside the document's open read belongs to that read.
    pub(super) fn begin_forced_read(&self, by_script: bool) {
        let mut begun = self.forced_read.borrow_mut();
        begun.scopes += 1;
        if begun.scopes > 1 {
            return;
        }
        begun.read = Some(if by_script {
            ForcedRead::Script(ScriptForcedRead::at_script_entry(&FORCED_READ_SCOPE))
        } else {
            ForcedRead::Host(LockstepProof::for_reason(&HOST_READS_LAYOUT))
        });
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
        self.state
            .get()
            .is_some_and(|state| state.element_random_base_values_exist.load(Ordering::Relaxed))
    }

    /// The arena of the document's render state, for a unit test that writes it directly.
    #[cfg(test)]
    pub(crate) fn arena_for_test(&self) -> *mut crate::layout::LayoutNodeArena {
        self.created_state().arena.as_ptr().cast()
    }

    /// Answers `question` from the document's render state as of every write the host queued, where the host is,
    /// spending `wait`.
    pub(super) fn answer_in_place<Q: Question>(&self, wait: &impl RenderWait, question: Q) -> Q::Answer {
        if !Q::LEAVES_PAINT_PREPARATION_CURRENT {
            self.note_render_state_write();
        }
        self.apply_queued_changes(wait);
        let state = self.created_state();
        // SAFETY: The state keeps its arena and engine where they are until it is destroyed, and nothing on the render
        // side reaches them while the host runs.
        let arena = unsafe { (*state.arena.as_ptr()).arena_mut() };
        // A question that reads overflow first measures what it finds unmeasured, which can change what the preparation
        // answers: such a measurement writes the render state, whatever the question.
        let measurement_awaited_preparation = arena.scrollable_overflow.measurement_awaits_preparation();
        // SAFETY: As above.
        let answer = unsafe { question.answer(arena, state.engine) };
        if !measurement_awaited_preparation && arena.scrollable_overflow.measurement_awaits_preparation() {
            self.note_render_state_write();
        }
        answer
    }

    fn created_state(&self) -> &CreatedState {
        self.state
            .get()
            .expect("the host of a live document has a render state")
    }

    /// The rows the render state published last, unless the host wrote them since, or none were published yet.
    #[cfg(test)]
    pub(crate) fn rows(&self) -> Option<Rc<RowSnapshot>> {
        self.apply_queued_changes(&ScriptForcedRead::for_test());
        self.rows
            .borrow()
            .clone()
            .filter(|rows| self.still_reads_as_arena(rows))
    }

    /// Whether `rows` read as the arena does now, once the writes the host queued are applied. The arena's rows version
    /// moves with every write to the rows, queued or direct, and with nothing else.
    fn still_reads_as_arena(&self, rows: &RowSnapshot) -> bool {
        // SAFETY: The arena lives as long as the document's render state, which outlives its host's reads.
        self.state
            .get()
            .is_none_or(|state| rows.reads_as(unsafe { state.arena.as_ref() }.arena().rows_version()))
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
        self.apply_queued_changes(&wait);
        if let Some(rows) = self
            .rows
            .borrow()
            .as_ref()
            .filter(|rows| self.identities_still_read_as_arena(rows))
        {
            return RowIdentities::of(Rc::clone(rows));
        }
        RowIdentities::of(self.rows_as_of_writes(wait, false))
    }

    /// Whether what each row of `rows` is, and the row each node is bound to, read as the arena's do now (see
    /// [`Self::still_reads_as_arena`]).
    fn identities_still_read_as_arena(&self, rows: &RowSnapshot) -> bool {
        // SAFETY: The arena lives as long as the document's render state, which outlives its host's reads.
        self.state
            .get()
            .is_none_or(|state| rows.reads_identity_as(unsafe { state.arena.as_ref() }.arena().rows_identity_version()))
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
        self.apply_queued_changes(&wait);
        let usable =
            |rows: &RowSnapshot| self.still_reads_as_arena(rows) && (!measure_overflow || rows.overflow_is_measured());
        if let Some(rows) = self.rows.borrow().as_ref().filter(|rows| usable(rows)) {
            return Rc::clone(rows);
        }
        let rows = Rc::new(ask(wait, self, CommittedRows { measure_overflow }));
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
    let document = DocumentId::mint();
    let host = Box::new(DocumentHost::new(document));
    let created = wait_for_render_state(LockstepProof::for_reason(&NEW_DOCUMENT), &host, |reply| {
        RenderMessage::Create {
            document,
            device_class,
            reply,
        }
    });
    assert!(host.state.set(created).is_ok(), "a document has one render state");
    Box::into_raw(host)
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

    /// The style engine of the host's document, which the test reaches between the host's calls.
    pub(crate) fn engine(&self) -> crate::css::style::StyleEngineHandle {
        // SAFETY: The host lives until the test host is dropped.
        unsafe { &*self.0 }.created_state().engine
    }
}

#[cfg(test)]
impl Drop for TestHost {
    fn drop(&mut self) {
        // SAFETY: The test host made the host, and destroys it once.
        unsafe { document_host_destroy(self.0) };
    }
}

/// The reason a new document's host waits for its render state: it keeps where the state's arena and style engine are.
pub(crate) struct NewDocument {
    _private: (),
}

const NEW_DOCUMENT: NewDocument = NewDocument { _private: () };

/// Marks the scope of a read of a document's render state that a script API call begins, which mints the call's
/// forced read.
pub(crate) struct ForcedReadScope {
    _private: (),
}

const FORCED_READ_SCOPE: ForcedReadScope = ForcedReadScope { _private: () };

/// The reason the host waits for its document's frame in flight: it reaches the document's render state, which the
/// frame's job holds until it lands.
pub(crate) struct HostReachesFrameInFlight {
    _private: (),
}

const HOST_REACHES_FRAME_IN_FLIGHT: HostReachesFrameInFlight = HostReachesFrameInFlight { _private: () };

/// The reason the host waits for its document's render state in a read of its own, for no script API call: an event's
/// dispatch, a child document's style update, an inspection, a rendering update.
pub(crate) struct HostReadsLayout {
    _private: (),
}

const HOST_READS_LAYOUT: HostReadsLayout = HostReadsLayout { _private: () };

/// Begins a read of the render state of `host`'s document that the host waits for, for a script API call where
/// `by_script` and for the host's own read otherwise. The read's first style or layout job spends it.
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
    // The render state goes first: freeing its rows may still answer to the host.
    send(
        &host,
        RenderMessage::Destroy {
            document: host.document,
        },
    );
    assert_eq!(
        host.host_tables.shells.borrow().len(),
        0,
        "document host destroyed with layout nodes"
    );
}
