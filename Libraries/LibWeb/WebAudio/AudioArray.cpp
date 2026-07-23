/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebAudio/AudioArray.h>

namespace Web::WebAudio {

// The current element length of a Float32Array, or 0 if it is out of bounds of its backing ArrayBuffer.
size_t float32_array_length(JS::Float32Array const& array)
{
    auto record = JS::make_typed_array_with_buffer_witness_record(array, JS::ArrayBuffer::Order::SeqCst);
    if (JS::is_typed_array_out_of_bounds(record))
        return 0;
    return JS::typed_array_length(record);
}

// Snapshots the live contents of a Float32Array into an owned buffer.
Vector<float> copy_float32_array(JS::Float32Array const& array)
{
    auto length = float32_array_length(array);
    Vector<float> values;
    values.resize(length);
    array.viewed_array_buffer()->copy_to(array.byte_offset(), { reinterpret_cast<u8*>(values.data()), length * sizeof(float) });
    return values;
}

// Overwrites the leading elements of a Float32Array with the given samples.
void overwrite_float32_array(JS::Float32Array& array, ReadonlySpan<float> values)
{
    array.viewed_array_buffer()->overwrite(array.byte_offset(), values.data(), values.size() * sizeof(float));
}

}
