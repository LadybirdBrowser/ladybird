/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibHTTP/Forward.h>
#include <LibIPC/Forward.h>

namespace HTTP {

struct DiskCacheSettings {
    u64 maximum_size { DEFAULT_MAXIMUM_DISK_CACHE_SIZE };
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, HTTP::DiskCacheSettings const&);

template<>
ErrorOr<HTTP::DiskCacheSettings> decode(Decoder&);

}
