/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Swift.h>
#include <AK/Time.h>
#include <LibCore/Event.h>
#include <LibCore/Forward.h>

namespace Core {

class EventLoopImplementation;
class ThreadEventQueue;

// The event loop enables asynchronous (not parallel or multi-threaded) computing by efficiently handling events from various sources.
// Event loops are most important for GUI programs, where the various GUI updates and action callbacks run on the EventLoop,
// as well as services, where asynchronous remote procedure calls of multiple clients are handled.
// Event loops, through select(), allow programs to "go to sleep" for most of their runtime until some event happens.
// EventLoop is too expensive to use in realtime scenarios (read: audio) where even the time required by a single select() system call is too large and unpredictable.
//
// There is at most one running event loop per thread.
// Another event loop can be started while another event loop is already running; that new event loop will take over for the other event loop.
// This is mainly used in LibGUI, where each modal window stacks another event loop until it is closed.
// However, that means you need to be careful with storing the current event loop, as it might already be gone at the time of use.
// Event loops currently handle these kinds of events:
// - Deferred invocations caused by various objects. These are just a generic way of telling the EventLoop to run some function as soon as possible at a later point.
// - Timers, which repeatedly (or once after a delay) run a function on the EventLoop. Note that timers are not super accurate.
// - Filesystem notifications, i.e. whenever a file is read from, written to, etc.
// - POSIX signals, which allow the event loop to act as a signal handler and dispatch those signals in a more user-friendly way.
// - Fork events, because the child process event loop needs to clear its events and handlers.
// - Quit events, i.e. the event loop should exit.
// Any event that the event loop needs to wait on or needs to repeatedly handle is stored in a handle, e.g. s_timers.
class EventLoop {
    AK_MAKE_NONMOVABLE(EventLoop);
    AK_MAKE_NONCOPYABLE(EventLoop);

private:
    friend struct EventLoopPusher;

public:
    enum class WaitMode {
        WaitForEvents,
        PollForEvents,
    };

    EventLoop();
    ~EventLoop();

    // Pump the event loop until its exit is requested.
    int exec();

    // Process events, generally called by exec() in a loop.
    // This should really only be used for integrating with other event loops.
    // The wait mode determines whether pump() uses select() to wait for the next event.
    size_t pump(WaitMode = WaitMode::WaitForEvents);

    // Pump the event loop until some condition is met.
    void spin_until(Function<bool()>);

    // Post an event to this event loop.
    void post_event(EventReceiver& receiver, NonnullOwnPtr<Event>&&);

    void add_job(NonnullRefPtr<Promise<NonnullRefPtr<EventReceiver>>> job_promise);

    void deferred_invoke(ESCAPING Function<void()>);

    void wake();

    void quit(int);

    // The registration functions act upon the current loop of the current thread.
    static intptr_t register_timer(EventReceiver&, int milliseconds, bool should_reload, TimerShouldFireWhenNotVisible);
    static void unregister_timer(intptr_t timer_id);

    static void register_notifier(Badge<Notifier>, Notifier&);
    static void unregister_notifier(Badge<Notifier>, Notifier&);

    static int register_signal(int signo, ESCAPING Function<void(int)> handler);
    static void unregister_signal(int handler_id);

    static bool is_running();
    static EventLoop& current();

    EventLoopImplementation& impl() { return *m_impl; }

private:
    NonnullOwnPtr<EventLoopImplementation> m_impl;
} SWIFT_UNSAFE_REFERENCE;

void deferred_invoke(ESCAPING Function<void()>);

}
