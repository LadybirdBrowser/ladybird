/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime side of the optimizing JIT in builds without it (the crate's "jit" feature off): the items of
//! `jit/mod.rs` that the rest of the runtime uses, with nothing behind them. Every executable runs with the plain
//! handlers of the interpreter, which is the only variant of it these builds have, nothing collects feedback or
//! counts tier-up budgets, and no code is ever compiled, so the functions that only run for compiled code are
//! unreachable.

use core::ffi::c_void;
use core::ptr::NonNull;

use crate::bytecode::executable::Executable;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::dispatch_tables::plain_dispatch_table;
use crate::interpreter::vm::Vm;

/// Whether this build has the JIT.
pub const BUILT: bool = false;

/// Which interpreter handlers an executable's frames run with: the plain ones.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum InterpreterTier {
    Plain = 0,
}

/// The JIT's state on a VM, which has none.
pub struct JitState;

impl JitState {
    pub fn new(_options: options::Options) -> Self {
        Self
    }

    pub fn collects_feedback(&self) -> bool {
        false
    }

    pub fn dispatch_tables(&self) -> impl Iterator<Item = (InterpreterTier, *const c_void)> {
        [(InterpreterTier::Plain, plain_dispatch_table())].into_iter()
    }

    pub fn abandon_compile_jobs(&self) {}

    pub fn flush_coverage(&self) {}
}

unsafe impl Trace for JitState {
    fn trace(&self, _visitor: &mut Visitor) {}
}

/// Keeps the heap from collecting garbage while it lives, which only matters for frames of inlined calls.
pub struct DeferGc;

impl DeferGc {
    pub fn new(_vm: &Vm) -> Self {
        Self
    }
}

pub mod options {
    /// The options of the JIT, which these builds ignore.
    pub struct Options;

    impl Options {
        /// Warns once if LIBJS_JIT asks for the JIT, which this build does not have.
        pub fn from_environment() -> Self {
            static WARNING: std::sync::Once = std::sync::Once::new();
            if let Ok(value) = std::env::var("LIBJS_JIT")
                && value.trim() != "off"
            {
                WARNING.call_once(|| eprintln!("LIBJS_JIT: This build has no JIT, ignoring LIBJS_JIT={value}"));
            }
            Self
        }
    }
}

pub mod code {
    use super::{Executable, Visitor, Vm};

    /// No return pc marks a call site in an inlined callee.
    pub const INLINED_CALL_SITE_BIT: u32 = 0;

    pub enum EntryStatus {
        Returned,
        Resume,
        ExitInterpreter,
    }

    pub struct JitResult {
        pub value: u64,
        pub status: EntryStatus,
    }

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub enum CompileState {
        None,
        Installed,
    }

    #[derive(Default)]
    pub struct ExecutableJitState;

    impl Executable {
        pub fn allocate_jit_entry_slot(&self, _vm: &Vm) {}

        pub fn free_jit_entry_slot(&self, _vm: &Vm) {}

        pub fn trace_jit_code(&self, _visitor: &mut Visitor) {}

        pub fn jit_compile_state(&self) -> CompileState {
            CompileState::None
        }

        pub fn discard_jit_code(&self, _vm: &Vm) {
            unreachable!("no executable has JIT code");
        }
    }
}

pub mod tier_up {
    use super::{InterpreterTier, JitState, Vm};

    /// The interpreter tier and the tier-up budget a new executable starts with: it never tiers up.
    pub fn initial_tier(_jit: &JitState) -> (InterpreterTier, i32) {
        (InterpreterTier::Plain, i32::MAX)
    }

    pub fn tier_up_check(_vm: &Vm, _encoded_pc: u64) -> i64 {
        unreachable!("only the profiling interpreter checks tier-up budgets");
    }
}

pub mod entry_exit {
    use super::code::JitResult;
    use super::{Executable, NonNull, Vm};
    use crate::layout::execution_context::ExecutionContext;

    pub fn can_enter_jit_code(_vm: &Vm, _executable: &Executable) -> bool {
        false
    }

    pub fn enter_jit_code(_vm: &Vm, _frame: NonNull<ExecutionContext>) -> JitResult {
        unreachable!("no executable has JIT code");
    }

    pub fn helper_enter_jit_code(_vm: &Vm) -> i64 {
        unreachable!("only the profiling interpreter enters JIT code");
    }
}

pub mod translate {
    use super::{Executable, Vm};
    use crate::layout::cell::Gc;
    use crate::layout::execution_context::ExecutionContext;
    use crate::layout::function_object::FunctionObject;
    use crate::layout::value::Value;

    pub struct CallSiteFrame {
        pub function: Gc<FunctionObject>,
        pub executable: Gc<Executable>,
        pub program_counter: u32,
        pub frame_state: usize,
    }

    pub fn frame_pointer_from_low_half(low_half: u32) -> u64 {
        u64::from(low_half)
    }

    pub fn call_site_frames(_frame: &ExecutionContext, _index: u32, _frame_pointer: u64) -> Vec<CallSiteFrame> {
        Vec::new()
    }

    pub fn call_site_frame_arguments(
        _vm: &Vm,
        _frame: &ExecutionContext,
        _index: u32,
        _frame_pointer: u64,
        _frame_state: usize,
    ) -> Vec<Value> {
        unreachable!("no frame runs JIT code");
    }
}

pub mod snapshot {
    use crate::layout::cell::{CellHeader, Gc};
    use crate::layout::function_object::EcmascriptFunctionObject;

    pub fn ecmascript_function(_cell: Option<Gc<CellHeader>>) -> Option<Gc<EcmascriptFunctionObject>> {
        unreachable!("nothing collects call feedback");
    }
}

pub mod calls {
    use super::Vm;
    use crate::bytecode::op;
    use crate::interpreter::runtime_functions::SlowPathControl;

    pub fn get_by_id_from_jit(
        _vm: &Vm,
        _pc: u32,
        _instruction: &op::GetById,
        _values: &mut op::GetByIdValues,
    ) -> Option<SlowPathControl> {
        unreachable!("only JIT code calls its slow paths");
    }
}

pub mod testing {
    use super::Vm;
    use crate::layout::cell::Gc;
    use crate::layout::object::Object;
    use crate::runtime::realm::Realm;

    /// Without the JIT, there is no jit object.
    pub fn define_jit_testing_object(_vm: &Vm, _realm: Gc<Realm>, _global: &Object) {}
}
