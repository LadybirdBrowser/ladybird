/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Descriptions of the virtual machines Flap compiles interpreters for.
//!
//! The compiler pipeline is shared, but each virtual machine decides how its
//! instructions are encoded, which interpreter state its handlers keep pinned
//! in registers, which runtime constants the generated code needs, how the
//! entry, exit, and fallback stubs talk to the runtime, and what the generated
//! symbols are called.

use crate::Architecture;
use crate::frontend::layout::KnownLayoutConstant;
use crate::types::InterpreterRegister;

/// The virtual machine an interpreter is compiled for.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Profile {
    /// The LibJS bytecode interpreter: fixed-layout instructions whose
    /// operands index NaN-boxed values, entered with a `VM*`.
    JavaScript,
    /// The in-place WebAssembly interpreter: handlers run directly on a
    /// function's binary code, decode their own immediates, and keep the
    /// operand stack and locals in 8-byte machine stack slots.
    WebAssembly,
}

/// How the bytes of one instruction are laid out.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum InstructionEncoding {
    /// Every handler parameter is a field at a fixed offset of a fixed-size
    /// instruction record, and `dispatch_next` advances by the record size.
    FixedLayout,
    /// Instructions are raw bytes that start with an opcode byte. Handlers
    /// take no parameters and decode their own immediates, so `dispatch_next`
    /// only steps over the opcode byte, and handlers with immediates compute
    /// the next instruction themselves.
    RawBytes,
}

/// How the pinned program counter addresses the current instruction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum ProgramCounterAddressing {
    /// `pc` is a 32-bit offset from the program base `pb`.
    BaseRelative,
    /// `ip` is a pointer to the current instruction.
    Absolute,
}

impl Profile {
    /// The name every global symbol of the generated interpreter starts with.
    pub fn symbol_prefix(self) -> &'static str {
        match self {
            Self::JavaScript => "js_interpreter",
            Self::WebAssembly => "wasm_interpreter",
        }
    }

    pub(crate) fn instruction_encoding(self) -> InstructionEncoding {
        match self {
            Self::JavaScript => InstructionEncoding::FixedLayout,
            Self::WebAssembly => InstructionEncoding::RawBytes,
        }
    }

    pub(crate) fn program_counter_addressing(self) -> ProgramCounterAddressing {
        match self {
            Self::JavaScript => ProgramCounterAddressing::BaseRelative,
            Self::WebAssembly => ProgramCounterAddressing::Absolute,
        }
    }

    /// The pinned interpreter registers handlers of this profile may name.
    pub(crate) fn registers(self) -> &'static [InterpreterRegister] {
        use InterpreterRegister::*;
        match self {
            Self::JavaScript => &[
                ProgramCounter,
                ProgramBase,
                Values,
                ExecutionContext,
                DispatchTable,
                HeapRegionBase,
                StackPointer,
                FramePointer,
            ],
            Self::WebAssembly => &[
                InstructionPointer,
                SideTablePointer,
                ValueStackPointer,
                MemoryBase,
                DispatchTable,
                StackPointer,
                FramePointer,
            ],
        }
    }

    pub(crate) fn has_register(self, register: InterpreterRegister) -> bool {
        self.registers().contains(&register)
    }

    /// The layout constants the generated entry, exit, and dispatch code of
    /// this profile cannot be emitted without.
    pub(crate) fn required_runtime_constants(self, architecture: Architecture) -> &'static [KnownLayoutConstant] {
        use KnownLayoutConstant::*;
        match (self, architecture) {
            (Self::JavaScript, Architecture::X86_64) => &[
                SlowPathContinuationBit,
                VmRunningExecutionContext,
                VmBreakpointController,
                VmHeapRegionBase,
                ExecutionContextExecutable,
                ExecutionContextProgramCounter,
                ExecutableBytecodeData,
                ExecutableDispatchTableIndex,
                VmDispatchTables,
                DispatchTableIndexMask,
                SizeOfExecutionContext,
                CanonicalNanBits,
            ],
            (Self::JavaScript, Architecture::Aarch64) => &[
                Int32Tag,
                BooleanTag,
                SlowPathContinuationBit,
                VmRunningExecutionContext,
                VmBreakpointController,
                VmHeapRegionBase,
                ExecutionContextExecutable,
                ExecutionContextProgramCounter,
                ExecutableBytecodeData,
                ExecutableDispatchTableIndex,
                VmDispatchTables,
                DispatchTableIndexMask,
                SizeOfExecutionContext,
                CanonicalNanBits,
            ],
            (Self::WebAssembly, _) => &[],
        }
    }
}
