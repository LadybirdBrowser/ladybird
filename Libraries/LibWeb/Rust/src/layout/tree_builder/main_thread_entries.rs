/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The FFI entry points of the parent module that mint a main thread token. The token's marker
//! can be made only here, and this module is private, so the parent's own code can neither mint
//! a token nor call an entry that does.

use super::*;

/// Mints the main thread token for this module's FFI entry points; only this module can make one.
pub(crate) struct MainThreadFfiEntry {
    _private: (),
}

const MAIN_THREAD_FFI_ENTRY: MainThreadFfiEntry = MainThreadFfiEntry { _private: () };

/// Detaches what is left of the boxes of the node `style_node` names as the node leaves the document (see
/// [`super::detach_remaining_rows_for_removal`]), and pays what that owes the host.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_detach_remaining_rows_for_removal(
    host: *const crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    style_node: u32,
) {
    let Some(node) = StyleNodeID::from_raw(style_node) else {
        return;
    };
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    let written = crate::layout::layout_changes::write(
        read,
        host,
        crate::layout::layout_changes::LayoutWrite::DetachRemainingRowsForRemoval(node),
    );
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, host) };
    written.host_work.pay(&main_thread);
}
