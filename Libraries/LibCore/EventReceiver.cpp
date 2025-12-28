/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Badge.h>
#include <AK/JsonObject.h>
#include <AK/WeakPtr.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventReceiver.h>

namespace Core {

EventReceiver::EventReceiver() = default;

EventReceiver::~EventReceiver()
{
    stop_timer();
}

void EventReceiver::event(Core::Event& event)
{
    switch (event.type()) {
    case Core::Event::Timer:
        if (!m_timer_id)
            break; // Too late, the timer was already stopped.
        return timer_event(static_cast<TimerEvent&>(event));
    case Core::Event::Invalid:
        VERIFY_NOT_REACHED();
        break;
    default:
        break;
    }
}

void EventReceiver::timer_event(Core::TimerEvent&)
{
}

void EventReceiver::start_timer(int ms)
{
    if (m_timer_id) {
        dbgln("{} {:p} already has a timer!", class_name(), this);
        VERIFY_NOT_REACHED();
    }

    m_timer_id = Core::EventLoop::register_timer(*this, ms, true);
}

void EventReceiver::stop_timer()
{
    if (!m_timer_id)
        return;
    Core::EventLoop::unregister_timer(m_timer_id);
    m_timer_id = 0;
}

void EventReceiver::deferred_invoke(Function<void()> invokee)
{
    Core::deferred_invoke([invokee = move(invokee), weak_this = make_weak_ptr()] {
        auto strong_this = weak_this.strong_ref();
        if (!strong_this)
            return;
        invokee();
    });
}

void EventReceiver::dispatch_event(Core::Event& e)
{
    event(e);
}

}
