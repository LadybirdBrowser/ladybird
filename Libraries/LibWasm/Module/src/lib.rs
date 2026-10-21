/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebAssembly module format: the binary decoder, the validator and its side tables, quickening, and the layout
//! the interpreter and compiled code share (`abi`). It is the part of the engine both the engine and the JIT depend
//! on. The sources stay with the engine's, which re-exports them.

#[path = "../../Engine/src/abi.rs"]
pub mod abi;
#[path = "../../Engine/src/error.rs"]
pub mod error;
#[path = "../../Engine/src/instruction_tables.rs"]
pub mod instruction_tables;
#[path = "../../Engine/src/module.rs"]
pub mod module;
#[path = "../../Engine/src/opcode.rs"]
pub mod opcode;
#[path = "../../Engine/src/parser.rs"]
pub mod parser;
#[path = "../../Engine/src/quickening.rs"]
pub mod quickening;
#[path = "../../Engine/src/reader.rs"]
pub mod reader;
#[path = "../../Engine/src/types.rs"]
pub mod types;
#[path = "../../Engine/src/validator.rs"]
pub mod validator;

pub use error::{Error, Result};
pub use module::Module;

/// Decode and validate a binary module.
pub fn compile(bytes: &[u8]) -> Result<Module> {
    let mut module = parser::parse(bytes)?;
    validator::validate(&mut module)?;
    Ok(module)
}
