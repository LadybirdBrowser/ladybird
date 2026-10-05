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

use smallvec::SmallVec;

use super::super::capacity::ShallowCapacityBytes;
use super::super::fast_hash::FastSet as HashSet;
use super::super::selector::RoutingKey;
use super::super::tree::TreeScopeID;
use super::{
    Column, Counter, Counters, DispatchKey, EntryID, HashMap, PrefixAutomaton, PrefixEvaluation, PrefixOutputKind,
    PrefixPredicate, PrefixStates, PrefixStepID, PrefixTransitionLookup, StyleNodeID, StyleNodeTree, matches_feature,
};

// This is derived only from the immutable automaton. Memberships and pending edits stay in
// PrefixRelation, while every scope using that automaton shares its traversal and lookup tables.
pub(super) struct PrefixRelationProgram {
    // Step identities are in dispatch order, so retain their dependency order separately.
    queue: Vec<QueuedStep>,
    step_ranks: Vec<u32>,
    compound_step_offsets: Vec<u32>,
    compound_steps: Vec<u32>,
    has_following_steps: bool,
    compounds_by_key: HashMap<DispatchKey, std::ops::Range<u32>>,
    keyed_compounds: Vec<u32>,
    terminal_steps: HashMap<EntryID, SmallVec<[usize; 1]>>,
    memory: super::super::memory::MemoryLease,
}

/// The ledger relation programs are charged to, once each however many scopes share one. It has its
/// own lock so that building a relation never holds the dispatch pools.
static RELATION_PROGRAM_MEMORY: std::sync::LazyLock<std::sync::Mutex<super::super::memory::MemoryController>> =
    std::sync::LazyLock::new(|| std::sync::Mutex::new(super::super::memory::MemoryController::new()));

/// A step in dependency order, with what an update reads of it as it runs: how it is reached, its compound and its
/// predecessor (`u32::MAX` for none). Updates run steps in this order, so they read these one after another.
#[derive(Clone, Copy)]
struct QueuedStep {
    step: u32,
    axis: PrefixOutputKind,
    compound: u32,
    predecessor: u32,
}

impl PrefixRelationProgram {
    fn capacity_bytes(&self) -> u64 {
        self.queue.shallow_capacity_bytes()
            + self.step_ranks.shallow_capacity_bytes()
            + self.compound_step_offsets.shallow_capacity_bytes()
            + self.compound_steps.shallow_capacity_bytes()
            + self.compounds_by_key.shallow_capacity_bytes()
            + self.terminal_steps.shallow_capacity_bytes()
            + self.keyed_compounds.shallow_capacity_bytes()
            + self
                .terminal_steps
                .values()
                .filter(|steps| steps.spilled())
                .map(|steps| (steps.capacity() * size_of::<usize>()) as u64)
                .sum::<u64>()
    }

    fn compounds_for_key(&self, key: &DispatchKey) -> Option<impl Iterator<Item = usize> + '_> {
        let range = self.compounds_by_key.get(key)?;
        Some(
            self.keyed_compounds[range.start as usize..range.end as usize]
                .iter()
                .map(|&index| index as usize),
        )
    }

    fn steps_for_compound(&self, compound: usize) -> impl Iterator<Item = usize> + '_ {
        self.compound_steps
            [self.compound_step_offsets[compound] as usize..self.compound_step_offsets[compound + 1] as usize]
            .iter()
            .map(|&step| step as usize)
    }
}

/// The positions a compound or a step holds, which the relation asks about far more often than it changes. Most sets
/// hold none or a handful: a set is listed in ascending order while that takes less room than a bitmap up to its last
/// position, and is that bitmap, which answers with one load, once it takes more. The set lives in the dense table of
/// sets, so asking it reads its buffer and nothing else; a set that never held a position allocates nothing.
enum PositionSet {
    /// Ascending.
    Listed(Vec<u32>),
    /// Bit `position % 64` of word `position / 64` for each position, and how many there are.
    Bits { words: Vec<u64>, len: u32 },
}

// The dense table holds a set per compound and per step, most of them empty.
const _: () = assert!(size_of::<PositionSet>() == 32);

impl Default for PositionSet {
    fn default() -> Self {
        Self::Listed(Vec::new())
    }
}

impl FromIterator<usize> for PositionSet {
    /// The set of the ascending `positions`.
    fn from_iter<I: IntoIterator<Item = usize>>(positions: I) -> Self {
        let list: Vec<u32> = positions.into_iter().map(position_u32).collect();
        debug_assert!(list.is_sorted());
        let mut set = Self::Listed(list);
        set.settle();
        set
    }
}

impl PositionSet {
    fn len(&self) -> usize {
        match self {
            Self::Listed(list) => list.len(),
            Self::Bits { len, .. } => *len as usize,
        }
    }

    fn is_empty(&self) -> bool {
        self.len() == 0
    }

    fn contains(&self, position: usize) -> bool {
        match self {
            Self::Listed(list) => u32::try_from(position).is_ok_and(|position| list.binary_search(&position).is_ok()),
            Self::Bits { words, .. } => has_bit(words, position),
        }
    }

    /// The positions, ascending.
    fn iter(&self) -> impl Iterator<Item = usize> + '_ {
        let (list, words): (&[u32], &[u64]) = match self {
            Self::Listed(list) => (list, &[]),
            Self::Bits { words, .. } => (&[], words),
        };
        list.iter().map(|&position| position as usize).chain(
            words
                .iter()
                .enumerate()
                .flat_map(|(index, &word)| set_bits(index, word)),
        )
    }

    /// Removes the `departed` positions, which `live` no longer holds, returning whether the set held any. A list
    /// keeps what is live; a bitmap clears the departed.
    fn remove_departed(&mut self, departed: &[usize], live: &[bool]) -> bool {
        match self {
            Self::Listed(list) => {
                let before = list.len();
                list.retain(|&position| live[position as usize]);
                list.len() != before
            }
            Self::Bits { words, len } => {
                let before = *len;
                for &position in departed {
                    if has_bit(words, position) {
                        words[position / 64] &= !(1 << (position % 64));
                        *len -= 1;
                    }
                }
                *len != before
            }
        }
    }

    /// Empties the set into `into`, in ascending order. The set keeps its buffer.
    fn drain_into(&mut self, into: &mut Vec<usize>) {
        match self {
            Self::Listed(list) => into.extend(list.drain(..).map(|position| position as usize)),
            Self::Bits { words, len } => {
                for (index, word) in words.iter_mut().enumerate() {
                    into.extend(set_bits(index, std::mem::take(word)));
                }
                *len = 0;
            }
        }
    }

    /// Flips whether the set holds each of the ascending, distinct `changes`.
    fn toggle(&mut self, changes: &[usize]) {
        let Some(&last) = changes.last() else {
            return;
        };
        match self {
            Self::Listed(list) => toggle_listed(list, changes),
            Self::Bits { words, len } => {
                if words.len() <= last / 64 {
                    words.resize(last / 64 + 1, 0);
                }
                for &position in changes {
                    let word = &mut words[position / 64];
                    let bit = 1 << (position % 64);
                    *word ^= bit;
                    if *word & bit != 0 {
                        *len += 1;
                    } else {
                        *len -= 1;
                    }
                }
            }
        }
        self.settle();
    }

    /// Keeps the set in the form that takes less room: listed until its positions take more room than the bitmap up
    /// to the last of them, and listed again once they take less than half the bitmap, so that a set whose size wavers
    /// around the bound does not convert back and forth.
    fn settle(&mut self) {
        match self {
            Self::Listed(list) => {
                let Some(&last) = list.last() else {
                    return;
                };
                let word_count = last as usize / 64 + 1;
                if size_of_val(list.as_slice()) <= word_count * size_of::<u64>() {
                    return;
                }
                let mut words = vec![0_u64; word_count];
                for &position in list.iter() {
                    words[position as usize / 64] |= 1 << (position % 64);
                }
                let len = position_u32(list.len());
                *self = Self::Bits { words, len };
            }
            Self::Bits { words, len } => {
                if 2 * *len as usize * size_of::<u32>() >= size_of_val(words.as_slice()) {
                    return;
                }
                let list = words
                    .iter()
                    .enumerate()
                    .flat_map(|(index, &word)| set_bits(index, word))
                    .map(position_u32)
                    .collect();
                *self = Self::Listed(list);
            }
        }
    }
}

impl ShallowCapacityBytes for PositionSet {
    fn shallow_capacity_bytes(&self) -> u64 {
        match self {
            Self::Listed(list) => list.shallow_capacity_bytes(),
            Self::Bits { words, .. } => words.shallow_capacity_bytes(),
        }
    }
}

/// A position as a set lists it: a relation holds nowhere near 2^32 elements.
fn position_u32(position: usize) -> u32 {
    debug_assert!(
        u32::try_from(position).is_ok(),
        "prefix relation position space exhausted"
    );
    position as u32
}

fn has_bit(words: &[u64], position: usize) -> bool {
    words
        .get(position / 64)
        .is_some_and(|word| word & (1 << (position % 64)) != 0)
}

/// The positions the bits of word `index` of a bitmap stand for, ascending.
fn set_bits(index: usize, mut word: u64) -> impl Iterator<Item = usize> {
    std::iter::from_fn(move || {
        (word != 0).then(|| {
            let bit = word.trailing_zeros() as usize;
            word &= word - 1;
            index * 64 + bit
        })
    })
}

pub(in crate::css::style) struct PrefixRelation {
    program: std::sync::Arc<PrefixRelationProgram>,
    // Membership positions are stable slots. Inserting or removing a node does not renumber
    // unrelated matches; retired slots are reused only after their memberships are removed.
    nodes: Vec<StyleNodeID>,
    positions: Column<usize>,
    parents: Vec<usize>,
    previous: Vec<usize>,
    live: Vec<bool>,
    free_slots: Vec<usize>,
    departures: Vec<usize>,
    compound_matches: Vec<PositionSet>,
    matches: Vec<PositionSet>,
    walk_truth: PrefixWalkMemo,
    pending_steps: PendingPrefixSteps,
    positional: Vec<u32>,
    geometry_targets: [Vec<usize>; 4],
    old_previous: Vec<(usize, usize)>,
    sibling_order_is_preserved: bool,
    answers: Vec<Vec<EntryID>>,
    pub(in crate::css::style) changed_answers: Vec<(StyleNodeID, Vec<EntryID>, Vec<EntryID>)>,
    arrivals: Vec<usize>,
    /// A bit per position for the arrivals of the update in progress, which hold no membership yet: an arrival's slot
    /// is new, or was taken out of every set as it departed.
    arrived: Vec<u64>,
    pub(in crate::css::style) handled_routing_keys: HashMap<RoutingKey, bool>,
    // Routine prefix-cache accounting must not walk every selector set for each matching node.
    capacity_bytes: u64,
    nested_capacity_bytes: u64,
}

impl PrefixRelation {
    fn verify_answers(&self, evaluation: &mut PrefixEvaluation<'_, '_>) {
        if !cfg!(test) && !super::super::verification::prefix_relation_is_enabled() {
            return;
        }
        assert_eq!(self.nested_capacity_bytes, self.measure_nested_capacity_bytes());
        let mut scalar = PrefixStates::new();
        let counters = Counters::default();
        let mut context = if evaluation.facts_are_composite() {
            super::PrefixTransitionContext::new_composite(&mut scalar, evaluation.facts, &[])
        } else {
            super::PrefixTransitionContext::new(&mut scalar, evaluation.facts)
        };
        for (position, &node) in self.nodes.iter().enumerate() {
            if !self.live[position] {
                continue;
            }
            let PrefixTransitionLookup::Known(answer) =
                scalar.match_set_for(&mut context.scratch, &mut context.effects, evaluation, node, &counters)
            else {
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
                .answers
                .iter()
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
            + self.walk_truth.shallow_capacity_bytes()
            + self.positional.shallow_capacity_bytes()
            + self.old_previous.shallow_capacity_bytes()
            + self
                .geometry_targets
                .iter()
                .map(ShallowCapacityBytes::shallow_capacity_bytes)
                .sum::<u64>()
            + self.answers.shallow_capacity_bytes()
            + self.changed_answers.shallow_capacity_bytes()
            + self
                .changed_answers
                .iter()
                .map(|(_, old, new)| old.shallow_capacity_bytes() + new.shallow_capacity_bytes())
                .sum::<u64>()
            + self.arrivals.shallow_capacity_bytes()
            + self.arrived.shallow_capacity_bytes()
            + self.handled_routing_keys.shallow_capacity_bytes();
    }

    pub(in crate::css::style) fn install_answers(&self, states: &mut PrefixStates) {
        if states.relation_answers.is_empty() {
            for (position, (&node, entries)) in self.nodes.iter().zip(&self.answers).enumerate() {
                if self.live[position] {
                    states.install_relation_answer(node, entries);
                }
            }
        } else {
            for (node, _, entries) in &self.changed_answers {
                states.install_relation_answer(*node, entries);
            }
            for &position in &self.arrivals {
                states.install_relation_answer(self.nodes[position], &self.answers[position]);
            }
        }
    }

    /// Whether `node` is one of the live nodes the relation answers for.
    pub(in crate::css::style) fn holds(&self, node: StyleNodeID) -> bool {
        let position = self.position_of(Some(node));
        position != usize::MAX && self.live[position]
    }

    fn position_of(&self, node: Option<StyleNodeID>) -> usize {
        node.and_then(|node| self.positions.get(node.raw() as usize))
            .copied()
            .unwrap_or(usize::MAX)
    }

    pub(in crate::css::style) fn update_geometry(
        &mut self,
        automaton: &PrefixAutomaton,
        evaluation: &mut PrefixEvaluation<'_, '_>,
        changed: &[StyleNodeID],
        counters: &Counters,
    ) -> Vec<StyleNodeID> {
        let tree = evaluation.tree;
        let mut touched = changed.to_vec();
        self.arrivals.clear();
        self.departures.clear();
        self.old_previous.clear();
        self.sibling_order_is_preserved = self.program.has_following_steps;
        for &node in changed {
            if !tree.is_live(node)
                || self.position_of(Some(node)) != usize::MAX
                || tree.tree_scope(node) != TreeScopeID::DOCUMENT
            {
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
                if self.arrived.len() <= position / 64 {
                    self.arrived.resize(position / 64 + 1, 0);
                }
                self.arrived[position / 64] |= 1 << (position % 64);
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
                if self.program.has_following_steps {
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
                if has_bit(&self.arrived, position) {
                    return true;
                }
                // Departed slots still hold their old links. Arrivals hold only current links.
                // Removing both from the comparison leaves the order of retained siblings.
                while previous != usize::MAX && !self.live[previous] {
                    previous = self.previous[previous];
                }
                let mut current_previous = self.previous[position];
                while current_previous != usize::MAX && has_bit(&self.arrived, current_previous) {
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
                }
            }
        }
        if self.program.has_following_steps && !self.sibling_order_is_preserved {
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
        old_evaluation: &mut PrefixEvaluation<'_, '_>,
    ) -> HashMap<usize, Vec<usize>> {
        let mut result: HashMap<usize, Vec<usize>> = HashMap::default();
        if !self.sibling_order_is_preserved {
            return result;
        }
        let mut removed_steps = Vec::new();
        for &(frontier, old_previous) in &self.old_previous {
            if has_bit(&self.arrived, frontier) {
                continue;
            }
            removed_steps.clear();
            let mut removed = old_previous;
            while removed != usize::MAX && !self.live[removed] {
                let row = old_evaluation.row_of(self.nodes[removed]).unwrap();
                row.facts.for_each_dispatch_key(row.row, false, |key| {
                    if let Some(compounds) = self.program.compounds_for_key(&key) {
                        for compound in compounds {
                            for step in self.program.steps_for_compound(compound) {
                                if self.matches[step].contains(removed)
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
                let mut previous = self.previous[frontier];
                while previous != usize::MAX && !members.contains(previous) {
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
                                .contains(position)
                        {
                            result.entry(output.target as usize).or_default().push(position);
                        }
                    }
                    if members.contains(position) {
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
        evaluation: &mut PrefixEvaluation<'_, '_>,
        old_evaluation: &mut PrefixEvaluation<'_, '_>,
        changed_nodes: &[StyleNodeID],
        counters: &Counters,
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
            let Some(compounds) = self.program.compounds_for_key(key) else {
                continue;
            };
            for compound in compounds {
                if !self.compound_matches[compound].remove_departed(&self.departures, &self.live) {
                    continue;
                }
                for step in self.program.steps_for_compound(compound) {
                    self.matches[step].remove_departed(&self.departures, &self.live);
                }
            }
        }
        // Transaction rows can come from several fact stores. Intern within each store so
        // representative row indices are never interpreted in another store's columns.
        let mut local_facts: HashMap<*const _, LocalFactStore> = HashMap::default();
        let mut next_local_identity = 0_u32;
        let mut local_matches: HashMap<u64, bool> = HashMap::default();
        self.geometry_targets[0].sort_unstable();
        self.geometry_targets[0].dedup();
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
            let positional = evaluation.positional_bits(node, counters).unwrap();
            let positional_changes = self.positional[position] ^ positional;
            self.positional[position] = positional;
            // Geometry frontiers include retained nodes whose local facts did not change.
            // Preserve their predicate memberships, revisiting only changed positional tests.
            // A changed parent conservatively requires checking :root again.
            let arrived = has_bit(&self.arrived, position);
            let local_changed = arrived
                || self.geometry_targets[0].binary_search(&position).is_ok()
                || !super::rows_have_equal_local_facts_between(
                    row.facts,
                    row.row,
                    previous_row.facts,
                    previous_row.row,
                    &automaton.local_fact_dependencies,
                );
            if !local_changed && positional_changes == 0 {
                continue;
            }
            keys.clear();
            for row in [row, previous_row] {
                row.facts
                    .for_each_dispatch_key(row.row, evaluation.tree.parent(node).is_none(), |key| keys.push(key));
            }
            keys.sort_unstable();
            keys.dedup();
            let store = std::ptr::from_ref(row.facts);
            let facts = local_facts.entry(store).or_insert_with(LocalFactStore::new);
            let identity = facts.interner.intern(
                &mut facts.representatives,
                row.facts,
                row.row,
                &automaton.local_fact_dependencies,
                counters,
            ) as usize;
            let identity = facts.dense_identity(identity, &mut next_local_identity);
            let is_root = evaluation.tree.parent(node).is_none();
            for key in &keys {
                let Some(compounds) = self.program.compounds_for_key(key) else {
                    continue;
                };
                for index in compounds {
                    let compound = &automaton.compounds[index];
                    if !local_changed {
                        match &compound.predicate {
                            PrefixPredicate::Features {
                                required_positional_bits,
                                ..
                            } if required_positional_bits & positional_changes != 0 => {}
                            _ => continue,
                        }
                    }

                    // Positional truth is checked per node; it does not change the local
                    // predicate result shared by nodes with identical facts.
                    let positional_matches = match &compound.predicate {
                        PrefixPredicate::Features {
                            required_positional_bits,
                            ..
                        } => positional & required_positional_bits == *required_positional_bits,
                        PrefixPredicate::Program { .. } => true,
                    };
                    let matched = positional_matches
                        && *local_matches
                            .entry(local_match_key(identity, is_root, index))
                            .or_insert_with(|| {
                                counters.bump(Counter::PrefixCompoundsEvaluated);
                                match &compound.predicate {
                                    PrefixPredicate::Features {
                                        feature_start,
                                        feature_len,
                                        ..
                                    } => automaton
                                        .features_for(*feature_start, *feature_len)
                                        .all(|feature| matches_feature(row, feature)),
                                    PrefixPredicate::Program { program, local, .. } => evaluation
                                        .evaluator
                                        .matches_prefix_local(
                                            *program,
                                            evaluation.programs.get(*program),
                                            *local,
                                            node,
                                            counters,
                                        )
                                        .unwrap(),
                                }
                            });
                    if (!arrived && self.compound_matches[index].contains(position)) != matched {
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
            members.toggle(changes);
            self.nested_capacity_bytes = self.nested_capacity_bytes - before + members.shallow_capacity_bytes();
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
                        if let Some(compounds) = self.program.compounds_for_key(&key) {
                            for compound in compounds {
                                if self.compound_matches[compound].contains(position) {
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
            for step in self.program.steps_for_compound(compound) {
                if step_is_inert(automaton, &self.matches, step) {
                    continue;
                }
                self.pending_steps.insert(self.program.step_ranks[step] as usize);
            }
        }
        for &step in following_geometry.keys() {
            if step_is_inert(automaton, &self.matches, step) {
                continue;
            }
            self.pending_steps.insert(self.program.step_ranks[step] as usize);
        }
        // What each step that ran changed is a run of `changed_positions`, which `step_changes` names by step.
        let mut changed_positions = Vec::new();
        let mut step_changes: HashMap<usize, std::ops::Range<usize>> = HashMap::default();
        let mut terminal_changes = Vec::new();
        let mut affected = Vec::new();
        let mut descendants = Vec::new();
        let mut following_parents = HashSet::default();
        let mut walk_truth = std::mem::take(&mut self.walk_truth);
        let mut ancestor_chain = Vec::new();
        // Every local change is seeded before evaluation. Dependency order ensures that a step
        // runs once, after all changes to its predecessor, and only propagates a changed result.
        while let Some(rank) = self.pending_steps.pop_first() {
            let QueuedStep {
                step: step_index,
                axis,
                compound: compound_index,
                predecessor,
            } = self.program.queue[rank];
            let (step_index, compound_index) = (step_index as usize, compound_index as usize);
            let predecessor = (predecessor != u32::MAX).then_some(predecessor as usize);
            let first_change = changed_positions.len();
            if predecessor.is_some_and(|predecessor| self.matches[predecessor].is_empty()) {
                // No predecessor witness can satisfy any outgoing combinator. Remove the old
                // matches directly without evaluating local predicates or changed geometry.
                self.matches[step_index].drain_into(&mut changed_positions);
            } else {
                let predecessor_changes = predecessor
                    .and_then(|predecessor| step_changes.get(&predecessor))
                    .map_or(&[][..], |run| &changed_positions[run.clone()]);
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
                                    if candidates.contains(position) {
                                        affected.push(position);
                                    }
                                }
                            }
                        }
                        PrefixOutputKind::NextSibling => {
                            for &source in predecessor_changes {
                                if let Some(next) = evaluation.tree.next_element_sibling(self.nodes[source]) {
                                    let position = self.positions[next.element_index().unwrap() as usize];
                                    if candidates.contains(position) {
                                        affected.push(position);
                                    }
                                }
                            }
                        }
                        PrefixOutputKind::Descendant => {
                            // Enumerate a small changed subtree directly. If it is larger than
                            // the candidate set, test those candidates against the changed roots.
                            // The bound is the other join input's size, not a tuned batch cutoff.
                            descendants.clear();
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
                                        .iter()
                                        .copied()
                                        .filter(|&position| candidates.contains(position)),
                                );
                            } else {
                                walk_truth.clear();
                                for position in candidates.iter() {
                                    ancestor_chain.clear();
                                    let mut source = self.parents[position];
                                    let mut found = false;
                                    while source != usize::MAX {
                                        if predecessor_changes.binary_search(&source).is_ok() {
                                            found = true;
                                            break;
                                        }
                                        if let Some(truth) = walk_truth.get(source) {
                                            found = truth;
                                            break;
                                        }
                                        ancestor_chain.push(source);
                                        source = self.parents[source];
                                    }
                                    for &source in &ancestor_chain {
                                        walk_truth.insert(source, found);
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
                                    if follows_source && candidates.contains(position) {
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
                walk_truth.clear();
                for &position in &affected {
                    // A step runs once per update, so what it held is what it held before, which no arrival is in.
                    let previously_matched =
                        !has_bit(&self.arrived, position) && self.matches[step_index].contains(position);
                    let mut matched = candidates.contains(position);
                    if matched && let Some(predecessor) = predecessor {
                        let predecessor = &self.matches[predecessor];
                        let contains = |position| predecessor.contains(position);
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
                                    if let Some(truth) = walk_truth.get(current) {
                                        found = truth;
                                        break;
                                    }
                                    ancestor_chain.push(current);
                                    current = self.parents[current];
                                }
                                for &ancestor in &ancestor_chain {
                                    walk_truth.insert(ancestor, found);
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
                                    if let Some(truth) = walk_truth.get(source) {
                                        found = truth;
                                        break;
                                    }
                                    ancestor_chain.push(source);
                                    source = self.previous[source];
                                }
                                for &source in &ancestor_chain {
                                    walk_truth.insert(source, found);
                                }
                                found
                            }
                            _ => unreachable!(),
                        };
                    }
                    if previously_matched != matched {
                        changed_positions.push(position);
                    }
                }
                let members = &mut self.matches[step_index];
                let before = members.shallow_capacity_bytes();
                members.toggle(&changed_positions[first_change..]);
                self.nested_capacity_bytes = self.nested_capacity_bytes - before + members.shallow_capacity_bytes();
            }
            let changes = first_change..changed_positions.len();
            if changes.is_empty() {
                counters.bump(Counter::PrefixRelationStops);
                continue;
            }
            for successor in automaton.outputs_for(&automaton.steps[step_index]) {
                match successor.kind {
                    PrefixOutputKind::UniqueTerminal | PrefixOutputKind::SharedTerminal => {
                        for &position in &changed_positions[changes.clone()] {
                            terminal_changes.push((position, EntryID(successor.target)));
                        }
                    }
                    _ => {
                        let successor = successor.target as usize;
                        self.pending_steps.insert(self.program.step_ranks[successor] as usize);
                    }
                }
            }
            step_changes.insert(step_index, changes);
        }
        terminal_changes.sort_unstable();
        terminal_changes.dedup();
        self.changed_answers.clear();
        let mut old = Vec::new();
        let mut cursor = 0;
        while cursor < terminal_changes.len() {
            let position = terminal_changes[cursor].0;
            old.clear();
            old.extend_from_slice(&self.answers[position]);
            while cursor < terminal_changes.len() && terminal_changes[cursor].0 == position {
                let entry = terminal_changes[cursor].1;
                // Multiple paths can produce the same terminal. Losing one path changes the
                // selector answer only when no other path still matches this element.
                let matched = self.program.terminal_steps[&entry]
                    .iter()
                    .any(|&step| self.matches[step].contains(position));
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
                self.changed_answers.push((
                    self.nodes[position],
                    std::mem::take(&mut old),
                    self.answers[position].clone(),
                ));
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
        for &position in &self.arrivals {
            self.arrived[position / 64] = 0;
        }
        self.walk_truth = walk_truth;
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
                .all(|node| tree.tree_scope(node) == TreeScopeID::DOCUMENT)
    }

    pub(in crate::css::style) fn build_relation(
        &self,
        evaluation: &mut PrefixEvaluation<'_, '_>,
        root: StyleNodeID,
        counters: &Counters,
    ) -> PrefixRelation {
        counters.bump(Counter::PrefixRelationBuilds);
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
        for compound in self.compounds.iter() {
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
        // Local predicates read the same facts for every member of a fact cohort. Reuse their
        // answers while building memberships, as scalar prefix transitions already do. Keep
        // document-root identity in the key and check positional truth separately per node.
        let mut local_facts = HashMap::default();
        let mut identities = HashMap::default();
        let local_fact_keys: Vec<_> = rows
            .iter()
            .enumerate()
            .map(|(position, row)| {
                let store = std::ptr::from_ref(row.facts);
                let (interner, representatives) = local_facts
                    .entry(store)
                    .or_insert_with(|| (super::LocalFactInterner::new(), Default::default()));
                let identity = interner.intern(
                    representatives,
                    row.facts,
                    row.row,
                    &self.local_fact_dependencies,
                    counters,
                );
                let next = identities.len();
                *identities
                    .entry((store, identity, parents[position] == usize::MAX))
                    .or_insert(next)
            })
            .collect();
        let mut local_matches = vec![None; identities.len()];
        let mut compound_matches = Vec::with_capacity(self.compounds.len());
        for compound in self.compounds.iter() {
            // Dispatch postings already prove universal, ID, and class predicates. With
            // no additional local or positional test, the posting is the membership set.
            if let PrefixPredicate::Features {
                feature_start,
                feature_len,
                required_positional_bits: 0,
            } = &compound.predicate
                && self
                    .features_for(*feature_start, *feature_len)
                    .all(|feature| match feature {
                        super::FeatureTest::AnyElement => true,
                        super::FeatureTest::Id(id) => compound.dispatch_key == DispatchKey::Id(id),
                        super::FeatureTest::Class(class) => compound.dispatch_key == DispatchKey::Class(class),
                        _ => false,
                    })
            {
                compound_matches.push(candidates[&compound.dispatch_key].clone());
                continue;
            }
            local_matches.fill(None);
            let matched: Vec<_> = candidates[&compound.dispatch_key]
                .iter()
                .copied()
                .filter(|&position| {
                    if let PrefixPredicate::Features {
                        required_positional_bits,
                        ..
                    } = &compound.predicate
                        && positional[position] & required_positional_bits != *required_positional_bits
                    {
                        return false;
                    }
                    *local_matches[local_fact_keys[position]].get_or_insert_with(|| {
                        counters.bump(Counter::PrefixCompoundsEvaluated);
                        let row = rows[position];
                        match &compound.predicate {
                            PrefixPredicate::Features {
                                feature_start,
                                feature_len,
                                ..
                            } => self
                                .features_for(*feature_start, *feature_len)
                                .all(|feature| matches_feature(row, feature)),
                            PrefixPredicate::Program { program, local, .. } => evaluation
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
        let program = std::sync::Arc::clone(self.relation_program.get_or_init(|| {
            // Pack compounds with the same dispatch key into one immutable array. Preserve their
            // original order within each key without retaining a separate allocation for each list.
            let mut keyed_compounds: Vec<u32> = (0..self.compounds.len())
                .map(|index| u32::try_from(index).expect("prefix compound space exhausted"))
                .collect();
            keyed_compounds.sort_unstable_by_key(|&index| (self.compounds[index as usize].dispatch_key, index));
            let mut compounds_by_key: HashMap<DispatchKey, std::ops::Range<u32>> = HashMap::default();
            for (position, &index) in keyed_compounds.iter().enumerate() {
                let position = u32::try_from(position).expect("prefix compound space exhausted");
                compounds_by_key
                    .entry(self.compounds[index as usize].dispatch_key)
                    .or_insert(position..position)
                    .end = position.checked_add(1).expect("prefix compound space exhausted");
            }
            compounds_by_key.shrink_to_fit();
            let mut terminal_steps: HashMap<EntryID, SmallVec<[usize; 1]>> = HashMap::default();
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
            let mut compound_step_offsets = vec![0_u32; self.compounds.len() + 1];
            for step in &self.steps {
                let count = &mut compound_step_offsets[step.compound.0 as usize + 1];
                *count = count.checked_add(1).expect("prefix step space exhausted");
            }
            for index in 1..compound_step_offsets.len() {
                compound_step_offsets[index] = compound_step_offsets[index]
                    .checked_add(compound_step_offsets[index - 1])
                    .expect("prefix step space exhausted");
            }
            let mut positions = compound_step_offsets[..self.compounds.len()].to_vec();
            let mut compound_steps = vec![0; self.steps.len()];
            for (rank, &(step, _)) in queue.iter().enumerate() {
                step_ranks[step] = u32::try_from(rank).expect("prefix step space exhausted");
                let position = &mut positions[self.steps[step].compound.0 as usize];
                compound_steps[*position as usize] = u32::try_from(step).expect("prefix step space exhausted");
                *position += 1;
            }
            let has_following_steps = queue
                .iter()
                .any(|(_, axis)| matches!(axis, PrefixOutputKind::FollowingSibling));
            let mut program = PrefixRelationProgram {
                queue: queue
                    .into_iter()
                    .map(|(step, axis)| (u32::try_from(step).expect("prefix step space exhausted"), axis))
                    .map(|(step, axis)| QueuedStep {
                        step,
                        axis,
                        compound: self.steps[step as usize].compound.0,
                        predecessor: self.step_predecessors[step as usize],
                    })
                    .collect::<Box<[_]>>()
                    .into_vec(),
                step_ranks,
                compound_step_offsets,
                compound_steps,
                has_following_steps,
                compounds_by_key,
                keyed_compounds,
                terminal_steps,
                memory: super::super::memory::MemoryLease::new(super::super::memory::MemoryCategory::RuleProgram),
            };
            let mut memory = RELATION_PROGRAM_MEMORY
                .lock()
                .expect("the relation program ledger is never held across a panic");
            program.memory.resize_required_to(&mut memory, program.capacity_bytes());
            std::sync::Arc::new(program)
        }));
        let mut relation = PrefixRelation {
            program,
            nodes,
            positions,
            parents,
            previous,
            live: vec![true; count],
            free_slots: Vec::new(),
            departures: Vec::new(),
            compound_matches: compound_matches
                .into_iter()
                .map(PositionSet::from_iter)
                .collect::<Box<[_]>>()
                .into_vec(),
            matches: matches
                .into_iter()
                .map(|positions| positions.into_iter().map(|position| position as usize).collect())
                .collect::<Box<[_]>>()
                .into_vec(),
            walk_truth: PrefixWalkMemo::default(),
            pending_steps: PendingPrefixSteps::new(self.steps.len()),
            positional,
            geometry_targets: std::array::from_fn(|_| Vec::new()),
            old_previous: Vec::new(),
            sibling_order_is_preserved: false,
            answers: output,
            changed_answers: Vec::new(),
            arrivals: Vec::new(),
            arrived: Vec::new(),
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

// Walks only memoize stable relation slots. Reuse their storage across selector steps
// and transactions; a fresh generation separates queries without clearing or hashing
// the visited nodes. The column grows only when a walk records a result.
struct PrefixWalkMemo {
    stamps: Column<u64>,
    generation: u64,
}

impl Default for PrefixWalkMemo {
    fn default() -> Self {
        Self {
            stamps: Column::default(),
            generation: 2,
        }
    }
}

impl ShallowCapacityBytes for PrefixWalkMemo {
    fn shallow_capacity_bytes(&self) -> u64 {
        self.stamps.shallow_capacity_bytes()
    }
}

impl PrefixWalkMemo {
    fn clear(&mut self) {
        self.generation = self
            .generation
            .checked_add(2)
            .expect("prefix walk generation overflowed");
    }

    fn get(&self, position: usize) -> Option<bool> {
        let stamp = *self.stamps.get(position)?;
        (stamp & !1 == self.generation).then_some(stamp & 1 != 0)
    }

    fn insert(&mut self, position: usize, truth: bool) {
        self.stamps.insert(position, self.generation | u64::from(truth));
    }
}

/// One transaction's local fact identities for one fact store, numbered densely across every
/// store so that a memo entry names a compound result with a single integer.
struct LocalFactStore {
    interner: super::LocalFactInterner,
    representatives: super::super::intern_table::InternTable<super::LocalFactSlot, (u32, u32)>,
    dense: Vec<u32>,
}

impl LocalFactStore {
    fn new() -> Self {
        Self {
            interner: super::LocalFactInterner::new(),
            representatives: Default::default(),
            dense: Vec::new(),
        }
    }

    fn dense_identity(&mut self, identity: usize, next: &mut u32) -> u32 {
        if self.dense.len() <= identity {
            self.dense.resize(identity + 1, u32::MAX);
        }
        if self.dense[identity] == u32::MAX {
            self.dense[identity] = *next;
            *next += 1;
        }
        self.dense[identity]
    }
}

/// Name one compound's result for one local fact identity. Hashing a single integer costs less
/// than hashing the store, identity, root flag and compound the memo used to carry separately.
fn local_match_key(identity: u32, is_root: bool, compound: usize) -> u64 {
    debug_assert!(compound < 1 << 31, "compound identity space exhausted");
    (u64::from(identity) << 32) | (u64::from(is_root) << 31) | compound as u64
}

// A step with no predecessor witness can match nothing, so one which also holds no members has
// nothing to add and nothing to remove. Leaving it out of the queue is free: a predecessor that
// gains members later in the same update queues its successors through the automaton's outputs,
// and the queued step still reads its own compound and geometry changes when it runs.
fn step_is_inert(automaton: &PrefixAutomaton, matches: &[PositionSet], step: usize) -> bool {
    matches[step].is_empty()
        && automaton
            .predecessor_of(PrefixStepID(step as u32))
            .is_some_and(|predecessor| matches[predecessor.0 as usize].is_empty())
}

// Merge a batch of membership flips once. Repeated Vec::insert/remove would move the
// unaffected tail once per changed element, multiplying batch size by the set's population.
fn toggle_listed(members: &mut Vec<u32>, changes: &[usize]) {
    let (lowest, highest) = match *changes {
        [] => return,
        [position] => {
            let position = position_u32(position);
            match members.binary_search(&position) {
                Ok(index) => {
                    members.remove(index);
                }
                Err(index) => members.insert(index, position),
            }
            return;
        }
        [lowest, .., highest] => (position_u32(lowest), position_u32(highest)),
    };
    let first = members.partition_point(|&position| position < lowest);
    let end = members.partition_point(|&position| position <= highest);
    let removed_prefix = members[first..end]
        .iter()
        .zip(changes)
        .take_while(|&(member, position)| *member == position_u32(*position))
        .count();
    let mut cursor = first + removed_prefix;
    let changes = &changes[removed_prefix..];
    let mut replacement = Vec::with_capacity(end - cursor + changes.len());
    for &position in changes {
        let position = position_u32(position);
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
    use super::{PendingPrefixSteps, PositionSet, PrefixWalkMemo, ShallowCapacityBytes, toggle_listed};
    use std::collections::BTreeSet;

    fn is_bitmap(set: &PositionSet) -> bool {
        matches!(set, PositionSet::Bits { .. })
    }

    #[test]
    fn ordered_prefix_membership_queries_preserve_gaps_duplicates_and_boundary_slots() {
        for slots in [
            Vec::new(),
            vec![0],
            vec![u32::MAX],
            (0..256).step_by(3).collect(),
            (0..10_000).step_by(997).collect(),
            vec![1, 63, 64, 127, 128, 10_000, u32::MAX],
        ] {
            let expected: BTreeSet<_> = slots.iter().copied().collect();
            let members: PositionSet = slots.iter().map(|&slot| slot as usize).collect();
            assert!(members.iter().eq(slots.iter().map(|&slot| slot as usize)));
            for positions in [
                Vec::new(),
                vec![5000],
                vec![usize::MAX],
                vec![0, 1, 1, 63, 64, 64, 127, 128, 10_000, u32::MAX as usize, usize::MAX],
                (0..10_001).collect(),
            ] {
                let actual: Vec<_> = positions.iter().map(|&position| members.contains(position)).collect();
                let expected: Vec<_> = positions
                    .iter()
                    .map(|&position| u32::try_from(position).is_ok_and(|position| expected.contains(&position)))
                    .collect();
                assert_eq!(actual, expected);
            }
        }
    }

    #[test]
    fn position_sets_answer_alike_listed_or_as_a_bitmap() {
        let mut set = PositionSet::default();
        let mut model = BTreeSet::new();
        let mut seed = 1_u64;
        let mut was_bitmap = false;
        let mut was_listed_again = false;
        for round in 0..400 {
            // Batches grow the set past the bitmap bound, then shrink it back below half of it.
            let mut changes = BTreeSet::new();
            if round < 200 {
                for _ in 0..16 {
                    seed = seed
                        .wrapping_mul(6_364_136_223_846_793_005)
                        .wrapping_add(1_442_695_040_888_963_407);
                    changes.insert((seed >> 33) as usize % 2_000);
                }
            } else {
                changes.extend(model.iter().copied().step_by(3).take(16));
            }
            let changes: Vec<_> = changes.into_iter().collect();
            set.toggle(&changes);
            for &position in &changes {
                if !model.remove(&position) {
                    model.insert(position);
                }
            }
            was_listed_again |= was_bitmap && !is_bitmap(&set);
            was_bitmap |= is_bitmap(&set);
            assert_eq!(set.len(), model.len());
            assert!(set.iter().eq(model.iter().copied()));
            assert!((0..2_100).all(|position| set.contains(position) == model.contains(&position)));
        }
        assert!(was_bitmap && was_listed_again);

        for stride in [1, 97] {
            let positions: Vec<usize> = (0..2_000).step_by(stride).collect();
            let mut set: PositionSet = positions.iter().copied().collect();
            assert_eq!(is_bitmap(&set), stride == 1);
            let departed: Vec<_> = positions.iter().copied().step_by(2).collect();
            let mut live = vec![true; 2_000];
            for &position in &departed {
                live[position] = false;
            }
            assert!(set.remove_departed(&departed, &live));
            assert!(!set.remove_departed(&departed, &live));
            let kept: Vec<_> = positions.iter().copied().skip(1).step_by(2).collect();
            assert_eq!(set.len(), kept.len());
            assert!(set.iter().eq(kept.iter().copied()));
            let mut drained = Vec::new();
            set.drain_into(&mut drained);
            assert_eq!(drained, kept);
            assert!(set.is_empty() && set.iter().next().is_none());
        }
    }

    #[test]
    fn prefix_walk_memo_separates_true_false_and_unknown_across_queries() {
        let mut memo = PrefixWalkMemo::default();
        for query in 0..128 {
            for position in [0, 63, 64, 128] {
                assert_eq!(memo.get(position), None);
                let truth = (position + query) % 2 == 0;
                memo.insert(position, truth);
                assert_eq!(memo.get(position), Some(truth));
                memo.insert(position, !truth);
                assert_eq!(memo.get(position), Some(!truth));
            }
            memo.clear();
        }
    }

    #[test]
    fn batched_membership_flips_preserve_the_unchanged_ranges() {
        let mut members = vec![1_u32, 3, 5, 7, 9];
        toggle_listed(&mut members, &[0, 3, 4, 7, 8]);
        assert_eq!(members, [0, 1, 4, 5, 8, 9]);
        toggle_listed(&mut members, &[0, 3, 4, 7, 8]);
        assert_eq!(members, [1, 3, 5, 7, 9]);
        toggle_listed(&mut members, &[1, 3, 5, 7, 9]);
        assert!(members.is_empty());
        toggle_listed(&mut members, &[2, 4, 6]);
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

    #[test]
    fn empty_memberships_allocate_on_first_use_and_retain_warm_buffers() {
        let listed = |set: &PositionSet| match set {
            PositionSet::Listed(list) => Some(list.as_ptr()),
            _ => None,
        };
        let mut members = PositionSet::default();
        assert_eq!(members.shallow_capacity_bytes(), 0);
        let mut drained = Vec::new();
        members.drain_into(&mut drained);
        assert!(drained.is_empty());
        assert!(!members.remove_departed(&[1], &[true, false]));
        assert_eq!(members.shallow_capacity_bytes(), 0);
        members.toggle(&[1, 3]);
        assert!(members.iter().eq([1, 3]));
        let buffer = listed(&members);
        assert!(buffer.is_some(), "a small set is listed");
        let capacity = members.shallow_capacity_bytes();
        members.drain_into(&mut drained);
        assert_eq!(drained, [1, 3]);
        members.toggle(&[2, 4]);
        assert_eq!(listed(&members), buffer);
        assert_eq!(members.shallow_capacity_bytes(), capacity);
        assert!(members.iter().eq([2, 4]));
    }
}
