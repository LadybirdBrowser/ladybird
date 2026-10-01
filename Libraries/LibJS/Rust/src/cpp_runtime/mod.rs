/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Glue between the frontend and the C++ LibJS runtime.
//!
//! Everything here exists only because the C++ runtime embeds the frontend: the
//! `extern "C"` entry points it calls, the C++ factories that turn compiled
//! bytecode into GC-managed `Executable`s, and the bytecode cache that rebuilds
//! them without reparsing.

mod entry_points;

#[path = "../../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../../RustPanic.rs"]
mod rust_panic;

mod bytecode_cache;
mod dump;
mod ffi;
