/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibCore/EventLoopImplementation.h>

#include <glib.h>

namespace Ladybird {

class EventLoopImplementationGtk;

class EventLoopManagerGtk final : public Core::EventLoopManager {
public:
    virtual NonnullOwnPtr<Core::EventLoopImplementation> make_implementation() override;

    virtual intptr_t register_timer(Core::EventReceiver&, int milliseconds, bool should_reload) override;
    virtual void unregister_timer(intptr_t timer_id) override;

    virtual void register_notifier(Core::Notifier&) override;
    virtual void unregister_notifier(Core::Notifier&) override;

    virtual void did_post_event() override;

    virtual int register_signal(int signal_number, Function<void(int)> handler) override;
    virtual void unregister_signal(int handler_id) override;

private:
    bool m_idle_pending { false };
};

class EventLoopImplementationGtk final : public Core::EventLoopImplementation {
public:
    static NonnullOwnPtr<EventLoopImplementationGtk> create() { return adopt_own(*new EventLoopImplementationGtk); }

    virtual ~EventLoopImplementationGtk() override;

    virtual int exec() override;
    virtual size_t pump(PumpMode) override;
    virtual void quit(int) override;
    virtual void wake() override;
    virtual bool was_exit_requested() const override;

    void set_main_loop();

private:
    friend class EventLoopManagerGtk;

    EventLoopImplementationGtk();

    GMainLoop* m_loop { nullptr };
    bool m_exit_requested { false };
    int m_exit_code { 0 };
};

}
