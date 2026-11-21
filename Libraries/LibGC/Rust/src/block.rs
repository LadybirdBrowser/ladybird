/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The descriptors of the heap's blocks.
//!
//! A block holds cells of one type and one size. What the collector knows about a block is in its descriptor, which
//! lives outside the heap region: the heap and the type of the block, and which of its cells are allocated and which
//! are marked. So nothing that writes to a cell, or past it, can change what the collector does with the block. The
//! descriptor of a block is at the block's index in the region in a table of descriptors, which is reserved once for the
//! whole region and only backed by memory where blocks are.

use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicPtr, AtomicU64, Ordering};
use std::sync::OnceLock;

use crate::evacuation::Evacuation;
use crate::os;

pub const BLOCK_SIZE: usize = 16 * 1024;

/// The smallest cell, which has room for the link of a free cell after the header of a cell.
pub const MIN_CELL_SIZE: usize = 24;

/// A cell's bit in the bitmaps of its block is its offset in the block divided by its size, which the smallest cells
/// take the most bits for.
pub const CELL_BITMAP_WORDS: usize = 11;

const _: () = assert!(CELL_BITMAP_WORDS == (BLOCK_SIZE / MIN_CELL_SIZE + 1).div_ceil(64));

/// What the collector knows about a block. LibGC's C++ side fills it in when it creates the block, and empties it when
/// the block goes away.
#[repr(C)]
pub struct BlockDescriptor {
    /// The heap the block belongs to, or null while there is no block.
    pub heap: *mut c_void,
    /// The CellTypeInfo of the block's cells.
    pub type_info: *const c_void,
    pub cell_size: u32,
    /// ceil(2^32 / cell_size): the offset of a cell in its block times this, shifted right by 32, is its mark bit.
    pub cell_index_magic: u32,
    /// Whether the block's cells may only be traced on the thread that owns their heap, as C++ cells must.
    pub traced_on_owner_thread_only: bool,
    /// Whether a collection may move the block's cells to other blocks of their allocator.
    pub movable: bool,
    /// Whether a collection evacuated the block and could not empty it because something pinned some of its cells.
    /// Compaction leaves the block alone until one of the cells it kept dies.
    pub kept_by_pins: bool,
    pub reserved: u8,
    /// How many cells fit in the block, after its header.
    pub cell_count: u16,
    /// The index of the first cell of the block, see cell_index(). Its cells have the indices from this one on.
    pub first_cell_index: u16,
    /// What a collection that evacuates the block knows about its cells, and null in every other collection. Marking
    /// reads it for every cell it visits, to pin the cells of evacuated blocks that something visits by value.
    pub evacuation: *mut Evacuation,
    /// One bit for each cell, which marking sets atomically, see mark_word().
    pub marked: [UnsafeCell<u64>; CELL_BITMAP_WORDS],
    /// One bit for each cell that is allocated: set when a cell is handed out, and when an allocator takes the free
    /// cells of the block to hand out itself, until it gives back the ones it did not hand out. Only the thread that
    /// owns the heap reads and writes them.
    pub allocated: [u64; CELL_BITMAP_WORDS],
    /// Where the block is in its allocator's list of blocks.
    pub list_index: u32,
    /// Whether the incremental sweep has yet to sweep the block.
    pub pending_sweep: bool,
    /// Whether a sweep is sweeping the block right now, which a destructor it calls may be in the middle of.
    pub being_swept: bool,
    /// Whether the block is on its allocator's list of blocks with free cells.
    pub usable: bool,
    /// Rounds the descriptor up to 256 bytes, four whole cache lines in the table.
    pub reserved2: [u8; 33],
}

const _: () = assert!(size_of::<BlockDescriptor>() == 256);

impl BlockDescriptor {
    pub fn heap(&self) -> *mut c_void {
        // SAFETY: A reference is a valid pointer.
        unsafe { heap_of_descriptor(self) }
    }

    /// A word of the mark bits, which other threads may set while marking runs.
    #[inline(always)]
    pub fn mark_word(&self, word: usize) -> &AtomicU64 {
        // SAFETY: The word is in an UnsafeCell, so writing to it through a shared reference is fine, and it is only
        //         ever accessed atomically while other threads may access it.
        unsafe { AtomicU64::from_ptr(self.marked[word].get()) }
    }

    /// The evacuation state of the block, if a collection evacuates it.
    #[inline(always)]
    pub fn evacuation(&self) -> Option<&Evacuation> {
        // SAFETY: The field is only accessed atomically while other threads may read it, only written while no marker
        //         runs, and the evacuation outlives the collection's use of it.
        unsafe {
            AtomicPtr::from_ptr((&raw const self.evacuation).cast_mut())
                .load(Ordering::Relaxed)
                .as_ref()
        }
    }

    /// The index of the cell at `address` in the block, which is also the index of its mark bit. Cells are a cell size
    /// apart, so the offsets of their starts divided by the cell size tell them apart.
    #[inline(always)]
    pub fn cell_index(&self, address: usize) -> CellIndex {
        CellIndex((((address & (BLOCK_SIZE - 1)) as u64 * u64::from(self.cell_index_magic)) >> 32) as usize)
    }

    /// The start of the cell that `address` points into, for a block whose cells start `cells_offset` bytes into it.
    pub fn cell_containing(&self, address: usize, cells_offset: usize) -> usize {
        let block = address & !(BLOCK_SIZE - 1);
        let offset_in_cells = (address & (BLOCK_SIZE - 1)) - cells_offset;
        // NB: As for the mark bit index, multiplying by the magic and shifting right by 32 divides offsets below 2^14 by
        //     the cell size exactly.
        let index = ((offset_in_cells as u64 * u64::from(self.cell_index_magic)) >> 32) as usize;
        block + cells_offset + index * self.cell_size as usize
    }

    #[inline(always)]
    pub fn mark_bit(&self, address: usize) -> (&AtomicU64, u64) {
        let index = self.cell_index(address).0;
        (self.mark_word(index / 64), 1 << (index % 64))
    }

    pub fn is_marked(&self, address: usize) -> bool {
        let (word, bit) = self.mark_bit(address);
        word.load(Ordering::Relaxed) & bit != 0
    }

    /// Sets the mark bit of the cell at `address`, and returns whether it was clear.
    #[inline(always)]
    pub fn mark(&self, address: usize) -> bool {
        let (word, bit) = self.mark_bit(address);
        // NB: A bit that is set already costs no atomic read-modify-write, which would take the word's cache line from
        //     the other threads that set bits in it.
        if word.load(Ordering::Relaxed) & bit != 0 {
            return false;
        }
        word.fetch_or(bit, Ordering::Relaxed) & bit == 0
    }

    /// Like mark(), for a thread that no other thread sets mark bits concurrently with.
    #[inline(always)]
    pub fn mark_exclusive(&self, address: usize) -> bool {
        let (word, bit) = self.mark_bit(address);
        let value = word.load(Ordering::Relaxed);
        if value & bit != 0 {
            return false;
        }
        word.store(value | bit, Ordering::Relaxed);
        true
    }
}

/// The index of a cell in its block, which marks and other per-cell bits of the block are indexed by.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CellIndex(pub usize);

/// The most cells a block has, and so the most bits that per-cell bitmaps of a block need.
pub const MAX_CELLS_PER_BLOCK: usize = CELL_BITMAP_WORDS * 64;

struct Table {
    descriptors: NonNull<BlockDescriptor>,
    count: usize,
}

// SAFETY: The table is only ever read through, and its descriptors are accessed atomically where threads share them.
unsafe impl Send for Table {}
// SAFETY: As above.
unsafe impl Sync for Table {}

static TABLE: OnceLock<Table> = OnceLock::new();

/// The table of block descriptors, see rust_gc_block_descriptor_table().
pub fn descriptor_table() -> NonNull<BlockDescriptor> {
    TABLE.get().expect("the table of block descriptors exists").descriptors
}

/// The descriptor of the block that `address` is in, for an address in the heap region at `heap_region_base`.
///
/// # Safety
///
/// `address` must be in a block of the region, whose descriptor is backed by memory.
#[inline(always)]
pub unsafe fn descriptor_for<'a>(heap_region_base: usize, address: usize) -> &'a BlockDescriptor {
    // SAFETY: The caller passes an address in a block, whose descriptor is in the table.
    unsafe {
        descriptor_table()
            .add((address - heap_region_base) / BLOCK_SIZE)
            .as_ref()
    }
}

/// Sets the evacuation state of the block that `address` is in, which only a collection that is not marking may do.
///
/// NB: This writes through a pointer from the table rather than through a reference to the descriptor, which would let
///     the compiler assume that the descriptor does not change.
///
/// # Safety
///
/// As for descriptor_for().
pub unsafe fn set_evacuation_of_block(heap_region_base: usize, address: usize, evacuation: *mut Evacuation) {
    // SAFETY: The caller passes an address in a block, whose descriptor is in the table, and the field is only accessed
    //         atomically while other threads may read it.
    unsafe {
        let descriptor = descriptor_table()
            .add((address - heap_region_base) / BLOCK_SIZE)
            .as_ptr();
        AtomicPtr::from_ptr(&raw mut (*descriptor).evacuation).store(evacuation, Ordering::Relaxed);
    }
}

/// The heap of a block, read without borrowing its descriptor. The thread of another heap may be filling in the other
/// fields of the descriptor of a block that it takes, which a reference to the descriptor would assume do not change.
///
/// # Safety
///
/// `descriptor` must point to a descriptor in the table that is backed by memory.
#[inline(always)]
pub unsafe fn heap_of_descriptor(descriptor: *const BlockDescriptor) -> *mut c_void {
    // SAFETY: The caller passes a valid descriptor, and the field is only ever accessed atomically while other threads
    //         may read it.
    unsafe { AtomicPtr::from_ptr((&raw const (*descriptor).heap).cast_mut()) }.load(Ordering::Relaxed)
}

/// The descriptor of the block that `address` is in, if that block belongs to `heap`, which only the calling thread
/// changes, so nothing changes the descriptor while the reference is in use.
///
/// # Safety
///
/// As for descriptor_for().
#[inline(always)]
pub unsafe fn descriptor_of_heap<'a>(
    heap_region_base: usize,
    address: usize,
    heap: *const c_void,
) -> Option<&'a BlockDescriptor> {
    // SAFETY: The caller passes an address in a block, whose descriptor is in the table.
    let descriptor = unsafe { descriptor_table().add((address - heap_region_base) / BLOCK_SIZE) };
    // SAFETY: As above.
    if unsafe { heap_of_descriptor(descriptor.as_ptr()) }.cast_const() != heap {
        return None;
    }
    // SAFETY: As above, and only the calling thread changes the descriptor of a block of the heap.
    Some(unsafe { descriptor.as_ref() })
}

/// Reserves the table of block descriptors for a heap region of `region_size` bytes, the first time it is called, and
/// returns it. The descriptor of the block at offset N of the region is at index N / BLOCK_SIZE.
#[unsafe(no_mangle)]
pub extern "C" fn rust_gc_block_descriptor_table(region_size: usize) -> *mut BlockDescriptor {
    TABLE
        .get_or_init(|| {
            let count = region_size / BLOCK_SIZE;
            let descriptors = os::reserve_zeroed(count * size_of::<BlockDescriptor>())
                .expect("reserve the table of block descriptors");
            Table {
                descriptors: descriptors.cast(),
                count,
            }
        })
        .descriptors
        .as_ptr()
}

/// Makes sure that the `count` descriptors from `first_index` on are backed by memory, before blocks are created there.
pub fn commit_descriptors(first_index: usize, count: usize) {
    let table = TABLE.get().expect("the table of block descriptors exists");
    assert!(
        first_index + count <= table.count,
        "the blocks are within the heap region"
    );
    // SAFETY: The descriptors are within the reserved table.
    unsafe {
        os::commit_reserved_zeroed(
            table.descriptors.add(first_index).cast(),
            count * size_of::<BlockDescriptor>(),
        );
    }
}
