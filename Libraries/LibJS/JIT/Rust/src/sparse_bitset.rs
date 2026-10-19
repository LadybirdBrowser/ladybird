/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A fixed-size set of integers that stores only its nonempty words. Block liveness sets cover the whole graph's
//! node space but usually contain few values, so allocating that space for every block would be quadratic.

#[derive(Clone, Copy, Default, PartialEq, Eq)]
struct Word {
    index: usize,
    bits: u64,
}

#[derive(Default, PartialEq, Eq)]
pub struct SparseBitSet {
    // Sorted by index, with no empty words.
    words: Vec<Word>,
    len: usize,
}

impl Clone for SparseBitSet {
    fn clone(&self) -> Self {
        Self {
            words: self.words.clone(),
            len: self.len,
        }
    }

    fn clone_from(&mut self, other: &Self) {
        self.words.clone_from(&other.words);
        self.len = other.len;
    }
}

impl SparseBitSet {
    /// An empty set that can hold the integers `0..len`, without allocating storage for them.
    pub fn new(len: usize) -> Self {
        Self { words: Vec::new(), len }
    }

    pub fn contains(&self, index: usize) -> bool {
        assert!(index < self.len, "bit {index} out of range {}", self.len);
        self.words
            .binary_search_by_key(&(index / 64), |word| word.index)
            .is_ok_and(|position| self.words[position].bits & (1 << (index % 64)) != 0)
    }

    pub fn insert(&mut self, index: usize) {
        assert!(index < self.len, "bit {index} out of range {}", self.len);
        let word_index = index / 64;
        let bits = 1 << (index % 64);
        match self.words.binary_search_by_key(&word_index, |word| word.index) {
            Ok(position) => self.words[position].bits |= bits,
            Err(position) => self.words.insert(
                position,
                Word {
                    index: word_index,
                    bits,
                },
            ),
        }
    }

    pub fn remove(&mut self, index: usize) {
        assert!(index < self.len, "bit {index} out of range {}", self.len);
        if let Ok(position) = self.words.binary_search_by_key(&(index / 64), |word| word.index) {
            self.words[position].bits &= !(1 << (index % 64));
            if self.words[position].bits == 0 {
                self.words.remove(position);
            }
        }
    }

    /// Adds every element of `other`. Returns whether this set changed.
    pub fn union_with(&mut self, other: &Self) -> bool {
        assert_eq!(self.len, other.len, "union of differently sized sets");
        // Update the words already present and count the missing ones before growing. Most unions add no words;
        // the rest merge backwards into the same allocation, without shifting the suffix for each insertion.
        let mut position = 0;
        let mut missing = 0;
        let mut changed = false;
        for other_word in &other.words {
            while position < self.words.len() && self.words[position].index < other_word.index {
                position += 1;
            }
            if position < self.words.len() && self.words[position].index == other_word.index {
                let word = &mut self.words[position];
                let bits = word.bits | other_word.bits;
                changed |= bits != word.bits;
                word.bits = bits;
                position += 1;
            } else {
                missing += 1;
            }
        }
        if missing == 0 {
            return changed;
        }
        let mut left = self.words.len();
        let mut right = other.words.len();
        self.words.resize(left + missing, Word::default());
        let mut output = self.words.len();
        while right > 0 {
            let other_word = other.words[right - 1];
            let word = if left > 0 && self.words[left - 1].index > other_word.index {
                left -= 1;
                self.words[left]
            } else if left > 0 && self.words[left - 1].index == other_word.index {
                left -= 1;
                right -= 1;
                Word {
                    index: other_word.index,
                    bits: self.words[left].bits | other_word.bits,
                }
            } else {
                right -= 1;
                other_word
            };
            output -= 1;
            self.words[output] = word;
        }
        true
    }

    /// Removes every element of `other`.
    pub fn subtract(&mut self, other: &Self) {
        assert_eq!(self.len, other.len, "difference of differently sized sets");
        let mut position = 0;
        self.words.retain_mut(|word| {
            while position < other.words.len() && other.words[position].index < word.index {
                position += 1;
            }
            if position < other.words.len() && other.words[position].index == word.index {
                word.bits &= !other.words[position].bits;
            }
            word.bits != 0
        });
    }

    pub fn is_empty(&self) -> bool {
        self.words.is_empty()
    }

    /// The elements in increasing order.
    pub fn iter(&self) -> impl Iterator<Item = usize> + '_ {
        self.words.iter().flat_map(|word| {
            let mut bits = word.bits;
            std::iter::from_fn(move || {
                if bits == 0 {
                    return None;
                }
                let bit = bits.trailing_zeros() as usize;
                bits &= bits - 1;
                Some(word.index * 64 + bit)
            })
        })
    }
}

impl std::fmt::Debug for SparseBitSet {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_set().entries(self.iter()).finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bitset::BitSet;

    #[test]
    fn storage_depends_on_nonempty_words_not_the_universe() {
        let mut set = SparseBitSet::new(1_000_000);
        assert_eq!(set.words.capacity(), 0);
        for index in [999_999, 64, 63, 65] {
            set.insert(index);
        }
        assert_eq!(set.words.len(), 3);
        assert_eq!(set.iter().collect::<Vec<_>>(), [63, 64, 65, 999_999]);
        set.remove(64);
        assert_eq!(set.words.len(), 3);
        set.remove(65);
        assert_eq!(set.words.len(), 2);
        let copy = set.clone();
        set.subtract(&copy);
        assert!(set.is_empty());
    }

    #[test]
    fn operations_match_dense_sets() {
        let mut random = 42u64;
        let mut next = || {
            random ^= random << 13;
            random ^= random >> 7;
            random ^= random << 17;
            random as usize
        };
        let mut sparse = SparseBitSet::default();
        let mut dense = BitSet::default();
        for len in [0, 1, 63, 64, 65, 130, 4097] {
            sparse.clone_from(&SparseBitSet::new(len));
            dense.clone_from(&BitSet::new(len));
            for _ in 0..500 {
                let mut other_sparse = SparseBitSet::new(len);
                let mut other_dense = BitSet::new(len);
                for _ in 0..next() % 32 {
                    if len > 0 {
                        let index = next() % len;
                        other_sparse.insert(index);
                        other_dense.insert(index);
                    }
                }
                match next() % 5 {
                    0 if len > 0 => {
                        let index = next() % len;
                        sparse.insert(index);
                        dense.insert(index);
                    }
                    1 if len > 0 => {
                        let index = next() % len;
                        sparse.remove(index);
                        dense.remove(index);
                    }
                    2 => assert_eq!(sparse.union_with(&other_sparse), dense.union_with(&other_dense)),
                    3 => {
                        sparse.subtract(&other_sparse);
                        dense.subtract(&other_dense);
                    }
                    _ => {
                        sparse.clone_from(&other_sparse);
                        dense.clone_from(&other_dense);
                    }
                }
                assert_eq!(sparse.iter().collect::<Vec<_>>(), dense.iter().collect::<Vec<_>>());
                assert_eq!(sparse.is_empty(), dense.is_empty());
                assert!(sparse.words.windows(2).all(|words| words[0].index < words[1].index));
                assert!(sparse.words.iter().all(|word| word.bits != 0));
                for index in 0..len {
                    assert_eq!(sparse.contains(index), dense.contains(index));
                }
                assert_eq!(sparse, sparse.clone());
            }
        }
    }
}
