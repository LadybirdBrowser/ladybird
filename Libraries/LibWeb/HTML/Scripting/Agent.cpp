/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#relevant-agent
Agent& relevant_agent(JS::Object const& object)
{
    // The relevant agent for a platform object platformObject is platformObject's relevant Realm's agent.
    // Spec Note: This pointer is not yet defined in the JavaScript specification; see tc39/ecma262#1357.
    return as<Bindings::WebEngineCustomData>(relevant_realm(object).vm().custom_data())->agent;
}

}
