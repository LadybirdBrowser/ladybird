/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ffi::c_void;

use super::execution_context::ExecutionContext;
use super::function_object::NativeFunctionTableEntry;

/// The memory that execution contexts and their value slots are bump-allocated from.
#[repr(C)]
pub struct InterpreterStack {
    pub base: Cell<*mut u8>,
    pub top: Cell<*mut u8>,
    pub limit: Cell<*mut u8>,
    pub next_frame_id: Cell<u64>,
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
}
