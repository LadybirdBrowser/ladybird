/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Message.h>
#include <LibIPC/Transport.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

namespace Fuzzing {

// Exit 77 denotes missing infrastructure/progress, not an oracle violation.
// Establish the deadline on the intended machine before running a campaign.
[[noreturn]] inline void unavailable(char const* phase)
{
    fprintf(stderr, "FUZZ HARNESS INCONCLUSIVE: %s\n", phase);
    _exit(77);
}

template<typename Predicate, typename Progress>
void pump_until(Core::EventLoop& loop, Predicate&& done, Progress&& progress, char const* phase)
{
    auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(5);
    while (!done()) {
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        progress();
        if (MonotonicTime::now() >= deadline)
            unavailable(phase);
        sched_yield();
    }
}

template<typename Endpoint, typename Callback>
void receive(IPC::Transport& transport, Callback&& callback)
{
    (void)transport.read_as_many_messages_as_possible_without_blocking([&](IPC::Transport::Message&& wire) {
        auto message = MUST(Endpoint::decode_message(wire.bytes.bytes(), wire.attachments));
        VERIFY(wire.attachments.is_empty());
        callback(*message);
    });
}

}
