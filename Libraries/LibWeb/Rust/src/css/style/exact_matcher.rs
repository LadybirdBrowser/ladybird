/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The cache-free selector correctness floor.
//!
//! This matcher enumerates attached rules directly from the rule store and matches their selectors
//! against committed facts and tree relations. It deliberately owns no
//! dispatch index, Bloom summary, prefix state, relation or position cache, retained witness,
//! retained answer, or cascade-pruning state.

use super::RetainedState;
use super::pipeline::projection::Projection;
use super::pipeline::rule_store::CascadeOrigin;
use super::pipeline::rule_store::RuleID;
use super::pipeline::rule_store::RuleStore;
use super::selector::Incomplete;
use super::selector::Specificity;
use super::tree::PseudoElementTarget;
use super::tree::StyleNodeID;
use super::tree::TreeScopeID;
use smallvec::SmallVec;

/// One selector-list answer produced without derived matching state.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ExactRuleMatch {
    pub node: StyleNodeID,
    pub pseudo_element: Option<PseudoElementTarget>,
    pub rule: RuleID,
    pub entry: u32,
    pub specificity: Specificity,
    pub tree_scope: TreeScopeID,
    pub scope_proximity: u32,
}

/// How a scope is allowed to reach the node being asked.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ExactMatchContext {
    #[default]
    Ordinary,
    Slotted,
    Part,
    Host,
}

/// Direct evaluator over attached rules and committed base relations only.
pub struct ExactMatcher<'a> {
    projection: &'a Projection,
    program: &'a RuleStore,
    scope: TreeScopeID,
    shadow_root: Option<StyleNodeID>,
    context: ExactMatchContext,
    document: super::component_matching::DocumentMatching,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum DocumentSheets {
    All,
    NonAuthorOnly,
}

impl<'a> ExactMatcher<'a> {
    #[must_use]
    pub(super) fn new(
        projection: &'a Projection,
        program: &'a RuleStore,
        document: super::component_matching::DocumentMatching,
    ) -> Self {
        Self {
            projection,
            program,
            scope: TreeScopeID::DOCUMENT,
            shadow_root: None,
            context: ExactMatchContext::Ordinary,
            document,
        }
    }

    #[must_use]
    pub fn in_scope(mut self, scope: TreeScopeID) -> Self {
        self.scope = scope;
        self
    }

    #[must_use]
    pub fn in_shadow_tree(mut self, shadow_root: StyleNodeID) -> Self {
        self.shadow_root = Some(shadow_root);
        self
    }

    #[must_use]
    pub fn with_context(mut self, context: ExactMatchContext) -> Self {
        self.context = context;
        self
    }

    /// Evaluate one element by walking every attached rule entry directly.
    pub fn match_node(&self, node: StyleNodeID, out: &mut Vec<ExactRuleMatch>) -> Result<(), Incomplete> {
        if self.projection.facts(node).is_none() {
            return Err(Incomplete::MissingFacts(node));
        }
        let start = out.len();
        let result = (|| {
            self.match_sheets(node, self.scope, DocumentSheets::All, start, out)?;
            if self.scope != TreeScopeID::DOCUMENT {
                let document_sheets = match self.program.scope_uses_document_sheets(self.scope) {
                    true => DocumentSheets::All,
                    false => DocumentSheets::NonAuthorOnly,
                };
                self.match_sheets(node, TreeScopeID::DOCUMENT, document_sheets, start, out)?;
            }
            Ok(())
        })();
        if let Err(incomplete) = result {
            out.truncate(start);
            return Err(incomplete);
        }
        out[start..].sort_unstable_by_key(|matched| (matched.rule, matched.pseudo_element));
        Ok(())
    }

    fn match_sheets(
        &self,
        node: StyleNodeID,
        sheet_scope: TreeScopeID,
        take: DocumentSheets,
        match_start: usize,
        out: &mut Vec<ExactRuleMatch>,
    ) -> Result<(), Incomplete> {
        for &sheet in self.program.sheets_in_scope(sheet_scope) {
            if take == DocumentSheets::NonAuthorOnly
                && !matches!(
                    self.program.sheet_origin(sheet),
                    CascadeOrigin::UserAgent | CascadeOrigin::User
                )
            {
                continue;
            }
            for rule in self.program.rules_in_sheet(sheet) {
                if !self.program.rule_can_decide(rule) {
                    continue;
                }
                let version = self.program.rule_version(rule);
                if !version.kind.matches_elements() {
                    continue;
                }
                let Some(selectors) = self.program.selectors_of(sheet, version) else {
                    continue;
                };
                let scope = self.program.scope_chain(version.scope).map(|chain| &**chain);
                let mut evaluator = self.projection.evaluator();
                evaluator.set_scope_shadow_root(self.shadow_root);
                if self.context == ExactMatchContext::Part {
                    evaluator.set_part_exposure_scope(Some(self.scope));
                }
                for (index, (selector, specificity, pseudo_kind)) in super::pipeline::entries_of(&selectors).enumerate()
                {
                    let entry_selector = super::component_matching::EntrySelector::new(selector, scope);
                    let facts = super::selector_analysis::EntryFacts::of(entry_selector);
                    let context_admits = match self.context {
                        ExactMatchContext::Ordinary => true,
                        ExactMatchContext::Slotted => facts.slotted,
                        ExactMatchContext::Part => facts.part,
                        ExactMatchContext::Host => facts.host,
                    };
                    if !context_admits {
                        continue;
                    }
                    let matched = evaluator.matches_entry_selector(entry_selector, node, self.document)?;
                    let pseudo_element = (pseudo_kind != u8::MAX)
                        .then(|| PseudoElementTarget::new(super::tree::PseudoElementKind(u16::from(pseudo_kind))));
                    if !matched {
                        continue;
                    }
                    let scope_proximity = evaluator.scope_proximity_of_match(node);
                    let candidate = ExactRuleMatch {
                        node,
                        pseudo_element,
                        rule,
                        entry: u32::try_from(index).expect("selector entry space exhausted"),
                        specificity,
                        tree_scope: self.scope,
                        scope_proximity,
                    };
                    if let Some(existing) = out[match_start..].iter_mut().find(|existing| {
                        existing.node == node
                            && existing.rule == rule
                            && existing.tree_scope == self.scope
                            && existing.pseudo_element == pseudo_element
                    }) {
                        if candidate.specificity > existing.specificity {
                            *existing = candidate;
                        }
                    } else {
                        out.push(candidate);
                    }
                }
            }
        }
        Ok(())
    }
}

impl RetainedState {
    pub(super) fn append_exact_matches_in_scope(
        &self,
        node: StyleNodeID,
        scope: TreeScopeID,
        context: ExactMatchContext,
        matches: &mut Vec<ExactRuleMatch>,
    ) -> Result<(), Incomplete> {
        let shadow_root = self.scope_root(scope);
        let mut matcher = ExactMatcher::new(&self.projection, &self.rules, self.document_matching())
            .in_scope(scope)
            .with_context(context);
        if let Some(shadow_root) = shadow_root {
            matcher = matcher.in_shadow_tree(shadow_root);
        }
        matcher.match_node(node, matches)
    }

    /// Append what `node` matches from every tree whose rules can decide for it, evaluating every
    /// attached rule directly, and answer whether a rule can match it from more than one tree.
    pub(super) fn append_exact_matches_in_every_scope(
        &self,
        node: StyleNodeID,
        exact: &mut Vec<ExactRuleMatch>,
    ) -> Result<bool, Incomplete> {
        let scope = self.projection.tree_scope(node);
        let inner_scope = self
            .projection
            .shadow_root_of(node)
            .and_then(|shadow_root| self.scope_by_root.get(shadow_root).filter(|&inner| inner != scope));
        let slotted_scopes: SmallVec<[TreeScopeID; 1]> = self
            .scopes_slotted_into(node)
            .filter(|&slotted| slotted != scope && Some(slotted) != inner_scope)
            .collect();
        let part_scopes: SmallVec<[TreeScopeID; 1]> = self
            .part_exposure_scopes(node)
            .filter(|&exposed| exposed != scope && Some(exposed) != inner_scope && !slotted_scopes.contains(&exposed))
            .collect();

        for_each_matching_scope(scope, inner_scope, &slotted_scopes, &part_scopes, |scope, context| {
            self.append_exact_matches_in_scope(node, scope, context, exact)
        })?;
        Ok(inner_scope.is_some() || !slotted_scopes.is_empty() || !part_scopes.is_empty())
    }
}

fn for_each_matching_scope(
    scope: TreeScopeID,
    inner_scope: Option<TreeScopeID>,
    slotted_scopes: &[TreeScopeID],
    part_scopes: &[TreeScopeID],
    mut ask: impl FnMut(TreeScopeID, ExactMatchContext) -> Result<(), Incomplete>,
) -> Result<(), Incomplete> {
    ask(scope, ExactMatchContext::Ordinary)?;
    if let Some(inner_scope) = inner_scope {
        ask(inner_scope, ExactMatchContext::Host)?;
    }
    for &slotted_scope in slotted_scopes {
        ask(slotted_scope, ExactMatchContext::Slotted)?;
    }
    for &part_scope in part_scopes {
        ask(part_scope, ExactMatchContext::Part)?;
    }
    Ok(())
}
