/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Attaching a debugger, its breakpoints, and the callback through which it pauses execution.
//!
//! Everything here runs on the thread that owns the VM, and everything but js_debugger_enable() and
//! js_debugger_is_enabled() needs debugging to be enabled, as C++ reaches the debugger through VM::debugger(), which is
//! null otherwise.

use core::ffi::c_void;
use core::ptr::NonNull;
use std::rc::Rc;

use crate::breakpoint::BreakpointID;
use crate::debugger::{Debugger, PauseInfo, PauseOnExceptions, PauseReason, ResumeMode};
use crate::embedding::abi_types::{JSSourceCode, JSUtf16View, completion_into_abi, vm_from_abi, vm_into_abi};
use crate::embedding::execution_context::JSExecutionContext;
use crate::embedding::source_code::{shared_source_code_from_abi, source_code_into_abi};
use crate::interpreter::vm::Vm;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::host_class::{JSCompletion, JSVM, JSValue};
use crate::layout::value::Value;
use crate::source_range::SourceRange;
use crate::utf16::Utf16View;

// Debugger::PauseReason.
pub const JS_PAUSE_REASON_ENTRY: u8 = 0;
pub const JS_PAUSE_REASON_BREAKPOINT: u8 = 1;
pub const JS_PAUSE_REASON_DEBUGGER_STATEMENT: u8 = 2;
pub const JS_PAUSE_REASON_EXCEPTION: u8 = 3;
pub const JS_PAUSE_REASON_STEP: u8 = 4;

// Debugger::PauseOnExceptions.
pub const JS_PAUSE_ON_EXCEPTIONS_NONE: u8 = 0;
pub const JS_PAUSE_ON_EXCEPTIONS_ALL: u8 = 1;
pub const JS_PAUSE_ON_EXCEPTIONS_UNCAUGHT: u8 = 2;

// Debugger::ResumeMode.
pub const JS_RESUME_MODE_CONTINUE: u8 = 0;
pub const JS_RESUME_MODE_STEP_INTO: u8 = 1;
pub const JS_RESUME_MODE_STEP_OUT: u8 = 2;
pub const JS_RESUME_MODE_STEP_OVER: u8 = 3;

/// An optional SourceRange: none when `source_code` is null. The source code is borrowed, and the line and column
/// count from 1.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct JSDebuggerSourceRange {
    pub source_code: *const JSSourceCode,
    pub line: u32,
    pub column: u32,
}

/// A StackTraceElement: a frame's execution context, and where in its source code it is.
#[repr(C)]
pub struct JSDebuggerStackFrame {
    pub execution_context: *mut JSExecutionContext,
    pub source_range: JSDebuggerSourceRange,
}

/// Debugger::PauseInfo, without the executable. The stack frames go from the paused frame outwards, and the
/// exception is only meaningful when has_exception is set. Everything it points to lives until the pause callback
/// returns, and the frames' execution contexts until execution continues.
#[repr(C)]
pub struct JSDebuggerPauseInfo {
    pub reason: u8,
    pub bytecode_offset: u32,
    pub source_range: JSDebuggerSourceRange,
    pub stack_frames: *const JSDebuggerStackFrame,
    pub stack_frame_count: usize,
    pub breakpoint_ids: *const u32,
    pub breakpoint_id_count: usize,
    pub exception: JSValue,
    pub has_exception: bool,
    pub exception_will_be_caught: bool,
}

/// Called each time the debugger pauses, with the context given to js_debugger_set_pause_callback(). It must resume
/// with js_debugger_continue_execution() or js_debugger_continue_execution_preserving_step_state() before it returns.
/// It may run JavaScript, which never pauses again while it runs.
pub type JSDebuggerPauseCallback =
    Option<unsafe extern "C" fn(context: *mut c_void, vm: *mut JSVM, pause_info: *const JSDebuggerPauseInfo)>;

/// The id of a new breakpoint, or why the debugger refused it: a static UTF-8 message, not null-terminated, which is
/// null when the breakpoint was added.
#[repr(C)]
pub struct JSDebuggerAddBreakpointResult {
    pub breakpoint_id: u32,
    pub error_message: *const u8,
    pub error_message_length: usize,
}

/// Debugger::FrameBinding: a binding a paused frame can see, and its value, which is the empty value while it is
/// uninitialized.
#[repr(C)]
pub struct JSDebuggerFrameBinding {
    pub name: JSUtf16View,
    pub value: JSValue,
    pub is_mutable: bool,
}

/// Receives each binding, which lives for the call.
#[repr(C)]
pub struct JSDebuggerFrameBindingSink {
    pub context: *mut c_void,
    pub append: Option<unsafe extern "C" fn(context: *mut c_void, binding: *const JSDebuggerFrameBinding)>,
}

fn pause_reason_into_abi(reason: PauseReason) -> u8 {
    match reason {
        PauseReason::Entry => JS_PAUSE_REASON_ENTRY,
        PauseReason::Breakpoint => JS_PAUSE_REASON_BREAKPOINT,
        PauseReason::DebuggerStatement => JS_PAUSE_REASON_DEBUGGER_STATEMENT,
        PauseReason::Exception => JS_PAUSE_REASON_EXCEPTION,
        PauseReason::Step => JS_PAUSE_REASON_STEP,
    }
}

fn pause_on_exceptions_from_abi(mode: u8) -> PauseOnExceptions {
    match mode {
        JS_PAUSE_ON_EXCEPTIONS_NONE => PauseOnExceptions::None,
        JS_PAUSE_ON_EXCEPTIONS_ALL => PauseOnExceptions::All,
        JS_PAUSE_ON_EXCEPTIONS_UNCAUGHT => PauseOnExceptions::Uncaught,
        _ => panic!("{mode} is not a mode of pausing on exceptions"),
    }
}

fn resume_mode_from_abi(mode: u8) -> ResumeMode {
    match mode {
        JS_RESUME_MODE_CONTINUE => ResumeMode::Continue,
        JS_RESUME_MODE_STEP_INTO => ResumeMode::StepInto,
        JS_RESUME_MODE_STEP_OUT => ResumeMode::StepOut,
        JS_RESUME_MODE_STEP_OVER => ResumeMode::StepOver,
        _ => panic!("{mode} is not a resume mode"),
    }
}

fn source_range_into_abi(source_range: Option<&SourceRange>) -> JSDebuggerSourceRange {
    source_range.map_or(
        JSDebuggerSourceRange {
            source_code: core::ptr::null(),
            line: 0,
            column: 0,
        },
        |source_range| JSDebuggerSourceRange {
            source_code: source_code_into_abi(&source_range.code),
            line: source_range.start.line,
            column: source_range.start.column,
        },
    )
}

/// # Safety
///
/// `vm` must point to a live VM.
unsafe fn debugger_of<'a>(vm: *mut JSVM) -> (&'a Vm, Rc<Debugger>) {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    (vm, vm.debugger().expect("debugging is enabled"))
}

/// # Safety
///
/// `execution_context` must point to a live execution context.
unsafe fn execution_context_from_abi<'a>(execution_context: *const JSExecutionContext) -> &'a ExecutionContext {
    // SAFETY: The caller passes a live execution context.
    unsafe { execution_context.cast::<ExecutionContext>().as_ref() }.expect("an execution context is not null")
}

fn call_pause_callback(
    callback: unsafe extern "C" fn(*mut c_void, *mut JSVM, *const JSDebuggerPauseInfo),
    context: *mut c_void,
    vm: &Vm,
    pause_info: &PauseInfo,
) {
    let stack_frames: Vec<JSDebuggerStackFrame> = pause_info
        .stack_trace
        .iter()
        .map(|element| JSDebuggerStackFrame {
            execution_context: element.execution_context.as_ptr().cast(),
            source_range: source_range_into_abi(element.source_range.as_ref()),
        })
        .collect();
    let pause_info_for_c = JSDebuggerPauseInfo {
        reason: pause_reason_into_abi(pause_info.reason),
        bytecode_offset: pause_info.bytecode_offset,
        source_range: source_range_into_abi(pause_info.source_range.as_ref()),
        stack_frames: stack_frames.as_ptr(),
        stack_frame_count: stack_frames.len(),
        breakpoint_ids: pause_info.breakpoint_ids.as_ptr(),
        breakpoint_id_count: pause_info.breakpoint_ids.len(),
        exception: pause_info.exception.unwrap_or(Value::UNDEFINED).0,
        has_exception: pause_info.exception.is_some(),
        exception_will_be_caught: pause_info.exception_will_be_caught,
    };
    // SAFETY: The embedder's callback receives the context it gave the debugger, and a pause info that outlives the
    //         call, whose exception the PauseInfo on the stack keeps alive.
    unsafe { callback(context, vm_into_abi(vm), &raw const pause_info_for_c) };
}

fn add_breakpoint_result_into_abi(result: Result<BreakpointID, &'static str>) -> JSDebuggerAddBreakpointResult {
    match result {
        Ok(breakpoint_id) => JSDebuggerAddBreakpointResult {
            breakpoint_id,
            error_message: core::ptr::null(),
            error_message_length: 0,
        },
        Err(message) => JSDebuggerAddBreakpointResult {
            breakpoint_id: 0,
            error_message: message.as_ptr(),
            error_message_length: message.len(),
        },
    }
}

/// VM::enable_debugging(): attaches a debugger, unless one is attached. The interpreter checks for breakpoints from
/// the next function or script it enters. Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_enable(vm: *mut JSVM) {
    // SAFETY: The caller passes a live VM.
    unsafe { vm_from_abi(vm) }.enable_debugging();
}

/// VM::disable_debugging(): detaches the debugger, with its breakpoints, pause callback and the callback's context.
/// It must not be paused. Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_disable(vm: *mut JSVM) {
    // SAFETY: The caller passes a live VM.
    unsafe { vm_from_abi(vm) }.disable_debugging();
}

/// VM::debugging_enabled(). Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_is_enabled(vm: *mut JSVM) -> bool {
    // SAFETY: The caller passes a live VM.
    unsafe { vm_from_abi(vm) }.debugging_enabled()
}

/// Debugger::set_pause_callback(): `callback` is called with `context` each time the debugger pauses, in place of
/// the previous callback. A null callback leaves pauses unreported. The debugger keeps `context` alive until the
/// callback is replaced or debugging is disabled. Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM, and `context` must be null or a live C++ GC cell of the VM's heap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_set_pause_callback(
    vm: *mut JSVM,
    callback: JSDebuggerPauseCallback,
    context: *mut c_void,
) {
    // SAFETY: The caller passes a live VM.
    let (_, debugger) = unsafe { debugger_of(vm) };
    match callback {
        Some(callback) => debugger.set_pause_callback(move |vm, pause_info| {
            call_pause_callback(callback, context, vm, pause_info);
        }),
        None => debugger.clear_pause_callback(),
    }
    // SAFETY: The caller passes null or a live cell of the heap, which the slot keeps alive from now on.
    unsafe { debugger.pause_callback_context().set(NonNull::new(context)) };
}

/// Debugger::continue_execution(): resumes a paused debugger, stepping as `resume_mode` says. Only on the VM's
/// thread.
///
/// # Safety
///
/// `vm` must point to a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_continue_execution(vm: *mut JSVM, resume_mode: u8) {
    // SAFETY: The caller passes a live VM.
    let (_, debugger) = unsafe { debugger_of(vm) };
    debugger.continue_execution(resume_mode_from_abi(resume_mode));
}

/// Debugger::continue_execution_preserving_step_state(): resumes after the embedder filters out a pause, without
/// cancelling a step in progress. Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_continue_execution_preserving_step_state(vm: *mut JSVM) {
    // SAFETY: The caller passes a live VM.
    let (_, debugger) = unsafe { debugger_of(vm) };
    debugger.continue_execution_preserving_step_state();
}

/// Debugger::is_paused(). Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_is_paused(vm: *mut JSVM) -> bool {
    // SAFETY: The caller passes a live VM.
    let (_, debugger) = unsafe { debugger_of(vm) };
    debugger.is_paused()
}

/// Debugger::request_pause_on_next_bytecode_execution(): pauses at the next instruction with a source position that
/// any script or function runs. Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_request_pause_on_next_bytecode_execution(vm: *mut JSVM) {
    // SAFETY: The caller passes a live VM.
    let (_, debugger) = unsafe { debugger_of(vm) };
    debugger.request_pause_on_next_bytecode_execution();
}

/// Debugger::set_pause_on_exceptions(), with a JS_PAUSE_ON_EXCEPTIONS_* mode. Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_set_pause_on_exceptions(vm: *mut JSVM, mode: u8) {
    // SAFETY: The caller passes a live VM.
    let (_, debugger) = unsafe { debugger_of(vm) };
    debugger.set_pause_on_exceptions(pause_on_exceptions_from_abi(mode));
}

/// Debugger::did_finish_exception_propagation(): an exception the debugger paused at reached the embedder, so a
/// later throw of the same value pauses again. Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_did_finish_exception_propagation(vm: *mut JSVM, exception: JSValue) {
    // SAFETY: The caller passes a live VM.
    let (_, debugger) = unsafe { debugger_of(vm) };
    debugger.did_finish_exception_propagation(Value(exception));
}

/// Debugger::add_breakpoint(filename, line, column): a breakpoint in every source code with that filename, at the
/// first position at or after the line, and the column if there is one. Adding a breakpoint that exists returns its
/// id. Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM and `filename` must be valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_add_breakpoint(
    vm: *mut JSVM,
    filename: JSUtf16View,
    line: u32,
    has_column: bool,
    column: u32,
) -> JSDebuggerAddBreakpointResult {
    // SAFETY: The caller passes a live VM.
    let (_, debugger) = unsafe { debugger_of(vm) };
    // SAFETY: The caller guarantees that the view is valid for the call.
    let filename = unsafe { filename.as_view() };
    add_breakpoint_result_into_abi(debugger.add_breakpoint(filename, line, has_column.then_some(column)))
}

/// Debugger::add_breakpoint(source_code, line, column): like js_debugger_add_breakpoint(), but only in that source
/// code, which the breakpoint keeps alive. Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM and `source_code` to a live source code, such as one a pause or a breakpoint reported.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_add_breakpoint_for_source_code(
    vm: *mut JSVM,
    source_code: *const JSSourceCode,
    line: u32,
    has_column: bool,
    column: u32,
) -> JSDebuggerAddBreakpointResult {
    // SAFETY: The caller passes a live VM.
    let (_, debugger) = unsafe { debugger_of(vm) };
    // SAFETY: The caller passes a live source code.
    let source_code = unsafe { shared_source_code_from_abi(source_code) };
    add_breakpoint_result_into_abi(debugger.add_breakpoint_for_source_code(
        source_code,
        line,
        has_column.then_some(column),
    ))
}

/// Debugger::remove_breakpoint(): whether there was a breakpoint with that id. Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_remove_breakpoint(vm: *mut JSVM, breakpoint_id: u32) -> bool {
    // SAFETY: The caller passes a live VM.
    let (_, debugger) = unsafe { debugger_of(vm) };
    debugger.remove_breakpoint(breakpoint_id)
}

/// Debugger::evaluate_in_frame(): runs `source_text` as a direct eval in a frame of the paused stack, which sees and
/// may assign the frame's arguments and locals. Only while paused, on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM, `execution_context` to a context of the paused stack, and `source_text` must be valid
/// for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_evaluate_in_frame(
    vm: *mut JSVM,
    execution_context: *mut JSExecutionContext,
    source_text: JSUtf16View,
) -> JSCompletion {
    // SAFETY: The caller passes a live VM.
    let (vm, debugger) = unsafe { debugger_of(vm) };
    // SAFETY: The caller passes a context of the paused stack.
    let execution_context = unsafe { execution_context_from_abi(execution_context) };
    // SAFETY: The caller guarantees that the view is valid for the call.
    let source_text = unsafe { source_text.as_view() };
    completion_into_abi(debugger.evaluate_in_frame(vm, execution_context, source_text))
}

/// Debugger::bindings_for_frame(): hands the sink each argument and local of the frame's function that is in scope at
/// the position where the frame is. The values stay alive until the call returns. Only on the VM's thread.
///
/// # Safety
///
/// `vm` must point to a live VM, `execution_context` to a live context that runs an executable, and `sink` to a sink
/// with an append function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_debugger_bindings_for_frame(
    vm: *mut JSVM,
    execution_context: *const JSExecutionContext,
    sink: *const JSDebuggerFrameBindingSink,
) {
    // SAFETY: The caller passes a live VM.
    let (vm, debugger) = unsafe { debugger_of(vm) };
    // SAFETY: The caller passes a live execution context.
    let execution_context = unsafe { execution_context_from_abi(execution_context) };
    // SAFETY: The caller passes a live sink.
    let sink = unsafe { sink.as_ref() }.expect("a sink is not null");
    let append = sink.append.expect("a sink has an append function");
    let bindings = debugger.bindings_for_frame(vm, execution_context);
    // The copy leaves the marked list unborrowed while the sink runs, and the list keeps the values alive.
    for binding in bindings.to_vec() {
        let binding_for_c = JSDebuggerFrameBinding {
            name: JSUtf16View::of(Utf16View::of_fly_string(&binding.name)),
            value: binding.value.0,
            is_mutable: binding.is_mutable,
        };
        // SAFETY: The embedder's sink receives its context and a binding that outlives the call.
        unsafe { append(sink.context, &raw const binding_for_c) };
    }
}
