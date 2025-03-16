/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2020, Marcin Gasperowicz <xnooga@gmail.com>
 * Copyright (c) 2021, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayConstructor.h>
#include <LibJS/Runtime/ArrayIterator.h>
#include <LibJS/Runtime/ArrayPrototype.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/ObjectPrototype.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

GC_DEFINE_ALLOCATOR(ArrayPrototype);

static HashTable<GC::Ref<Object>> s_array_join_seen_objects;

ArrayPrototype::ArrayPrototype(Realm& realm)
    : Array(realm.intrinsics().object_prototype())
{
}

void ArrayPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;

    define_native_function(realm, vm.names.at, at, 1, attr);
    define_native_function(realm, vm.names.concat, concat, 1, attr);
    define_native_function(realm, vm.names.copyWithin, copy_within, 2, attr);
    define_native_function(realm, vm.names.entries, entries, 0, attr);
    define_native_function(realm, vm.names.every, every, 1, attr);
    define_native_function(realm, vm.names.fill, fill, 1, attr);
    define_native_function(realm, vm.names.filter, filter, 1, attr);
    define_native_function(realm, vm.names.find, find, 1, attr);
    define_native_function(realm, vm.names.findIndex, find_index, 1, attr);
    define_native_function(realm, vm.names.findLast, find_last, 1, attr);
    define_native_function(realm, vm.names.findLastIndex, find_last_index, 1, attr);
    define_native_function(realm, vm.names.flat, flat, 0, attr);
    define_native_function(realm, vm.names.flatMap, flat_map, 1, attr);
    define_native_function(realm, vm.names.forEach, for_each, 1, attr);
    define_native_function(realm, vm.names.includes, includes, 1, attr);
    define_native_function(realm, vm.names.indexOf, index_of, 1, attr);
    define_native_function(realm, vm.names.join, join, 1, attr);
    define_native_function(realm, vm.names.keys, keys, 0, attr);
    define_native_function(realm, vm.names.lastIndexOf, last_index_of, 1, attr);
    define_native_function(realm, vm.names.map, map, 1, attr);
    define_native_function(realm, vm.names.pop, pop, 0, attr);
    define_native_function(realm, vm.names.push, push, 1, attr);
    define_native_function(realm, vm.names.reduce, reduce, 1, attr);
    define_native_function(realm, vm.names.reduceRight, reduce_right, 1, attr);
    define_native_function(realm, vm.names.reverse, reverse, 0, attr);
    define_native_function(realm, vm.names.shift, shift, 0, attr);
    define_native_function(realm, vm.names.slice, slice, 2, attr);
    define_native_function(realm, vm.names.some, some, 1, attr);
    define_native_function(realm, vm.names.sort, sort, 1, attr);
    define_native_function(realm, vm.names.splice, splice, 2, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toReversed, to_reversed, 0, attr);
    define_native_function(realm, vm.names.toSorted, to_sorted, 1, attr);
    define_native_function(realm, vm.names.toSpliced, to_spliced, 2, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.unshift, unshift, 1, attr);
    define_native_function(realm, vm.names.values, values, 0, attr);
    define_native_function(realm, vm.names.with, with, 2, attr);

    // Use define_direct_property here instead of define_native_function so that
    // Object.is(Array.prototype[Symbol.iterator], Array.prototype.values)
    // evaluates to true
    // 23.1.3.40 Array.prototype [ @@iterator ] ( ), https://tc39.es/ecma262/#sec-array.prototype-@@iterator
    define_direct_property(vm.well_known_symbol_iterator(), get_without_side_effects(vm.names.values), attr);

    // 23.1.3.41 Array.prototype [ @@unscopables ], https://tc39.es/ecma262/#sec-array.prototype-@@unscopables
    auto unscopable_list = Object::create(realm, nullptr);
    MUST(unscopable_list->create_data_property_or_throw(vm.names.at, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.copyWithin, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.entries, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.fill, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.find, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.findIndex, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.findLast, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.findLastIndex, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.flat, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.flatMap, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.includes, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.keys, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.toReversed, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.toSorted, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.toSpliced, Value(true)));
    MUST(unscopable_list->create_data_property_or_throw(vm.names.values, Value(true)));

    define_direct_property(vm.well_known_symbol_unscopables(), unscopable_list, Attribute::Configurable);
}

// 10.4.2.3 ArraySpeciesCreate ( originalArray, length ), https://tc39.es/ecma262/#sec-arrayspeciescreate
static ThrowCompletionOr<Object*> array_species_create(VM& vm, Object& original_array, size_t length)
{
    auto& realm = *vm.current_realm();

    auto is_array = TRY(Value(&original_array).is_array(vm));

    if (!is_array)
        return TRY(Array::create(realm, length)).ptr();

    auto constructor = TRY(original_array.get(vm.names.constructor));
    if (constructor.is_constructor()) {
        auto& constructor_function = constructor.as_function();
        auto* this_realm = vm.current_realm();
        auto* constructor_realm = TRY(get_function_realm(vm, constructor_function));
        if (constructor_realm != this_realm) {
            if (&constructor_function == constructor_realm->intrinsics().array_constructor())
                constructor = js_undefined();
        }
    }

    if (constructor.is_object()) {
        constructor = TRY(constructor.as_object().get(vm.well_known_symbol_species()));
        if (constructor.is_null())
            constructor = js_undefined();
    }

    if (constructor.is_undefined())
        return TRY(Array::create(realm, length)).ptr();

    if (!constructor.is_constructor())
        return vm.throw_completion<TypeError>(ErrorType::NotAConstructor, constructor.to_string_without_side_effects());

    return TRY(construct(vm, constructor.as_function(), Value(length))).ptr();
}

// 23.1.3.1 Array.prototype.at ( index ), https://tc39.es/ecma262/#sec-array.prototype.at
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::at)
{
    auto this_object = TRY(vm.this_value().to_object(vm));
    auto length = TRY(length_of_array_like(vm, this_object));
    auto relative_index = TRY(vm.argument(0).to_integer_or_infinity(vm));
    if (Value(relative_index).is_infinity())
        return js_undefined();
    Checked<size_t> index { 0 };
    if (relative_index >= 0) {
        index += relative_index;
    } else {
        index += length;
        index -= -relative_index;
    }
    if (index.has_overflow() || index.value() >= length)
        return js_undefined();
    return TRY(this_object->get(index.value()));
}

// 23.1.3.2 Array.prototype.concat ( ...items ), https://tc39.es/ecma262/#sec-array.prototype.concat
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::concat)
{
    auto this_object = TRY(vm.this_value().to_object(vm));

    auto* new_array = TRY(array_species_create(vm, this_object, 0));

    size_t n = 0;

    // 23.1.3.2.1 IsConcatSpreadable ( O ), https://tc39.es/ecma262/#sec-isconcatspreadable
    auto is_concat_spreadable = [&vm](Value const& val) -> ThrowCompletionOr<bool> {
        if (!val.is_object())
            return false;
        auto& object = val.as_object();
        auto spreadable = TRY(object.get(vm.well_known_symbol_is_concat_spreadable()));
        if (!spreadable.is_undefined())
            return spreadable.to_boolean();

        return TRY(val.is_array(vm));
    };

    auto append_to_new_array = [&vm, &is_concat_spreadable, &new_array, &n](Value arg) -> ThrowCompletionOr<void> {
        auto spreadable = TRY(is_concat_spreadable(arg));
        if (spreadable) {
            VERIFY(arg.is_object());
            Object& obj = arg.as_object();
            size_t k = 0;
            auto length = TRY(length_of_array_like(vm, obj));

            if (n + length > MAX_ARRAY_LIKE_INDEX)
                return vm.throw_completion<TypeError>(ErrorType::ArrayMaxSize);
            while (k < length) {
                auto k_exists = TRY(obj.has_property(k));
                if (k_exists) {
                    auto k_value = TRY(obj.get(k));
                    TRY(new_array->create_data_property_or_throw(n, k_value));
                }
                ++n;
                ++k;
            }
        } else {
            if (n >= MAX_ARRAY_LIKE_INDEX)
                return vm.throw_completion<TypeError>(ErrorType::ArrayMaxSize);
            TRY(new_array->create_data_property_or_throw(n, arg));
            ++n;
        }
        return {};
    };

    TRY(append_to_new_array(this_object));

    for (size_t i = 0; i < vm.argument_count(); ++i)
        TRY(append_to_new_array(vm.argument(i)));

    TRY(new_array->set(vm.names.length, Value(n), Object::ShouldThrowExceptions::Yes));
    return Value(new_array);
}

// 23.1.3.4 Array.prototype.copyWithin ( target, start [ , end ] ), https://tc39.es/ecma262/#sec-array.prototype.copywithin
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::copy_within)
{
    auto this_object = TRY(vm.this_value().to_object(vm));
    auto length = TRY(length_of_array_like(vm, this_object));

    auto relative_target = TRY(vm.argument(0).to_integer_or_infinity(vm));

    double to;
    if (relative_target < 0)
        to = max(length + relative_target, 0.0);
    else
        to = min(relative_target, (double)length);

    auto relative_start = TRY(vm.argument(1).to_integer_or_infinity(vm));

    double from;
    if (relative_start < 0)
        from = max(length + relative_start, 0.0);
    else
        from = min(relative_start, (double)length);

    auto relative_end = vm.argument(2).is_undefined() ? length : TRY(vm.argument(2).to_integer_or_infinity(vm));

    double final;
    if (relative_end < 0)
        final = max(length + relative_end, 0.0);
    else
        final = min(relative_end, (double)length);

    double count = min(final - from, length - to);

    i32 direction = 1;

    if (from < to && to < from + count) {
        direction = -1;
        from = from + count - 1;
        to = to + count - 1;
    }

    if (count < 0) {
        return this_object;
    }

    size_t from_i = from;
    size_t to_i = to;
    size_t count_i = count;

    while (count_i > 0) {
        auto from_present = TRY(this_object->has_property(from_i));

        if (from_present) {
            auto from_value = TRY(this_object->get(from_i));
            TRY(this_object->set(to_i, from_value, Object::ShouldThrowExceptions::Yes));
        } else {
            TRY(this_object->delete_property_or_throw(to_i));
        }

        from_i += direction;
        to_i += direction;
        --count_i;
    }

    return this_object;
}

// 23.1.3.5 Array.prototype.entries ( ), https://tc39.es/ecma262/#sec-array.prototype.entries
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::entries)
{
    auto& realm = *vm.current_realm();

    auto this_object = TRY(vm.this_value().to_object(vm));

    return ArrayIterator::create(realm, this_object, Object::PropertyKind::KeyAndValue);
}

// 23.1.3.6 Array.prototype.every ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.every
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::every)
{
    auto callback_function = vm.argument(0);
    auto this_arg = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
    if (!callback_function.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, callback_function.to_string_without_side_effects());

    // 4. Let k be 0.
    // 5. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // b. Let kPresent be ? HasProperty(O, Pk).
        auto k_present = TRY(object->has_property(property_key));

        // c. If kPresent is true, then
        if (k_present) {
            // i. Let kValue be ? Get(O, Pk).
            auto k_value = TRY(object->get(property_key));

            // ii. Let testResult be ToBoolean(? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »)).
            auto test_result = TRY(call(vm, callback_function.as_function(), this_arg, k_value, Value(k), object)).to_boolean();

            // iii. If testResult is false, return false.
            if (!test_result)
                return Value(false);
        }

        // d. Set k to k + 1.
    }

    // 6. Return true.
    return Value(true);
}

// 23.1.3.7 Array.prototype.fill ( value [ , start [ , end ] ] ), https://tc39.es/ecma262/#sec-array.prototype.fill
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::fill)
{
    auto this_object = TRY(vm.this_value().to_object(vm));

    auto length = TRY(length_of_array_like(vm, this_object));

    double relative_start = 0;
    double relative_end = length;

    if (vm.argument_count() >= 2) {
        relative_start = TRY(vm.argument(1).to_integer_or_infinity(vm));
        if (Value(relative_start).is_negative_infinity())
            relative_start = 0;
    }

    // If end is undefined, let relativeEnd be len; else let relativeEnd be ? ToIntegerOrInfinity(end).
    if (vm.argument_count() >= 3 && !vm.argument(2).is_undefined()) {
        relative_end = TRY(vm.argument(2).to_integer_or_infinity(vm));
        if (Value(relative_end).is_negative_infinity())
            relative_end = 0;
    }

    u64 from, to;

    if (relative_start < 0)
        from = max(length + relative_start, 0L);
    else
        from = min(relative_start, length);

    if (relative_end < 0)
        to = max(length + relative_end, 0L);
    else
        to = min(relative_end, length);

    for (u64 i = from; i < to; i++)
        TRY(this_object->set(i, vm.argument(0), Object::ShouldThrowExceptions::Yes));

    return this_object;
}

// 23.1.3.8 Array.prototype.filter ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.filter
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::filter)
{
    auto callback_function = vm.argument(0);
    auto this_arg = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
    if (!callback_function.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, callback_function.to_string_without_side_effects());

    // 4. Let A be ? ArraySpeciesCreate(O, 0).
    auto* array = TRY(array_species_create(vm, object, 0));

    // 5. Let k be 0.
    size_t k = 0;

    // 6. Let to be 0.
    size_t to = 0;

    // 7. Repeat, while k < len,
    for (; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // b. Let kPresent be ? HasProperty(O, Pk).
        auto k_present = TRY(object->has_property(property_key));

        // c. If kPresent is true, then
        if (k_present) {
            // i. Let kValue be ? Get(O, Pk).
            auto k_value = TRY(object->get(k));

            // ii. Let selected be ToBoolean(? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »)).
            auto selected = TRY(call(vm, callback_function.as_function(), this_arg, k_value, Value(k), object)).to_boolean();

            // iii. If selected is true, then
            if (selected) {
                // 1. Perform ? CreateDataPropertyOrThrow(A, ! ToString(𝔽(to)), kValue).
                TRY(array->create_data_property_or_throw(to, k_value));

                // 2. Set to to to + 1.
                ++to;
            }
        }

        // d. Set k to k + 1.
    }

    // 8. Return A.
    return array;
}

// 23.1.3.9 Array.prototype.find ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.find
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::find)
{
    auto predicate = vm.argument(0);
    auto this_arg = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If IsCallable(predicate) is false, throw a TypeError exception.
    if (!predicate.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, predicate.to_string_without_side_effects());

    // 4. Let k be 0.
    // 5. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // b. Let kValue be ? Get(O, Pk).
        auto k_value = TRY(object->get(property_key));

        // c. Let testResult be ToBoolean(? Call(predicate, thisArg, « kValue, 𝔽(k), O »)).
        auto test_result = TRY(call(vm, predicate.as_function(), this_arg, k_value, Value(k), object)).to_boolean();

        // d. If testResult is true, return kValue.
        if (test_result)
            return k_value;

        // e. Set k to k + 1.
    }

    // 6. Return undefined.
    return js_undefined();
}

// 23.1.3.10 Array.prototype.findIndex ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.findindex
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::find_index)
{
    auto predicate = vm.argument(0);
    auto this_arg = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If IsCallable(predicate) is false, throw a TypeError exception.
    if (!predicate.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, predicate.to_string_without_side_effects());

    // 4. Let k be 0.
    // 5. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // b. Let kValue be ? Get(O, Pk).
        auto k_value = TRY(object->get(property_key));

        // c. Let testResult be ToBoolean(? Call(predicate, thisArg, « kValue, 𝔽(k), O »)).
        auto test_result = TRY(call(vm, predicate.as_function(), this_arg, k_value, Value(k), object)).to_boolean();

        // d. If testResult is true, return 𝔽(k).
        if (test_result)
            return Value(k);

        // e. Set k to k + 1.
    }

    // 6. Return -1𝔽.
    return Value(-1);
}

// 23.1.3.11 Array.prototype.findLast ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.findlast
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::find_last)
{
    auto predicate = vm.argument(0);
    auto this_arg = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If IsCallable(predicate) is false, throw a TypeError exception.
    if (!predicate.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, predicate.to_string_without_side_effects());

    // 4. Let k be len - 1.
    // 5. Repeat, while k ≥ 0,
    for (i64 k = static_cast<i64>(length) - 1; k >= 0; --k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // b. Let kValue be ? Get(O, Pk).
        auto k_value = TRY(object->get(property_key));

        // c. Let testResult be ToBoolean(? Call(predicate, thisArg, « kValue, 𝔽(k), O »)).
        auto test_result = TRY(call(vm, predicate.as_function(), this_arg, k_value, Value((double)k), object)).to_boolean();

        // d. If testResult is true, return kValue.
        if (test_result)
            return k_value;

        // e. Set k to k - 1.
    }

    // 6. Return undefined.
    return js_undefined();
}

// 23.1.3.12 Array.prototype.findLastIndex ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.findlastindex
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::find_last_index)
{
    auto predicate = vm.argument(0);
    auto this_arg = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If IsCallable(predicate) is false, throw a TypeError exception.
    if (!predicate.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, predicate.to_string_without_side_effects());

    // 4. Let k be len - 1.
    // 5. Repeat, while k ≥ 0,
    for (i64 k = static_cast<i64>(length) - 1; k >= 0; --k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // b. Let kValue be ? Get(O, Pk).
        auto k_value = TRY(object->get(property_key));

        // c. Let testResult be ToBoolean(? Call(predicate, thisArg, « kValue, 𝔽(k), O »)).
        auto test_result = TRY(call(vm, predicate.as_function(), this_arg, k_value, Value((double)k), object)).to_boolean();

        // d. If testResult is true, return 𝔽(k).
        if (test_result)
            return Value((double)k);

        // e. Set k to k - 1.
    }

    // 6. Return -1𝔽.
    return Value(-1);
}

// 23.1.3.13.1 FlattenIntoArray ( target, source, sourceLen, start, depth [ , mapperFunction [ , thisArg ] ] ), https://tc39.es/ecma262/#sec-flattenintoarray
static ThrowCompletionOr<size_t> flatten_into_array(VM& vm, Object& new_array, Object& array, size_t array_length, size_t target_index, double depth, FunctionObject* mapper_func = {}, Value this_arg = {})
{
    VERIFY(!mapper_func || (!this_arg.is_empty() && depth == 1));

    for (size_t j = 0; j < array_length; ++j) {
        auto value_exists = TRY(array.has_property(j));

        if (!value_exists)
            continue;
        auto value = TRY(array.get(j));

        if (mapper_func)
            value = TRY(call(vm, *mapper_func, this_arg, value, Value(j), &array));

        if (depth > 0 && TRY(value.is_array(vm))) {
            if (vm.did_reach_stack_space_limit())
                return vm.throw_completion<InternalError>(ErrorType::CallStackSizeExceeded);

            auto length = TRY(length_of_array_like(vm, value.as_object()));
            target_index = TRY(flatten_into_array(vm, new_array, value.as_object(), length, target_index, depth - 1));
            continue;
        }

        if (target_index >= MAX_ARRAY_LIKE_INDEX)
            return vm.throw_completion<TypeError>(ErrorType::InvalidIndex);

        TRY(new_array.create_data_property_or_throw(target_index, value));

        ++target_index;
    }
    return target_index;
}

// 23.1.3.13 Array.prototype.flat ( [ depth ] ), https://tc39.es/ecma262/#sec-array.prototype.flat
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::flat)
{
    auto this_object = TRY(vm.this_value().to_object(vm));

    auto length = TRY(length_of_array_like(vm, this_object));

    double depth = 1;
    if (!vm.argument(0).is_undefined()) {
        auto depth_num = TRY(vm.argument(0).to_integer_or_infinity(vm));
        depth = max(depth_num, 0.0);
    }

    auto* new_array = TRY(array_species_create(vm, this_object, 0));

    TRY(flatten_into_array(vm, *new_array, this_object, length, 0, depth));
    return new_array;
}

// 23.1.3.14 Array.prototype.flatMap ( mapperFunction [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.flatmap
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::flat_map)
{
    auto mapper_function = vm.argument(0);
    auto this_arg = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let sourceLen be ? LengthOfArrayLike(O).
    auto source_length = TRY(length_of_array_like(vm, object));

    // 3. If IsCallable(mapperFunction) is false, throw a TypeError exception.
    if (!mapper_function.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, mapper_function.to_string_without_side_effects());

    // 4. Let A be ? ArraySpeciesCreate(O, 0).
    auto* array = TRY(array_species_create(vm, object, 0));

    // 5. Perform ? FlattenIntoArray(A, O, sourceLen, 0, 1, mapperFunction, thisArg).
    TRY(flatten_into_array(vm, *array, object, source_length, 0, 1, &mapper_function.as_function(), this_arg));

    // 6. Return A.
    return array;
}

// 23.1.3.15 Array.prototype.forEach ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.foreach
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::for_each)
{
    auto callback_function = vm.argument(0);
    auto this_arg = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
    if (!callback_function.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, callback_function.to_string_without_side_effects());

    // 4. Let k be 0.
    // 5. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // b. Let kPresent be ? HasProperty(O, Pk).
        auto k_present = TRY(object->has_property(property_key));

        // c. If kPresent is true, then
        if (k_present) {
            // i. Let kValue be ? Get(O, Pk).
            auto k_value = TRY(object->get(property_key));

            // ii. Perform ? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »).
            TRY(call(vm, callback_function.as_function(), this_arg, k_value, Value(k), object));
        }

        // d. Set k to k + 1.
    }

    // 6. Return undefined.
    return js_undefined();
}

// 23.1.3.16 Array.prototype.includes ( searchElement [ , fromIndex ] ), https://tc39.es/ecma262/#sec-array.prototype.includes
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::includes)
{
    auto this_object = TRY(vm.this_value().to_object(vm));
    auto length = TRY(length_of_array_like(vm, this_object));
    if (length == 0)
        return Value(false);
    u64 from_index = 0;
    if (vm.argument_count() >= 2) {
        auto from_argument = TRY(vm.argument(1).to_integer_or_infinity(vm));

        if (Value(from_argument).is_positive_infinity() || from_argument >= length)
            return Value(false);

        if (Value(from_argument).is_negative_infinity())
            from_argument = 0;

        if (from_argument < 0)
            from_index = max(length + from_argument, 0);
        else
            from_index = from_argument;
    }
    auto value_to_find = vm.argument(0);
    for (u64 i = from_index; i < length; ++i) {
        auto element = TRY(this_object->get(i));
        if (same_value_zero(element, value_to_find))
            return Value(true);
    }
    return Value(false);
}

// 23.1.3.17 Array.prototype.indexOf ( searchElement [ , fromIndex ] ), https://tc39.es/ecma262/#sec-array.prototype.indexof
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::index_of)
{
    auto search_element = vm.argument(0);
    auto from_index = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If len is 0, return -1𝔽.
    if (length == 0)
        return Value(-1);

    // 4. Let n be ? ToIntegerOrInfinity(fromIndex).
    auto n = TRY(from_index.to_integer_or_infinity(vm));

    // 5. Assert: If fromIndex is undefined, then n is 0.
    if (from_index.is_undefined())
        VERIFY(n == 0);

    // 6. If n is +∞, return -1𝔽.
    if (Value(n).is_positive_infinity())
        return Value(-1);

    // 7. Else if n is -∞, set n to 0.
    if (Value(n).is_negative_infinity())
        n = 0;

    size_t k;

    // 8. If n ≥ 0, then
    if (n >= 0) {
        // a. Let k be n.
        k = (size_t)n;
    }
    // 9. Else,
    else {
        // a. Let k be len + n.
        // b. If k < 0, set k to 0.
        k = max(length + n, 0);
    }

    // 10. Repeat, while k < len,
    for (; k < length; ++k) {
        auto property_key = PropertyKey { k };

        // a. Let kPresent be ? HasProperty(O, ! ToString(𝔽(k))).
        auto k_present = TRY(object->has_property(property_key));

        // b. If kPresent is true, then
        if (k_present) {
            // i. Let elementK be ? Get(O, ! ToString(𝔽(k))).
            auto element_k = TRY(object->get(property_key));

            // ii. Let same be IsStrictlyEqual(searchElement, elementK).
            auto same = is_strictly_equal(search_element, element_k);

            // iii. If same is true, return 𝔽(k).
            if (same)
                return Value(k);
        }

        // c. Set k to k + 1.
    }

    // 11. Return -1𝔽.
    return Value(-1);
}

// 23.1.3.18 Array.prototype.join ( separator ), https://tc39.es/ecma262/#sec-array.prototype.join
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::join)
{
    auto this_object = TRY(vm.this_value().to_object(vm));

    // This is not part of the spec, but all major engines do some kind of circular reference checks.
    // FWIW: engine262, a "100% spec compliant" ECMA-262 impl, aborts with "too much recursion".
    // Same applies to Array.prototype.toLocaleString().
    if (s_array_join_seen_objects.contains(this_object))
        return PrimitiveString::create(vm, String {});
    s_array_join_seen_objects.set(this_object);
    ArmedScopeGuard unsee_object_guard = [&] {
        s_array_join_seen_objects.remove(this_object);
    };

    auto length = TRY(length_of_array_like(vm, this_object));
    String separator = ","_string;
    if (!vm.argument(0).is_undefined())
        separator = TRY(vm.argument(0).to_string(vm));
    StringBuilder builder;
    for (size_t i = 0; i < length; ++i) {
        if (i > 0)
            builder.append(separator);
        auto value = TRY(this_object->get(i));
        if (value.is_nullish())
            continue;
        auto string = TRY(value.to_string(vm));
        builder.append(string);
    }

    return PrimitiveString::create(vm, builder.to_string_without_validation());
}

// 23.1.3.19 Array.prototype.keys ( ), https://tc39.es/ecma262/#sec-array.prototype.keys
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::keys)
{
    auto& realm = *vm.current_realm();

    auto this_object = TRY(vm.this_value().to_object(vm));

    return ArrayIterator::create(realm, this_object, Object::PropertyKind::Key);
}

// 23.1.3.20 Array.prototype.lastIndexOf ( searchElement [ , fromIndex ] ), https://tc39.es/ecma262/#sec-array.prototype.lastindexof
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::last_index_of)
{
    auto search_element = vm.argument(0);
    auto from_index = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If len is 0, return -1𝔽.
    if (length == 0)
        return Value(-1);

    double n;

    // 4. If fromIndex is present, let n be ? ToIntegerOrInfinity(fromIndex); else let n be len - 1.
    if (vm.argument_count() >= 2)
        n = TRY(from_index.to_integer_or_infinity(vm));
    else
        n = (double)length - 1;

    // 5. If n is -∞, return -1𝔽.
    if (Value(n).is_negative_infinity())
        return Value(-1);

    ssize_t k;

    // 6. If n ≥ 0, then
    if (n >= 0) {
        // a. Let k be min(n, len - 1).
        k = min(n, (double)length - 1);
    }
    // 7. Else,
    else {
        //  a. Let k be len + n.
        k = (double)length + n;
    }

    // 8. Repeat, while k ≥ 0,
    for (; k >= 0; --k) {
        auto property_key = PropertyKey { k };

        // a. Let kPresent be ? HasProperty(O, ! ToString(𝔽(k))).
        auto k_present = TRY(object->has_property(property_key));

        // b. If kPresent is true, then
        if (k_present) {
            // i. Let elementK be ? Get(O, ! ToString(𝔽(k))).
            auto element_k = TRY(object->get(property_key));

            // ii. Let same be IsStrictlyEqual(searchElement, elementK).
            auto same = is_strictly_equal(search_element, element_k);

            // iii. If same is true, return 𝔽(k).
            if (same)
                return Value((size_t)k);
        }

        // c. Set k to k - 1.
    }

    // 9. Return -1𝔽.
    return Value(-1);
}

// 23.1.3.21 Array.prototype.map ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.map
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::map)
{
    auto callback_function = vm.argument(0);
    auto this_arg = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
    if (!callback_function.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, callback_function.to_string_without_side_effects());

    // 4. Let A be ? ArraySpeciesCreate(O, len).
    auto* array = TRY(array_species_create(vm, object, length));

    // 5. Let k be 0.
    // 6. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // b. Let kPresent be ? HasProperty(O, Pk).
        auto k_present = TRY(object->has_property(property_key));

        // c. If kPresent is true, then
        if (k_present) {
            // i. Let kValue be ? Get(O, Pk).
            auto k_value = TRY(object->get(property_key));

            // ii. Let mappedValue be ? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »).
            auto mapped_value = TRY(call(vm, callback_function.as_function(), this_arg, k_value, Value(k), object));

            // iii. Perform ? CreateDataPropertyOrThrow(A, Pk, mappedValue).
            TRY(array->create_data_property_or_throw(property_key, mapped_value));
        }

        // d. Set k to k + 1.
    }

    // 7. Return A.
    return array;
}

// 23.1.3.22 Array.prototype.pop ( ), https://tc39.es/ecma262/#sec-array.prototype.pop
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::pop)
{
    auto this_object = TRY(vm.this_value().to_object(vm));
    auto length = TRY(length_of_array_like(vm, this_object));
    if (length == 0) {
        TRY(this_object->set(vm.names.length, Value(0), Object::ShouldThrowExceptions::Yes));
        return js_undefined();
    }
    auto index = length - 1;
    auto element = TRY(this_object->get(index));
    TRY(this_object->delete_property_or_throw(index));
    TRY(this_object->set(vm.names.length, Value(index), Object::ShouldThrowExceptions::Yes));
    return element;
}

// 23.1.3.23 Array.prototype.push ( ...items ), https://tc39.es/ecma262/#sec-array.prototype.push
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::push)
{
    auto this_object = TRY(vm.this_value().to_object(vm));
    auto length = TRY(length_of_array_like(vm, this_object));
    auto argument_count = vm.argument_count();
    auto new_length = length + argument_count;
    if (new_length > MAX_ARRAY_LIKE_INDEX)
        return vm.throw_completion<TypeError>(ErrorType::ArrayMaxSize);
    for (size_t i = 0; i < argument_count; ++i)
        TRY(this_object->set(length + i, vm.argument(i), Object::ShouldThrowExceptions::Yes));
    auto new_length_value = Value(new_length);
    TRY(this_object->set(vm.names.length, new_length_value, Object::ShouldThrowExceptions::Yes));
    return new_length_value;
}

// 23.1.3.24 Array.prototype.reduce ( callbackfn [ , initialValue ] ), https://tc39.es/ecma262/#sec-array.prototype.reduce
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::reduce)
{
    auto callback_function = vm.argument(0);
    auto initial_value = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
    if (!callback_function.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, callback_function.to_string_without_side_effects());

    // 4. If len = 0 and initialValue is not present, throw a TypeError exception.
    if (length == 0 && vm.argument_count() <= 1)
        return vm.throw_completion<TypeError>(ErrorType::ReduceNoInitial);

    // 5. Let k be 0.
    size_t k = 0;

    // 6. Let accumulator be undefined.
    auto accumulator = js_undefined();

    // 7. If initialValue is present, then
    if (vm.argument_count() > 1) {
        // a. Set accumulator to initialValue.
        accumulator = initial_value;
    }
    // 8. Else,
    else {
        // a. Let kPresent be false.
        bool k_present = false;

        // b. Repeat, while kPresent is false and k < len,
        for (; !k_present && k < length; ++k) {
            // i. Let Pk be ! ToString(𝔽(k)).
            auto property_key = PropertyKey { k };

            // ii. Set kPresent to ? HasProperty(O, Pk).
            k_present = TRY(object->has_property(property_key));

            // iii. If kPresent is true, then
            if (k_present) {
                // 1. Set accumulator to ? Get(O, Pk).
                accumulator = TRY(object->get(property_key));
            }

            // iv. Set k to k + 1.
        }

        // c. If kPresent is false, throw a TypeError exception.
        if (!k_present)
            return vm.throw_completion<TypeError>(ErrorType::ReduceNoInitial);
    }

    // 9. Repeat, while k < len,
    for (; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // b. Let kPresent be ? HasProperty(O, Pk).
        auto k_present = TRY(object->has_property(property_key));

        // c. If kPresent is true, then
        if (k_present) {
            // i. Let kValue be ? Get(O, Pk).
            auto k_value = TRY(object->get(property_key));

            // ii. Set accumulator to ? Call(callbackfn, undefined, « accumulator, kValue, 𝔽(k), O »).
            accumulator = TRY(call(vm, callback_function.as_function(), js_undefined(), accumulator, k_value, Value(k), object));
        }

        // d. Set k to k + 1.
    }

    // 10. Return accumulator.
    return accumulator;
}

// 23.1.3.25 Array.prototype.reduceRight ( callbackfn [ , initialValue ] ), https://tc39.es/ecma262/#sec-array.prototype.reduceright
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::reduce_right)
{
    auto callback_function = vm.argument(0);
    auto initial_value = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
    if (!callback_function.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, callback_function.to_string_without_side_effects());

    // 4. If len = 0 and initialValue is not present, throw a TypeError exception.
    if (length == 0 && vm.argument_count() <= 1)
        return vm.throw_completion<TypeError>(ErrorType::ReduceNoInitial);

    // 5. Let k be len - 1.
    ssize_t k = length - 1;

    // 6. Let accumulator be undefined.
    auto accumulator = js_undefined();

    // 7. If initialValue is present, then
    if (vm.argument_count() > 1) {
        // a. Set accumulator to initialValue.
        accumulator = initial_value;
    }
    // 8. Else,
    else {
        // a. Let kPresent be false.
        bool k_present = false;

        // b. Repeat, while kPresent is false and k ≥ 0,
        for (; !k_present && k >= 0; --k) {
            // i. Let Pk be ! ToString(𝔽(k)).
            auto property_key = PropertyKey { k };

            // ii. Set kPresent to ? HasProperty(O, Pk).
            k_present = TRY(object->has_property(property_key));

            // iii. If kPresent is true, then
            if (k_present) {
                // 1. Set accumulator to ? Get(O, Pk).
                accumulator = TRY(object->get(property_key));
            }

            // iv. Set k to k - 1.
        }

        // c. If kPresent is false, throw a TypeError exception.
        if (!k_present)
            return vm.throw_completion<TypeError>(ErrorType::ReduceNoInitial);
    }

    // 9. Repeat, while k ≥ 0,
    for (; k >= 0; --k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // b. Let kPresent be ? HasProperty(O, Pk).
        auto k_present = TRY(object->has_property(property_key));

        // c. If kPresent is true, then
        if (k_present) {
            // i. Let kValue be ? Get(O, Pk).
            auto k_value = TRY(object->get(property_key));

            // ii. Set accumulator to ? Call(callbackfn, undefined, « accumulator, kValue, 𝔽(k), O »).
            accumulator = TRY(call(vm, callback_function.as_function(), js_undefined(), accumulator, k_value, Value((size_t)k), object));
        }

        // d. Set k to k - 1.
    }

    // 10. Return accumulator.
    return accumulator;
}

// 23.1.3.26 Array.prototype.reverse ( ), https://tc39.es/ecma262/#sec-array.prototype.reverse
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::reverse)
{
    auto this_object = TRY(vm.this_value().to_object(vm));
    auto length = TRY(length_of_array_like(vm, this_object));

    auto middle = length / 2;
    for (size_t lower = 0; lower < middle; ++lower) {
        auto upper = length - lower - 1;

        auto lower_exists = TRY(this_object->has_property(lower));
        Value lower_value;
        if (lower_exists)
            lower_value = TRY(this_object->get(lower));

        auto upper_exists = TRY(this_object->has_property(upper));
        Value upper_value;
        if (upper_exists)
            upper_value = TRY(this_object->get(upper));

        if (lower_exists && upper_exists) {
            TRY(this_object->set(lower, upper_value, Object::ShouldThrowExceptions::Yes));
            TRY(this_object->set(upper, lower_value, Object::ShouldThrowExceptions::Yes));
        } else if (!lower_exists && upper_exists) {
            TRY(this_object->set(lower, upper_value, Object::ShouldThrowExceptions::Yes));
            TRY(this_object->delete_property_or_throw(upper));
        } else if (lower_exists && !upper_exists) {
            TRY(this_object->delete_property_or_throw(lower));
            TRY(this_object->set(upper, lower_value, Object::ShouldThrowExceptions::Yes));
        }
    }

    return this_object;
}

// 23.1.3.27 Array.prototype.shift ( ), https://tc39.es/ecma262/#sec-array.prototype.shift
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::shift)
{
    auto this_object = TRY(vm.this_value().to_object(vm));
    auto length = TRY(length_of_array_like(vm, this_object));
    if (length == 0) {
        TRY(this_object->set(vm.names.length, Value(0), Object::ShouldThrowExceptions::Yes));
        return js_undefined();
    }
    auto first = TRY(this_object->get(0));
    for (size_t k = 1; k < length; ++k) {
        size_t from = k;
        size_t to = k - 1;
        bool from_present = TRY(this_object->has_property(from));
        if (from_present) {
            auto from_value = TRY(this_object->get(from));
            TRY(this_object->set(to, from_value, Object::ShouldThrowExceptions::Yes));
        } else {
            TRY(this_object->delete_property_or_throw(to));
        }
    }

    TRY(this_object->delete_property_or_throw(length - 1));
    TRY(this_object->set(vm.names.length, Value(length - 1), Object::ShouldThrowExceptions::Yes));
    return first;
}

// 23.1.3.28 Array.prototype.slice ( start, end ), https://tc39.es/ecma262/#sec-array.prototype.slice
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::slice)
{
    auto this_object = TRY(vm.this_value().to_object(vm));

    auto initial_length = TRY(length_of_array_like(vm, this_object));

    auto relative_start = TRY(vm.argument(0).to_integer_or_infinity(vm));

    double actual_start;

    if (Value(relative_start).is_negative_infinity())
        actual_start = 0.0;
    else if (relative_start < 0.0)
        actual_start = max((double)initial_length + relative_start, 0.0);
    else
        actual_start = min(relative_start, (double)initial_length);

    double relative_end;

    if (vm.argument(1).is_undefined() || vm.argument(1).is_empty())
        relative_end = (double)initial_length;
    else
        relative_end = TRY(vm.argument(1).to_integer_or_infinity(vm));

    double final;

    if (Value(relative_end).is_negative_infinity())
        final = 0.0;
    else if (relative_end < 0.0)
        final = max((double)initial_length + relative_end, 0.0);
    else
        final = min(relative_end, (double)initial_length);

    auto count = max(final - actual_start, 0.0);

    auto* new_array = TRY(array_species_create(vm, this_object, count));

    size_t index = 0;
    size_t k = actual_start;

    while (k < final) {
        bool present = TRY(this_object->has_property(k));
        if (present) {
            auto value = TRY(this_object->get(k));
            TRY(new_array->create_data_property_or_throw(index, value));
        }

        ++k;
        ++index;
    }

    TRY(new_array->set(vm.names.length, Value(index), Object::ShouldThrowExceptions::Yes));
    return new_array;
}

// 23.1.3.29 Array.prototype.some ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-array.prototype.some
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::some)
{
    auto callback_function = vm.argument(0);
    auto this_arg = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. If IsCallable(callbackfn) is false, throw a TypeError exception.
    if (!callback_function.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, callback_function.to_string_without_side_effects());

    // 4. Let k be 0.
    // 5. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // b. Let kPresent be ? HasProperty(O, Pk).
        auto k_present = TRY(object->has_property(property_key));

        // c. If kPresent is true, then
        if (k_present) {
            // i. Let kValue be ? Get(O, Pk).
            auto k_value = TRY(object->get(property_key));

            // ii. Let testResult be ToBoolean(? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »)).
            auto test_result = TRY(call(vm, callback_function.as_function(), this_arg, k_value, Value(k), object)).to_boolean();

            // iii. If testResult is true, return true.
            if (test_result)
                return Value(true);
        }

        // d. Set k to k + 1.
    }

    // 6. Return false.
    return Value(false);
}

ThrowCompletionOr<void> array_merge_sort(VM& vm, Function<ThrowCompletionOr<double>(Value, Value)> const& compare_func, GC::RootVector<Value>& arr_to_sort)
{
    // FIXME: it would probably be better to switch to insertion sort for small arrays for
    // better performance
    if (arr_to_sort.size() <= 1)
        return {};

    GC::RootVector<Value> left(vm.heap());
    GC::RootVector<Value> right(vm.heap());

    left.ensure_capacity(arr_to_sort.size() / 2);
    right.ensure_capacity(arr_to_sort.size() / 2 + (arr_to_sort.size() & 1));

    for (size_t i = 0; i < arr_to_sort.size(); ++i) {
        if (i < arr_to_sort.size() / 2) {
            left.append(arr_to_sort[i]);
        } else {
            right.append(arr_to_sort[i]);
        }
    }

    TRY(array_merge_sort(vm, compare_func, left));
    TRY(array_merge_sort(vm, compare_func, right));

    arr_to_sort.clear();

    size_t left_index = 0, right_index = 0;

    while (left_index < left.size() && right_index < right.size()) {
        auto x = left[left_index];
        auto y = right[right_index];

        double comparison_result = TRY(compare_func(x, y));

        if (comparison_result <= 0) {
            arr_to_sort.append(x);
            left_index++;
        } else {
            arr_to_sort.append(y);
            right_index++;
        }
    }

    while (left_index < left.size()) {
        arr_to_sort.append(left[left_index]);
        left_index++;
    }

    while (right_index < right.size()) {
        arr_to_sort.append(right[right_index]);
        right_index++;
    }

    return {};
}

// 23.1.3.30 Array.prototype.sort ( comparefn ), https://tc39.es/ecma262/#sec-array.prototype.sort
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::sort)
{
    // 1. If comparefn is not undefined and IsCallable(comparefn) is false, throw a TypeError exception.
    auto comparefn = vm.argument(0);
    if (!comparefn.is_undefined() && !comparefn.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, comparefn.to_string_without_side_effects());

    // 2. Let obj be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 3. Let len be ? LengthOfArrayLike(obj).
    auto length = TRY(length_of_array_like(vm, object));

    // 4. Let SortCompare be a new Abstract Closure with parameters (x, y) that captures comparefn and performs the following steps when called:
    Function<ThrowCompletionOr<double>(Value, Value)> sort_compare = [&](auto x, auto y) -> ThrowCompletionOr<double> {
        // a. Return ? CompareArrayElements(x, y, comparefn).
        return TRY(compare_array_elements(vm, x, y, comparefn.is_undefined() ? nullptr : &comparefn.as_function()));
    };

    // 5. Let sortedList be ? SortIndexedProperties(obj, len, SortCompare, skip-holes).
    auto sorted_list = TRY(sort_indexed_properties(vm, object, length, sort_compare, Holes::SkipHoles));

    // 6. Let itemCount be the number of elements in sortedList.
    auto item_count = sorted_list.size();

    // 7. Let j be 0.
    size_t j = 0;

    // 8. Repeat, while j < itemCount,
    for (; j < item_count; ++j) {
        // a. Perform ? Set(obj, ! ToString(𝔽(j)), sortedList[j], true).
        TRY(object->set(j, sorted_list[j], Object::ShouldThrowExceptions::Yes));
        // b. Set j to j + 1.
    }

    // 9. NOTE: The call to SortIndexedProperties in step 5 uses skip-holes. The remaining indices are deleted to preserve the number of holes that were detected and excluded from the sort.
    // 10. Repeat, while j < len,
    for (; j < length; ++j) {
        // a. Perform ? DeletePropertyOrThrow(obj, ! ToString(𝔽(j))).
        TRY(object->delete_property_or_throw(j));
        // b. Set j to j + 1.
    }

    // 11. Return obj.
    return object;
}

// 23.1.3.31 Array.prototype.splice ( start, deleteCount, ...items ), https://tc39.es/ecma262/#sec-array.prototype.splice
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::splice)
{
    // 1. Let O be ? ToObject(this value).
    auto this_object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto initial_length = TRY(length_of_array_like(vm, this_object));

    // 3. Let relativeStart be ? ToIntegerOrInfinity(start).
    auto relative_start = TRY(vm.argument(0).to_integer_or_infinity(vm));

    u64 actual_start;

    // 4. If relativeStart = -∞, let actualStart be 0.
    if (Value(relative_start).is_negative_infinity())
        actual_start = 0;
    // 5. Else if relativeStart < 0, let actualStart be max(len + relativeStart, 0).
    else if (relative_start < 0)
        actual_start = max((ssize_t)initial_length + relative_start, (ssize_t)0);
    // 6. Else, let actualStart be min(relativeStart, len).
    else
        actual_start = min(relative_start, initial_length);

    // 7. Let itemCount be the number of elements in items.
    u64 item_count = vm.argument_count() >= 2 ? vm.argument_count() - 2 : 0;

    u64 actual_delete_count;

    // 8. If start is not present, then
    if (vm.argument_count() == 0) {
        // a. Let actualDeleteCount be 0.
        actual_delete_count = 0;
    }
    // 9. Else if deleteCount is not present, then
    else if (vm.argument_count() == 1) {
        // a. Let actualDeleteCount be len - actualStart.
        actual_delete_count = initial_length - actual_start;
    }
    // 10. Else,
    else {
        // a. Let dc be ? ToIntegerOrInfinity(deleteCount).
        auto delete_count = TRY(vm.argument(1).to_integer_or_infinity(vm));

        // b. Let actualDeleteCount be the result of clamping dc between 0 and len - actualStart.
        actual_delete_count = clamp(delete_count, 0, initial_length - actual_start);
    }

    // 11. If len + itemCount - actualDeleteCount > 2^53 - 1, throw a TypeError exception.
    if (initial_length + item_count - actual_delete_count > MAX_ARRAY_LIKE_INDEX)
        return vm.throw_completion<TypeError>(ErrorType::ArrayMaxSize);

    // 12. Let A be ? ArraySpeciesCreate(O, actualDeleteCount).
    auto* removed_elements = TRY(array_species_create(vm, this_object, actual_delete_count));

    // 13. Let k be 0.
    // 14. Repeat, while k < actualDeleteCount,
    for (u64 k = 0; k < actual_delete_count; ++k) {
        // a. Let from be ! ToString(𝔽(actualStart + k)).
        auto from = PropertyKey { actual_start + k };

        // b. If ? HasProperty(O, from) is true, then
        if (TRY(this_object->has_property(from))) {
            // i. Let fromValue be ? Get(O, from).
            auto from_value = TRY(this_object->get(from));

            // ii. Perform ? CreateDataPropertyOrThrow(A, ! ToString(𝔽(k)), fromValue).
            TRY(removed_elements->create_data_property_or_throw(k, from_value));
        }

        // c. Set k to k + 1.
    }

    // 15. Perform ? Set(A, "length", 𝔽(actualDeleteCount), true).
    TRY(removed_elements->set(vm.names.length, Value(actual_delete_count), Object::ShouldThrowExceptions::Yes));

    // 16. If itemCount < actualDeleteCount, then
    if (item_count < actual_delete_count) {
        // a. Set k to actualStart.
        // b. Repeat, while k < (len - actualDeleteCount),
        for (u64 k = actual_start; k < initial_length - actual_delete_count; ++k) {
            // i. Let from be ! ToString(𝔽(k + actualDeleteCount)).
            auto from = PropertyKey { k + actual_delete_count };

            // ii. Let to be ! ToString(𝔽(k + itemCount)).
            auto to = PropertyKey { k + item_count };

            // iii. If ? HasProperty(O, from) is true, then
            if (TRY(this_object->has_property(from))) {
                // 1. Let fromValue be ? Get(O, from).
                auto from_value = TRY(this_object->get(from));

                // 2. Perform ? Set(O, to, fromValue, true).
                TRY(this_object->set(to, from_value, Object::ShouldThrowExceptions::Yes));
            }
            // iv. Else,
            else {
                // 1. Perform ? DeletePropertyOrThrow(O, to).
                TRY(this_object->delete_property_or_throw(to));
            }

            // v. Set k to k + 1.
        }

        // c. Set k to len.
        // d. Repeat, while k > (len - actualDeleteCount + itemCount),
        for (u64 k = initial_length; k > initial_length - actual_delete_count + item_count; --k) {
            // i. Perform ? DeletePropertyOrThrow(O, ! ToString(𝔽(k - 1))).
            TRY(this_object->delete_property_or_throw(k - 1));

            // ii. Set k to k - 1.
        }
    }
    // 17. Else if itemCount > actualDeleteCount, then
    else if (item_count > actual_delete_count) {
        // a. Set k to (len - actualDeleteCount).
        // b. Repeat, while k > actualStart,
        for (u64 k = initial_length - actual_delete_count; k > actual_start; --k) {
            // i. Let from be ! ToString(𝔽(k + actualDeleteCount - 1)).
            auto from = PropertyKey { k + actual_delete_count - 1 };

            // ii. Let to be ! ToString(𝔽(k + itemCount - 1)).
            auto to = PropertyKey { k + item_count - 1 };

            // iii. If ? HasProperty(O, from) is true, then
            if (TRY(this_object->has_property(from))) {
                // 1. Let fromValue be ? Get(O, from).
                auto from_value = TRY(this_object->get(from));

                // 2. Perform ? Set(O, to, fromValue, true).
                TRY(this_object->set(to, from_value, Object::ShouldThrowExceptions::Yes));
            }
            // iv. Else,
            else {
                // 1. Perform ? DeletePropertyOrThrow(O, to).
                TRY(this_object->delete_property_or_throw(to));
            }

            // v. Set k to k - 1.
        }
    }

    // 18. Set k to actualStart.
    auto k = actual_start;

    // 19. For each element E of items, do
    for (size_t element_index = 2; element_index < vm.argument_count(); ++element_index) {
        auto element = vm.argument(element_index);

        // a. Perform ? Set(O, ! ToString(𝔽(k)), E, true).
        TRY(this_object->set(k, element, Object::ShouldThrowExceptions::Yes));

        // b. Set k to k + 1.
        ++k;
    }

    // 20. Perform ? Set(O, "length", 𝔽(len - actualDeleteCount + itemCount), true).
    TRY(this_object->set(vm.names.length, Value(initial_length - actual_delete_count + item_count), Object::ShouldThrowExceptions::Yes));

    // 21. Return A.
    return removed_elements;
}

// 23.1.3.32 Array.prototype.toLocaleString ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-array.prototype.tolocalestring
// 20.5.1 Array.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-array.prototype.tolocalestring
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::to_locale_string)
{
    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let array be ? ToObject(this value).
    auto this_object = TRY(vm.this_value().to_object(vm));

    if (s_array_join_seen_objects.contains(this_object))
        return PrimitiveString::create(vm, String {});
    s_array_join_seen_objects.set(this_object);
    ArmedScopeGuard unsee_object_guard = [&] {
        s_array_join_seen_objects.remove(this_object);
    };

    // 2. Let len be ? ToLength(? Get(array, "length")).
    auto length = TRY(length_of_array_like(vm, this_object));

    // 3. Let separator be the implementation-defined list-separator String value appropriate for the host environment's current locale (such as ", ").
    constexpr auto separator = ","sv;

    // 4. Let R be the empty String.
    StringBuilder builder;

    // 5. Let k be 0.
    // 6. Repeat, while k < len,
    for (size_t i = 0; i < length; ++i) {
        // a. If k > 0, then
        if (i > 0) {
            // i. Set R to the string-concatenation of R and separator.
            builder.append(separator);
        }

        // b. Let nextElement be ? Get(array, ! ToString(k)).
        auto value = TRY(this_object->get(i));

        // c. If nextElement is not undefined or null, then
        if (!value.is_nullish()) {
            // i. Let S be ? ToString(? Invoke(nextElement, "toLocaleString", « locales, options »)).
            auto locale_string_result = TRY(value.invoke(vm, vm.names.toLocaleString, locales, options));

            // ii. Set R to the string-concatenation of R and S.
            auto string = TRY(locale_string_result.to_string(vm));
            builder.append(string);
        }

        // d. Increase k by 1.
    }

    // 7. Return R.
    return PrimitiveString::create(vm, builder.to_string_without_validation());
}

// 23.1.3.33 Array.prototype.toReversed ( ), https://tc39.es/ecma262/#sec-array.prototype.toreversed
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::to_reversed)
{
    auto& realm = *vm.current_realm();

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. Let A be ? ArrayCreate(𝔽(len)).
    auto array = TRY(Array::create(realm, length));

    // 4. Let k be 0.
    // 5. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let from be ! ToString(𝔽(len - k - 1)).
        auto from = PropertyKey { length - k - 1 };

        // b. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // c. Let fromValue be ? Get(O, from).
        auto from_value = TRY(object->get(from));

        // d. Perform ! CreateDataPropertyOrThrow(A, Pk, fromValue).
        MUST(array->create_data_property_or_throw(property_key, from_value));

        // e. Set k to k + 1.
    }

    // 6. Return A.
    return array;
}

// 23.1.3.34 Array.prototype.toSorted ( comparefn ), https://tc39.es/ecma262/#sec-array.prototype.tosorted
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::to_sorted)
{
    auto& realm = *vm.current_realm();

    auto comparefn = vm.argument(0);

    // 1. If comparefn is not undefined and IsCallable(comparefn) is false, throw a TypeError exception.
    if (!comparefn.is_undefined() && !comparefn.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, comparefn);

    // 2. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 3. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 4. Let A be ? ArrayCreate(𝔽(len)).
    auto array = TRY(Array::create(realm, length));

    // 5. Let SortCompare be a new Abstract Closure with parameters (x, y) that captures comparefn and performs the following steps when called:
    Function<ThrowCompletionOr<double>(Value, Value)> sort_compare = [&](auto x, auto y) -> ThrowCompletionOr<double> {
        // a. Return ? CompareArrayElements(x, y, comparefn).
        return TRY(compare_array_elements(vm, x, y, comparefn.is_undefined() ? nullptr : &comparefn.as_function()));
    };

    // 6. Let sortedList be ? SortIndexedProperties(obj, len, SortCompare, read-through-holes).
    auto sorted_list = TRY(sort_indexed_properties(vm, object, length, sort_compare, Holes::ReadThroughHoles));

    // 7. Let j be 0.
    // 8. Repeat, while j < len,
    for (size_t j = 0; j < length; ++j) {
        // a. Perform ! CreateDataPropertyOrThrow(A, ! ToString(𝔽(j)), sortedList[j]).
        MUST(array->create_data_property_or_throw(j, sorted_list[j]));

        // b. Set j to j + 1.
    }

    // 9. Return A.
    return array;
}

// 23.1.3.35 Array.prototype.toSpliced ( start, skipCount, ...items ), https://tc39.es/ecma262/#sec-array.prototype.tospliced
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::to_spliced)
{
    auto& realm = *vm.current_realm();

    auto start = vm.argument(0);
    auto skip_count = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. Let relativeStart be ? ToIntegerOrInfinity(start).
    auto relative_start = TRY(start.to_integer_or_infinity(vm));

    size_t actual_start;

    // 4. If relativeStart is -∞, let actualStart be 0.
    if (Value(relative_start).is_negative_infinity())
        actual_start = 0;
    // 5. Else if relativeStart < 0, let actualStart be max(len + relativeStart, 0).
    else if (relative_start < 0)
        actual_start = static_cast<size_t>(max(static_cast<double>(length) + relative_start, 0));
    // 6. Else, let actualStart be min(relativeStart, len).
    else
        actual_start = static_cast<size_t>(min(relative_start, static_cast<double>(length)));

    // Sanity check
    VERIFY(actual_start <= length);

    // 7. Let insertCount be the number of elements in items.
    auto insert_count = vm.argument_count() >= 2 ? vm.argument_count() - 2 : 0;

    size_t actual_skip_count;

    // 8. If start is not present, then
    if (vm.argument_count() == 0) {
        // a. Let actualSkipCount be 0.
        actual_skip_count = 0;
    }
    // 9. Else if deleteCount is not present, then
    else if (vm.argument_count() == 1) {
        // a. Let actualSkipCount be len - actualStart.
        actual_skip_count = length - actual_start;
    }
    // 10. Else,
    else {
        // a. Let sc be ? ToIntegerOrInfinity(skipCount).
        auto sc = TRY(skip_count.to_integer_or_infinity(vm));

        // b. Let actualSkipCount be the result of clamping sc between 0 and len - actualStart.
        actual_skip_count = static_cast<size_t>(clamp(sc, 0, static_cast<double>(length - actual_start)));
    }

    // Sanity check
    VERIFY(actual_skip_count <= (length - actual_start));

    // 11. Let newLen be len + insertCount - actualSkipCount.
    auto new_length_double = static_cast<double>(length) + static_cast<double>(insert_count) - static_cast<double>(actual_skip_count);

    // 12. If newLen > 2^53 - 1, throw a TypeError exception.
    if (new_length_double > MAX_ARRAY_LIKE_INDEX)
        return vm.throw_completion<TypeError>(ErrorType::ArrayMaxSize);

    auto new_length = static_cast<u64>(new_length_double);

    // 13. Let A be ? ArrayCreate(𝔽(newLen)).
    auto array = TRY(Array::create(realm, new_length));

    // 14. Let i be 0.
    size_t i = 0;

    // 15. Let r be actualStart + actualSkipCount.
    auto r = actual_start + actual_skip_count;

    // 16. Repeat, while i < actualStart,
    while (i < actual_start) {
        // a. Let Pi be ! ToString(𝔽(i)).
        auto property_key = PropertyKey { i };

        // b. Let iValue be ? Get(O, Pi).
        auto i_value = TRY(object->get(property_key));

        // c. Perform ! CreateDataPropertyOrThrow(A, Pi, iValue).
        MUST(array->create_data_property_or_throw(property_key, i_value));

        // d. Set i to i + 1.
        ++i;
    }

    // 17. For each element E of items, do
    for (size_t element_index = 2; element_index < vm.argument_count(); ++element_index) {
        auto element = vm.argument(element_index);

        // a. Let Pi be ! ToString(𝔽(i)).
        auto property_key = PropertyKey { i };

        // b. Perform ! CreateDataPropertyOrThrow(A, Pi, E).
        MUST(array->create_data_property_or_throw(property_key, element));

        // c. Set i to i + 1.
        ++i;
    }

    // 18. Repeat, while i < newLen,
    while (i < new_length) {
        // a. Let Pi be ! ToString(𝔽(i)).
        auto property_key = PropertyKey { i };

        // b. Let from be ! ToString(𝔽(r)).
        auto from = PropertyKey { r };

        // c. Let fromValue be ? Get(O, from).
        auto from_value = TRY(object->get(from));

        // d. Perform ! CreateDataPropertyOrThrow(A, Pi, fromValue).
        MUST(array->create_data_property_or_throw(property_key, from_value));

        // e. Set i to i + 1.
        ++i;

        // f. Set r to r + 1.
        ++r;
    }

    // 19. Return A.
    return array;
}

// 23.1.3.36 Array.prototype.toString ( ), https://tc39.es/ecma262/#sec-array.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::to_string)
{
    auto& realm = *vm.current_realm();

    // 1. Let array be ? ToObject(this value).
    auto array = TRY(vm.this_value().to_object(vm));

    // 2. Let func be ? Get(array, "join").
    auto func = TRY(array->get(vm.names.join));

    // 3. If IsCallable(func) is false, set func to the intrinsic function %Object.prototype.toString%.
    if (!func.is_function())
        func = realm.intrinsics().object_prototype_to_string_function();

    // 4. Return ? Call(func, array).
    return TRY(call(vm, func.as_function(), array));
}

// 23.1.3.37 Array.prototype.unshift ( ...items ), https://tc39.es/ecma262/#sec-array.prototype.unshift
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::unshift)
{
    auto this_object = TRY(vm.this_value().to_object(vm));
    auto length = TRY(length_of_array_like(vm, this_object));
    auto arg_count = vm.argument_count();
    size_t new_length = length + arg_count;
    if (arg_count > 0) {
        if (new_length > MAX_ARRAY_LIKE_INDEX)
            return vm.throw_completion<TypeError>(ErrorType::ArrayMaxSize);

        for (size_t k = length; k > 0; --k) {
            auto from = k - 1;
            auto to = k + arg_count - 1;

            bool from_present = TRY(this_object->has_property(from));
            if (from_present) {
                auto from_value = TRY(this_object->get(from));
                TRY(this_object->set(to, from_value, Object::ShouldThrowExceptions::Yes));
            } else {
                TRY(this_object->delete_property_or_throw(to));
            }
        }

        for (size_t j = 0; j < arg_count; j++)
            TRY(this_object->set(j, vm.argument(j), Object::ShouldThrowExceptions::Yes));
    }

    TRY(this_object->set(vm.names.length, Value(new_length), Object::ShouldThrowExceptions::Yes));
    return Value(new_length);
}

// 23.1.3.38 Array.prototype.values ( ), https://tc39.es/ecma262/#sec-array.prototype.values
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::values)
{
    auto& realm = *vm.current_realm();

    auto this_object = TRY(vm.this_value().to_object(vm));

    return ArrayIterator::create(realm, this_object, Object::PropertyKind::Value);
}

// 23.1.3.39 Array.prototype.with ( index, value ), https://tc39.es/ecma262/#sec-array.prototype.with
JS_DEFINE_NATIVE_FUNCTION(ArrayPrototype::with)
{
    auto& realm = *vm.current_realm();

    auto index = vm.argument(0);
    auto value = vm.argument(1);

    // 1. Let O be ? ToObject(this value).
    auto object = TRY(vm.this_value().to_object(vm));

    // 2. Let len be ? LengthOfArrayLike(O).
    auto length = TRY(length_of_array_like(vm, object));

    // 3. Let relativeIndex be ? ToIntegerOrInfinity(index).
    auto relative_index = TRY(index.to_integer_or_infinity(vm));

    double actual_index;

    // 4. If relativeIndex ≥ 0, let actualIndex be relativeIndex.
    if (relative_index >= 0)
        actual_index = relative_index;
    // 5. Else, let actualIndex be len + relativeIndex.
    else
        actual_index = static_cast<double>(length) + relative_index;

    // 6. If actualIndex ≥ len or actualIndex < 0, throw a RangeError exception.
    if (actual_index >= static_cast<double>(length) || actual_index < 0)
        return vm.throw_completion<RangeError>(ErrorType::IndexOutOfRange, actual_index, length);

    // 7. Let A be ? ArrayCreate(𝔽(len)).
    auto array = TRY(Array::create(realm, length));

    // 8. Let k be 0.
    // 9. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        Value from_value;

        // b. If k is actualIndex, let fromValue be value.
        if (k == static_cast<size_t>(actual_index))
            from_value = value;
        // c. Else, let fromValue be ? Get(O, Pk).
        else
            from_value = TRY(object->get(property_key));

        // d. Perform ! CreateDataPropertyOrThrow(A, Pk, fromValue).
        MUST(array->create_data_property_or_throw(property_key, from_value));

        // e. Set k to k + 1.
    }

    // 10. Return A.
    return array;
}

}
