/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Flap compiler library.
//!
//! Flap compiles typed interpreter handler definitions through semantic
//! analysis, SSA construction and optimization, machine lowering, register
//! allocation, and target-specific assembly emission. The compiler operates
//! entirely on in-memory inputs and outputs; the `flapc` binary is only a file
//! system and command-line adapter.

pub(crate) mod bytecode;
pub(crate) mod identity;
pub(crate) mod intrinsic;
pub(crate) mod types;
