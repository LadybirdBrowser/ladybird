/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::get_prototype_from_constructor;
use crate::runtime::boolean_object::BooleanObject;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
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
    #[allow(clippy::unnecessary_wraps, reason = "[[Call]] can throw for other functions")]
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        let value = vm.argument(0);

        // 1. Let b be ToBoolean(value).
        let b = value.to_boolean();

        // 2. If NewTarget is undefined, return b.
        Ok(Value::from_bool(b))
    }

    // 20.3.1.1 Boolean ( value ), https://tc39.es/ecma262/#sec-boolean-constructor-boolean-value
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let value = vm.argument(0);

        // 1. Let b be ToBoolean(value).
        let b = value.to_boolean();

        // 3. Let O be ? OrdinaryCreateFromConstructor(NewTarget, "%Boolean.prototype%", « [[BooleanData]] »).
        // 4. Set O.[[BooleanData]] to b.
        // 5. Return O.
        let prototype = get_prototype_from_constructor(vm, new_target, Intrinsics::boolean_prototype)?;
        Ok(realm
            .create_object(vm, BooleanObject::new(vm, BooleanObject::CLASS, b, prototype))
            .upcast())
    }
}
