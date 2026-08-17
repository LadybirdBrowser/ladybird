/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ops::{Deref, DerefMut};

use super::capacity::ShallowCapacityBytes;

/// A directly indexed column whose missing rows all have the same value.
#[derive(Clone)]
pub(super) struct Column<T> {
    entries: Vec<T>,
    vacant: fn() -> T,
}

impl<T: Default> Default for Column<T> {
    fn default() -> Self {
        Self::new(T::default)
    }
}

impl<T> Column<T> {
    pub(super) fn new(vacant: fn() -> T) -> Self {
        Self {
            entries: Vec::new(),
            vacant,
        }
    }

    /// Ensure that `index` is addressable and return newly allocated bytes.
    pub(super) fn ensure(&mut self, index: usize) -> u64 {
        if self.entries.len() > index {
            return 0;
        }
        let capacity_before = self.entries.capacity();
        self.entries.resize_with(
            index.checked_add(1).expect("column identity space exhausted"),
            self.vacant,
        );
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
    pub(super) fn insert(&mut self, index: usize, value: P::Value) -> (Option<P::Value>, bool) {
        let page_index = index >> P::SHIFT;
        if self.pages.len() <= page_index {
            self.pages.resize_with(page_index + 1, || None);
        }
        let page_was_absent = self.pages[page_index].is_none();
        let page = self.pages[page_index].get_or_insert_with(Box::default);
        let index = index & ((1 << P::SHIFT) - 1);
        let previous = page.get(index);
        page.insert(index, value);
        self.page_count += usize::from(page_was_absent);
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
