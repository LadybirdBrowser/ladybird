/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <LibCore/EventLoopImplementation.h>

#include <glibmm/refptr.h>

namespace Glib {

class MainContext;
class Source;

}

namespace Ladybird {

class EventLoopManagerGLib final : public Core::EventLoopManager {
    virtual NonnullOwnPtr<Core::EventLoopImplementation> make_implementation() override;

    virtual intptr_t register_timer(Core::EventReceiver&, int interval_milliseconds, bool should_reload) override;
    virtual void unregister_timer(intptr_t timer_id) override;

    virtual void register_notifier(Core::Notifier&) override;
    virtual void unregister_notifier(Core::Notifier&) override;

    virtual void did_post_event() override;

    virtual int register_signal(int, Function<void(int)>) override;
    virtual void unregister_signal(int) override;
};

class EventLoopImplementationGLib final : public Core::EventLoopImplementation {
public:
    static NonnullOwnPtr<EventLoopImplementationGLib> create();
    ~EventLoopImplementationGLib() override;

    virtual int exec() override;
    virtual size_t pump(PumpMode) override;
    virtual void quit(int) override;
    virtual void wake() override;
    virtual bool was_exit_requested() const override;

private:
    EventLoopImplementationGLib();

    Glib::RefPtr<Glib::MainContext> m_context;
    Glib::RefPtr<Glib::Source> m_core_event_source;
    int m_exit_code { 0 };
    bool m_should_quit { false };
};

}
