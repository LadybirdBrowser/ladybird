/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::marker::PhantomData;
use core::ptr::NonNull;

use super::capi::{self, GCWeakImpl};
use super::heap::Heap;
use super::lint_markers::HoldsNoCells;
use super::visitor::{Trace, Visitor};
use crate::layout::cell::Gc;

/// Mirrors the part of GC::WeakImpl the runtime reads: the pointer at this offset, which LibGC clears once the cell is
/// collected. The startup layout check compares the offset with the one gc_get_layout() reports.
pub(super) const WEAK_IMPL_POINTER_OFFSET: usize = 16;

/// A type whose GC pointers do not keep their cells alive: wherever a value of it is, the pointers to cells that a
/// collection frees are cleared before anything reads them. Ladybird's Rust GC lints accept such values in a Box or a
/// Vec.
///
/// # Safety
///
/// Every value of the type must have its pointers to dead cells cleared after each collection.
pub unsafe trait SweptAfterCollection {}

/// A reference to a cell that does not keep it alive.
pub struct GcWeak<T> {
    weak_impl: NonNull<GCWeakImpl>,
    // NB: A GcWeak gives a Gc<T> out, but holds none.
    _cell: PhantomData<fn() -> Gc<T>>,
}

impl<T> GcWeak<T> {
    pub fn new(heap: &Heap, cell: Gc<T>) -> Self {
        // SAFETY: The heap and the cell are live. The impl comes with a reference this value owns.
        let weak_impl = unsafe { capi::gc_heap_create_weak_impl(heap.raw(), cell.as_ptr().cast()) };
        Self {
            weak_impl: NonNull::new(weak_impl).expect("LibGC creates the weak impl"),
            _cell: PhantomData,
        }
    }

    pub fn null() -> Self {
        // NB: Returns the shared null impl with a reference this value owns.
        let weak_impl = capi::gc_weak_impl_null();
        Self {
            weak_impl: NonNull::new(weak_impl).expect("LibGC has a null weak impl"),
            _cell: PhantomData,
        }
    }

    pub fn get(&self) -> Option<Gc<T>> {
        // SAFETY: The impl stays allocated while this value holds a reference to it.
        let pointer = unsafe {
            self.weak_impl
                .as_ptr()
                .cast::<u8>()
                .add(WEAK_IMPL_POINTER_OFFSET)
                .cast::<*mut T>()
                .read()
        };
        // SAFETY: A non-null pointer is a cell that has not been collected.
        NonNull::new(pointer).map(|pointer| unsafe { Gc::from_non_null(pointer) })
    }
}

impl<T> Clone for GcWeak<T> {
    fn clone(&self) -> Self {
        // SAFETY: The impl is live; the clone owns the new reference.
        unsafe { capi::gc_weak_impl_ref(self.weak_impl.as_ptr()) };
        Self {
            weak_impl: self.weak_impl,
            _cell: PhantomData,
        }
    }
}

impl<T> Drop for GcWeak<T> {
    fn drop(&mut self) {
        // SAFETY: This value owns one reference.
        unsafe { capi::gc_weak_impl_unref(self.weak_impl.as_ptr()) };
    }
}

// SAFETY: A weak reference keeps nothing alive.
unsafe impl<T> Trace for GcWeak<T> {
    fn trace(&self, _: &mut Visitor) {}
}

// SAFETY: A weak reference holds LibGC's weak impl of its cell, not the cell.
unsafe impl<T> HoldsNoCells for GcWeak<T> {}
