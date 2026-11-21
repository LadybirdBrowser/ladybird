/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of LibGC's collector that are written in Rust.

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../RustPanic.rs"]
mod rust_panic;

mod block;
mod marker;
mod os;
mod pool;

pub use block::{BlockDescriptor, rust_gc_block_descriptor_commit, rust_gc_block_descriptor_table};
pub use marker::{
    MarkRequest, Marker, MarkerLayout, rust_gc_marker_mark, rust_gc_marker_thread_count, rust_gc_marker_visit_cell,
    rust_gc_marker_visit_ephemeron, rust_gc_marker_visit_values,
};
