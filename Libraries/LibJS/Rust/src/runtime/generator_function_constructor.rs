/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_constructor::FunctionConstructor;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::runtime::shared_function_instance_data::FunctionKind;

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
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        Ok(Value::from_object(Self::construct(
            function,
            vm,
            function.as_function_object_gc(),
        )?))
    }

    // 27.3.1.1 GeneratorFunction ( ...parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-generatorfunction
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let marked_arguments = MarkedVec::with_capacity(vm, vm.argument_count());
        for index in 0..vm.argument_count() {
            marked_arguments.push(vm.argument(index));
        }
        let arguments = marked_arguments.to_vec();

        let parameter_args = &arguments[..arguments.len().saturating_sub(1)];

        // 1. Let C be the active function object.
        let constructor = vm
            .active_function_object()
            .expect("a native function is the active function object");

        // 2. If bodyArg is not present, set bodyArg to the empty String.
        let body_arg = arguments
            .last()
            .copied()
            .unwrap_or_else(|| Value::from_string(vm.empty_string()));

        // 3. Return ? CreateDynamicFunction(C, NewTarget, generator, parameterArgs, bodyArg).
        Ok(FunctionConstructor::create_dynamic_function(
            vm,
            constructor,
            Some(new_target),
            FunctionKind::Generator,
            parameter_args,
            body_arg,
        )?
        .upcast())
    }
}
