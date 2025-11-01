/*
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2024, the Ladybird developers.
 * Copyright (c) 2025, Colleirose <criticskate@pm.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Platform.h>
#include <AK/Random.h>
#include <AK/UFixedBigInt.h>
#include <AK/UFixedBigIntDivision.h>

#if defined(AK_OS_LINUX)
#    include <sys/random.h>
#endif

#if defined(AK_OS_WINDOWS)
#    include <AK/NumericLimits.h>
#    include <AK/Windows.h>
#    include <bcrypt.h>
#    include <ntstatus.h>
#    include <winternl.h>
#endif

static inline ErrorOr<void> csprng(void* const buf, size_t size)
{
#if defined(AK_OS_SERENITY) || defined(AK_OS_ANDROID) || defined(AK_OS_BSD_GENERIC) || defined(AK_OS_HAIKU) || AK_LIBC_GLIBC_PREREQ(2, 36)
    arc4random_buf(buf, size);
#elif defined(AK_OS_LINUX)
    int ret;
    while (size > 0u) {
        // The possible errors that can be returned here are:
        //
        // EINTR, interrupted by a signal handler.
        // EAGAIN, the requested entropy wasn't available. Only possible if the GRND_NONBLOCK flag is set, which it isn't.
        // EINVAL, invalid flags provided, which there aren't.
        // ENOSYS, the kernel doesn't implement the syscall. The syscall has been available since Linux 3.17, which was released in 2019.
        // EFAULT, the address is outside the accessible space; the caller is responsible for providing a valid address.
        //
        // Therefore, the only somewhat plausible errors are EFAULT and EINTR.
        // Nonetheless, we will check for the possibility of all the error codes out of an abundance of caution.
        ret = getrandom(buf, size, 0);

        if (ret == -1 && errno != EINTR) [[unlikely]]
            return Error::from_errno(errno);

        // Per the manual:
        //
        // > On success, getrandom() returns the number of bytes that were
        // > copied to the buffer buf.  This may be less than the number of
        // > bytes requested via buflen if either GRND_RANDOM was specified in
        // > flags and insufficient entropy was present in the random source or
        // > the system call was interrupted by a signal.
        //
        // > The user of getrandom() *must* always check the return value, to
        // > determine whether either an error occurred or fewer bytes than
        // > requested were returned.
        if (ret > 0) [[likely]] {
            size -= ret;
            buf += ret;
        }
    }
#elif defined(AK_OS_WINDOWS)
    if (size > NumericLimits<u32>::max()) [[unlikely]]
        return Error::from_string_literal("The input is too large");

    unsigned char* ptr = reinterpret_cast<unsigned char*>(const_cast<void*>(buf));
    NTSTATUS status = ::BCryptGenRandom(BCRYPT_RNG_ALG_HANDLE, ptr, size, 0);
    if (!NT_SUCCESS(status)) [[unlikely]] {
        ULONG error = RtlNtStatusToDosError(status);
        return Error::from_windows_error(error);
    }
#else
    // There shouldn't be a build target where this can happen.
    //
    // As for MacOS and iOS, they appear to both support arc4random_buf, and AK/Platform.h defines AK_OS_BSD_GENERIC when building for them,
    // so they will be covered by the compile target that uses arc4random_buf.
    //
    // Note that although [SecRandomCopyBytes](https://developer.apple.com/documentation/security/secrandomcopybytes(_:_:_:)) appears to be more commonly used on these platforms,
    // it doesn't seem to be required.
    static_assert(false, "This build target doesn't have a valid CSPRNG interface specified in AK/Random.cpp. This needs to be fixed before you can build for this target.");
#endif
    return {};
}

namespace AK {

void crypto_randombytes_buf(Bytes bytes)
{
    if (csprng(bytes.data(), bytes.size()).is_error())
        VERIFY_NOT_REACHED();
}

u32 crypto_random_uniform(u32 max_bounds)
{
    // If we try to divide all 2**32 numbers into groups of "max_bounds" numbers, we may end up
    // with a group around 2**32-1 that is a bit too small. For this reason, the implementation
    // `arc4random() % max_bounds` would be insufficient. Here we compute the last number of the
    // last "full group". Note that if max_bounds is a divisor of UINT32_MAX,
    // then we end up with UINT32_MAX:
    u32 const max_usable = UINT32_MAX - (static_cast<u64>(UINT32_MAX) + 1) % max_bounds;
    auto random_value = crypto_random<u32>();
    for (int i = 0; i < 20 && random_value > max_usable; ++i) {
        // By chance we picked a value from the incomplete group. Note that this group has size at
        // most 2**31-1, so picking this group has a chance of less than 50%.
        // In practice, this means that for the worst possible input, there is still only a
        // once-in-a-million chance to get to iteration 20. In theory we should be able to loop
        // forever. Here we prefer marginally imperfect random numbers over weird runtime behavior.
        random_value = crypto_random<u32>();
    }
    return random_value % max_bounds;
}

u64 crypto_random_uniform_64(u64 max_bounds)
{
    // Uses the same algorithm as `crypto_random_uniform`,
    // by replacing u64 with u128 and u32 with u64.
    u64 const max_usable = UINT64_MAX - static_cast<u64>((static_cast<u128>(UINT64_MAX) + 1) % max_bounds);
    auto random_value = crypto_random<u64>();
    for (int i = 0; i < 20 && random_value > max_usable; ++i) {
        random_value = crypto_random<u64>();
    }
    return random_value % max_bounds;
}

}
