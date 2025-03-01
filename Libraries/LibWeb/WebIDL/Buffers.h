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

namespace Web::WebIDL {

bool is_buffer_source_detached(JS::Value const&);

using BufferableObject = Variant<
    GC::Ref<JS::TypedArrayBase>,
    GC::Ref<JS::DataView>,
    GC::Ref<JS::ArrayBuffer>>;

class BufferableObjectBase : public JS::Cell {
    GC_CELL(BufferableObjectBase, JS::Cell);
    GC_DECLARE_ALLOCATOR(BufferableObjectBase);

public:
    virtual ~BufferableObjectBase() override = default;

    u32 byte_length() const;
    u32 byte_offset() const;
    u32 element_size() const;

    GC::Ref<JS::Object> raw_object();
    GC::Ref<JS::Object const> raw_object() const { return const_cast<BufferableObjectBase&>(*this).raw_object(); }

    GC::Ptr<JS::ArrayBuffer> viewed_array_buffer();

    BufferableObject const& bufferable_object() const { return m_bufferable_object; }
    BufferableObject& bufferable_object() { return m_bufferable_object; }

protected:
    BufferableObjectBase(GC::Ref<JS::Object>);

    virtual void visit_edges(Visitor&) override;

    bool is_data_view() const;
    bool is_typed_array_base() const;
    bool is_array_buffer() const;

    static BufferableObject bufferable_object_from_raw_object(GC::Ref<JS::Object>);

    BufferableObject m_bufferable_object;
};

// https://webidl.spec.whatwg.org/#ArrayBufferView
//
// typedef (Int8Array or Int16Array or Int32Array or
//          Uint8Array or Uint16Array or Uint32Array or Uint8ClampedArray or
//          BigInt64Array or BigUint64Array or
//          Float32Array or Float64Array or DataView) ArrayBufferView;
class ArrayBufferView : public BufferableObjectBase {
    GC_CELL(ArrayBufferView, BufferableObjectBase);
    GC_DECLARE_ALLOCATOR(ArrayBufferView);

public:
    using BufferableObjectBase::BufferableObjectBase;

    virtual ~ArrayBufferView() override;

    using BufferableObjectBase::is_data_view;
    using BufferableObjectBase::is_typed_array_base;

    void write(ReadonlyBytes, u32 starting_offset = 0);
};

// https://webidl.spec.whatwg.org/#BufferSource
//
// typedef (ArrayBufferView or ArrayBuffer) BufferSource;
class BufferSource : public BufferableObjectBase {
    GC_CELL(BufferSource, BufferableObjectBase);
    GC_DECLARE_ALLOCATOR(BufferSource);

public:
    using BufferableObjectBase::BufferableObjectBase;

    virtual ~BufferSource() override;

    using BufferableObjectBase::is_array_buffer;
    using BufferableObjectBase::is_data_view;
    using BufferableObjectBase::is_typed_array_base;
};

}
