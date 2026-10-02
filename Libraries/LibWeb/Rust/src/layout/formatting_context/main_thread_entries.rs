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

/// # Safety
///
/// `arena` must be a live handle with a registered layout host, used on the document thread,
/// and `viewport` must be its live viewport box.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_run_root_layout(
    arena: *mut c_void,
    viewport: NodeSlotId,
    viewport_inline_size_raw: i32,
    viewport_block_size_raw: i32,
    document_in_quirks_mode: bool,
    should_collect_devtools_layout_data: bool,
) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: Guaranteed by the entry point's contract.
    unsafe {
        run_root_layout(
            &main_thread,
            arena,
            viewport,
            viewport_inline_size_raw,
            viewport_block_size_raw,
            document_in_quirks_mode,
            should_collect_devtools_layout_data,
        );
    }
}

/// # Safety
///
/// `arena` must be a live handle with a registered layout host, used on the document thread, and
/// `root` must be a live partial relayout boundary.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_compute_subtree_layout(
    arena: *mut c_void,
    root: NodeSlotId,
    viewport_inline_size_raw: i32,
    viewport_block_size_raw: i32,
    document_in_quirks_mode: bool,
) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    // SAFETY: Guaranteed by the entry point's contract.
    unsafe {
        compute_subtree_layout(
            &main_thread,
            arena,
            root,
            viewport_inline_size_raw,
            viewport_block_size_raw,
            document_in_quirks_mode,
        );
    }
}
