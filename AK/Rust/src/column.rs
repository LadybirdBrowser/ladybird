/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Arrays that grow in place, in memory that comes straight from the system.
//!
//! A `Column` reserves the address space for the most elements it may hold when it gets its first element, and takes
//! memory for the pages it uses as it grows. Its elements never move, so growing copies nothing and leaves nothing
//! behind, and the memory of a column that shrank goes back to the system and not into the free lists of an allocator
//! (see `Column::trim()`). Reserved address space costs nothing until it is used, so a limit can be far above what a
//! column usually holds, and a column that never holds an element costs the system nothing at all.

use core::ffi::CStr;
use core::fmt;
use core::marker::PhantomData;
use core::ops::{Deref, DerefMut};

use crate::region::Region;

/// A column is at its limit, and has no room for what was to be added.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ColumnFull;

/// A growable array of plain values, which stay where they are when it grows.
///
/// It holds at most the number of elements it was made for: the limit is a budget of its owner, not a size it grows
/// to. An owner for which more elements are possible adds them with the `try_` functions and gives up what it does
/// when they fail. A column that is cleared keeps its memory for its next use; `trim()` gives it back.
pub struct Column<T: Copy> {
    region: Region,
    len: usize,
    /// How many elements the memory of the region has room for, and at most `limit`.
    capacity: usize,
    limit: usize,
    elements: PhantomData<T>,
}

impl<T: Copy> Column<T> {
    /// The memory a column takes at least, and at most, when it has to grow.
    const MIN_GROWTH: usize = 64 * 1024;
    const MAX_GROWTH: usize = 16 * 1024 * 1024;

    // NB: A region starts at a multiple of 4096.
    const LAYOUT_IS_SUPPORTED: () = assert!(size_of::<T>() != 0 && align_of::<T>() <= 4096);

    /// A column that holds up to `limit` elements. It takes no memory and no address space before it has elements.
    /// `name` shows up for its memory in /proc/<pid>/smaps on Linux.
    pub fn new(limit: usize, name: &'static CStr) -> Self {
        let () = Self::LAYOUT_IS_SUPPORTED;
        let size = limit
            .checked_mul(size_of::<T>())
            .expect("the limit of a column fits in the address space");
        Self {
            region: Region::new(size, name),
            len: 0,
            capacity: 0,
            limit,
            elements: PhantomData,
        }
    }

    /// The most elements the column can hold.
    pub fn limit(&self) -> usize {
        self.limit
    }

    /// How many bytes of memory the column has taken from the system.
    pub fn memory(&self) -> usize {
        self.region.committed()
    }

    /// Adds an element, if the column is below its limit.
    pub fn try_push(&mut self, value: T) -> Result<(), ColumnFull> {
        if self.len == self.capacity {
            self.grow(1)?;
        }
        // SAFETY: `capacity` is above `len`, so the element is in the memory of the region, which is aligned for it.
        unsafe { self.first().add(self.len).write(value) };
        self.len += 1;
        Ok(())
    }

    /// Adds elements, if that does not take the column past its limit. Adds none of them otherwise.
    pub fn try_extend_from_slice(&mut self, values: &[T]) -> Result<(), ColumnFull> {
        if values.len() > self.capacity - self.len {
            self.grow(values.len())?;
        }
        // SAFETY: The elements are in the memory of the region, as `capacity` is at least `len` plus their count, and
        //         `values` is not part of the column, which is borrowed mutably.
        unsafe { core::ptr::copy_nonoverlapping(values.as_ptr(), self.first().add(self.len), values.len()) };
        self.len += values.len();
        Ok(())
    }

    /// Makes the column `len` elements long, if that is within its limit: drops the elements from index `len` on, or
    /// adds copies of `value`.
    pub fn try_resize(&mut self, len: usize, value: T) -> Result<(), ColumnFull> {
        if len > self.capacity {
            self.grow(len - self.len)?;
        }
        for index in self.len..len {
            // SAFETY: `capacity` is at least `len`, so the element is in the memory of the region, which is aligned
            //         for it.
            unsafe { self.first().add(index).write(value) };
        }
        self.len = len;
        Ok(())
    }

    /// Adds an element. Panics if the column is at its limit.
    pub fn push(&mut self, value: T) {
        if self.try_push(value).is_err() {
            self.panic_at_limit();
        }
    }

    /// Adds elements. Panics if that takes the column past its limit.
    pub fn extend_from_slice(&mut self, values: &[T]) {
        if self.try_extend_from_slice(values).is_err() {
            self.panic_at_limit();
        }
    }

    /// Drops the elements from index `len` on. The column keeps its memory.
    pub fn truncate(&mut self, len: usize) {
        self.len = self.len.min(len);
    }

    /// Drops all elements. The column keeps its memory.
    pub fn clear(&mut self) {
        self.len = 0;
    }

    /// Gives memory back to the system: all that is past the elements and past the room for `keep` elements.
    pub fn trim(&mut self, keep: usize) {
        // NB: Neither count is above `limit`, whose size in bytes new() computed.
        let kept = self.len.max(keep.min(self.limit)) * size_of::<T>();
        self.region.decommit_past(kept);
        self.capacity = (self.region.committed() / size_of::<T>()).min(self.limit);
    }

    fn first(&self) -> *mut T {
        self.region.base().as_ptr().cast()
    }

    #[cold]
    fn panic_at_limit(&self) -> ! {
        panic!("a column holds no more than {} elements", self.limit);
    }

    /// Takes memory for `additional` more elements, if the column can hold that many.
    #[cold]
    fn grow(&mut self, additional: usize) -> Result<(), ColumnFull> {
        let needed = self
            .len
            .checked_add(additional)
            .filter(|needed| *needed <= self.limit)
            .ok_or(ColumnFull)?;
        let step = self.region.committed().clamp(Self::MIN_GROWTH, Self::MAX_GROWTH);
        // NB: Memory that is taken and never written costs nothing on Unix, where a page becomes real when it is
        //     first written, so the steps can be large.
        let size = (needed * size_of::<T>())
            .max(self.region.committed() + step)
            .min(self.region.reserved());
        self.region.commit(size);
        self.capacity = (self.region.committed() / size_of::<T>()).min(self.limit);
        Ok(())
    }
}

impl<T: Copy> Deref for Column<T> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        // SAFETY: The first `len` elements were written, the region keeps their memory for as long as the column
        //         lives, and nothing changes them while the column is borrowed.
        unsafe { crate::slice_from_raw_parts(self.first(), self.len) }
    }
}

impl<T: Copy> DerefMut for Column<T> {
    fn deref_mut(&mut self) -> &mut [T] {
        // SAFETY: As in deref(), and the mutable borrow of the column is the only access to them.
        unsafe { crate::slice_from_raw_parts_mut(self.first(), self.len) }
    }
}

impl<T: Copy + fmt::Debug> fmt::Debug for Column<T> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_list().entries(self.iter()).finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn elements_stay_in_place_while_a_column_grows() {
        let mut column = Column::<u32>::new(1 << 20, c"test");
        assert_eq!(column.memory(), 0);
        assert!(column.is_empty());
        assert!(!column.region.has_address_space());
        column.clear();
        column.trim(0);
        assert!(!column.region.has_address_space());
        column.push(7);
        let first = column.as_ptr();
        for value in 1..100_000 {
            column.push(value);
        }
        assert_eq!(column.as_ptr(), first);
        assert_eq!(column.len(), 100_000);
        assert_eq!(column[0], 7);
        assert!(column[1..].iter().copied().eq(1..100_000));
    }

    #[test]
    fn extends_from_slices() {
        let mut column = Column::<u16>::new(1 << 20, c"test");
        let values: Vec<u16> = (0..50_000).collect();
        column.extend_from_slice(&values[..3]);
        column.extend_from_slice(&[]);
        column.extend_from_slice(&values[3..]);
        column.extend_from_slice(&values);
        assert_eq!(column[..50_000], values[..]);
        assert_eq!(column[50_000..], values[..]);
        column[49_999] = 1;
        assert_eq!(column[49_999], 1);
    }

    #[test]
    fn a_cleared_column_keeps_its_memory_and_a_trimmed_one_gives_it_back() {
        let mut column = Column::<u64>::new(1 << 20, c"test");
        column.extend_from_slice(&vec![1; 100_000]);
        let grown = column.memory();
        assert!(grown >= 800_000);
        column.truncate(10);
        column.truncate(20);
        assert_eq!(column[..], [1; 10]);
        assert_eq!(column.memory(), grown);

        column.trim(1000);
        assert!(column.memory() >= 8000 && column.memory() < grown);
        column.trim(0);
        assert!(column.memory() >= 80 && column.memory() < 8000 + 2 * crate::region::page_size());
        assert_eq!(column[..], [1; 10]);

        // NB: Memory that comes back holds zeros, and the column must not show them as elements.
        column.extend_from_slice(&vec![2; 100_000]);
        assert_eq!(column.len(), 100_010);
        assert_eq!(column[..10], [1; 10]);
        assert!(column[10..].iter().all(|value| *value == 2));

        column.clear();
        assert!(column.is_empty());
        column.trim(0);
        assert_eq!(column.memory(), 0);
        column.push(3);
        assert_eq!(column[..], [3]);
    }

    #[test]
    fn holds_as_many_elements_as_its_limit() {
        let mut column = Column::<u8>::new(3, c"test");
        column.extend_from_slice(&[1, 2]);
        column.push(3);
        assert_eq!(column[..], [1, 2, 3]);
        assert_eq!(column.limit(), 3);
    }

    #[test]
    #[should_panic(expected = "a column holds no more than 3 elements")]
    fn a_push_past_the_limit_panics() {
        let mut column = Column::<u8>::new(3, c"test");
        for value in 0..4 {
            column.push(value);
        }
    }

    #[test]
    #[should_panic(expected = "a column holds no more than 3 elements")]
    fn a_slice_past_the_limit_panics() {
        let mut column = Column::<u64>::new(3, c"test");
        column.push(0);
        column.extend_from_slice(&[1, 2, 3]);
    }

    #[test]
    fn adds_nothing_past_the_limit_when_asked_to_try() {
        let mut column = Column::<u32>::new(4, c"test");
        assert_eq!(column.try_extend_from_slice(&[1, 2, 3]), Ok(()));
        assert_eq!(column.try_extend_from_slice(&[4, 5]), Err(ColumnFull));
        assert_eq!(column.try_resize(5, 0), Err(ColumnFull));
        assert_eq!(column[..], [1, 2, 3]);
        assert_eq!(column.try_push(4), Ok(()));
        assert_eq!(column.try_push(5), Err(ColumnFull));
        assert_eq!(column[..], [1, 2, 3, 4]);
    }

    #[test]
    fn resizes_with_copies_of_a_value() {
        let mut column = Column::<u32>::new(1 << 20, c"test");
        column.push(1);
        assert_eq!(column.try_resize(100_000, 7), Ok(()));
        assert_eq!(column.len(), 100_000);
        assert_eq!(column[0], 1);
        assert!(column[1..].iter().all(|value| *value == 7));
        assert_eq!(column.try_resize(2, 9), Ok(()));
        assert_eq!(column[..], [1, 7]);
        // NB: The elements a resize adds are all new, whatever the memory held before.
        assert_eq!(column.try_resize(4, 9), Ok(()));
        assert_eq!(column[..], [1, 7, 9, 9]);
    }

    #[test]
    fn holds_elements_with_an_alignment() {
        #[derive(Clone, Copy, Debug, PartialEq)]
        #[repr(align(16))]
        struct Wide(u64, u8);

        let mut column = Column::<Wide>::new(10_000, c"test");
        for index in 0..10_000 {
            column.push(Wide(index, 1));
        }
        assert_eq!(column[9_999], Wide(9_999, 1));
        assert!(column.as_ptr().is_aligned());
    }

    #[test]
    fn moves_to_another_thread() {
        let mut column = Column::<u32>::new(1024, c"test");
        column.push(1);
        let column = std::thread::spawn(move || {
            column.push(2);
            column
        })
        .join()
        .unwrap();
        assert_eq!(column[..], [1, 2]);
    }
}
