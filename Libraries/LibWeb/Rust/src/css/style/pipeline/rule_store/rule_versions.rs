/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::style::fast_hash::fast_hasher;
use crate::css::style::pipeline::rule_store::{RuleKind, RuleVersion};
use crate::css::style::weak_pool::WeakPool;
use std::hash::{Hash, Hasher};
use std::ops::Index;
use std::sync::{Arc, Mutex};

const RULE_VERSIONS_PER_PAGE: usize = 128;
const EMPTY_VERSION: RuleVersion = RuleVersion::new(RuleKind::Style);

pub(in crate::css::style) struct RuleVersionPage {
    values: [RuleVersion; RULE_VERSIONS_PER_PAGE],
    shared_hash: Option<u64>,
}

impl RuleVersionPage {
    fn new(values: [RuleVersion; RULE_VERSIONS_PER_PAGE]) -> Self {
        Self {
            values,
            shared_hash: None,
        }
    }
}

/// The pool equal pages of rule versions are shared through.
pub(in crate::css::style) type RuleVersionPages = Mutex<WeakPool<RuleVersionPage>>;

/// Rule versions contain only identities: numbers, and selector programs compared by address. Equal
/// pages are interned through the process's pool rather than copied.
#[derive(Clone, Default)]
pub(super) struct RuleVersionTable {
    pages: Vec<Arc<RuleVersionPage>>,
    len: usize,
    needs_sharing: bool,
}

impl RuleVersionTable {
    /// The bytes of the table's pages, each counted once.
    pub(super) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        crate::heap_size::vec_bytes(&self.pages) + self.pages.iter().map(|page| sizer.arc_shallow(page)).sum::<usize>()
    }

    pub(super) fn len(&self) -> usize {
        self.len
    }

    pub(super) fn push(&mut self, value: RuleVersion) {
        if self.len.is_multiple_of(RULE_VERSIONS_PER_PAGE) {
            self.pages
                .push(Arc::new(RuleVersionPage::new([EMPTY_VERSION; RULE_VERSIONS_PER_PAGE])));
        }
        let index = self.len;
        self.len = self.len.checked_add(1).expect("rule version space exhausted");
        self.needs_sharing = true;
        self.set(index, value);
    }

    pub(super) fn set(&mut self, index: usize, value: RuleVersion) {
        assert!(index < self.len);
        if self[index] == value {
            return;
        }
        let page = &mut self.pages[index / RULE_VERSIONS_PER_PAGE];
        // NB: A page the pool knows is copied, as one another table holds is: the pool keeps no
        //     page alive, and the copy is published again where the table is next shared.
        if Arc::get_mut(page).is_none() {
            *page = Arc::new(RuleVersionPage::new(page.values.clone()));
        }
        let page = Arc::get_mut(page).expect("a private page has one owner");
        page.shared_hash = None;
        page.values[index % RULE_VERSIONS_PER_PAGE] = value;
        self.needs_sharing = true;
    }

    pub(super) fn share(&mut self, pool: &RuleVersionPages) {
        if !self.needs_sharing {
            return;
        }
        let mut pool = pool.lock().unwrap();
        for page in &mut self.pages {
            if page.shared_hash.is_some() {
                continue;
            }
            let mut hasher = fast_hasher();
            page.values.hash(&mut hasher);
            let hash = hasher.finish();
            if let Some(found) = pool.find(hash, |candidate| candidate.values == page.values) {
                *page = found;
                continue;
            }
            // A clone of the table, as a fork of the style engine, holds the unpublished page too, and publishes a
            // copy of its own.
            if Arc::get_mut(page).is_none() {
                *page = Arc::new(RuleVersionPage::new(page.values.clone()));
            }
            Arc::get_mut(page).expect("a private page has one owner").shared_hash = Some(hash);
            pool.insert(hash, page);
        }
        self.needs_sharing = false;
    }
}

impl Index<usize> for RuleVersionTable {
    type Output = RuleVersion;

    fn index(&self, index: usize) -> &Self::Output {
        assert!(index < self.len);
        &self.pages[index / RULE_VERSIONS_PER_PAGE].values[index % RULE_VERSIONS_PER_PAGE]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn equal_pages_share_and_edits_detach_only_the_changed_page() {
        let pool = RuleVersionPages::default();
        let make_table = || {
            let mut table = RuleVersionTable::default();
            for _ in 0..RULE_VERSIONS_PER_PAGE * 2 + 1 {
                table.push(RuleVersion::new(RuleKind::Style));
            }
            table.share(&pool);
            table
        };
        let first = make_table();
        let mut second = make_table();
        for (left, right) in first.pages.iter().zip(&second.pages) {
            assert!(Arc::ptr_eq(left, right));
        }
        let index = RULE_VERSIONS_PER_PAGE + 7;
        let mut changed = second[index].clone();
        changed.kind = RuleKind::Media;
        second.set(index, changed);
        assert_eq!(first[index].kind, RuleKind::Style);
        assert_eq!(second[index].kind, RuleKind::Media);
        assert!(Arc::ptr_eq(&first.pages[0], &second.pages[0]));
        assert!(!Arc::ptr_eq(&first.pages[1], &second.pages[1]));
        second.share(&pool);
        second.push(RuleVersion::new(RuleKind::Style));
        assert_eq!(first.len(), RULE_VERSIONS_PER_PAGE * 2 + 1);
        assert_eq!(second.len(), first.len() + 1);
        drop(second);
        let third = make_table();
        for (left, right) in first.pages.iter().zip(&third.pages) {
            assert!(Arc::ptr_eq(left, right));
        }
    }

    #[test]
    fn a_clone_shares_the_unpublished_pages_it_holds_with_the_original() {
        let pool = RuleVersionPages::default();
        let mut table = RuleVersionTable::default();
        for index in 0..RULE_VERSIONS_PER_PAGE + 1 {
            // NB: Names no other test gives, so that no page another test published is equal.
            let mut version = RuleVersion::new(RuleKind::Keyframes);
            version.set_declared_name(crate::css::style::StyleAtomID(0x5ee7_0000 + index as u32));
            table.push(version);
        }
        let mut clone = table.clone();
        clone.share(&pool);
        table.share(&pool);
        for (left, right) in table.pages.iter().zip(&clone.pages) {
            assert!(Arc::ptr_eq(left, right));
        }
    }
}
