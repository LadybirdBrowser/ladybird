/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2021-2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayIterator.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/TypedArrayPrototype.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

GC_DEFINE_ALLOCATOR(TypedArrayPrototype);

TypedArrayPrototype::TypedArrayPrototype(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{
}

void TypedArrayPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;

    define_native_accessor(realm, vm.names.buffer, buffer_getter, nullptr, Attribute::Configurable);
    define_native_accessor(realm, vm.names.byteLength, byte_length_getter, nullptr, Attribute::Configurable);
    define_native_accessor(realm, vm.names.byteOffset, byte_offset_getter, nullptr, Attribute::Configurable);
    define_native_accessor(realm, vm.names.length, length_getter, nullptr, Attribute::Configurable);

    define_native_function(realm, vm.names.at, at, 1, attr);
    define_native_function(realm, vm.names.copyWithin, copy_within, 2, attr);
    define_native_function(realm, vm.names.entries, entries, 0, attr);
    define_native_function(realm, vm.names.every, every, 1, attr);
    define_native_function(realm, vm.names.fill, fill, 1, attr);
    define_native_function(realm, vm.names.filter, filter, 1, attr);
    define_native_function(realm, vm.names.find, find, 1, attr);
    define_native_function(realm, vm.names.findIndex, find_index, 1, attr);
    define_native_function(realm, vm.names.findLast, find_last, 1, attr);
    define_native_function(realm, vm.names.findLastIndex, find_last_index, 1, attr);
    define_native_function(realm, vm.names.forEach, for_each, 1, attr);
    define_native_function(realm, vm.names.includes, includes, 1, attr);
    define_native_function(realm, vm.names.indexOf, index_of, 1, attr);
    define_native_function(realm, vm.names.join, join, 1, attr);
    define_native_function(realm, vm.names.keys, keys, 0, attr);
    define_native_function(realm, vm.names.lastIndexOf, last_index_of, 1, attr);
    define_native_function(realm, vm.names.map, map, 1, attr);
    define_native_function(realm, vm.names.reduce, reduce, 1, attr);
    define_native_function(realm, vm.names.reduceRight, reduce_right, 1, attr);
    define_native_function(realm, vm.names.reverse, reverse, 0, attr);
    define_native_function(realm, vm.names.set, set, 1, attr);
    define_native_function(realm, vm.names.slice, slice, 2, attr);
    define_native_function(realm, vm.names.some, some, 1, attr);
    define_native_function(realm, vm.names.sort, sort, 1, attr);
    define_native_function(realm, vm.names.subarray, subarray, 2, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toReversed, to_reversed, 0, attr);
    define_native_function(realm, vm.names.toSorted, to_sorted, 1, attr);
    define_native_function(realm, vm.names.with, with, 2, attr);
    define_native_function(realm, vm.names.values, values, 0, attr);

    define_native_accessor(realm, vm.well_known_symbol_to_string_tag(), to_string_tag_getter, nullptr, Attribute::Configurable);

    // 23.2.3.34 %TypedArray%.prototype.toString ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.tostring
    define_direct_property(vm.names.toString, realm.intrinsics().array_prototype()->get_without_side_effects(vm.names.toString), attr);

    // 23.2.3.37 %TypedArray%.prototype [ @@iterator ] ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype-@@iterator
    define_direct_property(vm.well_known_symbol_iterator(), get_without_side_effects(vm.names.values), attr);
}

static ThrowCompletionOr<TypedArrayBase*> typed_array_from_this(VM& vm)
{
    auto this_value = vm.this_value();
    return typed_array_from(vm, this_value);
}

static ThrowCompletionOr<GC::Ref<FunctionObject>> callback_from_args(VM& vm, StringView prototype_name)
{
    if (vm.argument_count() < 1)
        return vm.throw_completion<TypeError>(ErrorType::TypedArrayPrototypeOneArg, prototype_name);
    auto callback = vm.argument(0);
    if (!callback.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, callback);
    return callback.as_function();
}

// 23.2.4.1 TypedArraySpeciesCreate ( exemplar, argumentList ), https://tc39.es/ecma262/#typedarray-species-create
static ThrowCompletionOr<TypedArrayBase*> typed_array_species_create(VM& vm, TypedArrayBase const& exemplar, GC::RootVector<Value> arguments)
{
    auto& realm = *vm.current_realm();

    // 1. Let defaultConstructor be the intrinsic object listed in column one of Table 72 for exemplar.[[TypedArrayName]].
    auto default_constructor = (realm.intrinsics().*exemplar.intrinsic_constructor())();

    // 2. Let constructor be ? SpeciesConstructor(exemplar, defaultConstructor).
    auto* constructor = TRY(species_constructor(vm, exemplar, *default_constructor));

    // 3. Let result be ? TypedArrayCreate(constructor, argumentList).
    auto* result = TRY(typed_array_create(vm, *constructor, move(arguments)));

    // 4. Assert: result has [[TypedArrayName]] and [[ContentType]] internal slots.
    // 5. If result.[[ContentType]] ≠ exemplar.[[ContentType]], throw a TypeError exception.
    if (result->content_type() != exemplar.content_type())
        return vm.throw_completion<TypeError>(ErrorType::TypedArrayContentTypeMismatch, result->class_name(), exemplar.class_name());

    // 6. Return result.
    return result;
}

// 23.2.3.1 %TypedArray%.prototype.at ( index ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.at
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::at)
{
    auto index = vm.argument(0);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. Let relativeIndex be ? ToIntegerOrInfinity(index).
    auto relative_index = TRY(index.to_integer_or_infinity(vm));

    if (Value { relative_index }.is_infinity())
        return js_undefined();

    Checked<size_t> k_checked { 0 };

    // 5. If relativeIndex ≥ 0, then
    if (relative_index >= 0) {
        // a. Let k be relativeIndex.
        k_checked += relative_index;
    }
    // 6. Else,
    else {
        // a. Let k be len + relativeIndex.
        k_checked += length;
        k_checked -= -relative_index;
    }

    // 7. If k < 0 or k ≥ len, return undefined.
    if (k_checked.has_overflow() || k_checked.value() >= length)
        return js_undefined();

    // 8. Return ! Get(O, ! ToString(𝔽(k))).
    return MUST(typed_array->get(k_checked.value()));
}

// 23.2.3.2 get %TypedArray%.prototype.buffer, https://tc39.es/ecma262/#sec-get-%typedarray%.prototype.buffer
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::buffer_getter)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[TypedArrayName]]).
    // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 4. Let buffer be O.[[ViewedArrayBuffer]].
    auto* buffer = typed_array->viewed_array_buffer();
    VERIFY(buffer);

    // 5. Return buffer.
    return Value(buffer);
}

// 23.2.3.3 get %TypedArray%.prototype.byteLength, https://tc39.es/ecma262/#sec-get-%typedarray%.prototype.bytelength
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::byte_length_getter)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[TypedArrayName]]).
    // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 4. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
    auto typed_array_record = make_typed_array_with_buffer_witness_record(*typed_array, ArrayBuffer::Order::SeqCst);

    // 5. Let size be TypedArrayByteLength(taRecord).
    auto size = typed_array_byte_length(typed_array_record);

    // 6. Return 𝔽(size).
    return Value { size };
}

// 23.2.3.4 get %TypedArray%.prototype.byteOffset, https://tc39.es/ecma262/#sec-get-%typedarray%.prototype.byteoffset
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::byte_offset_getter)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[TypedArrayName]]).
    // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 4. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
    auto typed_array_record = make_typed_array_with_buffer_witness_record(*typed_array, ArrayBuffer::Order::SeqCst);

    // 5. If IsTypedArrayOutOfBounds(taRecord) is true, return +0𝔽.
    if (is_typed_array_out_of_bounds(typed_array_record))
        return Value { 0 };

    // 6. Let offset be O.[[ByteOffset]].
    auto offset = typed_array->byte_offset();

    // 7. Return 𝔽(offset).
    return Value { offset };
}

// 23.2.3.6 %TypedArray%.prototype.copyWithin ( target, start [ , end ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.copywithin
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::copy_within)
{
    auto target = vm.argument(0);
    auto start = vm.argument(1);
    auto end = vm.argument(2);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. Let relativeTarget be ? ToIntegerOrInfinity(target).
    auto relative_target = TRY(target.to_integer_or_infinity(vm));

    double to;
    // 5. If relativeTarget = -∞, let to be 0.
    if (Value { relative_target }.is_negative_infinity()) {
        to = 0.0;
    }
    // 6. Else if relativeTarget < 0, let to be max(len + relativeTarget, 0).
    else if (relative_target < 0) {
        to = max(length + relative_target, 0.0);
    }
    // 7. Else, let to be min(relativeTarget, len).
    else {
        to = min(relative_target, (double)length);
    }

    // 8. Let relativeStart be ? ToIntegerOrInfinity(start).
    auto relative_start = TRY(start.to_integer_or_infinity(vm));

    double from;
    // 9. If relativeStart = -∞, let from be 0.
    if (Value { relative_start }.is_negative_infinity()) {
        from = 0.0;
    }
    // 10. Else if relativeStart < 0, let from be max(len + relativeStart, 0).
    else if (relative_start < 0) {
        from = max(length + relative_start, 0.0);
    }
    // 11. Else, let from be min(relativeStart, len).
    else {
        from = min(relative_start, (double)length);
    }

    double relative_end;
    // 12. If end is undefined, let relativeEnd be len; else let relativeEnd be ? ToIntegerOrInfinity(end).
    if (end.is_undefined())
        relative_end = length;
    else
        relative_end = TRY(end.to_integer_or_infinity(vm));

    double final;
    // 13. If relativeEnd = -∞, let final be 0.
    if (Value { relative_end }.is_negative_infinity()) {
        final = 0.0;
    }
    // 14. Else if relativeEnd < 0, let final be max(len + relativeEnd, 0).
    else if (relative_end < 0) {
        final = max(length + relative_end, 0.0);
    }
    // 15. Else, let final be min(relativeEnd, len).
    else {
        final = min(relative_end, (double)length);
    }

    // 16. Let count be min(final - from, len - to).
    double count = min(final - from, length - to);

    // 17. If count > 0, then
    if (count > 0.0) {
        // a. NOTE: The copying must be performed in a manner that preserves the bit-level encoding of the source data.

        // b. Let buffer be O.[[ViewedArrayBuffer]].
        auto* buffer = typed_array->viewed_array_buffer();

        // c. Set taRecord to MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
        typed_array_record = make_typed_array_with_buffer_witness_record(*typed_array, ArrayBuffer::Order::SeqCst);

        // d. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
        if (is_typed_array_out_of_bounds(typed_array_record))
            return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

        // e. Set len to TypedArrayLength(taRecord).
        length = typed_array_length(typed_array_record);

        // f. Let elementSize be TypedArrayElementSize(O).
        auto element_size = typed_array->element_size();

        // g. Let byteOffset be O.[[ByteOffset]].
        auto byte_offset = typed_array->byte_offset();

        // FIXME: Not exactly sure what we should do when overflow occurs. Just return as if succeeded for now.

        // h. Let bufferByteLimit be len × elementSize + byteOffset.
        Checked<size_t> buffer_byte_limit_checked = static_cast<size_t>(length);
        buffer_byte_limit_checked *= element_size;
        buffer_byte_limit_checked += byte_offset;
        if (buffer_byte_limit_checked.has_overflow()) {
            dbgln("TypedArrayPrototype::copy_within: buffer_byte_limit overflowed, returning as if succeeded.");
            return typed_array;
        }

        // i. Let toByteIndex be to × elementSize + byteOffset.
        Checked<size_t> to_byte_index_checked = static_cast<size_t>(to);
        to_byte_index_checked *= element_size;
        to_byte_index_checked += byte_offset;
        if (to_byte_index_checked.has_overflow()) {
            dbgln("TypedArrayPrototype::copy_within: to_byte_index overflowed, returning as if succeeded.");
            return typed_array;
        }

        // j. Let fromByteIndex be from × elementSize + byteOffset.
        Checked<size_t> from_byte_index_checked = static_cast<size_t>(from);
        from_byte_index_checked *= element_size;
        from_byte_index_checked += byte_offset;
        if (from_byte_index_checked.has_overflow()) {
            dbgln("TypedArrayPrototype::copy_within: from_byte_index overflowed, returning as if succeeded.");
            return typed_array;
        }

        // k. Let countBytes be count × elementSize.
        Checked<size_t> count_bytes_checked = static_cast<size_t>(count);
        count_bytes_checked *= element_size;
        if (count_bytes_checked.has_overflow()) {
            dbgln("TypedArrayPrototype::copy_within: count_bytes overflowed, returning as if succeeded.");
            return typed_array;
        }

        auto buffer_byte_limit = buffer_byte_limit_checked.value();
        auto to_byte_index = to_byte_index_checked.value();
        auto from_byte_index = from_byte_index_checked.value();
        auto count_bytes = count_bytes_checked.value();

        Checked<size_t> from_plus_count = from_byte_index;
        from_plus_count += count_bytes;
        if (from_plus_count.has_overflow()) {
            dbgln("TypedArrayPrototype::copy_within: from_plus_count overflowed, returning as if succeeded.");
            return typed_array;
        }

        i8 direction;

        // l. If fromByteIndex < toByteIndex and toByteIndex < fromByteIndex + countBytes, then
        if (from_byte_index < to_byte_index && to_byte_index < from_plus_count.value()) {
            // i. Let direction be -1.
            direction = -1;
            // ii. Set fromByteIndex to fromByteIndex + countBytes - 1.
            from_byte_index = from_plus_count.value() - 1;

            Checked<size_t> to_plus_count = to_byte_index;
            to_plus_count += count_bytes;
            if (to_plus_count.has_overflow()) {
                dbgln("TypedArrayPrototype::copy_within: to_plus_count overflowed, returning as if succeeded.");
                return typed_array;
            }

            // iii. Set toByteIndex to toByteIndex + countBytes - 1.
            to_byte_index = to_plus_count.value() - 1;
        }
        // m. Else,
        else {
            // i. Let direction be 1.
            direction = 1;
        }

        // n. Repeat, while countBytes > 0,
        while (count_bytes > 0) {
            // i. If fromByteIndex < bufferByteLimit and toByteIndex < bufferByteLimit, then
            if (from_byte_index < buffer_byte_limit && to_byte_index < buffer_byte_limit) {
                // 1. Let value be GetValueFromBuffer(buffer, fromByteIndex, uint8, true, unordered).
                auto value = buffer->get_value<u8>(from_byte_index, true, ArrayBuffer::Order::Unordered);

                // 2. Perform SetValueInBuffer(buffer, toByteIndex, uint8, value, true, unordered).
                buffer->set_value<u8>(to_byte_index, value, true, ArrayBuffer::Order::Unordered);

                // 3. Set fromByteIndex to fromByteIndex + direction.
                from_byte_index += direction;

                // 4. Set toByteIndex to toByteIndex + direction.
                to_byte_index += direction;

                // 5. Set countBytes to countBytes - 1.
                --count_bytes;
            }
            // ii. Else,
            else {
                // 1. Set countBytes to 0.
                count_bytes = 0;
            }
        }
    }

    // 18. Return O.
    return typed_array;
}

// 23.2.3.7 %TypedArray%.prototype.entries ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.entries
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::entries)
{
    auto& realm = *vm.current_realm();

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Perform ? ValidateTypedArray(O, seq-cst).
    (void)TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Return CreateArrayIterator(O, key+value).
    return ArrayIterator::create(realm, typed_array, Object::PropertyKind::KeyAndValue);
}

// 23.2.3.8 %TypedArray%.prototype.every ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.every
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::every)
{
    auto this_arg = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
    auto callback_function = TRY(callback_from_args(vm, "every"sv));

    // 5. Let k be 0.
    // 6. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        // b. Let kValue be ! Get(O, Pk).
        auto value = MUST(typed_array->get(property_key));

        // c. Let testResult be ToBoolean(? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »)).
        auto test_result = TRY(call(vm, *callback_function, this_arg, value, Value { k }, typed_array)).to_boolean();

        // d. If testResult is false, return false.
        if (!test_result)
            return false;

        // e. Set k to k + 1.
    }

    // 7. Return true.
    return true;
}

// NOTE: This function assumes that the index is valid within the TypedArray,
//       and that the TypedArray is not detached.
template<typename T>
inline void fast_typed_array_fill(TypedArrayBase& typed_array, u32 begin, u32 end, T value)
{
    Checked<size_t> computed_begin = begin;
    computed_begin *= sizeof(T);
    computed_begin += typed_array.byte_offset();

    Checked<size_t> computed_end = end;
    computed_end *= sizeof(T);
    computed_end += typed_array.byte_offset();

    if (computed_begin.has_overflow() || computed_end.has_overflow()) [[unlikely]] {
        return;
    }

    if (computed_begin.value() >= typed_array.viewed_array_buffer()->byte_length()
        || computed_end.value() > typed_array.viewed_array_buffer()->byte_length()) [[unlikely]] {
        return;
    }

    auto& array_buffer = *typed_array.viewed_array_buffer();
    auto* slot = reinterpret_cast<T*>(array_buffer.buffer().offset_pointer(computed_begin.value()));
    for (auto i = begin; i < end; ++i)
        *(slot++) = value;
}

// 23.2.3.9 %TypedArray%.prototype.fill ( value [ , start [ , end ] ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.fill
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::fill)
{
    auto value = vm.argument(0);
    auto start = vm.argument(1);
    auto end = vm.argument(2);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. If O.[[ContentType]] is BigInt, set value to ? ToBigInt(value).
    if (typed_array->content_type() == TypedArrayBase::ContentType::BigInt)
        value = TRY(value.to_bigint(vm));
    // 5. Otherwise, set value to ? ToNumber(value).
    else
        value = TRY(value.to_number(vm));

    // 6. Let relativeStart be ? ToIntegerOrInfinity(start).
    auto relative_start = TRY(start.to_integer_or_infinity(vm));

    u32 k;
    // 7. If relativeStart = -∞, let k be 0.
    if (Value { relative_start }.is_negative_infinity())
        k = 0;
    // 8. Else if relativeStart < 0, let k be max(len + relativeStart, 0).
    else if (relative_start < 0)
        k = max(length + relative_start, 0);
    // 9. Else, let k be min(relativeStart, len).
    else
        k = min(relative_start, length);

    double relative_end;
    // 10. If end is undefined, let relativeEnd be len; else let relativeEnd be ? ToIntegerOrInfinity(end).
    if (end.is_undefined())
        relative_end = length;
    else
        relative_end = TRY(end.to_integer_or_infinity(vm));

    u32 final;
    // 11. If relativeEnd = -∞, let final be 0.
    if (Value { relative_end }.is_negative_infinity())
        final = 0;
    // 12. Else if relativeEnd < 0, let final be max(len + relativeEnd, 0).
    else if (relative_end < 0)
        final = max(length + relative_end, 0);
    // 13. Else, let final be min(relativeEnd, len).
    else
        final = min(relative_end, length);

    // 14. Set taRecord to MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
    typed_array_record = make_typed_array_with_buffer_witness_record(*typed_array, ArrayBuffer::Order::SeqCst);

    // 15. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
    if (is_typed_array_out_of_bounds(typed_array_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

    // 16. Set len to TypedArrayLength(taRecord).
    length = typed_array_length(typed_array_record);

    // 17. Set final to min(final, len).
    final = min(final, length);

    if (value.is_int32()) {
        switch (typed_array->kind()) {
        case TypedArrayBase::Kind::Uint8Array:
            fast_typed_array_fill<u8>(*typed_array, k, final, static_cast<u8>(value.as_i32()));
            return typed_array;
        case TypedArrayBase::Kind::Uint16Array:
            fast_typed_array_fill<u16>(*typed_array, k, final, static_cast<u16>(value.as_i32()));
            return typed_array;
        case TypedArrayBase::Kind::Uint32Array:
            fast_typed_array_fill<u32>(*typed_array, k, final, static_cast<u32>(value.as_i32()));
            return typed_array;
        case TypedArrayBase::Kind::Int8Array:
            fast_typed_array_fill<i8>(*typed_array, k, final, static_cast<i8>(value.as_i32()));
            return typed_array;
        case TypedArrayBase::Kind::Int16Array:
            fast_typed_array_fill<i16>(*typed_array, k, final, static_cast<i16>(value.as_i32()));
            return typed_array;
        case TypedArrayBase::Kind::Int32Array:
            fast_typed_array_fill<i32>(*typed_array, k, final, value.as_i32());
            return typed_array;
        case TypedArrayBase::Kind::Uint8ClampedArray:
            fast_typed_array_fill<u8>(*typed_array, k, final, clamp(value.as_i32(), 0, 255));
            return typed_array;
        default:
            // FIXME: Support more TypedArray kinds.
            break;
        }
    }

    // 18. Repeat, while k < final,
    while (k < final) {
        // a. Let Pk be ! ToString(𝔽(k)).
        // b. Perform ! Set(O, Pk, value, true).
        CanonicalIndex canonical_index { CanonicalIndex::Type::Index, k };
        switch (typed_array->kind()) {
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    case TypedArrayBase::Kind::ClassName:                                           \
        (void)typed_array_set_element<Type>(*typed_array, canonical_index, value);  \
        break;
            JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
        }

        // c. Set k to k + 1.
        ++k;
    }

    // 19. Return O.
    return typed_array;
}

// 23.2.3.10 %TypedArray%.prototype.filter ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.filter
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::filter)
{
    auto this_arg = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
    auto callback_function = TRY(callback_from_args(vm, "filter"sv));

    // 5. Let kept be a new empty List.
    GC::RootVector<Value> kept { vm.heap() };

    // 6. Let captured be 0.
    size_t captured = 0;

    // 7. Let k be 0.
    // 8. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        // b. Let kValue be ! Get(O, Pk).
        auto value = MUST(typed_array->get(property_key));

        // c. Let selected be ToBoolean(? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »)).
        auto selected = TRY(call(vm, *callback_function, this_arg, value, Value { k }, typed_array)).to_boolean();

        // d. If selected is true, then
        if (selected) {
            // i. Append kValue to kept.
            kept.append(value);

            // ii. Set captured to captured + 1.
            ++captured;
        }

        // e. Set k to k + 1.
    }

    // 9. Let A be ? TypedArraySpeciesCreate(O, « 𝔽(captured) »).
    GC::RootVector<Value> arguments(vm.heap());
    arguments.empend(captured);
    auto* filter_array = TRY(typed_array_species_create(vm, *typed_array, move(arguments)));

    // 10. Let n be 0.
    size_t index = 0;

    // 11. For each element e of kept, do
    for (auto& value : kept) {
        // a. Perform ! Set(A, ! ToString(𝔽(n)), e, true).
        MUST(filter_array->set(index, value, Object::ShouldThrowExceptions::Yes));

        // b. Set n to n + 1.
        ++index;
    }

    // 12. Return A.
    return filter_array;
}

enum class Direction {
    Ascending,
    Descending,
};

struct FoundValue {
    Value index_to_value() const
    {
        if (!index.has_value())
            return Value { -1 };
        return Value { *index };
    }

    Optional<u32> index; // [[Index]]
    Value value;         // [[Value]]
};

// 23.1.3.12.1 FindViaPredicate ( O, len, direction, predicate, thisArg ), https://tc39.es/ecma262/#sec-findviapredicate
static ThrowCompletionOr<FoundValue> find_via_predicate(VM& vm, TypedArrayBase const& typed_array, u32 length, Direction direction, Value this_arg, StringView prototype_name)
{
    // 1. If IsCallable(predicate) is false, throw a TypeError exception.
    auto predicate = TRY(callback_from_args(vm, prototype_name));

    Vector<u32> indices;
    indices.ensure_capacity(length);

    // 2. If direction is ascending, then
    if (direction == Direction::Ascending) {
        // a. Let indices be a List of the integers in the interval from 0 (inclusive) to len (exclusive), in ascending order.
        for (u32 i = 0; i < length; ++i)
            indices.unchecked_append(i);
    }
    // 3. Else,
    else {
        // a. Let indices be a List of the integers in the interval from 0 (inclusive) to len (exclusive), in descending order.
        for (u32 i = length; i > 0; --i)
            indices.unchecked_append(i - 1);
    }

    // 4. For each integer k of indices, do
    for (auto k : indices) {
        // a. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        // b. NOTE: If O is a TypedArray, the following invocation of Get will return a normal completion.

        // c. Let kValue be ? Get(O, Pk).
        auto value = TRY(typed_array.get(property_key));

        // d. Let testResult be ? Call(predicate, thisArg, « kValue, 𝔽(k), O »).
        auto test_result = TRY(call(vm, *predicate, this_arg, value, Value { k }, &typed_array));

        // e. If ToBoolean(testResult) is true, return the Record { [[Index]]: 𝔽(k), [[Value]]: kValue }.
        if (test_result.to_boolean())
            return FoundValue { k, value };
    }

    // 5. Return the Record { [[Index]]: -1𝔽, [[Value]]: undefined }.
    return FoundValue { {}, js_undefined() };
}

// 23.2.3.11 %TypedArray%.prototype.find ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.find
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::find)
{
    auto this_arg = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. Let findRec be ? FindViaPredicate(O, len, ascending, predicate, thisArg).
    auto find_record = TRY(find_via_predicate(vm, *typed_array, length, Direction::Ascending, this_arg, "find"sv));

    // 5. Return findRec.[[Value]].
    return find_record.value;
}

// 23.2.3.12 %TypedArray%.prototype.findIndex ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.findindex
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::find_index)
{
    auto this_arg = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. Let findRec be ? FindViaPredicate(O, len, ascending, predicate, thisArg).
    auto find_record = TRY(find_via_predicate(vm, *typed_array, length, Direction::Ascending, this_arg, "findIndex"sv));

    // 5. Return findRec.[[Index]].
    return find_record.index_to_value();
}

// 23.2.3.13 %TypedArray%.prototype.findLast ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.findlast
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::find_last)
{
    auto this_arg = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. Let findRec be ? FindViaPredicate(O, len, descending, predicate, thisArg).
    auto find_record = TRY(find_via_predicate(vm, *typed_array, length, Direction::Descending, this_arg, "findLast"sv));

    // 5. Return findRec.[[Value]].
    return find_record.value;
}

// 23.2.3.14 %TypedArray%.prototype.findLastIndex ( predicate [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.findlastindex
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::find_last_index)
{
    auto this_arg = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. Let findRec be ? FindViaPredicate(O, len, descending, predicate, thisArg).
    auto find_record = TRY(find_via_predicate(vm, *typed_array, length, Direction::Descending, this_arg, "findLastIndex"sv));

    // 5. Return findRec.[[Index]].
    return find_record.index_to_value();
}

// 23.2.3.15 %TypedArray%.prototype.forEach ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.foreach
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::for_each)
{
    auto this_arg = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
    auto callback_function = TRY(callback_from_args(vm, "forEach"sv));

    // 5. Let k be 0.
    // 6. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        // b. Let kValue be ! Get(O, Pk).
        auto value = MUST(typed_array->get(property_key));

        // c. Perform ? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »).
        TRY(call(vm, *callback_function, this_arg, value, Value { k }, typed_array));

        // d. Set k to k + 1.
    }

    // 7. Return undefined.
    return js_undefined();
}

// 23.2.3.16 %TypedArray%.prototype.includes ( searchElement [ , fromIndex ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.includes
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::includes)
{
    auto search_element = vm.argument(0);
    auto from_index = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. If len = 0, return false.
    if (length == 0)
        return Value { false };

    // 5. Let n be ? ToIntegerOrInfinity(fromIndex).
    auto n = TRY(from_index.to_integer_or_infinity(vm));

    // 6. Assert: If fromIndex is undefined, then n is 0.
    if (from_index.is_undefined())
        VERIFY(n == 0);

    Value value_n { n };
    // 7. If n = +∞, return false.
    if (value_n.is_positive_infinity())
        return Value { false };
    // 8. Else if n = -∞, set n to 0.
    else if (value_n.is_negative_infinity())
        n = 0;

    u32 k;
    // 9. If n ≥ 0, then
    if (n >= 0) {
        // a. Let k be n.
        k = n;
    }
    // 10. Else,
    else {
        // a. Let k be len + n.
        auto relative_k = length + n; // Ensures we dont overflow `k`.

        // b. If k < 0, set k to 0.
        if (relative_k < 0)
            relative_k = 0;

        k = relative_k;
    }

    // 11. Repeat, while k < len,
    while (k < length) {
        // a. Let elementK be ! Get(O, ! ToString(𝔽(k))).
        auto element_k = MUST(typed_array->get(k));

        // b. If SameValueZero(searchElement, elementK) is true, return true.
        if (same_value_zero(search_element, element_k))
            return Value { true };

        // c. Set k to k + 1.
        ++k;
    }

    // 12. Return false.
    return Value { false };
}

// 23.2.3.17 %TypedArray%.prototype.indexOf ( searchElement [ , fromIndex ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.indexof
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::index_of)
{
    auto search_element = vm.argument(0);
    auto from_index = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. If len = 0, return -1𝔽.
    if (length == 0)
        return Value { -1 };

    // 5. Let n be ? ToIntegerOrInfinity(fromIndex).
    auto n = TRY(from_index.to_integer_or_infinity(vm));

    // 6. Assert: If fromIndex is undefined, then n is 0.
    if (from_index.is_undefined())
        VERIFY(n == 0);

    Value value_n { n };
    // 7. If n = +∞, return -1𝔽.
    if (value_n.is_positive_infinity())
        return Value { -1 };
    // 8. Else if n = -∞, set n to 0.
    else if (value_n.is_negative_infinity())
        n = 0;

    u32 k;
    // 9. If n ≥ 0, then
    if (n >= 0) {
        // a. Let k be n.
        k = n;
    }
    // 10. Else,
    else {
        // a. Let k be len + n.
        auto relative_k = length + n;

        // b. If k < 0, set k to 0.
        if (relative_k < 0)
            relative_k = 0;

        k = relative_k;
    }

    // 11. Repeat, while k < len,
    while (k < length) {
        // a. Let kPresent be ! HasProperty(O, ! ToString(𝔽(k))).
        auto k_present = MUST(typed_array->has_property(k));

        // b. If kPresent is true, then
        if (k_present) {
            // i. Let elementK be ! Get(O, ! ToString(𝔽(k))).
            auto element_k = MUST(typed_array->get(k));

            // ii. If IsStrictlyEqual(searchElement, elementK) is true, return 𝔽(k).
            if (is_strictly_equal(search_element, element_k))
                return Value { k };
        }

        // c. Set k to k + 1.
        ++k;
    }

    // 12. Return -1𝔽.
    return Value { -1 };
}

// 23.2.3.18 %TypedArray%.prototype.join ( separator ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.join
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::join)
{
    auto separator = vm.argument(0);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    String sep {};

    // 4. If separator is undefined, let sep be ",".
    if (separator.is_undefined())
        sep = String::from_code_point(',');
    // 5. Else, let sep be ? ToString(separator).
    else
        sep = TRY(separator.to_string(vm));

    // 6. Let R be the empty String.
    StringBuilder builder;

    // 7. Let k be 0.
    // 8. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. If k > 0, set R to the string-concatenation of R and sep.
        if (k > 0)
            builder.append(sep);

        // b. Let element be ! Get(O, ! ToString(𝔽(k))).
        auto element = MUST(typed_array->get(k));

        String next {};

        // c. If element is undefined, let next be the empty String; otherwise, let next be ! ToString(element).
        if (!element.is_undefined())
            next = MUST(element.to_string(vm));

        // d. Set R to the string-concatenation of R and next.
        builder.append(next);

        // e. Set k to k + 1.
    }

    // 9. Return R.
    return PrimitiveString::create(vm, MUST(builder.to_string()));
}

// 23.2.3.19 %TypedArray%.prototype.keys ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.keys
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::keys)
{
    auto& realm = *vm.current_realm();

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Perform ? ValidateTypedArray(O, seq-cst).
    (void)TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Return CreateArrayIterator(O, key).
    return ArrayIterator::create(realm, typed_array, Object::PropertyKind::Key);
}

// 23.2.3.20 %TypedArray%.prototype.lastIndexOf ( searchElement [ , fromIndex ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.lastindexof
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::last_index_of)
{
    auto search_element = vm.argument(0);
    auto from_index = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. If len = 0, return -1𝔽.
    if (length == 0)
        return Value { -1 };

    double n;
    // 5. If fromIndex is present, let n be ? ToIntegerOrInfinity(fromIndex); else let n be len - 1.
    if (vm.argument_count() > 1)
        n = TRY(from_index.to_integer_or_infinity(vm));
    else
        n = length - 1;

    // 6. If n = -∞, return -1𝔽.
    if (Value { n }.is_negative_infinity())
        return Value { -1 };

    i32 k;
    // 7. If n ≥ 0, then
    if (n >= 0) {
        // a. Let k be min(n, len - 1).
        k = min(n, (i32)length - 1);
    }
    // 8. Else,
    else {
        // a. Let k be len + n.
        auto relative_k = length + n; // Ensures we dont overflow `k`.

        if (relative_k < 0)
            relative_k = -1;

        k = relative_k;
    }

    // 9. Repeat, while k ≥ 0,
    while (k >= 0) {
        // a. Let kPresent be ! HasProperty(O, ! ToString(𝔽(k))).
        auto k_present = MUST(typed_array->has_property(k));

        // b. If kPresent is true, then
        if (k_present) {
            // i. Let elementK be ! Get(O, ! ToString(𝔽(k))).
            auto element_k = MUST(typed_array->get(k));

            // ii. If IsStrictlyEqual(searchElement, elementK) is true, return 𝔽(k).
            if (is_strictly_equal(search_element, element_k))
                return Value { k };
        }

        // c. Set k to k - 1.
        --k;
    }

    // 10. Return -1𝔽.
    return Value { -1 };
}

// 23.2.3.21 get %TypedArray%.prototype.length, https://tc39.es/ecma262/#sec-get-%typedarray%.prototype.length
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::length_getter)
{
    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[TypedArrayName]]).
    // 3. Assert: O has [[ViewedArrayBuffer]] and [[ArrayLength]] internal slots.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 4. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
    auto typed_array_record = make_typed_array_with_buffer_witness_record(*typed_array, ArrayBuffer::Order::SeqCst);

    // 5. If IsTypedArrayOutOfBounds(taRecord) is true, return +0𝔽.
    if (is_typed_array_out_of_bounds(typed_array_record))
        return Value { 0 };

    // 6. Let length be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 7. Return 𝔽(length).
    return Value { length };
}

// 23.2.3.22 %TypedArray%.prototype.map ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.map
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::map)
{
    auto this_arg = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
    auto callback_function = TRY(callback_from_args(vm, "map"sv));

    // 5. Let A be ? TypedArraySpeciesCreate(O, « 𝔽(len) »).
    GC::RootVector<Value> arguments(vm.heap());
    arguments.empend(length);
    auto* array = TRY(typed_array_species_create(vm, *typed_array, move(arguments)));

    // 6. Let k be 0.
    // 7. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        // b. Let kValue be ! Get(O, Pk).
        auto value = MUST(typed_array->get(property_key));

        // c. Let mappedValue be ? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »).
        auto mapped_value = TRY(call(vm, *callback_function, this_arg, value, Value { k }, typed_array));

        // d. Perform ? Set(A, Pk, mappedValue, true).
        TRY(array->set(property_key, mapped_value, Object::ShouldThrowExceptions::Yes));

        // e. Set k to k + 1.
    }

    // 8. Return A.
    return array;
}

// 23.2.3.23 %TypedArray%.prototype.reduce ( callbackfn [ , initialValue ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.reduce
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::reduce)
{
    auto initial_value = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
    auto callback_function = TRY(callback_from_args(vm, "reduce"sv));

    // 5. If len = 0 and initialValue is not present, throw a TypeError exception.
    if (length == 0 && vm.argument_count() <= 1)
        return vm.throw_completion<TypeError>(ErrorType::ReduceNoInitial);

    // 6. Let k be 0.
    u32 k = 0;

    // 7. Let accumulator be undefined.
    auto accumulator = js_undefined();

    // 8. If initialValue is present, then
    if (vm.argument_count() > 1) {
        // a. Set accumulator to initialValue.
        accumulator = initial_value;
    }
    // 9. Else,
    else {
        // a. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        // b. Set accumulator to ! Get(O, Pk).
        accumulator = MUST(typed_array->get(property_key));

        // c. Set k to k + 1.
        ++k;
    }

    // 10. Repeat, while k < len,
    while (k < length) {
        // a. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        // b. Let kValue be ! Get(O, Pk).
        auto value = MUST(typed_array->get(k));

        // c. Set accumulator to ? Call(callbackfn, undefined, « accumulator, kValue, 𝔽(k), O »).
        accumulator = TRY(call(vm, *callback_function, js_undefined(), accumulator, value, Value { k }, typed_array));

        // d. Set k to k + 1.
        ++k;
    }

    // 11. Return accumulator.
    return accumulator;
}

// 23.2.3.24 %TypedArray%.prototype.reduceRight ( callbackfn [ , initialValue ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.reduceright
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::reduce_right)
{
    auto initial_value = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
    auto callback_function = TRY(callback_from_args(vm, "reduceRight"sv));

    // 5. If len = 0 and initialValue is not present, throw a TypeError exception.
    if (length == 0 && vm.argument_count() <= 1)
        return vm.throw_completion<TypeError>(ErrorType::ReduceNoInitial);

    // 6. Let k be len - 1.
    auto k = static_cast<i32>(length) - 1;

    // 7. Let accumulator be undefined.
    auto accumulator = js_undefined();

    // 8. If initialValue is present, then
    if (vm.argument_count() > 1) {
        // a. Set accumulator to initialValue.
        accumulator = initial_value;
    }
    // 9. Else,
    else {
        // a. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        // b. Set accumulator to ! Get(O, Pk).
        accumulator = MUST(typed_array->get(property_key));

        // c. Set k to k - 1.
        --k;
    }

    // 10. Repeat, while k ≥ 0,
    while (k >= 0) {
        // a. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        // b. Let kValue be ! Get(O, Pk).
        auto value = MUST(typed_array->get(k));

        // c. Set accumulator to ? Call(callbackfn, undefined, « accumulator, kValue, 𝔽(k), O »).
        accumulator = TRY(call(vm, *callback_function, js_undefined(), accumulator, value, Value { k }, typed_array));

        // d. Set k to k - 1.
        --k;
    }

    // 11. Return accumulator.
    return accumulator;
}

// 23.2.3.25 %TypedArray%.prototype.reverse ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.reverse
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::reverse)
{
    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. Let middle be floor(len / 2).
    auto middle = length / 2;

    // 5. Let lower be 0.
    // 6. Repeat, while lower ≠ middle,
    for (u32 lower = 0; lower != middle; ++lower) {
        // a. Let upper be len - lower - 1.
        auto upper = length - lower - 1;

        // b. Let upperP be ! ToString(𝔽(upper)).
        PropertyKey upper_property_key { upper };

        // c. Let lowerP be ! ToString(𝔽(lower)).
        PropertyKey lower_property_key { lower };

        // d. Let lowerValue be ! Get(O, lowerP).
        auto lower_value = MUST(typed_array->get(lower_property_key));

        // e. Let upperValue be ! Get(O, upperP).
        auto upper_value = MUST(typed_array->get(upper_property_key));

        // f. Perform ! Set(O, lowerP, upperValue, true).
        MUST(typed_array->set(lower_property_key, upper_value, Object::ShouldThrowExceptions::Yes));

        // g. Perform ! Set(O, upperP, lowerValue, true).
        MUST(typed_array->set(upper_property_key, lower_value, Object::ShouldThrowExceptions::Yes));

        // h. Set lower to lower + 1.
    }

    // 7. Return O.
    return typed_array;
}

// 23.2.3.26.1 SetTypedArrayFromTypedArray ( target, targetOffset, source ), https://tc39.es/ecma262/#sec-settypedarrayfromtypedarray
static ThrowCompletionOr<void> set_typed_array_from_typed_array(VM& vm, TypedArrayBase& target, double target_offset, TypedArrayBase const& source)
{
    // 1. Let targetBuffer be target.[[ViewedArrayBuffer]].
    auto* target_buffer = target.viewed_array_buffer();

    // 2. Let targetRecord be MakeTypedArrayWithBufferWitnessRecord(target, seq-cst)
    auto target_record = make_typed_array_with_buffer_witness_record(target, ArrayBuffer::Order::SeqCst);

    // 3. If IsTypedArrayOutOfBounds(targetRecord) is true, throw a TypeError exception.
    if (is_typed_array_out_of_bounds(target_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

    // 4. Let targetLength be TypedArrayLength(targetRecord).
    auto target_length = typed_array_length(target_record);

    // 5. Let srcBuffer be source.[[ViewedArrayBuffer]].
    auto* source_buffer = source.viewed_array_buffer();

    // 6. Let srcRecord be MakeTypedArrayWithBufferWitnessRecord(source, seq-cst).
    auto source_record = make_typed_array_with_buffer_witness_record(source, ArrayBuffer::Order::SeqCst);

    // 7. If IsTypedArrayOutOfBounds(srcRecord) is true, throw a TypeError exception.
    if (is_typed_array_out_of_bounds(source_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

    // 8. Let srcLength be TypedArrayLength(srcRecord).
    auto source_length = typed_array_length(source_record);

    // 9. Let targetType be TypedArrayElementType(target).
    // 10. Let targetElementSize be TypedArrayElementSize(target).
    auto target_element_size = target.element_size();

    // 11. Let targetByteOffset be target.[[ByteOffset]].
    auto target_byte_offset = target.byte_offset();

    // 12. Let srcType be TypedArrayElementType(source).
    // 13. Let srcElementSize be TypedArrayElementSize(source).
    auto source_element_size = source.element_size();

    // 14. Let srcByteOffset be source.[[ByteOffset]].
    auto source_byte_offset = source.byte_offset();

    // 15. If targetOffset = +∞, throw a RangeError exception.
    if (Value { target_offset }.is_positive_infinity())
        return vm.throw_completion<RangeError>(ErrorType::TypedArrayInvalidTargetOffset, "finite");

    // 16. If srcLength + targetOffset > targetLength, throw a RangeError exception.
    Checked<size_t> checked = source_length;

    if (target_offset > static_cast<double>(NumericLimits<size_t>::max()))
        return vm.throw_completion<RangeError>(ErrorType::TypedArrayOverflowOrOutOfBounds, "target offset");
    checked += static_cast<size_t>(target_offset);

    if (checked.has_overflow() || checked.value() > target_length)
        return vm.throw_completion<RangeError>(ErrorType::TypedArrayOverflowOrOutOfBounds, "target length");

    // 17. If target.[[ContentType]] is not source.[[ContentType]], throw a TypeError exception.
    if (target.content_type() != source.content_type())
        return vm.throw_completion<TypeError>(ErrorType::TypedArrayInvalidCopy, target.class_name(), source.class_name());

    auto same_shared_array_buffer = false;

    // 18. If IsSharedArrayBuffer(srcBuffer) is true, IsSharedArrayBuffer(targetBuffer) is true, and srcBuffer.[[ArrayBufferData]] is targetBuffer.[[ArrayBufferData]], let sameSharedArrayBuffer be true; otherwise, let sameSharedArrayBuffer be false.
    if (source_buffer->is_shared_array_buffer() && target_buffer->is_shared_array_buffer() && (&source_buffer->buffer() == &target_buffer->buffer()))
        same_shared_array_buffer = true;

    size_t source_byte_index = 0;

    // 19. If SameValue(srcBuffer, targetBuffer) is true or sameSharedArrayBuffer is true, then
    if (same_shared_array_buffer || same_value(source_buffer, target_buffer)) {
        // a. Let srcByteLength be TypedArrayByteLength(srcRecord).
        auto source_byte_length = typed_array_byte_length(source_record);

        // b. Set srcBuffer to ? CloneArrayBuffer(srcBuffer, srcByteOffset, srcByteLength).
        source_buffer = TRY(clone_array_buffer(vm, *source_buffer, source_byte_offset, source_byte_length));

        // c. Let srcByteIndex be 0.
        source_byte_index = 0;
    }
    // 20. Else,
    else {
        // a. Let srcByteIndex be srcByteOffset.
        source_byte_index = source_byte_offset;
    }

    // 21. Let targetByteIndex be targetOffset × targetElementSize + targetByteOffset.
    Checked<size_t> checked_target_byte_index(static_cast<size_t>(target_offset));
    checked_target_byte_index *= target_element_size;
    checked_target_byte_index += target_byte_offset;
    if (checked_target_byte_index.has_overflow())
        return vm.throw_completion<RangeError>(ErrorType::TypedArrayOverflow, "target byte index");
    auto target_byte_index = checked_target_byte_index.value();

    // 22. Let limit be targetByteIndex + targetElementSize × srcLength.
    Checked<size_t> checked_limit(source_length);
    checked_limit *= target_element_size;
    checked_limit += target_byte_index;
    if (checked_limit.has_overflow())
        return vm.throw_completion<RangeError>(ErrorType::TypedArrayOverflow, "target limit");
    auto limit = checked_limit.value();

    // 23. If srcType is targetType, then
    if (source.element_name() == target.element_name()) {
        // a. NOTE: The transfer must be performed in a manner that preserves the bit-level encoding of the source data.
        // b. Repeat, while targetByteIndex < limit,
        //     i. Let value be GetValueFromBuffer(srcBuffer, srcByteIndex, Uint8, true, Unordered).
        //     ii. Perform SetValueInBuffer(targetBuffer, targetByteIndex, Uint8, value, true, Unordered).
        //     iii. Set srcByteIndex to srcByteIndex + 1.
        //     iv. Set targetByteIndex to targetByteIndex + 1.
        target_buffer->buffer().overwrite(target_byte_index, source_buffer->buffer().data() + source_byte_index, limit - target_byte_index);
    }
    // 24. Else,
    else {
        // a. Repeat, while targetByteIndex < limit,
        while (target_byte_index < limit) {
            // i. Let value be GetValueFromBuffer(srcBuffer, srcByteIndex, srcType, true, Unordered).
            auto value = source.get_value_from_buffer(source_byte_index, ArrayBuffer::Unordered);

            // ii. Perform SetValueInBuffer(targetBuffer, targetByteIndex, targetType, value, true, Unordered).
            target.set_value_in_buffer(target_byte_index, value, ArrayBuffer::Unordered);

            // iii. Set srcByteIndex to srcByteIndex + srcElementSize.
            source_byte_index += source_element_size;

            // iv. Set targetByteIndex to targetByteIndex + targetElementSize.
            target_byte_index += target_element_size;
        }
    }

    // 25. Return unused.
    return {};
}

// 23.2.3.26.2 SetTypedArrayFromArrayLike ( target, targetOffset, source ), https://tc39.es/ecma262/#sec-settypedarrayfromarraylike
static ThrowCompletionOr<void> set_typed_array_from_array_like(VM& vm, TypedArrayBase& target, double target_offset, Value source)
{
    // 1. Let targetRecord be MakeTypedArrayWithBufferWitnessRecord(target, seq-cst)
    auto target_record = make_typed_array_with_buffer_witness_record(target, ArrayBuffer::Order::SeqCst);

    // 2. If IsTypedArrayOutOfBounds(targetRecord) is true, throw a TypeError exception.
    if (is_typed_array_out_of_bounds(target_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

    // 3. Let targetLength be TypedArrayLength(targetRecord).
    auto target_length = typed_array_length(target_record);

    // 4. Let src be ? ToObject(source).
    auto source_object = TRY(source.to_object(vm));

    // 5. Let srcLength be ? LengthOfArrayLike(src).
    auto source_length = TRY(length_of_array_like(vm, source_object));

    // 6. If targetOffset = +∞, throw a RangeError exception.
    if (Value { target_offset }.is_positive_infinity())
        return vm.throw_completion<RangeError>(ErrorType::TypedArrayInvalidTargetOffset, "finite");

    // 7. If srcLength + targetOffset > targetLength, throw a RangeError exception.
    Checked<size_t> checked = source_length;

    if (target_offset > static_cast<double>(NumericLimits<size_t>::max()))
        return vm.throw_completion<RangeError>(ErrorType::TypedArrayOverflowOrOutOfBounds, "target offset");
    checked += static_cast<size_t>(target_offset);

    if (checked.has_overflow() || checked.value() > target_length)
        return vm.throw_completion<RangeError>(ErrorType::TypedArrayOverflowOrOutOfBounds, "target length");

    // 8. Let k be 0.
    size_t k = 0;

    // 9. Repeat, while k < srcLength,
    while (k < source_length) {
        // a. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        // b. Let value be ? Get(src, Pk).
        auto value = TRY(source_object->get(property_key));

        // c. Let targetIndex be 𝔽(targetOffset + k).
        // NOTE: We verify above that target_offset + source_length is valid, so this cannot fail.
        auto target_index = MUST(CanonicalIndex::from_double(vm, CanonicalIndex::Type::Index, target_offset + k));

        // d. Perform ? TypedArraySetElement(target, targetIndex, value).
        // FIXME: This is very awkward.
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    if (is<ClassName>(target))                                                      \
        TRY(typed_array_set_element<Type>(target, target_index, value));
        JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

        // e. Set k to k + 1.
        ++k;
    }

    // 10. Return unused.
    return {};
}

// 23.2.3.26 %TypedArray%.prototype.set ( source [ , offset ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.set
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::set)
{
    auto source = vm.argument(0);
    auto offset = vm.argument(1);

    // 1. Let target be the this value.
    // 2. Perform ? RequireInternalSlot(target, [[TypedArrayName]]).
    // 3. Assert: target has a [[ViewedArrayBuffer]] internal slot.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 4. Let targetOffset be ? ToIntegerOrInfinity(offset).
    auto target_offset = TRY(offset.to_integer_or_infinity(vm));

    // 5. If targetOffset < 0, throw a RangeError exception.
    if (target_offset < 0)
        return vm.throw_completion<RangeError>(ErrorType::TypedArrayInvalidTargetOffset, "positive");

    // 6. If source is an Object that has a [[TypedArrayName]] internal slot, then
    if (source.is_object() && is<TypedArrayBase>(source.as_object())) {
        auto& source_typed_array = static_cast<TypedArrayBase&>(source.as_object());

        // a. Perform ? SetTypedArrayFromTypedArray(target, targetOffset, source).
        TRY(set_typed_array_from_typed_array(vm, *typed_array, target_offset, source_typed_array));
    }
    // 7. Else,
    else {
        // a. Perform ? SetTypedArrayFromArrayLike(target, targetOffset, source).
        TRY(set_typed_array_from_array_like(vm, *typed_array, target_offset, source));
    }

    // 8. Return undefined.
    return js_undefined();
}

// 23.2.3.27 %TypedArray%.prototype.slice ( start, end ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.slice
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::slice)
{
    auto start = vm.argument(0);
    auto end = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. Let relativeStart be ? ToIntegerOrInfinity(start).
    auto relative_start = TRY(start.to_integer_or_infinity(vm));

    i32 k = 0;
    // 5. If relativeStart = -∞, let k be 0.
    if (Value { relative_start }.is_negative_infinity())
        k = 0;
    // 6. Else if relativeStart < 0, let k be max(len + relativeStart, 0).
    else if (relative_start < 0)
        k = max(length + relative_start, 0);
    // 7. Else, let k be min(relativeStart, len).
    else
        k = min(relative_start, length);

    double relative_end = 0;
    // 8. If end is undefined, let relativeEnd be len; else let relativeEnd be ? ToIntegerOrInfinity(end).
    if (end.is_undefined())
        relative_end = length;
    else
        relative_end = TRY(end.to_integer_or_infinity(vm));

    i32 final = 0;
    // 9. If relativeEnd is -∞, let final be 0.
    if (Value { relative_end }.is_negative_infinity())
        final = 0;
    // 10. Else if relativeEnd < 0, let final be max(len + relativeEnd, 0).
    else if (relative_end < 0)
        final = max(length + relative_end, 0);
    // 11. Else, let final be min(relativeEnd, len).
    else
        final = min(relative_end, length);

    // 12. Let count be max(final - k, 0).
    auto count = max(final - k, 0);

    // 13. Let A be ? TypedArraySpeciesCreate(O, « 𝔽(count) »).
    GC::RootVector<Value> arguments(vm.heap());
    arguments.empend(count);
    auto* array = TRY(typed_array_species_create(vm, *typed_array, move(arguments)));

    // 14. If count > 0, then
    if (count > 0) {
        // a. Set taRecord to MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
        typed_array_record = make_typed_array_with_buffer_witness_record(*typed_array, ArrayBuffer::Order::SeqCst);

        // b. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
        if (is_typed_array_out_of_bounds(typed_array_record))
            return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

        // c. Set len to TypedArrayLength(taRecord).
        length = typed_array_length(typed_array_record);

        // d. Set final to min(final, len).
        final = min(final, length);

        // e. Set count to max(final - k, 0).
        count = max(final - k, 0);

        // f. Let srcType be TypedArrayElementType(O).
        // g. Let targetType be TypedArrayElementType(A).

        // h. If srcType is targetType, then
        if (typed_array->element_name() == array->element_name()) {
            // i. NOTE: The transfer must be performed in a manner that preserves the bit-level encoding of the source data.

            // ii. Let srcBuffer be O.[[ViewedArrayBuffer]].
            auto& source_buffer = *typed_array->viewed_array_buffer();

            // iii. Let targetBuffer be A.[[ViewedArrayBuffer]].
            auto& target_buffer = *array->viewed_array_buffer();

            // iv. Let elementSize be TypedArrayElementSize(O).
            auto element_size = typed_array->element_size();

            // v. Let srcByteOffset be O.[[ByteOffset]].
            auto source_byte_offset = typed_array->byte_offset();

            // vi. Let srcByteIndex be (k × elementSize) + srcByteOffset.
            Checked<u32> source_byte_index = k;
            source_byte_index *= element_size;
            source_byte_index += source_byte_offset;
            if (source_byte_index.has_overflow()) {
                dbgln("TypedArrayPrototype::slice: source_byte_index overflowed, returning as if succeeded.");
                return array;
            }

            // vii. Let targetByteIndex be A.[[ByteOffset]].
            auto target_byte_index = array->byte_offset();

            // viii. Let limit be targetByteIndex + (count × elementSize).
            Checked<u32> limit = count;
            limit *= element_size;
            limit += target_byte_index;
            if (limit.has_overflow()) {
                dbgln("TypedArrayPrototype::slice: limit overflowed, returning as if succeeded.");
                return array;
            }

            // ix. Repeat, while targetByteIndex < limit,
            while (target_byte_index < limit) {
                // 1. Let value be GetValueFromBuffer(srcBuffer, srcByteIndex, uint8, true, unordered).
                auto value = source_buffer.get_value<u8>(source_byte_index.value(), true, ArrayBuffer::Unordered);

                // 2. Perform SetValueInBuffer(targetBuffer, targetByteIndex, uint8, value, true, unordered).
                target_buffer.set_value<u8>(target_byte_index, value, true, ArrayBuffer::Unordered);

                // 3. Set srcByteIndex to srcByteIndex + 1.
                ++source_byte_index;

                // 4. Set targetByteIndex to targetByteIndex + 1.
                ++target_byte_index;
            }
        }
        // i. Else,
        else {
            // i. Let n be 0.
            u32 n = 0;

            // ii. Repeat, while k < final,
            while (k < final) {
                // 1. Let Pk be ! ToString(𝔽(k)).
                PropertyKey property_key { k };

                // 2. Let kValue be ! Get(O, Pk).
                auto value = MUST(typed_array->get(property_key));

                // 3. Perform ! Set(A, ! ToString(𝔽(n)), kValue, true).
                MUST(array->set(n, value, Object::ShouldThrowExceptions::Yes));

                // 4. Set k to k + 1.
                ++k;

                // 5. Set n to n + 1.
                ++n;
            }
        }
    }

    // 15. Return A.
    return array;
}

// 23.2.3.28 %TypedArray%.prototype.some ( callbackfn [ , thisArg ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.some
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::some)
{
    auto this_arg = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. If IsCallable(callbackfn) is false, throw a TypeError exception.
    auto callback_function = TRY(callback_from_args(vm, "some"sv));

    // 5. Let k be 0.
    // 6. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        // b. Let kValue be ! Get(O, Pk).
        auto value = MUST(typed_array->get(property_key));

        // c. Let testResult be ToBoolean(? Call(callbackfn, thisArg, « kValue, 𝔽(k), O »)).
        auto test_result = TRY(call(vm, *callback_function, this_arg, value, Value { k }, typed_array)).to_boolean();

        // d. If testResult is true, return true.
        if (test_result)
            return true;

        // e. Set k to k + 1.
    }

    // 7. Return false.
    return false;
}

// 23.2.3.29 %TypedArray%.prototype.sort ( comparefn ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.sort
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::sort)
{
    auto compare_function = vm.argument(0);

    // 1. If comparefn is not undefined and IsCallable(comparefn) is false, throw a TypeError exception.
    if (!compare_function.is_undefined() && !compare_function.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, compare_function);

    // 2. Let obj be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 3. Let taRecord be ? ValidateTypedArray(obj, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 4. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 5. NOTE: The following closure performs a numeric comparison rather than the string comparison used in 23.1.3.30.
    // 6. Let SortCompare be a new Abstract Closure with parameters (x, y) that captures comparefn and performs the following steps when called:
    Function<ThrowCompletionOr<double>(Value, Value)> sort_compare = [&](auto x, auto y) -> ThrowCompletionOr<double> {
        // a. Return ? CompareTypedArrayElements(x, y, comparefn).
        return TRY(compare_typed_array_elements(vm, x, y, compare_function.is_undefined() ? nullptr : &compare_function.as_function()));
    };

    // 7. Let sortedList be ? SortIndexedProperties(obj, len, SortCompare, read-through-holes).
    auto sorted_list = TRY(sort_indexed_properties(vm, *typed_array, length, sort_compare, Holes::ReadThroughHoles));

    // 8. Let j be 0.
    // 9. Repeat, while j < len,
    for (size_t j = 0; j < length; j++) {
        // a. Perform ! Set(obj, ! ToString(𝔽(j)), sortedList[j], true).
        MUST(typed_array->set(j, sorted_list[j], Object::ShouldThrowExceptions::Yes));

        // b. Set j to j + 1.
    }

    // 10. Return obj.
    return typed_array;
}

// 23.2.3.30 %TypedArray%.prototype.subarray ( begin, end ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.subarray
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::subarray)
{
    auto begin = vm.argument(0);
    auto end = vm.argument(1);

    // 1. Let O be the this value.
    // 2. Perform ? RequireInternalSlot(O, [[TypedArrayName]]).
    // 3. Assert: O has a [[ViewedArrayBuffer]] internal slot.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 4. Let buffer be O.[[ViewedArrayBuffer]].
    auto* buffer = typed_array->viewed_array_buffer();

    // 5. Let srcRecord be MakeTypedArrayWithBufferWitnessRecord(O, seq-cst).
    auto source_record = make_typed_array_with_buffer_witness_record(*typed_array, ArrayBuffer::Order::SeqCst);

    u32 source_length = 0;
    // 6. If IsTypedArrayOutOfBounds(srcRecord) is true, then
    if (is_typed_array_out_of_bounds(source_record)) {
        // a. Let srcLength be 0.
        source_length = 0;
    }
    // 7. Else,
    else {
        // a. Let srcLength be TypedArrayLength(srcRecord).
        source_length = typed_array_length(source_record);
    }

    // 8. Let relativeBegin be ? ToIntegerOrInfinity(begin).
    auto relative_begin = TRY(begin.to_integer_or_infinity(vm));

    i32 begin_index = 0;
    // 7. If relativeBegin = -∞, let beginIndex be 0.
    if (Value(relative_begin).is_negative_infinity())
        begin_index = 0;
    // 8. Else if relativeBegin < 0, let beginIndex be max(srcLength + relativeBegin, 0).
    else if (relative_begin < 0)
        begin_index = max(source_length + relative_begin, 0);
    // 9. Else, let beginIndex be min(relativeBegin, srcLength).
    else
        begin_index = min(relative_begin, source_length);

    // 12. Let elementSize be TypedArrayElementSize(O).
    auto element_size = typed_array->element_size();

    // 13. Let srcByteOffset be O.[[ByteOffset]].
    auto source_byte_offset = typed_array->byte_offset();

    // 14. Let beginByteOffset be srcByteOffset + beginIndex × elementSize.
    Checked<u32> begin_byte_offset = begin_index;
    begin_byte_offset *= element_size;
    begin_byte_offset += source_byte_offset;
    if (begin_byte_offset.has_overflow()) {
        dbgln("TypedArrayPrototype::begin_byte_offset: limit overflowed, returning as if succeeded.");
        return typed_array;
    }

    GC::RootVector<Value> arguments(vm.heap());

    // 15. If O.[[ArrayLength]] is auto and end is undefined, then
    if (typed_array->array_length().is_auto() && end.is_undefined()) {
        // a. Let argumentsList be « buffer, 𝔽(beginByteOffset) ».
        arguments.empend(buffer);
        arguments.empend(begin_byte_offset.value());
    }
    // 16. Else,
    else {
        double relative_end = 0;
        // a. If end is undefined, let relativeEnd be srcLength; else let relativeEnd be ? ToIntegerOrInfinity(end).
        if (end.is_undefined())
            relative_end = source_length;
        else
            relative_end = TRY(end.to_integer_or_infinity(vm));

        i32 end_index = 0;
        // 11. If relativeEnd = -∞, let endIndex be 0.
        if (Value(relative_end).is_negative_infinity())
            end_index = 0;
        // 12. Else if relativeEnd < 0, let endIndex be max(srcLength + relativeEnd, 0).
        else if (relative_end < 0)
            end_index = max(source_length + relative_end, 0);
        // 13. Else, let endIndex be min(relativeEnd, srcLength).
        else
            end_index = min(relative_end, source_length);

        // e. Let newLength be max(endIndex - beginIndex, 0).
        auto new_length = max(end_index - begin_index, 0);

        // f. Let argumentsList be « buffer, 𝔽(beginByteOffset), 𝔽(newLength) ».
        arguments.empend(buffer);
        arguments.empend(begin_byte_offset.value());
        arguments.empend(new_length);
    }

    // 17. Return ? TypedArraySpeciesCreate(O, argumentsList).
    return TRY(typed_array_species_create(vm, *typed_array, move(arguments)));
}

// 23.2.3.31 %TypedArray%.prototype.toLocaleString ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.tolocalestring
// 19.5.1 Array.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-array.prototype.tolocalestring
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::to_locale_string)
{
    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // This function is not generic. ValidateTypedArray is applied to the this value prior to evaluating the algorithm.
    // If its result is an abrupt completion that exception is thrown instead of evaluating the algorithm.

    // 1. Let array be ? ToObject(this value).
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let len be ? ToLength(? Get(array, "length")).
    // The implementation of the algorithm may be optimized with the knowledge that the this value is an object that
    // has a fixed length and whose integer-indexed properties are not sparse.
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));
    auto length = typed_array_length(typed_array_record);

    // 3. Let separator be the implementation-defined list-separator String value appropriate for the host environment's current locale (such as ", ").
    constexpr auto separator = ',';

    // 4. Let R be the empty String.
    StringBuilder builder;

    // 5. Let k be 0.
    // 6. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. If k > 0, then
        if (k > 0) {
            // i. Set R to the string-concatenation of R and separator.
            builder.append(separator);
        }

        // b. Let nextElement be ? Get(array, ! ToString(k)).
        auto next_element = TRY(typed_array->get(k));

        // c. If nextElement is not undefined or null, then
        if (!next_element.is_nullish()) {
            // i. Let S be ? ToString(? Invoke(nextElement, "toLocaleString", « locales, options »)).
            auto locale_string_value = TRY(next_element.invoke(vm, vm.names.toLocaleString, locales, options));
            auto locale_string = TRY(locale_string_value.to_string(vm));

            // ii. Set R to the string-concatenation of R and S.
            builder.append(locale_string);
        }

        // d. Set k to k + 1.
    }

    // 7. Return R.
    return PrimitiveString::create(vm, builder.to_string_without_validation());
}

// 23.2.3.32 %TypedArray%.prototype.toReversed ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.toreversed
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::to_reversed)
{
    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let length be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. Let A be ? TypedArrayCreateSameType(O, « 𝔽(length) »).
    GC::RootVector<Value> arguments(vm.heap());
    arguments.empend(length);
    auto* array = TRY(typed_array_create_same_type(vm, *typed_array, move(arguments)));

    // 5. Let k be 0.
    // 6. Repeat, while k < length,
    for (size_t k = 0; k < length; ++k) {
        // a. Let from be ! ToString(𝔽(length - k - 1)).
        PropertyKey from { length - k - 1 };

        // b. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        // c. Let fromValue be ! Get(O, from).
        auto from_value = MUST(typed_array->get(from));

        // d. Perform ! Set(A, Pk, fromValue, true).
        MUST(array->set(property_key, from_value, Object::ShouldThrowExceptions::Yes));

        // e. Set k to k + 1.
    }

    // 7. Return A.
    return array;
}

// 23.2.3.33 %TypedArray%.prototype.toSorted ( comparefn ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.tosorted
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::to_sorted)
{
    auto compare_function = vm.argument(0);

    // 1. If comparefn is not undefined and IsCallable(comparefn) is false, throw a TypeError exception.
    if (!compare_function.is_undefined() && !compare_function.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, compare_function);

    // 2. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 3. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 4. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 5. Let A be ? TypedArrayCreateSameType(O, « 𝔽(len) »).
    GC::RootVector<Value> arguments(vm.heap());
    arguments.empend(length);
    auto* array = TRY(typed_array_create_same_type(vm, *typed_array, move(arguments)));

    // 6. NOTE: The following closure performs a numeric comparison rather than the string comparison used in 23.1.3.34.
    Function<ThrowCompletionOr<double>(Value, Value)> sort_compare = [&](auto x, auto y) -> ThrowCompletionOr<double> {
        // a. Return ? CompareTypedArrayElements(x, y, comparefn).
        return TRY(compare_typed_array_elements(vm, x, y, compare_function.is_undefined() ? nullptr : &compare_function.as_function()));
    };

    // 8. Let sortedList be ? SortIndexedProperties(O, len, SortCompare, read-through-holes).
    auto sorted_list = TRY(sort_indexed_properties(vm, *typed_array, length, sort_compare, Holes::ReadThroughHoles));

    // 9. Let j be 0.
    // 10. Repeat, while j < len,
    for (size_t j = 0; j < length; j++) {
        // a. Perform ! Set(A, ! ToString(𝔽(j)), sortedList[j], true).
        MUST(array->set(j, sorted_list[j], Object::ShouldThrowExceptions::Yes));

        // b. Set j to j + 1.
    }

    // 11. Return A.
    return array;
}

// 23.2.3.35 %TypedArray%.prototype.values ( ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.values
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::values)
{
    auto& realm = *vm.current_realm();

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Perform ? ValidateTypedArray(O, seq-cst).
    (void)TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Return CreateArrayIterator(O, value).
    return ArrayIterator::create(realm, typed_array, Object::PropertyKind::Value);
}

// 23.2.3.36 %TypedArray%.prototype.with ( index, value ), https://tc39.es/ecma262/#sec-%typedarray%.prototype.with
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::with)
{
    auto index = vm.argument(0);
    auto value = vm.argument(1);

    // 1. Let O be the this value.
    auto* typed_array = TRY(typed_array_from_this(vm));

    // 2. Let taRecord be ? ValidateTypedArray(O, seq-cst).
    auto typed_array_record = TRY(validate_typed_array(vm, *typed_array, ArrayBuffer::Order::SeqCst));

    // 3. Let len be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 4. Let relativeIndex be ? ToIntegerOrInfinity(index).
    auto relative_index = TRY(index.to_integer_or_infinity(vm));

    double actual_index = 0;
    // 5. If relativeIndex ≥ 0, let actualIndex be relativeIndex.
    if (relative_index >= 0)
        actual_index = relative_index;
    // 6. Else, let actualIndex be len + relativeIndex.
    else
        actual_index = length + relative_index;

    Value numeric_value;
    // 7. If O.[[ContentType]] is BigInt, let numericValue be ? ToBigInt(value).
    if (typed_array->content_type() == TypedArrayBase::ContentType::BigInt)
        numeric_value = TRY(value.to_bigint(vm));
    // 8. Else, let numericValue be ? ToNumber(value).
    else
        numeric_value = TRY(value.to_number(vm));

    // 9. If IsValidIntegerIndex(O, 𝔽(actualIndex)) is false, throw a RangeError exception.
    if (!is_valid_integer_index(*typed_array, TRY(CanonicalIndex::from_double(vm, CanonicalIndex::Type::Index, actual_index))))
        return vm.throw_completion<RangeError>(ErrorType::TypedArrayInvalidIntegerIndex, actual_index);

    // 10. Let A be ? TypedArrayCreateSameType(O, « 𝔽(len) »).
    GC::RootVector<Value> arguments(vm.heap());
    arguments.empend(length);
    auto* array = TRY(typed_array_create_same_type(vm, *typed_array, move(arguments)));

    // 11. Let k be 0.
    // 12. Repeat, while k < len,
    for (size_t k = 0; k < length; k++) {
        // a. Let Pk be ! ToString(𝔽(k)).
        PropertyKey property_key { k };

        Value from_value;
        // b. If k is actualIndex, let fromValue be numericValue.
        if (k == actual_index)
            from_value = numeric_value;
        // c. Else, let fromValue be ! Get(O, Pk).
        else
            from_value = MUST(typed_array->get(property_key));

        // d. Perform ! Set(A, Pk, fromValue, true).
        MUST(array->set(property_key, from_value, Object::ShouldThrowExceptions::Yes));

        // e. Set k to k + 1.
    }

    // 13. Return A.
    return array;
}

// 23.2.3.38 get %TypedArray%.prototype [ @@toStringTag ], https://tc39.es/ecma262/#sec-get-%typedarray%.prototype-@@tostringtag
JS_DEFINE_NATIVE_FUNCTION(TypedArrayPrototype::to_string_tag_getter)
{
    // 1. Let O be the this value.
    auto this_value = vm.this_value();

    // 2. If O is not an Object, return undefined.
    if (!this_value.is_object())
        return js_undefined();

    auto& this_object = this_value.as_object();

    // 3. If O does not have a [[TypedArrayName]] internal slot, return undefined.
    if (!this_object.is_typed_array())
        return js_undefined();

    // 4. Let name be O.[[TypedArrayName]].
    // 5. Assert: name is a String.
    // 6. Return name.
    return PrimitiveString::create(vm, static_cast<TypedArrayBase&>(this_object).element_name());
}

}
