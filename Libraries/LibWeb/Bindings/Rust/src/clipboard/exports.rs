/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points through which LibWeb's C++ code creates the Clipboard objects, declared in
//! Libraries/LibWeb/Clipboard/Clipboard.cpp and ClipboardEvent.cpp.

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Vm};

use crate::bindings::interface::cell_into_abi;
use crate::bindings::lent_string::LentUtf16FlyString;
use crate::clipboard::clipboard::Clipboard;
use crate::clipboard::clipboard_event::ClipboardEvent;
use crate::dom::event::EventInit;
use crate::html::data_transfer::DataTransfer;
use crate::html::drag_data_store::LentDragDataStore;

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_clipboard_create_clipboard(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    cell_into_abi(Clipboard::create(vm, realm))
}

/// The Event of a new ClipboardEvent in the realm.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_clipboard_create_clipboard_event(
    vm: &Vm,
    realm: Gc<Realm>,
    event_name: LentUtf16FlyString<'_>,
    bubbles: bool,
    cancelable: bool,
    composed: bool,
    store: LentDragDataStore<'_>,
) -> *mut c_void {
    let event_name: Utf16String = event_name.to_utf16_fly_string().into();
    let event_init = EventInit {
        bubbles,
        cancelable,
        composed,
    };
    let clipboard_data = DataTransfer::create(vm, store.to_rc());
    ClipboardEvent::create(vm, realm, &event_name, &event_init, Some(clipboard_data))
        .event()
        .as_ptr()
}
