/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Bytecode generator infrastructure for the parser.
//!
//! This module contains the types and machinery needed to generate
//! bytecode from the AST, in the binary format that the interpreter runs.
//!
//! ## Submodules
//!
//! - `operand` -- Register, Operand, Label, and table index types
//! - `instruction` -- Instruction enum (generated from interpreter.flap by build.rs)
//! - `basic_block` -- BasicBlock: list of instructions with control flow metadata
//! - `generator` -- Generator: manages registers, constants, tables, and assembly
//! - `codegen` -- AST-walking code that emits instructions via the Generator
//! - `constant` -- VM-dependent constants codegen refers to by kind
//! - `executable_data` -- ExecutableData: a compiled body and everything needed to run it
//!
//! And the runtime's side of bytecode:
//!
//! - `encoding` -- How instructions and their operands are laid out in an executable's bytecode
//! - `op` -- Instruction structs the interpreter reads (generated from interpreter.flap by build.rs)
//! - `executable` -- Executable: the cell that holds bytecode and its caches
//! - `feedback` -- What the profiling interpreter observes for the optimizing JIT, per executable
//! - `property_access` -- Property lookups through the inline caches
//! - `class_blueprint`, `bytecode_cache` -- Classes and bytecode cache blobs as the runtime keeps them

pub mod basic_block;
#[cfg(not(test))]
pub mod bytecode_cache;
#[cfg(not(test))]
pub mod class_blueprint;
pub mod codegen;
pub mod constant;
#[cfg(not(test))]
pub mod dump;
pub mod encoded_value;
#[cfg(not(test))]
pub mod encoding;
#[cfg(not(test))]
pub mod executable;
pub mod executable_data;
#[cfg(not(test))]
pub mod feedback;
pub mod generator;
pub mod instruction;
mod native_disassembler;
pub mod operand;
#[cfg(not(test))]
pub mod property_access;
pub mod validator;

/// The view of an instruction in `op`, which lays out the instruction of its opcode in the bytecode.
///
/// # Safety
///
/// The type must lay out the instruction of `OPCODE`, and only ever be a view of bytecode that holds the instruction.
#[cfg(not(test))]
pub unsafe trait BytecodeOp: Sized {
    const OPCODE: u8;

    /// The instruction's size in the bytecode.
    fn length(&self) -> u32;
}

/// The instruction at `pc` of the executable's bytecode, which has to be an `Op`.
#[cfg(not(test))]
pub fn instruction_at<Op: BytecodeOp>(executable: &executable::Executable, pc: u32) -> &Op {
    let bytes = &executable.bytecode()[pc as usize..];
    assert_eq!(bytes[0], Op::OPCODE);
    assert!(size_of::<Op>() <= bytes.len() && bytes.as_ptr().addr().is_multiple_of(align_of::<Op>()));
    // SAFETY: Executables only hold validated bytecode, so an instruction of this opcode, with the layout of Op and
    //         valid fields, starts at pc.
    let instruction = unsafe { &*bytes.as_ptr().cast::<Op>() };
    assert!(instruction.length() as usize <= bytes.len());
    instruction
}

#[cfg(not(test))]
#[allow(
    clippy::manual_non_exhaustive,
    reason = "the instruction views keep code outside this module, not only other crates, from making them"
)]
pub mod op {
    use super::encoding::*;
    use crate::layout::property_lookup_cache::EnvironmentCoordinate;
    use crate::layout::value::Value;

    include!(concat!(env!("OUT_DIR"), "/bytecode_ops.rs"));
}
