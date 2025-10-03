/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/ByteLength.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class JS_API DataView : public Object {
    JS_OBJECT(DataView, Object);
    GC_DECLARE_ALLOCATOR(DataView);

public:
    static GC::Ref<DataView> create(Realm&, GC::Ref<ArrayBuffer>, ByteLength byte_length, size_t byte_offset);

    virtual ~DataView() override = default;

    GC::Ref<ArrayBuffer> viewed_array_buffer() const { return m_viewed_array_buffer; }
    ByteLength const& byte_length() const { return m_byte_length; }
    u32 byte_offset() const { return m_byte_offset; }

private:
    DataView(GC::Ref<ArrayBuffer>, ByteLength byte_length, size_t byte_offset, Object& prototype);

    virtual void visit_edges(Visitor& visitor) override;

    GC::Ref<ArrayBuffer> m_viewed_array_buffer;
    ByteLength m_byte_length { 0 };
    size_t m_byte_offset { 0 };
};

// 25.3.1.1 DataView With Buffer Witness Records, https://tc39.es/ecma262/#sec-dataview-with-buffer-witness-records
struct DataViewWithBufferWitness {
    GC::Ref<DataView const> object;       // [[Object]]
    ByteLength cached_buffer_byte_length; // [[CachedBufferByteLength]]
};

JS_API DataViewWithBufferWitness make_data_view_with_buffer_witness_record(DataView const&, ArrayBuffer::Order);
JS_API u32 get_view_byte_length(DataViewWithBufferWitness const&);
JS_API bool is_view_out_of_bounds(DataViewWithBufferWitness const&);

}
