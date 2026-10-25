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
pub struct JitState {
    pub dependents: dependencies::Dependents,
}

impl JitState {
    pub fn new(_options: options::Options) -> Self {
        Self {
            dependents: dependencies::Dependents,
        }
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

        pub fn has_jit_code_or_compile(&self) -> bool {
            false
        }

        pub fn take_over_jit_refusal_from(&self, _other: &Executable) {}

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

    pub fn finish_top_level_code(_vm: &Vm, _executable: &crate::bytecode::executable::Executable) {}
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

    pub fn enter_jit_code_at_continuation(_vm: &Vm, _frame: NonNull<ExecutionContext>, _pc: u32) -> Option<JitResult> {
        None
    }

    pub fn helper_enter_jit_code(_vm: &Vm) -> i64 {
        unreachable!("only the profiling interpreter enters JIT code");
    }

    pub fn helper_enter_jit_code_after_catch(_vm: &Vm, _pc: u32) -> i64 {
        unreachable!("only the profiling interpreter enters JIT code");
    }
}

pub mod translate {
    use super::{Executable, Vm};
    use crate::layout::cell::Gc;
    use crate::layout::execution_context::ExecutionContext;
    use crate::layout::function_object::FunctionObject;
    use crate::layout::value::Value;

    #[derive(Clone, libjs_runtime_macros::Trace)]
    pub struct CallSiteFrame {
        pub function: Gc<FunctionObject>,
        pub executable: Gc<Executable>,
        #[gc(untraced)]
        pub program_counter: u32,
        #[gc(untraced)]
        pub frame_state: usize,
    }

    pub fn frame_pointer_from_low_half(low_half: u32) -> u64 {
        u64::from(low_half)
    }

    pub fn call_site_frames<'vm>(
        vm: &'vm Vm,
        _frame: &ExecutionContext,
        _index: u32,
        _frame_pointer: u64,
    ) -> crate::gc::root::MarkedVec<'vm, CallSiteFrame> {
        crate::gc::root::MarkedVec::new(vm)
    }

    pub fn call_site_frame_arguments<'d>(
        _vm: &Vm,
        _frame: &ExecutionContext,
        _index: u32,
        _frame_pointer: u64,
        _frame_state: usize,
        _defer_gc: &'d crate::gc::defer_gc::DeferGc<'_>,
    ) -> crate::gc::defer_gc::Deferred<'d, Vec<Value>> {
        unreachable!("no frame runs JIT code");
    }

    pub fn resolve_deferred_arguments(_vm: &Vm, _frame: &ExecutionContext) {}
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

pub mod dependencies {
    use super::Vm;
    use crate::layout::object::Object;

    #[derive(Clone, Copy)]
    pub struct CellId(pub u64);

    /// What JIT code may depend on, which the runtime notes when it stops holding.
    pub enum Dependency {
        WatchpointSetValid(CellId),
        GlobalDeclarations { environment: CellId, serial: u64 },
        GlobalBindingUnassigned { environment: CellId, index: u32 },
        StableShape(CellId),
    }

    pub struct Dependents;

    impl Dependents {
        pub fn remove_dead_entries(&self, _: &crate::gc::sweep::Sweep<'_>) {}
    }

    pub fn invalidate_dependents(_vm: &Vm, _dependency: Dependency) {}

    /// Without the JIT, nothing depends on the slots of global objects, and the caches may write to any of them.
    pub fn global_object_slot_may_have_been_assigned(_vm: &Vm, _object: &Object, _offset: u32) -> bool {
        true
    }

    pub fn note_global_object_slot_assignments(_vm: &Vm, _object: &Object, _offsets: core::ops::Range<u32>) {}

    pub fn note_htmldda_object(_vm: &Vm) {}
}

pub mod testing {
    use super::Vm;
    use crate::layout::cell::Gc;
    use crate::layout::object::Object;
    use crate::runtime::realm::Realm;

    /// Without the JIT, there is no jit object.
    pub fn define_jit_testing_object(_vm: &Vm, _realm: Gc<Realm>, _global: &Object) {}
}
