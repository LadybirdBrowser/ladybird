/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibRequests/CacheSizes.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Requests::CacheSizes const& sizes)
{
    TRY(encoder.encode(sizes.since_requested_time));
    TRY(encoder.encode(sizes.total));

    return {};
}

template<>
ErrorOr<Requests::CacheSizes> decode(Decoder& decoder)
{
    auto since_requested_time = TRY(decoder.decode<u64>());
    auto total = TRY(decoder.decode<u64>());

    return Requests::CacheSizes { since_requested_time, total };
}

}
