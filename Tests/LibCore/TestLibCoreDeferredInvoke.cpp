/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibTest/TestCase.h>

TEST_CASE(deferred_invoke)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA Core::EventLoop event_loop;
    auto reaper = Core::Timer::create_single_shot(250, [] {
        warnln("I waited for the deferred_invoke to happen, but it never did!");
        VERIFY_NOT_REACHED();
    });

    Core::deferred_invoke([&event_loop] {
        event_loop.quit(0);
    });

    event_loop.exec();
}
