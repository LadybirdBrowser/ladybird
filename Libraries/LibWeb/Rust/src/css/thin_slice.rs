/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A slice held by one pointer, with its length in the word before its first element, which takes
//! no allocation when empty. It suits arrays that are written once and read many times, which need
//! their count but no capacity.

use std::alloc::{Layout, alloc, dealloc, handle_alloc_error, realloc};
use std::ptr::NonNull;

pub(crate) struct ThinSlice<T> {
    /// The first element, after the length; dangling when empty.
    pointer: NonNull<T>,
}

// SAFETY: The slice owns its elements as a `Box<[T]>` would.
unsafe impl<T: Send> Send for ThinSlice<T> {}
unsafe impl<T: Sync> Sync for ThinSlice<T> {}

impl<T> ThinSlice<T> {
    /// The bytes before the first element: the length, padded to the elements' alignment.
    const HEADER: usize = {
        assert!(align_of::<T>() <= align_of::<usize>());
        size_of::<usize>()
    };

    fn layout(length: usize) -> Layout {
        let bytes = length
            .checked_mul(size_of::<T>())
            .and_then(|bytes| bytes.checked_add(Self::HEADER))
            .expect("thin slice too long");
        Layout::from_size_align(bytes, align_of::<usize>()).expect("thin slice too long")
    }

    pub(crate) const fn new() -> Self {
        Self {
            pointer: NonNull::dangling(),
        }
    }

    fn base(&self) -> *mut u8 {
        // SAFETY: Only called on a non-empty slice, whose pointer follows the length.
        unsafe { self.pointer.as_ptr().cast::<u8>().sub(Self::HEADER) }
    }

    pub(crate) fn len(&self) -> usize {
        if self.pointer == NonNull::dangling() {
            return 0;
        }
        // SAFETY: A non-empty slice's length is in the word before its first element.
        unsafe { self.base().cast::<usize>().read() }
    }

    /// Resizes the allocation to hold `length` elements, keeping the first ones.
    ///
    /// # Safety
    /// The elements beyond `length` must already have been moved out or dropped, and the ones
    /// beyond the current length must be written before they are read.
    unsafe fn resize(&mut self, length: usize) {
        let old_length = self.len();
        let base = if length == 0 {
            if old_length != 0 {
                unsafe { dealloc(self.base(), Self::layout(old_length)) };
            }
            self.pointer = NonNull::dangling();
            return;
        } else if old_length == 0 {
            unsafe { alloc(Self::layout(length)) }
        } else {
            unsafe { realloc(self.base(), Self::layout(old_length), Self::layout(length).size()) }
        };
        if base.is_null() {
            handle_alloc_error(Self::layout(length));
        }
        unsafe {
            base.cast::<usize>().write(length);
            self.pointer = NonNull::new_unchecked(base.add(Self::HEADER).cast::<T>());
        }
    }

    pub(crate) fn push(&mut self, value: T) {
        let length = self.len();
        // SAFETY: The new last element is written at once.
        unsafe {
            self.resize(length + 1);
            self.pointer.as_ptr().add(length).write(value);
        }
    }

    pub(crate) fn remove(&mut self, index: usize) -> T {
        let length = self.len();
        assert!(index < length, "thin slice index out of bounds");
        // SAFETY: The removed element is moved out and the ones after it moved down before the
        //         allocation shrinks.
        unsafe {
            let element = self.pointer.as_ptr().add(index);
            let removed = element.read();
            std::ptr::copy(element.add(1), element, length - index - 1);
            self.resize(length - 1);
            removed
        }
    }
}

impl<T> ThinSlice<T> {
    /// The bytes of the allocation, with the length before the elements; none when empty.
    pub(crate) fn heap_bytes(&self) -> usize {
        match self.len() {
            0 => 0,
            length => Self::layout(length).size(),
        }
    }

    pub(crate) fn as_slice(&self) -> &[T] {
        self
    }
}

impl<'a, T> IntoIterator for &'a ThinSlice<T> {
    type Item = &'a T;
    type IntoIter = std::slice::Iter<'a, T>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<T> Default for ThinSlice<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T> std::ops::Deref for ThinSlice<T> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        // SAFETY: The pointer is aligned and, when not dangling, points at `len()` elements.
        unsafe { ak::slice_from_raw_parts(self.pointer.as_ptr(), self.len()) }
    }
}

impl<T> std::ops::DerefMut for ThinSlice<T> {
    fn deref_mut(&mut self) -> &mut [T] {
        // SAFETY: As in `deref`, borrowed exclusively.
        unsafe { ak::slice_from_raw_parts_mut(self.pointer.as_ptr(), self.len()) }
    }
}

impl<T> FromIterator<T> for ThinSlice<T> {
    fn from_iter<I: IntoIterator<Item = T>>(iterator: I) -> Self {
        let values: Vec<T> = iterator.into_iter().collect();
        let mut slice = Self::new();
        // SAFETY: Each of the new elements is written at once, and the vector forgets them.
        unsafe {
            slice.resize(values.len());
            let mut values = std::mem::ManuallyDrop::new(values);
            std::ptr::copy_nonoverlapping(values.as_ptr(), slice.pointer.as_ptr(), values.len());
            values.set_len(0);
            std::mem::ManuallyDrop::drop(&mut values);
        }
        slice
    }
}

impl<T: Clone> Clone for ThinSlice<T> {
    fn clone(&self) -> Self {
        self.iter().cloned().collect()
    }
}

impl<T: PartialEq> PartialEq for ThinSlice<T> {
    fn eq(&self, other: &Self) -> bool {
        self.as_slice() == other.as_slice()
    }
}

impl<T> Drop for ThinSlice<T> {
    fn drop(&mut self) {
        let length = self.len();
        if length == 0 {
            return;
        }
        // SAFETY: The slice owns its elements and the allocation they are in.
        unsafe {
            std::ptr::drop_in_place(std::ptr::slice_from_raw_parts_mut(self.pointer.as_ptr(), length));
            dealloc(self.base(), Self::layout(length));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::ThinSlice;

    #[test]
    fn grows_shrinks_and_drops_its_elements() {
        let mut slice: ThinSlice<String> = ThinSlice::new();
        assert!(slice.is_empty());
        for index in 0..5 {
            slice.push(index.to_string());
        }
        assert_eq!(&*slice, ["0", "1", "2", "3", "4"]);
        assert_eq!(slice.remove(1), "1");
        assert_eq!(&*slice, ["0", "2", "3", "4"]);
        let copy = slice.clone();
        while !slice.is_empty() {
            slice.remove(0);
        }
        assert_eq!(copy.len(), 4);
        let collected: ThinSlice<String> = copy.iter().rev().cloned().collect();
        assert_eq!(&*collected, ["4", "3", "2", "0"]);
        assert_eq!(size_of::<ThinSlice<String>>(), size_of::<usize>());
    }
}
