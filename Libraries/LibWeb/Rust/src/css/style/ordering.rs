/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::cascade::Top1Winner;
use super::*;

#[derive(Clone, Copy)]
enum CascadeCompactionCandidate {
    Rule(usize),
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

pub(super) struct CascadeCompactionWorkspace {
    top_1: CascadeCompactionTop1,
    element_top_1: ElementCascadeCompactionTop1,
}

impl Default for CascadeCompactionWorkspace {
    fn default() -> Self {
        Self {
            top_1: CascadeCompactionTop1::with_capacity(0),
            element_top_1: ElementCascadeCompactionTop1 {
                winners: Vec::new(),
                winner_by_property: Vec::new(),
            },
        }
    }
}

impl CascadeCompactionWorkspace {
    pub(super) fn capacity_bytes(&self) -> u64 {
        self.top_1.capacity_bytes() as u64 + self.element_top_1.capacity_bytes()
    }
}

impl StyleEngine {
    /// Order matches the way the cascade applies them, dropping repeats from asking more than one
    /// tree scope when that could have happened.
    pub(super) fn order_matches_in_cascade(&self, all: &mut Vec<RuleMatch>, can_have_scope_duplicates: bool) {
        // One match per rule and target, not per scope that asked. The user-agent and user origins
        // decide in every scope, so a node asked in two of them - a host, which is asked its own
        // scope's rules and its shadow tree's - meets those rules twice. Their priority is the same
        // either way, because a sheet the asking scope does not hold is weighed where it is
        // attached.
        let identity = |entry: &RuleMatch| {
            (
                entry.node,
                entry.rule,
                entry.pseudo_element.map_or(u32::MAX, |target| u32::from(target.kind.0)),
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

    pub(super) fn cascade_stratum_of(&self, rule: RuleID, tree_scope: TreeScopeID, important: bool) -> CascadeStratum {
        let sheet = self.program.rule_sheet(rule);
        let context_scope = self.cascade_context_scope(rule, tree_scope);
        let layer = self.program.rule_version(rule).layer;
        CascadeStratum::new(
            self.program.sheet_origin(sheet),
            important,
            self.tree_scope_depth(context_scope),
            layer,
            self.program.layer_rank(context_scope, layer),
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
        )
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
        let wants = |property| properties.is_none_or(|properties| properties.binary_search(&property).is_ok());
        candidates.clear();
        for entry in matches.iter().filter(|entry| entry.pseudo_element == pseudo) {
            for &declared in self.program.declared_properties_of(entry.rule) {
                if !wants(declared.property) {
                    continue;
                }
                let priority = self.cascade_priority_of(
                    entry.rule,
                    entry.tree_scope,
                    entry.specificity,
                    entry.scope_proximity,
                    declared.important,
                );
                candidates.push(OrderedCascadeCandidate {
                    winner: PropertyWinner {
                        property: declared.property,
                        important: declared.important,
                        key: Self::retained_rule_winner_key(declared),
                        priority,
                        source: WinnerSource::Rule(entry.rule),
                    },
                    priority,
                    stratum: self.cascade_stratum_of(entry.rule, entry.tree_scope, declared.important),
                });
            }
        }
        if pseudo.is_none() {
            for kind in ElementDeclarationKind::ALL {
                let (declared_properties, declarations_are_complete) =
                    self.facts.element_declared_properties(node, kind);
                if !declarations_are_complete {
                    continue;
                }
                for &declared in declared_properties {
                    if !wants(declared.property) {
                        continue;
                    }
                    let priority = self.element_cascade_priority(node, kind, declared.important);
                    candidates.push(OrderedCascadeCandidate {
                        winner: PropertyWinner {
                            property: declared.property,
                            important: declared.important,
                            key: Self::retained_rule_winner_key(declared),
                            priority,
                            source: WinnerSource::Element(kind),
                        },
                        priority,
                        stratum: self.element_cascade_stratum(node, kind, declared.important),
                    });
                }
            }
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
        all: &mut Vec<RuleMatch>,
        can_have_scope_duplicates: bool,
        publish_winners_for: Option<StyleNodeID>,
    ) {
        let mut workspace = CascadeCompactionWorkspace::default();
        self.compact_matches_for_cascade_with_scratch(
            all,
            can_have_scope_duplicates,
            publish_winners_for,
            &mut workspace,
        );
        let workspace_bytes = workspace.capacity_bytes();
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, workspace_bytes);
        self.memory.release(MemoryCategory::BatchScratch, workspace_bytes);
    }

    pub(super) fn compact_matches_for_cascade_with_scratch(
        &mut self,
        all: &mut Vec<RuleMatch>,
        can_have_scope_duplicates: bool,
        mut publish_winners_for: Option<StyleNodeID>,
        workspace: &mut CascadeCompactionWorkspace,
    ) {
        if publish_winners_for.is_some_and(|node| {
            !self.winner_groups.admits_new_rows()
                && matches!(
                    self.winner_groups
                        .lookup(WinnerGroupKey::current(node, self.program.version())),
                    Lookup::Missing(_)
                )
        }) {
            publish_winners_for = None;
        }
        self.order_matches_in_cascade(all, can_have_scope_duplicates);
        self.counters
            .add(Counter::CascadeMatchesBeforeCompaction, all.len() as u64);
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
                    .0
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

        // The number of declaration candidates can be orders of magnitude larger than the number
        // of semantic outputs: every rule may declare the same properties, while this reduction
        // stores only one winner per pseudo/property pair. Let the table grow with distinct keys
        // instead of reserving for every losing declaration.
        let top_1 = &mut workspace.top_1;
        top_1.clear();
        let element_top_1 = &mut workspace.element_top_1;
        element_top_1.clear();
        for (match_index, entry) in all.iter().enumerate() {
            if compaction_blocked
                && (entry.tree_scope != TreeScopeID::DOCUMENT
                    || self.program.rule_is_gated_by_container_query(entry.rule)
                    || !self.program.declarations_are_complete_for(entry.rule))
            {
                continue;
            }
            let declared_properties = self.program.declared_properties_of(entry.rule);
            let normal_priority = self.cascade_priority_of(
                entry.rule,
                entry.tree_scope,
                entry.specificity,
                entry.scope_proximity,
                false,
            );
            let mut important_priority = None;
            for declared in declared_properties {
                let priority = match declared.important {
                    true => *important_priority.get_or_insert_with(|| {
                        self.cascade_priority_of(
                            entry.rule,
                            entry.tree_scope,
                            entry.specificity,
                            entry.scope_proximity,
                            true,
                        )
                    }),
                    false => normal_priority,
                };
                match entry.pseudo_element {
                    Some(_) => top_1.consider(
                        (entry.pseudo_element, declared.property),
                        priority,
                        CascadeCompactionCandidate::Rule(match_index),
                    ),
                    None => element_top_1.consider(
                        declared.property,
                        priority,
                        CascadeCompactionCandidate::Rule(match_index),
                    ),
                }
            }
        }
        if let Some(node) = publish_winners_for {
            for kind in ElementDeclarationKind::ALL {
                let (declared_properties, declarations_are_complete) =
                    self.facts.element_declared_properties(node, kind);
                if !declarations_are_complete {
                    continue;
                }
                for &declared in declared_properties {
                    element_top_1.consider(
                        declared.property,
                        self.element_cascade_priority(node, kind, declared.important),
                        CascadeCompactionCandidate::Element(kind, declared),
                    );
                }
            }
        }
        let mut scratch_bytes = 0;

        let published_winners = if let Some(node) = publish_winners_for {
            let mut targets = vec![None];
            for target in all.iter().filter_map(|entry| entry.pseudo_element) {
                if !targets.contains(&Some(target)) {
                    targets.push(Some(target));
                }
            }
            // A target which lost its final matching rule still needs an empty winner state.
            // Otherwise its previous sparse row would remain current and could incorrectly prove
            // that the now-absent pseudo does not need recomputation.
            for (target, _, _, _) in self.winner_groups.pseudo_states(node) {
                if !targets.contains(&Some(target)) {
                    targets.push(Some(target));
                }
            }
            if has_continuations {
                Some(
                    targets
                        .into_iter()
                        .map(|target| {
                            let winners = self.resolved_cascade_winners_for_properties(node, all, target, None);
                            (target, winners)
                        })
                        .collect::<Vec<_>>(),
                )
            } else {
                Some(
                    targets
                        .into_iter()
                        .map(|target| {
                            let materialize = |property, priority, payload| {
                                let (key, important, source) = match payload {
                                    CascadeCompactionCandidate::Rule(match_index) => {
                                        let entry = &all[match_index];
                                        let declared = *self
                                            .program
                                            .declared_properties_of(entry.rule)
                                            .iter()
                                            .find(|declared| declared.property == property)
                                            .expect("winner candidate came from the rule's declaration inventory");
                                        (
                                            Self::retained_rule_winner_key(declared),
                                            declared.important,
                                            WinnerSource::Rule(entry.rule),
                                        )
                                    }
                                    CascadeCompactionCandidate::Element(kind, declared) => (
                                        Self::retained_rule_winner_key(declared),
                                        declared.important,
                                        WinnerSource::Element(kind),
                                    ),
                                };
                                PropertyWinner {
                                    property,
                                    important,
                                    key,
                                    priority,
                                    source,
                                }
                            };
                            let winners = match target {
                                None => element_top_1
                                    .winners()
                                    .iter()
                                    .map(|winner| materialize(winner.key, winner.priority, winner.payload))
                                    .collect(),
                                Some(_) => top_1
                                    .winners()
                                    .filter(|winner| winner.key.0 == target)
                                    .map(|winner| materialize(winner.key.1, winner.priority, winner.payload))
                                    .collect(),
                            };
                            (target, winners)
                        })
                        .collect::<Vec<_>>(),
                )
            }
        } else {
            None
        };

        if let Some(node) = publish_winners_for {
            let published_winners = published_winners.as_ref().expect("winner publication requested");
            let winner_count = published_winners
                .iter()
                .map(|(_, winners)| winners.len())
                .sum::<usize>();
            let winner_scratch_bytes = (winner_count * size_of::<PropertyWinner>()) as u64;
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, winner_scratch_bytes);
            let mut published_row_count = 0;
            for (target, winners) in published_winners {
                let key = target.map_or_else(
                    || WinnerGroupKey::current(node, self.program.version()),
                    |target| WinnerGroupKey::current_pseudo(node, target, self.program.version()),
                );
                let previous = self.winner_groups.token_for(key).sparse().ok().map(|(_, state)| state);
                let state = self.intern_cascade_state(winners, previous);
                if let Some(target) = target {
                    let inventory_is_complete =
                        self.cascade_winner_inventory_is_complete_for_target(all, Some(node), Some(*target));
                    let published = self
                        .winner_groups
                        .set_pseudo(node, *target, state, self.program.version());
                    if published && !inventory_is_complete {
                        self.winner_groups.mark_pseudo_inventory_incomplete(node, *target);
                    }
                    published_row_count += usize::from(published);
                } else {
                    published_row_count += usize::from(self.winner_groups.set(node, state, self.program.version()));
                }
            }
            self.winner_groups.settle_memory(&mut self.memory);
            self.counters
                .add(Counter::CascadeNodeHandlesPublished, published_row_count as u64);
            self.memory.release(MemoryCategory::BatchScratch, winner_scratch_bytes);
        }

        if compaction_blocked {
            self.memory.release(MemoryCategory::BatchScratch, scratch_bytes);
            return;
        }

        let compaction_scratch_bytes =
            (all.len().div_ceil(8) + all.len() * size_of::<tree::PseudoElementTarget>()) as u64;
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, compaction_scratch_bytes);
        scratch_bytes += compaction_scratch_bytes;
        let mut keep = vec![false; all.len()];
        for (index, entry) in all.iter().enumerate() {
            if self.program.sheet_origin(self.program.rule_sheet(entry.rule)) != CascadeOrigin::Author {
                keep[index] = true;
            }
        }
        for winner in top_1.winners() {
            if let CascadeCompactionCandidate::Rule(match_index) = winner.payload {
                keep[match_index] = true;
            }
        }
        for winner in element_top_1.winners() {
            if let CascadeCompactionCandidate::Rule(match_index) = winner.payload {
                keep[match_index] = true;
            }
        }
        for (_, winners) in published_winners.as_deref().unwrap_or_default() {
            for &winner in winners {
                let mut current = Some(winner);
                while let Some(winner) = current {
                    if let WinnerSource::Rule(rule) = winner.source
                        && let Some(index) = all.iter().position(|entry| entry.rule == rule)
                    {
                        keep[index] = true;
                    }
                    current = self
                        .winner_groups
                        .continuation(winner.key.continuation)
                        .and_then(|continuation| continuation.winner);
                }
            }
        }

        let mut retained_pseudo_targets = Vec::with_capacity(all.len());
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

        let actual_scratch_bytes = (keep.capacity().div_ceil(8)
            + retained_pseudo_targets.capacity() * size_of::<tree::PseudoElementTarget>())
            as u64;
        if actual_scratch_bytes > scratch_bytes {
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, actual_scratch_bytes - scratch_bytes);
        } else {
            self.memory
                .release(MemoryCategory::BatchScratch, scratch_bytes - actual_scratch_bytes);
        }
        scratch_bytes = actual_scratch_bytes;

        let mut index = 0;
        all.retain(|_| {
            let retained = keep[index];
            index += 1;
            retained
        });
        self.memory.release(MemoryCategory::BatchScratch, scratch_bytes);
    }

    pub(super) fn matches_for_cascade(
        &mut self,
        mut all: Vec<RuleMatch>,
        can_have_scope_duplicates: bool,
        publish_winners_for: Option<StyleNodeID>,
    ) -> Vec<RuleMatch> {
        self.compact_matches_for_cascade(&mut all, can_have_scope_duplicates, publish_winners_for);
        all
    }

    pub(super) fn matches_for_cascade_with_scratch(
        &mut self,
        mut all: Vec<RuleMatch>,
        can_have_scope_duplicates: bool,
        publish_winners_for: Option<StyleNodeID>,
        workspace: &mut CascadeCompactionWorkspace,
    ) -> Vec<RuleMatch> {
        self.compact_matches_for_cascade_with_scratch(
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

        Some(
            properties
                .iter()
                .copied()
                .map(|property| PropertyWinnerUpdate {
                    property,
                    winner: winners
                        .binary_search_by_key(&property, |winner| winner.property)
                        .ok()
                        .map(|index| winners[index]),
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
        node: StyleNodeID,
        matches: &[RuleMatch],
        deltas: &[SelectorTruthDelta],
        candidates: &mut Vec<OrderedCascadeCandidate>,
    ) -> bool {
        let mut targets = Vec::new();
        for delta in deltas {
            let entry = self.programs.entry(delta.entry).1;
            if !targets.contains(&entry.pseudo_element) {
                targets.push(entry.pseudo_element);
            }
        }
        targets
            .into_iter()
            .all(|target| self.apply_cascade_winner_match_deltas_for_target(node, matches, deltas, target, candidates))
    }

    pub(super) fn apply_cascade_winner_match_deltas_for_target(
        &mut self,
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
        let Some((_, previous)) = self.winner_groups.token_for(key).sparse().ok() else {
            return false;
        };

        let mut repair_properties = Vec::new();
        let mut updates: Vec<PropertyWinnerUpdate> = Vec::new();
        let mut update_priorities = Vec::new();
        for delta in deltas {
            let entry = *self.programs.entry(delta.entry).1;
            if entry.pseudo_element != pseudo {
                continue;
            }
            for &declared in self.program.declared_properties_of(delta.rule) {
                let retained_winner_has_continuation = self
                    .winner_groups
                    .winner_in_state(previous, declared.property)
                    .is_some_and(|winner| self.winner_groups.continuation(winner.key.continuation).is_some());
                if delta.change == SetChange::Removed
                    || retained_winner_has_continuation
                    || matches!(
                        declared.operator,
                        CascadeOperator::Revert | CascadeOperator::RevertLayer
                    )
                {
                    repair_properties.push(declared.property);
                    continue;
                }
                let Some(matched) = matches.iter().find(|matched| {
                    matched.rule == delta.rule && self.programs.entry_id(matched.program, matched.entry) == delta.entry
                }) else {
                    return false;
                };
                let priority = self.cascade_priority_of(
                    delta.rule,
                    matched.tree_scope,
                    entry.specificity,
                    matched.scope_proximity,
                    declared.important,
                );
                let winner = PropertyWinner {
                    property: declared.property,
                    important: declared.important,
                    key: Self::retained_rule_winner_key(declared),
                    priority,
                    source: WinnerSource::Rule(delta.rule),
                };
                if let Some(index) = updates.iter().position(|update| update.property == declared.property) {
                    if priority >= update_priorities[index] {
                        updates[index].winner = Some(winner);
                        update_priorities[index] = priority;
                    }
                } else if let Some(previous_winner) = self.winner_groups.winner_in_state(previous, declared.property) {
                    if priority >= previous_winner.priority {
                        updates.push(PropertyWinnerUpdate {
                            property: declared.property,
                            winner: Some(winner),
                        });
                        update_priorities.push(priority);
                    }
                } else {
                    updates.push(PropertyWinnerUpdate {
                        property: declared.property,
                        winner: Some(winner),
                    });
                    update_priorities.push(priority);
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
            self.winner_groups
                .set_pseudo(node, pseudo, state, self.program.version())
        } else {
            self.winner_groups.set(node, state, self.program.version())
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
                self.program.rule_is_gated_by_container_query(entry.rule)
                    || !self.program.declarations_are_complete_for(entry.rule)
                    || entry.tree_scope != TreeScopeID::DOCUMENT
            })
            || (pseudo.is_none()
                && node.is_some_and(|node| {
                    ElementDeclarationKind::ALL
                        .iter()
                        .any(|&kind| !self.facts.element_declared_properties(node, kind).1)
                })))
    }

    pub(super) fn cascade_winner_inventory_is_complete(
        &self,
        matches: &[RuleMatch],
        node: Option<StyleNodeID>,
    ) -> bool {
        self.cascade_winner_inventory_is_complete_for_target(matches, node, None)
            && matches
                .iter()
                .filter_map(|entry| entry.pseudo_element)
                .all(|pseudo| self.cascade_winner_inventory_is_complete_for_target(matches, node, Some(pseudo)))
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
            let (_, dispatch) = self.ranked_scope_program(scope);
            let ancestor_requirements =
                ancestor_requirements_cache.get_or_build(&self.tree, &batch, &dispatch, &mut self.memory);
            // `:host` names the host of this tree, which stands outside it, so the tree's own rules
            // are asked of it as well.
            let host = self.scope_root(scope).and_then(|root| self.tree.host_of(root));
            for node in scope_nodes.into_iter().chain(host) {
                if let Err(incomplete) = self.match_node_in_scope(
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
                ) {
                    result = Err(incomplete);
                    break;
                }
            }
            if result.is_err() {
                break;
            }
        }
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
    /// and one inside that two. It is what orders the contexts against each other, outermost first
    /// for a normal declaration and innermost first for an important one.
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
        let sheet = self.program.rule_sheet(rule);
        match self.program.attachment_in_scope(sheet, tree_scope).is_some() {
            true => tree_scope,
            false => TreeScopeID::DOCUMENT,
        }
    }

    /// Where one rule's declarations sit in the cascade for an element in `tree_scope`.
    ///
    /// The components that change globally - layer topology, sheet order - are read through the
    /// identities the rule references rather than copied into it, so moving a layer or a sheet
    /// updates one token instead of every rule that lives in it.
    #[must_use]
    pub(super) fn cascade_priority_of(
        &self,
        rule: RuleID,
        tree_scope: TreeScopeID,
        specificity: Specificity,
        scope_proximity: u32,
        important: bool,
    ) -> CascadePriority {
        let sheet = self.program.rule_sheet(rule);
        let version = self.program.rule_version(rule);
        // A sheet the rule's own scope does not hold is one of the origins that decide everywhere,
        // and those are attached to the document.
        let attachment = self.program.attachment_in_scope(sheet, tree_scope);
        // A sheet the element's own scope does not hold is one of the origins that decide
        // everywhere, and those are attached to the document, which is the outermost context.
        let context_scope = self.cascade_context_scope(rule, tree_scope);
        let attachment = attachment.or_else(|| self.program.attachment_in_scope(sheet, TreeScopeID::DOCUMENT));
        CascadePriority::new(PriorityInputs {
            origin: self.program.sheet_origin(sheet),
            important,
            context_depth: self.tree_scope_depth(context_scope),
            element_attachment: ElementAttachment::Rule,
            layer: version.layer,
            layer_rank: self.program.layer_rank(context_scope, version.layer),
            specificity,
            scope_proximity,
            sheet_rank: attachment.map_or((0, 0), |attachment| self.program.sheet_rank(attachment)),
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
            layer: CascadeLayerID::UNLAYERED,
            layer_rank: self.program.layer_rank(tree_scope, CascadeLayerID::UNLAYERED),
            specificity: Specificity::default(),
            scope_proximity: u32::MAX,
            sheet_rank: (u64::MAX, u64::MAX),
            rule_rank: (u64::MAX, u64::MAX),
        })
    }

    pub(super) fn node_has_element_declaration_input(&self, node: StyleNodeID) -> bool {
        ElementDeclarationKind::ALL.iter().any(|&kind| {
            let (declared, complete) = self.facts.element_declared_properties(node, kind);
            !declared.is_empty() || !complete
        })
    }

    /// Normalize the pending inputs without advancing the committed snapshot.
    pub(super) fn drain_transaction(&mut self) -> StyleTransaction {
        self.merge_deferred_geometry_transaction();
        self.initial_tree_bulk_load_is_pending = false;
        self.finalize_staged_sheet_rule_replacements();
        // A diagnostic or retained planning snapshot may still hold this exact immutable routing
        // program. It remains queryable in builder form; compact it at the next unshared boundary.
        if let Some(routing) = Rc::get_mut(&mut self.routing)
            && routing.finish_directories()
        {
            routing.settle_memory(&mut self.memory);
        }
        self.journal.take_transaction(&mut self.memory, &mut self.counters)
    }

    /// Advance staged program and tree state to the transaction's final snapshot.
    pub(super) fn apply_staged_structural_state(&mut self) {
        self.commit_staged_program();
        self.program_staging.rule_change_is_carried_by_sheet.clear();
        self.apply_staged_tree_deltas();
    }

    /// Advance staged local facts to the transaction's final snapshot.
    pub(super) fn apply_staged_facts(&mut self, transaction: &mut StyleTransaction) {
        if self.facts.has_staged_input() {
            transaction.install_before_facts(self.facts.staged_before_facts(), &mut self.memory);
        }
        self.facts.apply_staged(&mut self.memory);
    }

    /// Finish the transaction metadata which depends on committed program state.
    pub(super) fn finish_staged_application(&mut self, transaction: &mut StyleTransaction) {
        transaction.program_base_version = self.program_staging.base_version.take();
        let mut program_joins = Vec::new();
        for input in &transaction.inputs {
            self.append_program_join_deltas(input, &mut program_joins);
        }
        transaction.install_program_joins(program_joins, &mut self.memory);
        let mut declaration_changes = std::mem::take(&mut self.program_staging.rule_declaration_changes);
        declaration_changes.retain(|change| {
            transaction
                .inputs
                .iter()
                .any(|input| input.key == InputKey::RuleField(change.rule, RuleField::Declarations))
        });
        transaction.install_rule_declaration_changes(
            declaration_changes
                .into_iter()
                .map(|change| RuleDeclarationChange {
                    rule: change.rule,
                    old_properties: change.old_properties,
                    new_properties: change.new_properties,
                })
                .collect(),
            &mut self.memory,
        );
    }

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

    /// Settle inputs which cannot be planned while the document has no style root. Exact element
    /// style reactions are edge-triggered, so preserve them for the first transaction with a root.
    pub(crate) fn flush_without_document_root(&mut self) {
        let transaction = self.take_transaction();
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
        self.deferred_element_style_inputs_are_pending = false;
        self.externally_recorded_style_input_nodes.extend(
            self.deferred_element_style_inputs
                .iter()
                .filter_map(|input| input.key.style_node()),
        );
        self.release_transaction(transaction);
    }

    /// Merge one element style input into the deferred inputs, which are kept sorted by key.
    pub(crate) fn defer_element_style_input(&mut self, node: StyleNodeID, reaction: u8, inherited_style_groups: u8) {
        let key = InputKey::ElementStyleInput(node);
        match self
            .deferred_element_style_inputs
            .binary_search_by_key(&key, |pending| pending.key)
        {
            Ok(index) => {
                let InputValue::ElementStyleInput {
                    reaction: pending_reaction,
                    inherited_style_groups: pending_inherited_style_groups,
                } = &mut self.deferred_element_style_inputs[index].new
                else {
                    unreachable!();
                };
                *pending_reaction |= reaction;
                *pending_inherited_style_groups |= inherited_style_groups;
            }
            Err(index) => self.deferred_element_style_inputs.insert(
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
            (self.deferred_element_style_inputs.capacity() * size_of::<NormalizedInput>()) as u64;
        self.deferred_element_style_input_memory
            .resize_required_to(&mut self.memory, deferred_style_input_bytes);
    }

    /// Release a drained transaction's scratch charge.
    pub fn release_transaction(&mut self, transaction: StyleTransaction) {
        transaction.release(&mut self.memory);
        self.forget_departed_elements();
        self.tree_staging.clear();
        self.tree_staging_memory.resize_required_to(&mut self.memory, 0);
        self.facts.release_staging(&mut self.memory);
        self.program_staging.clear();
        self.sweep_selector_programs();
        self.shed_routing_for_detached_sheets();
    }

    /// Release a transaction taken through the bridge and reclaim atoms before the bridge installs
    /// a new primary view. The returned reclamation batch lets C++ purge its atom memos before any
    /// reclaimed identity can be reused.
    pub(super) fn release_transaction_and_sweep_atoms(&mut self, transaction: StyleTransaction) {
        self.release_transaction(transaction);
        self.sweep_style_atoms();
    }

    pub(super) fn collect_live_style_atoms(&self) -> (HashSet<StyleAtomID>, u64) {
        assert!(
            self.tree_staging.is_empty(),
            "style atom sweeping requires settled tree staging"
        );
        assert!(
            self.facts.staging_is_empty(),
            "style atom sweeping requires settled fact staging"
        );
        let mut atoms = HashSet::default();
        let mut visited = self.tree.collect_atoms(&mut atoms);
        visited += self.facts.collect_atoms(&mut atoms);
        visited += self.program.collect_atoms(&mut atoms);
        visited += self.programs.collect_atoms(&mut atoms);
        if !self.html_element_namespace.is_none() {
            atoms.insert(self.html_element_namespace);
        }
        (atoms, visited)
    }

    pub(super) fn sweep_style_atoms(&mut self) {
        let decision = self.atoms.sweep_decision();
        self.counters
            .add(Counter::AtomSweepPinReleasesSkipped, decision.skipped_pin_releases);
        if !decision.should_sweep && self.replay_reclaimed_style_atoms.is_none() {
            return;
        }
        if self.batch_matching_traversal.is_some() {
            self.counters.bump(Counter::AtomSweepsDeferredForActiveTraversal);
            return;
        }
        let replay_reclaimed = self.replay_reclaimed_style_atoms.take();
        self.style_atoms_swept = true;
        self.facts.sweep_auxiliary_catalogs_without_sync();
        let (mut live, visited) = self.collect_live_style_atoms();
        self.atoms.mark_sweep_dependencies(&mut live);
        loop {
            let previous_live_count = live.len();
            self.facts.extend_live_attribute_name_forms(&mut live);
            self.atoms.mark_sweep_dependencies(&mut live);
            if live.len() == previous_live_count {
                break;
            }
        }
        let mut reclaimable = self.atoms.reclaimable_for_sweep(&live);
        if let Some(recorded) = replay_reclaimed {
            assert!(
                recorded.iter().all(|atom| reclaimable.contains(atom)),
                "a recorded atom release still has a semantic replay owner"
            );
            reclaimable = recorded;
        }
        self.facts.forget_atoms(&reclaimable);
        let requirement_count = self.attribute_value_text_names.len();
        self.attribute_value_text_names
            .retain(|atom| !reclaimable.contains(atom));
        if self.attribute_value_text_names.len() != requirement_count {
            self.attribute_value_text_requirements_version += 1;
        }
        let reclaimed = self.atoms.finish_sweep(&reclaimable);
        self.counters.bump(Counter::AtomSweeps);
        self.counters.add(Counter::AtomSweepRootSlotsVisited, visited);
        self.counters.add(
            Counter::StyleAtomsReclaimed,
            u64::try_from(reclaimed.len()).expect("reclaimed atom count exceeds u64"),
        );
        self.reclaimed_style_atoms.extend(reclaimed);
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
        let mut previous_routing = std::mem::replace(&mut self.routing, Rc::new(rebuilt_routing));
        Rc::get_mut(&mut previous_routing)
            .expect("routing program is shared outside a planning epoch")
            .release_memory();
        Rc::get_mut(&mut self.routing)
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
        let mut previous_routing = std::mem::replace(&mut self.routing, Rc::new(rebuilt_routing));
        Rc::get_mut(&mut previous_routing)
            .expect("routing program is shared outside a planning epoch")
            .release_memory();
        Rc::get_mut(&mut self.routing)
            .expect("new routing program cannot be shared")
            .settle_memory(&mut self.memory);

        self.relational_witnesses.borrow_mut().clear_all();
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

    /// Drop the fact rows of the elements that left, now that nothing can still route from them.
    ///
    /// A retired identity is not reused until the epoch that could name it has retired, so a row
    /// left behind here would be read as the next occupant's. Dropping it at the transaction
    /// boundary keeps the row alive for exactly as long as routing needs it.
    pub(super) fn forget_departed_elements(&mut self) {
        let departed: Vec<_> = self
            .tree_staging
            .rows()
            .into_iter()
            .filter_map(|(node, _, after)| after.is_none().then_some(node))
            .collect();
        if departed.is_empty() {
            return;
        }
        for node in departed {
            self.facts.forget(node);
            self.retained_match_answers.forget(&mut self.match_answers, node);
            if let Some(tree_scope) = self.scope_by_root.remove(node) {
                self.scope_roots[tree_scope.0 as usize] = None;
            }
        }
        // The catalog sweep needs unique primary rows, like the atom sweep; while a traversal
        // borrows them the dead entries wait for the next boundary.
        if self.batch_matching_traversal.is_none() {
            self.facts.sweep_auxiliary_catalogs();
        }
    }

    /// The document budget is written in connected elements and compact program bytes, so it has to
    /// follow the live element count rather than a high-water mark.
    pub(super) fn publish_budget_inputs(&mut self) {
        let inputs = BudgetInputs {
            connected_element_count: self.tree.connected_element_count(),
            ..BudgetInputs::default()
        };
        self.memory.set_budget_inputs(inputs);
        self.journal
            .set_document_capacity_limit(self.tree.connected_element_count());
    }
}
