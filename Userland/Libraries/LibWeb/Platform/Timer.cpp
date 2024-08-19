/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullRefPtr.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::Platform {

Timer::~Timer() = default;

NonnullRefPtr<Timer> Timer::create()
{
    return EventLoopPlugin::the().create_timer();
}

NonnullRefPtr<Timer> Timer::create_repeating(int interval_ms, GC::SafeFunction<void()>&& timeout_handler)
{
    auto timer = EventLoopPlugin::the().create_timer();
    timer->set_single_shot(false);
    timer->set_interval(interval_ms);
    timer->on_timeout = move(timeout_handler);
    return timer;
}

NonnullRefPtr<Timer> Timer::create_single_shot(int interval_ms, GC::SafeFunction<void()>&& timeout_handler)
{
    auto timer = EventLoopPlugin::the().create_timer();
    timer->set_single_shot(true);
    timer->set_interval(interval_ms);
    timer->on_timeout = move(timeout_handler);
    return timer;
}

}
