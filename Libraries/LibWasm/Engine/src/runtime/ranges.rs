/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Sets of address ranges that the fault handler looks addresses up in: the reservations of memories, and the code
//! of compiled functions. A lookup takes no lock and only reads memory that is never freed, so it is
//! async-signal-safe and cannot be blocked by a thread that is adding or removing a range. Adding and removing take a
//! lock among themselves.
//!
//! The slots are in a fixed array and, once that is full, in further segments that adding allocates and links in;
//! segments are never freed, and a lookup follows the links.
//!
//! Each slot is a seqlock: its writer makes the sequence odd, writes the range, and makes the sequence even again. A
//! lookup that sees an odd sequence, or a sequence that changed while it read the range, skips the slot. That is safe
//! because a range is only added before anything accesses it and removed after nothing can, so no fault that the
//! handler must recognize lands in a range that is changing.

use std::sync::Mutex;
use std::sync::atomic::{AtomicPtr, AtomicUsize, Ordering, fence};

/// The slots of the fixed array, and of each further segment.
pub const CAPACITY: usize = 4096;

#[derive(Default)]
struct Slot {
    sequence: AtomicUsize,
    start: AtomicUsize,
    end: AtomicUsize,
}

/// Slots beyond the fixed array.
struct Segment {
    slots: Box<[Slot]>,
    next: AtomicPtr<Segment>,
}

pub struct RangeSet {
    slots: [Slot; CAPACITY],
    /// The first further segment, or null.
    next: AtomicPtr<Segment>,
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
            next: AtomicPtr::new(std::ptr::null_mut()),
            lock: Mutex::new(()),
        }
    }

    /// Calls `visit` with every slot, until it returns true. Returns whether it did.
    fn any_slot(&self, mut visit: impl FnMut(&Slot) -> bool) -> bool {
        if self.slots.iter().any(&mut visit) {
            return true;
        }
        let mut segment = self.next.load(Ordering::Acquire);
        while !segment.is_null() {
            // SAFETY: Segments are never freed once they are linked in.
            let current = unsafe { &*segment };
            if current.slots.iter().any(&mut visit) {
                return true;
            }
            segment = current.next.load(Ordering::Acquire);
        }
        false
    }

    fn write(slot: &Slot, start: usize, end: usize) {
        let sequence = slot.sequence.load(Ordering::Relaxed);
        slot.sequence.store(sequence.wrapping_add(1), Ordering::Relaxed);
        fence(Ordering::Release);
        slot.start.store(start, Ordering::Relaxed);
        slot.end.store(end, Ordering::Relaxed);
        slot.sequence.store(sequence.wrapping_add(2), Ordering::Release);
    }

    /// Add the range [start, start + length). Returns false if the set is full, which it never is: it grows.
    pub fn add(&self, start: usize, length: usize) -> bool {
        assert!(start != 0 && length != 0, "ranges are not empty and not at null");
        let _guard = self.lock.lock().expect("the range lock is never poisoned");
        let mut free = None;
        self.any_slot(|slot| {
            if slot.end.load(Ordering::Relaxed) == 0 {
                free = Some(std::ptr::from_ref(slot));
                return true;
            }
            false
        });
        if let Some(slot) = free {
            // SAFETY: Slots are never freed.
            Self::write(unsafe { &*slot }, start, start + length);
            return true;
        }
        // Every slot is taken: link in a segment whose first slot holds the range before anyone can see it.
        let segment = Box::leak(Box::new(Segment {
            slots: (0..CAPACITY).map(|_| Slot::default()).collect(),
            next: AtomicPtr::new(std::ptr::null_mut()),
        }));
        Self::write(&segment.slots[0], start, start + length);
        let mut link = &self.next;
        loop {
            let next = link.load(Ordering::Acquire);
            if next.is_null() {
                break;
            }
            // SAFETY: As in any_slot().
            link = unsafe { &(*next).next };
        }
        link.store(segment, Ordering::Release);
        true
    }

    /// Remove the range that starts at `start`.
    pub fn remove(&self, start: usize) {
        let _guard = self.lock.lock().expect("the range lock is never poisoned");
        self.any_slot(|slot| {
            if slot.end.load(Ordering::Relaxed) != 0 && slot.start.load(Ordering::Relaxed) == start {
                Self::write(slot, 0, 0);
                return true;
            }
            false
        });
    }

    /// Whether `address` is in one of the ranges. Async-signal-safe.
    pub fn contains(&self, address: usize) -> bool {
        self.any_slot(|slot| {
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

    #[test]
    fn grows_past_its_fixed_slots() {
        let set = RangeSet::new();
        let count = 3 * super::CAPACITY;
        for index in 0..count {
            assert!(set.add(0x1000 * (index + 1), 0x10));
        }
        assert!(set.contains(0x1000) && set.contains(0x1000 * count + 0xf));
        set.remove(0x1000 * count);
        assert!(!set.contains(0x1000 * count));
        assert!(set.add(0x1000 * (count + 1), 0x10));
        assert!(set.contains(0x1000 * (count + 1)));
    }
}
