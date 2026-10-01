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
pub struct FunctionConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    FunctionConstructor,
    initialize: FunctionConstructor::initialize,
    call: FunctionConstructor::call,
    construct: FunctionConstructor::construct
);

impl FunctionConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<FunctionConstructor> {
        realm.create_object(
            vm,
            FunctionConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Function.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 20.2.2.2 Function.prototype, https://tc39.es/ecma262/#sec-function.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.function_prototype()),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 20.2.1.1 Function ( p1, p2, … , pn, body ), https://tc39.es/ecma262/#sec-function-p1-p2-pn-body
    fn call(_: &NativeFunction, _: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function("FunctionConstructor::call, the [[Call]] of %Function%", 0)
    }

    // 20.2.1.1 Function ( ...parameterArgs, bodyArg ), https://tc39.es/ecma262/#sec-function-p1-p2-pn-body
    fn construct(_: &NativeFunction, _: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        unimplemented_runtime_function("FunctionConstructor::construct, the [[Construct]] of %Function%", 0)
    }
}
