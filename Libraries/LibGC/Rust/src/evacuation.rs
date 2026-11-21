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
//! allocator is movable and nothing pinned it. Each cell that moves gets a free slot of another block first, then every
//! recorded slot that points at one is rewritten where it is, and only then is each cell copied bytewise to its new
//! slot, with its slots rewritten already, and its old slot freed without being destroyed. Cells move before the sweep,
//! so a dead cell's slot is never taken, and the sweep that follows destroys the dead cells once the collection is
//! over.
//!
//! The collector finds slots in cells, in the heap region: a slot elsewhere, like in malloc memory that a cell owns,
//! could be freed or moved by the finalizers and sweep callbacks that run between marking and moving, so the visit of
//! such a slot pins its target instead. The exception is memory that a live cell owns and that nothing frees, moves or
//! writes until the collection is over, whose values the cell visits as owned values (see
//! rust_gc_marker_visit_owned_values()). Holders of weak references, whose memory changes as they forget the cells that
//! died, hand their slots over once they did, before cells move (see rust_gc_evacuation_record_slot()).

use core::ffi::c_void;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use crate::block::{
    BLOCK_SIZE, BlockDescriptor, CELL_BITMAP_WORDS, CellIndex, MAX_CELLS_PER_BLOCK, descriptor_for, descriptor_of_heap,
    set_evacuation_of_block,
};
use crate::identity_hash::{CompressedAddress, IDENTITY_HASH_STATE_OFFSET, identity_hashes_of};
use crate::marker::COMPRESSED_POINTER_SHIFT;
use crate::os;
use crate::rust_panic::abort_on_panic;

/// Zero-filled room for `capacity` values of `T`, mapped from the system and given back to it as a whole when dropped,
/// for what a collection that moves cells keeps until it is over. Only the pages that are written to take memory.
///
/// NB: A compacting collection needs a burst of memory that it frees all at once. Malloc would keep those pages for
///     later and hand them out among long-lived allocations, which then keep them in the process.
struct MappedArray<T> {
    pointer: NonNull<T>,
    capacity: usize,
}

impl<T> MappedArray<T> {
    /// # Safety
    ///
    /// All-zero bytes must be a valid `T`.
    unsafe fn new_zeroed(capacity: usize) -> Self {
        let capacity = capacity.max(1);
        let size = capacity * size_of::<T>();
        let pointer = os::reserve_zeroed(size).expect("map memory for a collection that moves cells");
        // SAFETY: The range is all of what reserve_zeroed() just returned.
        unsafe { os::commit_reserved_zeroed(pointer, size) };
        Self {
            pointer: pointer.cast(),
            capacity,
        }
    }

    fn get(&self, index: usize) -> &T {
        assert!(index < self.capacity);
        // SAFETY: The index is in bounds, and every value is valid from the start, as new_zeroed() requires.
        unsafe { self.pointer.add(index).as_ref() }
    }
}

impl<T> Drop for MappedArray<T> {
    fn drop(&mut self) {
        // SAFETY: The range is all of the reservation, which goes away with this.
        unsafe { os::release(self.pointer.cast(), self.capacity * size_of::<T>()) };
    }
}

/// How many slots a chunk of recorded slots has room for: 1 MiB of address space.
const RECORDED_SLOTS_PER_CHUNK: usize = 128 * 1024;

/// The slots one marker records, in chunks of mapped memory (see MappedArray).
pub struct RecordedSlots {
    /// The chunks, the one being filled last.
    chunks: Vec<MappedArray<RecordedSlot>>,
    /// Where the next slot goes in the last chunk, and the end of that chunk; both null before the first chunk.
    next: *mut RecordedSlot,
    end: *mut RecordedSlot,
}

impl Default for RecordedSlots {
    fn default() -> Self {
        Self {
            chunks: Vec::new(),
            next: core::ptr::null_mut(),
            end: core::ptr::null_mut(),
        }
    }
}

impl RecordedSlots {
    #[inline(always)]
    pub fn push(&mut self, slot: RecordedSlot) {
        if self.next == self.end {
            self.add_chunk();
        }
        // SAFETY: The last chunk has room at next, which stays within it.
        unsafe {
            self.next.write(slot);
            self.next = self.next.add(1);
        }
    }

    #[cold]
    fn add_chunk(&mut self) {
        // SAFETY: A RecordedSlot is an integer.
        let chunk = unsafe { MappedArray::<RecordedSlot>::new_zeroed(RECORDED_SLOTS_PER_CHUNK) };
        self.next = chunk.pointer.as_ptr();
        // SAFETY: The chunk has room for its capacity.
        self.end = unsafe { self.next.add(chunk.capacity) };
        self.chunks.push(chunk);
    }

    pub fn is_empty(&self) -> bool {
        self.chunks.is_empty()
    }

    /// The recorded slots, chunk by chunk.
    fn slices(&self) -> impl Iterator<Item = &[RecordedSlot]> {
        let last = self.chunks.len().wrapping_sub(1);
        self.chunks.iter().enumerate().map(move |(index, chunk)| {
            let length = if index == last {
                // SAFETY: next is in the last chunk.
                unsafe { self.next.offset_from(chunk.pointer.as_ptr()) as usize }
            } else {
                chunk.capacity
            };
            // SAFETY: The first length slots of the chunk were written.
            unsafe { ak::slice_from_raw_parts(chunk.pointer.as_ptr(), length) }
        })
    }
}

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

/// What a collection that evacuates a block knows about the block's cells. All zeros is nothing pinned or moved yet.
pub struct Evacuation {
    pinned_by_edge: [AtomicU64; CELL_BITMAP_WORDS],
    pinned_by_root: [AtomicU64; CELL_BITMAP_WORDS],
    /// The compressed address each cell that moved moved to, by cell index, and 0 for the cells that did not.
    forwarding: [AtomicU32; MAX_CELLS_PER_BLOCK],
}

impl Evacuation {
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

    /// Whether the cell at `address` of the block this evacuates is pinned, for whatever reason.
    #[inline(always)]
    pub fn is_pinned(&self, descriptor: &BlockDescriptor, address: usize) -> bool {
        let index = descriptor.cell_index(address).0;
        let bit = 1u64 << (index % 64);
        (self.pinned_by_root[index / 64].load(Ordering::Relaxed)
            | self.pinned_by_edge[index / 64].load(Ordering::Relaxed))
            & bit
            != 0
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
///
/// NB: The address is kept as an integer, so that a slot costs one word in the chunks that markers fill on several
///     threads. Recording a slot exposes its provenance, which pointer() picks up again.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RecordedSlot(usize);

impl RecordedSlot {
    #[inline(always)]
    pub fn new(slot: *const u8, kind: SlotKind) -> Self {
        let address = slot.expose_provenance();
        debug_assert!(address & SLOT_KIND_MASK == 0, "slots are at least 4-aligned");
        Self(address | kind as usize)
    }

    pub fn address(self) -> usize {
        self.0 & !SLOT_KIND_MASK
    }

    /// The slot, with the provenance that new() exposed.
    fn pointer<T>(self) -> *mut T {
        core::ptr::with_exposed_provenance_mut(self.address())
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
    /// The evacuated blocks, and their evacuations at the same indices.
    blocks: Vec<usize>,
    evacuations: MappedArray<Evacuation>,
    slots: Vec<RecordedSlots>,
    /// The slots handed over after marking, see rust_gc_evacuation_record_slot().
    slots_recorded_after_marking: RecordedSlots,
}

impl EvacuationSet {
    pub(crate) fn add_recorded_slots(&mut self, slots: RecordedSlots) {
        if !slots.is_empty() {
            self.slots.push(slots);
        }
    }

    #[inline(always)]
    fn is_in_heap_region(&self, address: usize) -> bool {
        (address.wrapping_sub(self.layout.heap_region_base) as u64) <= self.layout.value_offset_mask
    }

    /// Every recorded slot, chunk by chunk.
    fn all_slots(&self) -> impl Iterator<Item = &[RecordedSlot]> {
        self.slots
            .iter()
            .chain(core::iter::once(&self.slots_recorded_after_marking))
            .flat_map(RecordedSlots::slices)
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

    /// A pointer to `address` in the heap region, whose provenance rust_gc_heap_region_init() exposed.
    fn heap_region_pointer(address: usize) -> *mut u8 {
        core::ptr::with_exposed_provenance_mut(address)
    }

    /// Where the cell that starts at `cell` moved to, if it moved.
    #[inline(always)]
    fn forwarding_address(&self, cell: usize) -> Option<usize> {
        let (descriptor, evacuation) = self.evacuation_of(cell)?;
        evacuation
            .forwarding(descriptor.cell_index(cell))
            .map(|to| self.decompress(to))
    }

    /// Rewrites a slot that marking recorded to point where its target moves, and returns whether it did. Cells are only
    /// copied once their slots are rewritten, so the slot is still where marking found it, and the copy of a cell that
    /// moves gets the rewritten slots.
    ///
    /// # Safety
    ///
    /// The slot must be in a live cell that has not moved yet, or in memory that such a cell owns, and hold what its
    /// kind says.
    unsafe fn fix_up(&self, slot: RecordedSlot) -> bool {
        let base = self.layout.heap_region_base;
        let address = slot.address();
        // SAFETY: The caller passes a slot of a live cell.
        unsafe {
            match slot.kind() {
                SlotKind::Member => {
                    let slot = slot.pointer::<u32>();
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
                    let slot = slot.pointer::<u64>();
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
                    let slot = slot.pointer::<*mut u8>();
                    let pointer = slot.read().addr();
                    let Some((descriptor, _)) = self.evacuation_of(pointer) else {
                        return false;
                    };
                    let cell = descriptor.cell_containing(pointer, self.layout.block_cells_offset as usize);
                    let Some(to) = self.forwarding_address(cell) else {
                        return false;
                    };
                    slot.write(Self::heap_region_pointer(to).wrapping_add(pointer - cell));
                }
                SlotKind::SelfPointer => {
                    let Some((descriptor, _)) = self.evacuation_of(address) else {
                        return false;
                    };
                    let holder = descriptor.cell_containing(address, self.layout.block_cells_offset as usize);
                    let Some(to) = self.forwarding_address(holder) else {
                        return false;
                    };
                    let slot = slot.pointer::<*mut u8>();
                    let pointer = slot.read().addr();
                    // NB: The pointer may point one past the holder's fields, at the start of the next cell.
                    if pointer.wrapping_sub(holder) > descriptor.cell_size as usize {
                        return false;
                    }
                    slot.write(Self::heap_region_pointer(to).wrapping_add(pointer - holder));
                }
            }
        }
        true
    }
}

/// Starts the evacuation state of a collection of `heap`, which evacuates up to `block_count` blocks that it adds, and
/// owns their evacuations.
#[unsafe(no_mangle)]
pub extern "C" fn rust_gc_evacuation_create(
    layout: EvacuationLayout,
    heap: *mut c_void,
    block_count: usize,
) -> *mut EvacuationSet {
    Box::into_raw(Box::new(EvacuationSet {
        layout,
        heap,
        blocks: Vec::with_capacity(block_count),
        // SAFETY: All zeros is an evacuation that pinned and moved nothing.
        evacuations: unsafe { MappedArray::new_zeroed(block_count) },
        slots: Vec::new(),
        slots_recorded_after_marking: RecordedSlots::default(),
    }))
}

/// Has the collection evacuate the block at `block`, which must belong to the set's heap. Must be called before marking.
///
/// # Safety
///
/// `set` must be live, and `block` a live block of its heap that the set does not have yet. The set must have room for
/// it, see rust_gc_evacuation_create().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_add_block(set: *mut EvacuationSet, block: *mut c_void) {
    // SAFETY: The caller passes a live set.
    let set = unsafe { &mut *set };
    let address = block.addr();
    debug_assert!(address & (BLOCK_SIZE - 1) == 0);
    // SAFETY: The caller passes a live block.
    let descriptor = unsafe { descriptor_for(set.layout.heap_region_base, address) };
    debug_assert!(descriptor.heap() == set.heap && descriptor.evacuation().is_none());
    let evacuation = set.evacuations.get(set.blocks.len());
    // SAFETY: The caller passes a live block, and no marker runs yet.
    unsafe {
        set_evacuation_of_block(
            set.layout.heap_region_base,
            address,
            core::ptr::from_ref(evacuation).cast_mut(),
        );
    }
    set.blocks.push(address);
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

/// How many marked cells of an evacuated block were pinned, by reason.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct PinnedCells {
    pub by_edge: usize,
    pub by_root: usize,
}

/// Finds the cells of an evacuated block that move once marking is done: the marked ones that nothing pinned. Writes
/// their addresses to `cells`, returns how many there are, and adds the marked cells that are pinned to `pinned`. Only
/// the mark bits are read, so the dead cells and the free slots of the block are never touched.
///
/// # Safety
///
/// `set` must be live, `block` one of its blocks whose cells start `block_cells_offset` bytes into it and whose marks
/// are the ones marking left, and `cells` must have room for MAX_CELLS_PER_BLOCK addresses.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_cells_to_move(
    set: *const EvacuationSet,
    block: *const c_void,
    cells: *mut *mut c_void,
    pinned: *mut PinnedCells,
) -> usize {
    abort_on_panic(|| {
        // SAFETY: The caller passes a live set, room for the cells, and a live count.
        let (set, cells, pinned) = unsafe {
            (
                &*set,
                ak::slice_from_raw_parts_mut(cells, MAX_CELLS_PER_BLOCK),
                &mut *pinned,
            )
        };
        let (descriptor, evacuation) = set.evacuation_of(block.addr()).expect("the block is evacuated");
        let cell_size = descriptor.cell_size as usize;
        let cells_offset = set.layout.block_cells_offset as usize;
        // NB: A cell's mark bit is its offset in the block divided by the cell size, see BlockDescriptor::cell_index().
        let first_bit = cells_offset / cell_size;
        let mut count = 0;
        for word_index in 0..CELL_BITMAP_WORDS {
            let mut bits = descriptor.mark_word(word_index).load(Ordering::Relaxed);
            while bits != 0 {
                let bit = word_index * 64 + bits.trailing_zeros() as usize;
                bits &= bits - 1;
                let cell = block.addr() + cells_offset + (bit - first_bit) * cell_size;
                debug_assert!(descriptor.cell_index(cell).0 == bit);
                match evacuation.pin_reason(CellIndex(bit)) {
                    PinReason::Unpinned => {
                        cells[count] = block.with_addr(cell).cast_mut();
                        count += 1;
                    }
                    PinReason::Edge => pinned.by_edge += 1,
                    PinReason::Root => pinned.by_root += 1,
                }
            }
        }
        count
    })
}

/// A cell to move, from its slot to a free slot of another block of its allocator.
#[repr(C)]
pub struct CellMove {
    pub from: *mut c_void,
    pub to: *mut c_void,
}

/// Notes where each cell moves to, before anything moves. The slots of both are `cell_size` bytes.
///
/// # Safety
///
/// `set` must be live, and the moves name live, unpinned cells of its blocks and free slots of the same allocators
/// outside its blocks, each once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_forward_cells(
    set: *mut EvacuationSet,
    moves: *const CellMove,
    count: usize,
    cell_size: usize,
) {
    abort_on_panic(|| {
        // SAFETY: The caller passes a live set, and count moves.
        let (set, moves) = unsafe { (&*set, ak::slice_from_raw_parts(moves, count)) };
        for cell_move in moves {
            let (from, to) = (cell_move.from.addr(), cell_move.to.addr());
            let (descriptor, evacuation) = set.evacuation_of(from).expect("the cell is in an evacuated block");
            debug_assert!(descriptor.cell_size as usize == cell_size);
            debug_assert!(set.evacuation_of(to).is_none(), "no cell moves into an evacuated block");
            let index = descriptor.cell_index(from);
            debug_assert!(evacuation.forwarding(index).is_none());
            evacuation.forwarding[index.0].store((to >> COMPRESSED_POINTER_SHIFT) as u32, Ordering::Relaxed);
        }
    });
}

/// Copies each cell to the slot it moves to, once its slots and the slots that point at it are rewritten. Both slots
/// are `cell_size` bytes.
///
/// # Safety
///
/// As for rust_gc_evacuation_forward_cells(), with the same moves.
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
        }
    });
}

/// Rewrites every slot that marking recorded and that points at a cell that moves, and returns how many it rewrote.
///
/// # Safety
///
/// `set` must be live, every cell that moves has to be forwarded and none copied yet, and the cells that held recorded
/// slots must still be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_fix_up_slots(set: *mut EvacuationSet) -> usize {
    abort_on_panic(|| {
        // SAFETY: The caller passes a live set.
        let set = unsafe { &*set };
        let mut rewritten = 0;
        for slots in set.all_slots() {
            for &slot in slots {
                // SAFETY: The slot was recorded in a live cell or in memory that one owns, which the caller makes sure is
                //         still live and where it was.
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
    unsafe { &*set }.all_slots().map(<[RecordedSlot]>::len).sum()
}

/// What a slot handed to rust_gc_evacuation_record_slot() holds.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecordedSlotKind {
    /// A NaN-boxed value, which may hold a cell.
    Value = 0,
    /// A raw pointer to the start of a cell, or null.
    CellPointer = 1,
}

/// Records a slot that marking did not visit, which the collection then rewrites like the slots marking recorded, for
/// holders that keep cells without keeping them alive or in place: the keys of weak maps, the targets of weak
/// references. They hand over the slots of the cells that survived once they forgot those that died.
///
/// # Safety
///
/// `set` must be live, and its marking done. The slot must hold what `kind` says, in memory that nothing frees, moves or
/// writes until the collection is over, and its cell, if it has one, must be marked.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_record_slot(
    set: *mut EvacuationSet,
    slot: *const c_void,
    kind: RecordedSlotKind,
) {
    // SAFETY: The caller passes a live set, and a slot that holds what its kind says.
    let set = unsafe { &mut *set };
    let (target, kind) = match kind {
        RecordedSlotKind::Value => {
            // SAFETY: See above.
            let value = unsafe { slot.cast::<u64>().read() };
            if value & set.layout.value_is_cell_pattern != set.layout.value_is_cell_pattern {
                return;
            }
            (
                set.layout.heap_region_base + (value & set.layout.value_offset_mask) as usize,
                SlotKind::Value,
            )
        }
        // SAFETY: See above.
        RecordedSlotKind::CellPointer => (unsafe { slot.cast::<usize>().read() }, SlotKind::InteriorPointer),
    };
    // NB: Only slots of cells that may move need rewriting.
    if set.evacuation_of(target).is_some() {
        set.slots_recorded_after_marking
            .push(RecordedSlot::new(slot.cast(), kind));
    }
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
        Some(address) => EvacuationSet::heap_region_pointer(address).cast(),
        None => core::ptr::null_mut(),
    }
}

/// Ends the evacuation state of a collection: its blocks are no longer evacuated.
///
/// # Safety
///
/// `set` must be live, and nothing may use it after. Its blocks must all still be its heap's, which they are until
/// the sweep that follows the collection, the only thing that lets go of a block.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_evacuation_destroy(set: *mut EvacuationSet) {
    // SAFETY: The caller passes a live set, which it gives up.
    let set = unsafe { Box::from_raw(set) };
    for (index, block) in set.blocks.iter().enumerate() {
        // SAFETY: The block is still the heap's, as the caller makes sure, so only this thread changes its descriptor.
        let descriptor = unsafe { descriptor_for(set.layout.heap_region_base, *block) };
        assert!(
            descriptor.heap() == set.heap
                && descriptor
                    .evacuation()
                    .is_some_and(|current| core::ptr::eq(current, set.evacuations.get(index))),
            "an evacuated block stays its heap's until its evacuation ends"
        );
        // SAFETY: The block is the heap's, and no marker runs.
        unsafe { set_evacuation_of_block(set.layout.heap_region_base, *block, core::ptr::null_mut()) };
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
