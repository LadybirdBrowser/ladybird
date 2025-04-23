/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/Streams/QueuingStrategy.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#validate-and-normalize-high-water-mark
WebIDL::ExceptionOr<double> extract_high_water_mark(QueuingStrategy const& strategy, double default_hwm)
{
    // 1. If strategy["highWaterMark"] does not exist, return defaultHWM.
    if (!strategy.high_water_mark.has_value())
        return default_hwm;

    // 2. Let highWaterMark be strategy["highWaterMark"].
    auto high_water_mark = strategy.high_water_mark.value();

    // 3. If highWaterMark is NaN or highWaterMark < 0, throw a RangeError exception.
    if (isnan(high_water_mark) || high_water_mark < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Invalid value for high water mark"sv };

    // 4. Return highWaterMark.
    return high_water_mark;
}

// https://streams.spec.whatwg.org/#make-size-algorithm-from-size-function
GC::Ref<SizeAlgorithm> extract_size_algorithm(JS::VM& vm, QueuingStrategy const& strategy)
{
    // 1. If strategy["size"] does not exist, return an algorithm that returns 1.
    if (!strategy.size)
        return GC::create_function(vm.heap(), [](JS::Value) { return JS::normal_completion(JS::Value(1)); });

    // 2. Return an algorithm that performs the following steps, taking a chunk argument:
    return GC::create_function(vm.heap(), [size = strategy.size](JS::Value chunk) {
        // 1. Return the result of invoking strategy["size"] with argument list « chunk ».
        return WebIDL::invoke_callback(*size, {}, { { chunk } });
    });
}

// https://streams.spec.whatwg.org/#can-transfer-array-buffer
bool can_transfer_array_buffer(JS::ArrayBuffer const& array_buffer)
{
    // 1. Assert: O is an Object.
    // 2. Assert: O has an [[ArrayBufferData]] internal slot.

    // 3. If ! IsDetachedBuffer(O) is true, return false.
    if (array_buffer.is_detached())
        return false;

    // 4. If SameValue(O.[[ArrayBufferDetachKey]], undefined) is false, return false.
    if (!JS::same_value(array_buffer.detach_key(), JS::js_undefined()))
        return false;

    // 5. Return true.
    return true;
}

// https://streams.spec.whatwg.org/#is-non-negative-number
bool is_non_negative_number(JS::Value value)
{
    // 1. If v is not a Number, return false.
    if (!value.is_number())
        return false;

    // 2. If v is NaN, return false.
    if (value.is_nan())
        return false;

    // 3. If v < 0, return false.
    if (value.as_double() < 0.0)
        return false;

    // 4. Return true.
    return true;
}

// https://streams.spec.whatwg.org/#transfer-array-buffer
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> transfer_array_buffer(JS::Realm& realm, JS::ArrayBuffer& buffer)
{
    auto& vm = realm.vm();

    // 1. Assert: ! IsDetachedBuffer(O) is false.
    VERIFY(!buffer.is_detached());

    // 2. Let arrayBufferData be O.[[ArrayBufferData]].
    // 3. Let arrayBufferByteLength be O.[[ArrayBufferByteLength]].
    auto array_buffer = buffer.buffer();

    // 4. Perform ? DetachArrayBuffer(O).
    TRY(JS::detach_array_buffer(vm, buffer));

    // 5. Return a new ArrayBuffer object, created in the current Realm, whose [[ArrayBufferData]] internal slot value is arrayBufferData and whose [[ArrayBufferByteLength]] internal slot value is arrayBufferByteLength.
    return JS::ArrayBuffer::create(realm, move(array_buffer));
}

// https://streams.spec.whatwg.org/#abstract-opdef-cloneasuint8array
WebIDL::ExceptionOr<JS::Value> clone_as_uint8_array(JS::Realm& realm, WebIDL::ArrayBufferView& view)
{
    auto& vm = realm.vm();

    // 1. Assert: O is an Object.
    // 2. Assert: O has an [[ViewedArrayBuffer]] internal slot.

    // 3. Assert: ! IsDetachedBuffer(O.[[ViewedArrayBuffer]]) is false.
    VERIFY(!view.viewed_array_buffer()->is_detached());

    // 4. Let buffer be ? CloneArrayBuffer(O.[[ViewedArrayBuffer]], O.[[ByteOffset]], O.[[ByteLength]], %ArrayBuffer%).
    auto* buffer = TRY(JS::clone_array_buffer(vm, *view.viewed_array_buffer(), view.byte_offset(), view.byte_length()));

    // 5. Let array be ! Construct(%Uint8Array%, « buffer »).
    auto array = MUST(JS::construct(vm, *realm.intrinsics().uint8_array_constructor(), buffer));

    // 5. Return array.
    return array;
}

// https://streams.spec.whatwg.org/#abstract-opdef-structuredclone
WebIDL::ExceptionOr<JS::Value> structured_clone(JS::Realm& realm, JS::Value value)
{
    auto& vm = realm.vm();

    // 1. Let serialized be ? StructuredSerialize(v).
    auto serialized = TRY(HTML::structured_serialize(vm, value));

    // 2. Return ? StructuredDeserialize(serialized, the current Realm).
    return TRY(HTML::structured_deserialize(vm, serialized, realm));
}

// https://streams.spec.whatwg.org/#abstract-opdef-cancopydatablockbytes
bool can_copy_data_block_bytes_buffer(JS::ArrayBuffer const& to_buffer, u64 to_index, JS::ArrayBuffer const& from_buffer, u64 from_index, u64 count)
{
    // 1. Assert: toBuffer is an Object.
    // 2. Assert: toBuffer has an [[ArrayBufferData]] internal slot.
    // 3. Assert: fromBuffer is an Object.
    // 4. Assert: fromBuffer has an [[ArrayBufferData]] internal slot.

    // 5. If toBuffer is fromBuffer, return false.
    if (&to_buffer == &from_buffer)
        return false;

    // 6. If ! IsDetachedBuffer(toBuffer) is true, return false.
    if (to_buffer.is_detached())
        return false;

    // 7. If ! IsDetachedBuffer(fromBuffer) is true, return false.
    if (from_buffer.is_detached())
        return false;

    // 8. If toIndex + count > toBuffer.[[ArrayBufferByteLength]], return false.
    if (to_index + count > to_buffer.byte_length())
        return false;

    // 9. If fromIndex + count > fromBuffer.[[ArrayBufferByteLength]], return false.
    if (from_index + count > from_buffer.byte_length())
        return false;

    // 10. Return true.
    return true;
}

}
