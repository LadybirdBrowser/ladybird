/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::HTML {

bool Agent::can_block() const
{
    // similar-origin window agents can not block, see: https://html.spec.whatwg.org/multipage/webappapis.html#obtain-similar-origin-window-agent
    return false;
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

}
