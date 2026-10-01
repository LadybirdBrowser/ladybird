/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::fast_hash::FastMap as HashMap;
use super::memory::{DeviceClass, MemoryController};
use std::sync::{Arc, Weak};

/// The smallest reference count a pool lets itself grow to before it first sweeps.
const MINIMUM_SWEEP_THRESHOLD: usize = 64;

/// An interning pool: weak references to equal immutable values, by content hash.
///
/// The pool never keeps a value alive, and a value never reaches back into its pool when it is
/// dropped, so a value can die on any thread, and a pool shared behind a lock is only locked
/// while something is interned through it.
/// A dead reference still holds its value's allocation, though not the value, until it is dropped
/// when its bucket is next searched or by a full sweep, which runs whenever the references have
/// doubled since the last one, so dead references never outnumber the live ones by much. Shared
/// values are charged once to the pool's own ledger.
pub(super) struct WeakPool<T> {
    by_hash: HashMap<u64, Vec<Weak<T>>>,
    references: usize,
    sweep_threshold: usize,
    pub(super) memory: MemoryController,
}

impl<T> Default for WeakPool<T> {
    fn default() -> Self {
        Self {
            by_hash: HashMap::default(),
            references: 0,
            sweep_threshold: MINIMUM_SWEEP_THRESHOLD,
            memory: MemoryController::new(DeviceClass::ForegroundDesktop),
        }
    }
}

impl<T> WeakPool<T> {
    /// A live value stored under `hash` that `matches` accepts.
    pub(super) fn find(&mut self, hash: u64, mut matches: impl FnMut(&T) -> bool) -> Option<Arc<T>> {
        let bucket = self.by_hash.get_mut(&hash)?;
        let before = bucket.len();
        bucket.retain(|candidate| candidate.strong_count() != 0);
        self.references -= before - bucket.len();
        let found = bucket
            .iter()
            .filter_map(Weak::upgrade)
            .find(|candidate| matches(candidate));
        if bucket.is_empty() {
            self.by_hash.remove(&hash);
        }
        found
    }

    /// Remember `value` under `hash`, for a later equal value to find.
    pub(super) fn insert(&mut self, hash: u64, value: &Arc<T>) {
        self.by_hash.entry(hash).or_default().push(Arc::downgrade(value));
        self.references += 1;
        if self.references >= self.sweep_threshold {
            self.sweep();
        }
    }

    fn sweep(&mut self) {
        self.by_hash.retain(|_, bucket| {
            bucket.retain(|candidate| candidate.strong_count() != 0);
            !bucket.is_empty()
        });
        self.references = self.by_hash.values().map(Vec::len).sum();
        self.sweep_threshold = (self.references * 2).max(MINIMUM_SWEEP_THRESHOLD);
    }

    #[cfg(test)]
    pub(super) fn live_values_under(&self, hash: u64) -> usize {
        self.by_hash.get(&hash).map_or(0, |bucket| {
            bucket.iter().filter(|candidate| candidate.strong_count() != 0).count()
        })
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
        assert!(pool.references < MINIMUM_SWEEP_THRESHOLD);

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
