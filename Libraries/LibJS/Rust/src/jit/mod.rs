/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime side of the optimizing JIT. For now, that is the profiling tier of the interpreter, which collects
//! feedback for the JIT to speculate on: the JIT's options, the interpreter tiers executables move through, and the
//! dispatch tables of each.

pub mod dispatch_tables;
pub mod options;
pub mod testing;

use crate::interpreter::dispatch_tables::DispatchTable;
use options::Options;

/// Which interpreter handlers an executable's frames run with. Each tier's value is the index of its dispatch table
/// in the VM's dispatch tables.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum InterpreterTier {
    /// The plain handlers, which collect no feedback. Everything runs on them while the profiling tier is off.
    Plain = 0,
    /// The plain handlers, except that calls and returns use the profiling handlers, which switch to the dispatch
    /// table of the frame they continue in. New executables start here while the interpreter collects feedback, so
    /// that the frames of the executables they call and return to run with the handlers of those.
    WarmingUp,
    /// The profiling handlers, which collect feedback for the JIT.
    Profiling,
}

impl InterpreterTier {
    pub const ALL: [InterpreterTier; 3] = [Self::Plain, Self::WarmingUp, Self::Profiling];
}

/// The JIT's state on a VM.
pub struct JitState {
    pub options: Options,
    /// The dispatch table of executables that warm up, see `InterpreterTier::WarmingUp`. Only built while the
    /// interpreter collects feedback.
    pub(crate) warming_up_dispatch_table: Option<Box<DispatchTable>>,
}

impl JitState {
    pub fn new(options: Options) -> Self {
        let warming_up_dispatch_table = options
            .collects_feedback()
            .then(dispatch_tables::warming_up_dispatch_table);
        Self {
            options,
            warming_up_dispatch_table,
        }
    }

    /// Whether the interpreter collects feedback.
    pub fn collects_feedback(&self) -> bool {
        self.options.collects_feedback()
    }

    /// The interpreter tier a new executable starts in: while the interpreter collects feedback, new executables warm
    /// up.
    pub fn initial_tier(&self) -> InterpreterTier {
        if self.collects_feedback() {
            InterpreterTier::WarmingUp
        } else {
            InterpreterTier::Plain
        }
    }
}
