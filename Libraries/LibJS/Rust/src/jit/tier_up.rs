/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tier-up policy, which runs when an executable has used up its tier-up budget.

use super::{InterpreterTier, JitState};
use crate::bytecode::feedback::tier_up_costs;
use crate::interpreter::vm::Vm;

/// `threshold` invocations.
fn threshold_budget(jit: &JitState) -> i32 {
    let budget = i64::from(jit.options.threshold) * i64::from(tier_up_costs::FUNCTION_ENTRY);
    i32::try_from(budget).unwrap_or(i32::MAX)
}

/// The budget of an executable that warms up (see `InterpreterTier::WarmingUp`): `warmup` invocations.
fn warmup_budget(jit: &JitState) -> i32 {
    let budget = i64::from(jit.options.warmup) * i64::from(tier_up_costs::FUNCTION_ENTRY);
    i32::try_from(budget.max(1)).unwrap_or(i32::MAX)
}

/// The interpreter tier and the tier-up budget a new executable starts with. While the interpreter collects feedback,
/// new executables warm up before it collects feedback for them, unless there is no warmup.
pub fn initial_tier(jit: &JitState) -> (InterpreterTier, i32) {
    if jit.collects_feedback() && jit.options.warmup == 0 {
        (InterpreterTier::Profiling, threshold_budget(jit))
    } else if jit.collects_feedback() {
        (InterpreterTier::WarmingUp, warmup_budget(jit))
    } else {
        (InterpreterTier::Plain, i32::MAX)
    }
}

/// Called by the interpreter once the running frame's executable has used up its tier-up budget.
fn on_budget_exhausted(vm: &Vm) {
    let executable = vm.current_executable();

    // A warm executable starts collecting feedback, and the tier-up threshold counts from here.
    if executable.interpreter_tier() == InterpreterTier::WarmingUp {
        executable.set_interpreter_tier(InterpreterTier::Profiling);
        executable.head.tier_up_budget.set(threshold_budget(&vm.jit));
        return;
    }

    // NB: The executable is hot. Until there is a compiler to hand it to, it keeps collecting feedback.
    executable
        .feedback()
        .expect("executables in the profiling tier have feedback")
        .update_value_feedback();
    executable.head.tier_up_budget.set(threshold_budget(&vm.jit));
}

/// `asm_helper_tier_up_check`: the interpreter calls this when the running frame's executable has used up its tier-up
/// budget, with the pc shifted left by one and the low bit set for a loop back edge (rather than a function entry).
/// Returns 0 to continue interpreting, and 1 to continue interpreting the running execution context at its program
/// counter with the handlers of its executable's tier.
pub fn tier_up_check(vm: &Vm, encoded_pc: u64) -> i64 {
    let pc = (encoded_pc >> 1) as u32;
    let is_loop = encoded_pc & 1 != 0;
    let tier = vm.current_executable().interpreter_tier();
    on_budget_exhausted(vm);
    // NB: A frame that loops on switches to the handlers of the executable's new tier, so that the loop collects
    //     feedback. The back edge instruction runs again from the start; it has no effects before counting.
    if is_loop && vm.current_executable().interpreter_tier() != tier {
        vm.running_execution_context_ref().program_counter.set(pc);
        return 1;
    }
    0
}
