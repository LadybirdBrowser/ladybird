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
use crate::layout::buffer::{InterpreterBuffer, ThinSlice};

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
        self.size.get() as usize
    }

    pub fn capacity(&self) -> usize {
        self.capacity.get() as usize
    }

    fn set_size(&self, size: usize) {
        self.size
            .set(u32::try_from(size).expect("the size of a buffer fits in u32"));
    }

    fn set_capacity(&self, capacity: usize) {
        self.capacity
            .set(u32::try_from(capacity).expect("the capacity of a buffer fits in u32"));
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
        self.set_size(size + 1);
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

    fn reallocate(&self, new_capacity: usize) {
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
        if !old_data.is_null() {
            let old_layout = Layout::array::<T>(self.capacity()).expect("the buffer was allocated with this layout");
            // SAFETY: The old storage was allocated by this method with this layout.
            unsafe { dealloc(old_data.cast(), old_layout) };
        }
        self.data.set(new_data);
        self.set_capacity(new_capacity);
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
        let elements = unsafe { ak::slice_from_raw_parts(self.data.get(), self.size()) };
        T::trace_slice(elements, visitor);
    }
}

const THIN_SLICE_HEADER_SIZE: usize = size_of::<usize>();

impl<T> ThinSlice<T> {
    const fn layout_for(length: usize) -> Layout {
        const { assert!(size_of::<T>() != 0 && align_of::<T>() <= THIN_SLICE_HEADER_SIZE) };
        // SAFETY: The size fits in memory, since the caller holds that many elements, and the alignment is a power of
        //         two.
        unsafe {
            Layout::from_size_align_unchecked(THIN_SLICE_HEADER_SIZE + length * size_of::<T>(), THIN_SLICE_HEADER_SIZE)
        }
    }

    pub fn as_slice(&self) -> &[T] {
        if self.data.is_null() {
            return &[];
        }
        // SAFETY: A non-null slice has its length in front of its initialized elements, which it owns.
        unsafe { ak::slice_from_raw_parts(self.data, self.data.cast::<usize>().sub(1).read()) }
    }

    pub fn as_mut_slice(&mut self) -> &mut [T] {
        if self.data.is_null() {
            return &mut [];
        }
        // SAFETY: A non-null slice has its length in front of its initialized elements, which it owns.
        unsafe { ak::slice_from_raw_parts_mut(self.data, self.data.cast::<usize>().sub(1).read()) }
    }
}

impl<T> From<Vec<T>> for ThinSlice<T> {
    fn from(mut elements: Vec<T>) -> Self {
        let length = elements.len();
        if length == 0 {
            return Self::empty();
        }
        let layout = Self::layout_for(length);
        // SAFETY: The layout has a non-zero size. The allocation has room for the length and for `length` elements,
        //         aligned for them, which move out of the vector.
        unsafe {
            let allocation = alloc(layout);
            if allocation.is_null() {
                handle_alloc_error(layout);
            }
            allocation.cast::<usize>().write(length);
            let data = allocation.add(THIN_SLICE_HEADER_SIZE).cast::<T>();
            core::ptr::copy_nonoverlapping(elements.as_ptr(), data, length);
            elements.set_len(0);
            Self::from_data(data)
        }
    }
}

impl<T> FromIterator<T> for ThinSlice<T> {
    fn from_iter<I: IntoIterator<Item = T>>(iterator: I) -> Self {
        iterator.into_iter().collect::<Vec<T>>().into()
    }
}

impl<T: Clone> Clone for ThinSlice<T> {
    fn clone(&self) -> Self {
        self.as_slice().to_vec().into()
    }
}

impl<T> core::ops::Deref for ThinSlice<T> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        self.as_slice()
    }
}

impl<T> core::ops::DerefMut for ThinSlice<T> {
    fn deref_mut(&mut self) -> &mut [T] {
        self.as_mut_slice()
    }
}

impl<'a, T> IntoIterator for &'a ThinSlice<T> {
    type Item = &'a T;
    type IntoIter = core::slice::Iter<'a, T>;

    fn into_iter(self) -> Self::IntoIter {
        self.as_slice().iter()
    }
}

impl<T> Drop for ThinSlice<T> {
    fn drop(&mut self) {
        if self.data.is_null() {
            return;
        }
        let elements = core::ptr::from_mut(self.as_mut_slice());
        let layout = Self::layout_for(elements.len());
        // SAFETY: The elements are initialized and nothing uses them after this. The allocation starts at the length,
        //         and From<Vec<T>> made it with this layout.
        unsafe {
            core::ptr::drop_in_place(elements);
            dealloc(self.data.cast::<u8>().sub(THIN_SLICE_HEADER_SIZE), layout);
        }
    }
}

// SAFETY: Visits every element.
unsafe impl<T: Trace> Trace for ThinSlice<T> {
    fn trace(&self, visitor: &mut Visitor) {
        T::trace_slice(self.as_slice(), visitor);
    }
}
