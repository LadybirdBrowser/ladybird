/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AK::HashTable, for the places where the runtime walks one where the spec walks a List: the order it visits its
//! values in is the order of its buckets, which is observable, so this keeps the buckets of AK::HashTable.

use ak::Utf16FlyString;

use crate::utf16::Utf16View;

/// AK::string_hash over a sequence of characters.
fn string_hash(characters: impl Iterator<Item = u32>) -> u32 {
    let mut hash: u32 = 0;
    for character in characters {
        hash = hash.wrapping_add(character);
        hash = hash.wrapping_add(hash << 10);
        hash ^= hash >> 6;
    }
    hash = hash.wrapping_add(hash << 3);
    hash ^= hash >> 11;
    hash = hash.wrapping_add(hash << 15);
    hash
}

/// AK::string_hash over the code units of a string, which is what Utf16FlyString::hash() computes for ASCII and
/// UTF-16 storage alike.
pub fn utf16_fly_string_hash(string: &Utf16FlyString) -> u32 {
    string_hash(Utf16View::of_fly_string(string).code_units().map(u32::from))
}

/// AK::Traits<T>::hash() of the values a HashTable holds.
pub trait HashTableTraits: Eq {
    fn hash(&self) -> u32;
}

impl HashTableTraits for Utf16FlyString {
    fn hash(&self) -> u32 {
        utf16_fly_string_hash(self)
    }
}

/// The bytes of a ByteString, which AK hashes with string_hash.
impl HashTableTraits for Vec<u8> {
    fn hash(&self) -> u32 {
        string_hash(self.iter().map(|&byte| u32::from(byte)))
    }
}

struct Bucket<T> {
    value: T,
    hash: u32,
    probe_length: usize,
}

/// Open addressing with linear probing and Robin Hood displacement in a power-of-two table that doubles once it is
/// 70% full, like AK::HashTable.
pub struct HashTable<T> {
    buckets: Vec<Option<Bucket<T>>>,
    size: usize,
}

impl<T> Default for HashTable<T> {
    fn default() -> Self {
        Self {
            buckets: Vec::new(),
            size: 0,
        }
    }
}

pub type Utf16FlyStringHashTable = HashTable<Utf16FlyString>;

impl<T: HashTableTraits> HashTable<T> {
    const GROW_CAPACITY_AT_LEAST: usize = 8;
    const GROW_AT_LOAD_FACTOR_PERCENT: usize = 70;

    pub fn is_empty(&self) -> bool {
        self.size == 0
    }

    pub fn size(&self) -> usize {
        self.size
    }

    fn capacity(&self) -> usize {
        self.buckets.len()
    }

    fn mask(&self) -> usize {
        self.capacity() - 1
    }

    fn should_grow(&self) -> bool {
        (self.size + 1) * 100 >= self.capacity() * Self::GROW_AT_LOAD_FACTOR_PERCENT
    }

    /// HashTable::set, which replaces an equal value that is already in the table.
    pub fn set(&mut self, value: T) {
        if self.should_grow() {
            self.rehash((self.capacity() * 2).max(Self::GROW_CAPACITY_AT_LEAST));
        }
        self.write_value(value);
    }

    pub fn contains(&self, value: &T) -> bool {
        if self.is_empty() {
            return false;
        }
        let hash = value.hash();
        let mut bucket_index = hash as usize & self.mask();
        loop {
            let Some(bucket) = &self.buckets[bucket_index] else {
                return false;
            };
            if bucket.hash == hash && bucket.value == *value {
                return true;
            }
            bucket_index = (bucket_index + 1) & self.mask();
        }
    }

    /// The values in the order iterating an AK::HashTable visits them: by bucket.
    pub fn iter(&self) -> impl Iterator<Item = &T> {
        self.buckets.iter().flatten().map(|bucket| &bucket.value)
    }

    fn rehash(&mut self, new_capacity: usize) {
        let new_capacity = new_capacity.max(self.capacity() + 1).next_power_of_two();
        let old_buckets = core::mem::replace(&mut self.buckets, (0..new_capacity).map(|_| None).collect());
        self.size = 0;
        for bucket in old_buckets.into_iter().flatten() {
            self.write_value(bucket.value);
        }
    }

    fn write_value(&mut self, value: T) {
        let hash = value.hash();
        let mask = self.mask();
        let mut bucket_index = hash as usize & mask;
        let mut probe_length = 0;
        loop {
            let Some(bucket) = &mut self.buckets[bucket_index] else {
                self.buckets[bucket_index] = Some(Bucket {
                    value,
                    hash,
                    probe_length,
                });
                self.size += 1;
                return;
            };

            if bucket.hash == hash && bucket.value == value {
                bucket.value = value;
                return;
            }

            // Robin hood: if our probe length is larger (poor) than this bucket's (rich), steal its position!
            if probe_length > bucket.probe_length {
                let mut bucket_to_move = core::mem::replace(
                    bucket,
                    Bucket {
                        value,
                        hash,
                        probe_length,
                    },
                );
                probe_length = bucket_to_move.probe_length;

                // Find a free bucket, swapping with smaller probe length buckets along the way
                loop {
                    bucket_index = (bucket_index + 1) & mask;
                    probe_length += 1;

                    let Some(bucket) = &mut self.buckets[bucket_index] else {
                        bucket_to_move.probe_length = probe_length;
                        self.buckets[bucket_index] = Some(bucket_to_move);
                        break;
                    };

                    if probe_length > bucket.probe_length {
                        bucket_to_move.probe_length = probe_length;
                        core::mem::swap(&mut bucket_to_move, bucket);
                        probe_length = bucket_to_move.probe_length;
                    }
                }

                self.size += 1;
                return;
            }

            bucket_index = (bucket_index + 1) & mask;
            probe_length += 1;
        }
    }
}
