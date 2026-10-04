/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Values and their conversions. Every function runs on the thread that owns the VM, and the conversions that can
//! throw follow the completion conventions of abi_types.rs.

use crate::embedding::abi_types::{
    JSBigInt, JSOwnedUtf16String, cell_into_abi, completion_into_abi, completion_writing_result_to,
    owned_utf16_string_into_abi, value_from_abi, vm_from_abi,
};
use crate::layout::host_class::{JSCompletion, JSVM, JSValue};
use crate::runtime::value::{same_value, same_value_zero};

/// ToBoolean(value). Call on the VM's thread.
///
/// # Safety
///
/// `value` must be a value of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_boolean(value: JSValue) -> bool {
    value_from_abi(value).to_boolean()
}

/// IsConstructor(value). Call on the VM's thread.
///
/// # Safety
///
/// `value` must be a value of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_is_constructor(value: JSValue) -> bool {
    value_from_abi(value).is_constructor()
}

/// SameValue(lhs, rhs). Call on the VM's thread.
///
/// # Safety
///
/// `lhs` and `rhs` must be values of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_same_value(lhs: JSValue, rhs: JSValue) -> bool {
    same_value(value_from_abi(lhs), value_from_abi(rhs))
}

/// SameValueZero(lhs, rhs). Call on the VM's thread.
///
/// # Safety
///
/// `lhs` and `rhs` must be values of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_same_value_zero(lhs: JSValue, rhs: JSValue) -> bool {
    same_value_zero(value_from_abi(lhs), value_from_abi(rhs))
}

/// A description of `value` for diagnostics, which never runs JavaScript: strings as they are, numbers, symbols and
/// BigInts as ToString would show them, and objects as "[object ClassName]". The caller owns the returned string. Call
/// on the VM's thread.
///
/// # Safety
///
/// `value` must be a value of the embedder's VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_utf16_string_without_side_effects(value: JSValue) -> JSOwnedUtf16String {
    owned_utf16_string_into_abi(value_from_abi(value).to_utf16_string_without_side_effects())
}

/// ToNumber(value), whose Number result is the payload of the normal completion. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM and `value` a value of it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_number(vm: *mut JSVM, value: JSValue) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    completion_into_abi(value_from_abi(value).to_number(vm))
}

/// ToObject(value), whose JSObject result is the payload of the normal completion. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM and `value` a value of it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_object(vm: *mut JSVM, value: JSValue) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    completion_into_abi(value_from_abi(value).to_object(vm))
}

/// ToNumber(value) as a double, written to `out`. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, `value` a value of it, and `out` valid for writing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_double(vm: *mut JSVM, value: JSValue, out: *mut f64) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    let result = value_from_abi(value).to_double(vm);
    // SAFETY: The caller passes an out parameter valid for writing.
    unsafe { completion_writing_result_to(result, out) }
}

/// ToInt32(value), written to `out`. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, `value` a value of it, and `out` valid for writing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_i32(vm: *mut JSVM, value: JSValue, out: *mut i32) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    let result = value_from_abi(value).to_i32(vm);
    // SAFETY: The caller passes an out parameter valid for writing.
    unsafe { completion_writing_result_to(result, out) }
}

/// ToUint32(value), written to `out`. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, `value` a value of it, and `out` valid for writing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_u32(vm: *mut JSVM, value: JSValue, out: *mut u32) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    let result = value_from_abi(value).to_u32(vm);
    // SAFETY: The caller passes an out parameter valid for writing.
    unsafe { completion_writing_result_to(result, out) }
}

/// ToUint16(value), written to `out`. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, `value` a value of it, and `out` valid for writing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_u16(vm: *mut JSVM, value: JSValue, out: *mut u16) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    let result = value_from_abi(value).to_u16(vm);
    // SAFETY: The caller passes an out parameter valid for writing.
    unsafe { completion_writing_result_to(result, out) }
}

/// ToUint8(value), written to `out`. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, `value` a value of it, and `out` valid for writing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_u8(vm: *mut JSVM, value: JSValue, out: *mut u8) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    let result = value_from_abi(value).to_u8(vm);
    // SAFETY: The caller passes an out parameter valid for writing.
    unsafe { completion_writing_result_to(result, out) }
}

/// ToBigInt(value), whose JSBigInt result is the payload of the normal completion. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM and `value` a value of it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_bigint(vm: *mut JSVM, value: JSValue) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    completion_into_abi(value_from_abi(value).to_bigint(vm).map(cell_into_abi::<JSBigInt>))
}

/// ToBigUint64(value), written to `out`. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, `value` a value of it, and `out` valid for writing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_bigint_uint64(vm: *mut JSVM, value: JSValue, out: *mut u64) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    let result = value_from_abi(value).to_bigint_uint64(vm);
    // SAFETY: The caller passes an out parameter valid for writing.
    unsafe { completion_writing_result_to(result, out) }
}

/// ToLength(value), written to `out`. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, `value` a value of it, and `out` valid for writing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_length(vm: *mut JSVM, value: JSValue, out: *mut u64) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    let result = value_from_abi(value).to_length(vm);
    // SAFETY: The caller passes an out parameter valid for writing.
    unsafe { completion_writing_result_to(result, out) }
}

/// ToString(value), written to `out`. The caller owns the string. Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM, `value` a value of it, and `out` valid for writing.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_to_utf16_string(
    vm: *mut JSVM,
    value: JSValue,
    out: *mut JSOwnedUtf16String,
) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    let result = value_from_abi(value)
        .to_utf16_string(vm)
        .map(owned_utf16_string_into_abi);
    // SAFETY: The caller passes an out parameter valid for writing.
    unsafe { completion_writing_result_to(result, out) }
}

/// IsArray(value), whose bool result is the payload of the normal completion, and which throws for a revoked proxy.
/// Call on the VM's thread.
///
/// # Safety
///
/// `vm` must be the embedder's VM and `value` a value of it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_value_is_array(vm: *mut JSVM, value: JSValue) -> JSCompletion {
    // SAFETY: The caller passes its VM.
    let vm = unsafe { vm_from_abi(vm) };
    completion_into_abi(value_from_abi(value).is_array(vm))
}
