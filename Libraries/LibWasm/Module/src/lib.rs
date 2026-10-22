/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebAssembly module format: the binary decoder, the validator and its side tables, quickening, and the layout
//! the interpreter and compiled code share (`abi`). It is the part of the engine both the engine and the JIT depend
//! on, and the engine re-exports it.

pub mod abi;
pub mod error;
pub mod instruction_tables;
pub mod module;
pub mod opcode;
pub mod parser;
pub mod quickening;
pub mod reader;
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
