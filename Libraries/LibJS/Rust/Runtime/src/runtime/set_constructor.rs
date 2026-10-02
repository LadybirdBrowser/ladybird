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
use crate::runtime::abstract_operations::{call_function_object, ordinary_create_from_constructor_of};
use crate::runtime::completion::{Completion, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::iterator::get_iterator_values;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::set::Set;

#[repr(C)]
#[derive(Trace)]
pub struct SetConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    SetConstructor,
    initialize: SetConstructor::initialize,
    call: SetConstructor::call,
    construct: SetConstructor::construct
);

impl SetConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<SetConstructor> {
        realm.create_object(
            vm,
            SetConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Set.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 24.2.2.1 Set.prototype, https://tc39.es/ecma262/#sec-set.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().set_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_native_accessor(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().species),
            raw_native!(SetConstructor::symbol_species_getter),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 24.2.1.1 Set ( [ iterable ] ), https://tc39.es/ecma262/#sec-set-iterable
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(ErrorKind::TypeError, ErrorType::ConstructorWithoutNew, &[&"Set"])
    }

    // 24.2.1.1 Set ( [ iterable ] ), https://tc39.es/ecma262/#sec-set-iterable
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let iterable = vm.argument(0);

        // 2. Let set be ? OrdinaryCreateFromConstructor(NewTarget, "%Set.prototype%", « [[SetData]] »).
        let set = ordinary_create_from_constructor_of(vm, realm, new_target, Intrinsics::set_prototype, |prototype| {
            Set::new(vm, prototype)
        })?;

        // 3. Set set.[[SetData]] to a new empty List.

        // 4. If iterable is either undefined or null, return set.
        if iterable.is_nullish() {
            return Ok(set.upcast());
        }

        // 5. Let adder be ? Get(set, "add").
        let adder = set.get(vm, &vm.names.add)?;

        // 6. If IsCallable(adder) is false, throw a TypeError exception.
        if !adder.is_function() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::NotAFunction,
                &[&"'add' property of Set"],
            );
        }
        let adder = adder.as_function();

        // 7. Let iteratorRecord be ? GetIterator(iterable, sync).
        // 8. Repeat,
        get_iterator_values(vm, iterable, |next| {
            // a. Let next be ? IteratorStepValue(iteratorRecord).
            // b. If next is DONE, return set.
            // c. Let status be Completion(Call(adder, set, « nextValue »)).
            // d. IfAbruptCloseIterator(status, iteratorRecord).
            call_function_object(vm, adder, Value::from_object(set), &[next])
                .err()
                .map(Completion::from)
        })
        .into_throw_completion_or()?;

        // b. If next is false, return set.
        Ok(set.upcast())
    }

    // 24.2.2.2 get Set [ @@species ], https://tc39.es/ecma262/#sec-get-set-@@species
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn symbol_species_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return the this value.
        Ok(vm.this_value())
    }
}
