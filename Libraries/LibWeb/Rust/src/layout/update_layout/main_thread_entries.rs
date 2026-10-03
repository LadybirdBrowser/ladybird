/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The FFI entry points of the parent module that mint a main thread token. The token's marker
//! can be made only here, and this module is private, so the parent's own code can neither mint
//! a token nor call an entry that does.

use super::*;
use crate::render_state::BegunRead;

/// Mints the main thread token for this module's FFI entry points; only this module can make one.
pub(crate) struct MainThreadFfiEntry {
    _private: (),
}

const MAIN_THREAD_FFI_ENTRY: MainThreadFfiEntry = MainThreadFfiEntry { _private: () };

/// Runs the document's layout update to a fixed point: style, then the layout tree build, then
/// either a partial relayout of the registered boundaries or a full pass, until nothing is
/// pending. The document-side steps run through the registered layout update host.
///
/// # Safety
///
/// `host` must be a live document host with registered layout and layout update hosts, on its document's thread
/// between `document_host_begin_update_layout` and its end, and `inputs` must remain valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_update_layout(
    host: *const DocumentHost,
    read: &BegunRead,
    inputs: *const FfiLayoutUpdateInputs,
) {
    assert!(!host.is_null(), "document host is null");
    assert!(!inputs.is_null());
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, host) };
    abort_on_panic(|| {
        // SAFETY: Guaranteed by the entry point's contract.
        unsafe { update_layout(&main_thread, host, read, &*inputs) };
        // The image resources the update's tree builds owe are attached once its layout is done.
        host.host_tables()
            .layout_update_host
            .get()
            .expect("the document has no layout update host")
            .attach_owed_image_resources(&main_thread, host, read);
    });
}
