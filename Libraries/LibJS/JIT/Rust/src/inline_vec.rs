/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A vector of copyable values that keeps up to `N` of them inline, for the
//! small per-node lists the register allocator makes for most nodes (their
//! input locations, temps and the like), so that those need no heap
//! allocation.

/// Up to `N` values inline; more spill to the heap.
#[derive(Clone)]
pub struct InlineVec<T: Copy + Default, const N: usize> {
    len: usize,
    inline: [T; N],
    heap: Vec<T>,
}

impl<T: Copy + Default, const N: usize> InlineVec<T, N> {
    pub fn new() -> Self {
        Self {
            len: 0,
            inline: [T::default(); N],
            heap: Vec::new(),
        }
    }

    /// `count` copies of `value`.
    pub fn from_elem(value: T, count: usize) -> Self {
        let mut vec = Self::new();
        for _ in 0..count {
            vec.push(value);
        }
        vec
    }

    pub fn push(&mut self, value: T) {
        if self.len < N {
            self.inline[self.len] = value;
        } else {
            if self.heap.is_empty() {
                self.heap.extend_from_slice(&self.inline);
            }
            self.heap.push(value);
        }
        self.len += 1;
    }
}

impl<T: Copy + Default, const N: usize> Default for InlineVec<T, N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T: Copy + Default, const N: usize> std::ops::Deref for InlineVec<T, N> {
    type Target = [T];

    fn deref(&self) -> &[T] {
        if self.len <= N {
            &self.inline[..self.len]
        } else {
            &self.heap
        }
    }
}

impl<T: Copy + Default, const N: usize> std::ops::DerefMut for InlineVec<T, N> {
    fn deref_mut(&mut self) -> &mut [T] {
        if self.len <= N {
            &mut self.inline[..self.len]
        } else {
            &mut self.heap
        }
    }
}

impl<T: Copy + Default + std::fmt::Debug, const N: usize> std::fmt::Debug for InlineVec<T, N> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_list().entries(self.iter()).finish()
    }
}

impl<T: Copy + Default + PartialEq, const N: usize> PartialEq for InlineVec<T, N> {
    fn eq(&self, other: &Self) -> bool {
        **self == **other
    }
}

impl<T: Copy + Default + Eq, const N: usize> Eq for InlineVec<T, N> {}

impl<'a, T: Copy + Default, const N: usize> IntoIterator for &'a InlineVec<T, N> {
    type Item = &'a T;
    type IntoIter = std::slice::Iter<'a, T>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<T: Copy + Default, const N: usize> FromIterator<T> for InlineVec<T, N> {
    fn from_iter<I: IntoIterator<Item = T>>(iterator: I) -> Self {
        let mut vec = Self::new();
        for value in iterator {
            vec.push(value);
        }
        vec
    }
}

impl<T: Copy + Default, const N: usize> Extend<T> for InlineVec<T, N> {
    fn extend<I: IntoIterator<Item = T>>(&mut self, iterator: I) {
        for value in iterator {
            self.push(value);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::InlineVec;

    #[test]
    fn values_stay_inline_until_they_spill() {
        let mut vec = InlineVec::<u32, 2>::new();
        vec.push(1);
        vec.push(2);
        assert_eq!(&*vec, &[1, 2]);
        vec.push(3);
        vec[0] = 7;
        assert_eq!(&*vec, &[7, 2, 3]);
        assert_eq!(InlineVec::<u8, 4>::from_elem(5, 3).len(), 3);
        assert_eq!((0..6).collect::<InlineVec<u32, 2>>().iter().sum::<u32>(), 15);
    }
}
