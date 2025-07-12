/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullRefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibWeb/Platform/EventLoopPluginLadybird.h>
#include <LibWeb/Platform/TimerLadybird.h>

namespace Web::Platform {

EventLoopPluginLadybird::EventLoopPluginLadybird() = default;
EventLoopPluginLadybird::~EventLoopPluginLadybird() = default;

void EventLoopPluginLadybird::spin_until(GC::Root<GC::Function<bool()>> goal_condition)
{
    Core::EventLoop::current().spin_until([goal_condition = move(goal_condition)]() {
        if (Core::EventLoop::current().was_exit_requested())
            ::exit(0);
        return goal_condition->function()();
    });
}

void EventLoopPluginLadybird::deferred_invoke(GC::Root<GC::Function<void()>> function)
{
    Core::deferred_invoke([function = move(function)]() {
        function->function()();
    });
}

GC::Ref<Timer> EventLoopPluginLadybird::create_timer(GC::Heap& heap)
{
    return TimerLadybird::create(heap);
}

void EventLoopPluginLadybird::quit()
{
    Core::EventLoop::current().quit(0);
}

}
