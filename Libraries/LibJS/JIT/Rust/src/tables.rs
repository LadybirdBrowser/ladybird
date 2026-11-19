/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lists in the tables of a compile.
//!
//! The data of a compile is rows in columns (see `ak::Column`). Where a row has a list of something, the elements of
//! the list are next to each other in a column that holds the lists of all the rows, and the row says which part of
//! that column is its own. So a compile makes no block of the heap for a list, however many lists it has.

use core::ffi::CStr;
use core::ops::Range;

use ak::Column;
use ak::ColumnFull;

/// The elements of one list: a part of a column that holds many lists.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Run {
    start: u32,
    len: u32,
}

impl Run {
    /// In place of the elements of a list that is not written yet. It is a part of no column.
    pub const NOT_WRITTEN: Run = Run {
        start: u32::MAX,
        len: 0,
    };

    /// The elements from index `start` to index `end` of a column, without the one at `end`.
    pub fn new(start: usize, end: usize) -> Self {
        // NB: The columns of a compile hold fewer elements than this (see their limits).
        let start = u32::try_from(start).expect("the columns of a compile have 32-bit indices");
        let end = u32::try_from(end).expect("the columns of a compile have 32-bit indices");
        Self {
            start,
            len: end - start,
        }
    }

    /// Where the elements are in their column.
    pub fn range(self) -> Range<usize> {
        self.start as usize..self.start as usize + self.len as usize
    }

    pub fn len(self) -> usize {
        self.len as usize
    }

    pub fn is_empty(self) -> bool {
        self.len == 0
    }
}

/// Drops the elements of `column` and gives its memory back to the system.
pub fn release<T: Copy>(column: &mut Column<T>) {
    column.clear();
    column.trim(0);
}

/// Makes `column` hold `len` copies of `value` and nothing else.
pub fn reset<T: Copy>(column: &mut Column<T>, len: usize, value: T) -> Result<(), ColumnFull> {
    column.clear();
    column.try_resize(len, value)
}

/// Takes the last element out of `column`, if it has one.
pub fn pop<T: Copy>(column: &mut Column<T>) -> Option<T> {
    let last = column.last().copied()?;
    column.truncate(column.len() - 1);
    Some(last)
}

/// A list of `T` for each index from 0 on, all in one column, in the order of the indices.
///
/// The lists are written in one of two ways. In the order of their indices: `begin()`, then for each index
/// `try_push()` for its elements and `end_list()`. Or in any order, when it is known first how long each list is:
/// `begin_counts()`, `count()` for each element, `end_counts()`, `fill()` for each element, `end_fill()`.
pub struct Lists<T: Copy> {
    /// Where the list of each index starts in `items`, and one more: where the last list ends.
    starts: Column<u32>,
    items: Column<T>,
}

impl<T: Copy> Lists<T> {
    /// Lists for up to `max_lists` indices, with up to `max_items` elements in all. They take no memory before they
    /// have elements. `name` shows up for their memory in /proc/<pid>/smaps on Linux.
    pub fn new(max_lists: usize, max_items: usize, name: &'static CStr) -> Self {
        Self {
            starts: Column::new(max_lists + 2, name),
            items: Column::new(max_items.min(u32::MAX as usize), name),
        }
    }

    /// Drops the lists and gives their memory back to the system.
    pub fn release_memory(&mut self) {
        release(&mut self.starts);
        release(&mut self.items);
    }

    /// How many indices have a list.
    pub fn len(&self) -> usize {
        self.starts.len().saturating_sub(1)
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// The list of `index`.
    pub fn get(&self, index: usize) -> &[T] {
        &self.items[self.starts[index] as usize..self.starts[index + 1] as usize]
    }

    /// Drops the lists, to write new ones in the order of their indices.
    pub fn begin(&mut self) -> Result<(), ColumnFull> {
        self.starts.clear();
        self.items.clear();
        self.starts.try_push(0)
    }

    /// Adds an element to the list that is being written: the list of the first index that has none.
    pub fn try_push(&mut self, item: T) -> Result<(), ColumnFull> {
        self.items.try_push(item)
    }

    /// Ends the list that is being written.
    pub fn end_list(&mut self) -> Result<(), ColumnFull> {
        // NB: new() keeps the count of elements in 32 bits.
        self.starts.try_push(self.items.len() as u32)
    }

    /// Drops the lists, to write the lists of the indices below `count` in any order.
    pub fn begin_counts(&mut self, count: usize) -> Result<(), ColumnFull> {
        self.starts.clear();
        self.items.clear();
        // NB: While the lists are counted and filled, the entry of an index is one place further on, so that the
        //     entries are where get() reads them when all the lists are full (see fill()).
        self.starts.try_resize(count + 2, 0)
    }

    /// Notes that the list of `index` gets one more element.
    pub fn count(&mut self, index: usize) {
        self.starts[index + 2] += 1;
    }

    /// Makes room for the elements that were counted, as copies of `filler`.
    pub fn end_counts(&mut self, filler: T) -> Result<(), ColumnFull> {
        let mut total = 0_u64;
        for start in self.starts.iter_mut().skip(2) {
            total += u64::from(*start);
            *start = u32::try_from(total).map_err(|_| ColumnFull)?;
        }
        self.items.try_resize(total as usize, filler)
    }

    /// Puts `item` in the next place of the list of `index`, which has a place left.
    pub fn fill(&mut self, index: usize, item: T) {
        let place = self.starts[index + 1];
        self.items[place as usize] = item;
        self.starts[index + 1] = place + 1;
    }

    /// Ends the writing of lists that were counted, when all of them are full.
    pub fn end_fill(&mut self) {
        let count = self.starts.len() - 1;
        debug_assert!(self.starts[count - 1] == self.starts[count]);
        self.starts.truncate(count);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lists_written_in_the_order_of_their_indices() {
        let mut lists = Lists::new(8, 64, c"test");
        assert!(lists.is_empty());
        lists.begin().unwrap();
        lists.try_push(5_u32).unwrap();
        lists.try_push(6).unwrap();
        lists.end_list().unwrap();
        lists.end_list().unwrap();
        lists.try_push(7).unwrap();
        lists.end_list().unwrap();
        assert_eq!(lists.len(), 3);
        assert_eq!(lists.get(0), [5, 6]);
        assert_eq!(lists.get(1), []);
        assert_eq!(lists.get(2), [7]);
        lists.begin().unwrap();
        assert!(lists.is_empty());
    }

    #[test]
    fn lists_written_in_any_order() {
        let mut lists = Lists::new(8, 64, c"test");
        let items = [(2, 'a'), (0, 'b'), (2, 'c'), (3, 'd'), (0, 'e'), (2, 'f')];
        for _ in 0..2 {
            lists.begin_counts(5).unwrap();
            for (index, _) in items {
                lists.count(index);
            }
            lists.end_counts(' ').unwrap();
            for (index, item) in items {
                lists.fill(index, item);
            }
            lists.end_fill();
            assert_eq!(lists.len(), 5);
            assert_eq!(lists.get(0), ['b', 'e']);
            assert_eq!(lists.get(1), []);
            assert_eq!(lists.get(2), ['a', 'c', 'f']);
            assert_eq!(lists.get(3), ['d']);
            assert_eq!(lists.get(4), []);
        }
    }

    #[test]
    fn lists_with_no_room_fail() {
        let mut lists = Lists::new(2, 3, c"test");
        lists.begin_counts(2).unwrap();
        for _ in 0..4 {
            lists.count(1);
        }
        assert_eq!(lists.end_counts(0_u8), Err(ColumnFull));
        assert_eq!(lists.begin_counts(3), Err(ColumnFull));
        lists.begin().unwrap();
        for item in 0..3 {
            lists.try_push(item).unwrap();
        }
        assert_eq!(lists.try_push(3), Err(ColumnFull));
    }

    #[test]
    fn columns_are_reset_and_popped() {
        let mut column = Column::new(8, c"test");
        reset(&mut column, 3, 7_u8).unwrap();
        assert_eq!(*column, [7, 7, 7]);
        column[2] = 9;
        assert_eq!(pop(&mut column), Some(9));
        reset(&mut column, 1, 1).unwrap();
        assert_eq!(pop(&mut column), Some(1));
        assert_eq!(pop(&mut column), None);
        assert_eq!(reset(&mut column, 9, 0), Err(ColumnFull));
        column.push(5);
        release(&mut column);
        assert!(column.is_empty());
        assert_eq!(column.memory(), 0);
    }

    #[test]
    fn a_run_is_a_part_of_a_column() {
        let run = Run::new(3, 7);
        assert_eq!(run.range(), 3..7);
        assert_eq!(run.len(), 4);
        assert!(!run.is_empty());
        assert!(Run::default().is_empty());
        assert!(Run::new(9, 9).is_empty());
    }
}
