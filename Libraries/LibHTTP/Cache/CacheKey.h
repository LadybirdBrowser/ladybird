/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteReader.h>
#include <AK/HashFunctions.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Traits.h>

namespace HTTP {

// A memory cache already belongs to a single partition.
struct CacheKey {
    String url;
    String method;

    bool operator==(CacheKey const&) const = default;
};

// A SHA-256 digest. It is not truncated, so that pages cannot construct colliding requests.
struct CacheDigest {
    static Optional<CacheDigest> from_bytes(ReadonlyBytes bytes)
    {
        CacheDigest digest;
        if (bytes.size() != digest.bytes.size())
            return {};
        bytes.copy_to(digest.bytes);
        return digest;
    }

    Array<u8, 32> bytes {};

    bool operator==(CacheDigest const&) const = default;
};

}

namespace AK {

template<>
struct Traits<HTTP::CacheKey> : DefaultTraits<HTTP::CacheKey> {
    static unsigned hash(HTTP::CacheKey const& key) { return pair_int_hash(key.url.hash(), key.method.hash()); }
};

template<>
struct Traits<HTTP::CacheDigest> : DefaultTraits<HTTP::CacheDigest> {
    static unsigned hash(HTTP::CacheDigest const& digest) { return ByteReader::load32(digest.bytes.data()); }
};

}
