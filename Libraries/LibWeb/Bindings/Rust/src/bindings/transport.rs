/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What LibWeb's C++ IPC transports lend to Rust, which the Rust bindings hold as opaque pointers. They are reached
//! through the C ABI of RustBindingsServices.cpp.

use core::ffi::c_void;
use core::ptr::NonNull;

unsafe extern "C" {
    fn web_bindings_transport_handle_destroy(handle: *mut c_void);
}

/// A handle to one end of an IPC transport that this owns: an IPC::TransportHandle, which holds the rights of that end
/// until a data holder carries them to another agent.
pub(crate) struct TransportHandle(NonNull<c_void>);

impl TransportHandle {
    /// # Safety
    ///
    /// `handle` must be a handle that LibWeb hands over.
    pub(crate) unsafe fn from_raw(handle: *mut c_void) -> Self {
        Self(NonNull::new(handle).expect("LibWeb hands over a handle"))
    }

    pub(crate) fn as_raw(&self) -> *mut c_void {
        self.0.as_ptr()
    }
}

impl Drop for TransportHandle {
    fn drop(&mut self) {
        // SAFETY: This owns the handle.
        unsafe { web_bindings_transport_handle_destroy(self.0.as_ptr()) };
    }
}
