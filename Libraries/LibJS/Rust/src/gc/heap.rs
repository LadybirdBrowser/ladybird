/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, RefCell};
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
pub struct Heap {
    raw: NonNull<GCHeap>,
    allocators: [Cell<*mut GCAllocator>; CLASS_COUNT],
    /// The allocators of the classes derived at run time, by the address of their class.
    runtime_class_allocators: RefCell<HashMap<usize, NonNull<GCAllocator>, foldhash::fast::RandomState>>,
    /// The allocators of size classes, by the address of their class and their cell size, each with the type info its
    /// blocks dispatch through, which has to outlive the allocator.
    size_class_allocators: RefCell<SizeClassAllocators>,
}

type SizeClassAllocators =
    HashMap<(usize, u32), (NonNull<GCAllocator>, Box<CellTypeInfo>), foldhash::fast::RandomState>;

/// The allocator of a class derived at run time, which only the cells of that class come from, as each C++ class that
/// declares GC_DECLARE_ALLOCATOR has one of its own. It belongs to the heap that created it and lives as long as it.
#[derive(Clone, Copy)]
pub struct RuntimeClassAllocator {
    class: &'static Class,
    raw: NonNull<GCAllocator>,
}

impl RuntimeClassAllocator {
    pub fn class(&self) -> &'static Class {
        self.class
    }
}

impl PartialEq for RuntimeClassAllocator {
    fn eq(&self, other: &Self) -> bool {
        self.raw == other.raw
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
    raw: NonNull<GCAllocator>,
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
        self.raw == other.raw
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
        check_layout();
        // SAFETY: The caller keeps the context alive for the heap's lifetime.
        let raw = unsafe { capi::gc_heap_create(gather_roots, context, become_process_default) };
        Self {
            raw: NonNull::new(raw).expect("LibGC creates the heap"),
            allocators: [const { Cell::new(core::ptr::null_mut()) }; CLASS_COUNT],
            runtime_class_allocators: RefCell::default(),
            size_class_allocators: RefCell::default(),
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
        unsafe { self.allocate_with(allocator, AllocationKind::Cell, cell, |_| {}) }
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
        unsafe { self.allocate_with(allocator.raw.as_ptr(), AllocationKind::Cell, cell, |_| {}) }
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
        unsafe { self.allocate_with(size_class.raw.as_ptr(), kind, cell, initialize_tail) }
    }

    /// # Safety
    ///
    /// The allocator must belong to this heap and hand out storage of at least the size and the alignment of a T, and
    /// `initialize_tail` must write every byte beyond the T, which it gets the address of.
    #[inline(always)]
    unsafe fn allocate_with<T: GcCell>(
        &self,
        allocator: *mut GCAllocator,
        kind: AllocationKind,
        cell: T,
        initialize_tail: impl FnOnce(*mut u8),
    ) -> Gc<T> {
        let mut must_mark = false;
        // SAFETY: The heap and the allocator are live.
        let storage = unsafe {
            match kind {
                AllocationKind::Cell => capi::gc_heap_allocate_cell(self.raw.as_ptr(), allocator, &raw mut must_mark),
                AllocationKind::Storage => {
                    capi::gc_heap_allocate_storage_cell(self.raw.as_ptr(), allocator, &raw mut must_mark)
                }
            }
        }
        .cast::<T>();
        let storage = NonNull::new(storage).expect("LibGC allocates the cell");
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

    /// The allocator of `class`, a class derived at run time, which the first call for the class creates.
    pub fn runtime_class_allocator(&self, class: &'static Class) -> RuntimeClassAllocator {
        assert!(
            class.is_derived_at_run_time(),
            "the cells of {} come from the allocator of its id",
            class.name
        );
        let raw = *self
            .runtime_class_allocators
            .borrow_mut()
            .entry(core::ptr::from_ref(class) as usize)
            .or_insert_with(|| {
                // SAFETY: The class and its name live as long as the process, so they outlive the allocator.
                let allocator = unsafe {
                    capi::gc_allocator_create(
                        core::ptr::from_ref(&class.type_info),
                        class.name.as_ptr().cast(),
                        class.name.len(),
                    )
                };
                NonNull::new(allocator).expect("LibGC creates the allocator")
            });
        RuntimeClassAllocator { class, raw }
    }

    /// The size class of `class` whose cells have `cell_size` bytes, which the first call for the class and the size
    /// creates.
    pub fn size_class_allocator(&self, class: &'static Class, cell_size: u32) -> SizeClassAllocator {
        assert!(
            cell_size >= class.type_info.cell_size && cell_size.is_multiple_of(8),
            "a size class of {} has cells of {cell_size} bytes",
            class.name
        );
        let raw = self
            .size_class_allocators
            .borrow_mut()
            .entry((core::ptr::from_ref(class) as usize, cell_size))
            .or_insert_with(|| {
                let type_info = Box::new(CellTypeInfo {
                    cell_size,
                    ..class.type_info
                });
                // SAFETY: The heap destroys the allocator before the type info, and the class name is static.
                let allocator = unsafe {
                    capi::gc_allocator_create(&raw const *type_info, class.name.as_ptr().cast(), class.name.len())
                };
                (NonNull::new(allocator).expect("LibGC creates the allocator"), type_info)
            })
            .0;
        SizeClassAllocator { class, cell_size, raw }
    }

    fn allocator_for(&self, class: &'static Class) -> *mut GCAllocator {
        let slot = &self.allocators[class.id as usize];
        if slot.get().is_null() {
            // SAFETY: The class and its name are static, so they outlive the allocator.
            let allocator = unsafe {
                capi::gc_allocator_create(
                    core::ptr::from_ref(&class.type_info),
                    class.name.as_ptr().cast(),
                    class.name.len(),
                )
            };
            slot.set(allocator);
        }
        slot.get()
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
            for allocator in &self.allocators {
                if !allocator.get().is_null() {
                    capi::gc_allocator_destroy(allocator.get());
                }
            }
            for allocator in self.runtime_class_allocators.get_mut().values() {
                capi::gc_allocator_destroy(allocator.as_ptr());
            }
            for (allocator, _) in self.size_class_allocators.get_mut().values() {
                capi::gc_allocator_destroy(allocator.as_ptr());
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

/// Checks the layouts this crate mirrors against the LibGC it is linked with.
fn check_layout() {
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
}
