/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The objects that wrap a Boolean, Number, BigInt or String value: the C++ BooleanObject, NumberObject, BigIntObject
//! and StringObject.
//!
//! The functions here follow the contract object.rs states for the embedding module. Each object crosses as its
//! JSObject, and the functions that read one abort for any other kind of object, as the C++ as<T>() does; the
//! embedder tells the kinds apart with js_object_is_subclass_of() and the JS_LAYOUT_CLASS_ID_*_OBJECT ids.

#![allow(
    clippy::missing_safety_doc,
    reason = "object.rs states the contract every exported function shares"
)]

use crate::embedding::abi_types::{
    JSBigInt, JSPrimitiveString, JSRealm, cell_from_abi, cell_into_abi, object_into_abi, vm_from_abi,
};
use crate::gc::class::{Extends, GcCell};
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSObject, JSVM};
use crate::layout::object::Object;
use crate::runtime::big_int_object::BigIntObject;
use crate::runtime::boolean_object::BooleanObject;
use crate::runtime::number_object::NumberObject;
use crate::runtime::string_object::StringObject;

/// # Safety
///
/// `object` must be a live object of class T or a subclass of it.
unsafe fn wrapper_from_abi<T: GcCell + Extends<Object>>(object: *mut JSObject) -> Gc<T> {
    // SAFETY: The caller passes a live object.
    unsafe { cell_from_abi::<JSObject>(object) }
        .downcast::<T>()
        .expect("the object wraps a primitive of the expected type")
}

/// BooleanObject::create(realm, value): a Boolean object of the realm's %Boolean.prototype%. Returns an unrooted
/// object. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_primitive_wrapper_create_boolean(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    value: bool,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSRealm>(realm)) };
    object_into_abi(BooleanObject::create(vm, realm, value))
}

/// NumberObject::create(realm, value): a Number object of the realm's %Number.prototype%. Returns an unrooted object.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_primitive_wrapper_create_number(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    value: f64,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSRealm>(realm)) };
    object_into_abi(NumberObject::create(vm, realm, value))
}

/// BigIntObject::create(realm, bigint): a BigInt object of the realm's %BigInt.prototype%. Returns an unrooted
/// object. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_primitive_wrapper_create_bigint(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    bigint: *mut JSBigInt,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm, bigint) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSRealm>(realm),
            cell_from_abi::<JSBigInt>(bigint),
        )
    };
    object_into_abi(BigIntObject::create(vm, realm, bigint))
}

/// StringObject::create(realm, string, prototype), StringCreate ( value, prototype ): a String exotic object whose
/// "length" and indices reflect the string. Returns an unrooted object. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_primitive_wrapper_create_string(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    string: *mut JSPrimitiveString,
    prototype: *mut JSObject,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm, string, prototype) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSRealm>(realm),
            cell_from_abi::<JSPrimitiveString>(string),
            cell_from_abi::<JSObject>(prototype),
        )
    };
    object_into_abi(StringObject::create(vm, realm, string, prototype))
}

/// [[BooleanData]] of a Boolean object. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_primitive_wrapper_boolean(object: *mut JSObject) -> bool {
    // SAFETY: See the module documentation.
    unsafe { wrapper_from_abi::<BooleanObject>(object) }.boolean()
}

/// [[NumberData]] of a Number object. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_primitive_wrapper_number(object: *mut JSObject) -> f64 {
    // SAFETY: See the module documentation.
    unsafe { wrapper_from_abi::<NumberObject>(object) }.number()
}

/// [[BigIntData]] of a BigInt object. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_primitive_wrapper_bigint(object: *mut JSObject) -> *mut JSBigInt {
    // SAFETY: See the module documentation.
    cell_into_abi(unsafe { wrapper_from_abi::<BigIntObject>(object) }.bigint())
}

/// [[StringData]] of a String object. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_primitive_wrapper_string(object: *mut JSObject) -> *mut JSPrimitiveString {
    // SAFETY: See the module documentation.
    cell_into_abi(unsafe { wrapper_from_abi::<StringObject>(object) }.primitive_string())
}
