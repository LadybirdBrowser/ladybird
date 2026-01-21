/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace HTTP {

enum class CacheMode : u8 {
    // The cache is searched for a matching response. The cache is updated with the server's response.
    Default,

    // The cache is not searched for a matching response. The cache is not updated with the server's response.
    NoStore,

    // The cache is not searched for a matching response. The cache is updated with the server's response.
    Reload,

    // The cache is searched for a matching response, but must always revalidate. The cache is updated with the server's response.
    NoCache,

    // The cache is searched for a matching fresh or stale response. The cache is updated with the server's response.
    ForceCache,

    // The cache is searched for a matching fresh or stale response. A cache miss results in a network error.
    OnlyIfCached,
};

}
