/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>

namespace Core {

NonnullRefPtr<Timer> Timer::create()
{
    return adopt_ref(*new Timer);
}

NonnullRefPtr<Timer> Timer::create_repeating(int interval_ms, Function<void()>&& timeout_handler, EventReceiver* parent)
{
    return adopt_ref(*new Timer(interval_ms, move(timeout_handler), parent));
}

NonnullRefPtr<Timer> Timer::create_single_shot(int interval_ms, Function<void()>&& timeout_handler, EventReceiver* parent)
{
    auto timer = adopt_ref(*new Timer(interval_ms, move(timeout_handler), parent));
    timer->set_single_shot(true);
    return timer;
}

Timer::~Timer() = default;

Timer::Timer(EventReceiver* parent)
    : EventReceiver(parent)
{
}

Timer::Timer(int interval_ms, Function<void()>&& timeout_handler, EventReceiver* parent)
    : EventReceiver(parent)
    , on_timeout(move(timeout_handler))
    , m_interval_ms(interval_ms)
{
}

void Timer::start()
{
    start(m_interval_ms);
}

void Timer::start(int interval_ms)
{
    if (m_active)
        return;
    m_interval_ms = interval_ms;
    start_timer(interval_ms);
    m_active = true;
}

void Timer::restart()
{
    restart(m_interval_ms);
}

void Timer::restart(int interval_ms)
{
    if (m_active)
        stop();
    start(interval_ms);
}

void Timer::stop()
{
    if (!m_active)
        return;
    stop_timer();
    m_active = false;
}

void Timer::set_active(bool active)
{
    if (active)
        start();
    else
        stop();
}

void Timer::timer_event(TimerEvent&)
{
    if (m_single_shot)
        stop();
    else {
        if (m_interval_dirty) {
            stop();
            start(m_interval_ms);
        }
    }

    if (on_timeout)
        on_timeout();
}

}
