/*
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ creates FormData objects, reads their entry lists and creates entries.

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{JSVM, vm_from_abi};

use super::form_data::{Entry, FormData, FormDataEntryValue, StringOrBlob, create_an_entry};
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Ref, allocate, cell_from_abi, cell_into_abi};
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
pub unsafe extern "C" fn web_form_data_create(vm: *mut JSVM, entries: ak::Span<'_, EntryAbi>) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
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
///
/// # Safety
///
/// `cell` must be the live cell of a FormData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_form_data_entry_count(cell: *mut c_void) -> usize {
    // SAFETY: The caller passes a live cell of a FormData.
    let form_data: Ref<FormData> = unsafe { cell_from_abi(cell) };
    form_data.entry_list().len()
}

/// The entry at the index in the entry list of the FormData. C++ owns its strings.
///
/// # Safety
///
/// `cell` must be the live cell of a FormData, and `index` less than the size of its entry list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_form_data_entry(cell: *mut c_void, index: usize) -> EntryAbi {
    // SAFETY: The caller passes a live cell of a FormData.
    let form_data: Ref<FormData> = unsafe { cell_from_abi(cell) };
    EntryAbi::from_entry(&form_data.entry_list()[index])
}

/// The value of the first entry named `name`, a raw AK::Utf16String that C++ hands over. C++ owns the string of the
/// value.
///
/// # Safety
///
/// `cell` must be the live cell of a FormData, and `name` a raw string that C++ owns a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_form_data_value_named(
    cell: *mut c_void,
    name: usize,
    out_value: &mut EntryValueAbi,
) -> bool {
    // SAFETY: The caller passes a live cell of a FormData and hands over the name.
    let (form_data, name): (Ref<FormData>, _) = unsafe { (cell_from_abi(cell), Utf16String::from_raw_owned(name)) };
    let Some(value) = form_data.value_named(&UsvString::from_utf16_string(&name)) else {
        return false;
    };
    *out_value = EntryValueAbi::from_value(&value);
    true
}

/// Creates an entry with the name and the Blob or File `blob`, or the string if `blob` is null. C++ hands over the raw
/// AK::Utf16Strings and owns those of the entry,
/// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#create-an-entry
///
/// # Safety
///
/// `vm` must be the live VM, `name` and `string` raw strings that C++ owns a reference to, and `blob` null or the live
/// cell of a Blob or a File.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_html_create_an_entry(
    vm: *mut JSVM,
    name: usize,
    blob: *mut c_void,
    string: usize,
) -> EntryAbi {
    // SAFETY: The caller passes the live VM and hands over the strings.
    let (vm, name, string) = unsafe {
        (
            vm_from_abi(vm),
            Utf16String::from_raw_owned(name),
            Utf16String::from_raw_owned(string),
        )
    };
    let value = if blob.is_null() {
        StringOrBlob::String(string)
    } else {
        // SAFETY: The caller passes a live cell of a Blob or a File.
        StringOrBlob::Blob(unsafe { any_blob_from_abi(blob) })
    };
    EntryAbi::from_entry(&create_an_entry(vm, &name, value, None))
}
