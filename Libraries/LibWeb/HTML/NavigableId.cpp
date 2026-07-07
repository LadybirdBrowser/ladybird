/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/NavigableId.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NavigableId const& id)
{
    TRY(encoder.encode(id.namespace_id));
    TRY(encoder.encode(id.local_id));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigableId> decode(Decoder& decoder)
{
    return Web::HTML::NavigableId {
        .namespace_id = TRY(decoder.decode<u64>()),
        .local_id = TRY(decoder.decode<u64>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::NavigableIdAllocator const& allocator)
{
    TRY(encoder.encode(allocator.namespace_id));
    TRY(encoder.encode(allocator.next_local_id));
    return {};
}

template<>
ErrorOr<Web::HTML::NavigableIdAllocator> decode(Decoder& decoder)
{
    return Web::HTML::NavigableIdAllocator {
        .namespace_id = TRY(decoder.decode<u64>()),
        .next_local_id = TRY(decoder.decode<u64>()),
    };
}

}
