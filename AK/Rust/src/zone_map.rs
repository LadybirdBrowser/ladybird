/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Maps and sets that keep their entries in a zone (see `Zone`).
//!
//! A `ZoneMap` keeps its entries next to each other in a list of its zone, in the order in which they were added,
//! and finds them through a table of indices into that list. So a compile makes no block of the heap for a map, and
//! the order in which a map gives its entries back does not depend on the hash of a key: what a compile does with
//! the entries of a map is the same on each run.

use core::fmt;
use core::hash::BuildHasher;
use core::hash::BuildHasherDefault;
use core::hash::Hash;

use crate::Zone;
use crate::ZoneVec;

use crate::fast_hash::FastHasher;

/// A map from `K` to `V` in a zone. Keys and values are plain data (see `ak::ZoneVec`).
#[derive(Clone)]
pub struct ZoneMap<'z, K, V> {
    /// The entries, in the order in which they were added. An entry that is removed gives its place to the last one.
    entries: ZoneVec<'z, (K, V)>,
    /// Where the entries are: for each slot, 0 or one more than the index of an entry. The entry of a key is in the
    /// first slot that is free from the home slot of the key on, in the order of the slots, which wraps around. The
    /// count of slots is a power of two, or 0 for a map that never had an entry.
    slots: ZoneVec<'z, u32>,
}

/// The fewest slots of a map that has entries.
const MIN_SLOTS: usize = 8;

impl<'z, K: Hash + Eq, V> ZoneMap<'z, K, V> {
    /// A map with no entries, which takes no memory before it has one.
    pub fn new_in(zone: &'z Zone) -> Self {
        Self {
            entries: ZoneVec::new_in(zone),
            slots: ZoneVec::new_in(zone),
        }
    }

    /// A map with no entries and room for `capacity` of them.
    pub fn with_capacity_in(capacity: usize, zone: &'z Zone) -> Self {
        let mut map = Self {
            entries: ZoneVec::with_capacity_in(capacity, zone),
            slots: ZoneVec::new_in(zone),
        };
        if capacity != 0 {
            map.slots.resize(Self::slots_for(capacity), 0);
        }
        map
    }

    /// A map of the entries that `entries` gives. For a key that comes more than one time, the last value stays.
    pub fn from_iter_in(entries: impl IntoIterator<Item = (K, V)>, zone: &'z Zone) -> Self {
        let mut map = Self::new_in(zone);
        map.extend(entries);
        map
    }

    /// How many slots keep `count` entries at most three quarters full.
    fn slots_for(count: usize) -> usize {
        (count * 4).div_ceil(3).next_power_of_two().max(MIN_SLOTS)
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// Drops all entries. The map keeps its memory.
    pub fn clear(&mut self) {
        self.entries.clear();
        self.slots.fill(0);
    }

    /// The home slot of `key`. The map has slots.
    fn home(&self, key: &K) -> usize {
        let hash = BuildHasherDefault::<FastHasher>::default().hash_one(key);
        // NB: The hash ends with a multiplication, so its high bits depend on all the bits of the key.
        (hash >> (u64::BITS - self.slots.len().trailing_zeros())) as usize
    }

    /// The slot and the index of the entry of `key`.
    fn find(&self, key: &K) -> Option<(usize, usize)> {
        if self.slots.is_empty() {
            return None;
        }
        let mask = self.slots.len() - 1;
        let mut slot = self.home(key);
        loop {
            let entry = self.slots[slot] as usize;
            if entry == 0 {
                return None;
            }
            if self.entries[entry - 1].0 == *key {
                return Some((slot, entry - 1));
            }
            slot = (slot + 1) & mask;
        }
    }

    /// Puts the entry at `index` in the first free slot from its home on.
    fn place(&mut self, index: usize) {
        let mask = self.slots.len() - 1;
        let mut slot = self.home(&self.entries[index].0);
        while self.slots[slot] != 0 {
            slot = (slot + 1) & mask;
        }
        // NB: A list of a zone holds fewer than 2^32 elements.
        self.slots[slot] = index as u32 + 1;
    }

    /// Adds an entry for `key`, which has none.
    fn add(&mut self, key: K, value: V) {
        if (self.entries.len() + 1) * 4 > self.slots.len() * 3 {
            let count = Self::slots_for(self.entries.len() + 1).max(self.slots.len() * 2);
            self.slots.clear();
            self.slots.resize(count, 0);
            for index in 0..self.entries.len() {
                self.place(index);
            }
        }
        self.entries.push((key, value));
        self.place(self.entries.len() - 1);
    }

    pub fn get(&self, key: &K) -> Option<&V> {
        let (_, index) = self.find(key)?;
        Some(&self.entries[index].1)
    }

    pub fn get_mut(&mut self, key: &K) -> Option<&mut V> {
        let (_, index) = self.find(key)?;
        Some(&mut self.entries[index].1)
    }

    pub fn contains_key(&self, key: &K) -> bool {
        self.find(key).is_some()
    }

    /// Makes `value` the value of `key`, and returns the value that `key` had.
    pub fn insert(&mut self, key: K, value: V) -> Option<V> {
        match self.find(&key) {
            Some((_, index)) => Some(core::mem::replace(&mut self.entries[index].1, value)),
            None => {
                self.add(key, value);
                None
            }
        }
    }

    /// Takes the entry of `key` out, and returns its value. The entry that was added last takes its place in the
    /// order of the entries.
    pub fn remove(&mut self, key: &K) -> Option<V> {
        let (slot, index) = self.find(key)?;
        // Free the slot, and move each entry after it that was pushed away from its home back toward it, so that
        // every entry stays in the first free slot from its home on.
        let mask = self.slots.len() - 1;
        let mut hole = slot;
        let mut next = (hole + 1) & mask;
        while self.slots[next] != 0 {
            let home = self.home(&self.entries[self.slots[next] as usize - 1].0);
            // NB: The entry can move to the hole unless its home is after the hole on the way to its slot.
            if (next.wrapping_sub(home) & mask) >= (next.wrapping_sub(hole) & mask) {
                self.slots[hole] = self.slots[next];
                hole = next;
            }
            next = (next + 1) & mask;
        }
        self.slots[hole] = 0;
        let last = self.entries.len() - 1;
        if index != last {
            let (last_slot, _) = self.find(&self.entries[last].0).expect("each entry has a slot");
            self.slots[last_slot] = index as u32 + 1;
        }
        Some(self.entries.swap_remove(index).1)
    }

    /// The entry of `key`, to add it if the map has none.
    pub fn entry(&mut self, key: K) -> Entry<'_, 'z, K, V> {
        let found = self.find(&key).map(|(_, index)| index);
        Entry { map: self, key, found }
    }

    /// The entries, in the order in which they were added.
    pub fn iter(&self) -> impl ExactSizeIterator<Item = (&K, &V)> + DoubleEndedIterator + '_ {
        self.entries.iter().map(|(key, value)| (key, value))
    }

    pub fn iter_mut(&mut self) -> impl ExactSizeIterator<Item = (&K, &mut V)> + '_ {
        self.entries.iter_mut().map(|(key, value)| (&*key, value))
    }

    pub fn keys(&self) -> impl ExactSizeIterator<Item = &K> + DoubleEndedIterator + '_ {
        self.entries.iter().map(|(key, _)| key)
    }

    pub fn values(&self) -> impl ExactSizeIterator<Item = &V> + DoubleEndedIterator + '_ {
        self.entries.iter().map(|(_, value)| value)
    }

    pub fn values_mut(&mut self) -> impl ExactSizeIterator<Item = &mut V> + '_ {
        self.entries.iter_mut().map(|(_, value)| value)
    }
}

impl<K: Hash + Eq, V> Extend<(K, V)> for ZoneMap<'_, K, V> {
    fn extend<I: IntoIterator<Item = (K, V)>>(&mut self, entries: I) {
        for (key, value) in entries {
            self.insert(key, value);
        }
    }
}

impl<K: fmt::Debug, V: fmt::Debug> fmt::Debug for ZoneMap<'_, K, V> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_map()
            .entries(self.entries.iter().map(|(key, value)| (key, value)))
            .finish()
    }
}

/// The place of a key in a map, which has an entry or gets one (see `ZoneMap::entry()`).
pub struct Entry<'a, 'z, K, V> {
    map: &'a mut ZoneMap<'z, K, V>,
    key: K,
    /// The index of the entry of the key, if it has one.
    found: Option<usize>,
}

impl<'a, K: Hash + Eq, V> Entry<'a, '_, K, V> {
    /// The value of the key, which is `value` if the key had none.
    pub fn or_insert(self, value: V) -> &'a mut V {
        self.or_insert_with(|| value)
    }

    /// The value of the key, which is what `make` returns if the key had none.
    pub fn or_insert_with(self, make: impl FnOnce() -> V) -> &'a mut V {
        let index = match self.found {
            Some(index) => index,
            None => {
                self.map.add(self.key, make());
                self.map.entries.len() - 1
            }
        };
        &mut self.map.entries[index].1
    }

    /// The value of the key, which is the default value if the key had none.
    pub fn or_default(self) -> &'a mut V
    where
        V: Default,
    {
        self.or_insert_with(V::default)
    }
}

/// A set of `K` in a zone. It gives its elements back in the order in which they were added.
#[derive(Clone)]
pub struct ZoneSet<'z, K> {
    map: ZoneMap<'z, K, ()>,
}

impl<'z, K: Hash + Eq> ZoneSet<'z, K> {
    /// A set with no elements, which takes no memory before it has one.
    pub fn new_in(zone: &'z Zone) -> Self {
        Self {
            map: ZoneMap::new_in(zone),
        }
    }

    /// A set of the elements that `elements` gives.
    pub fn from_iter_in(elements: impl IntoIterator<Item = K>, zone: &'z Zone) -> Self {
        let mut set = Self::new_in(zone);
        set.extend(elements);
        set
    }

    pub fn len(&self) -> usize {
        self.map.len()
    }

    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }

    /// Drops all elements. The set keeps its memory.
    pub fn clear(&mut self) {
        self.map.clear();
    }

    pub fn contains(&self, element: &K) -> bool {
        self.map.contains_key(element)
    }

    /// Adds `element`, and returns whether the set did not have it.
    pub fn insert(&mut self, element: K) -> bool {
        self.map.insert(element, ()).is_none()
    }

    /// Takes `element` out, and returns whether the set had it. The element that was added last takes its place in
    /// the order of the elements.
    pub fn remove(&mut self, element: &K) -> bool {
        self.map.remove(element).is_some()
    }

    /// The elements, in the order in which they were added.
    pub fn iter(&self) -> impl ExactSizeIterator<Item = &K> + DoubleEndedIterator + '_ {
        self.map.keys()
    }
}

impl<K: Hash + Eq> Extend<K> for ZoneSet<'_, K> {
    fn extend<I: IntoIterator<Item = K>>(&mut self, elements: I) {
        for element in elements {
            self.insert(element);
        }
    }
}

impl<K: fmt::Debug> fmt::Debug for ZoneSet<'_, K> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_set()
            .entries(self.map.entries.iter().map(|(key, _)| key))
            .finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_map_finds_what_was_added() {
        let zone = Zone::new(1 << 24, c"test");
        let mut map = ZoneMap::new_in(&zone);
        assert!(map.is_empty());
        assert_eq!(zone.used(), 0);
        assert_eq!(map.get(&1_u64), None);
        for key in 0..10_000_u64 {
            // NB: Keys that differ only in their high bits, like the bits of boxed values.
            assert_eq!(map.insert(key << 48 | 7, key), None);
        }
        assert_eq!(map.len(), 10_000);
        for key in 0..10_000_u64 {
            assert_eq!(map.get(&(key << 48 | 7)), Some(&key));
            assert!(!map.contains_key(&(key << 48 | 6)));
        }
        assert_eq!(map.insert(5 << 48 | 7, 99), Some(5));
        *map.get_mut(&(6 << 48 | 7)).unwrap() += 1;
        assert_eq!(map.get(&(5 << 48 | 7)), Some(&99));
        assert_eq!(map.get(&(6 << 48 | 7)), Some(&7));
        assert_eq!(map.len(), 10_000);
        map.clear();
        assert!(map.is_empty());
        assert_eq!(map.get(&7), None);
        assert_eq!(map.insert(7, 1), None);
    }

    #[test]
    fn a_map_gives_its_entries_back_in_the_order_they_were_added() {
        let zone = Zone::new(1 << 20, c"test");
        let keys = [900_u32, 3, 77, 1 << 30, 0, 12];
        let mut map = ZoneMap::from_iter_in(keys.iter().map(|key| (*key, key.count_ones())), &zone);
        assert!(map.keys().copied().eq(keys));
        assert!(map.iter().all(|(key, ones)| key.count_ones() == *ones));
        for ones in map.values_mut() {
            *ones += 1;
        }
        assert!(map.values().copied().eq(keys.iter().map(|key| key.count_ones() + 1)));
        let copy = map.clone();
        map.insert(5, 0);
        assert_eq!(copy.len(), keys.len());
        assert_eq!(copy.get(&5), None);
        assert_eq!(
            format!("{:?}", ZoneMap::from_iter_in([(1, 'a'), (0, 'b')], &zone)),
            "{1: 'a', 0: 'b'}"
        );
    }

    #[test]
    fn a_map_removes_entries() {
        let zone = Zone::new(1 << 24, c"test");
        let mut map = ZoneMap::with_capacity_in(16, &zone);
        let mut model = std::collections::HashMap::new();
        // NB: A sequence that does not repeat for a long time, in place of random numbers.
        let mut state = 0x9e37_79b9_u32;
        for round in 0..20_000_u32 {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            let key = (state >> 16) % 512;
            if state & 3 == 0 {
                assert_eq!(map.remove(&key), model.remove(&key));
            } else {
                assert_eq!(map.insert(key, round), model.insert(key, round));
            }
            assert_eq!(map.len(), model.len());
        }
        for key in 0..512 {
            assert_eq!(map.get(&key), model.get(&key));
        }
        let mut entries = map.iter().map(|(key, value)| (*key, *value)).collect::<Vec<_>>();
        entries.sort_unstable();
        let mut expected = model.into_iter().collect::<Vec<_>>();
        expected.sort_unstable();
        assert_eq!(entries, expected);
    }

    #[test]
    fn an_entry_adds_a_value_one_time() {
        let zone = Zone::new(1 << 20, c"test");
        let mut map = ZoneMap::new_in(&zone);
        *map.entry("a").or_insert(1) += 10;
        *map.entry("a").or_insert(1) += 10;
        *map.entry("b").or_insert_with(|| 5) += 1;
        *map.entry("c").or_default() += 2;
        assert_eq!(map.entry("b").or_insert_with(|| unreachable!()), &6);
        assert!(
            map.iter()
                .map(|(key, value)| (*key, *value))
                .eq([("a", 21), ("b", 6), ("c", 2)])
        );
    }

    #[test]
    fn a_set_has_each_element_one_time() {
        let zone = Zone::new(1 << 20, c"test");
        let mut set = ZoneSet::from_iter_in([4_u8, 9, 4, 1], &zone);
        assert_eq!(set.len(), 3);
        assert!(set.contains(&9));
        assert!(!set.contains(&5));
        assert!(!set.insert(1));
        assert!(set.insert(5));
        assert!(set.iter().copied().eq([4, 9, 1, 5]));
        assert!(set.remove(&4));
        assert!(!set.remove(&4));
        assert!(set.iter().copied().eq([5, 9, 1]));
        assert_eq!(format!("{set:?}"), "{5, 9, 1}");
        let copy = set.clone();
        set.clear();
        assert!(set.is_empty());
        assert_eq!(copy.len(), 3);
    }
}
