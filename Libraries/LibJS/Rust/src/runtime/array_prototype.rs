/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::RefCell;
use std::collections::HashSet;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::bytecode::executable::StaticPropertyLookupCacheSite;
use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::{IndexedStorageKind, Object};
use crate::layout::value::Value;
use crate::layout_forward::RawNativeFunctionPointer;
use crate::runtime::abstract_operations::{call_function_object, construct, get_function_realm, length_of_array_like};
use crate::runtime::array::{ARRAY_OBJECT_METHODS, Array, Holes, compare_array_elements, sort_indexed_properties};
use crate::runtime::array_iterator::ArrayIterator;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::{AkDouble, ErrorType};
use crate::runtime::function_object::FunctionObject;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{PropertyKind, ShouldThrowExceptions, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, DEFAULT_ATTRIBUTES, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::value::{is_strictly_equal, same_value_zero};
use crate::runtime::value_conversions::MAX_ARRAY_LIKE_INDEX;
use crate::utf16::Utf16View;

thread_local! {
    /// The objects Array.prototype.join and Array.prototype.toLocaleString are joining, by address. Each join keeps its
    /// object alive while it is in here.
    static ARRAY_JOIN_SEEN_OBJECTS: RefCell<HashSet<usize>> = RefCell::new(HashSet::new());
}

/// Takes an object out of the seen objects of joins when its join returns.
struct UnseeObjectGuard {
    object_address: usize,
}

impl Drop for UnseeObjectGuard {
    fn drop(&mut self) {
        ARRAY_JOIN_SEEN_OBJECTS.with_borrow_mut(|seen_objects| seen_objects.remove(&self.object_address));
    }
}

/// Adds `object` to the seen objects of joins until the guard is dropped, unless a join already sees it.
fn see_object_unless_seen(object: Gc<Object>) -> Option<UnseeObjectGuard> {
    let object_address = object.as_ptr().addr();
    ARRAY_JOIN_SEEN_OBJECTS
        .with_borrow_mut(|seen_objects| seen_objects.insert(object_address))
        .then(|| UnseeObjectGuard { object_address })
}

/// %Array.prototype%, which is an Array exotic object.
#[repr(C)]
#[derive(Trace)]
pub struct ArrayPrototype {
    base: Array,
}

define_object_class!(ArrayPrototype, extends: [Array, Object], methods: {
    initialize: ArrayPrototype::initialize,
    ..ARRAY_OBJECT_METHODS
});

fn property_key(index: u64) -> PropertyKey {
    PropertyKey::from_number(index)
}

fn number(value: u64) -> Value {
    Value::from_f64(value as f64)
}

/// AK::max(a, b) for doubles.
fn max(a: f64, b: f64) -> f64 {
    if a < b { b } else { a }
}

/// AK::min(a, b) for doubles.
fn min(a: f64, b: f64) -> f64 {
    if b < a { b } else { a }
}

/// AK::clamp(value, min, max) for doubles.
fn clamp(value: f64, min: f64, max: f64) -> f64 {
    assert!(max >= min);
    if value > max {
        return max;
    }
    if value < min {
        return min;
    }
    value
}

impl ArrayPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ArrayPrototype> {
        realm.create_object(
            vm,
            ArrayPrototype {
                base: Array::new_with_class(vm, Self::CLASS, realm, realm.object_prototype()),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function: RawNativeFunctionPointer, length: i32| {
            object.define_native_function(vm, realm, name, function, length, attributes, None);
        };

        define_native_function(&names.at, raw_native!(ArrayPrototype::at), 1);
        define_native_function(&names.concat, raw_native!(ArrayPrototype::concat), 1);
        define_native_function(&names.copyWithin, raw_native!(ArrayPrototype::copy_within), 2);
        define_native_function(&names.entries, raw_native!(ArrayPrototype::entries), 0);
        define_native_function(&names.every, raw_native!(ArrayPrototype::every), 1);
        define_native_function(&names.fill, raw_native!(ArrayPrototype::fill), 1);
        define_native_function(&names.filter, raw_native!(ArrayPrototype::filter), 1);
        define_native_function(&names.find, raw_native!(ArrayPrototype::find), 1);
        define_native_function(&names.findIndex, raw_native!(ArrayPrototype::find_index), 1);
        define_native_function(&names.findLast, raw_native!(ArrayPrototype::find_last), 1);
        define_native_function(&names.findLastIndex, raw_native!(ArrayPrototype::find_last_index), 1);
        define_native_function(&names.flat, raw_native!(ArrayPrototype::flat), 0);
        define_native_function(&names.flatMap, raw_native!(ArrayPrototype::flat_map), 1);
        define_native_function(&names.forEach, raw_native!(ArrayPrototype::for_each), 1);
        define_native_function(&names.includes, raw_native!(ArrayPrototype::includes), 1);
        define_native_function(&names.indexOf, raw_native!(ArrayPrototype::index_of), 1);
        define_native_function(&names.join, raw_native!(ArrayPrototype::join), 1);
        define_native_function(&names.keys, raw_native!(ArrayPrototype::keys), 0);
        define_native_function(&names.lastIndexOf, raw_native!(ArrayPrototype::last_index_of), 1);
        define_native_function(&names.map, raw_native!(ArrayPrototype::map), 1);
        define_native_function(&names.pop, raw_native!(ArrayPrototype::pop), 0);
        define_native_function(&names.push, raw_native!(ArrayPrototype::push), 1);
        define_native_function(&names.reduce, raw_native!(ArrayPrototype::reduce), 1);
        define_native_function(&names.reduceRight, raw_native!(ArrayPrototype::reduce_right), 1);
        define_native_function(&names.reverse, raw_native!(ArrayPrototype::reverse), 0);
        define_native_function(&names.shift, raw_native!(ArrayPrototype::shift), 0);
        define_native_function(&names.slice, raw_native!(ArrayPrototype::slice), 2);
        define_native_function(&names.some, raw_native!(ArrayPrototype::some), 1);
        define_native_function(&names.sort, raw_native!(ArrayPrototype::sort), 1);
        define_native_function(&names.splice, raw_native!(ArrayPrototype::splice), 2);
        define_native_function(&names.toLocaleString, raw_native!(ArrayPrototype::to_locale_string), 0);
        define_native_function(&names.toReversed, raw_native!(ArrayPrototype::to_reversed), 0);
        define_native_function(&names.toSorted, raw_native!(ArrayPrototype::to_sorted), 1);
        define_native_function(&names.toSpliced, raw_native!(ArrayPrototype::to_spliced), 2);
        define_native_function(&names.toString, raw_native!(ArrayPrototype::to_string), 0);
        define_native_function(&names.unshift, raw_native!(ArrayPrototype::unshift), 1);
        define_native_function(&names.values, raw_native!(ArrayPrototype::values), 0);
        define_native_function(&names.with, raw_native!(ArrayPrototype::with), 2);

        // Use define_direct_property here instead of define_native_function so that
        // Object.is(Array.prototype[Symbol.iterator], Array.prototype.values)
        // evaluates to true
        // 23.1.3.40 Array.prototype [ @@iterator ] ( ), https://tc39.es/ecma262/#sec-array.prototype-@@iterator
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().iterator),
            object.get_without_side_effects(vm, &names.values),
            attributes,
        );

        // 23.1.3.41 Array.prototype [ @@unscopables ], https://tc39.es/ecma262/#sec-array.prototype-@@unscopables
        let unscopable_list = Object::create(vm, realm, None);
        for name in [
            &names.at,
            &names.copyWithin,
            &names.entries,
            &names.fill,
            &names.find,
            &names.findIndex,
            &names.findLast,
            &names.findLastIndex,
            &names.flat,
            &names.flatMap,
            &names.includes,
            &names.keys,
            &names.toReversed,
            &names.toSorted,
            &names.toSpliced,
            &names.values,
        ] {
            unscopable_list
                .create_data_property_or_throw(vm, name, Value::TRUE)
                .must();
        }

        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().unscopables),
            Value::from_object(unscopable_list),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 23.1.3.1 Array.prototype.at ( index ), https://tc39.es/ecma262/#sec-array.prototype.at
    fn at(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;
        let length = length_of_array_like(vm, &this_object)?;
        let relative_index = vm.argument(0).to_integer_or_infinity(vm)?;
        if relative_index.is_infinite() {
            return Ok(Value::UNDEFINED);
        }
        let index = if relative_index >= 0.0 {
            relative_index
        } else {
            length as f64 + relative_index
        };
        if index < 0.0 || index >= length as f64 {
            return Ok(Value::UNDEFINED);
        }
        this_object.get(vm, &property_key(index as u64))
    }

    // 23.1.3.2 Array.prototype.concat ( ...items ), https://tc39.es/ecma262/#sec-array.prototype.concat
    fn concat(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;

        let new_array = array_species_create(vm, &this_object, 0)?;
        let concat_spreadable_key = PropertyKey::from(vm.well_known_symbols().is_concat_spreadable);

        // OPTIMIZATION: Fast path for packed or empty arrays when ArraySpeciesCreate produced an empty default Array and
        //               every object argument is a packed or empty Array without an own or inherited @@isConcatSpreadable,
        //               so every Array is spread and every other value is appended as a single element.
        //               The result must not be one of the inputs, since we copy input storage into it.
        if let Some(array) = this_object.downcast::<Array>()
            && can_use_packed_or_empty_array_fast_path(&array)
            && !this_object.has_property(vm, &concat_spreadable_key)?
            && let Some(result_array) = fast_array_species_result(new_array)
            && result_array != array
            && result_array.indexed_array_like_size() == 0
            && matches!(
                result_array.indexed_storage_kind(),
                IndexedStorageKind::None | IndexedStorageKind::Packed
            )
        {
            let mut all_fast_path_arguments = true;
            let mut total_length = u64::from(array.indexed_array_like_size());
            for i in 0..vm.argument_count() {
                let arg = vm.argument(i);
                if !arg.is_object() {
                    total_length += 1;
                    continue;
                }

                let argument_array = arg.as_object().downcast::<Array>();
                let Some(argument_array) = argument_array.filter(|argument_array| {
                    *argument_array != result_array && can_use_packed_or_empty_array_fast_path(argument_array)
                }) else {
                    all_fast_path_arguments = false;
                    break;
                };
                if argument_array.has_property(vm, &concat_spreadable_key)? {
                    all_fast_path_arguments = false;
                    break;
                }
                total_length += u64::from(argument_array.indexed_array_like_size());
            }

            if all_fast_path_arguments && total_length <= u64::from(u32::MAX) {
                let append_array = |source: &Array| {
                    if source.indexed_storage_kind() == IndexedStorageKind::Packed {
                        result_array.indexed_append_packed_elements_of(source);
                    }
                };

                append_array(&array);
                for argument_index in 0..vm.argument_count() {
                    let arg = vm.argument(argument_index);
                    if arg.is_object() {
                        append_array(
                            &arg.as_object()
                                .downcast::<Array>()
                                .expect("every object argument of the fast path is an Array"),
                        );
                    } else {
                        result_array.indexed_append(arg, DEFAULT_ATTRIBUTES);
                    }
                }
                return Ok(Value::from_object(result_array));
            }
        }

        let mut n: u64 = 0;

        // 23.1.3.2.1 IsConcatSpreadable ( O ), https://tc39.es/ecma262/#sec-isconcatspreadable
        let is_concat_spreadable = |val: Value| -> ThrowCompletionOr<bool> {
            if !val.is_object() {
                return Ok(false);
            }
            let object = val.as_object();
            let spreadable = object.get(vm, &concat_spreadable_key)?;
            if !spreadable.is_undefined() {
                return Ok(spreadable.to_boolean());
            }

            val.is_array(vm)
        };

        let mut append_to_new_array = |arg: Value| -> ThrowCompletionOr<()> {
            let spreadable = is_concat_spreadable(arg)?;
            if spreadable {
                assert!(arg.is_object());
                let obj = arg.as_object();
                let mut k: u64 = 0;
                let length = length_of_array_like(vm, &obj)?;

                if (n + length) as f64 > MAX_ARRAY_LIKE_INDEX {
                    return vm.throw_completion(ErrorKind::TypeError, ErrorType::ArrayMaxSize, &[]);
                }
                while k < length {
                    let k_exists = obj.has_property(vm, &property_key(k))?;
                    if k_exists {
                        let k_value = obj.get(vm, &property_key(k))?;
                        new_array.create_data_property_or_throw(vm, &property_key(n), k_value)?;
                    }
                    n += 1;
                    k += 1;
                }
            } else {
                if n as f64 >= MAX_ARRAY_LIKE_INDEX {
                    return vm.throw_completion(ErrorKind::TypeError, ErrorType::ArrayMaxSize, &[]);
                }
                new_array.create_data_property_or_throw(vm, &property_key(n), arg)?;
                n += 1;
            }
            Ok(())
        };

        append_to_new_array(Value::from_object(this_object))?;

        for i in 0..vm.argument_count() {
            append_to_new_array(vm.argument(i))?;
        }

        new_array.set(vm, &vm.names.length, number(n), ShouldThrowExceptions::Yes)?;
        Ok(Value::from_object(new_array))
    }

    // 23.1.3.4 Array.prototype.copyWithin ( target, start [ , end ] ), https://tc39.es/ecma262/#sec-array.prototype.copywithin
    fn copy_within(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;
        let length = length_of_array_like(vm, &this_object)? as f64;

        let relative_target = vm.argument(0).to_integer_or_infinity(vm)?;

        let mut to = if relative_target < 0.0 {
            max(length + relative_target, 0.0)
        } else {
            min(relative_target, length)
        };

        let relative_start = vm.argument(1).to_integer_or_infinity(vm)?;

        let mut from = if relative_start < 0.0 {
            max(length + relative_start, 0.0)
        } else {
            min(relative_start, length)
        };

        let relative_end = if vm.argument(2).is_undefined() {
            length
        } else {
            vm.argument(2).to_integer_or_infinity(vm)?
        };

        let final_ = if relative_end < 0.0 {
            max(length + relative_end, 0.0)
        } else {
            min(relative_end, length)
        };

        let count = min(final_ - from, length - to);

        let mut direction: i64 = 1;

        if from < to && to < from + count {
            direction = -1;
            from = from + count - 1.0;
            to = to + count - 1.0;
        }

        if count < 0.0 {
            return Ok(Value::from_object(this_object));
        }

        let mut from_i = from as u64;
        let mut to_i = to as u64;
        let mut count_i = count as u64;

        while count_i > 0 {
            let from_present = this_object.has_property(vm, &property_key(from_i))?;

            if from_present {
                let from_value = this_object.get(vm, &property_key(from_i))?;
                this_object.set(vm, &property_key(to_i), from_value, ShouldThrowExceptions::Yes)?;
            } else {
                this_object.delete_property_or_throw(vm, &property_key(to_i))?;
            }

            from_i = from_i.wrapping_add_signed(direction);
            to_i = to_i.wrapping_add_signed(direction);
            count_i -= 1;
        }

        Ok(Value::from_object(this_object))
    }

    // 23.1.3.5 Array.prototype.entries ( ), https://tc39.es/ecma262/#sec-array.prototype.entries
    fn entries(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let this_object = vm.this_value().to_object(vm)?;

        Ok(Value::from_object(ArrayIterator::create(
            vm,
            realm,
            Value::from_object(this_object),
            PropertyKind::KeyAndValue,
        )))
    }

    // 23.1.3.6 Array.prototype.every ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.every
    fn every(vm: &Vm) -> ThrowCompletionOr<Value> {
        let callback_function = vm.argument(0);
        let this_arg = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
        if !callback_function.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback_function]);
        }

        // 4. Let k be 0.
        // 5. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k);

            // b. Let kPresent be ? HasProperty(O, Pk).
            let k_present = object.has_property(vm, &property_key)?;

            // c. If kPresent is true, then
            if k_present {
                // i. Let kValue be ? Get(O, Pk).
                let k_value = object.get(vm, &property_key)?;

                // ii. Let testResult be ToBoolean(? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »)).
                let test_result = call_function_object(
                    vm,
                    callback_function.as_function(),
                    this_arg,
                    &[k_value, number(k), Value::from_object(object)],
                )?
                .to_boolean();

                // iii. If testResult is false, return false.
                if !test_result {
                    return Ok(Value::FALSE);
                }
            }

            // d. Set k to k + 1.
        }

        // 6. Return true.
        Ok(Value::TRUE)
    }

    // 23.1.3.7 Array.prototype.fill ( value [ , start [ , end ] ] ), https://tc39.es/ecma262/#sec-array.prototype.fill
    fn fill(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;

        let length = length_of_array_like(vm, &this_object)?;

        let mut relative_start = 0.0;
        let mut relative_end = length as f64;

        if vm.argument_count() >= 2 {
            relative_start = vm.argument(1).to_integer_or_infinity(vm)?;
            if relative_start == f64::NEG_INFINITY {
                relative_start = 0.0;
            }
        }

        // If end is undefined, let relativeEnd be len; else let relativeEnd be ? ToIntegerOrInfinity(end).
        if vm.argument_count() >= 3 && !vm.argument(2).is_undefined() {
            relative_end = vm.argument(2).to_integer_or_infinity(vm)?;
            if relative_end == f64::NEG_INFINITY {
                relative_end = 0.0;
            }
        }

        let from = if relative_start < 0.0 {
            max(length as f64 + relative_start, 0.0)
        } else {
            min(relative_start, length as f64)
        } as u64;

        let to = if relative_end < 0.0 {
            max(length as f64 + relative_end, 0.0)
        } else {
            min(relative_end, length as f64)
        } as u64;

        for i in from..to {
            this_object.set(vm, &property_key(i), vm.argument(0), ShouldThrowExceptions::Yes)?;
        }

        Ok(Value::from_object(this_object))
    }

    // 23.1.3.8 Array.prototype.filter ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.filter
    fn filter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let callback_function = vm.argument(0);
        let this_arg = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
        if !callback_function.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback_function]);
        }

        // 4. Let A be ? ArraySpeciesCreate(O, 0).
        let array = array_species_create(vm, &object, 0)?;

        // 5. Let k be 0.
        // 6. Let to be 0.
        let mut to: u64 = 0;

        // 7. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k);

            // b. Let kPresent be ? HasProperty(O, Pk).
            let k_present = object.has_property(vm, &property_key)?;

            // c. If kPresent is true, then
            if k_present {
                // i. Let kValue be ? Get(O, Pk).
                let k_value = object.get(vm, &property_key)?;

                // ii. Let selected be ToBoolean(? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »)).
                let selected = call_function_object(
                    vm,
                    callback_function.as_function(),
                    this_arg,
                    &[k_value, number(k), Value::from_object(object)],
                )?
                .to_boolean();

                // iii. If selected is true, then
                if selected {
                    // 1. Perform ? CreateDataPropertyOrThrow(A, ! ToString(𝔽(to)), kValue).
                    array.create_data_property_or_throw(vm, &self::property_key(to), k_value)?;

                    // 2. Set to to to + 1.
                    to += 1;
                }
            }

            // d. Set k to k + 1.
        }

        // 8. Return A.
        Ok(Value::from_object(array))
    }

    // 23.1.3.9 Array.prototype.find ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.find
    fn find(vm: &Vm) -> ThrowCompletionOr<Value> {
        let predicate = vm.argument(0);
        let this_arg = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If IsCallable(predicate) is false, throw a TypeError exception.
        if !predicate.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&predicate]);
        }

        // 4. Let k be 0.
        // 5. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k);

            // b. Let kValue be ? Get(O, Pk).
            let k_value = object.get(vm, &property_key)?;

            // c. Let testResult be ToBoolean(? Call(predicate, thisArg, « kValue, 𝔽(k), O »)).
            let test_result = call_function_object(
                vm,
                predicate.as_function(),
                this_arg,
                &[k_value, number(k), Value::from_object(object)],
            )?
            .to_boolean();

            // d. If testResult is true, return kValue.
            if test_result {
                return Ok(k_value);
            }

            // e. Set k to k + 1.
        }

        // 6. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 23.1.3.10 Array.prototype.findIndex ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.findindex
    fn find_index(vm: &Vm) -> ThrowCompletionOr<Value> {
        let predicate = vm.argument(0);
        let this_arg = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If IsCallable(predicate) is false, throw a TypeError exception.
        if !predicate.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&predicate]);
        }

        // 4. Let k be 0.
        // 5. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k);

            // b. Let kValue be ? Get(O, Pk).
            let k_value = object.get(vm, &property_key)?;

            // c. Let testResult be ToBoolean(? Call(predicate, thisArg, « kValue, 𝔽(k), O »)).
            let test_result = call_function_object(
                vm,
                predicate.as_function(),
                this_arg,
                &[k_value, number(k), Value::from_object(object)],
            )?
            .to_boolean();

            // d. If testResult is true, return 𝔽(k).
            if test_result {
                return Ok(number(k));
            }

            // e. Set k to k + 1.
        }

        // 6. Return -1𝔽.
        Ok(Value::from_i32(-1))
    }

    // 23.1.3.11 Array.prototype.findLast ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.findlast
    fn find_last(vm: &Vm) -> ThrowCompletionOr<Value> {
        let predicate = vm.argument(0);
        let this_arg = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If IsCallable(predicate) is false, throw a TypeError exception.
        if !predicate.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&predicate]);
        }

        // 4. Let k be len - 1.
        // 5. Repeat, while k ≥ 0,
        for k in (0..length).rev() {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k);

            // b. Let kValue be ? Get(O, Pk).
            let k_value = object.get(vm, &property_key)?;

            // c. Let testResult be ToBoolean(? Call(predicate, thisArg, « kValue, 𝔽(k), O »)).
            let test_result = call_function_object(
                vm,
                predicate.as_function(),
                this_arg,
                &[k_value, number(k), Value::from_object(object)],
            )?
            .to_boolean();

            // d. If testResult is true, return kValue.
            if test_result {
                return Ok(k_value);
            }

            // e. Set k to k - 1.
        }

        // 6. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 23.1.3.12 Array.prototype.findLastIndex ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.findlastindex
    fn find_last_index(vm: &Vm) -> ThrowCompletionOr<Value> {
        let predicate = vm.argument(0);
        let this_arg = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If IsCallable(predicate) is false, throw a TypeError exception.
        if !predicate.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&predicate]);
        }

        // 4. Let k be len - 1.
        // 5. Repeat, while k ≥ 0,
        for k in (0..length).rev() {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k);

            // b. Let kValue be ? Get(O, Pk).
            let k_value = object.get(vm, &property_key)?;

            // c. Let testResult be ToBoolean(? Call(predicate, thisArg, « kValue, 𝔽(k), O »)).
            let test_result = call_function_object(
                vm,
                predicate.as_function(),
                this_arg,
                &[k_value, number(k), Value::from_object(object)],
            )?
            .to_boolean();

            // d. If testResult is true, return 𝔽(k).
            if test_result {
                return Ok(number(k));
            }

            // e. Set k to k - 1.
        }

        // 6. Return -1𝔽.
        Ok(Value::from_i32(-1))
    }

    // 23.1.3.13 Array.prototype.flat ( [ depth ] ), https://tc39.es/ecma262/#sec-array.prototype.flat
    fn flat(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;

        let length = length_of_array_like(vm, &this_object)?;

        let mut depth = 1.0;
        if !vm.argument(0).is_undefined() {
            let depth_num = vm.argument(0).to_integer_or_infinity(vm)?;
            depth = max(depth_num, 0.0);
        }

        let new_array = array_species_create(vm, &this_object, 0)?;

        flatten_into_array(vm, &new_array, &this_object, length, 0, depth, None, Value::UNDEFINED)?;
        Ok(Value::from_object(new_array))
    }

    // 23.1.3.14 Array.prototype.flatMap ( mapperFunction [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.flatmap
    fn flat_map(vm: &Vm) -> ThrowCompletionOr<Value> {
        let mapper_function = vm.argument(0);
        let this_arg = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let sourceLen be ? LengthOfArrayLike(O).
        let source_length = length_of_array_like(vm, &object)?;

        // 3. If IsCallable(mapperFunction) is false, throw a TypeError exception.
        if !mapper_function.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&mapper_function]);
        }

        // 4. Let A be ? ArraySpeciesCreate(O, 0).
        let array = array_species_create(vm, &object, 0)?;

        // 5. Perform ? FlattenIntoArray(A, O, sourceLen, 0, 1, mapperFunction, thisArg).
        flatten_into_array(
            vm,
            &array,
            &object,
            source_length,
            0,
            1.0,
            Some(mapper_function.as_function()),
            this_arg,
        )?;

        // 6. Return A.
        Ok(Value::from_object(array))
    }

    // 23.1.3.15 Array.prototype.forEach ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.foreach
    fn for_each(vm: &Vm) -> ThrowCompletionOr<Value> {
        let callback_function = vm.argument(0);
        let this_arg = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
        if !callback_function.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback_function]);
        }

        // 4. Let k be 0.
        // 5. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k);

            // b. Let kPresent be ? HasProperty(O, Pk).
            let k_present = object.has_property(vm, &property_key)?;

            // c. If kPresent is true, then
            if k_present {
                // i. Let kValue be ? Get(O, Pk).
                let k_value = object.get(vm, &property_key)?;

                // ii. Perform ? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »).
                call_function_object(
                    vm,
                    callback_function.as_function(),
                    this_arg,
                    &[k_value, number(k), Value::from_object(object)],
                )?;
            }

            // d. Set k to k + 1.
        }

        // 6. Return undefined.
        Ok(Value::UNDEFINED)
    }

    // 23.1.3.16 Array.prototype.includes ( searchElement [ , fromIndex ] ), https://tc39.es/ecma262/#sec-array.prototype.includes
    fn includes(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;
        let length = length_of_array_like(vm, &this_object)?;
        if length == 0 {
            return Ok(Value::FALSE);
        }
        let mut from_index: u64 = 0;
        if vm.argument_count() >= 2 {
            let mut from_argument = vm.argument(1).to_integer_or_infinity(vm)?;

            if from_argument == f64::INFINITY || from_argument >= length as f64 {
                return Ok(Value::FALSE);
            }

            if from_argument == f64::NEG_INFINITY {
                from_argument = 0.0;
            }

            from_index = if from_argument < 0.0 {
                max(length as f64 + from_argument, 0.0) as u64
            } else {
                from_argument as u64
            };
        }
        let value_to_find = vm.argument(0);
        for i in from_index..length {
            let element = this_object.get(vm, &property_key(i))?;
            if same_value_zero(element, value_to_find) {
                return Ok(Value::TRUE);
            }
        }
        Ok(Value::FALSE)
    }

    // 23.1.3.17 Array.prototype.indexOf ( searchElement [ , fromIndex ] ), https://tc39.es/ecma262/#sec-array.prototype.indexof
    fn index_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let search_element = vm.argument(0);
        let from_index = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If len is 0, return -1𝔽.
        if length == 0 {
            return Ok(Value::from_i32(-1));
        }

        // 4. Let n be ? ToIntegerOrInfinity(fromIndex).
        let mut n = from_index.to_integer_or_infinity(vm)?;

        // 5. Assert: If fromIndex is undefined, then n is 0.
        if from_index.is_undefined() {
            assert!(n == 0.0);
        }

        // 6. If n is +∞, return -1𝔽.
        if n == f64::INFINITY {
            return Ok(Value::from_i32(-1));
        }

        // 7. Else if n is -∞, set n to 0.
        if n == f64::NEG_INFINITY {
            n = 0.0;
        }

        let mut k: u64;

        // 8. If n ≥ 0, then
        if n >= 0.0 {
            // AD-HOC: A fromIndex at or beyond len matches nothing. Return before converting it to an unsigned type.
            if n >= length as f64 {
                return Ok(Value::from_i32(-1));
            }

            // a. Let k be n.
            k = n as u64;
        }
        // 9. Else,
        else {
            // a. Let k be len + n.
            // b. If k < 0, set k to 0.
            k = max(length as f64 + n, 0.0) as u64;
        }

        // OPTIMIZATION: Simple packed arrays have an own data property for every index below their length,
        // so HasProperty and Get cannot produce side effects or observe prototype indexed properties.
        if let Some(array) = object.downcast::<Array>()
            && array.is_simple_packed_array()
            && u64::from(array.indexed_array_like_size()) == length
        {
            let element_count = u64::from(array.indexed_packed_element_count());
            while k < element_count {
                let element = array
                    .indexed_get(k as u32)
                    .expect("a packed array has every element below its size")
                    .value;
                if is_strictly_equal(search_element, element) {
                    return Ok(number(k));
                }
                k += 1;
            }
            return Ok(Value::from_i32(-1));
        }

        // 10. Repeat, while k < len,
        while k < length {
            let property_key = property_key(k);

            // a. Let kPresent be ? HasProperty(O, ! ToString(𝔽(k))).
            let k_present = object.has_property(vm, &property_key)?;

            // b. If kPresent is true, then
            if k_present {
                // i. Let elementK be ? Get(O, ! ToString(𝔽(k))).
                let element_k = object.get(vm, &property_key)?;

                // ii. Let same be IsStrictlyEqual(searchElement, elementK).
                let same = is_strictly_equal(search_element, element_k);

                // iii. If same is true, return 𝔽(k).
                if same {
                    return Ok(number(k));
                }
            }

            // c. Set k to k + 1.
            k += 1;
        }

        // 11. Return -1𝔽.
        Ok(Value::from_i32(-1))
    }

    // 23.1.3.18 Array.prototype.join ( separator ), https://tc39.es/ecma262/#sec-array.prototype.join
    fn join(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;

        // This is not part of the spec, but all major engines do some kind of circular reference checks.
        // FWIW: engine262, a "100% spec compliant" ECMA-262 impl, aborts with "too much recursion".
        // Same applies to Array.prototype.toLocaleString().
        let Some(_unsee_object_guard) = see_object_unless_seen(this_object) else {
            return Ok(Value::from_string(PrimitiveString::create(vm, Utf16String::default())));
        };

        let length = length_of_array_like(vm, &this_object)?;
        let mut separator = Utf16String::from_utf8(",");
        if !vm.argument(0).is_undefined() {
            separator = vm.argument(0).to_utf16_string(vm)?;
        }
        let mut builder = Vec::new();
        for i in 0..length {
            if i > 0 {
                Utf16View::of_string(&separator).append_to(&mut builder);
            }
            let value = this_object.get(vm, &property_key(i))?;
            if value.is_nullish() {
                continue;
            }
            let string = value.to_utf16_string(vm)?;
            Utf16View::of_string(&string).append_to(&mut builder);
        }

        Ok(Value::from_string(PrimitiveString::create(
            vm,
            Utf16String::from_utf16(&builder),
        )))
    }

    // 23.1.3.19 Array.prototype.keys ( ), https://tc39.es/ecma262/#sec-array.prototype.keys
    fn keys(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let this_object = vm.this_value().to_object(vm)?;

        Ok(Value::from_object(ArrayIterator::create(
            vm,
            realm,
            Value::from_object(this_object),
            PropertyKind::Key,
        )))
    }

    // 23.1.3.20 Array.prototype.lastIndexOf ( searchElement [ , fromIndex ] ), https://tc39.es/ecma262/#sec-array.prototype.lastindexof
    fn last_index_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        let search_element = vm.argument(0);
        let from_index = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If len is 0, return -1𝔽.
        if length == 0 {
            return Ok(Value::from_i32(-1));
        }

        // 4. If fromIndex is present, let n be ? ToIntegerOrInfinity(fromIndex); else let n be len - 1.
        let n = if vm.argument_count() >= 2 {
            from_index.to_integer_or_infinity(vm)?
        } else {
            length as f64 - 1.0
        };

        // 5. If n is -∞, return -1𝔽.
        if n == f64::NEG_INFINITY {
            return Ok(Value::from_i32(-1));
        }

        let mut k: i64;

        // 6. If n ≥ 0, then
        if n >= 0.0 {
            // a. Let k be min(n, len - 1).
            k = min(n, length as f64 - 1.0) as i64;
        }
        // 7. Else,
        else {
            //  a. Let k be len + n.
            let relative_k = length as f64 + n;

            // AD-HOC: A large negative fromIndex visits nothing. Clamp k to -1 rather than converting an
            //         out-of-range value to a signed type.
            k = if relative_k < 0.0 { -1 } else { relative_k as i64 };
        }

        // 8. Repeat, while k ≥ 0,
        while k >= 0 {
            let property_key = property_key(k as u64);

            // a. Let kPresent be ? HasProperty(O, ! ToString(𝔽(k))).
            let k_present = object.has_property(vm, &property_key)?;

            // b. If kPresent is true, then
            if k_present {
                // i. Let elementK be ? Get(O, ! ToString(𝔽(k))).
                let element_k = object.get(vm, &property_key)?;

                // ii. Let same be IsStrictlyEqual(searchElement, elementK).
                let same = is_strictly_equal(search_element, element_k);

                // iii. If same is true, return 𝔽(k).
                if same {
                    return Ok(number(k as u64));
                }
            }

            // c. Set k to k - 1.
            k -= 1;
        }

        // 9. Return -1𝔽.
        Ok(Value::from_i32(-1))
    }

    // 23.1.3.21 Array.prototype.map ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.map
    fn map(vm: &Vm) -> ThrowCompletionOr<Value> {
        let callback_function = vm.argument(0);
        let this_arg = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
        if !callback_function.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback_function]);
        }

        // 4. Let A be ? ArraySpeciesCreate(O, len).
        let array = array_species_create(vm, &object, length)?;

        // 5. Let k be 0.
        // 6. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k);

            // b. Let kPresent be ? HasProperty(O, Pk).
            let k_present = object.has_property(vm, &property_key)?;

            // c. If kPresent is true, then
            if k_present {
                // i. Let kValue be ? Get(O, Pk).
                let k_value = object.get(vm, &property_key)?;

                // ii. Let mappedValue be ? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »).
                let mapped_value = call_function_object(
                    vm,
                    callback_function.as_function(),
                    this_arg,
                    &[k_value, number(k), Value::from_object(object)],
                )?;

                // iii. Perform ? CreateDataPropertyOrThrow(A, Pk, mappedValue).
                array.create_data_property_or_throw(vm, &property_key, mapped_value)?;
            }

            // d. Set k to k + 1.
        }

        // 7. Return A.
        Ok(Value::from_object(array))
    }

    // 23.1.3.22 Array.prototype.pop ( ), https://tc39.es/ecma262/#sec-array.prototype.pop
    fn pop(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;

        // OPTIMIZATION: Fast path for packed arrays.
        if let Some(array) = this_object.downcast::<Array>()
            && array.is_simple_packed_array()
            && array.length_is_writable()
        {
            if array.indexed_array_like_size() == 0 {
                return Ok(Value::UNDEFINED);
            }
            let last = array.indexed_take_last();
            if last.value.is_empty() {
                return Ok(Value::UNDEFINED);
            }
            return Ok(last.value);
        }

        let length = length_of_array_like(vm, &this_object)?;
        if length == 0 {
            this_object.set(vm, &vm.names.length, Value::from_i32(0), ShouldThrowExceptions::Yes)?;
            return Ok(Value::UNDEFINED);
        }
        let index = length - 1;
        let element = this_object.get(vm, &property_key(index))?;
        this_object.delete_property_or_throw(vm, &property_key(index))?;
        this_object.set(vm, &vm.names.length, number(index), ShouldThrowExceptions::Yes)?;
        Ok(element)
    }

    // 23.1.3.23 Array.prototype.push ( ...items ), https://tc39.es/ecma262/#sec-array.prototype.push
    fn push(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;

        // OPTIMIZATION: Fast path for packed arrays.
        if let Some(array) = this_object.downcast::<Array>()
            && array.is_simple_packed_array()
            && array.default_prototype_chain_intact()
            && array.extensible()
            && array.length_is_writable()
        {
            let argument_count = vm.argument_count();
            for i in 0..argument_count {
                array.indexed_append(vm.argument(i), DEFAULT_ATTRIBUTES);
            }
            return Ok(Value::from_f64(f64::from(array.indexed_array_like_size())));
        }

        let length = length_of_array_like(vm, &this_object)?;
        let argument_count = vm.argument_count() as u64;
        let new_length = length + argument_count;
        if new_length as f64 > MAX_ARRAY_LIKE_INDEX {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ArrayMaxSize, &[]);
        }
        for i in 0..argument_count {
            this_object.set(
                vm,
                &property_key(length + i),
                vm.argument(i as usize),
                ShouldThrowExceptions::Yes,
            )?;
        }
        let new_length_value = number(new_length);
        this_object.set(vm, &vm.names.length, new_length_value, ShouldThrowExceptions::Yes)?;
        Ok(new_length_value)
    }

    // 23.1.3.24 Array.prototype.reduce ( callbackfn [ , initialValue ] ), https://tc39.es/ecma262/#sec-array.prototype.reduce
    fn reduce(vm: &Vm) -> ThrowCompletionOr<Value> {
        let callback_function = vm.argument(0);
        let initial_value = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
        if !callback_function.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback_function]);
        }

        // 4. If len = 0 and initialValue is not present, throw a TypeError exception.
        if length == 0 && vm.argument_count() <= 1 {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ReduceNoInitial, &[]);
        }

        // 5. Let k be 0.
        let mut k: u64 = 0;

        // 6. Let accumulator be undefined.
        let mut accumulator = Value::UNDEFINED;

        // 7. If initialValue is present, then
        if vm.argument_count() > 1 {
            // a. Set accumulator to initialValue.
            accumulator = initial_value;
        }
        // 8. Else,
        else {
            // a. Let kPresent be false.
            let mut k_present = false;

            // b. Repeat, while kPresent is false and k < len,
            while !k_present && k < length {
                // i. Let Pk be ! ToString(𝔽(k)).
                let property_key = property_key(k);

                // ii. Set kPresent to ? HasProperty(O, Pk).
                k_present = object.has_property(vm, &property_key)?;

                // iii. If kPresent is true, then
                if k_present {
                    // 1. Set accumulator to ? Get(O, Pk).
                    accumulator = object.get(vm, &property_key)?;
                }

                // iv. Set k to k + 1.
                k += 1;
            }

            // c. If kPresent is false, throw a TypeError exception.
            if !k_present {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::ReduceNoInitial, &[]);
            }
        }

        // 9. Repeat, while k < len,
        while k < length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k);

            // b. Let kPresent be ? HasProperty(O, Pk).
            let k_present = object.has_property(vm, &property_key)?;

            // c. If kPresent is true, then
            if k_present {
                // i. Let kValue be ? Get(O, Pk).
                let k_value = object.get(vm, &property_key)?;

                // ii. Set accumulator to ? Call(callbackfn, undefined, « accumulator, kValue, 𝔽(k), O »).
                accumulator = call_function_object(
                    vm,
                    callback_function.as_function(),
                    Value::UNDEFINED,
                    &[accumulator, k_value, number(k), Value::from_object(object)],
                )?;
            }

            // d. Set k to k + 1.
            k += 1;
        }

        // 10. Return accumulator.
        Ok(accumulator)
    }

    // 23.1.3.25 Array.prototype.reduceRight ( callbackfn [ , initialValue ] ), https://tc39.es/ecma262/#sec-array.prototype.reduceright
    fn reduce_right(vm: &Vm) -> ThrowCompletionOr<Value> {
        let callback_function = vm.argument(0);
        let initial_value = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
        if !callback_function.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback_function]);
        }

        // 4. If len = 0 and initialValue is not present, throw a TypeError exception.
        if length == 0 && vm.argument_count() <= 1 {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ReduceNoInitial, &[]);
        }

        // 5. Let k be len - 1.
        let mut k = length as i64 - 1;

        // 6. Let accumulator be undefined.
        let mut accumulator = Value::UNDEFINED;

        // 7. If initialValue is present, then
        if vm.argument_count() > 1 {
            // a. Set accumulator to initialValue.
            accumulator = initial_value;
        }
        // 8. Else,
        else {
            // a. Let kPresent be false.
            let mut k_present = false;

            // b. Repeat, while kPresent is false and k ≥ 0,
            while !k_present && k >= 0 {
                // i. Let Pk be ! ToString(𝔽(k)).
                let property_key = property_key(k as u64);

                // ii. Set kPresent to ? HasProperty(O, Pk).
                k_present = object.has_property(vm, &property_key)?;

                // iii. If kPresent is true, then
                if k_present {
                    // 1. Set accumulator to ? Get(O, Pk).
                    accumulator = object.get(vm, &property_key)?;
                }

                // iv. Set k to k - 1.
                k -= 1;
            }

            // c. If kPresent is false, throw a TypeError exception.
            if !k_present {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::ReduceNoInitial, &[]);
            }
        }

        // 9. Repeat, while k ≥ 0,
        while k >= 0 {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k as u64);

            // b. Let kPresent be ? HasProperty(O, Pk).
            let k_present = object.has_property(vm, &property_key)?;

            // c. If kPresent is true, then
            if k_present {
                // i. Let kValue be ? Get(O, Pk).
                let k_value = object.get(vm, &property_key)?;

                // ii. Set accumulator to ? Call(callbackfn, undefined, « accumulator, kValue, 𝔽(k), O »).
                accumulator = call_function_object(
                    vm,
                    callback_function.as_function(),
                    Value::UNDEFINED,
                    &[accumulator, k_value, number(k as u64), Value::from_object(object)],
                )?;
            }

            // d. Set k to k - 1.
            k -= 1;
        }

        // 10. Return accumulator.
        Ok(accumulator)
    }

    // 23.1.3.26 Array.prototype.reverse ( ), https://tc39.es/ecma262/#sec-array.prototype.reverse
    fn reverse(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;
        let length = length_of_array_like(vm, &this_object)?;

        let middle = length / 2;
        for lower in 0..middle {
            let upper = length - lower - 1;

            let lower_exists = this_object.has_property(vm, &property_key(lower))?;
            let mut lower_value = Value::UNDEFINED;
            if lower_exists {
                lower_value = this_object.get(vm, &property_key(lower))?;
            }

            let upper_exists = this_object.has_property(vm, &property_key(upper))?;
            let mut upper_value = Value::UNDEFINED;
            if upper_exists {
                upper_value = this_object.get(vm, &property_key(upper))?;
            }

            if lower_exists && upper_exists {
                this_object.set(vm, &property_key(lower), upper_value, ShouldThrowExceptions::Yes)?;
                this_object.set(vm, &property_key(upper), lower_value, ShouldThrowExceptions::Yes)?;
            } else if !lower_exists && upper_exists {
                this_object.set(vm, &property_key(lower), upper_value, ShouldThrowExceptions::Yes)?;
                this_object.delete_property_or_throw(vm, &property_key(upper))?;
            } else if lower_exists && !upper_exists {
                this_object.delete_property_or_throw(vm, &property_key(lower))?;
                this_object.set(vm, &property_key(upper), lower_value, ShouldThrowExceptions::Yes)?;
            }
        }

        Ok(Value::from_object(this_object))
    }

    // 23.1.3.27 Array.prototype.shift ( ), https://tc39.es/ecma262/#sec-array.prototype.shift
    fn shift(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;
        let length = length_of_array_like(vm, &this_object)?;
        if length == 0 {
            this_object.set(vm, &vm.names.length, Value::from_i32(0), ShouldThrowExceptions::Yes)?;
            return Ok(Value::UNDEFINED);
        }

        // OPTIMIZATION: If this object is Array that:
        // - is not a proxy target, which means get/set/has/delete will not trap.
        // - has intact prototype chain, which means we don't have to worry about getters/setters potentially defined for holes.
        // - has simple storage type, which means all values have default attributes (if some elements have configurable=false, we cannot use fast path, because delete operation will fail).
        // then we could take a fast path by directly taking first element from indexed storage.
        if let Some(array) = this_object.downcast::<Array>()
            && (can_use_packed_shift_fast_path(&array) || can_use_holey_shift_fast_path(&array))
        {
            let first = array.indexed_take_first().value;
            if first.is_empty() {
                return Ok(Value::UNDEFINED);
            }
            return Ok(first);
        }

        let first = this_object.get(vm, &property_key(0))?;
        for k in 1..length {
            let from = k;
            let to = k - 1;
            let from_present = this_object.has_property(vm, &property_key(from))?;
            if from_present {
                let from_value = this_object.get(vm, &property_key(from))?;
                this_object.set(vm, &property_key(to), from_value, ShouldThrowExceptions::Yes)?;
            } else {
                this_object.delete_property_or_throw(vm, &property_key(to))?;
            }
        }

        this_object.delete_property_or_throw(vm, &property_key(length - 1))?;
        this_object.set(vm, &vm.names.length, number(length - 1), ShouldThrowExceptions::Yes)?;
        Ok(first)
    }

    // 23.1.3.28 Array.prototype.slice ( start, end ), https://tc39.es/ecma262/#sec-array.prototype.slice
    fn slice(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;

        let initial_length = length_of_array_like(vm, &this_object)?;

        let relative_start = vm.argument(0).to_integer_or_infinity(vm)?;

        let actual_start = if relative_start == f64::NEG_INFINITY {
            0.0
        } else if relative_start < 0.0 {
            max(initial_length as f64 + relative_start, 0.0)
        } else {
            min(relative_start, initial_length as f64)
        };

        let relative_end = if vm.argument(1).is_undefined() {
            initial_length as f64
        } else {
            vm.argument(1).to_integer_or_infinity(vm)?
        };

        let final_ = if relative_end == f64::NEG_INFINITY {
            0.0
        } else if relative_end < 0.0 {
            max(initial_length as f64 + relative_end, 0.0)
        } else {
            min(relative_end, initial_length as f64)
        };

        let count = max(final_ - actual_start, 0.0);

        let new_array = array_species_create(vm, &this_object, count as u64)?;

        // OPTIMIZATION: Fast path for packed arrays when ArraySpeciesCreate
        // produced a default Array result.
        if let Some(array) = this_object.downcast::<Array>()
            && can_use_packed_array_fast_path(&array)
            && u64::from(array.indexed_array_like_size()) == initial_length
            && let Some(result_array) = fast_array_species_result(new_array)
        {
            let start = actual_start as u32;
            let end = final_ as u32;
            for i in start..end {
                result_array.indexed_put(
                    i - start,
                    array
                        .indexed_get(i)
                        .expect("a packed array has every element below its size")
                        .value,
                    DEFAULT_ATTRIBUTES,
                );
            }

            // NB: A species constructor can return an array with more elements than the slice. The spec sets the
            //     length of the result at the end, which removes them.
            let result_length = end.saturating_sub(start);
            if result_array.indexed_array_like_size() != result_length {
                assert!(result_array.set_indexed_array_like_size(result_length as usize));
            }
            return Ok(Value::from_object(result_array));
        }

        let mut index: u64 = 0;
        let mut k = actual_start as u64;

        while (k as f64) < final_ {
            let present = this_object.has_property(vm, &property_key(k))?;
            if present {
                let value = this_object.get(vm, &property_key(k))?;
                new_array.create_data_property_or_throw(vm, &property_key(index), value)?;
            }

            k += 1;
            index += 1;
        }

        new_array.set(vm, &vm.names.length, number(index), ShouldThrowExceptions::Yes)?;
        Ok(Value::from_object(new_array))
    }

    // 23.1.3.29 Array.prototype.some ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.some
    fn some(vm: &Vm) -> ThrowCompletionOr<Value> {
        let callback_function = vm.argument(0);
        let this_arg = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
        if !callback_function.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback_function]);
        }

        // 4. Let k be 0.
        // 5. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k);

            // b. Let kPresent be ? HasProperty(O, Pk).
            let k_present = object.has_property(vm, &property_key)?;

            // c. If kPresent is true, then
            if k_present {
                // i. Let kValue be ? Get(O, Pk).
                let k_value = object.get(vm, &property_key)?;

                // ii. Let testResult be ToBoolean(? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »)).
                let test_result = call_function_object(
                    vm,
                    callback_function.as_function(),
                    this_arg,
                    &[k_value, number(k), Value::from_object(object)],
                )?
                .to_boolean();

                // iii. If testResult is true, return true.
                if test_result {
                    return Ok(Value::TRUE);
                }
            }

            // d. Set k to k + 1.
        }

        // 6. Return false.
        Ok(Value::FALSE)
    }

    // 23.1.3.30 Array.prototype.sort ( comparefn ), https://tc39.es/ecma262/#sec-array.prototype.sort
    fn sort(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If comparefn is not undefined and IsCallable(comparefn) is false, throw a TypeError exception.
        let comparefn = vm.argument(0);
        if !comparefn.is_undefined() && !comparefn.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&comparefn]);
        }

        // 2. Let obj be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 3. Let len be ? LengthOfArrayLike(obj).
        let length = length_of_array_like(vm, &object)?;

        // 4. Let SortCompare be a new Abstract Closure with parameters (x, y) that captures comparefn and performs the following steps when called:
        let comparefn_function = (!comparefn.is_undefined()).then(|| comparefn.as_function());
        let sort_compare = |x: Value, y: Value| -> ThrowCompletionOr<f64> {
            // a. Return ? CompareArrayElements(x, y, comparefn).
            compare_array_elements(vm, x, y, comparefn_function)
        };

        // 5. Let sortedList be ? SortIndexedProperties(obj, len, SortCompare, skip-holes).
        let sorted_list = sort_indexed_properties(vm, &object, length, &sort_compare, Holes::SkipHoles)?;

        // 6. Let itemCount be the number of elements in sortedList.
        let item_count = sorted_list.len() as u64;

        // 7. Let j be 0.
        let mut j: u64 = 0;

        // 8. Repeat, while j < itemCount,
        while j < item_count {
            // a. Perform ? Set(obj, ! ToString(𝔽(j)), sortedList[j], true).
            let sorted_value = sorted_list.get(j as usize).expect("j is below the item count");
            object.set(vm, &property_key(j), sorted_value, ShouldThrowExceptions::Yes)?;
            // b. Set j to j + 1.
            j += 1;
        }

        // 9. NOTE: The call to SortIndexedProperties in step 5 uses skip-holes. The remaining indices are deleted to preserve the number of holes that were detected and excluded from the sort.
        // 10. Repeat, while j < len,
        while j < length {
            // a. Perform ? DeletePropertyOrThrow(obj, ! ToString(𝔽(j))).
            object.delete_property_or_throw(vm, &property_key(j))?;
            // b. Set j to j + 1.
            j += 1;
        }

        // 11. Return obj.
        Ok(Value::from_object(object))
    }

    // 23.1.3.31 Array.prototype.splice ( start, deleteCount, ...items ), https://tc39.es/ecma262/#sec-array.prototype.splice
    fn splice(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be ? ToObject(this value).
        let this_object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let initial_length = length_of_array_like(vm, &this_object)?;

        // 3. Let relativeStart be ? ToIntegerOrInfinity(start).
        let relative_start = vm.argument(0).to_integer_or_infinity(vm)?;

        // 4. If relativeStart = -∞, let actualStart be 0.
        let actual_start = if relative_start == f64::NEG_INFINITY {
            0
        }
        // 5. Else if relativeStart < 0, let actualStart be max(len + relativeStart, 0).
        else if relative_start < 0.0 {
            max(initial_length as i64 as f64 + relative_start, 0.0) as u64
        }
        // 6. Else, let actualStart be min(relativeStart, len).
        else {
            min(relative_start, initial_length as f64) as u64
        };

        // 7. Let itemCount be the number of elements in items.
        let item_count: u64 = if vm.argument_count() >= 2 {
            vm.argument_count() as u64 - 2
        } else {
            0
        };

        // 8. If start is not present, then
        let actual_delete_count = if vm.argument_count() == 0 {
            // a. Let actualDeleteCount be 0.
            0
        }
        // 9. Else if deleteCount is not present, then
        else if vm.argument_count() == 1 {
            // a. Let actualDeleteCount be len - actualStart.
            initial_length - actual_start
        }
        // 10. Else,
        else {
            // a. Let dc be ? ToIntegerOrInfinity(deleteCount).
            let delete_count = vm.argument(1).to_integer_or_infinity(vm)?;

            // b. Let actualDeleteCount be the result of clamping dc between 0 and len - actualStart.
            clamp(delete_count, 0.0, (initial_length - actual_start) as f64) as u64
        };

        // 11. If len + itemCount - actualDeleteCount > 2^53 - 1, throw a TypeError exception.
        if (initial_length + item_count - actual_delete_count) as f64 > MAX_ARRAY_LIKE_INDEX {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ArrayMaxSize, &[]);
        }

        // 12. Let A be ? ArraySpeciesCreate(O, actualDeleteCount).
        let removed_elements = array_species_create(vm, &this_object, actual_delete_count)?;

        // OPTIMIZATION: Fast path for packed arrays when ArraySpeciesCreate
        // produced a default Array result and the splice can mutate indexed
        // storage without going through observable accessors.
        if let Some(array) = this_object.downcast::<Array>()
            && can_use_packed_array_fast_path(&array)
            && u64::from(array.indexed_array_like_size()) == initial_length
            && array.extensible()
            && array.length_is_writable()
            && actual_start <= u64::from(u32::MAX)
            && actual_delete_count <= u64::from(u32::MAX)
            && item_count <= u64::from(u32::MAX)
            // NB: The removed elements array must not be this array, since we copy from it while shifting its elements.
            && let Some(removed_array) = fast_array_species_result(removed_elements)
            && removed_array != array
        {
            let element = |index: u32| {
                array
                    .indexed_get(index)
                    .expect("a packed array has every element below its size")
                    .value
            };
            let item = |index: u32| vm.argument(2 + index as usize);
            let start = actual_start as u32;
            let delete_count = actual_delete_count as u32;
            let item_count_u32 = item_count as u32;
            let len = array.indexed_array_like_size();

            for i in 0..delete_count {
                removed_array.indexed_put(i, element(start + i), DEFAULT_ATTRIBUTES);
            }

            // NB: A species constructor can return an array with more elements than were removed. The spec sets its
            //     length to actualDeleteCount, which removes them.
            if removed_array.indexed_array_like_size() != delete_count {
                assert!(removed_array.set_indexed_array_like_size(delete_count as usize));
            }

            if item_count_u32 == delete_count {
                for i in 0..item_count_u32 {
                    array.indexed_put(start + i, item(i), DEFAULT_ATTRIBUTES);
                }
            } else if item_count_u32 < delete_count {
                for i in 0..item_count_u32 {
                    array.indexed_put(start + i, item(i), DEFAULT_ATTRIBUTES);
                }

                let shift = delete_count - item_count_u32;
                for i in start + item_count_u32..len - shift {
                    array.indexed_put(i, element(i + shift), DEFAULT_ATTRIBUTES);
                }
                assert!(array.set_indexed_array_like_size((len - shift) as usize));
            } else {
                let growth = item_count_u32 - delete_count;
                let new_length = len + growth;
                assert!(array.set_indexed_array_like_size(new_length as usize));
                let mut i = new_length;
                while i > start + item_count_u32 {
                    array.indexed_put(i - 1, element(i - 1 - growth), DEFAULT_ATTRIBUTES);
                    i -= 1;
                }
                for i in 0..item_count_u32 {
                    array.indexed_put(start + i, item(i), DEFAULT_ATTRIBUTES);
                }

                if array.indexed_storage_kind() == IndexedStorageKind::Holey && array.indexed_array_like_size() != 0 {
                    let last_index = array.indexed_array_like_size() - 1;
                    let last_value = array
                        .indexed_get(last_index)
                        .expect("every index of the spliced array has an element");
                    array.indexed_put(last_index, last_value.value, DEFAULT_ATTRIBUTES);
                }
            }

            return Ok(Value::from_object(removed_array));
        }

        // 13. Let k be 0.
        // 14. Repeat, while k < actualDeleteCount,
        for k in 0..actual_delete_count {
            // a. Let from be ! ToString(𝔽(actualStart + k)).
            let from = property_key(actual_start + k);

            // b. If ? HasProperty(O, from) is true, then
            if this_object.has_property(vm, &from)? {
                // i. Let fromValue be ? Get(O, from).
                let from_value = this_object.get(vm, &from)?;

                // ii. Perform ? CreateDataPropertyOrThrow(A, ! ToString(𝔽(k)), fromValue).
                removed_elements.create_data_property_or_throw(vm, &property_key(k), from_value)?;
            }

            // c. Set k to k + 1.
        }

        // 15. Perform ? Set(A, "length", 𝔽(actualDeleteCount), true).
        removed_elements.set(
            vm,
            &vm.names.length,
            number(actual_delete_count),
            ShouldThrowExceptions::Yes,
        )?;

        // 16. If itemCount < actualDeleteCount, then
        if item_count < actual_delete_count {
            // a. Set k to actualStart.
            // b. Repeat, while k < (len - actualDeleteCount),
            for k in actual_start..initial_length - actual_delete_count {
                // i. Let from be ! ToString(𝔽(k + actualDeleteCount)).
                let from = property_key(k + actual_delete_count);

                // ii. Let to be ! ToString(𝔽(k + itemCount)).
                let to = property_key(k + item_count);

                // iii. If ? HasProperty(O, from) is true, then
                if this_object.has_property(vm, &from)? {
                    // 1. Let fromValue be ? Get(O, from).
                    let from_value = this_object.get(vm, &from)?;

                    // 2. Perform ? Set(O, to, fromValue, true).
                    this_object.set(vm, &to, from_value, ShouldThrowExceptions::Yes)?;
                }
                // iv. Else,
                else {
                    // 1. Perform ? DeletePropertyOrThrow(O, to).
                    this_object.delete_property_or_throw(vm, &to)?;
                }

                // v. Set k to k + 1.
            }

            // c. Set k to len.
            // d. Repeat, while k > (len - actualDeleteCount + itemCount),
            let mut k = initial_length;
            while k > initial_length - actual_delete_count + item_count {
                // i. Perform ? DeletePropertyOrThrow(O, ! ToString(𝔽(k - 1))).
                this_object.delete_property_or_throw(vm, &property_key(k - 1))?;

                // ii. Set k to k - 1.
                k -= 1;
            }
        }
        // 17. Else if itemCount > actualDeleteCount, then
        else if item_count > actual_delete_count {
            // a. Set k to (len - actualDeleteCount).
            // b. Repeat, while k > actualStart,
            let mut k = initial_length - actual_delete_count;
            while k > actual_start {
                // i. Let from be ! ToString(𝔽(k + actualDeleteCount - 1)).
                let from = property_key(k + actual_delete_count - 1);

                // ii. Let to be ! ToString(𝔽(k + itemCount - 1)).
                let to = property_key(k + item_count - 1);

                // iii. If ? HasProperty(O, from) is true, then
                if this_object.has_property(vm, &from)? {
                    // 1. Let fromValue be ? Get(O, from).
                    let from_value = this_object.get(vm, &from)?;

                    // 2. Perform ? Set(O, to, fromValue, true).
                    this_object.set(vm, &to, from_value, ShouldThrowExceptions::Yes)?;
                }
                // iv. Else,
                else {
                    // 1. Perform ? DeletePropertyOrThrow(O, to).
                    this_object.delete_property_or_throw(vm, &to)?;
                }

                // v. Set k to k - 1.
                k -= 1;
            }
        }

        // 18. Set k to actualStart.
        // 19. For each element E of items, do
        for (k, element_index) in (actual_start..).zip(2..vm.argument_count()) {
            let element = vm.argument(element_index);

            // a. Perform ? Set(O, ! ToString(𝔽(k)), E, true).
            this_object.set(vm, &property_key(k), element, ShouldThrowExceptions::Yes)?;

            // b. Set k to k + 1.
        }

        // 20. Perform ? Set(O, "length", 𝔽(len - actualDeleteCount + itemCount), true).
        this_object.set(
            vm,
            &vm.names.length,
            number(initial_length - actual_delete_count + item_count),
            ShouldThrowExceptions::Yes,
        )?;

        // 21. Return A.
        Ok(Value::from_object(removed_elements))
    }

    // 23.1.3.32 Array.prototype.toLocaleString ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-array.prototype.tolocalestring
    // 20.5.1 Array.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-array.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let array be ? ToObject(this value).
        let this_object = vm.this_value().to_object(vm)?;

        let Some(_unsee_object_guard) = see_object_unless_seen(this_object) else {
            return Ok(Value::from_string(PrimitiveString::create(vm, Utf16String::default())));
        };

        // 2. Let len be ? ToLength(? Get(array, "length")).
        let length = length_of_array_like(vm, &this_object)?;

        // 3. Let separator be the implementation-defined list-separator String value appropriate for the host environment's current locale (such as ", ").
        let separator = Utf16View::Ascii(b",");

        // 4. Let R be the empty String.
        let mut builder = Vec::new();

        // 5. Let k be 0.
        // 6. Repeat, while k < len,
        for i in 0..length {
            // a. If k > 0, then
            if i > 0 {
                // i. Set R to the string-concatenation of R and separator.
                separator.append_to(&mut builder);
            }

            // b. Let nextElement be ? Get(array, ! ToString(k)).
            let value = this_object.get(vm, &property_key(i))?;

            // c. If nextElement is not undefined or null, then
            if !value.is_nullish() {
                // i. Let S be ? ToString(? Invoke(nextElement, "toLocaleString", « locales, options »)).
                let locale_string_result = value.invoke(vm, &vm.names.toLocaleString, &[locales, options])?;

                // ii. Set R to the string-concatenation of R and S.
                let string = locale_string_result.to_utf16_string(vm)?;
                Utf16View::of_string(&string).append_to(&mut builder);
            }

            // d. Increase k by 1.
        }

        // 7. Return R.
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            Utf16String::from_utf16(&builder),
        )))
    }

    // 23.1.3.33 Array.prototype.toReversed ( ), https://tc39.es/ecma262/#sec-array.prototype.toreversed
    fn to_reversed(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. Let A be ? ArrayCreate(𝔽(len)).
        let array = Array::create(vm, realm, length, None)?;

        // 4. Let k be 0.
        // 5. Repeat, while k < len,
        for k in 0..length {
            // a. Let from be ! ToString(𝔽(len - k - 1)).
            let from = property_key(length - k - 1);

            // b. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k);

            // c. Let fromValue be ? Get(O, from).
            let from_value = object.get(vm, &from)?;

            // d. Perform ! CreateDataPropertyOrThrow(A, Pk, fromValue).
            array
                .create_data_property_or_throw(vm, &property_key, from_value)
                .must();

            // e. Set k to k + 1.
        }

        // 6. Return A.
        Ok(Value::from_object(array))
    }

    // 23.1.3.34 Array.prototype.toSorted ( comparefn ), https://tc39.es/ecma262/#sec-array.prototype.tosorted
    fn to_sorted(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let comparefn = vm.argument(0);

        // 1. If comparefn is not undefined and IsCallable(comparefn) is false, throw a TypeError exception.
        if !comparefn.is_undefined() && !comparefn.is_function() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&comparefn]);
        }

        // 2. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 3. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 4. Let A be ? ArrayCreate(𝔽(len)).
        let array = Array::create(vm, realm, length, None)?;

        // 5. Let SortCompare be a new Abstract Closure with parameters (x, y) that captures comparefn and performs the following steps when called:
        let comparefn_function = (!comparefn.is_undefined()).then(|| comparefn.as_function());
        let sort_compare = |x: Value, y: Value| -> ThrowCompletionOr<f64> {
            // a. Return ? CompareArrayElements(x, y, comparefn).
            compare_array_elements(vm, x, y, comparefn_function)
        };

        // 6. Let sortedList be ? SortIndexedProperties(obj, len, SortCompare, read-through-holes).
        let sorted_list = sort_indexed_properties(vm, &object, length, &sort_compare, Holes::ReadThroughHoles)?;

        // 7. Let j be 0.
        // 8. Repeat, while j < len,
        for j in 0..length {
            // a. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(j)), sortedList[j]).
            let sorted_value = sorted_list
                .get(j as usize)
                .expect("reading through holes sorts every index");
            array
                .create_data_property_or_throw(vm, &property_key(j), sorted_value)
                .must();

            // b. Set j to j + 1.
        }

        // 9. Return A.
        Ok(Value::from_object(array))
    }

    // 23.1.3.35 Array.prototype.toSpliced ( start, skipCount, ...items ), https://tc39.es/ecma262/#sec-array.prototype.tospliced
    fn to_spliced(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let start = vm.argument(0);
        let skip_count = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. Let relativeStart be ? ToIntegerOrInfinity(start).
        let relative_start = start.to_integer_or_infinity(vm)?;

        // 4. If relativeStart is -∞, let actualStart be 0.
        let actual_start = if relative_start == f64::NEG_INFINITY {
            0
        }
        // 5. Else if relativeStart < 0, let actualStart be max(len + relativeStart, 0).
        else if relative_start < 0.0 {
            max(length as f64 + relative_start, 0.0) as u64
        }
        // 6. Else, let actualStart be min(relativeStart, len).
        else {
            min(relative_start, length as f64) as u64
        };

        // Sanity check
        assert!(actual_start <= length);

        // 7. Let insertCount be the number of elements in items.
        let insert_count = if vm.argument_count() >= 2 {
            vm.argument_count() - 2
        } else {
            0
        };

        // 8. If start is not present, then
        let actual_skip_count = if vm.argument_count() == 0 {
            // a. Let actualSkipCount be 0.
            0
        }
        // 9. Else if deleteCount is not present, then
        else if vm.argument_count() == 1 {
            // a. Let actualSkipCount be len - actualStart.
            length - actual_start
        }
        // 10. Else,
        else {
            // a. Let sc be ? ToIntegerOrInfinity(skipCount).
            let sc = skip_count.to_integer_or_infinity(vm)?;

            // b. Let actualSkipCount be the result of clamping sc between 0 and len - actualStart.
            clamp(sc, 0.0, (length - actual_start) as f64) as u64
        };

        // Sanity check
        assert!(actual_skip_count <= length - actual_start);

        // 11. Let newLen be len + insertCount - actualSkipCount.
        let new_length_double = length as f64 + insert_count as f64 - actual_skip_count as f64;

        // 12. If newLen > 2^53 - 1, throw a TypeError exception.
        if new_length_double > MAX_ARRAY_LIKE_INDEX {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::ArrayMaxSize, &[]);
        }

        let new_length = new_length_double as u64;

        // 13. Let A be ? ArrayCreate(𝔽(newLen)).
        let array = Array::create(vm, realm, new_length, None)?;

        // 14. Let i be 0.
        let mut i: u64 = 0;

        // 15. Let r be actualStart + actualSkipCount.
        let mut r = actual_start + actual_skip_count;

        // 16. Repeat, while i < actualStart,
        while i < actual_start {
            // a. Let Pi be ! ToString(𝔽(i)).
            let property_key = property_key(i);

            // b. Let iValue be ? Get(O, Pi).
            let i_value = object.get(vm, &property_key)?;

            // c. Perform ! CreateDataPropertyOrThrow(A, Pi, iValue).
            array.create_data_property_or_throw(vm, &property_key, i_value).must();

            // d. Set i to i + 1.
            i += 1;
        }

        // 17. For each element E of items, do
        for element_index in 2..vm.argument_count() {
            let element = vm.argument(element_index);

            // a. Let Pi be ! ToString(𝔽(i)).
            let property_key = property_key(i);

            // b. Perform ! CreateDataPropertyOrThrow(A, Pi, E).
            array.create_data_property_or_throw(vm, &property_key, element).must();

            // c. Set i to i + 1.
            i += 1;
        }

        // 18. Repeat, while i < newLen,
        while i < new_length {
            // a. Let Pi be ! ToString(𝔽(i)).
            let property_key = property_key(i);

            // b. Let from be ! ToString(𝔽(r)).
            let from = self::property_key(r);

            // c. Let fromValue be ? Get(O, from).
            let from_value = object.get(vm, &from)?;

            // d. Perform ! CreateDataPropertyOrThrow(A, Pi, fromValue).
            array
                .create_data_property_or_throw(vm, &property_key, from_value)
                .must();

            // e. Set i to i + 1.
            i += 1;

            // f. Set r to r + 1.
            r += 1;
        }

        // 19. Return A.
        Ok(Value::from_object(array))
    }

    // 23.1.3.36 Array.prototype.toString ( ), https://tc39.es/ecma262/#sec-array.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let array be ? ToObject(this value).
        let array = vm.this_value().to_object(vm)?;

        // 2. Let func be ? Get(array, "join").
        let mut func = array.get(vm, &vm.names.join)?;

        // 3. If IsCallable(func) is false, set func to the intrinsic function %Object.prototype.toString%.
        if !func.is_function() {
            func = Value::from_object(realm.intrinsics().object_prototype_to_string_function());
        }

        // 4. Return ? Call(func, array).
        call_function_object(vm, func.as_function(), Value::from_object(array), &[])
    }

    // 23.1.3.37 Array.prototype.unshift ( ...items ), https://tc39.es/ecma262/#sec-array.prototype.unshift
    fn unshift(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = vm.this_value().to_object(vm)?;
        let length = length_of_array_like(vm, &this_object)?;
        let arg_count = vm.argument_count() as u64;
        let new_length = length + arg_count;
        if arg_count > 0 {
            if new_length as f64 > MAX_ARRAY_LIKE_INDEX {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::ArrayMaxSize, &[]);
            }

            for k in (1..=length).rev() {
                let from = k - 1;
                let to = k + arg_count - 1;

                let from_present = this_object.has_property(vm, &property_key(from))?;
                if from_present {
                    let from_value = this_object.get(vm, &property_key(from))?;
                    this_object.set(vm, &property_key(to), from_value, ShouldThrowExceptions::Yes)?;
                } else {
                    this_object.delete_property_or_throw(vm, &property_key(to))?;
                }
            }

            for j in 0..arg_count {
                this_object.set(
                    vm,
                    &property_key(j),
                    vm.argument(j as usize),
                    ShouldThrowExceptions::Yes,
                )?;
            }
        }

        this_object.set(vm, &vm.names.length, number(new_length), ShouldThrowExceptions::Yes)?;
        Ok(number(new_length))
    }

    // 23.1.3.38 Array.prototype.values ( ), https://tc39.es/ecma262/#sec-array.prototype.values
    fn values(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let this_object = vm.this_value().to_object(vm)?;

        Ok(Value::from_object(ArrayIterator::create(
            vm,
            realm,
            Value::from_object(this_object),
            PropertyKind::Value,
        )))
    }

    // 23.1.3.39 Array.prototype.with ( index, value ), https://tc39.es/ecma262/#sec-array.prototype.with
    fn with(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let index = vm.argument(0);
        let value = vm.argument(1);

        // 1. Let O be ? ToObject(this value).
        let object = vm.this_value().to_object(vm)?;

        // 2. Let len be ? LengthOfArrayLike(O).
        let length = length_of_array_like(vm, &object)?;

        // 3. Let relativeIndex be ? ToIntegerOrInfinity(index).
        let relative_index = index.to_integer_or_infinity(vm)?;

        // 4. If relativeIndex ≥ 0, let actualIndex be relativeIndex.
        let actual_index = if relative_index >= 0.0 {
            relative_index
        }
        // 5. Else, let actualIndex be len + relativeIndex.
        else {
            length as f64 + relative_index
        };

        // 6. If actualIndex ≥ len or actualIndex < 0, throw a RangeError exception.
        if actual_index >= length as f64 || actual_index < 0.0 {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::IndexOutOfRange,
                &[&AkDouble(actual_index), &length],
            );
        }

        // 7. Let A be ? ArrayCreate(𝔽(len)).
        let array = Array::create(vm, realm, length, None)?;

        // 8. Let k be 0.
        // 9. Repeat, while k < len,
        for k in 0..length {
            // a. Let Pk be ! ToString(𝔽(k)).
            let property_key = property_key(k);

            // b. If k is actualIndex, let fromValue be value.
            let from_value = if k == actual_index as u64 {
                value
            }
            // c. Else, let fromValue be ? Get(O, Pk).
            else {
                object.get(vm, &property_key)?
            };

            // d. Perform ! CreateDataPropertyOrThrow(A, Pk, fromValue).
            array
                .create_data_property_or_throw(vm, &property_key, from_value)
                .must();

            // e. Set k to k + 1.
        }

        // 10. Return A.
        Ok(Value::from_object(array))
    }
}

// 10.4.2.3 ArraySpeciesCreate ( originalArray, length ), https://tc39.es/ecma262/#sec-arrayspeciescreate
fn array_species_create(vm: &Vm, original_array: &Object, length: u64) -> ThrowCompletionOr<Gc<Object>> {
    let realm = vm.current_realm().expect("a builtin runs in a realm");

    let is_array = Value::from_object(original_array.as_gc()).is_array(vm)?;

    if !is_array {
        return Ok(Array::create(vm, realm, length, None)?.upcast());
    }

    let mut constructor = original_array.get_with_cache(
        vm,
        &vm.names.constructor,
        vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::ArraySpeciesCreateConstructor),
    )?;
    if constructor.is_constructor() {
        let constructor_function = constructor.as_function();
        let this_realm = vm.current_realm();
        let constructor_realm = get_function_realm(vm, constructor_function)?;
        if Some(constructor_realm) != this_realm
            && constructor_function
                == constructor_realm
                    .intrinsics()
                    .array_constructor(vm)
                    .upcast::<FunctionObject>()
        {
            constructor = Value::UNDEFINED;
        }
    }

    if constructor.is_object() {
        constructor = constructor.as_object().get_with_cache(
            vm,
            &PropertyKey::from(vm.well_known_symbols().species),
            vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::ArraySpeciesCreateSpecies),
        )?;
        if constructor.is_null() {
            constructor = Value::UNDEFINED;
        }
    }

    if constructor.is_undefined() {
        return Ok(Array::create(vm, realm, length, None)?.upcast());
    }

    if !constructor.is_constructor() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAConstructor, &[&constructor]);
    }

    construct(vm, constructor.as_function(), &[number(length)], None)
}

fn can_use_packed_array_fast_path(array: &Array) -> bool {
    array.is_simple_packed_array() && array.default_prototype_chain_intact()
}

fn can_use_packed_or_empty_array_fast_path(array: &Array) -> bool {
    // An array that never had any elements has no indexed storage at all. Like a packed array, it has no holes that
    // could read through to the prototype chain.
    if array.indexed_storage_kind() == IndexedStorageKind::None && array.indexed_array_like_size() == 0 {
        return !array.is_proxy_target()
            && !array.may_interfere_with_indexed_property_access()
            && array.default_prototype_chain_intact();
    }
    can_use_packed_array_fast_path(array)
}

fn can_use_packed_shift_fast_path(array: &Array) -> bool {
    // Packed: every index in [0, size) is an own property, so memmove semantics match the spec even if the prototype
    // chain has indexed properties (HasProperty never escapes to the proto) and even if the array is non-extensible
    // (no new own properties are created).
    array.is_simple_packed_array() && array.length_is_writable()
}

fn can_use_holey_shift_fast_path(array: &Array) -> bool {
    // Holey: the spec path uses HasProperty + Get on every index, so a poisoned prototype changes the outcome on holes.
    // A set() on a hole slot also creates a new own property, which a non-extensible array rejects with TypeError.
    !array.is_proxy_target()
        && !array.may_interfere_with_indexed_property_access()
        && array.indexed_storage_kind() == IndexedStorageKind::Holey
        && array.default_prototype_chain_intact()
        && array.extensible()
        && array.length_is_writable()
}

fn fast_array_species_result(object: Gc<Object>) -> Option<Gc<Array>> {
    let array = object.downcast::<Array>()?;
    if array.is_proxy_target()
        || array.indexed_storage_kind() == IndexedStorageKind::Dictionary
        || !array.default_prototype_chain_intact()
        || !array.extensible()
        || !array.length_is_writable()
    {
        return None;
    }
    Some(array)
}

// 23.1.3.13.1 FlattenIntoArray ( target, source, sourceLen, start, depth [ , mapperFunction [ , thisArg ] ] ), https://tc39.es/ecma262/#sec-flattenintoarray
#[allow(clippy::too_many_arguments)]
fn flatten_into_array(
    vm: &Vm,
    new_array: &Object,
    array: &Object,
    array_length: u64,
    mut target_index: u64,
    depth: f64,
    mapper_func: Option<Gc<FunctionObject>>,
    this_arg: Value,
) -> ThrowCompletionOr<u64> {
    assert!(mapper_func.is_none() || (!this_arg.is_empty() && depth == 1.0));

    for j in 0..array_length {
        let value_exists = array.has_property(vm, &property_key(j))?;

        if !value_exists {
            continue;
        }
        let mut value = array.get(vm, &property_key(j))?;

        if let Some(mapper_func) = mapper_func {
            value = call_function_object(
                vm,
                mapper_func,
                this_arg,
                &[value, number(j), Value::from_object(array.as_gc())],
            )?;
        }

        if depth > 0.0 && value.is_array(vm)? {
            if vm.did_reach_stack_space_limit() {
                return vm.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
            }

            let length = length_of_array_like(vm, &value.as_object())?;
            target_index = flatten_into_array(
                vm,
                new_array,
                &value.as_object(),
                length,
                target_index,
                depth - 1.0,
                None,
                Value::UNDEFINED,
            )?;
            continue;
        }

        if target_index as f64 >= MAX_ARRAY_LIKE_INDEX {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::InvalidIndex, &[]);
        }

        new_array.create_data_property_or_throw(vm, &property_key(target_index), value)?;

        target_index += 1;
    }
    Ok(target_index)
}

/// Sorts `arr_to_sort` in place with a stable merge sort, stopping at the first comparison that throws.
pub fn array_merge_sort(
    vm: &Vm,
    compare_func: &dyn Fn(Value, Value) -> ThrowCompletionOr<f64>,
    arr_to_sort: &MarkedVec<'_, Value>,
) -> ThrowCompletionOr<()> {
    // FIXME: it would probably be better to switch to insertion sort for small arrays for
    // better performance
    let size = arr_to_sort.len();
    if size <= 1 {
        return Ok(());
    }

    let left = MarkedVec::with_capacity(vm, size / 2);
    let right = MarkedVec::with_capacity(vm, size / 2 + (size & 1));

    for i in 0..size {
        let value = arr_to_sort.get(i).expect("i is below the size");
        if i < size / 2 {
            left.push(value);
        } else {
            right.push(value);
        }
    }

    array_merge_sort(vm, compare_func, &left)?;
    array_merge_sort(vm, compare_func, &right)?;

    // NB: Every element is written back in order, so this overwrites arr_to_sort in place; a throwing comparison leaves
    //     it half merged.
    let mut sorted_count = 0;
    let mut append = |value: Value| {
        arr_to_sort.set(sorted_count, value);
        sorted_count += 1;
    };

    let mut left_index = 0;
    let mut right_index = 0;

    while left_index < left.len() && right_index < right.len() {
        let x = left.get(left_index).expect("the index is below the size");
        let y = right.get(right_index).expect("the index is below the size");

        let comparison_result = compare_func(x, y)?;

        if comparison_result <= 0.0 {
            append(x);
            left_index += 1;
        } else {
            append(y);
            right_index += 1;
        }
    }

    while left_index < left.len() {
        append(left.get(left_index).expect("the index is below the size"));
        left_index += 1;
    }

    while right_index < right.len() {
        append(right.get(right_index).expect("the index is below the size"));
        right_index += 1;
    }

    Ok(())
}
