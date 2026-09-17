/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/FullscreenDebounce.h>

#include <QEventLoop>

namespace {

void wait_for_timers()
{
    QEventLoop event_loop;
    QTimer::singleShot(10, &event_loop, &QEventLoop::quit);
    event_loop.exec();
}

}

TEST_CASE(deleting_the_owner_cancels_the_debounce_timer)
{
    Ladybird::FullscreenDebounce debounce;
    auto* owner = new QObject;
    debounce.start(*owner, 1);
    EXPECT(debounce.is_active());

    delete owner;
    wait_for_timers();

    EXPECT(debounce.is_active());
}

TEST_CASE(live_owner_allows_the_debounce_timer_to_finish)
{
    QObject owner;
    Ladybird::FullscreenDebounce debounce;
    debounce.start(owner, 1);

    wait_for_timers();

    EXPECT(!debounce.is_active());
}
