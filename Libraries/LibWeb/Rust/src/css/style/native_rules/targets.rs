/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::NativeRuleTarget;
use crate::css::container_conditions::ContainerConditionsData;
use crate::css::declaration_block::DeclarationBlockData;
use crate::css::style::capacity::ShallowCapacityBytes;
use crate::css::style::fast_hash::fast_hasher;
use crate::css::style::memory::{MemoryCategory, MemoryController, MemoryLease};
use crate::css::style::program::RuleID;
use crate::css::style::weak_pool::WeakPool;
use std::hash::{Hash, Hasher};
use std::num::NonZeroU64;
use std::sync::{Arc, Mutex, MutexGuard, OnceLock};

const TARGETS_PER_PAGE: usize = 128;

#[derive(Clone)]

struct TargetPage {
    values: [Option<NativeRuleTarget>; TARGETS_PER_PAGE],
    live_count: u16,
    shared_hash: Option<u64>,
    memory: MemoryLease,
}

impl TargetPage {
    fn new(values: [Option<NativeRuleTarget>; TARGETS_PER_PAGE], memory_controller: &mut MemoryController) -> Self {
        let mut memory = MemoryLease::new(MemoryCategory::RuleProgram);
        let nested_bytes: u64 = values.iter().flatten().map(NativeRuleTarget::owned_bytes).sum();
        memory.resize_required_to(memory_controller, size_of::<Self>() as u64 + nested_bytes);
        Self {
            live_count: u16::try_from(values.iter().flatten().count()).unwrap(),
            values,
            shared_hash: None,
            memory,
        }
    }
}

#[derive(Clone)]

struct BoundTargetPage {
    data: Arc<TargetPage>,
    identity_base: u64,
    source_base: u64,
}

impl BoundTargetPage {
    fn new(memory: &mut MemoryController) -> Self {
        Self {
            data: Arc::new(TargetPage::new(std::array::from_fn(|_| None), memory)),
            identity_base: 0,
            source_base: 0,
        }
    }

    fn normalize(&mut self) {
        debug_assert_eq!(self.identity_base, 0);
        debug_assert_eq!(self.source_base, 0);
        let data = Arc::make_mut(&mut self.data);
        self.identity_base = data
            .values
            .iter()
            .flatten()
            .map(|target| target.identity.get())
            .min()
            .unwrap_or(1)
            - 1;
        self.source_base = data
            .values
            .iter()
            .flatten()
            .map(|target| target.source_identity)
            .min()
            .unwrap_or(0);
        for target in data.values.iter_mut().flatten() {
            target.identity = NonZeroU64::new(target.identity.get() - self.identity_base).unwrap();
            target.source_identity -= self.source_base;
        }
    }
}

/// Borrow shared payloads while resolving this document's native identities from its page binding.
pub(in crate::css::style) struct NativeRuleTargetRef<'a> {
    data: &'a NativeRuleTarget,
    pub identity: NonZeroU64,
    pub source_identity: u64,
}

impl<'a> NativeRuleTargetRef<'a> {
    pub(in crate::css::style) fn declarations(&self) -> Option<&'a Arc<DeclarationBlockData>> {
        self.data.declarations.as_ref()
    }

    pub(in crate::css::style) fn layer_name(&self) -> &'a [u16] {
        self.data.layer_name()
    }

    pub(in crate::css::style) fn containers(&self) -> &'a [Arc<ContainerConditionsData>] {
        self.data.containers()
    }
}

fn target_pages() -> MutexGuard<'static, WeakPool<TargetPage>> {
    // Every engine interns through the same pool, from whichever thread runs it. Pages never reach
    // back into the pool when they are dropped, so the lock is only held while interning.
    static TARGET_PAGES: OnceLock<Mutex<WeakPool<TargetPage>>> = OnceLock::new();
    TARGET_PAGES.get_or_init(Mutex::default).lock().unwrap()
}

/// Dense semantic rule identities address immutable pages directly. Store native identities
/// relative to each page's document-local bases so copies of one parsed stylesheet can share
/// payloads, through the process's pool, while retaining distinct native rule and stylesheet
/// identities.
#[derive(Clone, Default)]
pub(in crate::css::style) struct NativeRuleTargets {
    pages: Vec<Option<BoundTargetPage>>,
    needs_sharing: bool,
}

impl NativeRuleTargets {
    fn page_mut(&mut self, index: usize) -> &mut TargetPage {
        self.needs_sharing = true;
        let page = self.pages[index / TARGETS_PER_PAGE].as_mut().unwrap();
        if Arc::get_mut(&mut page.data).is_none() {
            target_pages().make_private(page.data.shared_hash, &mut page.data, |data, memory| {
                TargetPage::new(data.values.clone(), memory)
            });
        }
        let data = Arc::get_mut(&mut page.data).expect("a private page has one owner");
        if page.identity_base != 0 || page.source_base != 0 {
            for target in data.values.iter_mut().flatten() {
                target.identity =
                    NonZeroU64::new(target.identity.get().checked_add(page.identity_base).unwrap()).unwrap();
                target.source_identity = target.source_identity.checked_add(page.source_base).unwrap();
            }
            page.identity_base = 0;
            page.source_base = 0;
        }
        data.shared_hash = None;
        data
    }

    pub(in crate::css::style) fn get(&self, id: &RuleID) -> Option<NativeRuleTargetRef<'_>> {
        let index = id.0 as usize;
        let page = self.pages.get(index / TARGETS_PER_PAGE)?.as_ref()?;
        let data = page.data.values[index % TARGETS_PER_PAGE].as_ref()?;
        Some(NativeRuleTargetRef {
            data,
            identity: NonZeroU64::new(data.identity.get().checked_add(page.identity_base).unwrap()).unwrap(),
            source_identity: data.source_identity.checked_add(page.source_base).unwrap(),
        })
    }

    pub(in crate::css::style) fn get_mut(&mut self, id: &RuleID) -> Option<&mut NativeRuleTarget> {
        self.get(id)?;
        let index = id.0 as usize;
        self.page_mut(index).values[index % TARGETS_PER_PAGE].as_mut()
    }

    pub(super) fn insert(&mut self, id: RuleID, target: NativeRuleTarget) {
        let index = id.0 as usize;
        let page_index = index / TARGETS_PER_PAGE;
        if self.pages.len() <= page_index {
            self.pages.resize_with(page_index + 1, || None);
        }
        if self.pages[page_index].is_none() {
            self.pages[page_index] = Some(BoundTargetPage::new(&mut target_pages().memory));
        }
        let page = self.page_mut(index);
        page.memory.grow_committed(target.owned_bytes());
        if let Some(previous) = page.values[index % TARGETS_PER_PAGE].replace(target) {
            page.memory.shrink_committed(previous.owned_bytes());
        } else {
            page.live_count += 1;
        }
    }

    pub(super) fn remove(&mut self, id: &RuleID) -> Option<NativeRuleTarget> {
        self.get(id)?;
        let index = id.0 as usize;
        let page = self.page_mut(index);
        let target = page.values[index % TARGETS_PER_PAGE].take().unwrap();
        page.memory.shrink_committed(target.owned_bytes());
        page.live_count -= 1;
        if page.live_count == 0 {
            self.pages[index / TARGETS_PER_PAGE] = None;
        }
        Some(target)
    }

    #[cfg(test)]
    pub(super) fn len(&self) -> usize {
        self.pages
            .iter()
            .flatten()
            .map(|page| usize::from(page.data.live_count))
            .sum()
    }

    pub(in crate::css::style) fn share(&mut self) {
        if !self.needs_sharing {
            return;
        }
        let mut pool = target_pages();
        for page in self.pages.iter_mut().flatten() {
            if page.data.shared_hash.is_some() {
                continue;
            }
            page.normalize();
            let mut hasher = fast_hasher();
            page.data.values.hash(&mut hasher);
            let hash = hasher.finish();
            if let Some(found) = pool.find(hash, |candidate| candidate.values == page.data.values) {
                page.data = found;
                continue;
            }
            Arc::make_mut(&mut page.data).shared_hash = Some(hash);
            pool.insert(hash, &page.data);
        }
        self.needs_sharing = false;
    }
}

impl ShallowCapacityBytes for NativeRuleTargets {
    fn shallow_capacity_bytes(&self) -> u64 {
        self.pages.shallow_capacity_bytes()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::num::NonZeroU64;

    fn target(identity: u64) -> NativeRuleTarget {
        NativeRuleTarget {
            identity: NonZeroU64::new(identity).unwrap(),
            declarations: None,
            source_identity: 1,
            conditions: None,
        }
    }

    #[test]
    fn relative_pages_preserve_native_identities_across_sharing_and_mutation() {
        let mut first = NativeRuleTargets::default();
        let mut second = NativeRuleTargets::default();
        for (base, table) in [(0, &mut first), (1000, &mut second)] {
            for index in 0..TARGETS_PER_PAGE {
                let mut value = target(index as u64 + 1 + base);
                value.source_identity += base;
                table.insert(RuleID(index as u32), value);
            }
        }
        first.insert(RuleID(TARGETS_PER_PAGE as u32), target(200));
        second.insert(RuleID(TARGETS_PER_PAGE as u32), target(201));
        first.share();
        second.share();
        assert!(Arc::ptr_eq(
            &first.pages[0].as_ref().unwrap().data,
            &second.pages[0].as_ref().unwrap().data
        ));
        assert!(Arc::ptr_eq(
            &first.pages[1].as_ref().unwrap().data,
            &second.pages[1].as_ref().unwrap().data
        ));
        assert_eq!(first.get(&RuleID(TARGETS_PER_PAGE as u32)).unwrap().identity.get(), 200);
        assert_eq!(
            second.get(&RuleID(TARGETS_PER_PAGE as u32)).unwrap().identity.get(),
            201
        );
        assert_eq!(first.get(&RuleID(7)).unwrap().source_identity, 1);
        assert_eq!(second.get(&RuleID(7)).unwrap().source_identity, 1001);
        assert_eq!(second.remove(&RuleID(7)).unwrap().identity.get(), 1008);
        assert!(second.get(&RuleID(7)).is_none());
        assert_eq!(first.get(&RuleID(7)).unwrap().identity.get(), 8);
        assert!(!Arc::ptr_eq(
            &first.pages[0].as_ref().unwrap().data,
            &second.pages[0].as_ref().unwrap().data
        ));
        let mut replacement = target(1008);
        replacement.source_identity = 1001;
        second.insert(RuleID(7), replacement);
        second.share();
        assert!(Arc::ptr_eq(
            &first.pages[0].as_ref().unwrap().data,
            &second.pages[0].as_ref().unwrap().data
        ));
        second.get_mut(&RuleID(7)).unwrap().identity = NonZeroU64::new(300).unwrap();
        assert_eq!(first.get(&RuleID(7)).unwrap().identity.get(), 8);
        assert_eq!(second.get(&RuleID(7)).unwrap().identity.get(), 300);
        for index in 0..=TARGETS_PER_PAGE {
            second.remove(&RuleID(index as u32));
        }
        assert_eq!(second.len(), 0);
        assert!(second.pages.iter().all(Option::is_none));
        let mut largest = target(u64::MAX);
        largest.source_identity = u64::MAX;
        second.insert(RuleID(0), largest);
        second.share();
        assert_eq!(second.get(&RuleID(0)).unwrap().identity.get(), u64::MAX);
        assert_eq!(second.get(&RuleID(0)).unwrap().source_identity, u64::MAX);
        let removed = second.remove(&RuleID(0)).unwrap();
        assert_eq!(removed.identity.get(), u64::MAX);
        assert_eq!(removed.source_identity, u64::MAX);
        assert_eq!(size_of::<Option<NativeRuleTarget>>(), size_of::<NativeRuleTarget>());
    }
}
