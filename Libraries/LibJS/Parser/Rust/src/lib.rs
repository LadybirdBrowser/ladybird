/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The core of the LibJS parser: the lexer, the parser, the interner of names, the columns of the syntax tree and the
//! resolver.
//!
//! A parse job has all of its work data in one zone (see `ak::Zone`), which the code that starts the job hands it. What
//! the job keeps of its results is copied out of the zone by that code, which is also what reaches the runtime for the
//! parser (see `base::ValidateRegex`).

// NB: A parse job makes no block in the heap: all its work data is in its zone. Without `std` (and without `alloc`),
//     rustc rejects code of the parser that would make one. The tests keep `std`.
#![cfg_attr(not(test), no_std)]

pub mod base;
pub mod columns;
pub mod escapes;
mod expr;
pub mod intern;
mod lexer;
pub mod resolve;
mod stmt;
pub mod token;
pub mod tree;
pub mod unit;

/// Convert a `usize` to `u32`, panicking if the value exceeds `u32::MAX`.
/// Prefer this over `as u32` which silently truncates on 64-bit platforms.
#[inline]
pub fn u32_from_usize(value: usize) -> u32 {
    u32::try_from(value).expect("value exceeds u32::MAX")
}
