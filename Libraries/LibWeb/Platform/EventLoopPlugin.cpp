/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <LibCore/EventLoop.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::Platform {

EventLoopPlugin* s_the;

EventLoopPlugin& EventLoopPlugin::the()
{
    VERIFY(s_the);
    return *s_the;
}

void EventLoopPlugin::install(EventLoopPlugin& plugin)
{
    VERIFY(!s_the);
    s_the = &plugin;
}

EventLoopPlugin::~EventLoopPlugin() = default;

bool EventLoopPlugin::spin_until(GC::Root<GC::Function<bool()>> goal_condition)
{
    bool goal_condition_met = false;
    Core::EventLoop::current().spin_until([goal_condition = move(goal_condition), &goal_condition_met]() {
        if (Core::EventLoop::current().was_exit_requested())
            return true;
        goal_condition_met = goal_condition->function()();
        return goal_condition_met;
    });
    return goal_condition_met;
}

void EventLoopPlugin::deferred_invoke(GC::Root<GC::Function<void()>> function)
{
    Core::deferred_invoke([function = move(function)]() {
        function->function()();
    });
}

void EventLoopPlugin::quit()
{
    Core::EventLoop::current().quit(0);
}

}
