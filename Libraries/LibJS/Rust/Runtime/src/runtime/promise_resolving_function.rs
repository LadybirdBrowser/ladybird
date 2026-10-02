/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{
    NATIVE_FUNCTION_METHODS, NATIVE_FUNCTION_VIRTUAL_METHODS, NativeFunction, NativeFunctionMethods,
};
use crate::runtime::object::ObjectMethods;
use crate::runtime::promise::Promise;
use crate::runtime::realm::Realm;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PromiseResolvingFunctionKind {
    Resolve,
    Reject,
}

#[repr(C)]
#[derive(Trace)]
pub struct PromiseResolvingFunction {
    base: NativeFunction,
    promise: Cell<Gc<Promise>>,
    resolve_function: Cell<Option<Gc<PromiseResolvingFunction>>>,
    already_resolved: Cell<bool>,
    #[gc(untraced)]
    kind: PromiseResolvingFunctionKind,
}

static PROMISE_RESOLVING_FUNCTION_VIRTUAL_METHODS: NativeFunctionMethods = NativeFunctionMethods {
    call: PromiseResolvingFunction::call,
    ..NATIVE_FUNCTION_VIRTUAL_METHODS
};

static PROMISE_RESOLVING_FUNCTION_METHODS: ObjectMethods = ObjectMethods {
    initialize: PromiseResolvingFunction::initialize,
    native_function: Some(&PROMISE_RESOLVING_FUNCTION_VIRTUAL_METHODS),
    ..NATIVE_FUNCTION_METHODS
};

define_cell!(
    PromiseResolvingFunction,
    Object,
    extends: [NativeFunction, FunctionObject, Object],
    methods: PROMISE_RESOLVING_FUNCTION_METHODS
);

impl Deref for PromiseResolvingFunction {
    type Target = NativeFunction;

    fn deref(&self) -> &NativeFunction {
        &self.base
    }
}

impl PromiseResolvingFunction {
    pub fn create_resolve(vm: &Vm, realm: Gc<Realm>, promise: Gc<Promise>) -> Gc<PromiseResolvingFunction> {
        realm.create_object(
            vm,
            Self::new(
                vm,
                promise,
                PromiseResolvingFunctionKind::Resolve,
                None,
                realm.function_prototype(),
            ),
        )
    }

    pub fn create_reject(
        vm: &Vm,
        realm: Gc<Realm>,
        promise: Gc<Promise>,
        resolve_function: Gc<PromiseResolvingFunction>,
    ) -> Gc<PromiseResolvingFunction> {
        realm.create_object(
            vm,
            Self::new(
                vm,
                promise,
                PromiseResolvingFunctionKind::Reject,
                Some(resolve_function),
                realm.function_prototype(),
            ),
        )
    }

    fn new(
        vm: &Vm,
        promise: Gc<Promise>,
        kind: PromiseResolvingFunctionKind,
        resolve_function: Option<Gc<PromiseResolvingFunction>>,
        prototype: Gc<Object>,
    ) -> PromiseResolvingFunction {
        PromiseResolvingFunction {
            base: NativeFunction::new_with_prototype(vm, Self::CLASS, prototype),
            promise: Cell::new(promise),
            resolve_function: Cell::new(resolve_function),
            already_resolved: Cell::new(false),
            kind,
        }
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        (NATIVE_FUNCTION_METHODS.initialize)(object, vm, realm);

        object.unsafe_set_shape(realm.native_function_shape());
        object.put_direct(realm.native_function_length_offset(), Value::from_i32(1));
        object.put_direct(
            realm.native_function_name_offset(),
            Value::from_string(vm.empty_string()),
        );
    }

    #[allow(
        clippy::unnecessary_wraps,
        reason = "the call of a native function returns a completion"
    )]
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        assert!(function.is::<PromiseResolvingFunction>());
        // SAFETY: The function is a PromiseResolvingFunction, which starts with its NativeFunction.
        let function = unsafe { &*core::ptr::from_ref(function).cast::<PromiseResolvingFunction>() };
        let promise = function.promise.get();
        let already_resolved_owner = function.already_resolved_owner();
        let already_resolved = &already_resolved_owner.already_resolved;
        Ok(match function.kind {
            PromiseResolvingFunctionKind::Resolve => Promise::resolve_function_steps(vm, promise, already_resolved),
            PromiseResolvingFunctionKind::Reject => Promise::reject_function_steps(vm, promise, already_resolved),
        })
    }

    /// The function whose [[AlreadyResolved]] this one uses: the resolve function keeps the bit both share.
    fn already_resolved_owner(&self) -> Gc<PromiseResolvingFunction> {
        if self.kind == PromiseResolvingFunctionKind::Resolve {
            // SAFETY: Promise resolving functions only exist as cells once constructed.
            return unsafe { Gc::from_ref(self) };
        }

        self.resolve_function
            .get()
            .expect("a reject function shares the state of its resolve function")
    }
}
