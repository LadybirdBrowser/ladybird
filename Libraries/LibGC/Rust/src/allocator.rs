/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The cell allocators of a heap, one for each type of cell, and the sweep of their blocks.
//!
//! Which cells of a block are free is in the allocated bitmap of the block's descriptor, never in the cells. An
//! allocator hands out the free cells of a block in runs: it takes the first run of free cells of a block at once, sets
//! their bits, and gives the code that allocates (C++, LibJS's Rust code and the JIT) the run as a cursor and an end to
//! bump through, see LocalFreeCells. Whatever is left of the run when a collection starts goes back to the block.
//!
//! Sweeping a block frees the cells that are allocated and not marked. Freeing a cell runs its destructor, which may
//! allocate cells and sweep other blocks, so nothing here keeps a borrow of the allocator's lists, or a reference into
//! a descriptor, across a call into C++.

use core::cell::RefCell;
use core::ffi::c_void;
use core::sync::atomic::{AtomicPtr, AtomicU64, Ordering};
use std::collections::HashMap;

use crate::bitmap::{self, CellBits};
use crate::block::{BLOCK_SIZE, BlockDescriptor, CELL_BITMAP_WORDS, MIN_CELL_SIZE, descriptor_table};
use crate::block_pool::{self, BlockPool};
use crate::identity_hash::{forget_identity_hash, has_moved_since_hashed};
use crate::rust_panic::abort_on_panic;

/// The free cells that code allocates from without calling into the allocator, which C++ `CellAllocator` keeps and
/// which `gc_get_layout` describes to LibJS's Rust code and the JIT. The allocator only ever hands out runs, so the free
/// list is always empty: null, which ends a free list as an address at the start of a block does.
#[repr(C)]
pub struct LocalFreeCells {
    pub free_list: *mut c_void,
    pub cursor: usize,
    pub end: usize,
    pub cell_size: usize,
}

/// Fills in the header of a block that is about to hold cells.
pub type InitializeBlock = unsafe extern "C" fn(context: *mut c_void, block: *mut c_void);

/// Frees a cell's slot: runs its destructor first if `destroy` is set, and marks it dead.
pub type FreeCell = unsafe extern "C" fn(context: *mut c_void, cell: *mut c_void, destroy: bool);

/// What an allocator needs to know about its heap and its type of cell.
#[repr(C)]
pub struct AllocatorConfig {
    pub heap_region_base: usize,
    pub heap: *mut c_void,
    pub type_info: *const c_void,
    pub pool: *const BlockPool,
    pub local: *mut LocalFreeCells,
    /// How far into a block its first cell is.
    pub cells_offset: u32,
    pub cell_size: u32,
    pub traced_on_owner_thread_only: bool,
    pub movable: bool,
    /// What the callbacks get, which is C++'s `CellAllocator`.
    pub context: *mut c_void,
    pub initialize_block: InitializeBlock,
    pub free_cell: FreeCell,
}

/// What sweeping blocks did, which the sweeps add to what the caller passes in.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SweepStats {
    pub swept_blocks: usize,
    pub freed_blocks: usize,
    pub collected_cells: usize,
    pub live_cells: usize,
}

pub struct Allocator {
    config: AllocatorConfig,
    cell_count: usize,
    first_cell_index: usize,
    cell_index_magic: u32,
    lists: RefCell<Lists>,
}

#[derive(Default)]
struct Lists {
    /// Every block of the allocator, at the list index in its descriptor.
    blocks: Vec<usize>,
    /// The blocks that have free cells and are swept, the last one first.
    usable: Vec<usize>,
    /// The blocks the incremental sweep has yet to sweep.
    pending_sweep: Vec<usize>,
    /// The block of the run the local free cells come from, or 0.
    local_block: usize,
    /// The cells of the blocks that a collection could not empty because of pins, see BlockDescriptor::kept_by_pins.
    kept_by_pins: HashMap<usize, CellBits>,
}

enum Next {
    Sweep(usize),
    Take(usize),
    TakeOneFromPending(usize),
    Create,
}

impl Allocator {
    fn new(config: AllocatorConfig) -> Allocator {
        let cell_size = config.cell_size as usize;
        assert!((MIN_CELL_SIZE..BLOCK_SIZE).contains(&cell_size));
        let cell_count = (BLOCK_SIZE - config.cells_offset as usize) / cell_size;
        let cell_index_magic = (1u64 << 32).div_ceil(cell_size as u64);
        Allocator {
            first_cell_index: config.cells_offset as usize / cell_size,
            cell_count,
            cell_index_magic: cell_index_magic as u32,
            config,
            lists: RefCell::new(Lists::default()),
        }
    }

    /// The descriptor of `block`. Other threads only ever read its heap, atomically, so the thread that owns the heap
    /// writes the rest through this pointer, never through a reference to the whole descriptor.
    fn descriptor(&self, block: usize) -> *mut BlockDescriptor {
        // SAFETY: Every block of the allocator is in the region, whose table has a descriptor for each block.
        unsafe {
            descriptor_table()
                .add((block - self.config.heap_region_base) / BLOCK_SIZE)
                .as_ptr()
        }
    }

    #[allow(clippy::mut_from_ref)]
    fn allocated(&self, block: usize) -> &mut CellBits {
        // SAFETY: Only the thread that owns the heap accesses the allocated bits, and none of the callers keeps the
        //         reference past a call that could access them again.
        unsafe { &mut (*self.descriptor(block)).allocated }
    }

    fn mark_word(&self, block: usize, word: usize) -> &AtomicU64 {
        // SAFETY: The block is one of this allocator's, whose descriptor only this thread writes to, but for the mark
        //         bits, which are atomic.
        unsafe { &*self.descriptor(block) }.mark_word(word)
    }

    fn cells(&self) -> (usize, usize) {
        (self.first_cell_index, self.first_cell_index + self.cell_count)
    }

    fn cell_address(&self, block: usize, index: usize) -> usize {
        block + self.config.cells_offset as usize + (index - self.first_cell_index) * self.config.cell_size as usize
    }

    fn cell_index(&self, cell: usize) -> usize {
        (((cell & (BLOCK_SIZE - 1)) as u64 * u64::from(self.cell_index_magic)) >> 32) as usize
    }

    fn has_free_cells(&self, block: usize) -> bool {
        bitmap::count(self.allocated(block)) < self.cell_count
    }

    fn create_block(&self) -> usize {
        // SAFETY: The pool outlives every allocator that takes blocks from it.
        let block = unsafe { &*self.config.pool }.allocate_block();
        let descriptor = self.descriptor(block);
        // SAFETY: The block is not anyone's, so nothing else accesses its descriptor but markers of other heaps, which
        //         only read its heap atomically, and see this one only after the descriptor is filled in.
        unsafe {
            (*descriptor).type_info = self.config.type_info;
            (*descriptor).cell_size = self.config.cell_size;
            (*descriptor).cell_index_magic = self.cell_index_magic;
            (*descriptor).traced_on_owner_thread_only = self.config.traced_on_owner_thread_only;
            (*descriptor).movable = self.config.movable;
            (*descriptor).kept_by_pins = false;
            (*descriptor).cell_count = self.cell_count as u16;
            (*descriptor).first_cell_index = self.first_cell_index as u16;
            (*descriptor).pending_sweep = false;
            (*descriptor).being_swept = false;
            (*descriptor).usable = false;
            AtomicPtr::from_ptr(&raw mut (*descriptor).evacuation).store(core::ptr::null_mut(), Ordering::Relaxed);
            (*descriptor).allocated = [0; CELL_BITMAP_WORDS];
        }
        for word in 0..CELL_BITMAP_WORDS {
            self.mark_word(block, word).store(0, Ordering::Relaxed);
        }
        // SAFETY: The block is ours now, and its header is for C++ to fill in.
        unsafe { (self.config.initialize_block)(self.config.context, block as *mut c_void) };
        // NB: Markers of other heaps read the heap of any block their cells point into, to leave it alone if it is not
        //     theirs.
        // SAFETY: As above.
        unsafe { AtomicPtr::from_ptr(&raw mut (*descriptor).heap).store(self.config.heap, Ordering::Release) };

        let mut lists = self.lists.borrow_mut();
        // SAFETY: As above.
        unsafe { (*descriptor).list_index = lists.blocks.len() as u32 };
        lists.blocks.push(block);
        block
    }

    fn release_block(&self, block: usize, defer_decommit: bool) {
        let descriptor = self.descriptor(block);
        {
            let mut lists = self.lists.borrow_mut();
            // SAFETY: Only this thread writes the list indices of the allocator's blocks.
            let index = unsafe { (*descriptor).list_index } as usize;
            debug_assert_eq!(lists.blocks[index], block);
            lists.blocks.swap_remove(index);
            if let Some(&moved) = lists.blocks.get(index) {
                // SAFETY: As above.
                unsafe { (*self.descriptor(moved)).list_index = index as u32 };
            }
            lists.kept_by_pins.remove(&block);
            debug_assert!(lists.local_block != block);
        }
        // SAFETY: As in create_block().
        unsafe {
            debug_assert!(!(*descriptor).usable && !(*descriptor).pending_sweep);
            AtomicPtr::from_ptr(&raw mut (*descriptor).heap).store(core::ptr::null_mut(), Ordering::Release);
        }
        // SAFETY: The block came from this pool, and nothing uses it any more.
        unsafe { block_pool::deallocate_block_of(self.config.pool, block, defer_decommit) };
    }

    #[allow(clippy::mut_from_ref)]
    fn local(&self) -> &mut LocalFreeCells {
        // SAFETY: The local free cells outlive the allocator, and only the thread that owns the heap accesses them.
        unsafe { &mut *self.config.local }
    }

    /// Hands out a cell once the local free cells ran out: a cell of a new run, or a single cell if `single_cell` is
    /// set, or if the free cells left are in blocks that the incremental sweep has yet to sweep and `can_sweep` is not.
    fn allocate(&self, can_sweep: bool, single_cell: bool, stats: &mut SweepStats) -> usize {
        {
            let local = self.local();
            debug_assert!(local.cursor == local.end);
            *local = LocalFreeCells {
                free_list: core::ptr::null_mut(),
                cursor: 0,
                end: 0,
                cell_size: self.config.cell_size as usize,
            };
            self.lists.borrow_mut().local_block = 0;
        }
        loop {
            // NB: A destructor that a sweep below ran may have allocated a cell of this type, which took a new run.
            //     Its cells count as allocated, so they have to be handed out rather than dropped.
            let local = self.local();
            if local.cursor < local.end {
                let cell = local.cursor;
                local.cursor += local.cell_size;
                return cell;
            }
            let next = {
                let mut lists = self.lists.borrow_mut();
                if let Some(&block) = lists.usable.last() {
                    Next::Take(block)
                } else if can_sweep && let Some(block) = lists.pending_sweep.pop() {
                    Next::Sweep(block)
                } else if let Some(&block) = lists.pending_sweep.last()
                    && self.has_free_cells(block)
                {
                    Next::TakeOneFromPending(block)
                } else {
                    Next::Create
                }
            };
            match next {
                Next::Sweep(block) => self.sweep_block(block, true, stats),
                Next::Create => {
                    let block = self.create_block();
                    self.make_usable(block);
                }
                // NB: The sweep keeps a cell allocated in a block it has yet to sweep if it is marked, which the
                //     caller sees to, see Heap::mark_if_allocated_during_incremental_sweep().
                Next::TakeOneFromPending(block) => return self.take_one(block),
                Next::Take(block) => {
                    if single_cell {
                        return self.take_one(block);
                    }
                    return self.take_run(block);
                }
            }
        }
    }

    fn make_usable(&self, block: usize) {
        let descriptor = self.descriptor(block);
        // SAFETY: Only this thread accesses the flags of the allocator's blocks.
        unsafe {
            if !(*descriptor).usable {
                (*descriptor).usable = true;
                self.lists.borrow_mut().usable.push(block);
            }
        }
    }

    /// Takes `block` off the list of usable blocks once it has no free cells left.
    fn forget_usable_if_full(&self, block: usize) {
        if self.has_free_cells(block) {
            return;
        }
        let descriptor = self.descriptor(block);
        // SAFETY: As in make_usable().
        unsafe {
            if (*descriptor).usable {
                (*descriptor).usable = false;
                let mut lists = self.lists.borrow_mut();
                let index = lists
                    .usable
                    .iter()
                    .rposition(|&usable| usable == block)
                    .expect("a usable block is listed");
                lists.usable.remove(index);
            }
        }
    }

    fn take_one(&self, block: usize) -> usize {
        let (begin, end) = self.cells();
        let (index, _) = bitmap::first_clear_run(self.allocated(block), begin, end).expect("the block has a free cell");
        bitmap::set(self.allocated(block), index);
        self.forget_usable_if_full(block);
        self.cell_address(block, index)
    }

    fn take_run(&self, block: usize) -> usize {
        let (begin, end) = self.cells();
        let (first, run_end) =
            bitmap::first_clear_run(self.allocated(block), begin, end).expect("the block has a free cell");
        bitmap::set_range(self.allocated(block), first, run_end);
        self.forget_usable_if_full(block);
        let cell = self.cell_address(block, first);
        let cell_size = self.config.cell_size as usize;
        let local = self.local();
        local.cursor = cell + cell_size;
        local.end = cell + (run_end - first) * cell_size;
        self.lists.borrow_mut().local_block = block;
        cell
    }

    /// Gives the cells of the local run that were not handed out back to their block.
    fn give_back_local_free_cells(&self) {
        let block = core::mem::take(&mut self.lists.borrow_mut().local_block);
        let local = self.local();
        let (cursor, end) = (local.cursor, local.end);
        local.cursor = 0;
        local.end = 0;
        if block == 0 || cursor == end {
            return;
        }
        let cell_size = self.config.cell_size as usize;
        let first = self.cell_index(cursor);
        bitmap::clear_range(self.allocated(block), first, first + (end - cursor) / cell_size);
        self.make_usable(block);
    }

    /// Makes every block pending sweep, after a collection marked them.
    fn start_sweep(&self) -> usize {
        // NB: The local free cells not handed out yet are no cells the sweep may free. The collection gave them back
        //     already, and nothing allocates during one, so this only makes sure.
        self.give_back_local_free_cells();
        let mut lists = self.lists.borrow_mut();
        for &block in &lists.usable {
            // SAFETY: As in make_usable().
            unsafe { (*self.descriptor(block)).usable = false };
        }
        lists.usable.clear();
        let blocks = lists.blocks.clone();
        for &block in &blocks {
            // SAFETY: As above.
            unsafe { (*self.descriptor(block)).pending_sweep = true };
        }
        // NB: Blocks are swept from the end of the list, so this sweeps them in the order of the list.
        lists.pending_sweep = blocks.into_iter().rev().collect();
        lists.pending_sweep.len()
    }

    fn sweep_next_block(&self, defer_decommit: bool, stats: &mut SweepStats) -> bool {
        let Some(block) = self.lists.borrow_mut().pending_sweep.pop() else {
            return false;
        };
        self.sweep_block(block, defer_decommit, stats);
        true
    }

    /// Sweeps `block` now if the incremental sweep has yet to, which a test does instead of the sweep timer.
    fn sweep_pending_block(&self, block: usize, stats: &mut SweepStats) {
        {
            let mut lists = self.lists.borrow_mut();
            let Some(index) = lists.pending_sweep.iter().position(|&pending| pending == block) else {
                return;
            };
            lists.pending_sweep.remove(index);
        }
        self.sweep_block(block, true, stats);
    }

    /// Frees the cells of `block` that are allocated and not marked, after taking it off the list of pending blocks.
    fn sweep_block(&self, block: usize, defer_decommit: bool, stats: &mut SweepStats) {
        let descriptor = self.descriptor(block);
        // SAFETY: As in make_usable().
        unsafe {
            (*descriptor).pending_sweep = false;
            (*descriptor).being_swept = true;
        }
        let kept = self.lists.borrow().kept_by_pins.get(&block).copied();
        let mut kept_cell_died = false;
        for word in 0..CELL_BITMAP_WORDS {
            loop {
                // NB: Read again after each destructor, which may have allocated or freed cells elsewhere.
                let dead = self.allocated(block)[word] & !self.mark_word(block, word).load(Ordering::Relaxed);
                if dead == 0 {
                    break;
                }
                let index = word * 64 + dead.trailing_zeros() as usize;
                kept_cell_died |= kept.is_some_and(|kept| bitmap::get(&kept, index));
                let cell = self.cell_address(block, index);
                // SAFETY: The cell is allocated.
                let moved_since_hashed = unsafe { has_moved_since_hashed(cell) };
                // NB: The cell stays allocated while its destructor runs, so nothing that it allocates takes its slot.
                // SAFETY: The cell is allocated and nothing marked it, so nothing reaches it any more.
                unsafe { (self.config.free_cell)(self.config.context, cell as *mut c_void, true) };
                if moved_since_hashed {
                    // SAFETY: The cell is one of the heap's, and moved since it was hashed.
                    unsafe { forget_identity_hash(self.config.heap, cell) };
                }
                bitmap::clear(self.allocated(block), index);
                stats.collected_cells += 1;
            }
        }
        // SAFETY: As above.
        unsafe { (*descriptor).being_swept = false };
        if kept_cell_died {
            self.forget_cells_kept_by_pins(block);
        }
        stats.swept_blocks += 1;
        let live = bitmap::count(self.allocated(block));
        self.clear_marks(block);
        if live == 0 {
            self.release_block(block, defer_decommit);
            stats.freed_blocks += 1;
            return;
        }
        stats.live_cells += live;
        if live < self.cell_count && self.lists.borrow().local_block != block {
            self.make_usable(block);
        }
    }

    fn clear_marks(&self, block: usize) {
        for word in 0..CELL_BITMAP_WORDS {
            self.mark_word(block, word).store(0, Ordering::Relaxed);
        }
    }

    fn forget_cells_kept_by_pins(&self, block: usize) {
        self.lists.borrow_mut().kept_by_pins.remove(&block);
        // SAFETY: As in make_usable().
        unsafe { (*self.descriptor(block)).kept_by_pins = false };
    }

    /// Remembers that a collection could not empty `block` because of pins, so that compaction leaves it alone until
    /// one of the cells left in it dies. The cells left are the marked ones that did not move: the others died, and the
    /// sweep that follows frees them.
    fn keep_cells_pinned(&self, block: usize) {
        let mut kept = *self.allocated(block);
        for (word, kept_word) in kept.iter_mut().enumerate() {
            *kept_word &= self.mark_word(block, word).load(Ordering::Relaxed);
        }
        self.lists.borrow_mut().kept_by_pins.insert(block, kept);
        // SAFETY: As in make_usable().
        unsafe { (*self.descriptor(block)).kept_by_pins = true };
    }

    fn is_evacuated(&self, block: usize) -> bool {
        // SAFETY: Only the thread that owns the heap writes the evacuation state of its blocks, and not while it
        //         allocates.
        !unsafe { AtomicPtr::from_ptr(&raw mut (*self.descriptor(block)).evacuation) }
            .load(Ordering::Relaxed)
            .is_null()
    }

    /// Takes a free slot for a cell that moves, in a block that the collection does not evacuate. The evacuated blocks
    /// it comes across stop being usable, until the sweep that follows the collection lists them again.
    fn allocate_destination(&self) -> usize {
        loop {
            let last = self.lists.borrow().usable.last().copied();
            match last {
                Some(block) if self.is_evacuated(block) => {
                    self.lists.borrow_mut().usable.pop();
                    // SAFETY: As in make_usable().
                    unsafe { (*self.descriptor(block)).usable = false };
                }
                Some(block) => return self.take_one(block),
                None => {
                    let block = self.create_block();
                    self.make_usable(block);
                }
            }
        }
    }

    /// Frees the slot of a cell that moved, without destroying it, since its copy lives on.
    fn release_moved_cell(&self, cell: usize) {
        let block = cell & !(BLOCK_SIZE - 1);
        let index = self.cell_index(cell);
        assert!(
            bitmap::get(self.allocated(block), index),
            "a cell that moved was allocated"
        );
        // SAFETY: The cell moved, so nothing uses its old slot.
        unsafe { (self.config.free_cell)(self.config.context, cell as *mut c_void, false) };
        bitmap::clear(self.allocated(block), index);
    }
}

impl Drop for Allocator {
    fn drop(&mut self) {
        let blocks = core::mem::take(&mut self.lists.get_mut().blocks);
        for block in blocks {
            let descriptor = self.descriptor(block);
            // SAFETY: The heap is going away, so nothing accesses its blocks any more but markers of other heaps, which
            //         only read the heap of a block.
            unsafe {
                (*descriptor).usable = false;
                (*descriptor).pending_sweep = false;
                AtomicPtr::from_ptr(&raw mut (*descriptor).heap).store(core::ptr::null_mut(), Ordering::Release);
                // NB: Another heap may not come along to use the blocks of this type for a long time.
                block_pool::deallocate_block_of(self.config.pool, block, false);
            }
        }
        // SAFETY: The pool outlives every allocator that takes blocks from it.
        unsafe { &*self.config.pool }.return_freed_blocks_to_system();
    }
}

/// # Safety
///
/// `config` must describe a live heap, pool and local free cells, which outlive the allocator.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_create(config: *const AllocatorConfig) -> *mut Allocator {
    // SAFETY: The caller passes a valid config.
    abort_on_panic(|| Box::into_raw(Box::new(Allocator::new(unsafe { config.read() }))))
}

/// Lets go of every block of the allocator, whose heap is going away.
///
/// # Safety
///
/// `allocator` must come from rust_gc_allocator_create(), and nothing may use it after this.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_destroy(allocator: *mut Allocator) {
    // SAFETY: The caller passes an allocator from rust_gc_allocator_create().
    abort_on_panic(|| drop(unsafe { Box::from_raw(allocator) }));
}

/// # Safety
///
/// `allocator` must be a live allocator from rust_gc_allocator_create(), used on the thread that owns its heap.
unsafe fn allocator_from<'a>(allocator: *const Allocator) -> &'a Allocator {
    // SAFETY: The caller passes a live allocator.
    unsafe { &*allocator }
}

/// Hands out a cell once the local free cells ran out, see Allocator::allocate(). What the blocks it swept for that
/// did is added to `stats`.
///
/// # Safety
///
/// As for allocator_from().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_allocate(
    allocator: *const Allocator,
    can_sweep: bool,
    single_cell: bool,
    stats: &mut SweepStats,
) -> *mut c_void {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { allocator_from(allocator) }.allocate(can_sweep, single_cell, stats) as *mut c_void)
}

/// # Safety
///
/// As for allocator_from().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_give_back_local_free_cells(allocator: *const Allocator) {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { allocator_from(allocator) }.give_back_local_free_cells());
}

/// Makes every block of the allocator pending sweep, and returns how many there are.
///
/// # Safety
///
/// As for allocator_from().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_start_sweep(allocator: *const Allocator) -> usize {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { allocator_from(allocator) }.start_sweep())
}

/// Sweeps the next block that is pending sweep, if there is one, and returns whether there was.
///
/// # Safety
///
/// As for allocator_from().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_sweep_next_block(
    allocator: *const Allocator,
    defer_decommit: bool,
    stats: &mut SweepStats,
) -> bool {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { allocator_from(allocator) }.sweep_next_block(defer_decommit, stats))
}

/// Sweeps `block` now if the incremental sweep has yet to.
///
/// # Safety
///
/// As for allocator_from(), and `block` must be one of the allocator's blocks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_sweep_pending_block(
    allocator: *const Allocator,
    block: *mut c_void,
    stats: &mut SweepStats,
) {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { allocator_from(allocator) }.sweep_pending_block(block as usize, stats));
}

/// # Safety
///
/// As for allocator_from().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_has_blocks_pending_sweep(allocator: *const Allocator) -> bool {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| {
        !unsafe { allocator_from(allocator) }
            .lists
            .borrow()
            .pending_sweep
            .is_empty()
    })
}

/// # Safety
///
/// As for allocator_from().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_block_count(allocator: *const Allocator) -> usize {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { allocator_from(allocator) }.lists.borrow().blocks.len())
}

/// The block at `index` of the allocator's list of blocks, which only changes as blocks come and go.
///
/// # Safety
///
/// As for allocator_from().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_block_at(allocator: *const Allocator, index: usize) -> *mut c_void {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { allocator_from(allocator) }.lists.borrow().blocks[index] as *mut c_void)
}

/// The block that the local free cells come from, or null.
///
/// # Safety
///
/// As for allocator_from().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_local_block(allocator: *const Allocator) -> *mut c_void {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { allocator_from(allocator) }.lists.borrow().local_block as *mut c_void)
}

/// # Safety
///
/// As for allocator_from().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_allocate_destination(allocator: *const Allocator) -> *mut c_void {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { allocator_from(allocator) }.allocate_destination() as *mut c_void)
}

/// # Safety
///
/// As for allocator_from(), and `cell` must be an allocated cell of the allocator that moved.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_release_moved_cell(allocator: *const Allocator, cell: *mut c_void) {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { allocator_from(allocator) }.release_moved_cell(cell as usize));
}

/// # Safety
///
/// As for allocator_from(), and `block` must be one of the allocator's blocks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_allocator_keep_cells_pinned(allocator: *const Allocator, block: *mut c_void) {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { allocator_from(allocator) }.keep_cells_pinned(block as usize));
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::block_pool::CHUNK_SIZE;
    use core::cell::UnsafeCell;
    use std::sync::OnceLock;

    /// A heap region of its own for the tests, set up once for all of them.
    fn region_base() -> usize {
        static BASE: OnceLock<usize> = OnceLock::new();
        *BASE.get_or_init(|| {
            const SIZE: usize = 256 * 1024 * 1024;
            let reservation = crate::os::reserve_zeroed(2 * SIZE)
                .expect("reserve a test region")
                .as_ptr() as usize;
            let base = reservation.next_multiple_of(SIZE);
            crate::block::rust_gc_block_descriptor_table(SIZE);
            block_pool::rust_gc_heap_region_init(
                core::ptr::with_exposed_provenance_mut(base),
                CHUNK_SIZE,
                SIZE - 2 * CHUNK_SIZE,
            );
            base
        })
    }

    struct TestHeap {
        allocator: *mut Allocator,
        /// The allocator writes to these through the pointer in its config, so they are in an UnsafeCell.
        local: Box<UnsafeCell<LocalFreeCells>>,
        freed: RefCell<Vec<(usize, bool)>>,
        /// How many cells the destructor of the next cells to die allocates.
        allocations_in_destructor: core::cell::Cell<usize>,
        pool: *const BlockPool,
    }

    unsafe extern "C" fn initialize_block(_: *mut c_void, _: *mut c_void) {}

    unsafe extern "C" fn free_cell(context: *mut c_void, cell: *mut c_void, destroy: bool) {
        // SAFETY: The tests pass their heap as the context.
        let heap = unsafe { &*(context as *const TestHeap) };
        heap.freed.borrow_mut().push((cell as usize, destroy));
        if destroy {
            for _ in 0..heap.allocations_in_destructor.replace(0) {
                heap.allocate();
            }
        }
    }

    impl TestHeap {
        fn new(cell_size: u32) -> Box<TestHeap> {
            let base = region_base();
            let mut heap = Box::new(TestHeap {
                allocator: core::ptr::null_mut(),
                local: Box::new(UnsafeCell::new(LocalFreeCells {
                    free_list: core::ptr::null_mut(),
                    cursor: 0,
                    end: 0,
                    cell_size: cell_size as usize,
                })),
                freed: RefCell::new(Vec::new()),
                allocations_in_destructor: core::cell::Cell::new(0),
                pool: block_pool::rust_gc_block_pool_create(),
            });
            let config = AllocatorConfig {
                heap_region_base: base,
                heap: &raw mut *heap as *mut c_void,
                type_info: core::ptr::null(),
                pool: heap.pool,
                local: heap.local.get(),
                cells_offset: 64,
                cell_size,
                traced_on_owner_thread_only: false,
                movable: true,
                context: &raw mut *heap as *mut c_void,
                initialize_block,
                free_cell,
            };
            // SAFETY: The config describes this heap, which outlives the allocator.
            heap.allocator = unsafe { rust_gc_allocator_create(&config) };
            heap
        }

        fn local(&self) -> &LocalFreeCells {
            // SAFETY: Only this thread uses the local free cells, and the callers drop this reference before the
            //         allocator runs again.
            unsafe { &*self.local.get() }
        }

        fn allocator(&self) -> &Allocator {
            // SAFETY: The allocator lives as long as the heap.
            unsafe { &*self.allocator }
        }

        /// Allocates as the inline fast path does, falling back to the allocator.
        fn allocate(&self) -> usize {
            {
                // SAFETY: Only this thread uses the local free cells, and the allocator does not run while this
                //         reference lives.
                let local = unsafe { &mut *self.local.get() };
                if local.cursor < local.end {
                    let cell = local.cursor;
                    local.cursor += local.cell_size;
                    return cell;
                }
            }
            self.allocator().allocate(true, false, &mut SweepStats::default())
        }

        fn mark(&self, cell: usize) {
            let block = cell & !(BLOCK_SIZE - 1);
            let index = self.allocator().cell_index(cell);
            self.allocator()
                .mark_word(block, index / 64)
                .fetch_or(bitmap::bit(index), Ordering::Relaxed);
        }

        fn collect(&self, marked: &[usize]) -> SweepStats {
            self.allocator().give_back_local_free_cells();
            for &cell in marked {
                self.mark(cell);
            }
            self.allocator().start_sweep();
            let mut stats = SweepStats::default();
            while self.allocator().sweep_next_block(true, &mut stats) {}
            stats
        }

        fn allocated_count(&self) -> usize {
            let lists = self.allocator().lists.borrow();
            lists
                .blocks
                .iter()
                .map(|&block| bitmap::count(self.allocator().allocated(block)))
                .sum()
        }
    }

    impl Drop for TestHeap {
        fn drop(&mut self) {
            // SAFETY: Nothing uses the allocator or the pool after this.
            unsafe {
                rust_gc_allocator_destroy(self.allocator);
                block_pool::rust_gc_block_pool_destroy(self.pool);
            }
        }
    }

    #[test]
    fn hands_out_runs_of_distinct_cells() {
        let heap = TestHeap::new(48);
        let cell_count = heap.allocator().cell_count;
        let cells: Vec<usize> = (0..cell_count * 3 + 5).map(|_| heap.allocate()).collect();
        let mut sorted = cells.clone();
        sorted.sort();
        sorted.dedup();
        assert_eq!(sorted.len(), cells.len());
        assert_eq!(heap.allocator().lists.borrow().blocks.len(), 4);
        for &cell in &cells {
            let offset = (cell & (BLOCK_SIZE - 1)) - 64;
            assert_eq!(offset % 48, 0);
            assert!(offset / 48 < cell_count);
        }
    }

    #[test]
    fn sweeps_unmarked_cells_and_reuses_their_slots() {
        let heap = TestHeap::new(64);
        let cells: Vec<usize> = (0..100).map(|_| heap.allocate()).collect();
        let survivors: Vec<usize> = cells.iter().copied().step_by(2).collect();
        let stats = heap.collect(&survivors);
        assert_eq!(stats.collected_cells, 50);
        assert_eq!(stats.live_cells, 50);
        assert_eq!(heap.allocated_count(), 50);
        let freed: Vec<usize> = heap.freed.borrow().iter().map(|&(cell, _)| cell).collect();
        assert!(freed.iter().all(|cell| !survivors.contains(cell)));

        // The holes come back one run of one cell at a time.
        let first = heap.allocate();
        assert_eq!(first, cells[1]);
        let second = heap.allocate();
        assert_eq!(second, cells[3]);
    }

    #[test]
    fn gives_back_what_is_left_of_the_local_run() {
        let heap = TestHeap::new(64);
        heap.allocate();
        heap.allocator().give_back_local_free_cells();
        assert_eq!(heap.allocated_count(), 1);
        assert_eq!(heap.local().cursor, heap.local().end);
    }

    #[test]
    fn lets_go_of_blocks_whose_cells_all_died() {
        let heap = TestHeap::new(128);
        let cell_count = heap.allocator().cell_count;
        let cells: Vec<usize> = (0..cell_count * 2).map(|_| heap.allocate()).collect();
        let stats = heap.collect(&cells[cell_count..]);
        assert_eq!(stats.freed_blocks, 1);
        assert_eq!(heap.allocator().lists.borrow().blocks.len(), 1);
        // SAFETY: The block was released, so its descriptor no longer names a heap.
        let descriptor = unsafe { &*heap.allocator().descriptor(cells[0] & !(BLOCK_SIZE - 1)) };
        assert!(descriptor.heap().is_null());
    }

    #[test]
    fn a_destructor_may_allocate_while_its_block_is_swept() {
        let heap = TestHeap::new(64);
        let cell_count = heap.allocator().cell_count;
        // Two full blocks, where the cells of the first die and the second keeps one cell.
        let cells: Vec<usize> = (0..cell_count * 2).map(|_| heap.allocate()).collect();
        heap.allocations_in_destructor.set(cell_count + 3);
        heap.collect(&cells[cell_count..cell_count + 1]);
        // NB: The sweep of the second block happened for an allocation of the destructor.
        let destroyed = heap.freed.borrow().iter().filter(|&&(_, destroy)| destroy).count();
        assert_eq!(destroyed, cell_count * 2 - 1);
        // The destructor allocated cell_count + 3 cells, which a new block and the slots the sweep freed already took.
        let allocated_by_destructor = heap.allocated_count() - 1;
        // NB: The cells of the local run that were not handed out yet count as allocated.
        let local_left = (heap.local().end - heap.local().cursor) / 64;
        assert_eq!(allocated_by_destructor - local_left, cell_count + 3);
        // No slot that a destructor ran for was handed out before its destructor returned.
        let freed: Vec<usize> = heap.freed.borrow().iter().map(|&(cell, _)| cell).collect();
        let mut distinct = freed.clone();
        distinct.sort();
        distinct.dedup();
        assert_eq!(distinct.len(), freed.len());
    }

    #[test]
    fn a_run_that_a_destructor_takes_during_an_allocation_is_handed_out() {
        let heap = TestHeap::new(64);
        let cell_count = heap.allocator().cell_count;
        let cells: Vec<usize> = (0..cell_count).map(|_| heap.allocate()).collect();
        heap.allocator().give_back_local_free_cells();
        heap.mark(cells[0]);
        heap.allocator().start_sweep();
        // The next allocation sweeps the block, whose first dead cell's destructor allocates one cell.
        heap.allocations_in_destructor.set(1);
        let cell = heap.allocate();
        // NB: Every allocated cell is the destructor's, this one, or the survivor, or is still in the local run.
        let local_left = (heap.local().end - heap.local().cursor) / 64;
        assert_eq!(heap.allocated_count(), 3 + local_left);
        assert_ne!(cell, cells[0]);
    }

    #[test]
    fn evacuation_moves_cells_out_of_blocks_that_the_sweep_then_lets_go_of() {
        let heap = TestHeap::new(64);
        let cell_count = heap.allocator().cell_count;
        let cells: Vec<usize> = (0..cell_count * 2).map(|_| heap.allocate()).collect();
        // Half of each block survives a first collection, which leaves room for the cells that move.
        let survivors: Vec<usize> = cells.iter().copied().step_by(2).collect();
        heap.collect(&survivors);
        heap.freed.borrow_mut().clear();

        // The next collection marks half of the survivors, and evacuates the first block before it sweeps.
        heap.allocator().give_back_local_free_cells();
        let marked: Vec<usize> = survivors.iter().copied().step_by(2).collect();
        for &cell in &marked {
            heap.mark(cell);
        }
        let evacuated = cells[0] & !(BLOCK_SIZE - 1);
        let mut evacuation = 0u8;
        // SAFETY: Nothing else uses the descriptor while this test runs.
        unsafe { (*heap.allocator().descriptor(evacuated)).evacuation = (&raw mut evacuation).cast() };
        let mut destinations = Vec::new();
        for &cell in marked.iter().filter(|&&cell| cell & !(BLOCK_SIZE - 1) == evacuated) {
            let destination = heap.allocator().allocate_destination();
            assert_ne!(destination & !(BLOCK_SIZE - 1), evacuated);
            // NB: The dead cells are not swept yet, so their slots are not free to take.
            assert!(!survivors.contains(&destination));
            heap.mark(destination);
            destinations.push(destination);
            heap.allocator().release_moved_cell(cell);
        }
        // SAFETY: As above.
        unsafe { (*heap.allocator().descriptor(evacuated)).evacuation = core::ptr::null_mut() };

        // The sweep destroys the cells that died, in the evacuated block too, and lets go of the block.
        heap.allocator().start_sweep();
        let mut stats = SweepStats::default();
        while heap.allocator().sweep_next_block(true, &mut stats) {}
        assert_eq!(stats.freed_blocks, 1);
        assert_eq!(heap.allocator().lists.borrow().blocks.len(), 1);
        let freed = heap.freed.borrow();
        let destroyed: Vec<usize> = freed
            .iter()
            .filter(|&&(_, destroy)| destroy)
            .map(|&(cell, _)| cell)
            .collect();
        assert_eq!(destroyed.len(), survivors.len() - marked.len());
        assert!(
            destroyed
                .iter()
                .all(|cell| !marked.contains(cell) && !destinations.contains(cell))
        );
        assert_eq!(heap.allocated_count(), marked.len());
    }

    #[test]
    fn blocks_kept_by_pins_are_forgotten_once_a_kept_cell_dies() {
        let heap = TestHeap::new(64);
        let cells: Vec<usize> = (0..10).map(|_| heap.allocate()).collect();
        let block = cells[0] & !(BLOCK_SIZE - 1);
        // NB: A collection keeps the cells that pins left in a block it evacuated once they moved, before it sweeps.
        for &cell in &cells {
            heap.mark(cell);
        }
        heap.allocator().keep_cells_pinned(block);
        heap.collect(&cells);
        // SAFETY: Nothing else uses the descriptor while this test runs.
        let kept_by_pins = || unsafe { (*heap.allocator().descriptor(block)).kept_by_pins };
        assert!(kept_by_pins());

        // A cell that came after the block was kept dies, which leaves it kept.
        let newcomer = heap.allocate();
        heap.collect(&cells);
        assert!(heap.freed.borrow().iter().any(|&(cell, _)| cell == newcomer));
        assert!(kept_by_pins());

        // One of the cells it kept dies.
        heap.collect(&cells[1..]);
        assert!(!kept_by_pins());
    }
}
