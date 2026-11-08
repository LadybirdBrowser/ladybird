/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGC/Heap.h>
#include <LibWeb/HTML/Timer.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Timer);

GC::Ref<Timer> Timer::create(i32 milliseconds, Callback callback, i32 id, Repeating repeating, TimerThrottlingClass throttling_class, double deadline)
{
    return GC::Heap::the().allocate<Timer>(milliseconds, callback, id, repeating, throttling_class, deadline);
}

Timer::Timer(i32 milliseconds, Callback callback, i32 id, Repeating repeating, TimerThrottlingClass throttling_class, double deadline)
    : m_callback(callback)
    , m_id(id)
    , m_throttling_class(throttling_class)
    , m_deadline(deadline)
    , m_earliest_firing(MonotonicTime::now() + AK::Duration::from_milliseconds(milliseconds))
{
    // NB: The timer belongs to this cell, which outlives it, as it is never finalized while the timer is active.
    auto on_timeout = [this] { (*m_callback)(); };
    if (repeating == Repeating::Yes)
        m_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(milliseconds), move(on_timeout));
    else
        m_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(milliseconds), move(on_timeout));
}

void Timer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
}

void Timer::finalize()
{
    Base::finalize();
    VERIFY(!m_timer->is_active());
}

void Timer::start()
{
    m_timer->start();
}

void Timer::stop()
{
    m_timer->stop();
}

void Timer::fire_now()
{
    (*m_callback)();
}

void Timer::restart(i32 milliseconds)
{
    m_earliest_firing = MonotonicTime::now() + AK::Duration::from_milliseconds(milliseconds);
    m_timer->restart(AK::Duration::from_milliseconds(milliseconds));
}

bool Timer::claim_firing()
{
    if (m_has_queued_firing)
        return false;
    // NB: A firing that comes before the restarted schedule allows one was posted before the restart. Only a repeating
    //     timer drops it: it fires again anyway, while a single-shot timer whose one firing came early must still run.
    if (!m_timer->is_single_shot() && MonotonicTime::now() < m_earliest_firing)
        return false;
    m_has_queued_firing = true;
    return true;
}

bool Timer::is_active() const
{
    return m_timer->is_active();
}

void Timer::set_interval(i32 milliseconds)
{
    if (m_timer->interval() != AK::Duration::from_milliseconds(milliseconds))
        restart(milliseconds);
}

}
