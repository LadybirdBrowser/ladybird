/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The core of the LibRegex compiler: the parser, the syntax tree, the compiler to a program and the literal hints.
//!
//! A compile has all of its work data in one zone (see `ak::Zone`), which the code that starts the compile hands it.
//! What the compile keeps of its results is copied out of the zone by that code.

// NB: The Rust code of a compile makes no block in the heap: all its work data is in its zone. Without `std` (and
//     without `alloc`), rustc rejects code of this crate that would make one. The LibUnicode helper that gives the
//     data of a string property (`get_string_property_data()`) still makes temporary C++ vectors.
#![cfg_attr(not(test), no_std)]

pub mod ast;
pub mod compiler;
pub mod literal_hints;
pub mod parser;
mod stack;
pub mod zone_program;
