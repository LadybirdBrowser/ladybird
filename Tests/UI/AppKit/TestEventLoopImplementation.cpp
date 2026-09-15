/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ConditionVariable.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

TEST_CASE(quit_event_loop_before_worker_run_loop_waits)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Mutex mutex;
    IGNORE_USE_IN_ESCAPING_LAMBDA ConditionVariable condition { mutex };
    IGNORE_USE_IN_ESCAPING_LAMBDA RefPtr<Core::WeakEventLoopReference> weak_ref;
    IGNORE_USE_IN_ESCAPING_LAMBDA Core::ThreadEventQueue* thread_event_queue { nullptr };
    IGNORE_USE_IN_ESCAPING_LAMBDA bool may_exec { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA bool callback_is_running { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA bool callback_may_return { false };

    auto thread = Threading::Thread::construct("AppKit event loop"sv, [&] {
        Core::EventLoop event_loop;
        {
            MutexLocker locker { mutex };
            weak_ref = Core::EventLoop::current_weak();
            thread_event_queue = &Core::ThreadEventQueue::current();
            condition.broadcast();
            condition.wait_while([&] { return !may_exec; });
        }
        return event_loop.exec();
    });
    thread->start();

    RefPtr<Core::WeakEventLoopReference> event_loop;
    Core::ThreadEventQueue* event_queue;
    {
        MutexLocker locker { mutex };
        condition.wait_while([&] { return !thread_event_queue; });
        event_loop = weak_ref;
        event_queue = thread_event_queue;
    }

    event_queue->deferred_invoke([&] {
        MutexLocker locker { mutex };
        callback_is_running = true;
        condition.broadcast();
        condition.wait_while([&] { return !callback_may_return; });
    });

    {
        MutexLocker locker { mutex };
        may_exec = true;
        condition.broadcast();
        condition.wait_while([&] { return !callback_is_running; });
    }

    {
        auto strong_event_loop = event_loop->take();
        VERIFY(strong_event_loop);
        EXPECT(!strong_event_loop->was_exit_requested());
        strong_event_loop->quit(42);
        EXPECT(strong_event_loop->was_exit_requested());
    }

    {
        MutexLocker locker { mutex };
        callback_may_return = true;
        condition.broadcast();
    }

    auto exit_code = MUST(thread->join<void*>());
    EXPECT_EQ(reinterpret_cast<intptr_t>(exit_code), 42);
}
