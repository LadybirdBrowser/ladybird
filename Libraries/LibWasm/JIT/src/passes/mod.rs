/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The passes over the IR after building it.

pub mod cleanup;
pub mod dominators;
pub mod licm;
pub mod schedule;
