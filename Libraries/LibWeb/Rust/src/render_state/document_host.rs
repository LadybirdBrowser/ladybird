/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host keeps of a document's render state.

use super::questions::Question;
use super::{
    ArenaChange, CommittedRows, CreatedState, DocumentId, LockstepProof, RenderMessage, RenderWait, ask, send,
    wait_for_render_state,
};
use crate::css::style::bridge::FfiDeviceClass;
use crate::css::style::style_job::StyleJobAnswer;
use crate::layout::HostTables;
use crate::layout::row_reads::{RowIdentities, RowSnapshot};
use crate::painting::paint_read::PaintSource;
use crate::painting::record::recorder_state::AbsoluteRectMemo;
use crate::painting::recording_slot::RecordingSlot;
use crate::painting::visual_animation::VisualAnimation;
use std::cell::{OnceCell, RefCell, RefMut};
use std::ffi::c_void;
use std::ptr::NonNull;
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
    /// whether the rows it has still read as the arena's after a write the host made through an entry that reaches the
    /// arena directly, the style engine such entries reach, and whether any element has had random base values.
    state: OnceCell<CreatedState>,
    /// The compositor animations the document's effects published in the current update pass, which the host hands
    /// the render state as the pass ends.
    compositor_animations: RefCell<Vec<VisualAnimation>>,
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

    /// Queues `change` for the document's render state, which applies it before anything that reads what it changes.
    pub(crate) fn queue_change(&self, change: ArenaChange) {
        send(RenderMessage::Change {
            document: self.document,
            change,
        });
    }

    /// Whether some element may have random base values to keep, which only then is worth asking the render state.
    pub(crate) fn element_random_base_values_may_exist(&self) -> bool {
        self.state
            .get()
            .is_some_and(|state| state.element_random_base_values_exist.load(Ordering::Relaxed))
    }

    /// The arena of the document's render state, for the host's entries that still reach it directly.
    ///
    /// This is the one door from the host into a render state that does not go through a message; every use of it is
    /// an entry that has not been converted yet. The arena stays at the address answered until the document is
    /// destroyed.
    pub(crate) fn arena_for_unconverted_entry(&self) -> *mut c_void {
        self.created_state().arena.as_ptr().cast()
    }

    /// The style engine of the document's render state, for the host's entries that still reach it directly, as
    /// [`Self::arena_for_unconverted_entry`] answers its arena.
    pub(crate) fn style_engine_for_unconverted_entry(&self) -> crate::css::style::StyleEngineHandle {
        self.created_state().engine
    }

    /// Answers `question` from the document's render state, where the host is.
    pub(super) fn answer_in_place<Q: Question>(&self, question: Q) -> Q::Answer {
        let state = self.created_state();
        // SAFETY: The state keeps its arena and engine where they are until it is destroyed, and nothing on the render
        // side reaches them while the host runs.
        unsafe { question.answer((*state.arena.as_ptr()).arena_mut(), state.engine) }
    }

    fn created_state(&self) -> &CreatedState {
        self.state
            .get()
            .expect("the host of a live document has a render state")
    }

    /// The rows the render state published last, unless the host wrote them since, or none were published yet.
    /// Reading them waits for nothing.
    #[cfg_attr(not(test), expect(dead_code, reason = "only tests read the rows without a wait yet"))]
    pub(crate) fn rows(&self) -> Option<Rc<RowSnapshot>> {
        self.rows
            .borrow()
            .clone()
            .filter(|rows| self.still_reads_as_arena(rows))
    }

    /// Whether `rows` read as the arena does now. The render state applies each change as the host queues it, so the
    /// arena's rows version moves with every write to the rows, queued or direct, and with nothing else.
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
    // The render state and the host's document hold the one pointer the box was let go of as.
    let host = NonNull::from(Box::leak(Box::new(DocumentHost::new(document))));
    // SAFETY: The host was made above, and only its document reaches it after.
    let host_ref = unsafe { host.as_ref() };
    let created = wait_for_render_state(LockstepProof::for_reason(&NEW_DOCUMENT), host_ref, |reply| {
        RenderMessage::Create {
            document,
            host,
            device_class,
            reply,
        }
    });
    assert!(host_ref.state.set(created).is_ok(), "a document has one render state");
    host.as_ptr()
}

/// The reason a new document's host waits for its render state: it keeps where the state's arena and style engine are.
pub(crate) struct NewDocument {
    _private: (),
}

const NEW_DOCUMENT: NewDocument = NewDocument { _private: () };

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
    send(RenderMessage::Destroy {
        document: host.document,
    });
    assert_eq!(
        host.host_tables.shells.borrow().len(),
        0,
        "document host destroyed with layout nodes"
    );
}

/// The arena of the render state of `host`'s document, which the host's entries that have not been converted to
/// messages still take.
///
/// # Safety
///
/// `host` must come from [`document_host_create`] and not be destroyed yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_arena_for_unconverted_entry(host: *const DocumentHost) -> *mut c_void {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.arena_for_unconverted_entry()
}

/// The style engine of the render state of `host`'s document, which the host's entries that have not been converted
/// to messages still take.
///
/// # Safety
///
/// `host` must come from [`document_host_create`] and not be destroyed yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_style_engine_for_unconverted_entry(
    host: *const DocumentHost,
) -> crate::css::style::StyleEngineHandle {
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.style_engine_for_unconverted_entry()
}
