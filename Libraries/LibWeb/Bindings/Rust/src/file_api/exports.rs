/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ creates the Blob, File and FileList objects that it hands to script, and reads
//! those that it receives, which Libraries/LibWeb/FileAPI/Blob.h, File.h and FileList.h declare. A Blob cell can be that
//! of a File.

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::Vm;

use super::blob::{AnyBlob, Blob, BlobPropertyBag, EndingType, ForeignBytesAbi};
use super::file::{File, FilePropertyBag};
use super::file_list::FileList;
use crate::bindings::interface::{Inherits, InterfaceCell, allocate, cell_into_abi};

/// The Blob or File whose cell C++ lends, as a Blob.
fn blob_from_abi(cell: &InterfaceCell) -> &Blob {
    if let Some(blob) = cell.implementation_if::<Blob>() {
        return blob;
    }
    cell.implementation::<File>().parent()
}

/// The Blob or File whose cell C++ lends.
pub(crate) fn any_blob_from_abi(cell: &InterfaceCell) -> AnyBlob {
    match cell.object_if::<Blob>() {
        Some(blob) => AnyBlob::Blob(blob),
        None => AnyBlob::File(cell.object()),
    }
}

/// A new Blob that refers to the bytes that C++ hands over, with the type, a raw AK::Utf16String that C++ hands over.
///
/// # Safety
///
/// `vm` must be the live VM, `bytes` bytes that C++ hands over, and `type` a raw string that C++ owns a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_blob_create(vm: &Vm, bytes: ForeignBytesAbi, r#type: Utf16String) -> *mut c_void {
    // SAFETY: The caller hands over the bytes and the string.
    let blob = unsafe { Blob::with_foreign_bytes(bytes, r#type) };
    cell_into_abi(allocate(vm, |_| blob))
}

/// The byte sequence of the Blob or File, which lives until the object dies or C++ moves its bytes into immutable
/// bytes or shared memory.
#[unsafe(no_mangle)]
pub extern "C" fn web_blob_byte_sequence(cell: &InterfaceCell, out_length: &mut usize) -> *const u8 {
    let bytes = blob_from_abi(cell).raw_bytes();
    *out_length = bytes.len();
    bytes.as_ptr()
}

/// The owner of the bytes of the Blob or File if C++ handed them over, or null.
#[unsafe(no_mangle)]
pub extern "C" fn web_blob_foreign_bytes_owner(cell: &InterfaceCell) -> *mut c_void {
    blob_from_abi(cell).foreign_bytes_owner()
}

/// Makes the Blob or File refer to the same bytes in storage that C++ hands over.
///
/// # Safety
///
/// `cell` must be the live cell of a Blob or a File, and `bytes` bytes that C++ hands over, which are those of the
/// object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_blob_set_foreign_bytes(cell: &InterfaceCell, bytes: ForeignBytesAbi) {
    // SAFETY: The caller passes a live cell and hands over the bytes.
    unsafe { blob_from_abi(cell).set_foreign_bytes(bytes) };
}

/// The type of the Blob or File, a raw AK::Utf16String that the call lends for as long as the object lives.
#[unsafe(no_mangle)]
pub extern "C" fn web_blob_type(cell: &InterfaceCell) -> usize {
    blob_from_abi(cell).type_string().raw_identity()
}

/// The result of slice blob given the Blob or File, start, end and contentType, which are null unless `has_start`,
/// `has_end` and C++ passes a contentType, https://w3c.github.io/FileAPI/#slice-blob
#[unsafe(no_mangle)]
#[allow(
    clippy::too_many_arguments,
    reason = "the arguments are the parameters of slice blob, which can each be null"
)]
pub extern "C" fn web_blob_slice(
    vm: &Vm,
    cell: &InterfaceCell,
    has_start: bool,
    start: i64,
    has_end: bool,
    end: i64,
    content_type: Option<Utf16String>,
) -> *mut c_void {
    let blob = blob_from_abi(cell);
    let sliced_blob = blob.slice_blob(
        has_start.then_some(start),
        has_end.then_some(end),
        content_type.as_ref(),
    );
    cell_into_abi(allocate(vm, |_| sliced_blob))
}

/// Whether the cell of a Blob or a File is that of a File.
#[unsafe(no_mangle)]
pub extern "C" fn web_blob_is_file(cell: &InterfaceCell) -> bool {
    cell.object_if::<File>().is_some()
}

/// A new File, as the File constructor creates it given one part of the bytes, the file name and the options with the
/// type, which are raw AK::Utf16Strings that C++ hands over, and the last modified date if `has_last_modified`.
#[unsafe(no_mangle)]
pub extern "C" fn web_file_create(
    vm: &Vm,
    bytes: ak::Span<'_, u8>,
    file_name: Utf16String,
    r#type: Utf16String,
    has_last_modified: bool,
    last_modified: i64,
) -> *mut c_void {
    let options = FilePropertyBag {
        parent: BlobPropertyBag {
            endings: EndingType::Transparent,
            r#type,
        },
        last_modified: has_last_modified.then_some(last_modified),
    };
    let file = File::create(bytes.as_slice().to_vec(), file_name, &options);
    cell_into_abi(allocate(vm, |_| file))
}

/// The name of the File, a raw AK::Utf16String that the call lends for as long as the File lives.
#[unsafe(no_mangle)]
pub extern "C" fn web_file_name(cell: &InterfaceCell) -> usize {
    cell.object::<File>().name_string().raw_identity()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_file_last_modified(cell: &InterfaceCell) -> i64 {
    cell.object::<File>().last_modified_value()
}

/// A new FileList with no files.
#[unsafe(no_mangle)]
pub extern "C" fn web_file_list_create(vm: &Vm) -> *mut c_void {
    cell_into_abi(allocate(vm, |defer_gc| FileList::new(defer_gc, &[])))
}

#[unsafe(no_mangle)]
pub extern "C" fn web_file_list_add_file(vm: &Vm, list: &InterfaceCell, file: &InterfaceCell) {
    let (list, file) = (list.object::<FileList>(), file.object::<File>());
    list.add_file(vm, file);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_file_list_length(list: &InterfaceCell) -> u32 {
    list.object::<FileList>().file_count()
}

/// The cell of the File at the index of the FileList, or null if it has none there.
#[unsafe(no_mangle)]
pub extern "C" fn web_file_list_item(list: &InterfaceCell, index: u32) -> *mut c_void {
    list.object::<FileList>()
        .file(index)
        .map_or(core::ptr::null_mut(), cell_into_abi)
}
