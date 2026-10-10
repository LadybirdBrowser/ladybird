/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Analyses and transformations of the IR, run between graph building and
//! register allocation.
//!
//! Debug builds and tests check the graph's invariants (see `verify`) after
//! building and after every pass.

pub mod dominators;
pub mod edit;
pub mod verify;
