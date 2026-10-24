/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Exported GC Objects, https://webassembly.github.io/spec/js-api/#exported-gc-object: objects that stand for a struct
//! or an array, whose reference is the address of its cell. An object keeps that cell as its host data, and has no
//! properties, no prototype, and no way to get either.

use core::ptr::NonNull;

use libjs_rust::api::{
    ErrorKind, Gc, HostObject, JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE, JS_HOST_CLASS_OBJECT, JSCompletion,
    JSGetCacheMetadata, JSHostClass, JSHostObjectHooks, JSObject, JSPropertyDescriptor, JSPropertyKey,
    JSSetCacheMetadata, JSValue, JSValueSink, Object, Value, Vm, completion_into_abi, host_data_of,
    is_host_instance_of,
};

use crate::web_assembly::{Agent, host_object, set_host_object};

static EXPORTED_GC_OBJECT_HOOKS: JSHostObjectHooks = JSHostObjectHooks {
    get_prototype_of: Some(get_prototype_of),
    set_prototype_of: Some(set_prototype_of),
    is_extensible: Some(is_extensible),
    prevent_extensions: Some(prevent_extensions),
    get_own_property: Some(get_own_property),
    define_own_property: Some(define_own_property),
    has_property: Some(has_property),
    get: Some(get),
    set: Some(set),
    delete_property: Some(delete_property),
    own_property_keys: Some(own_property_keys),
    is_cacheable_for_inherited_property: None,
    error_data: None,
    finalize: None,
    get_indexed_value: None,
};

static EXPORTED_GC_OBJECT_CLASS: JSHostClass = JSHostClass::new(
    JS_HOST_CLASS_OBJECT,
    "WebAssemblyGCObject",
    None,
    (&raw const EXPORTED_GC_OBJECT_HOOKS).cast(),
    JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE,
);

fn throw_type_error(message: &str) -> JSCompletion {
    let vm = Agent::current().vm();
    completion_into_abi(vm.throw_completion_with_message::<()>(ErrorKind::TypeError, message.to_string()))
}

// https://webassembly.github.io/spec/js-api/#exported-gc-object-getprototypeof
unsafe extern "C" fn get_prototype_of(_: *mut JSObject) -> JSCompletion {
    // 1. Return null.
    completion_into_abi(Ok(()))
}

// https://webassembly.github.io/spec/js-api/#exported-gc-object-setprototypeof
unsafe extern "C" fn set_prototype_of(_: *mut JSObject, _: *mut JSObject) -> JSCompletion {
    // 1. Throw a TypeError.
    throw_type_error("Cannot set the prototype of a WebAssembly GC object")
}

// https://webassembly.github.io/spec/js-api/#exported-gc-object-isextensible
unsafe extern "C" fn is_extensible(_: *mut JSObject) -> JSCompletion {
    // 1. Return false.
    completion_into_abi(Ok(false))
}

// https://webassembly.github.io/spec/js-api/#exported-gc-object-preventextensions
unsafe extern "C" fn prevent_extensions(_: *mut JSObject) -> JSCompletion {
    // 1. Throw a TypeError.
    throw_type_error("Cannot prevent extensions of a WebAssembly GC object")
}

// https://webassembly.github.io/spec/js-api/#exported-gc-object-getownproperty
unsafe extern "C" fn get_own_property(
    _: *mut JSObject,
    _: JSPropertyKey,
    _: *mut JSPropertyDescriptor,
) -> JSCompletion {
    // 1. Return undefined.
    completion_into_abi(Ok(()))
}

// https://webassembly.github.io/spec/js-api/#exported-gc-object-defineownproperty
unsafe extern "C" fn define_own_property(
    _: *mut JSObject,
    _: JSPropertyKey,
    _: *mut JSPropertyDescriptor,
    _: *const JSPropertyDescriptor,
) -> JSCompletion {
    // 1. Throw a TypeError.
    throw_type_error("Cannot define properties of a WebAssembly GC object")
}

// https://webassembly.github.io/spec/js-api/#exported-gc-object-hasproperty
unsafe extern "C" fn has_property(_: *mut JSObject, _: JSPropertyKey) -> JSCompletion {
    // 1. Return false.
    completion_into_abi(Ok(false))
}

// https://webassembly.github.io/spec/js-api/#exported-gc-object-get
unsafe extern "C" fn get(
    _: *mut JSObject,
    _: JSPropertyKey,
    _: JSValue,
    _: *mut JSGetCacheMetadata,
    _: u8,
) -> JSCompletion {
    // 1. Return undefined.
    completion_into_abi(Ok(Value::UNDEFINED))
}

// https://webassembly.github.io/spec/js-api/#exported-gc-object-set
unsafe extern "C" fn set(
    _: *mut JSObject,
    _: JSPropertyKey,
    _: JSValue,
    _: JSValue,
    _: *mut JSSetCacheMetadata,
    _: u8,
) -> JSCompletion {
    // 1. Throw a TypeError.
    throw_type_error("Cannot set properties of a WebAssembly GC object")
}

// https://webassembly.github.io/spec/js-api/#exported-gc-object-delete
unsafe extern "C" fn delete_property(_: *mut JSObject, _: JSPropertyKey) -> JSCompletion {
    // 1. Throw a TypeError.
    throw_type_error("Cannot delete properties of a WebAssembly GC object")
}

// https://webassembly.github.io/spec/js-api/#exported-gc-object-ownpropertykeys
unsafe extern "C" fn own_property_keys(_: *mut JSObject, _: *mut JSValueSink) -> JSCompletion {
    // 1. Let keys be a new empty list.
    // 2. Return keys.
    completion_into_abi(Ok(()))
}

/// https://webassembly.github.io/spec/js-api/#create-a-new-exported-gc-object
pub(crate) fn create_exported_gc_object(vm: &Vm, reference: u64) -> Gc<Object> {
    // 1. Assert: objectaddr is a struct or array address.
    // 2. Let map be the current agent's associated exported GC object cache.
    // 3. If map[objectaddr] exists, return map[objectaddr].
    let cell = reference as *mut u8;
    if let Some(object) = host_object::<Object>(cell) {
        return object;
    }

    // 4. Let object be a new Exported GC Object, with [[ObjectAddress]] set to objectaddr and [[ObjectKind]] set to
    //    objectKind.
    let realm = vm.current_realm().expect("a conversion runs in a realm");
    // SAFETY: The class is of kind JS_HOST_CLASS_OBJECT, and the host data is the live cell of the struct or array.
    let object = unsafe {
        HostObject::create(
            vm,
            realm,
            &EXPORTED_GC_OBJECT_CLASS,
            None,
            None,
            Some(NonNull::new_unchecked(cell.cast())),
        )
    }
    .upcast::<Object>();

    // 5. Set map[objectaddr] to object.
    set_host_object(cell, object);

    // 6. Return object.
    object
}

/// The value of the object's [[ObjectAddress]] internal slot, if it has one and is therefore an Exported GC Object.
pub(crate) fn exported_gc_object_address(object: Gc<Object>) -> Option<u64> {
    if !is_host_instance_of(&object, &EXPORTED_GC_OBJECT_CLASS) {
        return None;
    }
    host_data_of(&object).map(|cell| cell.as_ptr() as u64)
}
