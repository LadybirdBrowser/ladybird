/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The dispatch tables of the interpreter flapc generates from interpreter.flap, which the interpreter runs each
//! executable's frames with.

use core::ffi::c_void;
use core::sync::atomic::{AtomicPtr, Ordering, fence};

use crate::bytecode::instruction::NUM_OPCODES;
use crate::interpreter::vm::Vm;

/// How many entries a dispatch table has: one per possible opcode byte.
pub const DISPATCH_TABLE_SIZE: usize = 256;

pub type DispatchTable = [*const c_void; DISPATCH_TABLE_SIZE];

// NB: The tables are immutable data in the assembled interpreter.
unsafe extern "C" {
    /// The handlers of the interpreter, indexed by opcode.
    safe static js_interpreter_dispatch_table: DispatchTable;
    /// Every entry checks for breakpoints before running the instruction's handler.
    safe static js_interpreter_debug_dispatch_table: DispatchTable;
}

fn table_address(table: &'static DispatchTable) -> *const c_void {
    table.as_ptr().cast()
}

/// The handlers of the interpreter, indexed by opcode.
pub fn plain_handlers() -> &'static DispatchTable {
    &js_interpreter_dispatch_table
}

/// The handlers of the interpreter.
pub fn plain_dispatch_table() -> *const c_void {
    table_address(plain_handlers())
}

/// The handlers that check for breakpoints, which every executable runs with while the VM is debugging.
pub fn debug_dispatch_table() -> *const c_void {
    table_address(&js_interpreter_debug_dispatch_table)
}

/// The most tables a VM runs executables with: one for each interpreter tier (see `InterpreterTier`).
const MAX_TIER_COUNT: usize = 4;

/// The dispatch tables a VM's interpreter runs with. They start as copies of the tables above, which are immutable and
/// shared by all VMs, so that another thread can stop this VM's interpreter wherever it is. The table a running
/// interpreter dispatches through lives in a register, out of reach of other threads, but what that table holds is
/// not. That makes the interrupt free for the interpreter (see `Vm::request_interrupt()`): a check at every back edge
/// costs tight loops several percent.
pub struct VmDispatchTables {
    tables: Box<[[AtomicPtr<c_void>; DISPATCH_TABLE_SIZE]; MAX_TIER_COUNT]>,
}

impl VmDispatchTables {
    pub fn new() -> Self {
        Self {
            tables: Box::new(core::array::from_fn(|_| {
                core::array::from_fn(|_| AtomicPtr::new(core::ptr::null_mut()))
            })),
        }
    }

    /// Makes the table at `index` a copy of `source`, and returns it.
    fn copy(&self, index: usize, source: *const c_void) -> *const c_void {
        let table = &self.tables[index];
        // SAFETY: Dispatch tables have DISPATCH_TABLE_SIZE entries, and this only reads the immutable ones.
        let source = unsafe { &*source.cast::<DispatchTable>() };
        for (entry, handler) in table.iter().zip(source) {
            entry.store(handler.cast_mut(), Ordering::Relaxed);
        }
        table.as_ptr().cast()
    }

    /// Makes the interpreter call the fallback handler at its next dispatch in every table, which any thread may do.
    pub fn stop_dispatching(&self) {
        const { assert!((NUM_OPCODES as usize) < DISPATCH_TABLE_SIZE) };
        let fallback_handler = plain_handlers()[DISPATCH_TABLE_SIZE - 1].cast_mut();
        for entry in self.tables.iter().flatten() {
            entry.store(fallback_handler, Ordering::Relaxed);
        }
    }
}

impl Default for VmDispatchTables {
    fn default() -> Self {
        Self::new()
    }
}

impl Vm {
    /// Fills in the VM's dispatch tables: copies of the debug table while the VM is debugging, and otherwise of the
    /// table of each interpreter tier at its index, with the plain table at every other index.
    pub(crate) fn update_dispatch_tables(&self) {
        let source_of = |table| {
            if self.debugging_enabled() {
                debug_dispatch_table()
            } else {
                table
            }
        };
        let plain_table = self.own_dispatch_tables.copy(0, source_of(plain_dispatch_table()));
        for table in &self.head.dispatch_tables {
            table.set(plain_table);
        }
        for (tier, table) in self.jit.dispatch_tables() {
            let table = self.own_dispatch_tables.copy(tier as usize, source_of(table));
            self.head.dispatch_tables[tier as usize].set(table);
        }

        // NB: An interrupt that came in while this ran must not get lost.
        fence(Ordering::SeqCst);
        if self.interrupt_requested() {
            self.own_dispatch_tables.stop_dispatching();
        }
    }
}
