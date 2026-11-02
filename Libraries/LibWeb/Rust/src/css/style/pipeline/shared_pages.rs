/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A vector held in pages that documents holding equal pages share through a pool of the
//! process's: frames of a site whose rules are numbered alike hold one copy of each page.
//!
//! A page is shared once it is published to the pool, and copied by the first edit after that, so
//! an edit never reaches another holder of the page.

use super::super::weak_pool::WeakPool;
use std::hash::{Hash, Hasher};
use std::sync::{Arc, Mutex};

pub(super) const VALUES_PER_PAGE: usize = 128;

#[derive(Clone)]
pub(super) struct Page<T> {
    values: Vec<T>,
    /// The hash the page is published to the pool under, or none while it is unpublished.
    shared_hash: Option<u64>,
}

/// The pool of a process's pages of one kind.
pub(super) type PagePool<T> = Mutex<WeakPool<Page<T>>>;

#[derive(Clone)]
pub(super) struct SharedPages<T> {
    pages: Vec<Arc<Page<T>>>,
    len: usize,
}

impl<T> Default for SharedPages<T> {
    fn default() -> Self {
        Self {
            pages: Vec::new(),
            len: 0,
        }
    }
}

impl<T: Clone + PartialEq + Hash> SharedPages<T> {
    pub(super) fn len(&self) -> usize {
        self.len
    }

    pub(super) fn get(&self, index: usize) -> Option<&T> {
        (index < self.len).then(|| &self.pages[index / VALUES_PER_PAGE].values[index % VALUES_PER_PAGE])
    }

    pub(super) fn get_mut(&mut self, index: usize) -> &mut T {
        assert!(index < self.len);
        let page = Arc::make_mut(&mut self.pages[index / VALUES_PER_PAGE]);
        page.shared_hash = None;
        &mut page.values[index % VALUES_PER_PAGE]
    }

    pub(super) fn push(&mut self, value: T) {
        if self.len.is_multiple_of(VALUES_PER_PAGE) {
            self.pages.push(Arc::new(Page {
                values: Vec::with_capacity(VALUES_PER_PAGE),
                shared_hash: None,
            }));
        }
        let page = Arc::make_mut(self.pages.last_mut().expect("a page has room"));
        page.shared_hash = None;
        page.values.push(value);
        self.len += 1;
    }

    /// Keep the first `len` values alone.
    pub(super) fn truncate(&mut self, len: usize) {
        if len >= self.len {
            return;
        }
        self.pages.truncate(len.div_ceil(VALUES_PER_PAGE));
        if let Some(last) = self.pages.last_mut()
            && !len.is_multiple_of(VALUES_PER_PAGE)
        {
            let page = Arc::make_mut(last);
            page.shared_hash = None;
            page.values.truncate(len % VALUES_PER_PAGE);
        }
        self.len = len;
    }

    pub(super) fn shrink_to_fit(&mut self) {
        self.pages.shrink_to_fit();
        if let Some(last) = self.pages.last_mut()
            && last.values.len() != last.values.capacity()
        {
            Arc::make_mut(last).values.shrink_to_fit();
        }
    }

    /// Publish the pages to `pool`, taking an equal page another holder published instead of each.
    pub(super) fn share(&mut self, pool: &PagePool<T>) {
        let mut pool = pool.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        for page in &mut self.pages {
            if page.shared_hash.is_some() {
                continue;
            }
            let mut hasher = super::super::fast_hash::fast_hasher();
            page.values.hash(&mut hasher);
            let hash = hasher.finish();
            if let Some(found) = pool.find(hash, |candidate| candidate.values == page.values) {
                *page = found;
                continue;
            }
            Arc::make_mut(page).shared_hash = Some(hash);
            pool.insert(hash, page);
        }
    }

    /// The bytes of the pages, each counted the first time `sizer` reaches it, with what
    /// `value_heap` says each value holds.
    pub(super) fn heap_size(
        &self,
        sizer: &mut crate::heap_size::HeapSizer,
        mut value_heap: impl FnMut(&T, &mut crate::heap_size::HeapSizer) -> usize,
    ) -> usize {
        let mut bytes = crate::heap_size::vec_bytes(&self.pages);
        for page in &self.pages {
            if sizer.first_sight(Arc::as_ptr(page)) {
                bytes += crate::heap_size::ARC_HEADER_BYTES
                    + size_of::<Page<T>>()
                    + crate::heap_size::vec_bytes(&page.values)
                    + page.values.iter().map(|value| value_heap(value, sizer)).sum::<usize>();
            }
        }
        bytes
    }
}

impl<T: Clone + PartialEq + Hash> std::ops::Index<usize> for SharedPages<T> {
    type Output = T;

    fn index(&self, index: usize) -> &T {
        self.get(index).expect("an index within the pages")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn equal_pages_share_and_an_edit_copies_only_its_page() {
        let pool = PagePool::<u32>::default();
        let make = || {
            let mut pages = SharedPages::default();
            for value in 0..(VALUES_PER_PAGE * 2 + 1) as u32 {
                pages.push(value);
            }
            pages.share(&pool);
            pages
        };
        let first = make();
        let mut second = make();
        for (left, right) in first.pages.iter().zip(&second.pages) {
            assert!(Arc::ptr_eq(left, right));
        }
        *second.get_mut(VALUES_PER_PAGE + 3) = 7;
        assert_eq!(first[VALUES_PER_PAGE + 3], (VALUES_PER_PAGE + 3) as u32);
        assert_eq!(second[VALUES_PER_PAGE + 3], 7);
        assert!(Arc::ptr_eq(&first.pages[0], &second.pages[0]));
        assert!(!Arc::ptr_eq(&first.pages[1], &second.pages[1]));
        second.push(9);
        assert_eq!(first.len(), VALUES_PER_PAGE * 2 + 1);
        assert_eq!(second.len(), first.len() + 1);
        assert_eq!(second[first.len()], 9);
    }
}
