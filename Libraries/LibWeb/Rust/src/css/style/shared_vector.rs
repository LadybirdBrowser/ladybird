/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::capacity::ShallowCapacityBytes;
use super::fast_hash::{FastMap as HashMap, fast_hasher};
use super::memory::{DeviceClass, MemoryCategory, MemoryController, MemoryLease};
use std::cell::RefCell;
use std::hash::{Hash, Hasher};
use std::ops::Deref;
use std::rc::{Rc, Weak};
use std::thread::LocalKey;

pub(super) struct SharedVectorPool<T: 'static> {
    by_hash: HashMap<u64, Vec<Weak<SharedVectorData<T>>>>,
    memory: MemoryController,
    category: MemoryCategory,
}

impl<T> SharedVectorPool<T> {
    pub(super) fn new(category: MemoryCategory) -> Self {
        Self {
            by_hash: HashMap::default(),
            memory: MemoryController::new(DeviceClass::ForegroundDesktop),
            category,
        }
    }
}

struct SharedVectorData<T: 'static> {
    values: Vec<T>,
    hash: u64,
    pool: &'static LocalKey<RefCell<SharedVectorPool<T>>>,
    _memory: MemoryLease,
}

impl<T> Drop for SharedVectorData<T> {
    fn drop(&mut self) {
        let _ = self.pool.try_with(|pool| {
            let Ok(mut pool) = pool.try_borrow_mut() else { return };
            if let std::collections::hash_map::Entry::Occupied(mut entry) = pool.by_hash.entry(self.hash) {
                entry.get_mut().retain(|values| values.strong_count() != 0);
                if entry.get().is_empty() {
                    entry.remove();
                }
            }
        });
    }
}

enum Storage<T: 'static> {
    Owned(Vec<T>),
    Shared(Rc<SharedVectorData<T>>),
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

impl<T: Clone> Clone for SharedVector<T> {
    fn clone(&self) -> Self {
        Self {
            storage: match &self.storage {
                Storage::Owned(values) => Storage::Owned(values.clone()),
                Storage::Shared(data) => Storage::Shared(Rc::clone(data)),
            },
        }
    }
}

impl<T: PartialEq> PartialEq for SharedVector<T> {
    fn eq(&self, other: &Self) -> bool {
        self.as_slice() == other.as_slice()
    }
}

impl<T: Eq> Eq for SharedVector<T> {}

impl<T: Hash> Hash for SharedVector<T> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.as_slice().hash(state);
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
            let values = match Rc::try_unwrap(data) {
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
    pub(super) fn share(&mut self, pool_key: &'static LocalKey<RefCell<SharedVectorPool<T>>>) {
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
        let shared = pool_key.with_borrow_mut(|pool| {
            let bucket = pool.by_hash.entry(hash).or_default();
            bucket.retain(|values| values.strong_count() != 0);
            if let Some(found) = bucket
                .iter()
                .filter_map(Weak::upgrade)
                .find(|data| data.values == values)
            {
                return found;
            }
            let mut memory = MemoryLease::new(pool.category);
            memory.resize_required_to(
                &mut pool.memory,
                size_of::<SharedVectorData<T>>() as u64 + values.shallow_capacity_bytes(),
            );
            let shared = Rc::new(SharedVectorData {
                values,
                hash,
                pool: pool_key,
                _memory: memory,
            });
            bucket.push(Rc::downgrade(&shared));
            shared
        });
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

#[cfg(test)]
mod tests {
    use super::*;

    thread_local! {
        static POOL: RefCell<SharedVectorPool<u32>> = RefCell::new(SharedVectorPool::new(MemoryCategory::RuleProgram));
    }

    #[test]
    fn equal_vectors_share_and_mutation_detaches_without_changing_other_owners() {
        let make_vector = || {
            let mut vector = SharedVector::default();
            vector.make_mut().extend([1, 2, 3]);
            vector.share(&POOL);
            vector
        };
        let first = make_vector();
        let mut second = make_vector();
        let (Storage::Shared(left), Storage::Shared(right)) = (&first.storage, &second.storage) else {
            panic!("equal vectors should be shared");
        };
        assert!(Rc::ptr_eq(left, right));
        second.make_mut()[0] = 9;
        assert_eq!(first.as_slice(), &[1, 2, 3]);
        assert_eq!(second.as_slice(), &[9, 2, 3]);
        second.share(&POOL);
        let previous_buffer = second.as_ptr();
        // The pool owns only a weak reference: the last document can recover its buffer.
        assert_eq!(second.make_mut().as_ptr(), previous_buffer);
        second.clear();
        assert!(second.is_empty());
        assert_eq!(first.as_slice(), &[1, 2, 3]);
    }
}
