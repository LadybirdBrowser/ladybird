/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lists of rows of many types in one block of memory that comes straight from the system.
//!
//! A `RowBlock` holds lists, one after the other, in a `Column` of words. A list is rows of one plain type that are
//! next to each other. Adding a list gives a `RowList`, which is small and plain itself, so that rows can hold the
//! lists of other rows. The block gives the rows of a list back as a slice, with no decoding. A data structure of
//! many lists is one piece of memory this way: it is written from start to end, it is read where it is, and all of
//! it goes back to the system at one time.

use core::ffi::CStr;
use core::fmt;
use core::marker::PhantomData;
use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicU32, Ordering};

use crate::column::Column;

/// A block has no room for a list, or cannot hold lists at all (see `RowBlock::can_hold_lists()`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RowBlockFull;

/// A list of rows of type `T` in a `RowBlock`. The block that the list was added to gives its rows.
pub struct RowList<T> {
    /// The index of the word of the block at which the first row starts.
    start: u32,
    len: u32,
    /// The number of the block that holds the rows, or 0 for a list with no rows.
    block: u32,
    // NB: Invariant in `T`: copies of a list write and read the same rows, so a list must not change its type.
    rows: PhantomData<fn(T) -> T>,
}

impl<T> RowList<T> {
    /// A list with no rows. All blocks have it.
    pub const fn empty() -> Self {
        Self {
            start: 0,
            len: 0,
            block: 0,
            rows: PhantomData,
        }
    }

    pub fn len(self) -> usize {
        self.len as usize
    }

    pub fn is_empty(self) -> bool {
        self.len == 0
    }
}

impl<T> Clone for RowList<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Copy for RowList<T> {}

impl<T> Default for RowList<T> {
    fn default() -> Self {
        Self::empty()
    }
}

// NB: Lists are equal if they are the same rows of the same block.
impl<T> PartialEq for RowList<T> {
    fn eq(&self, other: &Self) -> bool {
        (self.start, self.len, self.block) == (other.start, other.len, other.block)
    }
}

impl<T> Eq for RowList<T> {}

impl<T> fmt::Debug for RowList<T> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "RowList({} rows at word {})", self.len, self.start)
    }
}

/// The number that the next block gets. Numbers start at 1 and are never used again.
static NEXT_BLOCK: AtomicU32 = AtomicU32::new(1);

/// A number that no block has had, or 0 when all the numbers are used.
fn new_block_number() -> u32 {
    NEXT_BLOCK
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |number| number.checked_add(1))
        .unwrap_or(0)
}

/// Lists of rows of many types, one after the other, in memory that stays where it is.
///
/// A block that is cleared keeps its memory for the lists that are added next; `trim()` gives memory back. The lists
/// of a block are only good for that block, and only until it is cleared: the block panics when it is asked for the
/// rows of another list.
pub struct RowBlock {
    words: Column<MaybeUninit<u64>>,
    /// Which block this is since it was made or cleared, or 0 for a block that cannot hold lists. The lists of the
    /// block have the number, and no other list has it.
    number: u32,
}

impl RowBlock {
    /// A block that holds up to `limit` bytes of rows. It takes no memory and no address space before it has a list.
    /// `name` shows up for its memory in /proc/<pid>/smaps on Linux.
    pub fn new(limit: usize, name: &'static CStr) -> Self {
        Self {
            words: Column::new((limit / size_of::<u64>()).min(u32::MAX as usize), name),
            number: new_block_number(),
        }
    }

    /// Whether lists can be added. A process can make or clear a block about four thousand million times. After
    /// that, blocks hold no lists.
    pub fn can_hold_lists(&self) -> bool {
        self.number != 0
    }

    /// How many bytes of memory the block has taken from the system.
    pub fn memory(&self) -> usize {
        self.words.memory()
    }

    /// How many bytes the lists of the block fill.
    pub fn bytes(&self) -> usize {
        self.words.len() * size_of::<u64>()
    }

    /// Drops all lists. The block keeps its memory.
    pub fn clear(&mut self) {
        self.words.clear();
        // NB: This makes the lists that were added so far lists of no block.
        self.number = new_block_number();
    }

    /// Gives memory back to the system: all that is past the lists and past the room for `keep` bytes.
    pub fn trim(&mut self, keep: usize) {
        self.words.trim(keep / size_of::<u64>());
    }

    /// Adds `rows` as a list, if the block has room for them.
    pub fn try_add<T: Copy + Send + Sync + 'static>(&mut self, rows: &[T]) -> Result<RowList<T>, RowBlockFull> {
        const {
            // NB: The rows of a list start at a word.
            assert!(size_of::<T>() != 0 && align_of::<T>() <= align_of::<u64>());
        }
        if rows.is_empty() {
            return Ok(RowList::empty());
        }
        if self.number == 0 {
            return Err(RowBlockFull);
        }
        let len = u32::try_from(rows.len()).map_err(|_| RowBlockFull)?;
        let start = self.words.len();
        let end = start + size_of_val(rows).div_ceil(size_of::<u64>());
        self.words
            .try_resize(end, MaybeUninit::uninit())
            .map_err(|_| RowBlockFull)?;
        let words = &mut self.words[start..end];
        // SAFETY: `words` has room for the bytes of `rows`, and `rows` is not part of the block, which is borrowed
        //         mutably. The words may hold any bytes, also the padding of the rows.
        unsafe {
            core::ptr::copy_nonoverlapping(
                rows.as_ptr().cast::<u8>(),
                words.as_mut_ptr().cast::<u8>(),
                size_of_val(rows),
            );
        }
        Ok(RowList {
            // NB: new() keeps the count of words in 32 bits.
            start: start as u32,
            len,
            block: self.number,
            rows: PhantomData,
        })
    }

    /// The words that hold the rows of `list`. Panics if the list is not a list of this block.
    fn words_of<T>(&self, list: RowList<T>) -> core::ops::Range<usize> {
        assert!(list.block == self.number, "the list is a list of this block");
        let start = list.start as usize;
        start..start + (list.len as usize * size_of::<T>()).div_ceil(size_of::<u64>())
    }

    /// The rows of `list`. Panics if the list is not a list of this block.
    pub fn rows<T: Copy + Send + Sync + 'static>(&self, list: RowList<T>) -> &[T] {
        if list.is_empty() {
            return &[];
        }
        let words = &self.words[self.words_of(list)];
        // SAFETY: A list with rows has the number of the block only if try_add::<T>() of this block returned it, or a
        //         copy of it, since the block was last cleared: the fields of a list are private, and no two blocks
        //         have the same number between two clears. So `words` starts with `len` rows of type `T` that
        //         try_add() copied there, at an address that is aligned for `T`, and only a mutable borrow of the
        //         block changes them.
        unsafe { crate::slice_from_raw_parts(words.as_ptr().cast::<T>(), list.len as usize) }
    }

    /// The rows of `list`, to change them. Panics if the list is not a list of this block.
    pub fn rows_mut<T: Copy + Send + Sync + 'static>(&mut self, list: RowList<T>) -> &mut [T] {
        if list.is_empty() {
            return &mut [];
        }
        let range = self.words_of(list);
        let words = &mut self.words[range];
        // SAFETY: As in rows(), and the mutable borrow of the block is the only access to the rows. Rows are plain
        //         values, so each value that is written there is a row of type `T` again.
        unsafe { crate::slice_from_raw_parts_mut(words.as_mut_ptr().cast::<T>(), list.len as usize) }
    }
}

impl fmt::Debug for RowBlock {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "RowBlock({} bytes)", self.bytes())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Debug, Clone, Copy, PartialEq)]
    struct Row {
        flag: bool,
        value: Option<u32>,
        inner: RowList<u16>,
    }

    #[test]
    fn lists_of_many_types_share_a_block() {
        let mut block = RowBlock::new(1 << 20, c"test");
        assert_eq!(block.memory(), 0);
        let bytes = block.try_add(&[1u8, 2, 3]).unwrap();
        let halves = block.try_add(&[500u16, 600]).unwrap();
        let rows = block
            .try_add(&[
                Row {
                    flag: true,
                    value: None,
                    inner: halves,
                },
                Row {
                    flag: false,
                    value: Some(9),
                    inner: RowList::empty(),
                },
            ])
            .unwrap();
        let none = block.try_add::<u64>(&[]).unwrap();
        assert_eq!(block.rows(bytes), [1, 2, 3]);
        assert_eq!(block.rows(none), []);
        assert_eq!(block.rows(rows).len(), 2);
        let first = block.rows(rows)[0];
        assert!(first.flag);
        assert_eq!(block.rows(first.inner), [500, 600]);
        block.rows_mut(rows)[1].value = Some(10);
        block.rows_mut(halves)[0] = 501;
        assert_eq!(block.rows(rows)[1].value, Some(10));
        assert_eq!(block.rows(halves), [501, 600]);
        assert_eq!(block.rows(bytes), [1, 2, 3]);
    }

    #[test]
    fn rows_stay_in_place_while_a_block_grows() {
        let mut block = RowBlock::new(1 << 24, c"test");
        let first = block.try_add(&[7u64]).unwrap();
        let address = block.rows(first).as_ptr();
        let values: Vec<u32> = (0..100_000).collect();
        let lists: Vec<_> = (0..8).map(|_| block.try_add(&values).unwrap()).collect();
        assert_eq!(block.rows(first).as_ptr(), address);
        assert!(lists.iter().all(|list| block.rows(*list) == values));
        assert!(block.bytes() >= 8 * 400_000);
    }

    #[test]
    fn a_full_block_takes_no_list() {
        let mut block = RowBlock::new(64, c"test");
        let list = block.try_add(&[1u64; 8]).unwrap();
        assert_eq!(block.try_add(&[1u8]), Err(RowBlockFull));
        assert_eq!(block.try_add::<u8>(&[]), Ok(RowList::empty()));
        assert_eq!(block.rows(list), [1; 8]);
    }

    #[test]
    #[should_panic(expected = "the list is a list of this block")]
    fn a_block_gives_no_rows_for_a_list_of_another_block() {
        let mut block = RowBlock::new(1 << 12, c"test");
        let mut other = RowBlock::new(1 << 12, c"test");
        let list = block.try_add(&[true, false]).unwrap();
        other.try_add(&[7u8, 7]).unwrap();
        let _ = other.rows(list);
    }

    #[test]
    #[should_panic(expected = "the list is a list of this block")]
    fn a_cleared_block_gives_no_rows_for_its_old_lists() {
        let mut block = RowBlock::new(1 << 12, c"test");
        let list = block.try_add(&[true, false]).unwrap();
        block.clear();
        block.try_add(&[7u8, 7]).unwrap();
        let _ = block.rows(list);
    }

    #[test]
    fn a_cleared_block_keeps_its_memory_until_it_is_trimmed() {
        let mut block = RowBlock::new(1 << 24, c"test");
        block.try_add(&[0u8; 300_000]).unwrap();
        let memory = block.memory();
        assert!(memory >= 300_000);
        block.clear();
        assert_eq!(block.bytes(), 0);
        assert_eq!(block.memory(), memory);
        block.trim(0);
        assert_eq!(block.memory(), 0);
        let list = block.try_add(&[5u32]).unwrap();
        assert_eq!(block.rows(list), [5]);
    }
}
