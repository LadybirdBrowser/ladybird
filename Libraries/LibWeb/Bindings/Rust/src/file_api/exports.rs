/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ creates the Blob and File objects that it hands to script, and reads those that
//! it receives, which Libraries/LibWeb/FileAPI/Blob.h and File.h declare. A Blob cell can be that of a File.

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::Vm;

use super::blob::{Blob, BlobPropertyBag, EndingType, ForeignBytesAbi};
use super::file::{File, FilePropertyBag};
use crate::bindings::interface::{Inherits, Interface, Ref, allocate, cell_from_abi, cell_if_abi, cell_into_abi};

/// # Safety
///
/// `vm` must be the live VM.
unsafe fn vm_from_abi<'vm>(vm: *mut c_void) -> &'vm Vm {
    assert!(!vm.is_null(), "C++ passes the VM");
    // SAFETY: The caller passes the live VM.
    unsafe { &*vm.cast::<Vm>() }
}

/// The implementation of the object whose cell C++ lends, for as long as C++ keeps the cell alive.
///
/// # Safety
///
/// `cell` must be the live cell of an object of the interface, and it must outlive `'a`.
unsafe fn object_from_abi<'a, T: Interface>(cell: *mut c_void) -> &'a T {
    // SAFETY: The caller passes a live cell of the interface.
    let object: Ref<T> = unsafe { cell_from_abi(cell) };
    // SAFETY: The caller keeps the cell alive for 'a.
    unsafe { &*object.as_ptr() }
}

/// The Blob or File whose cell C++ lends, as a Blob.
///
/// # Safety
///
/// `cell` must be the live cell of a Blob or a File, and it must outlive `'a`.
unsafe fn blob_from_abi<'a>(cell: *mut c_void) -> &'a Blob {
    // SAFETY: The caller passes a live cell.
    if let Some(blob) = unsafe { cell_if_abi::<Blob>(cell) } {
        // SAFETY: The caller keeps the cell alive for 'a.
        return unsafe { &*blob.as_ptr() };
    }
    // SAFETY: The caller passes the cell of a File, which it keeps alive for 'a.
    unsafe { object_from_abi::<File>(cell) }.parent()
}

/// A new Blob that refers to the bytes that C++ hands over, with the type, a raw AK::Utf16String that C++ hands over.
///
/// # Safety
///
/// `vm` must be the live VM, `bytes` bytes that C++ hands over, and `type` a raw string that C++ owns a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_blob_create(vm: *mut c_void, bytes: ForeignBytesAbi, r#type: usize) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller hands over the bytes and the string.
    let blob = unsafe { Blob::with_foreign_bytes(bytes, Utf16String::from_raw_owned(r#type)) };
    cell_into_abi(allocate(vm, |_| blob))
}

/// The byte sequence of the Blob or File, which lives until the object dies or its bytes move to shared memory.
///
/// # Safety
///
/// `cell` must be the live cell of a Blob or a File.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_blob_byte_sequence(cell: *mut c_void, out_length: &mut usize) -> *const u8 {
    // SAFETY: The caller passes a live cell.
    let bytes = unsafe { blob_from_abi(cell) }.raw_bytes();
    *out_length = bytes.len();
    bytes.as_ptr()
}

/// The owner of the bytes of the Blob or File if C++ handed them over, or null.
///
/// # Safety
///
/// `cell` must be the live cell of a Blob or a File.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_blob_foreign_bytes_owner(cell: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live cell.
    unsafe { blob_from_abi(cell) }.foreign_bytes_owner()
}

/// Makes the Blob or File refer to the same bytes in storage that C++ hands over.
///
/// # Safety
///
/// `cell` must be the live cell of a Blob or a File, and `bytes` bytes that C++ hands over, which are those of the
/// object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_blob_set_foreign_bytes(cell: *mut c_void, bytes: ForeignBytesAbi) {
    // SAFETY: The caller passes a live cell and hands over the bytes.
    unsafe { blob_from_abi(cell).set_foreign_bytes(bytes) };
}

/// The type of the Blob or File, a raw AK::Utf16String that the call lends for as long as the object lives.
///
/// # Safety
///
/// `cell` must be the live cell of a Blob or a File.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_blob_type(cell: *mut c_void) -> usize {
    // SAFETY: The caller passes a live cell.
    unsafe { blob_from_abi(cell) }.type_string().raw_identity()
}

/// The result of slice blob given the Blob or File, start, end and contentType, which are null unless `has_start`,
/// `has_end` and `has_content_type`. contentType is a raw AK::Utf16String that C++ hands over,
/// https://w3c.github.io/FileAPI/#slice-blob
///
/// # Safety
///
/// `vm` must be the live VM, `cell` the live cell of a Blob or a File, and `content_type` a raw string that C++ owns a
/// reference to if `has_content_type`.
#[unsafe(no_mangle)]
#[allow(
    clippy::too_many_arguments,
    reason = "the arguments are the parameters of slice blob, which can each be null"
)]
pub unsafe extern "C" fn web_blob_slice(
    vm: *mut c_void,
    cell: *mut c_void,
    has_start: bool,
    start: i64,
    has_end: bool,
    end: i64,
    has_content_type: bool,
    content_type: usize,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller hands over the string if it has one.
    let content_type = has_content_type.then(|| unsafe { Utf16String::from_raw_owned(content_type) });
    // SAFETY: The caller passes a live cell.
    let blob = unsafe { blob_from_abi(cell) };
    let sliced_blob = blob.slice_blob(
        has_start.then_some(start),
        has_end.then_some(end),
        content_type.as_ref(),
    );
    cell_into_abi(allocate(vm, |_| sliced_blob))
}

/// Whether the cell of a Blob or a File is that of a File.
///
/// # Safety
///
/// `cell` must be the live cell of a Blob or a File.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_blob_is_file(cell: *mut c_void) -> bool {
    // SAFETY: The caller passes a live cell.
    unsafe { cell_if_abi::<File>(cell) }.is_some()
}

/// A new File, as the File constructor creates it given one part of the bytes, the file name and the options with the
/// type, which are raw AK::Utf16Strings that C++ hands over, and the last modified date if `has_last_modified`.
///
/// # Safety
///
/// `vm` must be the live VM, and `file_name` and `type` raw strings that C++ owns a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_file_create(
    vm: *mut c_void,
    bytes: ak::Span<'_, u8>,
    file_name: usize,
    r#type: usize,
    has_last_modified: bool,
    last_modified: i64,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller hands over the strings.
    let (file_name, r#type) = unsafe {
        (
            Utf16String::from_raw_owned(file_name),
            Utf16String::from_raw_owned(r#type),
        )
    };
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
///
/// # Safety
///
/// `cell` must be the live cell of a File.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_file_name(cell: *mut c_void) -> usize {
    // SAFETY: The caller passes a live cell of a File.
    unsafe { object_from_abi::<File>(cell) }.name_string().raw_identity()
}

/// # Safety
///
/// `cell` must be the live cell of a File.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_file_last_modified(cell: *mut c_void) -> i64 {
    // SAFETY: The caller passes a live cell of a File.
    unsafe { object_from_abi::<File>(cell) }.last_modified_value()
}
