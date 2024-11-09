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

void EventLoopPluginSerenity::spin_until(JS::Handle<JS::HeapFunction<bool()>> goal_condition)
{
    Core::EventLoop::current().spin_until([goal_condition = move(goal_condition)]() {
        return goal_condition->function()();
    });
}

void EventLoopPluginSerenity::deferred_invoke(JS::Handle<JS::HeapFunction<void()>> function)
{
    VERIFY(function);
    Core::deferred_invoke([function = move(function)]() {
        function->function()();
    });
}

JS::NonnullGCPtr<Timer> EventLoopPluginSerenity::create_timer(JS::Heap& heap)
{
    return TimerSerenity::create(heap);
}

void EventLoopPluginSerenity::quit()
{
    Core::EventLoop::current().quit(0);
}

}
