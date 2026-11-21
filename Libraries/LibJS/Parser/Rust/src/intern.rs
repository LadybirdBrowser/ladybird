/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Name interning. An identifier's bytes up to 16 long are its own key, so a lookup is one multiply and a compare of
//! two words, with no text compare. Names of one or two bytes, which is most of minified code, skip even that.
//!
//! The tables of an interner are slices. Those of a first parse are in its work zone, and the ones that it outgrows
//! stay there until the parse ends. The reparse context of a source keeps the names of the source in tables of its own
//! (see `KeptNames` in LibJS), and each parse of a function again interns its names with those tables in place.

use super::token::{KEYWORDS, SPECIAL_NAMES};
use ak::Zone;
use core::mem::MaybeUninit;

pub const NONE: u32 = u32::MAX;

#[derive(Clone, Copy)]
#[repr(C, align(32))]
pub struct Row {
    k0: u64,
    k1: u64,
    id: u32,
}

#[derive(Clone, Copy)]
pub struct LongRow {
    hash: u64,
    id: u32,
}

const EMPTY_ROW: Row = Row { k0: 0, k1: 0, id: NONE };
const EMPTY_LONG_ROW: LongRow = LongRow { hash: 0, id: NONE };

pub struct Interner<'z> {
    /// Where the tables move when they grow.
    zone: &'z Zone,
    short: &'z mut [u32],
    rows: &'z mut [Row],
    shift: u32,
    count: u32,
    long_rows: &'z mut [LongRow],
    long_count: u32,
    /// Each name's text, in UTF-8, in the first `text_len` bytes, which are written. The rest is room for more.
    text: &'z mut [MaybeUninit<u8>],
    text_len: u32,
    /// The start and the length of the text of each name, in the first `len` entries, which are written.
    spans: &'z mut [MaybeUninit<(u32, u32)>],
    len: u32,
}

/// The tables of an interner, all of which are written, for a copy of them that stays around.
pub struct NameTables<'t> {
    pub rows: &'t [Row],
    pub shift: u32,
    pub count: u32,
    pub long_rows: &'t [LongRow],
    pub long_count: u32,
    pub text: &'t [u8],
    pub spans: &'t [(u32, u32)],
}

/// Tables that an interner can work with in place (see `Interner::of_tables()`).
pub struct NameTablesMut<'t> {
    pub rows: &'t mut [Row],
    pub shift: u32,
    pub count: u32,
    pub long_rows: &'t mut [LongRow],
    pub long_count: u32,
    pub text: &'t mut [u8],
    pub spans: &'t mut [(u32, u32)],
}

/// Makes room for `more` values after the first `len` of `list`. A list that is too short for them moves to new memory
/// of `zone`, with at least twice the room.
fn reserve<'z, T: Copy>(zone: &'z Zone, list: &mut &'z mut [MaybeUninit<T>], len: usize, more: usize) {
    if len + more <= list.len() {
        return;
    }
    let grown = zone.alloc_uninit_slice((len + more).max(list.len() * 2));
    grown[..len].copy_from_slice(&list[..len]);
    *list = grown;
}

/// `values` as room for values, all of which is written.
fn as_room<T: Copy>(values: &mut [T]) -> &mut [MaybeUninit<T>] {
    // SAFETY: A `MaybeUninit<T>` has the layout of a `T`. An interner only writes values into its room, so the values
    //         that the caller sees again stay written.
    unsafe { &mut *(core::ptr::from_mut(values) as *mut [MaybeUninit<T>]) }
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

impl<'z> Interner<'z> {
    /// An interner for a source of `source_len` units. `initial` are the tables of an interner with the names that every
    /// interner starts with (see `Interner::initial()`), which a small source starts from.
    pub(super) fn new(zone: &'z Zone, source_len: usize, initial: &NameTables<'_>) -> Self {
        let wanted = (source_len / 16).clamp(1 << 8, 1 << 22).next_power_of_two();
        // OPTIMIZATION: Tiny scripts, such as repeated evals, do not amortize the setup of a direct table for two-byte
        // names.
        let short: &'z mut [u32] = if source_len >= 1 << 16 {
            zone.alloc_slice_fill(1 << 16, NONE)
        } else {
            &mut []
        };
        let long_rows = (source_len / 128).clamp(1 << 4, 1 << 10).next_power_of_two();
        if wanted == 1 << 8 && long_rows == 1 << 4 {
            // OPTIMIZATION: Every parser starts with the same reserved names. Copy their tables for small sources
            // instead of interning all of them again, which is more work than parsing many eval strings.
            let text = zone.alloc_uninit_slice(initial.text.len().max(512));
            text[..initial.text.len()].write_copy_of_slice(initial.text);
            let spans = zone.alloc_uninit_slice(initial.spans.len().max(1 << 7));
            spans[..initial.spans.len()].write_copy_of_slice(initial.spans);
            return Self {
                zone,
                short,
                rows: zone.alloc_slice_copy(initial.rows),
                shift: initial.shift,
                count: initial.count,
                long_rows: zone.alloc_slice_copy(initial.long_rows),
                long_count: initial.long_count,
                text,
                text_len: initial.text.len() as u32,
                spans,
                len: initial.spans.len() as u32,
            };
        }
        Self::with_tables(zone, short, wanted, long_rows, (source_len / 8).max(512))
    }

    /// An interner with only the names that every interner starts with, at the size that `new()` gives a small source.
    pub fn initial(zone: &'z Zone) -> Self {
        Self::with_tables(zone, &mut [], 1 << 8, 1 << 4, 512)
    }

    fn with_tables(zone: &'z Zone, short: &'z mut [u32], rows: usize, long_rows: usize, text_capacity: usize) -> Self {
        let mut interner = Self::empty(zone, short, rows, long_rows, text_capacity);
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

    fn empty(zone: &'z Zone, short: &'z mut [u32], rows: usize, long_rows: usize, text_capacity: usize) -> Self {
        Self {
            zone,
            short,
            rows: zone.alloc_slice_fill(rows, EMPTY_ROW),
            shift: 64 - rows.trailing_zeros(),
            count: 0,
            long_rows: zone.alloc_slice_fill(long_rows, EMPTY_LONG_ROW),
            long_count: 0,
            text: zone.alloc_uninit_slice(text_capacity),
            text_len: 0,
            spans: zone.alloc_uninit_slice(rows / 2),
            len: 0,
        }
    }

    /// An interner that gives the names of an earlier parse the ids they had, sized to stay around with a source of any
    /// size (see `tables()`). `names` yields them in id order, none of them twice, and starts with the names that every
    /// interner starts with.
    pub fn with_names<'a>(zone: &'z Zone, names: impl ExactSizeIterator<Item = &'a [u8]>) -> Self {
        let rows = (names.len() * 2).next_power_of_two();
        let mut interner = Self::empty(zone, &mut [], rows, 1 << 4, names.len() * 8);
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

    /// The tables, at the size they have, to copy. The table of short names is left out.
    #[inline]
    pub fn tables(&self) -> NameTables<'_> {
        NameTables {
            rows: self.rows,
            shift: self.shift,
            count: self.count,
            long_rows: self.long_rows,
            long_count: self.long_count,
            text: self.all_text(),
            spans: self.spans(),
        }
    }

    /// An interner with `tables` in place. The text and the span of a name that they do not have go to `zone`, and so
    /// do the rows when they grow, so that the tables never refer to the zone. But the row of such a name is written
    /// into `tables` first, so that they stay as they were only if no name is new.
    pub fn of_tables(zone: &'z Zone, tables: NameTablesMut<'z>) -> Self {
        Self {
            zone,
            short: &mut [],
            shift: tables.shift,
            count: tables.count,
            long_count: tables.long_count,
            text_len: tables.text.len() as u32,
            len: tables.spans.len() as u32,
            rows: tables.rows,
            long_rows: tables.long_rows,
            text: as_room(tables.text),
            spans: as_room(tables.spans),
        }
    }

    // NB: An interner is never empty: it starts with the names that every interner has.
    #[allow(clippy::len_without_is_empty)]
    pub fn len(&self) -> usize {
        self.len as usize
    }

    #[inline]
    pub fn text(&self, id: u32) -> &[u8] {
        let (start, len) = self.spans()[id as usize];
        // SAFETY: The text of a name is written.
        unsafe { self.text[start as usize..(start + len) as usize].assume_init_ref() }
    }

    fn all_text(&self) -> &[u8] {
        // SAFETY: The first `text_len` bytes are written.
        unsafe { self.text[..self.text_len as usize].assume_init_ref() }
    }

    #[inline]
    fn spans(&self) -> &[(u32, u32)] {
        // SAFETY: The first `len` spans are written.
        unsafe { self.spans[..self.len()].assume_init_ref() }
    }

    fn add_name(&mut self, bytes: &[u8]) -> u32 {
        let id = self.len;
        let start = self.text_len as usize;
        reserve(self.zone, &mut self.spans, id as usize, 1);
        reserve(self.zone, &mut self.text, start, bytes.len());
        self.spans[id as usize].write((start as u32, bytes.len() as u32));
        self.text[start..start + bytes.len()].write_copy_of_slice(bytes);
        self.len += 1;
        self.text_len += bytes.len() as u32;
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
        // NB: The old table stays in the zone until the parse ends.
        let old = core::mem::replace(&mut self.rows, self.zone.alloc_slice_fill(new_len, EMPTY_ROW));
        self.shift -= 1;
        let mask = new_len - 1;
        for &row in old.iter() {
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
            let old = core::mem::replace(&mut self.long_rows, self.zone.alloc_slice_fill(new_len, EMPTY_LONG_ROW));
            mask = new_len - 1;
            for &row in old.iter() {
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
    use super::NameTablesMut;
    use ak::Zone;

    #[test]
    fn trailing_nuls_preserve_name_identity() {
        // Exercise the prepared small interner, the general fallback, and the direct short-name table.
        for source_len in [0, 20_000, 65_536] {
            for reverse in [false, true] {
                let zone = Zone::new(1 << 30, c"test");
                let initial = Interner::initial(&zone);
                let mut interner = Interner::new(&zone, source_len, &initial.tables());
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

                // NB: The tables of a reparse context are a copy of those that an interner makes with the names.
                let made = Interner::with_names(&zone, (0..interner.len()).map(|id| interner.text(id as u32)));
                let tables = made.tables();
                let kept = NameTablesMut {
                    rows: zone.alloc_slice_copy(tables.rows),
                    shift: tables.shift,
                    count: tables.count,
                    long_rows: zone.alloc_slice_copy(tables.long_rows),
                    long_count: tables.long_count,
                    text: zone.alloc_slice_copy(tables.text),
                    spans: zone.alloc_slice_copy(tables.spans),
                };
                let mut reparsed = Interner::of_tables(&zone, kept);
                for (name, id) in &expected {
                    assert_eq!(reparsed.intern_bytes(name), *id);
                    assert_eq!(reparsed.text(*id), name);
                }
            }
        }
    }
}
