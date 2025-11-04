/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/SecureRandom.h>

#include <openssl/rand.h>

namespace Crypto {

void fill_with_secure_random(Bytes bytes)
{
    auto const size = static_cast<int>(bytes.size());

    if (RAND_bytes(bytes.data(), size) != 1)
        VERIFY_NOT_REACHED();
}

XorShift128PlusRNG::XorShift128PlusRNG()
{
    // Splitmix64 is used as xorshift is sensitive to being seeded with all 0s
    u64 seed = Crypto::get_secure_random<u64>();
    m_low = splitmix64(seed);
    seed = Crypto::get_secure_random<u64>();
    m_high = splitmix64(seed);
}

double XorShift128PlusRNG::get()
{
    u64 value = advance() & ((1ULL << 53) - 1);
    return value * (1.0 / (1ULL << 53));
}

u64 XorShift128PlusRNG::splitmix64(u64& state)
{
    u64 z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// Apparently this set of constants is better: https://stackoverflow.com/a/34432126
u64 XorShift128PlusRNG::advance()
{
    u64 s1 = m_low;
    u64 const s0 = m_high;
    u64 const result = s0 + s1;
    m_low = s0;
    s1 ^= s1 << 23;
    s1 ^= s1 >> 18;
    s1 ^= s0 ^ (s0 >> 5);
    m_high = s1;
    return result + s1;
}

}
