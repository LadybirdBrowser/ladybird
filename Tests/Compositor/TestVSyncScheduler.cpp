/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/VSyncScheduler.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibTest/TestCase.h>

TEST_CASE(requests_before_a_tick_are_coalesced)
{
    Core::EventLoop event_loop;
    int tick_count = 0;
    auto timeout = Core::Timer::create_single_shot(1000, [&] { event_loop.quit(0); });
    auto after_tick = Core::Timer::create_single_shot(50, [&] { event_loop.quit(0); });
    auto scheduler = Compositor::create_vsync_scheduler({}, [&](MonotonicTime) {
        ++tick_count;
        if (tick_count == 1)
            after_tick->start();
    });

    timeout->start();
    scheduler->schedule(120);
    scheduler->schedule(120);
    scheduler->schedule(120);
    event_loop.exec();

    EXPECT_EQ(tick_count, 1);
}

TEST_CASE(a_tick_callback_can_request_the_next_tick)
{
    Core::EventLoop event_loop;
    int tick_count = 0;
    auto timeout = Core::Timer::create_single_shot(1000, [&] { event_loop.quit(0); });
    OwnPtr<Compositor::VSyncScheduler> scheduler;
    scheduler = Compositor::create_vsync_scheduler({}, [&](MonotonicTime) {
        if (++tick_count == 1)
            scheduler->schedule(120);
        else
            event_loop.quit(0);
    });

    timeout->start();
    scheduler->schedule(120);
    event_loop.exec();

    EXPECT_EQ(tick_count, 2);
}

TEST_CASE(a_refresh_rate_change_rearms_the_pending_tick)
{
    Core::EventLoop event_loop;
    int tick_count = 0;
    auto timeout = Core::Timer::create_single_shot(1000, [&] { event_loop.quit(0); });
    auto scheduler = Compositor::create_vsync_scheduler({}, [&](MonotonicTime) {
        ++tick_count;
        event_loop.quit(0);
    });

    timeout->start();
    scheduler->schedule(0.1);
    scheduler->schedule(120);
    event_loop.exec();

    EXPECT_EQ(tick_count, 1);
}
