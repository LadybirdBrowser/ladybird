/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host keeps of a document's render state.

use super::{Answer, ArenaChange, DocumentId, Query, RenderMessage, RenderWait, ask, send};
use crate::css::style::bridge::FfiDeviceClass;
use crate::layout::row_reads::RowSnapshot;
use crate::layout::{HostTables, LayoutNodeArena};
use crate::painting::paint_read::PaintSource;
use crate::painting::record::recorder_state::AbsoluteRectMemo;
use crate::painting::recording_slot::RecordingSlot;
use std::cell::{Cell, OnceCell, RefCell, RefMut};
use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

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
    /// Whether the host queued a change that alters the published rows since they were published.
    rows_may_be_stale: Cell<bool>,
    /// The absolute rects the host's reads of the rows computed, kept for as long as the geometry they were computed
    /// from stays.
    absolute_rects: RefCell<AbsoluteRectMemo>,
    /// The arena of the document's render state, whose rows version tells the host whether the rows it has still read
    /// as the arena's after a write the host made through an entry that reaches the arena directly.
    arena: Cell<Option<NonNull<LayoutNodeArena>>>,
    /// Whether any element has had random base values, which the render state raises and never lowers.
    element_random_base_values_exist: OnceCell<Arc<AtomicBool>>,
}

impl DocumentHost {
    fn new(document: DocumentId) -> Self {
        Self {
            document,
            host_tables: HostTables::default(),
            recording: RefCell::default(),
            rows: RefCell::default(),
            rows_may_be_stale: Cell::new(false),
            absolute_rects: RefCell::default(),
            arena: Cell::new(None),
            element_random_base_values_exist: OnceCell::new(),
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
        if change.alters_published_rows() {
            self.rows_may_be_stale.set(true);
        }
        send(RenderMessage::Change {
            document: self.document,
            change,
        });
    }

    /// Lets the host tell whether the rows it has read as the arena's do. The render state makes the arena, which stays
    /// where it is until the state is destroyed.
    pub(crate) fn watch_rows_of(&self, arena: NonNull<LayoutNodeArena>) {
        self.arena.set(Some(arena));
    }

    /// Watches the flag the render state raises once any element has random base values.
    pub(crate) fn watch_element_random_base_values(&self, exist: Arc<AtomicBool>) {
        assert!(
            self.element_random_base_values_exist.set(exist).is_ok(),
            "a document has one render state"
        );
    }

    /// Whether some element may have random base values to keep, which only then is worth asking the render state.
    pub(crate) fn element_random_base_values_may_exist(&self) -> bool {
        self.element_random_base_values_exist
            .get()
            .is_some_and(|exist| exist.load(Ordering::Relaxed))
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

    /// Whether the host neither queued a change that alters `rows` since they were published nor wrote the arena
    /// directly.
    fn still_reads_as_arena(&self, rows: &RowSnapshot) -> bool {
        if self.rows_may_be_stale.get() {
            return false;
        }
        // SAFETY: The arena lives as long as the document's render state, which outlives its host's reads.
        self.arena
            .get()
            .is_none_or(|arena| rows.reads_as(unsafe { arena.as_ref() }.rows_version()))
    }

    /// The rows as of every change the host queued, which the render state publishes again first where the ones the
    /// host has may be stale, spending `wait`.
    pub(crate) fn fresh_rows(&self, wait: impl RenderWait) -> Rc<RowSnapshot> {
        self.rows_as_of_writes(wait, false)
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
        let Answer::Rows(rows) = ask(wait, self, Query::CommittedRows { measure_overflow }) else {
            unreachable!("the rows are answered with rows");
        };
        let rows = Rc::new(rows);
        *self.rows.borrow_mut() = Some(Rc::clone(&rows));
        self.rows_may_be_stale.set(false);
        rows
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
    send(RenderMessage::Create {
        document,
        host,
        device_class,
    });
    host.as_ptr()
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
    super::arena_for_unconverted_entry(unsafe { (*host).document })
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
    super::style_engine_for_unconverted_entry(unsafe { (*host).document })
}
