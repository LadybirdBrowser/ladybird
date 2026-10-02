/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The host calls that destroy what a freed row held. The imports are private to this module, so
//! the only way to reach them is a wrapper that takes the main thread token. Destroying any of
//! them never reads a layout node, so their order against each other is free.

use crate::stage::MainThread;
use std::ffi::c_void;

unsafe extern "C" {
    fn ladybird_layout_node_shell_destroy(shell: *mut c_void);
    fn ladybird_layout_owned_image_provider_destroy(provider: *mut c_void);
    fn ladybird_layout_image_observers_destroy(observers: *mut c_void);
    fn ladybird_layout_owned_image_provider_notify_detach(provider: *mut c_void);
}

pub(crate) fn destroy_shell(_: &MainThread, shell: *mut c_void) {
    if shell.is_null() {
        return;
    }
    // SAFETY: The arena has already freed the shell's slot, and destroying a shell never
    // re-enters the arena.
    unsafe { ladybird_layout_node_shell_destroy(shell) };
}

pub(crate) fn destroy_owned_image_provider(_: &MainThread, provider: *mut c_void) {
    if provider.is_null() {
        return;
    }
    // SAFETY: The arena has already freed the provider's row, and deleting a provider never
    // re-enters the arena.
    unsafe { ladybird_layout_owned_image_provider_destroy(provider) };
}

pub(crate) fn destroy_image_observers(_: &MainThread, observers: *mut c_void) {
    if observers.is_null() {
        return;
    }
    // SAFETY: The arena has already freed the set's row, and deleting a set never re-enters the
    // arena.
    unsafe { ladybird_layout_image_observers_destroy(observers) };
}

/// Tells the provider a row owns that the row is leaving the layout tree. An element's provider
/// outlives its box and keeps nothing about it, so only a provider a row owns hears about this.
pub(crate) fn notify_owned_image_provider_of_detach(_: &MainThread, provider: *mut c_void) {
    if provider.is_null() {
        return;
    }
    // SAFETY: The provider belongs to a live row, and the notification does not re-enter the arena.
    unsafe { ladybird_layout_owned_image_provider_notify_detach(provider) };
}
