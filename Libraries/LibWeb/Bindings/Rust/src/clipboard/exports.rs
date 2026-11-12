/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points through which LibWeb's C++ code creates the Clipboard objects, declared in
//! Libraries/LibWeb/Clipboard/Clipboard.cpp and ClipboardEvent.cpp.

use core::ffi::c_void;

use crate::bindings::interface::{cell_into_abi, realm_arguments};
use crate::clipboard::clipboard::Clipboard;
use crate::clipboard::clipboard_event::ClipboardEvent;
use crate::dom::event::EventInit;
use ak::{Utf16FlyString, Utf16String};

/// # Safety
///
/// The pointers must be the live VM and a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_clipboard_create_clipboard(vm: *mut c_void, realm: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    cell_into_abi(Clipboard::create(vm, realm))
}

/// The Event of a new ClipboardEvent in the realm.
///
/// # Safety
///
/// The pointers must be the live VM and a live realm, `event_name` a live Utf16FlyString, and `clipboard_data` null or
/// a live DataTransfer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_clipboard_create_clipboard_event(
    vm: *mut c_void,
    realm: *mut c_void,
    event_name: usize,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    clipboard_data: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    // SAFETY: The caller lends a live fly string for the call.
    let event_name: Utf16String = unsafe { Utf16FlyString::from_raw_borrowed(event_name) }.into();
    let event_init = EventInit {
        bubbles,
        cancelable,
        composed,
    };
    // SAFETY: The caller passes null or a live DataTransfer.
    unsafe { ClipboardEvent::create(vm, realm, &event_name, &event_init, clipboard_data) }
        .event()
        .as_ptr()
}
