/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The cache-free selector correctness floor.
//!
//! This matcher enumerates attached rules directly from the stylesheet program and evaluates their
//! match selector IR against committed facts and tree relations. It deliberately owns no
//! dispatch index, Bloom summary, prefix state, relation or position cache, retained witness,
//! retained answer, or cascade-pruning state.

use super::index::StyleNodeFacts;
use super::instrumentation::Counters;
use super::program::CascadeOrigin;
use super::program::RuleID;
use super::program::SelectorProgramID;
use super::program::StyleSheetProgram;
use super::selector::Incomplete;
use super::selector::MatchEvaluator;
use super::selector::SelectorPrograms;
use super::selector::Specificity;
use super::tree::PseudoElementTarget;
use super::tree::StyleNodeID;
use super::tree::StyleNodeTree;
use super::tree::TreeScopeID;

/// One selector-list answer produced without derived matching state.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ExactRuleMatch {
    pub node: StyleNodeID,
    pub pseudo_element: Option<PseudoElementTarget>,
    pub rule: RuleID,
    pub program: SelectorProgramID,
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
    tree: &'a StyleNodeTree,
    facts: &'a StyleNodeFacts,
    programs: &'a SelectorPrograms,
    program: &'a StyleSheetProgram,
    scope: TreeScopeID,
    shadow_root: Option<StyleNodeID>,
    context: ExactMatchContext,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum DocumentSheets {
    All,
    NonAuthorOnly,
}

impl<'a> ExactMatcher<'a> {
    #[must_use]
    pub fn new(
        tree: &'a StyleNodeTree,
        facts: &'a StyleNodeFacts,
        programs: &'a SelectorPrograms,
        program: &'a StyleSheetProgram,
    ) -> Self {
        Self {
            tree,
            facts,
            programs,
            program,
            scope: TreeScopeID::DOCUMENT,
            shadow_root: None,
            context: ExactMatchContext::Ordinary,
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
    pub fn match_node(
        &self,
        node: StyleNodeID,
        out: &mut Vec<ExactRuleMatch>,
        counters: &mut Counters,
    ) -> Result<(), Incomplete> {
        if self.facts.row_of(node).is_none() {
            return Err(Incomplete::MissingFacts(node));
        }
        let start = out.len();
        let result = (|| {
            self.match_sheets(node, self.scope, DocumentSheets::All, start, out, counters)?;
            if self.scope != TreeScopeID::DOCUMENT {
                let document_sheets = match self.program.scope_uses_document_sheets(self.scope) {
                    true => DocumentSheets::All,
                    false => DocumentSheets::NonAuthorOnly,
                };
                self.match_sheets(node, TreeScopeID::DOCUMENT, document_sheets, start, out, counters)?;
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

    /// Evaluate every element under `root` in preorder.
    #[cfg(test)]
    pub fn match_subtree(
        &self,
        root: StyleNodeID,
        out: &mut Vec<ExactRuleMatch>,
        counters: &mut Counters,
    ) -> Result<(), Incomplete> {
        let start = out.len();
        for node in self.tree.preorder(root) {
            if let Err(incomplete) = self.match_node(node, out, counters) {
                out.truncate(start);
                return Err(incomplete);
            }
        }
        out[start..].sort_unstable_by_key(|matched| (matched.node, matched.rule, matched.pseudo_element));
        Ok(())
    }

    fn match_sheets(
        &self,
        node: StyleNodeID,
        sheet_scope: TreeScopeID,
        take: DocumentSheets,
        match_start: usize,
        out: &mut Vec<ExactRuleMatch>,
        counters: &mut Counters,
    ) -> Result<(), Incomplete> {
        for sheet in self.program.sheets_in_scope(sheet_scope) {
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
                let Some(program_id) = version.selector_program else {
                    continue;
                };
                let compiled = self.programs.get(program_id);
                let mut evaluator = MatchEvaluator::new(self.tree, self.facts);
                if let Some(shadow_root) = self.shadow_root {
                    evaluator = evaluator.in_shadow_tree(shadow_root);
                }
                if self.context == ExactMatchContext::Part {
                    evaluator = evaluator.for_a_part_exposed_in(self.scope);
                }
                for (index, entry) in compiled.entries().iter().enumerate() {
                    let context_admits = match self.context {
                        ExactMatchContext::Ordinary => true,
                        ExactMatchContext::Slotted => compiled.subject_is_slotted(entry.root),
                        ExactMatchContext::Part => compiled.subject_is_a_part(entry.root),
                        ExactMatchContext::Host => compiled.subject_is_the_host(entry.root),
                    };
                    if !context_admits || !evaluator.matches_entry(compiled, entry, node, counters)? {
                        continue;
                    }
                    let scope_proximity = evaluator.scope_proximity_of(compiled, entry, node, counters)?;
                    let candidate = ExactRuleMatch {
                        node,
                        pseudo_element: entry.pseudo_element,
                        rule,
                        program: program_id,
                        entry: u32::try_from(index).expect("selector entry space exhausted"),
                        specificity: entry.specificity,
                        tree_scope: self.scope,
                        scope_proximity,
                    };
                    if let Some(existing) = out[match_start..].iter_mut().find(|existing| {
                        existing.node == node
                            && existing.rule == rule
                            && existing.tree_scope == self.scope
                            && existing.pseudo_element == entry.pseudo_element
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

#[cfg(test)]
mod tests {
    use super::super::index::StateSet;
    use super::super::index::StyleAtomID;
    use super::super::memory::DeviceClass;
    use super::super::memory::MemoryController;
    use super::super::program::RuleKind;
    use super::super::program::StyleSheetObjectID;
    use super::super::selector::FeatureTest;
    use super::super::selector::SelectorOp;
    use super::super::selector::SelectorProgramBuilder;
    use super::*;

    #[test]
    fn exact_matching_enumerates_rules_without_a_dispatch() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut tree = StyleNodeTree::new(&mut memory);
        let root = tree.allocate_element(&mut memory);
        let child = tree.allocate_element(&mut memory);
        tree.set_first_element_child(root, Some(child));
        tree.set_parent(child, Some(root));

        let theme = StyleAtomID(1);
        let item = StyleAtomID(2);
        let mut facts = StyleNodeFacts::new();
        facts.push_row(
            root,
            StyleAtomID(10),
            StyleAtomID::NONE,
            StateSet::default(),
            &[theme],
            &[],
        );
        facts.push_row(
            child,
            StyleAtomID(11),
            StyleAtomID::NONE,
            StateSet::default(),
            &[item],
            &[],
        );

        let mut programs = SelectorPrograms::new();
        let mut builder = SelectorProgramBuilder::new();
        let ancestor = builder.push_feature(FeatureTest::Class(theme));
        let ancestor = builder.push(SelectorOp::Ancestor(ancestor));
        let subject = builder.push_feature(FeatureTest::Class(item));
        let selector = builder.push_compound(&[subject, ancestor]);
        builder.push_entry(selector);
        let selector = programs.add(builder.finish());

        let mut program = StyleSheetProgram::new();
        let sheet = program.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        program.attach_sheet(sheet, TreeScopeID::DOCUMENT);
        let rule = program.append_rule(sheet, None, RuleKind::Style);
        let mut version = program.rule_version(rule);
        version.selector_program = Some(selector);
        program.replace_rule_version(rule, version);

        let mut matches = Vec::new();
        let mut counters = Counters::new();
        ExactMatcher::new(&tree, &facts, &programs, &program)
            .match_subtree(root, &mut matches, &mut counters)
            .unwrap();

        assert_eq!(matches.len(), 1);
        assert_eq!(matches[0].node, child);
        assert_eq!(matches[0].rule, rule);
    }

    #[test]
    fn exact_matching_keeps_the_greatest_selector_list_specificity() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut tree = StyleNodeTree::new(&mut memory);
        let node = tree.allocate_element(&mut memory);
        let class = StyleAtomID(1);
        let id = StyleAtomID(2);
        let mut facts = StyleNodeFacts::new();
        facts.push_row(node, StyleAtomID(10), id, StateSet::default(), &[class], &[]);

        let mut programs = SelectorPrograms::new();
        let mut builder = SelectorProgramBuilder::new();
        let by_class = builder.push_feature(FeatureTest::Class(class));
        let by_class = builder.push_entry(by_class);
        builder.set_entry_specificity(
            by_class,
            Specificity {
                classes: 1,
                ..Specificity::default()
            },
        );
        let by_id = builder.push_feature(FeatureTest::Id(id));
        let by_id = builder.push_entry(by_id);
        builder.set_entry_specificity(
            by_id,
            Specificity {
                ids: 1,
                ..Specificity::default()
            },
        );
        let selector = programs.add(builder.finish());

        let mut program = StyleSheetProgram::new();
        let sheet = program.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        program.attach_sheet(sheet, TreeScopeID::DOCUMENT);
        let rule = program.append_rule(sheet, None, RuleKind::Style);
        let mut version = program.rule_version(rule);
        version.selector_program = Some(selector);
        program.replace_rule_version(rule, version);

        let mut matches = Vec::new();
        ExactMatcher::new(&tree, &facts, &programs, &program)
            .match_node(node, &mut matches, &mut Counters::new())
            .unwrap();

        assert_eq!(matches.len(), 1);
        assert_eq!(matches[0].entry, 1);
    }
}
