/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::HTML {

Bindings::WrapperWorld& Agent::main_world()
{
    if (!m_main_world)
        m_main_world = GC::Heap::the().allocate<Bindings::WrapperWorld>(Bindings::WrapperWorld::Type::Main);
    return *m_main_world;
}

Bindings::WrapperWorld const& Agent::main_world() const
{
    return const_cast<Agent&>(*this).main_world();
}

void Agent::spin_event_loop_until(GC::Root<GC::Function<bool()>> goal_condition)
{
    Platform::EventLoopPlugin::the().spin_until(move(goal_condition));
}

// https://html.spec.whatwg.org/multipage/webappapis.html#relevant-agent
Agent& relevant_agent(JS::Object const& object)
{
    // The relevant agent for a platform object platformObject is platformObject's relevant Realm's agent.
    // Spec Note: This pointer is not yet defined in the JavaScript specification; see tc39/ecma262#1357.
    return *static_cast<Agent*>(relevant_realm(object).vm().agent());
}

Agent& relevant_agent(DOM::Node const& node)
{
    return *static_cast<Agent*>(relevant_realm(node).vm().agent());
}

}
