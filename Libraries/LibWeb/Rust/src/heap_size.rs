/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Counting the heap bytes a structure holds, for a memory report by owner.
//!
//! A count is what the structure asked the allocator for: a vector's capacity, a hash table's
//! buckets and control bytes, a box's pointee. It does not ask the allocator what it rounded the
//! requests up to, so a count is the same on every allocator and every run of the same document.
//!
//! An allocation several owners hold, as an `Arc` is, is counted once, by the first owner that
//! reaches it: the [`HeapSizer`] remembers the address of every shared allocation it counted.

use crate::fast_hash::FastSet as HashSet;
use smallvec::SmallVec;
use std::sync::Arc;

/// The reference counts in front of the value of an `Arc`.
pub(crate) const ARC_HEADER_BYTES: usize = 2 * size_of::<usize>();
/// The count before a style value, which `StyleValueArc` keeps without a weak count.
pub(crate) const STYLE_VALUE_HEADER_BYTES: usize = size_of::<usize>();

/// What a memory report has counted so far.
#[derive(Default)]
pub(crate) struct HeapSizer {
    seen: HashSet<usize>,
    /// The bytes of the style values counted so far, which belong to no one owner: every owner
    /// holding a value shares it.
    pub(crate) style_value_bytes: usize,
}

impl HeapSizer {
    /// Whether the shared allocation at `pointer` is reached for the first time, which the caller
    /// then counts. A null pointer is never counted.
    pub(crate) fn first_sight<T: ?Sized>(&mut self, pointer: *const T) -> bool {
        let address = pointer.cast::<()>() as usize;
        address != 0 && self.seen.insert(address)
    }

    /// The bytes of `arc`'s allocation and what its value holds, the first time it is reached.
    pub(crate) fn arc<T: HeapSize + ?Sized>(&mut self, arc: &Arc<T>) -> usize {
        if !self.first_sight(Arc::as_ptr(arc)) {
            return 0;
        }
        ARC_HEADER_BYTES + size_of_val::<T>(arc) + (**arc).heap_size(self)
    }

    /// The bytes of `arc`'s allocation alone, the first time it is reached, for a value whose heap
    /// is counted elsewhere or not at all.
    pub(crate) fn arc_shallow<T: ?Sized>(&mut self, arc: &Arc<T>) -> usize {
        if !self.first_sight(Arc::as_ptr(arc)) {
            return 0;
        }
        ARC_HEADER_BYTES + size_of_val::<T>(arc)
    }
}

/// The heap bytes a value holds beyond its own size.
pub(crate) trait HeapSize {
    fn heap_size(&self, sizer: &mut HeapSizer) -> usize;
}

macro_rules! no_heap {
    ($($type:ty),* $(,)?) => {
        $(impl HeapSize for $type {
            fn heap_size(&self, _: &mut HeapSizer) -> usize {
                0
            }
        })*
    };
}

no_heap!(bool, u8, u16, u32, u64, usize, i8, i16, i32, i64, f32, f64, ());

/// The bytes of a vector's buffer, whose elements hold nothing on the heap.
pub(crate) fn vec_bytes<T>(vector: &Vec<T>) -> usize {
    vector.capacity() * size_of::<T>()
}

/// The bytes of a small vector's buffer where it spilled to the heap, whose elements hold nothing
/// on the heap.
pub(crate) fn small_vec_bytes<A: smallvec::Array>(vector: &SmallVec<A>) -> usize {
    if vector.spilled() {
        vector.capacity() * size_of::<A::Item>()
    } else {
        0
    }
}

/// The buckets a hash table of the standard library holds for `capacity` entries.
fn hash_table_buckets(capacity: usize) -> usize {
    if capacity == 0 {
        0
    } else if capacity < 8 {
        (capacity + 1).next_power_of_two()
    } else {
        (capacity * 8 / 7).next_power_of_two()
    }
}

/// The bytes of a hash table of `capacity` entries of `entry_size` bytes: its buckets and their
/// control bytes.
pub(crate) fn hash_table_bytes(capacity: usize, entry_size: usize) -> usize {
    let buckets = hash_table_buckets(capacity);
    if buckets == 0 {
        return 0;
    }
    // NB: One control byte per bucket, and one group of them repeated past the end.
    const GROUP_WIDTH: usize = 16;
    buckets * entry_size + buckets + GROUP_WIDTH
}

/// The bytes of a map's table, whose keys and values hold nothing on the heap.
pub(crate) fn map_bytes<K, V, S>(map: &std::collections::HashMap<K, V, S>) -> usize {
    hash_table_bytes(map.capacity(), size_of::<(K, V)>())
}

/// The bytes the `len` entries of a hash table of `entry_size` bytes hold, with their control
/// bytes, but for the room the table reserved beyond them. A table every document of the process
/// fills reports this, so that what one document adds to it does not depend on how much room the
/// documents before it had the table reserve.
pub(crate) fn live_hash_table_bytes(len: usize, entry_size: usize) -> usize {
    len * (entry_size + 1)
}

/// The bytes of a set's table, whose keys hold nothing on the heap.
pub(crate) fn set_bytes<K, S>(set: &std::collections::HashSet<K, S>) -> usize {
    hash_table_bytes(set.capacity(), size_of::<K>())
}

impl<T: HeapSize> HeapSize for Vec<T> {
    fn heap_size(&self, sizer: &mut HeapSizer) -> usize {
        vec_bytes(self) + self.iter().map(|item| item.heap_size(sizer)).sum::<usize>()
    }
}

impl<T: HeapSize> HeapSize for [T] {
    fn heap_size(&self, sizer: &mut HeapSizer) -> usize {
        self.iter().map(|item| item.heap_size(sizer)).sum()
    }
}

impl<T: HeapSize, const N: usize> HeapSize for [T; N] {
    fn heap_size(&self, sizer: &mut HeapSizer) -> usize {
        self.iter().map(|item| item.heap_size(sizer)).sum()
    }
}

impl<T: HeapSize + ?Sized> HeapSize for Box<T> {
    fn heap_size(&self, sizer: &mut HeapSizer) -> usize {
        size_of_val::<T>(self) + (**self).heap_size(sizer)
    }
}

impl<T: HeapSize + ?Sized> HeapSize for Arc<T> {
    fn heap_size(&self, sizer: &mut HeapSizer) -> usize {
        sizer.arc(self)
    }
}

impl<T: HeapSize> HeapSize for Option<T> {
    fn heap_size(&self, sizer: &mut HeapSizer) -> usize {
        self.as_ref().map_or(0, |value| value.heap_size(sizer))
    }
}

impl<A: smallvec::Array> HeapSize for SmallVec<A>
where
    A::Item: HeapSize,
{
    fn heap_size(&self, sizer: &mut HeapSizer) -> usize {
        small_vec_bytes(self) + self.iter().map(|item| item.heap_size(sizer)).sum::<usize>()
    }
}

impl<K: HeapSize, V: HeapSize, S> HeapSize for std::collections::HashMap<K, V, S> {
    fn heap_size(&self, sizer: &mut HeapSizer) -> usize {
        map_bytes(self)
            + self
                .iter()
                .map(|(key, value)| key.heap_size(sizer) + value.heap_size(sizer))
                .sum::<usize>()
    }
}

impl<K: HeapSize, S> HeapSize for std::collections::HashSet<K, S> {
    fn heap_size(&self, sizer: &mut HeapSizer) -> usize {
        set_bytes(self) + self.iter().map(|key| key.heap_size(sizer)).sum::<usize>()
    }
}

impl<A: HeapSize, B: HeapSize> HeapSize for (A, B) {
    fn heap_size(&self, sizer: &mut HeapSizer) -> usize {
        self.0.heap_size(sizer) + self.1.heap_size(sizer)
    }
}

impl<A: HeapSize, B: HeapSize, C: HeapSize> HeapSize for (A, B, C) {
    fn heap_size(&self, sizer: &mut HeapSizer) -> usize {
        self.0.heap_size(sizer) + self.1.heap_size(sizer) + self.2.heap_size(sizer)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_shared_allocation_is_counted_once() {
        let shared = Arc::new(vec![1u32; 4]);
        let holders = vec![Arc::clone(&shared), Arc::clone(&shared)];
        let mut sizer = HeapSizer::default();
        let bytes = holders.heap_size(&mut sizer);
        assert_eq!(
            bytes,
            holders.capacity() * size_of::<Arc<Vec<u32>>>()
                + ARC_HEADER_BYTES
                + size_of::<Vec<u32>>()
                + shared.capacity() * size_of::<u32>()
        );
    }
}
