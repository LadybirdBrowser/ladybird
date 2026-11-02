/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::fast_hash::FastMap;
use std::sync::Arc;
use std::time::{Duration, Instant};

/// How many bytes of what nothing else holds the process keeps of each kind of shared style data.
const BYTES_KEPT_PAST_RELEASE: usize = 15 * 1024 * 1024;

/// How long after its last use the process keeps what nothing else holds.
const KEPT_PAST_LAST_USE: Duration = Duration::from_secs(5 * 60);

struct Kept<T> {
    value: Arc<T>,
    bytes: usize,
    /// When the value was last asked for, in the order of such asks.
    used: u64,
    used_at: Instant,
}

/// Shared immutable values the process keeps alive once the documents using them let go: a navigation to a page of the
/// same site finds what the page it left parsed and indexed, even after a collection. They go when they exceed a budget,
/// the least recently used first, when they have gone unused for a while, and when the process is short of memory.
/// What a document still holds costs nothing here, and counts against no budget.
pub(in crate::css) struct KeptPastRelease<T> {
    entries: FastMap<usize, Kept<T>>,
    budget: usize,
    next_use: u64,
}

impl<T> Default for KeptPastRelease<T> {
    fn default() -> Self {
        Self::with_budget(BYTES_KEPT_PAST_RELEASE)
    }
}

fn is_released<T>(entry: &Kept<T>) -> bool {
    Arc::strong_count(&entry.value) == 1
}

impl<T> KeptPastRelease<T> {
    fn with_budget(budget: usize) -> Self {
        Self {
            entries: FastMap::default(),
            budget,
            next_use: 0,
        }
    }

    /// Keep `value`, which holds about `bytes`, and answer the values that left to make room, for the caller to drop
    /// past its locks.
    #[must_use]
    pub(in crate::css) fn keep(&mut self, value: &Arc<T>, bytes: usize) -> Vec<Arc<T>> {
        self.keep_at(value, bytes, Instant::now())
    }

    fn keep_at(&mut self, value: &Arc<T>, bytes: usize, now: Instant) -> Vec<Arc<T>> {
        self.next_use += 1;
        self.entries.insert(
            Arc::as_ptr(value).addr(),
            Kept {
                value: Arc::clone(value),
                bytes,
                used: self.next_use,
                used_at: now,
            },
        );
        let mut released: Vec<(u64, usize, usize)> = self
            .entries
            .iter()
            .filter(|(_, entry)| is_released(entry))
            .map(|(&key, entry)| (entry.used, key, entry.bytes))
            .collect();
        let mut over = released
            .iter()
            .map(|&(_, _, bytes)| bytes)
            .sum::<usize>()
            .saturating_sub(self.budget);
        let mut left = Vec::new();
        if over == 0 {
            return left;
        }
        released.sort_unstable();
        for (_, key, bytes) in released {
            if over == 0 {
                break;
            }
            over = over.saturating_sub(bytes);
            left.extend(self.entries.remove(&key).map(|entry| entry.value));
        }
        left
    }

    /// Note that a document asked for `value`, if it is kept.
    pub(in crate::css) fn note_use(&mut self, value: &Arc<T>) {
        self.note_use_at(value, Instant::now());
    }

    fn note_use_at(&mut self, value: &Arc<T>, now: Instant) {
        self.next_use += 1;
        if let Some(entry) = self.entries.get_mut(&Arc::as_ptr(value).addr()) {
            entry.used = self.next_use;
            entry.used_at = now;
        }
    }

    /// Let go of what nothing else holds and nothing asked for lately, and answer it, for the caller to drop past its
    /// locks.
    #[must_use]
    pub(in crate::css) fn prune(&mut self) -> Vec<Arc<T>> {
        let now = Instant::now();
        self.take_released(|entry| now.saturating_duration_since(entry.used_at) >= KEPT_PAST_LAST_USE)
    }

    /// Let go of everything nothing else holds, as the process is short of memory, and answer it, for the caller to
    /// drop past its locks.
    #[must_use]
    pub(in crate::css) fn release_all(&mut self) -> Vec<Arc<T>> {
        self.take_released(|_| true)
    }

    fn take_released(&mut self, goes: impl Fn(&Kept<T>) -> bool) -> Vec<Arc<T>> {
        let mut left = Vec::new();
        self.entries.retain(|_, entry| {
            if is_released(entry) && goes(entry) {
                left.push(Arc::clone(&entry.value));
                return false;
            }
            true
        });
        left
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_least_recently_used_released_values_go_first_and_only_past_the_budget() {
        let mut kept = KeptPastRelease::with_budget(10);
        let now = Instant::now();
        let held = Arc::new(0);
        assert!(kept.keep_at(&held, 100, now).is_empty());
        let first = Arc::new(1);
        assert!(kept.keep_at(&first, 5, now).is_empty());
        let second = Arc::new(2);
        assert!(kept.keep_at(&second, 5, now).is_empty());
        let (first_pointer, second_pointer) = (Arc::as_ptr(&first), Arc::as_ptr(&second));
        // A use makes the first the more recent, though it came first.
        kept.note_use_at(&first, now);
        drop(first);
        drop(second);
        let third = Arc::new(3);
        assert!(kept.keep_at(&third, 5, now).is_empty());
        drop(third);
        let fourth = Arc::new(4);
        let left = kept.keep_at(&fourth, 5, now);
        assert_eq!(left.len(), 1);
        assert_eq!(Arc::as_ptr(&left[0]), second_pointer);
        assert!(kept.entries.contains_key(&first_pointer.addr()));
        assert_eq!(Arc::strong_count(&held), 2);
    }

    #[test]
    fn being_short_of_memory_lets_go_of_all_but_what_a_document_holds() {
        let mut kept = KeptPastRelease::with_budget(100);
        let held = Arc::new(0);
        drop(kept.keep(&held, 1));
        let released = Arc::new(1);
        drop(kept.keep(&released, 1));
        drop(released);
        assert_eq!(kept.release_all().len(), 1);
        assert_eq!(kept.entries.len(), 1);
    }

    #[test]
    fn only_what_nothing_asked_for_lately_is_pruned() {
        let mut kept = KeptPastRelease::with_budget(100);
        let now = Instant::now();
        let long_unused = Arc::new(0);
        drop(kept.keep_at(&long_unused, 1, now.checked_sub(KEPT_PAST_LAST_USE).unwrap()));
        let held = Arc::new(1);
        drop(kept.keep_at(&held, 1, now.checked_sub(KEPT_PAST_LAST_USE).unwrap()));
        let recent = Arc::new(2);
        drop(kept.keep_at(&recent, 1, now));
        drop(long_unused);
        drop(recent);
        assert_eq!(kept.prune().len(), 1);
        assert_eq!(kept.entries.len(), 2);
    }
}
