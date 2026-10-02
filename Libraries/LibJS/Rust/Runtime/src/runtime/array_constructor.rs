/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::bytecode::executable::StaticPropertyLookupCacheSite;
use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    call_function_object, construct, get_prototype_from_constructor, length_of_array_like,
};
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::iterator::{
    get_iterator_from_method_impl, iterator_close, iterator_step_value, try_or_close_iterator,
};
use crate::runtime::native_function::{NativeFunction, RawNativeFunction, define_native_function_class, raw_native};
use crate::runtime::object::ShouldThrowExceptions;
use crate::runtime::property_attributes::{Attribute, DEFAULT_ATTRIBUTES, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::value_conversions::MAX_ARRAY_LIKE_INDEX;

#[repr(C)]
#[derive(Trace)]
pub struct ArrayConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    ArrayConstructor,
    initialize: ArrayConstructor::initialize,
    call: ArrayConstructor::call,
    construct: ArrayConstructor::construct
);

impl ArrayConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ArrayConstructor> {
        realm.create_object(
            vm,
            ArrayConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Array.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 23.1.2.4 Array.prototype, https://tc39.es/ecma262/#sec-array.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().array_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.from,
            raw_native!(ArrayConstructor::from),
            1,
            attributes,
            None,
        );
        // NB: C++ defines %Array.fromAsync%, which is written in JavaScript and needs promises. This stand-in has its
        //     length, name and attributes, and stops the process when it is called.
        let from_async = RawNativeFunction::create(
            vm,
            raw_native!(ArrayConstructor::from_async),
            1,
            &names.fromAsync,
            Some(realm),
            None,
            None,
        );
        object.define_direct_property(vm, &names.fromAsync, Value::from_object(from_async), attributes);
        object.define_native_function(
            vm,
            realm,
            &names.isArray,
            raw_native!(ArrayConstructor::is_array),
            1,
            attributes,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.of,
            raw_native!(ArrayConstructor::of),
            0,
            attributes,
            None,
        );

        // 23.1.2.5 get Array [ @@species ], https://tc39.es/ecma262/#sec-get-array-@@species
        object.define_native_accessor(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().species),
            raw_native!(ArrayConstructor::symbol_species_getter),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(1),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 23.1.1.1 Array ( ...values ), https://tc39.es/ecma262/#sec-array
    fn call(function: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, let newTarget be the active function object; else let newTarget be NewTarget.
        Ok(Value::from_object(Self::construct(
            function,
            vm,
            function.as_function_object_gc(),
        )?))
    }

    // 23.1.1.1 Array ( ...values ), https://tc39.es/ecma262/#sec-array
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");

        // 2. Let proto be ? GetPrototypeFromConstructor(newTarget, "%Array.prototype%").
        let prototype = get_prototype_from_constructor(vm, new_target, Intrinsics::array_prototype)?;

        // 3. Let numberOfArgs be the number of elements in values.
        // 4. If numberOfArgs = 0, then
        if vm.argument_count() == 0 {
            // a. Return ! ArrayCreate(0, proto).
            return Ok(Array::create(vm, realm, 0, Some(prototype)).must().upcast());
        }

        // 5. Else if numberOfArgs = 1, then
        if vm.argument_count() == 1 {
            // a. Let len be values[0].
            let length = vm.argument(0);

            // b. Let array be ! ArrayCreate(0, proto).
            let array = Array::create(vm, realm, 0, Some(prototype)).must();

            let int_length;

            // c. If len is not a Number, then
            if !length.is_number() {
                // i. Perform ! CreateDataPropertyOrThrow(array, "0", len).
                array
                    .create_data_property_or_throw(vm, &PropertyKey::from(0u32), length)
                    .must();

                // ii. Let intLen be 1𝔽.
                int_length = 1;
            }
            // d. Else,
            else {
                // i. Let intLen be ! ToUint32(len).
                int_length = length.to_u32(vm).must();

                // ii. If SameValueZero(intLen, len) is false, throw a RangeError exception.
                if f64::from(int_length) != length.as_f64() {
                    return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidLength, &[&"array"]);
                }
            }

            // e. Perform ! Set(array, "length", intLen, true).
            array.set(
                vm,
                &vm.names.length,
                Value::from_f64(f64::from(int_length)),
                ShouldThrowExceptions::Yes,
            )?;

            // f. Return array.
            return Ok(array.upcast());
        }

        // 6. Else,

        // a. Assert: numberOfArgs ≥ 2.
        assert!(vm.argument_count() >= 2);

        // b. Let array be ? ArrayCreate(numberOfArgs, proto).
        let array = Array::create(vm, realm, vm.argument_count() as u64, Some(prototype))?;

        // c. Let k be 0.
        // d. Repeat, while k < numberOfArgs,
        for k in 0..vm.argument_count() {
            // i. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from_number(k as u64);

            // ii. Let itemK be values[k].
            let item_k = vm.argument(k);

            // iii. Perform ! CreateDataPropertyOrThrow(array, Pk, itemK).
            array.create_data_property_or_throw(vm, &property_key, item_k).must();

            // iv. Set k to k + 1.
        }

        // e. Assert: The mathematical value of array's "length" property is numberOfArgs.

        // f. Return array.
        Ok(array.upcast())
    }

    // 23.1.2.1 Array.from ( items [ , mapfn [ , thisArg ] ] ), https://tc39.es/ecma262/#sec-array.from
    fn from(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let items = vm.argument(0);
        let mapfn_value = vm.argument(1);
        let this_arg = vm.argument(2);

        // 1. Let C be the this value.
        let constructor = vm.this_value();

        // 2. If mapfn is undefined, let mapping be false.
        let mut mapfn = None;

        // 3. Else,
        if !mapfn_value.is_undefined() {
            // a. If IsCallable(mapfn) is false, throw a TypeError exception.
            if !mapfn_value.is_function() {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&mapfn_value]);
            }

            // b. Let mapping be true.
            mapfn = Some(mapfn_value.as_function());
        }

        // 4. Let usingIterator be ? GetMethod(items, @@iterator).
        let using_iterator = items.get_method_with_cache(
            vm,
            &PropertyKey::from(vm.well_known_symbols().iterator),
            vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::ArrayFromIteratorMethod),
        )?;

        // 5. If usingIterator is not undefined, then
        if let Some(using_iterator) = using_iterator {
            // OPTIMIZATION: If C is %Array%, then Construct(C) is ArrayCreate(0) with %Array.prototype%, which is not
            //               observable. The new array is also unreachable from user code until we return it, so
            //               appending to its indexed storage is equivalent to CreateDataPropertyOrThrow.
            let constructor_is_intrinsic_array = constructor.is_object()
                && constructor.as_object() == realm.intrinsics().array_constructor(vm).upcast::<Object>();

            // NB: C++ copies the storage of a Set or a Map whose iteration is unobservable into the new array here,
            //     which comes with Set and Map.

            // a. If IsConstructor(C) is true, then
            let array = if constructor.is_constructor() && !constructor_is_intrinsic_array {
                // i. Let A be ? Construct(C).
                construct(vm, constructor.as_function(), &[], None)?
            }
            // b. Else,
            else {
                // i. Let A be ! ArrayCreate(0).
                Array::create(vm, realm, 0, None).must().upcast()
            };

            // c. Let iteratorRecord be ? GetIteratorFromMethod(items, usingIterator).
            let iterator = get_iterator_from_method_impl(vm, items, using_iterator)?;

            // d. Let k be 0.
            // e. Repeat,
            let mut k: u64 = 0;
            loop {
                // i. If k ≥ 2^53 - 1, then
                if k as f64 >= MAX_ARRAY_LIKE_INDEX {
                    // 1. Let error be ThrowCompletion(a newly created TypeError object).
                    let error = vm
                        .throw_completion::<()>(ErrorKind::TypeError, ErrorType::ArrayMaxSize, &[])
                        .expect_err("throw_completion throws");

                    // 2. Return ? IteratorClose(iteratorRecord, error).
                    return iterator_close(vm, &iterator, error.into()).into_throw_completion_or();
                }

                // ii. Let Pk be ! ToString(𝔽(k)).
                let property_key = PropertyKey::from_number(k);

                // iii. Let next be ? IteratorStepValue(iteratorRecord).
                let next = iterator_step_value(vm, &iterator)?;

                // iv. If next is DONE, then
                let Some(next) = next else {
                    // 1. Perform ? Set(A, "length", 𝔽(k), true).
                    array.set(
                        vm,
                        &vm.names.length,
                        Value::from_f64(k as f64),
                        ShouldThrowExceptions::Yes,
                    )?;

                    // 2. Return A.
                    return Ok(Value::from_object(array));
                };

                // v. If mapping is true, then
                let mapped_value = if let Some(mapfn) = mapfn {
                    // 1. Let mappedValue be Completion(Call(mapfn, thisArg, « nextValue, 𝔽(k) »)).
                    // 2. IfAbruptCloseIterator(mappedValue, iteratorRecord).
                    try_or_close_iterator!(
                        vm,
                        &iterator,
                        call_function_object(vm, mapfn, this_arg, &[next, Value::from_f64(k as f64)])
                    )
                }
                // vi. Else, let mappedValue be nextValue.
                else {
                    next
                };

                // vii. Let defineStatus be Completion(CreateDataPropertyOrThrow(A, Pk, mappedValue)).
                // viii. IfAbruptCloseIterator(defineStatus, iteratorRecord).
                // OPTIMIZATION: The intrinsic result array is unreachable by user code, so append directly.
                if constructor_is_intrinsic_array && k < u64::from(u32::MAX) {
                    array.indexed_append(mapped_value, DEFAULT_ATTRIBUTES);
                } else {
                    try_or_close_iterator!(
                        vm,
                        &iterator,
                        array.create_data_property_or_throw(vm, &property_key, mapped_value)
                    );
                }

                // ix. Set k to k + 1.
                k += 1;
            }
        }

        // 6. NOTE: items is not an Iterable so assume it is an array-like object.

        // 7. Let arrayLike be ! ToObject(items).
        let array_like = items.to_object(vm).must();

        // 8. Let len be ? LengthOfArrayLike(arrayLike).
        let length = length_of_array_like(vm, &array_like)?;

        // 9. If IsConstructor(C) is true, then
        let array = if constructor.is_constructor() {
            // a. Let A be ? Construct(C, « 𝔽(len) »).
            construct(vm, constructor.as_function(), &[Value::from_f64(length as f64)], None)?
        } else {
            // a. Let A be ? ArrayCreate(len).
            Array::create(vm, realm, length, None)?.upcast()
        };

        // 11. Let k be 0.
        // 12. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from_number(k);

            // b. Let kValue be ? Get(arrayLike, Pk).
            let k_value = array_like.get(vm, &property_key)?;

            // c. If mapping is true, then
            let mapped_value = if let Some(mapfn) = mapfn {
                // i. Let mappedValue be ? Call(mapfn, thisArg, « kValue, 𝔽(k) »).
                call_function_object(vm, mapfn, this_arg, &[k_value, Value::from_f64(k as f64)])?
            }
            // d. Else, let mappedValue be kValue.
            else {
                k_value
            };

            // e. Perform ? CreateDataPropertyOrThrow(A, Pk, mappedValue).
            array.create_data_property_or_throw(vm, &property_key, mapped_value)?;

            // f. Set k to k + 1.
        }

        // 13. Perform ? Set(A, "length", 𝔽(len), true).
        array.set(
            vm,
            &vm.names.length,
            Value::from_f64(length as f64),
            ShouldThrowExceptions::Yes,
        )?;

        // 14. Return A.
        Ok(Value::from_object(array))
    }

    // 23.1.2.2 Array.fromAsync ( asyncItems [ , mapper [ , thisArg ] ] ), https://tc39.es/proposal-array-from-async/#sec-array.fromAsync
    fn from_async(_vm: &Vm) -> ThrowCompletionOr<Value> {
        unimplemented_runtime_function(
            "Array.fromAsync, which C++ writes in JavaScript as an async function and which needs promises",
            0,
        )
    }

    // 23.1.2.2 Array.isArray ( arg ), https://tc39.es/ecma262/#sec-array.isarray
    fn is_array(vm: &Vm) -> ThrowCompletionOr<Value> {
        let arg = vm.argument(0);

        // 1. Return ? IsArray(arg).
        Ok(Value::from_bool(arg.is_array(vm)?))
    }

    // 23.1.2.3 Array.of ( ...items ), https://tc39.es/ecma262/#sec-array.of
    fn of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let len be the number of elements in items.
        let length = vm.argument_count();

        // 2. Let lenNumber be 𝔽(len).
        let length_number = Value::from_f64(length as f64);

        // 3. Let C be the this value.
        let constructor = vm.this_value();

        // 4. If IsConstructor(C) is true, then
        let array = if constructor.is_constructor() {
            // a. Let A be ? Construct(C, « lenNumber »).
            construct(
                vm,
                constructor.as_function(),
                &[Value::from_f64(vm.argument_count() as f64)],
                None,
            )?
        } else {
            // a. Let A be ? ArrayCreate(len).
            Array::create(vm, realm, length as u64, None)?.upcast()
        };

        // 6. Let k be 0.
        // 7. Repeat, while k < len,
        for k in 0..length {
            // a. Let kValue be items[k].
            let k_value = vm.argument(k);

            // b. Let Pk be ! ToString(𝔽(k)).
            let property_key = PropertyKey::from_number(k as u64);

            // c. Perform ? CreateDataPropertyOrThrow(A, Pk, kValue).
            array.create_data_property_or_throw(vm, &property_key, k_value)?;

            // d. Set k to k + 1.
        }

        // 8. Perform ? Set(A, "length", lenNumber, true).
        array.set(vm, &vm.names.length, length_number, ShouldThrowExceptions::Yes)?;

        // 9. Return A.
        Ok(Value::from_object(array))
    }

    // 23.1.2.5 get Array [ @@species ], https://tc39.es/ecma262/#sec-get-array-@@species
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn symbol_species_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return the this value.
        Ok(vm.this_value())
    }
}
