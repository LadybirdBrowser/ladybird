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
public:
    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_DATA_VIEW; }

    static GC::Ref<DataView> create(Realm&, ArrayBuffer*, ByteLength byte_length, size_t byte_offset);

    ArrayBuffer* viewed_array_buffer() const;

    // The runtime keeps [[ByteLength]] where only it reads it, so this returns a copy.
    ByteLength byte_length() const;

    u32 byte_offset() const;
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
