/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>

TEST_CASE(test_poll_for_events)
{
    Core::EventLoop event_loop;

    event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
}
