/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/WebGL/WebGLCommandList.h>

namespace Compositing::WebGL {

static size_t payload_layout_size(ReadonlyBytes payload, ReadonlyBytes inline_data, ReadonlyBytes more_inline_data)
{
    auto size = payload.size();
    if (!inline_data.is_empty())
        size = align_up_to(size, WebGLCommandList::command_alignment) + inline_data.size();
    if (!more_inline_data.is_empty())
        size = align_up_to(size, WebGLCommandList::command_alignment) + more_inline_data.size();
    return size;
}

static void write_payload(Bytes destination, ReadonlyBytes payload, ReadonlyBytes inline_data, ReadonlyBytes more_inline_data)
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

ByteBuffer WebGLSyncCall::encode_reply_bytes(ReadonlyBytes reply, ReadonlyBytes inline_data, ReadonlyBytes more_inline_data)
{
    VERIFY(more_inline_data.is_empty() || !inline_data.is_empty());

    auto reply_size = align_up_to(payload_layout_size(reply, inline_data, more_inline_data), WebGLCommandList::command_alignment);
    auto bytes = MUST(ByteBuffer::create_uninitialized(reply_size));
    write_payload(bytes, reply, inline_data, more_inline_data);
    return bytes;
}

}
