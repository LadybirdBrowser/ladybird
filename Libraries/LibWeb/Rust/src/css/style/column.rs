/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::RefCell;
use std::ops::{Deref, DerefMut};

use super::capacity::ShallowCapacityBytes;

/// A directly indexed column whose missing rows all have the same value.
///
/// NB: The default vacant value stays a type rather than a stored pointer. A column grows to the
///     highest identity written, so a column that a single high identity widens fills far more
///     rows than it will ever hold, and filling those through a pointer costs one indirect call
///     per row where the compiler can otherwise emit a block store.
#[derive(Clone)]
pub(super) struct Column<T> {
    entries: Vec<T>,
    vacant: Option<fn() -> T>,
}

impl<T: Default> Default for Column<T> {
    fn default() -> Self {
        Self {
            entries: Vec::new(),
            vacant: None,
        }
    }
}

impl<T: Default> Column<T> {
    pub(super) fn new(vacant: fn() -> T) -> Self {
        Self {
            entries: Vec::new(),
            vacant: Some(vacant),
        }
    }

    /// Ensure that `index` is addressable and return newly allocated bytes.
    pub(super) fn ensure(&mut self, index: usize) -> u64 {
        if self.entries.len() > index {
            return 0;
        }
        let capacity_before = self.entries.capacity();
        let length = index.checked_add(1).expect("column identity space exhausted");
        match self.vacant {
            Some(vacant) => self.entries.resize_with(length, vacant),
            None => self.entries.resize_with(length, T::default),
        }
        ((self.entries.capacity() - capacity_before) * size_of::<T>()) as u64
    }

    /// Replace one row and return newly allocated bytes.
    pub(super) fn insert(&mut self, index: usize, value: T) -> u64 {
        let growth = self.ensure(index);
        self.entries[index] = value;
        growth
    }

    /// Return one mutable row, growing the column when it is absent.
    pub(super) fn entry(&mut self, index: usize) -> &mut T {
        self.ensure(index);
        &mut self.entries[index]
    }
}

impl<T> Deref for Column<T> {
    type Target = Vec<T>;

    fn deref(&self) -> &Self::Target {
        &self.entries
    }
}

impl<T> DerefMut for Column<T> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.entries
    }
}

impl<T> IntoIterator for Column<T> {
    type Item = T;
    type IntoIter = std::vec::IntoIter<T>;

    fn into_iter(self) -> Self::IntoIter {
        self.entries.into_iter()
    }
}

impl<T> ShallowCapacityBytes for Column<T> {
    fn shallow_capacity_bytes(&self) -> u64 {
        self.entries.shallow_capacity_bytes()
    }
}

/// A directly indexed side table whose rows are invalidated by advancing a stamp instead of being
/// cleared, and whose storage is borrowed from a thread-local pool instead of allocated.
///
/// NB: A side table that one transaction fills and the next discards has two tempting shapes, and
///     both charge for something other than the batch. A hash map pays one probe per access and
///     rebuilds its table as the batch grows. A freshly allocated dense column pays to fill the
///     identity space between the rows it holds, which one high identity widens across the whole
///     element space. Keeping the storage between transactions removes both: growth is paid once
///     at the high-water mark, reset is one increment, and a row is one array read.
#[derive(Default)]
pub(super) struct StampedIndex {
    rows: Vec<StampedRow>,
    stamp: u32,
}

#[derive(Clone, Copy, Default)]
struct StampedRow {
    /// The stamp this row was written under. Zero is never a live stamp, so rows grown into
    /// existence and rows left by an earlier transaction both read as absent.
    stamp: u32,
    value: u32,
}

impl StampedIndex {
    /// Abandon every row written under the previous stamp.
    fn begin(&mut self) {
        match self.stamp.checked_add(1) {
            Some(stamp) => self.stamp = stamp,
            // The stamp space is exhausted only after four billion transactions on one buffer.
            // Dropping the rows makes every later row start from the unwritten stamp again.
            None => {
                self.rows.clear();
                self.stamp = 1;
            }
        }
    }

    fn get(&self, index: usize) -> Option<u32> {
        self.rows
            .get(index)
            .filter(|row| row.stamp == self.stamp)
            .map(|row| row.value)
    }

    fn insert(&mut self, index: usize, value: u32) {
        if self.rows.len() <= index {
            self.rows.resize(index + 1, StampedRow::default());
        }
        self.rows[index] = StampedRow {
            stamp: self.stamp,
            value,
        };
    }

    fn capacity_bytes(&self) -> u64 {
        self.rows.shallow_capacity_bytes()
    }
}

thread_local! {
    static STAMPED_INDEX_POOL: RefCell<Vec<StampedIndex>> = const { RefCell::new(Vec::new()) };
}

/// How many buffers one thread keeps between transactions. Concurrently live effect tables are
/// the contexts a single matching pass has open, not the nodes it visits.
///
/// NB: A buffer in the pool is memory the process holds while no owner leases it. These bounds
///     are what keep that off-book residency small: at most this many buffers, and at most this
///     many bytes across all of them, since a buffer grows to the highest element identity it
///     was ever asked for.
const STAMPED_INDEX_POOL_LIMIT: usize = 8;
const STAMPED_INDEX_POOL_BYTE_LIMIT: u64 = 4 << 20;

/// A [`StampedIndex`] taken from the thread's pool on first write and returned when dropped.
///
/// NB: Construction must stay free. Contexts that own one of these are created and discarded
///     around individual nodes, and most of them never record a row.
#[derive(Default)]
pub(super) struct PooledStampedIndex(Option<StampedIndex>);

impl PooledStampedIndex {
    pub(super) fn get(&self, index: usize) -> Option<u32> {
        self.0.as_ref()?.get(index)
    }

    pub(super) fn insert(&mut self, index: usize, value: u32) {
        self.0
            .get_or_insert_with(|| {
                let mut borrowed = STAMPED_INDEX_POOL
                    .try_with(|pool| pool.borrow_mut().pop())
                    .ok()
                    .flatten()
                    .unwrap_or_default();
                borrowed.begin();
                borrowed
            })
            .insert(index, value);
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        self.0.as_ref().map_or(0, StampedIndex::capacity_bytes)
    }
}

impl Drop for PooledStampedIndex {
    fn drop(&mut self) {
        let Some(borrowed) = self.0.take() else {
            return;
        };
        let _ = STAMPED_INDEX_POOL.try_with(|pool| {
            let mut pool = pool.borrow_mut();
            let pooled_bytes: u64 = pool.iter().map(StampedIndex::capacity_bytes).sum();
            if pool.len() < STAMPED_INDEX_POOL_LIMIT
                && pooled_bytes + borrowed.capacity_bytes() <= STAMPED_INDEX_POOL_BYTE_LIMIT
            {
                pool.push(borrowed);
            }
        });
    }
}

/// A directly indexed packed boolean column.
#[derive(Clone, Default)]
pub(super) struct BitColumn {
    words: Vec<u64>,
}

impl PartialEq for BitColumn {
    fn eq(&self, other: &Self) -> bool {
        let common_length = self.words.len().min(other.words.len());
        self.words[..common_length] == other.words[..common_length]
            && self.words[common_length..].iter().all(|word| *word == 0)
            && other.words[common_length..].iter().all(|word| *word == 0)
    }
}

impl Eq for BitColumn {}

impl ShallowCapacityBytes for BitColumn {
    fn shallow_capacity_bytes(&self) -> u64 {
        self.capacity_bytes()
    }
}

impl BitColumn {
    /// Set one bit and return whether its value changed and newly allocated bytes.
    pub(super) fn set(&mut self, index: usize, value: bool) -> (bool, u64) {
        let word_index = index / u64::BITS as usize;
        let capacity_before = self.words.capacity();
        if self.words.len() <= word_index {
            if !value {
                return (false, 0);
            }
            self.words.resize(word_index + 1, 0);
        }
        let growth = ((self.words.capacity() - capacity_before) * size_of::<u64>()) as u64;
        let mask = 1_u64 << (index % u64::BITS as usize);
        let previous = self.words[word_index] & mask != 0;
        if value {
            self.words[word_index] |= mask;
        } else {
            self.words[word_index] &= !mask;
        }
        (previous != value, growth)
    }

    pub(super) fn contains(&self, index: usize) -> bool {
        let word = index / u64::BITS as usize;
        let mask = 1_u64 << (index % u64::BITS as usize);
        self.words.get(word).is_some_and(|word| word & mask != 0)
    }

    pub(super) fn clear(&mut self) {
        self.words.fill(0);
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        self.words.shallow_capacity_bytes()
    }
}

/// One specialized page stored by a [`PagedColumn`].
pub(super) trait PagedColumnPage: Default {
    type Value: Copy;

    const SHIFT: usize;

    fn get(&self, index: usize) -> Option<Self::Value>;
    fn insert(&mut self, index: usize, value: Self::Value);
}

pub(super) trait RemovablePagedColumnPage: PagedColumnPage {
    fn remove(&mut self, index: usize) -> Option<Self::Value>;
}

pub(super) const PAGED_VALUE_PAGE_SHIFT: usize = 6;
const PAGED_VALUE_PAGE_SIZE: usize = 1 << PAGED_VALUE_PAGE_SHIFT;

#[derive(Clone)]
pub(super) struct PagedValuePage<T: Copy + Default> {
    known: u64,
    values: [T; PAGED_VALUE_PAGE_SIZE],
}

impl<T: Copy + Default> Default for PagedValuePage<T> {
    fn default() -> Self {
        Self {
            known: 0,
            values: [T::default(); PAGED_VALUE_PAGE_SIZE],
        }
    }
}

impl<T: Copy + Default> PagedColumnPage for PagedValuePage<T> {
    type Value = T;

    const SHIFT: usize = PAGED_VALUE_PAGE_SHIFT;

    fn get(&self, index: usize) -> Option<T> {
        (self.known & (1 << index) != 0).then_some(self.values[index])
    }

    fn insert(&mut self, index: usize, value: T) {
        self.known |= 1 << index;
        self.values[index] = value;
    }
}

/// A sparse column whose directory and page accounting are shared across page layouts.
#[derive(Clone)]
pub(super) struct PagedColumn<P: PagedColumnPage> {
    pages: Vec<Option<Box<P>>>,
    page_count: usize,
}

impl<P: PagedColumnPage> Default for PagedColumn<P> {
    fn default() -> Self {
        Self {
            pages: Vec::new(),
            page_count: 0,
        }
    }
}

impl<P: PagedColumnPage> PagedColumn<P> {
    pub(super) fn get(&self, index: usize) -> Option<P::Value> {
        let page = self.pages.get(index >> P::SHIFT)?.as_ref()?;
        page.get(index & ((1 << P::SHIFT) - 1))
    }

    /// Publish one value and return the previous value and whether this allocated a page.
    #[inline]
    pub(super) fn insert(&mut self, index: usize, value: P::Value) -> (Option<P::Value>, bool) {
        let (page, page_was_absent) = self.page_mut_or_default(index);
        let index = index & ((1 << P::SHIFT) - 1);
        let previous = page.get(index);
        page.insert(index, value);
        (previous, page_was_absent)
    }

    pub(super) fn page_mut_or_default(&mut self, index: usize) -> (&mut P, bool) {
        let page_index = index >> P::SHIFT;
        if self.pages.len() <= page_index {
            self.pages.resize_with(page_index + 1, || None);
        }
        let page_was_absent = self.pages[page_index].is_none();
        let page = self.pages[page_index].get_or_insert_with(Box::default);
        self.page_count += usize::from(page_was_absent);
        (page, page_was_absent)
    }

    pub(super) fn pages_mut(&mut self) -> impl Iterator<Item = &mut P> {
        self.pages.iter_mut().flatten().map(Box::as_mut)
    }

    #[cfg(test)]
    pub(super) fn page_count(&self) -> usize {
        self.page_count
    }

    #[cfg(test)]
    pub(super) fn directory_capacity(&self) -> usize {
        self.pages.capacity()
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        self.pages.shallow_capacity_bytes() + (self.page_count * size_of::<P>()) as u64
    }
}

impl<P: PagedColumnPage> ShallowCapacityBytes for PagedColumn<P> {
    fn shallow_capacity_bytes(&self) -> u64 {
        self.capacity_bytes()
    }
}

impl<P: RemovablePagedColumnPage> PagedColumn<P> {
    pub(super) fn remove(&mut self, index: usize) -> Option<P::Value> {
        self.pages
            .get_mut(index >> P::SHIFT)?
            .as_mut()?
            .remove(index & ((1 << P::SHIFT) - 1))
    }
}

/// Dense marks compared against an externally owned epoch.
#[derive(Default)]
pub(super) struct EpochColumn {
    marks: Vec<u32>,
}

impl EpochColumn {
    pub(super) fn ensure_len(&mut self, len: usize) {
        if self.marks.len() < len {
            self.marks.resize(len, 0);
        }
    }

    pub(super) fn mark(&mut self, index: usize, epoch: u32) -> bool {
        self.ensure_len(index + 1);
        let was_marked = self.marks[index] == epoch;
        self.marks[index] = epoch;
        !was_marked
    }

    fn clear(&mut self) {
        self.marks.fill(0);
    }
}

impl Deref for EpochColumn {
    type Target = Vec<u32>;

    fn deref(&self) -> &Self::Target {
        &self.marks
    }
}

impl DerefMut for EpochColumn {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.marks
    }
}

impl ShallowCapacityBytes for EpochColumn {
    fn shallow_capacity_bytes(&self) -> u64 {
        self.marks.shallow_capacity_bytes()
    }
}

/// Advance a shared epoch, clearing every participating column on wrap.
pub(super) fn advance_epoch(epoch: &mut u32, step: u32, columns: &mut [&mut EpochColumn]) -> u32 {
    if let Some(next) = epoch.checked_add(step) {
        *epoch = next;
    } else {
        for column in columns {
            column.clear();
        }
        *epoch = step;
    }
    *epoch
}

#[cfg(test)]
mod tests {
    use super::BitColumn;
    use super::PooledStampedIndex;

    #[test]
    fn stamped_index_rows_do_not_survive_their_owner() {
        let mut first = PooledStampedIndex::default();
        first.insert(4, 7);
        first.insert(9, 11);
        assert_eq!(first.get(4), Some(7));
        assert_eq!(first.get(9), Some(11));
        assert_eq!(first.get(5), None);
        assert_eq!(first.get(4000), None);
        drop(first);

        // The pooled rows are still allocated; the advanced stamp is what hides them.
        let mut second = PooledStampedIndex::default();
        assert_eq!(second.get(4), None);
        assert_eq!(second.get(9), None);
        second.insert(9, 3);
        assert_eq!(second.get(9), Some(3));
        assert_eq!(second.get(4), None);
    }

    #[test]
    fn stamped_index_costs_nothing_until_it_holds_a_row() {
        let index = PooledStampedIndex::default();
        assert_eq!(index.capacity_bytes(), 0);
        assert_eq!(index.get(0), None);
    }

    #[test]
    fn bit_column_equality_ignores_trailing_zero_words() {
        let mut left = BitColumn::default();
        left.set(1, true);
        left.set(70, true);
        left.set(70, false);

        let mut right = BitColumn::default();
        right.set(1, true);

        assert!(left == right);

        right.set(70, true);
        assert!(left != right);
    }
}
