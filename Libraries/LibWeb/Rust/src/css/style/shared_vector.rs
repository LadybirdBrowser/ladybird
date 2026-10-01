/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::capacity::ShallowCapacityBytes;
use super::fast_hash::fast_hasher;
use super::memory::{MemoryCategory, MemoryLease};
use super::weak_pool::WeakPool;
use std::hash::{Hash, Hasher};
use std::ops::Deref;
use std::sync::Arc;

/// A pool of shared vector contents of one element type.
pub(super) struct SharedVectorPool<T: 'static> {
    pool: WeakPool<SharedVectorData<T>>,
    category: MemoryCategory,
}

impl<T> SharedVectorPool<T> {
    pub(super) fn new(category: MemoryCategory) -> Self {
        Self {
            pool: WeakPool::default(),
            category,
        }
    }
}

struct SharedVectorData<T: 'static> {
    values: Vec<T>,
    hash: u64,
    _memory: MemoryLease,
}

enum Storage<T: 'static> {
    Owned(Vec<T>),
    Shared(Arc<SharedVectorData<T>>),
}

/// A flat vector that can share equal contents between documents. Its allocation is charged
/// once while shared; nested payloads, if any, remain their owners' accounting responsibility.
pub(super) struct SharedVector<T: 'static> {
    storage: Storage<T>,
}

impl<T> Default for SharedVector<T> {
    fn default() -> Self {
        Self {
            storage: Storage::Owned(Vec::new()),
        }
    }
}

impl<T: std::fmt::Debug> std::fmt::Debug for SharedVector<T> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        std::fmt::Debug::fmt(self.as_slice(), formatter)
    }
}

impl<T: Clone> Clone for SharedVector<T> {
    fn clone(&self) -> Self {
        Self {
            storage: match &self.storage {
                Storage::Owned(values) => Storage::Owned(values.clone()),
                Storage::Shared(data) => Storage::Shared(Arc::clone(data)),
            },
        }
    }
}

impl<T: PartialEq> PartialEq for SharedVector<T> {
    fn eq(&self, other: &Self) -> bool {
        if let (Storage::Shared(first), Storage::Shared(second)) = (&self.storage, &other.storage)
            && Arc::ptr_eq(first, second)
        {
            return true;
        }
        self.as_slice() == other.as_slice()
    }
}

impl<T: Eq> Eq for SharedVector<T> {}

impl<T: Hash> Hash for SharedVector<T> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        // Frozen vectors already computed this content hash when entering their pool.
        // Hash owned vectors the same way so equal storage forms always hash alike.
        let hash = match &self.storage {
            Storage::Shared(data) => data.hash,
            Storage::Owned(values) => {
                let mut hasher = fast_hasher();
                values.hash(&mut hasher);
                hasher.finish()
            }
        };
        hash.hash(state);
    }
}

impl<T> FromIterator<T> for SharedVector<T> {
    fn from_iter<I: IntoIterator<Item = T>>(iter: I) -> Self {
        Self {
            storage: Storage::Owned(iter.into_iter().collect()),
        }
    }
}

impl<T> Deref for SharedVector<T> {
    type Target = Vec<T>;

    fn deref(&self) -> &Self::Target {
        match &self.storage {
            Storage::Owned(values) => values,
            Storage::Shared(data) => &data.values,
        }
    }
}

impl<T: Clone> SharedVector<T> {
    pub(super) fn make_mut(&mut self) -> &mut Vec<T> {
        if matches!(self.storage, Storage::Shared(_)) {
            let Storage::Shared(data) = std::mem::replace(&mut self.storage, Storage::Owned(Vec::new())) else {
                unreachable!()
            };
            let values = match Arc::try_unwrap(data) {
                Ok(mut data) => std::mem::take(&mut data.values),
                Err(data) => data.values.clone(),
            };
            self.storage = Storage::Owned(values);
        }
        let Storage::Owned(values) = &mut self.storage else {
            unreachable!()
        };
        values
    }

    pub(super) fn clear(&mut self) {
        match &mut self.storage {
            Storage::Owned(values) => values.clear(),
            Storage::Shared(_) => self.storage = Storage::Owned(Vec::new()),
        }
    }
}

impl<T: Clone + Eq + Hash> SharedVector<T> {
    pub(super) fn share(&mut self, pool: &mut SharedVectorPool<T>) {
        let Storage::Owned(values) = &self.storage else { return };
        if values.is_empty() {
            return;
        }
        let mut hasher = fast_hasher();
        values.hash(&mut hasher);
        let hash = hasher.finish();
        let Storage::Owned(values) = std::mem::replace(&mut self.storage, Storage::Owned(Vec::new())) else {
            unreachable!()
        };
        let shared = match pool.pool.find(hash, |data| data.values == values) {
            Some(found) => found,
            None => {
                let mut memory = MemoryLease::new(pool.category);
                memory.resize_required_to(
                    &mut pool.pool.memory,
                    size_of::<SharedVectorData<T>>() as u64 + values.shallow_capacity_bytes(),
                );
                let shared = Arc::new(SharedVectorData {
                    values,
                    hash,
                    _memory: memory,
                });
                pool.pool.insert(hash, &shared);
                shared
            }
        };
        self.storage = Storage::Shared(shared);
    }
}

impl<T> ShallowCapacityBytes for SharedVector<T> {
    fn shallow_capacity_bytes(&self) -> u64 {
        match &self.storage {
            Storage::Owned(values) => values.shallow_capacity_bytes(),
            Storage::Shared(_) => 0,
        }
    }
}

const SHARED_VECTOR_PAGE_SIZE: usize = 128;

/// An append-only column whose equal immutable pages can be shared between snapshots.
#[derive(Clone)]
pub(super) struct PagedSharedVector<T: 'static> {
    pages: Vec<SharedVector<T>>,
    len: usize,
}

impl<T> Default for PagedSharedVector<T> {
    fn default() -> Self {
        Self {
            pages: Vec::new(),
            len: 0,
        }
    }
}

impl<T> PagedSharedVector<T> {
    pub(super) fn len(&self) -> usize {
        self.len
    }

    pub(super) fn iter(&self) -> impl Iterator<Item = &T> {
        self.pages.iter().flat_map(|page| page.iter())
    }

    pub(super) fn range(&self, range: std::ops::Range<usize>) -> impl Iterator<Item = &T> {
        assert!(range.start <= range.end && range.end <= self.len);
        range.map(|index| &self[index])
    }

    pub(super) fn shrink_to_fit(&mut self) {
        self.pages.shrink_to_fit();
        for page in &mut self.pages {
            if let Storage::Owned(values) = &mut page.storage {
                values.shrink_to_fit();
            }
        }
    }
}

impl<T: Clone> PagedSharedVector<T> {
    pub(super) fn push(&mut self, value: T) {
        let next_len = self.len.checked_add(1).expect("shared column identity space exhausted");
        if self.len.is_multiple_of(SHARED_VECTOR_PAGE_SIZE) {
            self.pages.push(SharedVector {
                storage: Storage::Owned(Vec::with_capacity(SHARED_VECTOR_PAGE_SIZE)),
            });
        }
        let values = self.pages.last_mut().unwrap().make_mut();
        values.reserve_exact(SHARED_VECTOR_PAGE_SIZE - values.len());
        values.push(value);
        self.len = next_len;
    }
}

impl<T: Clone> Extend<T> for PagedSharedVector<T> {
    fn extend<I: IntoIterator<Item = T>>(&mut self, iter: I) {
        for value in iter {
            self.push(value);
        }
    }
}

impl<T> std::ops::Index<usize> for PagedSharedVector<T> {
    type Output = T;

    fn index(&self, index: usize) -> &T {
        &self.pages[index / SHARED_VECTOR_PAGE_SIZE][index % SHARED_VECTOR_PAGE_SIZE]
    }
}

impl<T: Clone + Eq + Hash> PagedSharedVector<T> {
    pub(super) fn share(&mut self, pool: &mut SharedVectorPool<T>) {
        for page in &mut self.pages {
            page.share(pool);
        }
    }
}

impl<T> ShallowCapacityBytes for PagedSharedVector<T> {
    fn shallow_capacity_bytes(&self) -> u64 {
        self.pages.shallow_capacity_bytes()
            + self
                .pages
                .iter()
                .map(ShallowCapacityBytes::shallow_capacity_bytes)
                .sum::<u64>()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn equal_vectors_share_and_mutation_detaches_without_changing_other_owners() {
        let mut pool = SharedVectorPool::new(MemoryCategory::RuleProgram);
        let mut make_vector = || {
            let mut vector = SharedVector::default();
            vector.make_mut().extend([1, 2, 3]);
            vector.share(&mut pool);
            vector
        };
        let first = make_vector();
        let mut second = make_vector();
        let (Storage::Shared(left), Storage::Shared(right)) = (&first.storage, &second.storage) else {
            panic!("equal vectors should be shared");
        };
        assert!(Arc::ptr_eq(left, right));
        second.make_mut()[0] = 9;
        assert_eq!(first.as_slice(), &[1, 2, 3]);
        assert_eq!(second.as_slice(), &[9, 2, 3]);
        second.share(&mut pool);
        let previous_buffer = second.as_ptr();
        // The pool owns only a weak reference: the last document can recover its buffer.
        assert_eq!(second.make_mut().as_ptr(), previous_buffer);
        second.clear();
        assert!(second.is_empty());
        assert_eq!(first.as_slice(), &[1, 2, 3]);
    }
}
