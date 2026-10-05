/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Objects, their properties and their internal methods.
//!
//! The exported functions of this file, and of the files that share its conversions, run on the thread that owns the
//! VM, and trust their arguments: `vm` is the embedder's VM, whose storage starts with the runtime's; objects, realms
//! and environments are live cells of its heap; a JSPropertyKey pointer points to a key that outlives the call; and
//! out parameters point to writable storage. Cells these functions return are not rooted: like every engine value,
//! they stay alive while the stack or a root reaches them.
//!
//! Completions follow the conventions of abi_types.rs, which are those of LibJS/HostObjectABI.h.

#![allow(
    clippy::missing_safety_doc,
    reason = "the module documentation states the contract every exported function shares"
)]

use core::ffi::c_void;

use crate::embedding::abi_types::{
    JSRealm, JSUtf16View, append_to_value_sink, cell_from_abi, cell_into_abi, completion_into_abi, object_into_abi,
    optional_cell_from_abi, optional_object_into_abi, property_key_from_abi, vm_from_abi,
};
use crate::embedding::collections::{JSPropertyKind, property_kind_from_abi};
use crate::embedding::function::{JSNativeFunction, raw_native_function_from_abi};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::host_class::{
    JS_PD_CONFIGURABLE, JS_PD_ENUMERABLE, JS_PD_HAS_CONFIGURABLE, JS_PD_HAS_ENUMERABLE, JS_PD_HAS_GET,
    JS_PD_HAS_PROPERTY_OFFSET, JS_PD_HAS_SET, JS_PD_HAS_VALUE, JS_PD_HAS_WRITABLE, JS_PD_PRESENT, JS_PD_WRITABLE,
    JS_PROPERTY_LOOKUP_PHASE_OWN_PROPERTY, JS_PROPERTY_LOOKUP_PHASE_PROTOTYPE_CHAIN, JSCompletion, JSGetCacheMetadata,
    JSObject, JSPropertyDescriptor, JSPropertyKey, JSSetCacheMetadata, JSVM, JSValue, JSValueSink,
};
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::object::{
    CacheableGetPropertyMetadata, CacheableSetPropertyMetadata, HostIntrinsicAccessor, IntegrityLevel, Object,
    PropertyLookupPhase, ShouldThrowExceptions,
};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

// Conversions between the C types and the runtime's own, which the other files of the embedding module share.

const _: () = assert!(size_of::<JSValue>() == size_of::<Value>());

/// # Safety
///
/// `function` must be a live function object.
pub(crate) unsafe fn function_from_abi(function: *mut JSObject) -> Gc<FunctionObject> {
    // SAFETY: The caller passes a live object.
    unsafe { cell_from_abi::<JSObject>(function) }
        .downcast::<FunctionObject>()
        .expect("the object is a function")
}

/// # Safety
///
/// `function` must be null or a live function object.
pub(crate) unsafe fn optional_function_from_abi(function: *mut JSObject) -> Option<Gc<FunctionObject>> {
    // SAFETY: The caller passes null or a live object.
    unsafe { optional_cell_from_abi::<JSObject>(function) }
        .map(|function| function.downcast::<FunctionObject>().expect("the object is a function"))
}

/// # Safety
///
/// `values` must point to `count` values, or `count` must be 0.
pub(crate) unsafe fn values_from_abi<'values>(values: *const JSValue, count: usize) -> &'values [Value] {
    if count == 0 {
        return &[];
    }
    // SAFETY: Value is a transparent JSValue, and the caller passes `count` of them.
    unsafe { core::slice::from_raw_parts(values.cast::<Value>(), count) }
}

const ABSENT_PROPERTY_DESCRIPTOR: JSPropertyDescriptor = JSPropertyDescriptor {
    value: 0,
    get: core::ptr::null_mut(),
    set: core::ptr::null_mut(),
    property_offset: 0,
    flags: 0,
};

pub(crate) fn property_descriptor_to_abi(descriptor: Option<&PropertyDescriptor>) -> JSPropertyDescriptor {
    let mut abi_descriptor = ABSENT_PROPERTY_DESCRIPTOR;
    let Some(descriptor) = descriptor else {
        return abi_descriptor;
    };
    let mut flags = JS_PD_PRESENT;
    if let Some(value) = descriptor.value {
        flags |= JS_PD_HAS_VALUE;
        abi_descriptor.value = value.0;
    }
    if let Some(getter) = descriptor.get {
        flags |= JS_PD_HAS_GET;
        abi_descriptor.get = optional_object_into_abi(getter);
    }
    if let Some(setter) = descriptor.set {
        flags |= JS_PD_HAS_SET;
        abi_descriptor.set = optional_object_into_abi(setter);
    }
    let boolean_field_flags = |field: Option<bool>, has_field_flag: u16, field_is_true_flag: u16| match field {
        Some(true) => has_field_flag | field_is_true_flag,
        Some(false) => has_field_flag,
        None => 0,
    };
    flags |= boolean_field_flags(descriptor.writable, JS_PD_HAS_WRITABLE, JS_PD_WRITABLE);
    flags |= boolean_field_flags(descriptor.enumerable, JS_PD_HAS_ENUMERABLE, JS_PD_ENUMERABLE);
    flags |= boolean_field_flags(descriptor.configurable, JS_PD_HAS_CONFIGURABLE, JS_PD_CONFIGURABLE);
    if let Some(property_offset) = descriptor.property_offset {
        flags |= JS_PD_HAS_PROPERTY_OFFSET;
        abi_descriptor.property_offset = property_offset;
    }
    abi_descriptor.flags = flags;
    abi_descriptor
}

/// # Safety
///
/// The getter and setter of the descriptor must be null or live function objects.
pub(crate) unsafe fn property_descriptor_from_abi(abi_descriptor: &JSPropertyDescriptor) -> Option<PropertyDescriptor> {
    let has = |flag: u16| abi_descriptor.flags & flag != 0;
    if !has(JS_PD_PRESENT) {
        return None;
    }
    // SAFETY: The caller passes null or live functions as the getter and setter.
    let accessor_function = |function: *mut JSObject| unsafe { optional_function_from_abi(function) };
    Some(PropertyDescriptor {
        value: has(JS_PD_HAS_VALUE).then_some(Value(abi_descriptor.value)),
        get: has(JS_PD_HAS_GET).then(|| accessor_function(abi_descriptor.get)),
        set: has(JS_PD_HAS_SET).then(|| accessor_function(abi_descriptor.set)),
        writable: has(JS_PD_HAS_WRITABLE).then_some(has(JS_PD_WRITABLE)),
        enumerable: has(JS_PD_HAS_ENUMERABLE).then_some(has(JS_PD_ENUMERABLE)),
        configurable: has(JS_PD_HAS_CONFIGURABLE).then_some(has(JS_PD_CONFIGURABLE)),
        property_offset: has(JS_PD_HAS_PROPERTY_OFFSET).then_some(abi_descriptor.property_offset),
    })
}

/// # Safety
///
/// `descriptor` must be null or point to a descriptor whose getter and setter are null or live function objects.
unsafe fn optional_property_descriptor_from_abi(descriptor: *const JSPropertyDescriptor) -> Option<PropertyDescriptor> {
    // SAFETY: The caller passes null or a valid descriptor.
    unsafe { descriptor.as_ref() }.and_then(|descriptor| {
        // SAFETY: As above.
        unsafe { property_descriptor_from_abi(descriptor) }
    })
}

pub(crate) fn lookup_phase_from_abi(phase: u8) -> PropertyLookupPhase {
    match phase {
        JS_PROPERTY_LOOKUP_PHASE_OWN_PROPERTY => PropertyLookupPhase::OwnProperty,
        JS_PROPERTY_LOOKUP_PHASE_PROTOTYPE_CHAIN => PropertyLookupPhase::PrototypeChain,
        phase => panic!("{phase} is not a property lookup phase"),
    }
}

/// # Safety
///
/// `metadata` must be null or point to inline cache metadata the runtime handed out for a [[Get]] still running.
unsafe fn get_cache_metadata_from_abi<'metadata>(
    metadata: *mut JSGetCacheMetadata,
) -> Option<&'metadata mut CacheableGetPropertyMetadata> {
    // SAFETY: The runtime passes its own metadata through the embedder unchanged, as the caller guarantees.
    unsafe { metadata.cast::<CacheableGetPropertyMetadata>().as_mut() }
}

/// # Safety
///
/// `metadata` must be null or point to inline cache metadata the runtime handed out for a [[Set]] still running.
pub(crate) unsafe fn set_cache_metadata_from_abi<'metadata>(
    metadata: *mut JSSetCacheMetadata,
) -> Option<&'metadata mut CacheableSetPropertyMetadata> {
    // SAFETY: The runtime passes its own metadata through the embedder unchanged, as the caller guarantees.
    unsafe { metadata.cast::<CacheableSetPropertyMetadata>().as_mut() }
}

/// Appends every key of a list of property keys to the embedder's sink. The list stays rooted while the sink runs,
/// and no borrow of the runtime's state is held across a call into the sink, which may call back into the VM.
///
/// # Safety
///
/// `sink` must point to a sink with an append function.
unsafe fn append_keys_to_sink(
    keys: ThrowCompletionOr<crate::gc::root::MarkedVec<'_, Value>>,
    sink: *mut JSValueSink,
) -> JSCompletion {
    let keys = match keys {
        Ok(keys) => keys,
        Err(throw) => return completion_into_abi::<()>(Err(throw)),
    };
    // SAFETY: The caller passes a valid sink.
    let sink = unsafe { &*sink };
    for index in 0..keys.len() {
        append_to_value_sink(sink, keys.get(index).expect("the index is in bounds"));
    }
    completion_into_abi(Ok(()))
}

/// The bits of the attributes byte of a property, JS::Attribute.
pub const JS_ATTRIBUTE_WRITABLE: u8 = 1 << 0;
pub const JS_ATTRIBUTE_ENUMERABLE: u8 = 1 << 1;
pub const JS_ATTRIBUTE_CONFIGURABLE: u8 = 1 << 2;

const _: () = assert!(JS_ATTRIBUTE_WRITABLE == Attribute::WRITABLE);
const _: () = assert!(JS_ATTRIBUTE_ENUMERABLE == Attribute::ENUMERABLE);
const _: () = assert!(JS_ATTRIBUTE_CONFIGURABLE == Attribute::CONFIGURABLE);

pub const JS_INTEGRITY_LEVEL_SEALED: u8 = 0;
pub const JS_INTEGRITY_LEVEL_FROZEN: u8 = 1;

fn integrity_level_from_abi(level: u8) -> IntegrityLevel {
    match level {
        JS_INTEGRITY_LEVEL_SEALED => IntegrityLevel::Sealed,
        JS_INTEGRITY_LEVEL_FROZEN => IntegrityLevel::Frozen,
        level => panic!("{level} is not an integrity level"),
    }
}

/// Computes the value of a property that an intrinsic accessor defines, when it is first read: given the realm of the
/// object, returns the value, which the runtime then stores in the property.
pub type JSIntrinsicAccessor = Option<unsafe extern "C" fn(realm: *mut JSRealm) -> JSValue>;

pub fn call_host_intrinsic_accessor(accessor: HostIntrinsicAccessor, realm: Gc<Realm>) -> Value {
    // SAFETY: The embedder defined the accessor for a property of an object of this runtime, and computes its value in
    //         the realm of that object.
    Value(unsafe { accessor(cell_into_abi::<JSRealm>(realm)) })
}

// Creating objects

/// OrdinaryObjectCreate: a new ordinary object of the realm whose [[Prototype]] is `prototype`, which may be null.
/// Returns an unrooted object. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_create(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    prototype: *mut JSObject,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm, prototype) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSRealm>(realm),
            optional_cell_from_abi::<JSObject>(prototype),
        )
    };
    object_into_abi(Object::create(vm, realm, prototype))
}

// Operations on objects

/// Get(O, P). Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_get(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    completion_into_abi(object.get(vm, key))
}

/// Set(O, P, V, Throw), with an unused payload. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_set(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    value: JSValue,
    throw_exceptions: bool,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    let throw_exceptions = if throw_exceptions {
        ShouldThrowExceptions::Yes
    } else {
        ShouldThrowExceptions::No
    };
    completion_into_abi(object.set(vm, key, Value(value), throw_exceptions))
}

/// HasProperty(O, P). Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_has_property(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    completion_into_abi(object.has_property(vm, key))
}

/// HasOwnProperty(O, P). Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_has_own_property(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    completion_into_abi(object.has_own_property(vm, key))
}

/// DefinePropertyOrThrow(O, P, desc), with an unused payload. The descriptor must be present, and the runtime writes
/// it back, which reports the offset of a new property. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_define_property_or_throw(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    descriptor: *mut JSPropertyDescriptor,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    // SAFETY: As above, the descriptor is valid.
    let mut property_descriptor =
        unsafe { property_descriptor_from_abi(&*descriptor) }.expect("the descriptor is present");
    let result = object.define_property_or_throw(vm, key, &mut property_descriptor);
    // SAFETY: As above.
    unsafe { descriptor.write(property_descriptor_to_abi(Some(&property_descriptor))) };
    completion_into_abi(result)
}

/// CreateDataProperty(O, P, V). Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_create_data_property(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    value: JSValue,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    completion_into_abi(object.create_data_property(vm, key, Value(value), None, None))
}

/// CreateDataPropertyOrThrow(O, P, V). Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_create_data_property_or_throw(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    value: JSValue,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    completion_into_abi(object.create_data_property_or_throw(vm, key, Value(value)))
}

/// SetIntegrityLevel(O, level), where the level is JS_INTEGRITY_LEVEL_SEALED or JS_INTEGRITY_LEVEL_FROZEN. Main
/// thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_set_integrity_level(
    vm: *mut JSVM,
    object: *mut JSObject,
    level: u8,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(object)) };
    completion_into_abi(object.set_integrity_level(vm, integrity_level_from_abi(level)))
}

/// SetImmutablePrototype(O, V), whose payload is whether the prototype is now `prototype`, which may be null. An
/// exotic object with an immutable prototype, such as Location, calls it from its set_prototype_of hook. Main thread
/// only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_set_immutable_prototype(
    vm: *mut JSVM,
    object: *mut JSObject,
    prototype: *mut JSObject,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, prototype) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            optional_cell_from_abi::<JSObject>(prototype),
        )
    };
    completion_into_abi(object.set_immutable_prototype(vm, prototype))
}

/// EnumerableOwnProperties(O, kind), with kind a JS_PROPERTY_KIND_* value: appends to the sink, in order, the key, the
/// value or a [key, value] array of each enumerable own string-keyed property, with an unused payload. The getters of
/// the properties run, and the sink may call back into the VM. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_enumerable_own_property_names(
    vm: *mut JSVM,
    object: *mut JSObject,
    kind: JSPropertyKind,
    sink: *mut JSValueSink,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(object)) };
    // SAFETY: As above, the sink is valid.
    unsafe {
        append_keys_to_sink(
            object.enumerable_own_property_names(vm, property_kind_from_abi(kind)),
            sink,
        )
    }
}

/// Called with the context it came with and each key EnumerateObjectProperties produces, a String value. It may run
/// JavaScript, and returns true to stop the enumeration.
pub type JSPropertyEnumerationCallback = Option<unsafe extern "C" fn(context: *mut c_void, key: JSValue) -> bool>;

/// EnumerateObjectProperties(O), the keys a for-in loop visits: calls `callback` with each string key of an enumerable
/// property of the object and of its prototype chain, each at most once, until the callback returns true. The payload
/// is whether the callback stopped the enumeration. A throw comes from an internal method of an object the
/// enumeration visits. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_enumerate_object_properties(
    vm: *mut JSVM,
    object: *mut JSObject,
    callback: JSPropertyEnumerationCallback,
    context: *mut c_void,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(object)) };
    let callback = callback.expect("the embedder passes a callback");
    let stopped = object.enumerate_object_properties(vm, |key| {
        // SAFETY: The embedder's callback takes keys with the context it came with.
        unsafe { callback(context, key.0) }.then_some(())
    });
    completion_into_abi(stopped.map(|stopped| stopped.is_some()))
}

/// The value of the property of the key, looked up in the storage of the object and of its prototype chain without
/// running any internal method or getter, or undefined if there is none. For an accessor property, it is the
/// accessor itself, a cell of kind JS_LAYOUT_CELL_KIND_ACCESSOR. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_get_without_side_effects(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
) -> JSValue {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    object.get_without_side_effects(vm, key).0
}

// Defining properties directly in the object's storage, as built-in objects do

/// Stores a data property in the object's own storage, replacing any property of the key, without running any
/// internal method. The attributes are JS_ATTRIBUTE_* bits. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_define_direct_property(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    value: JSValue,
    attributes: u8,
) {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    object.define_direct_property(vm, key, Value(value), PropertyAttributes::new(attributes));
}

/// Stores an accessor property in the object's own storage. Either function may be null; when the key already holds an
/// accessor, a non-null function replaces its getter or setter and the attributes are kept. Borrows the key. Main
/// thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_define_direct_accessor(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    getter: *mut JSObject,
    setter: *mut JSObject,
    attributes: u8,
) {
    // SAFETY: See the module documentation.
    let (vm, object, key, getter, setter) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
            optional_function_from_abi(getter),
            optional_function_from_abi(setter),
        )
    };
    object.define_direct_accessor(vm, key, getter, setter, PropertyAttributes::new(attributes));
}

/// js_object_define_direct_accessor() for an accessor whose getter result the runtime caches in the object until
/// js_object_clear_cached_accessor_value(). Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_define_direct_cached_accessor(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    getter: *mut JSObject,
    setter: *mut JSObject,
    attributes: u8,
) {
    // SAFETY: See the module documentation.
    let (vm, object, key, getter, setter) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
            optional_function_from_abi(getter),
            optional_function_from_abi(setter),
        )
    };
    object.define_direct_cached_accessor(vm, key, getter, setter, PropertyAttributes::new(attributes));
}

/// Forgets the getter result that a cached accessor of the key keeps, if any. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_clear_cached_accessor_value(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
) {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    object.clear_cached_accessor_value(vm, key);
}

/// Defines a method whose behaviour is a raw native function, as a direct property of the key with the attributes.
/// The function's [[Realm]] is the realm, and its "name" property is the key. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_define_native_function(
    vm: *mut JSVM,
    object: *mut JSObject,
    realm: *mut JSRealm,
    key: *const JSPropertyKey,
    behaviour: JSNativeFunction,
    length: i32,
    attributes: u8,
) {
    // SAFETY: See the module documentation.
    let (vm, object, realm, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            cell_from_abi::<JSRealm>(realm),
            property_key_from_abi(key),
        )
    };
    object.define_native_function(
        vm,
        realm,
        key,
        raw_native_function_from_abi(behaviour),
        length,
        PropertyAttributes::new(attributes),
        None,
    );
}

/// Defines an accessor property whose getter ("get <key>", length 0) and setter ("set <key>", length 1) are raw native
/// functions. Either may be null for a missing function. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_define_native_accessor(
    vm: *mut JSVM,
    object: *mut JSObject,
    realm: *mut JSRealm,
    key: *const JSPropertyKey,
    getter: JSNativeFunction,
    setter: JSNativeFunction,
    attributes: u8,
) {
    // SAFETY: See the module documentation.
    let (vm, object, realm, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            cell_from_abi::<JSRealm>(realm),
            property_key_from_abi(key),
        )
    };
    object.define_native_accessor(
        vm,
        realm,
        key,
        raw_native_function_from_abi(getter),
        raw_native_function_from_abi(setter),
        PropertyAttributes::new(attributes),
    );
}

/// Defines a data property of a string key whose value the accessor computes the first time anything reads it.
/// Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_define_intrinsic_accessor(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    attributes: u8,
    accessor: JSIntrinsicAccessor,
) {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    object.define_host_intrinsic_accessor(
        vm,
        key,
        PropertyAttributes::new(attributes),
        accessor.expect("the intrinsic accessor is not null"),
    );
}

/// Records a property of a web platform interface that the embedder does not implement, so that reading it can be
/// reported without making it observable to JavaScript. Borrows the name. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_define_unimplemented_property(
    vm: *mut JSVM,
    object: *mut JSObject,
    name: JSUtf16View,
) {
    // SAFETY: See the module documentation.
    let (vm, object, name) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(object), name.as_view()) };
    object.define_unimplemented_property(vm, name.to_utf16_fly_string());
}

// The object's shape and storage

/// Sets the [[Prototype]] the object's shape records, without running [[SetPrototypeOf]]. The prototype may be null.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_set_prototype(vm: *mut JSVM, object: *mut JSObject, prototype: *mut JSObject) {
    // SAFETY: See the module documentation.
    let (vm, object, prototype) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            optional_cell_from_abi::<JSObject>(prototype),
        )
    };
    object.set_prototype(vm, prototype);
}

/// Whether the object's own storage has a property of the key, without running any internal method. Borrows the key.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_storage_has(object: *mut JSObject, key: *const JSPropertyKey) -> bool {
    // SAFETY: See the module documentation.
    let (object, key) = unsafe { (cell_from_abi::<JSObject>(object), property_key_from_abi(key)) };
    object.storage_has(key)
}

/// Removes the property of the key from the object's own storage, which must have it, without running any internal
/// method. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_storage_delete(vm: *mut JSVM, object: *mut JSObject, key: *const JSPropertyKey) {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    object.storage_delete(vm, key);
}

/// Gives the object a shape of its own suited to an object that serves as a prototype, unless it has one. Main thread
/// only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_convert_to_prototype_if_needed(vm: *mut JSVM, object: *mut JSObject) {
    // SAFETY: See the module documentation.
    let (vm, object) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(object)) };
    object.convert_to_prototype_if_needed(vm);
}

/// Gives the object a dictionary shape of its own, which no inline cache has seen, so that every cached lookup of its
/// properties misses and looks again. An embedder calls it when a property that its hooks report appears without the
/// object's shape changing, as a document's named properties do. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_invalidate_property_lookup_caches(vm: *mut JSVM, object: *mut JSObject) {
    // SAFETY: See the module documentation.
    let (vm, object) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(object)) };
    object.invalidate_property_lookup_caches(vm);
}

/// Lets new own properties of the object be added through the inline caches again, which its host class's
/// JS_HOST_CLASS_REQUIRES_SLOW_ADD_OWN_PROPERTY flag kept from it. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_clear_requires_slow_add_own_property(object: *mut JSObject) {
    // SAFETY: See the module documentation.
    unsafe { cell_from_abi::<JSObject>(object) }.clear_requires_slow_add_own_property();
}

/// Whether the object is an arguments object with a parameter map, as a mapped arguments object is. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_has_parameter_map(object: *mut JSObject) -> bool {
    // SAFETY: See the module documentation.
    unsafe { cell_from_abi::<JSObject>(object) }.has_parameter_map()
}

/// The name of the object's class, which for an object of a host class is the name in its JSHostClass: static UTF-8,
/// not null-terminated, whose length goes to `out_length`. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_class_name(object: *mut JSObject, out_length: *mut usize) -> *const u8 {
    // SAFETY: See the module documentation.
    let class_name = unsafe { cell_from_abi::<JSObject>(object) }.class().class_name();
    // SAFETY: As above, the out parameter is writable.
    unsafe { out_length.write(class_name.len()) };
    class_name.as_ptr()
}

// The engine queries that inline caches and enumeration ask of an object's class

/// Whether enumeration may read the object's own keys, attributes and values straight from its shape and storage.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_eligible_for_own_property_enumeration_fast_path(object: *mut JSObject) -> bool {
    // SAFETY: See the module documentation.
    unsafe { cell_from_abi::<JSObject>(object) }.eligible_for_own_property_enumeration_fast_path()
}

// The internal methods, which dispatch through the object's class like a call through a C++ Object pointer

/// [[GetPrototypeOf]] ( ), whose payload is the prototype or null. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_internal_get_prototype_of(vm: *mut JSVM, object: *mut JSObject) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(object)) };
    completion_into_abi(object.internal_get_prototype_of(vm))
}

/// [[SetPrototypeOf]] ( V ), where V may be null. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_internal_set_prototype_of(
    vm: *mut JSVM,
    object: *mut JSObject,
    prototype: *mut JSObject,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, prototype) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            optional_cell_from_abi::<JSObject>(prototype),
        )
    };
    completion_into_abi(object.internal_set_prototype_of(vm, prototype))
}

/// [[GetOwnProperty]] ( P ), with an unused payload. The runtime writes the property's descriptor, with its storage
/// offset when it has one, or a zeroed descriptor for an absent property to `out`. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_internal_get_own_property(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    out: *mut JSPropertyDescriptor,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    let descriptor = object.internal_get_own_property(vm, key);
    // SAFETY: As above, `out` is writable.
    unsafe { write_own_property_descriptor(descriptor, out) }
}

/// [[DefineOwnProperty]] ( P, Desc ). The descriptor must be present, and the runtime writes it back, which reports
/// the storage offset of a new property. A non-null `precomputed_get_own_property` is the result of a
/// [[GetOwnProperty]] the caller already ran, absent when JS_PD_PRESENT is clear. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_internal_define_own_property(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    descriptor: *mut JSPropertyDescriptor,
    precomputed_get_own_property: *const JSPropertyDescriptor,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    unsafe {
        define_own_property_from_abi(
            vm,
            object,
            key,
            descriptor,
            precomputed_get_own_property,
            Object::internal_define_own_property,
        )
    }
}

/// [[Get]] ( P, Receiver ). `cache_metadata` is null or the metadata a [[Get]] hook received, and the phase a
/// JS_PROPERTY_LOOKUP_PHASE_* value. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_internal_get(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    receiver: JSValue,
    cache_metadata: *mut JSGetCacheMetadata,
    phase: u8,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key, cache_metadata) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
            get_cache_metadata_from_abi(cache_metadata),
        )
    };
    completion_into_abi(object.internal_get(vm, key, Value(receiver), cache_metadata, lookup_phase_from_abi(phase)))
}

/// [[Get]] ( P, Receiver ) of the object as one found in the prototype chain of the lookup that
/// `metadata_for_caller` (null or the metadata a [[Get]] hook received) belongs to. The runtime fills that metadata
/// only for a hit an inline cache can keep, so an object that forwards its lookups to another one can let them be
/// cached. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_internal_get_as_prototype_of(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    receiver: JSValue,
    metadata_for_caller: *mut JSGetCacheMetadata,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key, metadata_for_caller) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
            get_cache_metadata_from_abi(metadata_for_caller),
        )
    };
    completion_into_abi(object.internal_get_as_prototype_of(vm, key, Value(receiver), metadata_for_caller))
}

/// [[Set]] ( P, V, Receiver ). `cache_metadata` is null or the metadata a [[Set]] hook received, and the phase a
/// JS_PROPERTY_LOOKUP_PHASE_* value. Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_internal_set(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    value: JSValue,
    receiver: JSValue,
    cache_metadata: *mut JSSetCacheMetadata,
    phase: u8,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key, cache_metadata) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
            set_cache_metadata_from_abi(cache_metadata),
        )
    };
    completion_into_abi(object.internal_set(
        vm,
        key,
        Value(value),
        Value(receiver),
        cache_metadata,
        lookup_phase_from_abi(phase),
    ))
}

/// [[Delete]] ( P ). Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_internal_delete(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    completion_into_abi(object.internal_delete(vm, key))
}

/// [[OwnPropertyKeys]] ( ), with an unused payload. The runtime appends each key, as a string or symbol value, to the
/// sink, which may call back into the VM. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_internal_own_property_keys(
    vm: *mut JSVM,
    object: *mut JSObject,
    keys: *mut JSValueSink,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(object)) };
    // SAFETY: As above, the sink is valid.
    unsafe { append_keys_to_sink(object.internal_own_property_keys(vm), keys) }
}

// The ordinary internal methods, which never dispatch to the object's class, for host classes whose hooks defer to them

/// OrdinaryGetPrototypeOf ( O ), whose payload is the prototype or null. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_ordinary_get_prototype_of(vm: *mut JSVM, object: *mut JSObject) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(object)) };
    completion_into_abi(object.ordinary_get_prototype_of(vm))
}

/// OrdinarySetPrototypeOf ( O, V ), where V may be null. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_ordinary_set_prototype_of(
    vm: *mut JSVM,
    object: *mut JSObject,
    prototype: *mut JSObject,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, prototype) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            optional_cell_from_abi::<JSObject>(prototype),
        )
    };
    completion_into_abi(object.ordinary_set_prototype_of(vm, prototype))
}

/// OrdinaryPreventExtensions ( O ). Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_ordinary_prevent_extensions(vm: *mut JSVM, object: *mut JSObject) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(object)) };
    completion_into_abi(object.ordinary_prevent_extensions(vm))
}

/// OrdinaryGetOwnProperty ( O, P ), which writes to `out` like js_object_internal_get_own_property(). Borrows the key.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_ordinary_get_own_property(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    out: *mut JSPropertyDescriptor,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    let descriptor = object.ordinary_get_own_property(vm, key);
    // SAFETY: As above, `out` is writable.
    unsafe { write_own_property_descriptor(descriptor, out) }
}

/// OrdinaryDefineOwnProperty ( O, P, Desc ), which takes its descriptors like
/// js_object_internal_define_own_property(). Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_ordinary_define_own_property(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    descriptor: *mut JSPropertyDescriptor,
    precomputed_get_own_property: *const JSPropertyDescriptor,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    unsafe {
        define_own_property_from_abi(
            vm,
            object,
            key,
            descriptor,
            precomputed_get_own_property,
            Object::ordinary_define_own_property,
        )
    }
}

/// OrdinaryGet ( O, P, Receiver ), which takes its cache metadata and phase like js_object_internal_get(). Borrows
/// the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_ordinary_get(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    receiver: JSValue,
    cache_metadata: *mut JSGetCacheMetadata,
    phase: u8,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key, cache_metadata) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
            get_cache_metadata_from_abi(cache_metadata),
        )
    };
    completion_into_abi(object.ordinary_get(vm, key, Value(receiver), cache_metadata, lookup_phase_from_abi(phase)))
}

/// OrdinarySet ( O, P, V, Receiver ), which takes its cache metadata and phase like js_object_internal_set(). Borrows
/// the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_ordinary_set(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    value: JSValue,
    receiver: JSValue,
    cache_metadata: *mut JSSetCacheMetadata,
    phase: u8,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key, cache_metadata) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
            set_cache_metadata_from_abi(cache_metadata),
        )
    };
    completion_into_abi(object.ordinary_set(
        vm,
        key,
        Value(value),
        Value(receiver),
        cache_metadata,
        lookup_phase_from_abi(phase),
    ))
}

/// OrdinarySetWithOwnDescriptor ( O, P, V, Receiver, ownDesc ), where ownDesc is absent when the pointer is null or
/// JS_PD_PRESENT is clear. Takes its cache metadata and phase like js_object_internal_set(). Borrows the key. Main
/// thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_ordinary_set_with_own_descriptor(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    value: JSValue,
    receiver: JSValue,
    own_descriptor: *const JSPropertyDescriptor,
    cache_metadata: *mut JSSetCacheMetadata,
    phase: u8,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key, own_descriptor, cache_metadata) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
            optional_property_descriptor_from_abi(own_descriptor),
            set_cache_metadata_from_abi(cache_metadata),
        )
    };
    completion_into_abi(object.ordinary_set_with_own_descriptor(
        vm,
        key,
        Value(value),
        Value(receiver),
        own_descriptor,
        cache_metadata,
        lookup_phase_from_abi(phase),
    ))
}

/// OrdinaryDelete ( O, P ). Borrows the key. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_ordinary_delete(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    completion_into_abi(object.ordinary_delete(vm, key))
}

/// OrdinaryOwnPropertyKeys ( O ), which appends to the sink like js_object_internal_own_property_keys(). Main thread
/// only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_object_ordinary_own_property_keys(
    vm: *mut JSVM,
    object: *mut JSObject,
    keys: *mut JSValueSink,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, object) = unsafe { (vm_from_abi(vm), cell_from_abi::<JSObject>(object)) };
    // SAFETY: As above, the sink is valid.
    unsafe { append_keys_to_sink(object.ordinary_own_property_keys(vm), keys) }
}

/// # Safety
///
/// `out` must be writable.
unsafe fn write_own_property_descriptor(
    descriptor: ThrowCompletionOr<Option<PropertyDescriptor>>,
    out: *mut JSPropertyDescriptor,
) -> JSCompletion {
    match descriptor {
        Ok(descriptor) => {
            // SAFETY: The caller passes writable storage.
            unsafe { out.write(property_descriptor_to_abi(descriptor.as_ref())) };
            completion_into_abi(Ok(()))
        }
        Err(throw) => completion_into_abi::<()>(Err(throw)),
    }
}

type DefineOwnProperty = fn(
    &Object,
    &Vm,
    &PropertyKey,
    &mut PropertyDescriptor,
    Option<&Option<PropertyDescriptor>>,
) -> ThrowCompletionOr<bool>;

/// # Safety
///
/// See the module documentation. `descriptor` must point to a present descriptor.
unsafe fn define_own_property_from_abi(
    vm: *mut JSVM,
    object: *mut JSObject,
    key: *const JSPropertyKey,
    descriptor: *mut JSPropertyDescriptor,
    precomputed_get_own_property: *const JSPropertyDescriptor,
    define_own_property: DefineOwnProperty,
) -> JSCompletion {
    // SAFETY: The caller passes valid arguments.
    let (vm, object, key) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(object),
            property_key_from_abi(key),
        )
    };
    // SAFETY: As above, the descriptor is valid and present.
    let mut property_descriptor =
        unsafe { property_descriptor_from_abi(&*descriptor) }.expect("the descriptor is present");
    // SAFETY: As above, the precomputed descriptor is null or valid.
    let precomputed_get_own_property = unsafe { precomputed_get_own_property.as_ref() }.map(|precomputed| {
        // SAFETY: As above.
        unsafe { property_descriptor_from_abi(precomputed) }
    });
    let result = define_own_property(
        &object,
        vm,
        key,
        &mut property_descriptor,
        precomputed_get_own_property.as_ref(),
    );
    // SAFETY: As above, the descriptor is writable.
    unsafe { descriptor.write(property_descriptor_to_abi(Some(&property_descriptor))) };
    completion_into_abi(result)
}
