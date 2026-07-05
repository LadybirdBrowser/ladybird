/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/Time.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Futex.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

// Back the word with cross-process shared memory — as a real SharedArrayBuffer word is — so os_sync and the kernel
// futex operate on the same kind of memory they do in production.
static Core::AnonymousBuffer make_word_buffer()
{
    return MUST(Core::AnonymousBuffer::create_with_size(sizeof(u64)));
}

TEST_CASE(wait_returns_not_equal_on_mismatch)
{
    auto buffer = make_word_buffer();
    auto* word = buffer.data<u32>();
    *word = 42;

    // A mismatched value must return "not-equal" immediately without blocking — even with no timeout. On macOS, this is
    // the regression guard: os_sync_wait_on_address reports a mismatch as a successful (wake) return — so without the
    // userspace pre-compare, this would wrongly report "ok".
    EXPECT(Core::atomic_wait(word, 99, sizeof(u32), {}) == Core::AtomicWaitResult::NotEqual);
}

TEST_CASE(wait_not_equal_beats_zero_timeout)
{
    auto buffer = make_word_buffer();
    auto* word = buffer.data<u32>();
    *word = 42;

    // With a zero (or sub-tick) timeout and a mismatched value, "not-equal" must win over "timed-out".
    EXPECT(Core::atomic_wait(word, 99, sizeof(u32), AK::Duration::from_milliseconds(0)) == Core::AtomicWaitResult::NotEqual);
}

TEST_CASE(wait_not_equal_with_finite_timeout)
{
    auto buffer = make_word_buffer();
    auto* word = buffer.data<u32>();
    *word = 42;

    EXPECT(Core::atomic_wait(word, 99, sizeof(u32), AK::Duration::from_milliseconds(50)) == Core::AtomicWaitResult::NotEqual);
}

TEST_CASE(wait_times_out_when_value_matches)
{
    auto buffer = make_word_buffer();
    auto* word = buffer.data<u32>();
    *word = 42;

    auto start = MonotonicTime::now();
    // The value matches and nothing notifies — so the wait must run to the timeout.
    EXPECT(Core::atomic_wait(word, 42, sizeof(u32), AK::Duration::from_milliseconds(50)) == Core::AtomicWaitResult::TimedOut);
    // And it must actually have waited, not returned instantly.
    EXPECT((MonotonicTime::now() - start) >= AK::Duration::from_milliseconds(20));
}

TEST_CASE(wait_zero_timeout_when_value_matches)
{
    auto buffer = make_word_buffer();
    auto* word = buffer.data<u32>();
    *word = 42;

    // The value matches but the timeout is zero — so it times out immediately.
    EXPECT(Core::atomic_wait(word, 42, sizeof(u32), AK::Duration::from_milliseconds(0)) == Core::AtomicWaitResult::TimedOut);
}

TEST_CASE(wait_eight_byte_not_equal_in_high_half)
{
    auto buffer = make_word_buffer();
    auto* word = buffer.data<u64>();
    *word = 0x0000000100000042ULL; // low half 0x42, high half 1

    // The low 32 bits match, but the high 32 differ; a full 64-bit compare must report "not-equal".
    EXPECT(Core::atomic_wait(word, 0x0000000200000042ULL, sizeof(u64), {}) == Core::AtomicWaitResult::NotEqual);
}

TEST_CASE(wait_eight_byte_times_out_when_value_matches)
{
    auto buffer = make_word_buffer();
    auto* word = buffer.data<u64>();
    *word = 0x0000000100000042ULL;

    EXPECT(Core::atomic_wait(word, 0x0000000100000042ULL, sizeof(u64), AK::Duration::from_milliseconds(50)) == Core::AtomicWaitResult::TimedOut);
}

TEST_CASE(notify_wakes_a_waiter)
{
    auto buffer = make_word_buffer();
    IGNORE_USE_IN_ESCAPING_LAMBDA auto* word = buffer.data<u32>();
    *word = 42;

    IGNORE_USE_IN_ESCAPING_LAMBDA AK::Atomic<bool> waiter_ready { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA AK::Atomic<int> result { -1 };

    auto thread = Threading::Thread::construct("FutexWaiter"sv, [&]() {
        waiter_ready.store(true);
        // The value stays 42 — so this blocks until notified; a generous timeout keeps a slow scheduler from timing us
        // out during the retry loop below.
        auto wait_result = Core::atomic_wait(word, 42, sizeof(u32), AK::Duration::from_milliseconds(5000));
        result.store(static_cast<int>(wait_result));
        return 0;
    });
    thread->start();

    while (!waiter_ready.load())
        (void)Core::System::sleep_ms(1);

    // Retry to cover the window between the ready flag and the thread actually entering the kernel wait.
    for (int i = 0; i < 200 && result.load() == -1; ++i) {
        (void)Core::System::sleep_ms(5);
        Core::atomic_notify(word, sizeof(u32), 1);
    }

    (void)thread->join();
    EXPECT(result.load() == static_cast<int>(Core::AtomicWaitResult::Woken));
}

TEST_CASE(notify_wakes_an_eight_byte_waiter)
{
    auto buffer = make_word_buffer();
    IGNORE_USE_IN_ESCAPING_LAMBDA auto* word = buffer.data<u64>();
    *word = 0x0000000100000042ULL;

    IGNORE_USE_IN_ESCAPING_LAMBDA AK::Atomic<bool> waiter_ready { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA AK::Atomic<int> result { -1 };

    auto thread = Threading::Thread::construct("FutexWaiter64"sv, [&]() {
        waiter_ready.store(true);
        // The value never changes — so only a real notify (not a value change) can wake this 8-byte waiter. Under the
        // old poll fallback, this hangs to the timeout — since polling only ever observes value changes.
        auto wait_result = Core::atomic_wait(word, 0x0000000100000042ULL, sizeof(u64), AK::Duration::from_milliseconds(5000));
        result.store(static_cast<int>(wait_result));
        return 0;
    });
    thread->start();

    while (!waiter_ready.load())
        (void)Core::System::sleep_ms(1);

    size_t max_woken = 0;
    for (int i = 0; i < 200 && result.load() == -1; ++i) {
        (void)Core::System::sleep_ms(5);
        auto woken = Core::atomic_notify(word, sizeof(u64), 1);
        if (woken > max_woken)
            max_woken = woken;
    }

    (void)thread->join();
    EXPECT(result.load() == static_cast<int>(Core::AtomicWaitResult::Woken));
    // An 8-byte notify now reports the woken count accurately (the poll fallback always returned 0).
    EXPECT(max_woken == 1u);
}

TEST_CASE(notify_from_a_four_byte_view_wakes_an_eight_byte_waiter)
{
    auto buffer = make_word_buffer();
    IGNORE_USE_IN_ESCAPING_LAMBDA auto* word = buffer.data<u64>();
    *word = 0x0000000100000042ULL;

    IGNORE_USE_IN_ESCAPING_LAMBDA AK::Atomic<bool> waiter_ready { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA AK::Atomic<int> result { -1 };

    // An 8-byte (BigInt64) waiter and a 4-byte (Int32) notify at the same address are one WaiterList — so, the notify
    // must wake the waiter. Keying the kernel wait/wake by element size (as os_sync does natively) rejects this
    // mixed-size pair with EINVAL, and the waiter never wakes — unless both normalize to the low 32-bit half.
    auto thread = Threading::Thread::construct("FutexWaiterMixed64"sv, [&]() {
        waiter_ready.store(true);
        auto wait_result = Core::atomic_wait(word, 0x0000000100000042ULL, sizeof(u64), AK::Duration::from_milliseconds(5000));
        result.store(static_cast<int>(wait_result));
        return 0;
    });
    thread->start();

    while (!waiter_ready.load())
        (void)Core::System::sleep_ms(1);

    for (int i = 0; i < 200 && result.load() == -1; ++i) {
        (void)Core::System::sleep_ms(5);
        Core::atomic_notify(word, sizeof(u32), 1);
    }

    (void)thread->join();
    EXPECT(result.load() == static_cast<int>(Core::AtomicWaitResult::Woken));
}

TEST_CASE(notify_from_an_eight_byte_view_wakes_a_four_byte_waiter)
{
    auto buffer = make_word_buffer();
    IGNORE_USE_IN_ESCAPING_LAMBDA auto* word = buffer.data<u64>();
    *word = 0x0000000100000042ULL;

    IGNORE_USE_IN_ESCAPING_LAMBDA AK::Atomic<bool> waiter_ready { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA AK::Atomic<int> result { -1 };

    // The mirror of the above: A 4-byte waiter (on the low half, value 0x42) must be woken by an 8-byte notify.
    auto thread = Threading::Thread::construct("FutexWaiterMixed32"sv, [&]() {
        waiter_ready.store(true);
        auto wait_result = Core::atomic_wait(word, 0x42, sizeof(u32), AK::Duration::from_milliseconds(5000));
        result.store(static_cast<int>(wait_result));
        return 0;
    });
    thread->start();

    while (!waiter_ready.load())
        (void)Core::System::sleep_ms(1);

    for (int i = 0; i < 200 && result.load() == -1; ++i) {
        (void)Core::System::sleep_ms(5);
        Core::atomic_notify(word, sizeof(u64), 1);
    }

    (void)thread->join();
    EXPECT(result.load() == static_cast<int>(Core::AtomicWaitResult::Woken));
}
