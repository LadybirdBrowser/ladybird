/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Host functions, of kind JS_HOST_CLASS_FUNCTION.
//!
//! The exported functions of this file run on the thread that owns the VM, and trust their arguments, as those of
//! embedding/object.rs do: `vm` is the embedder's VM, objects and realms are live cells of its heap, host classes are
//! tables that live as long as the process, and the embedder's cells are null or live GC cells of the same heap.
#![allow(
    clippy::missing_safety_doc,
    reason = "the module documentation states the contract every exported function shares"
)]

use core::ffi::c_void;
use core::ops::Deref;
use core::ptr::NonNull;

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::embedding::abi_types::{
    JSRealm, JSUtf16View, cell_from_abi, completion_from_abi, object_into_abi, optional_cell_from_abi, vm_from_abi,
    vm_into_abi,
};
use crate::embedding::host::class_table::{
    copy_host_class_flags_into_object, host_class_from_abi, lend_object_to_hook, object_completion_from_hook,
};
use crate::embedding::host::registry::runtime_class_and_allocator_of_host_class;
use crate::gc::class::{Class, Finalize, define_cell};
use crate::gc::foreign::ForeignCellSlot;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::host_class::{JS_HOST_CLASS_FUNCTION, JS_HOST_CLASS_HAS_CONSTRUCTOR, JSHostClass, JSObject, JSVM};
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{
    NATIVE_FUNCTION_METHODS, NATIVE_FUNCTION_VIRTUAL_METHODS, NativeFunction, NativeFunctionMethods,
};
use crate::runtime::object::{Object, ObjectMethods, allocate_object_in};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;

/// A built-in function whose [[Call]] and [[Construct]] come from its host class, with a C++ GC cell for any state
/// of its own.
#[repr(C)]
#[derive(Trace)]
pub struct HostFunction {
    pub base: NativeFunction,
    #[gc(untraced)]
    pub host_class: &'static JSHostClass,
    pub host_data: ForeignCellSlot,
}

static HOST_FUNCTION_VIRTUAL_METHODS: NativeFunctionMethods = NativeFunctionMethods {
    call: call_through_hook,
    construct: construct_through_hook,
    ..NATIVE_FUNCTION_VIRTUAL_METHODS
};

/// The internal methods of every host function class, which read the hooks and flags of the function's own table.
static HOST_FUNCTION_METHODS: ObjectMethods = ObjectMethods {
    has_constructor: host_function_has_constructor,
    native_function: Some(&HOST_FUNCTION_VIRTUAL_METHODS),
    ..NATIVE_FUNCTION_METHODS
};

// The class that the class of every host class table of this kind extends. No function has it as its class.
define_cell!(
    HostFunction,
    Object,
    extends: [NativeFunction, FunctionObject, Object],
    methods: HOST_FUNCTION_METHODS,
    finalize: finalize
);

impl Finalize for HostFunction {
    fn finalize(&self) {
        if let Some(finalize) = self.host_class.host_function_hooks().finalize {
            // SAFETY: The hook takes a dying function of its class, which stays intact while it runs.
            unsafe { finalize(lend_object_to_hook(self)) };
        }
    }
}

impl Deref for HostFunction {
    type Target = NativeFunction;

    fn deref(&self) -> &NativeFunction {
        &self.base
    }
}

/// The class of the functions of `table`, which extends `parent`.
pub fn derive_host_function_class(table: &'static JSHostClass, parent: &'static Class) -> &'static Class {
    Class::derive_runtime(parent, table.class_name(), &HOST_FUNCTION_METHODS)
}

/// The host function an internal method of a host function class was called on.
pub(crate) fn as_host_function(object: &Object) -> &HostFunction {
    debug_assert!(object.is::<HostFunction>());
    // SAFETY: The object is a HostFunction, which starts with its Object.
    unsafe { &*core::ptr::from_ref(object).cast::<HostFunction>() }
}

fn host_function_has_constructor(object: &Object) -> bool {
    as_host_function(object)
        .host_class
        .has_flag(JS_HOST_CLASS_HAS_CONSTRUCTOR)
}

// [[Call]] and [[Construct]] run the hooks in the function's own execution context, which NativeFunction pushes, so
// the hooks read the arguments and the this value from the VM. The runtime holds no borrow of its state across them.

fn call_through_hook(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
    let hook = as_host_function(function)
        .host_class
        .host_function_hooks()
        .call
        .expect("a host function class has a call hook");
    // SAFETY: The hook takes a function of its class and the VM.
    completion_from_abi(unsafe { hook(lend_object_to_hook(function), vm_into_abi(vm)) })
}

fn construct_through_hook(
    function: &NativeFunction,
    vm: &Vm,
    new_target: Gc<FunctionObject>,
) -> ThrowCompletionOr<Gc<Object>> {
    let Some(hook) = as_host_function(function).host_class.host_function_hooks().construct else {
        return (NATIVE_FUNCTION_VIRTUAL_METHODS.construct)(function, vm, new_target);
    };
    // SAFETY: The hook takes a function of its class, the VM and the new target, and completes with a live object.
    unsafe {
        object_completion_from_hook(hook(
            lend_object_to_hook(function),
            vm_into_abi(vm),
            object_into_abi(new_target),
        ))
    }
}

impl HostFunction {
    /// HostFunction::create(): a function of the class `table` describes, which defines "length" and then "name",
    /// as CreateBuiltinFunction does. The prototype defaults to %Function.prototype%.
    ///
    /// # Safety
    ///
    /// `table` must be of kind JS_HOST_CLASS_FUNCTION, and `host_data` absent or a live cell of the VM's heap.
    pub unsafe fn create(
        vm: &Vm,
        realm: Gc<Realm>,
        table: &'static JSHostClass,
        name: Utf16FlyString,
        length: i32,
        prototype: Option<Gc<Object>>,
        host_data: Option<NonNull<c_void>>,
    ) -> Gc<HostFunction> {
        // SAFETY: The caller's guarantees are those of this function.
        let function = unsafe { Self::create_without_own_properties(vm, realm, table, name, prototype, host_data) };
        let configurable = PropertyAttributes::new(Attribute::CONFIGURABLE);
        function.define_direct_property(vm, &vm.names.length.clone(), Value::from_i32(length), configurable);
        let name = Value::from_string(PrimitiveString::create_from_fly_string(vm, &function.name()));
        function.define_direct_property(vm, &vm.names.name.clone(), name, configurable);
        function
    }

    /// HostFunction::create_without_own_properties(): for a caller that defines the function's own properties
    /// itself, in an order of its own. The function's realm is that of the shape of its prototype, which defaults to
    /// %Function.prototype%.
    ///
    /// # Safety
    ///
    /// As for create().
    pub unsafe fn create_without_own_properties(
        vm: &Vm,
        realm: Gc<Realm>,
        table: &'static JSHostClass,
        name: Utf16FlyString,
        prototype: Option<Gc<Object>>,
        host_data: Option<NonNull<c_void>>,
    ) -> Gc<HostFunction> {
        let (class, allocator) = runtime_class_and_allocator_of_host_class(vm, table, JS_HOST_CLASS_FUNCTION);
        let prototype = prototype.unwrap_or_else(|| realm.function_prototype());
        let base = NativeFunction::new_with_name(vm, class, name, prototype);
        copy_host_class_flags_into_object(table, &base);
        let function = HostFunction {
            base,
            host_class: table,
            host_data: ForeignCellSlot::empty(),
        };
        // SAFETY: The caller passes an absent or live cell, which the slot then keeps alive.
        unsafe { function.host_data.set(host_data) };
        let function = allocate_object_in(vm, allocator, function);
        function.initialize(vm, realm);
        function
    }
}

// The embedding ABI of host functions

/// HostFunction::create(): a function of `host_class`, a table of kind JS_HOST_CLASS_FUNCTION, whose name is a copy of
/// the code units `name` views. It defines "length" and then "name", as CreateBuiltinFunction does. Its [[Prototype]]
/// is `prototype_or_null`, or %Function.prototype% of the realm for null, and its realm that of the prototype's shape.
/// The function keeps `host_data_or_null` alive. Returns an unrooted function. Main thread only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_host_function_create(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    host_class: *const JSHostClass,
    name: JSUtf16View,
    length: i32,
    prototype_or_null: *mut JSObject,
    host_data_or_null: *mut c_void,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    unsafe {
        object_into_abi(HostFunction::create(
            vm_from_abi(vm),
            cell_from_abi::<JSRealm>(realm),
            host_class_from_abi(host_class),
            name.as_view().to_utf16_fly_string(),
            length,
            optional_cell_from_abi::<JSObject>(prototype_or_null),
            NonNull::new(host_data_or_null),
        ))
    }
}

/// HostFunction::create_without_own_properties(): js_host_function_create() without the "length" and "name"
/// properties, for a caller that defines the function's own properties itself, in an order of its own. Main thread
/// only.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_host_function_create_without_own_properties(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    host_class: *const JSHostClass,
    name: JSUtf16View,
    prototype_or_null: *mut JSObject,
    host_data_or_null: *mut c_void,
) -> *mut JSObject {
    // SAFETY: See the module documentation.
    unsafe {
        object_into_abi(HostFunction::create_without_own_properties(
            vm_from_abi(vm),
            cell_from_abi::<JSRealm>(realm),
            host_class_from_abi(host_class),
            name.as_view().to_utf16_fly_string(),
            optional_cell_from_abi::<JSObject>(prototype_or_null),
            NonNull::new(host_data_or_null),
        ))
    }
}
