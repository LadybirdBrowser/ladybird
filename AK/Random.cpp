/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2024, the Ladybird developers.
 * Copyright (c) 2025-2026, Colleirose <criticskate@pm.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <AK/Random.h>
#include <AK/String.h>
#include <AK/UFixedBigInt.h>
#include <AK/UFixedBigIntDivision.h>

#if defined(AK_OS_LINUX)
#    include <sys/random.h>
#endif

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#endif

static inline ErrorOr<void> csprng(void* const buf, size_t size)
{
    // We shouldn't use OpenSSL's RAND_bytes function here, because we want to avoid adding dependencies to AK.
    // Therefore, we will use the best platform-specific CSPRNG.
#if defined(AK_OS_SERENITY) || defined(AK_OS_ANDROID) || defined(AK_OS_BSD_GENERIC) || defined(AK_OS_HAIKU) || AK_LIBC_GLIBC_PREREQ(2, 36)
    // This target also covers MacOS and iOS and they both seem to support arc4random_buf
    arc4random_buf(buf, size);
#elif defined(AK_OS_LINUX)
    unsigned char* out = (unsigned char*)buf;
    while (size > 0u) {
        // EINTR can be handled safely by just trying again. Others are fatal
        // See manual for more details
        int ret = getrandom(out, size, 0);
        if (ret == -1 && errno != EINTR) [[unlikely]]
            return Error::from_errno(errno);

        // If ret > 0 then ret indicates how much was copied
        if (ret > 0) [[likely]] {
            size -= ret;
            out += ret;
        }
    }
#elif defined(AK_OS_WINDOWS)
    // Documented to always return TRUE
    g_system.ProcessPrng((PBYTE)buf, size);
#else
    static_assert(false, "This build target doesn't have a valid CSPRNG interface specified in AK/Random.cpp.");
#endif
    return {};
}

namespace AK {

void fill_with_random(Bytes bytes)
{
    MUST(csprng(bytes.data(), bytes.size()));
}

u32 get_random_uniform(u32 max_bounds)
{
    // If we try to divide all 2**32 numbers into groups of "max_bounds" numbers, we may end up
    // with a group around 2**32-1 that is a bit too small. For this reason, the implementation
    // `arc4random() % max_bounds` would be insufficient. Here we compute the last number of the
    // last "full group". Note that if max_bounds is a divisor of UINT32_MAX,
    // then we end up with UINT32_MAX:
    u32 const max_usable = UINT32_MAX - (static_cast<u64>(UINT32_MAX) + 1) % max_bounds;
    auto random_value = get_random<u32>();
    for (int i = 0; i < 20 && random_value > max_usable; ++i) {
        // By chance we picked a value from the incomplete group. Note that this group has size at
        // most 2**31-1, so picking this group has a chance of less than 50%.
        // In practice, this means that for the worst possible input, there is still only a
        // once-in-a-million chance to get to iteration 20. In theory we should be able to loop
        // forever. Here we prefer marginally imperfect random numbers over weird runtime behavior.
        random_value = get_random<u32>();
    }
    return random_value % max_bounds;
}

u64 get_random_uniform_64(u64 max_bounds)
{
    // Uses the same algorithm as `get_random_uniform`,
    // by replacing u64 with u128 and u32 with u64.
    u64 const max_usable = UINT64_MAX - static_cast<u64>((static_cast<u128>(UINT64_MAX) + 1) % max_bounds);
    auto random_value = get_random<u64>();
    for (int i = 0; i < 20 && random_value > max_usable; ++i) {
        random_value = get_random<u64>();
    }
    return random_value % max_bounds;
}

// https://w3c.github.io/webcrypto/#dfn-generate-a-random-uuid
String generate_random_uuid()
{
    // 1. Let bytes be a byte sequence of length 16.
    u8 bytes[16];

    // 2. Fill bytes with cryptographically secure random bytes.
    fill_with_random(bytes);

    // 3. Set the 4 most significant bits of bytes[6], which represent the UUID version, to 0100.
    bytes[6] = (bytes[6] & 0x0f) | 0x40;

    // 4. Set the 2 most significant bits of bytes[8], which represent the UUID variant, to 10.
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    // 5. Return the string concatenation of «
    //     hexadecimal representation of bytes[0],
    //     hexadecimal representation of bytes[1],
    //     hexadecimal representation of bytes[2],
    //     hexadecimal representation of bytes[3],
    //     "-",
    //     hexadecimal representation of bytes[4],
    //     hexadecimal representation of bytes[5],
    //     "-",
    //     hexadecimal representation of bytes[6],
    //     hexadecimal representation of bytes[7],
    //     "-",
    //     hexadecimal representation of bytes[8],
    //     hexadecimal representation of bytes[9],
    //     "-",
    //     hexadecimal representation of bytes[10],
    //     hexadecimal representation of bytes[11],
    //     hexadecimal representation of bytes[12],
    //     hexadecimal representation of bytes[13],
    //     hexadecimal representation of bytes[14],
    //     hexadecimal representation of bytes[15]
    // ».
    return MUST(String::formatted(
        "{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5],
        bytes[6], bytes[7],
        bytes[8], bytes[9],
        bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]));
}

XorShift128PlusRNG::XorShift128PlusRNG()
{
    // Splitmix64 is used as xorshift is sensitive to being seeded with all 0s
    u64 seed = get_random<u64>();
    m_low = splitmix64(seed);
    seed = get_random<u64>();
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
