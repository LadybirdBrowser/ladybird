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
pub struct BooleanConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    BooleanConstructor,
    initialize: BooleanConstructor::initialize,
    call: BooleanConstructor::call,
    construct: BooleanConstructor::construct
);

impl BooleanConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<BooleanConstructor> {
        realm.create_object(
            vm,
            BooleanConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Boolean.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 20.3.2.1 Boolean.prototype, https://tc39.es/ecma262/#sec-boolean.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().boolean_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 20.3.1.1 Boolean ( value ), https://tc39.es/ecma262/#sec-boolean-constructor-boolean-value
    fn call(_: &NativeFunction, _: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function("BooleanConstructor::call, the [[Call]] of %Boolean%", 0)
    }

    // 20.3.1.1 Boolean ( value ), https://tc39.es/ecma262/#sec-boolean-constructor-boolean-value
    fn construct(_: &NativeFunction, _: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        unimplemented_runtime_function("BooleanConstructor::construct, the [[Construct]] of %Boolean%", 0)
    }
}
