/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/Cache/DiskCacheSettings.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, HTTP::DiskCacheSettings const& sizes)
{
    TRY(encoder.encode(sizes.maximum_size));

    return {};
}

template<>
ErrorOr<HTTP::DiskCacheSettings> decode(Decoder& decoder)
{
    auto maximum_size = TRY(decoder.decode<u64>());

    return HTTP::DiskCacheSettings { maximum_size };
}

}
