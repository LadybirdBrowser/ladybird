/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ConditionVariable.h>
#include <AK/OwnPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

#if !defined(AK_OS_WINDOWS)
#    include <LibCore/System.h>
#    include <pthread.h>
#    include <signal.h>
#endif

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

    IGNORE_USE_IN_ESCAPING_LAMBDA Mutex mutex;
    IGNORE_USE_IN_ESCAPING_LAMBDA ConditionVariable condition { mutex };
    IGNORE_USE_IN_ESCAPING_LAMBDA RefPtr<Core::WeakEventLoopReference> weak_ref;
    IGNORE_USE_IN_ESCAPING_LAMBDA bool exec_started { false };

    auto thread = Threading::Thread::construct("Worker"sv, [&] {
        Core::EventLoop event_loop;
        {
            MutexLocker locker { mutex };
            weak_ref = Core::EventLoop::current_weak();
        }
        event_loop.deferred_invoke([&] {
            MutexLocker locker { mutex };
            exec_started = true;
            condition.broadcast();
        });
        return event_loop.exec();
    });
    thread->start();

    RefPtr<Core::WeakEventLoopReference> event_loop;
    {
        MutexLocker locker { mutex };
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
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(10), [&] {
        ++count;
        loop.quit(0);
    });
    timer->start();
    loop.exec();
    EXPECT_EQ(count, 1);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(count, 1);
}

// The Timer event a single-shot timer posts can wait in the queue behind another event whose handler pumps the loop
// again, e.g. through spin_until(). The timer fires again in that nested pump, and its handler runs there. If the
// handler restarts the timer, the event still waiting in the queue is for the registration before the restart, and must
// not fire the restarted timer early.
TEST_CASE(single_shot_timer_fires_once_across_nested_pump)
{
    Core::EventLoop loop;
    int count = 0;
    RefPtr<Core::Timer> timer;
    timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(0), [&] {
        ++count;
        timer->restart(AK::Duration::from_seconds(10));
    });
    timer->start();
    // The pumping event queues itself again until the timer has fired, so the timer's event lands in a batch behind it
    // even where the timer's expiry reaches the loop only after a few polls.
    IGNORE_USE_IN_ESCAPING_LAMBDA Function<void()> pump_nested;
    pump_nested = [&] {
        for (int i = 0; i < 4; ++i)
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        if (count == 0)
            Core::deferred_invoke([&] { pump_nested(); });
    };
    Core::deferred_invoke([&] { pump_nested(); });
    while (count == 0)
        loop.pump(Core::EventLoop::WaitMode::WaitForEvents);
    for (int i = 0; i < 4; ++i)
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(count, 1);
    timer->stop();
}

// A single-shot timer whose Timer event waits in the queue behind an event whose handler spins the loop until the
// timer's handler has run fires again in that nested loop, so the spin ends.
TEST_CASE(single_shot_timer_fires_in_a_nested_pump_while_its_event_waits)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Core::EventLoop loop;
    IGNORE_USE_IN_ESCAPING_LAMBDA bool fired = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(0), [&] { fired = true; });
    timer->start();
    IGNORE_USE_IN_ESCAPING_LAMBDA bool spin_ended = false;
    Core::deferred_invoke([&] {
        auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(1);
        while (!fired && MonotonicTime::now() < deadline)
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        spin_ended = fired;
    });
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(spin_ended);
}

// A timer made repeating after it started as a single-shot one keeps firing.
TEST_CASE(single_shot_timer_made_repeating_keeps_firing)
{
    Core::EventLoop loop;
    int count = 0;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(0), [&] {
        if (++count == 3)
            loop.quit(0);
    });
    timer->start();
    timer->set_single_shot(false);
    loop.exec();
    EXPECT_EQ(count, 3);
}

TEST_CASE(zero_interval_repeating_timer)
{
    Core::EventLoop loop;
    int count = 0;
    auto timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(0), [&] {
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
    auto timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(10), [&] {
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
        timers.append(Core::Timer::create_single_shot(AK::Duration::from_milliseconds(20), [&order, &loop, i] {
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
    auto stopped_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(10), [&] { ++stopped_count; });
    stopped_timer->start();
    stopped_timer->stop();
    auto quit_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(50), [&] { loop.quit(0); });
    quit_timer->start();
    loop.exec();
    EXPECT_EQ(stopped_count, 0);
}

// A timer fires no earlier than its interval, on the monotonic clock the rest of the process reads. The loop is kept
// busy for a moment after each start: a clock that moves once per scheduler tick counts up to a tick more across that
// moment than really passed, so a loop timing its wait by that clock wakes early by the difference and fires the timer
// before the interval is up on the precise clock. The rounds drift through the tick, so a fair share of them hit that.
TEST_CASE(timer_does_not_fire_before_its_interval)
{
    Core::EventLoop loop;
    static constexpr int interval_ms = 10;
    static constexpr int rounds = 50;
    int round = 0;
    auto started_at = MonotonicTime::now();
    RefPtr<Core::Timer> timer;
    auto start_timer = [&] {
        started_at = MonotonicTime::now();
        timer->start();
        auto busy_until = started_at + AK::Duration::from_microseconds(2500);
        while (MonotonicTime::now() < busy_until)
            ;
    };
    timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(interval_ms), [&] {
        auto elapsed = MonotonicTime::now() - started_at;
        auto interval = AK::Duration::from_milliseconds(interval_ms);
        if (elapsed < interval)
            warnln("Round {}: the timer fired {} us early", round, (interval - elapsed).to_microseconds());
        EXPECT(elapsed >= interval);
        if (++round == rounds)
            loop.quit(0);
        else
            start_timer();
    });
    start_timer();
    loop.exec();
    EXPECT_EQ(round, rounds);
}

#if !defined(AK_OS_WINDOWS)
// Signals delivered to threads without event-loop state must wake the registering event loop.
TEST_CASE(signal_delivered_on_thread_without_event_loop)
{
    Core::EventLoop event_loop;

    IGNORE_USE_IN_ESCAPING_LAMBDA bool handled = false;
    auto handler_id = Core::EventLoop::register_signal(SIGUSR1, [&](int) { handled = true; });

    auto thread = Threading::Thread::construct("SignalRaiser"sv, []() -> intptr_t {
        VERIFY(pthread_kill(pthread_self(), SIGUSR1) == 0);
        return 0;
    });
    thread->start();
    (void)thread->join();

    for (int i = 0; i < 400 && !handled; ++i) {
        (void)event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        MUST(Core::System::sleep_ms(5));
    }

    EXPECT(handled);
    Core::EventLoop::unregister_signal(handler_id);
}

TEST_CASE(repeated_signal_deliveries_are_preserved)
{
    Core::EventLoop event_loop;

    static constexpr int signal_count = 4;
    IGNORE_USE_IN_ESCAPING_LAMBDA int handled_count = 0;
    auto handler_id = Core::EventLoop::register_signal(SIGUSR2, [&](int) { ++handled_count; });

    auto thread = Threading::Thread::construct("SignalRaiser"sv, []() -> intptr_t {
        for (int i = 0; i < signal_count; ++i)
            VERIFY(pthread_kill(pthread_self(), SIGUSR2) == 0);
        return 0;
    });
    thread->start();
    MUST(thread->join());

    for (int i = 0; i < 400 && handled_count < signal_count; ++i) {
        (void)event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        MUST(Core::System::sleep_ms(5));
    }

    EXPECT_EQ(handled_count, signal_count);
    Core::EventLoop::unregister_signal(handler_id);
}
#endif
