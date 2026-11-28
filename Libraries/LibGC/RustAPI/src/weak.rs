/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Weak references to cells: LibGC's GC::WeakImpl, which holds the address of its cell until the cell is collected,
//! and which a collection that moves the cell points at its new address.

use core::ffi::c_void;
use core::ptr::NonNull;

use crate::capi::{self, GCWeakImpl};
use crate::layout::WEAK_IMPL_POINTER_OFFSET;

/// A reference to a GC::WeakImpl, which the value owns.
#[repr(transparent)]
pub struct WeakImpl(NonNull<GCWeakImpl>);

impl WeakImpl {
    /// A weak reference to `cell`, in the heap the cell was allocated in.
    ///
    /// # Safety
    ///
    /// `cell` must be the start of a live cell, and the call made on the thread that owns its heap.
    #[inline]
    pub unsafe fn for_cell(cell: NonNull<c_void>) -> Self {
        // SAFETY: Guaranteed by the caller. The impl comes with a reference this value owns.
        Self(
            NonNull::new(unsafe { capi::gc_cell_create_weak_impl(cell.as_ptr()) })
                .expect("LibGC creates the weak impl"),
        )
    }

    /// A weak reference to `cell`, in `heap`.
    ///
    /// # Safety
    ///
    /// `heap` must be live, `cell` the start of a live cell of it, and the call made on the thread that owns it.
    #[inline]
    pub unsafe fn new(heap: *mut capi::GCHeap, cell: NonNull<c_void>) -> Self {
        // SAFETY: Guaranteed by the caller. The impl comes with a reference this value owns.
        Self(
            NonNull::new(unsafe { capi::gc_heap_create_weak_impl(heap, cell.as_ptr()) })
                .expect("LibGC creates the weak impl"),
        )
    }

    /// The shared weak reference to no cell.
    #[inline]
    pub fn null() -> Self {
        // NB: Returns the shared null impl with a reference this value owns.
        Self(NonNull::new(capi::gc_weak_impl_null()).expect("LibGC has a null weak impl"))
    }

    /// The start of the cell, or none once it was collected.
    #[inline(always)]
    pub fn cell(&self) -> Option<NonNull<c_void>> {
        // SAFETY: The impl stays allocated while this value holds a reference to it.
        unsafe { Self::cell_of_raw(self.0) }
    }

    /// The start of the cell of the GC::WeakImpl at `raw`, or none once it was collected.
    ///
    /// # Safety
    ///
    /// `raw` must be a live GC::WeakImpl, such as one a C++ GC::Weak holds.
    #[inline(always)]
    pub unsafe fn cell_of_raw(raw: NonNull<GCWeakImpl>) -> Option<NonNull<c_void>> {
        // SAFETY: Guaranteed by the caller.
        NonNull::new(unsafe {
            raw.as_ptr()
                .byte_add(WEAK_IMPL_POINTER_OFFSET)
                .cast::<*mut c_void>()
                .read()
        })
    }

    /// Gives up the reference for a pointer that from_raw() can take back.
    #[inline(always)]
    pub fn into_raw(self) -> NonNull<GCWeakImpl> {
        core::mem::ManuallyDrop::new(self).0
    }

    /// # Safety
    ///
    /// `raw` must have come from into_raw(), and must not be taken back any other time.
    #[inline(always)]
    pub unsafe fn from_raw(raw: NonNull<GCWeakImpl>) -> Self {
        Self(raw)
    }
}

impl Clone for WeakImpl {
    #[inline]
    fn clone(&self) -> Self {
        // SAFETY: The impl is live; the clone owns the new reference.
        unsafe { capi::gc_weak_impl_ref(self.0.as_ptr()) };
        Self(self.0)
    }
}

impl Drop for WeakImpl {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: This value owns one reference.
        unsafe { capi::gc_weak_impl_unref(self.0.as_ptr()) };
    }
}
