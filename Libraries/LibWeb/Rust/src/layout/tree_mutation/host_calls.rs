/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The host calls that destroy what a freed row held. The imports are private to this module, so
//! the only way to reach them is a wrapper that takes the main thread token.

use crate::stage::MainThread;
use std::ffi::c_void;

unsafe extern "C" {
    fn ladybird_layout_node_shell_destroy(shell: *mut c_void);
}

pub(crate) fn destroy_shell(_: &MainThread, shell: *mut c_void) {
    if shell.is_null() {
        return;
    }
    // SAFETY: The arena has already freed the shell's slot, and destroying a shell never
    // re-enters the arena.
    unsafe { ladybird_layout_node_shell_destroy(shell) };
}
