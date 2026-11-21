/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Evacuation: a collection that does not scan the stack may move the live cells out of some blocks, into free slots of
//! other blocks of the same allocator.
//!
//! An edge that marking visits through its slot can be updated once its target moves, while an edge visited by value
//! pins its target. Marking pins the cells of evacuated blocks that something visits by value, or that are roots, and
//! records the slots that point into evacuated blocks. After marking, a cell of an evacuated block moves if its
//! allocator is movable and nothing pinned it: it is copied bytewise to its new slot, and its old slot is freed without
//! being destroyed. Then every recorded slot that points at a moved cell is rewritten, after the slot itself is found
//! in the copy of its cell if that moved too.
//!
//! The collector finds slots only in cells, in the heap region: a slot elsewhere, like in malloc memory that a cell
//! owns, could be freed or moved by the finalizers and destructors that run between marking and moving, so the visit
//! of such a slot pins its target instead.

use core::ffi::c_void;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use crate::block::{
    BLOCK_SIZE, BlockDescriptor, CellIndex, MARK_BITMAP_WORDS, MAX_CELLS_PER_BLOCK, descriptor_for, descriptor_of_heap,
    set_evacuation_of_block,
};
use crate::identity_hash::{CompressedAddress, IDENTITY_HASH_STATE_OFFSET, identity_hashes_of};
use crate::marker::COMPRESSED_POINTER_SHIFT;
use crate::rust_panic::abort_on_panic;

/// Why a cell of an evacuated block stays where it is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum PinReason {
    Unpinned = 0,
    /// Something visited the cell by value rather than through a slot the collector can update.
    Edge = 1,
    /// The cell is a root, which roots hand over by value.
    Root = 2,
}

/// What a collection that evacuates a block knows about the block's cells.
pub struct Evacuation {
    pinned_by_edge: [AtomicU64; MARK_BITMAP_WORDS],
    pinned_by_root: [AtomicU64; MARK_BITMAP_WORDS],
    /// The compressed address each cell that moved moved to, by cell index, and 0 for the cells that did not.
    forwarding: [AtomicU32; MAX_CELLS_PER_BLOCK],
}

impl Evacuation {
    fn new() -> Box<Self> {
        Box::new(Self {
            pinned_by_edge: [const { AtomicU64::new(0) }; MARK_BITMAP_WORDS],
            pinned_by_root: [const { AtomicU64::new(0) }; MARK_BITMAP_WORDS],
            forwarding: [const { AtomicU32::new(0) }; MAX_CELLS_PER_BLOCK],
        })
    }

    /// Pins the cell at `address` of the block this evacuates, which markers may do from several threads at once.
    #[inline(always)]
    pub fn pin(&self, descriptor: &BlockDescriptor, address: usize, reason: PinReason) {
        let index = descriptor.cell_index(address).0;
        let bits = match reason {
            PinReason::Root => &self.pinned_by_root,
            _ => &self.pinned_by_edge,
        };
        let word = &bits[index / 64];
        let bit = 1u64 << (index % 64);
        // NB: As with mark bits, a bit that is set already costs no read-modify-write.
        if word.load(Ordering::Relaxed) & bit == 0 {
            word.fetch_or(bit, Ordering::Relaxed);
        }
    }

    pub fn pin_reason(&self, index: CellIndex) -> PinReason {
        let bit = 1u64 << (index.0 % 64);
        if self.pinned_by_root[index.0 / 64].load(Ordering::Relaxed) & bit != 0 {
            return PinReason::Root;
        }
        if self.pinned_by_edge[index.0 / 64].load(Ordering::Relaxed) & bit != 0 {
            return PinReason::Edge;
        }
        PinReason::Unpinned
    }

    fn forwarding(&self, index: CellIndex) -> Option<CompressedAddress> {
        let compressed = self.forwarding[index.0].load(Ordering::Relaxed);
        (compressed != 0).then_some(CompressedAddress(compressed))
    }
}

/// The kinds of slots that marking records, which differ in how they hold the address of their target.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(usize)]
pub enum SlotKind {
    /// A 32-bit compressed cell pointer, 0 for null, like GC::Member.
    Member = 0,
    /// A NaN-boxed value, which may hold a cell.
    Value = 1,
    /// A raw pointer into a cell, at the cell or past its start, as an object keeps to its property storage.
    InteriorPointer = 2,
    /// A raw pointer into the cell that holds it, as an object keeps to its inline property storage. It may point one
    /// past the end of the cell's own fields, at the start of the next cell, which only its holder tells apart.
    SelfPointer = 3,
}

const SLOT_KIND_MASK: usize = 3;

/// The address of a slot that marking recorded, with its kind in the low bits. Every slot is at least 4-aligned.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RecordedSlot(usize);

impl RecordedSlot {
    #[inline(always)]
    pub fn new(address: usize, kind: SlotKind) -> Self {
        debug_assert!(address & SLOT_KIND_MASK == 0, "slots are at least 4-aligned");
        Self(address | kind as usize)
    }

    pub fn address(self) -> usize {
        self.0 & !SLOT_KIND_MASK
    }

    pub fn kind(self) -> SlotKind {
        match self.0 & SLOT_KIND_MASK {
            0 => SlotKind::Member,
            1 => SlotKind::Value,
            2 => SlotKind::InteriorPointer,
            _ => SlotKind::SelfPointer,
        }
    }
}

/// Where the collector finds what it needs to move cells and fix up slots.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EvacuationLayout {
    pub heap_region_base: usize,
    /// How far into a block its first cell is.
    pub block_cells_offset: u32,
    /// As in MarkerLayout.
    pub value_is_cell_pattern: u64,
    pub value_offset_mask: u64,
}

/// The blocks a collection evacuates, and the slots its marking recorded.
pub struct EvacuationSet {
    layout: EvacuationLayout,
    heap: *mut c_void,
    blocks: Vec<(usize, Box<Evacuation>)>,
    slots: Vec<Vec<RecordedSlot>>,
}

impl EvacuationSet {
    pub(crate) fn add_recorded_slots(&mut self, slots: Vec<RecordedSlot>) {
        if !slots.is_empty() {
            self.slots.push(slots);
        }
    }

    #[inline(always)]
    fn is_in_heap_region(&self, address: usize) -> bool {
        (address.wrapping_sub(self.layout.heap_region_base) as u64) <= self.layout.value_offset_mask
    }

    /// The evacuation of the live block that `address` is in, with its descriptor.
    #[inline(always)]
    fn evacuation_of(&self, address: usize) -> Option<(&BlockDescriptor, &Evacuation)> {
        if !self.is_in_heap_region(address) {
            return None;
        }
        // SAFETY: The address is in the region, whose descriptors are backed by memory wherever a block ever was, and
        //         read as zero elsewhere.
        let descriptor = unsafe { descriptor_of_heap(self.layout.heap_region_base, address, self.heap) }?;
        Some((descriptor, descriptor.evacuation()?))
    }

    fn decompress(&self, compressed: CompressedAddress) -> usize {
        self.layout.heap_region_base + ((compressed.0 as usize) << COMPRESSED_POINTER_SHIFT)
    }

    /// Where the cell that starts at `cell` moved to, if it moved.
    #[inline(always)]
    fn forwarding_address(&self, cell: usize) -> Option<usize> {
        let (descriptor, evacuation) = self.evacuation_of(cell)?;
        evacuation
            .forwarding(descriptor.cell_index(cell))
            .map(|to| self.decompress(to))
    }

    /// Where the byte at `address` is now: in the copy of its cell if that moved.
    #[inline(always)]
    fn relocate(&self, address: usize) -> usize {
        let Some((descriptor, evacuation)) = self.evacuation_of(address) else {
            return address;
        };
        let cell = descriptor.cell_containing(address, self.layout.block_cells_offset as usize);
        match evacuation.forwarding(descriptor.cell_index(cell)) {
            Some(to) => self.decompress(to) + (address - cell),
            None => address,
        }
    }

    /// Rewrites a slot that marking recorded to point where its target moved, and returns whether it did.
    ///
    /// # Safety
    ///
    /// The slot must be in a live cell, and hold what its kind says.
    unsafe fn fix_up(&self, slot: RecordedSlot) -> bool {
        let base = self.layout.heap_region_base;
        let address = self.relocate(slot.address());
        // SAFETY: The caller passes a slot of a live cell, which is now at `address`.
        unsafe {
            match slot.kind() {
                SlotKind::Member => {
                    let slot = address as *mut u32;
                    let compressed = slot.read();
                    if compressed == 0 {
                        return false;
                    }
                    let Some(to) = self.forwarding_address(self.decompress(CompressedAddress(compressed))) else {
                        return false;
                    };
                    slot.write((to >> COMPRESSED_POINTER_SHIFT) as u32);
                }
                SlotKind::Value => {
                    let slot = address as *mut u64;
                    let value = slot.read();
                    if value & self.layout.value_is_cell_pattern != self.layout.value_is_cell_pattern {
                        return false;
                    }
                    let mask = self.layout.value_offset_mask;
                    let Some(to) = self.forwarding_address(base + (value & mask) as usize) else {
                        return false;
                    };
                    slot.write((value & !mask) | ((to - base) as u64 & mask));
                }
                SlotKind::InteriorPointer => {
                    let slot = address as *mut usize;
                    let pointer = slot.read();
                    let Some((descriptor, _)) = self.evacuation_of(pointer) else {
                        return false;
                    };
                    let cell = descriptor.cell_containing(pointer, self.layout.block_cells_offset as usize);
                    let Some(to) = self.forwarding_address(cell) else {
                        return false;
                    };
                    slot.write(to + (pointer - cell));
                }
                SlotKind::SelfPointer => {
                    let Some((descriptor, _)) = self.evacuation_of(slot.address()) else {
                        return false;
                    };
                    let holder = descriptor.cell_containing(slot.address(), self.layout.block_cells_offset as usize);
                    let Some(to) = self.forwarding_address(holder) else {
                        return false;
                    };
                    let slot = address as *mut usize;
                    let pointer = slot.read();
                    // NB: The pointer may point one past the holder's fields, at the start of the next cell.
                    if pointer.wrapping_sub(holder) > descriptor.cell_size as usize {
                        return false;
                    }
                    slot.write(to + (pointer - holder));
                }
            }
        }
        true
    }
}

/// Starts the evacuation state of a collection of `heap`, which owns the evacuations of the blocks it adds.
#[unsafe(no_mangle)]
pub extern "C" fn rust_gc_evacuation_create(layout: EvacuationLayout, heap: *mut c_void) -> *mut EvacuationSet {
    Box::into_raw(Box::new(EvacuationSet {
        layout,
        heap,
        blocks: Vec::new(),
        slots: Vec::new(),
    }))
}

/// Has the collection evacuate the block at `block`, which must belong to the set's heap. Must be called before marking.
///
/// # Safety
///
/// `set` must be live, and `block` a live block of its heap that the set does not have yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_add_block(set: *mut EvacuationSet, block: *mut c_void) {
    // SAFETY: The caller passes a live set.
    let set = unsafe { &mut *set };
    let address = block.addr();
    debug_assert!(address & (BLOCK_SIZE - 1) == 0);
    // SAFETY: The caller passes a live block.
    let descriptor = unsafe { descriptor_for(set.layout.heap_region_base, address) };
    debug_assert!(descriptor.heap() == set.heap && descriptor.evacuation().is_none());
    let evacuation = Evacuation::new();
    // SAFETY: The caller passes a live block, and no marker runs yet.
    unsafe {
        set_evacuation_of_block(
            set.layout.heap_region_base,
            address,
            core::ptr::from_ref(&*evacuation).cast_mut(),
        );
    }
    set.blocks.push((address, evacuation));
}

/// Why a live cell of an evacuated block was pinned.
///
/// # Safety
///
/// `set` must be live, and `cell` a cell of one of its blocks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_pin_reason(set: *const EvacuationSet, cell: *const c_void) -> PinReason {
    // SAFETY: The caller passes a live set.
    let set = unsafe { &*set };
    let (descriptor, evacuation) = set
        .evacuation_of(cell.addr())
        .expect("the cell is in an evacuated block");
    evacuation.pin_reason(descriptor.cell_index(cell.addr()))
}

/// A cell to move, from its slot to a free slot of another block of its allocator.
#[repr(C)]
pub struct CellMove {
    pub from: *mut c_void,
    pub to: *mut c_void,
}

/// Copies each cell to its new slot, and notes where it went. Both slots are `cell_size` bytes.
///
/// # Safety
///
/// `set` must be live, and the moves name live, unpinned cells of its blocks and free slots of the same allocators,
/// each once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_move_cells(
    set: *mut EvacuationSet,
    moves: *const CellMove,
    count: usize,
    cell_size: usize,
) {
    abort_on_panic(|| {
        // SAFETY: The caller passes a live set, and count moves.
        let (set, moves) = unsafe { (&mut *set, ak::slice_from_raw_parts(moves, count)) };
        let mut identity_hashes = None;
        for cell_move in moves {
            let (from, to) = (cell_move.from.addr(), cell_move.to.addr());
            let (descriptor, evacuation) = set.evacuation_of(from).expect("the cell is in an evacuated block");
            debug_assert!(descriptor.cell_size as usize == cell_size);
            let index = descriptor.cell_index(from);
            debug_assert!(evacuation.forwarding(index).is_none());
            // SAFETY: The caller passes a live cell and a free slot of the same size, which do not overlap.
            unsafe {
                core::ptr::copy_nonoverlapping(cell_move.from.cast::<u8>(), cell_move.to.cast::<u8>(), cell_size);
                let state = cell_move.to.cast::<u8>().add(IDENTITY_HASH_STATE_OFFSET);
                if state.read() != 0 {
                    let table = identity_hashes.get_or_insert_with(|| identity_hashes_of(set.heap));
                    state.write(table.did_move(
                        state.read(),
                        CompressedAddress((from >> COMPRESSED_POINTER_SHIFT) as u32),
                        CompressedAddress((to >> COMPRESSED_POINTER_SHIFT) as u32),
                    ));
                }
            }
            evacuation.forwarding[index.0].store((to >> COMPRESSED_POINTER_SHIFT) as u32, Ordering::Relaxed);
        }
    });
}

/// Rewrites every slot that marking recorded and that points at a cell that moved, and returns how many it rewrote.
///
/// # Safety
///
/// `set` must be live, every cell has to be moved already, and the cells that held recorded slots must still be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_fix_up_slots(set: *mut EvacuationSet) -> usize {
    abort_on_panic(|| {
        // SAFETY: The caller passes a live set.
        let set = unsafe { &*set };
        let mut rewritten = 0;
        for slots in &set.slots {
            for &slot in slots {
                // SAFETY: Marking recorded the slot in a live cell, as the caller makes sure it still is.
                if unsafe { set.fix_up(slot) } {
                    rewritten += 1;
                }
            }
        }
        rewritten
    })
}

/// How many slots marking recorded.
///
/// # Safety
///
/// `set` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_recorded_slot_count(set: *const EvacuationSet) -> usize {
    // SAFETY: The caller passes a live set.
    unsafe { &*set }.slots.iter().map(Vec::len).sum()
}

/// Where the cell at `cell` moved to, or null if it did not move.
///
/// # Safety
///
/// `set` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_forwarding_address(
    set: *const EvacuationSet,
    cell: *const c_void,
) -> *mut c_void {
    // SAFETY: The caller passes a live set.
    let set = unsafe { &*set };
    match set.forwarding_address(cell.addr()) {
        Some(address) => core::ptr::without_provenance_mut(address),
        None => core::ptr::null_mut(),
    }
}

/// Ends the evacuation state of a collection: its blocks are no longer evacuated.
///
/// # Safety
///
/// `set` must be live, and nothing may use it after.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_destroy(set: *mut EvacuationSet) {
    // SAFETY: The caller passes a live set, which it gives up.
    let set = unsafe { Box::from_raw(set) };
    for (block, evacuation) in &set.blocks {
        // SAFETY: Every block of the set was in the region, where descriptors stay backed by memory.
        let descriptor = unsafe { descriptor_for(set.layout.heap_region_base, *block) };
        // NB: A block that went away and came back since has a fresh descriptor, which may not be cleared.
        if descriptor
            .evacuation()
            .is_some_and(|current| core::ptr::eq(current, &raw const **evacuation))
        {
            // SAFETY: The block is in the region, and no marker runs.
            unsafe { set_evacuation_of_block(set.layout.heap_region_base, *block, core::ptr::null_mut()) };
        }
    }
}

/// Whether the collection will move the cell at `cell`: once marking is done, a live cell of an evacuated block moves if
/// its allocator is movable and nothing pinned it. Caches that hold cells weakly drop such a cell as if it died.
///
/// # Safety
///
/// `set` must be live, and its marking done.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_will_move(set: *const EvacuationSet, cell: *const c_void) -> bool {
    // SAFETY: The caller passes a live set.
    let set = unsafe { &*set };
    let Some((descriptor, evacuation)) = set.evacuation_of(cell.addr()) else {
        return false;
    };
    descriptor.movable
        && descriptor.is_marked(cell.addr())
        && evacuation.pin_reason(descriptor.cell_index(cell.addr())) == PinReason::Unpinned
}
