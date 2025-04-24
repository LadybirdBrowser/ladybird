/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Agent.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

Agent::~Agent() = default;

// 9.7.2 AgentCanSuspend ( ), https://tc39.es/ecma262/#sec-agentcansuspend
bool agent_can_suspend(VM const& vm)
{
    // 1. Let AR be the Agent Record of the surrounding agent.
    auto const* agent = vm.agent();

    // 2. Return AR.[[CanBlock]].
    // NOTE: We default to true if no agent has been provided (standalone LibJS with no embedder).
    if (!agent)
        return true;
    return agent->can_block() == Agent::CanBlock::Yes;
}

}
