/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Name interning. An identifier's bytes up to 16 long are its own key, so a lookup is one multiply and a compare of
//! two words, with no text compare. Names of one or two bytes, which is most of minified code, skip even that.

use super::token::{KEYWORDS, SPECIAL_NAMES};

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

fn key_from_bytes(bytes: &[u8]) -> (u64, u64) {
    let mut buffer = [0u8; 16];
    buffer[..bytes.len()].copy_from_slice(bytes);
    (
        u64::from_le_bytes(buffer[..8].try_into().unwrap()),
        u64::from_le_bytes(buffer[8..].try_into().unwrap()),
    )
}

impl Interner {
    pub fn new(source_len: usize) -> Self {
        let wanted = (source_len / 16).clamp(1 << 10, 1 << 22).next_power_of_two();
        Self::with_tables(vec![NONE; 1 << 16], wanted, 1 << 10, source_len / 8)
    }

    fn with_tables(short: Vec<u32>, rows: usize, long_rows: usize, text_capacity: usize) -> Self {
        let mut interner = Self {
            short,
            rows: vec![Row { k0: 0, k1: 0, id: NONE }; rows],
            shift: 64 - rows.trailing_zeros(),
            count: 0,
            long_rows: vec![LongRow { hash: 0, id: NONE }; long_rows],
            long_count: 0,
            text: Vec::with_capacity(text_capacity),
            spans: Vec::with_capacity(rows / 2),
        };
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

    /// An interner that gives the names of an earlier parse the ids they had, sized to stay around with a source of any
    /// size. `names` yields them in id order, and starts with the names that every interner starts with.
    pub fn with_names<'a>(names: impl ExactSizeIterator<Item = &'a [u8]>) -> Self {
        let rows = (names.len() * 2).next_power_of_two();
        let mut interner = Self::with_tables(Vec::new(), rows, 1 << 4, names.len() * 8);
        for (id, name) in names.enumerate() {
            let interned = interner.intern_bytes(name);
            debug_assert_eq!(interned as usize, id);
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
        if bytes.len() <= 16 {
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
