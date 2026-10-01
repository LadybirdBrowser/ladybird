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
pub struct ArrayConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    ArrayConstructor,
    initialize: ArrayConstructor::initialize,
    call: ArrayConstructor::call,
    construct: ArrayConstructor::construct
);

impl ArrayConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ArrayConstructor> {
        realm.create_object(
            vm,
            ArrayConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Array.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 23.1.2.4 Array.prototype, https://tc39.es/ecma262/#sec-array.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().array_prototype(vm)),
            PropertyAttributes::new(0),
        );

        // NB: Array.from, Array.fromAsync, Array.isArray, Array.of and get Array [ @@species ] come with the Array
        //     builtins.

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 23.1.1.1 Array ( ...values ), https://tc39.es/ecma262/#sec-array
    fn call(_: &NativeFunction, _: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function("ArrayConstructor::call, the [[Call]] of %Array%", 0)
    }

    // 23.1.1.1 Array ( ...values ), https://tc39.es/ecma262/#sec-array
    fn construct(_: &NativeFunction, _: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        unimplemented_runtime_function("ArrayConstructor::construct, the [[Construct]] of %Array%", 0)
    }
}
