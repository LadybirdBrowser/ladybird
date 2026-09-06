/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(feature = "allocator")]
#[path = "../../../RustAllocator.rs"]
mod rust_allocator;

pub mod calendar;
pub mod character_types;
#[cfg(any(test, feature = "ffi-stubs"))]
mod ffi_stubs;
#[cfg(feature = "idna")]
pub mod idna;
