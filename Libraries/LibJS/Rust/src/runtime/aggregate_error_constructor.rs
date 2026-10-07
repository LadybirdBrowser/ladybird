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
use crate::runtime::aggregate_error::AggregateError;
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::iterator::{IteratorHint, get_iterator_impl, iterator_to_list};
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_descriptor::PropertyDescriptor;
use crate::runtime::realm::Realm;

#[repr(C)]
#[derive(Trace)]
pub struct AggregateErrorConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    AggregateErrorConstructor,
    initialize: AggregateErrorConstructor::initialize,
    call: AggregateErrorConstructor::call,
    construct: AggregateErrorConstructor::construct
);

impl AggregateErrorConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AggregateErrorConstructor> {
        realm.create_object(
            vm,
            AggregateErrorConstructor {
                base: NativeFunction::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().error_constructor(vm).upcast(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 20.5.7.2.1 AggregateError.prototype, https://tc39.es/ecma262/#sec-aggregate-error.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().aggregate_error_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(2),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 20.5.7.1.1 AggregateError ( errors, message [ , options ] ), https://tc39.es/ecma262/#sec-aggregate-error
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, let newTarget be the active function object; else let newTarget be NewTarget.
        Ok(Value::from_object(Self::construct(
            function,
            vm,
            function.as_function_object_gc(),
        )?))
    }

    // 20.5.7.1.1 AggregateError ( errors, message [ , options ] ), https://tc39.es/ecma262/#sec-aggregate-error
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");

        let errors = vm.argument(0);
        let message = vm.argument(1);
        let options = vm.argument(2);

        // 2. Let O be ? OrdinaryCreateFromConstructor(newTarget, "%AggregateError.prototype%", « [[ErrorData]] »).
        let prototype = get_prototype_from_constructor(vm, new_target, Intrinsics::aggregate_error_prototype)?;
        let aggregate_error = realm.create_object(vm, AggregateError::new(vm, prototype));

        // 3. If message is not undefined, then
        if !message.is_undefined() {
            // a. Let msg be ? ToString(message).
            let msg = message.to_utf16_string(vm)?;

            // b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "message", msg).
            aggregate_error.create_non_enumerable_data_property_or_throw(
                vm,
                &vm.names.message,
                Value::from_string(PrimitiveString::create(vm, msg)),
            );
        }

        // 4. Perform ? InstallErrorCause(O, options).
        aggregate_error.install_error_cause(vm, options)?;

        // 5. Let errorsList be ? IteratorToList(? GetIterator(errors, sync)).
        let errors_list = iterator_to_list(vm, &get_iterator_impl(vm, errors, IteratorHint::Sync)?)?;

        // 6. Perform ! DefinePropertyOrThrow(O, "errors", PropertyDescriptor { [[Configurable]]: true, [[Enumerable]]: false, [[Writable]]: true, [[Value]]: CreateArrayFromList(errorsList) }).
        let mut descriptor = PropertyDescriptor {
            value: Some(Value::from_object(Array::create_from_list(vm, realm, &errors_list))),
            writable: Some(true),
            enumerable: Some(false),
            configurable: Some(true),
            ..Default::default()
        };
        aggregate_error
            .define_property_or_throw(vm, &vm.names.errors, &mut descriptor)
            .must();

        // 7. Return O.
        Ok(aggregate_error.upcast())
    }
}
