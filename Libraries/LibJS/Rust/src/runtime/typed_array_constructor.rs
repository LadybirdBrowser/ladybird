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
use crate::runtime::abstract_operations::{call_function_object, length_of_array_like};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::iterator::{get_iterator_from_method_impl, iterator_to_list};
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::object::ShouldThrowExceptions;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::typed_array::typed_array_create;

/// %TypedArray%.
#[repr(C)]
#[derive(Trace)]
pub struct TypedArrayConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    TypedArrayConstructor,
    initialize: TypedArrayConstructor::initialize,
    call: TypedArrayConstructor::call,
    construct: TypedArrayConstructor::construct
);

impl TypedArrayConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<TypedArrayConstructor> {
        realm.create_object(
            vm,
            TypedArrayConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.TypedArray.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 23.2.2.3 %TypedArray%.prototype, https://tc39.es/ecma262/#sec-%typedarray%.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().typed_array_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.from,
            raw_native!(TypedArrayConstructor::from),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.of,
            raw_native!(TypedArrayConstructor::of),
            0,
            attr,
            None,
        );

        object.define_native_accessor(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().species),
            raw_native!(TypedArrayConstructor::symbol_species_getter),
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

    // 23.2.1.1 %TypedArray% ( ), https://tc39.es/ecma262/#sec-%typedarray%
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        Ok(Value::from_object(Self::construct(
            function,
            vm,
            function.as_function_object_gc(),
        )?))
    }

    // 23.2.1.1 %TypedArray% ( ), https://tc39.es/ecma262/#sec-%typedarray%
    fn construct(_: &NativeFunction, vm: &Vm, _: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        // 1. Throw a TypeError exception.
        vm.throw_completion(ErrorKind::TypeError, ErrorType::ClassIsAbstract, &[&"TypedArray"])
    }

    // 23.2.2.1 %TypedArray%.from ( source [ , mapfn [ , thisArg ] ] ), https://tc39.es/ecma262/#sec-%typedarray%.from
    fn from(vm: &Vm) -> ThrowCompletionOr<Value> {
        let source = vm.argument(0);
        let map_fn_value = vm.argument(1);
        let this_arg = vm.argument(2);

        // 1. Let C be the this value.
        let constructor = vm.this_value();

        // 2. If IsConstructor(C) is false, throw a TypeError exception.
        if !constructor.is_constructor() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAConstructor, &[&constructor]);
        }

        // 3. If mapfn is undefined, let mapping be false.
        let mut map_fn = None;

        // 4. Else,
        if !map_fn_value.is_undefined() {
            // a. If IsCallable(mapfn) is false, throw a TypeError exception.
            if !map_fn_value.is_function() {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&map_fn_value]);
            }

            // b. Let mapping be true.
            map_fn = Some(map_fn_value.as_function());
        }

        // 5. Let usingIterator be ? GetMethod(source, @@iterator).
        let using_iterator = source.get_method(vm, &PropertyKey::from(vm.well_known_symbols().iterator))?;

        // 6. If usingIterator is not undefined, then
        if let Some(using_iterator) = using_iterator {
            // a. Let values be ? IteratorToList(? GetIteratorFromMethod(source, usingIterator)).
            let values = iterator_to_list(vm, &get_iterator_from_method_impl(vm, source, using_iterator)?)?;

            // b. Let len be the number of elements in values.
            let length = values.len();

            // c. Let targetObj be ? TypedArrayCreate(C, « 𝔽(len) »).
            let target_object = typed_array_create(vm, constructor.as_function(), &[Value::from_f64(length as f64)])?;

            // d. Let k be 0.
            // e. Repeat, while k < len,
            for k in 0..length {
                // i. Let Pk be ! ToString(𝔽(k)).
                let property_key = PropertyKey::from_number(k as u64);

                // ii. Let kValue be the first element of values.
                // iii. Remove the first element from values.
                let k_value = values.get(k).expect("the index is in bounds");

                // iv. If mapping is true, then
                let mapped_value = if let Some(map_fn) = map_fn {
                    // 1. Let mappedValue be ? Call(mapfn, thisArg, « kValue, 𝔽(k) »).
                    call_function_object(vm, map_fn, this_arg, &[k_value, Value::from_f64(k as f64)])?
                }
                // v. Else, let mappedValue be kValue.
                else {
                    k_value
                };

                // vi. Perform ? Set(targetObj, Pk, mappedValue, true).
                target_object.set(vm, &property_key, mapped_value, ShouldThrowExceptions::Yes)?;

                // vii. Set k to k + 1.
            }

            // f. Assert: values is now an empty List.
            // NOTE: We don't actually empty the list.

            // g. Return targetObj.
            return Ok(Value::from_object(target_object));
        }

        // 7. NOTE: source is not an Iterable so assume it is already an array-like object.
        // 8. Let arrayLike be ! ToObject(source).
        let array_like = source.to_object(vm).must();

        // 9. Let len be ? LengthOfArrayLike(arrayLike).
        let length = length_of_array_like(vm, &array_like)?;

        // 10. Let targetObj be ? TypedArrayCreate(C, « 𝔽(len) »).
        let target_object = typed_array_create(vm, constructor.as_function(), &[Value::from_f64(length as f64)])?;

        // 11. Let k be 0.
        // 12. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from_number(k);

            // b. Let kValue be ? Get(arrayLike, Pk).
            let k_value = array_like.get(vm, &property_key)?;

            // c. If mapping is true, then
            let mapped_value = if let Some(map_fn) = map_fn {
                // i. Let mappedValue be ? Call(mapfn, thisArg, « kValue, 𝔽(k) »).
                call_function_object(vm, map_fn, this_arg, &[k_value, Value::from_f64(k as f64)])?
            }
            // d. Else, let mappedValue be kValue.
            else {
                k_value
            };

            // e. Perform ? Set(targetObj, Pk, mappedValue, true).
            target_object.set(vm, &property_key, mapped_value, ShouldThrowExceptions::Yes)?;

            // f. Set k to k + 1.
        }

        // 13. Return targetObj.
        Ok(Value::from_object(target_object))
    }

    // 23.2.2.2 %TypedArray%.of ( ...items ), https://tc39.es/ecma262/#sec-%typedarray%.of
    fn of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let len be the number of elements in items.
        let length = vm.argument_count();

        // 2. Let C be the this value.
        let constructor = vm.this_value();

        // 3. If IsConstructor(C) is false, throw a TypeError exception.
        if !constructor.is_constructor() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAConstructor, &[&constructor]);
        }

        // 4. Let newObj be ? TypedArrayCreate(C, « 𝔽(len) »).
        let new_object = typed_array_create(vm, constructor.as_function(), &[Value::from_f64(length as f64)])?;

        // 5. Let k be 0.
        // 6. Repeat, while k < len,
        for k in 0..length {
            // a. Let kValue be items[k].
            let k_value = vm.argument(k);

            // b. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from_number(k as u64);

            // c. Perform ? Set(newObj, Pk, kValue, true).
            new_object.set(vm, &property_key, k_value, ShouldThrowExceptions::Yes)?;

            // d. Set k to k + 1.
        }

        // 7. Return newObj.
        Ok(Value::from_object(new_object))
    }

    // 23.2.2.4 get %TypedArray% [ @@species ], https://tc39.es/ecma262/#sec-get-%typedarray%-@@species
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn symbol_species_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return the this value.
        Ok(vm.this_value())
    }
}
