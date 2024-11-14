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

void EventLoopPluginSerenity::spin_until(GC::Root<GC::Function<bool()>> goal_condition)
{
    Core::EventLoop::current().spin_until([goal_condition = move(goal_condition)]() {
        return goal_condition->function()();
    });
}

void EventLoopPluginSerenity::deferred_invoke(GC::Root<GC::Function<void()>> function)
{
    VERIFY(function);
    Core::deferred_invoke([function = move(function)]() {
        function->function()();
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
