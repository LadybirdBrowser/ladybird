/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The host hooks: the embedder's table of callbacks and its data pointer.

use core::ffi::c_void;

/// The embedder's table of host hooks, which the VM calls in place of its own host-defined behavior.
#[repr(C)]
pub struct JSVmHostHooks {
    _reserved: [u8; 0],
}

/// What an embedder installs on the VM: its hooks, and the data pointer they receive.
#[derive(Clone, Copy)]
pub struct Embedder {
    pub hooks: &'static JSVmHostHooks,
    pub data: *mut c_void,
}
