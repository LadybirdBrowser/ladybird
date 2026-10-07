/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! String values: creating them from the embedder's strings, and reading their code units. Every function runs on the
//! thread that owns the VM.

use core::mem::ManuallyDrop;

use ak::Utf16FlyString;

use crate::embedding::abi_types::{
    JSOwnedUtf16String, JSPrimitiveString, JSUtf16View, cell_from_abi, cell_into_abi, owned_utf16_string_from_abi,
    owned_utf16_string_into_abi, vm_from_abi,
};
use crate::layout::host_class::JSVM;
use crate::runtime::primitive_string::PrimitiveString;

/// A string of the code units `string` views, which are copied. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, and `string` a view of code units that stay unchanged during the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_string_create_from_utf16_view(
    vm: *mut JSVM,
    string: JSUtf16View,
) -> *mut JSPrimitiveString {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a view of code units that stay unchanged during the call.
    cell_into_abi(PrimitiveString::create_from_utf16_view(vm, unsafe { string.as_view() }))
}

/// A string that adopts the storage of `string` without copying it, unless the string is short enough for the VM to
/// share a cached one. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, and `string` an AK::Utf16String whose reference the caller gives up.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_string_create_from_owned_utf16_string(
    vm: *mut JSVM,
    string: JSOwnedUtf16String,
) -> *mut JSPrimitiveString {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller gives up its reference to the string.
    cell_into_abi(PrimitiveString::create(vm, unsafe {
        owned_utf16_string_from_abi(string)
    }))
}

/// The string of an AK::Utf16FlyString, which the VM caches by the fly string's identity. The caller keeps its
/// reference to the fly string. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, and `fly_string` the raw word of an AK::Utf16FlyString that stays alive during the
/// call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_string_create_from_utf16_fly_string(
    vm: *mut JSVM,
    fly_string: usize,
) -> *mut JSPrimitiveString {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller keeps the fly string alive and its reference, which this borrows without releasing.
    let fly_string = ManuallyDrop::new(unsafe { Utf16FlyString::from_raw_owned(fly_string) });
    cell_into_abi(PrimitiveString::create_from_fly_string(vm, &fly_string))
}

/// The decimal digits of `number`, as a string the VM may share. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_string_create_from_unsigned_integer(vm: *mut JSVM, number: u64) -> *mut JSPrimitiveString {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    cell_into_abi(PrimitiveString::create_from_unsigned_integer(vm, number))
}

/// The code units of `string` as an AK::Utf16String, which the caller owns. It shares the string's storage, after
/// resolving a string the VM built lazily. Call on the VM's thread.
///
/// # Safety
///
/// `string` must be a string of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_string_utf16_string(string: *mut JSPrimitiveString) -> JSOwnedUtf16String {
    // SAFETY: The caller passes a string of the VM.
    owned_utf16_string_into_abi(unsafe { cell_from_abi(string) }.utf16_string())
}

/// A view of the code units of `string`, after resolving a string the VM built lazily. The view stays valid for as long
/// as the string lives. Call on the VM's thread.
///
/// # Safety
///
/// `string` must be a string of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_string_utf16_view(string: *mut JSPrimitiveString) -> JSUtf16View {
    // SAFETY: The caller passes a string of the VM.
    JSUtf16View::of(unsafe { cell_from_abi(string) }.resolved_utf16_string_view())
}

/// The length of `string` in UTF-16 code units. Call on the VM's thread.
///
/// # Safety
///
/// `string` must be a string of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_string_length_in_code_units(string: *mut JSPrimitiveString) -> usize {
    // SAFETY: The caller passes a string of the VM.
    unsafe { cell_from_abi(string) }.length_in_utf16_code_units()
}

/// Whether `lhs` and `rhs` have the same code units. Call on the VM's thread.
///
/// # Safety
///
/// `lhs` and `rhs` must be strings of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_string_equals(lhs: *mut JSPrimitiveString, rhs: *mut JSPrimitiveString) -> bool {
    // SAFETY: The caller passes strings of the VM.
    let (lhs, rhs) = unsafe { (cell_from_abi(lhs), cell_from_abi(rhs)) };
    *lhs == *rhs
}
