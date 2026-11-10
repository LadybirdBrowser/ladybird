/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::Platform {

extern "C" {

// Called by Bindings/Rust/src/platform/timer.rs.
Timer* web_platform_timer_create_single_shot(u64 milliseconds, void* cell, void (*steps)(JSVM*, void*));
void web_platform_timer_stop(Timer*);
}

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

void Timer::finalize()
{
    Base::finalize();
    stop();
    on_timeout = nullptr;
}

void Timer::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(on_timeout);
}

GC::Ref<Timer> Timer::create(GC::Heap& heap)
{
    return heap.allocate<Timer>();
}

GC::Ref<Timer> Timer::create_repeating(GC::Heap& heap, AK::Duration interval, GC::Ptr<GC::Function<void()>> timeout_handler)
{
    auto timer = heap.allocate<Timer>();
    timer->set_single_shot(false);
    timer->set_interval(interval);
    timer->on_timeout = move(timeout_handler);
    return timer;
}

GC::Ref<Timer> Timer::create_single_shot(GC::Heap& heap, AK::Duration interval, GC::Ptr<GC::Function<void()>> timeout_handler)
{
    auto timer = heap.allocate<Timer>();
    timer->set_single_shot(true);
    timer->set_interval(interval);
    timer->on_timeout = move(timeout_handler);
    return timer;
}

void Timer::start()
{
    m_timer->start();
}

void Timer::start(AK::Duration interval)
{
    m_timer->start(interval);
}

void Timer::restart()
{
    m_timer->restart();
}

void Timer::restart(AK::Duration interval)
{
    m_timer->restart(interval);
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

AK::Duration Timer::interval() const
{
    return m_timer->interval();
}

void Timer::set_interval(AK::Duration interval)
{
    m_timer->set_interval(interval);
}

bool Timer::is_single_shot() const
{
    return m_timer->is_single_shot();
}

void Timer::set_single_shot(bool single_shot)
{
    m_timer->set_single_shot(single_shot);
}

Timer* web_platform_timer_create_single_shot(u64 milliseconds, void* cell, void (*steps)(JSVM*, void*))
{
    // NB: The timer's captures, which the collector traces, keep the cell alive until the steps run or the timer stops.
    auto timeout_handler = GC::create_function(GC::Heap::the(), GC::captures(GC::Ref { *static_cast<GC::Cell*>(cell) }, steps), [](auto& cell, auto& steps) {
        steps(JS::HostABI::vm_to_abi(JS::VM::the()), cell.ptr());
    });
    auto timer = Timer::create_single_shot(GC::Heap::the(), AK::Duration::from_milliseconds(static_cast<i64>(milliseconds)), timeout_handler);
    timer->start();
    return timer.ptr();
}

void web_platform_timer_stop(Timer* timer)
{
    timer->stop();
    timer->on_timeout = nullptr;
}

}
