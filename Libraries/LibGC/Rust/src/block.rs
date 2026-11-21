/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The descriptors of the heap's blocks.
//!
//! A block holds cells of one type and one size. What the collector knows about a block is in its descriptor, which
//! lives outside the heap region: the heap and the type of the block, and which of its cells are marked. So nothing that
//! writes to a cell, or past it, can change what the collector does with the block. The descriptor of a block is at the
//! block's index in the region in a table of descriptors, which is reserved once for the whole region and only backed
//! by memory where blocks are.

use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicPtr, AtomicU64, Ordering};
use std::sync::OnceLock;

use crate::os;

pub const BLOCK_SIZE: usize = 16 * 1024;

/// The smallest cell, which has room for the link of a free cell after the header of a cell.
pub const MIN_CELL_SIZE: usize = 24;

/// A cell's mark bit is its offset in its block divided by its size, which the smallest cells take the most bits for.
pub const MARK_BITMAP_WORDS: usize = 11;

const _: () = assert!(MARK_BITMAP_WORDS == (BLOCK_SIZE / MIN_CELL_SIZE + 1).div_ceil(64));

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
    /// Rounds the descriptor up to 128 bytes, two whole cache lines in the table.
    pub reserved: [u8; 7],
    pub reserved_word: u64,
    /// One bit for each cell, which marking sets atomically, see mark_word().
    pub marked: [UnsafeCell<u64>; MARK_BITMAP_WORDS],
}

const _: () = assert!(size_of::<BlockDescriptor>() == 128);

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

    #[inline(always)]
    pub fn mark_bit(&self, address: usize) -> (&AtomicU64, u64) {
        let index = (((address & (BLOCK_SIZE - 1)) as u64 * u64::from(self.cell_index_magic)) >> 32) as usize;
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

struct Table {
    descriptors: NonNull<BlockDescriptor>,
    count: usize,
}

// SAFETY: The table is only ever read through, and its descriptors are accessed atomically where threads share them.
unsafe impl Send for Table {}
// SAFETY: As above.
unsafe impl Sync for Table {}

static TABLE: OnceLock<Table> = OnceLock::new();

/// The table of block descriptors, which this reserves for a region of `region_size` bytes on first use.
pub fn descriptor_table() -> NonNull<BlockDescriptor> {
    TABLE.get().expect("the table of block descriptors exists").descriptors
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

/// Makes sure that the descriptor at `index` is backed by memory, before a block is created there.
#[unsafe(no_mangle)]
pub extern "C" fn rust_gc_block_descriptor_commit(index: usize) {
    let table = TABLE.get().expect("the table of block descriptors exists");
    assert!(index < table.count, "a block is within the heap region");
    // SAFETY: The descriptor is within the reserved table.
    unsafe { os::commit_reserved_zeroed(table.descriptors.add(index).cast(), size_of::<BlockDescriptor>()) };
}
