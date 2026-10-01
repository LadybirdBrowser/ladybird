/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A JavaScript runtime for LibJS, written in Rust. It runs the same bytecode as the C++ runtime, produced by the
//! same frontend and executed by the same Flap-generated interpreter, on cells allocated from LibGC.

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../../RustPanic.rs"]
mod rust_panic;

pub mod build_configuration;
pub mod bytecode;
pub mod frontend_host;
pub mod gc;
pub mod hash_table;
pub mod interpreter;
pub mod layout;
pub mod layout_forward;
pub mod parser_error;
pub mod runtime;
pub mod script;
pub mod source_code;
pub mod utf16;
pub mod utilities;

mod layout_static_assertions {
    include!(concat!(env!("OUT_DIR"), "/layout_static_assertions.rs"));
}
