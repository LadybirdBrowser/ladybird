/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibIPC/Forward.h>

namespace Requests {

struct CacheSizes {
    u64 since_requested_time { 0 };
    u64 total { 0 };
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Requests::CacheSizes const&);

template<>
ErrorOr<Requests::CacheSizes> decode(Decoder&);

}
