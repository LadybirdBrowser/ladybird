/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The FFI entry points of the parent module that mint a main thread token. The token's marker
//! can be made only here, and this module is private, so the parent's own code can neither mint
//! a token nor call an entry that does.

/// Mints the main thread token for this module's FFI entry points; only this module can make one.
pub(crate) struct MainThreadFfiEntry {
    _private: (),
}

const MAIN_THREAD_FFI_ENTRY: MainThreadFfiEntry = MainThreadFfiEntry { _private: () };

/// Detaches what is left of the boxes of the `count` nodes `style_nodes` names as they leave the document (see
/// [`super::detach_remaining_rows_for_removal`]), and pays what that owes the host. An identity of 0 names no node.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `style_nodes` must point at `count` identities.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_detach_remaining_rows_for_removal(
    host: *const crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    style_nodes: *const u32,
    count: usize,
) {
    if count == 0 {
        return;
    }
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let (host, style_nodes) = unsafe { (&*host, std::slice::from_raw_parts(style_nodes, count)) };
    let written = crate::layout::layout_changes::write(
        read,
        host,
        crate::layout::layout_changes::LayoutWrite::DetachRemainingRowsForRemoval(style_nodes),
    );
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, host) };
    written.host_work.pay(&main_thread);
}
