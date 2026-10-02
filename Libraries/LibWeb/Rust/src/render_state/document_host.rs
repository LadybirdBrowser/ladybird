/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host keeps of a document's render state.

use super::{ArenaChange, DocumentId, RenderMessage, RenderWait, send, wait_for_render_state};
use crate::css::style::bridge::FfiDeviceClass;
use crate::layout::HostTables;
use crate::layout::row_reads::RowSnapshot;
use crate::painting::recording_slot::RecordingSlot;
use std::cell::{Cell, RefCell, RefMut};
use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::Rc;

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
    #[cfg_attr(
        not(test),
        expect(dead_code, reason = "the host reads the published rows from the next commit on")
    )]
    rows: RefCell<Option<Rc<RowSnapshot>>>,
    /// Whether the host queued a change that alters the published rows since they were published.
    rows_may_be_stale: Cell<bool>,
}

impl DocumentHost {
    fn new(document: DocumentId) -> Self {
        Self {
            document,
            host_tables: HostTables::default(),
            recording: RefCell::default(),
            rows: RefCell::default(),
            rows_may_be_stale: Cell::new(false),
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

    /// The rows the render state published last, unless the host queued a change since that alters them, or none were
    /// published yet. Reading them waits for nothing.
    #[cfg_attr(
        not(test),
        expect(dead_code, reason = "the host reads the published rows from the next commit on")
    )]
    pub(crate) fn rows(&self) -> Option<Rc<RowSnapshot>> {
        if self.rows_may_be_stale.get() {
            return None;
        }
        self.rows.borrow().clone()
    }

    /// The rows as of every change the host queued, which the render state publishes again first where the ones the
    /// host has may be stale, spending `wait`.
    #[cfg_attr(
        not(test),
        expect(dead_code, reason = "the host reads the published rows from the next commit on")
    )]
    pub(crate) fn fresh_rows(&self, wait: impl RenderWait) -> Rc<RowSnapshot> {
        if !self.rows_may_be_stale.get()
            && let Some(rows) = self.rows.borrow().as_ref()
        {
            return Rc::clone(rows);
        }
        let document = self.document;
        let rows = Rc::new(wait_for_render_state(wait, self, |reply| {
            RenderMessage::CommittedRows { document, reply }
        }));
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
