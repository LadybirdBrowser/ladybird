/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Slices that share one block of memory.

use std::alloc::{Layout, alloc, dealloc, handle_alloc_error};

use crate::layout::buffer::BlockSlice;

pub(crate) const THIN_SLICE_HEADER_SIZE: usize = size_of::<usize>();

/// The memory of slices that get made together and go away together, which takes one trip to the allocator instead of
/// one for each. The size of the block is in a word in front of the slices.
pub struct SliceBlock {
    memory: *mut u8,
}

// SAFETY: A block is memory that nothing else knows of, and it does nothing with the slices in it.
unsafe impl Send for SliceBlock {}
// SAFETY: As above, and a shared block has no operations.
unsafe impl Sync for SliceBlock {}

impl SliceBlock {
    pub const fn empty() -> Self {
        Self {
            memory: core::ptr::null_mut(),
        }
    }

    /// How much of a block a slice of `length` elements takes.
    pub const fn room_for<T>(length: usize) -> usize {
        const { assert!(size_of::<T>() != 0 && align_of::<T>() <= THIN_SLICE_HEADER_SIZE) };
        if length == 0 {
            return 0;
        }
        assert!(length <= isize::MAX as usize / size_of::<T>(), "a slice fits in memory");
        THIN_SLICE_HEADER_SIZE + (length * size_of::<T>()).next_multiple_of(THIN_SLICE_HEADER_SIZE)
    }

    const fn layout_for(size: usize) -> Layout {
        // SAFETY: The size fits in memory, since it is the sum of what slices take, and the alignment is a power of two.
        unsafe { Layout::from_size_align_unchecked(THIN_SLICE_HEADER_SIZE + size, THIN_SLICE_HEADER_SIZE) }
    }

    /// A block with `size` bytes for slices to be put in.
    ///
    /// # Safety
    ///
    /// Slices do not keep their block alive, so every slice put in this one has to go away before it does.
    pub unsafe fn builder(size: usize) -> SliceBlockBuilder {
        if size == 0 {
            return SliceBlockBuilder {
                block: Self::empty(),
                free: core::ptr::null_mut(),
                remaining: 0,
            };
        }
        let layout = Self::layout_for(size);
        // SAFETY: The layout has a non-zero size, with room for the size of the block in front of the slices.
        unsafe {
            let memory = alloc(layout);
            if memory.is_null() {
                handle_alloc_error(layout);
            }
            memory.cast::<usize>().write(size);
            SliceBlockBuilder {
                block: Self { memory },
                free: memory.add(THIN_SLICE_HEADER_SIZE),
                remaining: size,
            }
        }
    }
}

impl Default for SliceBlock {
    fn default() -> Self {
        Self::empty()
    }
}

impl Drop for SliceBlock {
    fn drop(&mut self) {
        if self.memory.is_null() {
            return;
        }
        // SAFETY: The size is where builder() put it, and that made the allocation with this layout.
        unsafe {
            let layout = Self::layout_for(self.memory.cast::<usize>().read());
            dealloc(self.memory, layout);
        }
    }
}

/// A block on its way, which slices get put into one at a time.
pub struct SliceBlockBuilder {
    block: SliceBlock,
    free: *mut u8,
    remaining: usize,
}

impl SliceBlockBuilder {
    /// Moves `elements` into the block. It has to have been made with room for them.
    pub fn slice<T>(&mut self, elements: impl ExactSizeIterator<Item = T>) -> BlockSlice<T> {
        self.slice_of_length(elements.len(), elements)
    }

    /// Moves the `length` elements that `elements` yields into the block.
    pub fn slice_of_length<T>(&mut self, length: usize, elements: impl Iterator<Item = T>) -> BlockSlice<T> {
        let data = self.claim::<T>(length);
        let mut written = 0;
        for element in elements.take(length) {
            // SAFETY: There is room for `length` elements.
            unsafe { data.add(written).write(element) };
            written += 1;
        }
        assert_eq!(written, length, "an iterator yields as much as it says");
        BlockSlice::from_data(data)
    }

    /// Copies two slices into the block, one after the other, as one.
    pub fn slice_from_parts<T: Copy>(&mut self, first: &[T], second: &[T]) -> BlockSlice<T> {
        let data = self.claim::<T>(first.len() + second.len());
        // SAFETY: There is room for both, and nothing else knows of that memory.
        unsafe {
            core::ptr::copy_nonoverlapping(first.as_ptr(), data, first.len());
            core::ptr::copy_nonoverlapping(second.as_ptr(), data.add(first.len()), second.len());
        }
        BlockSlice::from_data(data)
    }

    /// Room for `length` elements with their length in front of them, or null if that is none.
    fn claim<T>(&mut self, length: usize) -> *mut T {
        let room = SliceBlock::room_for::<T>(length);
        assert!(room <= self.remaining, "a block has room for what goes in it");
        if length == 0 {
            return core::ptr::null_mut();
        }
        // SAFETY: There is room, aligned for the length and for the elements, since everything in a block takes a
        //         multiple of the size of the length.
        unsafe {
            self.free.cast::<usize>().write(length);
            let data = self.free.add(THIN_SLICE_HEADER_SIZE).cast::<T>();
            self.free = self.free.add(room);
            self.remaining -= room;
            data
        }
    }

    /// The block, for as long as the slices in it live.
    pub fn finish(self) -> SliceBlock {
        self.block
    }
}

// SAFETY: A slice owns its elements, like a Box<[T]>.
unsafe impl<T: Send> Send for BlockSlice<T> {}
// SAFETY: As above.
unsafe impl<T: Sync> Sync for BlockSlice<T> {}

impl<T> BlockSlice<T> {
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

    /// Moves the elements out and leaves the slice empty. Elements the caller does not get to stay undropped.
    pub fn take_elements(&mut self) -> impl ExactSizeIterator<Item = T> {
        let length = self.len();
        let data = core::mem::replace(&mut self.data, core::ptr::null_mut());
        // SAFETY: The slice let go of the elements, and each gets read once.
        (0..length).map(move |index| unsafe { data.add(index).read() })
    }
}

impl<T> core::ops::Deref for BlockSlice<T> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        self.as_slice()
    }
}

impl<T> core::ops::DerefMut for BlockSlice<T> {
    fn deref_mut(&mut self) -> &mut [T] {
        self.as_mut_slice()
    }
}

impl<'a, T> IntoIterator for &'a BlockSlice<T> {
    type Item = &'a T;
    type IntoIter = core::slice::Iter<'a, T>;

    fn into_iter(self) -> Self::IntoIter {
        self.as_slice().iter()
    }
}

impl<T> Drop for BlockSlice<T> {
    fn drop(&mut self) {
        if core::mem::needs_drop::<T>() {
            let elements = core::ptr::from_mut(self.as_mut_slice());
            // SAFETY: The elements are initialized and nothing uses them after this.
            unsafe { core::ptr::drop_in_place(elements) };
        }
    }
}
