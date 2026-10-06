/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The functions the interpreter calls into the runtime. Their entry points are generated from interpreter.flap; the
//! runtime implements them by overriding the methods of RuntimeFunctions for Runtime.

use core::cell::Cell;

use crate::bytecode::op;
use crate::interpreter::run::HandleExceptionResponse;
use crate::interpreter::vm::Vm;
use crate::layout::value::Value;

/// What the interpreter does after a slow path returns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(transparent)]
pub struct SlowPathControl(pub i64);

impl SlowPathControl {
    const SAME_FRAME_BIT: u32 = 32;

    /// Leave the interpreter.
    pub const EXIT: Self = Self(-1);

    /// Continue in the same frame at `next_pc`, after storing the slow path's outputs into the frame.
    pub fn continue_at(next_pc: u32) -> Self {
        Self((1 << Self::SAME_FRAME_BIT) | i64::from(next_pc))
    }

    /// Continue at `pc` in whatever frame is now running, as after a call, a return or an exception.
    pub fn dispatch_at(pc: u32) -> Self {
        Self(i64::from(pc))
    }
}

/// The control word and the primary output of a slow path that receives its operands in registers, returned in two
/// registers as the interpreter expects.
#[repr(C)]
pub struct AsmSlowPathResult {
    pub control: i64,
    pub value: u64,
}

/// The runtime's implementation of the functions the interpreter calls.
pub struct Runtime;

/// Hands an exception a slow path threw to the interpreter: it continues at the handler, or leaves.
#[cold]
pub fn handle_asm_exception(vm: &Vm, pc: u32, exception: Value) -> SlowPathControl {
    match vm.handle_exception(pc, exception) {
        HandleExceptionResponse::ExitFromExecutable => SlowPathControl::EXIT,
        HandleExceptionResponse::ContinueInThisExecutable => {
            let context = vm.running_execution_context().expect("the handler's frame is running");
            // SAFETY: The running context is live.
            SlowPathControl::dispatch_at(unsafe { context.as_ref() }.program_counter.get())
        }
    }
}

/// Stops the process at a runtime function or operation that is not implemented yet. This panics, so that a tool can
/// report it through its panic hook.
pub fn unimplemented_runtime_function(symbol: &str, pc: u32) -> ! {
    panic!("libjs_runtime_rust: unimplemented runtime function {symbol} (pc {pc})")
}

/// Unwraps a ThrowCompletionOr in a slow path, or returns the control word that hands its exception to the
/// interpreter.
#[allow(unused_macros)] // Until the first slow path that calls a throwing operation lands.
macro_rules! asm_try {
    ($vm:expr, $pc:expr, $expression:expr) => {
        match $expression {
            Ok(value) => value,
            Err(throw) => {
                return $crate::interpreter::runtime_functions::handle_asm_exception($vm, $pc, throw.value());
            }
        }
    };
}
#[allow(unused_imports)]
pub(crate) use asm_try;

mod generated {
    #![allow(clippy::missing_safety_doc)]

    use super::*;

    include!(concat!(env!("OUT_DIR"), "/runtime_functions.rs"));
}

pub use generated::{RUNTIME_FUNCTION_SYMBOLS, RuntimeFunctions};
