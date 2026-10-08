/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Growable storage for the buffers a cell owns and the interpreter reads through their data pointer. A buffer
//! allocated through these methods belongs to the cell that holds it, which frees it with clear() in its finalizer. The
//! interpreter may read and write elements at any time, so elements are only ever copied in and out, never borrowed.
//! A buffer can also use storage it does not own, like room in its cell: its owner then makes sure it never grows,
//! shrinks or clears it in place, but moves the elements to storage of its own or forgets the storage instead.

use core::cell::Cell;
use std::alloc::{Layout, alloc, dealloc, handle_alloc_error};

use super::visitor::{Trace, Visitor};
use crate::layout::buffer::InterpreterBuffer;

/// Whether a buffer owns its storage, which it then allocated itself.
#[derive(PartialEq, Eq)]
enum Ownership {
    Owned,
    Unowned,
}

/// Mirrors AK::Vector's growth policy.
fn padded_capacity(capacity: usize) -> usize {
    4 + capacity + capacity / 4
}

impl<T: Copy> InterpreterBuffer<T> {
    pub const fn new() -> Self {
        Self {
            data: Cell::new(core::ptr::null_mut()),
            size: Cell::new(0),
            capacity: Cell::new(0),
        }
    }

    pub fn size(&self) -> usize {
        self.size.get()
    }

    pub fn capacity(&self) -> usize {
        self.capacity.get()
    }

    pub fn is_empty(&self) -> bool {
        self.size() == 0
    }

    pub fn get(&self, index: usize) -> T {
        assert!(
            index < self.size(),
            "index {index} is out of bounds of a buffer of {}",
            self.size()
        );
        // SAFETY: The first `size` elements are initialized.
        unsafe { self.data.get().add(index).read() }
    }

    pub fn set(&self, index: usize, value: T) {
        assert!(
            index < self.size(),
            "index {index} is out of bounds of a buffer of {}",
            self.size()
        );
        // SAFETY: The element is in bounds, and nothing borrows the buffer's elements.
        unsafe { self.data.get().add(index).write(value) };
    }

    pub fn append(&self, value: T) {
        let size = self.size();
        if size == self.capacity() {
            self.reallocate(padded_capacity(size + 1));
        }
        // SAFETY: The buffer has room for one more element.
        unsafe { self.data.get().add(size).write(value) };
        self.size.set(size + 1);
    }

    /// Makes room for `needed_capacity` elements in total, exactly like AK::Vector::ensure_capacity.
    pub fn ensure_capacity(&self, needed_capacity: usize) {
        if needed_capacity > self.capacity() {
            self.reallocate(needed_capacity);
        }
    }

    pub fn shrink_to_fit(&self) {
        if self.size() != self.capacity() {
            self.reallocate(self.size());
        }
    }

    /// Removes every element and frees the storage, like AK::Vector::clear.
    pub fn clear(&self) {
        self.size.set(0);
        self.reallocate(0);
    }

    /// Makes room for `capacity` elements at `data`, which the buffer does not own, the storage of this buffer.
    ///
    /// # Safety
    ///
    /// The buffer must have no storage, and `data` must have room for `capacity` elements for as long as the buffer
    /// uses it.
    pub unsafe fn use_unowned_storage(&self, data: *mut T, capacity: usize) {
        assert!(self.data.get().is_null());
        self.data.set(data);
        self.capacity.set(capacity);
    }

    /// Moves the elements from storage the buffer does not own to storage of its own, with room for at least
    /// `needed_capacity` elements and as much more as append() would make.
    pub fn move_to_owned_storage(&self, needed_capacity: usize) {
        self.reallocate_from(needed_capacity.max(padded_capacity(self.size())), Ownership::Unowned);
    }

    /// Removes every element and forgets storage the buffer does not own.
    pub fn forget_unowned_storage(&self) {
        self.data.set(core::ptr::null_mut());
        self.size.set(0);
        self.capacity.set(0);
    }

    fn reallocate(&self, new_capacity: usize) {
        self.reallocate_from(new_capacity, Ownership::Owned);
    }

    fn reallocate_from(&self, new_capacity: usize, ownership: Ownership) {
        const { assert!(size_of::<T>() != 0) };
        let size = self.size();
        assert!(new_capacity >= size);
        let old_data = self.data.get();
        let new_data = if new_capacity == 0 {
            core::ptr::null_mut()
        } else {
            let layout = Layout::array::<T>(new_capacity).expect("the buffer fits in memory");
            // SAFETY: The layout has a non-zero size.
            let new_data = unsafe { alloc(layout) }.cast::<T>();
            if new_data.is_null() {
                handle_alloc_error(layout);
            }
            if size != 0 {
                // SAFETY: Both allocations hold at least `size` elements and are distinct.
                unsafe { core::ptr::copy_nonoverlapping(old_data, new_data, size) };
            }
            new_data
        };
        if !old_data.is_null() && ownership == Ownership::Owned {
            let old_layout = Layout::array::<T>(self.capacity()).expect("the buffer was allocated with this layout");
            // SAFETY: The old storage was allocated by this method with this layout.
            unsafe { dealloc(old_data.cast(), old_layout) };
        }
        self.data.set(new_data);
        self.capacity.set(new_capacity);
    }
}

impl InterpreterBuffer<u8> {
    pub fn to_vec(&self) -> Vec<u8> {
        (0..self.size()).map(|index| self.get(index)).collect()
    }
}

impl<T: Copy> Default for InterpreterBuffer<T> {
    fn default() -> Self {
        Self::new()
    }
}

// SAFETY: Visits every element.
unsafe impl<T: Copy + Trace> Trace for InterpreterBuffer<T> {
    fn trace(&self, visitor: &mut Visitor) {
        if self.is_empty() {
            return;
        }
        // SAFETY: The first `size` elements are initialized, and nothing writes them while they are visited.
        let elements = unsafe { core::slice::from_raw_parts(self.data.get(), self.size()) };
        T::trace_slice(elements, visitor);
    }
}
