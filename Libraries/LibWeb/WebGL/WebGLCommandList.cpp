/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibWeb/WebGL/WebGLCommandList.h>

namespace Web::WebGL {

static size_t payload_layout_size(ReadonlyBytes payload, ReadonlyBytes inline_data, ReadonlyBytes more_inline_data = {})
{
    auto size = payload.size();
    if (!inline_data.is_empty())
        size = align_up_to(size, WebGLCommandList::command_alignment) + inline_data.size();
    if (!more_inline_data.is_empty())
        size = align_up_to(size, WebGLCommandList::command_alignment) + more_inline_data.size();
    return size;
}

static void write_payload(Bytes destination, ReadonlyBytes payload, ReadonlyBytes inline_data, ReadonlyBytes more_inline_data = {})
{
    __builtin_memcpy(destination.data(), payload.data(), payload.size());
    auto cursor = payload.size();
    for (auto blob : { inline_data, more_inline_data }) {
        if (blob.is_empty())
            continue;
        auto offset = align_up_to(cursor, WebGLCommandList::command_alignment);
        __builtin_memset(destination.offset_pointer(cursor), 0, offset - cursor);
        __builtin_memcpy(destination.offset_pointer(offset), blob.data(), blob.size());
        cursor = offset + blob.size();
    }
    __builtin_memset(destination.offset_pointer(cursor), 0, destination.size() - cursor);
}

void WebGLCommandList::append_bytes(WebGLCommandType type, ReadonlyBytes payload, ReadonlyBytes inline_data)
{
    VERIFY(m_bytes.size() % command_alignment == 0);

    auto record_size = sizeof(WebGLCommandHeader) + payload_layout_size(payload, inline_data);
    auto padded_record_size = align_up_to(record_size, command_alignment);
    auto padded_payload_size = padded_record_size - sizeof(WebGLCommandHeader);
    VERIFY(padded_payload_size <= NumericLimits<u32>::max());

    WebGLCommandHeader header {
        .type = type,
        .payload_size = static_cast<u32>(padded_payload_size),
    };

    auto record_offset = m_bytes.size();
    m_bytes.resize(record_offset + padded_record_size);
    auto record = m_bytes.bytes().slice(record_offset);
    __builtin_memcpy(record.data(), &header, sizeof(header));
    write_payload(record.slice(sizeof(header)), payload, inline_data);
}

ByteBuffer WebGLSyncCall::encode_request_bytes(WebGLSyncCallType type, ReadonlyBytes request, ReadonlyBytes inline_data)
{
    auto padded_payload_size = align_up_to(payload_layout_size(request, inline_data), WebGLCommandList::command_alignment);
    VERIFY(padded_payload_size <= NumericLimits<u32>::max());

    WebGLSyncCallHeader header {
        .type = type,
        .payload_size = static_cast<u32>(padded_payload_size),
    };

    auto bytes = MUST(ByteBuffer::create_uninitialized(sizeof(header) + padded_payload_size));
    __builtin_memcpy(bytes.data(), &header, sizeof(header));
    write_payload(bytes.bytes().slice(sizeof(header)), request, inline_data);
    return bytes;
}

ByteBuffer WebGLSyncCall::encode_reply_bytes(ReadonlyBytes reply, ReadonlyBytes inline_data, ReadonlyBytes more_inline_data)
{
    VERIFY(more_inline_data.is_empty() || !inline_data.is_empty());

    auto reply_size = align_up_to(payload_layout_size(reply, inline_data, more_inline_data), WebGLCommandList::command_alignment);
    auto bytes = MUST(ByteBuffer::create_uninitialized(reply_size));
    write_payload(bytes, reply, inline_data, more_inline_data);
    return bytes;
}

}
