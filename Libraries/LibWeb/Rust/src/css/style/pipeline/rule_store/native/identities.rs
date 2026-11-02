/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::style::pipeline::rule_store::RuleID;
use std::collections::BTreeMap;

/// Native identities that follow one another by a constant step, naming rules that follow one another.
#[derive(Clone, Copy)]
struct IdentityRun {
    first_rule: u32,
    count: u32,
    /// How far each identity is from the one before.
    step: u32,
}

impl IdentityRun {
    /// Where `identity` is in the run starting at `first`, if it is there.
    fn place_of(self, first: u64, identity: u64) -> Option<u32> {
        let distance = identity - first;
        let step = u64::from(self.step);
        (distance.is_multiple_of(step) && distance / step < u64::from(self.count)).then(|| (distance / step) as u32)
    }

    fn last_identity(self, first: u64) -> u64 {
        first + u64::from(self.count - 1) * u64::from(self.step)
    }
}

/// The rule each native identity names. A parsed sheet numbers its rules in one range, and a document publishes them
/// in that order, so a sheet's rules take one run however many they are: what a document holds per rule of a sheet
/// other documents share is nothing, as the rest of a rule's records are shared pages.
#[derive(Clone, Default)]
pub(in crate::css::style) struct NativeRuleIdentities {
    /// The runs, by their first identity.
    runs: BTreeMap<u64, IdentityRun>,
}

impl NativeRuleIdentities {
    fn run_of(&self, identity: u64) -> Option<(u64, IdentityRun, u32)> {
        let (&first, &run) = self.runs.range(..=identity).next_back()?;
        Some((first, run, run.place_of(first, identity)?))
    }

    pub(in crate::css::style) fn get(&self, identity: &u64) -> Option<RuleID> {
        let (_, run, place) = self.run_of(*identity)?;
        Some(RuleID(run.first_rule + place))
    }

    /// Have no run span `identity`, which no run holds: the identities of a run past it make a run of their own.
    fn split_at(&mut self, identity: u64) {
        let Some((&first, run)) = self.runs.range_mut(..identity).next_back() else {
            return;
        };
        if run.last_identity(first) < identity {
            return;
        }
        let before = (identity - first).div_ceil(u64::from(run.step)) as u32;
        let after = IdentityRun {
            first_rule: run.first_rule + before,
            count: run.count - before,
            step: run.step,
        };
        run.count = before;
        self.runs
            .insert(first + u64::from(before) * u64::from(after.step), after);
    }

    pub(super) fn insert(&mut self, identity: u64, id: RuleID) {
        self.remove(&identity);
        self.split_at(identity);
        if let Some((&first, run)) = self.runs.range_mut(..identity).next_back() {
            let last_rule = run.first_rule + run.count - 1;
            let last = run.last_identity(first);
            let extends = last < identity
                && last_rule.checked_add(1) == Some(id.0)
                && u32::try_from(identity - last).is_ok_and(|step| run.count == 1 || step == run.step);
            if extends {
                run.step = u32::try_from(identity - last).expect("the step fits");
                run.count += 1;
                return;
            }
        }
        self.runs.insert(
            identity,
            IdentityRun {
                first_rule: id.0,
                count: 1,
                step: 1,
            },
        );
    }

    pub(super) fn remove(&mut self, identity: &u64) -> Option<RuleID> {
        let (first, run, place) = self.run_of(*identity)?;
        self.runs.remove(&first);
        if place > 0 {
            self.runs.insert(first, IdentityRun { count: place, ..run });
        }
        let after = run.count - place - 1;
        if after > 0 {
            self.runs.insert(
                identity + u64::from(run.step),
                IdentityRun {
                    first_rule: run.first_rule + place + 1,
                    count: after,
                    step: run.step,
                },
            );
        }
        Some(RuleID(run.first_rule + place))
    }

    #[cfg(test)]
    pub(super) fn is_empty(&self) -> bool {
        self.runs.is_empty()
    }

    #[cfg(test)]
    pub(super) fn contains_key(&self, identity: &u64) -> bool {
        self.get(identity).is_some()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_sheet_published_in_order_takes_one_run() {
        let mut identities = NativeRuleIdentities::default();
        for index in 0..1000 {
            identities.insert(5000 + index * 2, RuleID(7 + index as u32));
        }
        assert_eq!(identities.runs.len(), 1);
        assert_eq!(identities.get(&5000), Some(RuleID(7)));
        assert_eq!(identities.get(&5002), Some(RuleID(8)));
        assert_eq!(identities.get(&5001), None);
        assert_eq!(identities.get(&(5000 + 999 * 2)), Some(RuleID(1006)));
        assert_eq!(identities.get(&(5000 + 1000 * 2)), None);
        assert_eq!(identities.get(&4998), None);
    }

    #[test]
    fn removing_splits_a_run_and_overwriting_moves_an_identity() {
        let mut identities = NativeRuleIdentities::default();
        for index in 0..10 {
            identities.insert(100 + index, RuleID(index as u32));
        }
        assert_eq!(identities.remove(&104), Some(RuleID(4)));
        assert_eq!(identities.remove(&104), None);
        assert_eq!(identities.runs.len(), 2);
        assert_eq!(identities.get(&103), Some(RuleID(3)));
        assert_eq!(identities.get(&105), Some(RuleID(5)));
        identities.insert(102, RuleID(50));
        assert_eq!(identities.get(&102), Some(RuleID(50)));
        assert_eq!(identities.get(&101), Some(RuleID(1)));
        assert_eq!(identities.get(&103), Some(RuleID(3)));
        for identity in [100, 101, 102, 103, 105, 106, 107, 108, 109] {
            assert!(identities.remove(&identity).is_some());
        }
        assert!(identities.is_empty());
    }

    #[test]
    fn an_identity_between_those_of_a_run_splits_it() {
        let mut identities = NativeRuleIdentities::default();
        for index in 0..5 {
            identities.insert(100 + index * 2, RuleID(index as u32));
        }
        identities.insert(103, RuleID(50));
        assert_eq!(identities.get(&103), Some(RuleID(50)));
        for index in 0..5 {
            assert_eq!(identities.get(&(100 + index * 2)), Some(RuleID(index as u32)));
        }
        assert_eq!(identities.get(&101), None);
        assert_eq!(identities.get(&105), None);
    }

    #[test]
    fn sparse_identities_and_the_largest_one_are_found() {
        let mut identities = NativeRuleIdentities::default();
        for (identity, rule) in [(1, 0), (127, 1), (128, 2), (u64::MAX, 3)] {
            identities.insert(identity, RuleID(rule));
        }
        assert_eq!(identities.get(&u64::MAX), Some(RuleID(3)));
        assert_eq!(identities.get(&2), None);
        assert_eq!(identities.remove(&2), None);
        for identity in [1, 127, 128, u64::MAX] {
            assert!(identities.remove(&identity).is_some());
        }
        assert!(identities.is_empty());
    }
}
