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
pub struct IteratorConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    IteratorConstructor,
    initialize: IteratorConstructor::initialize,
    call: IteratorConstructor::call,
    construct: IteratorConstructor::construct
);

impl IteratorConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<IteratorConstructor> {
        realm.create_object(
            vm,
            IteratorConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Iterator.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 27.1.3.2.3 Iterator.prototype Iterator.prototype, https://tc39.es/ecma262/#sec-iterator.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().iterator_prototype(vm)),
            PropertyAttributes::new(0),
        );

        // NB: Iterator.concat, Iterator.from, Iterator.zip and Iterator.zipKeyed come with the Iterator builtins.

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 27.1.3.1.1 Iterator ( ), https://tc39.es/ecma262/#sec-iterator
    fn call(_: &NativeFunction, _: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function("IteratorConstructor::call, the [[Call]] of %Iterator%", 0)
    }

    // 27.1.3.1.1 Iterator ( ), https://tc39.es/ecma262/#sec-iterator
    fn construct(_: &NativeFunction, _: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        unimplemented_runtime_function("IteratorConstructor::construct, the [[Construct]] of %Iterator%", 0)
    }
}
