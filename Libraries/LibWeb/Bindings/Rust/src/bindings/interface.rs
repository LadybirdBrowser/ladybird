/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the generated bindings of an interface do with its wrappers, its interface object and its interface prototype
//! object, https://webidl.spec.whatwg.org/#es-interfaces
//!
//! A wrapper of an interface implemented in Rust is a platform object of the interface's wrapper class, which LibWeb's
//! C++ bindings define. Its host data holds the cell of its implementation, and its wrappable slot stays empty, so the
//! C++ bindings treat it like a platform object without a wrappable.

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{
    Attribute, EmbedderCell, EmbedderClass, EmbedderValue, ErrorKind, ErrorType, FunctionObject, Gc, HostObject,
    JSHostClass, Object, PrimitiveString, PropertyAttributes, PropertyKey, RawNativeFunctionPointer, Realm,
    ThrowCompletionOr, Value, Vm, get_function_realm, host_class_of, host_data_of,
};

use crate::bindings::CallContext;
use crate::bindings::services::web_bindings_ensure_web_prototype;

/// The object an attribute or an operation of an interface runs on, if it is a wrapper of the interface, with the
/// object's realm. A nullish this value stands for the global object of the current realm.
pub(crate) fn this_object(
    vm: &Vm,
    wrapper_class: &'static JSHostClass,
    interface_name: &str,
) -> ThrowCompletionOr<(Gc<Object>, Gc<Realm>)> {
    let mut this_value = vm.this_value();
    if this_value.is_nullish() {
        let current_realm = vm.current_realm().expect("a native function runs in a realm");
        this_value = Value::from_object(current_realm.global_object());
    }
    if this_value.is_object() {
        let object = this_value.as_object();
        if host_class_of(&object).is_some_and(|host_class| core::ptr::eq(host_class, wrapper_class)) {
            return Ok((object, object.shape().realm()));
        }
    }
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&interface_name])
}

pub(crate) fn implementation_of<T: EmbedderValue>(
    wrapper: Gc<Object>,
    cell_class: &'static EmbedderClass<T>,
) -> Gc<EmbedderCell<T>> {
    let host_data = host_data_of(&wrapper).expect("a wrapper holds the cell of its implementation");
    // SAFETY: The wrapper is a host object of the interface's wrapper class, whose host data the bindings set to a
    //         cell of the interface's implementation.
    unsafe { cell_class.downcast(host_data) }.expect("a wrapper holds the cell of its implementation")
}

/// The interface prototype object of the interface in the realm.
pub(crate) fn interface_prototype_object(realm: Gc<Realm>, interface_name: u16) -> Gc<Object> {
    // SAFETY: The realm is live, and the interface is implemented in Rust.
    let prototype = unsafe { web_bindings_ensure_web_prototype(realm.as_ptr().cast(), interface_name) };
    let prototype = NonNull::new(prototype.cast::<Object>()).expect("the realm has the interface prototype object");
    // SAFETY: LibWeb returns the live prototype.
    unsafe { Gc::from_non_null(prototype) }
}

/// The prototype of an object that newTarget constructs, steps 3.2 and 3.3 of internally creating a new object
/// implementing the interface, https://webidl.spec.whatwg.org/#internally-create-a-new-object-implementing-the-interface
pub(crate) fn prototype_from_new_target(
    vm: &Vm,
    new_target: Gc<Object>,
    interface_name: u16,
) -> ThrowCompletionOr<Gc<Object>> {
    // 3.2. Let prototype be ? Get(newTarget, "prototype").
    let prototype = new_target.get(vm, &vm.names.prototype)?;

    // 3.3. If Type(prototype) is not Object, then:
    if prototype.is_object() {
        return Ok(prototype.as_object());
    }
    // 1. Let targetRealm be ? GetFunctionRealm(newTarget).
    let new_target = new_target
        .downcast::<FunctionObject>()
        .expect("the new target of a construction is a constructor");
    let target_realm = get_function_realm(vm, new_target)?;
    // 2. Set prototype to the interface prototype object for interface in targetRealm.
    Ok(interface_prototype_object(target_realm, interface_name))
}

/// A new wrapper of the interface in the realm, whose host data holds the implementation.
pub(crate) fn create_wrapper<T: EmbedderValue>(
    vm: &Vm,
    realm: Gc<Realm>,
    wrapper_class: &'static JSHostClass,
    prototype: Gc<Object>,
    implementation: Gc<EmbedderCell<T>>,
) -> Gc<HostObject> {
    // SAFETY: The wrapper class's hooks expect no wrappable, and the host data is a live cell.
    unsafe {
        HostObject::create(
            vm,
            realm,
            wrapper_class,
            Some(prototype),
            None,
            Some(implementation.as_non_null().cast()),
        )
    }
}

/// Unwraps the raw arguments that LibWeb's C++ bindings pass the entry points that initialize the interface object and
/// the interface prototype object, which run before any binding of the interface. The C++ bindings pass the runtime's
/// VM and cells, as JS::HostABI::vm_to_abi() and object_to_abi() do.
///
/// # Safety
///
/// The pointers must be the live VM, a live realm and a live object.
pub(crate) unsafe fn entry_point_arguments<'vm>(
    vm: *mut c_void,
    realm: *mut c_void,
    object: *mut c_void,
) -> (&'vm Vm, Gc<Realm>, Gc<Object>) {
    let realm = NonNull::new(realm.cast::<Realm>()).expect("C++ passes a realm");
    let object = NonNull::new(object.cast::<Object>()).expect("C++ passes an object");
    // SAFETY: The caller guarantees that the pointers are live.
    unsafe { (&*vm.cast::<Vm>(), Gc::from_non_null(realm), Gc::from_non_null(object)) }
}

/// Unwraps the raw arguments that LibWeb's C++ bindings pass the entry point that constructs an object.
///
/// # Safety
///
/// The pointers must be the live VM and the live new target of a construction.
pub(crate) unsafe fn construct_arguments<'vm>(vm: *mut c_void, new_target: *mut c_void) -> (&'vm Vm, Gc<Object>) {
    let new_target = NonNull::new(new_target.cast::<Object>()).expect("a construction has a new target");
    // SAFETY: The caller guarantees that the pointers are live.
    unsafe { (&*vm.cast::<Vm>(), Gc::from_non_null(new_target)) }
}

/// Gives the interface prototype object its [[Prototype]], %Object.prototype%. The Rust bindings do not handle
/// interfaces that inherit from another interface yet.
pub(crate) fn set_interface_prototype_object_prototype(vm: &Vm, realm: Gc<Realm>, prototype: Gc<Object>) {
    prototype.set_prototype(vm, Some(realm.intrinsics().object_prototype(vm)));
}

/// Defines a regular attribute on the interface prototype object, https://webidl.spec.whatwg.org/#es-attributes
pub(crate) fn define_attribute(
    vm: &Vm,
    realm: Gc<Realm>,
    prototype: Gc<Object>,
    name: &str,
    getter: RawNativeFunctionPointer,
    setter: RawNativeFunctionPointer,
) {
    prototype.define_native_accessor(
        vm,
        realm,
        &PropertyKey::from_utf8(name),
        getter,
        setter,
        PropertyAttributes::new(Attribute::ENUMERABLE | Attribute::CONFIGURABLE),
    );
}

/// Defines a regular operation on the interface prototype object, https://webidl.spec.whatwg.org/#es-operations
pub(crate) fn define_operation(
    vm: &Vm,
    realm: Gc<Realm>,
    prototype: Gc<Object>,
    name: &str,
    behaviour: RawNativeFunctionPointer,
    length: i32,
) {
    prototype.define_native_function(
        vm,
        realm,
        &PropertyKey::from_utf8(name),
        behaviour,
        length,
        PropertyAttributes::new(Attribute::WRITABLE | Attribute::ENUMERABLE | Attribute::CONFIGURABLE),
        None,
    );
}

/// Defines @@toStringTag on the interface prototype object, https://webidl.spec.whatwg.org/#es-interface-prototype-object
pub(crate) fn define_to_string_tag(vm: &Vm, prototype: Gc<Object>, name: &str) {
    prototype.define_direct_property(
        vm,
        &PropertyKey::from(vm.well_known_symbols().to_string_tag),
        ascii_string(vm, name),
        PropertyAttributes::new(Attribute::CONFIGURABLE),
    );
}

/// Defines the "length", "name" and "prototype" properties of the interface object,
/// https://webidl.spec.whatwg.org/#es-interface-call
pub(crate) fn define_interface_object_properties(
    vm: &Vm,
    realm: Gc<Realm>,
    constructor: Gc<Object>,
    name: &str,
    length: i32,
    interface_name: u16,
) {
    constructor.define_direct_property(
        vm,
        &vm.names.length,
        Value::from_f64(f64::from(length)),
        PropertyAttributes::new(Attribute::CONFIGURABLE),
    );
    constructor.define_direct_property(
        vm,
        &vm.names.name,
        ascii_string(vm, name),
        PropertyAttributes::new(Attribute::CONFIGURABLE),
    );
    let prototype = interface_prototype_object(realm, interface_name);
    constructor.define_direct_property(
        vm,
        &vm.names.prototype,
        Value::from_object(prototype),
        PropertyAttributes::new(0),
    );
}

fn ascii_string(vm: &Vm, string: &str) -> Value {
    Value::from_string(PrimitiveString::create_from_ascii(vm, string.as_bytes()))
}

/// A new object for a dictionary value, https://webidl.spec.whatwg.org/#js-dictionary
pub(crate) fn create_dictionary_object(cx: &CallContext) -> Gc<Object> {
    // 1. Let O be OrdinaryObjectCreate(%Object.prototype%).
    let object_prototype = cx.realm().intrinsics().object_prototype(cx.vm());
    Object::create(cx.vm(), cx.realm(), Some(object_prototype))
}

/// Stores a member of a dictionary value in its object, step 3.2.3 of https://webidl.spec.whatwg.org/#js-dictionary
pub(crate) fn set_dictionary_member(
    cx: &CallContext,
    object: Gc<Object>,
    key: &PropertyKey,
    value: Value,
) -> ThrowCompletionOr<()> {
    // 3. Perform ! CreateDataPropertyOrThrow(O, key, value).
    object.create_data_property(cx.vm(), key, value, None, None)?;
    Ok(())
}
