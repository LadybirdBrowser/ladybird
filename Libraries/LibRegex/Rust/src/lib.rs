/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(feature = "allocator")]
/// cbindgen:ignore
#[path = "../../../RustAllocator.rs"]
mod rust_allocator;

#[path = "../../../RustPanic.rs"]
mod rust_panic;

pub mod ast;
pub mod bytecode;
pub mod compiler;
pub mod ffi;
mod optimizer;
pub mod parser;
pub mod regex;
pub mod vm;
