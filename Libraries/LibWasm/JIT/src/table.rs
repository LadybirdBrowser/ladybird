/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A hash table for the compiler's per-compile maps, which emptying makes no work.
//!
//! OPTIMIZATION: Every compile starts with empty maps, and most functions are small, while a few are huge. A
//! general hash map's `clear()` touches its whole capacity, so after one huge function every small one would pay
//! for it. Here every slot carries the number of the compile that wrote it, and a slot of an older compile is empty:
//! emptying the table is one increment. Keys are never removed; callers that need removal store a value that means
//! none. Open addressing with linear probing, at most half full.

/// A key: hashes to 64 bits without a hasher object.
pub trait TableKey: Copy + Eq + Default {
    fn hash(&self) -> u64;
}

impl TableKey for u64 {
    #[inline]
    fn hash(&self) -> u64 {
        self.wrapping_mul(0x9e37_79b9_7f4a_7c15)
    }
}

impl TableKey for [u32; 4] {
    #[inline]
    fn hash(&self) -> u64 {
        let low = u64::from(self[0]) | (u64::from(self[1]) << 32);
        let high = u64::from(self[2]) | (u64::from(self[3]) << 32);
        (low.wrapping_mul(0x9e37_79b9_7f4a_7c15) ^ high).wrapping_mul(0xc2b2_ae3d_27d4_eb4f)
    }
}

impl TableKey for [u32; 6] {
    #[inline]
    fn hash(&self) -> u64 {
        let words = [
            u64::from(self[0]) | (u64::from(self[1]) << 32),
            u64::from(self[2]) | (u64::from(self[3]) << 32),
            u64::from(self[4]) | (u64::from(self[5]) << 32),
        ];
        let mut hash = 0u64;
        for word in words {
            hash = (hash.rotate_left(26) ^ word).wrapping_mul(0x9e37_79b9_7f4a_7c15);
        }
        hash
    }
}

#[derive(Clone, Copy, Default)]
struct Slot<K: TableKey> {
    key: K,
    value: u32,
    stamp: u32,
}

pub struct Table<K: TableKey> {
    slots: Vec<Slot<K>>,
    stamp: u32,
    len: usize,
    shift: u32,
}

impl<K: TableKey> Default for Table<K> {
    fn default() -> Self {
        Self {
            slots: Vec::new(),
            stamp: 1,
            len: 0,
            shift: 64,
        }
    }
}

const MINIMUM_CAPACITY: usize = 64;

impl<K: TableKey> Table<K> {
    /// Empties the table.
    pub fn clear(&mut self) {
        self.len = 0;
        self.stamp = self.stamp.wrapping_add(1);
        if self.stamp == 0 {
            // NB: Once in 2^32 compiles, the stamps start over, and the old ones must not look current.
            for slot in &mut self.slots {
                slot.stamp = 0;
            }
            self.stamp = 1;
        }
    }

    #[inline]
    fn index(&self, key: &K) -> usize {
        (key.hash() >> self.shift) as usize
    }

    #[inline]
    pub fn get(&self, key: K) -> Option<u32> {
        if self.slots.is_empty() {
            return None;
        }
        let mask = self.slots.len() - 1;
        let mut index = self.index(&key);
        loop {
            let slot = &self.slots[index];
            if slot.stamp != self.stamp {
                return None;
            }
            if slot.key == key {
                return Some(slot.value);
            }
            index = (index + 1) & mask;
        }
    }

    /// Sets the value of `key`, and returns its previous one.
    #[inline]
    pub fn insert(&mut self, key: K, value: u32) -> Option<u32> {
        if (self.len + 1) * 2 > self.slots.len() {
            self.grow();
        }
        let mask = self.slots.len() - 1;
        let mut index = self.index(&key);
        loop {
            let stamp = self.stamp;
            let slot = &mut self.slots[index];
            if slot.stamp != stamp {
                *slot = Slot { key, value, stamp };
                self.len += 1;
                return None;
            }
            if slot.key == key {
                let previous = slot.value;
                slot.value = value;
                return Some(previous);
            }
            index = (index + 1) & mask;
        }
    }

    fn grow(&mut self) {
        let capacity = (self.slots.len() * 2).max(MINIMUM_CAPACITY);
        let old = std::mem::replace(&mut self.slots, vec![Slot::default(); capacity]);
        let old_stamp = self.stamp;
        self.shift = 64 - capacity.trailing_zeros();
        self.stamp = 1;
        self.len = 0;
        for slot in old {
            if slot.stamp == old_stamp {
                self.insert(slot.key, slot.value);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn inserts_finds_and_empties() {
        let mut table = Table::<u64>::default();
        for key in 0..1000u64 {
            assert_eq!(table.insert(key * 7, key as u32), None);
        }
        for key in 0..1000u64 {
            assert_eq!(table.get(key * 7), Some(key as u32));
        }
        assert_eq!(table.insert(14, 99), Some(2));
        assert_eq!(table.get(15), None);
        table.clear();
        assert_eq!(table.get(14), None);
        assert_eq!(table.insert(14, 5), None);
        assert_eq!(table.get(14), Some(5));
    }
}
