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

void EventLoopPluginSerenity::spin_until(JS::SafeFunction<bool()> goal_condition)
{
    Core::EventLoop::current().spin_until(move(goal_condition));
}

void EventLoopPluginSerenity::deferred_invoke(JS::SafeFunction<void()> function)
{
    VERIFY(function);
    Core::deferred_invoke(move(function));
}

NonnullRefPtr<Timer> EventLoopPluginSerenity::create_timer()
{
    return TimerSerenity::create();
}

void EventLoopPluginSerenity::quit()
{
    Core::EventLoop::current().quit(0);
}

}
