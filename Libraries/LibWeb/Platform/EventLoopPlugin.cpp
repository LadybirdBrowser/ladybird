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

void EventLoopPlugin::spin_until(GC::Root<GC::Function<bool()>> goal_condition)
{
    Core::EventLoop::current().spin_until([goal_condition = move(goal_condition)]() {
        if (Core::EventLoop::current().was_exit_requested())
            ::exit(0);
        return goal_condition->function()();
    });
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
