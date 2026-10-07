/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! RegExp objects, their source and flags.
//!
//! The functions here follow the contract object.rs states for the embedding module. A RegExp crosses as its
//! JSObject, and the functions that take one abort for any other object, as the C++ as<JS::RegExpObject>() does.

#![allow(
    clippy::missing_safety_doc,
    reason = "object.rs states the contract every exported function shares"
)]

use crate::embedding::abi_types::{
    JSOwnedUtf16String, cell_from_abi, completion_into_abi, owned_utf16_string_into_abi, vm_from_abi,
};
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSCompletion, JSObject, JSVM, JSValue};
use crate::layout::value::Value;
use crate::runtime::regexp_object::{RegExpObject, regexp_create};

/// # Safety
///
/// `regexp` must be a live RegExp object.
unsafe fn regexp_from_abi(regexp: *mut JSObject) -> Gc<RegExpObject> {
    // SAFETY: The caller passes a live object.
    unsafe { cell_from_abi::<JSObject>(regexp) }
        .downcast::<RegExpObject>()
        .expect("the object is a RegExp")
}

/// RegExpCreate ( P, F ), whose payload is a RegExp of the current realm's %RegExp%. P and F are converted to strings,
/// undefined to the empty one, and a pattern or flags that do not parse throw a SyntaxError. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_regexp_create(vm: *mut JSVM, pattern: JSValue, flags: JSValue) -> JSCompletion {
    // SAFETY: See the module documentation.
    let vm = unsafe { vm_from_abi(vm) };
    completion_into_abi(regexp_create(vm, Value(pattern), Value(flags)))
}

/// [[OriginalSource]], the pattern the RegExp was created with, as an owned string. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_regexp_pattern(regexp: *mut JSObject) -> JSOwnedUtf16String {
    // SAFETY: See the module documentation.
    owned_utf16_string_into_abi(unsafe { regexp_from_abi(regexp) }.pattern())
}

/// [[OriginalFlags]], the flags the RegExp was created with, as an owned string. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_regexp_flags(regexp: *mut JSObject) -> JSOwnedUtf16String {
    // SAFETY: See the module documentation.
    owned_utf16_string_into_abi(unsafe { regexp_from_abi(regexp) }.flags())
}
