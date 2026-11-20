/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A fixed-size set of small integers that keeps its words in a zone (see `Zone`).

use crate::Zone;
use crate::ZoneVec;

/// A fixed-size set of small integers, whose words are in a zone (see `Zone`).
#[derive(PartialEq, Eq)]
pub struct ZoneBitSet<'z> {
    // NB: A set of up to 64 integers fits in one word and takes no memory of the zone.
    first_word: u64,
    extra_words: ZoneVec<'z, u64>,
    len: usize,
}

impl Clone for ZoneBitSet<'_> {
    fn clone(&self) -> Self {
        Self {
            first_word: self.first_word,
            extra_words: self.extra_words.clone(),
            len: self.len,
        }
    }

    fn clone_from(&mut self, other: &Self) {
        self.first_word = other.first_word;
        // NB: This keeps the memory of the words where they fit.
        self.extra_words.assign(&other.extra_words);
        self.len = other.len;
    }
}

impl<'z> ZoneBitSet<'z> {
    /// An empty set that can hold the integers `0..len`, with its words in `zone`.
    pub fn new_in(len: usize, zone: &'z Zone) -> Self {
        Self {
            first_word: 0,
            extra_words: ZoneVec::from_elem_in(0, len.div_ceil(64).saturating_sub(1), zone),
            len,
        }
    }

    /// Takes all elements out. The set keeps its memory.
    pub fn clear(&mut self) {
        self.first_word = 0;
        self.extra_words.fill(0);
    }

    /// The exclusive upper bound of the integers this set can hold.
    pub fn capacity(&self) -> usize {
        self.len
    }

    pub fn contains(&self, index: usize) -> bool {
        assert!(index < self.len, "bit {index} out of range {}", self.len);
        let word = if index < 64 {
            self.first_word
        } else {
            self.extra_words[index / 64 - 1]
        };
        word & (1 << (index % 64)) != 0
    }

    pub fn insert(&mut self, index: usize) {
        assert!(index < self.len, "bit {index} out of range {}", self.len);
        if index < 64 {
            self.first_word |= 1 << index;
        } else {
            self.extra_words[index / 64 - 1] |= 1 << (index % 64);
        }
    }

    pub fn remove(&mut self, index: usize) {
        assert!(index < self.len, "bit {index} out of range {}", self.len);
        if index < 64 {
            self.first_word &= !(1 << index);
        } else {
            self.extra_words[index / 64 - 1] &= !(1 << (index % 64));
        }
    }

    pub fn insert_range(&mut self, range: std::ops::Range<usize>) {
        for index in range {
            self.insert(index);
        }
    }

    /// Adds every element of `other`. Returns whether this set changed.
    pub fn union_with(&mut self, other: &ZoneBitSet<'_>) -> bool {
        assert_eq!(self.len, other.len, "union of differently sized sets");
        let first_word = self.first_word | other.first_word;
        let mut changed = first_word != self.first_word;
        self.first_word = first_word;
        for (word, other_word) in self.extra_words.iter_mut().zip(other.extra_words.iter()) {
            let new_word = *word | other_word;
            changed |= new_word != *word;
            *word = new_word;
        }
        changed
    }

    /// Removes every element of `other`.
    pub fn subtract(&mut self, other: &ZoneBitSet<'_>) {
        assert_eq!(self.len, other.len, "difference of differently sized sets");
        self.first_word &= !other.first_word;
        for (word, other_word) in self.extra_words.iter_mut().zip(other.extra_words.iter()) {
            *word &= !other_word;
        }
    }

    pub fn is_empty(&self) -> bool {
        self.first_word == 0 && self.extra_words.iter().all(|word| *word == 0)
    }

    pub fn count(&self) -> usize {
        self.first_word.count_ones() as usize
            + self
                .extra_words
                .iter()
                .map(|word| word.count_ones() as usize)
                .sum::<usize>()
    }

    /// The elements in increasing order.
    pub fn iter(&self) -> impl Iterator<Item = usize> + '_ {
        std::iter::once(&self.first_word)
            .chain(self.extra_words.iter())
            .enumerate()
            .flat_map(|(word_index, word)| {
                let mut bits = *word;
                std::iter::from_fn(move || {
                    if bits == 0 {
                        return None;
                    }
                    let bit = bits.trailing_zeros() as usize;
                    bits &= bits - 1;
                    Some(word_index * 64 + bit)
                })
            })
    }
}

impl std::fmt::Debug for ZoneBitSet<'_> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_set().entries(self.iter()).finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_and_single_word_sets_clone_without_losing_bits() {
        let zone = Zone::new(1 << 20, c"test");
        for len in [0, 1, 63, 64, 65, 128, 129] {
            let mut set = ZoneBitSet::new_in(len, &zone);
            assert_eq!(set.capacity(), len);
            assert!(set.iter().next().is_none());
            if len > 0 {
                set.insert(len - 1);
            }
            let mut cloned = ZoneBitSet::new_in(130, &zone);
            cloned.clone_from(&set);
            assert_eq!(cloned, set);
            assert_eq!(cloned.count(), usize::from(len > 0));
            set.subtract(&cloned);
            assert!(set.is_empty());
            assert_eq!(set.union_with(&cloned), len > 0);
            assert!(!set.union_with(&cloned));
        }
    }

    #[test]
    fn a_set_in_a_zone_inserts_removes_and_iterates_across_words() {
        let zone = Zone::new(1 << 20, c"test");
        assert_eq!(ZoneBitSet::new_in(64, &zone).capacity(), 64);
        assert_eq!(zone.used(), 0);
        let mut set = ZoneBitSet::new_in(130, &zone);
        assert!(set.is_empty());
        for index in [0, 63, 64, 129] {
            set.insert(index);
        }
        assert_eq!(set.iter().collect::<Vec<_>>(), [0, 63, 64, 129]);
        assert_eq!(set.count(), 4);
        let mut other = ZoneBitSet::new_in(130, &zone);
        other.insert_range(0..2);
        other.insert(129);
        assert!(other.union_with(&set));
        assert!(!other.union_with(&set));
        assert_eq!(other.iter().collect::<Vec<_>>(), [0, 1, 63, 64, 129]);
        other.subtract(&set);
        assert_eq!(other.iter().collect::<Vec<_>>(), [1]);
        set.remove(63);
        assert!(!set.contains(63) && set.contains(64));
        let copy = set.clone();
        other.clone_from(&set);
        set.clear();
        assert!(set.is_empty());
        assert_eq!(copy, other);
        assert_eq!(format!("{copy:?}"), "{0, 64, 129}");
    }
}
