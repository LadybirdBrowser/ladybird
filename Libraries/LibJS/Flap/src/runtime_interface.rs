/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime functions generated interpreter code calls, and how it calls
//! them.
//!
//! A runtime that links the generated assembly defines every function that
//! [`Compiler::runtime_functions`](crate::Compiler::runtime_functions) reports
//! with the C signature documented on its [`RuntimeFunctionKind`]. In those
//! signatures `VM*` is the pointer the interpreter was entered with, `pc` is
//! the bytecode offset of the current instruction, and `Value` is a NaN-boxed
//! `u64`. Slow paths return the control word that the runtime's
//! `SlowPathControl` describes.

use crate::intrinsic::CallOperation;
use crate::metadata::{SlowPathAbi, SlowPathLayout};
use crate::ssa::{Constant, Intrinsic, Operation, ValueDefinition};
use crate::{Architecture, CompileError, CompileStage, ObjectFormat, PreparedProgram, Target};
use std::collections::BTreeMap;

pub(crate) const FALLBACK_HANDLER: &str = "asm_fallback_handler";
pub(crate) const BREAKPOINT_CHECK: &str = "asm_debugger_check_breakpoint";
pub(crate) const STACK_OVERFLOW_SLOW_PATH: &str = "asm_slow_path_stack_overflow";

/// An external function the generated assembly calls.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RuntimeFunction {
    pub symbol: String,
    pub kind: RuntimeFunctionKind,
}

/// How the generated assembly calls a runtime function.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RuntimeFunctionKind {
    /// A `call_slow_path` target of the handler for `op`, with the signature
    /// `abi` documents. `layout` describes `Op::{op}::Values`.
    SlowPath {
        op: String,
        abi: SlowPathAbi,
        layout: SlowPathLayout,
    },
    /// A `call_interp` target of the handler for `op`:
    /// `i64 f(VM*, u32 pc, Op::{op} const*, Op::{op}::Values& values)`.
    /// It returns zero after handling the instruction and storing its outputs
    /// in `values`, and nonzero to leave the instruction to the handler.
    Try { op: String, layout: SlowPathLayout },
    /// A `call_binary_slow_path` target:
    /// `i64 f(VM*, u32 pc, Value& dst, Value lhs, Value rhs)`.
    BinarySlowPath,
    /// A `call_jump_slow_path` target:
    /// `i64 f(VM*, u32 pc, Value lhs, Value rhs, u32 true_target, u32 false_target)`.
    JumpSlowPath,
    /// A `call_helper` target: `u64 f(u64)`. The handler decides what the
    /// argument and result words hold, such as an encoded `Value` or a `VM*`.
    Helper,
    /// A `call_helper_with_two_arguments` target: `u64 f(u64, u64)`.
    HelperWithTwoArguments,
    /// `i64 asm_fallback_handler(VM*, u32 pc, u8 const* instruction)`, which
    /// runs opcodes that have no handler.
    FallbackHandler,
    /// `void asm_debugger_check_breakpoint(VM*, u32 pc)`, which runs before
    /// each instruction while a debugger is attached.
    BreakpointCheck,
    /// `i64 asm_slow_path_stack_overflow(VM*, u32 pc)`, which throws when the
    /// `Op::Values` record of a variable-length operation does not fit on the
    /// stack.
    StackOverflowSlowPath,
}

impl RuntimeFunctionKind {
    fn description(&self) -> String {
        match self {
            Self::SlowPath { op, .. } => format!("a slow path of {op}"),
            Self::Try { op, .. } => format!("a try call of {op}"),
            Self::BinarySlowPath => "a binary slow path".to_string(),
            Self::JumpSlowPath => "a jump slow path".to_string(),
            Self::Helper => "a one-argument helper".to_string(),
            Self::HelperWithTwoArguments => "a two-argument helper".to_string(),
            Self::FallbackHandler => "the fallback handler".to_string(),
            Self::BreakpointCheck => "the breakpoint check".to_string(),
            Self::StackOverflowSlowPath => "the stack-overflow slow path".to_string(),
        }
    }
}

/// Where a `call_raw_native` call finds the two words of the
/// `ThrowCompletionOr<Value>` its native function returns.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RawNativeReturnConvention {
    /// The function takes the `VM&` and returns both words in the first two
    /// integer return registers.
    Registers,
    /// The function takes a pointer to a 16-byte result, then the `VM&`.
    OutPointer,
}

/// How `call_raw_native` receives the result of a native function on `target`.
pub fn raw_native_return_convention(target: Target) -> RawNativeReturnConvention {
    match (target.architecture, target.object_format) {
        (_, ObjectFormat::Coff) | (Architecture::X86_64, ObjectFormat::MachO) => RawNativeReturnConvention::OutPointer,
        (Architecture::X86_64, ObjectFormat::Elf)
        | (Architecture::Aarch64, ObjectFormat::Elf | ObjectFormat::MachO) => RawNativeReturnConvention::Registers,
    }
}

pub(crate) fn runtime_functions(
    prepared: &PreparedProgram,
    target: Target,
) -> Result<Vec<RuntimeFunction>, CompileError> {
    let record_form_only = target.object_format == ObjectFormat::Coff;
    let mut functions = BTreeMap::<String, RuntimeFunctionKind>::new();
    let mut add = |symbol: &str, kind: RuntimeFunctionKind, handler: Option<&str>| {
        if let Some(previous) = functions.get(symbol) {
            if *previous == kind {
                return Ok(());
            }
            return Err(CompileError::new(
                CompileStage::Semantic,
                handler,
                format!(
                    "runtime function '{symbol}' is called as both {} and {}",
                    previous.description(),
                    kind.description()
                ),
            ));
        }
        functions.insert(symbol.to_string(), kind);
        Ok(())
    };

    add(FALLBACK_HANDLER, RuntimeFunctionKind::FallbackHandler, None)?;
    add(BREAKPOINT_CHECK, RuntimeFunctionKind::BreakpointCheck, None)?;
    for (handler, handler_layout) in prepared.handlers.iter().zip(&prepared.bytecode.handler_layouts) {
        let function = &handler.function;
        let op_layout = || {
            handler_layout.slow_path.clone().ok_or_else(|| {
                CompileError::new(
                    CompileStage::Semantic,
                    Some(handler.name()),
                    "handler without a bytecode layout calls an operation's runtime function",
                )
            })
        };
        for instruction in function
            .blocks
            .iter()
            .flat_map(|block| &block.instructions)
            .map(|instruction| &function.instructions[instruction.0])
        {
            let Operation::Intrinsic(Intrinsic::Call(call)) = instruction.operation else {
                continue;
            };
            let kind = match call {
                CallOperation::SlowPath => {
                    let layout = op_layout()?;
                    if layout.array.is_some() {
                        add(
                            STACK_OVERFLOW_SLOW_PATH,
                            RuntimeFunctionKind::StackOverflowSlowPath,
                            Some(handler.name()),
                        )?;
                    }
                    RuntimeFunctionKind::SlowPath {
                        op: handler.name().to_string(),
                        abi: layout.abi(record_form_only),
                        layout,
                    }
                }
                CallOperation::Interpreter => RuntimeFunctionKind::Try {
                    op: handler.name().to_string(),
                    layout: op_layout()?,
                },
                CallOperation::BinarySlowPath => RuntimeFunctionKind::BinarySlowPath,
                CallOperation::JumpSlowPath => RuntimeFunctionKind::JumpSlowPath,
                CallOperation::Helper => RuntimeFunctionKind::Helper,
                CallOperation::HelperWithTwoArguments => RuntimeFunctionKind::HelperWithTwoArguments,
                CallOperation::RawNative => continue,
            };
            let ValueDefinition::Constant(Constant::SlowPath(symbol) | Constant::FunctionSymbol(symbol)) =
                &function.values[instruction.inputs[0].0].definition
            else {
                return Err(CompileError::new(
                    CompileStage::Semantic,
                    Some(handler.name()),
                    format!("'{}' target is not a link-time symbol", call.name()),
                ));
            };
            add(symbol.as_str(), kind, Some(handler.name()))?;
        }
    }

    Ok(functions
        .into_iter()
        .map(|(symbol, kind)| RuntimeFunction { symbol, kind })
        .collect())
}
