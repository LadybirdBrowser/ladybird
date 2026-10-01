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
pub struct ObjectConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    ObjectConstructor,
    initialize: ObjectConstructor::initialize,
    call: ObjectConstructor::call,
    construct: ObjectConstructor::construct
);

impl ObjectConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ObjectConstructor> {
        realm.create_object(
            vm,
            ObjectConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Object.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 20.1.2.21 Object.prototype, https://tc39.es/ecma262/#sec-object.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.object_prototype()),
            PropertyAttributes::new(0),
        );

        // NB: The functions of the Object constructor come with the Object builtins.

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 20.1.1.1 Object ( [ value ] ), https://tc39.es/ecma262/#sec-object-value
    fn call(_: &NativeFunction, _: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function("ObjectConstructor::call, the [[Call]] of %Object%", 0)
    }

    // 20.1.1.1 Object ( [ value ] ), https://tc39.es/ecma262/#sec-object-value
    fn construct(_: &NativeFunction, _: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        unimplemented_runtime_function("ObjectConstructor::construct, the [[Construct]] of %Object%", 0)
    }
}
