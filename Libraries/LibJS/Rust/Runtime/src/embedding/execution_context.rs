/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Execution contexts, the execution context stack and the interpreter stack.
//!
//! A JSExecutionContext is the runtime's ExecutionContext. LibJS/Embedding/Layout.h gives the offset and size of each
//! of its fields as JS_LAYOUT_EXECUTION_CONTEXT_*, and its value slots, the arguments last, follow it directly. An
//! embedder reads and writes the fields there, among them the realm, the ScriptOrModule, the this value and the
//! skip-when-determining-incumbent counter. It also finds the running context at
//! JS_LAYOUT_VM_RUNNING_EXECUTION_CONTEXT_OFFSET in the VM, and with it the current realm and the arguments and this
//! value of the running code, without a call.
//!
//! Every function here must be called on the thread that runs the VM.

use core::ffi::c_void;
use core::ptr::NonNull;

use crate::embedding::abi_types::vm_from_abi;
use crate::gc::capi::GCVisitor;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::execution_context::OwnedExecutionContext;
use crate::layout::cell::Gc;
use crate::layout::execution_context::{
    ExecutionContext, SCRIPT_OR_MODULE_TAG_EMPTY, SCRIPT_OR_MODULE_TAG_MODULE, SCRIPT_OR_MODULE_TAG_SCRIPT,
    ScriptOrModule,
};
use crate::layout::host_class::JSVM;
use crate::runtime::module::Module;
use crate::script::Script;

/// An execution context, laid out as the JS_LAYOUT_EXECUTION_CONTEXT_* values of LibJS/Embedding/Layout.h describe.
pub struct JSExecutionContext {
    _opaque: [u8; 0],
}

/// A ScriptOrModule, laid out like the field of an execution context. The tag is one of the
/// JS_LAYOUT_SCRIPT_OR_MODULE_TAG_* values of LibJS/Embedding/Layout.h, and the cell is the Script or the Module, or
/// null for an empty one.
#[repr(C)]
pub struct JSScriptOrModule {
    pub tag: u8,
    pub cell: *mut c_void,
}

const _: () = assert!(size_of::<JSScriptOrModule>() == size_of::<ScriptOrModule>());
const _: () = assert!(align_of::<JSScriptOrModule>() == align_of::<ScriptOrModule>());

impl From<ScriptOrModule> for JSScriptOrModule {
    fn from(script_or_module: ScriptOrModule) -> Self {
        let (tag, cell) = match script_or_module {
            ScriptOrModule::Empty => (SCRIPT_OR_MODULE_TAG_EMPTY, core::ptr::null_mut()),
            ScriptOrModule::Script(script) => (SCRIPT_OR_MODULE_TAG_SCRIPT, script.as_ptr().cast()),
            ScriptOrModule::Module(module) => (SCRIPT_OR_MODULE_TAG_MODULE, module.as_ptr().cast()),
        };
        Self { tag, cell }
    }
}

/// # Safety
///
/// The cell of `script_or_module` must be a live Script or Module, as its tag says, unless the tag is the empty one.
pub unsafe fn script_or_module_from_abi(script_or_module: JSScriptOrModule) -> ScriptOrModule {
    let cell = || NonNull::new(script_or_module.cell).expect("a script or module has its cell");
    // SAFETY: The caller guarantees that the cell is a live record of the kind the tag names.
    unsafe {
        match script_or_module.tag {
            SCRIPT_OR_MODULE_TAG_EMPTY => ScriptOrModule::Empty,
            SCRIPT_OR_MODULE_TAG_SCRIPT => ScriptOrModule::Script(Gc::from_non_null(cell().cast::<Script>())),
            SCRIPT_OR_MODULE_TAG_MODULE => ScriptOrModule::Module(Gc::from_non_null(cell().cast::<Module>())),
            tag => panic!("{tag} is not the tag of a script or module"),
        }
    }
}

/// Decides whether an execution context is the one js_execution_context_last_matching() looks for.
pub type JSExecutionContextPredicate =
    Option<unsafe extern "C" fn(predicate_context: *mut c_void, execution_context: *mut JSExecutionContext) -> bool>;

fn execution_context_from_abi(execution_context: *mut JSExecutionContext) -> NonNull<ExecutionContext> {
    NonNull::new(execution_context.cast()).expect("the embedder passes an execution context")
}

fn execution_context_to_abi(execution_context: Option<NonNull<ExecutionContext>>) -> *mut JSExecutionContext {
    execution_context.map_or(core::ptr::null_mut(), |execution_context| {
        execution_context.as_ptr().cast()
    })
}

/// ExecutionContext::create(): a new execution context with room for `registers_and_locals_count` registers and locals,
/// `constant_count` constants and `argument_count` arguments. The caller owns it and frees it with
/// js_execution_context_destroy(). Its argument slots are uninitialized, and the caller fills them before the context
/// is pushed or visited. The garbage collector only sees what the context holds while the context is on an execution
/// context stack, or when its owner visits it with js_execution_context_visit().
///
/// # Safety
///
/// Must be called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_create(
    registers_and_locals_count: u32,
    constant_count: u32,
    argument_count: u32,
) -> *mut JSExecutionContext {
    let execution_context = OwnedExecutionContext::create(registers_and_locals_count, constant_count, argument_count);
    execution_context_to_abi(Some(execution_context.into_raw()))
}

/// ExecutionContext::copy(): a new execution context with the fields, the slot counts and the live slots of
/// `execution_context`, which the caller owns and frees with js_execution_context_destroy().
///
/// # Safety
///
/// `execution_context` must be a live execution context. Must be called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_copy(
    execution_context: *const JSExecutionContext,
) -> *mut JSExecutionContext {
    let execution_context = execution_context_from_abi(execution_context.cast_mut());
    // SAFETY: The caller passes a live execution context.
    let copy = unsafe { execution_context.as_ref() }.copy();
    execution_context_to_abi(Some(copy.into_raw()))
}

/// Frees an execution context that js_execution_context_create() or js_execution_context_copy() returned.
///
/// # Safety
///
/// `execution_context` must come from one of those functions, must not be freed already, and must not be on an
/// execution context stack, a saved one included. Must be called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_destroy(execution_context: *mut JSExecutionContext) {
    // SAFETY: The caller passes a context that one of the creating functions gave up ownership of.
    drop(unsafe { OwnedExecutionContext::from_raw(execution_context_from_abi(execution_context)) });
}

/// ExecutionContext::visit_edges(): visits every cell the execution context holds, as the owner of a context that
/// js_execution_context_create() or js_execution_context_copy() returned does from its own visit_edges().
///
/// # Safety
///
/// `execution_context` must be a live execution context, and `visitor` the visitor LibGC passed to the caller. Must be
/// called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_visit(
    execution_context: *const JSExecutionContext,
    visitor: *mut GCVisitor,
) {
    let execution_context = execution_context_from_abi(execution_context.cast_mut());
    // SAFETY: The caller passes the visitor LibGC is visiting with, which outlives this call.
    let mut visitor = unsafe { Visitor::from_raw(visitor) };
    // SAFETY: The caller passes a live execution context.
    unsafe { execution_context.as_ref() }.trace(&mut visitor);
}

/// InterpreterStack::allocate(): an execution context on the interpreter stack with room for
/// `registers_and_locals_count` registers and locals, `constant_count` constants and `argument_count` arguments, or
/// null if the stack is full. The context lives until js_execution_context_interpreter_stack_deallocate() is passed a
/// mark taken before it was allocated. Its argument slots are uninitialized, and the caller fills them before the
/// context is pushed.
///
/// # Safety
///
/// `vm` must be a live VM. Must be called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_interpreter_stack_allocate(
    vm: *mut JSVM,
    registers_and_locals_count: u32,
    constant_count: u32,
    argument_count: u32,
) -> *mut JSExecutionContext {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    execution_context_to_abi(vm.interpreter_stack().allocate(
        registers_and_locals_count,
        constant_count,
        argument_count,
    ))
}

/// InterpreterStack::deallocate(): frees every context allocated on the interpreter stack since `mark` was taken.
///
/// # Safety
///
/// `vm` must be a live VM, and `mark` a mark of the interpreter stack taken since, with none of the contexts it frees
/// on an execution context stack. Must be called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_interpreter_stack_deallocate(vm: *mut JSVM, mark: *mut c_void) {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    vm.interpreter_stack().deallocate(mark.cast());
}

/// VM::push_execution_context(): pushes `execution_context` onto the execution context stack, which makes it the
/// running execution context. The stack does not own it.
///
/// # Safety
///
/// `vm` must be a live VM, and `execution_context` a live execution context that stays live until it is popped. Must be
/// called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_push(vm: *mut JSVM, execution_context: *mut JSExecutionContext) {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    vm.push_execution_context(execution_context_from_abi(execution_context));
}

/// VM::pop_execution_context(): pops the execution context stack and returns the context it popped. The context that
/// was running when that one was pushed is running again.
///
/// # Safety
///
/// `vm` must be a live VM whose execution context stack is not empty. Must be called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_pop(vm: *mut JSVM) -> *mut JSExecutionContext {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    execution_context_to_abi(Some(vm.pop_execution_context()))
}

/// The number of contexts on the execution context stack, VM::execution_context_stack().size(). The frames of
/// JavaScript functions that the interpreter calls directly are not on it.
///
/// # Safety
///
/// `vm` must be a live VM. Must be called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_stack_size(vm: *mut JSVM) -> usize {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    vm.execution_context_stack_size()
}

/// VM::save_execution_context_stack(): sets the execution context stack aside and leaves an empty one with nothing
/// running. The garbage collector keeps tracing the contexts on the saved stack until
/// js_execution_context_restore_stack() brings it back.
///
/// # Safety
///
/// `vm` must be a live VM. Must be called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_save_stack(vm: *mut JSVM) {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    vm.save_execution_context_stack();
}

/// VM::clear_execution_context_stack(): removes every context from the execution context stack.
///
/// # Safety
///
/// `vm` must be a live VM. Must be called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_clear_stack(vm: *mut JSVM) {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    vm.clear_execution_context_stack();
}

/// VM::restore_execution_context_stack(): replaces the execution context stack with the one the last unmatched
/// js_execution_context_save_stack() set aside.
///
/// # Safety
///
/// `vm` must be a live VM with a saved execution context stack. Must be called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_restore_stack(vm: *mut JSVM) {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    vm.restore_execution_context_stack();
}

/// VM::last_execution_context_matching(): calls `predicate` with `predicate_context` and each execution context from
/// the running one down, the frames of JavaScript functions that the interpreter calls directly included, and returns
/// the first context it accepts, or null if it accepts none. The predicate may run JavaScript, as long as it leaves the
/// execution context stack as it found it.
///
/// # Safety
///
/// `vm` must be a live VM, and `predicate` a function that may be called with `predicate_context`. Must be called on the
/// thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_last_matching(
    vm: *mut JSVM,
    predicate: JSExecutionContextPredicate,
    predicate_context: *mut c_void,
) -> *mut JSExecutionContext {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    let predicate = predicate.expect("the embedder passes a predicate");
    execution_context_to_abi(vm.last_execution_context_matching(|execution_context| {
        // SAFETY: The caller passes a predicate that takes its context, and the execution context is live.
        unsafe { predicate(predicate_context, execution_context_to_abi(Some(execution_context))) }
    }))
}

/// GetActiveScriptOrModule(): the ScriptOrModule of the topmost execution context that has one, or an empty one.
///
/// # Safety
///
/// `vm` must be a live VM. Must be called on the thread that runs the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_execution_context_get_active_script_or_module(vm: *mut JSVM) -> JSScriptOrModule {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { vm_from_abi(vm) };
    vm.get_active_script_or_module().into()
}
