/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A vector that starts with the elements of a frozen one it shares with others, and goes on with
//! its own: a structure built once for the process, such as the invalidation map of the user
//! agent's rules, is the shared start of every document's, which adds its own after it, and the
//! numbers of the shared elements are the same in every document.

use std::ops::{Index, IndexMut, Range};
use std::sync::Arc;

#[derive(Clone)]
pub(super) struct SharedPrefixVec<T> {
    shared: Arc<[T]>,
    own: Vec<T>,
}

impl<T> Default for SharedPrefixVec<T> {
    fn default() -> Self {
        Self {
            shared: Arc::from(Vec::new()),
            own: Vec::new(),
        }
    }
}

impl<T: Clone> SharedPrefixVec<T> {
    pub(super) fn len(&self) -> usize {
        self.shared.len() + self.own.len()
    }

    pub(super) fn push(&mut self, value: T) {
        self.own.push(value);
    }

    #[cfg(test)]
    pub(super) fn iter(&self) -> impl Iterator<Item = &T> {
        self.shared.iter().chain(&self.own)
    }

    pub(super) fn shrink_to_fit(&mut self) {
        self.own.shrink_to_fit();
    }

    /// Make every element shared: clones of the vector then share all of them.
    pub(super) fn freeze(&mut self) {
        if self.own.is_empty() {
            return;
        }
        let mut all = Vec::with_capacity(self.len());
        all.extend_from_slice(&self.shared);
        all.append(&mut self.own);
        self.shared = Arc::from(all);
        self.own = Vec::new();
    }

    /// Whether the vector shares every element `base` holds, all of which it shares.
    #[cfg(test)]
    pub(super) fn shares_prefix_of(&self, base: &Self) -> bool {
        base.own.is_empty() && Arc::ptr_eq(&self.shared, &base.shared)
    }

    /// The bytes of the vector's own elements.
    pub(super) fn heap_size(&self) -> usize {
        crate::heap_size::vec_bytes(&self.own)
    }

    /// The bytes of the elements the vector shares.
    pub(super) fn shared_heap_size(&self) -> usize {
        size_of_val::<[T]>(&self.shared)
    }
}

impl<T> Index<usize> for SharedPrefixVec<T> {
    type Output = T;

    fn index(&self, index: usize) -> &T {
        match index.checked_sub(self.shared.len()) {
            Some(own) => &self.own[own],
            None => &self.shared[index],
        }
    }
}

impl<T> IndexMut<usize> for SharedPrefixVec<T> {
    fn index_mut(&mut self, index: usize) -> &mut T {
        let own = index
            .checked_sub(self.shared.len())
            .expect("a shared element is never written");
        &mut self.own[own]
    }
}

/// A range of elements all shared or all the vector's own, as the elements one thing added are.
impl<T> Index<Range<usize>> for SharedPrefixVec<T> {
    type Output = [T];

    fn index(&self, range: Range<usize>) -> &[T] {
        let shared = self.shared.len();
        if range.end <= shared {
            return &self.shared[range];
        }
        assert!(range.start >= shared, "a range is all shared or all own");
        &self.own[range.start - shared..range.end - shared]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_clone_of_a_frozen_vector_shares_its_elements_and_adds_its_own() {
        let mut base = SharedPrefixVec::default();
        for value in [1, 2, 3] {
            base.push(value);
        }
        base.freeze();
        let mut document = base.clone();
        document.push(4);
        document.push(5);
        assert!(Arc::ptr_eq(&base.shared, &document.shared));
        assert_eq!(document.len(), 5);
        assert_eq!(document[1], 2);
        assert_eq!(document[4], 5);
        assert_eq!(&document[3..5], &[4, 5]);
        assert_eq!(&document[0..2], &[1, 2]);
        document[3] = 6;
        document[4] = 7;
        assert_eq!(document.iter().copied().collect::<Vec<_>>(), [1, 2, 3, 6, 7]);
        assert_eq!(base.len(), 3);
    }
}
