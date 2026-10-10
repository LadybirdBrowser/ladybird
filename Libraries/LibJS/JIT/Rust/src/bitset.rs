/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A fixed-size set of small integers.

#[derive(Clone, PartialEq, Eq, Default)]
pub struct BitSet {
    words: Vec<u64>,
    len: usize,
}

impl BitSet {
    /// An empty set that can hold the integers `0..len`.
    pub fn new(len: usize) -> Self {
        Self {
            words: vec![0; len.div_ceil(64)],
            len,
        }
    }

    /// The exclusive upper bound of the integers this set can hold.
    pub fn capacity(&self) -> usize {
        self.len
    }

    pub fn contains(&self, index: usize) -> bool {
        assert!(index < self.len, "bit {index} out of range {}", self.len);
        self.words[index / 64] & (1 << (index % 64)) != 0
    }

    pub fn insert(&mut self, index: usize) {
        assert!(index < self.len, "bit {index} out of range {}", self.len);
        self.words[index / 64] |= 1 << (index % 64);
    }

    pub fn remove(&mut self, index: usize) {
        assert!(index < self.len, "bit {index} out of range {}", self.len);
        self.words[index / 64] &= !(1 << (index % 64));
    }

    pub fn insert_range(&mut self, range: std::ops::Range<usize>) {
        for index in range {
            self.insert(index);
        }
    }

    /// Adds every element of `other`. Returns whether this set changed.
    pub fn union_with(&mut self, other: &BitSet) -> bool {
        assert_eq!(self.len, other.len, "union of differently sized sets");
        let mut changed = false;
        for (word, other_word) in self.words.iter_mut().zip(&other.words) {
            let new_word = *word | other_word;
            changed |= new_word != *word;
            *word = new_word;
        }
        changed
    }

    /// Removes every element of `other`.
    pub fn subtract(&mut self, other: &BitSet) {
        assert_eq!(self.len, other.len, "difference of differently sized sets");
        for (word, other_word) in self.words.iter_mut().zip(&other.words) {
            *word &= !other_word;
        }
    }

    pub fn is_empty(&self) -> bool {
        self.words.iter().all(|word| *word == 0)
    }

    pub fn count(&self) -> usize {
        self.words.iter().map(|word| word.count_ones() as usize).sum()
    }

    /// The elements in increasing order.
    pub fn iter(&self) -> impl Iterator<Item = usize> + '_ {
        self.words.iter().enumerate().flat_map(|(word_index, word)| {
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

impl std::fmt::Debug for BitSet {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_set().entries(self.iter()).finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn inserts_removes_and_iterates_across_words() {
        let mut set = BitSet::new(130);
        assert!(set.is_empty());
        for index in [0, 63, 64, 129] {
            set.insert(index);
        }
        assert!(set.contains(63) && set.contains(64) && !set.contains(65));
        assert_eq!(set.iter().collect::<Vec<_>>(), [0, 63, 64, 129]);
        set.remove(63);
        assert_eq!(set.count(), 3);

        let mut other = BitSet::new(130);
        other.insert(1);
        assert!(set.union_with(&other));
        assert!(!set.union_with(&other));
        assert_eq!(set.iter().collect::<Vec<_>>(), [0, 1, 64, 129]);
        other.insert(129);
        set.subtract(&other);
        assert_eq!(set.iter().collect::<Vec<_>>(), [0, 64]);
    }
}
