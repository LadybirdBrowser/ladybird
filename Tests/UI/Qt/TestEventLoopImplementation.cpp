/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibSync/ConditionVariable.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

#include <QAbstractEventDispatcher>

TEST_CASE(quit_event_loop_from_another_thread)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Sync::Mutex mutex;
    IGNORE_USE_IN_ESCAPING_LAMBDA Sync::ConditionVariable condition { mutex };
    IGNORE_USE_IN_ESCAPING_LAMBDA RefPtr<Core::WeakEventLoopReference> weak_ref;
    IGNORE_USE_IN_ESCAPING_LAMBDA bool exec_started { false };

    auto thread = Threading::Thread::construct("Qt event loop"sv, [&] {
        Core::EventLoop event_loop;
        {
            Sync::MutexLocker locker { mutex };
            weak_ref = Core::EventLoop::current_weak();
        }
        auto* event_dispatcher = QAbstractEventDispatcher::instance();
        VERIFY(event_dispatcher);
        QObject::connect(event_dispatcher, &QAbstractEventDispatcher::aboutToBlock, [&] {
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
