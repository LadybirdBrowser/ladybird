/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "EventLoopPluginSerenity.h"
#include <AK/NonnullRefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibWeb/Platform/TimerSerenity.h>

namespace Web::Platform {

EventLoopPluginSerenity::EventLoopPluginSerenity() = default;
EventLoopPluginSerenity::~EventLoopPluginSerenity() = default;

Coroutine<void> EventLoopPluginSerenity::spin_until(GC::Root<GC::Function<bool()>> goal_condition)
{
    while (!goal_condition->function()) {
        dbgln("Toiling away in spin_until :(");
        co_await Core::EventLoop::current().next_turn();
    }
}

void EventLoopPluginSerenity::deferred_invoke(GC::Root<GC::Function<void()>> function)
{
    Core::deferred_invoke([function = move(function)]() {
        function->function()();
    });
}

void EventLoopPluginSerenity::deferred_invoke(GC::Root<GC::Function<Coroutine<void>()>> function)
{
    Core::deferred_invoke([function = move(function)] {
        Core::EventLoop::current().adopt_coroutine(function->function()());
    });
}

GC::Ref<Timer> EventLoopPluginSerenity::create_timer(GC::Heap& heap)
{
    return TimerSerenity::create(heap);
}

void EventLoopPluginSerenity::quit()
{
    Core::EventLoop::current().quit(0);
}

}
