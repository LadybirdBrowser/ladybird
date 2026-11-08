/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Agent.h>
#include <LibWeb/Fetch/Engine/FetchExecutor.h>
#include <LibWeb/Fetch/Infrastructure/FetchControllerRegistry.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/Scripting/ScriptAbortHandle.h>

namespace JS {

class HostObject;

}

namespace Web::HTML {

struct WEB_API Agent : public JS::Agent {
    Bindings::WrapperWorld& main_world();
    Bindings::WrapperWorld const& main_world() const;

    // https://html.spec.whatwg.org/multipage/webappapis.html#window-event-loop
    // The event loop of a similar-origin window agent is known as a window event loop.
    // The event loop of a dedicated worker agent, shared worker agent, or service worker agent is known as a worker event loop.
    // And the event loop of a worklet agent is known as a worklet event loop.
    GC::Root<HTML::EventLoop> event_loop;

    // Set by whatever runs the agent's thread, before the agent fetches anything.
    RefPtr<Fetch::Engine::FetchExecutor> fetch_executor;

    // Rooted by Bindings::create_global_object_wrapper() until Bindings::cache_global_object_wrapper() runs.
    Vector<GC::Root<JS::HostObject>> global_object_wrappers_of_realms_being_set_up;

    Fetch::Infrastructure::FetchControllerRegistry fetch_controller_registry;

    // https://w3c.github.io/web-locks/#lock-task-queue
    RefPtr<ParallelQueue> lock_task_queue;

    virtual void spin_event_loop_until(GC::Root<GC::Function<bool()>> goal_condition) override;

    virtual bool is_similar_origin_window_agent() const { return false; }

    // Only the agent's thread may call this, the first time at least. The handle is for other threads.
    NonnullRefPtr<ScriptAbortHandle> script_abort_handle();
    void detach_script_abort_handle();

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

    RefPtr<ScriptAbortHandle> m_script_abort_handle;
};

// The agent of the current thread, which has one if it runs any web code at all.
WEB_API Agent& surrounding_agent();

Agent& relevant_agent(JS::Object const&);
Agent& relevant_agent(DOM::Node const&);

}
