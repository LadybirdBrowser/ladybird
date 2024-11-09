/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// This file explicitly implements support for JS Atomics API, which can
// involve slow (non-lock-free) atomic ops.
#include <AK/Platform.h>

#ifdef AK_COMPILER_CLANG
#    pragma clang diagnostic ignored "-Watomic-alignment"
#endif

#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <AK/Endian.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Agent.h>
#include <LibJS/Runtime/AtomicsObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

JS_DEFINE_ALLOCATOR(AtomicsObject);

// 25.4.2.1 ValidateIntegerTypedArray ( typedArray, waitable ), https://tc39.es/ecma262/#sec-validateintegertypedarray
static ThrowCompletionOr<TypedArrayWithBufferWitness> validate_integer_typed_array(VM& vm, TypedArrayBase const& typed_array, bool waitable)
{
    // 1. Let taRecord be ? ValidateTypedArray(typedArray, unordered).
    auto typed_array_record = TRY(validate_typed_array(vm, typed_array, ArrayBuffer::Order::Unordered));

    // 2. NOTE: Bounds checking is not a synchronizing operation when typedArray's backing buffer is a growable SharedArrayBuffer.

    auto const& type_name = typed_array.element_name();

    // 3. If waitable is true, then
    if (waitable) {
        // a. If typedArray.[[TypedArrayName]] is neither "Int32Array" nor "BigInt64Array", throw a TypeError exception.
        if ((type_name != vm.names.Int32Array.as_string()) && (type_name != vm.names.BigInt64Array.as_string()))
            return vm.throw_completion<TypeError>(ErrorType::TypedArrayTypeIsNot, type_name, "Int32 or BigInt64"sv);
    }
    // 4. Else,
    else {
        // a. Let type be TypedArrayElementType(typedArray).

        // b. If IsUnclampedIntegerElementType(type) is false and IsBigIntElementType(type) is false, throw a TypeError exception.
        if (!typed_array.is_unclamped_integer_element_type() && !typed_array.is_bigint_element_type())
            return vm.throw_completion<TypeError>(ErrorType::TypedArrayTypeIsNot, type_name, "an unclamped integer or BigInt"sv);
    }

    // 5. Return taRecord.
    return typed_array_record;
}

// 25.4.2.2 ValidateAtomicAccess ( taRecord, requestIndex ), https://tc39.es/ecma262/#sec-validateatomicaccess
static ThrowCompletionOr<size_t> validate_atomic_access(VM& vm, TypedArrayWithBufferWitness const& typed_array_record, Value request_index)
{
    // 1. Let length be TypedArrayLength(taRecord).
    auto length = typed_array_length(typed_array_record);

    // 2. Let accessIndex be ? ToIndex(requestIndex).
    // 3. Assert: accessIndex ≥ 0.
    auto access_index = TRY(request_index.to_index(vm));

    // 4. If accessIndex ≥ length, throw a RangeError exception.
    if (access_index >= length)
        return vm.throw_completion<RangeError>(ErrorType::IndexOutOfRange, access_index, length);

    // 5. Let typedArray be taRecord.[[Object]].
    auto const& typed_array = *typed_array_record.object;

    // 6. Let elementSize be TypedArrayElementSize(typedArray).
    auto element_size = typed_array.element_size();

    // 7. Let offset be typedArray.[[ByteOffset]].
    auto offset = typed_array.byte_offset();

    // 8. Return (accessIndex × elementSize) + offset.
    return (access_index * element_size) + offset;
}

// 25.4.3.3 ValidateAtomicAccessOnIntegerTypedArray ( typedArray, requestIndex [ , waitable ] ), https://tc39.es/ecma262/#sec-validateatomicaccessonintegertypedarray
static ThrowCompletionOr<size_t> validate_atomic_access_on_integer_typed_array(VM& vm, TypedArrayBase const& typed_array, Value request_index, bool waitable = false)
{
    // 1. If waitable is not present, set waitable to false.

    // 2. Let taRecord be ? ValidateIntegerTypedArray(typedArray, waitable).
    auto typed_array_record = TRY(validate_integer_typed_array(vm, typed_array, waitable));

    // 3. Return ? ValidateAtomicAccess(taRecord, requestIndex).
    return TRY(validate_atomic_access(vm, typed_array_record, request_index));
}

// 25.4.3.4 RevalidateAtomicAccess ( typedArray, byteIndexInBuffer ), https://tc39.es/ecma262/#sec-revalidateatomicaccess
static ThrowCompletionOr<void> revalidate_atomic_access(VM& vm, TypedArrayBase const& typed_array, size_t byte_index_in_buffer)
{
    // 1. Let taRecord be MakeTypedArrayWithBufferWitnessRecord(typedArray, unordered).
    auto typed_array_record = make_typed_array_with_buffer_witness_record(typed_array, ArrayBuffer::Order::Unordered);

    // 2. NOTE: Bounds checking is not a synchronizing operation when typedArray's backing buffer is a growable SharedArrayBuffer.
    // 3. If IsTypedArrayOutOfBounds(taRecord) is true, throw a TypeError exception.
    if (is_typed_array_out_of_bounds(typed_array_record))
        return vm.throw_completion<TypeError>(ErrorType::BufferOutOfBounds, "TypedArray"sv);

    // 4. Assert: byteIndexInBuffer ≥ typedArray.[[ByteOffset]].
    VERIFY(byte_index_in_buffer >= typed_array.byte_offset());

    // 5. If byteIndexInBuffer ≥ taRecord.[[CachedBufferByteLength]], throw a RangeError exception.
    if (byte_index_in_buffer >= typed_array_record.cached_buffer_byte_length.length())
        return vm.throw_completion<RangeError>(ErrorType::IndexOutOfRange, byte_index_in_buffer, typed_array_record.cached_buffer_byte_length.length());

    // 6. Return unused.
    return {};
}

// 25.4.2.17 AtomicReadModifyWrite ( typedArray, index, value, op ), https://tc39.es/ecma262/#sec-atomicreadmodifywrite
static ThrowCompletionOr<Value> atomic_read_modify_write(VM& vm, TypedArrayBase& typed_array, Value index, Value value, ReadWriteModifyFunction operation)
{
    // 1. Let byteIndexInBuffer be ? ValidateAtomicAccessOnIntegerTypedArray(typedArray, index).
    auto byte_index_in_buffer = TRY(validate_atomic_access_on_integer_typed_array(vm, typed_array, index));

    Value value_to_set;

    // 2. If typedArray.[[ContentType]] is bigint, let v be ? ToBigInt(value).
    if (typed_array.content_type() == TypedArrayBase::ContentType::BigInt)
        value_to_set = TRY(value.to_bigint(vm));
    // 3. Otherwise, let v be 𝔽(? ToIntegerOrInfinity(value)).
    else
        value_to_set = Value(TRY(value.to_integer_or_infinity(vm)));

    // 4. Perform ? RevalidateAtomicAccess(typedArray, byteIndexInBuffer).
    TRY(revalidate_atomic_access(vm, typed_array, byte_index_in_buffer));

    // 5. Let buffer be typedArray.[[ViewedArrayBuffer]].
    // 6. Let elementType be TypedArrayElementType(typedArray).
    // 7. Return GetModifySetValueInBuffer(buffer, byteIndexInBuffer, elementType, v, op).
    return typed_array.get_modify_set_value_in_buffer(byte_index_in_buffer, value_to_set, move(operation));
}

enum class WaitMode {
    Sync,
    Async,
};

// 25.4.3.14 DoWait ( mode, typedArray, index, value, timeout ), https://tc39.es/ecma262/#sec-dowait
static ThrowCompletionOr<Value> do_wait(VM& vm, WaitMode mode, TypedArrayBase& typed_array, Value index_value, Value expected_value, Value timeout_value)
{
    // 1. Let taRecord be ? ValidateIntegerTypedArray(typedArray, true).
    auto typed_array_record = TRY(validate_integer_typed_array(vm, typed_array, true));

    // 2. Let buffer be taRecord.[[Object]].[[ViewedArrayBuffer]].
    auto* buffer = typed_array_record.object->viewed_array_buffer();

    // 3. If IsSharedArrayBuffer(buffer) is false, throw a TypeError exception.
    if (!buffer->is_shared_array_buffer())
        return vm.throw_completion<TypeError>(ErrorType::NotASharedArrayBuffer);

    // 4. Let i be ? ValidateAtomicAccess(taRecord, index).
    auto index = TRY(validate_atomic_access(vm, typed_array_record, index_value));

    // 5. Let arrayTypeName be typedArray.[[TypedArrayName]].
    auto const& array_type_name = typed_array.element_name();

    // 6. If arrayTypeName is "BigInt64Array", let v be ? ToBigInt64(value).
    i64 value = 0;
    if (array_type_name == vm.names.BigInt64Array.as_string())
        value = TRY(expected_value.to_bigint_int64(vm));
    // 7. Else, let v be ? ToInt32(value).
    else
        value = TRY(expected_value.to_i32(vm));

    // 8. Let q be ? ToNumber(timeout).
    auto timeout_number = TRY(timeout_value.to_number(vm));

    // 9. If q is either NaN or +∞𝔽, let t be +∞; else if q is -∞𝔽, let t be 0; else let t be max(ℝ(q), 0).
    double timeout = 0;
    if (timeout_number.is_nan() || timeout_number.is_positive_infinity())
        timeout = js_infinity().as_double();
    else if (timeout_number.is_negative_infinity())
        timeout = 0.0;
    else
        timeout = max(timeout_number.as_double(), 0.0);

    // 10. If mode is sync and AgentCanSuspend() is false, throw a TypeError exception.
    if (mode == WaitMode::Sync && !agent_can_suspend())
        return vm.throw_completion<TypeError>(ErrorType::AgentCannotSuspend);

    // FIXME: Implement the remaining steps when we support SharedArrayBuffer.
    (void)index;
    (void)value;
    (void)timeout;

    return vm.throw_completion<InternalError>(ErrorType::NotImplemented, "SharedArrayBuffer"sv);
}

template<typename T, typename AtomicFunction>
static ThrowCompletionOr<Value> perform_atomic_operation(VM& vm, TypedArrayBase& typed_array, AtomicFunction&& operation)
{
    auto index = vm.argument(1);
    auto value = vm.argument(2);

    auto operation_wrapper = [&, operation = forward<AtomicFunction>(operation)](ByteBuffer x_bytes, ByteBuffer y_bytes) -> ByteBuffer {
        if constexpr (IsFloatingPoint<T>) {
            (void)operation;
            VERIFY_NOT_REACHED();
        } else {
            using U = Conditional<IsSame<ClampedU8, T>, u8, T>;

            auto* x = reinterpret_cast<U*>(x_bytes.data());
            auto* y = reinterpret_cast<U*>(y_bytes.data());
            operation(x, *y);

            return x_bytes;
        }
    };

    return atomic_read_modify_write(vm, typed_array, index, value, move(operation_wrapper));
}

AtomicsObject::AtomicsObject(Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{
}

void AtomicsObject::initialize(Realm& realm)
{
    Base::initialize(realm);
    auto& vm = this->vm();

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.add, add, 3, attr);
    define_native_function(realm, vm.names.and_, and_, 3, attr);
    define_native_function(realm, vm.names.compareExchange, compare_exchange, 4, attr);
    define_native_function(realm, vm.names.exchange, exchange, 3, attr);
    define_native_function(realm, vm.names.isLockFree, is_lock_free, 1, attr);
    define_native_function(realm, vm.names.load, load, 2, attr);
    define_native_function(realm, vm.names.or_, or_, 3, attr);
    define_native_function(realm, vm.names.pause, pause, 0, attr);
    define_native_function(realm, vm.names.store, store, 3, attr);
    define_native_function(realm, vm.names.sub, sub, 3, attr);
    define_native_function(realm, vm.names.wait, wait, 4, attr);
    define_native_function(realm, vm.names.waitAsync, wait_async, 4, attr);
    define_native_function(realm, vm.names.notify, notify, 3, attr);
    define_native_function(realm, vm.names.xor_, xor_, 3, attr);

    // 25.4.17 Atomics [ @@toStringTag ], https://tc39.es/ecma262/#sec-atomics-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Atomics"_string), Attribute::Configurable);
}

// 25.4.4 Atomics.add ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.add
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::add)
{
    auto* typed_array = TRY(typed_array_from(vm, vm.argument(0)));

    auto atomic_add = [](auto* storage, auto value) { return AK::atomic_fetch_add(storage, value); };

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    if (is<ClassName>(typed_array))                                                 \
        return TRY(perform_atomic_operation<Type>(vm, *typed_array, move(atomic_add)));
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

    VERIFY_NOT_REACHED();
}

// 25.4.5 Atomics.and ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.and
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::and_)
{
    auto* typed_array = TRY(typed_array_from(vm, vm.argument(0)));

    auto atomic_and = [](auto* storage, auto value) { return AK::atomic_fetch_and(storage, value); };

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    if (is<ClassName>(typed_array))                                                 \
        return TRY(perform_atomic_operation<Type>(vm, *typed_array, move(atomic_and)));
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

    VERIFY_NOT_REACHED();
}

// 25.4.6 Atomics.compareExchange ( typedArray, index, expectedValue, replacementValue ), https://tc39.es/ecma262/#sec-atomics.compareexchange
template<typename T>
static ThrowCompletionOr<Value> atomic_compare_exchange_impl(VM& vm, TypedArrayBase& typed_array, Value index, Value expected_value, Value replacement_value)
{
    // 1. Let byteIndexInBuffer be ? ValidateAtomicAccessOnIntegerTypedArray(typedArray, index).
    auto byte_index_in_buffer = TRY(validate_atomic_access_on_integer_typed_array(vm, typed_array, index));

    // 2. Let buffer be typedArray.[[ViewedArrayBuffer]].
    auto* buffer = typed_array.viewed_array_buffer();

    // 3. Let block be buffer.[[ArrayBufferData]].
    auto& block = buffer->buffer();

    Value expected;
    Value replacement;

    // 4. If typedArray.[[ContentType]] is bigint, then
    if (typed_array.content_type() == TypedArrayBase::ContentType::BigInt) {
        // a. Let expected be ? ToBigInt(expectedValue).
        expected = TRY(expected_value.to_bigint(vm));

        // b. Let replacement be ? ToBigInt(replacementValue).
        replacement = TRY(replacement_value.to_bigint(vm));
    }
    // 5. Else,
    else {
        // a. Let expected be 𝔽(? ToIntegerOrInfinity(expectedValue)).
        expected = Value(TRY(expected_value.to_integer_or_infinity(vm)));

        // b. Let replacement be 𝔽(? ToIntegerOrInfinity(replacementValue)).
        replacement = Value(TRY(replacement_value.to_integer_or_infinity(vm)));
    }

    // 6. Perform ? RevalidateAtomicAccess(typedArray, byteIndexInBuffer).
    TRY(revalidate_atomic_access(vm, typed_array, byte_index_in_buffer));

    // 7. Let elementType be TypedArrayElementType(typedArray).
    // 8. Let elementSize be TypedArrayElementSize(typedArray).

    // 9. Let isLittleEndian be the value of the [[LittleEndian]] field of the surrounding agent's Agent Record.
    static constexpr bool is_little_endian = AK::HostIsLittleEndian;

    // 10. Let expectedBytes be NumericToRawBytes(elementType, expected, isLittleEndian).
    auto expected_bytes = MUST(ByteBuffer::create_uninitialized(sizeof(T)));
    numeric_to_raw_bytes<T>(vm, expected, is_little_endian, expected_bytes);

    // 11. Let replacementBytes be NumericToRawBytes(elementType, replacement, isLittleEndian).
    auto replacement_bytes = MUST(ByteBuffer::create_uninitialized(sizeof(T)));
    numeric_to_raw_bytes<T>(vm, replacement, is_little_endian, replacement_bytes);

    // FIXME: Implement SharedArrayBuffer case.
    // 12. If IsSharedArrayBuffer(buffer) is true, then
    //     a. Let rawBytesRead be AtomicCompareExchangeInSharedBlock(block, byteIndexInBuffer, elementSize, expectedBytes, replacementBytes).
    // 13. Else,

    // a. Let rawBytesRead be a List of length elementSize whose elements are the sequence of elementSize bytes starting with block[byteIndexInBuffer].
    // FIXME: Propagate errors.
    auto raw_bytes_read = MUST(block.slice(byte_index_in_buffer, sizeof(T)));

    // b. If ByteListEqual(rawBytesRead, expectedBytes) is true, then
    //    i. Store the individual bytes of replacementBytes into block, starting at block[byteIndexInBuffer].
    if constexpr (IsFloatingPoint<T>) {
        VERIFY_NOT_REACHED();
    } else {
        using U = Conditional<IsSame<ClampedU8, T>, u8, T>;

        auto* v = reinterpret_cast<U*>(block.span().slice(byte_index_in_buffer).data());
        auto* e = reinterpret_cast<U*>(expected_bytes.data());
        auto* r = reinterpret_cast<U*>(replacement_bytes.data());
        (void)AK::atomic_compare_exchange_strong(v, *e, *r);
    }

    // 14. Return RawBytesToNumeric(elementType, rawBytesRead, isLittleEndian).
    return raw_bytes_to_numeric<T>(vm, raw_bytes_read, is_little_endian);
}

// 25.4.6 Atomics.compareExchange ( typedArray, index, expectedValue, replacementValue ), https://tc39.es/ecma262/#sec-atomics.compareexchange
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::compare_exchange)
{
    auto* typed_array = TRY(typed_array_from(vm, vm.argument(0)));
    auto index = vm.argument(1);
    auto expected_value = vm.argument(2);
    auto replacement_value = vm.argument(3);

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    if (is<ClassName>(typed_array))                                                 \
        return TRY(atomic_compare_exchange_impl<Type>(vm, *typed_array, index, expected_value, replacement_value));
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

    VERIFY_NOT_REACHED();
}

// 25.4.7 Atomics.exchange ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.exchange
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::exchange)
{
    auto* typed_array = TRY(typed_array_from(vm, vm.argument(0)));

    auto atomic_exchange = [](auto* storage, auto value) { return AK::atomic_exchange(storage, value); };

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    if (is<ClassName>(typed_array))                                                 \
        return TRY(perform_atomic_operation<Type>(vm, *typed_array, move(atomic_exchange)));
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

    VERIFY_NOT_REACHED();
}

// 25.4.8 Atomics.isLockFree ( size ), https://tc39.es/ecma262/#sec-atomics.islockfree
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::is_lock_free)
{
    auto size = TRY(vm.argument(0).to_integer_or_infinity(vm));
    if (size == 1)
        return Value(AK::atomic_is_lock_free<u8>());
    if (size == 2)
        return Value(AK::atomic_is_lock_free<u16>());
    if (size == 4)
        return Value(true);
    if (size == 8)
        return Value(AK::atomic_is_lock_free<u64>());
    return Value(false);
}

// 25.4.9 Atomics.load ( typedArray, index ), https://tc39.es/ecma262/#sec-atomics.load
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::load)
{
    auto* typed_array = TRY(typed_array_from(vm, vm.argument(0)));
    auto index = vm.argument(1);

    // 1. Let byteIndexInBuffer be ? ValidateAtomicAccessOnIntegerTypedArray(typedArray, index).
    auto byte_index_in_buffer = TRY(validate_atomic_access_on_integer_typed_array(vm, *typed_array, index));

    // 2. Perform ? RevalidateAtomicAccess(typedArray, byteIndexInBuffer).
    TRY(revalidate_atomic_access(vm, *typed_array, byte_index_in_buffer));

    // 3. Let buffer be typedArray.[[ViewedArrayBuffer]].
    // 4. Let elementType be TypedArrayElementType(typedArray).
    // 5. Return GetValueFromBuffer(buffer, byteIndexInBuffer, elementType, true, seq-cst).
    return typed_array->get_value_from_buffer(byte_index_in_buffer, ArrayBuffer::Order::SeqCst, true);
}

// 25.4.10 Atomics.or ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.or
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::or_)
{
    auto* typed_array = TRY(typed_array_from(vm, vm.argument(0)));

    auto atomic_or = [](auto* storage, auto value) { return AK::atomic_fetch_or(storage, value); };

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    if (is<ClassName>(typed_array))                                                 \
        return TRY(perform_atomic_operation<Type>(vm, *typed_array, move(atomic_or)));
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

    VERIFY_NOT_REACHED();
}

// 1 Atomics.pause ( [ N ] ), http://tc39.es/proposal-atomics-microwait/
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::pause)
{
    // NOTE: This value is arbitrary, but intends to put an upper bound on the spin loop of between ~10-100ns on most systems.
    constexpr i32 MAXIMUM_ITERATIONS = 1000;
    // NOTE: This value is arbitrary, but intends to account for function call overhead.
    constexpr i32 DEFAULT_ITERATIONS = 100;

    // 1. If N is neither undefined nor an integral Number, throw a TypeError exception.
    auto pause = vm.argument(0);

    if (!pause.is_undefined() && !pause.is_integral_number())
        return vm.throw_completion<TypeError>(ErrorType::NotAnIntegerOrUndefined, "pause time");

    // 2. If the execution environment of the ECMAScript implementation supports signaling to the operating system or CPU that the current executing code is in a spin-wait loop, such as executing a pause CPU instruction, send that signal.
    //    When N is not undefined, it determines the number of times that signal is sent.
    u32 N = DEFAULT_ITERATIONS;
    if (!pause.is_undefined()) {
        auto integral = pause.as_i32_clamped_integral_number();
        if (integral < 0)
            N = MAXIMUM_ITERATIONS + max(integral, -MAXIMUM_ITERATIONS) + 1;
        else
            // Implementation note: `N` is not required to be the _number of times_ that the signal is sent, but it seems like reasonable behaviour regardless.
            N = min(integral, MAXIMUM_ITERATIONS);
    }

    // The number of times the signal is sent for an integral Number N is less than or equal to the number times it is sent for N + 1 if both N and N + 1 have the same sign.
    for (; N != 0; N--)
        AK::atomic_pause();

    // 3. Return undefined.
    return js_undefined();
}

// 25.4.11 Atomics.store ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.store
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::store)
{
    auto* typed_array = TRY(typed_array_from(vm, vm.argument(0)));
    auto index = vm.argument(1);
    auto value = vm.argument(2);

    // 1. Let byteIndexInBuffer be ? ValidateAtomicAccessOnIntegerTypedArray(typedArray, index).
    auto byte_index_in_buffer = TRY(validate_atomic_access_on_integer_typed_array(vm, *typed_array, index));

    // 2. If typedArray.[[ContentType]] is bigint, let v be ? ToBigInt(value).
    if (typed_array->content_type() == TypedArrayBase::ContentType::BigInt)
        value = TRY(value.to_bigint(vm));
    // 3. Otherwise, let v be 𝔽(? ToIntegerOrInfinity(value)).
    else
        value = Value(TRY(value.to_integer_or_infinity(vm)));

    // 4. Perform ? RevalidateAtomicAccess(typedArray, byteIndexInBuffer).
    TRY(revalidate_atomic_access(vm, *typed_array, byte_index_in_buffer));

    // 5. Let buffer be typedArray.[[ViewedArrayBuffer]].
    // 6. Let elementType be TypedArrayElementType(typedArray).
    // 7. Perform SetValueInBuffer(buffer, byteIndexInBuffer, elementType, v, true, seq-cst).
    typed_array->set_value_in_buffer(byte_index_in_buffer, value, ArrayBuffer::Order::SeqCst, true);

    // 8. Return v.
    return value;
}

// 25.4.12 Atomics.sub ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.sub
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::sub)
{
    auto* typed_array = TRY(typed_array_from(vm, vm.argument(0)));

    auto atomic_sub = [](auto* storage, auto value) { return AK::atomic_fetch_sub(storage, value); };

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    if (is<ClassName>(typed_array))                                                 \
        return TRY(perform_atomic_operation<Type>(vm, *typed_array, move(atomic_sub)));
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

    VERIFY_NOT_REACHED();
}

// 25.4.13 Atomics.wait ( typedArray, index, value, timeout ), https://tc39.es/ecma262/#sec-atomics.wait
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::wait)
{
    auto* typed_array = TRY(typed_array_from(vm, vm.argument(0)));
    auto index = vm.argument(1);
    auto value = vm.argument(2);
    auto timeout = vm.argument(3);

    // 1. Return ? DoWait(sync, typedArray, index, value, timeout).
    return TRY(do_wait(vm, WaitMode::Sync, *typed_array, index, value, timeout));
}

// 25.4.14 Atomics.waitAsync ( typedArray, index, value, timeout ), https://tc39.es/ecma262/#sec-atomics.waitasync
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::wait_async)
{
    auto* typed_array = TRY(typed_array_from(vm, vm.argument(0)));
    auto index = vm.argument(1);
    auto value = vm.argument(2);
    auto timeout = vm.argument(3);

    // 1. Return ? DoWait(async, typedArray, index, value, timeout).
    return TRY(do_wait(vm, WaitMode::Async, *typed_array, index, value, timeout));
}

// 25.4.15 Atomics.notify ( typedArray, index, count ), https://tc39.es/ecma262/#sec-atomics.notify
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::notify)
{
    auto* typed_array = TRY(typed_array_from(vm, vm.argument(0)));
    auto index = vm.argument(1);
    auto count_value = vm.argument(2);

    // 1. Let byteIndexInBuffer be ? ValidateAtomicAccessOnIntegerTypedArray(typedArray, index, true).
    auto byte_index_in_buffer = TRY(validate_atomic_access_on_integer_typed_array(vm, *typed_array, index, true));

    // 2. If count is undefined, then
    double count = 0.0;
    if (count_value.is_undefined()) {
        // a. Let c be +∞.
        count = js_infinity().as_double();
    }
    // 3. Else,
    else {
        // a. Let intCount be ? ToIntegerOrInfinity(count).
        auto int_count = TRY(count_value.to_integer_or_infinity(vm));

        // b. Let c be max(intCount, 0).
        count = max(int_count, 0.0);
    }

    // 4. Let buffer be typedArray.[[ViewedArrayBuffer]].
    auto* buffer = typed_array->viewed_array_buffer();

    // 5. Let block be buffer.[[ArrayBufferData]].
    auto& block = buffer->buffer();

    // 6. If IsSharedArrayBuffer(buffer) is false, return +0𝔽.
    if (!buffer->is_shared_array_buffer())
        return Value { 0 };

    // FIXME: Implement the remaining steps when we support SharedArrayBuffer.
    (void)byte_index_in_buffer;
    (void)count;
    (void)block;

    return vm.throw_completion<InternalError>(ErrorType::NotImplemented, "SharedArrayBuffer"sv);
}

// 25.4.16 Atomics.xor ( typedArray, index, value ), https://tc39.es/ecma262/#sec-atomics.xor
JS_DEFINE_NATIVE_FUNCTION(AtomicsObject::xor_)
{
    auto* typed_array = TRY(typed_array_from(vm, vm.argument(0)));

    auto atomic_xor = [](auto* storage, auto value) { return AK::atomic_fetch_xor(storage, value); };

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    if (is<ClassName>(typed_array))                                                 \
        return TRY(perform_atomic_operation<Type>(vm, *typed_array, move(atomic_xor)));
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

    VERIFY_NOT_REACHED();
}

}
