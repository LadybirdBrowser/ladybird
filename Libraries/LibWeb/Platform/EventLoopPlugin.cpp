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
        // NB: A process that shuts down fast has nothing to unwind for. A worker event loop quits only once no script is
        //     waiting in here, as its agent has an ordered shutdown ahead of it.
        if (Core::EventLoop::current().was_exit_requested())
            ::exit(0);
        return goal_condition->function()();
    });
}

static thread_local bool s_discard_deferred_invocations_on_this_thread { false };

void EventLoopPlugin::set_discard_deferred_invocations_on_this_thread(bool discard)
{
    s_discard_deferred_invocations_on_this_thread = discard;
}

void EventLoopPlugin::deferred_invoke(GC::Root<GC::Function<void()>> function)
{
    Core::deferred_invoke([function = move(function)]() {
        if (s_discard_deferred_invocations_on_this_thread)
            return;
        function->function()();
    });
}

void EventLoopPlugin::quit()
{
    Core::EventLoop::current().quit(0);
}

}
