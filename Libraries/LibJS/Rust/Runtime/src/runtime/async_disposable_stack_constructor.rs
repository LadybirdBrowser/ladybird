/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{new_dispose_capability, ordinary_create_from_constructor_of};
use crate::runtime::async_disposable_stack::AsyncDisposableStack;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct AsyncDisposableStackConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    AsyncDisposableStackConstructor,
    initialize: AsyncDisposableStackConstructor::initialize,
    call: AsyncDisposableStackConstructor::call,
    construct: AsyncDisposableStackConstructor::construct
);

impl AsyncDisposableStackConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AsyncDisposableStackConstructor> {
        realm.create_object(
            vm,
            AsyncDisposableStackConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.AsyncDisposableStack.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 12.4.2.1 AsyncDisposableStack.prototype, https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().async_disposable_stack_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 12.4.1.1 AsyncDisposableStack ( ), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&vm.names.AsyncDisposableStack],
        )
    }

    // 12.4.1.1 AsyncDisposableStack ( ), https://tc39.es/proposal-explicit-resource-management/#sec-asyncdisposablestack
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");

        // 2. Let asyncDisposableStack be ? OrdinaryCreateFromConstructor(NewTarget, "%AsyncDisposableStack.prototype%", « [[AsyncDisposableState]], [[DisposeCapability]] »).
        // 3. Set asyncDisposableStack.[[AsyncDisposableState]] to pending.
        // 4. Set asyncDisposableStack.[[DisposeCapability]] to NewDisposeCapability().
        // 5. Return asyncDisposableStack.
        let async_disposable_stack = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            Intrinsics::async_disposable_stack_prototype,
            |prototype| AsyncDisposableStack::new(vm, new_dispose_capability(), prototype),
        )?;
        Ok(async_disposable_stack.upcast())
    }
}
