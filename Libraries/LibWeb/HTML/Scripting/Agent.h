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

struct WEB_API Agent : public JS::Agent {
    Bindings::WrapperWorld& main_world();
    Bindings::WrapperWorld const& main_world() const;

    // https://html.spec.whatwg.org/multipage/webappapis.html#window-event-loop
    // The event loop of a similar-origin window agent is known as a window event loop.
    // The event loop of a dedicated worker agent, shared worker agent, or service worker agent is known as a worker event loop.
    // And the event loop of a worklet agent is known as a worklet event loop.
    GC::Root<HTML::EventLoop> event_loop;

    virtual void spin_event_loop_until(GC::Root<GC::Function<bool()>> goal_condition) override;

protected:
    using JS::Agent::Agent;

private:
    // Main-world wrapper identity is shared by all realms that can directly
    // observe each other's object graphs. For windows that boundary is the
    // similar-origin window agent, not Page; workers naturally get their own
    // worker agent and therefore their own main-world cell. Cross-origin
    // windows in one WebContent process currently share the process's
    // SimilarOriginWindowAgent; proper agent-cluster separation is future work.
    mutable GC::Root<Bindings::WrapperWorld> m_main_world;
};

Agent& relevant_agent(JS::Object const&);
Agent& relevant_agent(DOM::Node const&);

}
