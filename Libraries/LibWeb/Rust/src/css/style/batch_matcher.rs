/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The optimized exact batch matcher.
//!
//! This physical plan streams a scope through rule dispatch, ancestor summaries, prefix states,
//! relation and position workspaces, retained witnesses, and optional cascade-winner pruning. Its
//! output is exact, but its accelerators make it unsuitable as the correctness reference. That role
//! belongs to [`super::exact_matcher::ExactMatcher`].

use std::mem::size_of;

use super::ScopeDispatchShape;
use super::capacity::capacity_bytes;
use super::cascade::Top1Cascade;
use super::index::AncestorDispatchFacts;
use super::index::AncestorDispatchTopologyID;
use super::index::CandidateEntries;
use super::index::DispatchCandidateWorkspace;
use super::index::DispatchEntry;
use super::index::DispatchKey;
use super::index::ParentDispatchFacts;
use super::index::RuleDispatch;
use super::index::StyleNodeFacts;
use super::instrumentation::Counter;
use super::instrumentation::Counters;
use super::memory::MemoryCategory;
use super::memory::MemoryController;
use super::prefix::PrefixEvaluation;
use super::prefix::PrefixMatchSetID;
use super::prefix::PrefixStates;
use super::prefix::PrefixTransitionGap;
use super::prefix::PrefixTransitionLookup;
use super::program::CascadeOrigin;
use super::program::EntryID;
use super::program::RuleID;
use super::program::SelectorProgramID;
use super::program::StyleSheetProgram;
use super::relative_selector::RelationalWitnesses;
use super::selector::Incomplete;
use super::selector::MatchEvaluationSide;
use super::selector::MatchEvaluationWorkspace;
use super::selector::MatchEvaluator;
use super::selector::SelectorEntry;
use super::selector::SelectorProgram;
use super::selector::SelectorPrograms;
use super::selector::Specificity;
use super::tree::PseudoElementTarget;
use super::tree::StyleNodeID;
use super::tree::StyleNodeTree;
use super::tree::TreeScopeID;

/// One rule matching one style node, with the metadata later semantics need.
///
/// The metadata is deliberately small: a selector-list entry and its specificity. Anything static
/// for the selector stays in the program rather than being copied per match.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct RuleMatch {
    /// The originating element. A pseudo-element match carries its target separately.
    pub node: StyleNodeID,
    pub pseudo_element: Option<PseudoElementTarget>,
    pub rule: RuleID,
    pub program: SelectorProgramID,
    pub entry: u32,
    /// The entry's normal-declaration cascade rank in its tree scope's dispatch. This is complete
    /// for an unscoped match; a scoped match also needs its dynamically resolved proximity.
    pub cascade_order: u32,
    pub specificity: Specificity,
    /// The tree scope the rule decided from, which is where its sheet is attached and not
    /// necessarily where the element is. `:host`, `::slotted()` and `::part()` all decide for an
    /// element outside their own tree, and how deeply encapsulated the rule is is what orders it
    /// against the contexts around it - so the match carries the scope rather than the cascade
    /// deriving it from the element.
    pub tree_scope: TreeScopeID,
    /// Generational hops from the scoping root the match resolved through, or `u32::MAX` for a rule
    /// that is not scoped. A nearer root wins, which is why an unscoped rule is infinitely far.
    pub scope_proximity: u32,
}

/// Concrete matches for one evaluation, sorted by style node.
///
/// This is transaction-local Tier-4 scratch: the cascade consumes it and it is discarded. It is not
/// an inverse match relation and is never retained.
#[derive(Default)]
pub struct RuleMatches {
    matches: Vec<RuleMatch>,
    selector_truth: Option<Vec<super::SelectorTruth>>,
    charged_bytes: u64,
}

impl RuleMatches {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    #[must_use]
    pub fn as_slice(&self) -> &[RuleMatch] {
        &self.matches
    }

    #[must_use]
    pub fn as_mut_vec(&mut self) -> &mut Vec<RuleMatch> {
        &mut self.matches
    }

    #[must_use]
    pub fn len(&self) -> usize {
        self.matches.len()
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.matches.is_empty()
    }

    /// Matches for one style node, which is contiguous because the vector is sorted by node.
    #[must_use]
    pub fn matches_for(&self, node: StyleNodeID) -> &[RuleMatch] {
        let start = self.matches.partition_point(|entry| entry.node < node);
        let end = self.matches[start..].partition_point(|entry| entry.node == node) + start;
        &self.matches[start..end]
    }

    pub fn clear(&mut self) {
        self.matches.clear();
        if let Some(truth) = self.selector_truth.as_mut() {
            truth.clear();
        }
    }

    pub(super) fn enable_selector_truth(&mut self) {
        self.selector_truth.get_or_insert_default();
    }

    fn selector_truth_len(&self) -> usize {
        self.selector_truth.as_ref().map_or(0, Vec::len)
    }

    fn record_selector_truth(&mut self, entry: EntryID, tree_scope: TreeScopeID, scope_proximity: u32) {
        if let Some(truth) = self.selector_truth.as_mut() {
            truth.push(super::SelectorTruth {
                entry,
                tree_scope,
                scope_proximity,
            });
        }
    }

    fn truncate(&mut self, matches: usize, selector_truth: usize) {
        self.matches.truncate(matches);
        if let Some(truth) = self.selector_truth.as_mut() {
            truth.truncate(selector_truth);
        }
    }

    pub(super) fn prepared_selector_truth(&self) -> Option<Vec<super::SelectorTruth>> {
        let mut truth = self.selector_truth.clone()?;
        truth.sort_unstable();
        truth.dedup();
        Some(truth)
    }

    /// Reconcile the scratch charge after a pass.
    pub fn settle_memory(&mut self, memory: &mut MemoryController) {
        let current = (self.matches.capacity() * size_of::<RuleMatch>()
            + self
                .selector_truth
                .as_ref()
                .map_or(0, |truth| truth.capacity() * size_of::<super::SelectorTruth>())) as u64;
        if current > self.charged_bytes {
            memory.reserve_required(MemoryCategory::BatchScratch, current - self.charged_bytes);
        } else if self.charged_bytes > current {
            memory.release(MemoryCategory::BatchScratch, self.charged_bytes - current);
        }
        self.charged_bytes = current;
    }

    #[must_use]
    pub fn take(&mut self, memory: &mut MemoryController) -> Vec<RuleMatch> {
        self.settle_memory(memory);
        let matches = std::mem::take(&mut self.matches);
        self.settle_memory(memory);
        matches
    }

    pub fn release(&mut self, memory: &mut MemoryController) {
        memory.release(MemoryCategory::BatchScratch, self.charged_bytes);
        self.charged_bytes = 0;
        self.matches = Vec::new();
        self.selector_truth = None;
    }
}

/// Builds the dispatch index for one style scope from the attached program.
///
/// Only live style rules with a compiled selector participate. A rule of any other kind reaches its
/// consumers through its own typed delta - a keyframes edit starts at animations referencing its
/// name, not at every element in the scope - so bucketing it here would be routing it to the wrong
/// place.
#[must_use]
pub fn build_scope_dispatch(
    program: &StyleSheetProgram,
    programs: &SelectorPrograms,
    tree_scope: TreeScopeID,
) -> RuleDispatch {
    let mut dispatch = RuleDispatch::new();
    // The user-agent and user origins have no scope of their own: they decide for every element in
    // the document, whichever tree it is in. Their sheets are attached to the document scope, so a
    // shadow scope has to take them from there.
    if tree_scope != TreeScopeID::DOCUMENT {
        // A shadow root built from the document's styles rather than its own takes the author
        // origin from there too.
        let take = match program.scope_uses_document_sheets(tree_scope) {
            true => SheetsToTake::All,
            false => SheetsToTake::NonAuthorOnly,
        };
        insert_scope_sheets(&mut dispatch, program, programs, TreeScopeID::DOCUMENT, take);
    }
    // Dispatch identity is independent of cascade order, which is assigned after construction.
    // Put the document contribution first so shadow roots share a prefix even when their local
    // sheets differ, letting a later scope extend the already-built common topology.
    insert_scope_sheets(&mut dispatch, program, programs, tree_scope, SheetsToTake::All);
    dispatch.finish_prefixes();
    dispatch
}

/// Describe the selector topology of one effective scope and align each concrete rule with the
/// dense dispatch entries that topology will produce.
pub(super) fn scope_dispatch_shape_and_rules(
    program: &StyleSheetProgram,
    programs: &SelectorPrograms,
    tree_scope: TreeScopeID,
) -> (ScopeDispatchShape, Vec<RuleID>) {
    let mut shape = Vec::new();
    let mut rules = Vec::new();
    let mut collect = |tree_scope, take| {
        for_each_scope_rule(program, tree_scope, take, |sheet, rule, selector_program| {
            shape.push((selector_program, program.sheet_origin(sheet) == CascadeOrigin::Author));
            let compiled = programs.get(selector_program);
            for index in 0..compiled.entries().len() {
                let metadata = compiled.dispatch_metadata(index);
                let copies = if metadata.key == DispatchKey::Universal {
                    let mut branch_keys = metadata.subject_dispatch_keys().to_vec();
                    branch_keys.sort_unstable();
                    branch_keys.dedup();
                    branch_keys.len().max(1)
                } else {
                    1
                };
                rules.extend(std::iter::repeat_n(rule, copies));
            }
        });
    };
    if tree_scope != TreeScopeID::DOCUMENT {
        let take = match program.scope_uses_document_sheets(tree_scope) {
            true => SheetsToTake::All,
            false => SheetsToTake::NonAuthorOnly,
        };
        collect(TreeScopeID::DOCUMENT, take);
    }
    collect(tree_scope, SheetsToTake::All);
    (ScopeDispatchShape(shape), rules)
}

/// Which of a scope's sheets a dispatch build takes.
#[derive(Clone, Copy, PartialEq, Eq)]
enum SheetsToTake {
    All,
    /// Only the origins that decide outside the scope they are attached to.
    NonAuthorOnly,
}

fn insert_scope_sheets(
    dispatch: &mut RuleDispatch,
    program: &StyleSheetProgram,
    programs: &SelectorPrograms,
    tree_scope: TreeScopeID,
    take: SheetsToTake,
) {
    for_each_scope_rule(program, tree_scope, take, |sheet, rule, selector_program| {
        insert_scope_rule(
            dispatch,
            programs,
            rule,
            selector_program,
            program.sheet_origin(sheet) == CascadeOrigin::Author,
        );
    });
}

pub(super) fn insert_scope_rule(
    dispatch: &mut RuleDispatch,
    programs: &SelectorPrograms,
    rule: RuleID,
    selector_program: SelectorProgramID,
    author: bool,
) {
    let compiled = programs.get(selector_program);
    for index in 0..compiled.entries().len() {
        let metadata = compiled.dispatch_metadata(index);
        let key = metadata.key;
        let subject_dispatch = metadata.subject_dispatch_keys();
        let template = super::index::DispatchEntry {
            identity: programs.entry_id(
                selector_program,
                u32::try_from(index).expect("selector entry space exhausted"),
            ),
            rule,
            program: selector_program,
            entry: u32::try_from(index).expect("selector entry space exhausted"),
            cascade_order: 0,
            required_attribute_value: metadata.required_attribute_value,
            required_parent: metadata.required_parent,
            required_ancestor: metadata.required_ancestor,
            required_ancestor_index: None,
            required_subject_bloom: metadata.required_subject_bloom,
            prefix_matched: false,
            multi_key: false,
        };
        // A subject whose one key is universal but whose disjunction has selective branches
        // - `:is(dl, ol) :is(.a, .b)` dispatches on nothing alone - goes under each branch's
        // key instead of in front of every element. The cover is sound by the same contract
        // routing relies on, and it comes back empty rather than truncated, so an entry is
        // never unreachable from a key it needs.
        let branch_keys = match key {
            DispatchKey::Universal if !subject_dispatch.is_empty() => {
                let mut branch_keys = subject_dispatch.to_vec();
                branch_keys.sort_unstable();
                branch_keys.dedup();
                branch_keys
            }
            _ => Vec::new(),
        };
        if branch_keys.is_empty() {
            let dispatch_entry = dispatch.insert(key, template);
            if let Some(chain) = metadata.prefix_chain() {
                // NB: Structural truth bits are admitted for author rules only. The
                //     convergence walk consumes only author routes, so a user-agent
                //     positional chain would put its tests into every document's
                //     automaton, taxing each transition and widening every tree
                //     flush's re-compare frontier, without any route ever being
                //     subsumed in return.
                dispatch.add_prefix_entry(programs, selector_program, chain, dispatch_entry, author);
            }
        } else {
            // NB: A branch copy carries no required attribute value: a disjunction branch's
            //     value is not required by the subject, and one copy serves every branch
            //     naming the same attribute.
            //
            // NB: A branch copy stays out of the prefix program. The prefix pass emits one
            //     match per registered entry, so registering every copy would report the
            //     same rule once per branch key the element carries; the candidate walk
            //     deduplicates them by the rank the copies share instead.
            for &branch_key in &branch_keys {
                dispatch.insert(
                    branch_key,
                    super::index::DispatchEntry {
                        multi_key: true,
                        ..template
                    },
                );
            }
        }
    }
}

fn for_each_scope_rule(
    program: &StyleSheetProgram,
    tree_scope: TreeScopeID,
    take: SheetsToTake,
    mut callback: impl FnMut(super::program::SheetID, RuleID, SelectorProgramID),
) {
    for sheet in program.sheets_in_scope(tree_scope) {
        if take == SheetsToTake::NonAuthorOnly
            && !matches!(
                program.sheet_origin(sheet),
                CascadeOrigin::UserAgent | CascadeOrigin::User
            )
        {
            continue;
        }
        for rule in program.rules_in_sheet(sheet) {
            // A rule behind a condition that does not hold, or in a disabled sheet, stays in the
            // dispatch: activation is match-time state, filtered at candidate consumption, so a
            // condition flipping does not invalidate the dispatch structure or anything derived
            // from it. Only structural liveness excludes a rule here.
            if !program.rule_is_live(rule) {
                continue;
            }
            let version = program.rule_version(rule);
            if !version.kind.matches_elements() {
                continue;
            }
            let Some(selector_program) = version.selector_program else {
                continue;
            };
            callback(sheet, rule, selector_program);
        }
    }
}

/// Exact ancestor requirements, packed once per fact-batch row.
///
/// Many selector entries ask the same question of one candidate's ancestry. Walking that ancestry
/// for each entry repeats both relation steps and fact lookups, so a cold batch assigns every
/// required key one bit and inherits the bits from parent to child once.
pub(super) struct AncestorRequirements {
    words_per_row: usize,
    bits: Vec<u64>,
    /// Rows whose ancestry the bits do not fully describe, so nothing may be rejected from them.
    unbounded: Vec<bool>,
    /// The one row a per-element summary describes, or none when every row has a slice of its own.
    single_row: Option<u32>,
}

impl AncestorRequirements {
    #[must_use]
    pub(super) fn required_bytes(row_count: usize, key_count: usize) -> u64 {
        let words_per_row = key_count.div_ceil(u64::BITS as usize);
        let bytes = row_count
            .saturating_mul(words_per_row)
            .saturating_mul(size_of::<u64>())
            .saturating_add(row_count.saturating_mul(size_of::<bool>()))
            .saturating_add(row_count.saturating_mul(size_of::<u32>()));
        u64::try_from(bytes).unwrap_or(u64::MAX)
    }

    pub(super) fn build(tree: &StyleNodeTree, facts: &StyleNodeFacts, dispatch: &RuleDispatch) -> Self {
        let words_per_row = dispatch.ancestor_key_count().div_ceil(u64::BITS as usize);
        let row_count = facts.row_count();
        let mut bits = vec![0; row_count.saturating_mul(words_per_row)];
        let mut unbounded = vec![false; row_count];
        let mut computed = vec![false; row_count];
        let mut chain = Vec::with_capacity(row_count);

        // The batch holds a row per element, so a parent with no row is not one: a shadow root,
        // which publishes nothing and stands between a tree and whatever hosts it. Inheriting stops
        // there - and from there on the bits are no longer the whole answer, because a selector can
        // cross that boundary. `:host(.h) .inner` requires `.h` of the host, which is not an
        // ancestor of anything inside the tree. Rows below a boundary are therefore marked
        // unbounded and reject nothing, exactly as walking the ancestors does when it meets a node
        // it has no facts for.
        let parent_row = |node: StyleNodeID| tree.parent(node).and_then(|parent| facts.row_of(parent));

        // Rows normally arrive in tree order. Following parents until an already-built row also
        // keeps this exact for a differently ordered batch without making deep DOM trees recurse.
        for start in 0..row_count {
            if !facts.has_row(u32::try_from(start).expect("fact batch row space exhausted")) {
                computed[start] = true;
                unbounded[start] = true;
                continue;
            }
            if computed[start] {
                continue;
            }
            let mut row = start;
            while !computed[row] {
                chain.push(u32::try_from(row).expect("fact batch row space exhausted"));
                let Some(parent) = parent_row(facts.node_at(row as u32)) else {
                    break;
                };
                row = parent as usize;
            }

            while let Some(row) = chain.pop() {
                let row = row as usize;
                let node = facts.node_at(row as u32);
                if let Some(parent_row) = parent_row(node) {
                    let parent = tree.parent(node).expect("a parent row implies a parent");
                    let parent_row = parent_row as usize;
                    unbounded[row] = unbounded[parent_row];
                    for word in 0..words_per_row {
                        bits[row * words_per_row + word] = bits[parent_row * words_per_row + word];
                    }
                    facts.for_each_dispatch_key(parent_row as u32, tree.parent(parent).is_none(), |key| {
                        let Some(index) = dispatch.ancestor_key_index(key) else {
                            return;
                        };
                        let index = index as usize;
                        bits[row * words_per_row + index / u64::BITS as usize] |= 1_u64 << (index % u64::BITS as usize);
                    });
                } else {
                    unbounded[row] = tree.parent(node).is_some();
                }
                computed[row] = true;
            }
        }

        Self {
            words_per_row,
            bits,
            unbounded,
            single_row: None,
        }
    }

    #[must_use]
    pub(super) fn required_bytes_for_one_row(key_count: usize) -> u64 {
        let words = key_count.div_ceil(u64::BITS as usize);
        u64::try_from(words.saturating_mul(size_of::<u64>()).saturating_add(size_of::<bool>())).unwrap_or(u64::MAX)
    }

    /// Summarize one element's ancestor requirements with a single walk.
    ///
    /// `None` means the facts materialized so far do not describe the element itself, which is for
    /// the caller to answer rather than this. Running out of ancestors part of the way up is a
    /// different thing and is recorded the same way the batch build records it: as a row the bits
    /// do not describe, which rejects nothing.
    #[must_use]
    pub(super) fn build_for_node(
        tree: &StyleNodeTree,
        facts: &StyleNodeFacts,
        dispatch: &RuleDispatch,
        node: StyleNodeID,
    ) -> Option<Self> {
        let row = facts.row_of(node)?;
        let words_per_row = dispatch.ancestor_key_count().div_ceil(u64::BITS as usize);
        let mut bits = vec![0; words_per_row];
        let mut unbounded = false;
        let mut current = node;
        while let Some(parent) = tree.parent(current) {
            let Some(parent_row) = facts.row_of(parent) else {
                unbounded = true;
                break;
            };
            facts.for_each_dispatch_key(parent_row, tree.parent(parent).is_none(), |key| {
                let Some(index) = dispatch.ancestor_key_index(key) else {
                    return;
                };
                let index = index as usize;
                bits[index / u64::BITS as usize] |= 1_u64 << (index % u64::BITS as usize);
            });
            current = parent;
        }
        Some(Self {
            words_per_row,
            bits,
            unbounded: vec![unbounded],
            single_row: Some(row),
        })
    }

    #[must_use]
    pub(super) fn dispatch_facts(&self, row: u32) -> AncestorDispatchFacts<'_> {
        let row = match self.single_row {
            Some(single_row) => {
                debug_assert_eq!(row, single_row);
                0
            }
            None => row as usize,
        };
        let first = row * self.words_per_row;
        AncestorDispatchFacts::new(&self.bits[first..first + self.words_per_row], self.unbounded[row])
    }

    #[must_use]
    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.bits, self.unbounded];
            cached [];
            nested [];
            skip [self.words_per_row, self.single_row];
        }
    }
}

/// Exact ancestor summaries shared by one synchronous matching traversal.
const ANCESTOR_REQUIREMENTS_PROMOTION_ASKS: u32 = 128;

#[derive(Default)]
pub(super) struct AncestorRequirementsCache {
    // A document has a handful of selector topologies at most, so a list keeps the summaries
    // contiguous. Concrete rule identities and cascade ranks do not affect ancestor requirements.
    by_topology: Vec<(AncestorDispatchTopologyID, AncestorRequirements)>,
    local_asks: Vec<(AncestorDispatchTopologyID, u32)>,
    local_answer: Option<AncestorRequirements>,
    last_answered: usize,
    charged_bytes: u64,
}

impl AncestorRequirementsCache {
    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.by_topology, self.local_asks];
            cached [];
            nested [self
                .by_topology
                .iter()
                .map(|(_, requirements)| requirements.capacity_bytes())
                .sum::<u64>(), self.local_answer.as_ref().map_or(0, AncestorRequirements::capacity_bytes)];
            skip [self.last_answered, self.charged_bytes];
        }
    }

    fn index_of(&self, topology: AncestorDispatchTopologyID) -> Option<usize> {
        if let Some((held, _)) = self.by_topology.get(self.last_answered)
            && *held == topology
        {
            return Some(self.last_answered);
        }
        self.by_topology.iter().position(|(held, _)| *held == topology)
    }

    pub(super) fn get_or_build(
        &mut self,
        tree: &StyleNodeTree,
        facts: &StyleNodeFacts,
        dispatch: &RuleDispatch,
        memory: &mut MemoryController,
    ) -> &AncestorRequirements {
        let topology = dispatch.ancestor_topology_id();
        let index = match self.index_of(topology) {
            Some(index) => index,
            None => {
                let build_scratch =
                    AncestorRequirements::required_bytes(facts.row_count(), dispatch.ancestor_key_count());
                memory.reserve_required(MemoryCategory::BatchScratch, build_scratch);
                self.by_topology
                    .push((topology, AncestorRequirements::build(tree, facts, dispatch)));

                let current = self.capacity_bytes();
                let growth = current.saturating_sub(self.charged_bytes);
                if growth > build_scratch {
                    memory.reserve_required(MemoryCategory::BatchScratch, growth - build_scratch);
                } else {
                    memory.release(MemoryCategory::BatchScratch, build_scratch - growth);
                }
                self.charged_bytes = current;
                self.by_topology.len() - 1
            }
        };
        self.last_answered = index;
        &self.by_topology[index].1
    }

    pub(super) fn get_or_build_for_node(
        &mut self,
        tree: &StyleNodeTree,
        facts: &StyleNodeFacts,
        dispatch: &RuleDispatch,
        node: StyleNodeID,
        memory: &mut MemoryController,
    ) -> &AncestorRequirements {
        let topology = dispatch.ancestor_topology_id();
        if let Some(index) = self.index_of(topology) {
            self.last_answered = index;
            return &self.by_topology[index].1;
        }

        let asks = match self.local_asks.iter_mut().find(|(candidate, _)| *candidate == topology) {
            Some((_, asks)) => asks,
            None => {
                self.local_asks.push((topology, 0));
                &mut self.local_asks.last_mut().unwrap().1
            }
        };
        *asks = asks.checked_add(1).expect("ancestor summary ask count overflow");
        // A local summary walks only this node's shallow ancestry. Promote a topology only after
        // enough asks to amortize scanning every prepared fact row into a reusable dense matrix.
        if *asks >= ANCESTOR_REQUIREMENTS_PROMOTION_ASKS && tree.tree_scope(node) == TreeScopeID::DOCUMENT {
            self.local_answer = None;
            return self.get_or_build(tree, facts, dispatch, memory);
        }

        let build_scratch = AncestorRequirements::required_bytes_for_one_row(dispatch.ancestor_key_count());
        memory.reserve_required(MemoryCategory::BatchScratch, build_scratch);
        self.local_answer = Some(
            AncestorRequirements::build_for_node(tree, facts, dispatch, node)
                .expect("a matched node must have prepared facts"),
        );
        let current = self.capacity_bytes();
        let growth = current.saturating_sub(self.charged_bytes);
        if growth > build_scratch {
            memory.reserve_required(MemoryCategory::BatchScratch, growth - build_scratch);
        } else {
            memory.release(MemoryCategory::BatchScratch, build_scratch - growth);
        }
        self.charged_bytes = current;
        self.local_answer.as_ref().unwrap()
    }

    pub(super) fn release(&mut self, memory: &mut MemoryController) {
        memory.release(MemoryCategory::BatchScratch, self.charged_bytes);
        self.charged_bytes = 0;
        self.by_topology = Vec::new();
        self.local_asks = Vec::new();
        self.local_answer = None;
        self.last_answered = 0;
    }
}

/// Exactly evaluates rule matching over a region, consulting no derived state.
pub struct BatchMatcher<'a> {
    tree: &'a StyleNodeTree,
    facts: &'a StyleNodeFacts,
    dispatch: &'a RuleDispatch,
    programs: &'a SelectorPrograms,
    /// The stylesheet program, for the activation filter: the dispatch keeps every structurally
    /// live rule, and whether one decides right now is asked per candidate.
    program: &'a StyleSheetProgram,
    /// The tree scope whose rules are being evaluated.
    scope: TreeScopeID,
    /// The shadow root of the tree whose rules are being evaluated, when it is one.
    shadow_root: Option<StyleNodeID>,
    /// Whether the node being asked stands outside this tree and is slotted into it. Only
    /// `::slotted()` reaches such a node; the tree's ordinary selectors decide inside it.
    node_is_slotted_in: bool,
    /// Whether the node being asked is inside a tree this scope contains, exposed to it as a part.
    /// Only `::part()` reaches such a node.
    node_is_a_part_exposed_here: bool,
    /// Whether the node being asked is the host of the tree whose rules these are. A host stands
    /// outside the tree its own root opens and is featureless there, so only `:host` reaches it.
    node_is_the_host_of_this_tree: bool,
    ancestor_requirements: Option<&'a AncestorRequirements>,
    match_workspace: Option<&'a MatchEvaluationWorkspace>,
    /// Evaluate only these rules, sorted by (rule, program). The retained-answer patch asks one
    /// node about exactly the rules a transaction reached, through the same machinery a full
    /// match uses.
    rule_filter: Option<&'a [(RuleID, SelectorProgramID)]>,
    cascade_only: bool,
    witnesses: Option<&'a std::cell::RefCell<RelationalWitnesses>>,
}

/// Reusable output-side state for one exact matching attempt.
pub(super) struct BatchMatchState<'a> {
    pub dispatch_workspace: &'a mut DispatchCandidateWorkspace,
    pub requests: Option<&'a mut Vec<Incomplete>>,
    pub completed: Option<&'a mut [bool]>,
    pub prefix_states: Option<&'a mut PrefixStates>,
    pub deferred_prefix_matches: Option<&'a mut Option<PrefixMatchSetID>>,
}

#[allow(clippy::too_many_arguments)]
fn append_matched_entry(
    out: &mut RuleMatches,
    start: usize,
    node: StyleNodeID,
    scope: TreeScopeID,
    candidate: DispatchEntry,
    compiled: &SelectorProgram,
    entry: &SelectorEntry,
    scope_proximity: u32,
    completed: &mut Option<&mut [bool]>,
    counters: &mut Counters,
    count_emission: CountRuleMatchEmission,
) {
    out.record_selector_truth(candidate.identity, scope, scope_proximity);
    let candidate_index = candidate.cascade_order as usize;
    // A selector list can match through several entries. Only its greatest matching specificity
    // contributes, independently for the element and each pseudo-element target.
    if compiled.entries().len() > 1 {
        let existing = match completed.is_some() {
            true => out.matches.iter_mut().find(|existing| {
                existing.node == node
                    && existing.tree_scope == scope
                    && existing.rule == candidate.rule
                    && existing.pseudo_element == entry.pseudo_element
            }),
            false => out.matches[start..]
                .iter_mut()
                .find(|existing| existing.rule == candidate.rule && existing.pseudo_element == entry.pseudo_element),
        };
        if let Some(existing) = existing {
            if entry.specificity > existing.specificity {
                existing.entry = candidate.entry;
                existing.cascade_order = candidate.cascade_order;
                existing.specificity = entry.specificity;
                existing.scope_proximity = scope_proximity;
            }
            if let Some(completed) = completed.as_deref_mut() {
                completed[candidate_index] = true;
            }
            return;
        }
    }

    out.matches.push(RuleMatch {
        node,
        pseudo_element: entry.pseudo_element,
        rule: candidate.rule,
        program: candidate.program,
        entry: candidate.entry,
        cascade_order: candidate.cascade_order,
        tree_scope: scope,
        specificity: entry.specificity,
        scope_proximity,
    });
    if count_emission == CountRuleMatchEmission::Yes {
        counters.bump(Counter::RuleMatchesEmitted);
    }
    if let Some(completed) = completed.as_deref_mut() {
        completed[candidate_index] = true;
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum CountRuleMatchEmission {
    No,
    Yes,
}

#[allow(clippy::too_many_arguments)]
pub(super) fn append_prefix_matches(
    out: &mut RuleMatches,
    node: StyleNodeID,
    scope: TreeScopeID,
    dispatch: &RuleDispatch,
    program: &StyleSheetProgram,
    programs: &SelectorPrograms,
    matches: &[EntryID],
    counters: &mut Counters,
    count_emission: CountRuleMatchEmission,
) {
    let mut completed = None;
    for &matched in matches {
        for candidate in dispatch.entries_for_identity(matched) {
            if !candidate.prefix_matched {
                continue;
            }
            if !program.rule_can_decide(candidate.rule) {
                continue;
            }
            let compiled = programs.get(candidate.program);
            let entry = &compiled.entries()[candidate.entry as usize];
            append_matched_entry(
                out,
                0,
                node,
                scope,
                candidate,
                compiled,
                entry,
                u32::MAX,
                &mut completed,
                counters,
                count_emission,
            );
        }
    }
}

#[allow(clippy::too_many_arguments)]
pub(super) fn append_selector_truth_matches(
    out: &mut RuleMatches,
    node: StyleNodeID,
    program: &StyleSheetProgram,
    programs: &SelectorPrograms,
    dispatch: &RuleDispatch,
    truth: &[super::SelectorTruth],
    counters: &mut Counters,
) {
    let mut completed = None;
    for &matched in truth {
        let mut previous = None;
        for candidate in dispatch.entries_for_identity(matched.entry) {
            let candidate_key = (candidate.rule, candidate.program, candidate.entry);
            if previous == Some(candidate_key) {
                continue;
            }
            previous = Some(candidate_key);
            if !program.rule_can_decide(candidate.rule) {
                continue;
            }
            let compiled = programs.get(candidate.program);
            let entry = &compiled.entries()[candidate.entry as usize];
            append_matched_entry(
                out,
                0,
                node,
                matched.tree_scope,
                candidate,
                compiled,
                entry,
                matched.scope_proximity,
                &mut completed,
                counters,
                CountRuleMatchEmission::No,
            );
        }
    }
}

pub(super) fn append_retained_matches(
    out: &mut RuleMatches,
    node: StyleNodeID,
    tree_scope: TreeScopeID,
    programs: &SelectorPrograms,
    dispatch: &RuleDispatch,
    matches: &[super::RetainedRuleMatch],
) -> Option<()> {
    out.matches.reserve_exact(matches.len());
    for &stored_match in matches {
        let mut matched = stored_match;
        matched.tree_scope = tree_scope;
        let cascade_order = dispatch.cascade_order_for_entry(matched.rule, matched.program, matched.entry)?;
        let matched = matched.materialize(node, programs, cascade_order)?;
        if programs.get(matched.program).entries().len() > 1
            && let Some(existing) = out.matches.iter_mut().find(|existing| {
                existing.rule == matched.rule
                    && existing.tree_scope == matched.tree_scope
                    && existing.pseudo_element == matched.pseudo_element
            })
        {
            if matched.specificity > existing.specificity {
                *existing = matched;
            }
            continue;
        }
        out.matches.push(matched);
    }
    Some(())
}

impl<'a> BatchMatcher<'a> {
    #[must_use]
    pub fn new(
        tree: &'a StyleNodeTree,
        facts: &'a StyleNodeFacts,
        dispatch: &'a RuleDispatch,
        programs: &'a SelectorPrograms,
        program: &'a StyleSheetProgram,
    ) -> Self {
        Self {
            tree,
            facts,
            dispatch,
            programs,
            program,
            scope: TreeScopeID::DOCUMENT,
            shadow_root: None,
            node_is_slotted_in: false,
            node_is_a_part_exposed_here: false,
            node_is_the_host_of_this_tree: false,
            ancestor_requirements: None,
            match_workspace: None,
            rule_filter: None,
            cascade_only: false,
            witnesses: None,
        }
    }

    /// Match only rules that can still contribute to the cascade.
    #[must_use]
    pub fn for_cascade(mut self) -> Self {
        self.cascade_only = true;
        self
    }

    fn cascade_properties_of(&self, entry: DispatchEntry) -> Option<&'a [u16]> {
        self.dispatch.cascade_properties(entry)
    }

    /// Evaluate the rules of one tree scope. What a rule's scope is decides how deeply encapsulated
    /// it is, which the cascade compares.
    #[must_use]
    pub fn in_scope(mut self, scope: TreeScopeID) -> Self {
        self.scope = scope;
        self
    }

    /// Record completed simple relational evaluations in `witnesses`. The batch matcher reads
    /// the live tree and authoritative facts, which is exactly the evaluation retention trusts.
    #[must_use]
    pub fn observing_witnesses(mut self, witnesses: &'a std::cell::RefCell<RelationalWitnesses>) -> Self {
        self.witnesses = Some(witnesses);
        self
    }

    /// Evaluate rules attached to one shadow tree rather than to the document.
    #[must_use]
    pub fn in_shadow_tree(mut self, shadow_root: StyleNodeID) -> Self {
        self.shadow_root = Some(shadow_root);
        self
    }

    /// Ask this tree's rules of a node that is slotted into it rather than in it.
    #[must_use]
    pub fn for_a_node_slotted_in(mut self) -> Self {
        self.node_is_slotted_in = true;
        self
    }

    /// Ask this scope's rules of a node inside a shadow tree it contains, exposed to it as a part.
    #[must_use]
    pub fn for_a_part_exposed_here(mut self) -> Self {
        self.node_is_a_part_exposed_here = true;
        self
    }

    /// Ask this tree's rules of the host it hangs off, which stands outside the tree.
    #[must_use]
    pub fn for_the_host_of_this_tree(mut self) -> Self {
        self.node_is_the_host_of_this_tree = true;
        self
    }

    #[must_use]
    pub(super) fn with_ancestor_requirements(mut self, requirements: &'a AncestorRequirements) -> Self {
        self.ancestor_requirements = Some(requirements);
        self
    }

    #[must_use]
    pub(super) fn with_match_workspace(mut self, workspace: &'a MatchEvaluationWorkspace) -> Self {
        self.match_workspace = Some(workspace);
        self
    }

    /// Evaluate only these rules, sorted by (rule, program).
    #[must_use]
    pub(super) fn with_rule_filter(mut self, rules: &'a [(RuleID, SelectorProgramID)]) -> Self {
        self.rule_filter = Some(rules);
        self
    }

    /// A filtered ask is a patch re-deriving part of one answer; only a full evaluation counts
    /// as matching activity.
    fn count_rule_match_emission(&self) -> CountRuleMatchEmission {
        match self.rule_filter.is_none() {
            true => CountRuleMatchEmission::Yes,
            false => CountRuleMatchEmission::No,
        }
    }

    fn rule_filter_admits(&self, rule: RuleID, program: SelectorProgramID) -> bool {
        self.rule_filter
            .is_none_or(|rules| rules.binary_search(&(rule, program)).is_ok())
    }

    fn filtered_rules_are_narrow(&self, rules: &[(RuleID, SelectorProgramID)]) -> bool {
        let mut entry_count = 0;
        for &(rule, program) in rules {
            if !self.program.rule_can_decide(rule) || self.program.rule_version(rule).selector_program != Some(program)
            {
                continue;
            }
            entry_count += self.programs.get(program).entries().len();
            // Dispatch pays a fixed cost to gather the node's subject buckets before applying the
            // rule filter. Exact enumeration avoids that cost for a narrow patch, but gives up the
            // subject index's pruning, so keep broad selector lists on the indexed path.
            if entry_count > 128 {
                return false;
            }
        }
        true
    }

    fn match_filtered_rules_directly(
        &self,
        node: StyleNodeID,
        row: u32,
        rules: &[(RuleID, SelectorProgramID)],
        evaluator: &MatchEvaluator<'_>,
        out: &mut RuleMatches,
        counters: &mut Counters,
    ) -> Result<(), Incomplete> {
        let start = out.matches.len();
        let selector_truth_start = out.selector_truth_len();
        let is_document_root = self.tree.parent(node).is_none();
        for &(rule, program) in rules {
            if !self.program.rule_can_decide(rule) || self.program.rule_version(rule).selector_program != Some(program)
            {
                continue;
            }
            let compiled = self.programs.get(program);
            for (entry_index, entry) in compiled.entries().iter().enumerate() {
                let subject_dispatch = compiled.subject_dispatch_keys(entry_index);
                if !subject_dispatch.is_empty()
                    && !subject_dispatch
                        .iter()
                        .any(|&key| self.facts.carries_dispatch_key(row, key, is_document_root))
                {
                    continue;
                }
                if compiled
                    .subject_required_keys(entry_index)
                    .iter()
                    .any(|&key| !self.facts.carries_dispatch_key(row, key, is_document_root))
                {
                    continue;
                }
                let entry_index = u32::try_from(entry_index).expect("selector entry space exhausted");
                let Some(cascade_order) = self.dispatch.cascade_order_for_entry(rule, program, entry_index) else {
                    continue;
                };
                if self.node_is_slotted_in && !compiled.subject_is_slotted(entry.root) {
                    continue;
                }
                if self.node_is_a_part_exposed_here && !compiled.subject_is_a_part(entry.root) {
                    continue;
                }
                if self.node_is_the_host_of_this_tree && !compiled.subject_is_the_host(entry.root) {
                    continue;
                }
                counters.bump(Counter::CandidateChecks);
                let matches = match evaluator.matches_entry_for_program(program, compiled, entry, node, counters) {
                    Ok(matches) => matches,
                    Err(incomplete) => {
                        out.truncate(start, selector_truth_start);
                        return Err(incomplete);
                    }
                };
                if !matches {
                    continue;
                }
                let scope_proximity = match evaluator.scope_proximity_of(compiled, entry, node, counters) {
                    Ok(scope_proximity) => scope_proximity,
                    Err(incomplete) => {
                        out.truncate(start, selector_truth_start);
                        return Err(incomplete);
                    }
                };
                let matched = RuleMatch {
                    node,
                    pseudo_element: entry.pseudo_element,
                    rule,
                    program,
                    entry: entry_index,
                    cascade_order,
                    tree_scope: self.scope,
                    specificity: entry.specificity,
                    scope_proximity,
                };
                out.record_selector_truth(
                    self.programs.entry_id(program, entry_index),
                    self.scope,
                    scope_proximity,
                );
                if let Some(existing) = out.matches[start..]
                    .iter_mut()
                    .find(|existing| existing.rule == rule && existing.pseudo_element == entry.pseudo_element)
                {
                    if matched.specificity > existing.specificity {
                        *existing = matched;
                    }
                } else {
                    out.matches.push(matched);
                }
            }
        }
        Ok(())
    }

    /// Evaluate every style node in the subtree rooted at `root`, appending matches in node order.
    ///
    /// A missing fact row aborts the pass rather than being answered: the caller widens its batch
    /// and asks again. Partial output is never published.
    #[cfg(test)]
    pub fn match_subtree(
        &self,
        root: StyleNodeID,
        out: &mut RuleMatches,
        counters: &mut Counters,
    ) -> Result<(), Incomplete> {
        let start = out.matches.len();
        let selector_truth_start = out.selector_truth_len();
        let mut dispatch_workspace = DispatchCandidateWorkspace::default();
        let mut prefix_states = PrefixStates::new(self.facts.row_count());
        for node in self.tree.preorder(root) {
            if let Err(incomplete) = self.match_node_collecting_requests(
                node,
                out,
                counters,
                BatchMatchState {
                    dispatch_workspace: &mut dispatch_workspace,
                    requests: None,
                    completed: None,
                    prefix_states: Some(&mut prefix_states),
                    deferred_prefix_matches: None,
                },
            ) {
                out.truncate(start, selector_truth_start);
                return Err(incomplete);
            }
        }
        out.matches[start..].sort_unstable_by_key(|entry| (entry.node, entry.rule));
        Ok(())
    }

    /// Evaluate one style node exactly.
    #[cfg(test)]
    pub fn match_node(
        &self,
        node: StyleNodeID,
        out: &mut RuleMatches,
        counters: &mut Counters,
    ) -> Result<(), Incomplete> {
        let mut dispatch_workspace = DispatchCandidateWorkspace::default();
        let mut prefix_states = PrefixStates::new(self.facts.row_count());
        self.match_node_collecting_requests(
            node,
            out,
            counters,
            BatchMatchState {
                dispatch_workspace: &mut dispatch_workspace,
                requests: None,
                completed: None,
                prefix_states: Some(&mut prefix_states),
                deferred_prefix_matches: None,
            },
        )
    }

    /// Evaluate one style node while retaining every fact request the candidate set discovers.
    ///
    /// The ordinary document pass materializes the whole scope and has nothing to batch. An
    /// element-at-a-time cold ask starts from a deliberately small fact set, though, and restarting
    /// at the first missing row makes independent selector paths each replay every candidate that
    /// preceded them. Collecting the exact requests lets that caller materialize them together and
    /// retry the unfinished candidates together.
    pub(super) fn match_node_collecting_requests(
        &self,
        node: StyleNodeID,
        out: &mut RuleMatches,
        counters: &mut Counters,
        state: BatchMatchState<'_>,
    ) -> Result<(), Incomplete> {
        let BatchMatchState {
            dispatch_workspace,
            mut requests,
            mut completed,
            prefix_states,
            deferred_prefix_matches,
        } = state;
        let Some(row) = self.facts.row_of(node) else {
            return Err(Incomplete::MissingFacts(node));
        };
        // A filtered ask is a patch re-deriving part of one answer, not a cold evaluation.
        if self.rule_filter.is_none() {
            counters.bump(Counter::ColdNodesEvaluated);
        }

        let mut evaluator = MatchEvaluator::new(self.tree, self.facts);
        if let Some(workspace) = self.match_workspace {
            evaluator = evaluator.with_match_workspace(workspace, MatchEvaluationSide::Current);
        }
        if let Some(shadow_root) = self.shadow_root {
            evaluator = evaluator.in_shadow_tree(shadow_root);
        }
        if self.node_is_a_part_exposed_here {
            evaluator = evaluator.for_a_part_exposed_in(self.scope);
        }
        if let Some(witnesses) = self.witnesses {
            evaluator = evaluator.observing_witnesses(witnesses);
        }
        if let Some(rules) = self.rule_filter.filter(|rules| self.filtered_rules_are_narrow(rules)) {
            return self.match_filtered_rules_directly(node, row, rules, &evaluator, out, counters);
        }
        let start = out.matches.len();
        let selector_truth_start = out.selector_truth_len();
        let mut used_prefixes = false;
        let mut cascade_pruning_blocked = false;
        if !self.node_is_slotted_in && !self.node_is_a_part_exposed_here && !self.node_is_the_host_of_this_tree {
            let prefix_matches = match prefix_states {
                None => None,
                Some(states) => {
                    let evaluation = PrefixEvaluation::new(
                        self.dispatch.prefixes(),
                        self.tree,
                        self.facts,
                        self.programs,
                        &evaluator,
                        self.shadow_root,
                        None,
                    );
                    match states.match_set_for(&evaluation, node, counters) {
                        PrefixTransitionLookup::Known(matches) => Some((states, matches)),
                        // The sibling-aware walk reads the node's left context, which a selective
                        // batch may not have materialized yet. A missing row is answered the way
                        // every other missing row is, so the automaton keeps the entries it owns
                        // instead of flooding the exact fallback with them. A request-collecting
                        // pass names the whole left context at once to preserve its one-restart
                        // contract; an aborting pass reports the first gap and lets the caller's
                        // sibling window widen around it.
                        PrefixTransitionLookup::Missing(PrefixTransitionGap::Incomplete(Incomplete::MissingFacts(
                            missing,
                        ))) if self.dispatch.prefixes().has_sibling_steps() => {
                            let Some(requests) = requests.as_deref_mut() else {
                                return Err(Incomplete::MissingFacts(missing));
                            };
                            let mut level = Some(node);
                            while let Some(current) = level {
                                let mut previous = self.tree.previous_element_sibling(current);
                                while let Some(sibling) = previous {
                                    if self.facts.row_of(sibling).is_none() {
                                        requests.push(Incomplete::MissingFacts(sibling));
                                    }
                                    previous = self.tree.previous_element_sibling(sibling);
                                }
                                level = self
                                    .tree
                                    .parent(current)
                                    .filter(|&parent| Some(parent) != self.shadow_root);
                            }
                            None
                        }
                        PrefixTransitionLookup::Missing(_) => None,
                    }
                }
            };
            if let Some((states, prefix_matches)) = prefix_matches {
                used_prefixes = true;
                for &matched in states.matches_in(prefix_matches) {
                    out.record_selector_truth(matched, self.scope, u32::MAX);
                }
                cascade_pruning_blocked = self.cascade_only
                    && states.matches_in(prefix_matches).iter().any(|&matched| {
                        self.dispatch
                            .entries_for_identity(matched)
                            .filter(|candidate| candidate.prefix_matched)
                            .any(|candidate| self.dispatch.cascade_pruning_blocker(candidate))
                    });
                if let Some(deferred) = deferred_prefix_matches {
                    *deferred = Some(prefix_matches);
                } else {
                    for &matched in states.matches_in(prefix_matches) {
                        for candidate in self.dispatch.entries_for_identity(matched) {
                            if !candidate.prefix_matched {
                                continue;
                            }
                            if !self.rule_filter_admits(candidate.rule, candidate.program) {
                                continue;
                            }
                            if !self.program.rule_can_decide(candidate.rule) {
                                continue;
                            }
                            let candidate_index = candidate.cascade_order as usize;
                            if completed.as_deref().is_some_and(|completed| completed[candidate_index]) {
                                continue;
                            }
                            let compiled = self.programs.get(candidate.program);
                            let entry = &compiled.entries()[candidate.entry as usize];
                            append_matched_entry(
                                out,
                                start,
                                node,
                                self.scope,
                                candidate,
                                compiled,
                                entry,
                                u32::MAX,
                                &mut completed,
                                counters,
                                self.count_rule_match_emission(),
                            );
                        }
                    }
                }
            }
        }
        let mut first_incomplete = None;
        // Copies of a multi-key entry share one cascade order; an element carrying two of the
        // entry's branch keys walks past the second copy here. The list stays tiny because only
        // disjunction subjects are multi-key.
        let mut seen_multi_key_orders: Vec<u32> = Vec::new();
        let is_document_root = self.tree.parent(node).is_none();
        let parent_facts = match self.tree.parent(node) {
            None => ParentDispatchFacts::NoElementParent,
            Some(parent) => match self.facts.row_of(parent) {
                Some(row) => ParentDispatchFacts::Known {
                    row,
                    is_document_root: self.tree.parent(parent).is_none(),
                },
                None => ParentDispatchFacts::Unknown,
            },
        };
        let ancestor_facts = self
            .ancestor_requirements
            .map(|requirements| requirements.dispatch_facts(row));
        let mut cascade_winners = Top1Cascade::with_capacity(0);
        let mut matched_pseudo_targets = Vec::new();
        if self.cascade_only {
            for matched in out
                .matches
                .iter()
                .filter(|matched| matched.node == node && matched.tree_scope == self.scope)
            {
                if self.dispatch.cascade_pruning_blocker_for_order(matched.cascade_order) {
                    cascade_pruning_blocked = true;
                }
                let compiled = self.programs.get(matched.program);
                let entry = &compiled.entries()[matched.entry as usize];
                let Some(properties) = self.dispatch.cascade_properties_for_order(matched.cascade_order) else {
                    continue;
                };
                if let Some(target) = entry.pseudo_element
                    && !matched_pseudo_targets.contains(&target)
                {
                    matched_pseudo_targets.push(target);
                }
                for &property in properties {
                    cascade_winners.consider((entry.pseudo_element, property), matched.cascade_order, ());
                }
            }
        }
        let candidates = match used_prefixes {
            true => CandidateEntries::NonPrefix,
            false => CandidateEntries::All,
        };
        for candidate in self.dispatch.candidates_for(
            self.facts,
            row,
            is_document_root,
            parent_facts,
            ancestor_facts,
            candidates,
            self.cascade_only,
            dispatch_workspace,
        ) {
            let candidate_index = candidate.cascade_order as usize;
            if candidate.multi_key {
                if seen_multi_key_orders.contains(&candidate.cascade_order) {
                    continue;
                }
                seen_multi_key_orders.push(candidate.cascade_order);
            }
            if completed.as_deref().is_some_and(|completed| completed[candidate_index]) {
                continue;
            }
            if !self.rule_filter_admits(candidate.rule, candidate.program) {
                continue;
            }
            // A rule behind a condition that does not hold decides nothing right now. Its entry
            // stays in the dispatch so the condition can flip without rebuilding it.
            if !self.program.rule_can_decide(candidate.rule) {
                if let Some(completed) = completed.as_deref_mut() {
                    completed[candidate_index] = true;
                }
                continue;
            }
            if candidate.prefix_matched && used_prefixes {
                if let Some(completed) = completed.as_deref_mut() {
                    completed[candidate_index] = true;
                }
                continue;
            }
            let compiled = self.programs.get(candidate.program);
            let entry = &compiled.entries()[candidate.entry as usize];
            if self.cascade_only
                && !cascade_pruning_blocked
                && let Some(properties) = self.cascade_properties_of(candidate)
            {
                let pseudo_target_is_known = entry
                    .pseudo_element
                    .is_none_or(|target| matched_pseudo_targets.contains(&target));
                let every_property_has_a_winner = properties.iter().all(|&property| {
                    cascade_winners
                        .winner(&(entry.pseudo_element, property))
                        .is_some_and(|winner| winner.priority >= candidate.cascade_order)
                });
                if pseudo_target_is_known && every_property_has_a_winner {
                    counters.bump(Counter::CascadeCandidatesRejectedByWinner);
                    if let Some(completed) = completed.as_deref_mut() {
                        completed[candidate_index] = true;
                    }
                    continue;
                }
            }
            counters.bump(Counter::CandidateChecks);
            // A slotted element is in this tree's light DOM, not in the tree: only the rules that
            // name it as such reach it.
            if self.node_is_slotted_in && !compiled.subject_is_slotted(entry.root) {
                if let Some(completed) = completed.as_deref_mut() {
                    completed[candidate_index] = true;
                }
                continue;
            }
            // A part is inside a tree this scope does not decide in: only the rules that name it as
            // a part reach it.
            if self.node_is_a_part_exposed_here && !compiled.subject_is_a_part(entry.root) {
                if let Some(completed) = completed.as_deref_mut() {
                    completed[candidate_index] = true;
                }
                continue;
            }
            // Considered within its own shadow trees, a host is featureless: only `:host`, `:host()`
            // and `:host-context()` are allowed to match it, and a `*` or a `.thing` in the tree
            // decides nothing about the element the tree hangs off.
            // https://drafts.csswg.org/css-scoping-1/#host-element-in-tree
            if self.node_is_the_host_of_this_tree && !compiled.subject_is_the_host(entry.root) {
                if let Some(completed) = completed.as_deref_mut() {
                    completed[candidate_index] = true;
                }
                continue;
            }
            // The filter rejects what it can prove and decides nothing else. A node the fact store
            // holds no row for - a shadow root, which publishes nothing at all - answers neither
            // way, so a candidate standing under one goes to the evaluator rather than being
            // rejected on a question nobody answered.
            if let Some(required) = candidate.required_parent {
                let rejected = self
                    .tree
                    .parent(node)
                    .is_none_or(|parent| evaluator.node_cannot_carry_dispatch_key(required, parent));
                if rejected {
                    if let Some(completed) = completed.as_deref_mut() {
                        completed[candidate_index] = true;
                    }
                    continue;
                }
            }
            if let Some(required) = candidate.required_ancestor {
                // The summary is exact within the node's own scope, where it also knows that a
                // shadow root carries nothing. The walk cannot: a node it has no row for answers
                // neither way, so it admits and the evaluator settles it.
                let rejected = match (ancestor_facts, candidate.required_ancestor_index) {
                    (Some(_), Some(_)) => false,
                    _ => self
                        .tree
                        .ancestors(node)
                        .all(|ancestor| evaluator.node_cannot_carry_dispatch_key(required, ancestor)),
                };
                if rejected {
                    if let Some(completed) = completed.as_deref_mut() {
                        completed[candidate_index] = true;
                    }
                    continue;
                }
            }
            match evaluator.matches_entry_for_program(candidate.program, compiled, entry, node, counters) {
                Ok(true) => {}
                Ok(false) => {
                    if let Some(completed) = completed.as_deref_mut() {
                        completed[candidate_index] = true;
                    }
                    continue;
                }
                Err(incomplete) => {
                    if self.dispatch.cascade_pruning_blocker(candidate) {
                        cascade_pruning_blocked = true;
                    }
                    first_incomplete.get_or_insert(incomplete);
                    if let Some(requests) = requests.as_deref_mut() {
                        if !requests.contains(&incomplete) {
                            requests.push(incomplete);
                        }
                        continue;
                    }
                    out.truncate(start, selector_truth_start);
                    return Err(incomplete);
                }
            }

            let scope_proximity = match evaluator.scope_proximity_of(compiled, entry, node, counters) {
                Ok(proximity) => proximity,
                Err(incomplete) => {
                    first_incomplete.get_or_insert(incomplete);
                    if let Some(requests) = requests.as_deref_mut() {
                        if !requests.contains(&incomplete) {
                            requests.push(incomplete);
                        }
                        continue;
                    }
                    out.truncate(start, selector_truth_start);
                    return Err(incomplete);
                }
            };
            if self.dispatch.cascade_pruning_blocker(candidate) {
                cascade_pruning_blocked = true;
            }
            if self.cascade_only
                && !cascade_pruning_blocked
                && let Some(properties) = self.cascade_properties_of(candidate)
            {
                if let Some(target) = entry.pseudo_element
                    && !matched_pseudo_targets.contains(&target)
                {
                    matched_pseudo_targets.push(target);
                }
                for &property in properties {
                    cascade_winners.consider((entry.pseudo_element, property), candidate.cascade_order, ());
                }
            }
            append_matched_entry(
                out,
                start,
                node,
                self.scope,
                candidate,
                compiled,
                entry,
                scope_proximity,
                &mut completed,
                counters,
                self.count_rule_match_emission(),
            );
        }
        if let Some(incomplete) = first_incomplete {
            if completed.is_none() {
                out.truncate(start, selector_truth_start);
            }
            return Err(incomplete);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::super::index::AttributeFact;
    use super::super::index::StateSet;
    use super::super::index::StyleAtomID;
    use super::super::memory::DeviceClass;
    use super::super::planning::SelectorTruthChanges;
    use super::super::planning::record_match_set_difference;
    use super::super::program::CascadeOrigin;
    use super::super::program::RuleKind;
    use super::super::program::StyleSheetObjectID;
    use super::super::selector::FeatureTest;
    use super::super::selector::NthPosition;
    use super::super::selector::SelectorOp;
    use super::super::selector::SelectorProgramBuilder;
    use super::super::selector::TagTest;
    use super::*;

    const TAG_DIV: StyleAtomID = StyleAtomID(1);
    const TAG_SPAN: StyleAtomID = StyleAtomID(2);
    const CLASS_THEME: StyleAtomID = StyleAtomID(10);
    const CLASS_ITEM: StyleAtomID = StyleAtomID(11);
    const CLASS_MISSING: StyleAtomID = StyleAtomID(12);
    const ID_HEADER: StyleAtomID = StyleAtomID(20);

    /// `div.theme > [ div.item, span#header, div.item ]`
    struct Document {
        memory: MemoryController,
        tree: StyleNodeTree,
        facts: StyleNodeFacts,
        programs: SelectorPrograms,
        program: StyleSheetProgram,
        nodes: Vec<StyleNodeID>,
        counters: Counters,
    }

    impl Document {
        fn new() -> Self {
            let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
            let mut tree = StyleNodeTree::new(&mut memory);
            let nodes: Vec<StyleNodeID> = (0..4).map(|_| tree.allocate_element(&mut memory)).collect();
            tree.set_first_element_child(nodes[0], Some(nodes[1]));
            for index in 1..4 {
                tree.set_parent(nodes[index], Some(nodes[0]));
                tree.set_next_element_sibling(nodes[index], nodes.get(index + 1).copied());
                tree.set_previous_element_sibling(nodes[index], (index > 1).then(|| nodes[index - 1]));
            }

            let mut facts = StyleNodeFacts::new();
            facts.push_row(
                nodes[0],
                TAG_DIV,
                StyleAtomID::NONE,
                StateSet::default(),
                &[CLASS_THEME],
                &[],
            );
            facts.push_row(
                nodes[1],
                TAG_DIV,
                StyleAtomID::NONE,
                StateSet::default(),
                &[CLASS_ITEM],
                &[],
            );
            facts.push_row(nodes[2], TAG_SPAN, ID_HEADER, StateSet::default(), &[], &[]);
            facts.push_row(
                nodes[3],
                TAG_DIV,
                StyleAtomID::NONE,
                StateSet::default(),
                &[CLASS_ITEM],
                &[],
            );

            let mut program = StyleSheetProgram::new();
            let sheet = program.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
            program.attach_sheet(sheet, TreeScopeID::DOCUMENT);

            Self {
                memory,
                tree,
                facts,
                programs: SelectorPrograms::new(),
                program,
                nodes,
                counters: Counters::new(),
            }
        }

        fn sheet(&self) -> super::super::program::SheetID {
            super::super::program::SheetID(0)
        }

        /// Attach a style rule whose selector program is built by `build`.
        fn add_rule(&mut self, build: impl FnOnce(&mut SelectorProgramBuilder)) -> RuleID {
            let mut builder = SelectorProgramBuilder::new();
            build(&mut builder);
            let compiled = self.programs.add(builder.finish());
            let sheet = self.sheet();
            let rule = self.program.append_rule(sheet, None, RuleKind::Style);
            let mut version = self.program.rule_version(rule);
            version.selector_program = Some(compiled);
            self.program.replace_rule_version(rule, version);
            rule
        }

        fn run(&mut self) -> RuleMatches {
            let dispatch = build_scope_dispatch(&self.program, &self.programs, TreeScopeID::DOCUMENT);
            let ancestor_requirements = AncestorRequirements::build(&self.tree, &self.facts, &dispatch);
            let interpreter = BatchMatcher::new(&self.tree, &self.facts, &dispatch, &self.programs, &self.program)
                .with_ancestor_requirements(&ancestor_requirements);
            let mut matches = RuleMatches::new();
            interpreter
                .match_subtree(self.nodes[0], &mut matches, &mut self.counters)
                .expect("the batch covers the whole document");
            matches.settle_memory(&mut self.memory);
            matches
        }

        fn matched_nodes(&self, matches: &RuleMatches, rule: RuleID) -> Vec<usize> {
            matches
                .as_slice()
                .iter()
                .filter(|entry| entry.rule == rule)
                .map(|entry| self.nodes.iter().position(|node| *node == entry.node).unwrap())
                .collect()
        }
    }

    fn class_rule(class: StyleAtomID) -> impl FnOnce(&mut SelectorProgramBuilder) {
        move |builder| {
            let feature = builder.push_feature(FeatureTest::Class(class));
            builder.push_entry(feature);
        }
    }

    #[test]
    fn a_cold_pass_finds_exactly_the_matching_rules() {
        let mut document = Document::new();
        let item = document.add_rule(class_rule(CLASS_ITEM));
        let header = document.add_rule(|builder| {
            let feature = builder.push_feature(FeatureTest::Id(ID_HEADER));
            builder.push_entry(feature);
        });
        let descendant = document.add_rule(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let ancestor = builder.push(SelectorOp::Ancestor(theme));
            let span = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_SPAN)));
            let root = builder.push_compound(&[span, ancestor]);
            builder.push_entry(root);
        });

        let matches = document.run();
        assert_eq!(document.matched_nodes(&matches, item), vec![1, 3]);
        assert_eq!(document.matched_nodes(&matches, header), vec![2]);
        assert_eq!(document.matched_nodes(&matches, descendant), vec![2]);
    }

    #[test]
    fn shared_selector_entries_join_every_rule_after_prefix_matching() {
        let mut document = Document::new();
        let mut builder = SelectorProgramBuilder::new();
        class_rule(CLASS_ITEM)(&mut builder);
        let selector_program = document.programs.add(builder.finish());
        let mut rules = Vec::new();
        for _ in 0..2 {
            let rule = document.program.append_rule(document.sheet(), None, RuleKind::Style);
            let mut version = document.program.rule_version(rule);
            version.selector_program = Some(selector_program);
            document.program.replace_rule_version(rule, version);
            rules.push(rule);
        }

        let matches = document.run();
        assert_eq!(document.matched_nodes(&matches, rules[0]), vec![1, 3]);
        assert_eq!(document.matched_nodes(&matches, rules[1]), vec![1, 3]);
        assert_eq!(document.counters.get(Counter::CandidateChecks), 0);
    }

    #[test]
    fn shared_prefix_identity_emits_only_rows_admitted_to_the_automaton() {
        let mut document = Document::new();
        let user_agent_sheet = document
            .program
            .add_sheet(StyleSheetObjectID(2), CascadeOrigin::UserAgent);
        document.program.attach_sheet(user_agent_sheet, TreeScopeID::DOCUMENT);

        let mut builder = SelectorProgramBuilder::new();
        let any = builder.push_feature(FeatureTest::AnyElement);
        let first_of_type = builder.push(SelectorOp::NthPosition(NthPosition {
            step: 0,
            offset: 1,
            from_end: false,
            of_selector: None,
            of_type: true,
        }));
        let root = builder.push_compound(&[any, first_of_type]);
        builder.push_entry(root);
        let selector_program = document.programs.add(builder.finish());

        let author_sheet = document.sheet();
        let mut add_rule = |sheet| {
            let rule = document.program.append_rule(sheet, None, RuleKind::Style);
            let mut version = document.program.rule_version(rule);
            version.selector_program = Some(selector_program);
            document.program.replace_rule_version(rule, version);
            rule
        };
        let user_agent_rule = add_rule(user_agent_sheet);
        let author_rule = add_rule(author_sheet);

        let matches = document.run();
        assert_eq!(document.matched_nodes(&matches, user_agent_rule), vec![0, 1, 2]);
        assert_eq!(document.matched_nodes(&matches, author_rule), vec![0, 1, 2]);

        let dispatch = build_scope_dispatch(&document.program, &document.programs, TreeScopeID::DOCUMENT);
        let entry = document.programs.entry_id(selector_program, 0);
        let rows: Vec<_> = dispatch.entries_for_identity(entry).collect();
        assert_eq!(rows.len(), 2);
        assert_eq!(rows.iter().filter(|row| row.prefix_matched).count(), 1);

        let mut changes = SelectorTruthChanges::default();
        record_match_set_difference(&mut changes, true, document.nodes[0], &[], &[entry], &dispatch);
        let mut changed_rules: Vec<_> = changes.deltas.as_slice().iter().map(|delta| delta.rule).collect();
        changed_rules.sort_unstable();
        assert_eq!(changed_rules, vec![user_agent_rule, author_rule]);
    }

    #[test]
    fn prefix_states_keep_descendants_but_expire_children() {
        let mut document = Document::new();
        document.tree.set_next_element_sibling(document.nodes[1], None);
        document.tree.set_parent(document.nodes[2], Some(document.nodes[1]));
        document
            .tree
            .set_first_element_child(document.nodes[1], Some(document.nodes[2]));
        document.tree.set_next_element_sibling(document.nodes[2], None);
        document.tree.set_previous_element_sibling(document.nodes[2], None);
        document.tree.set_parent(document.nodes[3], Some(document.nodes[2]));
        document
            .tree
            .set_first_element_child(document.nodes[2], Some(document.nodes[3]));

        let child = document.add_rule(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let parent = builder.push(SelectorOp::Parent(theme));
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let root = builder.push_compound(&[item, parent]);
            builder.push_entry(root);
        });
        let descendant = document.add_rule(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let ancestor = builder.push(SelectorOp::Ancestor(theme));
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let root = builder.push_compound(&[item, ancestor]);
            builder.push_entry(root);
        });

        let matches = document.run();
        assert_eq!(document.matched_nodes(&matches, child), vec![1]);
        assert_eq!(document.matched_nodes(&matches, descendant), vec![1, 3]);
        assert_eq!(document.counters.get(Counter::CandidateChecks), 0);
    }

    #[test]
    fn prefix_states_stop_at_a_shadow_root() {
        let mut document = Document::new();
        let rule = document.add_rule(|builder| {
            let any = builder.push_feature(FeatureTest::AnyElement);
            let parent = builder.push(SelectorOp::Parent(any));
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let root = builder.push_compound(&[item, parent]);
            builder.push_entry(root);
        });

        let dispatch = build_scope_dispatch(&document.program, &document.programs, TreeScopeID::DOCUMENT);
        let interpreter = BatchMatcher::new(
            &document.tree,
            &document.facts,
            &dispatch,
            &document.programs,
            &document.program,
        )
        .in_shadow_tree(document.nodes[0]);
        let mut matches = RuleMatches::new();
        interpreter
            .match_node(document.nodes[1], &mut matches, &mut document.counters)
            .expect("the batch covers the shadow child");
        assert!(document.matched_nodes(&matches, rule).is_empty());
    }

    #[test]
    fn non_prefix_entries_stay_in_the_exact_fallback_dispatch() {
        let mut document = Document::new();
        // An of-selector positional answer evaluates an arbitrary program against every
        // sibling, so the entry stays on the exact match evaluator. Sibling combinators no
        // longer qualify: they register in the unified prefix automaton like the down-axis
        // chains. Node 3 is the second `.item` among its siblings: the span between the items
        // counts only in the plain child sequence.
        let rule = document.add_rule(|builder| {
            let counted = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let second_item = builder.push(SelectorOp::NthPosition(NthPosition {
                step: 0,
                offset: 2,
                from_end: false,
                of_selector: Some(counted),
                of_type: false,
            }));
            let root = builder.push_compound(&[item, second_item]);
            builder.push_entry(root);
        });

        let matches = document.run();
        assert_eq!(document.matched_nodes(&matches, rule), vec![3]);
        assert_ne!(document.counters.get(Counter::CandidateChecks), 0);
    }

    #[test]
    fn plain_positional_entries_match_through_the_prefix_automaton() {
        let mut document = Document::new();
        // A plain an+b answer is a pure function of tree position, carried as a positional
        // truth bit in every transition key, so the entry is admitted and no candidate is ever
        // checked by the exact evaluator.
        let rule = document.add_rule(|builder| {
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let third = builder.push(SelectorOp::NthPosition(NthPosition {
                step: 0,
                offset: 3,
                from_end: false,
                of_selector: None,
                of_type: false,
            }));
            let root = builder.push_compound(&[item, third]);
            builder.push_entry(root);
        });

        let matches = document.run();
        assert_eq!(document.matched_nodes(&matches, rule), vec![3]);
        assert_eq!(document.counters.get(Counter::CandidateChecks), 0);
    }

    #[test]
    fn sibling_entries_match_through_the_prefix_automaton() {
        let mut document = Document::new();
        let rule = document.add_rule(|builder| {
            let header = builder.push_feature(FeatureTest::Id(ID_HEADER));
            let previous = builder.push(SelectorOp::PreviousSibling(header));
            let item = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let root = builder.push_compound(&[item, previous]);
            builder.push_entry(root);
        });

        let matches = document.run();
        assert_eq!(document.matched_nodes(&matches, rule), vec![3]);
        assert_eq!(document.counters.get(Counter::CandidateChecks), 0);
    }

    #[test]
    fn equivalent_prefix_compounds_are_evaluated_once_per_node() {
        let mut document = Document::new();
        let first = document.add_rule(class_rule(CLASS_ITEM));
        let second = document.add_rule(class_rule(CLASS_ITEM));

        let matches = document.run();
        assert_eq!(document.matched_nodes(&matches, first), vec![1, 3]);
        assert_eq!(document.matched_nodes(&matches, second), vec![1, 3]);
        assert_eq!(document.counters.get(Counter::PrefixCompoundsEvaluated), 1);
    }

    #[test]
    fn prefix_transitions_probe_parent_steps_by_local_dispatch_key() {
        let mut document = Document::new();
        let mut item_rule = None;
        for target in 100..164 {
            let target = if target == 100 { CLASS_ITEM } else { StyleAtomID(target) };
            let rule = document.add_rule(|builder| {
                let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
                let ancestor = builder.push(SelectorOp::Ancestor(theme));
                let target = builder.push_feature(FeatureTest::Class(target));
                let root = builder.push_compound(&[target, ancestor]);
                builder.push_entry(root);
            });
            if target == CLASS_ITEM {
                item_rule = Some(rule);
            }
        }

        let matches = document.run();
        assert_eq!(document.matched_nodes(&matches, item_rule.unwrap()), vec![1, 3]);
    }

    #[test]
    fn prefix_continuations_are_shared_independently_of_local_matches() {
        let mut document = Document::new();
        let item = document.add_rule(class_rule(CLASS_ITEM));
        let header = document.add_rule(|builder| {
            let feature = builder.push_feature(FeatureTest::Id(ID_HEADER));
            builder.push_entry(feature);
        });

        let matches = document.run();
        assert_eq!(document.matched_nodes(&matches, item), vec![1, 3]);
        assert_eq!(document.matched_nodes(&matches, header), vec![2]);
    }

    #[test]
    fn prefix_cache_hits_preserve_local_matches() {
        let mut document = Document::new();
        let item = document.add_rule(class_rule(CLASS_ITEM));
        let dispatch = build_scope_dispatch(&document.program, &document.programs, TreeScopeID::DOCUMENT);
        let interpreter = BatchMatcher::new(
            &document.tree,
            &document.facts,
            &dispatch,
            &document.programs,
            &document.program,
        );
        let mut dispatch_workspace = DispatchCandidateWorkspace::default();
        let mut prefix_states = PrefixStates::new(document.facts.row_count());
        let mut matches = RuleMatches::new();
        for _ in 0..2 {
            interpreter
                .match_node_collecting_requests(
                    document.nodes[1],
                    &mut matches,
                    &mut document.counters,
                    BatchMatchState {
                        dispatch_workspace: &mut dispatch_workspace,
                        requests: None,
                        completed: None,
                        prefix_states: Some(&mut prefix_states),
                        deferred_prefix_matches: None,
                    },
                )
                .expect("the node has complete facts");
        }
        assert_eq!(document.matched_nodes(&matches, item), vec![1, 1]);
    }

    #[test]
    fn matches_are_sorted_by_style_node_and_addressable_per_node() {
        let mut document = Document::new();
        document.add_rule(class_rule(CLASS_ITEM));
        document.add_rule(class_rule(CLASS_THEME));
        let universal = document.add_rule(|builder| {
            let any = builder.push_feature(FeatureTest::AnyElement);
            builder.push_entry(any);
        });

        let matches = document.run();
        let nodes: Vec<StyleNodeID> = matches.as_slice().iter().map(|entry| entry.node).collect();
        assert!(nodes.windows(2).all(|pair| pair[0] <= pair[1]));

        let for_second = matches.matches_for(document.nodes[1]);
        assert_eq!(for_second.len(), 2);
        assert!(for_second.iter().any(|entry| entry.rule == universal));
        assert_eq!(matches.matches_for(document.nodes[2]).len(), 1);
    }

    #[test]
    fn a_candidate_only_considers_rules_its_own_features_reach() {
        let mut document = Document::new();
        for class in 100..200_u32 {
            document.add_rule(class_rule(StyleAtomID(class)));
        }
        document.add_rule(class_rule(CLASS_ITEM));

        let matches = document.run();
        assert_eq!(matches.len(), 2);
        // Four nodes, and only the ones carrying .item reach a bucket with anything in it.
        assert!(
            document.counters.get(Counter::CandidateChecks) < 10,
            "a hundred unrelated rules must not be checked per candidate: {}",
            document.counters.get(Counter::CandidateChecks)
        );
    }

    #[test]
    fn a_shared_ancestor_summary_is_built_once_and_released() {
        let mut document = Document::new();
        document.add_rule(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let ancestor = builder.push(SelectorOp::Ancestor(theme));
            let any = builder.push_feature(FeatureTest::AnyElement);
            let root = builder.push_compound(&[any, ancestor]);
            builder.push_entry(root);
        });
        let dispatch = build_scope_dispatch(&document.program, &document.programs, TreeScopeID::DOCUMENT);
        let baseline = document.memory.bytes_in_category(MemoryCategory::BatchScratch);
        let mut cache = AncestorRequirementsCache::default();
        cache.get_or_build(&document.tree, &document.facts, &dispatch, &mut document.memory);
        let after_first = document.memory.bytes_in_category(MemoryCategory::BatchScratch);
        assert!(after_first > baseline);
        cache.get_or_build(&document.tree, &document.facts, &dispatch, &mut document.memory);
        assert_eq!(
            document.memory.bytes_in_category(MemoryCategory::BatchScratch),
            after_first,
            "a second ask for the same dispatch reuses its exact summary"
        );

        cache.release(&mut document.memory);
        assert_eq!(
            document.memory.bytes_in_category(MemoryCategory::BatchScratch),
            baseline
        );
    }

    #[test]
    fn one_node_ancestor_summaries_promote_after_sustained_reuse() {
        let mut document = Document::new();
        document.add_rule(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let ancestor = builder.push(SelectorOp::Ancestor(theme));
            let any = builder.push_feature(FeatureTest::AnyElement);
            let root = builder.push_compound(&[any, ancestor]);
            builder.push_entry(root);
        });
        let dispatch = build_scope_dispatch(&document.program, &document.programs, TreeScopeID::DOCUMENT);
        let baseline = document.memory.bytes_in_category(MemoryCategory::BatchScratch);
        let mut cache = AncestorRequirementsCache::default();
        for _ in 1..ANCESTOR_REQUIREMENTS_PROMOTION_ASKS {
            let requirements = cache.get_or_build_for_node(
                &document.tree,
                &document.facts,
                &dispatch,
                document.nodes[1],
                &mut document.memory,
            );
            assert!(requirements.single_row.is_some());
        }
        assert!(cache.by_topology.is_empty());

        let requirements = cache.get_or_build_for_node(
            &document.tree,
            &document.facts,
            &dispatch,
            document.nodes[1],
            &mut document.memory,
        );
        assert!(requirements.single_row.is_none());
        assert_eq!(cache.by_topology.len(), 1);

        cache.release(&mut document.memory);
        assert_eq!(
            document.memory.bytes_in_category(MemoryCategory::BatchScratch),
            baseline
        );
    }

    #[test]
    fn shadow_scope_ancestor_summaries_remain_local() {
        let mut document = Document::new();
        document.tree.enable_tree_scopes(&mut document.memory);
        document.tree.set_tree_scope(document.nodes[1], TreeScopeID(1));
        document.add_rule(|builder| {
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let ancestor = builder.push(SelectorOp::Ancestor(theme));
            let any = builder.push_feature(FeatureTest::AnyElement);
            let root = builder.push_compound(&[any, ancestor]);
            builder.push_entry(root);
        });
        let dispatch = build_scope_dispatch(&document.program, &document.programs, TreeScopeID::DOCUMENT);
        let mut cache = AncestorRequirementsCache::default();
        for _ in 0..ANCESTOR_REQUIREMENTS_PROMOTION_ASKS * 2 {
            let requirements = cache.get_or_build_for_node(
                &document.tree,
                &document.facts,
                &dispatch,
                document.nodes[1],
                &mut document.memory,
            );
            assert!(requirements.single_row.is_some());
        }
        assert!(cache.by_topology.is_empty());
    }

    #[test]
    fn a_missing_required_ancestor_rejects_before_selector_evaluation() {
        let mut document = Document::new();
        let rule = document.add_rule(|builder| {
            let missing = builder.push_feature(FeatureTest::Class(CLASS_MISSING));
            let ancestor = builder.push(SelectorOp::Ancestor(missing));
            let span = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_SPAN)));
            let root = builder.push_compound(&[span, ancestor]);
            builder.push_entry(root);
        });

        let matches = document.run();
        assert!(document.matched_nodes(&matches, rule).is_empty());
        assert_eq!(document.counters.get(Counter::CandidateChecks), 0);
        assert_eq!(document.counters.get(Counter::CombinatorSteps), 0);
    }

    #[test]
    fn a_required_ancestor_is_found_through_a_parent_compound() {
        let mut document = Document::new();
        let rule = document.add_rule(|builder| {
            let missing = builder.push_feature(FeatureTest::Class(CLASS_MISSING));
            let ancestor = builder.push(SelectorOp::Ancestor(missing));
            let theme = builder.push_feature(FeatureTest::Class(CLASS_THEME));
            let parent_compound = builder.push_compound(&[theme, ancestor]);
            let parent = builder.push(SelectorOp::Parent(parent_compound));
            let span = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_SPAN)));
            let root = builder.push_compound(&[span, parent]);
            builder.push_entry(root);
        });

        let matches = document.run();
        assert!(document.matched_nodes(&matches, rule).is_empty());
        assert_eq!(document.counters.get(Counter::CandidateChecks), 0);
        assert_eq!(document.counters.get(Counter::CombinatorSteps), 0);
    }

    #[test]
    fn a_selector_list_contributes_its_greatest_matching_specificity_once() {
        let mut document = Document::new();
        let rule = document.add_rule(|builder| {
            let class = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            let class_entry = builder.push_entry(class);
            builder.set_entry_specificity(
                class_entry,
                Specificity {
                    classes: 1,
                    ..Specificity::default()
                },
            );
            let tag = builder.push_feature(FeatureTest::TagName(TagTest::exact(TAG_DIV)));
            let tag_entry = builder.push_entry(tag);
            builder.set_entry_specificity(
                tag_entry,
                Specificity {
                    types: 1,
                    ..Specificity::default()
                },
            );
        });

        let matches = document.run();
        let for_item = matches.matches_for(document.nodes[1]);
        assert_eq!(for_item.len(), 1, "one rule matching twice is still one match");
        assert_eq!(for_item[0].rule, rule);
        assert_eq!(
            for_item[0].specificity,
            Specificity {
                classes: 1,
                ..Specificity::default()
            },
            "the greater matching entry wins regardless of declaration order"
        );
    }

    #[test]
    fn a_pseudo_element_rule_matches_the_originating_element_and_projects_onto_the_pseudo() {
        use super::super::tree::PseudoElementKind;

        let mut document = Document::new();
        let before = PseudoElementTarget::new(PseudoElementKind(1));
        let rule = {
            let mut builder = SelectorProgramBuilder::new();
            let class = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push_entry_for_pseudo(class, Some(before));
            let compiled = document.programs.add(builder.finish());
            let sheet = document.sheet();
            let rule = document.program.append_rule(sheet, None, RuleKind::Style);
            let mut version = document.program.rule_version(rule);
            version.selector_program = Some(compiled);
            document.program.replace_rule_version(rule, version);
            rule
        };

        let matches = document.run();
        assert_eq!(document.matched_nodes(&matches, rule), vec![1, 3]);
        assert!(
            matches
                .as_slice()
                .iter()
                .all(|entry| entry.pseudo_element == Some(before)),
            "the match names the originating element and carries the target"
        );
    }

    #[test]
    fn a_rule_matching_an_element_and_its_pseudo_produces_two_results() {
        use super::super::tree::PseudoElementKind;

        let mut document = Document::new();
        let marker = PseudoElementTarget::new(PseudoElementKind(2));
        let rule = {
            let mut builder = SelectorProgramBuilder::new();
            let class = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push_entry(class);
            let same = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
            builder.push_entry_for_pseudo(same, Some(marker));
            let compiled = document.programs.add(builder.finish());
            let sheet = document.sheet();
            let rule = document.program.append_rule(sheet, None, RuleKind::Style);
            let mut version = document.program.rule_version(rule);
            version.selector_program = Some(compiled);
            document.program.replace_rule_version(rule, version);
            rule
        };

        let matches = document.run();
        let for_item = matches.matches_for(document.nodes[1]);
        assert_eq!(for_item.len(), 2, "the element and its pseudo are separate results");
        assert!(for_item.iter().any(|entry| entry.pseudo_element.is_none()));
        assert!(for_item.iter().any(|entry| entry.pseudo_element == Some(marker)));
        assert!(for_item.iter().all(|entry| entry.rule == rule));
    }

    #[test]
    fn a_pseudo_element_target_adds_a_type_component_to_specificity() {
        use super::super::tree::PseudoElementKind;

        let mut builder = SelectorProgramBuilder::new();
        let class = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
        let element_entry = builder.push_entry(class);
        builder.set_entry_specificity(
            element_entry,
            Specificity {
                classes: 1,
                ..Specificity::default()
            },
        );
        let same = builder.push_feature(FeatureTest::Class(CLASS_ITEM));
        let pseudo_entry = builder.push_entry_for_pseudo(same, Some(PseudoElementTarget::new(PseudoElementKind(1))));
        builder.set_entry_specificity(
            pseudo_entry,
            Specificity {
                classes: 1,
                types: 1,
                ..Specificity::default()
            },
        );
        let program = builder.finish();

        assert_eq!(program.entries()[0].specificity.types, 0);
        assert_eq!(program.entries()[1].specificity.types, 1);
        assert_eq!(program.entries()[1].specificity.classes, 1);
    }

    #[test]
    fn a_disabled_sheet_contributes_no_matches() {
        let mut document = Document::new();
        document.add_rule(class_rule(CLASS_ITEM));
        assert_eq!(document.run().len(), 2);

        let sheet = document.sheet();
        document.program.set_sheet_enabled(sheet, false);
        assert!(document.run().is_empty());
    }

    #[test]
    fn a_detached_sheet_contributes_no_matches() {
        let mut document = Document::new();
        document.add_rule(class_rule(CLASS_ITEM));
        assert_eq!(document.run().len(), 2);

        let sheet = document.sheet();
        document.program.detach_sheet(sheet, TreeScopeID::DOCUMENT);
        assert!(document.run().is_empty());
    }

    #[test]
    fn a_non_style_rule_is_not_bucketed_for_element_matching() {
        let mut document = Document::new();
        let sheet = document.sheet();
        let keyframes = document.program.append_rule(sheet, None, RuleKind::Keyframes);
        let mut builder = SelectorProgramBuilder::new();
        let any = builder.push_feature(FeatureTest::AnyElement);
        builder.push_entry(any);
        let compiled = document.programs.add(builder.finish());
        let mut version = document.program.rule_version(keyframes);
        version.selector_program = Some(compiled);
        document.program.replace_rule_version(keyframes, version);

        assert!(
            document.run().is_empty(),
            "a keyframes rule reaches animations, not elements"
        );
    }

    #[test]
    fn an_uncovered_node_aborts_the_pass_without_publishing_partial_output() {
        let mut document = Document::new();
        document.add_rule(class_rule(CLASS_ITEM));
        // Extend the tree past what the fact batch covers.
        let extra = document.tree.allocate_element(&mut document.memory);
        document.tree.set_parent(extra, Some(document.nodes[0]));
        document.tree.set_next_element_sibling(document.nodes[3], Some(extra));
        document
            .tree
            .set_previous_element_sibling(extra, Some(document.nodes[3]));

        let dispatch = build_scope_dispatch(&document.program, &document.programs, TreeScopeID::DOCUMENT);
        let interpreter = BatchMatcher::new(
            &document.tree,
            &document.facts,
            &dispatch,
            &document.programs,
            &document.program,
        );
        let mut matches = RuleMatches::new();
        assert_eq!(
            interpreter.match_subtree(document.nodes[0], &mut matches, &mut document.counters),
            Err(Incomplete::MissingFacts(extra))
        );
        assert!(matches.is_empty(), "an aborted pass publishes nothing");
    }

    #[test]
    fn scratch_is_charged_and_released_around_a_pass() {
        use super::super::memory::Tier;

        let mut document = Document::new();
        document.add_rule(class_rule(CLASS_ITEM));
        let mut matches = document.run();
        assert!(document.memory.bytes_in_tier(Tier::Scratch) > 0);

        matches.release(&mut document.memory);
        assert_eq!(document.memory.bytes_in_tier(Tier::Scratch), 0);
    }

    #[test]
    fn attribute_rules_reach_candidates_through_the_attribute_name() {
        use super::super::selector::AttributeCase;
        use super::super::selector::AttributeOperator;
        use super::super::selector::AttributeTest;

        let mut document = Document::new();
        // Give the header a data attribute.
        let mut facts = StyleNodeFacts::new();
        facts.push_row(
            document.nodes[0],
            TAG_DIV,
            StyleAtomID::NONE,
            StateSet::default(),
            &[CLASS_THEME],
            &[],
        );
        facts.push_row(
            document.nodes[1],
            TAG_DIV,
            StyleAtomID::NONE,
            StateSet::default(),
            &[CLASS_ITEM],
            &[],
        );
        facts.push_row(
            document.nodes[2],
            TAG_SPAN,
            ID_HEADER,
            StateSet::default(),
            &[],
            &[AttributeFact {
                name: StyleAtomID(30),
                value: StyleAtomID(31),
                text_offset: u32::MAX,
                text_length: 0,
            }],
        );
        facts.push_row(
            document.nodes[3],
            TAG_DIV,
            StyleAtomID::NONE,
            StateSet::default(),
            &[CLASS_ITEM],
            &[],
        );
        document.facts = facts;

        let rule = document.add_rule(|builder| {
            let attribute = builder.push_feature(FeatureTest::Attribute(AttributeTest {
                fold_in_namespace: StyleAtomID::NONE,
                folded: StyleAtomID(30),
                any_namespace: false,
                name: StyleAtomID(30),
                operator: AttributeOperator::Exact,
                value_atom: StyleAtomID(31),
                value_offset: 0,
                value_length: 0,
                case: AttributeCase::Sensitive,
            }));
            builder.push_entry(attribute);
        });

        let matches = document.run();
        assert_eq!(document.matched_nodes(&matches, rule), vec![2]);
    }
}
