/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <AK/Random.h>
#include <AK/UFixedBigInt.h>
#include <AK/UFixedBigIntDivision.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/NumericLimits.h>
#    include <AK/Windows.h>
#    include <bcrypt.h>
#    include <ntstatus.h>
#endif

namespace AK {

// NOTE: This function is supposed to always give a random number. If possible it is of good quality, but it can fall
//       back to rand() if it fails on some systems. For high speed you should probably use a different generator.
//       See MathObject::random() from LibJS. Where cryptographic security is needed use LibCrypto/SecureRandom.h.
void fill_with_random([[maybe_unused]] Bytes bytes)
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
#    elif defined(AK_OS_WINDOWS)

    if (bytes.size() > NumericLimits<u32>::max()) [[unlikely]] {
        fill_with_random_fallback();
        return;
    }

    // NOTE: This is more secure than needed. But on modern hardware it be should more than fast enough.
    NTSTATUS result = ::BCryptGenRandom(NULL, bytes.data(), bytes.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (result == STATUS_SUCCESS)
        return;
#    endif

    fill_with_random_fallback();
#endif
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

}
