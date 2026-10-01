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
pub struct ProxyConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    ProxyConstructor,
    initialize: ProxyConstructor::initialize,
    call: ProxyConstructor::call,
    construct: ProxyConstructor::construct
);

impl ProxyConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ProxyConstructor> {
        realm.create_object(
            vm,
            ProxyConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Proxy.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, _realm: Gc<Realm>) {
        // NB: Proxy.revocable comes with the Proxy builtins.

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(2),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 28.2.1.1 Proxy ( target, handler ), https://tc39.es/ecma262/#sec-proxy-target-handler
    fn call(_: &NativeFunction, _: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function("ProxyConstructor::call, the [[Call]] of %Proxy%", 0)
    }

    // 28.2.1.1 Proxy ( target, handler ), https://tc39.es/ecma262/#sec-proxy-target-handler
    fn construct(_: &NativeFunction, _: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        unimplemented_runtime_function("ProxyConstructor::construct, the [[Construct]] of %Proxy%", 0)
    }
}
