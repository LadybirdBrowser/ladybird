/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibWeb/WebDriver/HeapTimer.h>

namespace Web::WebDriver {

JS_DEFINE_ALLOCATOR(HeapTimer);

HeapTimer::HeapTimer()
    : m_timer(Core::Timer::create())
{
}

HeapTimer::~HeapTimer() = default;

void HeapTimer::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_on_timeout);
}

void HeapTimer::start(u64 timeout_ms, JS::NonnullGCPtr<JS::HeapFunction<void()>> on_timeout)
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

void HeapTimer::stop_and_fire_timeout_handler()
{
    auto on_timeout = m_on_timeout;
    stop();

    if (on_timeout)
        on_timeout->function()();
}

void HeapTimer::stop()
{
    m_on_timeout = nullptr;
    m_timer->stop();
}

}
