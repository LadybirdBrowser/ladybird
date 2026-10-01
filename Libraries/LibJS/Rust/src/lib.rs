/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! # LibJS Parser
//!
//! A JavaScript parser that produces an AST.
//!
//! ## Architecture
//!
//! ```text
//! Source code (UTF-16)
//!     │
//!     ▼
//! ┌─────────────────────────────────────────────────────┐
//! │  Lexer (lexer.rs)                                   │
//! │  Tokenizes UTF-16 source into Token stream          │
//! └──────────────────────┬──────────────────────────────┘
//!                        │ tokens
//!                        ▼
//! ┌─────────────────────────────────────────────────────┐
//! │  Parser (parser.rs + parser/*.rs)                   │
//! │  Recursive descent with precedence climbing         │
//! │  Builds AST (ast.rs)                                │
//! └──────────────────────┬──────────────────────────────┘
//!                        │ AST
//!                        ▼
//! ┌─────────────────────────────────────────────────────┐
//! │  Codegen (bytecode/codegen.rs)                      │
//! │  Walks AST, emits bytecode via Generator            │
//! └──────────────────────┬──────────────────────────────┘
//!                        │ assembled bytecode
//!                        ▼
//! ┌─────────────────────────────────────────────────────┐
//! │  FFI (cpp_runtime/ffi.rs → BytecodeFactory.cpp)     │
//! │  Creates Executable from assembled data             │
//! └─────────────────────────────────────────────────────┘
//! ```
//!
//! ## Module overview
//!
//! - `compile.rs` — Parse and compile pipeline shared by every runtime
//! - `cpp_runtime/` — `extern "C"` entry points and FFI for the C++ runtime
//! - `host.rs` — Functions every embedding runtime provides to the frontend
//! - `token.rs` — Token types
//! - `lexer.rs` — Tokenizer: UTF-16 input → Token stream
//! - `parser.rs` — Parser state, helpers, token consumption
//! - `parser/expressions.rs` — Expression parsing (precedence climbing)
//! - `parser/statements.rs` — Statement parsing (if, for, while, etc.)
//! - `parser/declarations.rs` — Functions, classes, variables, modules
//! - `ast.rs` — AST type definitions
//! - `bytecode/` — Bytecode generator, instruction types, and dumper
//! - `scope_collector.rs` — Scope analysis

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

/// Compile-time conversion of an ASCII string literal to `&'static [u16]`.
///
/// Produces a static `[u16; N]` array, so comparisons like
/// `value == utf16!("eval")` involve zero heap allocation.
///
/// # Panics (at compile time)
/// Panics if the string contains non-ASCII characters. All JS keywords
/// and identifiers we compare against are pure ASCII.
macro_rules! utf16 {
    ($s:literal) => {{
        const VALUE: &[u16; $s.len()] = &{
            let bytes = $s.as_bytes();
            let mut arr = [0u16; $s.len()];
            let mut i = 0;
            while i < bytes.len() {
                assert!(bytes[i] < 128, "utf16! only supports ASCII literals");
                arr[i] = bytes[i] as u16;
                i += 1;
            }
            arr
        };
        VALUE.as_slice()
    }};
}

// cbindgen writes RustFFI.h in the order it parses modules, so the C++ glue is
// declared first to keep the entry points at the top of the header.
#[cfg(feature = "cpp-runtime")]
mod cpp_runtime;

pub mod ast;
pub mod ast_dump;
pub mod bytecode;
pub mod compile;
pub mod fast_hash;
pub mod host;
pub mod lexer;
pub mod parser;
pub mod runtime;
pub mod scope_collector;
pub mod token;

/// Convert a `usize` to `u32`, panicking if the value exceeds `u32::MAX`.
/// Prefer this over `as u32` which silently truncates on 64-bit platforms.
pub(crate) fn u32_from_usize(value: usize) -> u32 {
    u32::try_from(value).expect("value exceeds u32::MAX")
}
