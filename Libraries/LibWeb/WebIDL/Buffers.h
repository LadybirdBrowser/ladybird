/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/CellAllocator.h>

namespace Web::WebIDL {

using BufferableObject = Variant<
    JS::NonnullGCPtr<JS::TypedArrayBase>,
    JS::NonnullGCPtr<JS::DataView>,
    JS::NonnullGCPtr<JS::ArrayBuffer>>;

class BufferableObjectBase : public JS::Cell {
    JS_CELL(BufferableObjectBase, JS::Cell);
    JS_DECLARE_ALLOCATOR(BufferableObjectBase);

public:
    virtual ~BufferableObjectBase() override = default;

    u32 byte_length() const;

    JS::NonnullGCPtr<JS::Object> raw_object();
    JS::NonnullGCPtr<JS::Object const> raw_object() const { return const_cast<BufferableObjectBase&>(*this).raw_object(); }

    JS::GCPtr<JS::ArrayBuffer> viewed_array_buffer();

    BufferableObject const& bufferable_object() const { return m_bufferable_object; }
    BufferableObject& bufferable_object() { return m_bufferable_object; }

protected:
    BufferableObjectBase(JS::NonnullGCPtr<JS::Object>);

    virtual void visit_edges(Visitor&) override;

    bool is_data_view() const;
    bool is_typed_array_base() const;
    bool is_array_buffer() const;

    static BufferableObject bufferable_object_from_raw_object(JS::NonnullGCPtr<JS::Object>);

    BufferableObject m_bufferable_object;
};

// https://webidl.spec.whatwg.org/#ArrayBufferView
//
// typedef (Int8Array or Int16Array or Int32Array or
//          Uint8Array or Uint16Array or Uint32Array or Uint8ClampedArray or
//          BigInt64Array or BigUint64Array or
//          Float32Array or Float64Array or DataView) ArrayBufferView;
class ArrayBufferView : public BufferableObjectBase {
    JS_CELL(ArrayBufferView, BufferableObjectBase);
    JS_DECLARE_ALLOCATOR(ArrayBufferView);

public:
    using BufferableObjectBase::BufferableObjectBase;

    virtual ~ArrayBufferView() override;

    using BufferableObjectBase::is_data_view;
    using BufferableObjectBase::is_typed_array_base;

    u32 byte_offset() const;
};

// https://webidl.spec.whatwg.org/#BufferSource
//
// typedef (ArrayBufferView or ArrayBuffer) BufferSource;
class BufferSource : public BufferableObjectBase {
    JS_CELL(BufferSource, BufferableObjectBase);
    JS_DECLARE_ALLOCATOR(BufferSource);

public:
    using BufferableObjectBase::BufferableObjectBase;

    virtual ~BufferSource() override;

    using BufferableObjectBase::is_array_buffer;
    using BufferableObjectBase::is_data_view;
    using BufferableObjectBase::is_typed_array_base;
};

}
