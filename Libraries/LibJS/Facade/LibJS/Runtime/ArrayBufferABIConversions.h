/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/PrimitiveStorage.h>
#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/ByteLength.h>

// Conversions between the buffers, byte lengths and storage handles of the facade and those of the Rust runtime's
// embedding ABI. Like LibJS/EmbeddingABIConversions.h, only the facade's own .cpp files may include this header.

namespace JS::EmbeddingABI {

inline JSObject* array_buffer_to_abi(ArrayBuffer const& buffer)
{
    return object_to_abi(buffer);
}

inline ArrayBuffer& array_buffer_from_abi(JSObject* buffer)
{
    VERIFY(buffer);
    return *cell_from_abi<ArrayBuffer>(buffer);
}

// LibGC's C interface packs the index and the generation of a handle into one word, with 0 for no storage.
inline GCPrimitiveStorageHandle primitive_storage_handle_to_abi(GC::PrimitiveStorageHandle handle)
{
    if (!handle.is_valid())
        return GC_PRIMITIVE_STORAGE_NULL_HANDLE;
    return (static_cast<GCPrimitiveStorageHandle>(handle.generation) << GC_PRIMITIVE_STORAGE_HANDLE_GENERATION_SHIFT) | handle.index;
}

inline GC::PrimitiveStorageHandle primitive_storage_handle_from_abi(GCPrimitiveStorageHandle handle)
{
    if (handle == GC_PRIMITIVE_STORAGE_NULL_HANDLE)
        return {};
    return {
        .index = static_cast<u32>(handle),
        .generation = static_cast<u32>(handle >> GC_PRIMITIVE_STORAGE_HANDLE_GENERATION_SHIFT),
    };
}

inline JSByteLength byte_length_to_abi(ByteLength const& byte_length)
{
    if (byte_length.is_auto())
        return { .length = 0, .kind = JS_BYTE_LENGTH_AUTO };
    if (byte_length.is_detached())
        return { .length = 0, .kind = JS_BYTE_LENGTH_DETACHED };
    return { .length = byte_length.length(), .kind = JS_BYTE_LENGTH_LENGTH };
}

inline ByteLength byte_length_from_abi(JSByteLength byte_length)
{
    switch (byte_length.kind) {
    case JS_BYTE_LENGTH_AUTO:
        return ByteLength::auto_();
    case JS_BYTE_LENGTH_DETACHED:
        return ByteLength::detached();
    case JS_BYTE_LENGTH_LENGTH:
        return byte_length.length;
    default:
        VERIFY_NOT_REACHED();
    }
}

inline u8 order_to_abi(ArrayBuffer::Order order)
{
    switch (order) {
    case ArrayBuffer::Order::SeqCst:
        return JS_ARRAY_BUFFER_ORDER_SEQ_CST;
    case ArrayBuffer::Order::Unordered:
        return JS_ARRAY_BUFFER_ORDER_UNORDERED;
    }
    VERIFY_NOT_REACHED();
}

}
