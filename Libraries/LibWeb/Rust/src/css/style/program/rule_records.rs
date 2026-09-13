/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::Rule;
use crate::css::style::capacity::ShallowCapacityBytes;
use crate::css::style::fast_hash::{FastMap as HashMap, fast_hasher};
use crate::css::style::memory::{DeviceClass, MemoryCategory, MemoryController, MemoryLease};
use std::cell::RefCell;
use std::hash::{Hash, Hasher};
use std::ops::{Index, IndexMut};
use std::rc::{Rc, Weak};

const RULE_RECORDS_PER_PAGE: usize = 128;

struct RuleRecordPage {
    values: [Option<Rule>; RULE_RECORDS_PER_PAGE],
    shared_hash: Option<u64>,
    _memory: MemoryLease,
}

impl RuleRecordPage {
    fn new(values: [Option<Rule>; RULE_RECORDS_PER_PAGE]) -> Self {
        let mut memory = MemoryLease::new(MemoryCategory::RuleProgram);
        SHARED_RULE_RECORD_PAGES.with_borrow_mut(|pool| {
            memory.resize_required_to(&mut pool.memory, size_of::<Self>() as u64);
        });
        Self {
            values,
            shared_hash: None,
            _memory: memory,
        }
    }
}

impl Clone for RuleRecordPage {
    fn clone(&self) -> Self {
        Self::new(self.values.clone())
    }
}

impl Drop for RuleRecordPage {
    fn drop(&mut self) {
        forget_dead_pages(self.shared_hash);
    }
}

struct SharedRuleRecordPages {
    by_hash: HashMap<u64, Vec<Weak<RuleRecordPage>>>,
    memory: MemoryController,
}

thread_local! {
    static SHARED_RULE_RECORD_PAGES: RefCell<SharedRuleRecordPages> = RefCell::new(SharedRuleRecordPages {
        by_hash: HashMap::default(),
        memory: MemoryController::new(DeviceClass::ForegroundDesktop),
    });
}

fn forget_dead_pages(hash: Option<u64>) {
    let Some(hash) = hash else { return };
    let _ = SHARED_RULE_RECORD_PAGES.try_with(|pool| {
        let Ok(mut pool) = pool.try_borrow_mut() else { return };
        if let std::collections::hash_map::Entry::Occupied(mut entry) = pool.by_hash.entry(hash) {
            entry.get_mut().retain(|page| page.strong_count() != 0);
            if entry.get().is_empty() {
                entry.remove();
            }
        }
    });
}

/// Equal rule records share immutable pages. Document-local child lists remain outside the
/// records; mutations detach only their page, including updates to its child-list slot.
#[derive(Default)]
pub(super) struct RuleRecordTable {
    pages: Vec<Rc<RuleRecordPage>>,
    len: usize,
    needs_sharing: bool,
}

impl RuleRecordTable {
    pub(super) fn len(&self) -> usize {
        self.len
    }

    pub(super) fn push(&mut self, value: Rule) {
        if self.len.is_multiple_of(RULE_RECORDS_PER_PAGE) {
            self.pages
                .push(Rc::new(RuleRecordPage::new(std::array::from_fn(|_| None))));
        }
        let index = self.len;
        self.len = self.len.checked_add(1).expect("rule record space exhausted");
        self.needs_sharing = true;
        self.set(index, value);
    }

    fn page_mut(&mut self, index: usize) -> &mut RuleRecordPage {
        assert!(index < self.len);
        self.needs_sharing = true;
        let page = &mut self.pages[index / RULE_RECORDS_PER_PAGE];
        let previous_hash = page.shared_hash;
        let page = Rc::make_mut(page);
        page.shared_hash = None;
        // make_mut can detach the pool's weak reference even when this was the only owner.
        forget_dead_pages(previous_hash);
        page
    }

    fn set(&mut self, index: usize, value: Rule) {
        self.page_mut(index).values[index % RULE_RECORDS_PER_PAGE] = Some(value);
    }

    pub(super) fn iter(&self) -> impl Iterator<Item = &Rule> {
        self.pages
            .iter()
            .flat_map(|page| page.values.iter())
            .take(self.len)
            .map(|rule| rule.as_ref().expect("a live rule slot has a record"))
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
            let shared = SHARED_RULE_RECORD_PAGES.with_borrow_mut(|pool| {
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

impl Index<usize> for RuleRecordTable {
    type Output = Rule;

    fn index(&self, index: usize) -> &Self::Output {
        assert!(index < self.len);
        self.pages[index / RULE_RECORDS_PER_PAGE].values[index % RULE_RECORDS_PER_PAGE]
            .as_ref()
            .expect("a live rule slot has a record")
    }
}

impl IndexMut<usize> for RuleRecordTable {
    fn index_mut(&mut self, index: usize) -> &mut Self::Output {
        self.page_mut(index).values[index % RULE_RECORDS_PER_PAGE]
            .as_mut()
            .expect("a live rule slot has a record")
    }
}

impl ShallowCapacityBytes for RuleRecordTable {
    fn shallow_capacity_bytes(&self) -> u64 {
        // Page allocations are charged once to the process ledger, including private pages.
        self.pages.shallow_capacity_bytes()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::style::program::{CascadeOrigin, RuleID, RuleKind, StyleSheetObjectID, StyleSheetProgram};

    #[test]
    fn shared_rule_records_detach_flags_and_keep_child_lists_document_local() {
        let make_program = || {
            let mut program = StyleSheetProgram::new();
            let sheet = program.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
            for _ in 0..RULE_RECORDS_PER_PAGE * 2 + 1 {
                program.append_rule(sheet, None, RuleKind::Style);
            }
            program.share_rule_storage();
            program
        };
        let first = make_program();
        let mut second = make_program();
        assert!(!second.set_rule_live(RuleID(0), first.rules[0].live));
        for (left, right) in first.rules.pages.iter().zip(&second.rules.pages) {
            assert!(Rc::ptr_eq(left, right));
        }
        let changed = RuleID((RULE_RECORDS_PER_PAGE + 7) as u32);
        second.set_rule_conditions_hold(changed, false);
        assert!(first.rule_conditions_hold(changed));
        assert!(!second.rule_conditions_hold(changed));
        assert!(Rc::ptr_eq(&first.rules.pages[0], &second.rules.pages[0]));
        assert!(!Rc::ptr_eq(&first.rules.pages[1], &second.rules.pages[1]));
        second.share_rule_storage();

        let parent = RuleID(0);
        let child = second.append_rule(second.rule_sheet(parent), Some(parent), RuleKind::Style);
        assert!(first.rule_children(parent).is_empty());
        assert_eq!(second.rule_children(parent), &[child]);
        assert!(!Rc::ptr_eq(&first.rules.pages[0], &second.rules.pages[0]));
        drop(second);
        let third = make_program();
        for (left, right) in first.rules.pages.iter().zip(&third.rules.pages) {
            assert!(Rc::ptr_eq(left, right));
        }
    }
}
