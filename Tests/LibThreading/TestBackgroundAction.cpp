/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <LibThreading/BackgroundAction.h>
#include <pthread.h>

using namespace AK::TimeLiterals;

static void spin_until(Core::EventLoop& loop, Function<bool()> condition, AK::Duration timeout = 2000_ms)
{
    i64 const timeout_ms = timeout.to_milliseconds();
    for (i64 elapsed_ms = 0; elapsed_ms < timeout_ms; elapsed_ms += 5) {
        (void)loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        if (condition())
            return;
        MUST(Core::System::sleep_ms(5));
    }

    FAIL("Timed out waiting for condition");
}

TEST_CASE(background_action_on_error_called_on_action_failure_and_on_origin_thread)
{
    Core::EventLoop loop;

    pthread_t const origin_thread_id = pthread_self();

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> action_ran = false;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> on_error_called = false;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> on_complete_called = false;

    Optional<pthread_t> action_thread_id;

    auto background_action = Threading::BackgroundAction<int>::construct(
        [&](auto&) -> ErrorOr<int> {
            action_thread_id = pthread_self();
            action_ran.store(true, AK::MemoryOrder::memory_order_relaxed);
            return Error::from_string_literal("action failed");
        },
        [&](int) -> ErrorOr<void> {
            on_complete_called.store(true, AK::MemoryOrder::memory_order_relaxed);
            loop.quit(1);
            return {};
        },
        [&](Error error) {
            EXPECT(pthread_equal(origin_thread_id, pthread_self()));
            EXPECT_EQ(error.string_literal(), "action failed"sv);
            on_error_called.store(true, AK::MemoryOrder::memory_order_relaxed);
            loop.quit(0);
        });

    loop.exec();

    EXPECT(action_ran.load(AK::MemoryOrder::memory_order_relaxed));
    EXPECT(action_thread_id.has_value());
    EXPECT(!pthread_equal(action_thread_id.value(), origin_thread_id));
    EXPECT(on_error_called.load(AK::MemoryOrder::memory_order_relaxed));
    EXPECT(!on_complete_called.load(AK::MemoryOrder::memory_order_relaxed));

    (void)background_action;
}

TEST_CASE(background_action_on_error_called_when_on_complete_returns_error)
{
    Core::EventLoop loop;

    pthread_t const origin_thread_id = pthread_self();

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<int> on_error_count = 0;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<int> on_complete_count = 0;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<int> stage = 0;

    Optional<pthread_t> action_thread_id;

    auto background_action = Threading::BackgroundAction<int>::construct(
        [&](auto&) -> ErrorOr<int> {
            action_thread_id = pthread_self();
            return 42;
        },
        [&](int value) -> ErrorOr<void> {
            EXPECT(pthread_equal(origin_thread_id, pthread_self()));
            EXPECT_EQ(value, 42);
            on_complete_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
            stage.store(1, AK::MemoryOrder::memory_order_relaxed);
            return Error::from_string_literal("on_complete failed");
        },
        [&](Error error) {
            EXPECT(pthread_equal(origin_thread_id, pthread_self()));
            EXPECT_EQ(error.string_literal(), "on_complete failed"sv);
            EXPECT_EQ(stage.load(AK::MemoryOrder::memory_order_relaxed), 1);
            on_error_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
            stage.store(2, AK::MemoryOrder::memory_order_relaxed);
            loop.quit(0);
        });

    loop.exec();

    EXPECT(action_thread_id.has_value());
    EXPECT(!pthread_equal(action_thread_id.value(), origin_thread_id));

    EXPECT_EQ(on_complete_count.load(AK::MemoryOrder::memory_order_relaxed), 1);
    EXPECT_EQ(on_error_count.load(AK::MemoryOrder::memory_order_relaxed), 1);
    EXPECT_EQ(stage.load(AK::MemoryOrder::memory_order_relaxed), 2);

    (void)background_action;
}

TEST_CASE(background_action_cancel_suppresses_on_error_and_on_complete)
{
    Core::EventLoop loop;

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> started = false;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> finished = false;

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<int> on_error_count = 0;
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<int> on_complete_count = 0;

    auto background_action = Threading::BackgroundAction<int>::construct(
        [&](auto& action) -> ErrorOr<int> {
            started.store(true, AK::MemoryOrder::memory_order_relaxed);

            while (!action.is_canceled())
                MUST(Core::System::sleep_ms(1));

            finished.store(true, AK::MemoryOrder::memory_order_relaxed);
            return Error::from_string_literal("error after cancel");
        },
        [&](int) -> ErrorOr<void> {
            on_complete_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
            return {};
        },
        [&](Error) {
            on_error_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
        });

    spin_until(loop, [&] {
        return started.load(AK::MemoryOrder::memory_order_relaxed);
    });

    background_action->cancel();

    spin_until(loop, [&] {
        return finished.load(AK::MemoryOrder::memory_order_relaxed);
    });

    // Run the loop a bit more to ensure any incorrectly-posted callbacks would execute.
    for (size_t i = 0; i < 50; ++i) {
        (void)loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        MUST(Core::System::sleep_ms(1));
    }

    EXPECT(background_action->is_canceled());
    EXPECT_EQ(on_complete_count.load(AK::MemoryOrder::memory_order_relaxed), 0);
    EXPECT_EQ(on_error_count.load(AK::MemoryOrder::memory_order_relaxed), 0);
}
