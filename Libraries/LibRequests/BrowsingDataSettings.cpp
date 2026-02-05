/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibRequests/BrowsingDataSettings.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Requests::BrowsingDataSettings const& sizes)
{
    TRY(encoder.encode(sizes.maximum_disk_cache_size));

    return {};
}

template<>
ErrorOr<Requests::BrowsingDataSettings> decode(Decoder& decoder)
{
    auto maximum_disk_cache_size = TRY(decoder.decode<u64>());

    return Requests::BrowsingDataSettings { maximum_disk_cache_size };
}

}
