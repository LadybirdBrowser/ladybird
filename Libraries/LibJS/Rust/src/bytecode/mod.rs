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

#[cfg(not(test))]
pub mod op {
    use super::encoding::*;
    use crate::layout::property_lookup_cache::EnvironmentCoordinate;
    use crate::layout::value::Value;

    include!(concat!(env!("OUT_DIR"), "/bytecode_ops.rs"));
}
