/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Iterators and iterator result objects.
//!
//! The functions here follow the contract object.rs states for the embedding module. The operations on an Iterator
//! Record call the iterator's methods, which may run any JavaScript.

#![allow(
    clippy::missing_safety_doc,
    reason = "object.rs states the contract every exported function shares"
)]

use crate::embedding::abi_types::{JSRealm, cell_from_abi, completion_into_abi, object_into_abi, vm_from_abi};
use crate::layout::host_class::{JSCompletion, JSObject, JSVM, JSValue};
use crate::layout::value::Value;
use crate::runtime::iterator::{create_iterator_result_object, iterator_complete, iterator_value};

/// IteratorComplete ( iteratorResult ), whose payload is the bool of its "done". Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_iterator_complete(vm: *mut JSVM, iterator_result: *mut JSObject) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, iterator_result) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(iterator_result)) };
    completion_into_abi(iterator_complete(vm, iterator_result))
}

/// IteratorValue ( iteratorResult ), whose payload is its "value". Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_iterator_value(vm: *mut JSVM, iterator_result: *mut JSObject) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, iterator_result) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(iterator_result)) };
    completion_into_abi(iterator_value(vm, iterator_result))
}

/// CreateIteratorResultObject ( value, done ), an object of the realm's %Object.prototype%. Returns an unrooted object.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_iterator_create_result_object(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    value: JSValue,
    done: bool,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSRealm>(realm)) };
    object_into_abi(create_iterator_result_object(vm, realm, Value(value), done))
}
