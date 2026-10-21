/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! # LibWasm
//!
//! A WebAssembly engine: the binary format decoder, the validator, which also builds the side tables the in-place
//! interpreter branches with, and (in later steps) the runtime, the interpreter, and the JIT.

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../RustPanic.rs"]
mod rust_panic;

pub mod capi;
pub mod runtime;
pub mod wasi;

// The module format is a crate of its own, which the JIT depends on too.
pub use libwasm_module::{
    Error, Module, Result, abi, compile, error, instruction_tables, module, opcode, parser, quickening, reader, types,
    validator,
};
