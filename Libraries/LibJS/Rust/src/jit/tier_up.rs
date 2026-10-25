/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tier-up policy, which runs when an executable has used up its tier-up budget.

use core::ptr::NonNull;

use libjs_jit::code::CompiledCode;

use super::code::{CompileState, JitCode, SnapshotExecutable};
use super::compile_queue::CompileResult;
use super::executable_memory::ExecutableMemory;
use super::snapshot::{CapturedSnapshot, capture_snapshot};
use super::{CompileJob, JitState};
use crate::bytecode::executable::Executable;
use crate::bytecode::feedback::tier_up_costs;
use crate::gc::defer_gc::DeferGc;
use crate::interpreter::vm::Vm;
use crate::jit::InterpreterTier;
use crate::jit::options::Options;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::execution_context::ScriptOrModule;
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::native_javascript_backed_function::NativeJavaScriptBackedFunction;
use crate::runtime::realm_watchpoints;
use crate::runtime::source_text_module::SourceTextModule;

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

/// How much budget an executable gets once it has code while frames of it may still run in the interpreter, which
/// keep counting so that their loops can continue in the code at an on-stack replacement entry.
fn compile_wait_budget(options: &Options) -> i32 {
    (threshold_budget(options) / 8).max(tier_up_costs::FUNCTION_ENTRY)
}

/// The budget of an executable whose code was discarded, or not installed. It compiles again sooner than it first did:
/// it collected feedback for longer, and its exit sites keep the new code from speculating where the old code exited.
pub fn recompile_budget(jit: &JitState) -> i32 {
    if !jit.options.collects_feedback() {
        return i32::MAX;
    }
    randomized_budget(jit, compile_wait_budget(&jit.options))
}

/// How much budget an executable gets while it waits for its compile job: one function entry, so that the next call
/// or the next few loop iterations install the code if the job has finished, and run it.
const QUEUED_BUDGET: i32 = tier_up_costs::FUNCTION_ENTRY;

/// Charges the tier-up budget of `executable`, which the interpreter is about to run from its entry for a call from
/// compiled code, for the extra invocations the "callee-boost" option counts such a call as. Code that is hot enough
/// to be compiled calls the function, so the function is likely to be hot as well.
pub fn charge_call_from_compiled_code(vm: &Vm, executable: &Executable) {
    let boost = vm.jit.options.callee_boost;
    if boost <= 1
        || !matches!(
            executable.jit_compile_state(),
            CompileState::None | CompileState::Discarding
        )
    {
        return;
    }
    let extra = i32::try_from(boost - 1)
        .unwrap_or(i32::MAX)
        .saturating_mul(tier_up_costs::FUNCTION_ENTRY);
    let budget = &executable.head.tier_up_budget;
    budget.set(budget.get().saturating_sub(extra));
}

/// What the running frame's executable is compiled for.
#[derive(Clone, Copy)]
enum CompileTarget {
    /// The executable of a function, whose frames run it.
    Function(Gc<EcmascriptFunctionObject>),
    /// The top-level code of a script or of a module, which runs once, in the running frame.
    TopLevel,
}

impl CompileTarget {
    fn function(self) -> Option<Gc<EcmascriptFunctionObject>> {
        match self {
            CompileTarget::Function(function) => Some(function),
            CompileTarget::TopLevel => None,
        }
    }
}

/// What to compile the running frame's executable for, if it should be compiled at all.
fn code_to_compile(vm: &Vm) -> Option<CompileTarget> {
    if vm.debugging_enabled() {
        return None;
    }
    let frame = vm.running_execution_context_ref();
    let Some(function) = frame.function.get() else {
        // NB: The top-level code of a script or of a module runs once, but its loops can run long. Other code without
        //     a function, like that of eval, is not compiled.
        let top_level_executable = match frame.script_or_module.get() {
            ScriptOrModule::Script(script) => Some(script.cached_executable()),
            ScriptOrModule::Module(module) => module.downcast_ref::<SourceTextModule>()?.cached_executable(),
            _ => None,
        };
        return (top_level_executable? == vm.current_executable()).then_some(CompileTarget::TopLevel);
    };
    // NB: The body of a module with top-level await runs once, as an async function. Generators, async functions and
    //     async generators resume in their code at the entries of their continuations.
    let function = as_ecmascript_function_object(function)?;
    (!function.shared_data().is_module_wrapper()).then_some(CompileTarget::Function(function))
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

/// Drops the JIT code of the top-level code of a script or of a module whose frame returned, or its compile job, and
/// never compiles it again: the code never runs again, but its executable lives as long as the functions it created.
pub fn finish_top_level_code(vm: &Vm, executable: &Executable) {
    match executable.jit_compile_state() {
        CompileState::None | CompileState::Refused => {
            // NB: Code that was never compiled has nothing to drop, and keeps its tier: refuse_jit_compile() would
            //     give it feedback arrays that nothing ever fills.
            executable.set_jit_compile_state(CompileState::Refused);
            executable.head.tier_up_budget.set(i32::MAX);
            return;
        }
        CompileState::Queued => {
            vm.jit.abandon_compile_job_of(executable);
            executable.set_jit_compile_state(CompileState::None);
        }
        CompileState::Installed => executable.discard_jit_code(vm),
        // NB: No tier-up check of this executable would detach its code later.
        CompileState::Discarding => {
            executable.detach_discarded_jit_code_if_unused(vm);
        }
    }
    if executable.jit_compile_state() == CompileState::Discarding {
        executable.refuse_jit_compile_once_detached();
    } else {
        refuse_jit_compile(vm, executable);
    }
}

/// Called by the interpreter once the running frame's executable has used up its tier-up budget. `pc` is the entry pc
/// for function entries and the pc of the loop back edge instruction for loops.
pub fn on_budget_exhausted(vm: &Vm, pc: u32, is_loop: bool) {
    if vm.jit.take_stress_invalidation() {
        super::dependencies::invalidate_random_dependency(vm);
    }
    let executable = vm.current_executable();
    let options = &vm.jit.options;

    // NB: An executable whose compile job is queued checks at every call whether the job finished, so it does nothing
    //     else here.
    if executable.jit_compile_state() == CompileState::Queued {
        install_finished_compiles(vm);
        match executable.jit_compile_state() {
            CompileState::Queued => executable.head.tier_up_budget.set(QUEUED_BUDGET),
            CompileState::Installed => executable.head.tier_up_budget.set(compile_wait_budget(options)),
            _ => {}
        }
        return;
    }

    // NB: Frames with installed code may check successive back edges to find an OSR entry. Folding all their value
    //     feedback at each check is unnecessary: a new compile's snapshot folds it before using it.
    if executable.jit_compile_state() == CompileState::Installed && !options.dump_feedback {
        install_finished_compiles(vm);
        executable.head.tier_up_budget.set(compile_wait_budget(options));
        return;
    }

    // A warm executable starts collecting feedback, and the tier-up threshold counts from here, unless the JIT would
    // never compile it.
    if executable.interpreter_tier() == InterpreterTier::WarmingUp {
        if options.enabled && !vm.debugging_enabled() && code_to_compile(vm).is_none() {
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
        CompileState::Queued => unreachable!("queued executables return early"),
        CompileState::Discarding => {
            // NB: Once no frame may still run the discarded code, the executable is compiled again.
            if !executable.detach_discarded_jit_code_if_unused(vm) {
                executable.head.tier_up_budget.set(recompile_budget(&vm.jit));
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
    let Some(code) = code_to_compile(vm) else {
        refuse_jit_compile(vm, &executable);
        return;
    };
    compile(vm, executable, code.function(), is_loop.then_some(pc), options.sync);
    if executable.jit_compile_state() == CompileState::Queued {
        executable.head.tier_up_budget.set(QUEUED_BUDGET);
    }
}

/// `asm_helper_tier_up_check`: the interpreter calls this when the running frame's executable has used up its tier-up
/// budget, with the pc shifted left by one and the low bit set for a loop back edge (rather than a function entry). At
/// a loop back edge, the running frame may continue in JIT code. Returns 0 to continue interpreting, 1 to continue
/// interpreting the running execution context at its program counter (after JIT code ran, or with other handlers),
/// and -1 to leave the interpreter.
pub fn tier_up_check(vm: &Vm, encoded_pc: u64) -> i64 {
    let pc = (encoded_pc >> 1) as u32;
    let is_loop = encoded_pc & 1 != 0;
    let frame = vm.running_execution_context().expect("a frame is running");
    let tier = vm.current_executable().interpreter_tier();
    on_budget_exhausted(vm, pc, is_loop);
    if !is_loop {
        return 0;
    }

    let Some(result) = super::entry_exit::enter_jit_code_at_loop(vm, frame, pc) else {
        // NB: A frame that loops on in the interpreter switches to the handlers of the executable's new tier, so that
        //     the loop collects feedback and can continue in JIT code later. The back edge instruction runs again from
        //     the start; it has no effects before counting.
        if vm.current_executable().interpreter_tier() != tier {
            vm.running_execution_context_ref().program_counter.set(pc);
            return 1;
        }
        return 0;
    };
    super::entry_exit::control_after_entry(vm, result)
}

/// Whether the code of a finished compile goes into executable memory. Code whose dependencies stopped holding since
/// the snapshot was taken is not installed; the executable may be compiled again with what it learned.
fn should_install(vm: &Vm, result: &CompileResult) -> bool {
    result.as_ref().is_ok_and(|compiled| {
        compiled
            .dependencies
            .iter()
            .all(|dependency| super::dependencies::holds(vm, dependency))
    }) && !vm.debugging_enabled()
}

fn install(
    vm: &Vm,
    executable: Gc<Executable>,
    compiled: CompiledCode,
    memory: ExecutableMemory,
    snapshot: SnapshotExecutables,
    defer_gc: &DeferGc<'_>,
) {
    let embedded_cells = compiled
        .embedded_cells
        .iter()
        .map(|cell| {
            // SAFETY: The snapshot kept every cell it captured alive, and the code embeds only those.
            unsafe { Gc::from_non_null(NonNull::new(cell.0 as *mut CellHeader).expect("embedded cells are not null")) }
        })
        .collect();
    let id = vm.jit.next_code_id();
    let mut code = JitCode::new(
        memory,
        compiled.entry_offset,
        &compiled.osr_entries,
        compiled.osr_loop_back_edges,
        compiled.sites,
        embedded_cells,
        snapshot,
        defer_gc,
    );
    code.set_invalidation(id, compiled.invalidation_patches);
    code.set_frame_offsets(compiled.site_index_frame_offset, compiled.register_dump_frame_offset);
    code.set_osr_outer_loop_back_edges(compiled.osr_outer_loop_back_edges);
    code.set_uncaught_handlers(compiled.uncaught_handlers);
    vm.jit.dependents.register(vm, executable, id, &compiled.dependencies);
    // NB: Code may be installed while frames of the executable loop in the interpreter, because the compile was
    //     queued (from the tier-up check of another executable, like a callee's), or because they exited from the
    //     code before. Those frames keep counting, so that they continue in the code at its on-stack replacement
    //     entry rather than run in the interpreter until they return.
    executable.install_jit_code(vm, code, compile_wait_budget(&vm.jit.options), defer_gc);
}

type SnapshotExecutables = Vec<SnapshotExecutable>;

/// Finishes a compile whose code, if it should be installed, is in `memory`.
fn finish_compile(
    vm: &Vm,
    executable: Gc<Executable>,
    result: CompileResult,
    memory: Option<ExecutableMemory>,
    snapshot_executables: SnapshotExecutables,
    defer_gc: &DeferGc<'_>,
) {
    assert_eq!(executable.jit_compile_state(), CompileState::Queued);
    let options = &vm.jit.options;
    let compiled = match result {
        Ok(compiled) => compiled,
        Err(failure) => {
            if options.coverage.is_some() {
                let reason = format!("{failure:?}");
                let reason = reason.split([' ', '{', '(']).next().unwrap_or_default();
                vm.jit.count_coverage(format_args!("compile-failure.{reason}"));
            }
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
        install(vm, executable, compiled, memory, snapshot_executables, defer_gc);
    }
    if executable.jit_compile_state() != CompileState::Installed {
        executable.set_jit_compile_state(CompileState::None);
        executable.head.tier_up_budget.set(recompile_budget(&vm.jit));
    }
}

/// Compiles `executable`, whose frames run `function`, or top-level code without one, in the running frame:
/// right away if `sync`, otherwise on a compile thread, whose code a later tier-up check installs.
fn compile(
    vm: &Vm,
    executable: Gc<Executable>,
    function: Option<Gc<EcmascriptFunctionObject>>,
    osr_pc: Option<u32>,
    sync: bool,
) {
    assert_eq!(executable.jit_compile_state(), CompileState::None);
    executable.set_jit_compile_state(CompileState::Queued);

    // NB: Asking the realm watches its intrinsics anew with new sets where the old ones fired, which allocates, so it
    //     happens before the snapshot captures which valid sets the realm holds.
    let realm = match function {
        Some(function) => function.realm().expect("an ECMAScript function has a realm"),
        None => vm
            .running_execution_context_ref()
            .realm
            .get()
            .expect("the frame of top-level code has a realm"),
    };
    realm_watchpoints::array_iterators_have_no_return(vm, realm);
    realm_watchpoints::array_iterators_have_builtin_next(vm, realm);
    realm_watchpoints::arrays_have_builtin_iterator_method(vm, realm);

    // NB: Nothing may collect the cells the snapshot captured before something keeps them alive.
    let defer_gc = DeferGc::new(vm);
    let CapturedSnapshot {
        snapshot,
        cells,
        snapshot_executables,
    } = capture_snapshot(vm, executable, function, realm, osr_pc, &defer_gc);
    if sync {
        // NB: Nothing collects garbage while compiling on this thread, so the snapshot's cells stay alive.
        let result = libjs_jit::compile(&snapshot);
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
        finish_compile(
            vm,
            executable,
            result,
            memory,
            snapshot_executables.into_inner(),
            &defer_gc,
        );
        drop(defer_gc);
        return;
    }

    let job_id = vm.jit.add_job(
        CompileJob {
            executable,
            cells: cells.into_inner(),
            snapshot_executables: snapshot_executables.into_inner(),
        },
        &defer_gc,
    );
    drop(defer_gc);
    vm.jit.compile_queue().submit(job_id, snapshot);
}

/// Compiles the executable of `function` right away, from the feedback it has, unless it has code, is being compiled or
/// is never compiled. Returns whether it has code afterwards. For tests (see `jit::testing`).
pub fn compile_now(vm: &Vm, function: Gc<EcmascriptFunctionObject>) -> bool {
    if !vm.jit.options.enabled || vm.debugging_enabled() {
        return false;
    }
    let executable = function.compiled_executable(vm);
    wait_for_compile_job(vm, &executable);
    if executable.jit_compile_state() == CompileState::Discarding {
        executable.detach_discarded_jit_code_if_unused(vm);
    }
    if executable.jit_compile_state() == CompileState::None {
        executable.ensure_feedback().update_value_feedback();
        compile(vm, executable, Some(function), None, true);
    }
    executable.jit_compile_state() == CompileState::Installed
}

/// Installs the code of the compile jobs that finished since the last call.
pub fn install_finished_compiles(vm: &Vm) {
    let Some(queue) = vm.jit.started_compile_queue() else {
        return;
    };
    install_compile_results(
        vm,
        vm.jit.compile_results_to_install(queue.take_finished()),
        &DeferGc::new(vm),
    );
}

/// Waits for the compile job of `executable` to finish and installs its code, and that of every other job that
/// finished meanwhile.
pub(crate) fn wait_for_compile_job(vm: &Vm, executable: &Executable) {
    let Some(queue) = vm.jit.started_compile_queue() else {
        return;
    };
    // NB: The job may have finished already, and be held back by the "stress-install" option.
    install_compile_results(vm, vm.jit.take_held_compile_results(), &DeferGc::new(vm));
    while executable.jit_compile_state() == CompileState::Queued {
        install_compile_results(vm, queue.wait_for_finished(), &DeferGc::new(vm));
    }
}

/// Installs the code of finished compile jobs.
fn install_compile_results(vm: &Vm, results: Vec<(u64, CompileResult)>, defer_gc: &DeferGc<'_>) {
    let finished: Vec<(CompileJob, CompileResult)> = results
        .into_iter()
        // NB: Jobs abandoned meanwhile have no entry anymore.
        .filter_map(|(id, result)| Some((vm.jit.take_job(id, defer_gc)?.into_inner(), result)))
        .collect();
    if finished.is_empty() {
        return;
    }

    // NB: Code goes into executable memory for all compiles at once, which changes page protections less often. The
    //     verdicts are taken once, so that each compile that gets memory is one the memory was allocated for.
    let verdicts: Vec<bool> = finished.iter().map(|(_, result)| should_install(vm, result)).collect();
    let installed = || {
        finished
            .iter()
            .zip(&verdicts)
            .filter(|(_, install)| **install)
            .map(|(compile, _)| compile)
    };
    let codes: Vec<&[u8]> = installed()
        .map(|(_, result)| result.as_ref().expect("the compile succeeded").code.as_slice())
        .collect();
    let names: Vec<String> = installed()
        .map(|(job, _)| super::describe_executable(&job.executable))
        .collect();
    let name_views: Vec<&str> = names.iter().map(String::as_str).collect();
    let mut memories =
        ExecutableMemory::allocate(vm.jit.code_allocator(), &codes, &name_views, vm.jit.options.perf_map).into_iter();

    for ((job, result), install) in finished.into_iter().zip(verdicts) {
        let memory = install.then(|| memories.next().expect("there is memory for each installed code"));
        finish_compile(vm, job.executable, result, memory, job.snapshot_executables, defer_gc);
    }
}
