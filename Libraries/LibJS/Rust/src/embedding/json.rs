/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! JSON parsing and serialization.
//!
//! The functions here follow the contract object.rs states for the embedding module, and run in the current realm,
//! whose intrinsics the values they create use.

#![allow(
    clippy::missing_safety_doc,
    reason = "object.rs states the contract every exported function shares"
)]

use crate::embedding::abi_types::{
    JSOwnedUtf16String, JSUtf16View, completion_into_abi, owned_utf16_string_into_abi, vm_from_abi,
};
use crate::layout::host_class::{JSCompletion, JSVM, JSValue};
use crate::layout::value::Value;
use crate::runtime::json_object::JSONObject;

/// ParseJSON ( text ) without a reviver, whose payload is the value the JSON text describes. Text that is not JSON
/// throws a SyntaxError. The text is borrowed for the call. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_json_parse(vm: *mut JSVM, text: JSUtf16View) -> JSCompletion {
    // SAFETY: See the module documentation; the view is of code units that outlive the call.
    let (vm, text) = unsafe { (vm_from_abi(vm), text.as_view()) };
    completion_into_abi(JSONObject::parse_json(vm, text, None))
}

/// JSON.stringify ( value, replacer, space ), with undefined for an argument not given. The payload is whether there
/// is a string, which is then written to `string` as an owned string: values such as undefined and functions have
/// none. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_json_stringify(
    vm: *mut JSVM,
    value: JSValue,
    replacer: JSValue,
    space: JSValue,
    string: *mut JSOwnedUtf16String,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let vm = unsafe { vm_from_abi(vm) };
    let serialized = JSONObject::stringify_impl(vm, Value(value), Value(replacer), Value(space));
    completion_into_abi(serialized.map(|serialized| {
        let Some(serialized) = serialized else {
            return false;
        };
        assert!(!string.is_null(), "the embedder passes an out parameter");
        // SAFETY: As above, `string` is writable.
        unsafe { string.write(owned_utf16_string_into_abi(serialized)) };
        true
    }))
}
