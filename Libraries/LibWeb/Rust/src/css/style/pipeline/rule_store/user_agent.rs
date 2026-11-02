/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The rules of the user agent's sheets, which the process keeps once and every document reads.
//!
//! A document records no user agent rule. Its user agent sheets stand for sheets of the process's
//! [`UserAgentRules`], whose rules it names by [`RuleID::of_user_agent`], and it keeps only which
//! of them its media leave out. Those sheets and left-out rules are the document's
//! [`UserAgentVariant`], the key of the user agent index it decides with.

use super::{RuleID, RuleStore, SheetID};
use crate::css::style::atoms::DocumentAtoms;
use crate::css::style::selector_analysis::SelectorSummary;
use crate::fork::ForkShared;
use smallvec::SmallVec;
use std::sync::Arc;

/// The rules of the user agent's sheets the process published so far. Nothing edits a published
/// rule, and a sheet arriving makes a new table holding the old one's rules at their numbers, so
/// that a document holding an older table reads what it always read.
#[derive(Clone)]
pub(in crate::css::style) struct UserAgentRules {
    store: RuleStore,
    sheets: SmallVec<[UserAgentSheet; 4]>,
}

impl Default for UserAgentRules {
    fn default() -> Self {
        Self {
            store: RuleStore::new(),
            sheets: SmallVec::new(),
        }
    }
}

/// A user agent sheet the process published.
#[derive(Clone)]
struct UserAgentSheet {
    /// The identity of the native sheet the rules were compiled from.
    source_identity: u64,
    sheet: SheetID,
    /// What a document learns of the selectors of the sheet's rules as it attaches the sheet.
    selectors: Arc<SelectorSummary>,
    /// The references that keep the names the sheet's rules declare interned.
    _atoms: ForkShared<DocumentAtoms>,
}

impl UserAgentRules {
    /// The rules, numbered as [`RuleID::in_user_agent_rules`] answers.
    pub(in crate::css::style) fn store(&self) -> &RuleStore {
        &self.store
    }

    /// The table holding what this one holds and `store`, the rule store that went on to hold the
    /// rules of the sheet `sheet`, published from the native sheet `source_identity`.
    pub(in crate::css::style) fn with_sheet(
        &self,
        store: RuleStore,
        sheet: SheetID,
        source_identity: u64,
        selectors: SelectorSummary,
        atoms: ForkShared<DocumentAtoms>,
    ) -> Self {
        let mut sheets = self.sheets.clone();
        sheets.push(UserAgentSheet {
            source_identity,
            sheet,
            selectors: Arc::new(selectors),
            _atoms: atoms,
        });
        Self { store, sheets }
    }

    /// The sheet published from the native sheet `source_identity`.
    pub(in crate::css::style) fn sheet_of_source(&self, source_identity: u64) -> Option<SheetID> {
        self.sheets
            .iter()
            .find(|sheet| sheet.source_identity == source_identity)
            .map(|sheet| sheet.sheet)
    }

    /// What the selectors of the rules of `sheet` say to a document attaching it.
    pub(in crate::css::style) fn selector_summary(&self, sheet: SheetID) -> Arc<SelectorSummary> {
        let sheet = self.sheets.iter().find(|candidate| candidate.sheet == sheet);
        Arc::clone(&sheet.expect("the process published the sheet").selectors)
    }

    /// The bytes of the table, the first time `sizer` reaches each thing it holds.
    pub(in crate::css::style) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        self.store.heap_size(sizer) + self.sheets.len() * size_of::<UserAgentSheet>()
    }

    /// The bytes of the declarations of the rules, the first time `sizer` reaches each.
    pub(in crate::css::style) fn declarations_heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        self.store.declarations_heap_size(sizer)
    }
}

/// Which of the process's user agent rules decide in a document: every rule of its contributing
/// user agent sheets, in cascade order, but those its media leave out. Documents alike in this
/// decide with one user agent index.
#[derive(Clone, PartialEq, Eq)]
pub(in crate::css::style) struct UserAgentVariant {
    /// The contributing sheets, in cascade order, in the numbers of the process's rules.
    sheets: SmallVec<[SheetID; 4]>,
    /// The rules left out, in the numbers of the process's rules, in increasing order.
    left_out: SmallVec<[RuleID; 8]>,
}

impl UserAgentVariant {
    /// The rules of the sheet published from the native sheet `source_identity` alone, but those
    /// compiled from the native rules `failing`, whose conditions do not hold.
    pub(in crate::css::style) fn of_published_sheet(
        rules: &UserAgentRules,
        source_identity: u64,
        failing: &[u64],
    ) -> Option<Self> {
        let sheet = rules.sheet_of_source(source_identity)?;
        let mut left_out: SmallVec<[RuleID; 8]> = failing
            .iter()
            .filter_map(|&identity| rules.store.native.identities.get(&identity))
            .collect();
        left_out.sort_unstable();
        let mut sheets = SmallVec::new();
        sheets.push(sheet);
        Some(Self { sheets, left_out })
    }

    pub(in crate::css::style) fn sheets(&self) -> &[SheetID] {
        &self.sheets
    }

    /// Whether the media of the documents of the variant leave `rule` out.
    pub(in crate::css::style) fn leaves_out(&self, rule: RuleID) -> bool {
        self.left_out.binary_search(&rule).is_ok()
    }

    pub(in crate::css::style) fn heap_size(&self) -> usize {
        crate::heap_size::small_vec_bytes(&self.sheets) + crate::heap_size::small_vec_bytes(&self.left_out)
    }
}

/// What a document holds of the process's user agent rules.
#[derive(Clone)]
pub(super) struct UserAgentBinding {
    rules: Arc<UserAgentRules>,
    /// The document's sheet each published sheet it attached is, by the published sheet's number.
    sheets: SmallVec<[(SheetID, SheetID); 4]>,
    /// The rules whose conditions do not hold in the document, in increasing order.
    left_out: SmallVec<[RuleID; 8]>,
}

impl UserAgentBinding {
    #[inline]
    pub(super) fn store(&self) -> &RuleStore {
        &self.rules.store
    }

    /// The document's sheet the rule `rule` of the process is in.
    pub(super) fn sheet_of(&self, rule: RuleID) -> SheetID {
        let published = self.store().rule_sheet(rule);
        let found = self.sheets.iter().find(|(sheet, _)| *sheet == published);
        found.expect("a document names the rules of the sheets it holds").1
    }

    /// Whether the conditions of `rule`, as the document names it, hold in the document.
    pub(super) fn rule_conditions_hold(&self, rule: RuleID) -> bool {
        let rule = rule.in_user_agent_rules().expect("a user agent rule");
        self.left_out.binary_search(&rule).is_err()
    }

    /// Note whether the conditions of `rule` hold in the document, answering whether that moved.
    pub(super) fn set_rule_conditions_hold(&mut self, rule: RuleID, conditions_hold: bool) -> bool {
        let rule = rule.in_user_agent_rules().expect("a user agent rule");
        match (self.left_out.binary_search(&rule), conditions_hold) {
            (Ok(place), true) => {
                self.left_out.remove(place);
                true
            }
            (Err(place), false) => {
                self.left_out.insert(place, rule);
                true
            }
            _ => false,
        }
    }

    pub(super) fn heap_size(&self) -> usize {
        crate::heap_size::small_vec_bytes(&self.sheets) + crate::heap_size::small_vec_bytes(&self.left_out)
    }
}

impl RuleStore {
    #[inline]
    pub(super) fn user_agent_binding(&self) -> &UserAgentBinding {
        self.user_agent
            .as_ref()
            .expect("a document names the rules of the sheets it holds")
    }

    pub(super) fn user_agent_binding_mut(&mut self) -> &mut UserAgentBinding {
        self.user_agent
            .as_mut()
            .expect("a document names the rules of the sheets it holds")
    }

    /// Have `sheet`, a sheet of the document that holds no rules, stand for `published`, a sheet of
    /// `rules`, the newest table of the process's user agent rules.
    pub(in crate::css::style) fn bind_user_agent_sheet(
        &mut self,
        sheet: SheetID,
        rules: Arc<UserAgentRules>,
        published: SheetID,
    ) {
        self.sheets[sheet.0 as usize].user_agent = Some(published);
        match &mut self.user_agent {
            Some(binding) => {
                binding.rules = rules;
                binding.sheets.push((published, sheet));
            }
            None => {
                let mut sheets = SmallVec::new();
                sheets.push((published, sheet));
                self.user_agent = Some(UserAgentBinding {
                    rules,
                    sheets,
                    left_out: SmallVec::new(),
                });
            }
        }
    }

    /// The process's user agent rules the document holds.
    pub(in crate::css::style) fn user_agent_rules(&self) -> Option<&Arc<UserAgentRules>> {
        self.user_agent.as_ref().map(|binding| &binding.rules)
    }

    /// Which of the process's user agent rules decide in the document, if any can.
    pub(in crate::css::style) fn user_agent_variant(&self) -> Option<UserAgentVariant> {
        let binding = self.user_agent.as_ref()?;
        let sheets: SmallVec<[SheetID; 4]> = self
            .sheets_in_scope(super::TreeScopeID::DOCUMENT)
            .iter()
            .filter(|&&sheet| self.sheet_contributes(sheet))
            .filter_map(|&sheet| self.sheets[sheet.0 as usize].user_agent)
            .collect();
        (!sheets.is_empty()).then(|| UserAgentVariant {
            sheets,
            left_out: binding.left_out.clone(),
        })
    }

    /// The document's name of the user agent rule compiled from the native rule `identity`.
    pub(in crate::css::style) fn user_agent_rule_of_identity(&self, identity: u64) -> Option<RuleID> {
        let binding = self.user_agent.as_ref()?;
        let rule = binding.store().native.identities.get(&identity)?;
        // NB: The process's table holds sheets the document may not have attached.
        let published = binding.store().rule_sheet(rule);
        binding
            .sheets
            .iter()
            .any(|(sheet, _)| *sheet == published)
            .then(|| RuleID::of_user_agent(rule))
    }

    /// The native rule `rule` was compiled from, as the document names it.
    pub(in crate::css::style) fn native_target(&self, rule: RuleID) -> Option<super::native::NativeRuleTargetRef<'_>> {
        match rule.in_user_agent_rules() {
            Some(rule) => self.user_agent.as_ref()?.store().native.targets.get(&rule),
            None => self.native.targets.get(&rule),
        }
    }
}
