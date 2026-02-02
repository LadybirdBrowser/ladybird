/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/HTML/Timer.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Timer);

GC::Ref<Timer> Timer::create(JS::Object& window_or_worker_global_scope, i32 milliseconds, Function<void()> callback, i32 id, Repeating repeating)
{
    return window_or_worker_global_scope.heap().allocate<Timer>(window_or_worker_global_scope, milliseconds, move(callback), id, repeating);
}

Timer::Timer(JS::Object& window_or_worker_global_scope, i32 milliseconds, Function<void()> callback, i32 id, Repeating repeating)
    : m_window_or_worker_global_scope(window_or_worker_global_scope)
    , m_id(id)
{
    if (repeating == Repeating::Yes)
        m_timer = Core::Timer::create_repeating(milliseconds, move(callback));
    else
        m_timer = Core::Timer::create_single_shot(milliseconds, move(callback));
}

void Timer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window_or_worker_global_scope);
    visitor.visit_possible_values(m_timer->on_timeout.raw_capture_range());
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

void Timer::set_callback(Function<void()> callback)
{
    m_timer->on_timeout = move(callback);
}

void Timer::set_interval(i32 milliseconds)
{
    if (m_timer->interval() != milliseconds)
        m_timer->restart(milliseconds);
}

}
