/*
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ creates FormData objects, reads their entry lists and creates entries.

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Vm};

use super::form_data::{Entry, FormData, FormDataEntryValue, StringOrBlob, create_an_entry};
use super::xml_http_request::{NullOrFailureOrBytes, SyncSendRef, XMLHttpRequest};
use crate::bindings::conversions::UsvString;
use crate::bindings::cpp_cell::{LentCppCell, cpp_class};
use crate::bindings::interface::{InterfaceCell, Ref, allocate, cell_from_abi, cell_into_abi};
use crate::fetch::infrastructure::fetch_controller::CppFetchController;
use crate::fetch::infrastructure::http::response_view::CppResponseView;
use crate::file_api::exports::any_blob_from_abi;

#[repr(C)]
pub(crate) struct EntryValueAbi {
    file: *mut c_void,
    string: usize,
}

#[repr(C)]
pub(crate) struct EntryAbi {
    name: usize,
    value: EntryValueAbi,
}

impl EntryValueAbi {
    fn from_value(value: &FormDataEntryValue) -> Self {
        match value {
            FormDataEntryValue::File(file) => Self {
                file: cell_into_abi(*file),
                string: 0,
            },
            FormDataEntryValue::USVString(string) => Self {
                file: core::ptr::null_mut(),
                string: string.as_utf16_string().clone().into_raw(),
            },
        }
    }
}

impl EntryAbi {
    fn from_entry((name, value): &Entry) -> Self {
        Self {
            name: name.as_utf16_string().clone().into_raw(),
            value: EntryValueAbi::from_value(value),
        }
    }

    /// The entry that C++ hands over.
    ///
    /// # Safety
    ///
    /// The entry must be one that C++ hands over: its strings are raw strings that it owns a reference to, and its file
    /// is the live cell of a File.
    unsafe fn into_entry(self) -> Entry {
        // SAFETY: The caller hands over the name.
        let name = unsafe { Utf16String::from_raw_owned(self.name) };
        let value = if self.value.file.is_null() {
            // SAFETY: The caller hands over the string.
            let string = unsafe { Utf16String::from_raw_owned(self.value.string) };
            FormDataEntryValue::USVString(UsvString::from_utf16_string(&string))
        } else {
            // SAFETY: The caller passes a live cell of a File.
            FormDataEntryValue::File(unsafe { cell_from_abi(self.value.file) })
        };
        (UsvString::from_utf16_string(&name), value)
    }
}

/// A new FormData whose entry list is the entries that C++ hands over.
///
/// # Safety
///
/// `vm` must be the live VM, and `entries` entries that C++ hands over, whose files it keeps alive for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_form_data_create(vm: &Vm, entries: ak::Span<'_, EntryAbi>) -> *mut c_void {
    cell_into_abi(allocate(vm, |defer_gc| {
        let entry_list = entries
            .as_slice()
            .iter()
            .map(|entry| {
                // SAFETY: The caller hands over the entries, each of which is read once.
                unsafe { core::ptr::read(entry).into_entry() }
            })
            .collect();
        FormData::new(defer_gc, entry_list)
    }))
}

/// The size of the entry list of the FormData.
#[unsafe(no_mangle)]
pub extern "C" fn web_form_data_entry_count(cell: &InterfaceCell) -> usize {
    let form_data: Ref<FormData> = cell.object();
    form_data.entry_list().len()
}

/// The entry at the index in the entry list of the FormData. C++ owns its strings.
#[unsafe(no_mangle)]
pub extern "C" fn web_form_data_entry(cell: &InterfaceCell, index: usize) -> EntryAbi {
    let form_data: Ref<FormData> = cell.object();
    EntryAbi::from_entry(&form_data.entry_list()[index])
}

/// The value of the first entry named `name`, a raw AK::Utf16String that C++ hands over. C++ owns the string of the
/// value.
#[unsafe(no_mangle)]
pub extern "C" fn web_form_data_value_named(
    cell: &InterfaceCell,
    name: Utf16String,
    out_value: &mut EntryValueAbi,
) -> bool {
    let (form_data, name): (Ref<FormData>, _) = (cell.object(), name);
    let Some(value) = form_data.value_named(&UsvString::from_utf16_string(&name)) else {
        return false;
    };
    *out_value = EntryValueAbi::from_value(&value);
    true
}

/// Creates an entry with the name and the Blob or File `blob`, or the string if `blob` is null. C++ hands over the raw
/// AK::Utf16Strings and owns those of the entry,
/// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#create-an-entry
#[unsafe(no_mangle)]
pub extern "C" fn web_html_create_an_entry(
    vm: &Vm,
    name: Utf16String,
    blob: Option<&InterfaceCell>,
    string: Utf16String,
) -> EntryAbi {
    let value = match blob {
        Some(blob) => StringOrBlob::Blob(any_blob_from_abi(blob)),
        None => StringOrBlob::String(string),
    };
    EntryAbi::from_entry(&create_an_entry(vm, &name, value, None))
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_xhr_create(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    cell_into_abi(XMLHttpRequest::create(vm, realm))
}

/// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_xhr_process_request_body_chunk_length(
    vm: &Vm,
    xhr: &InterfaceCell,
    request_body_length: u64,
    bytes_length: u64,
) {
    let xhr = xhr.object::<XMLHttpRequest>();
    xhr.process_request_body_chunk_length(vm, request_body_length, bytes_length);
}

/// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_xhr_process_request_end_of_body(vm: &Vm, xhr: &InterfaceCell, request_body_length: u64) {
    let xhr = xhr.object::<XMLHttpRequest>();
    xhr.process_request_end_of_body(vm, request_body_length);
}

/// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_xhr_process_response(
    vm: &Vm,
    xhr: &InterfaceCell,
    response: &LentCppCell<cpp_class::ResponseView>,
) {
    let (xhr, response) = (xhr.object::<XMLHttpRequest>(), CppResponseView::from_lent(response));
    XMLHttpRequest::process_response(xhr, vm, response);
}

/// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_xhr_process_body_chunk(vm: &Vm, xhr: &InterfaceCell, length: u64, bytes: ak::Span<'_, u8>) {
    let xhr = xhr.object::<XMLHttpRequest>();
    xhr.process_body_chunk(vm, length, bytes.as_slice());
}

/// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_xhr_process_end_of_body(vm: &Vm, xhr: &InterfaceCell) {
    let xhr = xhr.object::<XMLHttpRequest>();
    xhr.process_end_of_body(vm);
}

/// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_xhr_process_body_error(vm: &Vm, xhr: &InterfaceCell) {
    let xhr = xhr.object::<XMLHttpRequest>();
    xhr.process_body_error(vm);
}

/// https://xhr.spec.whatwg.org/#dom-xmlhttprequest-send
///
/// # Safety
///
/// `sync_send` must be the live flags of the send() of the XMLHttpRequest.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_xhr_process_response_consume_body(
    _vm: &Vm,
    xhr: &InterfaceCell,
    sync_send: *mut c_void,
    response: &LentCppCell<cpp_class::ResponseView>,
    kind: u8,
    bytes: ak::Span<'_, u8>,
) {
    let (xhr, response) = (xhr.object::<XMLHttpRequest>(), CppResponseView::from_lent(response));
    // SAFETY: The caller passes the live flags of its send().
    let sync_send = unsafe { sync_send_from_abi(sync_send) };
    let null_or_failure_or_bytes = match kind {
        0 => NullOrFailureOrBytes::Null,
        1 => NullOrFailureOrBytes::Failure,
        _ => NullOrFailureOrBytes::Bytes(bytes.as_slice()),
    };
    xhr.process_response_consume_body(&sync_send, response, null_or_failure_or_bytes);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_xhr_fetch_stopped(
    vm: &Vm,
    xhr: &InterfaceCell,
    fetch_controller: &LentCppCell<cpp_class::FetchController>,
) {
    let (xhr, fetch_controller) = (
        xhr.object::<XMLHttpRequest>(),
        CppFetchController::from_lent(fetch_controller),
    );
    XMLHttpRequest::fetch_stopped(xhr, vm, fetch_controller);
}

/// # Safety
///
/// `sync_send` must be the live flags of a send().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_xhr_sync_send_is_over(sync_send: *mut c_void) -> bool {
    // SAFETY: The caller passes the live flags of a send().
    unsafe { sync_send_from_abi(sync_send) }.is_over()
}

/// # Safety
///
/// `sync_send` must be the live flags of a send().
unsafe fn sync_send_from_abi(sync_send: *mut c_void) -> SyncSendRef {
    // SAFETY: The caller passes the live flags of a send().
    unsafe { Gc::from_non_null(NonNull::new(sync_send.cast()).expect("C++ passes the flags of a send()")) }
}
