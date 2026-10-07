/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ffi::c_void;

use super::cell::Gc;
use super::execution_context::ExecutionContext;
use super::function_object::NativeFunctionTableEntry;
use super::realm::Realm;

/// The memory that execution contexts and their value slots are bump-allocated from.
#[repr(C)]
pub struct InterpreterStack {
    pub base: Cell<*mut u8>,
    pub top: Cell<*mut u8>,
    pub limit: Cell<*mut u8>,
    pub next_frame_id: Cell<u64>,
}

/// An entry of the execution context stack: a context pushed onto it, and the context that was running when it was
/// pushed.
#[repr(C)]
pub struct ExecutionContextStackEntry {
    pub execution_context: Cell<*mut ExecutionContext>,
    pub previous_running_execution_context: Cell<*mut ExecutionContext>,
}

/// The part of the VM that the interpreter reads and writes. The rest of the VM follows it.
#[repr(C)]
pub struct VmHead {
    pub running_execution_context: Cell<*mut ExecutionContext>,
    pub interpreter_stack: InterpreterStack,
    /// The lowest address of the stack of the thread that runs JavaScript.
    pub stack_base: Cell<usize>,
    pub execution_generation: Cell<u64>,
    pub primitive_storage_cage_base: Cell<usize>,
    pub heap_region_base: Cell<usize>,
    pub native_function_table_data: Cell<*const NativeFunctionTableEntry>,
    /// Non-null while a debugger is attached; the interpreter then dispatches through its breakpoint-checking table.
    pub debugger: Cell<*mut c_void>,
    /// The execution context stack: storage for `execution_context_stack_capacity` entries, of which the first
    /// `execution_context_stack_length` are on the stack. Only the VM grows the storage, but an embedder may push and
    /// pop entries in place while there is room, and walk the stack.
    pub execution_context_stack_entries: Cell<*mut ExecutionContextStackEntry>,
    pub execution_context_stack_length: Cell<usize>,
    pub execution_context_stack_capacity: Cell<usize>,
    /// The realm TypeErrors are created in while a TypeErrorRealmScope is active, at the execution context stack
    /// length it was created at. An embedder's TypeErrorRealmScope sets and restores both directly.
    pub type_error_realm_override: Cell<Option<Gc<Realm>>>,
    pub type_error_realm_override_depth: Cell<usize>,
}

/// The storage an embedder reserves to construct a Vm in place, and its alignment. build.rs cannot see the Vm, only its
/// head, so these are a capacity that the crate checks the Vm against rather than its exact size and alignment.
pub const VM_SIZE: usize = 16 * 1024;
pub const VM_ALIGN: usize = 16;
