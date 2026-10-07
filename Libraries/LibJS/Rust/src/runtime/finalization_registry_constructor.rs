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
use crate::runtime::abstract_operations::ordinary_create_from_constructor_of;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::finalization_registry::FinalizationRegistry;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct FinalizationRegistryConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    FinalizationRegistryConstructor,
    initialize: FinalizationRegistryConstructor::initialize,
    call: FinalizationRegistryConstructor::call,
    construct: FinalizationRegistryConstructor::construct
);

impl FinalizationRegistryConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<FinalizationRegistryConstructor> {
        realm.create_object(
            vm,
            FinalizationRegistryConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.FinalizationRegistry.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 26.2.2.1 FinalizationRegistry.prototype, https://tc39.es/ecma262/#sec-finalization-registry.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().finalization_registry_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 26.2.1.1 FinalizationRegistry ( cleanupCallback ), https://tc39.es/ecma262/#sec-finalization-registry-cleanup-callback
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"FinalizationRegistry"],
        )
    }

    // 26.2.1.1 FinalizationRegistry ( cleanupCallback ), https://tc39.es/ecma262/#sec-finalization-registry-cleanup-callback
    fn construct(function: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");

        // 2. If IsCallable(cleanupCallback) is false, throw a TypeError exception.
        let cleanup_callback = vm.argument(0);
        if !cleanup_callback.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&cleanup_callback]);
        }

        // 3. Let finalizationRegistry be ? OrdinaryCreateFromConstructor(NewTarget, "%FinalizationRegistry.prototype%", « [[Realm]], [[CleanupCallback]], [[Cells]] »).
        // 4. Let fn be the active function object.
        // NOTE: This is not necessary, the active function object is `this`.
        // 5. Set finalizationRegistry.[[Realm]] to fn.[[Realm]].
        // 6. Set finalizationRegistry.[[CleanupCallback]] to HostMakeJobCallback(cleanupCallback).
        // 7. Set finalizationRegistry.[[Cells]] to a new empty List.
        // NOTE: This is done inside FinalizationRegistry instead of here.
        // 8. Return finalizationRegistry.
        let function_realm = function.realm();
        let cleanup_job_callback = vm.host_make_job_callback()(vm, cleanup_callback.as_function());
        let finalization_registry = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            Intrinsics::finalization_registry_prototype,
            |prototype| FinalizationRegistry::new(vm, function_realm, cleanup_job_callback, prototype),
        )?;
        Ok(finalization_registry.upcast())
    }
}
