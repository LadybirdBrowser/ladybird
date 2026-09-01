/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibSync/ConditionVariable.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

TEST_CASE(test_poll_for_events)
{
    Core::EventLoop event_loop;

    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
}

// Simulate the condition that occurs during exit(): ThreadData (thread-local) is destroyed
// while the EventLoop (normally stack-allocated) is still alive. Another thread holding a
// WeakEventLoopReference can then call wake(), which must handle the closed pipe FD gracefully.
TEST_CASE(wake_after_thread_exit)
{
    Core::EventLoop main_loop;

    IGNORE_USE_IN_ESCAPING_LAMBDA OwnPtr<Core::EventLoop> worker_loop;
    IGNORE_USE_IN_ESCAPING_LAMBDA RefPtr<Core::WeakEventLoopReference> weak_ref;

    auto thread = Threading::Thread::construct("Worker"sv, [&] {
        worker_loop = make<Core::EventLoop>();
        weak_ref = Core::EventLoop::current_weak();
        return 0;
    });
    thread->start();
    MUST(thread->join());

    {
        auto strong = weak_ref->take();
        if (strong)
            strong->wake();
    }

    worker_loop.clear();
}

TEST_CASE(quit_event_loop_from_another_thread)
{
    Core::EventLoop main_loop;

    IGNORE_USE_IN_ESCAPING_LAMBDA Sync::Mutex mutex;
    IGNORE_USE_IN_ESCAPING_LAMBDA Sync::ConditionVariable condition { mutex };
    IGNORE_USE_IN_ESCAPING_LAMBDA RefPtr<Core::WeakEventLoopReference> weak_ref;
    IGNORE_USE_IN_ESCAPING_LAMBDA bool exec_started { false };

    auto thread = Threading::Thread::construct("Worker"sv, [&] {
        Core::EventLoop event_loop;
        {
            Sync::MutexLocker locker { mutex };
            weak_ref = Core::EventLoop::current_weak();
        }
        event_loop.deferred_invoke([&] {
            Sync::MutexLocker locker { mutex };
            exec_started = true;
            condition.broadcast();
        });
        return event_loop.exec();
    });
    thread->start();

    RefPtr<Core::WeakEventLoopReference> event_loop;
    {
        Sync::MutexLocker locker { mutex };
        condition.wait_while([&] { return !exec_started; });
        event_loop = weak_ref;
    }

    {
        auto strong_event_loop = event_loop->take();
        VERIFY(strong_event_loop);
        EXPECT(!strong_event_loop->was_exit_requested());
        strong_event_loop->quit(42);
        EXPECT(strong_event_loop->was_exit_requested());
        strong_event_loop->wake();
    }

    auto exit_code = MUST(thread->join<void*>());
    EXPECT_EQ(reinterpret_cast<intptr_t>(exit_code), 42);
}

TEST_CASE(single_shot_timer_fires_once)
{
    Core::EventLoop loop;
    int count = 0;
    auto timer = Core::Timer::create_single_shot(10, [&] {
        ++count;
        loop.quit(0);
    });
    timer->start();
    loop.exec();
    EXPECT_EQ(count, 1);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(count, 1);
}

TEST_CASE(zero_interval_repeating_timer)
{
    Core::EventLoop loop;
    int count = 0;
    auto timer = Core::Timer::create_repeating(0, [&] {
        if (++count == 5)
            loop.quit(0);
    });
    timer->start();
    loop.exec();
    EXPECT_EQ(count, 5);
}

TEST_CASE(repeating_timer_keeps_cadence)
{
    Core::EventLoop loop;
    int count = 0;
    auto start = MonotonicTime::now();
    auto timer = Core::Timer::create_repeating(10, [&] {
        if (++count == 20)
            loop.quit(0);
    });
    timer->start();
    loop.exec();
    auto elapsed_ms = (MonotonicTime::now() - start).to_milliseconds();
    EXPECT_EQ(count, 20);
    // 20 fires at a fixed 10ms cadence take ~200ms; rescheduling from the processing time
    // instead of the previous deadline accumulates latency and overshoots substantially.
    EXPECT(elapsed_ms >= 190);
    EXPECT(elapsed_ms <= 350);
}

TEST_CASE(equal_deadlines_fire_in_registration_order)
{
    Core::EventLoop loop;
    Vector<int> order;
    Vector<NonnullRefPtr<Core::Timer>> timers;
    for (int i = 0; i < 8; ++i) {
        timers.append(Core::Timer::create_single_shot(20, [&order, &loop, i] {
            order.append(i);
            if (order.size() == 8)
                loop.quit(0);
        }));
    }
    for (auto& timer : timers)
        timer->start();
    loop.exec();
    EXPECT_EQ(order.size(), 8uz);
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(order[i], i);
}

TEST_CASE(stopped_timer_does_not_fire)
{
    Core::EventLoop loop;
    int stopped_count = 0;
    auto stopped_timer = Core::Timer::create_single_shot(10, [&] { ++stopped_count; });
    stopped_timer->start();
    stopped_timer->stop();
    auto quit_timer = Core::Timer::create_single_shot(50, [&] { loop.quit(0); });
    quit_timer->start();
    loop.exec();
    EXPECT_EQ(stopped_count, 0);
}
