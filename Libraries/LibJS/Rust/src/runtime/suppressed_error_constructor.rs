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
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::runtime::suppressed_error::SuppressedError;

#[repr(C)]
#[derive(Trace)]
pub struct SuppressedErrorConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    SuppressedErrorConstructor,
    initialize: SuppressedErrorConstructor::initialize,
    call: SuppressedErrorConstructor::call,
    construct: SuppressedErrorConstructor::construct
);

impl SuppressedErrorConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<SuppressedErrorConstructor> {
        realm.create_object(vm, |_| SuppressedErrorConstructor {
            base: NativeFunction::new_with_prototype(
                vm,
                Self::CLASS,
                realm,
                realm.intrinsics().error_constructor(vm).upcast(),
                None,
            ),
        })
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 20.5.8.2.1 SuppressedError.prototype, https://tc39.es/ecma262/#sec-suppressederror.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().suppressed_error_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(3),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 20.5.8.1.1 SuppressedError ( error, suppressed, message ), https://tc39.es/ecma262/#sec-suppressederror
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, let newTarget be the active function object; else let newTarget be NewTarget.
        Ok(Value::from_object(Self::construct(
            function,
            vm,
            function.as_function_object_gc(),
        )?))
    }

    // 20.5.8.1.1 SuppressedError ( error, suppressed, message ), https://tc39.es/ecma262/#sec-suppressederror
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");

        let error = vm.argument(0);
        let suppressed = vm.argument(1);
        let message = vm.argument(2);

        // 2. Let obj be ? OrdinaryCreateFromConstructor(newTarget, "%SuppressedError.prototype%", « [[ErrorData]] »).
        let suppressed_error = ordinary_create_from_constructor_of(
            vm,
            realm,
            new_target,
            Intrinsics::suppressed_error_prototype,
            |_, prototype| SuppressedError::new_in_constructor(vm, realm, prototype),
        )?;

        // 3. If message is not undefined, then
        if !message.is_undefined() {
            // a. Let messageString be ? ToString(message).
            let message_string = message.to_utf16_string(vm)?;

            // b. Perform CreateNonEnumerableDataPropertyOrThrow(obj, "message", messageString).
            suppressed_error.create_non_enumerable_data_property_or_throw(
                vm,
                &vm.names.message,
                Value::from_string(PrimitiveString::create(vm, message_string)),
            );
        }

        // 4. Perform CreateNonEnumerableDataPropertyOrThrow(obj, "error", error).
        suppressed_error.create_non_enumerable_data_property_or_throw(vm, &vm.names.error, error);

        // 5. Perform CreateNonEnumerableDataPropertyOrThrow(obj, "suppressed", suppressed).
        suppressed_error.create_non_enumerable_data_property_or_throw(vm, &vm.names.suppressed, suppressed);

        // 6. Return obj.
        Ok(suppressed_error.upcast())
    }
}
