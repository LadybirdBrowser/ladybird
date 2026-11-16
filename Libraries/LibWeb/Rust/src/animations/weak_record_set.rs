/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Sets of animation records that do not keep them alive.

use std::hash::BuildHasher;
use std::rc::{Rc, Weak};

use hashbrown::HashTable;

/// A set of records, by identity, that keeps none of them alive.
///
/// A record is alive to the set while something keeps it alive and the set's liveness test accepts it. A record that
/// dies stays in the set as a dead entry, which no lookup or enumeration yields, until the set is next
/// pruned. Dropping a record therefore never reaches into a set that holds it. A dead entry still holds its record's
/// allocation, so the address of a record identifies it for as long as the set has an entry for it.
///
/// The entries are kept in insertion order, except that removing one moves the last entry into its place.
pub(crate) struct WeakRecordSet<T> {
    entries: Vec<Weak<T>>,
    /// The index of each entry in `entries`, by the hash of its record's address.
    indices: HashTable<u32>,
    is_alive: fn(&T) -> bool,
}

fn hash_of<T>(record: *const T) -> u64 {
    foldhash::fast::FixedState::default().hash_one(record as usize)
}

impl<T> WeakRecordSet<T> {
    /// A new empty set, to which a record that something keeps alive is alive while `is_alive` accepts it.
    pub(crate) fn new(is_alive: fn(&T) -> bool) -> Self {
        Self {
            entries: Vec::new(),
            indices: HashTable::new(),
            is_alive,
        }
    }

    fn live_record(&self, entry: &Weak<T>) -> Option<Rc<T>> {
        entry.upgrade().filter(|record| (self.is_alive)(record))
    }

    fn find(&self, record: *const T) -> Option<u32> {
        let entries = &self.entries;
        self.indices
            .find(hash_of(record), |&index| entries[index as usize].as_ptr() == record)
            .copied()
    }

    /// Whether the set holds `record`.
    pub(crate) fn contains(&self, record: *const T) -> bool {
        self.find(record).is_some()
    }

    /// Adds `record` to the set, answering whether the set did not hold it yet.
    pub(crate) fn insert(&mut self, record: &Rc<T>) -> bool {
        let pointer = Rc::as_ptr(record);
        if self.contains(pointer) {
            return false;
        }
        if self.entries.len() == self.entries.capacity() {
            self.prune();
        }
        let index = u32::try_from(self.entries.len()).expect("a set of records holds fewer than 2^32 entries");
        self.entries.push(Rc::downgrade(record));
        let entries = &self.entries;
        self.indices.insert_unique(hash_of(pointer), index, |&index| {
            hash_of(entries[index as usize].as_ptr())
        });
        true
    }

    /// Removes `record` from the set, answering whether the set held it.
    pub(crate) fn remove(&mut self, record: *const T) -> bool {
        let entries = &self.entries;
        let Ok(slot) = self
            .indices
            .find_entry(hash_of(record), |&index| entries[index as usize].as_ptr() == record)
        else {
            return false;
        };
        let (index, _) = slot.remove();
        let last = u32::try_from(self.entries.len() - 1).expect("a set of records holds fewer than 2^32 entries");
        if index != last {
            let moved = self.entries[last as usize].as_ptr();
            let entries = &self.entries;
            let slot = self
                .indices
                .find_mut(hash_of(moved), |&candidate| {
                    candidate == last && entries[candidate as usize].as_ptr() == moved
                })
                .expect("every entry of a set of records has an index");
            *slot = index;
        }
        self.entries.swap_remove(index as usize);
        true
    }

    /// Drops the entries of records that are no longer alive to the set.
    pub(crate) fn prune(&mut self) {
        let is_alive = self.is_alive;
        let entry_is_alive = |entry: &Weak<T>| entry.upgrade().is_some_and(|record| is_alive(&record));
        if self.entries.iter().all(entry_is_alive) {
            return;
        }
        self.entries.retain(entry_is_alive);
        self.indices.clear();
        for (index, entry) in self.entries.iter().enumerate() {
            let index = u32::try_from(index).expect("a set of records holds fewer than 2^32 entries");
            let entries = &self.entries;
            self.indices.insert_unique(hash_of(entry.as_ptr()), index, |&index| {
                hash_of(entries[index as usize].as_ptr())
            });
        }
    }

    /// The number of entries, live or dead, which enumeration runs over.
    pub(crate) fn entry_count(&self) -> usize {
        self.entries.len()
    }

    /// The record of the entry at `index`, where it is alive to the set.
    pub(crate) fn get(&self, index: usize) -> Option<Rc<T>> {
        self.live_record(&self.entries[index])
    }

    /// Empties the set, calling `callback` with each record alive to it, in insertion order.
    pub(crate) fn take_each(&mut self, mut callback: impl FnMut(Rc<T>)) {
        for entry in &self.entries {
            if let Some(record) = self.live_record(entry) {
                callback(record);
            }
        }
        self.clear();
    }

    /// Removes every entry.
    pub(crate) fn clear(&mut self) {
        self.entries.clear();
        self.indices.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn live(set: &WeakRecordSet<u32>) -> Vec<u32> {
        (0..set.entry_count())
            .filter_map(|index| set.get(index))
            .map(|record| *record)
            .collect()
    }

    #[test]
    fn insert_and_remove_track_membership_by_identity() {
        let mut set = WeakRecordSet::new(|_| true);
        let first = Rc::new(1);
        let second = Rc::new(1);
        assert!(set.insert(&first));
        assert!(!set.insert(&first));
        assert!(set.insert(&second));
        assert!(set.contains(Rc::as_ptr(&first)));
        assert!(set.remove(Rc::as_ptr(&first)));
        assert!(!set.remove(Rc::as_ptr(&first)));
        assert!(!set.contains(Rc::as_ptr(&first)));
        assert!(set.contains(Rc::as_ptr(&second)));
    }

    #[test]
    fn taking_each_record_yields_the_live_ones_and_empties_the_set() {
        let mut set = WeakRecordSet::new(|&record| record != 2);
        let records: Vec<_> = (0..4).map(Rc::new).collect();
        for record in &records {
            set.insert(record);
        }
        let mut taken = Vec::new();
        set.take_each(|record| taken.push(*record));
        assert_eq!(taken, [0, 1, 3]);
        assert_eq!(set.entry_count(), 0);
    }

    #[test]
    fn a_dead_record_is_not_yielded_and_is_pruned() {
        let mut set = WeakRecordSet::new(|_| true);
        let kept = Rc::new(1);
        let dropped = Rc::new(2);
        set.insert(&kept);
        set.insert(&dropped);
        drop(dropped);
        assert_eq!(live(&set), [1]);
        assert_eq!(set.entry_count(), 2);
        set.prune();
        assert_eq!(set.entry_count(), 1);
        assert!(set.contains(Rc::as_ptr(&kept)));
    }

    #[test]
    fn a_record_the_set_does_not_consider_alive_is_not_yielded_and_is_pruned() {
        let mut set = WeakRecordSet::new(|&record| record != 2);
        let kept = Rc::new(1);
        let retired = Rc::new(2);
        set.insert(&kept);
        set.insert(&retired);
        assert_eq!(set.get(1), None);
        set.prune();
        assert_eq!(live(&set), [1]);
        assert!(!set.contains(Rc::as_ptr(&retired)));
    }
}
