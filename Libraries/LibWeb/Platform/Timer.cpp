/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::Platform {

GC_DEFINE_ALLOCATOR(Timer);

Timer::Timer()
    : m_timer(Core::Timer::create())
{
    m_timer->on_timeout = [this] {
        if (on_timeout)
            on_timeout->function()();
    };
}

Timer::~Timer() = default;

void Timer::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(on_timeout);
}

GC::Ref<Timer> Timer::create(GC::Heap& heap)
{
    return heap.allocate<Timer>();
}

GC::Ref<Timer> Timer::create_repeating(GC::Heap& heap, int interval_ms, GC::Ptr<GC::Function<void()>> timeout_handler)
{
    auto timer = heap.allocate<Timer>();
    timer->set_single_shot(false);
    timer->set_interval(interval_ms);
    timer->on_timeout = move(timeout_handler);
    return timer;
}

GC::Ref<Timer> Timer::create_single_shot(GC::Heap& heap, int interval_ms, GC::Ptr<GC::Function<void()>> timeout_handler)
{
    auto timer = heap.allocate<Timer>();
    timer->set_single_shot(true);
    timer->set_interval(interval_ms);
    timer->on_timeout = move(timeout_handler);
    return timer;
}

void Timer::start()
{
    m_timer->start();
}

void Timer::start(int interval_ms)
{
    m_timer->start(interval_ms);
}

void Timer::restart()
{
    m_timer->restart();
}

void Timer::restart(int interval_ms)
{
    m_timer->restart(interval_ms);
}

void Timer::stop()
{
    m_timer->stop();
}

void Timer::set_active(bool active)
{
    m_timer->set_active(active);
}

bool Timer::is_active() const
{
    return m_timer->is_active();
}

int Timer::interval() const
{
    return m_timer->interval();
}

void Timer::set_interval(int interval_ms)
{
    m_timer->set_interval(interval_ms);
}

bool Timer::is_single_shot() const
{
    return m_timer->is_single_shot();
}

void Timer::set_single_shot(bool single_shot)
{
    m_timer->set_single_shot(single_shot);
}

}
