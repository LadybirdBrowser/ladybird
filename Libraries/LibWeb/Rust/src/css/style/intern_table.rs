/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shared hash index and dense payload storage for style-engine interners.

use hashbrown::HashTable;
use std::hash::Hash;
use std::hash::Hasher;
use std::ops::Index;
use std::ops::IndexMut;

use super::capacity::ShallowCapacityBytes;
use super::fast_hash::fast_hasher;

pub(super) fn content_hash(content: impl Hash) -> u64 {
    let mut hasher = fast_hasher();
    content.hash(&mut hasher);
    hasher.finish()
}

pub(super) trait InternIdentity: Copy {
    fn index(self) -> usize;
}

#[derive(Clone, Copy)]
struct HashedIdentity<Identity> {
    hash: u64,
    identity: Identity,
}

pub(super) struct InternTable<Identity, Payload> {
    entries: Vec<Payload>,
    identities: HashTable<HashedIdentity<Identity>>,
    free_identities: Vec<Identity>,
}

impl<Identity, Payload> Default for InternTable<Identity, Payload> {
    fn default() -> Self {
        Self {
            entries: Vec::new(),
            identities: HashTable::new(),
            free_identities: Vec::new(),
        }
    }
}

impl<Identity: InternIdentity, Payload> InternTable<Identity, Payload> {
    pub(super) fn find(&self, hash: u64, mut matches: impl FnMut(Identity, &Payload) -> bool) -> Option<Identity> {
        self.identities
            .find(hash, |candidate| {
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
        self.identities
            .insert_unique(hash, HashedIdentity { hash, identity }, |candidate| candidate.hash);
    }

    pub(super) fn get(&self, identity: Identity) -> &Payload {
        &self.entries[identity.index()]
    }

    pub(super) fn get_mut(&mut self, identity: Identity) -> &mut Payload {
        &mut self.entries[identity.index()]
    }

    pub(super) fn get_index(&self, index: usize) -> Option<&Payload> {
        self.entries.get(index)
    }

    pub(super) fn len(&self) -> usize {
        self.entries.len()
    }

    pub(super) fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub(super) fn iter(&self) -> impl Iterator<Item = &Payload> {
        self.entries.iter()
    }

    pub(super) fn iter_mut(&mut self) -> impl Iterator<Item = &mut Payload> {
        self.entries.iter_mut()
    }

    pub(super) fn live_identities(&self) -> impl Iterator<Item = Identity> + '_ {
        self.identities.iter().map(|candidate| candidate.identity)
    }

    pub(super) fn take_free_identity(&mut self) -> Option<Identity> {
        self.free_identities.pop()
    }

    pub(super) fn shrink_to_fit(&mut self) {
        self.entries.shrink_to_fit();
        self.identities.shrink_to_fit(|candidate| candidate.hash);
    }

    #[cfg(test)]
    pub(super) fn live_len(&self) -> usize {
        self.identities.len()
    }

    #[cfg(test)]
    pub(super) fn live_is_empty(&self) -> bool {
        self.identities.is_empty()
    }

    #[cfg(test)]
    pub(super) fn reserve(&mut self, additional: usize) {
        self.entries.reserve(additional);
        self.identities.reserve(additional, |candidate| candidate.hash);
    }

    #[cfg(test)]
    pub(super) fn capacity_bytes(&self) -> u64 {
        ShallowCapacityBytes::shallow_capacity_bytes(self)
    }

    #[cfg(test)]
    pub(super) fn index_is_empty(&self) -> bool {
        self.identities.is_empty()
    }
}

impl<Identity: InternIdentity> InternTable<Identity, ()> {
    pub(super) fn insert_identity(&mut self, hash: u64, identity: Identity) {
        if identity.index() >= self.entries.len() {
            self.entries.resize(identity.index() + 1, ());
        }
        self.identities
            .insert_unique(hash, HashedIdentity { hash, identity }, |candidate| candidate.hash);
    }
}

impl<Identity: InternIdentity + PartialEq, Payload> InternTable<Identity, Payload> {
    pub(super) fn remove_identity(&mut self, hash: u64, identity: Identity) {
        let Ok(entry) = self.identities.find_entry(hash, |candidate| {
            candidate.hash == hash && candidate.identity == identity
        }) else {
            panic!("interned identity must have an index entry");
        };
        entry.remove();
    }

    pub(super) fn retire_identity(&mut self, hash: u64, identity: Identity) {
        self.remove_identity(hash, identity);
        self.free_identities.push(identity);
    }
}

impl<Identity: InternIdentity, Payload> ShallowCapacityBytes for InternTable<Identity, Payload> {
    fn shallow_capacity_bytes(&self) -> u64 {
        self.entries.shallow_capacity_bytes()
            + (self.identities.capacity() * (size_of::<HashedIdentity<Identity>>() + 1)) as u64
            + self.free_identities.shallow_capacity_bytes()
    }
}

impl<Identity: InternIdentity, Payload: Clone> Clone for InternTable<Identity, Payload> {
    fn clone(&self) -> Self {
        let entries = self.entries.clone();
        let free_identities = self.free_identities.clone();
        let mut identities = HashTable::with_capacity(self.identities.len());
        for candidate in &self.identities {
            identities.insert_unique(candidate.hash, *candidate, |candidate| candidate.hash);
        }
        Self {
            entries,
            identities,
            free_identities,
        }
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
    fn retired_identities_can_replace_their_dense_payload() {
        let mut table = InternTable::default();
        let identity = TestIdentity(0);
        table.insert(7, identity, "first");

        table.retire_identity(7, identity);
        assert_eq!(table.find(7, |_identity, _payload| true), None);
        let recycled = table.take_free_identity().unwrap();
        assert_eq!(recycled, identity);

        table.insert(9, recycled, "second");
        assert_eq!(table.len(), 1);
        assert_eq!(table.find(9, |_identity, payload| *payload == "second"), Some(identity));
        assert_eq!(table.live_identities().collect::<Vec<_>>(), vec![identity]);
    }
}
