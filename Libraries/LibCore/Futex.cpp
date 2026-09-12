/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <AK/Format.h>
#include <AK/NumericLimits.h>
#include <AK/Platform.h>
#include <AK/Time.h>
#include <LibCore/Futex.h>

#include <errno.h>

#if defined(AK_OS_MACOS)
#    include <os/os_sync_wait_on_address.h>
#    include <unistd.h>
#elif defined(AK_OS_LINUX)
#    include <linux/futex.h>
#    include <sys/syscall.h>
#    include <time.h>
#    include <unistd.h>
#elif !defined(AK_OS_WINDOWS)
#    include <unistd.h>
#endif

#ifdef AK_OS_WINDOWS
#    include <AK/Windows.h>
#endif

namespace Core {

static u64 read_word(void* address, size_t size)
{
    if (size >= 8)
        return __atomic_load_n(reinterpret_cast<u64 volatile*>(address), __ATOMIC_SEQ_CST);
    return __atomic_load_n(reinterpret_cast<u32 volatile*>(address), __ATOMIC_SEQ_CST);
}

// Whether the word at address currently equals expected, masked to the wait size (Atomics.wait uses 4-byte or 8-byte
// words). A sequentially-consistent read, matching the memory model's read for the wait comparison.
static bool word_equals(void* address, u64 expected, size_t size)
{
    auto mask = size >= 8 ? NumericLimits<u64>::max() : NumericLimits<u32>::max();
    return (read_word(address, size) & mask) == (expected & mask);
}

// Fallback where there's no cross-process wait primitive: Poll for a change to the word. That observes value changes
// rather than notifications — which matches the lock/futex protocol Emscripten uses (the waker writes the word before
// notifying). The gap is a notify that leaves the word unchanged: It wakes no poll-waiter, and the paired atomic_notify
// reports zero woken. An untimed wait whose only wake would be such a notify therefore never returns. Closing that gap
// would take a waiter registry in shared memory — at the cost a lot of extra machinery most modern platforms would
// never use, but instead just for the benefit of limited number of platforms/legacy environments. Not a good tradeoff.
static AtomicWaitResult poll_wait(void* address, u64 expected, size_t size, Optional<AK::Duration> timeout)
{
    if (!word_equals(address, expected, size))
        return AtomicWaitResult::NotEqual;

    auto start = MonotonicTime::now();
    for (;;) {
#if defined(AK_OS_WINDOWS)
        // Sleep takes whole milliseconds; 1 is its shortest non-zero wait.
        Sleep(1);
#else
        usleep(200);
#endif
        if (!word_equals(address, expected, size))
            return AtomicWaitResult::Woken;
        if (timeout.has_value() && (MonotonicTime::now() - start) >= *timeout)
            return AtomicWaitResult::TimedOut;
    }
}

#if defined(AK_OS_MACOS)

AtomicWaitResult atomic_wait(void* address, u64 expected, size_t size, Optional<AK::Duration> timeout)
{
    // Compare the value in userspace first. os_sync_wait_on_address reports a value mismatch as a *successful* return
    // (it has no EAGAIN). So, without this, the "not-equal" result would be unreachable; it also faults the shared page
    // in — avoiding a transient first-touch EFAULT below. A store landing between this read and the kernel's own
    // comparison is reported as a wake, rather than "not-equal" — a nanoseconds-wide window inherent to the os_sync API
    // (Linux's kernel futex and the poll fallback don't have it).
    if (!word_equals(address, expected, size))
        return AtomicWaitResult::NotEqual;

    // Anchor the timeout to a fixed deadline — so that nothing restarts it. Both an EINTR retry and the fallback to
    // polling below continue with only the time that's left.
    Optional<MonotonicTime> deadline;
    if (timeout.has_value())
        deadline = MonotonicTime::now() + *timeout;

    if (__builtin_available(macOS 14.4, *)) {
        // os_sync_wait_on_address makes the element size part of the wait/wake key. Wait on the low 32-bit half (size
        // 4) for both 4-byte and 8-byte waits: the full-width userspace pre-compare above already rejected any full-
        // value mismatch, so the low half only has to close the lost-wake race at entry — and normalizing the size lets
        // 4-byte and 8-byte waiters at one address share kernel state (a single spec WaiterList) and cross-wake.
        // Passing the real size instead makes os_sync reject a mixed-size wait/notify pair with EINVAL. On little-
        // endian, the low half is the word at 'address' (byte offset 0).
        static_assert(AK::HostIsLittleEndian, "8-byte os_sync waits assume the low 32-bit half is at byte offset 0");

        for (;;) {
            int result;
            if (deadline.has_value()) {
                auto remaining = (*deadline - MonotonicTime::now()).to_nanoseconds();
                if (remaining <= 0)
                    return AtomicWaitResult::TimedOut;
                result = os_sync_wait_on_address_with_timeout(address, static_cast<u32>(expected), 4, OS_SYNC_WAIT_ON_ADDRESS_SHARED, OS_CLOCK_MACH_ABSOLUTE_TIME, static_cast<uint64_t>(remaining));
            } else {
                result = os_sync_wait_on_address(address, static_cast<u32>(expected), 4, OS_SYNC_WAIT_ON_ADDRESS_SHARED);
            }

            if (result >= 0)
                return AtomicWaitResult::Woken;
            if (errno == EINTR) {
                // os_sync reports a value mismatch as a successful wake — so retrying with the original expected would
                // turn a store that landed while we slept into a spurious "ok". Re-read first: If the word changed,
                // report "not-equal" (as if the critical section were entered late) — matching the Linux EAGAIN path.
                if (!word_equals(address, expected, size))
                    return AtomicWaitResult::NotEqual;
                continue;
            }
            if (errno == ETIMEDOUT)
                return AtomicWaitResult::TimedOut;
            if (errno == EAGAIN)
                return AtomicWaitResult::NotEqual;
            // os_sync couldn't key this memory (rare once faulted in above); fall back to polling for a change.
            break;
        }
    }
    if (deadline.has_value()) {
        auto remaining = *deadline - MonotonicTime::now();
        return poll_wait(address, expected, size, remaining > AK::Duration::zero() ? remaining : AK::Duration::zero());
    }
    return poll_wait(address, expected, size, timeout);
}

size_t atomic_notify(void* address, size_t size, size_t max_count)
{
    // Waiters key on the low 32-bit half — so, wake with size 4 regardless of the notifying view's element size. That
    // lets a 4-byte notify wake an 8-byte waiter (and vice versa) at the same address — which the spec's single per-
    // address WaiterList requires. Passing the view's own size would make os_sync reject a mixed-size wake with EINVAL.
    (void)size;
    if (__builtin_available(macOS 14.4, *)) {
        // os_sync wakes a single waiter at a time. So, wake up to max_count of them — stopping once there are none
        // left. ENOENT is that stop: it means the wait set is empty. Every other error is a real failure: EINVAL for a
        // size mismatch or a flag mismatch against the waiters, EFAULT for an address the kernel can't read. So, report
        // those — rather than letting them pass as an empty wait set.
        size_t woken = 0;
        while (woken < max_count) {
            if (os_sync_wake_by_address_any(address, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED) == 0) {
                ++woken;
                continue;
            }
            if (errno != ENOENT)
                dbgln("Core::atomic_notify: os_sync_wake_by_address_any failed with errno {}", errno);
            break;
        }
        return woken;
    }
    // The poll_wait fallback observes value changes directly — so no explicit wake is required.
    (void)address;
    (void)max_count;
    return 0;
}

#elif defined(AK_OS_LINUX)

AtomicWaitResult atomic_wait(void* address, u64 expected, size_t size, Optional<AK::Duration> timeout)
{
    // Compare the value in userspace first — so a zero or already-elapsed timeout still yields "not-equal" (which the
    // spec orders before "timed-out"), and so 8-byte waits and the poll fallback share one compare.
    if (!word_equals(address, expected, size))
        return AtomicWaitResult::NotEqual;

    // FUTEX_WAIT compares a 32-bit word. An 8-byte wait waits on the low 32-bit half instead: The full-width userspace
    // pre-compare above already rejected any full-value mismatch — so the low half only has to close the lost-wake race
    // at entry. On little-endian, the low half is the word at 'address' (byte offset 0) — so no offset is needed.
    static_assert(AK::HostIsLittleEndian, "8-byte futex waits assume the low 32-bit half is at byte offset 0");
    // Atomics.wait only ever uses size 4 or 8 (ValidateIntegerTypedArray); keep the poll fallback as a guard.
    if (size != 4 && size != 8)
        return poll_wait(address, expected, size, timeout);

    // Anchor the timeout to a fixed deadline — so EINTR retries reduce the remaining time, rather than restarting the
    // full timeout (FUTEX_WAIT takes a relative timeout that the kernel doesn't update across a retry).
    Optional<MonotonicTime> deadline;
    if (timeout.has_value())
        deadline = MonotonicTime::now() + *timeout;

    for (;;) {
        struct timespec relative_timeout;
        struct timespec* timeout_pointer = nullptr;
        if (deadline.has_value()) {
            auto remaining = (*deadline - MonotonicTime::now()).to_nanoseconds();
            if (remaining <= 0)
                return AtomicWaitResult::TimedOut;
            relative_timeout.tv_sec = static_cast<time_t>(remaining / 1'000'000'000);
            relative_timeout.tv_nsec = static_cast<long>(remaining % 1'000'000'000);
            timeout_pointer = &relative_timeout;
        }

        // A shared (non-private) FUTEX_WAIT keys on the underlying page — so it coordinates across processes.
        long result = syscall(SYS_futex, address, FUTEX_WAIT, static_cast<u32>(expected), timeout_pointer, nullptr, 0);
        if (result == 0)
            return AtomicWaitResult::Woken;
        if (errno == EINTR)
            continue;
        if (errno == ETIMEDOUT)
            return AtomicWaitResult::TimedOut;
        // EAGAIN means the value at the address no longer equals expected.
        return AtomicWaitResult::NotEqual;
    }
}

size_t atomic_notify(void* address, size_t size, size_t max_count)
{
    // FUTEX_WAKE wakes waiters keyed on the low 32-bit half at 'address'. That covers both 4-byte and 8-byte waiters
    // (8-byte waits key on the low half too), and returns the exact number of waiters woken.
    (void)size;
    auto count = max_count > static_cast<size_t>(NumericLimits<i32>::max()) ? NumericLimits<i32>::max() : static_cast<i32>(max_count);
    long result = syscall(SYS_futex, address, FUTEX_WAKE, count, nullptr, nullptr, 0);
    return result < 0 ? 0 : static_cast<size_t>(result);
}

#else

// No native wait/wake primitive is wired up on this platform. Poll for a change to the word, instead of reporting
// "not-equal" unconditionally — which would make every Atomics.wait return immediately, and spin its caller. Polling
// observes the waker's store directly — so, it works across processes for memory shared between agents.
AtomicWaitResult atomic_wait(void* address, u64 expected, size_t size, Optional<AK::Duration> timeout)
{
    return poll_wait(address, expected, size, timeout);
}

size_t atomic_notify(void*, size_t, size_t)
{
    // The waiters poll for the value change the waker has already made — so there's nothing to signal. This reports no
    // waiters woken, which Atomics.notify returns to script.
    return 0;
}

#endif

}
