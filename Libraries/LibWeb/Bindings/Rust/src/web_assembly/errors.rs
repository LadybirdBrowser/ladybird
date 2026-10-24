/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The error types of the namespace, https://webassembly.github.io/spec/js-api/#error-objects: objects implementing
//! the NativeError Object Structure, https://tc39.es/ecma262/#sec-nativeerror-object-structure, whose prototypes and
//! constructors every realm has its own of.

use core::ffi::c_void;

use ak::{Utf16FlyString, Utf16String};
use libjs_rust::api::{
    Attribute, Error, Gc, HostFunction, JS_HOST_CLASS_FUNCTION, JS_HOST_CLASS_HAS_CONSTRUCTOR, JSCompletion,
    JSHostClass, JSHostFunctionHooks, JSObject, JSVM, Object, PrimitiveString, PropertyAttributes, Realm, Throw,
    ThrowCompletionOr, Value, Vm, completion_into_abi, get_function_realm, object_from_abi, vm_from_abi,
};

use crate::bindings::realm_data::realm_data;

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum WebAssemblyError {
    Compile,
    Link,
    Runtime,
}

impl WebAssemblyError {
    pub(crate) const ALL: [Self; 3] = [Self::Compile, Self::Link, Self::Runtime];

    pub(crate) fn name(self) -> &'static str {
        match self {
            Self::Compile => "CompileError",
            Self::Link => "LinkError",
            Self::Runtime => "RuntimeError",
        }
    }

    fn index(self) -> usize {
        self as usize
    }

    const fn of_index(index: usize) -> Self {
        match index {
            0 => Self::Compile,
            1 => Self::Link,
            _ => Self::Runtime,
        }
    }
}

const fn constructor_hooks<const ERROR: usize>() -> JSHostFunctionHooks {
    JSHostFunctionHooks {
        call: Some(call_native_error_constructor::<ERROR>),
        construct: Some(construct_native_error::<ERROR>),
        finalize: None,
    }
}

static CONSTRUCTOR_HOOKS: [JSHostFunctionHooks; 3] = [
    constructor_hooks::<{ WebAssemblyError::Compile as usize }>(),
    constructor_hooks::<{ WebAssemblyError::Link as usize }>(),
    constructor_hooks::<{ WebAssemblyError::Runtime as usize }>(),
];

static CONSTRUCTOR_CLASSES: [JSHostClass; 3] = [
    JSHostClass::new(
        JS_HOST_CLASS_FUNCTION,
        "CompileErrorConstructor",
        None,
        (&raw const CONSTRUCTOR_HOOKS[0]).cast(),
        JS_HOST_CLASS_HAS_CONSTRUCTOR,
    ),
    JSHostClass::new(
        JS_HOST_CLASS_FUNCTION,
        "LinkErrorConstructor",
        None,
        (&raw const CONSTRUCTOR_HOOKS[1]).cast(),
        JS_HOST_CLASS_HAS_CONSTRUCTOR,
    ),
    JSHostClass::new(
        JS_HOST_CLASS_FUNCTION,
        "RuntimeErrorConstructor",
        None,
        (&raw const CONSTRUCTOR_HOOKS[2]).cast(),
        JS_HOST_CLASS_HAS_CONSTRUCTOR,
    ),
];

/// The NativeError prototype and constructor of the error type in the realm, which the first use in the realm
/// creates.
fn prototype_and_constructor(vm: &Vm, realm: Gc<Realm>, error: WebAssemblyError) -> (Gc<Object>, Gc<Object>) {
    let data = realm_data(vm, realm);
    let index = error.index();
    if let (Some(prototype), Some(constructor)) = (
        data.webassembly_error_prototypes[index].get(),
        data.webassembly_error_constructors[index].get(),
    ) {
        return (prototype, constructor);
    }

    let name = Value::from_string(PrimitiveString::create_from_utf8(vm, error.name()));
    let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);

    // 20.5.6.3 Properties of the NativeError Prototype Objects, https://tc39.es/ecma262/#sec-properties-of-the-nativeerror-prototype-objects
    let prototype = Object::create(vm, realm, Some(realm.intrinsics().error_prototype(vm)));
    prototype.define_direct_property(vm, &vm.names.name, name, attributes);
    let empty = Value::from_string(PrimitiveString::create(vm, Utf16String::default()));
    prototype.define_direct_property(vm, &vm.names.message, empty, attributes);

    // 20.5.6.2 Properties of the NativeError Constructors, https://tc39.es/ecma262/#sec-properties-of-the-nativeerror-constructors
    // SAFETY: The class is of kind JS_HOST_CLASS_FUNCTION, and the constructor has no host data.
    let constructor = unsafe {
        HostFunction::create_without_own_properties(
            vm,
            realm,
            &CONSTRUCTOR_CLASSES[index],
            Utf16FlyString::from_utf8(error.name()),
            Some(realm.intrinsics().error_constructor(vm).upcast()),
            None,
        )
    }
    .upcast::<Object>();
    constructor.define_direct_property(
        vm,
        &vm.names.prototype,
        Value::from_object(prototype),
        PropertyAttributes::new(0),
    );
    let configurable = PropertyAttributes::new(Attribute::CONFIGURABLE);
    constructor.define_direct_property(vm, &vm.names.name, name, configurable);
    constructor.define_direct_property(vm, &vm.names.length, Value::from_i32(1), configurable);

    prototype.define_direct_property(vm, &vm.names.constructor, Value::from_object(constructor), attributes);

    data.webassembly_error_prototypes[index].set(Some(prototype));
    data.webassembly_error_constructors[index].set(Some(constructor));
    (prototype, constructor)
}

/// The constructor of the error type in the realm.
pub(crate) fn error_constructor(vm: &Vm, realm: Gc<Realm>, error: WebAssemblyError) -> Gc<Object> {
    prototype_and_constructor(vm, realm, error).1
}

/// A new error of the type in the realm, with the message.
pub(crate) fn create_webassembly_error(vm: &Vm, realm: Gc<Realm>, error: WebAssemblyError, message: &str) -> Gc<Error> {
    let (prototype, _) = prototype_and_constructor(vm, realm, error);
    let object = Error::create_with_prototype(vm, realm, prototype);
    object.set_message(vm, Utf16String::from_utf8(message));
    object
}

/// What throwing a new error of the type in the realm, with the message, throws.
pub(crate) fn throw_webassembly_error(vm: &Vm, realm: Gc<Realm>, error: WebAssemblyError, message: &str) -> Throw {
    Throw::new(Value::from_object(
        create_webassembly_error(vm, realm, error, message).upcast::<Object>(),
    ))
}

// 20.5.6.1.1 NativeError ( message [ , options ] ), https://tc39.es/ecma262/#sec-nativeerror
unsafe extern "C" fn call_native_error_constructor<const ERROR: usize>(
    function: *mut JSObject,
    vm: *mut JSVM,
) -> JSCompletion {
    // 1. If NewTarget is undefined, let newTarget be the active function object; else let newTarget be NewTarget.
    // SAFETY: The runtime passes the live function and the VM.
    unsafe { construct_native_error::<ERROR>(function, vm, function) }
}

// 20.5.6.1.1 NativeError ( message [ , options ] ), https://tc39.es/ecma262/#sec-nativeerror
unsafe extern "C" fn construct_native_error<const ERROR: usize>(
    _function: *mut JSObject,
    vm: *mut JSVM,
    new_target: *mut JSObject,
) -> JSCompletion {
    // SAFETY: The runtime passes the VM and the live new target.
    let (vm, new_target) = unsafe { (vm_from_abi(vm), object_from_abi(new_target)) };
    completion_into_abi(native_error(vm, new_target, WebAssemblyError::of_index(ERROR)).map(Value::from_object))
}

fn native_error(vm: &Vm, new_target: Gc<Object>, error: WebAssemblyError) -> ThrowCompletionOr<Gc<Object>> {
    let message = vm.argument(0);
    let options = vm.argument(1);

    // 2. Let O be ? OrdinaryCreateFromConstructor(newTarget, "%NativeError.prototype%", « [[ErrorData]] »).
    // NB: The intrinsic default prototype is that of the error type in the realm of newTarget, which the realm's data
    //     keeps, not its intrinsics.
    // https://tc39.es/ecma262/#sec-getprototypefromconstructor
    // 2. Let proto be ? Get(constructor, "prototype").
    let mut prototype = new_target.get(vm, &vm.names.prototype)?;

    // 3. If Type(proto) is not Object, then
    if !prototype.is_object() {
        // a. Let realm be ? GetFunctionRealm(constructor).
        let realm = get_function_realm(
            vm,
            new_target
                .downcast::<libjs_rust::api::FunctionObject>()
                .expect("the new target of a construction is a function"),
        )?;

        // b. Set proto to realm's intrinsic object named intrinsicDefaultProto.
        prototype = Value::from_object(prototype_and_constructor(vm, realm, error).0);
    }
    let realm = vm.current_realm().expect("a constructor runs in a realm");
    let object = Error::create_with_prototype(vm, realm, prototype.as_object());

    // 3. If message is not undefined, then
    if !message.is_undefined() {
        // a. Let msg be ? ToString(message).
        let message = message.to_utf16_string(vm)?;

        // b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "message", msg).
        object.create_non_enumerable_data_property_or_throw(
            vm,
            &vm.names.message,
            Value::from_string(PrimitiveString::create(vm, message)),
        );
    }

    // 4. Perform ? InstallErrorCause(O, options).
    object.install_error_cause(vm, options)?;

    // 5. Return O.
    Ok(object.upcast())
}

/// A CompileError with the message in the realm, for C++ code that throws one: the CSP check of
/// HostEnsureCanCompileWasmBytes.
///
/// # Safety
///
/// The pointers must be the VM, a live realm, and the message, a borrowed AK::Utf16String.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_web_assembly_create_compile_error(
    vm: *mut c_void,
    realm: *mut c_void,
    message: usize,
) -> *mut c_void {
    // SAFETY: The caller passes the VM and a live realm.
    let (vm, realm) = unsafe { crate::bindings::interface::realm_arguments(vm, realm) };
    // SAFETY: The caller lends the message for the call, which this takes a reference of its own to.
    let message = unsafe {
        ak::reference_utf16_string(message);
        Utf16String::from_raw_owned(message)
    };
    let (prototype, _) = prototype_and_constructor(vm, realm, WebAssemblyError::Compile);
    let error = Error::create_with_prototype(vm, realm, prototype);
    error.set_message(vm, message);
    error.as_ptr().cast()
}
