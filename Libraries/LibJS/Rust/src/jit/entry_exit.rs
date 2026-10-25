/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Entering JIT code, and the calls JIT code makes into the runtime to leave it for the interpreter, which the
//! translator (see `translate`) serves.

use core::ptr::NonNull;

use libjs_abi::register;
use libjs_jit::code::SiteKind;

use super::code::{
    CompileState, EXIT_COUNT_BEFORE_DISCARD, EXIT_COUNT_BEFORE_DISCARD_FROM_LOOP, EntryStatus, JitEntry, JitResult,
};
use super::translate::{self, RegisterDump};
use super::{executable_of, frame_of, frames_above};
use crate::bytecode::executable::Executable;
use crate::bytecode::feedback::tier_up_costs;
use crate::bytecode::{instruction_at, op};
use crate::interpreter::vm::Vm;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::value::Value;

/// Whether the interpreter should run a new frame of this executable in its JIT code instead of interpreting it.
///
/// NB: Debuggers step through bytecode, so frames run in the interpreter while one is attached.
pub fn can_enter_jit_code(vm: &Vm, executable: &Executable) -> bool {
    executable.jit_compile_state() == CompileState::Installed && !vm.debugging_enabled()
}

/// The JIT entry table's entry of executables without JIT code, which JIT code that calls their functions directly
/// enters like their JIT code with the callee's frame. Compiled code would publish the frame before anything else sees
/// it (see `Op::PublishFrame`); this makes it the running frame and has the caller's runtime run it in the
/// interpreter (see `libjs_jit_finish_direct_call()`).
///
/// # Safety
///
/// JIT code calls this with its VM and the frame it built for the callee.
pub unsafe extern "C" fn libjs_jit_not_compiled_call_entry(vm: *const Vm, frame: *mut ExecutionContext) -> JitResult {
    // SAFETY: JIT code passes its live VM.
    let vm = unsafe { &*vm };
    vm.head.running_execution_context.set(frame);
    JitResult {
        value: 0,
        status: EntryStatus::Resume,
    }
}

/// Calls the entry of JIT code for the frame the way native code does, marking the frame as run by JIT code while
/// the code runs.
fn enter_from_native_code(vm: &Vm, frame: NonNull<ExecutionContext>, entry: JitEntry) -> JitResult {
    let frame_ref = frame_of(frame);
    // NB: The frame runs in the interpreter once the code returns other than with EntryStatus::Returned, and is
    //     popped otherwise.
    frame_ref.runs_jit_code.set(true);
    let trampoline = vm.jit.entry_trampoline();
    // SAFETY: The entry is for the frame's executable, whose frame is fully built and running.
    let result = unsafe { trampoline(core::ptr::from_ref(vm), frame.as_ptr(), entry) };
    frame_ref.runs_jit_code.set(false);
    result
}

/// Runs the JIT code of the frame's executable. The frame is the running execution context, fully built, at pc 0
/// with Enter not run yet.
pub fn enter_jit_code(vm: &Vm, frame: NonNull<ExecutionContext>) -> JitResult {
    assert_eq!(vm.running_execution_context(), Some(frame));
    let frame_ref = frame_of(frame);
    assert_eq!(frame_ref.program_counter.get(), 0);
    let executable = executable_of(frame_ref);
    // NB: The executable keeps its code attached while JIT code may run frames of it, even if it is discarded
    //     meanwhile (see Executable::detach_discarded_jit_code_if_unused()).
    let entry = executable.jit_code(vm).expect("the executable has JIT code").entry();
    enter_from_native_code(vm, frame, entry)
}

/// Continues the running frame, which the interpreter is running at the loop back edge `pc`, in its executable's JIT
/// code if that has an on-stack replacement entry there. Returns how the code returned, if it ran.
pub fn enter_jit_code_at_loop(vm: &Vm, frame: NonNull<ExecutionContext>, pc: u32) -> Option<JitResult> {
    assert_eq!(vm.running_execution_context(), Some(frame));
    let frame_ref = frame_of(frame);
    let executable = executable_of(frame_ref);
    if !can_enter_jit_code(vm, &executable) || !frame_ref.frame_initialized.get() {
        return None;
    }
    let code = executable.jit_code(vm).expect("the executable has JIT code");
    let Some(entry) = code.osr_entry(pc) else {
        // NB: A fixed budget can keep expiring at the same back edge in nested loops, missing the code's OSR entry
        //     forever. Try the next back edge when the code has an entry at a loop this one is in. Frames in other
        //     loops would check at every back edge without ever reaching an entry.
        // NB: Top-level code runs once, so no later frame would enter its code at the start.
        let runs_once = frame_ref.function.get().is_none();
        if code.reaches_osr_entry_from(pc) {
            executable.head.tier_up_budget.set(tier_up_costs::LOOP_ITERATION);
        } else if (runs_once || code.exit_count() >= EXIT_COUNT_BEFORE_DISCARD_FROM_LOOP)
            && !code.may_reach_osr_entry_from(pc)
        {
            // NB: The frame may have exited into this loop, and no later iteration of a loop it is in reaches an entry.
            //     Once the code is detached, a later check here compiles the executable again, with what the exits
            //     taught it and an entry at this loop.
            vm.jit.count_coverage("discard-from-loop");
            // NB: Top-level code that moved on to a later loop recompiles with an entry there, which is progress, so
            //     scripts with many long loops are not refused after MAX_FRUITLESS_DISCARD_COUNT of them.
            if runs_once {
                executable.note_learned_jit_exit_site();
            }
            executable.discard_jit_code(vm);
        }
        return None;
    };
    vm.jit.count_coverage("osr-entry");
    frame_ref.program_counter.set(pc);
    Some(enter_from_native_code(vm, frame, entry))
}

/// Resumes the running frame, a generator's frame suspended at the continuation `pc`, in its executable's JIT code if
/// that has a resume entry there. Returns how the code returned, if it ran.
pub fn enter_jit_code_at_continuation(vm: &Vm, frame: NonNull<ExecutionContext>, pc: u32) -> Option<JitResult> {
    enter_jit_code_with_frame_in_memory(vm, frame, pc, "resume-entry")
}

/// Continues the running frame at `pc`, where its memory holds every value live, in its executable's JIT code if that
/// has an entry there. Returns how the code returned, if it ran.
fn enter_jit_code_with_frame_in_memory(
    vm: &Vm,
    frame: NonNull<ExecutionContext>,
    pc: u32,
    coverage_key: &str,
) -> Option<JitResult> {
    assert_eq!(vm.running_execution_context(), Some(frame));
    let frame_ref = frame_of(frame);
    let executable = executable_of(frame_ref);
    if !can_enter_jit_code(vm, &executable) || !frame_ref.frame_initialized.get() {
        return None;
    }
    let entry = executable
        .jit_code(vm)
        .expect("the executable has JIT code")
        .osr_entry(pc)?;
    vm.jit.count_coverage(coverage_key);
    frame_ref.program_counter.set(pc);
    Some(enter_from_native_code(vm, frame, entry))
}

/// `asm_helper_enter_jit_code_after_catch`: the interpreter calls this when it ran the `Catch` at `pc` in the running
/// frame, whose executable has JIT code. Continues the frame in the code's catch entry after the Catch, if it has one
/// (see `libjs_jit::builder::osr`). Returns the interpreter's control word: 0 to continue interpreting after the
/// Catch, and otherwise what `control_after_entry()` returns.
pub fn helper_enter_jit_code_after_catch(vm: &Vm, pc: u32) -> i64 {
    let frame = vm.running_execution_context().expect("a frame is running");
    let executable = executable_of(frame_of(frame));
    let next_pc = pc + instruction_at::<op::Catch>(&executable, pc).length();
    if let Some(result) = enter_jit_code_with_frame_in_memory(vm, frame, next_pc, "catch-entry") {
        return control_after_entry(vm, result);
    }
    // NB: Code compiled before the handler first caught an exception has no entry after its Catch. Catches there count
    //     like exits, so that the executable is compiled again with the entry once they keep happening. Handlers
    //     without an entry for other reasons would get none from a compile either.
    if !can_enter_jit_code(vm, &executable) {
        return 0;
    }
    let code = executable.jit_code(vm).expect("the executable has JIT code");
    if code.compiled_before_handler_caught(pc) && code.count_exit() >= EXIT_COUNT_BEFORE_DISCARD {
        vm.jit.count_coverage("discard-from-catch");
        executable.discard_jit_code(vm);
    }
    0
}

/// The interpreter's control word after JIT code it entered in the running frame returned: 1 to continue interpreting
/// the running execution context, negative to leave the interpreter.
pub fn control_after_entry(vm: &Vm, result: JitResult) -> i64 {
    match result.status {
        EntryStatus::Returned => {
            // NB: Like the interpreter's Return, a frame without a caller, or whose caller waits in native code,
            //     returns from the interpreter.
            let frame = vm.running_execution_context_ref();
            let exits_interpreter = frame.caller_frame.get().is_null() || frame.returns_to_native_caller.get();
            vm.return_from_running_frame(Value::from_bits(result.value));
            if exits_interpreter { -1 } else { 1 }
        }
        EntryStatus::Resume => 1,
        EntryStatus::ExitInterpreter => -1,
    }
}

/// Runs the frame that just became the running execution context in its executable's JIT code, if it can, for the
/// interpreter's Call, which found that the executable has JIT code. Returns the control word of the interpreter's
/// `asm_helper_enter_jit_code`: negative if the interpreter leaves, 1 if the code returned (the frame is still the
/// running execution context, with the value it returned in its return value register, for the interpreter to return
/// it to the caller), and otherwise 0, to continue interpreting the running execution context.
pub fn helper_enter_jit_code(vm: &Vm) -> i64 {
    if vm.debugging_enabled() {
        return 0;
    }
    let frame = vm.running_execution_context().expect("a frame is running");
    let result = enter_jit_code(vm, frame);
    match result.status {
        EntryStatus::Returned => {
            frame_of(frame)
                .register(register::RETURN_VALUE)
                .set(Value::from_bits(result.value));
            1
        }
        EntryStatus::Resume => 0,
        EntryStatus::ExitInterpreter => -1,
    }
}

/// Called by exit and leave stubs, and by slow paths in inlined callees, with the index of their site: translates the
/// state of compiled code there into interpreter frames (see `translate::exit()`, `translate::leave()` and
/// `translate::publish()`). After an exit, the JIT function returns
/// `EntryStatus::Resume`, and the interpreter continues in the innermost frame.
///
/// # Safety
///
/// JIT code calls this with its VM, its frame, one of its code's exit or leave sites and the registers it saved.
pub unsafe extern "C" fn libjs_jit_exit(
    vm: *const Vm,
    frame: *mut ExecutionContext,
    site: u32,
    dump: *const RegisterDump,
) {
    // SAFETY: JIT code passes live pointers.
    let (vm, dump) = unsafe { (&*vm, &*dump) };
    let root = NonNull::new(frame).expect("JIT code passes its frame");
    // NB: The frame's executable keeps the code attached while JIT code runs the frame, also after it was discarded.
    let executable = executable_of(frame_of(root));
    let code = executable.jit_code(vm).expect("the code is attached");
    match code.site(site).kind {
        SiteKind::Exit(_) => translate::exit(vm, root, code, site, dump),
        SiteKind::Leave => translate::leave(vm, root, code, site, dump),
        SiteKind::Publish => translate::publish(vm, root, code, site, dump),
        SiteKind::Call => unreachable!("call sites are no exits"),
    }
}

/// The frame a call at a call site in an inlined callee of the code running `root` made, from the running frame: the
/// one linked to `root`.
fn call_site_callee_frame(vm: &Vm, root: NonNull<ExecutionContext>) -> NonNull<ExecutionContext> {
    *frames_above(vm, root)
        .expect("the frame is on the running frame's chain")
        .last()
        .expect("the callee frame is linked to the frame")
}

/// Called by compiled code on the generic paths of a call at an inlined call site, which run in the frames of its
/// inlined calls: pushes them (see `materialize_inlined_call_frames()`), and returns the innermost one, which runs.
///
/// # Safety
///
/// Compiled code calls this with its VM, its frame, one of its code's inlined call sites and its frame pointer.
pub unsafe extern "C" fn libjs_jit_push_inlined_call_frames(
    vm: *const Vm,
    frame: *mut ExecutionContext,
    site: u32,
    frame_pointer: u64,
) -> *mut ExecutionContext {
    // SAFETY: JIT code passes live pointers.
    let vm = unsafe { &*vm };
    let root = NonNull::new(frame).expect("JIT code passes its frame");
    translate::materialize_call_site_frames(vm, root, site, frame_pointer, None).as_ptr()
}

/// Called by compiled code when the callee of a direct call at an inlined call site did not return to it: materializes
/// the frames of the call's inlined calls below the callee's frame, and finishes the call in the innermost one like
/// `libjs_jit_finish_direct_call()`.
///
/// # Safety
///
/// Compiled code calls this with its VM, its frame, one of its code's inlined call sites, its frame pointer and the
/// callee's status.
pub unsafe extern "C" fn libjs_jit_finish_inlined_direct_call(
    vm: *const Vm,
    frame: *mut ExecutionContext,
    site: u32,
    frame_pointer: u64,
    status: u64,
) -> i64 {
    // SAFETY: JIT code passes live pointers.
    let vm_ref = unsafe { &*vm };
    let root = NonNull::new(frame).expect("JIT code passes its frame");
    let callee = call_site_callee_frame(vm_ref, root);
    let innermost = translate::materialize_call_site_frames(vm_ref, root, site, frame_pointer, Some(callee));
    let pc = executable_of(frame_of(root))
        .jit_code(vm_ref)
        .expect("the code is attached")
        .site(site)
        .frames[0]
        .pc;
    // SAFETY: The innermost frame of the inlined calls is the call's frame now.
    unsafe { super::calls::libjs_jit_finish_direct_call(vm, innermost.as_ptr(), pc, status) }
}

/// Called by compiled code when the native function of a native call at an inlined call site threw: materializes the
/// frames of the call's inlined calls below the native function's frame, and unwinds it like the runtime does for
/// native calls.
///
/// # Safety
///
/// Compiled code calls this with its VM, its frame, one of its code's inlined call sites, its frame pointer and the
/// exception.
pub unsafe extern "C" fn libjs_jit_inlined_raw_native_exception(
    vm: *const Vm,
    frame: *mut ExecutionContext,
    site: u32,
    frame_pointer: u64,
    exception: u64,
) -> i64 {
    // SAFETY: JIT code passes live pointers.
    let vm = unsafe { &*vm };
    let root = NonNull::new(frame).expect("JIT code passes its frame");
    let native_frame = vm
        .running_execution_context()
        .expect("the native function's frame runs");
    assert_eq!(frame_of(native_frame).caller_frame.get(), root.as_ptr());
    translate::materialize_call_site_frames(vm, root, site, frame_pointer, Some(native_frame));
    crate::interpreter::slow_paths::calls::handle_raw_native_exception(vm, Value::from_bits(exception)).0
}

/// Called by JIT code that never created the arguments object of the frame (whose arguments are unchanged) when it
/// needs one after all: creates it like CreateArguments of the given kind does and returns it as a value.
///
/// # Safety
///
/// JIT code calls this with its VM and its frame.
pub unsafe extern "C" fn libjs_jit_create_arguments(vm: *const Vm, frame: *mut ExecutionContext, mapped: u32) -> u64 {
    // SAFETY: JIT code passes live pointers.
    let (vm, frame) = unsafe { (&*vm, &*frame) };
    translate::create_arguments_object(vm, frame, mapped != 0).0
}
