/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Maps and sets whose fork shares their storage.
//!
//! A [`CowMap`] keeps its entries in a hash trie behind [`Arc`]s, so cloning it costs one reference count however many
//! entries it holds. A write copies only the nodes on the way to its key that a fork still shares: at most one small node
//! a level, of which a 64-bit hash has 13. Leaves hold a few entries each, so what a node costs is spread over them. Keys
//! hash with a fixed seed, so a map and its forks agree on where a key lives.

use smallvec::SmallVec;
use std::borrow::Borrow;
use std::hash::{BuildHasher, Hash};
use std::sync::Arc;

/// How many bits of a key's hash pick a child at each level of the trie.
const BITS_PER_LEVEL: u32 = 5;
const LEVEL_MASK: u64 = (1 << BITS_PER_LEVEL) - 1;

/// How many entries a leaf holds before it splits, which is also how many a write to it copies.
const ENTRIES_PER_LEAF: usize = 8;

#[derive(Clone)]
enum Node<K, V> {
    Leaf(Vec<(K, V)>),
    /// The children of the slots set in `occupied`, in slot order.
    Branch {
        occupied: u32,
        children: Vec<Arc<Self>>,
    },
}

fn hash_of<Q: Hash + ?Sized>(key: &Q) -> u64 {
    foldhash::fast::FixedState::default().hash_one(key)
}

fn slot_of(hash: u64, shift: u32) -> u32 {
    ((hash >> shift) & LEVEL_MASK) as u32
}

fn position_of(occupied: u32, slot: u32) -> usize {
    (occupied & ((1 << slot) - 1)).count_ones() as usize
}

impl<K: Clone + Eq + Hash, V: Clone> Node<K, V> {
    /// Replaces the leaf `node` with a branch over `entries`, which were the leaf's.
    fn split_into_branch(node: &mut Arc<Self>, entries: Vec<(K, V)>, shift: u32) {
        *node = Arc::new(Self::Branch {
            occupied: 0,
            children: Vec::new(),
        });
        for (key, value) in entries {
            Self::insert(node, hash_of(&key), shift, key, value);
        }
    }

    fn insert(node: &mut Arc<Self>, hash: u64, shift: u32, key: K, value: V) -> Option<V> {
        match Arc::make_mut(node) {
            Self::Leaf(entries) => {
                if let Some((_, existing)) = entries.iter_mut().find(|(existing, _)| *existing == key) {
                    return Some(std::mem::replace(existing, value));
                }
                entries.push((key, value));
                // NB: Keys that share what is left of their hash stay together.
                if entries.len() > ENTRIES_PER_LEAF && shift + BITS_PER_LEVEL < u64::BITS {
                    let entries = std::mem::take(entries);
                    Self::split_into_branch(node, entries, shift);
                }
                None
            }
            Self::Branch { occupied, children } => {
                let slot = slot_of(hash, shift);
                let position = position_of(*occupied, slot);
                if *occupied & (1 << slot) != 0 {
                    return Self::insert(&mut children[position], hash, shift + BITS_PER_LEVEL, key, value);
                }
                *occupied |= 1 << slot;
                children.insert(position, Arc::new(Self::Leaf(vec![(key, value)])));
                None
            }
        }
    }

    fn get<'a, Q>(mut node: &'a Self, hash: u64, key: &Q) -> Option<&'a V>
    where
        K: Borrow<Q>,
        Q: Eq + ?Sized,
    {
        let mut shift = 0;
        loop {
            match node {
                Self::Leaf(entries) => {
                    return entries
                        .iter()
                        .find(|(existing, _)| existing.borrow() == key)
                        .map(|(_, value)| value);
                }
                Self::Branch { occupied, children } => {
                    let slot = slot_of(hash, shift);
                    if *occupied & (1 << slot) == 0 {
                        return None;
                    }
                    node = &children[position_of(*occupied, slot)];
                    shift += BITS_PER_LEVEL;
                }
            }
        }
    }

    /// The value of `key`, for writing. Nodes on the way to a key the node does not hold are copied as for one it does.
    fn get_mut<'a, Q>(node: &'a mut Arc<Self>, hash: u64, shift: u32, key: &Q) -> Option<&'a mut V>
    where
        K: Borrow<Q>,
        Q: Eq + ?Sized,
    {
        match Arc::make_mut(node) {
            Self::Leaf(entries) => entries
                .iter_mut()
                .find(|(existing, _)| (*existing).borrow() == key)
                .map(|(_, value)| value),
            Self::Branch { occupied, children } => {
                let slot = slot_of(hash, shift);
                if *occupied & (1 << slot) == 0 {
                    return None;
                }
                let position = position_of(*occupied, slot);
                Self::get_mut(&mut children[position], hash, shift + BITS_PER_LEVEL, key)
            }
        }
    }

    /// The value of `key`, inserting what `make` answers where the node holds none, and whether it did.
    fn get_or_insert_with(
        node: &mut Arc<Self>,
        hash: u64,
        shift: u32,
        key: K,
        make: impl FnOnce() -> V,
    ) -> (&mut V, bool) {
        let splits = matches!(&**node, Self::Leaf(entries)
            if entries.len() >= ENTRIES_PER_LEAF
                && shift + BITS_PER_LEVEL < u64::BITS
                && !entries.iter().any(|(existing, _)| *existing == key));
        if splits {
            let Self::Leaf(entries) = Arc::make_mut(node) else {
                unreachable!("the node was just found to be a leaf");
            };
            let entries = std::mem::take(entries);
            Self::split_into_branch(node, entries, shift);
        }
        match Arc::make_mut(node) {
            Self::Leaf(entries) => match entries.iter().position(|(existing, _)| *existing == key) {
                Some(index) => (&mut entries[index].1, false),
                None => {
                    entries.push((key, make()));
                    (&mut entries.last_mut().expect("an entry was just pushed").1, true)
                }
            },
            Self::Branch { occupied, children } => {
                let slot = slot_of(hash, shift);
                let position = position_of(*occupied, slot);
                if *occupied & (1 << slot) == 0 {
                    *occupied |= 1 << slot;
                    children.insert(position, Arc::new(Self::Leaf(Vec::new())));
                }
                Self::get_or_insert_with(&mut children[position], hash, shift + BITS_PER_LEVEL, key, make)
            }
        }
    }

    /// Removes `key`, which the node must hold, answering its value.
    fn remove<Q>(node: &mut Arc<Self>, hash: u64, shift: u32, key: &Q) -> Option<V>
    where
        K: Borrow<Q>,
        Q: Eq + ?Sized,
    {
        match Arc::make_mut(node) {
            Self::Leaf(entries) => {
                let index = entries.iter().position(|(existing, _)| existing.borrow() == key)?;
                Some(entries.remove(index).1)
            }
            Self::Branch { occupied, children } => {
                let slot = slot_of(hash, shift);
                let position = position_of(*occupied, slot);
                let removed = Self::remove(&mut children[position], hash, shift + BITS_PER_LEVEL, key);
                if children[position].is_empty() {
                    children.remove(position);
                    *occupied &= !(1 << slot);
                }
                removed
            }
        }
    }

    fn is_empty(&self) -> bool {
        match self {
            Self::Leaf(entries) => entries.is_empty(),
            Self::Branch { children, .. } => children.is_empty(),
        }
    }

    fn heap_size(&self) -> usize {
        crate::heap_size::ARC_HEADER_BYTES
            + size_of::<Self>()
            + match self {
                Self::Leaf(entries) => entries.capacity() * size_of::<(K, V)>(),
                Self::Branch { children, .. } => {
                    children.capacity() * size_of::<Arc<Self>>()
                        + children.iter().map(|child| child.heap_size()).sum::<usize>()
                }
            }
    }
}

/// A hash map whose clone shares its storage. See the [module documentation](self).
pub(crate) struct CowMap<K, V> {
    root: Option<Arc<Node<K, V>>>,
    len: usize,
}

impl<K, V> Default for CowMap<K, V> {
    fn default() -> Self {
        Self { root: None, len: 0 }
    }
}

impl<K, V> Clone for CowMap<K, V> {
    fn clone(&self) -> Self {
        Self {
            root: self.root.clone(),
            len: self.len,
        }
    }
}

impl<K: Clone + Eq + Hash, V: Clone> CowMap<K, V> {
    pub(crate) fn len(&self) -> usize {
        self.len
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// The bytes of the trie, whose entries hold nothing else on the heap.
    pub(crate) fn heap_size(&self) -> usize {
        self.root.as_ref().map_or(0, |root| root.heap_size())
    }

    pub(crate) fn clear(&mut self) {
        *self = Self::default();
    }

    pub(crate) fn get<Q>(&self, key: &Q) -> Option<&V>
    where
        K: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        Node::get(self.root.as_deref()?, hash_of(key), key)
    }

    pub(crate) fn contains_key<Q>(&self, key: &Q) -> bool
    where
        K: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        self.get(key).is_some()
    }

    /// The value of `key`, for writing, copying what a fork shares on the way to it.
    pub(crate) fn get_mut<Q>(&mut self, key: &Q) -> Option<&mut V>
    where
        K: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        Node::get_mut(self.root.as_mut()?, hash_of(key), 0, key)
    }

    pub(crate) fn insert(&mut self, key: K, value: V) -> Option<V> {
        let hash = hash_of(&key);
        let Some(root) = &mut self.root else {
            self.root = Some(Arc::new(Node::Leaf(vec![(key, value)])));
            self.len = 1;
            return None;
        };
        let previous = Node::insert(root, hash, 0, key, value);
        self.len += usize::from(previous.is_none());
        previous
    }

    pub(crate) fn remove<Q>(&mut self, key: &Q) -> Option<V>
    where
        K: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        let hash = hash_of(key);
        let root = self.root.as_mut()?;
        Node::get(root.as_ref(), hash, key)?;
        let removed = Node::remove(root, hash, 0, key);
        self.len -= 1;
        if root.is_empty() {
            self.root = None;
        }
        removed
    }

    /// The value of `key`, inserting what `make` answers where the map holds none.
    pub(crate) fn get_or_insert_with(&mut self, key: K, make: impl FnOnce() -> V) -> &mut V {
        let hash = hash_of(&key);
        let root = self.root.get_or_insert_with(|| Arc::new(Node::Leaf(Vec::new())));
        let (value, inserted) = Node::get_or_insert_with(root, hash, 0, key, make);
        self.len += usize::from(inserted);
        value
    }

    pub(crate) fn entry(&mut self, key: K) -> Entry<'_, K, V> {
        Entry { map: self, key }
    }

    /// Every entry, in no particular order.
    pub(crate) fn iter(&self) -> impl Iterator<Item = (&K, &V)> {
        let mut stack: SmallVec<[&Node<K, V>; 16]> = self.root.as_deref().into_iter().collect();
        let mut entries: std::slice::Iter<'_, (K, V)> = [].iter();
        std::iter::from_fn(move || {
            loop {
                if let Some((key, value)) = entries.next() {
                    return Some((key, value));
                }
                match stack.pop()? {
                    Node::Leaf(leaf) => entries = leaf.iter(),
                    Node::Branch { children, .. } => stack.extend(children.iter().map(|child| &**child)),
                }
            }
        })
    }

    pub(crate) fn keys(&self) -> impl Iterator<Item = &K> {
        self.iter().map(|(key, _)| key)
    }

    pub(crate) fn values(&self) -> impl Iterator<Item = &V> {
        self.iter().map(|(_, value)| value)
    }

    /// Keeps the entries `keep` answers true for.
    pub(crate) fn retain(&mut self, mut keep: impl FnMut(&K, &V) -> bool) {
        let gone: Vec<K> = self
            .iter()
            .filter(|(key, value)| !keep(key, value))
            .map(|(key, _)| key.clone())
            .collect();
        for key in &gone {
            self.remove(key);
        }
    }
}

/// A key of a [`CowMap`], and what it holds, for writing.
pub(crate) struct Entry<'a, K, V> {
    map: &'a mut CowMap<K, V>,
    key: K,
}

impl<'a, K: Clone + Eq + Hash, V: Clone> Entry<'a, K, V> {
    pub(crate) fn or_insert_with(self, make: impl FnOnce() -> V) -> &'a mut V {
        self.map.get_or_insert_with(self.key, make)
    }

    pub(crate) fn or_default(self) -> &'a mut V
    where
        V: Default,
    {
        self.or_insert_with(V::default)
    }
}

impl<K: Clone + Eq + Hash, V: Clone> std::ops::Index<&K> for CowMap<K, V> {
    type Output = V;

    fn index(&self, key: &K) -> &V {
        self.get(key).expect("the map holds the key")
    }
}

impl<K: Clone + Eq + Hash, V: Clone> Extend<(K, V)> for CowMap<K, V> {
    fn extend<I: IntoIterator<Item = (K, V)>>(&mut self, entries: I) {
        for (key, value) in entries {
            self.insert(key, value);
        }
    }
}

/// A hash set whose clone shares its storage. See the [module documentation](self).
pub(crate) struct CowSet<T>(CowMap<T, ()>);

impl<T> Default for CowSet<T> {
    fn default() -> Self {
        Self(CowMap::default())
    }
}

impl<T> Clone for CowSet<T> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}

impl<T: Clone + Eq + Hash> CowSet<T> {
    /// The bytes of the trie, whose values hold nothing else on the heap.
    pub(crate) fn heap_size(&self) -> usize {
        self.0.heap_size()
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    pub(crate) fn clear(&mut self) {
        self.0.clear();
    }

    pub(crate) fn contains<Q>(&self, value: &Q) -> bool
    where
        T: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        self.0.contains_key(value)
    }

    /// Answers whether the set did not hold `value` yet.
    pub(crate) fn insert(&mut self, value: T) -> bool {
        self.0.insert(value, ()).is_none()
    }

    /// Answers whether the set held `value`.
    pub(crate) fn remove<Q>(&mut self, value: &Q) -> bool
    where
        T: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        self.0.remove(value).is_some()
    }

    /// Every value, in no particular order.
    pub(crate) fn iter(&self) -> impl Iterator<Item = &T> {
        self.0.keys()
    }
}

const _: () = {
    const fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<CowMap<u64, u64>>();
    assert_send_and_sync::<CowSet<u64>>();
};

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    /// A key whose hash is always the same, so that every entry lands in one leaf.
    #[derive(Clone, PartialEq, Eq)]
    struct Colliding(u32);

    impl Hash for Colliding {
        fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
            0u32.hash(state);
        }
    }

    #[test]
    fn a_map_acts_as_a_hash_map_through_many_writes() {
        let mut map = CowMap::default();
        let mut expected = HashMap::new();
        let mut state = 1u64;
        for _ in 0..50_000 {
            state = state
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            let key = (state >> 40) % 3000;
            match state >> 62 {
                0 => assert_eq!(map.remove(&key), expected.remove(&key)),
                1 => assert_eq!(map.get(&key), expected.get(&key)),
                _ => assert_eq!(map.insert(key, state), expected.insert(key, state)),
            }
            assert_eq!(map.len(), expected.len());
        }
        let mut entries: Vec<_> = map.iter().map(|(&key, &value)| (key, value)).collect();
        let mut expected: Vec<_> = expected.into_iter().collect();
        entries.sort_unstable();
        expected.sort_unstable();
        assert_eq!(entries, expected);
    }

    #[test]
    fn a_fork_shares_the_root_and_neither_side_sees_the_others_writes() {
        let mut map = CowMap::default();
        for key in 0..1000u32 {
            map.insert(key, key);
        }
        let mut fork = map.clone();
        assert!(Arc::ptr_eq(map.root.as_ref().unwrap(), fork.root.as_ref().unwrap()));
        *fork.get_mut(&1).unwrap() = 100;
        fork.remove(&2);
        map.insert(1000, 1000);
        map.remove(&3);
        assert_eq!(
            (map.get(&1), map.get(&2), map.get(&3), map.get(&1000)),
            (Some(&1), Some(&2), None, Some(&1000))
        );
        assert_eq!(
            (fork.get(&1), fork.get(&2), fork.get(&3), fork.get(&1000)),
            (Some(&100), None, Some(&3), None)
        );
        assert_eq!((map.len(), fork.len()), (1000, 999));
    }

    #[test]
    fn keys_of_one_hash_share_a_leaf() {
        let mut map = CowMap::default();
        for key in 0..2 * ENTRIES_PER_LEAF as u32 {
            map.insert(Colliding(key), key);
        }
        assert_eq!(map.get(&Colliding(5)), Some(&5));
        assert_eq!(map.remove(&Colliding(5)), Some(5));
        assert_eq!(map.get(&Colliding(5)), None);
        assert_eq!(map.iter().count(), 2 * ENTRIES_PER_LEAF - 1);
    }

    #[test]
    fn removing_every_entry_empties_the_map() {
        let mut map = CowMap::default();
        for key in 0..100u32 {
            map.insert(key, ());
        }
        for key in 0..100u32 {
            map.remove(&key);
        }
        assert!(map.is_empty());
        assert!(map.root.is_none());
    }

    #[test]
    fn a_set_answers_as_a_hash_set_does() {
        let mut set = CowSet::default();
        assert!(set.insert(1));
        assert!(!set.insert(1));
        assert!(set.contains(&1));
        assert!(set.remove(&1));
        assert!(!set.remove(&1));
        assert!(set.is_empty());
    }
}
