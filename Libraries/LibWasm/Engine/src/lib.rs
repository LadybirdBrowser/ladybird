/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! # LibWasm
//!
//! A WebAssembly engine: the binary format decoder, the validator, which also builds the side tables the in-place
//! interpreter branches with, the runtime, the interpreter, and the JIT's runtime half (the compiler is
//! libwasm_jit).

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../RustPanic.rs"]
mod rust_panic;

// NB: Atomics.wait and Atomics.notify's waiting and waking, which memory.atomic.wait and memory.atomic.notify share.
#[path = "../../../LibJS/Rust/src/futex.rs"]
mod futex;

#[cfg(feature = "capi")]
pub mod capi;
pub mod runtime;
pub mod wasi;

// The module format is a crate of its own, which the JIT depends on too.
pub use libwasm_module::{
    Error, Module, Result, abi, compile, error, instruction_tables, module, opcode, parser, quickening, reader, types,
    validator,
};
