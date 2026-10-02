/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the host keeps of a document's render state.

use super::{DocumentId, RenderMessage, send};
use crate::layout::HostTables;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::Rc;

/// The host's side of one document's render state: the document's name, and the host tables the host answers layout
/// through. The host's document owns it, and it lives on the host's thread.
pub struct DocumentHost {
    document: DocumentId,
    host_tables: HostTables,
}

/// Creates the host of a new document and its render state.
#[unsafe(no_mangle)]
pub extern "C" fn document_host_create() -> *mut DocumentHost {
    let host = Box::new(DocumentHost {
        document: DocumentId::mint(),
        host_tables: HostTables::default(),
    });
    send(RenderMessage::Create {
        document: host.document,
        host_tables: NonNull::from(&host.host_tables),
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
    // The render state goes first: freeing its rows may still answer to the host tables.
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
