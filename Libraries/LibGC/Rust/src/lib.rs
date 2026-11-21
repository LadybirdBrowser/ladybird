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

mod allocator;
mod bitmap;
mod block;
mod block_pool;
mod compaction;
mod evacuation;
mod identity_hash;
mod marker;
mod os;
mod pool;
mod sanitizer;

pub use allocator::{
    Allocator, AllocatorConfig, LocalFreeCells, SweepStats, rust_gc_allocator_allocate,
    rust_gc_allocator_allocate_destination, rust_gc_allocator_block_at, rust_gc_allocator_block_count,
    rust_gc_allocator_create, rust_gc_allocator_destroy, rust_gc_allocator_give_back_local_free_cells,
    rust_gc_allocator_has_blocks_pending_sweep, rust_gc_allocator_keep_cells_pinned, rust_gc_allocator_local_block,
    rust_gc_allocator_release_moved_cell, rust_gc_allocator_start_sweep, rust_gc_allocator_sweep_next_block,
    rust_gc_allocator_sweep_pending_block,
};
pub use block::{BlockDescriptor, rust_gc_block_descriptor_table};
pub use block_pool::{
    BlockPool, rust_gc_block_pool_allocate_block, rust_gc_block_pool_block_count, rust_gc_block_pool_create,
    rust_gc_block_pool_deallocate_block, rust_gc_block_pool_destroy, rust_gc_block_pool_return_freed_blocks_to_system,
    rust_gc_block_pool_returned_block_count, rust_gc_heap_region_committed_end, rust_gc_heap_region_init,
    rust_gc_set_cold_block_age_in_milliseconds, rust_gc_wake_decommit_worker,
};
pub use compaction::{CompactionCandidate, CompactionSelection, rust_gc_compaction_select};
pub use evacuation::{
    CellMove, EvacuationLayout, EvacuationSet, PinReason, PinnedCells, RecordedSlotKind, rust_gc_evacuation_add_block,
    rust_gc_evacuation_cells_to_move, rust_gc_evacuation_create, rust_gc_evacuation_destroy,
    rust_gc_evacuation_fix_up_slots, rust_gc_evacuation_forward_cells, rust_gc_evacuation_forwarding_address,
    rust_gc_evacuation_move_cells, rust_gc_evacuation_pin_reason, rust_gc_evacuation_record_slot,
    rust_gc_evacuation_recorded_slot_count, rust_gc_evacuation_will_move,
};
pub use identity_hash::{
    IDENTITY_HASH_HASHED, IDENTITY_HASH_MOVED, IDENTITY_HASH_STATE_OFFSET, IdentityHashes, rust_gc_cell_identity_hash,
    rust_gc_identity_hashes_destroy,
};
pub use marker::{
    MarkRequest, Marker, MarkerLayout, rust_gc_marker_mark, rust_gc_marker_pin_cell,
    rust_gc_marker_release_unused_helper_memory, rust_gc_marker_visit_cell, rust_gc_marker_visit_ephemeron,
    rust_gc_marker_visit_ephemeron_without_pinning, rust_gc_marker_visit_interior_pointer, rust_gc_marker_visit_member,
    rust_gc_marker_visit_owned_values, rust_gc_marker_visit_self_pointer, rust_gc_marker_visit_values,
};
