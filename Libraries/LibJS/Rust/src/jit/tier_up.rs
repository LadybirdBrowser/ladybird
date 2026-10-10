/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tier-up policy, which runs when an executable has used up its tier-up budget.

use core::ptr::NonNull;

use libjs_jit::code::CompiledCode;

use super::code::{CompileState, JitCode};
use super::compile_queue::CompileResult;
use super::executable_memory::ExecutableMemory;
use super::snapshot::capture_snapshot;
use super::{CompileJob, DeferGc, JitState};
use crate::bytecode::executable::Executable;
use crate::bytecode::feedback::tier_up_costs;
use crate::interpreter::vm::Vm;
use crate::jit::InterpreterTier;
use crate::jit::options::Options;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::native_javascript_backed_function::NativeJavaScriptBackedFunction;
use crate::runtime::shared_function_instance_data::FunctionKind;

/// `threshold` invocations.
fn threshold_budget(options: &Options) -> i32 {
    let budget = i64::from(options.threshold) * i64::from(tier_up_costs::FUNCTION_ENTRY);
    i32::try_from(budget).unwrap_or(i32::MAX)
}

/// The budget, or with the "random-thresholds" option, a random part of it.
fn randomized_budget(jit: &JitState, budget: i32) -> i32 {
    if !jit.options.random_thresholds {
        return budget;
    }
    let budget = u32::try_from(budget.max(1)).expect("the budget is positive");
    i32::try_from(jit.stress_random.between(1, budget)).expect("the budget fits")
}

/// The budget of an executable whose feedback the interpreter collects: `threshold` invocations.
pub fn initial_tier_up_budget(jit: &JitState) -> i32 {
    if !jit.options.collects_feedback() {
        return i32::MAX;
    }
    randomized_budget(jit, threshold_budget(&jit.options))
}

/// The interpreter tier and the tier-up budget a new executable starts with. While the JIT is on, new executables warm
/// up before the interpreter collects feedback for them, unless there is no warmup.
pub fn initial_tier(jit: &JitState) -> (InterpreterTier, i32) {
    if jit.options.collects_feedback() && jit.options.warmup == 0 {
        (InterpreterTier::Profiling, initial_tier_up_budget(jit))
    } else if jit.options.collects_feedback() {
        (InterpreterTier::WarmingUp, warmup_budget(jit))
    } else {
        (InterpreterTier::Plain, initial_tier_up_budget(jit))
    }
}

/// The budget of an executable that warms up (see `InterpreterTier::WarmingUp`).
fn warmup_budget(jit: &JitState) -> i32 {
    let budget = i64::from(jit.options.warmup) * i64::from(tier_up_costs::FUNCTION_ENTRY);
    randomized_budget(jit, i32::try_from(budget.max(1)).unwrap_or(i32::MAX))
}

/// How much budget an executable gets while it waits for its compile job. Its next tier-up check installs the code if
/// the job has finished.
fn compile_wait_budget(options: &Options) -> i32 {
    (threshold_budget(options) / 8).max(tier_up_costs::FUNCTION_ENTRY)
}

/// The function to compile the running frame's executable for, if it should be compiled at all.
fn function_to_compile(vm: &Vm) -> Option<Gc<EcmascriptFunctionObject>> {
    if vm.debugging_enabled() {
        return None;
    }
    // NB: Generators and async functions resume in the middle, and top-level code runs once.
    let function = as_ecmascript_function_object(vm.running_execution_context_ref().function.get()?)?;
    (function.kind() == FunctionKind::Normal).then_some(function)
}

/// Whether the running frame runs a builtin written in JavaScript.
fn runs_builtin(vm: &Vm) -> bool {
    vm.running_execution_context_ref()
        .function
        .get()
        .is_some_and(|function| function.is::<NativeJavaScriptBackedFunction>())
}

/// Makes the executable's frames run in the interpreter for good.
pub fn refuse_jit_compile(vm: &Vm, executable: &Executable) {
    if executable.jit_compile_state() != CompileState::Refused {
        vm.jit.count_coverage("refuse");
    }
    executable.set_jit_compile_state(CompileState::Refused);
    executable.head.tier_up_budget.set(i32::MAX);
    if !vm.jit.options.dump_feedback {
        executable.set_interpreter_tier(InterpreterTier::Unprofiled);
    }
}

/// Called by the interpreter once the running frame's executable has used up its tier-up budget. `pc` is the entry pc
/// for function entries and the pc of the loop back edge instruction for loops.
pub fn on_budget_exhausted(vm: &Vm, pc: u32, is_loop: bool) {
    let executable = vm.current_executable();
    let options = &vm.jit.options;

    // A warm executable starts collecting feedback, and the tier-up threshold counts from here, unless the JIT would
    // never compile it.
    if executable.interpreter_tier() == InterpreterTier::WarmingUp {
        if options.enabled && !vm.debugging_enabled() && function_to_compile(vm).is_none() {
            if runs_builtin(vm) {
                // NB: Builtins written in JavaScript are never compiled on their own, but code that calls them
                //     inlines them, and needs their feedback.
                executable.set_jit_compile_state(CompileState::Refused);
                executable.head.tier_up_budget.set(i32::MAX);
                executable.set_interpreter_tier(InterpreterTier::Profiling);
                return;
            }
            refuse_jit_compile(vm, &executable);
            return;
        }
        executable.set_interpreter_tier(InterpreterTier::Profiling);
        executable.head.tier_up_budget.set(initial_tier_up_budget(&vm.jit));
        return;
    }

    if let Some(feedback) = executable.feedback() {
        feedback.update_value_feedback();
    }
    if options.dump_feedback && !executable.has_dumped_feedback() {
        executable.set_has_dumped_feedback();
        if let Some(description) = super::feedback_dump::describe_feedback(&executable) {
            eprintln!("{description}");
        }
    }

    if !options.enabled {
        executable.head.tier_up_budget.set(initial_tier_up_budget(&vm.jit));
        return;
    }

    install_finished_compiles(vm);
    match executable.jit_compile_state() {
        CompileState::None => {}
        CompileState::Queued => {
            executable.head.tier_up_budget.set(compile_wait_budget(options));
            return;
        }
        CompileState::Discarding => {
            // NB: Once no frame may still run the discarded code, the executable is compiled again.
            if !executable.detach_discarded_jit_code_if_unused(vm) {
                executable.head.tier_up_budget.set(initial_tier_up_budget(&vm.jit));
                return;
            }
            if executable.jit_compile_state() != CompileState::None {
                return;
            }
        }
        CompileState::Installed => {
            // NB: Frames of the executable that still run in the interpreter, because they exited from its code or
            //     were already running when the code was installed, keep counting so that their loops can continue
            //     in the code at an on-stack replacement entry.
            executable.head.tier_up_budget.set(compile_wait_budget(options));
            return;
        }
        CompileState::Refused => {
            executable.head.tier_up_budget.set(i32::MAX);
            return;
        }
    }
    let Some(function) = function_to_compile(vm) else {
        refuse_jit_compile(vm, &executable);
        return;
    };
    compile(vm, executable, function, is_loop.then_some(pc), options.sync);
    if executable.jit_compile_state() == CompileState::Queued {
        executable.head.tier_up_budget.set(compile_wait_budget(options));
    }
}

/// `asm_helper_tier_up_check`: the interpreter calls this when the running frame's executable has used up its tier-up
/// budget, with the pc shifted left by one and the low bit set for a loop back edge (rather than a function entry).
/// Returns 0 to continue interpreting, and 1 to continue interpreting the running execution context at its program
/// counter with the handlers of its executable's tier.
pub fn tier_up_check(vm: &Vm, encoded_pc: u64) -> i64 {
    let pc = (encoded_pc >> 1) as u32;
    let is_loop = encoded_pc & 1 != 0;
    let tier = vm.current_executable().interpreter_tier();
    on_budget_exhausted(vm, pc, is_loop);
    // NB: A frame that loops on switches to the handlers of the executable's new tier, so that the loop collects
    //     feedback. The back edge instruction runs again from the start; it has no effects before counting.
    if is_loop && vm.current_executable().interpreter_tier() != tier {
        vm.running_execution_context_ref().program_counter.set(pc);
        return 1;
    }
    0
}

/// Whether the code of a finished compile goes into executable memory. The snapshot depends on nothing that can stop
/// holding (see the snapshot's `prototype_chain_valid`, `shape_is_stable`, `globals` and `no_htmldda_objects`), so the
/// code has no dependencies.
fn should_install(vm: &Vm, result: &CompileResult) -> bool {
    result.as_ref().is_ok_and(|compiled| {
        assert!(
            compiled.dependencies.is_empty(),
            "the snapshot lets code depend on nothing"
        );
        true
    }) && !vm.debugging_enabled()
}

fn install(vm: &Vm, executable: Gc<Executable>, compiled: CompiledCode, memory: ExecutableMemory) {
    let embedded_cells = compiled
        .embedded_cells
        .iter()
        .map(|cell| {
            // SAFETY: The snapshot kept every cell it captured alive, and the code embeds only those.
            unsafe { Gc::from_non_null(NonNull::new(cell.0 as *mut CellHeader).expect("embedded cells are not null")) }
        })
        .collect();
    let code = JitCode::new(
        memory,
        compiled.entry_offset,
        &compiled.osr_entries,
        compiled.sites,
        embedded_cells,
    );
    // NB: Code may be installed while frames of the executable loop in the interpreter, because the compile was
    //     queued (from the tier-up check of another executable, like a callee's), or because they exited from the
    //     code before. Those frames keep counting, so that they continue in the code at its on-stack replacement
    //     entry rather than run in the interpreter until they return.
    executable.install_jit_code(Box::new(code), compile_wait_budget(&vm.jit.options));
}

/// Finishes a compile whose code, if it should be installed, is in `memory`.
fn finish_compile(vm: &Vm, executable: Gc<Executable>, result: CompileResult, memory: Option<ExecutableMemory>) {
    assert_eq!(executable.jit_compile_state(), CompileState::Queued);
    let options = &vm.jit.options;
    let compiled = match result {
        Ok(compiled) => compiled,
        Err(failure) => {
            let reason = format!("{failure:?}");
            let reason = reason.split([' ', '{', '(']).next().unwrap_or_default();
            vm.jit.count_coverage(&format!("compile-failure.{reason}"));
            if options.dump_ir || options.dump_passes || options.dump_asm {
                eprintln!(
                    "LIBJS_JIT: Not compiling {}: {failure:?}",
                    super::describe_executable(&executable)
                );
            }
            refuse_jit_compile(vm, &executable);
            return;
        }
    };
    vm.jit.count_coverage("compile");
    for key in &compiled.coverage {
        vm.jit.count_coverage(key);
    }
    if let Some(dump) = &compiled.dump {
        eprintln!("JIT code for {}:\n{dump}", super::describe_executable(&executable));
    }
    if let Some(memory) = memory {
        install(vm, executable, compiled, memory);
    }
    if executable.jit_compile_state() != CompileState::Installed {
        executable.set_jit_compile_state(CompileState::None);
        executable.head.tier_up_budget.set(initial_tier_up_budget(&vm.jit));
    }
}

/// Compiles `executable`, whose frames run `function`: right away if `sync`, otherwise on the compile thread, whose
/// code a later tier-up check installs.
fn compile(
    vm: &Vm,
    executable: Gc<Executable>,
    function: Gc<EcmascriptFunctionObject>,
    osr_pc: Option<u32>,
    sync: bool,
) {
    assert_eq!(executable.jit_compile_state(), CompileState::None);
    executable.set_jit_compile_state(CompileState::Queued);

    // NB: Nothing may collect the cells the snapshot captured before something keeps them alive.
    let defer_gc = DeferGc::new(vm);
    let captured = capture_snapshot(vm, executable, function, osr_pc);
    if sync {
        // NB: Nothing collects garbage while compiling on this thread, so the snapshot's cells stay alive.
        let result = libjs_jit::compile(&captured.snapshot);
        let memory = should_install(vm, &result).then(|| {
            let code = &result.as_ref().expect("the compile succeeded").code;
            let name = super::describe_executable(&executable);
            ExecutableMemory::allocate(
                vm.jit.code_allocator(),
                &[code.as_slice()],
                &[name.as_str()],
                vm.jit.options.perf_map,
            )
            .pop()
            .expect("one code was allocated")
        });
        finish_compile(vm, executable, result, memory);
        drop(defer_gc);
        return;
    }

    let job_id = vm.jit.add_job(CompileJob {
        executable,
        cells: captured.cells,
    });
    drop(defer_gc);
    vm.jit.compile_queue().submit(job_id, captured.snapshot);
}

/// Compiles the executable of `function` right away, from the feedback it has, unless it has code, is being compiled or
/// is never compiled. Returns whether it has code afterwards. For tests (see `jit::testing`).
pub fn compile_now(vm: &Vm, function: Gc<EcmascriptFunctionObject>) -> bool {
    if !vm.jit.options.enabled || vm.debugging_enabled() || function.kind() != FunctionKind::Normal {
        return false;
    }
    let executable = function.compiled_executable(vm);
    wait_for_compile_job(vm, &executable);
    if executable.jit_compile_state() == CompileState::Discarding {
        executable.detach_discarded_jit_code_if_unused(vm);
    }
    if executable.jit_compile_state() == CompileState::None {
        executable.ensure_feedback().update_value_feedback();
        compile(vm, executable, function, None, true);
    }
    executable.jit_compile_state() == CompileState::Installed
}

/// Installs the code of the compile jobs that finished since the last call.
pub fn install_finished_compiles(vm: &Vm) {
    let Some(queue) = vm.jit.started_compile_queue() else {
        return;
    };
    install_compile_results(vm, vm.jit.compile_results_to_install(queue.take_finished()));
}

/// Waits for the compile job of `executable` to finish and installs its code, and that of every other job that
/// finished meanwhile.
pub(crate) fn wait_for_compile_job(vm: &Vm, executable: &Executable) {
    let Some(queue) = vm.jit.started_compile_queue() else {
        return;
    };
    // NB: The job may have finished already, and be held back by the "stress-install" option.
    install_compile_results(vm, vm.jit.take_held_compile_results());
    while executable.jit_compile_state() == CompileState::Queued {
        install_compile_results(vm, queue.wait_for_finished());
    }
}

fn install_compile_results(vm: &Vm, results: Vec<(u64, CompileResult)>) {
    let finished: Vec<(CompileJob, CompileResult)> = results
        .into_iter()
        // NB: Jobs abandoned meanwhile have no entry anymore.
        .filter_map(|(id, result)| Some((vm.jit.take_job(id)?, result)))
        .collect();
    if finished.is_empty() {
        return;
    }

    // NB: Code goes into executable memory for all compiles at once, which changes page protections less often.
    let installed: Vec<&(CompileJob, CompileResult)> = finished
        .iter()
        .filter(|(_, result)| should_install(vm, result))
        .collect();
    let codes: Vec<&[u8]> = installed
        .iter()
        .map(|(_, result)| result.as_ref().expect("the compile succeeded").code.as_slice())
        .collect();
    let names: Vec<String> = installed
        .iter()
        .map(|(job, _)| super::describe_executable(&job.executable))
        .collect();
    let name_views: Vec<&str> = names.iter().map(String::as_str).collect();
    let mut memories =
        ExecutableMemory::allocate(vm.jit.code_allocator(), &codes, &name_views, vm.jit.options.perf_map).into_iter();

    for (job, result) in finished {
        let memory = if should_install(vm, &result) {
            Some(memories.next().expect("there is memory for each installed code"))
        } else {
            None
        };
        finish_compile(vm, job.executable, result, memory);
    }
}
