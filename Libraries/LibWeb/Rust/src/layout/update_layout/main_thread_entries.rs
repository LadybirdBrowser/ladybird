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

/// Runs the document's layout update to a fixed point: style, then the layout tree build, then
/// either a partial relayout of the registered boundaries or a full pass, until nothing is
/// pending. The document-side steps run through the registered layout update host.
///
/// # Safety
///
/// `arena` must be a live handle with registered layout and layout update hosts, used on the
/// document thread between `layout_arena_begin_update_layout` and its end, and `inputs` must
/// remain valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_update_layout(arena: *mut c_void, inputs: *const FfiLayoutUpdateInputs) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    assert!(!inputs.is_null());
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, arena) };
    abort_on_panic(|| {
        // SAFETY: Guaranteed by the entry point's contract.
        unsafe { update_layout(&main_thread, arena, &*inputs) };
        // The image resources the update's tree builds owe are attached once its layout is done.
        main_thread
            .host_tables()
            .and_then(|host_tables| host_tables.layout_update_host.get())
            .expect("layout node arena has no layout update host")
            .attach_owed_image_resources(&main_thread, arena);
    });
}
