/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The dispatch tables of the interpreter tiers executables move through while the interpreter collects feedback (see
//! `InterpreterTier`), made from the handlers of the profiling build of the interpreter, which only builds with the JIT
//! have.

use core::ffi::c_void;

use super::{InterpreterTier, JitState};
use crate::bytecode::instruction::{NUM_OPCODES, instruction_name_from_opcode};
use crate::interpreter::dispatch_tables::{DispatchTable, plain_dispatch_table, plain_handlers};

unsafe extern "C" {
    /// The handlers of the profiling build of the interpreter, which collect feedback for the optimizing JIT.
    static js_interpreter_dispatch_table_profiling: DispatchTable;
}

/// The handlers of the profiling build of the interpreter.
fn profiling_dispatch_table() -> *const c_void {
    // SAFETY: The table is immutable data in the assembled interpreter.
    unsafe { js_interpreter_dispatch_table_profiling.as_ptr().cast() }
}

/// The plain handlers, with the profiling build's handlers for the instructions that count the tier-up budget
/// (function entry and loop back edges), or call or return to a frame that may run with other handlers (see
/// `InterpreterTier::WarmingUp`).
pub fn warming_up_dispatch_table() -> Box<DispatchTable> {
    mixed_dispatch_table(|name| matches!(name, "Enter" | "Call" | "Return" | "End") || name.contains("Loop"))
}

/// The plain handlers, with the profiling build's handlers for the instructions whose names `use_profiling` selects.
fn mixed_dispatch_table(use_profiling: impl Fn(&str) -> bool) -> Box<DispatchTable> {
    // SAFETY: The table is immutable data in the assembled interpreter.
    let profiling = unsafe { &js_interpreter_dispatch_table_profiling };
    let mut table = Box::new(*plain_handlers());
    for opcode in 0..NUM_OPCODES as usize {
        if use_profiling(instruction_name_from_opcode(opcode as u8)) {
            table[opcode] = profiling[opcode];
        }
    }
    table
}

impl JitState {
    /// The dispatch table the interpreter runs the frames of executables in each tier with, with the plain handlers for
    /// the tiers whose tables are not built.
    ///
    /// NB: Executables only move to the warming up tier while the interpreter collects feedback, which builds its
    ///     table.
    pub fn dispatch_tables(&self) -> impl Iterator<Item = (InterpreterTier, *const c_void)> {
        let mixed_table = |table: &Option<Box<DispatchTable>>| {
            table
                .as_deref()
                .map_or(plain_dispatch_table(), |table| table.as_ptr().cast())
        };
        InterpreterTier::ALL.into_iter().map(move |tier| {
            let table = match tier {
                InterpreterTier::Plain => plain_dispatch_table(),
                InterpreterTier::WarmingUp => mixed_table(&self.warming_up_dispatch_table),
                InterpreterTier::Profiling => profiling_dispatch_table(),
            };
            (tier, table)
        })
    }
}
