/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/CrossProcessId.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::CrossProcessId const& id)
{
    TRY(encoder.encode(id.namespace_id));
    TRY(encoder.encode(id.local_id));
    return {};
}

template<>
ErrorOr<Web::HTML::CrossProcessId> decode(Decoder& decoder)
{
    return Web::HTML::CrossProcessId {
        .namespace_id = TRY(decoder.decode<u64>()),
        .local_id = TRY(decoder.decode<u64>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::CrossProcessIdAllocator const& allocator)
{
    TRY(encoder.encode(allocator.namespace_id));
    TRY(encoder.encode(allocator.next_local_id));
    return {};
}

template<>
ErrorOr<Web::HTML::CrossProcessIdAllocator> decode(Decoder& decoder)
{
    return Web::HTML::CrossProcessIdAllocator {
        .namespace_id = TRY(decoder.decode<u64>()),
        .next_local_id = TRY(decoder.decode<u64>()),
    };
}

}
