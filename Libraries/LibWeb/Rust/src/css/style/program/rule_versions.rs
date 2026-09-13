/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{CascadeLayerID, RuleID, RuleKind, RuleVersion, StyleScopeID};
use crate::css::style::capacity::ShallowCapacityBytes;
use crate::css::style::fast_hash::{FastMap as HashMap, fast_hasher};
use crate::css::style::memory::{DeviceClass, MemoryCategory, MemoryController, MemoryLease};
use std::cell::RefCell;
use std::hash::{Hash, Hasher};
use std::ops::Index;
use std::rc::{Rc, Weak};

const RULE_VERSIONS_PER_PAGE: usize = 128;
const EMPTY_VERSION: RuleVersion = RuleVersion {
    rule: RuleID(0),
    kind: RuleKind::Style,
    declared_name: None,
    selector_program: None,
    declaration_block: None,
    activation_predicate: None,
    layer: CascadeLayerID::UNLAYERED,
    scope: StyleScopeID::NONE,
};

struct RuleVersionPage {
    values: [RuleVersion; RULE_VERSIONS_PER_PAGE],
    shared_hash: Option<u64>,
    _memory: MemoryLease,
}

impl RuleVersionPage {
    fn new(values: [RuleVersion; RULE_VERSIONS_PER_PAGE]) -> Self {
        let mut memory = MemoryLease::new(MemoryCategory::RuleProgram);
        SHARED_RULE_VERSION_PAGES.with_borrow_mut(|pool| {
            memory.resize_required_to(&mut pool.memory, size_of::<Self>() as u64);
        });
        Self {
            values,
            shared_hash: None,
            _memory: memory,
        }
    }
}

impl Clone for RuleVersionPage {
    fn clone(&self) -> Self {
        Self::new(self.values)
    }
}

impl Drop for RuleVersionPage {
    fn drop(&mut self) {
        forget_dead_pages(self.shared_hash);
    }
}

struct SharedRuleVersionPages {
    by_hash: HashMap<u64, Vec<Weak<RuleVersionPage>>>,
    memory: MemoryController,
}

thread_local! {
    static SHARED_RULE_VERSION_PAGES: RefCell<SharedRuleVersionPages> = RefCell::new(SharedRuleVersionPages {
        by_hash: HashMap::default(),
        memory: MemoryController::new(DeviceClass::ForegroundDesktop),
    });
}

fn forget_dead_pages(hash: Option<u64>) {
    let Some(hash) = hash else { return };
    let _ = SHARED_RULE_VERSION_PAGES.try_with(|pool| {
        let Ok(mut pool) = pool.try_borrow_mut() else { return };
        if let std::collections::hash_map::Entry::Occupied(mut entry) = pool.by_hash.entry(hash) {
            entry.get_mut().retain(|page| page.strong_count() != 0);
            if entry.get().is_empty() {
                entry.remove();
            }
        }
    });
}

/// Rule versions contain only numeric identities. Share equal pages rather than whole tables,
/// so appending author rules does not duplicate the common non-author prefix in every document.
#[derive(Default)]
pub(super) struct RuleVersionTable {
    pages: Vec<Rc<RuleVersionPage>>,
    len: usize,
    needs_sharing: bool,
}

impl RuleVersionTable {
    pub(super) fn len(&self) -> usize {
        self.len
    }

    pub(super) fn push(&mut self, value: RuleVersion) {
        if self.len.is_multiple_of(RULE_VERSIONS_PER_PAGE) {
            self.pages
                .push(Rc::new(RuleVersionPage::new([EMPTY_VERSION; RULE_VERSIONS_PER_PAGE])));
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
        let previous_hash = page.shared_hash;
        let page = Rc::make_mut(page);
        page.shared_hash = None;
        page.values[index % RULE_VERSIONS_PER_PAGE] = value;
        self.needs_sharing = true;
        // make_mut can detach the pool's weak reference even when this was the only owner.
        forget_dead_pages(previous_hash);
    }

    pub(super) fn share(&mut self) {
        if !self.needs_sharing {
            return;
        }
        for page in &mut self.pages {
            if page.shared_hash.is_some() {
                continue;
            }
            let mut hasher = fast_hasher();
            page.values.hash(&mut hasher);
            let hash = hasher.finish();
            let shared = SHARED_RULE_VERSION_PAGES.with_borrow_mut(|pool| {
                let bucket = pool.by_hash.entry(hash).or_default();
                bucket.retain(|candidate| candidate.strong_count() != 0);
                if let Some(found) = bucket
                    .iter()
                    .filter_map(Weak::upgrade)
                    .find(|candidate| candidate.values == page.values)
                {
                    return found;
                }
                Rc::get_mut(page).expect("an unpublished page is private").shared_hash = Some(hash);
                bucket.push(Rc::downgrade(page));
                Rc::clone(page)
            });
            *page = shared;
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

impl ShallowCapacityBytes for RuleVersionTable {
    fn shallow_capacity_bytes(&self) -> u64 {
        // Page allocations are charged once to the process ledger, including private pages.
        self.pages.shallow_capacity_bytes()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn equal_pages_share_and_edits_detach_only_the_changed_page() {
        let make_table = || {
            let mut table = RuleVersionTable::default();
            for index in 0..RULE_VERSIONS_PER_PAGE * 2 + 1 {
                table.push(RuleVersion::new(RuleID(index as u32), RuleKind::Style));
            }
            table.share();
            table
        };
        let first = make_table();
        let mut second = make_table();
        for (left, right) in first.pages.iter().zip(&second.pages) {
            assert!(Rc::ptr_eq(left, right));
        }
        let index = RULE_VERSIONS_PER_PAGE + 7;
        let mut changed = second[index];
        changed.kind = RuleKind::Media;
        second.set(index, changed);
        assert_eq!(first[index].kind, RuleKind::Style);
        assert_eq!(second[index].kind, RuleKind::Media);
        assert!(Rc::ptr_eq(&first.pages[0], &second.pages[0]));
        assert!(!Rc::ptr_eq(&first.pages[1], &second.pages[1]));
        second.share();
        second.push(RuleVersion::new(RuleID(second.len() as u32), RuleKind::Style));
        assert_eq!(first.len(), RULE_VERSIONS_PER_PAGE * 2 + 1);
        assert_eq!(second.len(), first.len() + 1);
        drop(second);
        let third = make_table();
        for (left, right) in first.pages.iter().zip(&third.pages) {
            assert!(Rc::ptr_eq(left, right));
        }
    }
}
