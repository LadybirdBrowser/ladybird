/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Marking: from the roots, every cell of the heaps being collected that can be reached gets its mark bit set, and its
//! edges visited.
//!
//! The thread that collects marks alone at first. If cells are left after a few thousand, the helper threads of the
//! pool join in, each with a marker of its own: a cell's mark bit is set atomically, so the marker that sets it is the
//! one that visits its edges, and markers that run out of cells steal them from the others. The cells that markers have
//! yet to visit are kept as 32-bit compressed pointers.
//!
//! A cell's edges are visited through the visit_edges() of the type its block was created for, which gets the visitor
//! of the marker, which hands the cells it finds straight back to the marker. The cells of some blocks may only be
//! visited on the thread that collects, as C++ cells must, so helpers hand those to it.

use core::cell::{Cell, UnsafeCell};
use core::ffi::c_void;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering, fence};
use std::sync::{Barrier, Condvar, Mutex, MutexGuard, PoisonError};

use crossbeam_deque::{Injector, Steal, Stealer, Worker};

use crate::block::{BLOCK_SIZE, BlockDescriptor, descriptor_table, heap_of_descriptor};
use crate::evacuation::{EvacuationSet, PinReason, RecordedSlot, RecordedSlots, SlotKind};
use crate::pool::{Pool, PoolGuard};
use crate::rust_panic::abort_on_panic;

pub(crate) const COMPRESSED_POINTER_SHIFT: u32 = 3;

/// How many cells the thread that collects marks alone before the helpers join in, which takes about as long as waking
/// them. Collections that mark fewer are done before then.
const CELLS_TO_MARK_BEFORE_HELPERS: usize = 4096;

/// How many times a marker that ran out of work looks for more before it parks, see Marker::wait_for_work().
const LOOKS_FOR_WORK_BEFORE_PARKING: u32 = 128;

/// How many cells a marker visits between looks at whether a marker that parked could take some of its work.
const VISITS_BETWEEN_WAKE_CHECKS: u32 = 64;

/// Where the marker finds what it needs in LibGC's cells, type infos and NaN-boxed values.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MarkerLayout {
    pub heap_region_base: usize,
    /// The state of a cell, a u8 that is cell_state_live while the cell is live.
    pub cell_state_offset: u32,
    pub cell_state_live: u8,
    /// `void (*)(Cell*, Visitor*)`.
    pub type_info_visit_edges_offset: u32,
    /// `size_t (*)(Cell const*)`, which may be null.
    pub type_info_external_memory_size_offset: u32,
    /// The bits that are all set in a NaN-boxed value that holds a cell, whose bits under value_offset_mask are then the
    /// cell's offset into the heap region.
    pub value_is_cell_pattern: u64,
    pub value_offset_mask: u64,
}

/// Creates the visitor of a marker, a Cell::Visitor that hands what it visits to the marker, on the marker's thread.
pub type CreateVisitor = unsafe extern "C" fn(context: *mut c_void, marker: *mut Marker) -> *mut c_void;
pub type DestroyVisitor = unsafe extern "C" fn(context: *mut c_void, visitor: *mut c_void);

#[repr(C)]
pub struct MarkRequest {
    pub layout: MarkerLayout,
    /// The heaps whose cells this marks. Cells of other heaps are neither marked nor visited.
    pub heaps: *const *const c_void,
    pub heap_count: usize,
    pub roots: *const *const c_void,
    pub root_count: usize,
    /// Whether the heaps are large enough for helpers to be worth waking, should the collecting thread not finish
    /// alone soon.
    pub parallel: bool,
    pub context: *mut c_void,
    pub create_visitor: CreateVisitor,
    pub destroy_visitor: DestroyVisitor,
    /// Arrays of heap_count entries, which this sets to the bytes of the cells it marked in each heap, and the bytes
    /// that they own outside the heap.
    pub marked_cell_bytes: *mut usize,
    pub marked_external_bytes: *mut usize,
    /// Set to the number of threads that marked.
    pub threads_used: usize,
    /// The evacuation state of a collection that moves cells, which gets the slots marking records, or null.
    pub evacuation: *mut EvacuationSet,
}

type VisitEdges = unsafe extern "C" fn(cell: *mut c_void, visitor: *mut c_void);
type ExternalMemorySize = unsafe extern "C" fn(cell: *const c_void) -> usize;

/// What every marker of one collection shares.
struct Job<'a> {
    layout: MarkerLayout,
    descriptors: NonNull<BlockDescriptor>,
    heaps: &'a [*const c_void],
    markers: Vec<Marker>,
    stealers: Vec<Stealer<u32>>,
    /// The cells that only the thread that collects may visit the edges of.
    owner_queue: Injector<u32>,
    /// The markers that found no work, see Marker::run_round().
    idle_markers: AtomicUsize,
    /// What the markers that ran out of work park on, see Marker::wait_for_work(), and the condition variables that
    /// wake the helpers and the owner. Markers that have work wake them without taking turns: a marker that parks holds
    /// the lock from its last look for work until it waits, so that it misses no wake-up.
    parking: Mutex<()>,
    helpers_wakeup: Condvar,
    owner_wakeup: Condvar,
    /// How many helpers parked, and whether the owner did.
    parked_helpers: AtomicUsize,
    owner_parked: AtomicBool,
    round_done: Barrier,
    /// Whether the markers go for another round, which the owner decides once a round is done.
    another_round: AtomicBool,
    context: *mut c_void,
    create_visitor: CreateVisitor,
    destroy_visitor: DestroyVisitor,
}

// SAFETY: Each marker is only used by its own thread while markers run. The thread that collects reads all of them
//         while every other marker waits at a barrier or is done. The rest is either immutable or synchronized.
unsafe impl Sync for Job<'_> {}

/// One thread's marker. The first is the one of the thread that collects (the owner).
///
/// Marking reenters the marker: a cell's visit_edges() hands the cells it finds back to the marker that visits it. So
/// a marker is only ever used through shared references.
pub struct Marker {
    job: Cell<*const Job<'static>>,
    is_owner: bool,
    /// Whether other markers may set mark bits at the same time.
    shared: Cell<bool>,
    queue: Worker<u32>,
    /// The owner's cells that only it may visit, which no other marker can steal. Once helpers mark too, the owner
    /// visits these first: they are what only it can do, while the other markers take the rest.
    owner_only_queue: Worker<u32>,
    visitor: Cell<*mut c_void>,
    marked_cell_bytes: Box<[Cell<usize>]>,
    marked_external_bytes: Box<[Cell<usize>]>,
    /// Ephemerons whose key was not marked yet when they were visited.
    pending_ephemerons: UnsafeCell<Vec<PendingEphemeron>>,
    /// The slots that point into evacuated blocks, which a collection that moves cells rewrites.
    recorded_slots: UnsafeCell<RecordedSlots>,
}

/// An ephemeron whose key was not marked yet when it was visited, with compressed addresses.
#[derive(Clone, Copy)]
struct PendingEphemeron {
    key: u32,
    value: u32,
    /// Whether marking the value pins it, see Marker::visit_ephemeron().
    pins_value: bool,
}

impl Job<'_> {
    #[inline(always)]
    fn compress(&self, address: usize) -> u32 {
        ((address - self.layout.heap_region_base) >> COMPRESSED_POINTER_SHIFT) as u32
    }

    #[inline(always)]
    fn decompress(&self, compressed: u32) -> usize {
        self.layout.heap_region_base + ((compressed as usize) << COMPRESSED_POINTER_SHIFT)
    }

    /// The descriptor of the block of a cell of the heaps being collected.
    #[inline(always)]
    fn descriptor(&self, address: usize) -> &BlockDescriptor {
        self.descriptor_in_domain(address)
            .expect("the cell is in a heap being collected")
            .1
    }

    /// The index among the heaps being collected of the heap of the block that `address` is in, and the block's
    /// descriptor, if it is one of them. Only then is the descriptor borrowed, as the descriptor of a block of another
    /// heap may change meanwhile, see heap_of_descriptor().
    #[inline(always)]
    fn descriptor_in_domain(&self, address: usize) -> Option<(usize, &BlockDescriptor)> {
        let index = (address - self.layout.heap_region_base) / BLOCK_SIZE;
        // SAFETY: Every cell is in a block of the region, whose descriptor is in the table.
        let descriptor = unsafe { self.descriptors.add(index) };
        // SAFETY: As above.
        let heap = unsafe { heap_of_descriptor(descriptor.as_ptr()) }.cast_const();
        let heap_index = if self.heaps.len() == 1 {
            (self.heaps[0] == heap).then_some(0)
        } else {
            self.heaps.iter().position(|&candidate| candidate == heap)
        }?;
        // SAFETY: The block belongs to a heap being collected, which is stopped, so nothing changes its descriptor but
        //         the mark bits, which are atomic.
        Some((heap_index, unsafe { descriptor.as_ref() }))
    }

    /// Whether `address` is in the heap region, and so in a cell, since the collector only records slots in cells.
    #[inline(always)]
    fn is_in_heap_region(&self, address: usize) -> bool {
        (address.wrapping_sub(self.layout.heap_region_base) as u64) <= self.layout.value_offset_mask
    }
}

impl Marker {
    fn job(&self) -> &Job<'static> {
        // SAFETY: The job outlives its markers' use.
        unsafe { &*self.job.get() }
    }

    /// Sets the mark bit of the cell at `address` if it is in one of the heaps being collected, and queues the cell if
    /// that was news, which this returns. This is a visit by value, which pins the cell if its block is evacuated.
    #[inline(always)]
    fn mark(&self, address: usize) -> bool {
        self.mark_pinned(address, PinReason::Edge)
    }

    #[inline(always)]
    fn mark_pinned(&self, address: usize, reason: PinReason) -> bool {
        let job = self.job();
        let Some((_, descriptor)) = job.descriptor_in_domain(address) else {
            return false;
        };
        if let Some(evacuation) = descriptor.evacuation() {
            evacuation.pin(descriptor, address, reason);
        }
        self.mark_in(descriptor, address)
    }

    /// Marks the cell at `address`, which a slot at `slot` points to. If the cell's block is evacuated, the slot is
    /// recorded, so that it can be rewritten once the cell moves, or the cell pinned if the slot is not in a cell. A
    /// cell that is pinned already, as roots are before marking starts, stays where it is, so its slots are left out.
    #[inline(always)]
    fn mark_through_slot(&self, slot: RecordedSlot, address: usize) {
        self.mark_through_slot_of::<false>(slot, address);
    }

    /// Like mark_through_slot(), and if `SLOT_IS_OWNED`, the slot is recorded even outside the heap region, as it is in
    /// memory that a live cell owns and that stays where it is until the collection is over (see visit_owned_values()).
    #[inline(always)]
    fn mark_through_slot_of<const SLOT_IS_OWNED: bool>(&self, slot: RecordedSlot, address: usize) {
        let job = self.job();
        let Some((_, descriptor)) = job.descriptor_in_domain(address) else {
            return;
        };
        if let Some(evacuation) = descriptor.evacuation()
            && !evacuation.is_pinned(descriptor, address)
        {
            if SLOT_IS_OWNED || job.is_in_heap_region(slot.address()) {
                // SAFETY: Only this marker's thread touches its recorded slots while it marks.
                unsafe { &mut *self.recorded_slots.get() }.push(slot);
            } else {
                evacuation.pin(descriptor, address, PinReason::Edge);
            }
        }
        self.mark_in(descriptor, address);
    }

    /// Pins the cell at `address` without marking it, for a weak reference that the collector cannot update.
    fn pin(&self, address: usize) {
        let job = self.job();
        let Some((_, descriptor)) = job.descriptor_in_domain(address) else {
            return;
        };
        if let Some(evacuation) = descriptor.evacuation() {
            evacuation.pin(descriptor, address, PinReason::Edge);
        }
    }

    /// Records a slot that points into the cell that holds it, if that cell's block is evacuated.
    fn visit_self_pointer(&self, slot: *const usize) {
        let job = self.job();
        if !job.is_in_heap_region(slot.addr()) {
            return;
        }
        if let Some((_, descriptor)) = job.descriptor_in_domain(slot.addr())
            && descriptor.evacuation().is_some()
        {
            // SAFETY: Only this marker's thread touches its recorded slots while it marks.
            unsafe { &mut *self.recorded_slots.get() }.push(RecordedSlot::new(slot.cast(), SlotKind::SelfPointer));
        }
    }

    #[inline(always)]
    fn mark_in(&self, descriptor: &BlockDescriptor, address: usize) -> bool {
        let job = self.job();
        let newly_marked = if self.shared.get() {
            descriptor.mark(address)
        } else {
            descriptor.mark_exclusive(address)
        };
        if newly_marked {
            // NB: The cell's edges are visited soon, as the queue is a stack.
            #[cfg(target_arch = "x86_64")]
            // SAFETY: Prefetching has no effect on memory.
            unsafe {
                core::arch::x86_64::_mm_prefetch(address as *const i8, core::arch::x86_64::_MM_HINT_T0);
            }
            let compressed = job.compress(address);
            if descriptor.traced_on_owner_thread_only && self.shared.get() {
                if self.is_owner {
                    self.owner_only_queue.push(compressed);
                } else {
                    job.owner_queue.push(compressed);
                }
            } else {
                self.queue.push(compressed);
            }
        }
        newly_marked
    }

    /// Marks the cells of the values in `values`, which are slots.
    #[inline(always)]
    fn visit_values(&self, values: &[u64]) {
        self.visit_values_of::<false>(values);
    }

    /// Marks the cells of the values in `values`, which are slots in memory outside the heap region that the cell
    /// being visited owns, and that nothing frees, moves or writes until the collection is over. A collection rewrites
    /// them where they are, like slots in cells, rather than pinning their cells.
    #[inline(always)]
    fn visit_owned_values(&self, values: &[u64]) {
        self.visit_values_of::<true>(values);
    }

    #[inline(always)]
    fn visit_values_of<const SLOTS_ARE_OWNED: bool>(&self, values: &[u64]) {
        let layout = &self.job().layout;
        for value in values {
            let bits = *value;
            if bits & layout.value_is_cell_pattern == layout.value_is_cell_pattern {
                self.mark_through_slot_of::<SLOTS_ARE_OWNED>(
                    RecordedSlot::new(core::ptr::from_ref(value).cast(), SlotKind::Value),
                    layout.heap_region_base + (bits & layout.value_offset_mask) as usize,
                );
            }
        }
    }

    /// Marks the cell a compressed pointer at `slot` points to, if it is not null.
    #[inline(always)]
    fn visit_member(&self, slot: *const u32) {
        self.visit_member_of::<false>(slot);
    }

    #[inline(always)]
    fn visit_member_of<const SLOT_IS_OWNED: bool>(&self, slot: *const u32) {
        // SAFETY: The caller passes a live slot.
        let compressed = unsafe { slot.read() };
        if compressed != 0 {
            let address = self.job().decompress(compressed);
            self.mark_through_slot_of::<SLOT_IS_OWNED>(RecordedSlot::new(slot.cast(), SlotKind::Member), address);
        }
    }

    /// Marks the cell that the pointer at `slot` points to, if it is not null, as a slot in memory that the cell being
    /// visited owns, which the collector can update.
    fn visit_owned_cell_pointer(&self, slot: *const usize) {
        // SAFETY: The caller passes a live slot.
        let address = unsafe { slot.read() };
        if address != 0 {
            // NB: A pointer to the start of a cell is an interior pointer at offset 0.
            self.mark_through_slot_of::<true>(RecordedSlot::new(slot.cast(), SlotKind::InteriorPointer), address);
        }
    }

    /// Marks `value` once `key` is marked. If `pins`, the key and the value stay where they are, as their holder visits
    /// them by value. Otherwise the holder hands their slots to the collection once marking is done (see
    /// rust_gc_evacuation_record_slot()), and only cells pinned by others stay where they are.
    fn visit_ephemeron(&self, key: usize, value: usize, pins: bool) {
        let job = self.job();
        if pins {
            self.pin(key);
        }
        // NB: Keys outside the heaps being collected are never marked here, so their values stay reachable.
        if job
            .descriptor_in_domain(key)
            .is_none_or(|(_, descriptor)| descriptor.is_marked(key))
        {
            self.mark_value_of_ephemeron(value, pins);
            return;
        }
        // SAFETY: Only this marker's thread touches its pending ephemerons while it marks.
        unsafe { &mut *self.pending_ephemerons.get() }.push(PendingEphemeron {
            key: job.compress(key),
            value: job.compress(value),
            pins_value: pins,
        });
    }

    /// Marks the value of an ephemeron whose key is marked, and returns whether that was news.
    fn mark_value_of_ephemeron(&self, value: usize, pins: bool) -> bool {
        if pins {
            return self.mark(value);
        }
        let job = self.job();
        let Some((_, descriptor)) = job.descriptor_in_domain(value) else {
            return false;
        };
        self.mark_in(descriptor, value)
    }

    fn find_work(&self) -> Option<u32> {
        let job = self.job();
        if self.is_owner
            && let Some(cell) = self.owner_only_queue.pop()
        {
            return Some(cell);
        }
        loop {
            let mut retry = false;
            if self.is_owner {
                match job.owner_queue.steal_batch_and_pop(&self.owner_only_queue) {
                    Steal::Success(cell) => return Some(cell),
                    Steal::Retry => retry = true,
                    Steal::Empty => {}
                }
            }
            if let Some(cell) = self.queue.pop() {
                return Some(cell);
            }
            if self.shared.get() {
                for stealer in &job.stealers {
                    match stealer.steal_batch_and_pop(&self.queue) {
                        Steal::Success(cell) => return Some(cell),
                        Steal::Retry => retry = true,
                        Steal::Empty => {}
                    }
                }
            }
            if !retry {
                return None;
            }
        }
    }

    fn has_visible_work(&self) -> bool {
        let job = self.job();
        (self.is_owner && !job.owner_queue.is_empty()) || job.stealers.iter().any(|stealer| !stealer.is_empty())
    }

    /// Visits the edges of a marked cell, and counts the cell and the memory it owns outside the heap.
    // OPTIMIZATION: Every marked cell takes this path, which a call per cell would make slower.
    #[inline(always)]
    fn visit_edges_of(&self, compressed: u32) {
        let job = self.job();
        let layout = &job.layout;
        let address = job.decompress(compressed);
        let (heap_index, descriptor) = job
            .descriptor_in_domain(address)
            .expect("only cells of the heaps being collected are marked");
        if !self.is_owner && descriptor.traced_on_owner_thread_only {
            job.owner_queue.push(compressed);
            return;
        }
        // NB: Cell addresses are in the heap region, whose provenance rust_gc_heap_region_init() exposed.
        let cell = core::ptr::with_exposed_provenance_mut::<c_void>(address);
        // SAFETY: The cell is in a block of a heap being collected, which no thread changes meanwhile.
        if unsafe { cell.cast::<u8>().add(layout.cell_state_offset as usize).read() } != layout.cell_state_live {
            return;
        }
        let marked_cell_bytes = &self.marked_cell_bytes[heap_index];
        marked_cell_bytes.set(marked_cell_bytes.get() + descriptor.cell_size as usize);
        let type_info = descriptor.type_info.cast::<u8>();
        // SAFETY: The type info of a block outlives it, and the layout says where its functions are.
        let (external_memory_size, visit_edges) = unsafe {
            (
                type_info
                    .add(layout.type_info_external_memory_size_offset as usize)
                    .cast::<Option<ExternalMemorySize>>()
                    .read(),
                type_info
                    .add(layout.type_info_visit_edges_offset as usize)
                    .cast::<VisitEdges>()
                    .read(),
            )
        };
        if let Some(external_memory_size) = external_memory_size {
            // SAFETY: The cell is a live cell of the type whose function this is.
            let size = unsafe { external_memory_size(cell) };
            let marked_external_bytes = &self.marked_external_bytes[heap_index];
            marked_external_bytes.set(marked_external_bytes.get().saturating_add(size));
        }
        // SAFETY: The cell is a live cell of the type whose function this is, which may be visited on this thread, and
        //         the visitor is this marker's.
        unsafe { visit_edges(cell, self.visitor.get()) };
    }

    /// Visits the edges of at most `limit` cells, on this thread alone, and returns whether no cell was left.
    fn drain_alone(&self, mut limit: usize) -> bool {
        while limit != 0 {
            let Some(cell) = self.find_work() else {
                return true;
            };
            self.visit_edges_of(cell);
            limit -= 1;
        }
        self.queue.is_empty() && self.owner_only_queue.is_empty() && self.job().owner_queue.is_empty()
    }

    /// Visits edges until no marker has cells left.
    fn run_round(&self) {
        let job = self.job();
        loop {
            let mut visits = 0u32;
            while let Some(cell) = self.find_work() {
                self.visit_edges_of(cell);
                visits += 1;
                if visits == VISITS_BETWEEN_WAKE_CHECKS {
                    visits = 0;
                    self.wake_parked_markers();
                }
            }
            // NB: The cells this marker queued last for the owner are only in the owner queue.
            self.wake_parked_markers();

            // NB: A marker only queues cells while it is not idle, and only the owner takes from the owner queue. So
            //     once every marker is idle and the owner queue is empty, no cell is left anywhere.
            job.idle_markers.fetch_add(1, Ordering::SeqCst);
            if !self.wait_for_work() {
                return;
            }
            job.idle_markers.fetch_sub(1, Ordering::SeqCst);
        }
    }

    /// Whether every marker ran out of work, and no cell is left anywhere, see run_round().
    fn round_is_over(&self) -> bool {
        let job = self.job();
        // NB: Only markers that mark in parallel run rounds, and then every marker of the job does.
        job.idle_markers.load(Ordering::SeqCst) == job.markers.len() && job.owner_queue.is_empty()
    }

    /// Waits, as an idle marker, until another marker has work this one can take, and returns true, or until the round
    /// is over, and returns false. A marker looks for work a few times, and then parks until a marker with work to
    /// spare or cells for the owner wakes it, see wake_parked_markers(), or the round is over.
    fn wait_for_work(&self) -> bool {
        let job = self.job();
        for _ in 0..LOOKS_FOR_WORK_BEFORE_PARKING {
            if self.has_visible_work() {
                return true;
            }
            if self.round_is_over() {
                self.end_round();
                return false;
            }
            core::hint::spin_loop();
        }

        let mut guard = lock(&job.parking);
        if self.is_owner {
            job.owner_parked.store(true, Ordering::SeqCst);
        } else {
            job.parked_helpers.fetch_add(1, Ordering::SeqCst);
        }
        // NB: A marker with work stores its cells before it looks at who parked, see wake_parked_markers(), and this one
        //     says it parked before it looks for cells, so that one of them sees what the other did.
        fence(Ordering::SeqCst);
        let found_work = loop {
            if self.has_visible_work() {
                break true;
            }
            if self.round_is_over() {
                break false;
            }
            let wakeup = if self.is_owner {
                &job.owner_wakeup
            } else {
                &job.helpers_wakeup
            };
            guard = wakeup.wait(guard).unwrap_or_else(PoisonError::into_inner);
            fence(Ordering::SeqCst);
        };
        if self.is_owner {
            job.owner_parked.store(false, Ordering::SeqCst);
        } else {
            job.parked_helpers.fetch_sub(1, Ordering::SeqCst);
        }
        drop(guard);
        if !found_work {
            self.end_round();
        }
        found_work
    }

    /// Wakes every marker that parked, once the round is over.
    fn end_round(&self) {
        let job = self.job();
        let _guard = lock(&job.parking);
        job.helpers_wakeup.notify_all();
        job.owner_wakeup.notify_all();
    }

    /// Wakes a marker that parked, where this one has work in its queue to spare, which any marker may steal, and the
    /// owner, where it parked while the owner queue has cells.
    fn wake_parked_markers(&self) {
        let job = self.job();
        fence(Ordering::SeqCst);
        let owner_parked = job.owner_parked.load(Ordering::Relaxed);
        let has_work_to_spare = self.queue.len() > 1;
        let wake_helper = has_work_to_spare && job.parked_helpers.load(Ordering::Relaxed) != 0;
        let wake_owner = owner_parked && ((has_work_to_spare && !wake_helper) || !job.owner_queue.is_empty());
        if !wake_helper && !wake_owner {
            return;
        }
        let _guard = lock(&job.parking);
        if wake_helper {
            job.helpers_wakeup.notify_one();
        }
        if wake_owner {
            job.owner_wakeup.notify_one();
        }
    }

    /// Marks the values of the pending ephemerons of every marker whose key was marked since, into this marker, which
    /// must be the owner's. Returns whether that marked any.
    fn mark_values_of_reached_ephemerons(&self) -> bool {
        let job = self.job();
        let mut marked_a_value = false;
        for marker in &job.markers {
            // SAFETY: Every other marker waits at the barrier or is done.
            let pending = unsafe { &mut *marker.pending_ephemerons.get() };
            pending.retain(|ephemeron| {
                let key = job.decompress(ephemeron.key);
                if !job.descriptor(key).is_marked(key) {
                    return true;
                }
                marked_a_value |= self.mark_value_of_ephemeron(job.decompress(ephemeron.value), ephemeron.pins_value);
                false
            });
        }
        marked_a_value
    }

    fn create_visitor(&self) {
        let job = self.job();
        // SAFETY: The marker outlives its visitor.
        let visitor = unsafe { (job.create_visitor)(job.context, core::ptr::from_ref(self).cast_mut()) };
        self.visitor.set(visitor);
    }

    fn destroy_visitor(&self) {
        let job = self.job();
        // SAFETY: The visitor was created for this marker, which is done with it.
        unsafe { (job.destroy_visitor)(job.context, self.visitor.get()) };
    }
}

/// What every marker runs once the helpers joined in, with its index; the owner's is 0.
fn mark_in_parallel(job: &Job<'static>, index: usize) {
    let marker = &job.markers[index];
    marker.shared.set(true);
    if index != 0 {
        marker.create_visitor();
    }
    loop {
        marker.run_round();
        job.round_done.wait();
        // NB: Marking can reach the key of a pending ephemeron, so the owner looks at those between rounds, and the
        //     markers go again while that marks more.
        if index == 0 {
            job.idle_markers.store(0, Ordering::SeqCst);
            let another_round = marker.mark_values_of_reached_ephemerons();
            job.another_round.store(another_round, Ordering::SeqCst);
        }
        job.round_done.wait();
        if !job.another_round.load(Ordering::SeqCst) {
            break;
        }
    }
    if index != 0 {
        marker.destroy_visitor();
    }
}

/// # Safety
///
/// `data` must be a live Job, and `index` a marker of it that runs nowhere else.
unsafe fn mark_in_parallel_from_pool(data: *const (), index: usize) {
    // SAFETY: The caller passes a live job.
    let job = unsafe { &*data.cast::<Job<'static>>() };
    abort_on_panic(|| mark_in_parallel(job, index));
}

/// Marks the cell a visitor visits, which must be the start of a cell.
///
/// # Safety
///
/// `marker` must be the marker the visitor was created for, on its thread, and `cell` a cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_marker_visit_cell(marker: *mut Marker, cell: *const c_void) {
    // SAFETY: The caller passes a live marker.
    unsafe { &*marker }.mark(cell.addr());
}

/// Marks the cells of the NaN-boxed values a visitor visits.
///
/// # Safety
///
/// As for rust_gc_marker_visit_cell(), with `count` values at `values`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_marker_visit_values(marker: *mut Marker, values: *const u64, count: usize) {
    // SAFETY: The caller passes a live marker and count values.
    unsafe { &*marker }.visit_values(unsafe { ak::slice_from_raw_parts(values, count) });
}

/// Marks the cells of the NaN-boxed values a visitor visits in memory that the cell being visited owns outside the heap
/// region, as slots the collector can update.
///
/// # Safety
///
/// As for rust_gc_marker_visit_values(), and the values must be in memory that the cell being visited owns, which
/// nothing frees, moves or writes until the collection is over: no finalizer, destructor or sweep callback touches it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_marker_visit_owned_values(marker: *mut Marker, values: *const u64, count: usize) {
    // SAFETY: The caller passes a live marker and count values.
    unsafe { &*marker }.visit_owned_values(unsafe { ak::slice_from_raw_parts(values, count) });
}

/// Marks the cell that the compressed pointer at `slot` points to, if it is not null, as a slot the collector can update.
///
/// # Safety
///
/// As for rust_gc_marker_visit_cell(), and `slot` must hold a compressed cell pointer or 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_marker_visit_member(marker: *mut Marker, slot: *const u32) {
    // SAFETY: The caller passes a live marker and slot.
    unsafe { &*marker }.visit_member(slot);
}

/// Marks the cell that the compressed pointer at `slot` points to, if it is not null, as a slot in memory that the cell
/// being visited owns outside the heap region, which the collector can update.
///
/// # Safety
///
/// As for rust_gc_marker_visit_member(), and the slot must be in memory that the cell being visited owns, which nothing
/// frees, moves or writes until the collection is over: no finalizer, destructor or sweep callback touches it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_marker_visit_owned_member(marker: *mut Marker, slot: *const u32) {
    // SAFETY: The caller passes a live marker and slot.
    unsafe { &*marker }.visit_member_of::<true>(slot);
}

/// Marks the cell that the pointer at `slot` points to, if it is not null, as a slot in memory that the cell being
/// visited owns outside the heap region, which the collector can update.
///
/// # Safety
///
/// As for rust_gc_marker_visit_owned_member(), and `slot` must hold a pointer to the start of a cell, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_marker_visit_owned_cell_pointer(marker: *mut Marker, slot: *const usize) {
    // SAFETY: The caller passes a live marker and slot.
    unsafe { &*marker }.visit_owned_cell_pointer(slot);
}

/// Marks `cell`, which the pointer at `slot` points into, as a slot the collector can update.
///
/// # Safety
///
/// As for rust_gc_marker_visit_cell(), and `slot` must hold a pointer into `cell`, past its start.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_marker_visit_interior_pointer(
    marker: *mut Marker,
    slot: *const usize,
    cell: *const c_void,
) {
    // SAFETY: The caller passes a live marker.
    unsafe { &*marker }.mark_through_slot(RecordedSlot::new(slot.cast(), SlotKind::InteriorPointer), cell.addr());
}

/// Notes that the pointer at `slot` points into the cell that holds the slot, which keeps it right if that cell moves.
///
/// # Safety
///
/// As for rust_gc_marker_visit_cell(), and `slot` must be in a cell being visited, and point into it, up to one past
/// its end.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_marker_visit_self_pointer(marker: *mut Marker, slot: *const usize) {
    // SAFETY: The caller passes a live marker.
    unsafe { &*marker }.visit_self_pointer(slot);
}

/// Keeps `cell` from moving, without marking it, for a weak reference the collector cannot update.
///
/// # Safety
///
/// As for rust_gc_marker_visit_cell().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_marker_pin_cell(marker: *mut Marker, cell: *const c_void) {
    // SAFETY: The caller passes a live marker.
    unsafe { &*marker }.pin(cell.addr());
}

/// Marks a cell that is reachable only while `key` is, such as a WeakMap entry's value, once the key is marked. Both
/// stay where they are, as their holder visits them by value.
///
/// # Safety
///
/// As for rust_gc_marker_visit_cell(), and `key` and `value` must be cells.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_marker_visit_ephemeron(marker: *mut Marker, key: *const c_void, value: *const c_void) {
    // SAFETY: The caller passes a live marker.
    unsafe { &*marker }.visit_ephemeron(key.addr(), value.addr(), true);
}

/// Like rust_gc_marker_visit_ephemeron(), for a holder that keeps neither cell from moving: once marking is done, and
/// before cells move, it hands the slots of the key and the value of each entry that survived to the collection.
///
/// # Safety
///
/// As for rust_gc_marker_visit_ephemeron().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_marker_visit_ephemeron_without_pinning(
    marker: *mut Marker,
    key: *const c_void,
    value: *const c_void,
) {
    // SAFETY: The caller passes a live marker.
    unsafe { &*marker }.visit_ephemeron(key.addr(), value.addr(), false);
}

/// Has the threads that help mark give back the memory they no longer use, see Pool::release_unused_memory().
#[unsafe(no_mangle)]
pub extern "C" fn rust_gc_marker_release_unused_helper_memory() {
    abort_on_panic(|| {
        if let Some(pool) = Pool::the() {
            pool.release_unused_memory();
        }
    });
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Marks every cell of the heaps that the roots reach, and adds up the bytes they take.
///
/// # Safety
///
/// The request must be valid as its fields describe. Nothing else may use the heaps until this returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_gc_marker_mark(request: *mut MarkRequest) {
    abort_on_panic(|| {
        // SAFETY: The caller passes a valid request.
        let request = unsafe { &mut *request };
        // SAFETY: The caller passes heap_count heaps.
        let heaps = unsafe { ak::slice_from_raw_parts(request.heaps, request.heap_count) };
        // SAFETY: The caller passes root_count roots.
        let roots = unsafe { ak::slice_from_raw_parts(request.roots, request.root_count) };
        mark(request, heaps, roots);
    });
}

fn mark(request: &mut MarkRequest, heaps: &[*const c_void], roots: &[*const c_void]) {
    let pool = if request.parallel { Pool::the() } else { None };
    let marker_count = pool.map_or(0, Pool::helper_count) + 1;
    let mut job = Job {
        layout: request.layout,
        descriptors: descriptor_table(),
        heaps,
        markers: Vec::with_capacity(marker_count),
        stealers: Vec::with_capacity(marker_count),
        owner_queue: Injector::new(),
        idle_markers: AtomicUsize::new(0),
        parking: Mutex::new(()),
        helpers_wakeup: Condvar::new(),
        owner_wakeup: Condvar::new(),
        parked_helpers: AtomicUsize::new(0),
        owner_parked: AtomicBool::new(false),
        round_done: Barrier::new(marker_count),
        another_round: AtomicBool::new(false),
        context: request.context,
        create_visitor: request.create_visitor,
        destroy_visitor: request.destroy_visitor,
    };
    for index in 0..marker_count {
        let queue = Worker::new_lifo();
        job.stealers.push(queue.stealer());
        job.markers.push(Marker {
            job: Cell::new(core::ptr::null()),
            is_owner: index == 0,
            shared: Cell::new(false),
            queue,
            owner_only_queue: Worker::new_lifo(),
            visitor: Cell::new(core::ptr::null_mut()),
            marked_cell_bytes: (0..heaps.len()).map(|_| Cell::new(0)).collect(),
            marked_external_bytes: (0..heaps.len()).map(|_| Cell::new(0)).collect(),
            pending_ephemerons: UnsafeCell::new(Vec::new()),
            recorded_slots: UnsafeCell::new(RecordedSlots::default()),
        });
    }
    // NB: The job lives on this stack until every marker is done with it, so the markers may point at it as if it lived
    //     forever.
    let job_pointer = (&raw const job).cast::<Job<'static>>();
    for marker in &job.markers {
        marker.job.set(job_pointer);
    }
    // SAFETY: See job_pointer.
    let job = unsafe { &*job_pointer };
    let owner = &job.markers[0];
    owner.create_visitor();
    for &root in roots {
        owner.mark_pinned(root.addr(), PinReason::Root);
    }

    // NB: Collections that mark few cells are done before helpers could have started.
    let mut threads_used = 1;
    let mut done_alone = owner.drain_alone(CELLS_TO_MARK_BEFORE_HELPERS);
    let helpers: Option<PoolGuard> = if done_alone {
        None
    } else {
        pool.and_then(Pool::try_acquire)
    };
    match helpers {
        Some(helpers) => {
            threads_used = marker_count;
            // SAFETY: mark_in_parallel_from_pool() takes the job and the index of a helper's marker, which runs nowhere
            //         else.
            unsafe {
                helpers.run(mark_in_parallel_from_pool, job_pointer.cast(), || {
                    mark_in_parallel(job, 0);
                });
            };
        }
        None => {
            // NB: Another heap may be using the helpers, so this one marks alone.
            loop {
                if !done_alone {
                    owner.drain_alone(usize::MAX);
                }
                if !owner.mark_values_of_reached_ephemerons() {
                    break;
                }
                done_alone = false;
            }
        }
    }
    owner.destroy_visitor();

    for heap_index in 0..heaps.len() {
        let mut cell_bytes = 0usize;
        let mut external_bytes = 0usize;
        for marker in &job.markers {
            cell_bytes += marker.marked_cell_bytes[heap_index].get();
            external_bytes = external_bytes.saturating_add(marker.marked_external_bytes[heap_index].get());
        }
        // SAFETY: The caller passes arrays of heap_count entries.
        unsafe {
            request.marked_cell_bytes.add(heap_index).write(cell_bytes);
            request.marked_external_bytes.add(heap_index).write(external_bytes);
        }
    }
    request.threads_used = threads_used;

    // SAFETY: The caller passes null or a live evacuation set, which nothing else uses meanwhile.
    if let Some(evacuation) = unsafe { request.evacuation.as_mut() } {
        for marker in &job.markers {
            // SAFETY: Every marker is done.
            evacuation.add_recorded_slots(core::mem::take(unsafe { &mut *marker.recorded_slots.get() }));
        }
    }
}
