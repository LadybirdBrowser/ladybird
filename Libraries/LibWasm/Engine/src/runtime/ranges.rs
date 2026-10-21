/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Sets of address ranges that the fault handler looks addresses up in: the reservations of memories, and later the
//! code of compiled functions. A lookup takes no lock and touches only a fixed static array, so it is async-signal-safe
//! and cannot be blocked by a thread that is adding or removing a range. Adding and removing take a lock among
//! themselves.
//!
//! Each slot is a seqlock: its writer makes the sequence odd, writes the range, and makes the sequence even again. A
//! lookup that sees an odd sequence, or a sequence that changed while it read the range, skips the slot. That is safe
//! because a range is only added before anything accesses it and removed after nothing can, so no fault that the
//! handler must recognize lands in a range that is changing.

use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering, fence};

/// The most ranges a set holds.
pub const CAPACITY: usize = 4096;

struct Slot {
    sequence: AtomicUsize,
    start: AtomicUsize,
    end: AtomicUsize,
}

pub struct RangeSet {
    slots: [Slot; CAPACITY],
    /// Serializes the writers.
    lock: Mutex<()>,
}

impl RangeSet {
    pub const fn new() -> Self {
        Self {
            slots: [const {
                Slot {
                    sequence: AtomicUsize::new(0),
                    start: AtomicUsize::new(0),
                    end: AtomicUsize::new(0),
                }
            }; CAPACITY],
            lock: Mutex::new(()),
        }
    }

    fn write(slot: &Slot, start: usize, end: usize) {
        let sequence = slot.sequence.load(Ordering::Relaxed);
        slot.sequence.store(sequence.wrapping_add(1), Ordering::Relaxed);
        fence(Ordering::Release);
        slot.start.store(start, Ordering::Relaxed);
        slot.end.store(end, Ordering::Relaxed);
        slot.sequence.store(sequence.wrapping_add(2), Ordering::Release);
    }

    /// Add the range [start, start + length). Returns false if the set is full.
    pub fn add(&self, start: usize, length: usize) -> bool {
        assert!(start != 0 && length != 0, "ranges are not empty and not at null");
        let _guard = self.lock.lock().expect("the range lock is never poisoned");
        let Some(slot) = self.slots.iter().find(|slot| slot.end.load(Ordering::Relaxed) == 0) else {
            return false;
        };
        Self::write(slot, start, start + length);
        true
    }

    /// Remove the range that starts at `start`.
    pub fn remove(&self, start: usize) {
        let _guard = self.lock.lock().expect("the range lock is never poisoned");
        if let Some(slot) = self
            .slots
            .iter()
            .find(|slot| slot.end.load(Ordering::Relaxed) != 0 && slot.start.load(Ordering::Relaxed) == start)
        {
            Self::write(slot, 0, 0);
        }
    }

    /// Whether `address` is in one of the ranges. Async-signal-safe.
    pub fn contains(&self, address: usize) -> bool {
        self.slots.iter().any(|slot| {
            let before = slot.sequence.load(Ordering::Acquire);
            if before & 1 != 0 {
                return false;
            }
            let start = slot.start.load(Ordering::Relaxed);
            let end = slot.end.load(Ordering::Relaxed);
            fence(Ordering::Acquire);
            let after = slot.sequence.load(Ordering::Relaxed);
            before == after && start <= address && address < end
        })
    }
}

impl Default for RangeSet {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::RangeSet;

    #[test]
    fn adds_finds_and_removes_ranges() {
        let set = RangeSet::new();
        assert!(set.add(0x1000, 0x100));
        assert!(set.add(0x4000, 0x10));
        assert!(set.contains(0x1000) && set.contains(0x10ff) && set.contains(0x400f));
        assert!(!set.contains(0x1100) && !set.contains(0xfff) && !set.contains(0x4010));
        set.remove(0x1000);
        assert!(!set.contains(0x1000) && set.contains(0x4000));
    }
}
