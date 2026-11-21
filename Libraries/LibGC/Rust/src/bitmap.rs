/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The per-cell bitmaps of a block: one bit for each cell, at the cell's index (see BlockDescriptor::cell_index()).

use crate::block::CELL_BITMAP_WORDS;

pub type CellBits = [u64; CELL_BITMAP_WORDS];

#[inline(always)]
pub fn bit(index: usize) -> u64 {
    1 << (index % 64)
}

#[inline(always)]
pub fn get(bits: &CellBits, index: usize) -> bool {
    bits[index / 64] & bit(index) != 0
}

#[inline(always)]
pub fn set(bits: &mut CellBits, index: usize) {
    bits[index / 64] |= bit(index);
}

#[inline(always)]
pub fn clear(bits: &mut CellBits, index: usize) {
    bits[index / 64] &= !bit(index);
}

pub fn count(bits: &CellBits) -> usize {
    bits.iter().map(|word| word.count_ones() as usize).sum()
}

/// The bits of word `word` that are within `begin..end`.
fn word_mask(word: usize, begin: usize, end: usize) -> u64 {
    let first = word * 64;
    let below = |limit: usize| match limit.saturating_sub(first) {
        0 => 0,
        n if n >= 64 => u64::MAX,
        n => (1 << n) - 1,
    };
    below(end) & !below(begin)
}

pub fn set_range(bits: &mut CellBits, begin: usize, end: usize) {
    for (word, value) in bits.iter_mut().enumerate() {
        *value |= word_mask(word, begin, end);
    }
}

pub fn clear_range(bits: &mut CellBits, begin: usize, end: usize) {
    for (word, value) in bits.iter_mut().enumerate() {
        *value &= !word_mask(word, begin, end);
    }
}

/// The first run of clear bits within `begin..end`, as the range of their indices.
pub fn first_clear_run(bits: &CellBits, begin: usize, end: usize) -> Option<(usize, usize)> {
    let start = first_with(bits, begin, end, false)?;
    let run_end = first_with(bits, start, end, true).unwrap_or(end);
    Some((start, run_end))
}

/// The first index within `begin..end` whose bit is `value`.
fn first_with(bits: &CellBits, begin: usize, end: usize, value: bool) -> Option<usize> {
    for (word, &bits) in bits.iter().enumerate().take(end.div_ceil(64)).skip(begin / 64) {
        let candidates = if value { bits } else { !bits } & word_mask(word, begin, end);
        if candidates != 0 {
            return Some(word * 64 + candidates.trailing_zeros() as usize);
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sets_and_clears_ranges_across_words() {
        let mut bits: CellBits = [0; CELL_BITMAP_WORDS];
        set_range(&mut bits, 3, 130);
        assert_eq!(count(&bits), 127);
        assert!(!get(&bits, 2) && get(&bits, 3) && get(&bits, 129) && !get(&bits, 130));
        clear_range(&mut bits, 60, 70);
        assert_eq!(count(&bits), 117);
        assert!(get(&bits, 59) && !get(&bits, 60) && !get(&bits, 69) && get(&bits, 70));
    }

    #[test]
    fn finds_runs_of_clear_bits() {
        let mut bits: CellBits = [0; CELL_BITMAP_WORDS];
        assert_eq!(first_clear_run(&bits, 2, 500), Some((2, 500)));
        set_range(&mut bits, 2, 100);
        set(&mut bits, 120);
        assert_eq!(first_clear_run(&bits, 2, 500), Some((100, 120)));
        set_range(&mut bits, 100, 120);
        assert_eq!(first_clear_run(&bits, 2, 500), Some((121, 500)));
        set_range(&mut bits, 121, 500);
        assert_eq!(first_clear_run(&bits, 2, 500), None);
        // NB: Bits past the end, which belong to no cell, never count.
        assert!(!get(&bits, 500));
    }
}
