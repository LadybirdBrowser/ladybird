/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A map from keys to lists of values, all held in one flat backing, of which each key names an
//! extent: a key costs its extent, rather than a vector and the allocation behind it.
//!
//! NB: A list growing past the room of its extent moves to the end of the backing with twice the
//!     room, so appends in any order cost amortized constant time, and `shrink_to_fit` packs the
//!     lists again, once a map is built.

use super::super::*;
use super::shared_pages::{PagePool, SharedPages};
use std::ops::Range;
use std::sync::Arc;

/// The hash a frozen map looks a key up by, for a caller looking the same key up in many maps.
#[derive(Clone, Copy)]
pub(super) struct KeyHash(u64);

/// Where one key's list stands in the backing, and how much room it has there.
#[derive(Clone, Copy, Default, PartialEq, Eq, Hash)]
pub(super) struct Extent {
    start: u32,
    len: u32,
    room: u32,
}

impl Extent {
    fn range(self) -> Range<usize> {
        self.start as usize..(self.start + self.len) as usize
    }
}

#[derive(Clone)]
pub(super) struct ExtentMap<K, V> {
    extents: HashMap<K, Extent>,
    backing: Vec<V>,
    /// The slots of the backing no list holds: the room lists left as they moved, shrank or went,
    /// and the room past their ends.
    unused: usize,
}

impl<K, V> Default for ExtentMap<K, V> {
    fn default() -> Self {
        Self {
            extents: HashMap::default(),
            backing: Vec::new(),
            unused: 0,
        }
    }
}

impl<K: Copy + Eq + std::hash::Hash, V: Copy> ExtentMap<K, V> {
    /// The list of `key`, where the map has it, which may be empty.
    pub(super) fn get(&self, key: &K) -> Option<&[V]> {
        self.extents.get(key).map(|extent| &self.backing[extent.range()])
    }

    pub(super) fn get_mut(&mut self, key: &K) -> Option<&mut [V]> {
        self.extents.get(key).map(|extent| &mut self.backing[extent.range()])
    }

    pub(super) fn contains_key(&self, key: &K) -> bool {
        self.extents.contains_key(key)
    }

    /// Whether the map has no list, not even an empty one.
    pub(super) fn is_empty(&self) -> bool {
        self.extents.is_empty()
    }

    pub(super) fn keys(&self) -> impl Iterator<Item = &K> {
        self.extents.keys()
    }

    /// Append `value` to the list of `key`, which the map starts where it has none.
    pub(super) fn push(&mut self, key: K, value: V) {
        let extent = self.extents.entry(key).or_default();
        if extent.len == extent.room {
            let room = (extent.room * 2).max(1);
            let end = (extent.start + extent.room) as usize;
            if end == self.backing.len() && extent.room != 0 {
                // The list ends the backing, which grows under it.
                self.backing.resize(end + extent.room as usize, value);
            } else {
                let start = self.backing.len();
                self.backing.extend_from_within(extent.range());
                self.backing.resize(start + room as usize, value);
                self.unused += extent.room as usize;
                extent.start = u32::try_from(start).expect("extent map space exhausted");
            }
            self.unused += (room - extent.room) as usize;
            extent.room = room;
        }
        self.backing[(extent.start + extent.len) as usize] = value;
        extent.len += 1;
        self.unused -= 1;
    }

    /// Note an empty list for `key`, where the map has none.
    pub(super) fn ensure(&mut self, key: K) {
        self.extents.entry(key).or_default();
    }

    /// Keep only the values of the list of `key` that `keep` holds to, answering how many are
    /// left, or none where the map has no list for `key`.
    pub(super) fn retain(&mut self, key: &K, mut keep: impl FnMut(&V) -> bool) -> Option<usize> {
        let extent = self.extents.get_mut(key)?;
        let range = extent.range();
        let mut kept = range.start;
        for index in range.clone() {
            if keep(&self.backing[index]) {
                self.backing[kept] = self.backing[index];
                kept += 1;
            }
        }
        let len = kept - range.start;
        self.unused += range.len() - len;
        extent.len = u32::try_from(len).expect("extent map space exhausted");
        Some(len)
    }

    /// Sort the list of `key` by what `by` says of each value, and keep one of the values it says
    /// the same of.
    pub(super) fn sort_and_dedup_by_key<T: Ord>(&mut self, key: &K, mut by: impl FnMut(&V) -> T) {
        let Some(list) = self.get_mut(key) else {
            return;
        };
        list.sort_unstable_by_key(&mut by);
        let mut kept = 0;
        for index in 0..list.len() {
            if kept == 0 || by(&list[kept - 1]) != by(&list[index]) {
                list[kept] = list[index];
                kept += 1;
            }
        }
        let removed = list.len() - kept;
        if removed != 0 {
            let extent = self.extents.get_mut(key).expect("the list was just found");
            extent.len -= u32::try_from(removed).expect("extent map space exhausted");
            self.unused += removed;
        }
    }

    pub(super) fn remove(&mut self, key: &K) {
        // NB: The room past the list's end is unused already.
        if let Some(extent) = self.extents.remove(key) {
            self.unused += extent.len as usize;
        }
    }

    /// Whether at least half the backing is room no list holds, which `shrink_to_fit` gives back.
    pub(super) fn has_slack(&self) -> bool {
        self.unused != 0 && self.unused * 2 >= self.backing.len()
    }

    /// Pack the lists next to one another, with no room past their ends, and give back what
    /// they left unused.
    pub(super) fn shrink_to_fit(&mut self) {
        self.extents.shrink_to_fit();
        if self.unused == 0 {
            self.backing.shrink_to_fit();
            return;
        }
        let mut backing = Vec::with_capacity(self.backing.len() - self.unused);
        for extent in self.extents.values_mut() {
            let start = u32::try_from(backing.len()).expect("extent map space exhausted");
            backing.extend_from_slice(&self.backing[extent.range()]);
            *extent = Extent {
                start,
                len: extent.len,
                room: extent.len,
            };
        }
        self.backing = backing;
        self.unused = 0;
    }

    /// The bytes of the extents and the backing.
    pub(super) fn heap_size(&self) -> usize {
        crate::heap_size::map_bytes(&self.extents) + crate::heap_size::vec_bytes(&self.backing)
    }
}

impl<K: Copy + Eq + std::hash::Hash, V: Copy> std::ops::Index<&K> for ExtentMap<K, V> {
    type Output = [V];

    fn index(&self, key: &K) -> &[V] {
        self.get(key).expect("the extent map has a list for the key")
    }
}

/// An extent map that no longer changes, which holds a key, where its list starts and how long it
/// is, in that order, and finds them by a table of one word per slot: the slot's tag rejects other
/// keys without reading them, so a lookup that misses touches the table alone, and a hit one record.
#[derive(Clone)]
pub(super) struct FrozenExtentMap<K, V> {
    /// A record's number plus one in the low bits, none being zero, under the high bits of its
    /// key's hash. Its length is a power of two, at most three of four slots taken.
    slots: Box<[u32]>,
    records: Box<[FrozenRecord<K>]>,
    backing: Box<[V]>,
}

#[derive(Clone, Copy)]
struct FrozenRecord<K> {
    key: K,
    start: u32,
    len: u32,
}

const FROZEN_TAG_SHIFT: u32 = 24;
const FROZEN_RECORD_MASK: u32 = (1 << FROZEN_TAG_SHIFT) - 1;

impl<K, V> Default for FrozenExtentMap<K, V> {
    fn default() -> Self {
        Self {
            slots: Box::default(),
            records: Box::default(),
            backing: Box::default(),
        }
    }
}

impl<K: Copy + Eq + std::hash::Hash, V: Copy> FrozenExtentMap<K, V> {
    pub(super) fn hash_of(key: &K) -> KeyHash {
        use std::hash::Hasher;
        let mut hasher = super::super::fast_hash::fast_hasher();
        key.hash(&mut hasher);
        KeyHash(hasher.finish())
    }

    /// The lists of `map`, packed.
    pub(super) fn of(mut map: ExtentMap<K, V>) -> Self {
        map.shrink_to_fit();
        let count = map.extents.len();
        assert!(count < FROZEN_RECORD_MASK as usize, "extent map space exhausted");
        let records: Box<[FrozenRecord<K>]> = map
            .extents
            .iter()
            .map(|(&key, extent)| FrozenRecord {
                key,
                start: extent.start,
                len: extent.len,
            })
            .collect();
        let length = match count {
            0 => 0,
            _ => (count * 4 / 3 + 1).next_power_of_two(),
        };
        let mut slots = vec![0_u32; length].into_boxed_slice();
        for (number, record) in records.iter().enumerate() {
            let KeyHash(hash) = Self::hash_of(&record.key);
            let mut slot = hash as usize & (length - 1);
            while slots[slot] != 0 {
                slot = (slot + 1) & (length - 1);
            }
            let tag = (hash >> (64 - (32 - FROZEN_TAG_SHIFT))) as u32;
            slots[slot] = tag << FROZEN_TAG_SHIFT | (number as u32 + 1);
        }
        Self {
            slots,
            records,
            backing: map.backing.into_boxed_slice(),
        }
    }

    /// The list of `key`, where the map has it, which may be empty.
    #[inline]
    pub(super) fn get(&self, key: &K) -> Option<&[V]> {
        self.get_hashed(key, Self::hash_of(key))
    }

    /// The list of `key`, whose hash is `hash`, where the map has it, which may be empty.
    #[inline]
    pub(super) fn get_hashed(&self, key: &K, KeyHash(hash): KeyHash) -> Option<&[V]> {
        if self.slots.is_empty() {
            return None;
        }
        let mask = self.slots.len() - 1;
        let tag = (hash >> (64 - (32 - FROZEN_TAG_SHIFT))) as u32;
        let mut slot = hash as usize & mask;
        loop {
            let held = self.slots[slot];
            if held == 0 {
                return None;
            }
            if held >> FROZEN_TAG_SHIFT == tag {
                let record = &self.records[(held & FROZEN_RECORD_MASK) as usize - 1];
                if record.key == *key {
                    let start = record.start as usize;
                    return Some(&self.backing[start..start + record.len as usize]);
                }
            }
            slot = (slot + 1) & mask;
        }
    }

    pub(super) fn contains_key(&self, key: &K) -> bool {
        self.get(key).is_some()
    }

    /// Whether the map has no list, not even an empty one.
    pub(super) fn is_empty(&self) -> bool {
        self.records.is_empty()
    }

    pub(super) fn keys(&self) -> impl Iterator<Item = &K> {
        self.records.iter().map(|record| &record.key)
    }

    /// The bytes of the table, the records and the lists.
    pub(super) fn heap_size(&self) -> usize {
        size_of_val::<[u32]>(&self.slots)
            + size_of_val::<[FrozenRecord<K>]>(&self.records)
            + size_of_val::<[V]>(&self.backing)
    }
}

/// An extent map that starts with the lists of a frozen one it shares with others, and goes on with
/// lists of its own: the list of a key is the shared one's, followed by its own.
#[derive(Clone)]
pub(super) struct SharedExtentMap<K, V> {
    shared: Arc<FrozenExtentMap<K, V>>,
    own: ExtentMap<K, V>,
}

impl<K, V> Default for SharedExtentMap<K, V> {
    fn default() -> Self {
        Self {
            shared: Arc::new(FrozenExtentMap::default()),
            own: ExtentMap::default(),
        }
    }
}

impl<K: Copy + Eq + std::hash::Hash, V: Copy> SharedExtentMap<K, V> {
    /// The list of `key`, the shared values first, where either map has one, which may be empty.
    pub(super) fn get(&self, key: &K) -> Option<impl Iterator<Item = &V> + Clone + use<'_, K, V>> {
        let (shared, own) = (self.shared.get(key), self.own.get(key));
        (shared.is_some() || own.is_some()).then(|| shared.into_iter().flatten().chain(own.into_iter().flatten()))
    }

    /// The list of `key`, which is empty where neither map has one.
    pub(super) fn values(&self, key: &K) -> impl Iterator<Item = &V> + Clone + use<'_, K, V> {
        self.get(key).into_iter().flatten()
    }

    pub(super) fn contains_key(&self, key: &K) -> bool {
        self.own.contains_key(key) || self.shared.contains_key(key)
    }

    pub(super) fn push(&mut self, key: K, value: V) {
        self.own.push(key, value);
    }

    /// Keep only the values of the map's own list of `key` that `keep` holds to, answering how many
    /// are left, or none where it has no list of its own for `key`. The shared lists never change.
    pub(super) fn retain_own(&mut self, key: &K, keep: impl FnMut(&V) -> bool) -> Option<usize> {
        self.own.retain(key, keep)
    }

    pub(super) fn sort_and_dedup_own_by_key<T: Ord>(&mut self, key: &K, by: impl FnMut(&V) -> T) {
        self.own.sort_and_dedup_by_key(key, by);
    }

    pub(super) fn remove_own(&mut self, key: &K) {
        self.own.remove(key);
    }

    /// The keys the map's own lists are of.
    pub(super) fn own_keys(&self) -> impl Iterator<Item = &K> {
        self.own.keys()
    }

    pub(super) fn has_slack(&self) -> bool {
        self.own.has_slack()
    }

    pub(super) fn shrink_to_fit(&mut self) {
        self.own.shrink_to_fit();
    }

    /// Make every list shared, packed: clones of the map then share all of them.
    pub(super) fn freeze(&mut self) {
        if self.own.keys().next().is_none() {
            return;
        }
        let mut all = ExtentMap::default();
        let keys: Vec<K> = self.shared.keys().chain(self.own.keys()).copied().collect();
        for key in keys {
            if all.contains_key(&key) {
                continue;
            }
            for &value in self.values(&key).collect::<Vec<_>>() {
                all.push(key, value);
            }
            if !all.contains_key(&key) {
                all.ensure(key);
            }
        }
        self.shared = Arc::new(FrozenExtentMap::of(all));
        self.own = ExtentMap::default();
    }

    /// The bytes of the map's own lists.
    pub(super) fn heap_size(&self) -> usize {
        self.own.heap_size()
    }

    /// The bytes of the lists the map shares.
    pub(super) fn shared_heap_size(&self) -> usize {
        self.shared.heap_size()
    }
}

/// An extent map keyed by dense numbers, its extents and its backing held in pages that the
/// documents of a process holding equal pages share: frames of a site whose rules are numbered
/// alike hold one copy of the lists of their rules.
#[derive(Clone)]
pub(super) struct PagedExtentMap<V> {
    extents: SharedPages<Extent>,
    backing: SharedPages<V>,
    unused: usize,
}

impl<V> Default for PagedExtentMap<V> {
    fn default() -> Self {
        Self {
            extents: SharedPages::default(),
            backing: SharedPages::default(),
            unused: 0,
        }
    }
}

impl<V: Copy + Default + PartialEq + std::hash::Hash> PagedExtentMap<V> {
    fn extent(&self, key: usize) -> Extent {
        self.extents.get(key).copied().unwrap_or_default()
    }

    /// The list of `key`, which is empty where it has none.
    pub(super) fn get(&self, key: usize) -> impl Iterator<Item = V> + '_ {
        self.extent(key).range().map(|index| self.backing[index])
    }

    pub(super) fn push(&mut self, key: usize, value: V) {
        while self.extents.len() <= key {
            self.extents.push(Extent::default());
        }
        let mut extent = self.extents[key];
        if extent.len == extent.room {
            let room = (extent.room * 2).max(1);
            let end = (extent.start + extent.room) as usize;
            if end == self.backing.len() && extent.room != 0 {
                // The list ends the backing, which grows under it.
                for _ in extent.room..room {
                    self.backing.push(V::default());
                }
            } else {
                let start = self.backing.len();
                for index in extent.range() {
                    let value = self.backing[index];
                    self.backing.push(value);
                }
                for _ in extent.len..room {
                    self.backing.push(V::default());
                }
                self.unused += extent.room as usize;
                extent.start = u32::try_from(start).expect("extent map space exhausted");
            }
            self.unused += (room - extent.room) as usize;
            extent.room = room;
        }
        *self.backing.get_mut((extent.start + extent.len) as usize) = value;
        extent.len += 1;
        self.unused -= 1;
        *self.extents.get_mut(key) = extent;
    }

    /// Keep only the values of the list of `key` that `keep` holds to, answering how many are left.
    pub(super) fn retain(&mut self, key: usize, mut keep: impl FnMut(&V) -> bool) -> usize {
        let mut extent = self.extent(key);
        let range = extent.range();
        let mut kept = range.start;
        for index in range.clone() {
            let value = self.backing[index];
            if keep(&value) {
                if kept != index {
                    *self.backing.get_mut(kept) = value;
                }
                kept += 1;
            }
        }
        let len = kept - range.start;
        if len != range.len() {
            self.unused += range.len() - len;
            extent.len = u32::try_from(len).expect("extent map space exhausted");
            *self.extents.get_mut(key) = extent;
        }
        len
    }

    pub(super) fn remove(&mut self, key: usize) {
        let extent = self.extent(key);
        if extent.room != 0 {
            // NB: The room past the list's end is unused already.
            self.unused += extent.len as usize;
            *self.extents.get_mut(key) = Extent::default();
        }
    }

    pub(super) fn has_slack(&self) -> bool {
        self.unused != 0 && self.unused * 2 >= self.backing.len()
    }

    pub(super) fn shrink_to_fit(&mut self) {
        if self.unused != 0 {
            let mut backing = SharedPages::default();
            for key in 0..self.extents.len() {
                let extent = self.extents[key];
                let start = u32::try_from(backing.len()).expect("extent map space exhausted");
                for index in extent.range() {
                    backing.push(self.backing[index]);
                }
                if extent
                    != (Extent {
                        start,
                        len: extent.len,
                        room: extent.len,
                    })
                {
                    *self.extents.get_mut(key) = Extent {
                        start,
                        len: extent.len,
                        room: extent.len,
                    };
                }
            }
            self.backing = backing;
            self.unused = 0;
        }
        self.extents.shrink_to_fit();
        self.backing.shrink_to_fit();
    }

    /// Publish the pages of the map to the pools, taking equal pages others published instead.
    pub(super) fn share(&mut self, extents: &PagePool<Extent>, backing: &PagePool<V>) {
        self.extents.share(extents);
        self.backing.share(backing);
    }

    pub(super) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        self.extents.heap_size(sizer, |_, _| 0) + self.backing.heap_size(sizer, |_, _| 0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_frozen_map_holds_the_lists_it_was_made_of() {
        let mut map = ExtentMap::<u32, u32>::default();
        for value in 0..1000 {
            map.push(value % 97, value);
        }
        map.ensure(500);
        let frozen = FrozenExtentMap::of(map);
        for key in 0..97 {
            let expected: Vec<u32> = (0..1000).filter(|value| value % 97 == key).collect();
            assert_eq!(frozen.get(&key).unwrap(), &expected[..]);
        }
        assert_eq!(frozen.get(&500).unwrap(), &[] as &[u32]);
        assert!(frozen.get(&501).is_none());
        assert_eq!(frozen.keys().count(), 98);
        assert!(FrozenExtentMap::<u32, u32>::default().get(&0).is_none());
    }

    #[test]
    fn lists_appended_in_any_order_keep_their_values_and_pack_again() {
        let mut map = ExtentMap::<u32, u32>::default();
        for value in 0..100 {
            map.push(value % 7, value);
        }
        for key in 0..7 {
            let expected: Vec<u32> = (0..100).filter(|value| value % 7 == key).collect();
            assert_eq!(map.get(&key).unwrap(), &expected[..]);
        }
        assert!(map.has_slack());
        map.retain(&3, |value| value % 2 == 0);
        map.remove(&5);
        let held: usize = map.extents.values().map(|extent| extent.len as usize).sum();
        assert_eq!(held, map.backing.len() - map.unused);
        map.shrink_to_fit();
        assert!(!map.has_slack());
        assert_eq!(map.unused, 0);
        assert_eq!(map.backing.len(), 100 - 14 - 7);
        assert_eq!(map.get(&3).unwrap(), &[10, 24, 38, 52, 66, 80, 94]);
        assert!(map.get(&5).is_none());
        map.push(5, 1);
        map.push(5, 1);
        map.sort_and_dedup_by_key(&5, |value| *value);
        assert_eq!(map.get(&5).unwrap(), &[1]);
    }
}
