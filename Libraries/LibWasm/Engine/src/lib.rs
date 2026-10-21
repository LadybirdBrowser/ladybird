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

pub mod abi;
pub mod capi;
pub mod error;
pub mod instruction_tables;
pub mod module;
pub mod opcode;
pub mod parser;
pub mod quickening;
pub mod reader;
pub mod runtime;
pub mod types;
pub mod validator;

pub use error::{Error, Result};
pub use module::Module;

/// Decode and validate a binary module.
pub fn compile(bytes: &[u8]) -> Result<Module> {
    let mut module = parser::parse(bytes)?;
    validator::validate(&mut module)?;
    Ok(module)
}
