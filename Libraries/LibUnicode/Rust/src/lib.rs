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

pub mod calendar;
pub mod character_types;
#[cfg(feature = "idna")]
pub mod idna;
