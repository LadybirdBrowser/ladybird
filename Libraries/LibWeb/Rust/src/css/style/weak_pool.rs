/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use hashbrown::HashTable;
use std::sync::{Arc, Weak};

/// The smallest reference count a pool lets itself grow to before it first sweeps.
const MINIMUM_SWEEP_THRESHOLD: usize = 64;

/// A weak reference to a pooled value, with the hash it is pooled under.
struct Slot<T> {
    hash: u64,
    value: Weak<T>,
}

/// An interning pool: weak references to equal immutable values, by content hash.
///
/// The pool never keeps a value alive, and a value never reaches back into its pool when it is
/// dropped, so a value can die on any thread, and a pool shared behind a lock is only locked
/// while something is interned through it.
/// A dead reference still holds its value's allocation, though not the value, until it is dropped
/// when its hash is next searched or by a full sweep, which runs whenever the references have
/// doubled since the last one, so dead references never outnumber the live ones by much.
///
/// NB: Each reference is a slot of one table, and values with equal hashes take slots of their
///     own, so a value costs one slot rather than a list per hash with an allocation behind it.
pub(super) struct WeakPool<T> {
    slots: HashTable<Slot<T>>,
    sweep_threshold: usize,
}

/// A fork's pool finds the values its parent's found, as long as they live.
impl<T> Clone for WeakPool<T> {
    fn clone(&self) -> Self {
        let mut slots = HashTable::with_capacity(self.slots.len());
        for slot in &self.slots {
            slots.insert_unique(
                slot.hash,
                Slot {
                    hash: slot.hash,
                    value: slot.value.clone(),
                },
                |slot| slot.hash,
            );
        }
        Self {
            slots,
            sweep_threshold: self.sweep_threshold,
        }
    }
}

impl<T> Default for WeakPool<T> {
    fn default() -> Self {
        Self {
            slots: HashTable::new(),
            sweep_threshold: MINIMUM_SWEEP_THRESHOLD,
        }
    }
}

impl<T> WeakPool<T> {
    /// A live value stored under `hash` that `matches` accepts.
    pub(super) fn find(&mut self, hash: u64, mut matches: impl FnMut(&T) -> bool) -> Option<Arc<T>> {
        self.forget(hash, |slot| slot.value.strong_count() == 0);
        self.slots
            .iter_hash(hash)
            .filter(|slot| slot.hash == hash)
            .filter_map(|slot| slot.value.upgrade())
            .find(|candidate| matches(candidate))
    }

    /// Remember `value` under `hash`, for a later equal value to find.
    pub(super) fn insert(&mut self, hash: u64, value: &Arc<T>) {
        let slot = Slot {
            hash,
            value: Arc::downgrade(value),
        };
        self.slots.insert_unique(hash, slot, |slot| slot.hash);
        if self.slots.len() >= self.sweep_threshold {
            self.sweep();
        }
    }

    /// Drop the references under `hash` that `forgotten` accepts.
    fn forget(&mut self, hash: u64, mut forgotten: impl FnMut(&Slot<T>) -> bool) {
        while let Ok(entry) = self.slots.find_entry(hash, |slot| slot.hash == hash && forgotten(slot)) {
            entry.remove();
        }
    }

    fn sweep(&mut self) {
        self.slots.retain(|slot| slot.value.strong_count() != 0);
        self.slots.shrink_to_fit(|slot| slot.hash);
        self.sweep_threshold = (self.slots.len() * 2).max(MINIMUM_SWEEP_THRESHOLD);
    }

    #[cfg(test)]
    pub(super) fn live_values_under(&self, hash: u64) -> usize {
        self.slots
            .iter_hash(hash)
            .filter(|slot| slot.hash == hash && slot.value.strong_count() != 0)
            .count()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dead_references_are_swept_without_the_values_reaching_back() {
        let mut pool = WeakPool::<u32>::default();
        for value in 0..(MINIMUM_SWEEP_THRESHOLD as u32 * 4) {
            let value = Arc::new(value);
            pool.insert(u64::from(value.as_ref() % 3), &value);
        }
        // Every value died right after insertion; the sweeps kept the table near empty.
        assert!(pool.slots.len() < MINIMUM_SWEEP_THRESHOLD);

        let kept = Arc::new(7);
        pool.insert(7, &kept);
        let found = pool
            .find(7, |candidate| *candidate == 7)
            .expect("a live value is found");
        assert!(Arc::ptr_eq(&found, &kept));
        drop((found, kept));
        assert!(pool.find(7, |_| true).is_none());
        assert_eq!(pool.live_values_under(7), 0);
    }
}
