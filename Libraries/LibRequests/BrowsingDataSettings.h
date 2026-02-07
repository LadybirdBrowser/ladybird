/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibIPC/Forward.h>

namespace Requests {

struct BrowsingDataSettings {
    u64 maximum_disk_cache_size { HTTP::DEFAULT_MAXIMUM_DISK_CACHE_SIZE };
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Requests::BrowsingDataSettings const&);

template<>
ErrorOr<Requests::BrowsingDataSettings> decode(Decoder&);

}
