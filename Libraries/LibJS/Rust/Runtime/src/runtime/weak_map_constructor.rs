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
use crate::runtime::native_function::{NativeFunction, define_native_function_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::weak_map::WeakMap;

#[repr(C)]
#[derive(Trace)]
pub struct WeakMapConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    WeakMapConstructor,
    initialize: WeakMapConstructor::initialize,
    call: WeakMapConstructor::call,
    construct: WeakMapConstructor::construct
);

impl WeakMapConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<WeakMapConstructor> {
        realm.create_object(
            vm,
            WeakMapConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.WeakMap.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 24.3.2.1 WeakMap.prototype, https://tc39.es/ecma262/#sec-weakmap.prototype
        object.define_direct_property(
            vm,
            &vm.names.prototype,
            Value::from_object(realm.intrinsics().weak_map_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &vm.names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 24.3.1.1 WeakMap ( [ iterable ] ), https://tc39.es/ecma262/#sec-weakmap-iterable
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(ErrorKind::TypeError, ErrorType::ConstructorWithoutNew, &[&"WeakMap"])
    }

    // 24.3.1.1 WeakMap ( [ iterable ] ), https://tc39.es/ecma262/#sec-weakmap-iterable
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");
        let iterable = vm.argument(0);

        // 2. Let map be ? OrdinaryCreateFromConstructor(NewTarget, "%WeakMap.prototype%", « [[WeakMapData]] »).
        // 3. Set map.[[WeakMapData]] to a new empty List.
        let map =
            ordinary_create_from_constructor_of(vm, realm, new_target, Intrinsics::weak_map_prototype, |prototype| {
                WeakMap::new(vm, prototype)
            })?;

        // 4. If iterable is either undefined or null, return map.
        if iterable.is_nullish() {
            return Ok(map.upcast());
        }

        // 5. Let adder be ? Get(map, "set").
        let adder = map.get(vm, &vm.names.set)?;

        // 6. If IsCallable(adder) is false, throw a TypeError exception.
        if !adder.is_function() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::NotAFunction,
                &[&"'set' property of WeakMap"],
            );
        }
        let adder = adder.as_function();

        // 7. Return ? AddEntriesFromIterable(map, iterable, adder).
        let add_entry = |iterator_value: Value| -> ThrowCompletionOr<()> {
            if iterator_value.is_object() {
                let object = iterator_value.as_object();
                let key = object.get(vm, &PropertyKey::from(0))?;
                let value = object.get(vm, &PropertyKey::from(1))?;
                call_function_object(vm, adder, Value::from_object(map), &[key, value])?;
                return Ok(());
            }

            vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::NotAnObject,
                &[&format!("Iterator value {iterator_value}")],
            )
        };
        get_iterator_values(vm, iterable, |iterator_value| {
            add_entry(iterator_value).err().map(Completion::from)
        })
        .into_throw_completion_or()?;

        Ok(map.upcast())
    }
}
