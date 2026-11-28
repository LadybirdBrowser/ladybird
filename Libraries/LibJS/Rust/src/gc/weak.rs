/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::marker::PhantomData;
use core::ptr::NonNull;

use libgc_api::WeakImpl;

use super::heap::Heap;
use super::lint_markers::HoldsNoCells;
use super::visitor::{Trace, Visitor};
use crate::layout::cell::Gc;

/// A type whose GC pointers do not keep their cells alive: wherever a value of it is, the pointers to cells that a
/// collection frees are cleared before anything reads them. Ladybird's Rust GC lints accept such values in a Box or a
/// Vec.
///
/// # Safety
///
/// Every value of the type must have its pointers to dead cells cleared after each collection.
pub unsafe trait SweptAfterCollection {}

/// A reference to a cell that does not keep it alive.
#[repr(transparent)]
pub struct GcWeak<T> {
    weak_impl: WeakImpl,
    // NB: A GcWeak gives a Gc<T> out, but holds none.
    _cell: PhantomData<fn() -> Gc<T>>,
}

impl<T> GcWeak<T> {
    pub fn new(heap: &Heap, cell: Gc<T>) -> Self {
        Self {
            // SAFETY: The heap and the cell are live.
            weak_impl: unsafe { WeakImpl::new(heap.raw(), cell.as_non_null().cast()) },
            _cell: PhantomData,
        }
    }

    pub fn null() -> Self {
        Self {
            weak_impl: WeakImpl::null(),
            _cell: PhantomData,
        }
    }

    /// Gives up the reference for a pointer that from_raw() can take back, which is at least 2-aligned.
    pub fn into_raw(self) -> *mut () {
        self.weak_impl.into_raw().as_ptr().cast()
    }

    /// # Safety
    ///
    /// The pointer must have come from into_raw() on a weak reference of the same type, and that reference must not be
    /// taken back any other time.
    pub unsafe fn from_raw(raw: *mut ()) -> Self {
        Self {
            // SAFETY: into_raw() gives out the non-null pointer of the impl.
            weak_impl: unsafe { WeakImpl::from_raw(NonNull::new_unchecked(raw.cast())) },
            _cell: PhantomData,
        }
    }

    pub fn get(&self) -> Option<Gc<T>> {
        // SAFETY: A cell that has not been collected is a live T.
        self.weak_impl
            .cell()
            .map(|cell| unsafe { Gc::from_non_null(cell.cast()) })
    }
}

impl<T> Clone for GcWeak<T> {
    fn clone(&self) -> Self {
        Self {
            weak_impl: self.weak_impl.clone(),
            _cell: PhantomData,
        }
    }
}

// SAFETY: A weak reference keeps nothing alive.
unsafe impl<T> Trace for GcWeak<T> {
    fn trace(&self, _: &mut Visitor) {}
}

// SAFETY: A weak reference holds LibGC's weak impl of its cell, not the cell.
unsafe impl<T> HoldsNoCells for GcWeak<T> {}
