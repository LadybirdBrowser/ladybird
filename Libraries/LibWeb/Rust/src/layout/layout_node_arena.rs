/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;
use crate::layout::AbsposLayoutInputs;
use crate::layout::CssPixels;
use crate::layout::kind_is_box;
use crate::layout::node_data::{MAX_NODE_SLOT_COUNT, NodeData, NodeFlag, NodeSlotId};
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::hash::{Hash, Hasher};
use std::thread;

pub(crate) const SLOTS_PER_CHUNK: usize = 256;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct IntrinsicSizeCacheKey {
    pub(crate) measured_at_inline_size: Option<CssPixels>,
    pub(crate) percentage_basis_inline_size: Option<CssPixels>,
    pub(crate) percentage_basis_block_size: Option<CssPixels>,
    pub(crate) quirks_mode_percentage_basis_block_size: Option<CssPixels>,
}

impl Hash for IntrinsicSizeCacheKey {
    fn hash<H: Hasher>(&self, state: &mut H) {
        fn hash_optional<H: Hasher>(value: Option<CssPixels>, state: &mut H) {
            match value {
                Some(value) => {
                    true.hash(state);
                    value.raw_value().hash(state);
                }
                None => false.hash(state),
            }
        }

        hash_optional(self.measured_at_inline_size, state);
        hash_optional(self.percentage_basis_inline_size, state);
        hash_optional(self.percentage_basis_block_size, state);
        hash_optional(self.quirks_mode_percentage_basis_block_size, state);
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum IntrinsicSizeCacheKind {
    MinContentInline,
    MaxContentInline,
    MinContentBlock,
    MaxContentBlock,
}

#[derive(Default)]
struct IntrinsicSizeMaps {
    min_content_inline_size: HashMap<IntrinsicSizeCacheKey, CssPixels>,
    max_content_inline_size: HashMap<IntrinsicSizeCacheKey, CssPixels>,
    min_content_block_size: HashMap<IntrinsicSizeCacheKey, CssPixels>,
    max_content_block_size: HashMap<IntrinsicSizeCacheKey, CssPixels>,
}

impl IntrinsicSizeMaps {
    fn values(&self, kind: IntrinsicSizeCacheKind) -> &HashMap<IntrinsicSizeCacheKey, CssPixels> {
        match kind {
            IntrinsicSizeCacheKind::MinContentInline => &self.min_content_inline_size,
            IntrinsicSizeCacheKind::MaxContentInline => &self.max_content_inline_size,
            IntrinsicSizeCacheKind::MinContentBlock => &self.min_content_block_size,
            IntrinsicSizeCacheKind::MaxContentBlock => &self.max_content_block_size,
        }
    }

    fn values_mut(&mut self, kind: IntrinsicSizeCacheKind) -> &mut HashMap<IntrinsicSizeCacheKey, CssPixels> {
        match kind {
            IntrinsicSizeCacheKind::MinContentInline => &mut self.min_content_inline_size,
            IntrinsicSizeCacheKind::MaxContentInline => &mut self.max_content_inline_size,
            IntrinsicSizeCacheKind::MinContentBlock => &mut self.min_content_block_size,
            IntrinsicSizeCacheKind::MaxContentBlock => &mut self.max_content_block_size,
        }
    }
}

#[derive(Default)]
struct IntrinsicSizeCacheSlot {
    generation: u8,
    epoch: u16,
    sizes: Option<Box<IntrinsicSizeMaps>>,
}

#[derive(Default)]
struct SavedAbsposLayoutInputsSlot {
    generation: u8,
    inputs: Option<Box<AbsposLayoutInputs>>,
}

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
    intrinsic_size_caches: RefCell<Vec<IntrinsicSizeCacheSlot>>,
    saved_abspos_layout_inputs: RefCell<Vec<SavedAbsposLayoutInputsSlot>>,
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
            intrinsic_size_caches: RefCell::new(Vec::new()),
            saved_abspos_layout_inputs: RefCell::new(Vec::new()),
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

        if let Some(slot) = self.intrinsic_size_caches.get_mut().get_mut(index as usize) {
            *slot = IntrinsicSizeCacheSlot::default();
        }
        if let Some(slot) = self.saved_abspos_layout_inputs.get_mut().get_mut(index as usize) {
            *slot = SavedAbsposLayoutInputsSlot::default();
        }
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

    pub(crate) fn set_node_flag(&self, id: NodeSlotId, flag: NodeFlag, value: bool) {
        self.assert_owner_thread();
        let data = self.data(id);
        // SAFETY: data() validated that id names a live slot, and layout
        // serializes mutation on the arena's owner thread.
        unsafe {
            let flags = &raw mut (*data).flags;
            let mut updated = flags.read();
            if value {
                updated |= flag as u32;
            } else {
                updated &= !(flag as u32);
            }
            flags.write(updated);
        }
    }

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

    pub(crate) fn intrinsic_size_cache_get(
        &self,
        data: &NodeData,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
    ) -> Option<CssPixels> {
        if data.intrinsic_cache_epoch == u16::MAX {
            return None;
        }

        let (index, metadata) = self.slot_for_data(std::ptr::from_ref(data));
        let caches = self.intrinsic_size_caches.borrow();
        let slot = caches.get(index as usize)?;
        if slot.generation != metadata.generation || slot.epoch != data.intrinsic_cache_epoch {
            return None;
        }
        slot.sizes.as_ref()?.values(kind).get(&key).copied()
    }

    pub(crate) fn intrinsic_size_cache_put(
        &self,
        data: &NodeData,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        value: CssPixels,
    ) {
        if data.intrinsic_cache_epoch == u16::MAX {
            return;
        }

        let (index, metadata) = self.slot_for_data(std::ptr::from_ref(data));
        let mut caches = self.intrinsic_size_caches.borrow_mut();
        if caches.len() <= index as usize {
            caches.resize_with(index as usize + 1, IntrinsicSizeCacheSlot::default);
        }
        let slot = &mut caches[index as usize];
        if slot.generation != metadata.generation || slot.epoch != data.intrinsic_cache_epoch {
            *slot = IntrinsicSizeCacheSlot {
                generation: metadata.generation,
                epoch: data.intrinsic_cache_epoch,
                sizes: Some(Box::default()),
            };
        }
        slot.sizes
            .get_or_insert_with(Box::default)
            .values_mut(kind)
            .insert(key, value);
    }

    pub(crate) fn saved_abspos_layout_inputs(&self, data: *const NodeData) -> Option<AbsposLayoutInputs> {
        let (index, metadata) = self.slot_for_data(data);
        let slots = self.saved_abspos_layout_inputs.borrow();
        let inputs = slots
            .get(index as usize)
            .filter(|slot| slot.generation == metadata.generation)
            .and_then(|slot| slot.inputs.as_deref().copied());

        // SAFETY: slot_for_data() established that data points to a live slot
        // in this arena.
        let flags = unsafe { (&raw const (*data).flags).read() };
        assert_eq!(
            flags & NodeFlag::HasSavedAbsposLayoutInputs as u32 != 0,
            inputs.is_some(),
            "saved abspos input presence flag disagrees with the arena side table"
        );
        assert_eq!(
            flags & NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues as u32 != 0,
            inputs.is_some_and(|inputs| inputs.containing_block_info.derives_from_own_computed_values),
            "saved abspos containing-block flag disagrees with the arena side table"
        );
        assert_eq!(
            flags & NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues as u32 != 0,
            inputs.is_some_and(|inputs| { inputs.static_position_rect.alignment_derives_from_own_computed_values }),
            "saved abspos alignment flag disagrees with the arena side table"
        );
        inputs
    }

    pub(crate) fn set_saved_abspos_layout_inputs(&self, data: *mut NodeData, inputs: Option<AbsposLayoutInputs>) {
        let (index, metadata) = self.slot_for_data(data);
        let mut slots = self.saved_abspos_layout_inputs.borrow_mut();
        if slots.len() <= index as usize {
            slots.resize_with(index as usize + 1, SavedAbsposLayoutInputsSlot::default);
        }
        let slot = &mut slots[index as usize];
        if slot.generation != metadata.generation {
            *slot = SavedAbsposLayoutInputsSlot {
                generation: metadata.generation,
                inputs: inputs.map(Box::new),
            };
        } else if let Some(inputs) = inputs {
            if let Some(saved_inputs) = &mut slot.inputs {
                **saved_inputs = inputs;
            } else {
                slot.inputs = Some(Box::new(inputs));
            }
        } else {
            slot.inputs = None;
        }
        drop(slots);

        let saved_abspos_flags = NodeFlag::HasSavedAbsposLayoutInputs as u32
            | NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues as u32
            | NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues as u32;
        // SAFETY: slot_for_data() established that data points to a live slot
        // in this arena, and layout/tree building serialize mutation on the
        // arena's owner thread.
        unsafe {
            let flags = &raw mut (*data).flags;
            let mut value = flags.read() & !saved_abspos_flags;
            if let Some(inputs) = inputs {
                value |= NodeFlag::HasSavedAbsposLayoutInputs as u32;
                if inputs.containing_block_info.derives_from_own_computed_values {
                    value |= NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues as u32;
                }
                if inputs.static_position_rect.alignment_derives_from_own_computed_values {
                    value |= NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues as u32;
                }
            }
            flags.write(value);
        }
    }

    pub(crate) fn transfer_saved_abspos_layout_inputs(&self, old: NodeSlotId, new: NodeSlotId) {
        self.assert_owner_thread();
        assert_ne!(old, new, "cannot transfer saved abspos inputs to the same arena slot");

        let old_data = self.data(old);
        let new_data = self.data(new);
        // SAFETY: data() returns pointers to live slots.
        if !unsafe { kind_is_box((*old_data).kind) && kind_is_box((*new_data).kind) } {
            return;
        }
        if let Some(inputs) = self.saved_abspos_layout_inputs(old_data) {
            self.set_saved_abspos_layout_inputs(new_data, Some(inputs));
        }
    }

    pub(crate) unsafe fn from_handle<'a>(arena: *mut c_void) -> &'a Self {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: Layout passes borrow the document's arena synchronously,
        // and the document keeps it alive for the duration of the pass.
        unsafe { &*arena.cast::<Self>() }
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

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_transfer_saved_abspos_layout_inputs(
    arena: *mut c_void,
    old: NodeSlotId,
    new: NodeSlotId,
) {
    abort_on_panic(|| {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: Both slots belong to this live arena for the duration of
        // the synchronous replacement callback.
        unsafe { &*arena.cast::<LayoutNodeArena>() }.transfer_saved_abspos_layout_inputs(old, new);
    });
}

#[cfg(test)]
mod tests {
    use crate::layout::CssPixels;
    use crate::layout::layout_node_arena::{
        Chunk, IntrinsicSizeCacheKey, IntrinsicSizeCacheKind, LayoutNodeArena, SLOTS_PER_CHUNK,
    };
    use crate::layout::node_data::{NodeFlag, NodeKind};
    use crate::layout::{
        AbsposAlignment, AbsposAxisMode, AbsposContainingBlockInfo, AbsposLayoutInputs, StaticPositionAlignment,
        StaticPositionRect,
    };

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

    #[test]
    fn intrinsic_size_cache_validates_epoch_and_generation() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate();
        let key = IntrinsicSizeCacheKey {
            measured_at_inline_size: Some(CssPixels::from_raw(64)),
            ..Default::default()
        };
        let value = CssPixels::from_raw(128);

        // SAFETY: The allocation remains live until it is explicitly freed below.
        let first_data = unsafe { &mut *first.data };
        arena.intrinsic_size_cache_put(first_data, IntrinsicSizeCacheKind::MinContentBlock, key, value);
        assert_eq!(
            arena.intrinsic_size_cache_get(first_data, IntrinsicSizeCacheKind::MinContentBlock, key),
            Some(value)
        );

        first_data.intrinsic_cache_epoch += 1;
        assert_eq!(
            arena.intrinsic_size_cache_get(first_data, IntrinsicSizeCacheKind::MinContentBlock, key),
            None
        );
        arena.free(first.slot, first.generation);

        let second = arena.allocate();
        assert_eq!(second.slot.slot_index(), first.slot.slot_index());
        assert_ne!(second.slot, first.slot);
        // SAFETY: The second allocation is live and reuses the first allocation's slot.
        let second_data = unsafe { &*second.data };
        assert_eq!(
            arena.intrinsic_size_cache_get(second_data, IntrinsicSizeCacheKind::MinContentBlock, key),
            None
        );
        arena.free(second.slot, second.generation);
    }

    #[test]
    fn saved_abspos_inputs_transfer_and_validate_generation() {
        let mut arena = LayoutNodeArena::new();
        let old = arena.allocate();
        let new = arena.allocate();
        // SAFETY: Both allocations remain live.
        unsafe {
            (*old.data).kind = NodeKind::Box;
            (*new.data).kind = NodeKind::Box;
        }
        let inputs = AbsposLayoutInputs {
            static_position_rect: StaticPositionRect {
                rect: Default::default(),
                inline_alignment: StaticPositionAlignment::Center,
                block_alignment: StaticPositionAlignment::End,
                alignment_derives_from_own_computed_values: true,
            },
            containing_block_info: AbsposContainingBlockInfo {
                rect: Default::default(),
                inline_axis_mode: AbsposAxisMode::StaticPosition,
                block_axis_mode: AbsposAxisMode::InsetFromRect,
                inline_alignment: Some(AbsposAlignment::Center),
                block_alignment: None,
                derives_from_own_computed_values: true,
            },
        };

        arena.set_saved_abspos_layout_inputs(old.data, Some(inputs));
        assert_eq!(arena.saved_abspos_layout_inputs(old.data), Some(inputs));
        // SAFETY: Both allocations remain live.
        unsafe {
            assert_ne!((*old.data).flags & NodeFlag::HasSavedAbsposLayoutInputs as u32, 0);
            assert_ne!(
                (*old.data).flags & NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues as u32,
                0
            );
            assert_ne!(
                (*old.data).flags & NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues as u32,
                0
            );
        }

        arena.transfer_saved_abspos_layout_inputs(old.slot, new.slot);
        assert_eq!(arena.saved_abspos_layout_inputs(new.data), Some(inputs));

        arena.set_saved_abspos_layout_inputs(old.data, None);
        assert_eq!(arena.saved_abspos_layout_inputs(old.data), None);
        // SAFETY: The old allocation remains live.
        unsafe {
            let saved_abspos_flags = NodeFlag::HasSavedAbsposLayoutInputs as u32
                | NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues as u32
                | NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues as u32;
            assert_eq!((*old.data).flags & saved_abspos_flags, 0);
        }
        arena.free(old.slot, old.generation);

        let reused = arena.allocate();
        assert_eq!(reused.slot.slot_index(), old.slot.slot_index());
        assert_ne!(reused.slot, old.slot);
        assert_eq!(arena.saved_abspos_layout_inputs(reused.data), None);
        arena.free(reused.slot, reused.generation);
        arena.free(new.slot, new.generation);
    }
}
