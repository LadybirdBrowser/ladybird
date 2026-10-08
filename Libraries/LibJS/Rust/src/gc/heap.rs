/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, OnceCell, RefCell};
use core::ffi::c_void;
use core::mem::offset_of;
use core::ptr::NonNull;
use std::collections::HashMap;

use super::capi::{self, GCAllocator, GCGatherRootsCallback, GCHeap, GCLayout};
use super::class::{CellTypeInfo, Class, GcCell};
use super::class_id::CLASS_COUNT;
use super::weak::WEAK_IMPL_POINTER_OFFSET;
use crate::build_configuration::{HEAP_REGION_OFFSET_MASK, PRIMITIVE_STORAGE_CAGE_OFFSET_MASK};
use crate::layout::cell::{CellHeader, CellState, Gc};

/// A LibGC heap, with one allocator per class of cell.
///
/// Cells are taken from the local free lists of the allocators in the heap without a call into LibGC whenever the heap
/// does not have to collect garbage (see gc_heap_allocator_local_free_list()).
pub struct Heap {
    raw: NonNull<GCHeap>,
    allocators: [Cell<Option<HeapAllocator>>; CLASS_COUNT],
    /// The allocators of the classes derived at run time, by the address of their class.
    runtime_class_allocators: RefCell<HashMap<usize, HeapAllocator, foldhash::fast::RandomState>>,
    /// The size classes of each class, by class id, out of line, as few classes have any.
    size_class_tables: Box<[OnceCell<SizeClassTable>; CLASS_COUNT]>,
    inline_allocation_info: InlineAllocationInfo,
}

/// The size classes of a class, with the type infos their blocks dispatch through, which have to outlive the
/// allocators.
struct SizeClassTable {
    allocators: Box<[SizeClassAllocator]>,
    _type_infos: Box<[CellTypeInfo]>,
}

/// What the heap needs to know to allocate cells from its local free lists itself, besides the address of the list:
/// where the heap's allocation counters are, and where a free cell keeps the link to the next free cell and which bits
/// of it count. See gc_heap_allocator_local_free_list() for how to allocate with them.
struct InlineAllocationInfo {
    allocated_bytes_since_last_gc: NonNull<usize>,
    gc_bytes_threshold: NonNull<usize>,
    total_allocated_bytes: NonNull<usize>,
    free_cell_next_offset: usize,
    free_cell_link_mask: usize,
}

/// An allocator of a heap, with the address of its local free list in the heap, which is null if every cell has to be
/// allocated through LibGC.
#[derive(Clone, Copy)]
struct HeapAllocator {
    raw: NonNull<GCAllocator>,
    local_free_list: *mut *mut c_void,
}

impl HeapAllocator {
    fn local_free_list(self) -> Option<NonNull<*mut c_void>> {
        NonNull::new(self.local_free_list)
    }
}

/// The allocator of a class derived at run time, which only the cells of that class come from, as each C++ class that
/// declares GC_DECLARE_ALLOCATOR has one of its own. It belongs to the heap that created it and lives as long as it.
#[derive(Clone, Copy)]
pub struct RuntimeClassAllocator {
    class: &'static Class,
    allocator: HeapAllocator,
}

impl RuntimeClassAllocator {
    pub fn class(&self) -> &'static Class {
        self.class
    }
}

impl PartialEq for RuntimeClassAllocator {
    fn eq(&self, other: &Self) -> bool {
        self.allocator.raw == other.allocator.raw
    }
}

impl Eq for RuntimeClassAllocator {}

/// One size class of a class whose cells end in a variable-size part, like an array of trailing values: an allocator of
/// its own, whose cells are larger than the type of the class. It belongs to the heap that created it and lives as long
/// as it. LibGC dispatches through the class's type info with the size class's cell size, so gc_cell_type_info() of
/// such a cell is not the type info of its class.
#[derive(Clone, Copy)]
pub struct SizeClassAllocator {
    class: &'static Class,
    cell_size: u32,
    allocator: HeapAllocator,
}

impl SizeClassAllocator {
    pub fn class(&self) -> &'static Class {
        self.class
    }

    /// The size of each cell, including the part beyond the type of the class.
    pub fn cell_size(&self) -> u32 {
        self.cell_size
    }
}

impl PartialEq for SizeClassAllocator {
    fn eq(&self, other: &Self) -> bool {
        self.allocator.raw == other.allocator.raw
    }
}

impl Eq for SizeClassAllocator {}

/// Whether an allocation may collect garbage and counts towards the next collection, as cells do, or does neither, as
/// the cells other cells keep their storage in.
#[derive(Clone, Copy)]
enum AllocationKind {
    Cell,
    Storage,
}

impl Heap {
    /// Creates a heap that calls `gather_roots` with `context` whenever it gathers its roots. A heap that becomes the
    /// process default is the one GC::Heap::the() returns to C++ code.
    ///
    /// # Safety
    ///
    /// `context` must stay valid for as long as the heap exists, including while it is being destroyed.
    pub unsafe fn new(gather_roots: GCGatherRootsCallback, context: *mut c_void, become_process_default: bool) -> Self {
        let layout = check_layout();
        // SAFETY: The caller keeps the context alive for the heap's lifetime.
        let raw = unsafe { capi::gc_heap_create(gather_roots, context, become_process_default) };
        let raw = NonNull::new(raw).expect("LibGC creates the heap");
        // SAFETY: LibGC reports offsets of fields of its heap.
        let counter = |offset: u32| unsafe { raw.byte_add(offset as usize) }.cast::<usize>();
        Self {
            raw,
            allocators: [const { Cell::new(None) }; CLASS_COUNT],
            runtime_class_allocators: RefCell::default(),
            size_class_tables: Box::new([const { OnceCell::new() }; CLASS_COUNT]),
            inline_allocation_info: InlineAllocationInfo {
                allocated_bytes_since_last_gc: counter(layout.heap_allocated_bytes_since_last_gc_offset),
                gc_bytes_threshold: counter(layout.heap_gc_bytes_threshold_offset),
                total_allocated_bytes: counter(layout.heap_total_allocated_bytes_offset),
                free_cell_next_offset: layout.free_cell_next_offset as usize,
                free_cell_link_mask: layout.free_cell_link_mask as usize,
            },
        }
    }

    pub fn raw(&self) -> *mut GCHeap {
        self.raw.as_ptr()
    }

    /// Moves `cell` into the heap. Nothing can collect garbage between taking the storage and writing the cell, so
    /// the collector never sees a partially written cell.
    pub fn allocate<T: GcCell>(&self, cell: T) -> Gc<T> {
        let allocator = self.allocator_for(T::CLASS);
        // SAFETY: The allocator is the one of the cell's static class, whose cells have the size and alignment of a T.
        unsafe { self.allocate_with(allocator, size_of::<T>(), AllocationKind::Cell, cell, |_| {}) }
    }

    /// Moves `cell`, whose class is the one `allocator` was created for or a class derived from it, into the storage
    /// of that allocator. A derived class shares its ancestor's allocator when its cells may share heap blocks with the
    /// ancestor's, as a host class that shares its parent's allocator does.
    pub fn allocate_in<T: GcCell>(&self, allocator: RuntimeClassAllocator, cell: T) -> Gc<T> {
        assert!(
            allocator.class.type_info.cell_size as usize == size_of::<T>() && allocator.class.is_subclass_of(T::CLASS),
            "{} cells are not {}s",
            allocator.class.name,
            T::CLASS.name
        );
        // SAFETY: Every cell starts with its header.
        debug_assert!(
            unsafe { (*core::ptr::from_ref(&cell).cast::<CellHeader>()).class }.is_subclass_of(allocator.class)
        );
        // SAFETY: The allocator's cells have the size of a T, checked above, and T's alignment, as its class extends T's.
        unsafe { self.allocate_with(allocator.allocator, size_of::<T>(), AllocationKind::Cell, cell, |_| {}) }
    }

    /// Moves `cell` into a cell of `size_class`, whose class is the class of the cell or one that it extends, and fills
    /// the rest of the larger cell with copies of `tail`. Finding and tracing the part beyond the T is up to the T.
    pub fn allocate_with_size_class<T: GcCell, E: Copy>(
        &self,
        size_class: SizeClassAllocator,
        cell: T,
        tail: E,
    ) -> Gc<T> {
        self.allocate_in_size_class(size_class, AllocationKind::Cell, cell, tail)
    }

    /// allocate_with_size_class() for a cell that another cell keeps its storage in, instead of malloc memory: it never
    /// collects garbage and does not count towards the next collection, so storage can grow while its owner is between
    /// states nothing may observe, and callers can hold on to anything across the growth. Live storage counts once a
    /// collection finds it, through the live cell bytes that set the next threshold.
    pub fn allocate_storage_with_size_class<T: GcCell, E: Copy>(
        &self,
        size_class: SizeClassAllocator,
        cell: T,
        tail: E,
    ) -> Gc<T> {
        self.allocate_in_size_class(size_class, AllocationKind::Storage, cell, tail)
    }

    #[inline(always)]
    fn allocate_in_size_class<T: GcCell, E: Copy>(
        &self,
        size_class: SizeClassAllocator,
        kind: AllocationKind,
        cell: T,
        tail: E,
    ) -> Gc<T> {
        const {
            assert!(
                size_of::<E>() > 0 && align_of::<E>() <= 8,
                "the tail starts 8-byte aligned"
            );
        }
        assert!(
            size_class.class.type_info.cell_size as usize == size_of::<T>()
                && size_class.class.is_subclass_of(T::CLASS),
            "{} cells are not {}s",
            size_class.class.name,
            T::CLASS.name
        );
        // SAFETY: Every cell starts with its header.
        debug_assert!(
            unsafe { (*core::ptr::from_ref(&cell).cast::<CellHeader>()).class }.is_subclass_of(size_class.class)
        );
        let tail_size = size_class.cell_size as usize - size_of::<T>();
        assert!(
            tail_size.is_multiple_of(size_of::<E>()),
            "the tail of a size class is a whole number of values"
        );
        let initialize_tail = |tail_start: *mut u8| {
            let tail_start = tail_start.cast::<E>();
            for index in 0..tail_size / size_of::<E>() {
                // SAFETY: The cell has tail_size bytes beyond the T, which starts 8-byte aligned, as each class's size
                // is a multiple of 8.
                unsafe { tail_start.add(index).write(tail) };
            }
        };
        // SAFETY: The size class's cells are larger than a T, checked above, and have T's alignment, as its class
        // extends T's.
        unsafe {
            self.allocate_with(
                size_class.allocator,
                size_class.cell_size as usize,
                kind,
                cell,
                initialize_tail,
            )
        }
    }

    /// # Safety
    ///
    /// The allocator must belong to this heap and hand out storage of `cell_size` bytes, at least the size of a T, with
    /// the alignment of a T, and `initialize_tail` must write every byte beyond the T, which it gets the address of.
    #[inline(always)]
    unsafe fn allocate_with<T: GcCell>(
        &self,
        allocator: HeapAllocator,
        cell_size: usize,
        kind: AllocationKind,
        cell: T,
        initialize_tail: impl FnOnce(*mut u8),
    ) -> Gc<T> {
        let mut must_mark = false;
        // SAFETY: The allocator belongs to this heap and hands out cells of cell_size bytes.
        let storage = match unsafe { self.take_from_local_free_list(allocator, cell_size, kind) } {
            Some(storage) => storage,
            // SAFETY: The heap and the allocator are live.
            None => unsafe {
                let raw = allocator.raw.as_ptr();
                let storage = match kind {
                    AllocationKind::Cell => capi::gc_heap_allocate_cell(self.raw.as_ptr(), raw, &raw mut must_mark),
                    AllocationKind::Storage => {
                        capi::gc_heap_allocate_storage_cell(self.raw.as_ptr(), raw, &raw mut must_mark)
                    }
                };
                NonNull::new(storage).expect("LibGC allocates the cell")
            },
        }
        .cast::<T>();
        // SAFETY: LibGC returned uninitialized storage of at least the size and the alignment of a T, as the caller
        // guarantees, which nothing can collect garbage before it is written.
        unsafe {
            storage.write(cell);
            initialize_tail(storage.as_ptr().cast::<u8>().add(size_of::<T>()));
        }
        let header = storage.cast::<CellHeader>();
        // SAFETY: Every cell starts with its header.
        unsafe {
            debug_assert!((*header.as_ptr()).class.is_subclass_of(T::CLASS));
            (*header.as_ptr()).mark.set(must_mark);
            Gc::from_non_null(storage)
        }
    }

    /// Pops the allocator's local free list, unless LibGC has to collect garbage first or the list is empty, with the
    /// bookkeeping gc_heap_allocator_local_free_list() describes. Cells from the list never need marking.
    ///
    /// # Safety
    ///
    /// The allocator must belong to this heap and hand out cells of `cell_size` bytes.
    #[inline(always)]
    unsafe fn take_from_local_free_list(
        &self,
        allocator: HeapAllocator,
        cell_size: usize,
        kind: AllocationKind,
    ) -> Option<NonNull<c_void>> {
        let local_free_list = allocator.local_free_list()?;
        let info = &self.inline_allocation_info;
        let link_mask = info.free_cell_link_mask;
        // SAFETY: The list and the counters are fields of the live heap, and every cell on the list is a free cell that
        //         keeps the link to the next one at free_cell_next_offset.
        unsafe {
            let cell = local_free_list.read();
            if cell.addr() & link_mask == 0 {
                return None;
            }
            let cell = NonNull::new_unchecked(cell);
            if let AllocationKind::Cell = kind {
                let allocated_bytes = info.allocated_bytes_since_last_gc.read() + cell_size;
                if allocated_bytes > info.gc_bytes_threshold.read() {
                    return None;
                }
                info.allocated_bytes_since_last_gc.write(allocated_bytes);
            }
            info.total_allocated_bytes
                .write(info.total_allocated_bytes.read() + cell_size);
            // NB: Only the bits of the link under the mask count, so that the next cell is in the block of this one
            //     whatever the link was overwritten with.
            let link = cell.byte_add(info.free_cell_next_offset).cast::<usize>().read();
            local_free_list.write(
                cell.as_ptr()
                    .map_addr(|address| (address & !link_mask) | (link & link_mask)),
            );
            Some(cell)
        }
    }

    /// Creates an allocator of cells with `type_info`.
    ///
    /// # Safety
    ///
    /// The type info and the name must outlive the allocator.
    unsafe fn create_allocator(&self, type_info: *const CellTypeInfo, name: &str) -> HeapAllocator {
        // SAFETY: The caller keeps the type info and the name alive for as long as the allocator.
        let raw = unsafe { capi::gc_allocator_create(type_info, name.as_ptr().cast(), name.len()) };
        let raw = NonNull::new(raw).expect("LibGC creates the allocator");
        // SAFETY: The heap and the allocator are live.
        let local_free_list = unsafe { capi::gc_heap_allocator_local_free_list(self.raw.as_ptr(), raw.as_ptr()) };
        HeapAllocator { raw, local_free_list }
    }

    /// The allocator of `class`, a class derived at run time, which the first call for the class creates.
    pub fn runtime_class_allocator(&self, class: &'static Class) -> RuntimeClassAllocator {
        assert!(
            class.is_derived_at_run_time(),
            "the cells of {} come from the allocator of its id",
            class.name
        );
        let allocator = *self
            .runtime_class_allocators
            .borrow_mut()
            .entry(core::ptr::from_ref(class) as usize)
            // SAFETY: The class and its name live as long as the process, so they outlive the allocator.
            .or_insert_with(|| unsafe { self.create_allocator(&raw const class.type_info, class.name) });
        RuntimeClassAllocator { class, allocator }
    }

    /// The size classes of `class`, whose cells have `cell_sizes` bytes, in that order, which the first call for the
    /// class creates. A class has one set of size classes, so every call for it passes the same sizes.
    #[inline(always)]
    pub fn size_classes(&self, class: &'static Class, cell_sizes: &[u32]) -> &[SizeClassAllocator] {
        let table =
            self.size_class_tables[class.id as usize].get_or_init(|| self.create_size_classes(class, cell_sizes));
        debug_assert!(
            table
                .allocators
                .iter()
                .map(SizeClassAllocator::cell_size)
                .eq(cell_sizes.iter().copied())
        );
        &table.allocators
    }

    #[cold]
    fn create_size_classes(&self, class: &'static Class, cell_sizes: &[u32]) -> SizeClassTable {
        assert!(
            !class.is_derived_at_run_time(),
            "a class derived at run time shares the id of {}",
            class.name
        );
        let type_infos: Box<[CellTypeInfo]> = cell_sizes
            .iter()
            .map(|&cell_size| {
                assert!(
                    cell_size >= class.type_info.cell_size && cell_size.is_multiple_of(8),
                    "a size class of {} has cells of {cell_size} bytes",
                    class.name
                );
                CellTypeInfo {
                    cell_size,
                    ..class.type_info
                }
            })
            .collect();
        let allocators = type_infos
            .iter()
            .map(|type_info| SizeClassAllocator {
                class,
                cell_size: type_info.cell_size,
                // SAFETY: The heap destroys the allocator before the type info, and the class name is static.
                allocator: unsafe { self.create_allocator(type_info, class.name) },
            })
            .collect();
        SizeClassTable {
            allocators,
            _type_infos: type_infos,
        }
    }

    #[inline(always)]
    fn allocator_for(&self, class: &'static Class) -> HeapAllocator {
        let slot = &self.allocators[class.id as usize];
        if let Some(allocator) = slot.get() {
            return allocator;
        }
        // SAFETY: The class and its name are static, so they outlive the allocator.
        let allocator = unsafe { self.create_allocator(&raw const class.type_info, class.name) };
        slot.set(Some(allocator));
        allocator
    }

    pub fn collect_garbage(&self) {
        // SAFETY: The heap is live.
        unsafe { capi::gc_heap_collect_garbage(self.raw.as_ptr(), capi::GC_COLLECTION_TYPE_COLLECT_GARBAGE, false) };
    }

    /// Calls `callback` with `context` during every collection, after marking and finalization and before dead cells
    /// are swept, so that weak holders can drop the cells whose mark is clear.
    ///
    /// # Safety
    ///
    /// `context` must stay valid for as long as the heap exists, including while it is being destroyed.
    pub unsafe fn register_sweep_callback(&self, callback: capi::GCCallback, context: *mut c_void) {
        // SAFETY: The heap is live, and the caller keeps the context alive for its lifetime.
        unsafe { capi::gc_heap_register_sweep_callback(self.raw.as_ptr(), callback, context) };
    }

    /// Calls `callback` with `context` once the collection in progress is over, when it is safe to allocate again.
    ///
    /// # Safety
    ///
    /// `context` must stay valid until the callback has run.
    pub unsafe fn enqueue_post_gc_task(&self, callback: capi::GCCallback, context: *mut c_void) {
        // SAFETY: The heap is live, and the caller keeps the context alive until the task runs.
        unsafe { capi::gc_heap_enqueue_post_gc_task(self.raw.as_ptr(), callback, context) };
    }

    /// Counts memory a cell allocated outside the heap towards the next collection, which this may start.
    pub fn did_allocate_external_memory(&self, size: usize) {
        // SAFETY: The heap is live.
        unsafe { capi::gc_heap_did_allocate_external_memory(self.raw.as_ptr(), size) };
    }

    pub fn did_free_external_memory(&self, size: usize) {
        // SAFETY: The heap is live.
        unsafe { capi::gc_heap_did_free_external_memory(self.raw.as_ptr(), size) };
    }

    pub fn account_external_memory_change(&self, old_size: usize, new_size: usize) {
        if new_size > old_size {
            self.did_allocate_external_memory(new_size - old_size);
        } else if old_size > new_size {
            self.did_free_external_memory(old_size - new_size);
        }
    }

    pub fn should_collect_on_every_allocation(&self) -> bool {
        // SAFETY: The heap is live.
        unsafe { capi::gc_heap_should_collect_on_every_allocation(self.raw.as_ptr()) }
    }

    pub fn set_should_collect_on_every_allocation(&self, should_collect: bool) {
        // SAFETY: The heap is live.
        unsafe { capi::gc_heap_set_should_collect_on_every_allocation(self.raw.as_ptr(), should_collect) };
    }

    /// Heap::uproot_cell(): has the next collection leave `cell` out of its roots, so that copies of it on the stack or
    /// in the registers of a running frame no longer keep it alive. A cell that something on the heap still points at
    /// stays alive.
    pub fn uproot_cell<T>(&self, cell: Gc<T>) {
        // SAFETY: The heap is live, and LibGC only keeps the cell's address until the next collection.
        unsafe { capi::gc_heap_uproot_cell(self.raw.as_ptr(), cell.as_ptr().cast()) };
    }

    /// The lowest and highest addresses of the stack of the thread that created the heap.
    pub fn stack_bounds(&self) -> (usize, usize) {
        let (mut base, mut top) = (0, 0);
        // SAFETY: The heap is live.
        unsafe { capi::gc_heap_stack_bounds(self.raw.as_ptr(), &raw mut base, &raw mut top) };
        (base, top)
    }
}

impl Drop for Heap {
    fn drop(&mut self) {
        // SAFETY: This owns the heap. The allocators outlive it, as LibGC requires.
        unsafe {
            capi::gc_heap_destroy(self.raw.as_ptr());
            for allocator in self.allocators.iter().filter_map(Cell::get) {
                capi::gc_allocator_destroy(allocator.raw.as_ptr());
            }
            for allocator in self.runtime_class_allocators.get_mut().values() {
                capi::gc_allocator_destroy(allocator.raw.as_ptr());
            }
            for table in self.size_class_tables.iter().filter_map(OnceCell::get) {
                for size_class in &table.allocators {
                    capi::gc_allocator_destroy(size_class.allocator.raw.as_ptr());
                }
            }
        }
    }
}

/// Whether a cell that something holds without keeping it alive did not survive the collection in progress. Only
/// meaningful in a sweep callback, when the marks are final and dead cells are not swept yet.
pub fn cell_is_dead<T>(cell: Gc<T>) -> bool {
    // SAFETY: Every cell starts with its header, and a weakly held cell is intact until the sweep that follows.
    let header = unsafe { &*cell.as_ptr().cast::<CellHeader>() };
    header.state.get() != CellState::Live || !header.mark.get()
}

/// Checks the layouts this crate mirrors against the LibGC it is linked with, and returns LibGC's layout.
fn check_layout() -> GCLayout {
    let mut layout = GCLayout::default();
    // SAFETY: LibGC fills in the whole struct.
    unsafe { capi::gc_get_layout(&raw mut layout) };
    let expect = |what: &str, actual: u64, expected: u64| {
        assert!(
            actual == expected,
            "LibGC's {what} is {actual}, the Rust runtime expects {expected}"
        );
    };
    expect(
        "cell mark offset",
        layout.cell_mark_offset.into(),
        offset_of!(CellHeader, mark) as u64,
    );
    expect(
        "cell state offset",
        layout.cell_state_offset.into(),
        offset_of!(CellHeader, state) as u64,
    );
    expect(
        "cell kind offset",
        layout.cell_kind_offset.into(),
        offset_of!(CellHeader, kind) as u64,
    );
    expect(
        "cell type info size",
        layout.cell_type_info_size.into(),
        size_of::<CellTypeInfo>() as u64,
    );
    expect(
        "heap region offset mask",
        layout.heap_region_offset_mask,
        HEAP_REGION_OFFSET_MASK,
    );
    expect(
        "primitive storage cage offset mask",
        layout.primitive_storage_cage_offset_mask,
        PRIMITIVE_STORAGE_CAGE_OFFSET_MASK,
    );
    expect(
        "weak impl pointer offset",
        layout.weak_impl_pointer_offset.into(),
        WEAK_IMPL_POINTER_OFFSET as u64,
    );
    layout
}
