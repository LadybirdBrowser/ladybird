/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Walking declarative and object environments and reading their bindings, as DevTools does to show scopes.
//!
//! Hosts also create environments, as event handlers close over the object environments of their element, form and
//! document, and WebAssembly module records keep their bindings in a module environment. The functions here follow
//! the contract object.rs states for the embedding module.

#![allow(
    clippy::missing_safety_doc,
    reason = "object.rs states the contract every exported function shares"
)]

use crate::embedding::abi_types::{
    CellAbi, JSUtf16View, append_to_string_sink, cell_from_abi, completion_into_abi, object_into_abi,
    optional_cell_from_abi, vm_from_abi,
};
use crate::gc::class::Extends;
use crate::gc::class_id::ClassId;
use crate::layout::cell::Gc;
use crate::layout::host_class::{JSCompletion, JSObject, JSStringSink, JSVM, JSValue};
use crate::layout::value::Value;
use crate::runtime::abstract_operations::new_object_environment;
use crate::runtime::declarative_environment::DeclarativeEnvironment;
use crate::runtime::environment::{Environment, InitializeBindingHint};
use crate::runtime::function_environment::FunctionEnvironment;
use crate::runtime::global_environment::GlobalEnvironment;
use crate::runtime::module_environment::ModuleEnvironment;
use crate::runtime::object_environment::ObjectEnvironment;
use crate::utf16::Utf16View;

/// An Environment Record, which C only ever sees behind a pointer.
pub struct JSEnvironment {
    _opaque: [u8; 0],
}

impl CellAbi for JSEnvironment {
    type Cell = Environment;
}

/// A PrivateEnvironment Record, which C only ever sees behind a pointer.
pub struct JSPrivateEnvironment {
    _opaque: [u8; 0],
}

pub(crate) fn environment_to_abi<T: Extends<Environment>>(environment: Gc<T>) -> *mut JSEnvironment {
    environment.upcast::<Environment>().as_ptr().cast()
}

fn binding_name_from_abi(name: Utf16View<'_>) -> ak::Utf16FlyString {
    name.to_utf16_fly_string()
}

/// The environment as an environment of type T, which its kind says it is.
fn downcast_environment<T: crate::gc::class::GcCell + Extends<Environment>>(environment: Gc<Environment>) -> Gc<T> {
    environment
        .downcast::<T>()
        .unwrap_or_else(|| panic!("the environment is not a {}", T::CLASS.name))
}

pub const JS_ENVIRONMENT_KIND_DECLARATIVE: u8 = 0;
pub const JS_ENVIRONMENT_KIND_FUNCTION: u8 = 1;
pub const JS_ENVIRONMENT_KIND_MODULE: u8 = 2;
pub const JS_ENVIRONMENT_KIND_GLOBAL: u8 = 3;
pub const JS_ENVIRONMENT_KIND_OBJECT: u8 = 4;

pub const JS_INITIALIZE_BINDING_HINT_NORMAL: u8 = 0;
pub const JS_INITIALIZE_BINDING_HINT_SYNC_DISPOSE: u8 = 1;
pub const JS_INITIALIZE_BINDING_HINT_ASYNC_DISPOSE: u8 = 2;

fn initialize_binding_hint_from_abi(hint: u8) -> InitializeBindingHint {
    match hint {
        JS_INITIALIZE_BINDING_HINT_NORMAL => InitializeBindingHint::Normal,
        JS_INITIALIZE_BINDING_HINT_SYNC_DISPOSE => InitializeBindingHint::SyncDispose,
        JS_INITIALIZE_BINDING_HINT_ASYNC_DISPOSE => InitializeBindingHint::AsyncDispose,
        hint => panic!("{hint} is not an initialize binding hint"),
    }
}

// Creating environments

/// NewObjectEnvironment(O, W, E), where the outer environment may be null. Returns an unrooted environment. Main
/// thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_new_object_environment(
    vm: *mut JSVM,
    binding_object: *mut JSObject,
    is_with_environment: bool,
    outer: *mut JSEnvironment,
) -> *mut JSEnvironment {
    // SAFETY: See the module documentation.
    let (vm, binding_object, outer) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSObject>(binding_object),
            optional_cell_from_abi::<JSEnvironment>(outer),
        )
    };
    environment_to_abi(new_object_environment(vm, binding_object, is_with_environment, outer))
}

/// NewModuleEnvironment(E), where the outer environment may be null. Returns an unrooted environment. Main thread
/// only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_new_module_environment(
    vm: *mut JSVM,
    outer: *mut JSEnvironment,
) -> *mut JSEnvironment {
    // SAFETY: See the module documentation.
    let (vm, outer) = unsafe { (vm_from_abi(vm), optional_cell_from_abi::<JSEnvironment>(outer)) };
    environment_to_abi(ModuleEnvironment::create(vm, outer))
}

// The abstract methods of Environment Records, which dispatch on the kind of environment

/// CreateImmutableBinding(N, S), with an unused payload. Borrows the name. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_create_immutable_binding(
    vm: *mut JSVM,
    environment: *mut JSEnvironment,
    name: JSUtf16View,
    strict: bool,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, environment, name) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSEnvironment>(environment),
            name.as_view(),
        )
    };
    completion_into_abi(environment.create_immutable_binding(vm, &binding_name_from_abi(name), strict))
}

/// InitializeBinding(N, V, hint), with an unused payload, where the hint is a JS_INITIALIZE_BINDING_HINT_* value.
/// Borrows the name. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_initialize_binding(
    vm: *mut JSVM,
    environment: *mut JSEnvironment,
    name: JSUtf16View,
    value: JSValue,
    hint: u8,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, environment, name) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSEnvironment>(environment),
            name.as_view(),
        )
    };
    completion_into_abi(environment.initialize_binding(
        vm,
        &binding_name_from_abi(name),
        Value(value),
        initialize_binding_hint_from_abi(hint),
    ))
}

/// SetMutableBinding(N, V, S), with an unused payload. Borrows the name. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_set_mutable_binding(
    vm: *mut JSVM,
    environment: *mut JSEnvironment,
    name: JSUtf16View,
    value: JSValue,
    strict: bool,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, environment, name) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSEnvironment>(environment),
            name.as_view(),
        )
    };
    completion_into_abi(environment.set_mutable_binding(vm, &binding_name_from_abi(name), Value(value), strict))
}

/// GetBindingValue(N, S), which throws a ReferenceError for a binding that is not initialized yet. Borrows the name.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_get_binding_value(
    vm: *mut JSVM,
    environment: *mut JSEnvironment,
    name: JSUtf16View,
    strict: bool,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, environment, name) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSEnvironment>(environment),
            name.as_view(),
        )
    };
    completion_into_abi(environment.get_binding_value(vm, &binding_name_from_abi(name), strict))
}

// Walking environments

/// The kind of the environment, a JS_ENVIRONMENT_KIND_* value. Function and module environments are declarative
/// environments too. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_kind(environment: *mut JSEnvironment) -> u8 {
    // SAFETY: See the module documentation.
    let environment = unsafe { cell_from_abi::<JSEnvironment>(environment) };
    match environment.class().id {
        ClassId::DeclarativeEnvironment => JS_ENVIRONMENT_KIND_DECLARATIVE,
        ClassId::FunctionEnvironment => JS_ENVIRONMENT_KIND_FUNCTION,
        ClassId::ModuleEnvironment => JS_ENVIRONMENT_KIND_MODULE,
        ClassId::GlobalEnvironment => JS_ENVIRONMENT_KIND_GLOBAL,
        ClassId::ObjectEnvironment => JS_ENVIRONMENT_KIND_OBJECT,
        class_id => unreachable!("{class_id:?} is not a class of environment"),
    }
}

/// [[OuterEnv]], or null for the global environment. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_outer(environment: *mut JSEnvironment) -> *mut JSEnvironment {
    // SAFETY: See the module documentation.
    let environment = unsafe { cell_from_abi::<JSEnvironment>(environment) };
    environment
        .outer_environment()
        .map_or(core::ptr::null_mut(), environment_to_abi)
}

/// Appends the name of every binding of a declarative, function or module environment to the sink, in the order the
/// bindings were created, as 16-bit code units. The sink may call back into the VM. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_declarative_binding_names(
    environment: *mut JSEnvironment,
    names: *mut JSStringSink,
) {
    // SAFETY: See the module documentation.
    let (environment, names) = unsafe { (cell_from_abi::<JSEnvironment>(environment), &*names) };
    let declarative_environment = environment
        .as_declarative_environment()
        .expect("the environment is declarative");
    // The names are copied out first, so that nothing of the environment is borrowed while the sink runs.
    for name in declarative_environment.bindings() {
        append_to_string_sink(names, Utf16View::of_fly_string(&name));
    }
}

/// Whether the binding of the name in a declarative, function or module environment is mutable. Borrows the name.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_declarative_binding_is_mutable(
    environment: *mut JSEnvironment,
    name: JSUtf16View,
) -> bool {
    // SAFETY: See the module documentation.
    let (environment, name) = unsafe { (cell_from_abi::<JSEnvironment>(environment), name.as_view()) };
    environment
        .as_declarative_environment()
        .expect("the environment is declarative")
        .binding_is_mutable_by_name(&binding_name_from_abi(name))
}

/// [[DeclarativeRecord]] of a global environment. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_global_declarative_record(
    environment: *mut JSEnvironment,
) -> *mut JSEnvironment {
    // SAFETY: See the module documentation.
    let environment = unsafe { cell_from_abi::<JSEnvironment>(environment) };
    environment_to_abi::<DeclarativeEnvironment>(
        downcast_environment::<GlobalEnvironment>(environment).declarative_record(),
    )
}

/// [[GlobalThisValue]] of a global environment. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_global_this_value(environment: *mut JSEnvironment) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let environment = unsafe { cell_from_abi::<JSEnvironment>(environment) };
    object_into_abi(downcast_environment::<GlobalEnvironment>(environment).global_this_value())
}

/// [[BindingObject]] of an object environment. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_object_binding_object(environment: *mut JSEnvironment) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let environment = unsafe { cell_from_abi::<JSEnvironment>(environment) };
    object_into_abi(downcast_environment::<ObjectEnvironment>(environment).binding_object())
}

/// [[FunctionObject]] of a function environment. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_environment_function_object(environment: *mut JSEnvironment) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let environment = unsafe { cell_from_abi::<JSEnvironment>(environment) };
    object_into_abi(downcast_environment::<FunctionEnvironment>(environment).function_object())
}
