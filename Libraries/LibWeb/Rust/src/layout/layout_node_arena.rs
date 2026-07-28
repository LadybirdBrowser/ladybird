/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;
use crate::layout::node_data::{MAX_NODE_SLOT_COUNT, NodeData, NodeSlotId};
use std::ffi::c_void;
use std::thread;

pub(crate) const SLOTS_PER_CHUNK: usize = 256;

// NodeData is sized to one cache line; the aligned chunk keeps every densely-strided slot
// line-aligned, and per-slot bookkeeping lives in a parallel array so it stays that way.
#[repr(align(64))]
pub(crate) struct Chunk {
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
    generation: u8,
    occupied: bool,
}

#[derive(Clone, Copy)]
#[allow(dead_code)]
struct ChunkAddress {
    start: usize,
    chunk_index: usize,
}

pub(crate) struct LayoutNodeArena {
    chunks: Vec<Box<Chunk>>,
    chunks_by_address: Vec<ChunkAddress>,
    slot_metadata: Vec<SlotMetadata>,
    free_list: Vec<u32>,
    next_index: u32,
    live_count: u32,
    owner_thread: thread::ThreadId,
}

impl LayoutNodeArena {
    pub(crate) fn new() -> Self {
        Self {
            chunks: Vec::new(),
            chunks_by_address: Vec::new(),
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
    pub(crate) fn allocate(&mut self) -> NodeAllocation {
        self.assert_owner_thread();

        let index = if let Some(index) = self.free_list.pop() {
            index
        } else {
            let index = self.next_index;
            assert!(
                index < MAX_NODE_SLOT_COUNT,
                "layout node arena exhausted its 24-bit slot index space"
            );
            if (index as usize).is_multiple_of(SLOTS_PER_CHUNK) {
                let chunk = new_chunk();
                let start = (&raw const chunk.slots) as usize;
                let chunk_index = self.chunks.len();
                let insertion_index = self.chunks_by_address.partition_point(|address| address.start < start);
                self.chunks_by_address
                    .insert(insertion_index, ChunkAddress { start, chunk_index });
                self.chunks.push(chunk);
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
        metadata.generation = metadata
            .generation
            .checked_add(1)
            .expect("retired layout node arena slot was reused");
        metadata.occupied = true;
        let generation = metadata.generation;
        self.data_mut(index).slot_generation = generation;

        NodeAllocation {
            slot: NodeSlotId::new(index, generation),
            data: self.data_mut(index),
            generation: u32::from(generation),
        }
    }

    pub(crate) fn free(&mut self, id: NodeSlotId, generation: u32) {
        self.assert_owner_thread();

        assert!(!id.is_invalid(), "invalid layout node arena slot ID");
        let index = id.slot_index();
        let id_generation = id.generation();
        assert_eq!(
            u32::from(id_generation),
            generation,
            "layout node arena slot ID and allocation generation disagree"
        );
        let metadata = self.metadata_mut(index);
        assert!(metadata.occupied, "layout node arena freed an unused slot");
        assert_eq!(
            metadata.generation, id_generation,
            "layout node arena freed a stale slot generation"
        );
        metadata.occupied = false;
        let should_reuse = metadata.generation != u8::MAX;
        *self.data_mut(index) = NodeData::default();

        self.live_count = self
            .live_count
            .checked_sub(1)
            .expect("layout node arena live count underflowed");
        if should_reuse {
            self.free_list.push(index);
        }
    }

    pub(crate) fn data(&self, id: NodeSlotId) -> *mut NodeData {
        assert!(!id.is_invalid(), "invalid layout node arena slot ID");
        let index = id.slot_index() as usize;
        let chunk = self
            .chunks
            .get(index / SLOTS_PER_CHUNK)
            .expect("invalid layout node arena slot ID");
        let data = (&raw const chunk.slots[index % SLOTS_PER_CHUNK]).cast_mut();
        // SAFETY: The chunk bounds check above established that data addresses
        // an initialized NodeData slot.
        let generation = unsafe { (&raw const (*data).slot_generation).read() };
        assert_eq!(
            generation,
            id.generation(),
            "layout node arena read a stale or unused slot"
        );
        data
    }

    #[allow(dead_code)]
    fn slot_for_data(&self, data: *const NodeData) -> (u32, SlotMetadata) {
        assert!(!data.is_null(), "layout node arena data pointer is null");
        let data_address = data as usize;
        let slot_size = size_of::<NodeData>();

        let address_index = self
            .chunks_by_address
            .partition_point(|address| address.start <= data_address);
        assert_ne!(
            address_index, 0,
            "layout node data pointer does not belong to this arena"
        );
        let address = self.chunks_by_address[address_index - 1];
        let chunk_end = address.start + size_of::<[NodeData; SLOTS_PER_CHUNK]>();
        assert!(
            data_address < chunk_end,
            "layout node data pointer does not belong to this arena"
        );

        let offset = data_address - address.start;
        assert_eq!(offset % slot_size, 0, "unaligned layout node arena data pointer");
        let index = address.chunk_index * SLOTS_PER_CHUNK + offset / slot_size;
        let index = u32::try_from(index).expect("layout node arena slot index overflowed");
        let metadata = *self.metadata(index);
        assert!(metadata.occupied, "layout node arena access for an unused slot");
        // SAFETY: The range and alignment checks established that data points
        // to the indexed NodeData slot.
        let generation = unsafe { (&raw const (*data).slot_generation).read() };
        assert_eq!(
            generation, metadata.generation,
            "layout node arena access used a stale slot"
        );
        (index, metadata)
    }

    #[allow(dead_code)]
    pub(crate) fn slot_index_for_data(&self, data: &NodeData) -> u32 {
        self.slot_for_data(std::ptr::from_ref(data)).0
    }

    #[allow(dead_code)]
    pub(crate) fn is_before(&self, node: &NodeData, other: &NodeData) -> bool {
        let (node_index, node_metadata) = self.slot_for_data(std::ptr::from_ref(node));
        let (other_index, other_metadata) = self.slot_for_data(std::ptr::from_ref(other));
        let mut node = NodeSlotId::new(node_index, node_metadata.generation);
        let mut other = NodeSlotId::new(other_index, other_metadata.generation);
        assert_ne!(node, other, "a layout node cannot precede itself");

        let depth = |mut slot: NodeSlotId| {
            let mut depth = 0usize;
            while !slot.is_invalid() {
                depth += 1;
                // SAFETY: data() validates the live generation, and the
                // topology links name slots in this arena.
                slot = unsafe { (*self.data(slot)).parent };
            }
            depth
        };
        let node_depth = depth(node);
        let other_depth = depth(other);

        for _ in other_depth..node_depth {
            // SAFETY: node is a validated live arena slot.
            node = unsafe { (*self.data(node)).parent };
        }
        for _ in node_depth..other_depth {
            // SAFETY: other is a validated live arena slot.
            other = unsafe { (*self.data(other)).parent };
        }
        if node == other {
            return node_depth < other_depth;
        }

        loop {
            // SAFETY: Both slots are live and have equal depth.
            let node_parent = unsafe { (*self.data(node)).parent };
            // SAFETY: Both slots are live and have equal depth.
            let other_parent = unsafe { (*self.data(other)).parent };
            assert_eq!(
                node_parent.is_invalid(),
                other_parent.is_invalid(),
                "layout nodes belong to different trees"
            );
            if node_parent == other_parent {
                break;
            }
            node = node_parent;
            other = other_parent;
        }

        while !other.is_invalid() {
            if node == other {
                return true;
            }
            // SAFETY: other is a validated live arena slot.
            other = unsafe { (*self.data(other)).previous_sibling };
        }
        false
    }

    fn data_mut(&mut self, index: u32) -> &mut NodeData {
        let index = index as usize;
        let chunk = self
            .chunks
            .get_mut(index / SLOTS_PER_CHUNK)
            .expect("invalid layout node arena slot ID");
        &mut chunk.slots[index % SLOTS_PER_CHUNK]
    }

    #[allow(dead_code)]
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
    use crate::layout::layout_node_arena::{Chunk, LayoutNodeArena, SLOTS_PER_CHUNK};

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
        assert_eq!(second.slot.slot_index(), first.slot.slot_index());
        assert_ne!(second.slot, first.slot);
        assert_ne!(second.generation, first.generation);
        arena.free(second.slot, second.generation);
    }

    #[test]
    fn stale_slot_ids_do_not_resolve_to_a_new_occupant() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate();
        arena.free(first.slot, first.generation);
        let second = arena.allocate();

        let stale_read = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| arena.data(first.slot)));
        assert!(stale_read.is_err());
        arena.free(second.slot, second.generation);
    }
}
