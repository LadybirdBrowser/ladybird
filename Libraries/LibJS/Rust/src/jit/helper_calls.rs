/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime helpers of the builtins JIT code calls without a native frame (`libjs_jit::snapshot::HelperCall`).
//! Each takes the VM, the `this` value and the arguments of the kinds its `HelperCall` lists (strings as values, int32
//! values as `i32`), runs no code, and returns what its `HelperResult` says, in 64 bits: the empty value where it
//! declines, having done nothing observable.

use libjs_jit::snapshot::Intrinsic;

use crate::interpreter::vm::Vm;
use crate::layout::value::Value;
use crate::runtime::keyed_collections::canonicalize_keyed_collection_key;
use crate::runtime::map::Map;
use crate::runtime::regexp_prototype::regexp_builtin_exec_running_no_code;
use crate::runtime::string_prototype::{StringPrototype, trim_primitive_string};
use crate::utf16::TrimMode;

/// The address of the runtime helper of `intrinsic`, or 0 if JIT code calls it in another way. Every intrinsic with a
/// `HelperCall` has a helper here.
pub fn address(intrinsic: Intrinsic) -> u64 {
    let helper: *const () = match intrinsic {
        Intrinsic::StringPrototypeIndexOf => string_index_of as *const (),
        Intrinsic::StringPrototypeLastIndexOf => string_last_index_of as *const (),
        Intrinsic::StringPrototypeIncludes => string_includes as *const (),
        Intrinsic::StringPrototypeStartsWith => string_starts_with as *const (),
        Intrinsic::StringPrototypeEndsWith => string_ends_with as *const (),
        Intrinsic::StringPrototypeSlice => string_slice as *const (),
        Intrinsic::StringPrototypeSubstring => string_substring as *const (),
        Intrinsic::StringPrototypeToLowerCase => string_to_lowercase as *const (),
        Intrinsic::StringPrototypeToUpperCase => string_to_uppercase as *const (),
        Intrinsic::StringPrototypeTrim => string_trim as *const (),
        Intrinsic::StringPrototypeTrimStart => string_trim_start as *const (),
        Intrinsic::StringPrototypeTrimEnd => string_trim_end as *const (),
        Intrinsic::RegExpPrototypeExec => regexp_exec as *const (),
        Intrinsic::MapPrototypeGet => map_get as *const (),
        Intrinsic::MapPrototypeHas => map_has as *const (),
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
        | Intrinsic::MathMin
        | Intrinsic::MathImul => return 0,
    };
    helper as u64
}

/// The string of a value JIT code checked to be a string.
fn string(value: u64) -> Value {
    let value = Value::from_bits(value);
    assert!(value.is_string());
    value
}

/// An int32 result, zero-extended.
fn int32(value: Value) -> u64 {
    // NB: Strings are shorter than 2^31 code units, so their indices are int32 values.
    u64::from(value.as_i32() as u32)
}

/// `string.indexOf(search_string, position)`.
extern "C" fn string_index_of(_vm: &Vm, string_value: u64, search_string: u64, position: i32) -> u64 {
    let (string_value, search_string) = (string(string_value), string(search_string));
    int32(StringPrototype::index_of_string(
        string_value.as_string(),
        search_string.as_string(),
        f64::from(position),
    ))
}

/// `string.toLowerCase()`.
extern "C" fn string_to_lowercase(vm: &Vm, string_value: u64) -> u64 {
    StringPrototype::to_lowercase_string(vm, string(string_value).as_string()).0
}

/// `string.toUpperCase()`.
extern "C" fn string_to_uppercase(vm: &Vm, string_value: u64) -> u64 {
    StringPrototype::to_uppercase_string(vm, string(string_value).as_string()).0
}

/// `string.lastIndexOf(search_string, position)`.
extern "C" fn string_last_index_of(_vm: &Vm, string_value: u64, search_string: u64, position: i32) -> u64 {
    let (string_value, search_string) = (string(string_value), string(search_string));
    int32(StringPrototype::last_index_of_string(
        string_value.as_string(),
        search_string.as_string(),
        f64::from(position),
    ))
}

/// `string.includes(search_string, position)`.
extern "C" fn string_includes(_vm: &Vm, string_value: u64, search_string: u64, position: i32) -> u64 {
    let (string_value, search_string) = (string(string_value), string(search_string));
    u64::from(StringPrototype::includes_string(
        string_value.as_string(),
        search_string.as_string(),
        f64::from(position),
    ))
}

/// `string.startsWith(search_string, position)`.
extern "C" fn string_starts_with(_vm: &Vm, string_value: u64, search_string: u64, position: i32) -> u64 {
    let (string_value, search_string) = (string(string_value), string(search_string));
    u64::from(StringPrototype::starts_with_string(
        string_value.as_string(),
        search_string.as_string(),
        f64::from(position),
    ))
}

/// `string.endsWith(search_string, end_position)`.
extern "C" fn string_ends_with(_vm: &Vm, string_value: u64, search_string: u64, end_position: i32) -> u64 {
    let (string_value, search_string) = (string(string_value), string(search_string));
    u64::from(StringPrototype::ends_with_string(
        string_value.as_string(),
        search_string.as_string(),
        f64::from(end_position),
    ))
}

/// `string.slice(start, end)`.
extern "C" fn string_slice(vm: &Vm, string_value: u64, start: i32, end: i32) -> u64 {
    StringPrototype::slice_string(vm, string(string_value).as_string(), f64::from(start), f64::from(end)).0
}

/// `string.substring(start, end)`.
extern "C" fn string_substring(vm: &Vm, string_value: u64, start: i32, end: i32) -> u64 {
    StringPrototype::substring_string(vm, string(string_value).as_string(), f64::from(start), f64::from(end)).0
}

/// `string.trim()`.
extern "C" fn string_trim(vm: &Vm, string_value: u64) -> u64 {
    trim_primitive_string(vm, string(string_value).as_string(), TrimMode::Both).0
}

/// `string.trimStart()`.
extern "C" fn string_trim_start(vm: &Vm, string_value: u64) -> u64 {
    trim_primitive_string(vm, string(string_value).as_string(), TrimMode::Left).0
}

/// `string.trimEnd()`.
extern "C" fn string_trim_end(vm: &Vm, string_value: u64) -> u64 {
    trim_primitive_string(vm, string(string_value).as_string(), TrimMode::Right).0
}

/// `regexp.exec(string)`, for a RegExp object `regexp` whose exec runs no code (see
/// `regexp_builtin_exec_running_no_code()`).
extern "C" fn regexp_exec(vm: &Vm, regexp: u64, string_value: u64) -> u64 {
    regexp_builtin_exec_running_no_code(vm, Value::from_bits(regexp), string(string_value).as_string())
        .map_or(Value::EMPTY.0, |result| result.0)
}

/// The Map a `this` value is, if it is one.
fn map(value: u64) -> Option<crate::layout::cell::Gc<Map>> {
    let value = Value::from_bits(value);
    if !value.is_object() {
        return None;
    }
    value.as_object().downcast::<Map>()
}

/// `map.get(key)`, or the empty value if the `this` value is no Map.
extern "C" fn map_get(_vm: &Vm, map_value: u64, key: u64) -> u64 {
    let Some(map) = map(map_value) else {
        return Value::EMPTY.0;
    };
    map.map_get(canonicalize_keyed_collection_key(Value::from_bits(key)))
        .unwrap_or(Value::UNDEFINED)
        .0
}

/// `map.has(key)`, or the empty value if the `this` value is no Map.
extern "C" fn map_has(_vm: &Vm, map_value: u64, key: u64) -> u64 {
    let Some(map) = map(map_value) else {
        return Value::EMPTY.0;
    };
    Value::from_bool(map.map_has(canonicalize_keyed_collection_key(Value::from_bits(key)))).0
}
