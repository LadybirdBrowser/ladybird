/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::call_function_object;
use crate::runtime::array::Array;
use crate::runtime::completion::{Completion, Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::generator_object::IterationResult as HelperIterationResult;
use crate::runtime::iterator::{
    IterationResult, IteratorRecord, PrimitiveHandling, get_iterator_direct, get_iterator_flattenable, iterator_close,
    iterator_step, iterator_step_value, setter_that_ignores_prototype_properties, try_or_close_iterator,
};
use crate::runtime::iterator_helper::{IteratorHelper, IteratorHelperClosure};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::this_object;
use crate::runtime::realm::Realm;
use crate::runtime::value_conversions::MAX_ARRAY_LIKE_INDEX;
use crate::utf16::{Utf16StringBuilder, Utf16View};

/// %Iterator.prototype%.
#[repr(C)]
#[derive(Trace)]
pub struct IteratorPrototype {
    base: Object,
}

define_object_class!(IteratorPrototype, extends: [Object], methods: {
    initialize: IteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn current_realm(vm: &Vm) -> Gc<Realm> {
    vm.current_realm()
        .expect("a built-in function runs in an execution context with a realm")
}

/// « iterated », the [[UnderlyingIterators]] of the helpers that read one iterator.
fn underlying_iterators_of_one(vm: &Vm, iterated: Gc<IteratorRecord>) -> MarkedVec<'_, Gc<IteratorRecord>> {
    let underlying_iterators = MarkedVec::new(vm);
    underlying_iterators.push(iterated);
    underlying_iterators
}

/// Iterator::IterationResult { value, false }: what an iterator helper yields.
fn yield_value(value: Value) -> HelperIterationResult {
    HelperIterationResult::new(value, false)
}

/// Iterator::IterationResult { undefined, true }: ReturnCompletion(undefined), with which an iterator helper is done.
fn return_undefined() -> HelperIterationResult {
    HelperIterationResult::new(Value::UNDEFINED, true)
}

impl IteratorPrototype {
    // 27.1.2 The %IteratorPrototype% Object, https://tc39.es/ecma262/#sec-%iteratorprototype%-object
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<IteratorPrototype> {
        realm.create_object(
            vm,
            IteratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define = |property_key: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, property_key, function, length, attr, None);
        };
        define(
            &PropertyKey::from(vm.well_known_symbols().iterator),
            raw_native!(IteratorPrototype::symbol_iterator),
            0,
        );
        define(&names.drop, raw_native!(IteratorPrototype::drop), 1);
        define(&names.every, raw_native!(IteratorPrototype::every), 1);
        define(&names.filter, raw_native!(IteratorPrototype::filter), 1);
        define(&names.find, raw_native!(IteratorPrototype::find), 1);
        define(&names.flatMap, raw_native!(IteratorPrototype::flat_map), 1);
        define(&names.forEach, raw_native!(IteratorPrototype::for_each), 1);
        define(&names.join, raw_native!(IteratorPrototype::join), 1);
        define(&names.map, raw_native!(IteratorPrototype::map), 1);
        define(&names.reduce, raw_native!(IteratorPrototype::reduce), 1);
        define(&names.some, raw_native!(IteratorPrototype::some), 1);
        define(&names.take, raw_native!(IteratorPrototype::take), 1);
        define(&names.toArray, raw_native!(IteratorPrototype::to_array), 0);

        // 27.1.4.1 Iterator.prototype.constructor, https://tc39.es/ecma262/#sec-iterator.prototype.constructor
        object.define_native_accessor(
            vm,
            realm,
            &names.constructor,
            raw_native!(IteratorPrototype::constructor_getter),
            raw_native!(IteratorPrototype::constructor_setter),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        // 27.1.4.14 Iterator.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma262/#sec-iterator.prototype-%symbol.tostringtag%
        object.define_native_accessor(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            raw_native!(IteratorPrototype::to_string_tag_getter),
            raw_native!(IteratorPrototype::to_string_tag_setter),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 27.1.4.1.1 get Iterator.prototype.constructor, https://tc39.es/ecma262/#sec-get-iterator.prototype.constructor
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn constructor_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        // 1. Return %Iterator%.
        Ok(Value::from_object(realm.intrinsics().iterator_constructor(vm)))
    }

    // 27.1.4.1.2 set Iterator.prototype.constructor, https://tc39.es/ecma262/#sec-set-iterator.prototype.constructor
    fn constructor_setter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        // 1. Perform ? SetterThatIgnoresPrototypeProperties(this value, %Iterator.prototype%, "constructor", v).
        setter_that_ignores_prototype_properties(
            vm,
            vm.this_value(),
            realm.intrinsics().iterator_prototype(vm),
            &vm.names.constructor,
            vm.argument(0),
        )?;

        // 2. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 27.1.4.2 Iterator.prototype.drop ( limit ), https://tc39.es/ecma262/#sec-iterator.prototype.drop
    fn drop(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        let limit = vm.argument(0);

        // 1. Let obj be the this value.
        // 2. If obj is not an Object, throw a TypeError exception.
        let object = this_object(vm)?;

        // 3. Let iterated be the Iterator Record { [[Iterator]]: obj, [[NextMethod]]: undefined, [[Done]]: false }.
        let mut iterated = IteratorRecord::create(vm, Some(object), Value::UNDEFINED, false);

        // 4. Let numberLimit be Completion(ToNumber(limit)).
        // 5. IfAbruptCloseIterator(numberLimit, iterated).
        let number_limit = try_or_close_iterator!(vm, &iterated, limit.to_number(vm));

        // 6. If numberLimit is NaN, then
        if number_limit.is_nan() {
            // a. Let error be ThrowCompletion(a newly created RangeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::RangeError, ErrorType::NumberIsNaN, &[&"limit"]);

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 7. If numberLimit is finite and numberLimit > 𝔽(2**53 - 1), then
        if number_limit.is_finite_number() && number_limit.as_f64() > MAX_ARRAY_LIKE_INDEX {
            // a. Let error be ThrowCompletion(a newly created RangeError object).
            let error = vm.throw_completion::<Value>(
                ErrorKind::RangeError,
                ErrorType::NumberIsLargerThanMaxSafeNumber,
                &[&"limit"],
            );

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 8. Let intLimit be ! ToIntegerOrInfinity(numberLimit).
        let integer_limit = number_limit.to_integer_or_infinity(vm).must();

        // 9. If intLimit < 0, then
        if integer_limit < 0.0 {
            // a. Let error be ThrowCompletion(a newly created RangeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::RangeError, ErrorType::NumberIsNegative, &[&"limit"]);

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 10. Set iterated to ? GetIteratorDirect(obj).
        iterated = get_iterator_direct(vm, object)?;

        // 11. Let closure be a new Abstract Closure with no parameters that captures iterated and intLimit and performs the
        //     following steps when called:
        let closure = IteratorHelperClosure::Drop {
            iterated,
            integer_limit,
        };

        // 12. Let result be CreateIteratorFromClosure(closure, "Iterator Helper", %IteratorHelperPrototype%, « [[UnderlyingIterators]] »).
        // 13. Set result.[[UnderlyingIterators]] to « iterated ».
        let result = IteratorHelper::create(vm, realm, &underlying_iterators_of_one(vm, iterated), closure);

        // 14. Return result.
        Ok(Value::from_object(result))
    }

    // 27.1.4.3 Iterator.prototype.every ( predicate ), https://tc39.es/ecma262/#sec-iterator.prototype.every
    fn every(vm: &Vm) -> ThrowCompletionOr<Value> {
        let predicate = vm.argument(0);

        // 1. Let obj be the this value.
        // 2. If obj is not an Object, throw a TypeError exception.
        let object = this_object(vm)?;

        // 3. Let iterated be the Iterator Record { [[Iterator]]: obj, [[NextMethod]]: undefined, [[Done]]: false }.
        let mut iterated = IteratorRecord::create(vm, Some(object), Value::UNDEFINED, false);

        // 4. If IsCallable(predicate) is false, then
        if !predicate.is_function() {
            // a. Let error be ThrowCompletion(a newly created TypeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::TypeError, ErrorType::NotAFunction, &[&"predicate"]);

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 5. Set iterated to ? GetIteratorDirect(obj).
        iterated = get_iterator_direct(vm, object)?;

        // 6. Let counter be +0𝔽.
        // 7. Repeat,
        let mut counter: f64 = 0.0;
        loop {
            // a. Let value be ? IteratorStepValue(iterated).
            let value = iterator_step_value(vm, &iterated)?;

            // b. If value is DONE, return true.
            let Some(value) = value else {
                return Ok(Value::TRUE);
            };

            // c. Let result be Completion(Call(predicate, undefined, « value, counter »)).
            // d. IfAbruptCloseIterator(result, iterated).
            let result = try_or_close_iterator!(
                vm,
                &iterated,
                call_function_object(
                    vm,
                    predicate.as_function(),
                    Value::UNDEFINED,
                    &[value, Value::from_f64(counter)]
                )
            );

            // e. If ToBoolean(result) is false, return ? IteratorClose(iterated, NormalCompletion(false)).
            if !result.to_boolean() {
                return iterator_close(vm, &iterated, Completion::normal(Value::FALSE)).into_throw_completion_or();
            }

            // f. NOTE: The following step will not change counter once it reaches 2**53𝔽.
            // g. Set counter to counter + 1𝔽.
            counter += 1.0;
        }
    }

    // 27.1.4.4 Iterator.prototype.filter ( predicate ), https://tc39.es/ecma262/#sec-iterator.prototype.filter
    fn filter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        let predicate = vm.argument(0);

        // 1. Let obj be the this value.
        // 2. If obj is not an Object, throw a TypeError exception.
        let object = this_object(vm)?;

        // 3. Let iterated be the Iterator Record { [[Iterator]]: obj, [[NextMethod]]: undefined, [[Done]]: false }.
        let mut iterated = IteratorRecord::create(vm, Some(object), Value::UNDEFINED, false);

        // 4. If IsCallable(predicate) is false, then
        if !predicate.is_function() {
            // a. Let error be ThrowCompletion(a newly created TypeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::TypeError, ErrorType::NotAFunction, &[&"predicate"]);

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 5. Set iterated to ? GetIteratorDirect(obj).
        iterated = get_iterator_direct(vm, object)?;

        // 6. Let closure be a new Abstract Closure with no parameters that captures iterated and predicate and performs the
        //    following steps when called:
        let closure = IteratorHelperClosure::Filter {
            iterated,
            predicate: predicate.as_function(),
        };

        // 7. Let result be CreateIteratorFromClosure(closure, "Iterator Helper", %IteratorHelperPrototype%, « [[UnderlyingIterators]] »).
        // 8. Set result.[[UnderlyingIterators]] to « iterated ».
        let result = IteratorHelper::create(vm, realm, &underlying_iterators_of_one(vm, iterated), closure);

        // 9. Return result.
        Ok(Value::from_object(result))
    }

    // 27.1.4.5 Iterator.prototype.find ( predicate ), https://tc39.es/ecma262/#sec-iterator.prototype.find
    fn find(vm: &Vm) -> ThrowCompletionOr<Value> {
        let predicate = vm.argument(0);

        // 1. Let obj be the this value.
        // 2. If obj is not an Object, throw a TypeError exception.
        let object = this_object(vm)?;

        // 3. Let iterated be the Iterator Record { [[Iterator]]: obj, [[NextMethod]]: undefined, [[Done]]: false }.
        let mut iterated = IteratorRecord::create(vm, Some(object), Value::UNDEFINED, false);

        // 4. If IsCallable(predicate) is false, then
        if !predicate.is_function() {
            // a. Let error be ThrowCompletion(a newly created TypeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::TypeError, ErrorType::NotAFunction, &[&"predicate"]);

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 5. Set iterated to ? GetIteratorDirect(obj).
        iterated = get_iterator_direct(vm, object)?;

        // 6. Let counter be +0𝔽.
        // 7. Repeat,
        let mut counter: f64 = 0.0;
        loop {
            // a. Let value be ? IteratorStepValue(iterated).
            let value = iterator_step_value(vm, &iterated)?;

            // b. If value is DONE, return undefined.
            let Some(value) = value else {
                return Ok(Value::UNDEFINED);
            };

            // c. Let result be Completion(Call(predicate, undefined, « value, counter »)).
            // d. IfAbruptCloseIterator(result, iterated).
            let result = try_or_close_iterator!(
                vm,
                &iterated,
                call_function_object(
                    vm,
                    predicate.as_function(),
                    Value::UNDEFINED,
                    &[value, Value::from_f64(counter)]
                )
            );

            // e. If ToBoolean(result) is true, return ? IteratorClose(iterated, NormalCompletion(value)).
            if result.to_boolean() {
                return iterator_close(vm, &iterated, Completion::normal(value)).into_throw_completion_or();
            }

            // f. NOTE: The following step will not change counter once it reaches 2**53𝔽.
            // g. Set counter to counter + 1𝔽.
            counter += 1.0;
        }
    }

    // 27.1.4.6 Iterator.prototype.flatMap ( mapper ), https://tc39.es/ecma262/#sec-iterator.prototype.flatmap
    fn flat_map(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        let mapper = vm.argument(0);

        // 1. Let obj be the this value.
        // 2. If obj is not an Object, throw a TypeError exception.
        let object = this_object(vm)?;

        // 3. Let iterated be the Iterator Record { [[Iterator]]: obj, [[NextMethod]]: undefined, [[Done]]: false }.
        let mut iterated = IteratorRecord::create(vm, Some(object), Value::UNDEFINED, false);

        // 4. If IsCallable(mapper) is false, then
        if !mapper.is_function() {
            // a. Let error be ThrowCompletion(a newly created TypeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::TypeError, ErrorType::NotAFunction, &[&"mapper"]);

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 5. Set iterated to ? GetIteratorDirect(obj).
        iterated = get_iterator_direct(vm, object)?;

        let flat_map_iterator = FlatMapIterator::create(vm);

        // 6. Let closure be a new Abstract Closure with no parameters that captures iterated and mapper and performs the
        //    following steps when called:
        let closure = IteratorHelperClosure::FlatMap {
            iterated,
            flat_map_iterator,
            mapper: mapper.as_function(),
        };

        // 7. Let result be CreateIteratorFromClosure(closure, "Iterator Helper", %IteratorHelperPrototype%, « [[UnderlyingIterators]] »).
        // 8. Set result.[[UnderlyingIterators]] to « iterated ».
        let result = IteratorHelper::create(vm, realm, &underlying_iterators_of_one(vm, iterated), closure);

        // 9. Return result.
        Ok(Value::from_object(result))
    }

    // 27.1.4.7 Iterator.prototype.forEach ( procedure ), https://tc39.es/ecma262/#sec-iterator.prototype.foreach
    fn for_each(vm: &Vm) -> ThrowCompletionOr<Value> {
        let procedure = vm.argument(0);

        // 1. Let obj be the this value.
        // 2. If obj is not an Object, throw a TypeError exception.
        let object = this_object(vm)?;

        // 3. Let iterated be the Iterator Record { [[Iterator]]: obj, [[NextMethod]]: undefined, [[Done]]: false }.
        let mut iterated = IteratorRecord::create(vm, Some(object), Value::UNDEFINED, false);

        // 4. If IsCallable(procedure) is false, then
        if !procedure.is_function() {
            // a. Let error be ThrowCompletion(a newly created TypeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::TypeError, ErrorType::NotAFunction, &[&"procedure"]);

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 5. Set iterated to ? GetIteratorDirect(obj).
        iterated = get_iterator_direct(vm, object)?;

        // 6. Let counter be +0𝔽.
        // 7. Repeat,
        let mut counter: f64 = 0.0;
        loop {
            // a. Let value be ? IteratorStepValue(iterated).
            let value = iterator_step_value(vm, &iterated)?;

            // b. If value is DONE, return undefined.
            let Some(value) = value else {
                return Ok(Value::UNDEFINED);
            };

            // c. Let result be Completion(Call(procedure, undefined, « value, counter »)).
            // d. IfAbruptCloseIterator(result, iterated).
            try_or_close_iterator!(
                vm,
                &iterated,
                call_function_object(
                    vm,
                    procedure.as_function(),
                    Value::UNDEFINED,
                    &[value, Value::from_f64(counter)]
                )
            );

            // e. NOTE: The following step will not change counter once it reaches 2**53𝔽.
            // f. Set counter to counter + 1𝔽.
            counter += 1.0;
        }
    }

    // 27.1.3.3.8 Iterator.prototype.join ( separator ), https://tc39.es/ecma262/#sec-iterator.prototype.join
    fn join(vm: &Vm) -> ThrowCompletionOr<Value> {
        let separator = vm.argument(0);

        // 1. Let obj be the this value.
        // 2. If obj is not an Object, throw a TypeError exception.
        let object = this_object(vm)?;

        // 3. Let iterated be the Iterator Record { [[Iterator]]: obj, [[NextMethod]]: undefined, [[Done]]: false }.
        let mut iterated = IteratorRecord::create(vm, Some(object), Value::UNDEFINED, false);

        // 4. If separator is undefined, then
        let separator_string = if separator.is_undefined() {
            // a. Let separatorString be ",".
            Utf16String::from_utf8(",")
        }
        // 5. Else,
        else {
            // a. Let separatorString be Completion(ToString(separator)).
            // b. IfAbruptCloseIterator(separatorString, iterated).
            try_or_close_iterator!(vm, &iterated, separator.to_utf16_string(vm))
        };

        // 6. Set iterated to ? GetIteratorDirect(obj).
        iterated = get_iterator_direct(vm, object)?;

        // 7. Let result be the empty String.
        let mut result = Utf16StringBuilder::new();

        // 8. Let first be true.
        let mut first = true;

        // 9. Repeat,
        loop {
            // a. Let value be ? IteratorStepValue(iterated).
            let value = iterator_step_value(vm, &iterated)?;

            // b. If value is DONE, return result.
            let Some(value) = value else {
                return Ok(Value::from_string(PrimitiveString::create(
                    vm,
                    result.to_utf16_string(),
                )));
            };

            // c. If first is true, then
            if first {
                // i. Set first to false.
                first = false;
            }
            // d. Else,
            else {
                // i. Set result to the string-concatenation of result and separatorString.
                result.append(Utf16View::of_string(&separator_string));
            }

            // e. If value is neither undefined nor null, then
            if !value.is_nullish() {
                // i. Let valueString be Completion(ToString(value)).
                // ii. IfAbruptCloseIterator(valueString, iterated).
                let value_string = try_or_close_iterator!(vm, &iterated, value.to_utf16_string(vm));

                // iii. Set result to the string-concatenation of result and valueString.
                result.append(Utf16View::of_string(&value_string));
            }
        }
    }

    // 27.1.4.9 Iterator.prototype.map ( mapper ), https://tc39.es/ecma262/#sec-iterator.prototype.map
    fn map(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        let mapper = vm.argument(0);

        // 1. Let obj be the this value.
        // 2. If obj is not an Object, throw a TypeError exception.
        let object = this_object(vm)?;

        // 3. Let iterated be the Iterator Record { [[Iterator]]: obj, [[NextMethod]]: undefined, [[Done]]: false }.
        let mut iterated = IteratorRecord::create(vm, Some(object), Value::UNDEFINED, false);

        // 4. If IsCallable(mapper) is false, then
        if !mapper.is_function() {
            // a. Let error be ThrowCompletion(a newly created TypeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::TypeError, ErrorType::NotAFunction, &[&"mapper"]);

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 5. Set iterated to ? GetIteratorDirect(obj).
        iterated = get_iterator_direct(vm, object)?;

        // 6. Let closure be a new Abstract Closure with no parameters that captures iterated and mapper and performs the
        //    following steps when called:
        let closure = IteratorHelperClosure::Map {
            iterated,
            mapper: mapper.as_function(),
        };

        // 7. Let result be CreateIteratorFromClosure(closure, "Iterator Helper", %IteratorHelperPrototype%, « [[UnderlyingIterators]] »).
        // 8. Set result.[[UnderlyingIterators]] to « iterated ».
        let result = IteratorHelper::create(vm, realm, &underlying_iterators_of_one(vm, iterated), closure);

        // 9. Return result.
        Ok(Value::from_object(result))
    }

    // 27.1.4.10 Iterator.prototype.reduce ( reducer [ , initialValue ] ), https://tc39.es/ecma262/#sec-iterator.prototype.reduce
    fn reduce(vm: &Vm) -> ThrowCompletionOr<Value> {
        let reducer = vm.argument(0);

        // 1. Let obj be the this value.
        // 2. If obj is not an Object, throw a TypeError exception.
        let object = this_object(vm)?;

        // 3. Let iterated be the Iterator Record { [[Iterator]]: obj, [[NextMethod]]: undefined, [[Done]]: false }.
        let mut iterated = IteratorRecord::create(vm, Some(object), Value::UNDEFINED, false);

        // 4. If IsCallable(reducer) is false, then
        if !reducer.is_function() {
            // a. Let error be ThrowCompletion(a newly created TypeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::TypeError, ErrorType::NotAFunction, &[&"reducer"]);

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 5. Set iterated to ? GetIteratorDirect(obj).
        iterated = get_iterator_direct(vm, object)?;

        let mut accumulator;
        let mut counter: f64;

        // 6. If initialValue is not present, then
        if vm.argument_count() < 2 {
            // a. Let accumulator be ? IteratorStepValue(iterated).
            let maybe_accumulator = iterator_step_value(vm, &iterated)?;

            // b. If accumulator is DONE, throw a TypeError exception.
            let Some(first_value) = maybe_accumulator else {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::ReduceNoInitial, &[]);
            };
            accumulator = first_value;

            // c. Let counter be 1𝔽.
            counter = 1.0;
        }
        // 7. Else,
        else {
            // a. Let accumulator be initialValue.
            accumulator = vm.argument(1);

            // b. Let counter be +0𝔽.
            counter = 0.0;
        }

        // 8. Repeat,
        loop {
            // a. Let value be ? IteratorStepValue(iterated).
            let value = iterator_step_value(vm, &iterated)?;

            // b. If value is DONE, return accumulator.
            let Some(value) = value else {
                return Ok(accumulator);
            };

            // c. Let result be Completion(Call(reducer, undefined, « accumulator, value, counter »)).
            // d. IfAbruptCloseIterator(result, iterated).
            let result = try_or_close_iterator!(
                vm,
                &iterated,
                call_function_object(
                    vm,
                    reducer.as_function(),
                    Value::UNDEFINED,
                    &[accumulator, value, Value::from_f64(counter)]
                )
            );

            // e. Set accumulator to result.
            accumulator = result;

            // f. NOTE: The following step will not change counter once it reaches 2**53𝔽.
            // g. Set counter to counter + 1𝔽.
            counter += 1.0;
        }
    }

    // 27.1.4.11 Iterator.prototype.some ( predicate ), https://tc39.es/ecma262/#sec-iterator.prototype.some
    fn some(vm: &Vm) -> ThrowCompletionOr<Value> {
        let predicate = vm.argument(0);

        // 1. Let obj be the this value.
        // 2. If obj is not an Object, throw a TypeError exception.
        let object = this_object(vm)?;

        // 3. Let iterated be the Iterator Record { [[Iterator]]: obj, [[NextMethod]]: undefined, [[Done]]: false }.
        let mut iterated = IteratorRecord::create(vm, Some(object), Value::UNDEFINED, false);

        // 4. If IsCallable(predicate) is false, then
        if !predicate.is_function() {
            // a. Let error be ThrowCompletion(a newly created TypeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::TypeError, ErrorType::NotAFunction, &[&"predicate"]);

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 5. Set iterated to ? GetIteratorDirect(obj).
        iterated = get_iterator_direct(vm, object)?;

        // 6. Let counter be +0𝔽.
        // 7. Repeat,
        let mut counter: f64 = 0.0;
        loop {
            // a. Let value be ? IteratorStepValue(iterated).
            let value = iterator_step_value(vm, &iterated)?;

            // b. If value is DONE, return false.
            let Some(value) = value else {
                return Ok(Value::FALSE);
            };

            // c. Let result be Completion(Call(predicate, undefined, « value, counter »)).
            // d. IfAbruptCloseIterator(result, iterated).
            let result = try_or_close_iterator!(
                vm,
                &iterated,
                call_function_object(
                    vm,
                    predicate.as_function(),
                    Value::UNDEFINED,
                    &[value, Value::from_f64(counter)]
                )
            );

            // e. If ToBoolean(result) is true, return ? IteratorClose(iterated, NormalCompletion(true)).
            if result.to_boolean() {
                return iterator_close(vm, &iterated, Completion::normal(Value::TRUE)).into_throw_completion_or();
            }

            // f. NOTE: The following step will not change counter once it reaches 2**53𝔽.
            // g. Set counter to counter + 1𝔽.
            counter += 1.0;
        }
    }

    // 27.1.4.12 Iterator.prototype.take ( limit ), https://tc39.es/ecma262/#sec-iterator.prototype.take
    fn take(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        let limit = vm.argument(0);

        // 1. Let obj be the this value.
        // 2. If obj is not an Object, throw a TypeError exception.
        let object = this_object(vm)?;

        // 3. Let iterated be the Iterator Record { [[Iterator]]: obj, [[NextMethod]]: undefined, [[Done]]: false }.
        let mut iterated = IteratorRecord::create(vm, Some(object), Value::UNDEFINED, false);

        // 4. Let numberLimit be Completion(ToNumber(limit)).
        // 5. IfAbruptCloseIterator(numberLimit, iterated).
        let number_limit = try_or_close_iterator!(vm, &iterated, limit.to_number(vm));

        // 6. If numberLimit is NaN, then
        if number_limit.is_nan() {
            // a. Let error be ThrowCompletion(a newly created RangeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::RangeError, ErrorType::NumberIsNaN, &[&"limit"]);

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 7. If numberLimit is finite and numberLimit > 𝔽(2**53 - 1), then
        if number_limit.is_finite_number() && number_limit.as_f64() > MAX_ARRAY_LIKE_INDEX {
            // a. Let error be ThrowCompletion(a newly created RangeError object).
            let error = vm.throw_completion::<Value>(
                ErrorKind::RangeError,
                ErrorType::NumberIsLargerThanMaxSafeNumber,
                &[&"limit"],
            );

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 8. Let intLimit be ! ToIntegerOrInfinity(numberLimit).
        let integer_limit = number_limit.to_integer_or_infinity(vm).must();

        // 9. If intLimit < 0, then
        if integer_limit < 0.0 {
            // a. Let error be ThrowCompletion(a newly created RangeError object).
            let error = vm.throw_completion::<Value>(ErrorKind::RangeError, ErrorType::NumberIsNegative, &[&"limit"]);

            // b. Return ? IteratorClose(iterated, error).
            return iterator_close(vm, &iterated, Completion::from(error)).into_throw_completion_or();
        }

        // 10. Set iterated to ? GetIteratorDirect(obj).
        iterated = get_iterator_direct(vm, object)?;

        // 11. Let closure be a new Abstract Closure with no parameters that captures iterated and intLimit and performs the
        //     following steps when called:
        let closure = IteratorHelperClosure::Take {
            iterated,
            integer_limit,
        };

        // 12. Let result be CreateIteratorFromClosure(closure, "Iterator Helper", %IteratorHelperPrototype%, « [[UnderlyingIterators]] »).
        // 13. Set result.[[UnderlyingIterators]] to « iterated ».
        let result = IteratorHelper::create(vm, realm, &underlying_iterators_of_one(vm, iterated), closure);

        // 14. Return result.
        Ok(Value::from_object(result))
    }

    // 27.1.4.13 Iterator.prototype.toArray ( ), https://tc39.es/ecma262/#sec-iterator.prototype.toarray
    fn to_array(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        // 1. Let obj be the this value.
        // 2. If obj is not an Object, throw a TypeError exception.
        let object = this_object(vm)?;

        // 3. Let iterated be ? GetIteratorDirect(obj).
        let iterated = get_iterator_direct(vm, object)?;

        // 4. Let items be a new empty List.
        let items = MarkedVec::new(vm);

        // 5. Repeat,
        loop {
            // a. Let value be ? IteratorStepValue(iterated).
            let value = iterator_step_value(vm, &iterated)?;

            // b. If value is DONE, return CreateArrayFromList(items).
            let Some(value) = value else {
                return Ok(Value::from_object(Array::create_from_list(vm, realm, &items)));
            };

            // c. Append value to items.
            items.push(value);
        }
    }

    // 27.1.4.15 Iterator.prototype [ %Symbol.iterator% ] ( ), https://tc39.es/ecma262/#sec-iterator.prototype-%symbol.iterator%
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn symbol_iterator(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return the this value.
        Ok(vm.this_value())
    }

    // 27.1.4.16.1 get Iterator.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma262/#sec-get-iterator.prototype-%symbol.tostringtag%
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn to_string_tag_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return "Iterator".
        Ok(Value::from_string(PrimitiveString::create_from_fly_string(
            vm,
            vm.names.Iterator.as_string(),
        )))
    }

    // 27.1.4.16.2 set Iterator.prototype [ %Symbol.toStringTag% ], https://tc39.es/ecma262/#sec-set-iterator.prototype-%symbol.tostringtag%
    fn to_string_tag_setter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = current_realm(vm);

        // 1. Perform ? SetterThatIgnoresPrototypeProperties(this value, %Iterator.prototype%, %Symbol.toStringTag%, v).
        setter_that_ignores_prototype_properties(
            vm,
            vm.this_value(),
            realm.intrinsics().iterator_prototype(vm),
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            vm.argument(0),
        )?;

        // 2. Return undefined.
        Ok(Value::UNDEFINED)
    }
}

/// The closure of Iterator.prototype.drop, step 11.
pub fn drop_closure(
    vm: &Vm,
    iterator: &IteratorHelper,
    iterated: Gc<IteratorRecord>,
    integer_limit: f64,
) -> ThrowCompletionOr<HelperIterationResult> {
    // a. Let remaining be 𝔽(intLimit).
    // b. Repeat, while remaining > +0𝔽,
    while iterator.counter() < integer_limit {
        // i. Set remaining to remaining - 1𝔽.
        iterator.increment_counter();

        // ii. Let next be ? IteratorStep(iterated).
        let next = iterator_step(vm, &iterated)?;

        // iii. If next is DONE, return ReturnCompletion(undefined).
        if matches!(next, IterationResult::Done) {
            return Ok(return_undefined());
        }
    }

    // c. Repeat,

    // i. Let value be ? IteratorStepValue(iterated).
    let value = iterator_step_value(vm, &iterated)?;

    // ii. If value is DONE, return ReturnCompletion(undefined).
    let Some(value) = value else {
        return Ok(return_undefined());
    };

    // iii. Let completion be Completion(Yield(value)).
    // iv. IfAbruptCloseIterator(completion, iterated).
    Ok(yield_value(value))
}

/// The closure of Iterator.prototype.filter, step 6.
pub fn filter_closure(
    vm: &Vm,
    iterator: &IteratorHelper,
    iterated: Gc<IteratorRecord>,
    predicate: Gc<FunctionObject>,
) -> ThrowCompletionOr<HelperIterationResult> {
    // a. Let counter be +0𝔽.
    // b. Repeat,
    loop {
        // i. Let value be ? IteratorStepValue(iterated).
        let value = iterator_step_value(vm, &iterated)?;

        // ii. If value is DONE, return ReturnCompletion(undefined).
        let Some(value) = value else {
            return Ok(return_undefined());
        };

        // iii. Let selected be Completion(Call(predicate, undefined, « value, counter »)).
        // iv. IfAbruptCloseIterator(selected, iterated).
        let selected = try_or_close_iterator!(
            vm,
            &iterated,
            call_function_object(
                vm,
                predicate,
                Value::UNDEFINED,
                &[value, Value::from_f64(iterator.counter())]
            )
        );

        // vi. NOTE: The following step will not change counter once it reaches 2**53𝔽.
        // vii. Set counter to counter + 1𝔽.
        // NB: We do this step early to ensure it occurs before returning.
        iterator.increment_counter();

        // v. If ToBoolean(selected) is true, then
        if selected.to_boolean() {
            // 1. Let completion be Completion(Yield(value)).
            // 2. IfAbruptCloseIterator(completion, iterated).
            return Ok(yield_value(value));
        }
    }
}

/// The closure of Iterator.prototype.map, step 6.
pub fn map_closure(
    vm: &Vm,
    iterator: &IteratorHelper,
    iterated: Gc<IteratorRecord>,
    mapper: Gc<FunctionObject>,
) -> ThrowCompletionOr<HelperIterationResult> {
    // a. Let counter be +0𝔽.
    // b. Repeat,

    // i. Let value be ? IteratorStepValue(iterated).
    let value = iterator_step_value(vm, &iterated)?;

    // ii. If value is DONE, return ReturnCompletion(undefined).
    let Some(value) = value else {
        return Ok(return_undefined());
    };

    // iii. Let mapped be Completion(Call(mapper, undefined, « value, counter »)).
    // iv. IfAbruptCloseIterator(mapped, iterated).
    let mapped = try_or_close_iterator!(
        vm,
        &iterated,
        call_function_object(
            vm,
            mapper,
            Value::UNDEFINED,
            &[value, Value::from_f64(iterator.counter())]
        )
    );

    // vii. NOTE: The following step will not change counter once it reaches 2**53𝔽.
    // viii. Set counter to counter + 1𝔽.
    // NB: We do this step early to ensure it occurs before returning.
    iterator.increment_counter();

    // v. Let completion be Completion(Yield(mapped)).
    // vi. IfAbruptCloseIterator(completion, iterated).
    Ok(yield_value(mapped))
}

/// The closure of Iterator.prototype.take, step 11.
pub fn take_closure(
    vm: &Vm,
    iterator: &IteratorHelper,
    iterated: Gc<IteratorRecord>,
    integer_limit: f64,
) -> ThrowCompletionOr<HelperIterationResult> {
    // a. Let remaining be 𝔽(intLimit).
    // b. Repeat,

    // i. If remaining is +0𝔽, then
    if iterator.counter() >= integer_limit {
        // 1. Return ? IteratorClose(iterated, ReturnCompletion(undefined)).
        let close_result =
            iterator_close(vm, &iterated, Completion::normal(Value::UNDEFINED)).into_throw_completion_or()?;
        return Ok(HelperIterationResult::new(close_result, true));
    }

    // ii. Set remaining to remaining - 1𝔽.
    iterator.increment_counter();

    // iii. Let value be ? IteratorStepValue(iterated).
    let value = iterator_step_value(vm, &iterated)?;

    // iv. If value is DONE, return ReturnCompletion(undefined).
    let Some(value) = value else {
        return Ok(return_undefined());
    };

    // v. Let completion be Completion(Yield(value)).
    // vi. IfAbruptCloseIterator(completion, iterated).
    Ok(yield_value(value))
}

/// The state of the closure of Iterator.prototype.flatMap: the inner iterator it is flattening.
#[repr(C)]
#[derive(Trace)]
pub struct FlatMapIterator {
    header: CellHeader,
    inner_iterator: Cell<Option<Gc<IteratorRecord>>>,
}

define_cell!(FlatMapIterator, Other);

impl FlatMapIterator {
    fn create(vm: &Vm) -> Gc<FlatMapIterator> {
        vm.heap().allocate(FlatMapIterator {
            header: CellHeader::for_class(Self::CLASS),
            inner_iterator: Cell::new(None),
        })
    }

    pub fn next(
        &self,
        vm: &Vm,
        iterated: Gc<IteratorRecord>,
        iterator: &IteratorHelper,
        mapper: Gc<FunctionObject>,
    ) -> ThrowCompletionOr<HelperIterationResult> {
        if self.inner_iterator.get().is_some() {
            return self.next_inner_iterator(vm, iterated, iterator, mapper);
        }
        self.next_outer_iterator(vm, iterated, iterator, mapper)
    }

    // NB: This implements step 6.b.vii.4.b of Iterator.prototype.flatMap.
    pub fn on_abrupt_completion(
        &self,
        vm: &Vm,
        iterated: Gc<IteratorRecord>,
        completion: Completion,
    ) -> ThrowCompletionOr<Value> {
        let inner_iterator = self
            .inner_iterator
            .get()
            .expect("a suspended flatMap helper has an inner iterator");

        // b. If completion is an abrupt completion, then
        //     i. Let backupCompletion be Completion(IteratorClose(innerIterator, completion)).
        //     ii. IfAbruptCloseIterator(backupCompletion, iterated).
        try_or_close_iterator!(
            vm,
            &iterated,
            iterator_close(vm, &inner_iterator, completion).into_throw_completion_or()
        );

        //     iii. Return ? IteratorClose(iterated, completion).
        iterator_close(vm, &iterated, completion).into_throw_completion_or()
    }

    fn next_outer_iterator(
        &self,
        vm: &Vm,
        iterated: Gc<IteratorRecord>,
        iterator: &IteratorHelper,
        mapper: Gc<FunctionObject>,
    ) -> ThrowCompletionOr<HelperIterationResult> {
        // i. Let value be ? IteratorStepValue(iterated).
        let value = iterator_step_value(vm, &iterated)?;

        // ii. If value is DONE, return ReturnCompletion(undefined).
        let Some(value) = value else {
            return Ok(return_undefined());
        };

        // iii. Let mapped be Completion(Call(mapper, undefined, « value, counter »)).
        // iv. IfAbruptCloseIterator(mapped, iterated).
        let mapped = try_or_close_iterator!(
            vm,
            &iterated,
            call_function_object(
                vm,
                mapper,
                Value::UNDEFINED,
                &[value, Value::from_f64(iterator.counter())]
            )
        );

        // v. Let innerIterator be Completion(GetIteratorFlattenable(mapped, REJECT-PRIMITIVES)).
        // vi. IfAbruptCloseIterator(innerIterator, iterated).
        let inner_iterator = try_or_close_iterator!(
            vm,
            &iterated,
            get_iterator_flattenable(vm, mapped, PrimitiveHandling::RejectPrimitives)
        );

        // vii. Let innerAlive be true.
        self.inner_iterator.set(Some(inner_iterator));

        // ix. NOTE: The following step will not change counter once it reaches 2**53𝔽.
        // x. Set counter to counter + 1𝔽.
        // NB: We do this step early to ensure it occurs before returning.
        iterator.increment_counter();

        // viii. Repeat, while innerAlive is true,
        self.next_inner_iterator(vm, iterated, iterator, mapper)
    }

    fn next_inner_iterator(
        &self,
        vm: &Vm,
        iterated: Gc<IteratorRecord>,
        iterator: &IteratorHelper,
        mapper: Gc<FunctionObject>,
    ) -> ThrowCompletionOr<HelperIterationResult> {
        let inner_iterator = self
            .inner_iterator
            .get()
            .expect("the flatMap helper is flattening an inner iterator");

        // 1. Let innerValue be Completion(IteratorStepValue(innerIterator)).
        // 2. IfAbruptCloseIterator(innerValue, iterated).
        let inner_value = try_or_close_iterator!(vm, &iterated, iterator_step_value(vm, &inner_iterator));

        // 3. If innerValue is DONE, then
        let Some(inner_value) = inner_value else {
            // a. Set innerAlive to false.
            self.inner_iterator.set(None);

            return self.next_outer_iterator(vm, iterated, iterator, mapper);
        };

        // 4. Else,
        // a. Let completion be Completion(Yield(innerValue)).
        // NB: Step b is implemented via on_abrupt_completion.
        Ok(yield_value(inner_value))
    }
}
