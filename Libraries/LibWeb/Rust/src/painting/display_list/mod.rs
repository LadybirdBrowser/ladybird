/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The recorder that produces display lists, on top of the display list code the compositor shares.

pub mod dump;
pub mod recorder;

pub use libcompositing_rust::display_list::*;
