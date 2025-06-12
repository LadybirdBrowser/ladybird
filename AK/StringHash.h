/*
 * Copyright (c) 2018-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace AK {

// FIXME: This hashing algorithm isn't well-known and may not be good at all.
//        We can't use SipHash since that depends on runtime parameters,
//        but some string hashes like IPC endpoint magic numbers need to be deterministic.
//        Maybe use a SipHash with a statically-known key?
constexpr u32 string_hash(char const* characters, size_t length, u32 seed = 0)
{
    u32 hash = seed;
    for (size_t i = 0; i < length; ++i) {
        hash += static_cast<u32>(characters[i]);
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += hash << 3;
    hash ^= hash >> 11;
    hash += hash << 15;
    return hash;
}

constexpr u32 case_insensitive_string_hash(char const* characters, size_t length, u32 seed = 0)
{
    // AK/CharacterTypes.h cannot be included from here.
    auto to_lowercase = [](char ch) -> u32 {
        if (ch >= 'A' && ch <= 'Z')
            return static_cast<u32>(ch) + 0x20;
        return static_cast<u32>(ch);
    };

    u32 hash = seed;
    for (size_t i = 0; i < length; ++i) {
        hash += to_lowercase(characters[i]);
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += hash << 3;
    hash ^= hash >> 11;
    hash += hash << 15;
    return hash;
}

}

#if USING_AK_GLOBALLY
using AK::string_hash;
#endif
