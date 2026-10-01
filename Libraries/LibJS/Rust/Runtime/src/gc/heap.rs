/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ffi::c_void;
use core::mem::offset_of;
use core::ptr::NonNull;

use super::capi::{self, GCAllocator, GCGatherRootsCallback, GCHeap, GCLayout};
use super::class::{CellTypeInfo, Class, GcCell};
use super::class_id::CLASS_COUNT;
use crate::build_configuration::{HEAP_REGION_OFFSET_MASK, PRIMITIVE_STORAGE_CAGE_OFFSET_MASK};
use crate::layout::cell::{CellHeader, CellState, Gc};

/// A LibGC heap, with one allocator per class of cell.
pub struct Heap {
    raw: NonNull<GCHeap>,
    allocators: [Cell<*mut GCAllocator>; CLASS_COUNT],
}

impl Heap {
    /// Creates a heap that calls `gather_roots` with `context` whenever it gathers its roots.
    ///
    /// # Safety
    ///
    /// `context` must stay valid for as long as the heap exists, including while it is being destroyed.
    pub unsafe fn new(gather_roots: GCGatherRootsCallback, context: *mut c_void) -> Self {
        check_layout();
        // SAFETY: The caller keeps the context alive for the heap's lifetime.
        let raw = unsafe { capi::gc_heap_create(gather_roots, context, false) };
        Self {
            raw: NonNull::new(raw).expect("LibGC creates the heap"),
            allocators: [const { Cell::new(core::ptr::null_mut()) }; CLASS_COUNT],
        }
    }

    pub fn raw(&self) -> *mut GCHeap {
        self.raw.as_ptr()
    }

    /// Moves `cell` into the heap. Nothing can collect garbage between taking the storage and writing the cell, so
    /// the collector never sees a partially written cell.
    pub fn allocate<T: GcCell>(&self, cell: T) -> Gc<T> {
        let allocator = self.allocator_for(T::CLASS);
        let mut must_mark = false;
        // SAFETY: The heap and the allocator are live.
        let storage =
            unsafe { capi::gc_heap_allocate_cell(self.raw.as_ptr(), allocator, &raw mut must_mark) }.cast::<T>();
        let storage = NonNull::new(storage).expect("LibGC allocates the cell");
        // SAFETY: LibGC returned uninitialized storage of the class's size and alignment.
        unsafe { storage.write(cell) };
        let header = storage.cast::<CellHeader>();
        // SAFETY: Every cell starts with its header.
        unsafe {
            debug_assert!(core::ptr::eq((*header.as_ptr()).class, T::CLASS));
            (*header.as_ptr()).mark.set(must_mark);
            Gc::from_non_null(storage)
        }
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

    pub fn set_should_collect_on_every_allocation(&self, should_collect: bool) {
        // SAFETY: The heap is live.
        unsafe { capi::gc_heap_set_should_collect_on_every_allocation(self.raw.as_ptr(), should_collect) };
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
}
