/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where the blocks of the heap come from, and where they go when they are empty.
//!
//! Every block is a 16 KiB slot of one process-wide region, which C++ reserves and which this commits 2 MiB at a time. A
//! slot that was never used belongs to nobody, so the pools of all types take their slots from the same chunk, and a
//! pool for a type that only needs one block costs one block. A slot that was used stays with the pool that took it, so
//! an address that held a cell of one type never holds a cell of another type.
//!
//! Giving the pages of an empty block back to the system is left to a single background thread, the decommit worker,
//! so it never costs a collection time, and a block that is used again before the worker sees it skips the system calls
//! altogether.

use core::ptr::NonNull;
use core::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard, OnceLock};
use std::time::{Duration, Instant};

use crate::block::BLOCK_SIZE;
use crate::rust_panic::abort_on_panic;
use crate::{os, sanitizer};

pub const CHUNK_SIZE: usize = 2 * 1024 * 1024;
pub const BLOCKS_PER_CHUNK: usize = CHUNK_SIZE / BLOCK_SIZE;

const _: () = assert!(CHUNK_SIZE.is_multiple_of(BLOCK_SIZE));
const _: () = assert!(BLOCKS_PER_CHUNK == 128);

/// How long a block whose pages were discarded can sit unused before its pages go back for real. This is longer than
/// most heaps take between collections, so that blocks they reuse keep skipping the page faults. And as the collections
/// of every heap drive the decommit worker, it keeps a busy heap from making the blocks of a slower one look cold.
static COLD_BLOCK_AGE_IN_MILLISECONDS: AtomicU64 = AtomicU64::new(5000);

fn cold_block_age() -> Duration {
    Duration::from_millis(COLD_BLOCK_AGE_IN_MILLISECONDS.load(Ordering::Relaxed))
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    // NB: Nothing panics while holding these locks without aborting the process, so a poisoned lock never happens.
    mutex.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn block_pointer(address: usize) -> NonNull<u8> {
    // NB: Blocks are in the heap region, whose provenance rust_gc_heap_region_init() exposed.
    NonNull::new(core::ptr::with_exposed_provenance_mut(address)).expect("a block is never at address 0")
}

/// The part of the heap region that blocks come from: the chunks between the guards at either end.
struct Region {
    base: usize,
    last_chunk_offset: usize,
    chunks: Mutex<Chunks>,
}

struct Chunks {
    next_chunk_offset: usize,
    /// A committed chunk whose pages are faulted in already, which is the next one that blocks come from.
    prepared_chunk: Option<usize>,
    /// The blocks of the current chunk that were never used.
    next_unused_block: usize,
    unused_blocks_end: usize,
}

static REGION: OnceLock<Region> = OnceLock::new();

fn region() -> &'static Region {
    REGION.get().expect("the heap region is reserved")
}

impl Region {
    fn commit_next_chunk(&self, chunks: &mut Chunks) -> usize {
        assert!(
            chunks.next_chunk_offset <= self.last_chunk_offset,
            "the heap region has room for another chunk"
        );
        let chunk = self.base + chunks.next_chunk_offset;
        // SAFETY: The chunk is within the reserved region, past every chunk committed so far.
        unsafe { os::commit(block_pointer(chunk), CHUNK_SIZE) };
        // NB: Every block of a committed chunk has a descriptor backed by memory, so that any address in the committed
        //     part of the region can be looked up.
        crate::block::commit_descriptors(chunks.next_chunk_offset / BLOCK_SIZE, BLOCKS_PER_CHUNK);
        chunks.next_chunk_offset += CHUNK_SIZE;
        chunk
    }

    fn take_unused_block(&self) -> usize {
        let block;
        let mut took_chunk = false;
        {
            let mut chunks = lock(&self.chunks);
            if chunks.next_unused_block == chunks.unused_blocks_end {
                let chunk = match chunks.prepared_chunk.take() {
                    Some(chunk) => chunk,
                    None => self.commit_next_chunk(&mut chunks),
                };
                // SAFETY: The chunk was just committed, and nothing uses it yet.
                unsafe { os::mark_reusable(block_pointer(chunk), CHUNK_SIZE) };
                sanitizer::poison(chunk, CHUNK_SIZE);
                chunks.next_unused_block = chunk;
                chunks.unused_blocks_end = chunk + CHUNK_SIZE;
                took_chunk = true;
            }
            block = chunks.next_unused_block;
            chunks.next_unused_block += BLOCK_SIZE;
        }
        // NB: Preparing a chunk only pays off where its pages can be faulted in ahead of time. And only a worker that
        //     is already running prepares chunks: starting it here could start a second thread before the process is
        //     sandboxed, which some sandboxes do not allow.
        if took_chunk
            && cfg!(any(target_os = "linux", target_os = "android"))
            && let Some(worker) = DecommitWorker::the_if_started()
        {
            worker.request_prepared_chunk();
        }
        block
    }

    /// Commits the next chunk and faults its pages in, so that the first touches of its blocks take no page faults on
    /// the thread that allocates them. Runs on the decommit worker.
    fn prepare_chunk(&self) {
        let chunk = {
            let mut chunks = lock(&self.chunks);
            // NB: Running out of the region is left to take_unused_block(), on the thread that needs another chunk.
            if chunks.prepared_chunk.is_some() || chunks.next_chunk_offset > self.last_chunk_offset {
                return;
            }
            let chunk = self.commit_next_chunk(&mut chunks);
            // NB: The chunk is there to take before its pages are in, so that a thread that needs another chunk
            //     meanwhile takes this one rather than commit the next, which might not exist.
            chunks.prepared_chunk = Some(chunk);
            chunk
        };
        // SAFETY: The chunk was just committed. Faulting its pages in leaves their contents alone, so a thread may use
        //         its blocks already.
        unsafe { os::populate(block_pointer(chunk), CHUNK_SIZE) };
    }

    fn committed_end(&self) -> usize {
        self.base + lock(&self.chunks).next_chunk_offset
    }
}

/// The blocks of one type of cell in all heaps.
pub struct BlockPool {
    state: Mutex<PoolState>,
}

#[derive(Default)]
struct PoolState {
    /// Blocks whose pages were discarded, which the system may have taken back.
    blocks: Vec<usize>,
    /// Blocks whose pages went back for real, as their heap went away or they stayed unused for long. They are the last
    /// ones to reuse, as their pages have to be faulted in again.
    returned_blocks: Vec<usize>,
    /// Blocks that were freed but not discarded yet. They are the first ones to reuse, with no system call at all.
    freshly_freed: Vec<usize>,
    /// Whether the pool is on the decommit worker's list.
    in_decommit_registry: bool,
}

impl BlockPool {
    pub(crate) fn allocate_block(&self) -> usize {
        let (block, needs_reuse) = {
            let mut state = lock(&self.state);
            if let Some(block) = state.freshly_freed.pop() {
                (Some(block), false)
            } else if let Some(block) = state.blocks.pop() {
                (Some(block), true)
            } else {
                (state.returned_blocks.pop(), true)
            }
        };
        let block = block.unwrap_or_else(|| region().take_unused_block());
        sanitizer::unpoison(block, BLOCK_SIZE);
        sanitizer::register_root_region(block, BLOCK_SIZE);
        if needs_reuse {
            // SAFETY: The block is committed, and was discarded or is part of a chunk that was marked reusable.
            unsafe { os::reuse(block_pointer(block), BLOCK_SIZE) };
        }
        block
    }

    fn deallocate_block(self: &Arc<Self>, block: usize, defer_decommit: bool) {
        sanitizer::poison(block, BLOCK_SIZE);
        sanitizer::unregister_root_region(block, BLOCK_SIZE);
        let needs_registering = {
            let mut state = lock(&self.state);
            state.freshly_freed.push(block);
            defer_decommit && !core::mem::replace(&mut state.in_decommit_registry, true)
        };
        if needs_registering {
            DecommitWorker::the().register_pending(self.clone());
        }
    }

    pub(crate) fn return_freed_blocks_to_system(&self) {
        let blocks = {
            let mut state = lock(&self.state);
            let mut blocks = core::mem::take(&mut state.freshly_freed);
            // NB: Discarded pages stay in the process until the system runs short of memory.
            if cfg!(any(target_os = "linux", target_os = "android")) {
                blocks.append(&mut state.blocks);
            }
            blocks
        };
        for &block in &blocks {
            // SAFETY: The blocks are free, so nothing needs their contents.
            unsafe { os::return_to_system(block_pointer(block), BLOCK_SIZE) };
        }
        lock(&self.state).returned_blocks.extend(blocks);
    }
}

/// The thread that gives the pages of freed blocks back to the system, and prepares the next chunk.
struct DecommitWorker {
    state: Mutex<WorkerState>,
    condition: Condvar,
}

struct WorkerState {
    pending: Vec<Arc<BlockPool>>,
    kicked: bool,
    chunk_requested: bool,
    last_return_of_cold_blocks: Instant,
}

static DECOMMIT_WORKER: OnceLock<&'static DecommitWorker> = OnceLock::new();

impl DecommitWorker {
    fn the() -> &'static DecommitWorker {
        DECOMMIT_WORKER.get_or_init(|| {
            let worker: &'static DecommitWorker = Box::leak(Box::new(DecommitWorker {
                state: Mutex::new(WorkerState {
                    pending: Vec::new(),
                    kicked: false,
                    chunk_requested: false,
                    last_return_of_cold_blocks: Instant::now(),
                }),
                condition: Condvar::new(),
            }));
            std::thread::Builder::new()
                .name("DecommitWorker".into())
                .spawn(move || abort_on_panic(|| worker.run()))
                .expect("start the decommit worker");
            worker
        })
    }

    fn the_if_started() -> Option<&'static DecommitWorker> {
        DECOMMIT_WORKER.get().copied()
    }

    fn register_pending(&self, pool: Arc<BlockPool>) {
        lock(&self.state).pending.push(pool);
    }

    fn deregister(&self, pool: &Arc<BlockPool>) {
        lock(&self.state).pending.retain(|pending| !Arc::ptr_eq(pending, pool));
    }

    fn kick(&self) {
        lock(&self.state).kicked = true;
        self.condition.notify_one();
    }

    fn request_prepared_chunk(&self) {
        lock(&self.state).chunk_requested = true;
        self.condition.notify_one();
    }

    fn run(&self) {
        loop {
            let mut snapshot = Vec::new();
            let chunk_requested;
            {
                let mut state = lock(&self.state);
                while !state.kicked && !state.chunk_requested {
                    // NB: Collections may never come again, and what is on the list must not wait for one. Each block
                    //     only takes a few rounds of this, after which the list is empty and this thread sleeps for
                    //     good.
                    if state.pending.is_empty() {
                        state = self
                            .condition
                            .wait(state)
                            .unwrap_or_else(|poisoned| poisoned.into_inner());
                    } else {
                        let (next_state, timeout) = self
                            .condition
                            .wait_timeout(state, cold_block_age())
                            .unwrap_or_else(|poisoned| poisoned.into_inner());
                        state = next_state;
                        if timeout.timed_out() {
                            state.kicked = true;
                        }
                    }
                }
                chunk_requested = core::mem::replace(&mut state.chunk_requested, false);
                if core::mem::replace(&mut state.kicked, false) {
                    snapshot = core::mem::take(&mut state.pending);
                }
            }

            if chunk_requested {
                region().prepare_chunk();
            }
            if snapshot.is_empty() {
                continue;
            }

            // NB: Give the thread that kicked this, typically at the end of a sweep, some room before this takes CPU
            //     time and system calls.
            std::thread::sleep(Duration::from_millis(50));

            let return_cold_blocks = {
                let mut state = lock(&self.state);
                let now = Instant::now();
                if now.duration_since(state.last_return_of_cold_blocks) >= cold_block_age() {
                    state.last_return_of_cold_blocks = now;
                    true
                } else {
                    false
                }
            };
            for pool in snapshot {
                self.process_one(pool, return_cold_blocks);
            }
        }
    }

    fn process_one(&self, pool: Arc<BlockPool>, return_cold_blocks: bool) {
        let (to_process, cold_blocks) = {
            let mut state = lock(&pool.state);
            state.in_decommit_registry = false;
            let to_process = core::mem::take(&mut state.freshly_freed);
            // NB: Blocks whose pages were discarded stay in the process until the system runs short of memory, and
            //     only their own type can reuse them. What that type did not reuse since the last time will likely
            //     stay unused.
            let cold_blocks = if cfg!(any(target_os = "linux", target_os = "android")) && return_cold_blocks {
                core::mem::take(&mut state.blocks)
            } else {
                Vec::new()
            };
            (to_process, cold_blocks)
        };

        // NB: The system calls happen outside the pool's lock, so that its heaps can go on allocating and freeing, and
        //     this yields now and then to leave the kernel's memory management to others as well.
        const BATCH: usize = 64;
        for (index, &block) in to_process.iter().enumerate() {
            // SAFETY: The block is free, so nothing needs its contents.
            unsafe { os::discard(block_pointer(block), BLOCK_SIZE) };
            if (index + 1) % BATCH == 0 {
                std::thread::yield_now();
            }
        }
        for (index, &block) in cold_blocks.iter().enumerate() {
            // SAFETY: As above.
            unsafe { os::return_to_system(block_pointer(block), BLOCK_SIZE) };
            if (index + 1) % BATCH == 0 {
                std::thread::yield_now();
            }
        }

        let needs_registering = {
            let mut state = lock(&pool.state);
            state.blocks.extend(to_process);
            state.returned_blocks.extend(cold_blocks);
            // NB: So a pool is only on the list while it has blocks to look at, however many pools there are.
            cfg!(any(target_os = "linux", target_os = "android"))
                && !state.blocks.is_empty()
                && !core::mem::replace(&mut state.in_decommit_registry, true)
        };
        if needs_registering {
            self.register_pending(pool);
        }
    }
}

/// Takes the heap region that C++ reserved at `base`, whose first and last chunks are `first_chunk_offset` and
/// `last_chunk_offset` bytes into it.
#[unsafe(no_mangle)]
pub extern "C" fn rust_gc_heap_region_init(base: *mut u8, first_chunk_offset: usize, last_chunk_offset: usize) {
    abort_on_panic(|| {
        // NB: The collector keeps the addresses of blocks and cells as integers, and compresses them, so this exposes
        //     the provenance of the region, which turning them back into pointers picks up with
        //     with_exposed_provenance().
        let base = base.expose_provenance();
        assert!(base.is_multiple_of(CHUNK_SIZE) && first_chunk_offset.is_multiple_of(CHUNK_SIZE));
        let region = Region {
            base,
            last_chunk_offset,
            chunks: Mutex::new(Chunks {
                next_chunk_offset: first_chunk_offset,
                prepared_chunk: None,
                next_unused_block: 0,
                unused_blocks_end: 0,
            }),
        };
        assert!(REGION.set(region).is_ok(), "the heap region is set up once");
    });
}

/// The end of the part of the region that chunks were committed from so far. No block starts at or above it.
#[unsafe(no_mangle)]
pub extern "C" fn rust_gc_heap_region_committed_end() -> usize {
    abort_on_panic(|| region().committed_end())
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_gc_block_pool_create() -> *const BlockPool {
    Arc::into_raw(Arc::new(BlockPool {
        state: Mutex::new(PoolState::default()),
    }))
}

/// # Safety
///
/// `pool` must come from rust_gc_block_pool_create(), and nothing may use it after this.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_block_pool_destroy(pool: *const BlockPool) {
    abort_on_panic(|| {
        // SAFETY: The caller passes a pool from rust_gc_block_pool_create(), whose reference this takes back.
        let pool = unsafe { Arc::from_raw(pool) };
        // NB: The worker keeps its own reference while it looks at the pool, which it drops once its blocks are done.
        if let Some(worker) = DecommitWorker::the_if_started() {
            worker.deregister(&pool);
        }
    });
}

/// Puts a block back into the pool it came from, see rust_gc_block_pool_deallocate_block().
///
/// # Safety
///
/// `pool` must be a live pool from rust_gc_block_pool_create(), and `block` a block it handed out that nothing uses.
pub(crate) unsafe fn deallocate_block_of(pool: *const BlockPool, block: usize, defer_decommit: bool) {
    // SAFETY: The caller passes a live pool, whose reference count the Arc it came from keeps up.
    let pool = core::mem::ManuallyDrop::new(unsafe { Arc::from_raw(pool) });
    pool.deallocate_block(block, defer_decommit);
}

/// # Safety
///
/// `pool` must be a live pool from rust_gc_block_pool_create().
unsafe fn pool_from<'a>(pool: *const BlockPool) -> &'a BlockPool {
    // SAFETY: The caller passes a live pool.
    unsafe { &*pool }
}

/// # Safety
///
/// `pool` must be a live pool from rust_gc_block_pool_create().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_block_pool_allocate_block(pool: *const BlockPool) -> *mut u8 {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { pool_from(pool) }.allocate_block() as *mut u8)
}

/// Puts a block that `pool` handed out back into it. Unless `defer_decommit` is false, the decommit worker discards its
/// pages after its next kick, if the block is not used again before.
///
/// # Safety
///
/// `pool` must be a live pool from rust_gc_block_pool_create(), and `block` a block it handed out that nothing uses.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_block_pool_deallocate_block(
    pool: *const BlockPool,
    block: *mut u8,
    defer_decommit: bool,
) {
    abort_on_panic(|| {
        assert!(!block.is_null());
        // SAFETY: Forwarded from the caller.
        unsafe { deallocate_block_of(pool, block as usize, defer_decommit) };
    });
}

/// How many blocks of the pool are free after the decommit worker looked at them.
///
/// # Safety
///
/// `pool` must be a live pool from rust_gc_block_pool_create().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_block_pool_block_count(pool: *const BlockPool) -> usize {
    abort_on_panic(|| {
        // SAFETY: Forwarded from the caller.
        let state = lock(&unsafe { pool_from(pool) }.state);
        state.blocks.len() + state.returned_blocks.len()
    })
}

/// How many blocks of the pool had their pages go back to the system for real.
///
/// # Safety
///
/// `pool` must be a live pool from rust_gc_block_pool_create().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_block_pool_returned_block_count(pool: *const BlockPool) -> usize {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| lock(&unsafe { pool_from(pool) }.state).returned_blocks.len())
}

/// Gives the pages of the pool's free blocks back to the system right away, rather than leaving them for the system to
/// take when it runs short of memory. For a heap that went away, whose blocks may never be used again.
///
/// # Safety
///
/// `pool` must be a live pool from rust_gc_block_pool_create().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_block_pool_return_freed_blocks_to_system(pool: *const BlockPool) {
    // SAFETY: Forwarded from the caller.
    abort_on_panic(|| unsafe { pool_from(pool) }.return_freed_blocks_to_system());
}

/// Wakes the decommit worker, starting it if it is not running yet, so that it discards the blocks freed since.
#[unsafe(no_mangle)]
pub extern "C" fn rust_gc_wake_decommit_worker() {
    abort_on_panic(|| DecommitWorker::the().kick());
}

/// Sets how long a block can sit unused before its pages go back to the system, and returns the age it replaces.
#[unsafe(no_mangle)]
pub extern "C" fn rust_gc_set_cold_block_age_in_milliseconds(milliseconds: u64) -> u64 {
    COLD_BLOCK_AGE_IN_MILLISECONDS.swap(milliseconds, Ordering::Relaxed)
}
