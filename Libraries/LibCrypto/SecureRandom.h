/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>

namespace Crypto {

void fill_with_secure_random(Bytes);

template<typename T>
inline T get_secure_random()
{
    T t;
    fill_with_secure_random({ &t, sizeof(T) });
    return t;
}

// http://vigna.di.unimi.it/ftp/papers/xorshiftplus.pdf
class XorShift128PlusRNG {
public:
    XorShift128PlusRNG();
    double get();

private:
    u64 splitmix64(u64& state);
    u64 advance();
    u64 m_low { 0 };
    u64 m_high { 0 };
};

}
