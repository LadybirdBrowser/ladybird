/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibCore/EventLoopImplementation.h>
#include <windows.h>

namespace Core {

class EventLoopManagerWindows final : public EventLoopManager {
public:
    virtual ~EventLoopManagerWindows() override;

    virtual NonnullOwnPtr<EventLoopImplementation> make_implementation() override;

    virtual int register_timer(EventReceiver&, int milliseconds, bool should_reload, TimerShouldFireWhenNotVisible) override;
    virtual bool unregister_timer(int timer_id) override;

    virtual void register_notifier(Notifier&) override;
    virtual void unregister_notifier(Notifier&) override;

    virtual void did_post_event() override;

    virtual int register_signal(int signal_number, Function<void(int)> handler) override;
    virtual void unregister_signal(int handler_id) override;

    void wait_for_events(EventLoopImplementation::PumpMode);
    static Optional<MonotonicTime> get_next_timer_expiration();

private:
    void dispatch_signal(int signal_number);
    static void handle_signal(int signal_number);
};

class EventLoopImplementationWindows final : public EventLoopImplementation {
public:
    static NonnullOwnPtr<EventLoopImplementationWindows> create() { return make<EventLoopImplementationWindows>(); }

    EventLoopImplementationWindows();
    virtual ~EventLoopImplementationWindows();

    virtual int exec() override;
    virtual size_t pump(PumpMode) override;
    virtual void quit(int) override;

    virtual void wake() override;

    virtual void unquit() override;
    virtual bool was_exit_requested() const override;
    virtual void notify_forked_and_in_child() override;
    virtual void post_event(EventReceiver& receiver, NonnullOwnPtr<Event>&&) override;

private:
    bool m_exit_requested { false };
    int m_exit_code { 0 };

    // The wake pipe of this event loop needs to be accessible from other threads.
    HANDLE m_wake_pipe_read_handle { INVALID_HANDLE_VALUE };
    HANDLE m_wake_pipe_write_handle { INVALID_HANDLE_VALUE };
};

}
