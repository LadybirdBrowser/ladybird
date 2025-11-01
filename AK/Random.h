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

void crypto_randombytes_buf([[maybe_unused]] Bytes bytes);

template<typename T>
inline T crypto_random()
{
    T t;
    crypto_randombytes_buf({ &t, sizeof(T) });
    return t;
}

u32 crypto_random_uniform(u32 max_bounds);
u64 crypto_random_uniform_64(u64 max_bounds);

template<typename Collection>
inline void shuffle(Collection& collection)
{
    // Fisher-Yates shuffle
    for (size_t i = collection.size() - 1; i >= 1; --i) {
        size_t j = crypto_random_uniform(i + 1);
        AK::swap(collection[i], collection[j]);
    }
}

}

#if USING_AK_GLOBALLY
using AK::crypto_random;
using AK::crypto_random_uniform;
using AK::crypto_randombytes_buf;
using AK::shuffle;
#endif
