/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::interpreter::vm::Vm;

/// The fields of the surrounding agent's Agent Record that the runtime reads, 9.7 Agents.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AgentRecord {
    pub can_block: bool, // [[CanBlock]]
}

/// The agent of a VM whose embedder provides none, such as standalone LibJS.
impl Default for AgentRecord {
    fn default() -> Self {
        Self { can_block: true }
    }
}

// 9.7.2 AgentCanSuspend ( ), https://tc39.es/ecma262/#sec-agentcansuspend
pub fn agent_can_suspend(vm: &Vm) -> bool {
    // 1. Let AR be the Agent Record of the surrounding agent.
    let agent = vm.agent();

    // 2. Return AR.[[CanBlock]].
    // NOTE: We default to true if no agent has been provided (standalone LibJS with no embedder).
    agent.can_block
}
