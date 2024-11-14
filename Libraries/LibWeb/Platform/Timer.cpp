/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::Platform {

Timer::~Timer() = default;

void Timer::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(on_timeout);
}

GC::Ref<Timer> Timer::create(GC::Heap& heap)
{
    return EventLoopPlugin::the().create_timer(heap);
}

GC::Ref<Timer> Timer::create_repeating(GC::Heap& heap, int interval_ms, GC::Ptr<GC::Function<void()>> timeout_handler)
{
    auto timer = EventLoopPlugin::the().create_timer(heap);
    timer->set_single_shot(false);
    timer->set_interval(interval_ms);
    timer->on_timeout = move(timeout_handler);
    return timer;
}

GC::Ref<Timer> Timer::create_single_shot(GC::Heap& heap, int interval_ms, GC::Ptr<GC::Function<void()>> timeout_handler)
{
    auto timer = EventLoopPlugin::the().create_timer(heap);
    timer->set_single_shot(true);
    timer->set_interval(interval_ms);
    timer->on_timeout = move(timeout_handler);
    return timer;
}

}
