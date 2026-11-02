/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shared hash index and dense payload storage for style-engine interners.

use hashbrown::HashTable;
use std::ops::Index;
use std::ops::IndexMut;

pub(super) trait InternIdentity: Copy {
    fn index(self) -> usize;
    fn from_index(index: usize) -> Self;
}

/// An entry's identity and 32 bits of its hash, which with a 32-bit identity fit in 8 bytes.
#[derive(Clone, Copy)]
struct HashedIdentity<Identity> {
    hash: u32,
    identity: Identity,
}

/// The 32 bits of `hash` an entry keeps.
fn short_hash(hash: u64) -> u32 {
    (hash ^ (hash >> 32)) as u32
}

/// The hash the table places an entry by, spread from the 32 bits it keeps over all 64, as the
/// table reads its top bits.
fn table_hash(short_hash: u32) -> u64 {
    u64::from(short_hash).wrapping_mul(0x9e37_79b9_7f4a_7c15)
}

pub(super) struct InternTable<Identity, Payload> {
    entries: Vec<Payload>,
    identities: HashTable<HashedIdentity<Identity>>,
}

impl<Identity, Payload> Default for InternTable<Identity, Payload> {
    fn default() -> Self {
        Self {
            entries: Vec::new(),
            identities: HashTable::new(),
        }
    }
}

impl<Identity: InternIdentity, Payload> InternTable<Identity, Payload> {
    /// The bytes of the payload column and the hash index, with what `payload_heap` says each
    /// payload holds.
    pub(super) fn heap_size(
        &self,
        sizer: &mut crate::heap_size::HeapSizer,
        mut payload_heap: impl FnMut(&Payload, &mut crate::heap_size::HeapSizer) -> usize,
    ) -> usize {
        crate::heap_size::vec_bytes(&self.entries)
            + self.identities.allocation_size()
            + self
                .entries
                .iter()
                .map(|payload| payload_heap(payload, sizer))
                .sum::<usize>()
    }

    pub(super) fn find(&self, hash: u64, mut matches: impl FnMut(Identity, &Payload) -> bool) -> Option<Identity> {
        let hash = short_hash(hash);
        self.identities
            .find(table_hash(hash), |candidate| {
                candidate.hash == hash && matches(candidate.identity, &self.entries[candidate.identity.index()])
            })
            .map(|candidate| candidate.identity)
    }

    pub(super) fn insert(&mut self, hash: u64, identity: Identity, payload: Payload) {
        if identity.index() == self.entries.len() {
            self.entries.push(payload);
        } else {
            self.entries[identity.index()] = payload;
        }
        self.identities.insert_unique(
            table_hash(short_hash(hash)),
            HashedIdentity {
                hash: short_hash(hash),
                identity,
            },
            |candidate| table_hash(candidate.hash),
        );
    }

    pub(super) fn get(&self, identity: Identity) -> &Payload {
        &self.entries[identity.index()]
    }

    pub(super) fn get_mut(&mut self, identity: Identity) -> &mut Payload {
        &mut self.entries[identity.index()]
    }

    pub(super) fn len(&self) -> usize {
        self.entries.len()
    }

    /// Keep only the payloads `keep` says to, packing the ones kept in their order: each keeps
    /// its hash, and the identity of its new index.
    pub(super) fn retain(&mut self, mut keep: impl FnMut(&Payload) -> bool) {
        let mut new_index = Vec::with_capacity(self.entries.len());
        let mut kept = 0_usize;
        let entries = std::mem::take(&mut self.entries);
        for payload in entries {
            if keep(&payload) {
                new_index.push(Some(kept));
                self.entries.push(payload);
                kept += 1;
            } else {
                new_index.push(None);
            }
        }
        self.entries.shrink_to_fit();
        self.identities
            .retain(|candidate| match new_index[candidate.identity.index()] {
                Some(index) => {
                    candidate.identity = Identity::from_index(index);
                    true
                }
                None => false,
            });
        self.identities.shrink_to_fit(|candidate| table_hash(candidate.hash));
    }
}

impl<Identity: InternIdentity, Payload: Clone> Clone for InternTable<Identity, Payload> {
    fn clone(&self) -> Self {
        let entries = self.entries.clone();
        let mut identities = HashTable::with_capacity(self.identities.len());
        for candidate in &self.identities {
            identities.insert_unique(table_hash(candidate.hash), *candidate, |candidate| {
                table_hash(candidate.hash)
            });
        }
        Self { entries, identities }
    }
}

impl<Identity: InternIdentity, Payload> Index<Identity> for InternTable<Identity, Payload> {
    type Output = Payload;

    fn index(&self, identity: Identity) -> &Self::Output {
        self.get(identity)
    }
}

impl<Identity: InternIdentity, Payload> IndexMut<Identity> for InternTable<Identity, Payload> {
    fn index_mut(&mut self, identity: Identity) -> &mut Self::Output {
        self.get_mut(identity)
    }
}

impl<Identity: InternIdentity, Payload> Index<usize> for InternTable<Identity, Payload> {
    type Output = Payload;

    fn index(&self, index: usize) -> &Self::Output {
        &self.entries[index]
    }
}

impl<Identity: InternIdentity, Payload> IndexMut<usize> for InternTable<Identity, Payload> {
    fn index_mut(&mut self, index: usize) -> &mut Self::Output {
        &mut self.entries[index]
    }
}

impl<'a, Identity: InternIdentity, Payload> IntoIterator for &'a InternTable<Identity, Payload> {
    type Item = &'a Payload;
    type IntoIter = std::slice::Iter<'a, Payload>;

    fn into_iter(self) -> Self::IntoIter {
        self.entries.iter()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    struct TestIdentity(u32);

    impl InternIdentity for TestIdentity {
        fn index(self) -> usize {
            self.0 as usize
        }

        fn from_index(index: usize) -> Self {
            Self(u32::try_from(index).unwrap())
        }
    }

    #[test]
    fn content_collisions_compare_dense_payloads() {
        let mut table = InternTable::default();
        table.insert(7, TestIdentity(0), "first");
        table.insert(7, TestIdentity(1), "second");

        assert_eq!(
            table.find(7, |_identity, payload| *payload == "second"),
            Some(TestIdentity(1))
        );
        assert_eq!(table.find(8, |_identity, _payload| true), None);
    }

    #[test]
    fn retained_payloads_are_found_under_their_new_identities() {
        let mut table = InternTable::default();
        table.insert(1, TestIdentity(0), "dropped");
        table.insert(2, TestIdentity(1), "kept");
        table.retain(|payload| *payload == "kept");

        assert_eq!(table.len(), 1);
        assert_eq!(
            table.find(2, |_identity, payload| *payload == "kept"),
            Some(TestIdentity(0))
        );
        assert_eq!(table.find(1, |_identity, _payload| true), None);
    }
}
