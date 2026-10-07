/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::Rule;
use crate::css::style::capacity::ShallowCapacityBytes;
use crate::css::style::fast_hash::fast_hasher;
use crate::css::style::memory::{MemoryCategory, MemoryController, MemoryLease};
use crate::css::style::weak_pool::WeakPool;
use std::hash::{Hash, Hasher};
use std::ops::{Index, IndexMut};
use std::sync::{Arc, Mutex, MutexGuard, OnceLock};

const RULE_RECORDS_PER_PAGE: usize = 128;

struct RuleRecordPage {
    values: [Option<Rule>; RULE_RECORDS_PER_PAGE],
    shared_hash: Option<u64>,
    _memory: MemoryLease,
}

impl RuleRecordPage {
    fn new(values: [Option<Rule>; RULE_RECORDS_PER_PAGE], memory_controller: &mut MemoryController) -> Self {
        let mut memory = MemoryLease::new(MemoryCategory::RuleProgram);
        memory.resize_required_to(memory_controller, size_of::<Self>() as u64);
        Self {
            values,
            shared_hash: None,
            _memory: memory,
        }
    }
}

fn rule_record_pages() -> MutexGuard<'static, WeakPool<RuleRecordPage>> {
    // Every engine interns through the same pool, from whichever thread runs it. Pages never reach
    // back into the pool when they are dropped, so the lock is only held while interning.
    static RULE_RECORD_PAGES: OnceLock<Mutex<WeakPool<RuleRecordPage>>> = OnceLock::new();
    RULE_RECORD_PAGES.get_or_init(Mutex::default).lock().unwrap()
}

/// Equal rule records share immutable pages through the process's pool. Child lists remain
/// outside the records; mutations detach only their page, including updates to its child-list slot.
#[derive(Clone, Default)]
pub(super) struct RuleRecordTable {
    pages: Vec<Arc<RuleRecordPage>>,
    len: usize,
    needs_sharing: bool,
}

impl RuleRecordTable {
    pub(super) fn len(&self) -> usize {
        self.len
    }

    pub(super) fn push(&mut self, value: Rule) {
        if self.len.is_multiple_of(RULE_RECORDS_PER_PAGE) {
            self.pages.push(Arc::new(RuleRecordPage::new(
                std::array::from_fn(|_| None),
                &mut rule_record_pages().memory,
            )));
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
        if Arc::get_mut(page).is_none() {
            rule_record_pages().make_private(page.shared_hash, page, |page, memory| {
                RuleRecordPage::new(page.values.clone(), memory)
            });
        }
        let page = Arc::get_mut(page).expect("a private page has one owner");
        page.shared_hash = None;
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
        let mut pool = rule_record_pages();
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
                *page = Arc::new(RuleRecordPage::new(page.values.clone(), &mut pool.memory));
            }
            Arc::get_mut(page).expect("a private page has one owner").shared_hash = Some(hash);
            pool.insert(hash, page);
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
        // Page allocations are charged to the pool's ledger, including private pages.
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
            assert!(Arc::ptr_eq(left, right));
        }
        let changed = RuleID((RULE_RECORDS_PER_PAGE + 7) as u32);
        second.set_rule_conditions_hold(changed, false);
        assert!(first.rule_conditions_hold(changed));
        assert!(!second.rule_conditions_hold(changed));
        assert!(Arc::ptr_eq(&first.rules.pages[0], &second.rules.pages[0]));
        assert!(!Arc::ptr_eq(&first.rules.pages[1], &second.rules.pages[1]));
        second.share_rule_storage();

        let parent = RuleID(0);
        let child = second.append_rule(second.rule_sheet(parent), Some(parent), RuleKind::Style);
        assert!(first.rule_children(parent).is_empty());
        assert_eq!(second.rule_children(parent), &[child]);
        assert!(!Arc::ptr_eq(&first.rules.pages[0], &second.rules.pages[0]));
        drop(second);
        let third = make_program();
        for (left, right) in first.rules.pages.iter().zip(&third.rules.pages) {
            assert!(Arc::ptr_eq(left, right));
        }
    }

    #[test]
    fn a_clone_shares_the_unpublished_pages_it_holds_with_the_original() {
        let mut program = StyleSheetProgram::new();
        let sheet = program.add_sheet(StyleSheetObjectID(0x5ee7), CascadeOrigin::Author);
        for _ in 0..RULE_RECORDS_PER_PAGE + 1 {
            program.append_rule(sheet, None, RuleKind::Style);
        }
        let mut clone = program.clone();
        clone.share_rule_storage();
        program.share_rule_storage();
        for (left, right) in program.rules.pages.iter().zip(&clone.rules.pages) {
            assert!(Arc::ptr_eq(left, right));
        }
    }
}
