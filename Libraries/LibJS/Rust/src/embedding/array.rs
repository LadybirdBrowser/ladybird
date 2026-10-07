/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Creating arrays and reading and writing their indexed storage.
//!
//! The functions here follow the contract object.rs states for the embedding module.

#![allow(
    clippy::missing_safety_doc,
    reason = "object.rs states the contract every exported function shares"
)]

use crate::embedding::abi_types::{
    JSRealm, cell_from_abi, completion_into_abi, completion_writing_result_to, object_into_abi, optional_cell_from_abi,
    vm_from_abi,
};
use crate::embedding::object::values_from_abi;
use crate::gc::root::MarkedVec;
use crate::layout::host_class::{JSCompletion, JSObject, JSVM, JSValue};
use crate::layout::value::Value;
use crate::runtime::abstract_operations::length_of_array_like;
use crate::runtime::array::Array;
use crate::runtime::property_attributes::DEFAULT_ATTRIBUTES;

/// ArrayCreate(length, proto), whose payload is the array. A null prototype stands for the realm's
/// %Array.prototype%, and a length above 2^32 - 1 throws a RangeError. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_create(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    length: u64,
    prototype: *mut JSObject,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, realm, prototype) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSRealm>(realm),
            optional_cell_from_abi::<JSObject>(prototype),
        )
    };
    completion_into_abi(Array::create(vm, realm, length, prototype))
}

/// CreateArrayFromList(elements), for `count` values at `elements`, which the runtime roots while it allocates the
/// array. Returns an unrooted array. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_create_from(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    elements: *const JSValue,
    count: usize,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm, elements) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSRealm>(realm),
            values_from_abi(elements, count),
        )
    };
    let rooted_elements = MarkedVec::with_capacity(vm, elements.len());
    for element in elements {
        rooted_elements.push(*element);
    }
    object_into_abi(Array::create_from_list(vm, realm, &rooted_elements))
}

/// LengthOfArrayLike ( obj ), which gets the object's "length" and converts it with ToLength, and writes it to
/// `length`. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_length_of_array_like(
    vm: *mut JSVM,
    object: *mut JSObject,
    length: *mut u64,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(object)) };
    // SAFETY: As above, `length` is writable.
    unsafe { completion_writing_result_to(length_of_array_like(vm, &object), length) }
}

/// The size of the object's indexed storage: the length of an array, or one past its highest index. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_indexed_array_like_size(object: *mut JSObject) -> u32 {
    // SAFETY: See the module documentation.
    unsafe { cell_from_abi::<JSObject>(object) }.indexed_array_like_size()
}

/// Appends a writable, enumerable and configurable element to the object's indexed storage, without running any
/// internal method. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_indexed_append(object: *mut JSObject, value: JSValue) {
    // SAFETY: See the module documentation.
    unsafe { cell_from_abi::<JSObject>(object) }.indexed_append(Value(value), DEFAULT_ATTRIBUTES);
}

/// Removes the first element of the object's indexed storage, which must not be empty, shifting the others down, and
/// returns its value, which is empty for a hole. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_array_indexed_take_first(object: *mut JSObject) -> JSValue {
    // SAFETY: See the module documentation.
    unsafe { cell_from_abi::<JSObject>(object) }
        .indexed_take_first()
        .value
        .0
}
