/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime helpers of the builtins JIT code calls without a native frame (`libjs_jit::snapshot::HelperCall`).
//! Each takes the VM, the `this` value and the arguments of the kinds its `HelperCall` lists (strings as values, int32
//! values as `i32`), runs no code, and returns what its `HelperResult` says, in 64 bits.

use libjs_jit::snapshot::Intrinsic;

use crate::interpreter::vm::Vm;
use crate::layout::value::Value;
use crate::runtime::string_prototype::StringPrototype;

/// The address of the runtime helper of `intrinsic`, or 0 if JIT code calls it in another way. Every intrinsic with a
/// `HelperCall` has a helper here.
pub fn address(intrinsic: Intrinsic) -> u64 {
    let helper: *const () = match intrinsic {
        Intrinsic::StringPrototypeIndexOf => string_index_of as *const (),
        Intrinsic::StringPrototypeToLowerCase => string_to_lowercase as *const (),
        Intrinsic::StringPrototypeToUpperCase => string_to_uppercase as *const (),
        Intrinsic::FunctionPrototypeApply
        | Intrinsic::FunctionPrototypeCall
        | Intrinsic::ArrayPrototypePush
        | Intrinsic::ArrayPrototypeSlice
        | Intrinsic::ObjectPrototypeHasOwnProperty
        | Intrinsic::StringFromCharCode
        | Intrinsic::StringConstructor
        | Intrinsic::ArrayConstructor
        | Intrinsic::ObjectConstructor
        | Intrinsic::BooleanConstructor
        | Intrinsic::TypedArrayPrototypeLength
        | Intrinsic::MathMax
        | Intrinsic::MathMin => return 0,
    };
    helper as u64
}

/// The string of a value JIT code checked to be a string.
fn string(value: u64) -> Value {
    let value = Value(value);
    assert!(value.is_string());
    value
}

/// An int32 result, zero-extended.
fn int32(value: Value) -> u64 {
    // NB: Strings are shorter than 2^31 code units, so their indices are int32 values.
    u64::from(value.as_i32() as u32)
}

/// `string.indexOf(search_string, position)`.
unsafe extern "C" fn string_index_of(_vm: *const Vm, string_value: u64, search_string: u64, position: i32) -> u64 {
    let (string_value, search_string) = (string(string_value), string(search_string));
    int32(StringPrototype::index_of_string(
        string_value.as_string(),
        search_string.as_string(),
        f64::from(position),
    ))
}

/// `string.toLowerCase()`.
unsafe extern "C" fn string_to_lowercase(vm: *const Vm, string_value: u64) -> u64 {
    // SAFETY: JIT code passes its VM.
    let vm = unsafe { &*vm };
    StringPrototype::to_lowercase_string(vm, string(string_value).as_string()).0
}

/// `string.toUpperCase()`.
unsafe extern "C" fn string_to_uppercase(vm: *const Vm, string_value: u64) -> u64 {
    // SAFETY: JIT code passes its VM.
    let vm = unsafe { &*vm };
    StringPrototype::to_uppercase_string(vm, string(string_value).as_string()).0
}
