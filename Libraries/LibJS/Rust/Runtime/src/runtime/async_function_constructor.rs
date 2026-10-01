/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct AsyncFunctionConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    AsyncFunctionConstructor,
    initialize: AsyncFunctionConstructor::initialize,
    call: AsyncFunctionConstructor::call,
    construct: AsyncFunctionConstructor::construct
);

impl AsyncFunctionConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AsyncFunctionConstructor> {
        realm.create_object(
            vm,
            AsyncFunctionConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.AsyncFunction.as_string().clone(),
                    realm.intrinsics().function_constructor(vm).upcast(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 27.7.2.2 AsyncFunction.prototype, https://tc39.es/ecma262/#sec-async-function-constructor-prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().async_function_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 27.7.1.1 AsyncFunction ( p1, p2, … , pn, body ), https://tc39.es/ecma262/#sec-async-function-constructor-arguments
    fn call(_: &NativeFunction, _: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function("AsyncFunctionConstructor::call, the [[Call]] of %AsyncFunction%", 0)
    }

    // 27.7.1.1 AsyncFunction ( ...parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-async-function-constructor-arguments
    fn construct(_: &NativeFunction, _: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        unimplemented_runtime_function(
            "AsyncFunctionConstructor::construct, the [[Construct]] of %AsyncFunction%",
            0,
        )
    }
}
