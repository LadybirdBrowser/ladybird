/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ops::{Deref, DerefMut};

/// A directly indexed column whose missing rows all hold the default value.
#[derive(Clone, Default)]
pub(super) struct Column<T> {
    entries: Vec<T>,
}

impl<T: Default> Column<T> {
    /// Ensure that `index` is addressable and return newly allocated bytes.
    pub(super) fn ensure(&mut self, index: usize) -> u64 {
        if self.entries.len() > index {
            return 0;
        }
        let capacity_before = self.entries.capacity();
        let length = index.checked_add(1).expect("column identity space exhausted");
        self.entries.resize_with(length, T::default);
        ((self.entries.capacity() - capacity_before) * size_of::<T>()) as u64
    }

    /// Replace one row and return newly allocated bytes.
    pub(super) fn insert(&mut self, index: usize, value: T) -> u64 {
        let growth = self.ensure(index);
        self.entries[index] = value;
        growth
    }
}

impl<T> Column<T> {
    /// The bytes of the column's rows, which hold nothing on the heap.
    pub(super) fn heap_size(&self) -> usize {
        crate::heap_size::vec_bytes(&self.entries)
    }
}

impl<P: PagedColumnPage> PagedColumn<P> {
    /// The bytes of the column's pages, which hold nothing on the heap.
    pub(super) fn heap_size(&self) -> usize {
        crate::heap_size::vec_bytes(&self.pages) + self.pages.iter().flatten().count() * size_of::<P>()
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

/// A directly indexed packed boolean column.
#[derive(Clone, Default)]
pub(super) struct BitColumn {
    words: crate::cow_column::CowVec<u64, 64>,
}

impl PartialEq for BitColumn {
    fn eq(&self, other: &Self) -> bool {
        let (longer, shorter) = match self.words.len() >= other.words.len() {
            true => (&self.words, &other.words),
            false => (&other.words, &self.words),
        };
        longer
            .iter()
            .enumerate()
            .all(|(index, word)| shorter.get(index).copied().unwrap_or(0) == *word)
    }
}

impl Eq for BitColumn {}

impl BitColumn {
    pub(super) fn heap_size(&self) -> usize {
        self.words.row_bytes()
    }

    /// Set one bit and return whether its value changed and newly allocated bytes.
    pub(super) fn set(&mut self, index: usize, value: bool) -> (bool, u64) {
        let word_index = index / u64::BITS as usize;
        let bytes_before = self.words.row_bytes();
        if self.words.len() <= word_index {
            if !value {
                return (false, 0);
            }
            self.words.resize_with_default(word_index + 1);
        }
        let growth = (self.words.row_bytes() - bytes_before) as u64;
        let mask = 1_u64 << (index % u64::BITS as usize);
        if (self.words[word_index] & mask != 0) == value {
            return (false, growth);
        }
        if value {
            self.words[word_index] |= mask;
        } else {
            self.words[word_index] &= !mask;
        }
        (true, growth)
    }

    pub(super) fn contains(&self, index: usize) -> bool {
        let word = index / u64::BITS as usize;
        let mask = 1_u64 << (index % u64::BITS as usize);
        self.words.get(word).is_some_and(|word| word & mask != 0)
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

    #[cfg(test)]
    pub(super) fn page_count(&self) -> usize {
        self.page_count
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
