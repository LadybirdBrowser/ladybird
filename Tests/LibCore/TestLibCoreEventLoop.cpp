/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibCore/EventLoop.h>
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
