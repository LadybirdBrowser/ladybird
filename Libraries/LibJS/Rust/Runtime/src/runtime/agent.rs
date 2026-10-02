/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::interpreter::vm::Vm;

// 9.7.2 AgentCanSuspend ( ), https://tc39.es/ecma262/#sec-agentcansuspend
pub fn agent_can_suspend(_vm: &Vm) -> bool {
    // 1. Let AR be the Agent Record of the surrounding agent.
    // 2. Return AR.[[CanBlock]].
    // NOTE: We default to true if no agent has been provided (standalone LibJS with no embedder).
    // NB: No embedder of the Rust runtime provides an agent yet.
    true
}
