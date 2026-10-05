/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ops::ControlFlow;

use smallvec::SmallVec;

use super::cascade::{
    CascadeAttachment, CascadeContinuationCeiling, CascadeContinuationID, Top1Winner, highest_candidate_below,
};
use super::*;

#[derive(Clone, Copy)]
enum CascadeCompactionCandidate {
    /// A declaration of the rule at this index of the match list.
    Rule(usize, DeclaredProperty),
    Element(ElementDeclarationKind, DeclaredProperty),
}

type CascadeCompactionTop1 =
    Top1Cascade<(Option<tree::PseudoElementTarget>, u16), CascadePriority, CascadeCompactionCandidate>;

// Element declarations use the overwhelmingly common target and a dense property identity. Keep
// their winner lookup in a property-indexed table; pseudo targets remain in the sparse table above.
struct ElementCascadeCompactionTop1 {
    winners: Vec<Top1Winner<u16, CascadePriority, CascadeCompactionCandidate>>,
    winner_by_property: Vec<u32>,
}

impl ElementCascadeCompactionTop1 {
    fn clear(&mut self) {
        for winner in &self.winners {
            self.winner_by_property[winner.key as usize] = 0;
        }
        self.winners.clear();
    }

    fn consider(&mut self, property: u16, priority: CascadePriority, payload: CascadeCompactionCandidate) {
        let property_index = property as usize;
        if self.winner_by_property.len() <= property_index {
            self.winner_by_property.resize(property_index + 1, 0);
        }
        let winner = self.winner_by_property[property_index];
        if winner == 0 {
            self.winners.push(Top1Winner {
                key: property,
                priority,
                payload,
            });
            self.winner_by_property[property_index] = u32::try_from(self.winners.len())
                .expect("a property winner table cannot exceed the u16 property identity space");
        } else if priority >= self.winners[winner as usize - 1].priority {
            self.winners[winner as usize - 1] = Top1Winner {
                key: property,
                priority,
                payload,
            };
        }
    }

    fn winners(&mut self) -> &[Top1Winner<u16, CascadePriority, CascadeCompactionCandidate>] {
        self.winners.sort_unstable_by_key(|winner| winner.key);
        &self.winners
    }

    fn capacity_bytes(&self) -> u64 {
        (self.winners.capacity() * size_of::<Top1Winner<u16, CascadePriority, CascadeCompactionCandidate>>()
            + self.winner_by_property.capacity() * size_of::<u32>()) as u64
    }
}

/// The winner state a compaction publishes for one of a node's targets.
#[derive(Clone, Copy)]
struct PublishedWinnerState {
    target: Option<tree::PseudoElementTarget>,
    state: CascadeStateID,
    /// Whether a pseudo-element target's state holds a winner for everything its rules declare.
    inventory_is_complete: bool,
}

/// What a match contributes to a compaction that depends on nothing but its match list. A match
/// from the node's own tree reaches the compaction only through its priority, which names
/// everything the tree decides about it, so alike rows of two instances of one shadow tree share
/// a compaction.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct CompactionMatch {
    rule: RuleID,
    pseudo_element: Option<tree::PseudoElementTarget>,
    specificity: Specificity,
    scope_proximity: u32,
    /// The normal priority of a match from the node's own tree; none for a document rule's.
    own_tree_priority: Option<CascadePriority>,
}

/// What compacting one match list decided: the states it published and, unless compaction was
/// blocked, which matches it kept.
struct RememberedCompaction {
    hash: u64,
    matches: Box<[CompactionMatch]>,
    states: SmallVec<[PublishedWinnerState; 2]>,
    keep: Option<Box<[bool]>>,
}

/// Compactions that depend on nothing but their match lists, by list, as decided under one program
/// version and winner group generation. A list is remembered the second time it is compacted: most
/// lists are one element's own, and remembering those would only fill the table.
#[derive(Default)]
struct RememberedCompactions {
    decided_under: Option<(ProgramVersion, u64)>,
    /// The hashes of the lists compacted once since the table was last cleared.
    seen: HashSet<u64>,
    by_matches: hashbrown::HashTable<RememberedCompaction>,
    nested_bytes: usize,
}

impl RememberedCompactions {
    const LIMIT: usize = 1024;

    fn hash_of(matches: &[CompactionMatch]) -> u64 {
        let mut hasher = fast_hash::fast_hasher();
        matches.hash(&mut hasher);
        hasher.finish()
    }

    /// Forget everything decided under another program version or winner group generation.
    fn settle(&mut self, version: ProgramVersion, generation: u64) {
        if self.decided_under != Some((version, generation)) {
            self.seen.clear();
            self.clear();
            self.decided_under = Some((version, generation));
        }
    }

    fn clear(&mut self) {
        self.by_matches.clear();
        self.nested_bytes = 0;
    }

    fn get(&self, hash: u64, matches: &[CompactionMatch]) -> Option<&RememberedCompaction> {
        self.by_matches.find(hash, |remembered| {
            remembered.hash == hash && *remembered.matches == *matches
        })
    }

    /// Whether the list hashing to `hash` was compacted before, noting that it is now.
    fn seen_before(&mut self, hash: u64) -> bool {
        if self.seen.len() >= 4 * Self::LIMIT {
            self.seen.clear();
        }
        !self.seen.insert(hash)
    }

    fn remember(
        &mut self,
        hash: u64,
        matches: &[CompactionMatch],
        states: &[PublishedWinnerState],
        keep: Option<&[bool]>,
    ) {
        if self.by_matches.len() >= Self::LIMIT {
            self.clear();
        }
        let remembered = RememberedCompaction {
            hash,
            matches: matches.into(),
            states: states.into(),
            keep: keep.map(Into::into),
        };
        self.nested_bytes += size_of_val(remembered.matches.as_ref())
            + keep.map_or(0, size_of_val)
            + if remembered.states.spilled() {
                size_of_val(states)
            } else {
                0
            };
        self.by_matches
            .insert_unique(hash, remembered, |remembered| remembered.hash);
    }

    fn capacity_bytes(&self) -> u64 {
        (self.seen.capacity() * size_of::<u64>()
            + self.by_matches.capacity() * size_of::<RememberedCompaction>()
            + self.nested_bytes) as u64
    }
}

pub(super) struct CascadeCompactionWorkspace {
    top_1: CascadeCompactionTop1,
    element_top_1: ElementCascadeCompactionTop1,
    keep: Vec<bool>,
    /// The winners a compaction publishes, every target's in one run, and each target's range of
    /// them.
    published_winners: Vec<PropertyWinner>,
    published_targets: Vec<(Option<tree::PseudoElementTarget>, std::ops::Range<usize>)>,
    published_states: SmallVec<[PublishedWinnerState; 2]>,
    /// The match list of a compaction that depends on nothing but it, as `remembered` keys it.
    remembered_key: Vec<CompactionMatch>,
    remembered: RememberedCompactions,
}

impl Default for CascadeCompactionWorkspace {
    fn default() -> Self {
        Self {
            top_1: CascadeCompactionTop1::with_capacity(0),
            element_top_1: ElementCascadeCompactionTop1 {
                winners: Vec::new(),
                winner_by_property: Vec::new(),
            },
            keep: Vec::new(),
            published_winners: Vec::new(),
            published_targets: Vec::new(),
            published_states: SmallVec::new(),
            remembered_key: Vec::new(),
            remembered: RememberedCompactions::default(),
        }
    }
}

impl CascadeCompactionWorkspace {
    pub(super) fn capacity_bytes(&self) -> u64 {
        self.top_1.capacity_bytes() as u64
            + self.element_top_1.capacity_bytes()
            + (self.keep.capacity() * size_of::<bool>()) as u64
            + (self.published_winners.capacity() * size_of::<PropertyWinner>()) as u64
            + (self.published_targets.capacity()
                * size_of::<(Option<tree::PseudoElementTarget>, std::ops::Range<usize>)>()) as u64
            + if self.published_states.spilled() {
                (self.published_states.capacity() * size_of::<PublishedWinnerState>()) as u64
            } else {
                0
            }
            + (self.remembered_key.capacity() * size_of::<CompactionMatch>()) as u64
            + self.remembered.capacity_bytes()
    }
}

/// Whether a declared property names a longhand column; a shorthand declared whole does not.
fn property_is_longhand(property: u16) -> bool {
    // Style-engine unit tests use small synthetic property IDs to keep their cascade fixtures
    // independent of the generated property table.
    #[cfg(test)]
    if property != crate::css::property_metadata::property_id::CUSTOM
        && property < crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
    {
        return true;
    }
    (crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
        ..=crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID)
        .contains(&property)
}

impl RetainedState {
    /// Order matches the way the cascade applies them, dropping repeats from asking more than one
    /// tree scope when that could have happened.
    pub(super) fn order_matches_in_cascade(&self, all: &mut Vec<RuleMatch>, can_have_scope_duplicates: bool) {
        // One match per rule, target and context the rule is weighed in, not per scope that asked.
        // The user-agent and user origins decide in every scope, so a node asked in two of them - a
        // host, which is asked its own scope's rules and its shadow tree's - meets those rules
        // twice, and both are weighed where their sheet is attached. A sheet several scopes adopt
        // decides in each of them, at each one's own context.
        let identity = |entry: &RuleMatch| {
            (
                entry.node,
                entry.rule,
                entry.pseudo_element.map_or(u32::MAX, |target| u32::from(target.kind.0)),
                self.cascade_context_scope(entry.rule, entry.tree_scope),
            )
        };
        if can_have_scope_duplicates {
            all.sort_unstable_by_key(identity);
            all.dedup_by_key(|entry| identity(entry));
        }

        // Cascade order is meaningful only among declarations for the same element. Sorting the
        // entire document by a key containing it makes the sort compare unrelated elements and
        // retain one large cached key for every match. Keep the outer node order and sort only the
        // contiguous answers that a style computation consumes together. Both callers emit one
        // node at a time, and the duplicate-removal sort above restores that grouping when several
        // tree scopes contributed answers.
        let mut start = 0;
        while start < all.len() {
            let node = all[start].node;
            let end = start + all[start..].partition_point(|entry| entry.node == node);
            let matches = &mut all[start..end];
            if !can_have_scope_duplicates && matches.iter().all(|entry| entry.scope_proximity == u32::MAX) {
                matches.sort_unstable_by_key(|entry| {
                    (
                        entry.cascade_order,
                        entry.rule,
                        entry.pseudo_element.map_or(u32::MAX, |target| u32::from(target.kind.0)),
                    )
                });
            } else {
                matches.sort_by_cached_key(|entry| {
                    self.cascade_priority_of(
                        entry.node,
                        entry.rule,
                        entry.tree_scope,
                        entry.specificity,
                        entry.scope_proximity,
                        false,
                    )
                });
            }
            start = end;
        }
    }

    #[must_use]
    pub(super) fn in_cascade_order(&self, mut all: Vec<RuleMatch>, can_have_scope_duplicates: bool) -> Vec<RuleMatch> {
        self.order_matches_in_cascade(&mut all, can_have_scope_duplicates);
        all
    }

    /// The semantic key of one ordinary declared value.
    #[must_use]
    pub(super) fn retained_rule_winner_key(declared: DeclaredProperty) -> SpecifiedWinnerKey {
        SpecifiedWinnerKey {
            value: declared.value,
            operator: declared.operator,
            continuation: cascade::CascadeContinuationID::default(),
            animation_relevance: 0,
            important: declared.important,
        }
    }

    pub(super) fn cascade_stratum_of(
        &self,
        node: StyleNodeID,
        rule: RuleID,
        tree_scope: TreeScopeID,
        important: bool,
    ) -> CascadeStratum {
        let sheet = self.program.rule_sheet(rule);
        let context_scope = self.cascade_context_scope(rule, tree_scope);
        let layer = self.program.rule_version(rule).layer;
        CascadeStratum::new(
            self.program.sheet_origin(sheet),
            important,
            self.encapsulation_context(node, context_scope),
            layer,
            self.program.layer_rank(context_scope, layer),
            CascadeAttachment::StyleSheet,
        )
    }

    pub(super) fn element_cascade_stratum(
        &self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
        important: bool,
    ) -> CascadeStratum {
        let origin = match kind {
            ElementDeclarationKind::InlineStyle => CascadeOrigin::Author,
            ElementDeclarationKind::PresentationalHint | ElementDeclarationKind::SvgPresentationAttribute => {
                CascadeOrigin::AuthorPresentationalHint
            }
        };
        let tree_scope = self.tree.tree_scope(node);
        CascadeStratum::new(
            origin,
            important,
            self.tree_scope_depth(tree_scope),
            CascadeLayerID::UNLAYERED,
            self.program.layer_rank(tree_scope, CascadeLayerID::UNLAYERED),
            if kind == ElementDeclarationKind::InlineStyle {
                CascadeAttachment::InlineStyle
            } else {
                CascadeAttachment::StyleSheet
            },
        )
    }

    /// What one property of the node's cascade falls back to once substitutions turned some of its
    /// declarations into `revert` or `revert-layer`. `reverted` names each such declaration by its
    /// priority, with the keyword it substituted to. The property's candidates are gathered again
    /// from the node's match answer, which keeps every declaration beaten by a winner whose
    /// substitution may come out as a revert keyword (see
    /// `compact_matches_for_cascade_with_scratch`), and from its own declarations, as the
    /// cascade gathers them, and the cascade is resolved again with each reverted declaration
    /// acting as its keyword. `Ok(None)` leaves the property undeclared. `Err` when the candidates are not
    /// the ones the node's winners were reduced from: they miss a declaration `reverted` names.
    pub(super) fn winner_below_substitution(
        &self,
        node: StyleNodeID,
        pseudo_kind: Option<u8>,
        property: u16,
        reverted: &[(CascadePriority, CascadeOperator)],
    ) -> Result<Option<PropertyWinner>, ()> {
        let mut candidates = Vec::new();
        let wants = |declared: u16| declared == property;
        let answered =
            self.try_for_each_answer_match(node, pseudo_kind, |rule, tree_scope, specificity, scope_proximity| {
                // A gated rule is a candidate where its conditions held for the target as the
                // node's winners were published.
                if self.published_container_verdict_holds(node, rule, pseudo_kind.is_some()) {
                    self.push_rule_cascade_candidates(
                        node,
                        rule,
                        tree_scope,
                        specificity,
                        scope_proximity,
                        &wants,
                        &mut candidates,
                    );
                }
                ControlFlow::Continue(())
            });
        if answered.is_none() {
            return Err(());
        }
        if pseudo_kind.is_none() {
            self.push_element_cascade_candidates(node, &wants, &mut candidates);
        }
        candidates.sort_unstable_by_key(|candidate| candidate.winner.priority);
        // As `WinnerGroups::resolve_candidates` resolves, each revert keyword setting a ceiling the
        // candidates below it have to be under.
        let mut ceilings = SmallVec::<[CascadeContinuationCeiling; 2]>::new();
        let mut end = candidates.len();
        let mut reverted_reached = 0;
        let winner = loop {
            let Some(index) = highest_candidate_below(&candidates[..end], &ceilings) else {
                break None;
            };
            let candidate = candidates[index];
            let operator = match reverted
                .iter()
                .find(|(priority, _)| *priority == candidate.winner.priority)
            {
                Some(&(_, operator)) => {
                    reverted_reached += 1;
                    operator
                }
                None => candidate.winner.key.operator,
            };
            let Some(ceiling) = candidate.stratum.ceiling(operator) else {
                break Some(candidate.winner);
            };
            ceilings.push(ceiling);
            end = index;
        };
        if reverted_reached != reverted.len() {
            return Err(());
        }
        Ok(winner)
    }

    pub(super) fn resolved_cascade_winners_for_properties(
        &mut self,
        node: StyleNodeID,
        matches: &[RuleMatch],
        pseudo: Option<tree::PseudoElementTarget>,
        properties: Option<&[u16]>,
    ) -> Vec<PropertyWinner> {
        self.resolved_cascade_winners_for_properties_with_scratch(node, matches, pseudo, properties, &mut Vec::new())
    }

    fn resolved_cascade_winners_for_properties_with_scratch(
        &mut self,
        node: StyleNodeID,
        matches: &[RuleMatch],
        pseudo: Option<tree::PseudoElementTarget>,
        properties: Option<&[u16]>,
        candidates: &mut Vec<OrderedCascadeCandidate>,
    ) -> Vec<PropertyWinner> {
        let wants = |property: u16| properties.is_none_or(|properties| properties.binary_search(&property).is_ok());
        candidates.clear();
        // A gated rule is a candidate where its container conditions hold for the target now.
        let conditions_hold = |rule: RuleID| {
            !self.program.rule_is_gated_by_container_query(rule)
                || self
                    .rule_container_verdict(rule, node, pseudo.is_some())
                    .is_some_and(|verdict| verdict.matches)
        };
        for entry in matches
            .iter()
            .filter(|entry| entry.pseudo_element == pseudo && conditions_hold(entry.rule))
        {
            self.push_rule_cascade_candidates(
                node,
                entry.rule,
                entry.tree_scope,
                entry.specificity,
                entry.scope_proximity,
                &wants,
                candidates,
            );
        }
        if pseudo.is_none() {
            self.push_element_cascade_candidates(node, &wants, candidates);
        }
        candidates.sort_unstable_by_key(|candidate| candidate.winner.property);
        let mut winners = Vec::new();
        let mut start = 0;
        while start < candidates.len() {
            let property = candidates[start].winner.property;
            let end = start + candidates[start..].partition_point(|candidate| candidate.winner.property == property);
            if let Some(winner) = self.winner_groups.resolve_candidates(&mut candidates[start..end]) {
                winners.push(winner);
            }
            start = end;
        }
        winners
    }

    /// Push the cascade candidates a matched rule declares for the longhands `wants` names. A
    /// shorthand written with a substitution is declared whole beside the longhands it pends; the
    /// longhands are the candidates, the shorthand names no column.
    #[allow(clippy::too_many_arguments)]
    fn push_rule_cascade_candidates(
        &self,
        node: StyleNodeID,
        rule: RuleID,
        tree_scope: TreeScopeID,
        specificity: Specificity,
        scope_proximity: u32,
        wants: &impl Fn(u16) -> bool,
        candidates: &mut Vec<OrderedCascadeCandidate>,
    ) {
        let mut priority_and_stratum_by_importance = [None; 2];
        for &declared in self.program.declared_properties_of(rule) {
            if !wants(declared.property) || !property_is_longhand(declared.property) {
                continue;
            }
            let (priority, stratum) = *priority_and_stratum_by_importance[declared.important as usize]
                .get_or_insert_with(|| {
                    (
                        self.cascade_priority_of(
                            node,
                            rule,
                            tree_scope,
                            specificity,
                            scope_proximity,
                            declared.important,
                        ),
                        self.cascade_stratum_of(node, rule, tree_scope, declared.important),
                    )
                });
            candidates.push(OrderedCascadeCandidate {
                winner: PropertyWinner {
                    property: declared.property,
                    important: declared.important,
                    key: Self::retained_rule_winner_key(declared),
                    priority,
                    source: WinnerSource::Rule(rule),
                },
                stratum,
            });
        }
    }

    /// Push the cascade candidates a node's own declarations hold for the longhands `wants` names.
    fn push_element_cascade_candidates(
        &self,
        node: StyleNodeID,
        wants: &impl Fn(u16) -> bool,
        candidates: &mut Vec<OrderedCascadeCandidate>,
    ) {
        for kind in ElementDeclarationKind::ALL {
            // Custom properties an inline style declares beside its longhands leave those
            // longhands as complete as any; the environment they decide is computed apart.
            let declared_properties = self.facts.element_declared_properties(node, kind);
            let mut priority_and_stratum_by_importance = [None; 2];
            for &declared in declared_properties {
                if !wants(declared.property) || !property_is_longhand(declared.property) {
                    continue;
                }
                let (priority, stratum) = *priority_and_stratum_by_importance[declared.important as usize]
                    .get_or_insert_with(|| {
                        (
                            self.element_cascade_priority(node, kind, declared.important),
                            self.element_cascade_stratum(node, kind, declared.important),
                        )
                    });
                candidates.push(OrderedCascadeCandidate {
                    winner: PropertyWinner {
                        property: declared.property,
                        important: declared.important,
                        key: Self::retained_rule_winner_key(declared),
                        priority,
                        source: WinnerSource::Element(kind),
                    },
                    stratum,
                });
            }
        }
    }

    pub(super) fn intern_cascade_state(
        &mut self,
        winners: &[PropertyWinner],
        previous: Option<CascadeStateID>,
    ) -> CascadeStateID {
        self.with_cascade_interning_counters(|groups| groups.intern_sorted(winners, previous))
    }

    pub(super) fn with_cascade_interning_counters<T>(&mut self, intern: impl FnOnce(&mut WinnerGroups) -> T) -> T {
        let previous_state_count = self.winner_groups.state_count();
        let previous_group_count = self.winner_groups.payload_count();
        let previous_winner_entry_count = self.winner_groups.winner_entry_count();
        let result = intern(&mut self.winner_groups);
        self.counters.add(
            Counter::CascadeStatesInterned,
            (self.winner_groups.state_count() - previous_state_count) as u64,
        );
        self.counters.add(
            Counter::CascadeWinnerGroupsInterned,
            (self.winner_groups.payload_count() - previous_group_count) as u64,
        );
        self.counters.add(
            Counter::CascadeWinnerEntriesInterned,
            (self.winner_groups.winner_entry_count() - previous_winner_entry_count) as u64,
        );
        result
    }

    /// Keep only rules that can supply a winning declaration to the cascade.
    ///
    /// Matching has already resolved selector specificity, tree-scoped encapsulation, layers and
    /// source order. A rule that loses every longhand it declares cannot affect the cascade, so it
    /// need not cross the language boundary or be visited again by property cascading. Rules whose
    /// declaration inventory is not exact, container-gated rules and non-document scopes
    /// conservatively keep the full answer. Non-author rules are retained verbatim so compaction
    /// does not change how the consumer resolves cascade origins. One otherwise-empty match per
    /// pseudo target is retained because the originating element uses its presence to decide which
    /// synthetic pseudo-elements to materialize.
    pub(super) fn compact_matches_for_cascade(
        &mut self,
        effects: &mut AnswerEffects,
        all: &mut Vec<RuleMatch>,
        can_have_scope_duplicates: bool,
        publish_winners_for: Option<StyleNodeID>,
    ) {
        let mut workspace = std::mem::take(&mut self.cascade_compaction_scratch);
        self.compact_matches_for_cascade_with_scratch(
            effects,
            all,
            can_have_scope_duplicates,
            publish_winners_for,
            &mut workspace,
        );
        self.cascade_compaction_scratch_memory
            .resize_required_to(&mut self.memory, workspace.capacity_bytes());
        self.cascade_compaction_scratch = workspace;
    }

    pub(super) fn compact_matches_for_cascade_with_scratch(
        &mut self,
        effects: &mut AnswerEffects,
        all: &mut Vec<RuleMatch>,
        can_have_scope_duplicates: bool,
        mut publish_winners_for: Option<StyleNodeID>,
        workspace: &mut CascadeCompactionWorkspace,
    ) {
        if publish_winners_for.is_some_and(|node| {
            !self.winner_groups.admits_new_rows()
                && matches!(
                    effects
                        .winners
                        .view(&self.winner_groups)
                        .lookup(WinnerGroupKey::current(node, self.program.version())),
                    Lookup::Missing(_)
                )
        }) {
            publish_winners_for = None;
        }
        self.order_matches_in_cascade(all, can_have_scope_duplicates);
        self.counters
            .add(Counter::CascadeMatchesBeforeCompaction, all.len() as u64);
        if let Some(node) = publish_winners_for
            && all
                .iter()
                .any(|entry| self.program.rule_is_gated_by_container_query(entry.rule))
        {
            self.note_container_gates_for_publication(node, effects);
        }
        let compaction_blocked = !self.cascade_winner_inventory_is_complete(all, publish_winners_for);
        let has_continuations = all.iter().any(|entry| {
            self.program.declared_properties_of(entry.rule).iter().any(|declared| {
                matches!(
                    declared.operator,
                    CascadeOperator::Revert | CascadeOperator::RevertLayer
                )
            })
        }) || publish_winners_for.is_some_and(|node| {
            ElementDeclarationKind::ALL.iter().any(|&kind| {
                self.facts
                    .element_declared_properties(node, kind)
                    .iter()
                    .any(|declared| {
                        matches!(
                            declared.operator,
                            CascadeOperator::Revert | CascadeOperator::RevertLayer
                        )
                    })
            })
        });
        if has_continuations && publish_winners_for.is_none() {
            return;
        }
        if compaction_blocked && publish_winners_for.is_none() {
            return;
        }

        // A compaction that depends on nothing but its match list is decided once per list: rows
        // built from one template repeat their lists, and each publishes what the first decided.
        let mut remember_as = None;
        if let Some(node) = publish_winners_for
            && !has_continuations
            && self.compaction_depends_only_on_matches(effects, node, all)
        {
            let key = &mut workspace.remembered_key;
            key.clear();
            key.extend(all.iter().map(|entry| CompactionMatch {
                rule: entry.rule,
                pseudo_element: entry.pseudo_element,
                specificity: entry.specificity,
                scope_proximity: entry.scope_proximity,
                own_tree_priority: (entry.tree_scope != TreeScopeID::DOCUMENT).then(|| {
                    self.cascade_priority_of(
                        entry.node,
                        entry.rule,
                        entry.tree_scope,
                        entry.specificity,
                        entry.scope_proximity,
                        false,
                    )
                }),
            }));
            let remembered = &mut workspace.remembered;
            remembered.settle(self.program.version(), self.winner_groups.generation());
            let hash = RememberedCompactions::hash_of(key);
            if let Some(remembered) = remembered.get(hash, key) {
                self.publish_winner_states(effects, node, &remembered.states);
                if let Some(keep) = &remembered.keep {
                    // The key holds every match, so the remembered decisions line up with the list; were one missing,
                    // keeping its match only keeps a rule the cascade finds losing again.
                    debug_assert_eq!(keep.len(), all.len());
                    let mut keep = keep.iter();
                    all.retain(|_| keep.next().copied().unwrap_or(true));
                }
                return;
            }
            if remembered.seen_before(hash) {
                remember_as = Some(hash);
            }
        }

        // The number of declaration candidates can be orders of magnitude larger than the number
        // of semantic outputs: every rule may declare the same properties, while this reduction
        // stores only one winner per pseudo/property pair. Let the table grow with distinct keys
        // instead of reserving for every losing declaration.
        let top_1 = &mut workspace.top_1;
        top_1.clear();
        let element_top_1 = &mut workspace.element_top_1;
        element_top_1.clear();
        // A gated rule's conditions are decided for the node over its containers as they stand:
        // the rule is a candidate only where they hold, and the verdicts are kept with the winners,
        // which the record loop checks again once the node's ancestors have settled. One the engine
        // cannot decide is kept as undecided, not as failed: it leaves the node to the host.
        let mut container_verdicts = Vec::new();
        for (match_index, entry) in all.iter().enumerate() {
            if compaction_blocked && !self.container_gate_is_held(publish_winners_for, entry.rule) {
                continue;
            }
            if self.program.rule_is_gated_by_container_query(entry.rule)
                && let Some(node) = publish_winners_for
            {
                let pseudo = entry.pseudo_element.is_some();
                let held = self
                    .rule_container_verdict(entry.rule, node, pseudo)
                    .map(|verdict| verdict.matches);
                container_verdicts.push(PublishedContainerVerdict {
                    rule: entry.rule,
                    pseudo,
                    held,
                });
                if held != Some(true) {
                    continue;
                }
            }
            let declared_properties = self.program.declared_properties_of(entry.rule);
            let mut priorities = [None; 2];
            for declared in declared_properties {
                if !property_is_longhand(declared.property) {
                    continue;
                }
                let priority = *priorities[declared.important as usize].get_or_insert_with(|| {
                    self.cascade_priority_of(
                        entry.node,
                        entry.rule,
                        entry.tree_scope,
                        entry.specificity,
                        entry.scope_proximity,
                        declared.important,
                    )
                });
                match entry.pseudo_element {
                    Some(_) => top_1.consider(
                        (entry.pseudo_element, declared.property),
                        priority,
                        CascadeCompactionCandidate::Rule(match_index, *declared),
                    ),
                    None => element_top_1.consider(
                        declared.property,
                        priority,
                        CascadeCompactionCandidate::Rule(match_index, *declared),
                    ),
                }
            }
        }
        if let Some(node) = publish_winners_for {
            self.publish_container_verdicts(node, container_verdicts);
            for kind in ElementDeclarationKind::ALL {
                let declared_properties = self.facts.element_declared_properties(node, kind);
                let mut priorities = [None; 2];
                for &declared in declared_properties {
                    if !property_is_longhand(declared.property) {
                        continue;
                    }
                    let priority = *priorities[declared.important as usize]
                        .get_or_insert_with(|| self.element_cascade_priority(node, kind, declared.important));
                    element_top_1.consider(
                        declared.property,
                        priority,
                        CascadeCompactionCandidate::Element(kind, declared),
                    );
                }
            }
        }
        let mut scratch_bytes = 0;

        let published_winners = &mut workspace.published_winners;
        published_winners.clear();
        let published_targets = &mut workspace.published_targets;
        published_targets.clear();
        if let Some(node) = publish_winners_for {
            let mut targets: SmallVec<[Option<tree::PseudoElementTarget>; 6]> = SmallVec::new();
            targets.push(None);
            for target in all.iter().filter_map(|entry| entry.pseudo_element) {
                if !targets.contains(&Some(target)) {
                    targets.push(Some(target));
                }
            }
            // A target which lost its final matching rule still needs an empty winner state.
            // Otherwise its previous sparse row would remain current and could incorrectly prove
            // that the now-absent pseudo does not need recomputation.
            for (target, _, _, _) in effects.winners.view(&self.winner_groups).pseudo_states(node) {
                if !targets.contains(&Some(target)) {
                    targets.push(Some(target));
                }
            }
            for target in targets {
                let start = published_winners.len();
                if has_continuations {
                    published_winners.extend(self.resolved_cascade_winners_for_properties(node, all, target, None));
                } else {
                    let materialize = |priority, payload| {
                        let (declared, source) = match payload {
                            CascadeCompactionCandidate::Rule(match_index, declared) => {
                                (declared, WinnerSource::Rule(all[match_index].rule))
                            }
                            CascadeCompactionCandidate::Element(kind, declared) => {
                                (declared, WinnerSource::Element(kind))
                            }
                        };
                        PropertyWinner {
                            property: declared.property,
                            important: declared.important,
                            key: Self::retained_rule_winner_key(declared),
                            priority,
                            source,
                        }
                    };
                    match target {
                        None => published_winners.extend(
                            element_top_1
                                .winners()
                                .iter()
                                .map(|winner| materialize(winner.priority, winner.payload)),
                        ),
                        Some(_) => published_winners.extend(
                            top_1
                                .winners()
                                .filter(|winner| winner.key.0 == target)
                                .map(|winner| materialize(winner.priority, winner.payload)),
                        ),
                    }
                }
                published_targets.push((target, start..published_winners.len()));
            }
        }

        if let Some(node) = publish_winners_for {
            let winner_count = published_winners.len();
            let winner_scratch_bytes = (winner_count * size_of::<PropertyWinner>()) as u64;
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, winner_scratch_bytes);
            let published_states = &mut workspace.published_states;
            published_states.clear();
            for (target, range) in published_targets.iter() {
                let winners = &published_winners[range.clone()];
                let key = target.map_or_else(
                    || WinnerGroupKey::current(node, self.program.version()),
                    |target| WinnerGroupKey::current_pseudo(node, target, self.program.version()),
                );
                let previous = effects
                    .winners
                    .view(&self.winner_groups)
                    .token_for(key)
                    .sparse()
                    .ok()
                    .map(|(_, state)| state);
                // A pseudo-element whose rules declare custom properties past their longhand
                // winners holds them in its state: the state then says all its rules declare.
                let pseudo_custom_declarations = target
                    .filter(|&target| {
                        !self.cascade_winner_inventory_is_complete_for_target(all, Some(node), Some(target))
                            && self.cascade_winner_inventory_is_complete_but_for_custom_properties_for_target(
                                node, all, target,
                            )
                    })
                    .and_then(|target| self.cascaded_pseudo_custom_declarations_in(node, all, target));
                let state = match &pseudo_custom_declarations {
                    Some(custom_declarations) => self.with_cascade_interning_counters(|groups| {
                        groups.intern_sorted_with_custom_declarations(winners, custom_declarations, previous)
                    }),
                    None => self.intern_cascade_state(winners, previous),
                };
                published_states.push(PublishedWinnerState {
                    target: *target,
                    state,
                    inventory_is_complete: target.is_none_or(|target| {
                        pseudo_custom_declarations.is_some()
                            || self.cascade_winner_inventory_is_complete_for_target(all, Some(node), Some(target))
                    }),
                });
            }
            self.publish_winner_states(effects, node, published_states);
            self.memory.release(MemoryCategory::BatchScratch, winner_scratch_bytes);
        }

        if compaction_blocked {
            if let Some(hash) = remember_as {
                workspace
                    .remembered
                    .remember(hash, &workspace.remembered_key, &workspace.published_states, None);
            }
            self.memory.release(MemoryCategory::BatchScratch, scratch_bytes);
            return;
        }

        let keep = &mut workspace.keep;
        keep.clear();
        keep.extend(all.iter().map(|entry| self.compaction_keeps_verbatim(entry.rule)));
        for winner in top_1.unordered_winners() {
            if let CascadeCompactionCandidate::Rule(match_index, _) = winner.payload {
                keep[match_index] = true;
            }
        }
        for winner in &element_top_1.winners {
            if let CascadeCompactionCandidate::Rule(match_index, _) = winner.payload {
                keep[match_index] = true;
            }
        }
        // A winner resolved through continuations names its rule but not the match it came from, so
        // the matches it needs are found by scanning. The scan stays inside the winner's own target:
        // a rule winning for one pseudo element says nothing about that rule's matches against
        // another target, and retaining those would keep matches that reduction from winner states
        // already drops. Every other published winner came from a top-1 candidate kept above.
        if has_continuations {
            for (target, range) in published_targets.iter() {
                for &winner in &published_winners[range.clone()] {
                    let mut current = Some(winner);
                    while let Some(winner) = current {
                        if let WinnerSource::Rule(rule) = winner.source {
                            for (index, entry) in all.iter().enumerate() {
                                if entry.rule == rule && entry.pseudo_element == *target {
                                    keep[index] = true;
                                }
                            }
                        }
                        current = self
                            .winner_groups
                            .continuation(winner.key.continuation)
                            .and_then(|continuation| continuation.winner);
                    }
                }
            }
        }

        // A winner written with a substitution that may substitute a `revert` or `revert-layer`
        // rolls its property back to the declarations it beat. A record computation gathers those
        // from the answer again (`winner_below_substitution`), so a match declaring such a
        // property is kept whatever else it loses.
        if keep.iter().any(|kept| !kept) {
            let may_substitute_revert = |value| match self.specified_values.value(value) {
                Lookup::Known(value) => custom_property_cascade::value_may_substitute_revert(value),
                _ => false,
            };
            let mut substituted: SmallVec<[(Option<tree::PseudoElementTarget>, u16); 4]> = SmallVec::new();
            let top_1_winners = top_1.unordered_winners().map(|winner| (winner.key, winner.payload));
            let element_winners = element_top_1
                .winners
                .iter()
                .map(|winner| ((None, winner.key), winner.payload));
            for (key, payload) in top_1_winners.chain(element_winners) {
                let (CascadeCompactionCandidate::Rule(_, declared) | CascadeCompactionCandidate::Element(_, declared)) =
                    payload;
                if may_substitute_revert(declared.value) {
                    substituted.push(key);
                }
            }
            // A winner a continuation reached is not a top candidate.
            if has_continuations {
                for (target, range) in published_targets.iter() {
                    for &winner in &published_winners[range.clone()] {
                        if let Some(winner) = self.winner_groups.resolved_winner(winner)
                            && may_substitute_revert(winner.key.value)
                        {
                            substituted.push((*target, winner.property));
                        }
                    }
                }
            }
            if !substituted.is_empty() {
                for (index, entry) in all.iter().enumerate() {
                    keep[index] |= self
                        .program
                        .declared_properties_of(entry.rule)
                        .iter()
                        .any(|declared| substituted.contains(&(entry.pseudo_element, declared.property)));
                }
            }
        }

        let mut retained_pseudo_targets: SmallVec<[tree::PseudoElementTarget; 4]> = SmallVec::new();
        for (index, entry) in all.iter().enumerate() {
            if keep[index]
                && let Some(target) = entry.pseudo_element
                && !retained_pseudo_targets.contains(&target)
            {
                retained_pseudo_targets.push(target);
            }
        }
        for (index, entry) in all.iter().enumerate() {
            if let Some(target) = entry.pseudo_element
                && !retained_pseudo_targets.contains(&target)
            {
                keep[index] = true;
                retained_pseudo_targets.push(target);
            }
        }

        let pseudo_target_scratch_bytes = if retained_pseudo_targets.spilled() {
            retained_pseudo_targets.capacity() * size_of::<tree::PseudoElementTarget>()
        } else {
            0
        };
        let actual_scratch_bytes = pseudo_target_scratch_bytes as u64;
        if actual_scratch_bytes > scratch_bytes {
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, actual_scratch_bytes - scratch_bytes);
        } else {
            self.memory
                .release(MemoryCategory::BatchScratch, scratch_bytes - actual_scratch_bytes);
        }
        scratch_bytes = actual_scratch_bytes;

        if let Some(hash) = remember_as {
            workspace
                .remembered
                .remember(hash, &workspace.remembered_key, &workspace.published_states, Some(keep));
        }
        let mut index = 0;
        all.retain(|_| {
            let retained = keep[index];
            index += 1;
            retained
        });
        self.memory.release(MemoryCategory::BatchScratch, scratch_bytes);
    }

    /// Whether compacting `all` for `node` decides what it would for any other node with the same
    /// matches: every match decides in the document or in the node's own tree, where its priority
    /// depends on that tree alone and not on where the node is assigned or what it hosts, none
    /// waits on a container, and the node has neither declarations of its own nor pseudo-element
    /// rows its compaction would have to empty.
    fn compaction_depends_only_on_matches(
        &self,
        effects: &AnswerEffects,
        node: StyleNodeID,
        all: &[RuleMatch],
    ) -> bool {
        let tree_scope = self.tree.tree_scope(node);
        all.iter().all(|entry| {
            (entry.tree_scope == TreeScopeID::DOCUMENT || entry.tree_scope == tree_scope)
                && !self.program.rule_is_gated_by_container_query(entry.rule)
        }) && !self.node_has_element_declaration_input(node)
            && effects
                .winners
                .view(&self.winner_groups)
                .pseudo_states(node)
                .next()
                .is_none()
    }

    fn publish_winner_states(
        &mut self,
        effects: &mut AnswerEffects,
        node: StyleNodeID,
        states: &[PublishedWinnerState],
    ) {
        let mut published_row_count = 0;
        for &PublishedWinnerState {
            target,
            state,
            inventory_is_complete,
        } in states
        {
            if let Some(target) = target {
                let published = effects.winners.set_pseudo(
                    &mut self.winner_groups,
                    node,
                    target,
                    state,
                    self.program.version(),
                    &mut self.memory,
                );
                if published && !inventory_is_complete {
                    effects.winners.mark_pseudo_inventory_incomplete(node, target);
                }
                published_row_count += usize::from(published);
            } else {
                published_row_count += usize::from(effects.winners.set(
                    &mut self.winner_groups,
                    node,
                    state,
                    self.program.version(),
                    &mut self.memory,
                ));
            }
        }
        self.winner_groups.settle_memory(&mut self.memory);
        self.counters
            .add(Counter::CascadeNodeHandlesPublished, published_row_count as u64);
    }

    /// Whether a winner of the state may substitute a revert keyword, which rolls its property back
    /// to the declarations it beat: the general compaction keeps those, which the winners do not
    /// name.
    fn state_may_substitute_revert(&self, state: CascadeStateID) -> bool {
        self.winner_groups.winners_in_state(state).any(|winner| {
            matches!(
                self.specified_values.value(winner.key.value),
                Lookup::Known(value) if custom_property_cascade::value_may_substitute_revert(value)
            )
        })
    }

    /// Whether compacting a cascade input keeps a rule's matches whatever the winners say: a
    /// non-author match keeps the origin the consumer resolves, and a container-gated one is
    /// decided again when its containers move, where a rule that loses now may win.
    pub(super) fn compaction_keeps_verbatim(&self, rule: RuleID) -> bool {
        self.program.sheet_origin(self.program.rule_sheet(rule)) != CascadeOrigin::Author
            || self.program.rule_is_gated_by_container_query(rule)
    }

    /// Reuse a complete, freshly updated element winner state instead of reducing declarations
    /// again. Cascade continuations retain the general compaction path.
    pub(super) fn compact_matches_from_updated_winners(
        &mut self,
        effects: &mut AnswerEffects,
        node: StyleNodeID,
        all: &mut Vec<RuleMatch>,
    ) -> bool {
        let mut has_author_pseudo_rules = false;
        if self.node_has_element_declaration_input(node)
            || all.iter().any(|entry| {
                has_author_pseudo_rules |= entry.pseudo_element.is_some()
                    && self.program.sheet_origin(self.program.rule_sheet(entry.rule)) == CascadeOrigin::Author;
                self.program.declared_properties_of(entry.rule).iter().any(|declared| {
                    matches!(
                        declared.operator,
                        CascadeOperator::Revert | CascadeOperator::RevertLayer
                    )
                })
            })
        {
            return false;
        }
        if has_author_pseudo_rules {
            return self.compact_pseudo_matches_from_updated_winners(effects, node, all);
        }
        let Some((_, state)) = effects
            .winners
            .view(&self.winner_groups)
            .token_for(WinnerGroupKey::current(node, self.program.version()))
            .sparse()
            .ok()
        else {
            return false;
        };
        if self.state_may_substitute_revert(state) {
            return false;
        }
        let Some(rules) = self.winner_groups.rules_for_compaction(state) else {
            return false;
        };
        self.counters
            .add(Counter::CascadeMatchesBeforeCompaction, all.len() as u64);
        all.retain(|entry| self.compaction_keeps_verbatim(entry.rule) || rules.binary_search(&entry.rule).is_ok());
        verify_style_answer_patch(self, |verifier| {
            verifier.verify_cascade_answer(all, node, "compaction from updated winners");
        });
        true
    }

    fn compact_pseudo_matches_from_updated_winners(
        &mut self,
        effects: &mut AnswerEffects,
        node: StyleNodeID,
        all: &mut Vec<RuleMatch>,
    ) -> bool {
        let Some((_, state)) = effects
            .winners
            .view(&self.winner_groups)
            .token_for(WinnerGroupKey::current(node, self.program.version()))
            .sparse()
            .ok()
        else {
            return false;
        };
        if self.state_may_substitute_revert(state) {
            return false;
        }
        let Some(element_rules) = self.winner_groups.rules_for_compaction(state) else {
            return false;
        };
        type PseudoCompactionEntry<'a> = (tree::PseudoElementTarget, &'a [RuleID], Option<usize>);
        let mut pseudo_rules: SmallVec<[PseudoCompactionEntry<'_>; 4]> = SmallVec::new();
        for entry in all.iter() {
            let Some(pseudo) = entry.pseudo_element else {
                continue;
            };
            if self.program.sheet_origin(self.program.rule_sheet(entry.rule)) != CascadeOrigin::Author
                || pseudo_rules.iter().any(|(target, _, _)| *target == pseudo)
            {
                continue;
            }
            let Some((_, state)) = effects
                .winners
                .view(&self.winner_groups)
                .token_for(WinnerGroupKey::current_pseudo(node, pseudo, self.program.version()))
                .sparse()
                .ok()
            else {
                return false;
            };
            if self.state_may_substitute_revert(state) {
                return false;
            }
            let Some(rules) = self.winner_groups.rules_for_compaction(state) else {
                return false;
            };
            // A target with no winning declarations still needs one match to preserve its
            // presence. A match already retained verbatim serves that purpose.
            let marker = if rules.is_empty()
                && !all
                    .iter()
                    .any(|entry| entry.pseudo_element == Some(pseudo) && self.compaction_keeps_verbatim(entry.rule))
            {
                all.iter().position(|entry| entry.pseudo_element == Some(pseudo))
            } else {
                None
            };
            pseudo_rules.push((pseudo, rules, marker));
        }
        let scratch_bytes = if pseudo_rules.spilled() {
            (pseudo_rules.capacity() * size_of::<PseudoCompactionEntry<'_>>()) as u64
        } else {
            0
        };
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, scratch_bytes);
        self.counters
            .add(Counter::CascadeMatchesBeforeCompaction, all.len() as u64);
        let mut index = 0;
        all.retain(|entry| {
            let retained = self.compaction_keeps_verbatim(entry.rule)
                || match entry.pseudo_element {
                    None => element_rules.binary_search(&entry.rule).is_ok(),
                    Some(pseudo) => {
                        let (_, rules, marker) = pseudo_rules
                            .iter()
                            .find(|(target, _, _)| *target == pseudo)
                            .expect("every author pseudo target has a winner set");
                        *marker == Some(index) || rules.binary_search(&entry.rule).is_ok()
                    }
                };
            index += 1;
            retained
        });
        self.memory.release(MemoryCategory::BatchScratch, scratch_bytes);
        drop(pseudo_rules);
        verify_style_answer_patch(self, |verifier| {
            verifier.verify_cascade_answer(all, node, "pseudo compaction from updated winners");
        });
        true
    }

    pub(super) fn matches_for_cascade_immediately(
        &mut self,
        all: Vec<RuleMatch>,
        can_have_scope_duplicates: bool,
        publish_winners_for: Option<StyleNodeID>,
    ) -> Vec<RuleMatch> {
        let mut effects = AnswerEffects::default();
        let result = self.matches_for_cascade(&mut effects, all, can_have_scope_duplicates, publish_winners_for);
        self.install_answer_effects(effects);
        result
    }

    pub(super) fn matches_for_cascade(
        &mut self,
        effects: &mut AnswerEffects,
        mut all: Vec<RuleMatch>,
        can_have_scope_duplicates: bool,
        publish_winners_for: Option<StyleNodeID>,
    ) -> Vec<RuleMatch> {
        self.compact_matches_for_cascade(effects, &mut all, can_have_scope_duplicates, publish_winners_for);
        all
    }

    pub(super) fn matches_for_cascade_with_scratch(
        &mut self,
        effects: &mut AnswerEffects,
        mut all: Vec<RuleMatch>,
        can_have_scope_duplicates: bool,
        publish_winners_for: Option<StyleNodeID>,
        workspace: &mut CascadeCompactionWorkspace,
    ) -> Vec<RuleMatch> {
        self.compact_matches_for_cascade_with_scratch(
            effects,
            &mut all,
            can_have_scope_duplicates,
            publish_winners_for,
            workspace,
        );
        all
    }

    /// Re-reduce only the requested element properties from an exact retained match answer.
    ///
    /// This is the typed upquery for winner deletion repair. It consumes no selector facts and
    /// returns `None` when the retained inventories do not completely describe the cascade.
    pub(super) fn exact_cascade_winner_updates_for_properties(
        &mut self,
        node: StyleNodeID,
        matches: &[RuleMatch],
        pseudo: Option<tree::PseudoElementTarget>,
        properties: &[u16],
    ) -> Option<Vec<PropertyWinnerUpdate>> {
        self.exact_cascade_winner_updates_for_properties_with_scratch(
            node,
            matches,
            pseudo,
            properties,
            &mut Vec::new(),
        )
    }

    pub(super) fn exact_cascade_winner_updates_for_properties_with_scratch(
        &mut self,
        node: StyleNodeID,
        matches: &[RuleMatch],
        pseudo: Option<tree::PseudoElementTarget>,
        properties: &[u16],
        candidates: &mut Vec<OrderedCascadeCandidate>,
    ) -> Option<Vec<PropertyWinnerUpdate>> {
        debug_assert!(properties.windows(2).all(|pair| pair[0] < pair[1]));
        if properties.is_empty() {
            return Some(Vec::new());
        }
        if !self.cascade_winner_inventory_is_complete_for_target(matches, Some(node), pseudo) {
            return None;
        }

        let winners = self.resolved_cascade_winners_for_properties_with_scratch(
            node,
            matches,
            pseudo,
            Some(properties),
            candidates,
        );

        // Winners are an ordered subset of the requested properties.
        let mut winners = winners.into_iter().peekable();
        Some(
            properties
                .iter()
                .copied()
                .map(|property| PropertyWinnerUpdate {
                    property,
                    winner: winners.next_if(|winner| winner.property == property),
                })
                .collect(),
        )
    }

    /// Repair only the winner properties named by signed match changes.
    ///
    /// The retained exact answer supplies every contender, so deletion repair is a property-level
    /// upquery over rule identities rather than another selector match or whole-cascade reduction.
    pub(super) fn apply_cascade_winner_match_deltas(
        &mut self,
        effects: &mut AnswerEffects,
        node: StyleNodeID,
        matches: &[RuleMatch],
        deltas: &[SelectorTruthDelta],
        candidates: &mut Vec<OrderedCascadeCandidate>,
    ) -> bool {
        let mut targets: SmallVec<[Option<tree::PseudoElementTarget>; 3]> = SmallVec::new();
        for delta in deltas {
            let entry = self.programs.entry(delta.entry).1;
            if !targets.contains(&entry.pseudo_element) {
                targets.push(entry.pseudo_element);
            }
        }
        targets.into_iter().all(|target| {
            self.apply_cascade_winner_match_deltas_for_target(effects, node, matches, deltas, target, candidates)
        })
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) fn apply_cascade_winner_match_deltas_for_target(
        &mut self,
        effects: &mut AnswerEffects,
        node: StyleNodeID,
        matches: &[RuleMatch],
        deltas: &[SelectorTruthDelta],
        pseudo: Option<tree::PseudoElementTarget>,
        candidates: &mut Vec<OrderedCascadeCandidate>,
    ) -> bool {
        if !self.cascade_winner_inventory_is_complete_for_target(matches, Some(node), pseudo) {
            return false;
        }
        let key = pseudo.map_or_else(
            || WinnerGroupKey::current(node, self.program.version()),
            |pseudo| WinnerGroupKey::current_pseudo(node, pseudo, self.program.version()),
        );
        let Some((_, previous)) = effects.winners.view(&self.winner_groups).token_for(key).sparse().ok() else {
            return false;
        };

        let mut repair_properties = Vec::new();
        let mut updates: Vec<PropertyWinnerUpdate> = Vec::new();
        for delta in deltas {
            let entry = *self.programs.entry(delta.entry).1;
            if entry.pseudo_element != pseudo {
                continue;
            }
            // A gated rule's declarations win only where its container conditions hold for the
            // node, which the exact cascade decides.
            if self.program.rule_is_gated_by_container_query(delta.rule) {
                return false;
            }
            let mut matched_rule = None;
            let mut priorities = [None; 2];
            for &declared in self.program.declared_properties_of(delta.rule) {
                if !property_is_longhand(declared.property) {
                    continue;
                }
                if delta.change == SetChange::Removed
                    && self
                        .winner_groups
                        .winner_in_state(previous, declared.property)
                        .is_some_and(|winner| {
                            winner.source != WinnerSource::Rule(delta.rule)
                                && winner.source != WinnerSource::ExactCascade
                                && winner.key.continuation == CascadeContinuationID::default()
                        })
                {
                    continue;
                }
                if delta.change == SetChange::Removed
                    || matches!(
                        declared.operator,
                        CascadeOperator::Revert | CascadeOperator::RevertLayer
                    )
                {
                    repair_properties.push(declared.property);
                    continue;
                }
                let previous_winner = self.winner_groups.winner_in_state(previous, declared.property);
                if previous_winner.is_some_and(|winner| winner.key.continuation != CascadeContinuationID::default()) {
                    repair_properties.push(declared.property);
                    continue;
                }
                if matched_rule.is_none() {
                    matched_rule = matches.iter().find(|matched| {
                        matched.rule == delta.rule
                            && self.programs.entry_id(matched.program, matched.entry) == delta.entry
                    });
                }
                let Some(matched) = matched_rule else {
                    return false;
                };
                let priority = *priorities[declared.important as usize].get_or_insert_with(|| {
                    self.cascade_priority_of(
                        node,
                        delta.rule,
                        matched.tree_scope,
                        entry.specificity,
                        matched.scope_proximity,
                        declared.important,
                    )
                });
                if previous_winner.is_some_and(|winner| priority < winner.priority) {
                    continue;
                }
                let winner = PropertyWinner {
                    property: declared.property,
                    important: declared.important,
                    key: Self::retained_rule_winner_key(declared),
                    priority,
                    source: WinnerSource::Rule(delta.rule),
                };
                if let Some(update) = updates.iter_mut().find(|update| update.property == declared.property) {
                    let pending = update.winner.as_mut().expect("an added declaration carries a winner");
                    if priority >= pending.priority {
                        *pending = winner;
                    }
                } else {
                    updates.push(PropertyWinnerUpdate {
                        property: declared.property,
                        winner: Some(winner),
                    });
                }
            }
        }
        repair_properties.sort_unstable();
        repair_properties.dedup();
        if !repair_properties.is_empty() {
            let Some(repairs) = self.exact_cascade_winner_updates_for_properties_with_scratch(
                node,
                matches,
                pseudo,
                &repair_properties,
                candidates,
            ) else {
                return false;
            };
            updates.retain(|update| repair_properties.binary_search(&update.property).is_err());
            updates.extend(repairs);
        }
        updates.sort_unstable_by_key(|update| update.property);

        let (state, _) =
            self.with_cascade_interning_counters(|groups| groups.apply_property_updates(previous, &updates));
        let published = if let Some(pseudo) = pseudo {
            effects.winners.set_pseudo(
                &mut self.winner_groups,
                node,
                pseudo,
                state,
                self.program.version(),
                &mut self.memory,
            )
        } else {
            effects.winners.set(
                &mut self.winner_groups,
                node,
                state,
                self.program.version(),
                &mut self.memory,
            )
        };
        self.winner_groups.settle_memory(&mut self.memory);
        if published {
            self.counters.bump(Counter::CascadeNodeHandlesPublished);
        }
        published
    }

    pub(super) fn cascade_winner_inventory_is_complete_for_target(
        &self,
        matches: &[RuleMatch],
        node: Option<StyleNodeID>,
        pseudo: Option<tree::PseudoElementTarget>,
    ) -> bool {
        !(matches
            .iter()
            .filter(|entry| entry.pseudo_element == pseudo)
            .any(|entry| {
                !self.container_gate_is_held(node, entry.rule)
                    || !self.program.declarations_are_complete_for(entry.rule)
            })
            || (pseudo.is_none() && node.is_some_and(|node| !self.facts.element_custom_declarations(node).is_empty())))
    }

    /// Whether a pseudo-element's rules declare nothing past its longhand winners but custom
    /// properties, which its state holds itself.
    fn cascade_winner_inventory_is_complete_but_for_custom_properties_for_target(
        &self,
        node: StyleNodeID,
        matches: &[RuleMatch],
        pseudo: tree::PseudoElementTarget,
    ) -> bool {
        !matches
            .iter()
            .filter(|entry| entry.pseudo_element == Some(pseudo))
            .any(|entry| !self.container_gate_is_held(Some(node), entry.rule))
    }

    /// Where a tree scope stands among the encapsulation contexts that decide for an element,
    /// outermost first, the order its cascade applies them in: the document, the shadow trees the
    /// element is in from the outermost inwards, the ones holding the slots it is assigned to along
    /// its assignment chain, then the element's own shadow tree, for `:host`. That is the
    /// shadow-including tree order of those trees. `None` when the scope is none of them.
    pub(super) fn author_context_index(&self, node: StyleNodeID, scope: TreeScopeID) -> Option<u32> {
        let tree_scope = self.tree.tree_scope(node);
        let enclosing = std::iter::successors(Some(tree_scope), |&current| {
            let host = self.scope_root(current).and_then(|root| self.tree.host_of(root))?;
            Some(self.tree.tree_scope(host))
        });
        let is_context = enclosing
            .take(self.tree_scope_depth(tree_scope) as usize + 1)
            .any(|enclosing| enclosing == scope)
            || self.scopes_slotted_into(node).any(|slotted_into| slotted_into == scope)
            || self.own_shadow_tree(node) == Some(scope);
        is_context.then(|| self.encapsulation_context(node, scope))
    }

    /// Where the cascade of `node` weighs a match from `scope`, as `author_context_index` places
    /// it. Each tree the element is in or slotted into stands at its depth, one deeper than the
    /// last, so only the element's own shadow tree, after all of those, stands elsewhere.
    fn encapsulation_context(&self, node: StyleNodeID, scope: TreeScopeID) -> u32 {
        if self.own_shadow_tree(node) != Some(scope) {
            return self.tree_scope_depth(scope);
        }
        let slotted_into = self.scopes_slotted_into(node).count() as u32;
        self.tree_scope_depth(self.tree.tree_scope(node)) + slotted_into + 1
    }

    /// The tree the element's shadow root roots, where its `:host` rules decide from.
    fn own_shadow_tree(&self, node: StyleNodeID) -> Option<TreeScopeID> {
        self.tree
            .shadow_root_of(node)
            .and_then(|shadow_root| self.scope_by_root.get(shadow_root))
    }

    pub(super) fn cascade_winner_inventory_is_complete(
        &self,
        matches: &[RuleMatch],
        node: Option<StyleNodeID>,
    ) -> bool {
        // Checking every target covers every match. Check each inventory once instead
        // of scanning the answer again for every rule matching the same pseudo target.
        !matches.iter().any(|entry| {
            !self.container_gate_is_held(node, entry.rule) || !self.program.declarations_are_complete_for(entry.rule)
        }) && !node.is_some_and(|node| !self.facts.element_custom_declarations(node).is_empty())
    }

    /// As `cascade_winner_inventory_is_complete`, for winners published in the transaction `effects`
    /// belong to. Whether a gated rule is held is noted as the node's winners are published, so
    /// before that it is what held for the winners published last; an ancestor's answer that
    /// moves now leaves the rule undecided again.
    pub(super) fn cascade_winner_inventory_is_complete_in_transaction(
        &self,
        effects: &AnswerEffects,
        matches: &[RuleMatch],
        node: StyleNodeID,
    ) -> bool {
        self.cascade_winner_inventory_is_complete(matches, Some(node))
            && !(matches
                .iter()
                .any(|entry| self.program.rule_is_gated_by_container_query(entry.rule))
                && self.container_ancestor_answer_moves(node, effects))
    }

    /// Exactly match every style node in the document scope against the attached program.
    ///
    /// This is the optimized batch path running on the real document. It packs the facts the store
    /// has published and evaluates the match programs its dispatch and prefix state reach. It
    /// reports how many concrete rule matches it found, or the node whose facts were missing - never
    /// a partial answer.
    pub fn match_document(&mut self, root: StyleNodeID) -> Result<usize, Incomplete> {
        let nodes = self.elements_under(root);

        let mut batch = StyleNodeFacts::new();
        self.facts.materialize(nodes.iter().copied(), &mut batch);

        // One dispatch per tree, because a sheet decides only in the scopes it is attached to.
        let mut by_scope: Column<Vec<StyleNodeID>> = Column::default();
        for &node in &nodes {
            let scope = self.tree.tree_scope(node);
            by_scope.entry(scope.0 as usize).push(node);
            // A slotted element is asked the rules of the tree it is slotted into as well, because
            // `::slotted()` in that tree names it and no rule of its own tree can.
            for slotted in self.scopes_slotted_into(node) {
                if slotted != scope {
                    by_scope.entry(slotted.0 as usize).push(node);
                }
            }
            // A part is asked the rules of the scope it is exposed to, because `::part()` there
            // names it and no rule of its own tree can be addressed from outside.
            for exposed in self.part_exposure_scopes(node) {
                if exposed != scope {
                    by_scope.entry(exposed.0 as usize).push(node);
                }
            }
        }
        let can_have_scope_duplicates = by_scope.iter().filter(|nodes| !nodes.is_empty()).take(2).count() > 1;

        let mut matches = RuleMatches::new();
        let mut dispatch_workspace = DispatchCandidateWorkspace::default();
        let mut ancestor_requirements_cache = AncestorRequirementsCache::default();
        let mut result = Ok(());
        for (index, scope_nodes) in by_scope.into_iter().enumerate() {
            if scope_nodes.is_empty() {
                continue;
            }
            let scope = TreeScopeID(u32::try_from(index).expect("tree scope identity space exhausted"));
            let (_, dispatch) = self.prepare_scope_program(scope);
            let ancestor_requirements =
                ancestor_requirements_cache.prepare(&self.tree, &batch, &dispatch, None, &mut self.memory);
            // `:host` names the host of this tree, which stands outside it, so the tree's own rules
            // are asked of it as well.
            let host = self.scope_root(scope).and_then(|root| self.tree.host_of(root));
            for node in scope_nodes.into_iter().chain(host) {
                let (matched, effects) = self.match_node_in_scope(
                    node,
                    scope,
                    &dispatch,
                    &batch,
                    &mut dispatch_workspace,
                    Some(ancestor_requirements),
                    None,
                    None,
                    BatchMatchAttempt {
                        matches: &mut matches,
                        requests: None,
                        completed: None,
                        deferred_prefix_matches: None,
                        answer_is_exact: None,
                        cascade_only: false,
                    },
                );
                self.append_witness_effects(effects);
                if let Err(incomplete) = matched {
                    result = Err(incomplete);
                    break;
                }
            }
            if result.is_err() {
                break;
            }
        }
        self.install_witness_effects();
        ancestor_requirements_cache.release(&mut self.memory);
        let dispatch_workspace_bytes = dispatch_workspace.capacity_bytes();
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, dispatch_workspace_bytes);

        // Within one style node the matches come out in the order the cascade applies them, so a
        // consumer reads them rather than sorting them again.
        let match_count = result.map(|()| {
            self.order_matches_in_cascade(matches.as_mut_vec(), can_have_scope_duplicates);
            matches.len()
        });
        matches.settle_memory(&mut self.memory);
        matches.release(&mut self.memory);
        self.memory
            .release(MemoryCategory::BatchScratch, dispatch_workspace_bytes);

        match_count
    }

    /// Every tree whose `::slotted()` can name this element.
    ///
    /// A slot is itself a slottable, so an element can be assigned to a slot that is assigned to
    /// another: what stands in the flat tree at the end of that chain is the element, not the slots
    /// it passed through, and each tree along the way is one whose `::slotted()` names it.
    pub(super) fn scopes_slotted_into(&self, node: StyleNodeID) -> impl Iterator<Item = TreeScopeID> {
        // A slot standing in a shadow tree is not a flattened slottable of the tree it is assigned
        // into: "find flattened slottables" recurses into its own slottables rather than appending
        // it. So no scope names it through `::slotted()`, even though its assignment is what carries
        // the chain onward for the nodes below it.
        let is_reslotted_slot = self.facts.is_slot(node) && self.tree.tree_scope(node) != TreeScopeID::DOCUMENT;
        // A chain is at most as deep as the trees are nested, and a cycle is impossible - a slot
        // cannot be assigned to itself - but a bound keeps a corrupted column from spinning.
        const MAX_REASSIGNMENTS: usize = 32;
        let mut current = node;
        let mut remaining = MAX_REASSIGNMENTS;
        std::iter::from_fn(move || {
            if remaining == 0 || is_reslotted_slot {
                return None;
            }
            remaining -= 1;
            let slot = self.tree.assigned_slot_of(current)?;
            current = slot;
            Some(self.tree.tree_scope(slot))
        })
    }

    /// How deeply a tree scope is encapsulated: the document is zero, a shadow tree inside it one,
    /// and one inside that two. It is where an element's cascade weighs a tree it is in or slotted
    /// into (`encapsulation_context`), outermost first for a normal declaration and innermost first
    /// for an important one.
    #[must_use]
    pub(super) fn tree_scope_depth(&self, tree_scope: TreeScopeID) -> u32 {
        let mut depth = 0;
        let mut scope = tree_scope;
        while scope != TreeScopeID::DOCUMENT {
            let Some(root) = self.scope_root(scope) else {
                break;
            };
            let Some(host) = self.tree.host_of(root) else {
                break;
            };
            depth += 1;
            let next = self.tree.tree_scope(host);
            if next == scope {
                break;
            }
            scope = next;
        }
        depth
    }

    /// The encapsulation context a match decides in, as the host whose shadow tree that is.
    ///
    /// A sheet the asking scope does not hold is one of the origins that decide everywhere, and
    /// those are attached to the document, which is the outermost context - so the scope a match
    /// carries is not always the context it is weighed in. The host is reported rather than the
    /// scope because the host is an element, which is what the consumer can name.
    #[must_use]
    pub fn cascade_context_host(&self, rule: RuleID, tree_scope: TreeScopeID) -> u32 {
        self.scope_root(self.cascade_context_scope(rule, tree_scope))
            .and_then(|root| self.tree.host_of(root))
            .map_or(0, StyleNodeID::raw)
    }

    pub(super) fn cascade_context_scope(&self, rule: RuleID, tree_scope: TreeScopeID) -> TreeScopeID {
        if tree_scope == TreeScopeID::DOCUMENT {
            return tree_scope;
        }
        let sheet = self.program.rule_sheet(rule);
        match self.program.attachment_in_scope(sheet, tree_scope).is_some() {
            true => tree_scope,
            false => TreeScopeID::DOCUMENT,
        }
    }

    /// Where one rule's declarations, matched from `tree_scope`, sit in the cascade of `node`.
    ///
    /// The components that change globally - layer topology, sheet order - are read through the
    /// identities the rule references rather than copied into it, so moving a layer or a sheet
    /// updates one token instead of every rule that lives in it.
    #[must_use]
    pub(super) fn cascade_priority_of(
        &self,
        node: StyleNodeID,
        rule: RuleID,
        tree_scope: TreeScopeID,
        specificity: Specificity,
        scope_proximity: u32,
        important: bool,
    ) -> CascadePriority {
        let context_scope = self.cascade_context_scope(rule, tree_scope);
        self.cascade_priority_in_context(
            rule,
            tree_scope,
            self.encapsulation_context(node, context_scope),
            specificity,
            scope_proximity,
            important,
        )
    }

    /// As `cascade_priority_of`, for any element of `tree_scope` itself, which weighs its own tree
    /// and the document at their depths: a rule decides for such an element from no other context.
    #[must_use]
    pub(super) fn own_scope_cascade_priority_of(
        &self,
        rule: RuleID,
        tree_scope: TreeScopeID,
        specificity: Specificity,
        scope_proximity: u32,
        important: bool,
    ) -> CascadePriority {
        let context_scope = self.cascade_context_scope(rule, tree_scope);
        self.cascade_priority_in_context(
            rule,
            tree_scope,
            self.tree_scope_depth(context_scope),
            specificity,
            scope_proximity,
            important,
        )
    }

    fn cascade_priority_in_context(
        &self,
        rule: RuleID,
        tree_scope: TreeScopeID,
        context_depth: u32,
        specificity: Specificity,
        scope_proximity: u32,
        important: bool,
    ) -> CascadePriority {
        let sheet = self.program.rule_sheet(rule);
        let version = self.program.rule_version(rule);
        // A sheet the element's own scope does not hold is one of the origins that decide
        // everywhere, and those are attached to the document, which is the outermost context.
        let attachment = self.program.attachment_in_scope(sheet, tree_scope);
        let context_scope = attachment.map_or(TreeScopeID::DOCUMENT, |attachment| attachment.tree_scope);
        let attachment = attachment.or_else(|| self.program.attachment_in_scope(sheet, TreeScopeID::DOCUMENT));
        CascadePriority::new(PriorityInputs {
            origin: self.program.sheet_origin(sheet),
            important,
            context_depth,
            element_attachment: ElementAttachment::Rule,
            layer_rank: self.program.layer_rank(context_scope, version.layer),
            specificity,
            scope_proximity,
            sheet_rank: attachment.map_or(0, |attachment| self.program.sheet_rank(attachment)),
            rule_rank: self.program.rule_rank(rule),
        })
    }

    pub(super) fn element_cascade_priority(
        &self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
        important: bool,
    ) -> CascadePriority {
        let tree_scope = self.tree.tree_scope(node);
        let (origin, element_attachment) = match kind {
            ElementDeclarationKind::InlineStyle => (CascadeOrigin::Author, ElementAttachment::InlineStyle),
            ElementDeclarationKind::PresentationalHint | ElementDeclarationKind::SvgPresentationAttribute => (
                CascadeOrigin::AuthorPresentationalHint,
                ElementAttachment::PresentationalHint,
            ),
        };
        CascadePriority::new(PriorityInputs {
            origin,
            important,
            context_depth: self.tree_scope_depth(tree_scope),
            element_attachment,
            layer_rank: self.program.layer_rank(tree_scope, CascadeLayerID::UNLAYERED),
            specificity: Specificity::default(),
            scope_proximity: u32::MAX,
            sheet_rank: u64::MAX,
            rule_rank: u64::MAX,
        })
    }

    pub(super) fn node_has_element_declaration_input(&self, node: StyleNodeID) -> bool {
        !self.facts.element_custom_declarations(node).is_empty()
            || ElementDeclarationKind::ALL
                .iter()
                .any(|&kind| !self.facts.element_declared_properties(node, kind).is_empty())
    }

    /// Advance staged local facts to the transaction's final snapshot.
    pub(super) fn apply_staged_facts(&mut self, transaction: &mut StyleTransaction) {
        if self.facts.has_staged_input() {
            transaction.install_before_facts(self.facts.staged_before_facts(), &mut self.memory);
        }
        self.facts.apply_staged(&mut self.memory);
    }

    /// Drop routing entry points for rules whose sheet is attached nowhere.
    ///
    /// The router already skips such rules one route at a time, but enumerating their entry points
    /// is itself a cost that grows with every sheet that ever came and went: a page that repeatedly
    /// inserts and removes `<style>` elements would make every later mutation pay for all of the
    /// dead ones. The rules stay compiled and keep their identity, so a sheet that reattaches gets
    /// its routes back from `restore_routing_for_reattached_sheet`.
    fn shed_routing_for_detached_sheets(&mut self) {
        if !self.routing_needs_detachment_sweep {
            return;
        }
        self.routing_needs_detachment_sweep = false;
        let mut excluded_sheets = BitColumn::default();
        let mut attached_sheets = Vec::new();
        for (sheet, attached) in self.program.sheets_with_attachment_state() {
            if attached {
                attached_sheets.push(sheet);
            } else {
                excluded_sheets.set(sheet.0 as usize, true);
            }
        }
        if excluded_sheets == self.sheets_excluded_from_routing {
            return;
        }
        let mut rebuilt_routing = RoutingRegistry::new();
        for sheet in attached_sheets {
            for rule in self
                .program
                .rules_in_sheet(sheet)
                .into_iter()
                .filter(|&rule| self.program.rule_is_live(rule))
            {
                if let Some(program) = self.program.rule_version(rule).selector_program {
                    rebuilt_routing.add_rule(rule, program, &self.programs);
                }
            }
        }
        rebuilt_routing.prepare_route_liveness(&self.program, &self.programs);
        let mut previous_routing = std::mem::replace(&mut self.routing, Arc::new(rebuilt_routing));
        Arc::get_mut(&mut previous_routing)
            .expect("routing program is shared outside a planning epoch")
            .release_memory();
        Arc::get_mut(&mut self.routing)
            .expect("new routing program cannot be shared")
            .settle_memory(&mut self.memory);
        self.sheets_excluded_from_routing = excluded_sheets;
    }

    pub(super) fn sweep_selector_programs(&mut self) {
        if !self.selector_programs_need_sweep {
            return;
        }
        let live_rules = self.program.live_selector_programs().collect::<Vec<_>>();
        let mut referenced = Vec::new();
        for &(_, program) in &live_rules {
            if referenced.len() <= program.0 as usize {
                referenced.resize(program.0 as usize + 1, false);
            }
            referenced[program.0 as usize] = true;
        }
        self.match_answers.mark_referenced_selector_programs(&mut referenced);
        if !self.programs.has_unreferenced_programs(&referenced) {
            self.selector_programs_need_sweep = false;
            return;
        }

        let mut rebuilt_routing = RoutingRegistry::new();
        let mut excluded_sheets = BitColumn::default();
        for &(rule, program) in &live_rules {
            // A detached sheet's rules keep no routing entry points; see
            // `shed_routing_for_detached_sheets`.
            let sheet = self.program.rule_sheet(rule);
            if !self.program.sheet_is_attached_somewhere(sheet) {
                excluded_sheets.set(sheet.0 as usize, true);
                continue;
            }
            rebuilt_routing.add_rule(rule, program, &self.programs);
        }
        self.sheets_excluded_from_routing = excluded_sheets;
        rebuilt_routing.prepare_route_liveness(&self.program, &self.programs);
        let mut previous_routing = std::mem::replace(&mut self.routing, Arc::new(rebuilt_routing));
        Arc::get_mut(&mut previous_routing)
            .expect("routing program is shared outside a planning epoch")
            .release_memory();
        Arc::get_mut(&mut self.routing)
            .expect("new routing program cannot be shared")
            .settle_memory(&mut self.memory);

        self.relational_witnesses.clear_all();
        self.relational_witness_residency.release();
        self.scope_dispatch_templates.retain(|shape, _| {
            shape
                .0
                .iter()
                .all(|&(program, _)| referenced.get(program.0 as usize).copied().unwrap_or(false))
        });
        self.scope_cascade_templates.clear();
        self.ancestor_dispatch_templates.clear();
        self.programs.sweep_unreferenced(&referenced);
        self.programs.settle_memory(&mut self.memory);
        self.selector_programs_need_sweep = false;
    }
}

impl StyleEngine {
    /// Normalize the pending inputs without advancing the committed snapshot.
    pub(super) fn drain_transaction(&mut self) -> StyleTransaction {
        self.merge_deferred_geometry_transaction();
        self.host.initial_tree_bulk_load_is_pending = false;
        self.finalize_staged_sheet_rule_replacements();
        // A diagnostic or retained planning snapshot may still hold this exact immutable routing
        // program. It remains queryable in builder form; compact it at the next unshared boundary.
        if let Some(routing) = Arc::get_mut(&mut self.retained.routing)
            && routing.finish_directories()
        {
            routing.settle_memory(&mut self.retained.memory);
        }
        self.host
            .journal
            .take_transaction(&mut self.retained.memory, &self.retained.counters)
    }

    /// Advance staged program and tree state to the transaction's final snapshot.
    pub(super) fn apply_staged_structural_state(&mut self) {
        self.commit_staged_program();
        self.host.program_staging.rule_change_is_carried_by_sheet.clear();
        self.apply_staged_tree_deltas();
    }

    /// Finish the transaction metadata which depends on committed program state.
    pub(super) fn finish_staged_application(&mut self, transaction: &mut StyleTransaction) {
        transaction.program_base_version = self.host.program_staging.base_version.take();
        let mut program_joins = Vec::new();
        for input in &transaction.inputs {
            self.append_program_join_deltas(input, &mut program_joins);
        }
        transaction.install_program_joins(program_joins, &mut self.retained.memory);
        let mut declaration_changes = std::mem::take(&mut self.host.program_staging.rule_declaration_changes);
        declaration_changes.retain(|change| {
            transaction
                .inputs
                .binary_search_by_key(&InputKey::RuleField(change.rule, RuleField::Declarations), |input| {
                    input.key
                })
                .is_ok()
        });
        transaction.install_rule_declaration_changes(
            declaration_changes
                .into_iter()
                .map(|change| RuleDeclarationChange {
                    rule: change.rule,
                    old_properties: change.old_properties,
                    new_properties: change.new_properties,
                    custom_declarations_changed: change.custom_declarations_changed,
                })
                .collect(),
            &mut self.retained.memory,
        );
    }

    /// Settle inputs which cannot be planned while the document has no style root. Exact element
    /// style reactions are edge-triggered, so preserve them for the first transaction with a root.
    pub(crate) fn flush_without_document_root(&mut self) {
        let transaction = self.take_transaction();
        // NB: Without routing, retained relations still describe the previous tree.
        if !self.host.tree_staging.is_empty() {
            self.discard_retained_prefix_caches();
        }
        for input in &transaction.inputs {
            if let (
                InputKey::ElementStyleInput(node),
                InputValue::ElementStyleInput {
                    reaction,
                    inherited_style_groups,
                },
            ) = (input.key, input.new)
            {
                self.defer_element_style_input(node, reaction, inherited_style_groups);
            }
        }
        // Held back rather than owed: without a root there is no transaction to take them.
        self.host.deferred_element_style_inputs_are_pending = false;
        self.release_transaction(transaction);
    }

    /// Merge one element style input into the deferred inputs, which are kept sorted by key.
    pub(crate) fn defer_element_style_input(&mut self, node: StyleNodeID, reaction: u8, inherited_style_groups: u8) {
        self.host.deferred_element_style_inputs_moved = true;
        let key = InputKey::ElementStyleInput(node);
        match self
            .host
            .deferred_element_style_inputs
            .binary_search_by_key(&key, |pending| pending.key)
        {
            Ok(index) => {
                let InputValue::ElementStyleInput {
                    reaction: pending_reaction,
                    inherited_style_groups: pending_inherited_style_groups,
                } = &mut self.host.deferred_element_style_inputs[index].new
                else {
                    unreachable!();
                };
                *pending_reaction |= reaction;
                *pending_inherited_style_groups |= inherited_style_groups;
            }
            Err(index) => self.host.deferred_element_style_inputs.insert(
                index,
                NormalizedInput {
                    key,
                    old: InputValue::ElementStyleInput {
                        reaction: 0,
                        inherited_style_groups: 0,
                    },
                    new: InputValue::ElementStyleInput {
                        reaction,
                        inherited_style_groups,
                    },
                },
            ),
        }
        let deferred_style_input_bytes =
            (self.host.deferred_element_style_inputs.capacity() * size_of::<NormalizedInput>()) as u64;
        self.host
            .deferred_element_style_input_memory
            .resize_required_to(&mut self.retained.memory, deferred_style_input_bytes);
    }

    /// Release a drained transaction's scratch charge.
    pub fn release_transaction(&mut self, transaction: StyleTransaction) {
        transaction.release(&mut self.retained.memory);
        self.forget_departed_elements();
        self.host.tree_staging.clear();
        self.host
            .tree_staging_memory
            .resize_required_to(&mut self.retained.memory, 0);
        self.retained.facts.release_staging(&mut self.retained.memory);
        self.host.program_staging.clear();
        self.sweep_selector_programs();
        self.shed_routing_for_detached_sheets();
    }

    pub(super) fn collect_live_style_atoms(&self) -> (HashSet<StyleAtomID>, u64) {
        assert!(
            self.host.tree_staging.is_empty(),
            "style atom sweeping requires settled tree staging"
        );
        assert!(
            self.retained.facts.staging_is_empty(),
            "style atom sweeping requires settled fact staging"
        );
        let mut atoms = HashSet::default();
        let mut visited = self.retained.tree.collect_atoms(&mut atoms);
        visited += self.retained.facts.collect_atoms(&mut atoms);
        visited += self.retained.program.collect_atoms(&mut atoms);
        visited += self.retained.programs.collect_atoms(&mut atoms);
        if !self.retained.html_element_namespace.is_none() {
            atoms.insert(self.retained.html_element_namespace);
        }
        (atoms, visited)
    }

    pub(super) fn sweep_style_atoms(&mut self) {
        if !self.retained.atoms.should_sweep() || self.host.defers_atom_sweep {
            return;
        }
        if self.retained.batch_matching_traversal.is_some() {
            self.retained
                .counters
                .bump(Counter::AtomSweepsDeferredForActiveTraversal);
            return;
        }
        self.retained.facts.sweep_auxiliary_catalogs_without_sync();
        let (mut live, visited) = self.collect_live_style_atoms();
        self.retained.atoms.mark_sweep_dependencies(&mut live);
        loop {
            let previous_live_count = live.len();
            self.retained.facts.extend_live_attribute_name_forms(&mut live);
            self.retained.atoms.mark_sweep_dependencies(&mut live);
            if live.len() == previous_live_count {
                break;
            }
        }
        let reclaimable = self.retained.atoms.reclaimable_for_sweep(&live);
        self.retained.facts.forget_atoms(&reclaimable);
        self.retained.custom_property_environments.forget_names(&reclaimable);
        let is_reclaimed = |atom: &StyleAtomID| reclaimable.binary_search(atom).is_ok();
        if self.retained.attribute_value_text_names.iter().any(is_reclaimed) {
            Arc::make_mut(&mut self.retained.attribute_value_text_names).retain(|atom| !is_reclaimed(atom));
            self.retained.attribute_value_text_requirements_version += 1;
        }
        let reclaimed = self.retained.atoms.finish_sweep(&reclaimable);
        self.retained.counters.bump(Counter::AtomSweeps);
        self.retained.counters.add(Counter::AtomSweepRootSlotsVisited, visited);
        self.retained.counters.add(
            Counter::StyleAtomsReclaimed,
            u64::try_from(reclaimed.len()).expect("reclaimed atom count exceeds u64"),
        );
        self.host.reclaimed_style_atoms.extend(reclaimed);
    }

    /// Drop the fact rows of the elements that left, now that nothing can still route from them.
    ///
    /// A retired identity is not reused until the epoch that could name it has retired, so a row
    /// left behind here would be read as the next occupant's. Dropping it at the transaction
    /// boundary keeps the row alive for exactly as long as routing needs it.
    pub(super) fn forget_departed_elements(&mut self) {
        let departed: Vec<StyleNodeID> = self
            .host
            .tree_staging
            .rows()
            .filter_map(|(node, _, after)| after.is_none().then_some(node))
            .collect();
        if departed.is_empty() {
            return;
        }
        // Routing walks a retained prefix relation through the departures it plans for. A
        // transaction that planned nothing for them (a rootless flush does not route at all) left
        // them live in it, and the relation would go on answering for nodes that have no facts.
        if self
            .retained
            .prefix_caches
            .borrow()
            .states
            .relation_holds_any(&departed)
        {
            self.discard_retained_prefix_caches();
        }
        for node in departed {
            self.retained.facts.forget(node);
            self.retained
                .retained_match_answers
                .forget(&mut self.retained.match_answers, node);
            if let Some(tree_scope) = self.retained.scope_by_root.remove(node) {
                self.retained.scope_roots[tree_scope.0 as usize] = None;
            }
        }
        // The catalog sweep needs unique primary rows, like the atom sweep; while a traversal
        // borrows them the dead entries wait for the next boundary.
        if self.retained.batch_matching_traversal.is_none() {
            self.retained.facts.sweep_auxiliary_catalogs();
        }
    }

    /// The document budget is written in connected elements, so it has to follow the live element
    /// count rather than a high-water mark.
    pub(super) fn publish_budget_inputs(&mut self) {
        let inputs = BudgetInputs {
            connected_element_count: self.retained.tree.connected_element_count(),
        };
        self.retained.memory.set_budget_inputs(inputs);
        self.host
            .journal
            .set_document_capacity_limit(self.retained.tree.connected_element_count());
    }
}

impl StyleEngine {
    /// Advance every staged input family to the transaction's final snapshot.
    pub(super) fn apply_staged_transaction(&mut self, transaction: &mut StyleTransaction) {
        self.apply_staged_structural_state();
        self.apply_staged_facts(transaction);
        self.finish_staged_application(transaction);
    }

    /// Normalize and apply the staged inputs into one transaction. A required style observation
    /// drains here first, so normalization never combines changes across an observation boundary.
    pub fn take_transaction(&mut self) -> StyleTransaction {
        let mut transaction = self.drain_transaction();
        self.apply_staged_transaction(&mut transaction);
        transaction
    }

    /// Release a transaction taken through the bridge and reclaim atoms before the bridge installs
    /// a new primary view. The returned reclamation batch lets C++ purge its atom memos before any
    /// reclaimed identity can be reused.
    pub(super) fn release_transaction_and_sweep_atoms(&mut self, transaction: StyleTransaction) {
        self.release_transaction(transaction);
        self.sweep_style_atoms();
    }
}
