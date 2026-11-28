/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Cells whose types Rust defines: the type info LibGC dispatches through for them, and allocating them from LibGC's
//! heap and destroying them, see gc_heap_allocate_cell() in Libraries/LibGC/CAPI.h.

use core::ffi::{c_char, c_void};
use core::ptr::NonNull;

use crate::capi::{self, CellTypeInfo, GCAllocator, GCHeap, GCVisitor};
use crate::visitor::{Trace, Visitor};

/// Mirrors GC::CellKind.
pub const CELL_KIND_OTHER: u8 = 0;

/// Mirrors GC::Cell::State::Live, which a new cell is.
pub const CELL_STATE_LIVE: u8 = 0;

/// A type whose values are cells, which start with the header of a cell, see the layout module.
///
/// # Safety
///
/// The type must be #[repr(C)], start with the header of a cell, and fit in a LibGC cell: at least
/// [`MIN_CELL_SIZE`](crate::layout::MIN_CELL_SIZE) bytes, a multiple of 8, and aligned to at most 16.
pub unsafe trait RustCell: Trace + Sized + 'static {
    /// The name LibGC reports the cells under.
    const NAME: &'static str;
}

/// The visit_edges of the type info of a Rust cell type, which traces the cell.
///
/// # Safety
///
/// Only LibGC calls it, with a live cell of type `T` and a visitor that outlives the call.
pub unsafe extern "C" fn visit_edges<T: Trace + 'static>(cell: *mut c_void, visitor: *mut GCVisitor) {
    // SAFETY: Guaranteed by the caller.
    unsafe { trace::<T>(cell, &mut Visitor::from_raw(visitor)) };
}

/// Traces `cell` with `visitor`, which may be one that is not LibGC's, such as the one of record_cell_slots().
///
/// # Safety
///
/// `cell` must be a live cell of type `T`, which stays where it is while the visitor visits it.
pub unsafe fn trace<T: Trace + 'static>(cell: *const c_void, visitor: &mut Visitor<'_>) {
    // SAFETY: Guaranteed by the caller.
    unsafe { &*cell.cast::<T>() }.trace(visitor);
}

/// The destroy of the type info of a Rust cell type, which drops the cell in place.
///
/// # Safety
///
/// Only LibGC calls it, once for each dead cell of type `T`.
pub unsafe extern "C" fn destroy<T>(cell: *mut c_void) {
    // SAFETY: Guaranteed by the caller.
    unsafe { core::ptr::drop_in_place(cell.cast::<T>()) };
}

unsafe extern "C" fn class_name<T: RustCell>(_cell: *const c_void, length: *mut usize) -> *const c_char {
    // SAFETY: LibGC passes somewhere to store the length.
    unsafe { length.write(T::NAME.len()) };
    T::NAME.as_ptr().cast()
}

/// The type info of the cells of `T`, which LibGC traces with T's Trace and destroys by dropping them in place.
pub const fn type_info_of<T: RustCell>(finalize: Option<unsafe extern "C" fn(cell: *mut c_void)>) -> CellTypeInfo {
    assert!(
        size_of::<T>() >= crate::layout::MIN_CELL_SIZE && size_of::<T>().is_multiple_of(8) && align_of::<T>() <= 16
    );
    CellTypeInfo {
        cell_size: size_of::<T>() as u32,
        alignment: align_of::<T>() as u32,
        kind: CELL_KIND_OTHER,
        visit_edges: visit_edges::<T>,
        finalize,
        destroy: if core::mem::needs_drop::<T>() {
            Some(destroy::<T>)
        } else {
            None
        },
        external_memory_size: None,
        class_name: Some(class_name::<T>),
    }
}

/// A LibGC allocator, which hands out the cells of one type info from the heaps it allocates from.
pub struct Allocator(NonNull<GCAllocator>);

impl Allocator {
    /// An allocator of the cells `type_info` describes, named `name`, whose cells LibGC tells apart from those of other
    /// types by `foreign_type`, see gc_allocator_create().
    ///
    /// # Safety
    ///
    /// The allocator must outlive every heap it allocates from, and `foreign_type` must be unique to the type.
    pub unsafe fn new(type_info: &'static CellTypeInfo, name: &'static str, foreign_type: *const c_void) -> Self {
        // SAFETY: Guaranteed by the caller; the type info and the name live for the rest of the process.
        let raw = unsafe { capi::gc_allocator_create(type_info, name.as_ptr().cast(), name.len(), foreign_type) };
        Self(NonNull::new(raw).expect("LibGC creates the allocator"))
    }

    pub fn as_raw(&self) -> *mut GCAllocator {
        self.0.as_ptr()
    }

    /// Destroys the allocator.
    ///
    /// # Safety
    ///
    /// Every heap the allocator allocated from must have been destroyed.
    pub unsafe fn destroy(self) {
        // SAFETY: Guaranteed by the caller.
        unsafe { capi::gc_allocator_destroy(self.0.as_ptr()) };
    }
}

/// Allocates a cell from `heap` with `allocator`, and moves the value `initialize` makes for the cell's address into it,
/// which lets the fields of a cell hold its own address. The allocation may collect garbage, which the values
/// `initialize` captures must not hold the only reference to a cell of.
///
/// # Safety
///
/// `heap` must be live and used from this thread, and `allocator` must be one of cells of type `T`. `initialize` must
/// neither allocate nor collect garbage, nor read the cell, whose storage is uninitialized until it returns.
#[inline]
pub unsafe fn allocate_with<T: RustCell>(
    heap: *mut GCHeap,
    allocator: &Allocator,
    initialize: impl FnOnce(NonNull<T>) -> T,
) -> NonNull<T> {
    // SAFETY: Guaranteed by the caller. The storage is uninitialized, and fits a T, which is written to it whole before
    //         anything else can collect garbage.
    unsafe {
        let cell = capi::gc_heap_allocate_cell(heap, allocator.as_raw()).cast::<T>();
        let cell = NonNull::new(cell).expect("LibGC allocates the cell");
        cell.write(initialize(cell));
        cell
    }
}

/// The heap the cell at `cell` was allocated in.
///
/// # Safety
///
/// `cell` must be the start of a live cell.
#[inline]
pub unsafe fn heap_of<T>(cell: NonNull<T>) -> *mut GCHeap {
    // SAFETY: Guaranteed by the caller.
    unsafe { capi::gc_cell_heap(cell.as_ptr().cast()) }
}
