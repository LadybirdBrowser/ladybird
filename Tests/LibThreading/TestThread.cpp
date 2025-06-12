/*
 * Copyright (c) 2021, Spencer Dixon <spencercdixon@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

using namespace AK::TimeLiterals;

static void sleep_until_thread_exits(Threading::Thread const& thread)
{
    static constexpr auto delay = 20_ms;

    for (auto i = 0; i < 100; ++i) {
        if (thread.has_exited())
            return;

        (void)Core::System::sleep_ms(delay.to_milliseconds());
    }

    FAIL("Timed out waiting for thread to exit");
}

TEST_CASE(threads_can_detach)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<int> should_be_42 = 0;

    auto thread = Threading::Thread::construct([&should_be_42]() {
        (void)Core::System::sleep_ms(10);
        should_be_42 = 42;
        return 0;
    });
    thread->start();
    thread->detach();

    sleep_until_thread_exits(*thread);
    EXPECT(should_be_42 == 42);
}

TEST_CASE(detached_threads_do_not_need_to_be_joined)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> should_exit { false };
    auto thread = Threading::Thread::construct([&]() {
        while (!should_exit.load())
            (void)Core::System::sleep_ms(10);
        return 0;
    });
    thread->start();
    thread->detach();

    // Because of how the crash test forks and removes the thread, we can't use that to verify that join() crashes. Instead, we check the join crash condition ourselves.
    EXPECT(!thread->needs_to_be_joined());

    // FIXME: Dropping a running thread crashes because of the Function destructor. For now, force the detached thread to exit.
    should_exit.store(true);
    sleep_until_thread_exits(*thread);
}

TEST_CASE(join_dead_thread)
{
    auto thread = Threading::Thread::construct([&]() { return 0 /*nullptr*/; });
    thread->start();

    // The thread should have exited by then.
    sleep_until_thread_exits(*thread);

    auto join_result = TRY_OR_FAIL(thread->join<int*>());
    EXPECT_EQ(join_result, static_cast<int*>(0));
}
