/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The dispatch tables of the interpreter flapc generates from interpreter.flap, which the interpreter runs each
//! executable's frames with.

use core::ffi::c_void;

use crate::interpreter::vm::Vm;

/// How many entries a dispatch table has: one per possible opcode byte.
pub const DISPATCH_TABLE_SIZE: usize = 256;

pub type DispatchTable = [*const c_void; DISPATCH_TABLE_SIZE];

unsafe extern "C" {
    /// The handlers of the interpreter, indexed by opcode.
    static js_interpreter_dispatch_table: DispatchTable;
    /// Every entry checks for breakpoints before running the instruction's handler.
    static js_interpreter_debug_dispatch_table: DispatchTable;
}

fn table_address(table: &'static DispatchTable) -> *const c_void {
    table.as_ptr().cast()
}

/// The handlers of the interpreter, indexed by opcode.
pub fn plain_handlers() -> &'static DispatchTable {
    // SAFETY: The table is immutable data in the assembled interpreter.
    unsafe { &js_interpreter_dispatch_table }
}

/// The handlers of the interpreter.
pub fn plain_dispatch_table() -> *const c_void {
    table_address(plain_handlers())
}

/// The handlers that check for breakpoints, which every executable runs with while the VM is debugging.
pub fn debug_dispatch_table() -> *const c_void {
    // SAFETY: The table is immutable data in the assembled interpreter.
    table_address(unsafe { &js_interpreter_debug_dispatch_table })
}

impl Vm {
    /// Fills in the VM's dispatch tables: the debug table at every index while the VM is debugging, and otherwise the
    /// table of each interpreter tier at its index and the plain table at every other index.
    pub(crate) fn update_dispatch_tables(&self) {
        if self.debugging_enabled() {
            for table in &self.head.dispatch_tables {
                table.set(debug_dispatch_table());
            }
            return;
        }
        for table in &self.head.dispatch_tables {
            table.set(plain_dispatch_table());
        }
        for (tier, table) in self.jit.dispatch_tables() {
            self.head.dispatch_tables[tier as usize].set(table);
        }
    }
}
