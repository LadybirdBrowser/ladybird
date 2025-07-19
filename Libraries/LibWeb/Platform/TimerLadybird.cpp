/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibWeb/Platform/TimerLadybird.h>

namespace Web::Platform {

GC::Ref<TimerLadybird> TimerLadybird::create(GC::Heap& heap)
{
    return heap.allocate<TimerLadybird>();
}

TimerLadybird::TimerLadybird()
    : m_timer(Core::Timer::try_create().release_value_but_fixme_should_propagate_errors())
{
    m_timer->on_timeout = [this] {
        if (on_timeout)
            on_timeout->function()();
    };
}

TimerLadybird::~TimerLadybird() = default;

void TimerLadybird::start()
{
    m_timer->start();
}

void TimerLadybird::start(int interval_ms)
{
    m_timer->start(interval_ms);
}

void TimerLadybird::restart()
{
    m_timer->restart();
}

void TimerLadybird::restart(int interval_ms)
{
    m_timer->restart(interval_ms);
}

void TimerLadybird::stop()
{
    m_timer->stop();
}

void TimerLadybird::set_active(bool active)
{
    m_timer->set_active(active);
}

bool TimerLadybird::is_active() const
{
    return m_timer->is_active();
}

int TimerLadybird::interval() const
{
    return m_timer->interval();
}

void TimerLadybird::set_interval(int interval_ms)
{
    m_timer->set_interval(interval_ms);
}

bool TimerLadybird::is_single_shot() const
{
    return m_timer->is_single_shot();
}

void TimerLadybird::set_single_shot(bool single_shot)
{
    m_timer->set_single_shot(single_shot);
}

}
