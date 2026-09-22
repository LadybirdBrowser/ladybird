/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the C++ host hands the compositor-shared code, and what it gets back.

pub mod replay;

pub use replay::*;

pub use crate::display_list::storage::FfiRecordedDisplayList;
pub use crate::visual_context::ffi_types::*;
