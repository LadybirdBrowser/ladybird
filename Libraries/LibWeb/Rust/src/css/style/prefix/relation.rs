/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Incremental selector-prefix membership, organized by selector step.
//!
//! Each step retains the elements matching its complete prefix. A changed local predicate or
//! predecessor membership propagates only to candidates reachable through the next combinator.
//! Terminal changes supply both the planner's signed selector truth and matching completion.

use super::super::capacity::ShallowCapacityBytes;
use super::super::fast_hash::FastSet as HashSet;
use super::super::selector::RoutingKey;
use super::{
    Column, Counter, Counters, DispatchKey, EntryID, HashMap, PrefixAutomaton, PrefixEvaluation, PrefixOutputKind,
    PrefixPredicate, PrefixStates, PrefixStepID, PrefixTransitionLookup, StyleNodeID, StyleNodeTree, matches_feature,
};

pub(in crate::css::style) struct PrefixRelation {
    // Membership positions are stable slots. Inserting or removing a node does not renumber
    // unrelated matches; retired slots are reused only after their memberships are removed.
    nodes: Vec<StyleNodeID>,
    positions: Column<usize>,
    parents: Vec<usize>,
    previous: Vec<usize>,
    live: Vec<bool>,
    free_slots: Vec<usize>,
    departures: Vec<usize>,
    compound_matches: Vec<Vec<usize>>,
    matches: Vec<Vec<u32>>,
    // Step identities are in dispatch order, so dependency order must be retained separately.
    queue: Vec<(usize, PrefixOutputKind)>,
    step_ranks: Vec<usize>,
    compound_steps: Vec<Vec<usize>>,
    pending_steps: PendingPrefixSteps,
    has_following_steps: bool,
    compounds_by_key: HashMap<DispatchKey, Vec<usize>>,
    positional: Vec<u32>,
    geometry_targets: [Vec<usize>; 4],
    old_previous: Vec<(usize, usize)>,
    sibling_order_is_preserved: bool,
    answers: Vec<Vec<EntryID>>,
    terminal_steps: HashMap<EntryID, Vec<usize>>,
    pub(in crate::css::style) changed_answers: Vec<(StyleNodeID, Vec<EntryID>, Vec<EntryID>)>,
    arrivals: Vec<usize>,
    pub(in crate::css::style) handled_routing_keys: HashMap<RoutingKey, bool>,
    // Routine prefix-cache accounting must not walk every selector set for each matching node.
    capacity_bytes: u64,
    nested_capacity_bytes: u64,
}

impl PrefixRelation {
    fn verify_answers(&self, evaluation: &PrefixEvaluation<'_, '_>) {
        if !cfg!(test) && !super::super::verification::prefix_relation_is_enabled() {
            return;
        }
        assert_eq!(self.nested_capacity_bytes, self.measure_nested_capacity_bytes());
        let mut scalar = PrefixStates::new(evaluation.facts.row_count());
        scalar.prepare_rows(evaluation.facts.generation(), evaluation.facts.row_count());
        let mut counters = Counters::default();
        for (position, &node) in self.nodes.iter().enumerate() {
            if !self.live[position] {
                continue;
            }
            let PrefixTransitionLookup::Known(answer) = scalar.match_set_for(evaluation, node, &mut counters) else {
                panic!("a complete prefix relation must have a complete scalar answer");
            };
            assert_eq!(
                self.answers[position],
                scalar.matches_in(answer),
                "prefix relation differs for {node:?}"
            );
        }
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        self.capacity_bytes
    }

    fn measure_nested_capacity_bytes(&self) -> u64 {
        self.compound_matches
            .iter()
            .map(ShallowCapacityBytes::shallow_capacity_bytes)
            .sum::<u64>()
            + self
                .matches
                .iter()
                .map(ShallowCapacityBytes::shallow_capacity_bytes)
                .sum::<u64>()
            + self
                .compound_steps
                .iter()
                .map(ShallowCapacityBytes::shallow_capacity_bytes)
                .sum::<u64>()
            + self
                .compounds_by_key
                .values()
                .map(ShallowCapacityBytes::shallow_capacity_bytes)
                .sum::<u64>()
            + self
                .answers
                .iter()
                .map(ShallowCapacityBytes::shallow_capacity_bytes)
                .sum::<u64>()
            + self
                .terminal_steps
                .values()
                .map(ShallowCapacityBytes::shallow_capacity_bytes)
                .sum::<u64>()
    }

    fn refresh_capacity_bytes(&mut self) {
        self.capacity_bytes = self.nested_capacity_bytes
            + self.pending_steps.capacity_bytes()
            + self.nodes.shallow_capacity_bytes()
            + self.positions.shallow_capacity_bytes()
            + self.parents.shallow_capacity_bytes()
            + self.previous.shallow_capacity_bytes()
            + self.live.shallow_capacity_bytes()
            + self.free_slots.shallow_capacity_bytes()
            + self.departures.shallow_capacity_bytes()
            + self.compound_matches.shallow_capacity_bytes()
            + self.matches.shallow_capacity_bytes()
            + self.queue.shallow_capacity_bytes()
            + self.step_ranks.shallow_capacity_bytes()
            + self.compound_steps.shallow_capacity_bytes()
            + self.compounds_by_key.shallow_capacity_bytes()
            + self.positional.shallow_capacity_bytes()
            + self.old_previous.shallow_capacity_bytes()
            + self
                .geometry_targets
                .iter()
                .map(ShallowCapacityBytes::shallow_capacity_bytes)
                .sum::<u64>()
            + self.answers.shallow_capacity_bytes()
            + self.terminal_steps.shallow_capacity_bytes()
            + self.changed_answers.shallow_capacity_bytes()
            + self
                .changed_answers
                .iter()
                .map(|(_, old, new)| old.shallow_capacity_bytes() + new.shallow_capacity_bytes())
                .sum::<u64>()
            + self.arrivals.shallow_capacity_bytes()
            + self.handled_routing_keys.shallow_capacity_bytes();
    }

    pub(in crate::css::style) fn install_answers(&self, states: &mut PrefixStates, counters: &mut Counters) {
        if states.relation_answers.is_empty() {
            for (position, (&node, entries)) in self.nodes.iter().zip(&self.answers).enumerate() {
                if self.live[position] {
                    states.install_relation_answer(node, entries, counters);
                }
            }
        } else {
            for (node, _, entries) in &self.changed_answers {
                states.install_relation_answer(*node, entries, counters);
            }
            for &position in &self.arrivals {
                states.install_relation_answer(self.nodes[position], &self.answers[position], counters);
            }
        }
    }

    fn position_of(&self, node: Option<StyleNodeID>) -> usize {
        node.and_then(|node| self.positions.get(node.raw() as usize))
            .copied()
            .unwrap_or(usize::MAX)
    }

    pub(in crate::css::style) fn update_geometry(
        &mut self,
        automaton: &PrefixAutomaton,
        evaluation: &PrefixEvaluation<'_, '_>,
        changed: &[StyleNodeID],
        counters: &mut Counters,
    ) -> Vec<StyleNodeID> {
        let tree = evaluation.tree;
        let mut touched = changed.to_vec();
        self.arrivals.clear();
        self.departures.clear();
        self.old_previous.clear();
        self.sibling_order_is_preserved = self.has_following_steps;
        for &node in changed {
            if !tree.is_live(node) || self.position_of(Some(node)) != usize::MAX {
                continue;
            }
            for node in tree.preorder(node) {
                if self.position_of(Some(node)) != usize::MAX {
                    continue;
                }
                let position = if let Some(position) = self.free_slots.pop() {
                    self.nodes[position] = node;
                    self.live[position] = true;
                    position
                } else {
                    self.nodes.push(node);
                    self.live.push(true);
                    self.parents.push(usize::MAX);
                    self.previous.push(usize::MAX);
                    self.positional.push(0);
                    self.answers.push(Vec::new());
                    self.nodes.len() - 1
                };
                self.positions.insert(node.raw() as usize, position);
                self.arrivals.push(position);
                touched.push(node);
            }
        }
        touched.sort_unstable();
        touched.dedup();
        self.arrivals.sort_unstable();
        let mut touched_parents = HashSet::default();
        let mut sibling_frontier = HashSet::default();
        let mut extra = Vec::new();
        for targets in &mut self.geometry_targets {
            targets.clear();
        }
        for &node in &touched {
            let position = self.position_of(Some(node));
            if position == usize::MAX {
                continue;
            }
            let old_parent = self.parents[position];
            if !tree.is_live(node) {
                self.live[position] = false;
                self.departures.push(position);
                extra.push(node);
                if old_parent != usize::MAX {
                    touched_parents.insert(old_parent);
                }
                continue;
            }
            let parent = self.position_of(tree.parent(node));
            let previous = self.position_of(tree.previous_element_sibling(node));
            if parent != old_parent {
                if old_parent != usize::MAX {
                    self.sibling_order_is_preserved = false;
                }
                self.geometry_targets[0].push(position);
                if old_parent != usize::MAX {
                    let descendants: Vec<_> = tree.preorder(node).map(|node| self.position_of(Some(node))).collect();
                    self.geometry_targets[1].extend(descendants);
                }
                if old_parent != usize::MAX {
                    touched_parents.insert(old_parent);
                }
                if parent != usize::MAX {
                    touched_parents.insert(parent);
                }
            }
            if parent != old_parent || previous != self.previous[position] {
                if self.has_following_steps {
                    self.old_previous.push((position, self.previous[position]));
                }
                self.geometry_targets[2].push(position);
                sibling_frontier.insert(position);
                if parent != usize::MAX {
                    touched_parents.insert(parent);
                }
            }
            self.parents[position] = parent;
            self.previous[position] = previous;
        }
        if self.sibling_order_is_preserved {
            self.sibling_order_is_preserved = self.old_previous.iter().all(|&(position, mut previous)| {
                if self.arrivals.binary_search(&position).is_ok() {
                    return true;
                }
                // Departed slots still hold their old links. Arrivals hold only current links.
                // Removing both from the comparison leaves the order of retained siblings.
                while previous != usize::MAX && !self.live[previous] {
                    previous = self.previous[previous];
                }
                let mut current_previous = self.previous[position];
                while current_previous != usize::MAX && self.arrivals.binary_search(&current_previous).is_ok() {
                    current_previous = self.previous[current_previous];
                }
                previous == current_previous
            });
        }
        let has_positional_tests = !automaton.positional_tests().is_empty();
        for &parent in &touched_parents {
            if !self.live[parent] || !has_positional_tests {
                continue;
            }
            extra.push(self.nodes[parent]);
            for node in tree.children(self.nodes[parent]) {
                let position = self.position_of(Some(node));
                assert_ne!(position, usize::MAX);
                let bits = evaluation.positional_bits(node, counters).unwrap();
                if bits != self.positional[position] {
                    extra.push(node);
                    self.positional[position] = bits;
                }
            }
        }
        if self.has_following_steps && !self.sibling_order_is_preserved {
            for parent in touched_parents {
                if !self.live[parent] {
                    continue;
                }
                let mut follows_frontier = false;
                for node in tree.children(self.nodes[parent]) {
                    let position = self.position_of(Some(node));
                    follows_frontier |= sibling_frontier.contains(&position);
                    if follows_frontier {
                        self.geometry_targets[3].push(position);
                    }
                }
            }
        }
        extra.extend(self.arrivals.iter().map(|&position| self.nodes[position]));
        extra
    }

    // Inserting nodes cannot remove a following-sibling witness. When retained siblings keep
    // their order, only a removed witness can change this context, and only until another old
    // witness survives. New memberships are propagated separately through the current tree.
    fn following_geometry_changes(
        &self,
        automaton: &PrefixAutomaton,
        old_evaluation: &PrefixEvaluation<'_, '_>,
    ) -> HashMap<usize, Vec<usize>> {
        let mut result: HashMap<usize, Vec<usize>> = HashMap::default();
        if !self.sibling_order_is_preserved {
            return result;
        }
        let mut removed_steps = Vec::new();
        for &(frontier, old_previous) in &self.old_previous {
            if self.arrivals.binary_search(&frontier).is_ok() {
                continue;
            }
            removed_steps.clear();
            let mut removed = old_previous;
            while removed != usize::MAX && !self.live[removed] {
                let row = old_evaluation.row_of(self.nodes[removed]).unwrap();
                row.facts.for_each_dispatch_key(row.row, false, |key| {
                    if let Some(compounds) = self.compounds_by_key.get(&key) {
                        for &compound in compounds {
                            for &step in &self.compound_steps[compound] {
                                if self.matches[step].binary_search(&(removed as u32)).is_ok()
                                    && automaton
                                        .outputs_for(&automaton.steps[step])
                                        .iter()
                                        .any(|output| matches!(output.kind, PrefixOutputKind::FollowingSibling))
                                {
                                    removed_steps.push(step);
                                }
                            }
                        }
                    }
                });
                removed = self.previous[removed];
            }
            removed_steps.sort_unstable();
            removed_steps.dedup();
            for &step in &removed_steps {
                let members = &self.matches[step];
                let contains = |position: usize| members.binary_search(&(position as u32)).is_ok();
                let mut previous = self.previous[frontier];
                while previous != usize::MAX && !contains(previous) {
                    previous = self.previous[previous];
                }
                if previous != usize::MAX {
                    continue;
                }
                let mut node = Some(self.nodes[frontier]);
                while let Some(current) = node {
                    let position = self.position_of(Some(current));
                    for output in automaton.outputs_for(&automaton.steps[step]) {
                        // Newly matching local predicates seed their own step changes below.
                        // Geometry only needs the candidates which matched before this batch.
                        if matches!(output.kind, PrefixOutputKind::FollowingSibling)
                            && self.compound_matches[automaton.steps[output.target as usize].compound.0 as usize]
                                .binary_search(&position)
                                .is_ok()
                        {
                            result.entry(output.target as usize).or_default().push(position);
                        }
                    }
                    if contains(position) {
                        break;
                    }
                    node = old_evaluation.tree.next_element_sibling(current);
                }
            }
        }
        result
    }

    pub(in crate::css::style) fn update(
        &mut self,
        automaton: &PrefixAutomaton,
        evaluation: &PrefixEvaluation<'_, '_>,
        old_evaluation: &PrefixEvaluation<'_, '_>,
        changed_nodes: &[StyleNodeID],
        counters: &mut Counters,
    ) {
        counters.bump(Counter::PrefixRelationUpdates);
        let following_geometry = self.following_geometry_changes(automaton, old_evaluation);
        let mut changed_compounds: HashMap<usize, Vec<usize>> = HashMap::default();
        let mut keys = Vec::new();
        // Departed nodes need no terminal delta or selector re-evaluation. Remove their positive
        // memberships once per affected set; live siblings are handled by the geometry frontier.
        // Sharing the dispatch-key lookup across the batch avoids testing every possible local
        // compound again for every departed element.
        for &position in &self.departures {
            let node = self.nodes[position];
            let row = old_evaluation.row_of(node).unwrap();
            row.facts
                .for_each_dispatch_key(row.row, self.parents[position] == usize::MAX, |key| keys.push(key));
        }
        keys.sort_unstable();
        keys.dedup();
        for key in &keys {
            let Some(compounds) = self.compounds_by_key.get(key) else {
                continue;
            };
            for &compound in compounds {
                let members = &mut self.compound_matches[compound];
                let before = members.len();
                members.retain(|&position| self.live[position]);
                if members.len() == before {
                    continue;
                }
                for &step in &self.compound_steps[compound] {
                    self.matches[step].retain(|&position| self.live[position as usize]);
                }
            }
        }
        for &node in changed_nodes {
            let Some(&position) = node
                .element_index()
                .and_then(|index| self.positions.get(index as usize))
            else {
                continue;
            };
            if position == usize::MAX || !self.live[position] {
                continue;
            }
            let row = evaluation.row_of(node);
            let previous_row = old_evaluation.row_of(node);
            let row = row.or(previous_row).unwrap();
            let previous_row = previous_row.unwrap_or(row);
            keys.clear();
            for row in [row, previous_row] {
                row.facts
                    .for_each_dispatch_key(row.row, evaluation.tree.parent(node).is_none(), |key| keys.push(key));
            }
            keys.sort_unstable();
            keys.dedup();
            let positional = if self.live[position] {
                evaluation.positional_bits(node, counters).unwrap()
            } else {
                0
            };
            self.positional[position] = positional;
            for key in &keys {
                let Some(compounds) = self.compounds_by_key.get(key) else {
                    continue;
                };
                for &index in compounds {
                    let compound = &automaton.compounds[index];
                    counters.bump(Counter::PrefixCompoundsEvaluated);
                    let matched = self.live[position]
                        && match &compound.predicate {
                            PrefixPredicate::Features {
                                feature_start,
                                feature_len,
                                required_positional_bits,
                            } => {
                                positional & required_positional_bits == *required_positional_bits
                                    && automaton
                                        .features_for(*feature_start, *feature_len)
                                        .iter()
                                        .all(|&feature| matches_feature(row.facts, row.row, feature))
                            }
                            PrefixPredicate::Program { program, local } => evaluation
                                .evaluator
                                .matches_prefix_local(
                                    *program,
                                    evaluation.programs.get(*program),
                                    *local,
                                    node,
                                    counters,
                                )
                                .unwrap(),
                        };
                    if self.compound_matches[index].binary_search(&position).is_ok() != matched {
                        changed_compounds.entry(index).or_default().push(position);
                    }
                }
            }
        }
        for (&index, changes) in &mut changed_compounds {
            changes.sort_unstable();
            changes.dedup();
            let members = &mut self.compound_matches[index];
            let before = members.shallow_capacity_bytes();
            toggle_members(members, changes);
            self.nested_capacity_bytes += members.shallow_capacity_bytes() - before;
        }
        let mut geometry_memberships: [HashMap<usize, Vec<usize>>; 4] = std::array::from_fn(|_| HashMap::default());
        for (axis, targets) in self.geometry_targets.iter().enumerate() {
            for &position in targets {
                if !self.live[position] {
                    continue;
                }
                let node = self.nodes[position];
                let row = evaluation.row_of(node).unwrap();
                row.facts
                    .for_each_dispatch_key(row.row, evaluation.tree.parent(node).is_none(), |key| {
                        if let Some(compounds) = self.compounds_by_key.get(&key) {
                            for &compound in compounds {
                                if self.compound_matches[compound].binary_search(&position).is_ok() {
                                    geometry_memberships[axis].entry(compound).or_default().push(position);
                                }
                            }
                        }
                    });
            }
        }
        for compound in changed_compounds.keys().copied().chain(
            geometry_memberships
                .iter()
                .flat_map(|compounds| compounds.keys().copied()),
        ) {
            for &step in &self.compound_steps[compound] {
                self.pending_steps.insert(self.step_ranks[step]);
            }
        }
        for &step in following_geometry.keys() {
            self.pending_steps.insert(self.step_ranks[step]);
        }
        let mut step_changes: HashMap<usize, Vec<usize>> = HashMap::default();
        let mut terminal_changes = Vec::new();
        let mut affected = Vec::new();
        let mut following_parents = HashSet::default();
        let mut ancestor_truth: HashMap<usize, bool> = HashMap::default();
        let mut ancestor_chain = Vec::new();
        let mut sibling_truth: HashMap<usize, bool> = HashMap::default();
        // Every local change is seeded before evaluation. Dependency order ensures that a step
        // runs once, after all changes to its predecessor, and only propagates a changed result.
        while let Some(rank) = self.pending_steps.pop_first() {
            let (step_index, axis) = self.queue[rank];
            let step = &automaton.steps[step_index];
            let compound_index = step.compound.0 as usize;
            let predecessor = automaton.predecessor_of(PrefixStepID(step_index as u32));
            if predecessor.is_some_and(|predecessor| self.matches[predecessor.0 as usize].is_empty()) {
                // No predecessor witness can satisfy any outgoing combinator. Remove the old
                // matches directly without evaluating local predicates or changed geometry.
                affected.clear();
                affected.extend(self.matches[step_index].drain(..).map(|position| position as usize));
            } else {
                let predecessor_changes = predecessor
                    .and_then(|predecessor| step_changes.get(&(predecessor.0 as usize)))
                    .map_or(&[][..], Vec::as_slice);
                let geometry = match axis {
                    PrefixOutputKind::Child => geometry_memberships[0]
                        .get(&compound_index)
                        .map_or(&[][..], Vec::as_slice),
                    PrefixOutputKind::Descendant => geometry_memberships[1]
                        .get(&compound_index)
                        .map_or(&[][..], Vec::as_slice),
                    PrefixOutputKind::NextSibling => geometry_memberships[2]
                        .get(&compound_index)
                        .map_or(&[][..], Vec::as_slice),
                    PrefixOutputKind::FollowingSibling => {
                        if self.sibling_order_is_preserved {
                            following_geometry.get(&step_index).map_or(&[][..], Vec::as_slice)
                        } else {
                            geometry_memberships[3]
                                .get(&compound_index)
                                .map_or(&[][..], Vec::as_slice)
                        }
                    }
                    _ => &[],
                };
                let compound_changes = changed_compounds.get(&compound_index).map_or(&[][..], Vec::as_slice);
                if compound_changes.is_empty() && predecessor_changes.is_empty() && geometry.is_empty() {
                    continue;
                }
                let candidates = &self.compound_matches[compound_index];
                affected.clear();
                affected.extend_from_slice(compound_changes);
                affected.extend_from_slice(geometry);
                if !predecessor_changes.is_empty() {
                    match axis {
                        PrefixOutputKind::Child => {
                            for &source in predecessor_changes {
                                for child in evaluation.tree.children(self.nodes[source]) {
                                    let position = self.positions[child.element_index().unwrap() as usize];
                                    if candidates.binary_search(&position).is_ok() {
                                        affected.push(position);
                                    }
                                }
                            }
                        }
                        PrefixOutputKind::NextSibling => {
                            for &source in predecessor_changes {
                                if let Some(next) = evaluation.tree.next_element_sibling(self.nodes[source]) {
                                    let position = self.positions[next.element_index().unwrap() as usize];
                                    if candidates.binary_search(&position).is_ok() {
                                        affected.push(position);
                                    }
                                }
                            }
                        }
                        PrefixOutputKind::Descendant => {
                            // Enumerate a small changed subtree directly. If it is larger than
                            // the candidate set, test those candidates against the changed roots.
                            // The bound is the other join input's size, not a tuned batch cutoff.
                            let mut descendants = Vec::new();
                            let mut complete = true;
                            'sources: for &source in predecessor_changes {
                                if !self.live[source] {
                                    continue;
                                }
                                for node in evaluation.tree.preorder(self.nodes[source]).skip(1) {
                                    if descendants.len() == candidates.len() {
                                        complete = false;
                                        break 'sources;
                                    }
                                    descendants.push(self.position_of(Some(node)));
                                }
                            }
                            if complete {
                                affected.extend(
                                    descendants
                                        .into_iter()
                                        .filter(|position| candidates.binary_search(position).is_ok()),
                                );
                            } else {
                                ancestor_truth.clear();
                                for &position in candidates {
                                    ancestor_chain.clear();
                                    let mut source = self.parents[position];
                                    let mut found = false;
                                    while source != usize::MAX {
                                        if predecessor_changes.binary_search(&source).is_ok() {
                                            found = true;
                                            break;
                                        }
                                        if let Some(&truth) = ancestor_truth.get(&source) {
                                            found = truth;
                                            break;
                                        }
                                        ancestor_chain.push(source);
                                        source = self.parents[source];
                                    }
                                    for &source in &ancestor_chain {
                                        ancestor_truth.insert(source, found);
                                    }
                                    if found {
                                        affected.push(position);
                                    }
                                }
                            }
                        }
                        PrefixOutputKind::FollowingSibling => {
                            following_parents.clear();
                            for &source in predecessor_changes {
                                if self.parents[source] != usize::MAX {
                                    following_parents.insert(self.parents[source]);
                                }
                            }
                            for &parent in &following_parents {
                                let mut follows_source = false;
                                for node in evaluation.tree.children(self.nodes[parent]) {
                                    let position = self.position_of(Some(node));
                                    if follows_source && candidates.binary_search(&position).is_ok() {
                                        affected.push(position);
                                    }
                                    follows_source |= predecessor_changes.binary_search(&position).is_ok();
                                }
                            }
                        }
                        _ => unreachable!(),
                    }
                }
                affected.sort_unstable();
                affected.dedup();
                if affected.is_empty() {
                    continue;
                }
                ancestor_truth.clear();
                sibling_truth.clear();
                let mut changes = Vec::new();
                for &position in &affected {
                    let mut matched = candidates.binary_search(&position).is_ok();
                    if matched && let Some(predecessor) = predecessor {
                        let predecessor = &self.matches[predecessor.0 as usize];
                        let contains = |position: usize| predecessor.binary_search(&(position as u32)).is_ok();
                        matched = match axis {
                            PrefixOutputKind::Child | PrefixOutputKind::NextSibling => {
                                let source = if matches!(axis, PrefixOutputKind::Child) {
                                    self.parents[position]
                                } else {
                                    self.previous[position]
                                };
                                source != usize::MAX && contains(source)
                            }
                            PrefixOutputKind::Descendant => {
                                ancestor_chain.clear();
                                let mut current = self.parents[position];
                                let mut found = false;
                                while current != usize::MAX {
                                    if contains(current) {
                                        found = true;
                                        break;
                                    }
                                    if let Some(&truth) = ancestor_truth.get(&current) {
                                        found = truth;
                                        break;
                                    }
                                    ancestor_chain.push(current);
                                    current = self.parents[current];
                                }
                                for &ancestor in &ancestor_chain {
                                    ancestor_truth.insert(ancestor, found);
                                }
                                found
                            }
                            PrefixOutputKind::FollowingSibling => {
                                ancestor_chain.clear();
                                let mut source = self.previous[position];
                                let mut found = false;
                                while source != usize::MAX {
                                    if contains(source) {
                                        found = true;
                                        break;
                                    }
                                    if let Some(&truth) = sibling_truth.get(&source) {
                                        found = truth;
                                        break;
                                    }
                                    ancestor_chain.push(source);
                                    source = self.previous[source];
                                }
                                for &source in &ancestor_chain {
                                    sibling_truth.insert(source, found);
                                }
                                found
                            }
                            _ => unreachable!(),
                        };
                    }
                    if self.matches[step_index].binary_search(&(position as u32)).is_ok() != matched {
                        changes.push(position);
                    }
                }
                let members = &mut self.matches[step_index];
                let before = members.shallow_capacity_bytes();
                toggle_members(members, &changes);
                self.nested_capacity_bytes += members.shallow_capacity_bytes() - before;
                affected = changes;
            }
            if affected.is_empty() {
                counters.bump(Counter::PrefixRelationStops);
                continue;
            }
            for successor in automaton.outputs_for(step) {
                match successor.kind {
                    PrefixOutputKind::UniqueTerminal | PrefixOutputKind::SharedTerminal => {
                        for &position in &affected {
                            terminal_changes.push((position, EntryID(successor.target)));
                        }
                    }
                    _ => {
                        let successor = successor.target as usize;
                        self.pending_steps.insert(self.step_ranks[successor]);
                    }
                }
            }
            step_changes.insert(step_index, std::mem::take(&mut affected));
        }
        terminal_changes.sort_unstable();
        terminal_changes.dedup();
        self.changed_answers.clear();
        let mut cursor = 0;
        while cursor < terminal_changes.len() {
            let position = terminal_changes[cursor].0;
            let old = self.answers[position].clone();
            while cursor < terminal_changes.len() && terminal_changes[cursor].0 == position {
                let entry = terminal_changes[cursor].1;
                // Multiple paths can produce the same terminal. Losing one path changes the
                // selector answer only when no other path still matches this element.
                let matched = self.terminal_steps[&entry]
                    .iter()
                    .any(|&step| self.matches[step].binary_search(&(position as u32)).is_ok());
                let entries = &mut self.answers[position];
                match (entries.binary_search(&entry), matched) {
                    (Ok(index), false) => {
                        entries.remove(index);
                    }
                    (Err(index), true) => {
                        let before = entries.shallow_capacity_bytes();
                        entries.insert(index, entry);
                        self.nested_capacity_bytes += entries.shallow_capacity_bytes() - before;
                    }
                    _ => {}
                }
                cursor += 1;
            }
            if self.live[position] && old != self.answers[position] {
                self.changed_answers
                    .push((self.nodes[position], old, self.answers[position].clone()));
            }
        }
        for targets in &mut self.geometry_targets {
            targets.clear();
        }
        for &position in &self.departures {
            self.answers[position].clear();
            self.positions[self.nodes[position].raw() as usize] = usize::MAX;
            self.parents[position] = usize::MAX;
            self.previous[position] = usize::MAX;
            self.positional[position] = 0;
            self.free_slots.push(position);
        }
        self.departures.clear();
        self.old_previous.clear();
        self.refresh_capacity_bytes();
        self.verify_answers(evaluation);
    }
}

impl PrefixAutomaton {
    pub(in crate::css::style) fn supports_relation(&self, tree: &StyleNodeTree, root: StyleNodeID) -> bool {
        !self.steps.is_empty()
            && tree.parent(root).is_none()
            && tree
                .preorder(root)
                .all(|node| tree.tree_scope(node) == super::super::tree::TreeScopeID::DOCUMENT)
    }

    pub(in crate::css::style) fn build_relation(
        &self,
        evaluation: &PrefixEvaluation<'_, '_>,
        root: StyleNodeID,
        counters: &mut Counters,
    ) -> PrefixRelation {
        let tree = evaluation.tree;
        let nodes: Vec<_> = tree.preorder(root).collect();
        let count = nodes.len();
        let mut positions = Column::new(|| usize::MAX);
        let mut rows = Vec::with_capacity(count);
        for (position, &node) in nodes.iter().enumerate() {
            positions.insert(node.element_index().unwrap() as usize, position);
            rows.push(evaluation.row_of(node).unwrap());
        }
        let position_of = |node: Option<StyleNodeID>| {
            node.and_then(|node| node.element_index())
                .map_or(usize::MAX, |index| positions[index as usize])
        };
        let parents: Vec<_> = nodes.iter().map(|&node| position_of(tree.parent(node))).collect();
        let previous: Vec<_> = nodes
            .iter()
            .map(|&node| position_of(tree.previous_element_sibling(node)))
            .collect();
        let mut subtree_ends: Vec<_> = (1..=count).collect();
        for position in (0..count).rev() {
            if parents[position] != usize::MAX {
                subtree_ends[parents[position]] = subtree_ends[parents[position]].max(subtree_ends[position]);
            }
        }
        let mut candidates: HashMap<DispatchKey, Vec<usize>> = HashMap::default();
        for compound in &self.compounds {
            candidates.entry(compound.dispatch_key).or_default();
        }
        for (position, row) in rows.iter().enumerate() {
            row.facts
                .for_each_dispatch_key(row.row, tree.parent(nodes[position]).is_none(), |key| {
                    if let Some(candidates) = candidates.get_mut(&key)
                        && candidates.last() != Some(&position)
                    {
                        candidates.push(position);
                    }
                });
        }
        let positional: Vec<_> = nodes
            .iter()
            .map(|&node| evaluation.positional_bits(node, counters).unwrap())
            .collect();
        let mut compound_matches = Vec::with_capacity(self.compounds.len());
        for compound in &self.compounds {
            let matched: Vec<_> = candidates[&compound.dispatch_key]
                .iter()
                .copied()
                .filter(|&position| {
                    counters.bump(Counter::PrefixCompoundsEvaluated);
                    let row = rows[position];
                    match &compound.predicate {
                        PrefixPredicate::Features {
                            feature_start,
                            feature_len,
                            required_positional_bits,
                        } => {
                            positional[position] & required_positional_bits == *required_positional_bits
                                && self
                                    .features_for(*feature_start, *feature_len)
                                    .iter()
                                    .all(|&feature| matches_feature(row.facts, row.row, feature))
                        }
                        PrefixPredicate::Program { program, local } => evaluation
                            .evaluator
                            .matches_prefix_local(
                                *program,
                                evaluation.programs.get(*program),
                                *local,
                                nodes[position],
                                counters,
                            )
                            .unwrap(),
                    }
                })
                .collect();
            compound_matches.push(matched);
        }
        let mut matches: Vec<Vec<u32>> = vec![Vec::new(); self.steps.len()];
        let mut queue: Vec<_> = (0..self.steps.len())
            .filter(|&step| self.predecessor_of(PrefixStepID(step as u32)).is_none())
            .map(|step| (step, PrefixOutputKind::UniqueTerminal))
            .collect();
        let mut cursor = 0;
        let mut selected = Vec::new();
        let mut first_following = vec![usize::MAX; count];
        let mut following_parents = Vec::new();
        let mut output: Vec<Vec<EntryID>> = vec![Vec::new(); count];
        while cursor < queue.len() {
            let (step_index, axis) = queue[cursor];
            cursor += 1;
            let step = &self.steps[step_index];
            let candidates = &compound_matches[step.compound.0 as usize];
            selected.clear();
            if let Some(predecessor) = self.predecessor_of(PrefixStepID(step_index as u32)) {
                let predecessor = &matches[predecessor.0 as usize];
                match axis {
                    _ if predecessor.is_empty() => {}
                    PrefixOutputKind::Child | PrefixOutputKind::NextSibling => {
                        let sources = if matches!(axis, PrefixOutputKind::Child) {
                            &parents
                        } else {
                            &previous
                        };
                        selected.extend(candidates.iter().copied().filter(|&position| {
                            let source = sources[position];
                            source != usize::MAX && predecessor.binary_search(&(source as u32)).is_ok()
                        }));
                    }
                    PrefixOutputKind::Descendant => {
                        let mut sources = predecessor.iter().map(|&position| position as usize).peekable();
                        let mut covered_end = 0;
                        for &position in candidates {
                            while let Some(&source) = sources.peek() {
                                if source >= position {
                                    break;
                                }
                                covered_end = covered_end.max(subtree_ends[source]);
                                sources.next();
                            }
                            if position < covered_end {
                                selected.push(position);
                            }
                        }
                    }
                    PrefixOutputKind::FollowingSibling => {
                        following_parents.clear();
                        for source in predecessor.iter().map(|&position| position as usize) {
                            let parent = parents[source];
                            if parent != usize::MAX && first_following[parent] == usize::MAX {
                                first_following[parent] = source;
                                following_parents.push(parent);
                            }
                        }
                        selected.extend(candidates.iter().copied().filter(|&position| {
                            let parent = parents[position];
                            parent != usize::MAX && first_following[parent] < position
                        }));
                        for &parent in &following_parents {
                            first_following[parent] = usize::MAX;
                        }
                    }
                    _ => unreachable!(),
                }
            } else {
                selected.extend_from_slice(candidates);
            }
            matches[step_index].extend(selected.iter().map(|&position| position as u32));
            for successor in self.outputs_for(step) {
                match successor.kind {
                    PrefixOutputKind::UniqueTerminal | PrefixOutputKind::SharedTerminal => {
                        for &position in &selected {
                            output[position].push(EntryID(successor.target));
                        }
                    }
                    axis => queue.push((successor.target as usize, axis)),
                }
            }
        }
        assert_eq!(queue.len(), self.steps.len());
        for entries in &mut output {
            entries.sort_unstable();
            entries.dedup();
        }
        let mut compounds_by_key: HashMap<DispatchKey, Vec<usize>> = HashMap::default();
        for (index, compound) in self.compounds.iter().enumerate() {
            compounds_by_key.entry(compound.dispatch_key).or_default().push(index);
        }
        let mut terminal_steps: HashMap<EntryID, Vec<usize>> = HashMap::default();
        for (index, step) in self.steps.iter().enumerate() {
            for output in self.outputs_for(step) {
                if matches!(
                    output.kind,
                    PrefixOutputKind::UniqueTerminal | PrefixOutputKind::SharedTerminal
                ) {
                    terminal_steps.entry(EntryID(output.target)).or_default().push(index);
                }
            }
        }
        let mut step_ranks = vec![0; self.steps.len()];
        let mut compound_steps = vec![Vec::new(); self.compounds.len()];
        for (rank, &(step, _)) in queue.iter().enumerate() {
            step_ranks[step] = rank;
            compound_steps[self.steps[step].compound.0 as usize].push(step);
        }
        let has_following_steps = queue
            .iter()
            .any(|(_, axis)| matches!(axis, PrefixOutputKind::FollowingSibling));
        let mut relation = PrefixRelation {
            nodes,
            positions,
            parents,
            previous,
            live: vec![true; count],
            free_slots: Vec::new(),
            departures: Vec::new(),
            compound_matches,
            matches,
            queue,
            step_ranks,
            compound_steps,
            pending_steps: PendingPrefixSteps::new(self.steps.len()),
            has_following_steps,
            compounds_by_key,
            positional,
            geometry_targets: std::array::from_fn(|_| Vec::new()),
            old_previous: Vec::new(),
            sibling_order_is_preserved: false,
            answers: output,
            terminal_steps,
            changed_answers: Vec::new(),
            arrivals: Vec::new(),
            handled_routing_keys: HashMap::default(),
            capacity_bytes: 0,
            nested_capacity_bytes: 0,
        };
        relation.nested_capacity_bytes = relation.measure_nested_capacity_bytes();
        relation.refresh_capacity_bytes();
        relation.verify_answers(evaluation);
        relation
    }
}

// Merge a batch of membership flips once. Repeated Vec::insert/remove would move the
// unaffected tail once per changed element, multiplying batch size by the set's population.
fn toggle_members<T: Copy + Ord + TryFrom<usize>>(members: &mut Vec<T>, changes: &[usize])
where
    T::Error: std::fmt::Debug,
{
    let convert = |position| T::try_from(position).unwrap();
    match changes {
        [] => return,
        &[position] => {
            let position = convert(position);
            match members.binary_search(&position) {
                Ok(index) => {
                    members.remove(index);
                }
                Err(index) => members.insert(index, position),
            }
            return;
        }
        _ => {}
    }
    let first = members.partition_point(|&position| position < convert(changes[0]));
    let end = members.partition_point(|&position| position <= convert(*changes.last().unwrap()));
    let mut replacement = Vec::new();
    let mut cursor = first;
    for &position in changes {
        let position = convert(position);
        while cursor < end && members[cursor] < position {
            replacement.push(members[cursor]);
            cursor += 1;
        }
        if cursor < end && members[cursor] == position {
            cursor += 1;
        } else {
            replacement.push(position);
        }
    }
    replacement.extend_from_slice(&members[cursor..end]);
    members.splice(first..end, replacement);
}

// Each upper bit names a nonempty word in the level below. Popping drains the marks, so
// subsequent transactions neither clear the whole program nor scan empty rank ranges.
struct PendingPrefixSteps {
    levels: Vec<Vec<u64>>,
}

impl PendingPrefixSteps {
    fn new(mut count: usize) -> Self {
        let mut levels = Vec::new();
        loop {
            count = count.max(1).div_ceil(u64::BITS as usize);
            levels.push(vec![0; count]);
            if count == 1 {
                return Self { levels };
            }
        }
    }

    fn insert(&mut self, mut rank: usize) {
        for level in &mut self.levels {
            let word = &mut level[rank / u64::BITS as usize];
            let was_empty = *word == 0;
            *word |= 1 << (rank % u64::BITS as usize);
            if !was_empty {
                break;
            }
            rank /= u64::BITS as usize;
        }
    }

    fn pop_first(&mut self) -> Option<usize> {
        if self.levels.last().unwrap()[0] == 0 {
            return None;
        }
        let mut rank = 0;
        for level in self.levels.iter().rev() {
            rank = rank * u64::BITS as usize + level[rank].trailing_zeros() as usize;
        }
        let result = rank;
        for level in &mut self.levels {
            let word = &mut level[rank / u64::BITS as usize];
            *word &= !(1 << (rank % u64::BITS as usize));
            if *word != 0 {
                break;
            }
            rank /= u64::BITS as usize;
        }
        Some(result)
    }

    fn capacity_bytes(&self) -> u64 {
        self.levels.shallow_capacity_bytes()
            + self
                .levels
                .iter()
                .map(ShallowCapacityBytes::shallow_capacity_bytes)
                .sum::<u64>()
    }
}

#[cfg(test)]
mod tests {
    use super::{PendingPrefixSteps, toggle_members};

    #[test]
    fn batched_membership_flips_preserve_the_unchanged_ranges() {
        let mut members = vec![1_u32, 3, 5, 7, 9];
        toggle_members(&mut members, &[0, 3, 4, 7, 8]);
        assert_eq!(members, [0, 1, 4, 5, 8, 9]);
        toggle_members(&mut members, &[0, 3, 4, 7, 8]);
        assert_eq!(members, [1, 3, 5, 7, 9]);
        toggle_members(&mut members, &[1, 3, 5, 7, 9]);
        assert!(members.is_empty());
        toggle_members(&mut members, &[2, 4, 6]);
        assert_eq!(members, [2, 4, 6]);
    }

    #[test]
    fn pending_prefix_steps_order_deduplicate_and_drain_across_word_boundaries() {
        let mut pending = PendingPrefixSteps::new(65_537);
        for _ in 0..3 {
            assert_eq!(pending.pop_first(), None);
            for rank in [65_536, 4_096, 63, 64, 0, 4_095, 64, 65_535] {
                pending.insert(rank);
            }
            for rank in [0, 63, 64, 4_095, 4_096, 65_535, 65_536] {
                assert_eq!(pending.pop_first(), Some(rank));
                // A dependency can schedule another step while the queue is draining.
                if rank == 64 {
                    pending.insert(65);
                    pending.insert(4_095);
                    assert_eq!(pending.pop_first(), Some(65));
                }
            }
            assert_eq!(pending.pop_first(), None);
        }
    }
}
