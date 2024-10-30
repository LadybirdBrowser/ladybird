/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::Platform {

Timer::~Timer() = default;

JS::NonnullGCPtr<Timer> Timer::create(JS::Heap& heap)
{
    return EventLoopPlugin::the().create_timer(heap);
}

JS::NonnullGCPtr<Timer> Timer::create_repeating(JS::Heap& heap, int interval_ms, JS::SafeFunction<void()>&& timeout_handler)
{
    auto timer = EventLoopPlugin::the().create_timer(heap);
    timer->set_single_shot(false);
    timer->set_interval(interval_ms);
    timer->on_timeout = move(timeout_handler);
    return timer;
}

JS::NonnullGCPtr<Timer> Timer::create_single_shot(JS::Heap& heap, int interval_ms, JS::SafeFunction<void()>&& timeout_handler)
{
    auto timer = EventLoopPlugin::the().create_timer(heap);
    timer->set_single_shot(true);
    timer->set_interval(interval_ms);
    timer->on_timeout = move(timeout_handler);
    return timer;
}

}
