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
mod evacuation;
mod identity_hash;
mod marker;
mod os;
mod pool;

pub use block::{BlockDescriptor, rust_gc_block_descriptor_commit, rust_gc_block_descriptor_table};
pub use evacuation::{
    CellMove, EvacuationLayout, EvacuationSet, PinReason, rust_gc_evacuation_add_block, rust_gc_evacuation_create,
    rust_gc_evacuation_destroy, rust_gc_evacuation_fix_up_slots, rust_gc_evacuation_forwarding_address,
    rust_gc_evacuation_move_cells, rust_gc_evacuation_pin_reason, rust_gc_evacuation_recorded_slot_count,
    rust_gc_evacuation_will_move,
};
pub use identity_hash::{
    IDENTITY_HASH_HASHED, IDENTITY_HASH_MOVED, IDENTITY_HASH_STATE_OFFSET, IdentityHashes, rust_gc_cell_identity_hash,
    rust_gc_identity_hashes_destroy, rust_gc_identity_hashes_drop_dead,
};
pub use marker::{
    MarkRequest, Marker, MarkerLayout, rust_gc_marker_mark, rust_gc_marker_pin_cell, rust_gc_marker_thread_count,
    rust_gc_marker_visit_cell, rust_gc_marker_visit_ephemeron, rust_gc_marker_visit_interior_pointer,
    rust_gc_marker_visit_member, rust_gc_marker_visit_self_pointer, rust_gc_marker_visit_values,
};
