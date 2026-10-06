/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBufferABIConversions.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<DataView> DataView::create(Realm& realm, ArrayBuffer* viewed_buffer, ByteLength byte_length, size_t byte_offset)
{
    VERIFY(viewed_buffer);
    auto* data_view = js_array_buffer_create_data_view(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), array_buffer_to_abi(*viewed_buffer), byte_length_to_abi(byte_length), byte_offset);
    VERIFY(data_view);
    return cell_ref_from_abi<DataView>(data_view);
}

ArrayBuffer* DataView::viewed_array_buffer() const
{
    return &array_buffer_from_abi(js_array_buffer_data_view_viewed_buffer(object_to_abi(*this)));
}

ByteLength DataView::byte_length() const
{
    return byte_length_from_abi(js_array_buffer_data_view_byte_length(object_to_abi(*this)));
}

u32 DataView::byte_offset() const
{
    return js_array_buffer_data_view_byte_offset(object_to_abi(*this));
}

static JSDataViewWithBufferWitness witness_record_to_abi(DataViewWithBufferWitness const& record)
{
    return {
        .data_view = object_to_abi(*record.object),
        .cached_buffer_byte_length = byte_length_to_abi(record.cached_buffer_byte_length),
    };
}

DataViewWithBufferWitness make_data_view_with_buffer_witness_record(DataView const& data_view, ArrayBuffer::Order order)
{
    auto record = js_array_buffer_make_data_view_witness_record(object_to_abi(data_view), order_to_abi(order));
    return {
        .object = cell_ref_from_abi<DataView>(record.data_view),
        .cached_buffer_byte_length = byte_length_from_abi(record.cached_buffer_byte_length),
    };
}

u32 get_view_byte_length(DataViewWithBufferWitness const& view_record)
{
    auto record = witness_record_to_abi(view_record);
    return js_array_buffer_data_view_view_byte_length(&record);
}

bool is_view_out_of_bounds(DataViewWithBufferWitness const& view_record)
{
    auto record = witness_record_to_abi(view_record);
    return js_array_buffer_is_data_view_out_of_bounds(&record);
}

}
