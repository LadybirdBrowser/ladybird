/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibCore/Forward.h>

namespace Core {

class EventLoopImplementation;
class ThreadEventQueue;

class EventLoopManager {
public:
    static EventLoopManager& the();
    static void install(EventLoopManager&);

    virtual ~EventLoopManager();

    virtual NonnullOwnPtr<EventLoopImplementation> make_implementation() = 0;

    virtual intptr_t register_timer(EventReceiver&, int milliseconds, bool should_reload, TimerShouldFireWhenNotVisible) = 0;
    virtual void unregister_timer(intptr_t timer_id) = 0;

    virtual void register_notifier(Notifier&) = 0;
    virtual void unregister_notifier(Notifier&) = 0;

    virtual void did_post_event() = 0;

    // FIXME: These APIs only exist for obscure use-cases inside SerenityOS. Try to get rid of them.
    virtual int register_signal(int signal_number, Function<void(int)> handler) = 0;
    virtual void unregister_signal(int handler_id) = 0;

protected:
    EventLoopManager();
};

class EventLoopImplementation {
public:
    virtual ~EventLoopImplementation();

    enum class PumpMode {
        WaitForEvents,
        DontWaitForEvents,
    };

    virtual int exec() = 0;
    virtual size_t pump(PumpMode) = 0;
    virtual void quit(int) = 0;
    virtual void wake() = 0;

    virtual void post_event(EventReceiver& receiver, NonnullOwnPtr<Event>&&) = 0;

protected:
    EventLoopImplementation();
    ThreadEventQueue& m_thread_event_queue;
};

}
