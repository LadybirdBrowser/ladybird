/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashFunctions.h>
#include <AK/String.h>
#include <AK/Traits.h>

namespace HTTP {

// A memory cache already belongs to a single partition.
struct CacheKey {
    String url;
    String method;

    bool operator==(CacheKey const&) const = default;
};

}

namespace AK {

template<>
struct Traits<HTTP::CacheKey> : DefaultTraits<HTTP::CacheKey> {
    static unsigned hash(HTTP::CacheKey const& key) { return pair_int_hash(key.url.hash(), key.method.hash()); }
};

}
