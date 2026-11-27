/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Ref, RefCell, RefMut};
use core::ops::{Deref, DerefMut};

use super::defer_gc::DeferGc;
use super::sweep::Sweep;
use super::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;

/// Interior mutability for the compound state of a cell, which a collection reads. A mutable borrow defers collections
/// for as long as it lives, so a collection never finds the state borrowed: allocating meanwhile only grows the heap.
pub struct GcRefCell<T>(RefCell<T>);

impl<T> GcRefCell<T> {
    pub const fn new(value: T) -> Self {
        Self(RefCell::new(value))
    }

    pub fn borrow(&self) -> Ref<'_, T> {
        self.0.borrow()
    }

    pub fn as_ptr(&self) -> *mut T {
        self.0.as_ptr()
    }

    pub fn borrow_mut<'a>(&'a self, vm: &'a Vm) -> GcRefMut<'a, T> {
        let defer_gc = DeferGc::new(vm);
        GcRefMut {
            borrow: self.0.borrow_mut(),
            _defer_gc: defer_gc,
        }
    }

    /// Borrows the state mutably from the sweep callback of a collection, which nothing collects during.
    pub(crate) fn borrow_mut_in_sweep(&self, _: &Sweep<'_>) -> RefMut<'_, T> {
        self.0.borrow_mut()
    }

    pub fn replace(&self, value: T) -> T {
        self.0.replace(value)
    }

    pub fn swap(&self, other: &Self) {
        self.0.swap(&other.0);
    }
}

impl<T: Default> Default for GcRefCell<T> {
    fn default() -> Self {
        Self::new(T::default())
    }
}

// SAFETY: Traces the current value.
unsafe impl<T: Trace> Trace for GcRefCell<T> {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        // SAFETY: Nothing writes the value while a collection traces it.
        unsafe { self.borrow_for_trace() }.trace(visitor);
    }
}

impl<T> GcRefCell<T> {
    /// The value, for a trace() that visits it in a way of its own. It is not borrowed, so the borrow state does not
    /// keep code from writing it.
    ///
    /// # Safety
    ///
    /// Nothing may borrow the value mutably, replace it or write it while the reference lives. This holds during a
    /// trace, as no code runs while a collection marks.
    pub(crate) unsafe fn borrow_for_trace(&self) -> &T {
        // NB: Marking may trace the cell on a thread other than the one that owns the heap, so this only reads the
        //     borrow state, rather than borrow the value.
        // SAFETY: The thread that owns the heap is stopped while its collection marks, and it holds no mutable borrow,
        //         which this checks, so nothing changes the value while it is traced.
        let Ok(value) = (unsafe { self.0.try_borrow_unguarded() }) else {
            eprintln!("libjs_rust: garbage collection while a GcRefCell is mutably borrowed");
            std::process::abort();
        };
        value
    }
}

/// The value of `cell`, for a trace() that visits the slots in it: like GcRefCell::borrow_for_trace(), for a RefCell
/// that only the thread that owns the heap uses, which is stopped while its collection marks.
///
/// # Safety
///
/// As for GcRefCell::borrow_for_trace().
pub unsafe fn ref_cell_for_trace<T>(cell: &core::cell::RefCell<T>) -> &T {
    // SAFETY: See GcRefCell::borrow_for_trace().
    let Ok(value) = (unsafe { cell.try_borrow_unguarded() }) else {
        eprintln!("libjs_rust: garbage collection while a RefCell it traces is mutably borrowed");
        std::process::abort();
    };
    value
}

/// A mutable borrow of the state in a GcRefCell, with the deferral of collections that it holds until it ends.
pub struct GcRefMut<'a, T: ?Sized> {
    // NB: Fields drop in order, so the borrow ends before the collection it may have deferred runs.
    borrow: RefMut<'a, T>,
    _defer_gc: DeferGc<'a>,
}

impl<'a, T: ?Sized> GcRefMut<'a, T> {
    /// Borrows a part of the state, as RefMut::map() does.
    pub fn map<U: ?Sized>(borrow: Self, part: impl FnOnce(&mut T) -> &mut U) -> GcRefMut<'a, U> {
        GcRefMut {
            borrow: RefMut::map(borrow.borrow, part),
            _defer_gc: borrow._defer_gc,
        }
    }
}

impl<T: ?Sized> Deref for GcRefMut<'_, T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.borrow
    }
}

impl<T: ?Sized> DerefMut for GcRefMut<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.borrow
    }
}
