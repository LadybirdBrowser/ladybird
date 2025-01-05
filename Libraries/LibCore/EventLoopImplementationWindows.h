/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibCore/EventLoopImplementation.h>

namespace Core {

class EventLoopManagerWindows final : public EventLoopManager {
public:
    virtual ~EventLoopManagerWindows() override = default;

    virtual NonnullOwnPtr<EventLoopImplementation> make_implementation() override;

    virtual intptr_t register_timer(EventReceiver&, int milliseconds, bool should_reload, TimerShouldFireWhenNotVisible) override;
    virtual void unregister_timer(intptr_t timer_id) override;

    virtual void register_notifier(Notifier&) override;
    virtual void unregister_notifier(Notifier&) override;

    virtual void did_post_event() override;

    virtual int register_signal(int signal_number, Function<void(int)> handler) override;
    virtual void unregister_signal(int handler_id) override;
};

class EventLoopImplementationWindows final : public EventLoopImplementation {
public:
    static NonnullOwnPtr<EventLoopImplementationWindows> create() { return make<EventLoopImplementationWindows>(); }

    EventLoopImplementationWindows();
    virtual ~EventLoopImplementationWindows() override = default;

    virtual int exec() override;
    virtual size_t pump(PumpMode) override;
    virtual void quit(int) override;

    virtual void wake() override;

    virtual void post_event(EventReceiver& receiver, NonnullOwnPtr<Event>&&) override;

private:
    bool m_exit_requested { false };
    int m_exit_code { 0 };

    // The wake event handle of this event loop needs to be accessible from other threads.
    void*& m_wake_event;
};

using EventLoopManagerPlatform = EventLoopManagerWindows;

}
