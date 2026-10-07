/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Native functions, and calling and constructing functions.
//!
//! The functions here follow the contract object.rs states for the embedding module.

#![allow(
    clippy::missing_safety_doc,
    reason = "object.rs states the contract every exported function shares"
)]

use core::ffi::{c_char, c_void};
use core::mem::offset_of;
use core::ptr::NonNull;

use crate::embedding::abi_types::{
    JSOwnedUtf16String, JSRealm, JSUtf16View, cell_from_abi, completion_from_abi, completion_into_abi, object_into_abi,
    optional_cell_from_abi, optional_cell_into_abi, owned_utf16_string_into_abi, property_key_from_abi, vm_from_abi,
    vm_into_abi,
};
use crate::embedding::environment::{JSEnvironment, JSPrivateEnvironment};
use crate::embedding::execution_context::{JSScriptOrModule, script_or_module_from_abi};
use crate::embedding::object::{function_from_abi, optional_function_from_abi, values_from_abi};
use crate::gc::foreign::ForeignCellSlot;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::layout::host_class::{JSCompletion, JSObject, JSPropertyKey, JSVM, JSValue};
use crate::layout::value::Value;
use crate::layout_forward::{RawNativeFunctionPointer, RawNativeFunctionResult};
use crate::runtime::abstract_operations::{call, construct};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_constructor::compile_dynamic_function;
use crate::runtime::native_function::{
    DirectGetterConfiguration, DirectGetterFunction, NativeFunction, RawNativeFunction,
};
use crate::runtime::private_environment::PrivateEnvironment;
use crate::runtime::shared_function_instance_data::{FunctionKind, SharedFunctionInstanceData};

/// A raw native function: LibJS's NativeFunctionPointer, ThrowCompletionOr<Value> (*)(VM&). It reads its arguments,
/// this value and callee from the running execution context. C++ returns the 16-byte ThrowCompletionOr in two
/// registers, as C returns a JSCompletion, except on Mach-O x86_64 and Windows, where it returns it through a buffer
/// the caller passes first.
#[cfg(not(any(all(target_arch = "x86_64", target_vendor = "apple"), target_os = "windows")))]
pub type JSNativeFunction = Option<unsafe extern "C" fn(vm: *mut JSVM) -> JSCompletion>;

/// A raw native function: LibJS's NativeFunctionPointer, ThrowCompletionOr<Value> (*)(VM&). It reads its arguments,
/// this value and callee from the running execution context. C++ returns the 16-byte ThrowCompletionOr in two
/// registers, as C returns a JSCompletion, except on Mach-O x86_64 and Windows, where it returns it through a buffer
/// the caller passes first.
#[cfg(any(all(target_arch = "x86_64", target_vendor = "apple"), target_os = "windows"))]
pub type JSNativeFunction = Option<unsafe extern "C" fn(result: *mut JSCompletion, vm: *mut JSVM)>;

const _: () = assert!(size_of::<JSNativeFunction>() == size_of::<RawNativeFunctionPointer>());
const _: () = assert!(size_of::<JSCompletion>() == size_of::<RawNativeFunctionResult>());
const _: () = assert!(offset_of!(JSCompletion, payload) == offset_of!(RawNativeFunctionResult, payload));
const _: () = assert!(offset_of!(JSCompletion, variant) == offset_of!(RawNativeFunctionResult, variant));

pub(crate) fn raw_native_function_from_abi(function: JSNativeFunction) -> RawNativeFunctionPointer {
    // SAFETY: The two types differ only in the names of the VM pointer and of the completion struct, which have the
    //         same layout, so they describe the same calling convention.
    unsafe { core::mem::transmute::<JSNativeFunction, RawNativeFunctionPointer>(function) }
}

/// The behaviour of a closure function: called with the context the closure was created with and the VM, it reads its
/// arguments like a raw native function.
pub type JSClosureCall = Option<unsafe extern "C" fn(context: *mut c_void, vm: *mut JSVM) -> JSCompletion>;

/// An embedder's closure: a call and the C++ GC cell it captures, which the function keeps alive.
struct HostClosure {
    call: unsafe extern "C" fn(context: *mut c_void, vm: *mut JSVM) -> JSCompletion,
    context: ForeignCellSlot,
}

// SAFETY: The context is the only cell a closure holds.
unsafe impl Trace for HostClosure {
    fn trace(&self, visitor: &mut Visitor) {
        self.context.trace(visitor);
    }
}

/// # Safety
///
/// `context` must be null or a live cell of the VM's heap.
unsafe fn host_closure_from_abi(call: JSClosureCall, context: *mut c_void) -> HostClosure {
    let closure = HostClosure {
        call: call.expect("the closure has a call"),
        context: ForeignCellSlot::empty(),
    };
    // SAFETY: The caller passes null or a live cell, which the closure keeps alive from here on.
    unsafe { closure.context.set(NonNull::new(context)) };
    closure
}

fn call_host_closure(vm: &Vm, closure: &HostClosure) -> ThrowCompletionOr<Value> {
    // SAFETY: The embedder created the closure with a call that takes this context and the VM. Nothing of the
    //         runtime's state is borrowed while the call runs, as it may call back into the VM.
    let completion = unsafe { (closure.call)(closure.context.as_ptr(), vm_into_abi(vm)) };
    completion_from_abi(completion)
}

/// # Safety
///
/// `prefix` must be null or point to `prefix_length` bytes of UTF-8.
unsafe fn prefix_from_abi<'prefix>(prefix: *const c_char, prefix_length: usize) -> Option<&'prefix str> {
    if prefix.is_null() {
        return None;
    }
    // SAFETY: The caller passes `prefix_length` bytes at `prefix`.
    let bytes = unsafe { core::slice::from_raw_parts(prefix.cast::<u8>(), prefix_length) };
    Some(core::str::from_utf8(bytes).expect("the prefix is UTF-8"))
}

// Creating native functions

/// CreateBuiltinFunction with a raw native function as its behaviour, as C++ NativeFunction::create() does: "length"
/// is the length, "name" is the key, after the prefix (null for none, or `prefix_length` bytes of UTF-8 such as
/// "get") and a space. Its [[Realm]] is the realm, or the current realm when that is null. Borrows the key. Returns an
/// unrooted function. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_function_create_native(
    vm: *mut JSVM,
    behaviour: JSNativeFunction,
    length: i32,
    name: *const JSPropertyKey,
    realm: *mut JSRealm,
    prefix: *const c_char,
    prefix_length: usize,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, name, realm, prefix) = unsafe {
        (
            vm_from_abi(vm),
            property_key_from_abi(name),
            optional_cell_from_abi::<JSRealm>(realm),
            prefix_from_abi(prefix, prefix_length),
        )
    };
    object_into_abi(RawNativeFunction::create(
        vm,
        raw_native_function_from_abi(behaviour),
        length,
        name,
        realm,
        prefix,
        None,
    ))
}

/// js_function_create_native() for a closure: the function calls `call` with `context`, which is null or a C++ GC cell
/// that the function keeps alive for as long as it lives, such as the GC::Function the facade wraps an AK::Function
/// in. Borrows the key. Returns an unrooted function. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_function_create_closure(
    vm: *mut JSVM,
    call: JSClosureCall,
    context: *mut c_void,
    length: i32,
    name: *const JSPropertyKey,
    realm: *mut JSRealm,
    prefix: *const c_char,
    prefix_length: usize,
) -> *mut JSObject {
    // SAFETY: See the module documentation. The context is null or a live cell.
    let (vm, closure, name, realm, prefix) = unsafe {
        (
            vm_from_abi(vm),
            host_closure_from_abi(call, context),
            property_key_from_abi(name),
            optional_cell_from_abi::<JSRealm>(realm),
            prefix_from_abi(prefix, prefix_length),
        )
    };
    object_into_abi(NativeFunction::create(
        vm,
        closure,
        call_host_closure,
        length,
        name,
        realm,
        prefix,
        None,
    ))
}

/// A closure of the realm with the name that call stacks show, and no "length" or "name" property, as C++
/// NativeFunction::create(realm, name, behaviour) creates one. Its context is null or a C++ GC cell that the function
/// keeps alive. Borrows the name. Returns an unrooted function. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_function_create_closure_with_name(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    name: JSUtf16View,
    call: JSClosureCall,
    context: *mut c_void,
) -> *mut JSObject {
    // SAFETY: See the module documentation. The context is null or a live cell.
    let (vm, realm, name, closure) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSRealm>(realm),
            name.as_view(),
            host_closure_from_abi(call, context),
        )
    };
    object_into_abi(NativeFunction::create_with_name(
        vm,
        realm,
        &name.to_utf16_fly_string(),
        closure,
        call_host_closure,
    ))
}

/// Where a direct getter finds the value it returns, as byte offsets of pointer-sized fields: the wrapped
/// implementation in the wrapper (JS_HOST_OBJECT_WRAPPABLE_OFFSET for a host object), the field of the implementation
/// that holds the value's GC::Weak implementation, the main world wrapper in that value, and the cell in a
/// GC::WeakImpl. This is LibJS's JS::DirectGetterConfiguration.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct JSDirectGetterConfiguration {
    pub wrapper_implementation_offset: usize,
    pub implementation_value_offset: usize,
    pub main_world_wrapper_offset: usize,
    pub weak_impl_value_offset: usize,
}

/// js_function_create_native() for an attribute getter that the interpreter can answer without calling the behaviour,
/// by following the configuration's offsets from the this value. Every offset must be a multiple of the pointer size.
/// The realm must not be null. Borrows the key and the configuration. Returns an unrooted function. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_function_create_direct_getter(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    behaviour: JSNativeFunction,
    length: i32,
    name: *const JSPropertyKey,
    configuration: *const JSDirectGetterConfiguration,
    prefix: *const c_char,
    prefix_length: usize,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm, name, configuration, prefix) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSRealm>(realm),
            property_key_from_abi(name),
            *configuration,
            prefix_from_abi(prefix, prefix_length),
        )
    };
    object_into_abi(DirectGetterFunction::create(
        vm,
        realm,
        raw_native_function_from_abi(behaviour),
        length,
        name,
        DirectGetterConfiguration {
            wrapper_implementation_offset: configuration.wrapper_implementation_offset,
            implementation_value_offset: configuration.implementation_value_offset,
            main_world_wrapper_offset: configuration.main_world_wrapper_offset,
            weak_impl_value_offset: configuration.weak_impl_value_offset,
        },
        prefix,
    ))
}

// What a function is

/// The function's [[Realm]], or null for a function without one, such as a bound function or a proxy. Main thread
/// only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_function_realm(function: *mut JSObject) -> *mut JSRealm {
    // SAFETY: See the module documentation.
    let function = unsafe { function_from_abi(function) };
    optional_cell_into_abi::<JSRealm>(function.realm())
}

/// The name of the function that call stacks show, which the caller owns. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_function_name_for_call_stack(function: *mut JSObject) -> JSOwnedUtf16String {
    // SAFETY: See the module documentation.
    let function = unsafe { function_from_abi(function) };
    owned_utf16_string_into_abi(
        function
            .upcast::<crate::runtime::object::Object>()
            .name_for_call_stack(),
    )
}

// Calling functions

/// Call(F, V, argumentsList), which throws a TypeError when the function is not callable. The arguments, `count`
/// values at `arguments`, must stay reachable until the call returns, as they do on the stack or in a root vector.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_function_call(
    vm: *mut JSVM,
    function: JSValue,
    this_value: JSValue,
    arguments: *const JSValue,
    count: usize,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, arguments) = unsafe { (vm_from_abi(vm), values_from_abi(arguments, count)) };
    completion_into_abi(call(vm, Value(function), Value(this_value), arguments))
}

/// Construct(F, argumentsList, newTarget), whose payload is the new object. The function must be a constructor, and
/// a null new target stands for the function itself. The arguments must stay reachable as for js_function_call().
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_function_construct(
    vm: *mut JSVM,
    function: *mut JSObject,
    arguments: *const JSValue,
    count: usize,
    new_target: *mut JSObject,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, function, arguments, new_target) = unsafe {
        (
            vm_from_abi(vm),
            function_from_abi(function),
            values_from_abi(arguments, count),
            optional_function_from_abi(new_target),
        )
    };
    completion_into_abi(construct(vm, function, arguments, new_target))
}

/// GetMethod(V, P), whose payload is the function, or null when the property is undefined or null. Borrows the key.
/// Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_function_get_method(
    vm: *mut JSVM,
    value: JSValue,
    key: *const JSPropertyKey,
) -> JSCompletion {
    // SAFETY: See the module documentation.
    let (vm, key) = unsafe { (vm_from_abi(vm), property_key_from_abi(key)) };
    completion_into_abi(Value(value).get_method(vm, key))
}

// Functions from source text, which hosts create outside of any script

/// The compiled code of a function, which C only ever sees behind a pointer.
pub struct JSSharedFunctionData {
    _opaque: [u8; 0],
}

pub const JS_FUNCTION_KIND_NORMAL: u8 = 0;
pub const JS_FUNCTION_KIND_GENERATOR: u8 = 1;
pub const JS_FUNCTION_KIND_ASYNC: u8 = 2;
pub const JS_FUNCTION_KIND_ASYNC_GENERATOR: u8 = 3;

fn function_kind_from_abi(kind: u8) -> FunctionKind {
    match kind {
        JS_FUNCTION_KIND_NORMAL => FunctionKind::Normal,
        JS_FUNCTION_KIND_GENERATOR => FunctionKind::Generator,
        JS_FUNCTION_KIND_ASYNC => FunctionKind::Async,
        JS_FUNCTION_KIND_ASYNC_GENERATOR => FunctionKind::AsyncGenerator,
        kind => panic!("{kind} is not a function kind"),
    }
}

/// Compiles the function that the source text defines the way CreateDynamicFunction does, after checking its
/// parameters and its body on their own, as JS::CompiledDynamicFunction::compile() does: the kind is a
/// JS_FUNCTION_KIND_* value, and the body is the body parse string, the body between two line feeds. Returns the
/// compiled code, an unrooted cell that the caller keeps alive (in a GC::Root) until it instantiates it, or null after
/// writing the message of the first syntax error, which the caller owns, to `error_message`. Borrows the strings. Main
/// thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_function_compile_dynamic(
    vm: *mut JSVM,
    source_text: JSUtf16View,
    parameters: JSUtf16View,
    body: JSUtf16View,
    kind: u8,
    error_message: *mut JSOwnedUtf16String,
) -> *mut JSSharedFunctionData {
    // SAFETY: See the module documentation.
    let (vm, source_text, parameters, body) = unsafe {
        (
            vm_from_abi(vm),
            source_text.as_view(),
            parameters.as_view(),
            body.as_view(),
        )
    };
    let code_units = |view: crate::utf16::Utf16View<'_>| {
        let mut code_units = Vec::with_capacity(view.length_in_code_units());
        view.append_to(&mut code_units);
        code_units
    };
    match compile_dynamic_function(
        vm,
        &source_text.to_utf16_string(),
        &code_units(parameters),
        &code_units(body),
        function_kind_from_abi(kind),
    ) {
        Ok(function_data) => function_data.as_ptr().cast(),
        Err(parser_error) => {
            let message = ak::Utf16String::from_utf8(&parser_error.to_string());
            // SAFETY: As above, `error_message` is writable.
            unsafe { error_message.write(owned_utf16_string_into_abi(message)) };
            core::ptr::null_mut()
        }
    }
}

/// OrdinaryFunctionCreate over code js_function_compile_dynamic() compiled: a function of the realm with the default
/// prototype for its kind, which closes over the scope and the private environment (null for none), and whose
/// [[ScriptOrModule]] is `script_or_module`, as an execution context holds it. Returns an unrooted function. Main
/// thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_function_instantiate_dynamic(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    function_data: *mut JSSharedFunctionData,
    scope: *mut JSEnvironment,
    private_environment: *mut JSPrivateEnvironment,
    script_or_module: JSScriptOrModule,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    let (vm, realm, scope) = unsafe {
        (
            vm_from_abi(vm),
            cell_from_abi::<JSRealm>(realm),
            cell_from_abi::<JSEnvironment>(scope),
        )
    };
    let function_data = NonNull::new(function_data.cast::<SharedFunctionInstanceData>()).expect("the code is not null");
    // SAFETY: The caller passes code js_function_compile_dynamic() compiled, which it kept alive.
    let function_data = unsafe { Gc::from_non_null(function_data) };
    let private_environment = NonNull::new(private_environment.cast::<PrivateEnvironment>()).map(|environment| {
        // SAFETY: The caller passes null or a live private environment.
        unsafe { Gc::from_non_null(environment) }
    });
    // SAFETY: The caller passes a live script or module, or an empty one.
    let script_or_module = unsafe { script_or_module_from_abi(script_or_module) };
    let function =
        EcmascriptFunctionObject::create_from_function_data(vm, realm, function_data, Some(scope), private_environment);
    function.set_script_or_module(script_or_module);
    object_into_abi(function)
}
