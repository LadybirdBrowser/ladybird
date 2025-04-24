/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Agent.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

struct Agent : public JS::Agent {
    // https://html.spec.whatwg.org/multipage/webappapis.html#window-event-loop
    // The event loop of a similar-origin window agent is known as a window event loop.
    // The event loop of a dedicated worker agent, shared worker agent, or service worker agent is known as a worker event loop.
    // And the event loop of a worklet agent is known as a worklet event loop.
    GC::Root<HTML::EventLoop> event_loop;

    virtual void spin_event_loop_until(GC::Root<GC::Function<bool()>> goal_condition) override;
};

Agent& relevant_agent(JS::Object const&);

}
