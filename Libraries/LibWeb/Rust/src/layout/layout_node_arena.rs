/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;
use crate::layout::node_data::NodeData;
use crate::layout::node_data::NodeSlotId;
use std::ffi::c_void;
use std::thread;

const SLOTS_PER_CHUNK: usize = 256;

// NodeData is sized to one cache line; the aligned chunk keeps every densely-strided slot
// line-aligned, and per-slot bookkeeping lives in a parallel array so it stays that way.
#[repr(align(64))]
struct Chunk {
    slots: [NodeData; SLOTS_PER_CHUNK],
}

fn new_chunk() -> Box<Chunk> {
    // SAFETY: Every slot is written with NodeData::default() before the chunk is exposed. The
    // chunk is built in place on the heap because it is far too large for the stack.
    unsafe {
        let mut chunk = Box::<Chunk>::new_uninit();
        let slots = &raw mut (*chunk.as_mut_ptr()).slots;
        for offset in 0..SLOTS_PER_CHUNK {
            (&raw mut (*slots)[offset]).write(NodeData::default());
        }
        chunk.assume_init()
    }
}

#[derive(Clone, Copy, Default)]
struct SlotMetadata {
    generation: u32,
    occupied: bool,
}

pub(crate) struct LayoutNodeArena {
    chunks: Vec<Box<Chunk>>,
    slot_metadata: Vec<SlotMetadata>,
    free_list: Vec<u32>,
    next_index: u32,
    live_count: u32,
    owner_thread: thread::ThreadId,
}

impl LayoutNodeArena {
    fn new() -> Self {
        Self {
            chunks: Vec::new(),
            slot_metadata: Vec::new(),
            free_list: Vec::new(),
            next_index: 0,
            live_count: 0,
            owner_thread: thread::current().id(),
        }
    }

    fn assert_owner_thread(&self) {
        debug_assert_eq!(self.owner_thread, thread::current().id());
    }

    // Freshly created chunks are default-initialized and free() resets slots on release, so
    // allocate() always hands out clean NodeData without writing it again.
    fn allocate(&mut self) -> NodeAllocation {
        self.assert_owner_thread();

        let index = if let Some(index) = self.free_list.pop() {
            index
        } else {
            let index = self.next_index;
            if (index as usize).is_multiple_of(SLOTS_PER_CHUNK) {
                self.chunks.push(new_chunk());
            }
            self.slot_metadata.push(SlotMetadata::default());
            self.next_index = self
                .next_index
                .checked_add(1)
                .expect("layout node arena exhausted its slot ID space");
            index
        };

        self.live_count = self
            .live_count
            .checked_add(1)
            .expect("layout node arena live count overflowed");

        let metadata = self.metadata_mut(index);
        assert!(!metadata.occupied, "layout node arena allocated a live slot");
        metadata.generation = metadata.generation.wrapping_add(1);
        if metadata.generation == 0 {
            metadata.generation = 1;
        }
        metadata.occupied = true;
        let generation = metadata.generation;

        NodeAllocation {
            slot: NodeSlotId { index },
            data: self.data_mut(index),
            generation,
        }
    }

    fn free(&mut self, id: NodeSlotId, generation: u32) {
        self.assert_owner_thread();

        let metadata = self.metadata_mut(id.index);
        assert!(metadata.occupied, "layout node arena freed an unused slot");
        assert_eq!(
            metadata.generation, generation,
            "layout node arena freed a stale slot generation"
        );
        metadata.occupied = false;
        *self.data_mut(id.index) = NodeData::default();

        self.live_count = self
            .live_count
            .checked_sub(1)
            .expect("layout node arena live count underflowed");
        self.free_list.push(id.index);
    }

    pub(crate) fn data(&self, id: NodeSlotId) -> *mut NodeData {
        assert!(!id.is_invalid(), "invalid layout node arena slot ID");
        debug_assert!(
            self.metadata(id.index).occupied,
            "layout node arena read an unused slot"
        );
        let index = id.index as usize;
        let chunk = self
            .chunks
            .get(index / SLOTS_PER_CHUNK)
            .expect("invalid layout node arena slot ID");
        (&raw const chunk.slots[index % SLOTS_PER_CHUNK]).cast_mut()
    }

    fn data_mut(&mut self, index: u32) -> &mut NodeData {
        let index = index as usize;
        let chunk = self
            .chunks
            .get_mut(index / SLOTS_PER_CHUNK)
            .expect("invalid layout node arena slot ID");
        &mut chunk.slots[index % SLOTS_PER_CHUNK]
    }

    fn metadata(&self, index: u32) -> &SlotMetadata {
        self.slot_metadata
            .get(index as usize)
            .expect("invalid layout node arena slot ID")
    }

    fn metadata_mut(&mut self, index: u32) -> &mut SlotMetadata {
        self.slot_metadata
            .get_mut(index as usize)
            .expect("invalid layout node arena slot ID")
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct NodeAllocation {
    pub slot: NodeSlotId,
    pub data: *mut NodeData,
    pub generation: u32,
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_arena_create() -> *mut c_void {
    abort_on_panic(|| Box::into_raw(Box::new(LayoutNodeArena::new())).cast())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_destroy(arena: *mut c_void) {
    abort_on_panic(|| {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: The handle came from layout_arena_create and ownership is
        // transferred back exactly once by the C++ RAII wrapper.
        let arena = unsafe { Box::from_raw(arena.cast::<LayoutNodeArena>()) };
        arena.assert_owner_thread();
        assert_eq!(arena.live_count, 0, "layout node arena destroyed with live slots");
    });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_allocate(arena: *mut c_void) -> NodeAllocation {
    abort_on_panic(|| {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: The C++ wrapper keeps the arena alive for this call and
        // serializes all access on the document thread.
        unsafe { &mut *arena.cast::<LayoutNodeArena>() }.allocate()
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_free(arena: *mut c_void, id: NodeSlotId, generation: u32) {
    abort_on_panic(|| {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: The C++ wrapper keeps the arena alive for this call and
        // serializes all access on the document thread.
        unsafe { &mut *arena.cast::<LayoutNodeArena>() }.free(id, generation);
    });
}

#[cfg(test)]
mod tests {
    use super::{Chunk, LayoutNodeArena, SLOTS_PER_CHUNK};

    #[test]
    fn node_data_addresses_remain_stable_when_chunks_are_added() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate();
        let first_data = first.data;

        let mut allocations = Vec::new();
        for _ in 0..SLOTS_PER_CHUNK * 2 {
            allocations.push(arena.allocate());
        }

        assert_eq!(first_data, arena.data(first.slot));
        // SAFETY: The first allocation is still live, and the comparison above
        // confirms that its pointer still addresses the arena slot.
        unsafe {
            (*first_data).initial_quote_nesting_level = 42;
            assert_eq!((*arena.data(first.slot)).initial_quote_nesting_level, 42);
        }
        arena.free(first.slot, first.generation);
        for allocation in allocations {
            arena.free(allocation.slot, allocation.generation);
        }
    }

    #[test]
    fn node_data_slots_are_cache_line_aligned() {
        assert_eq!(align_of::<Chunk>() % 64, 0);
        let mut arena = LayoutNodeArena::new();
        let allocation = arena.allocate();
        assert_eq!(allocation.data as usize % 64, 0);
        arena.free(allocation.slot, allocation.generation);
    }

    #[test]
    fn freed_slots_are_reused_with_a_new_generation() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate();
        arena.free(first.slot, first.generation);

        let second = arena.allocate();
        assert_eq!(second.slot, first.slot);
        assert_ne!(second.generation, first.generation);
        arena.free(second.slot, second.generation);
    }
}
