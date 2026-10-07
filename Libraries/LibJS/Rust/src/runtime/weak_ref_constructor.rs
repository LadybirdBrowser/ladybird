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
use crate::runtime::abstract_operations::{can_be_held_weakly, ordinary_create_from_constructor_of};
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::runtime::weak_ref::WeakRef;

#[repr(C)]
#[derive(Trace)]
pub struct WeakRefConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    WeakRefConstructor,
    initialize: WeakRefConstructor::initialize,
    call: WeakRefConstructor::call,
    construct: WeakRefConstructor::construct
);

impl WeakRefConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<WeakRefConstructor> {
        realm.create_object(
            vm,
            WeakRefConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.WeakRef.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 26.1.2.1 WeakRef.prototype, https://tc39.es/ecma262/#sec-weak-ref.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().weak_ref_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 26.1.1.1 WeakRef ( target ), https://tc39.es/ecma262/#sec-weak-ref-target
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(ErrorKind::TypeError, ErrorType::ConstructorWithoutNew, &[&"WeakRef"])
    }

    // 26.1.1.1 WeakRef ( target ), https://tc39.es/ecma262/#sec-weak-ref-target
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let target = vm.argument(0);

        // 2. If CanBeHeldWeakly(target) is false, throw a TypeError exception.
        if !can_be_held_weakly(target) {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::CannotBeHeldWeakly, &[&target]);
        }

        // 3. Let weakRef be ? OrdinaryCreateFromConstructor(NewTarget, "%WeakRef.prototype%", « [[WeakRefTarget]] »).
        // 4. Perform AddToKeptObjects(target).
        // 5. Set weakRef.[[WeakRefTarget]] to target.
        // 6. Return weakRef.
        let weak_ref =
            ordinary_create_from_constructor_of(vm, realm, new_target, Intrinsics::weak_ref_prototype, |prototype| {
                WeakRef::new(vm, target, prototype)
            })?;
        Ok(weak_ref.upcast())
    }
}
