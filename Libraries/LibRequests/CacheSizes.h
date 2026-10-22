/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <AK/Types.h>
#include <LibIPC/Forward.h>

namespace Requests {

struct CacheSizes {
    u64 since_requested_time { 0 };
    u64 total { 0 };
};

// Total bytes of entries last accessed in the time bucket starting at last_access_time.
struct CacheAccessBucket {
    UnixDateTime last_access_time;
    u64 size { 0 };
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Requests::CacheSizes const&);

template<>
ErrorOr<Requests::CacheSizes> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Requests::CacheAccessBucket const&);

template<>
ErrorOr<Requests::CacheAccessBucket> decode(Decoder&);

}
