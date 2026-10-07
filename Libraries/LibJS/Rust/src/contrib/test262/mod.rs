/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The global object test262 runs its tests with, as its INTERPRETING.md describes it, with print() and $262.

pub mod agent_object;
#[cfg(unix)]
pub mod agents;
/// $262. Its file has no dollar sign in its name, which some tools do not handle.
#[path = "262_object.rs"]
pub mod dollar_262_object;
pub mod global_object;
pub mod is_htmldda;
