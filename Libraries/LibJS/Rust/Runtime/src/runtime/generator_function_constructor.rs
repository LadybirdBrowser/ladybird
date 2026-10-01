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
pub struct GeneratorFunctionConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    GeneratorFunctionConstructor,
    initialize: GeneratorFunctionConstructor::initialize,
    call: GeneratorFunctionConstructor::call,
    construct: GeneratorFunctionConstructor::construct
);

impl GeneratorFunctionConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<GeneratorFunctionConstructor> {
        realm.create_object(
            vm,
            GeneratorFunctionConstructor {
                base: NativeFunction::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().function_constructor(vm).upcast(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 27.3.2.1 GeneratorFunction.length, https://tc39.es/ecma262/#sec-generatorfunction.length
        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        // 27.3.2.2 GeneratorFunction.prototype, https://tc39.es/ecma262/#sec-generatorfunction.length
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().generator_function_prototype(vm)),
            PropertyAttributes::new(0),
        );
    }

    // 27.3.1.1 GeneratorFunction ( p1, p2, … , pn, body ), https://tc39.es/ecma262/#sec-generatorfunction
    fn call(_: &NativeFunction, _: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function(
            "GeneratorFunctionConstructor::call, the [[Call]] of %GeneratorFunction%",
            0,
        )
    }

    // 27.3.1.1 GeneratorFunction ( ...parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-generatorfunction
    fn construct(_: &NativeFunction, _: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        unimplemented_runtime_function(
            "GeneratorFunctionConstructor::construct, the [[Construct]] of %GeneratorFunction%",
            0,
        )
    }
}
