/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Name interning. An identifier's bytes up to 16 long are its own key, so a lookup is one multiply and a compare of
//! two words, with no text compare. Names of one or two bytes, which is most of minified code, skip even that.

use super::token::{KEYWORDS, SPECIAL_NAMES};
use std::sync::OnceLock;

pub const NONE: u32 = u32::MAX;

#[derive(Clone, Copy)]
#[repr(C, align(32))]
struct Row {
    k0: u64,
    k1: u64,
    id: u32,
}

#[derive(Clone, Copy)]
struct LongRow {
    hash: u64,
    id: u32,
}

pub struct Interner {
    short: Vec<u32>,
    rows: Vec<Row>,
    shift: u32,
    count: u32,
    long_rows: Vec<LongRow>,
    long_count: u32,
    /// Each name's text, in UTF-8.
    text: Vec<u8>,
    spans: Vec<(u32, u32)>,
}

#[inline(always)]
fn hash_key(k0: u64, k1: u64) -> u64 {
    (k0 ^ k1.rotate_left(29)).wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ k1.wrapping_mul(0xC2B2_AE3D_27D4_EB4F)
}

fn hash_bytes(bytes: &[u8]) -> u64 {
    let mut h = bytes.len() as u64;
    let (chunks, remainder) = bytes.as_chunks::<8>();
    for chunk in chunks {
        h = (h ^ u64::from_le_bytes(*chunk)).wrapping_mul(0x9E37_79B9_7F4A_7C15);
        h ^= h >> 29;
    }
    for &b in remainder {
        h = (h ^ b as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15);
    }
    h ^ (h >> 32)
}

/// The key of a name of at most 16 bytes: its bytes in order, as two little-endian words.
/// Names ending in NUL use the full-text table, since zero padding would erase their length.
#[inline(always)]
fn key_from_bytes(bytes: &[u8]) -> (u64, u64) {
    let word_at = |at: usize| u64::from_le_bytes(bytes[at..at + 8].try_into().unwrap());
    let half_word_at = |at: usize| u64::from(u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap()));
    let length = bytes.len();
    match length {
        0 => (0, 0),
        1..=3 => {
            let (middle, last) = (length / 2, length - 1);
            let word = u64::from(bytes[0]) | u64::from(bytes[middle]) << (middle * 8);
            (word | u64::from(bytes[last]) << (last * 8), 0)
        }
        4..=7 => (half_word_at(0) | half_word_at(length - 4) << ((length - 4) * 8), 0),
        8 => (word_at(0), 0),
        _ => (word_at(0), word_at(length - 8) >> ((16 - length) * 8)),
    }
}

impl Interner {
    pub fn new(source_len: usize) -> Self {
        let wanted = (source_len / 16).clamp(1 << 8, 1 << 22).next_power_of_two();
        // OPTIMIZATION: Tiny scripts, such as repeated evals, do not amortize the setup of a direct table for two-byte
        // names.
        let short = if source_len >= 1 << 16 {
            vec![NONE; 1 << 16]
        } else {
            Vec::new()
        };
        let long_rows = (source_len / 128).clamp(1 << 4, 1 << 10).next_power_of_two();
        if wanted == 1 << 8 && long_rows == 1 << 4 {
            // OPTIMIZATION: Every parser starts with the same reserved names. Copy their tables for small sources
            // instead of interning all of them again, which is more work than parsing many eval strings.
            static INITIAL: OnceLock<Interner> = OnceLock::new();
            let initial = INITIAL.get_or_init(|| Self::with_tables(Vec::new(), 1 << 8, 1 << 4, 512));
            let mut interner = Self {
                short,
                rows: initial.rows.clone(),
                shift: initial.shift,
                count: initial.count,
                long_rows: initial.long_rows.clone(),
                long_count: initial.long_count,
                text: Vec::with_capacity(512),
                spans: Vec::with_capacity(1 << 7),
            };
            interner.text.extend_from_slice(&initial.text);
            interner.spans.extend_from_slice(&initial.spans);
            return interner;
        }
        Self::with_tables(short, wanted, long_rows, (source_len / 8).max(512))
    }

    fn with_tables(short: Vec<u32>, rows: usize, long_rows: usize, text_capacity: usize) -> Self {
        let mut interner = Self::empty(short, rows, long_rows, text_capacity);
        for (word, _) in KEYWORDS {
            interner.intern_bytes(word.as_bytes());
        }
        for word in SPECIAL_NAMES {
            interner.intern_bytes(word.as_bytes());
        }
        for pseudo in [".this", ".new.target", ".super", ".unbound"] {
            interner.intern_bytes(pseudo.as_bytes());
        }
        interner
    }

    fn empty(short: Vec<u32>, rows: usize, long_rows: usize, text_capacity: usize) -> Self {
        Self {
            short,
            rows: vec![Row { k0: 0, k1: 0, id: NONE }; rows],
            shift: 64 - rows.trailing_zeros(),
            count: 0,
            long_rows: vec![LongRow { hash: 0, id: NONE }; long_rows],
            long_count: 0,
            text: Vec::with_capacity(text_capacity),
            spans: Vec::with_capacity(rows / 2),
        }
    }

    /// An interner that gives the names of an earlier parse the ids they had, sized to stay around with a source of any
    /// size. `names` yields them in id order, none of them twice, and starts with the names that every interner starts
    /// with.
    pub fn with_names<'a>(names: impl ExactSizeIterator<Item = &'a [u8]>) -> Self {
        let rows = (names.len() * 2).next_power_of_two();
        let mut interner = Self::empty(Vec::new(), rows, 1 << 4, names.len() * 8);
        let mask = rows - 1;
        for name in names {
            if name.len() > 16 || name.last() == Some(&0) {
                interner.intern_bytes(name);
                continue;
            }
            let id = interner.add_name(name);
            let (k0, k1) = key_from_bytes(name);
            let mut index = (hash_key(k0, k1) >> interner.shift) as usize;
            // NB: The names are all different, so the first free row is the one.
            while interner.rows[index].id != NONE {
                index = (index + 1) & mask;
            }
            interner.rows[index] = Row { k0, k1, id };
            interner.count += 1;
        }
        interner
    }

    pub fn len(&self) -> usize {
        self.spans.len()
    }

    pub fn text(&self, id: u32) -> &[u8] {
        let (start, len) = self.spans[id as usize];
        &self.text[start as usize..(start + len) as usize]
    }

    fn add_name(&mut self, bytes: &[u8]) -> u32 {
        let id = self.spans.len() as u32;
        self.spans.push((self.text.len() as u32, bytes.len() as u32));
        self.text.extend_from_slice(bytes);
        id
    }

    /// Interns a name of at most 16 bytes, given as its key.
    #[inline(always)]
    pub fn intern_key(&mut self, k0: u64, k1: u64, len: usize) -> u32 {
        if len <= 2 && !self.short.is_empty() {
            let id = self.short[k0 as usize];
            if id != NONE {
                return id;
            }
            return self.add_short(k0, len);
        }
        let mask = self.rows.len() - 1;
        let mut index = (hash_key(k0, k1) >> self.shift) as usize;
        loop {
            // SAFETY: The index is masked to the table.
            let row = unsafe { self.rows.get_unchecked(index) };
            // NB: The empty name has the key of an empty row.
            if row.id == NONE {
                return self.insert_key(k0, k1, len, index);
            }
            if row.k0 == k0 && row.k1 == k1 {
                return row.id;
            }
            index = (index + 1) & mask;
        }
    }

    #[cold]
    #[inline(never)]
    fn add_short(&mut self, k0: u64, len: usize) -> u32 {
        let bytes = k0.to_le_bytes();
        let id = self.add_name(&bytes[..len]);
        self.short[k0 as usize] = id;
        id
    }

    #[inline(never)]
    fn insert_key(&mut self, k0: u64, k1: u64, len: usize, index: usize) -> u32 {
        let mut bytes = [0u8; 16];
        bytes[..8].copy_from_slice(&k0.to_le_bytes());
        bytes[8..].copy_from_slice(&k1.to_le_bytes());
        let id = self.add_name(&bytes[..len]);
        self.rows[index] = Row { k0, k1, id };
        self.count += 1;
        if self.count as usize * 2 > self.rows.len() {
            self.grow();
        }
        id
    }

    #[cold]
    fn grow(&mut self) {
        let new_len = self.rows.len() * 2;
        let old = std::mem::replace(&mut self.rows, vec![Row { k0: 0, k1: 0, id: NONE }; new_len]);
        self.shift -= 1;
        let mask = new_len - 1;
        for row in old {
            if row.id == NONE {
                continue;
            }
            let mut index = (hash_key(row.k0, row.k1) >> self.shift) as usize;
            while self.rows[index].id != NONE {
                index = (index + 1) & mask;
            }
            self.rows[index] = row;
        }
    }

    /// Interns any name, given as UTF-8.
    pub fn intern_bytes(&mut self, bytes: &[u8]) -> u32 {
        if bytes.len() <= 16 && bytes.last() != Some(&0) {
            let (k0, k1) = key_from_bytes(bytes);
            return self.intern_key(k0, k1, bytes.len());
        }
        let hash = hash_bytes(bytes);
        let mut mask = self.long_rows.len() - 1;
        let mut index = hash as usize & mask;
        loop {
            let row = self.long_rows[index];
            if row.id == NONE {
                break;
            }
            if row.hash == hash && self.text(row.id) == bytes {
                return row.id;
            }
            index = (index + 1) & mask;
        }
        let id = self.add_name(bytes);
        self.long_rows[index] = LongRow { hash, id };
        self.long_count += 1;
        if self.long_count as usize * 2 > self.long_rows.len() {
            let new_len = self.long_rows.len() * 2;
            let old = std::mem::replace(&mut self.long_rows, vec![LongRow { hash: 0, id: NONE }; new_len]);
            mask = new_len - 1;
            for row in old {
                if row.id == NONE {
                    continue;
                }
                let mut index = row.hash as usize & mask;
                while self.long_rows[index].id != NONE {
                    index = (index + 1) & mask;
                }
                self.long_rows[index] = row;
            }
        }
        id
    }
}

#[cfg(test)]
mod tests {
    use super::Interner;

    #[test]
    fn trailing_nuls_preserve_name_identity() {
        // Exercise the prepared small interner, the general fallback, and the direct short-name table.
        for source_len in [0, 20_000, 65_536] {
            for reverse in [false, true] {
                let mut interner = Interner::new(source_len);
                let mut expected = Vec::new();
                for prefix in [
                    b"".as_slice(),
                    b"a",
                    b"ab",
                    b"in",
                    b"\0a",
                    b"a\0b",
                    b"abcdefgh",
                    b"abcdefghijklmnop",
                ] {
                    for index in 0..=17 - prefix.len() {
                        let padding = if reverse { 17 - prefix.len() - index } else { index };
                        let mut name = prefix.to_vec();
                        name.resize(prefix.len() + padding, 0);
                        let id = interner.intern_bytes(&name);
                        assert_eq!(interner.text(id), name);
                        expected.push((name, id));
                    }
                }

                // Grow the tables before looking up the names again.
                for index in 0..512 {
                    interner.intern_bytes(format!("extra_name_{index}").as_bytes());
                }
                for (name, id) in &expected {
                    assert_eq!(interner.intern_bytes(name), *id);
                }

                let mut reparsed = Interner::with_names((0..interner.len()).map(|id| interner.text(id as u32)));
                for (name, id) in &expected {
                    assert_eq!(reparsed.intern_bytes(name), *id);
                    assert_eq!(reparsed.text(*id), name);
                }
            }
        }
    }
}
