/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGC/Timer.h>

namespace GC {

GC_DEFINE_ALLOCATOR(Timer);

Timer::Timer()
    : m_timer(Core::Timer::create())
{
}

Timer::~Timer() = default;

void Timer::finalize()
{
    Base::finalize();
    stop();
}

void Timer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_on_timeout);
}

void Timer::start(u64 timeout_ms, GC::Ref<GC::Function<void()>> on_timeout)
{
    m_on_timeout = on_timeout;

    m_timer->on_timeout = [this]() {
        m_timed_out = true;

        if (m_on_timeout) {
            m_on_timeout->function()();
            m_on_timeout = nullptr;
        }
    };

    m_timer->set_interval(static_cast<int>(timeout_ms));
    m_timer->set_single_shot(true);
    m_timer->start();
}

void Timer::stop_and_fire_timeout_handler()
{
    auto on_timeout = m_on_timeout;
    stop();

    if (on_timeout)
        on_timeout->function()();
}

void Timer::stop()
{
    m_on_timeout = nullptr;
    m_timer->stop();
}

}
