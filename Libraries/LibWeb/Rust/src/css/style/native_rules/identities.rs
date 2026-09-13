/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::style::capacity::ShallowCapacityBytes;
use crate::css::style::fast_hash::FastMap as HashMap;
use crate::css::style::program::RuleID;
use std::num::NonZeroU32;

const IDENTITIES_PER_PAGE: usize = 128;

struct IdentityPage {
    slots: [Option<NonZeroU32>; IDENTITIES_PER_PAGE],
    live_count: u16,
}

/// Native identities are allocated in ranges by parsed stylesheets. Hash only their page
/// number, then read the compact semantic identity directly from the page's slot.
#[derive(Default)]
pub(in crate::css::style) struct NativeRuleIdentities {
    pages: HashMap<u64, Box<IdentityPage>>,
}

impl NativeRuleIdentities {
    pub(in crate::css::style) fn get(&self, identity: &u64) -> Option<RuleID> {
        let page = self.pages.get(&(identity / IDENTITIES_PER_PAGE as u64))?;
        page.slots[(*identity % IDENTITIES_PER_PAGE as u64) as usize].map(|id| RuleID(id.get() - 1))
    }

    pub(super) fn insert(&mut self, identity: u64, id: RuleID) {
        let stored_id = NonZeroU32::new(id.0.checked_add(1).expect("semantic rule identity space exhausted")).unwrap();
        let page = self
            .pages
            .entry(identity / IDENTITIES_PER_PAGE as u64)
            .or_insert_with(|| {
                Box::new(IdentityPage {
                    slots: [None; IDENTITIES_PER_PAGE],
                    live_count: 0,
                })
            });
        if page.slots[(identity % IDENTITIES_PER_PAGE as u64) as usize]
            .replace(stored_id)
            .is_none()
        {
            page.live_count += 1;
        }
    }

    pub(super) fn remove(&mut self, identity: &u64) -> Option<RuleID> {
        let page_number = identity / IDENTITIES_PER_PAGE as u64;
        let page = self.pages.get_mut(&page_number)?;
        let id = page.slots[(*identity % IDENTITIES_PER_PAGE as u64) as usize].take()?;
        page.live_count -= 1;
        if page.live_count == 0 {
            self.pages.remove(&page_number);
        }
        Some(RuleID(id.get() - 1))
    }

    #[cfg(test)]
    pub(super) fn is_empty(&self) -> bool {
        self.pages.is_empty()
    }

    #[cfg(test)]
    pub(super) fn contains_key(&self, identity: &u64) -> bool {
        self.get(identity).is_some()
    }
}

impl ShallowCapacityBytes for NativeRuleIdentities {
    fn shallow_capacity_bytes(&self) -> u64 {
        self.pages.shallow_capacity_bytes() + (self.pages.len() * size_of::<IdentityPage>()) as u64
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identity_pages_handle_sparse_ranges_overwrites_and_empty_page_release() {
        let mut identities = NativeRuleIdentities::default();
        for (identity, rule) in [(1, 0), (127, 1), (128, 2), (u64::MAX, 3)] {
            identities.insert(identity, RuleID(rule));
        }
        assert_eq!(identities.pages.len(), 3);
        identities.insert(1, RuleID(10));
        assert_eq!(identities.get(&1), Some(RuleID(10)));
        assert_eq!(identities.pages[&0].live_count, 2);
        assert_eq!(identities.get(&u64::MAX), Some(RuleID(3)));
        assert_eq!(identities.get(&2), None);
        assert_eq!(identities.remove(&2), None);
        for identity in [1, 127, 128, u64::MAX] {
            assert!(identities.remove(&identity).is_some());
        }
        assert!(identities.is_empty());
    }
}
