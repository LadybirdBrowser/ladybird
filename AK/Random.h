/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <stdlib.h>

#if defined(__unix__)
#    include <unistd.h>
#endif

namespace AK {

inline void fill_with_random([[maybe_unused]] Bytes bytes)
{
#if defined(AK_OS_SERENITY) || defined(AK_OS_ANDROID) || defined(AK_OS_BSD_GENERIC) || defined(AK_OS_HAIKU) || AK_LIBC_GLIBC_PREREQ(2, 36)
    arc4random_buf(bytes.data(), bytes.size());
#elif defined(OSS_FUZZ)
#else
    auto fill_with_random_fallback = [&]() {
        for (auto& byte : bytes)
            byte = rand();
    };

#    if defined(__unix__)
    // The maximum permitted value for the getentropy length argument.
    static constexpr size_t getentropy_length_limit = 256;
    auto iterations = bytes.size() / getentropy_length_limit;

    for (size_t i = 0; i < iterations; ++i) {
        if (getentropy(bytes.data(), getentropy_length_limit) != 0) {
            fill_with_random_fallback();
            return;
        }

        bytes = bytes.slice(getentropy_length_limit);
    }

    if (bytes.is_empty() || getentropy(bytes.data(), bytes.size()) == 0)
        return;
#    endif

    fill_with_random_fallback();
#endif
}

template<typename T>
inline T get_random()
{
    T t;
    fill_with_random({ &t, sizeof(T) });
    return t;
}

u32 get_random_uniform(u32 max_bounds);
u64 get_random_uniform_64(u64 max_bounds);

template<typename Collection>
inline void shuffle(Collection& collection)
{
    // Fisher-Yates shuffle
    for (size_t i = collection.size() - 1; i >= 1; --i) {
        size_t j = get_random_uniform(i + 1);
        AK::swap(collection[i], collection[j]);
    }
}

}

#if USING_AK_GLOBALLY
using AK::fill_with_random;
using AK::get_random;
using AK::get_random_uniform;
using AK::shuffle;
#endif
