/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <LibCore/EventLoopImplementation.h>

namespace Ladybird {

class EventLoopManagerMacOS final : public Core::EventLoopManager {
public:
    virtual NonnullOwnPtr<Core::EventLoopImplementation> make_implementation() override;

    virtual intptr_t register_timer(Core::EventReceiver&, int interval_milliseconds, bool should_reload) override;
    virtual void unregister_timer(intptr_t timer_id) override;

    virtual void register_notifier(Core::Notifier&) override;
    virtual void unregister_notifier(Core::Notifier&) override;

    virtual void did_post_event() override;

    virtual int register_signal(int, Function<void(int)>) override;
    virtual void unregister_signal(int) override;
};

class EventLoopImplementationMacOS final : public Core::EventLoopImplementation {
public:
    static NonnullOwnPtr<EventLoopImplementationMacOS> create();

    virtual int exec() override;
    virtual size_t pump(PumpMode) override;
    virtual void quit(int) override;
    virtual void wake() override;
    virtual void deferred_invoke(Function<void()>&&) override;
    virtual bool was_exit_requested() const override;

    void set_main_loop();

    virtual ~EventLoopImplementationMacOS() override;

private:
    EventLoopImplementationMacOS();

    struct Impl;
    NonnullOwnPtr<Impl> m_impl;

    bool m_main_loop { false };
};

}
