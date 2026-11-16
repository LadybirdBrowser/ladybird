/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The table of every distinct name in a document: element and attribute names, prefixes, namespace names.
//!
//! A name is interned the first time it is seen and is a 32-bit id from then on, so that comparing two names, finding
//! a prefix's namespace or checking a tag for a repeated attribute never looks at the name's bytes again. An entry
//! doesn't own its bytes: it points at the first place the name was written, in the source or in the arena.

use std::hash::BuildHasher;
use std::hash::RandomState;
use std::sync::OnceLock;

use crate::chars;
use crate::tape::NONE;

pub mod flag {
    /// The name is "xmlns".
    pub const IS_XMLNS: u8 = 1 << 0;
    /// The name is a QName with the prefix "xmlns".
    pub const PREFIX_XMLNS: u8 = 1 << 1;
    /// The name is a QName with the prefix "xml".
    pub const PREFIX_XML: u8 = 1 << 2;
    /// The name is a QName with a prefix.
    pub const HAS_PREFIX: u8 = 1 << 3;
    /// The name has a colon but isn't a QName: more than one colon, or nothing or no NCName on one side of it.
    pub const NOT_A_QNAME: u8 = 1 << 4;
    /// As an element name: the DTD declares an attribute with a default value for it.
    pub const HAS_DEFAULTS: u8 = 1 << 5;
    /// The flags above have been worked out.
    pub const ANALYZED: u8 = 1 << 6;
    /// As an element name: the DTD declares an attribute of a type other than CDATA for it.
    pub const HAS_TOKENIZED: u8 = 1 << 7;

    /// The flags that are about a name as an element name, and say nothing when it is an attribute's.
    pub const ELEMENT_ONLY: u8 = HAS_DEFAULTS | HAS_TOKENIZED;
    /// The flags that send a start tag through namespace processing.
    pub const NEEDS_NAMESPACE_PROCESSING: u8 = IS_XMLNS | PREFIX_XMLNS | HAS_PREFIX | NOT_A_QNAME | HAS_DEFAULTS;
}

/// The first sixteen bytes of a name as two little-endian words, with zeros past its end.
///
/// OPTIMIZATION: Nearly every name is at most sixteen bytes long, and then its key is all of it. Hashing a name and
/// comparing it to an entry are a few operations on two registers, with no loop over bytes, no call to memcmp, and
/// no trip to wherever in the document the entry's bytes are.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Key(u64, u64);

impl Key {
    /// The key of `bytes[start..start + length]`.
    #[inline(always)]
    pub fn at(bytes: &[u8], start: usize, length: usize) -> Key {
        if start + 16 <= bytes.len() {
            let w0 = u64::from_le_bytes(bytes[start..start + 8].try_into().unwrap_or([0; 8]));
            let w1 = u64::from_le_bytes(bytes[start + 8..start + 16].try_into().unwrap_or([0; 8]));
            if length >= 16 {
                Key(w0, w1)
            } else if length >= 8 {
                Key(w0, w1 & !(u64::MAX << ((length - 8) * 8)))
            } else {
                Key(w0 & !(u64::MAX << (length * 8)), 0)
            }
        } else {
            Key::near_end(&bytes[start..start + length])
        }
    }

    #[inline(always)]
    pub fn of(bytes: &[u8]) -> Key {
        Key::at(bytes, 0, bytes.len())
    }

    /// For a name with fewer than sixteen bytes of input from its start on.
    #[cold]
    #[inline(never)]
    fn near_end(name: &[u8]) -> Key {
        let mut buffer = [0u8; 16];
        let length = name.len().min(16);
        buffer[..length].copy_from_slice(&name[..length]);
        let w0 = u64::from_le_bytes(buffer[..8].try_into().unwrap_or([0; 8]));
        let w1 = u64::from_le_bytes(buffer[8..].try_into().unwrap_or([0; 8]));
        Key(w0, w1)
    }
}

/// Hashes a name id for a map keyed by one.
///
/// OPTIMIZATION: Ids are small integers the parser hands out itself, in order, so there is nothing for a keyed hash
/// to defend against, and one multiplication spreads them well enough.
#[derive(Default)]
pub struct IdHasher(u64);

impl std::hash::Hasher for IdHasher {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        for &byte in bytes {
            self.0 = (self.0 ^ byte as u64).wrapping_mul(K0);
        }
    }

    fn write_u32(&mut self, id: u32) {
        self.0 = (id as u64 + 1).wrapping_mul(K0).rotate_left(32);
    }
}

pub type IdMap<V> = std::collections::HashMap<u32, V, std::hash::BuildHasherDefault<IdHasher>>;

#[derive(Clone, Copy)]
pub struct Name {
    key: Key,
    hash: u32,
    pub length: u32,
    pub offset: u32,
    /// The id of the prefix, or NONE.
    pub prefix: u32,
    /// The id of the local part. For a name without a prefix, the name itself.
    pub local: u32,
    /// As a prefix: the index of its innermost binding in scope, or NONE.
    pub binding: u32,
    /// The start tag the name was last seen as an attribute of.
    pub stamp: u32,
    pub flags: u8,
}

pub struct Names {
    entries: Vec<Name>,
    /// Open addressing with linear probing. A slot holds an id plus one, or zero when empty.
    table: Vec<u32>,
    seed: u64,
}

/// The two byte ranges a name's offset can point into.
#[derive(Clone, Copy)]
pub struct Storage<'a> {
    pub source: &'a [u8],
    pub arena: &'a [u8],
}

impl<'a> Storage<'a> {
    #[inline(always)]
    pub fn new(source: &'a [u8], arena: &'a [u8]) -> Self {
        Storage { source, arena }
    }

    #[inline(always)]
    pub fn get(self, offset: u32, length: u32) -> &'a [u8] {
        let offset = offset as usize;
        let length = length as usize;
        if offset < self.source.len() {
            &self.source[offset..offset + length]
        } else {
            let offset = offset - self.source.len();
            &self.arena[offset..offset + length]
        }
    }
}

fn process_seed() -> u64 {
    // The seed only has to be unpredictable to whoever wrote the document, so that a document can't be built to make
    // every name collide.
    static SEED: OnceLock<u64> = OnceLock::new();
    *SEED.get_or_init(|| RandomState::new().hash_one(0x584D4Cu32) | 1)
}

const K0: u64 = 0x9E37_79B9_7F4A_7C15;
const K1: u64 = 0xD6E8_FEB8_6659_FD93;

#[inline(always)]
fn hash_name(seed: u64, key: Key, name: &[u8]) -> u32 {
    let mut hash = (key.0 ^ seed).wrapping_mul(K0);
    hash = (hash.rotate_left(29) ^ key.1 ^ name.len() as u64).wrapping_mul(K1);
    if name.len() > 16 {
        hash = hash_tail(hash, &name[16..]);
    }
    (hash >> 32) as u32 ^ hash as u32
}

#[inline(never)]
fn hash_tail(mut hash: u64, tail: &[u8]) -> u64 {
    for chunk in tail.chunks(8) {
        let mut word = 0u64;
        for (index, &byte) in chunk.iter().enumerate() {
            word |= (byte as u64) << (index * 8);
        }
        hash = (hash.rotate_left(29) ^ word).wrapping_mul(K1);
    }
    hash
}

impl Default for Names {
    fn default() -> Self {
        Self::new()
    }
}

impl Names {
    pub fn new() -> Self {
        Names {
            entries: Vec::with_capacity(64),
            table: vec![0; 128],
            seed: process_seed(),
        }
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub fn entries(&self) -> &[Name] {
        &self.entries
    }

    #[inline(always)]
    pub fn get(&self, id: u32) -> &Name {
        &self.entries[id as usize]
    }

    #[inline(always)]
    pub fn get_mut(&mut self, id: u32) -> &mut Name {
        &mut self.entries[id as usize]
    }

    #[inline(always)]
    pub fn bytes<'a>(&self, id: u32, storage: Storage<'a>) -> &'a [u8] {
        let name = &self.entries[id as usize];
        storage.get(name.offset, name.length)
    }

    /// Whether the name `id` is what is written at `bytes[start..]`. The caller checks what comes after it.
    #[inline(always)]
    pub fn is_at(&self, id: u32, bytes: &[u8], start: usize, storage: Storage) -> bool {
        let name = &self.entries[id as usize];
        let length = name.length as usize;
        if start + length > bytes.len() || Key::at(bytes, start, length) != name.key {
            return false;
        }
        length <= 16 || storage.get(name.offset, name.length)[16..] == bytes[start + 16..start + length]
    }

    /// Looks `name` up. Returns its id, or the slot it belongs in.
    #[inline(always)]
    fn lookup(&self, key: Key, hash: u32, name: &[u8], storage: Storage) -> Result<u32, usize> {
        let mask = self.table.len() - 1;
        let mut slot = hash as usize & mask;
        loop {
            let entry = self.table[slot];
            if entry == 0 {
                return Err(slot);
            }
            let candidate = &self.entries[entry as usize - 1];
            if candidate.hash == hash
                && candidate.key == key
                && candidate.length as usize == name.len()
                && (name.len() <= 16 || storage.get(candidate.offset, candidate.length)[16..] == name[16..])
            {
                return Ok(entry - 1);
            }
            slot = (slot + 1) & mask;
        }
    }

    /// Returns the id of `name` if it is in the table.
    pub fn find(&self, name: &[u8], storage: Storage) -> Option<u32> {
        let key = Key::of(name);
        self.lookup(key, hash_name(self.seed, key, name), name, storage).ok()
    }

    /// Returns the id of `name`, adding it to the table if it is new. `offset` is where `name` is in `storage`: an
    /// entry doesn't copy its bytes.
    pub fn intern(&mut self, name: &[u8], offset: u32, storage: Storage) -> u32 {
        let key = Key::of(name);
        let hash = hash_name(self.seed, key, name);
        match self.lookup(key, hash, name, storage) {
            Ok(id) => id,
            Err(slot) => self.insert(slot, key, hash, name.len() as u32, offset),
        }
    }

    /// `intern` for the name at `source[start..end]`, where `source` is the document itself.
    #[inline(always)]
    pub fn intern_in_source(&mut self, source: &[u8], start: usize, end: usize, arena: &[u8]) -> u32 {
        let name = &source[start..end];
        let key = Key::at(source, start, end - start);
        let hash = hash_name(self.seed, key, name);
        match self.lookup(key, hash, name, Storage { source, arena }) {
            Ok(id) => id,
            Err(slot) => self.insert(slot, key, hash, name.len() as u32, start as u32),
        }
    }

    #[inline(never)]
    fn insert(&mut self, slot: usize, key: Key, hash: u32, length: u32, offset: u32) -> u32 {
        let id = self.entries.len() as u32;
        self.entries.push(Name {
            key,
            hash,
            length,
            offset,
            prefix: NONE,
            local: id,
            binding: NONE,
            stamp: 0,
            flags: 0,
        });
        self.table[slot] = id + 1;
        if self.entries.len() * 2 > self.table.len() {
            self.grow();
        }
        id
    }

    fn grow(&mut self) {
        let size = self.table.len() * 2;
        let mask = size - 1;
        let mut table = vec![0u32; size];
        for (index, name) in self.entries.iter().enumerate() {
            let mut slot = name.hash as usize & mask;
            while table[slot] != 0 {
                slot = (slot + 1) & mask;
            }
            table[slot] = index as u32 + 1;
        }
        self.table = table;
    }

    /// Works out how the name `id` splits into a prefix and a local part, interning both, if that hasn't been done.
    /// Returns the name's flags.
    #[inline(always)]
    pub fn analyze(&mut self, id: u32, storage: Storage) -> u8 {
        let flags = self.entries[id as usize].flags;
        if flags & flag::ANALYZED != 0 {
            return flags;
        }
        self.analyze_slow(id, storage)
    }

    #[inline(never)]
    fn analyze_slow(&mut self, id: u32, storage: Storage) -> u8 {
        let entry = self.entries[id as usize];
        let bytes = storage.get(entry.offset, entry.length);
        let mut flags = entry.flags | flag::ANALYZED;
        if let Some(colon) = bytes.iter().position(|&byte| byte == b':') {
            let (prefix, local) = (&bytes[..colon], &bytes[colon + 1..]);
            if chars::is_name(prefix, false) && chars::is_name(local, false) {
                let prefix_id = self.intern(prefix, entry.offset, storage);
                let local_id = self.intern(local, entry.offset + colon as u32 + 1, storage);
                flags |= flag::HAS_PREFIX;
                if prefix == b"xmlns" {
                    flags |= flag::PREFIX_XMLNS;
                } else if prefix == b"xml" {
                    flags |= flag::PREFIX_XML;
                }
                let entry = &mut self.entries[id as usize];
                entry.prefix = prefix_id;
                entry.local = local_id;
            } else {
                flags |= flag::NOT_A_QNAME;
            }
        } else if bytes == b"xmlns" {
            flags |= flag::IS_XMLNS;
        }
        self.entries[id as usize].flags = flags;
        flags
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keys() {
        let text = b"abcdefghijklmnopqrstuvwxyz0123456789";
        for start in 0..text.len() {
            for length in 0..=(text.len() - start).min(20) {
                let name = &text[start..start + length];
                // A key is the same wherever the name is read from, and however much input follows it.
                assert!(Key::at(text, start, length) == Key::of(name), "{start} {length}");
                assert!(Key::at(text, start, length) == Key::near_end(name), "{start} {length}");
            }
        }
        assert!(Key::of(b"abc") != Key::of(b"abd"));
        assert!(Key::of(b"abcdefghi") != Key::of(b"abcdefghj"));
    }

    #[test]
    fn interning() {
        let source = b"short a-name-longer-than-sixteen-bytes a-name-longer-than-sixteen-bytez short";
        let storage = Storage { source, arena: &[] };
        let mut names = Names::new();
        let short = names.intern(&source[0..5], 0, storage);
        let long = names.intern(&source[6..38], 6, storage);
        let other = names.intern(&source[39..71], 39, storage);
        assert_ne!(long, other);
        assert_eq!(names.intern_in_source(source, 72, 77, &[]), short);
        assert_eq!(names.find(b"a-name-longer-than-sixteen-bytes", storage), Some(long));
        assert_eq!(names.find(b"a-name-longer-than-sixteen-bytex", storage), None);
        assert!(names.is_at(long, source, 6, storage));
        assert!(!names.is_at(long, source, 39, storage));
        assert!(names.is_at(short, source, 72, storage));
        // Enough names to make the table grow several times.
        let mut arena = Vec::new();
        let mut ranges = Vec::new();
        for index in 0..1000u32 {
            let name = format!("name-number-{index}");
            ranges.push((arena.len(), name.len()));
            arena.extend_from_slice(name.as_bytes());
        }
        let storage = Storage { source, arena: &arena };
        let base = source.len();
        let ids: Vec<u32> = ranges
            .iter()
            .map(|&(start, length)| names.intern(&arena[start..start + length], (base + start) as u32, storage))
            .collect();
        for (&(start, length), &id) in ranges.iter().zip(&ids) {
            assert_eq!(names.find(&arena[start..start + length], storage), Some(id));
        }
        assert_eq!(names.find(b"short", storage), Some(short));
    }
}
