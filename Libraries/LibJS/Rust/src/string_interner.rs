/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! String interner for JavaScript identifiers and string literals.
//!
//! Deduplicates UTF-16 strings and returns lightweight `InternedId` handles
//! (4 bytes) instead of full string allocations. Uses `FxHash` for fast
//! hashing of trusted compiler inputs.
//!
//! Adapted from <https://github.com/anonrig/string_interner>.

use fxhash::{FxBuildHasher, FxHashMap};

use crate::ast::SharedUtf16String;

/// A compact, copyable handle to an interned string.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, Default)]
pub struct InternedId(u32);

impl InternedId {
    pub fn value(self) -> u32 {
        self.0
    }
}

pub struct StringInterner {
    // The &'static lifetime is a lie: the references point into the Box<[u16]>
    // entries in `list`. This is safe because:
    // 1. Box<[u16]> stores data on the heap with a stable address.
    // 2. Entries are never removed or modified.
    // 3. The references are invalidated when the StringInterner is dropped,
    //    at which point the HashMap is also dropped.
    data: FxHashMap<&'static [u16], InternedId>,
    list: Vec<Box<[u16]>>,
    /// Cached SharedUtf16String instances, parallel to `list`. Each unique
    /// interned string gets one Rc allocation; subsequent lookups return
    /// clones (Rc bump only).
    shared: Vec<SharedUtf16String>,
}

impl Clone for StringInterner {
    fn clone(&self) -> Self {
        let list = self.list.clone();
        let mut data = FxHashMap::with_capacity_and_hasher(list.len(), FxBuildHasher::default());

        for (index, entry) in list.iter().enumerate() {
            let ptr = entry.as_ptr();
            let len = entry.len();
            // SAFETY: The cloned Box<[u16]> entries have stable heap storage
            // owned by the cloned interner, and remain valid until it is dropped.
            let key: &'static [u16] = unsafe { std::slice::from_raw_parts(ptr, len) };
            data.insert(key, InternedId(index as u32));
        }

        Self {
            data,
            list,
            shared: self.shared.clone(),
        }
    }
}

impl Default for StringInterner {
    fn default() -> Self {
        Self::new()
    }
}

impl StringInterner {
    pub fn new() -> Self {
        Self {
            data: FxHashMap::default(),
            list: Vec::new(),
            shared: Vec::new(),
        }
    }

    pub fn with_capacity(capacity: usize) -> Self {
        Self {
            data: FxHashMap::with_capacity_and_hasher(capacity, FxBuildHasher::default()),
            list: Vec::with_capacity(capacity),
            shared: Vec::with_capacity(capacity),
        }
    }

    /// Intern a UTF-16 string slice. Returns the existing ID if the string
    /// is already interned, or allocates and stores it once otherwise.
    #[inline]
    pub fn intern(&mut self, input: &[u16]) -> InternedId {
        if let Some(&id) = self.data.get(input) {
            return id;
        }

        let owned: Box<[u16]> = input.into();

        let ptr = owned.as_ptr();
        let len = owned.len();

        let id = InternedId(self.list.len() as u32);
        self.shared.push(SharedUtf16String::from(input));
        self.list.push(owned);

        // SAFETY: Box<[u16]> stores data on the heap. Pushing the box into
        // the Vec transfers ownership of the Box wrapper (a pointer) but does
        // not move the heap data it points to. The &[u16] reference we create
        // here points into that stable heap allocation.
        //
        // We transmute to 'static because the data lives as long as the
        // StringInterner (entries are never removed). The HashMap and its
        // references are dropped together with the Vec when the interner
        // is dropped.
        let k: &'static [u16] = unsafe { std::slice::from_raw_parts(ptr, len) };

        self.data.insert(k, id);
        id
    }

    /// Look up the interned string by ID.
    ///
    /// # Panics
    /// Panics if the ID is not valid.
    #[inline]
    pub fn lookup(&self, id: InternedId) -> &[u16] {
        &self.list[id.0 as usize]
    }

    /// Find the `InternedId` for a string, returning `None` if not interned.
    #[inline]
    pub fn find_id(&self, s: &[u16]) -> Option<InternedId> {
        self.data.get(s).copied()
    }

    /// Look up the interned string by ID, returning `None` if invalid.
    #[inline]
    pub fn try_lookup(&self, id: InternedId) -> Option<&[u16]> {
        self.list.get(id.0 as usize).map(|s| &**s)
    }

    /// Intern and return a `SharedUtf16String` backed by the interner's
    /// cached Rc. Identical strings share the same Rc allocation.
    #[inline]
    pub fn intern_shared(&mut self, input: &[u16]) -> SharedUtf16String {
        let id = self.intern(input);
        self.shared[id.0 as usize].clone()
    }

    /// Get a cached `SharedUtf16String` for an already-interned ID.
    #[inline]
    pub fn get_shared(&self, id: InternedId) -> SharedUtf16String {
        self.shared[id.0 as usize].clone()
    }

    /// Number of unique strings interned.
    pub fn len(&self) -> usize {
        self.list.len()
    }

    pub fn is_empty(&self) -> bool {
        self.list.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn intern_and_lookup() {
        let mut interner = StringInterner::new();
        let hello: Vec<u16> = "hello".encode_utf16().collect();
        let id = interner.intern(&hello);
        assert_eq!(interner.lookup(id), hello.as_slice());
    }

    #[test]
    fn deduplication() {
        let mut interner = StringInterner::new();
        let hello: Vec<u16> = "hello".encode_utf16().collect();
        let id1 = interner.intern(&hello);
        let id2 = interner.intern(&hello);
        assert_eq!(id1, id2);
        assert_eq!(interner.len(), 1);
    }

    #[test]
    fn distinct_strings() {
        let mut interner = StringInterner::new();
        let hello: Vec<u16> = "hello".encode_utf16().collect();
        let world: Vec<u16> = "world".encode_utf16().collect();
        let id1 = interner.intern(&hello);
        let id2 = interner.intern(&world);
        assert_ne!(id1, id2);
        assert_eq!(interner.len(), 2);
        assert_eq!(interner.lookup(id1), hello.as_slice());
        assert_eq!(interner.lookup(id2), world.as_slice());
    }

    #[test]
    fn survives_reallocation() {
        let mut interner = StringInterner::with_capacity(1);
        let s1: Vec<u16> = "alpha".encode_utf16().collect();
        let id1 = interner.intern(&s1);

        // Force reallocation by interning many strings.
        for i in 0..100 {
            let s: Vec<u16> = format!("str_{i}").encode_utf16().collect();
            interner.intern(&s);
        }

        assert_eq!(interner.lookup(id1), s1.as_slice());
    }

    #[test]
    fn cloned_interner_preserves_ids() {
        let mut interner = StringInterner::new();
        let first: Vec<u16> = "first".encode_utf16().collect();
        let second: Vec<u16> = "second".encode_utf16().collect();
        let id1 = interner.intern(&first);
        let id2 = interner.intern(&second);

        let clone = interner.clone();
        assert_eq!(clone.lookup(id1), first.as_slice());
        assert_eq!(clone.lookup(id2), second.as_slice());
        assert_eq!(clone.find_id(&first), Some(id1));
        assert_eq!(clone.find_id(&second), Some(id2));
    }

    #[test]
    fn empty_string() {
        let mut interner = StringInterner::new();
        let empty: Vec<u16> = Vec::new();
        let id = interner.intern(&empty);
        assert_eq!(interner.lookup(id), &[] as &[u16]);
    }
}
