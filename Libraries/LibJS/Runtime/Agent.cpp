/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Agent.h>

namespace JS {

// 9.7.2 AgentCanSuspend ( ), https://tc39.es/ecma262/#sec-agentcansuspend
bool agent_can_suspend()
{
    // FIXME: 1. Let AR be the Agent Record of the surrounding agent.
    // FIXME: 2. Return AR.[[CanBlock]].
    return true;
}

}
