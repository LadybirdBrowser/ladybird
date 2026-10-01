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
pub struct AsyncGeneratorFunctionConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    AsyncGeneratorFunctionConstructor,
    initialize: AsyncGeneratorFunctionConstructor::initialize,
    call: AsyncGeneratorFunctionConstructor::call,
    construct: AsyncGeneratorFunctionConstructor::construct
);

impl AsyncGeneratorFunctionConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AsyncGeneratorFunctionConstructor> {
        // NB: The spec has %Function% as the [[Prototype]] of %AsyncGeneratorFunction%. The C++ runtime has
        //     %Function.prototype%, which this replicates.
        realm.create_object(
            vm,
            AsyncGeneratorFunctionConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.AsyncGeneratorFunction.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 27.4.2.1 AsyncGeneratorFunction.length, https://tc39.es/ecma262/#sec-asyncgeneratorfunction-length
        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        // 27.4.2.2 AsyncGeneratorFunction.prototype, https://tc39.es/ecma262/#sec-asyncgeneratorfunction-prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().async_generator_function_prototype(vm)),
            PropertyAttributes::new(0),
        );
    }

    // 27.4.1.1 AsyncGeneratorFunction ( p1, p2, … , pn, body ), https://tc39.es/ecma262/#sec-asyncgeneratorfunction
    fn call(_: &NativeFunction, _: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function(
            "AsyncGeneratorFunctionConstructor::call, the [[Call]] of %AsyncGeneratorFunction%",
            0,
        )
    }

    // 27.4.1.1 AsyncGeneratorFunction ( ...parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-asyncgeneratorfunction
    fn construct(_: &NativeFunction, _: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        unimplemented_runtime_function(
            "AsyncGeneratorFunctionConstructor::construct, the [[Construct]] of %AsyncGeneratorFunction%",
            0,
        )
    }
}
