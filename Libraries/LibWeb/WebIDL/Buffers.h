/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::WebIDL {

using ArrayBufferViewVariant = Variant<
    GC::Ref<JS::Int8Array>,
    GC::Ref<JS::Int16Array>,
    GC::Ref<JS::Int32Array>,
    GC::Ref<JS::Uint8Array>,
    GC::Ref<JS::Uint16Array>,
    GC::Ref<JS::Uint32Array>,
    GC::Ref<JS::Uint8ClampedArray>,
    GC::Ref<JS::BigInt64Array>,
    GC::Ref<JS::BigUint64Array>,
    GC::Ref<JS::Float16Array>,
    GC::Ref<JS::Float32Array>,
    GC::Ref<JS::Float64Array>,
    GC::Ref<JS::DataView>>;

using NullableArrayBufferViewVariant = FlattenVariant<ArrayBufferViewVariant, Variant<Empty>>;

using BufferSourceVariant = FlattenVariant<ArrayBufferViewVariant, Variant<GC::Ref<JS::ArrayBuffer>>>;
using NullableBufferSourceVariant = FlattenVariant<BufferSourceVariant, Variant<Empty>>;

class BufferSource {
public:
    static BufferSourceVariant from_object(GC::Ref<JS::Object>);
    static bool is_detached(JS::Value const&);

    BufferSource(BufferSourceVariant const&);
    BufferSource(ArrayBufferViewVariant const&);
    BufferSource(ArrayBufferView const&);

    u32 byte_length() const;
    u32 byte_offset() const;
    u32 element_size() const;

    BufferSourceVariant const& buffer_source() const { return m_buffer_source; }
    BufferSourceVariant& buffer_source() { return m_buffer_source; }

    GC::Ptr<JS::ArrayBuffer> viewed_array_buffer() const;
    GC::Ptr<JS::TypedArrayBase> typed_array_base() const;

    bool is_data_view() const;
    bool is_array_buffer() const;

private:
    BufferSourceVariant m_buffer_source;
};

// https://webidl.spec.whatwg.org/#ArrayBufferView
class ArrayBufferView {
public:
    static ArrayBufferViewVariant from_object(GC::Ref<JS::Object>);

    ArrayBufferView(ArrayBufferViewVariant const&);

    u32 byte_length() const;
    u32 byte_offset() const;
    u32 element_size() const;

    ArrayBufferViewVariant const& array_buffer_view() const { return m_array_buffer_view; }
    ArrayBufferViewVariant& array_buffer_view() { return m_array_buffer_view; }

    GC::Ptr<JS::ArrayBuffer> viewed_array_buffer() const;
    GC::Ptr<JS::TypedArrayBase> typed_array_base() const;

    bool is_data_view() const;

    void write(ReadonlyBytes, u32 starting_offset = 0);

private:
    ArrayBufferViewVariant m_array_buffer_view;
};

}
