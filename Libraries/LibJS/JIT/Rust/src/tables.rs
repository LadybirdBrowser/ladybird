/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lists in the tables of a compile.
//!
//! The data of a compile is rows in columns: lists in a zone, with one element for each row. Where a row has a list
//! of something, the elements of the list are next to each other in a column that holds the lists of all the rows,
//! and the row says which part of that column is its own. So a compile makes one list in its zone for all the lists
//! of a kind, however many lists it has.

use core::ops::Range;

use ak::Zone;
use ak::ZoneVec;

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
        // NB: A list of a zone holds fewer than 2^32 elements.
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

/// A list of `T` for each index from 0 on, all in one column, in the order of the indices.
///
/// The lists are written in one of two ways. In the order of their indices: `begin()`, then for each index `push()`
/// for its elements and `end_list()`. Or in any order, when it is known first how long each list is:
/// `begin_counts()`, `count()` for each element, `end_counts()`, `fill()` for each element, `end_fill()`.
pub struct Lists<'w, T: Copy> {
    /// Where the list of each index starts in `items`, and one more: where the last list ends.
    starts: ZoneVec<'w, u32>,
    items: ZoneVec<'w, T>,
}

impl<'w, T: Copy> Lists<'w, T> {
    /// Lists for no index, which take no memory before they have some.
    pub fn new_in(zone: &'w Zone) -> Self {
        Self {
            starts: ZoneVec::new_in(zone),
            items: ZoneVec::new_in(zone),
        }
    }

    /// Lists for no index, with room for the lists of `lists` indices with `items` elements in all.
    pub fn with_capacity_in(lists: usize, items: usize, zone: &'w Zone) -> Self {
        Self {
            starts: ZoneVec::with_capacity_in(lists + 2, zone),
            items: ZoneVec::with_capacity_in(items, zone),
        }
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
    pub fn begin(&mut self) {
        self.starts.clear();
        self.items.clear();
        self.starts.push(0);
    }

    /// Adds an element to the list that is being written: the list of the first index that has none.
    pub fn push(&mut self, item: T) {
        self.items.push(item);
    }

    /// Ends the list that is being written.
    pub fn end_list(&mut self) {
        // NB: A list of a zone holds fewer than 2^32 elements.
        self.starts.push(self.items.len() as u32);
    }

    /// Drops the lists, to write the lists of the indices below `count` in any order.
    pub fn begin_counts(&mut self, count: usize) {
        self.starts.clear();
        self.items.clear();
        // NB: While the lists are counted and filled, the entry of an index is one place further on, so that the
        //     entries are where get() reads them when all the lists are full (see fill()).
        self.starts.resize(count + 2, 0);
    }

    /// Notes that the list of `index` gets one more element.
    pub fn count(&mut self, index: usize) {
        self.starts[index + 2] += 1;
    }

    /// Makes room for the elements that were counted, as copies of `filler`.
    pub fn end_counts(&mut self, filler: T) {
        let mut total = 0_u32;
        for start in self.starts.iter_mut().skip(2) {
            total = total
                .checked_add(*start)
                .expect("a list of a zone holds fewer than 2^32 elements");
            *start = total;
        }
        self.items.resize(total as usize, filler);
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
        let mut lists = Lists::new_in(crate::test_zone());
        assert!(lists.is_empty());
        lists.begin();
        lists.push(5_u32);
        lists.push(6);
        lists.end_list();
        lists.end_list();
        lists.push(7);
        lists.end_list();
        assert_eq!(lists.len(), 3);
        assert_eq!(lists.get(0), [5, 6]);
        assert_eq!(lists.get(1), []);
        assert_eq!(lists.get(2), [7]);
        lists.begin();
        assert!(lists.is_empty());
    }

    #[test]
    fn lists_written_in_any_order() {
        let mut lists = Lists::new_in(crate::test_zone());
        let items = [(2, 'a'), (0, 'b'), (2, 'c'), (3, 'd'), (0, 'e'), (2, 'f')];
        for _ in 0..2 {
            lists.begin_counts(5);
            for (index, _) in items {
                lists.count(index);
            }
            lists.end_counts(' ');
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
    fn lists_grow_past_the_room_they_were_made_with() {
        let mut lists = Lists::with_capacity_in(2, 3, crate::test_zone());
        lists.begin_counts(2);
        for _ in 0..4 {
            lists.count(1);
        }
        lists.end_counts(0_u8);
        for item in 0..4 {
            lists.fill(1, item);
        }
        lists.end_fill();
        assert_eq!(lists.get(1), [0, 1, 2, 3]);
        lists.begin();
        for item in 0..4 {
            lists.push(item);
        }
        lists.end_list();
        lists.end_list();
        lists.end_list();
        assert_eq!(lists.len(), 3);
        assert_eq!(lists.get(0), [0, 1, 2, 3]);
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
