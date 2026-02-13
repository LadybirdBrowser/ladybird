/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/TransportSocket.h>
#include <LibTest/TestCase.h>

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

TEST_CASE(read_hook_is_notified_on_peer_hangup)
{
    Core::EventLoop loop;

    int fds[2] = {};
    MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));

    auto reader_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[0]));
    auto peer_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[1]));

    MUST(reader_socket->set_blocking(false));
    MUST(peer_socket->set_blocking(false));

    IPC::TransportSocket transport(move(reader_socket));

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> observed_shutdown = false;

    transport.set_up_read_hook([&] {
        auto should_shutdown = transport.read_as_many_messages_as_possible_without_blocking([](auto&&) {
        });
        if (should_shutdown == IPC::TransportSocket::ShouldShutdown::Yes)
            observed_shutdown.store(true, AK::MemoryOrder::memory_order_relaxed);
    });

    peer_socket->close();

    spin_until(loop, [&] {
        return observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed);
    });

    EXPECT(observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed));
}

TEST_CASE(read_hook_is_notified_when_io_thread_exits_on_close)
{
    Core::EventLoop loop;

    int fds[2] = {};
    MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));

    auto reader_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[0]));
    auto peer_socket = TRY_OR_FAIL(Core::LocalSocket::adopt_fd(fds[1]));

    MUST(reader_socket->set_blocking(false));
    MUST(peer_socket->set_blocking(false));

    IPC::TransportSocket transport(move(reader_socket));

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> observed_shutdown = false;

    transport.set_up_read_hook([&] {
        auto should_shutdown = transport.read_as_many_messages_as_possible_without_blocking([](auto&&) {
        });
        if (should_shutdown == IPC::TransportSocket::ShouldShutdown::Yes)
            observed_shutdown.store(true, AK::MemoryOrder::memory_order_relaxed);
    });

    transport.close();

    spin_until(loop, [&] {
        return observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed);
    });

    EXPECT(observed_shutdown.load(AK::MemoryOrder::memory_order_relaxed));
}
