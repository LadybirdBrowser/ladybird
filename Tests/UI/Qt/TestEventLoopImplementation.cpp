/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ConditionVariable.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
#include <UI/Qt/EventLoopImplementationQt.h>

#include <QAbstractEventDispatcher>
#include <fcntl.h>
#include <signal.h>

#if defined(AK_OS_LINUX)
TEST_CASE(signal_socket_descriptors_are_close_on_exec)
{
    Core::EventLoop event_loop;
    Vector<int> descriptors_before;
    for (int fd = 0; fd < 1024; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0)
            descriptors_before.append(fd);
    }

    static_cast<Ladybird::EventLoopImplementationQt&>(event_loop.impl()).set_main_loop();

    Vector<int> signal_descriptors;
    for (int fd = 0; fd < 1024; ++fd) {
        auto flags = fcntl(fd, F_GETFD);
        if (flags < 0 || descriptors_before.contains_slow(fd))
            continue;
        int type = 0;
        socklen_t type_size = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size) == 0 && type == SOCK_STREAM) {
            signal_descriptors.append(fd);
            EXPECT(flags & FD_CLOEXEC);
        }
    }
    EXPECT_EQ(signal_descriptors.size(), 2u);

    bool handled = false;
    auto handler_id = Core::EventLoop::register_signal(SIGUSR1, [&](int) { handled = true; });
    VERIFY(pthread_kill(pthread_self(), SIGUSR1) == 0);
    for (int i = 0; i < 400 && !handled; ++i) {
        (void)event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        MUST(Core::System::sleep_ms(5));
    }
    EXPECT(handled);
    Core::EventLoop::unregister_signal(handler_id);
}
#endif

TEST_CASE(quit_event_loop_from_another_thread)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Mutex mutex;
    IGNORE_USE_IN_ESCAPING_LAMBDA ConditionVariable condition { mutex };
    IGNORE_USE_IN_ESCAPING_LAMBDA RefPtr<Core::WeakEventLoopReference> weak_ref;
    IGNORE_USE_IN_ESCAPING_LAMBDA bool exec_started { false };

    auto thread = Threading::Thread::construct("Qt event loop"sv, [&] {
        Core::EventLoop event_loop;
        {
            MutexLocker locker { mutex };
            weak_ref = Core::EventLoop::current_weak();
        }
        auto* event_dispatcher = QAbstractEventDispatcher::instance();
        VERIFY(event_dispatcher);
        QObject::connect(event_dispatcher, &QAbstractEventDispatcher::aboutToBlock, [&] {
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
