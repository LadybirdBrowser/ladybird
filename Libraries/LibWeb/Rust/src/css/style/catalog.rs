/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::capacity::capacity_bytes;
use super::column::Column;
use super::fast_hash::fast_hasher;
use super::*;

define_id! { default pub(super) struct MatchAnswerID(pub(super)); }
define_id! { default pub(super) struct SelectorTruthSetID(pub(super)); }

impl super::intern_table::InternIdentity for MatchAnswerID {
    fn index(self) -> usize {
        self.0 as usize - 1
    }
}

impl super::intern_table::InternIdentity for SelectorTruthSetID {
    fn index(self) -> usize {
        self.0 as usize - 1
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(super) struct SelectorTruth {
    pub(super) entry: EntryID,
    pub(super) tree_scope: TreeScopeID,
    pub(super) scope_proximity: u32,
}

#[derive(Default)]
pub(super) struct SelectorTruthSetCatalog {
    sets: super::intern_table::InternTable<SelectorTruthSetID, Rc<[SelectorTruth]>>,
    verified_derived_answers: HashMap<(SelectorTruthSetID, TreeScopeID, u64), Rc<[RetainedRuleMatch]>>,
    last_identity: u32,
}

impl SelectorTruthSetCatalog {
    pub(super) fn intern_prepared(&mut self, truth: Vec<SelectorTruth>) -> (SelectorTruthSetID, bool) {
        let hash = super::intern_table::content_hash(&truth);
        if let Some(identity) = self
            .sets
            .find(hash, |_identity, candidate| candidate.as_ref() == truth.as_slice())
        {
            return (identity, true);
        }
        self.last_identity = self
            .last_identity
            .checked_add(1)
            .expect("selector truth-set identity space exhausted");
        let identity = SelectorTruthSetID(self.last_identity);
        self.sets.insert(hash, identity, truth.into());
        (identity, false)
    }

    pub(super) fn get(&self, identity: SelectorTruthSetID) -> &Rc<[SelectorTruth]> {
        &self.sets[identity]
    }

    pub(super) fn verify_derived_answer(
        &mut self,
        truth: SelectorTruthSetID,
        tree_scope: TreeScopeID,
        program_version: ProgramVersion,
        answer: &[RetainedRuleMatch],
    ) -> bool {
        match self
            .verified_derived_answers
            .entry((truth, tree_scope, program_version.0))
        {
            std::collections::hash_map::Entry::Vacant(entry) => {
                entry.insert(answer.into());
                false
            }
            std::collections::hash_map::Entry::Occupied(entry) => {
                assert_eq!(entry.get().as_ref(), answer, "selector truth derived two rule answers");
                true
            }
        }
    }
}

pub(super) struct MatchAnswerCatalogEntry {
    pub(super) answer: Rc<[RetainedRuleMatch]>,
    pub(super) prefix_references: u32,
    pub(super) cascade_references: u32,
    pub(super) cascade_payload_accounted: bool,
    pub(super) retained_references: u32,
}

#[derive(Default)]
pub(super) struct MatchAnswerCatalog {
    pub(super) answers: super::intern_table::InternTable<MatchAnswerID, Option<MatchAnswerCatalogEntry>>,
    pub(super) last_identity: u32,
    pub(super) prefix_payload_bytes: usize,
    pub(super) cascade_payload_bytes: u64,
    pub(super) retained_payload_bytes: u64,
    pub(super) retained_answer_count: usize,
    pub(super) needs_compaction: bool,
}

impl MatchAnswerCatalog {
    fn answer_payload_bytes(answer: &[RetainedRuleMatch]) -> u64 {
        (size_of_val(answer) + 2 * size_of::<usize>()) as u64
    }

    pub(super) fn mark_referenced_selector_programs(&self, referenced: &mut Vec<bool>) {
        let mut mark = |program: SelectorProgramID| {
            if referenced.len() <= program.0 as usize {
                referenced.resize(program.0 as usize + 1, false);
            }
            referenced[program.0 as usize] = true;
        };
        for entry in self.answers.iter().flatten().filter(|entry| {
            entry.prefix_references != 0 || entry.cascade_references != 0 || entry.retained_references != 0
        }) {
            for matched in entry.answer.iter() {
                mark(matched.program);
            }
        }
    }

    pub(super) fn new_identity(&mut self) -> MatchAnswerID {
        self.last_identity = self
            .last_identity
            .checked_add(1)
            .expect("match answer catalog identity space exhausted");
        MatchAnswerID(self.last_identity)
    }

    pub(super) fn intern(&mut self, answer: &[RuleMatch]) -> MatchAnswerID {
        const INLINE_ANSWER_LENGTH: usize = 16;
        if let Some(first) = answer.first().copied()
            && answer.len() <= INLINE_ANSWER_LENGTH
        {
            let first = RetainedRuleMatch::from_rule_match(first);
            let mut prepared = [first; INLINE_ANSWER_LENGTH];
            for (output, matched) in prepared.iter_mut().zip(answer.iter().copied()) {
                *output = RetainedRuleMatch::from_rule_match(matched);
            }
            let prepared = &mut prepared[..answer.len()];
            prepared.sort_unstable();
            let hash = hash_retained_rule_matches(prepared);
            if let Some(identity) = self.identity(prepared, hash) {
                return identity;
            }
            return self.insert_new(prepared.to_vec(), hash);
        }
        let mut prepared: Vec<RetainedRuleMatch> =
            answer.iter().copied().map(RetainedRuleMatch::from_rule_match).collect();
        prepared.sort_unstable();
        self.intern_prepared(prepared)
    }

    pub(super) fn intern_prepared(&mut self, answer: Vec<RetainedRuleMatch>) -> MatchAnswerID {
        let hash = hash_retained_rule_matches(&answer);
        if let Some(identity) = self.identity(&answer, hash) {
            return identity;
        }
        self.insert_new(answer, hash)
    }

    pub(super) fn insert_new(&mut self, answer: Vec<RetainedRuleMatch>, hash: u64) -> MatchAnswerID {
        let identity = self.new_identity();
        self.answers.insert(
            hash,
            identity,
            Some(MatchAnswerCatalogEntry {
                answer: answer.into(),
                prefix_references: 0,
                cascade_references: 0,
                cascade_payload_accounted: false,
                retained_references: 0,
            }),
        );
        identity
    }

    pub(super) fn answer(&self, identity: MatchAnswerID) -> Option<&Rc<[RetainedRuleMatch]>> {
        self.answers[identity].as_ref().map(|entry| &entry.answer)
    }

    pub(super) fn has_cascade_reference(&self, identity: MatchAnswerID) -> bool {
        self.answers[identity]
            .as_ref()
            .is_some_and(|entry| entry.cascade_references != 0)
    }

    pub(super) fn retain_cascade(&mut self, identity: MatchAnswerID) {
        let entry = self.answers[identity]
            .as_mut()
            .expect("a cascade-input identity must remain in its catalog");
        if entry.cascade_references == 0 && !entry.cascade_payload_accounted {
            self.cascade_payload_bytes = self
                .cascade_payload_bytes
                .checked_add(Self::answer_payload_bytes(&entry.answer))
                .expect("cascade-input payload byte count overflow");
            entry.cascade_payload_accounted = true;
        }
        entry.cascade_references = entry
            .cascade_references
            .checked_add(1)
            .expect("cascade-input reference count overflow");
    }

    pub(super) fn release_cascade(&mut self, identity: MatchAnswerID) {
        let entry = self.answers[identity]
            .as_mut()
            .expect("a cascade-input identity must remain in its catalog");
        entry.cascade_references = entry
            .cascade_references
            .checked_sub(1)
            .expect("releasing an unretained cascade input");
        if entry.cascade_references != 0 || (entry.prefix_references == 0 && entry.retained_references == 0) {
            return;
        }
        if entry.cascade_payload_accounted {
            self.cascade_payload_bytes = self
                .cascade_payload_bytes
                .checked_sub(Self::answer_payload_bytes(&entry.answer))
                .expect("cascade-input payload byte count underflow");
            entry.cascade_payload_accounted = false;
        }
    }

    pub(super) fn retain_prefix(&mut self, identity: MatchAnswerID) {
        let entry = self.answers[identity]
            .as_mut()
            .expect("a retained answer must remain in its catalog");
        if entry.prefix_references == 0 {
            self.prefix_payload_bytes += entry.answer.len() * size_of::<RetainedRuleMatch>();
        }
        entry.prefix_references = entry
            .prefix_references
            .checked_add(1)
            .expect("match answer reference count overflow");
    }

    pub(super) fn release_prefix(&mut self, identity: MatchAnswerID) {
        let remove = {
            let entry = self.answers[identity]
                .as_mut()
                .expect("a retained answer must remain in its catalog");
            entry.prefix_references = entry
                .prefix_references
                .checked_sub(1)
                .expect("releasing an unretained match answer");
            if entry.prefix_references != 0 {
                return;
            }
            self.prefix_payload_bytes -= entry.answer.len() * size_of::<RetainedRuleMatch>();
            entry.cascade_references == 0 && entry.retained_references == 0
        };
        if remove {
            self.remove_unreferenced(identity);
        }
    }

    pub(super) fn sweep_unreferenced(&mut self) -> u64 {
        let unreferenced = self
            .answers
            .iter()
            .enumerate()
            .filter_map(|(index, entry)| {
                let entry = entry.as_ref()?;
                (entry.prefix_references == 0 && entry.cascade_references == 0 && entry.retained_references == 0)
                    .then_some(MatchAnswerID(index as u32 + 1))
            })
            .collect::<Vec<_>>();
        unreferenced
            .into_iter()
            .map(|identity| self.remove_unreferenced(identity))
            .sum()
    }

    pub(super) fn remove_unreferenced(&mut self, identity: MatchAnswerID) -> u64 {
        let entry = self.answers[identity].take().expect("unreferenced answer must be live");
        let released_cascade_payload_bytes = if entry.cascade_payload_accounted {
            let bytes = Self::answer_payload_bytes(&entry.answer);
            self.cascade_payload_bytes = self
                .cascade_payload_bytes
                .checked_sub(bytes)
                .expect("cascade-input payload byte count underflow");
            bytes
        } else {
            0
        };
        let hash = hash_retained_rule_matches(&entry.answer);
        self.answers.remove_identity(hash, identity);
        self.needs_compaction = true;
        released_cascade_payload_bytes
    }

    pub(super) fn compact_if_needed(&mut self) {
        if !self.needs_compaction {
            return;
        }
        self.answers.shrink_to_fit();
        self.needs_compaction = false;
    }

    pub(super) fn identity(&self, answer: &[RetainedRuleMatch], hash: u64) -> Option<MatchAnswerID> {
        self.answers.find(hash, |_identity, entry| {
            entry.as_ref().is_some_and(|entry| entry.answer.as_ref() == answer)
        })
    }

    pub(super) fn identity_is_retained(&self, identity: MatchAnswerID) -> bool {
        self.answers[identity]
            .as_ref()
            .is_some_and(|entry| entry.retained_references != 0)
    }

    pub(super) fn insert_retained(&mut self, answer: Vec<RetainedRuleMatch>, hash: u64) -> MatchAnswerID {
        debug_assert!(self.identity(&answer, hash).is_none());
        let identity = self.insert_new(answer, hash);
        self.retain_identity(identity);
        identity
    }

    pub(super) fn retain_identity(&mut self, identity: MatchAnswerID) {
        let entry = self.answers[identity].as_mut().unwrap();
        if entry.retained_references == 0 {
            self.retained_payload_bytes += Self::answer_payload_bytes(&entry.answer);
            self.retained_answer_count += 1;
        }
        entry.retained_references = entry
            .retained_references
            .checked_add(1)
            .expect("retained match answer reference count overflow");
    }

    pub(super) fn release_identity(&mut self, identity: MatchAnswerID) {
        let remove = {
            let entry = self.answers[identity].as_mut().unwrap();
            entry.retained_references = entry
                .retained_references
                .checked_sub(1)
                .expect("releasing an unretained match answer identity");
            if entry.retained_references != 0 {
                return;
            }
            self.retained_payload_bytes -= Self::answer_payload_bytes(&entry.answer);
            self.retained_answer_count -= 1;
            entry.prefix_references == 0 && entry.cascade_references == 0
        };
        if remove {
            self.remove_unreferenced(identity);
        }
    }

    pub(super) fn retained_answer(&self, identity: MatchAnswerID) -> Option<&Rc<[RetainedRuleMatch]>> {
        self.answers[identity]
            .as_ref()
            .filter(|entry| entry.retained_references != 0)
            .map(|entry| &entry.answer)
    }

    pub(super) fn retained_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [self.retained_payload_bytes];
            nested [
                self.retained_answer_count
                    * (size_of::<MatchAnswerID>() + size_of::<MatchAnswerCatalogEntry>() + 1),
            ];
            skip [
                self.answers,
                self.last_identity,
                self.prefix_payload_bytes,
                self.cascade_payload_bytes,
                self.needs_compaction,
            ];
        }
    }

    pub(super) fn evict_retained(&mut self) {
        let retained = self
            .answers
            .iter_mut()
            .enumerate()
            .filter_map(|(index, entry)| {
                let entry = entry.as_mut()?;
                if entry.retained_references == 0 {
                    return None;
                }
                entry.retained_references = 0;
                (entry.prefix_references == 0 && entry.cascade_references == 0)
                    .then_some(MatchAnswerID(index as u32 + 1))
            })
            .collect::<Vec<_>>();
        for identity in retained {
            self.remove_unreferenced(identity);
        }
        self.retained_payload_bytes = 0;
        self.retained_answer_count = 0;
    }
}

pub(super) struct PrefixAnswer {
    pub(super) matches: MatchAnswerID,
    pub(super) winner_group: Option<(u64, CascadeStateID)>,
    pub(super) cascade_input: MatchAnswerID,
    pub(super) cascade_winner_inventory_is_complete: bool,
}

pub(super) struct PrefixAnswerCache {
    pub(super) prefix_contribution_by_match_set: Column<Column<MatchAnswerID>>,
    pub(super) exact_prefix_by_match_set: Column<Column<MatchAnswerID>>,
    pub(super) exact_answers: HashMap<PrefixAnswerKey, MatchAnswerID>,
    pub(super) answers: HashMap<PrefixAnswerKey, PrefixAnswer>,
    pub(super) scratch_memory: MemoryLease,
    pub(super) residency: MemoryLease,
    pub(super) retained: bool,
    pub(super) nested_footprint: MemoryLease,
}

#[derive(Default)]
pub(super) struct PrefixCaches {
    pub(super) states: PrefixStateCache,
    pub(super) answers: PrefixAnswerCache,
}

impl Default for PrefixAnswerCache {
    fn default() -> Self {
        Self {
            prefix_contribution_by_match_set: Column::default(),
            exact_prefix_by_match_set: Column::default(),
            exact_answers: HashMap::default(),
            answers: HashMap::default(),
            scratch_memory: MemoryLease::new(MemoryCategory::BatchScratch),
            residency: MemoryLease::new(MemoryCategory::PrefixAnswerCache),
            retained: false,
            nested_footprint: MemoryLease::new(MemoryCategory::BatchScratch),
        }
    }
}

impl PrefixAnswerCache {
    pub(super) fn non_prefix_identity(catalog: &mut MatchAnswerCatalog, matches: &[RuleMatch]) -> MatchAnswerID {
        catalog.intern(matches)
    }

    /// Wraps a catalog mutation so the prefix payload bytes it added or released are mirrored in
    /// this cache's nested footprint.
    pub(super) fn with_payload_accounting<R>(
        &mut self,
        catalog: &mut MatchAnswerCatalog,
        operation: impl FnOnce(&mut Self, &mut MatchAnswerCatalog) -> R,
    ) -> R {
        let payload_bytes_before = catalog.prefix_payload_bytes;
        let result = operation(self, catalog);
        let payload_bytes_after = catalog.prefix_payload_bytes;
        if payload_bytes_after >= payload_bytes_before {
            self.nested_footprint
                .grow_committed((payload_bytes_after - payload_bytes_before) as u64);
        } else {
            self.nested_footprint
                .shrink_committed((payload_bytes_before - payload_bytes_after) as u64);
        }
        result
    }

    pub(super) fn lane_lookup<'a>(
        lane: &Column<Column<MatchAnswerID>>,
        catalog: &'a MatchAnswerCatalog,
        key: PrefixContributionKey,
    ) -> Lookup<(MatchAnswerID, &'a Rc<[RetainedRuleMatch]>), PrefixContributionKey> {
        let Some(identity) = lane
            .get(key.program.0 as usize)
            .and_then(|by_match_set| by_match_set.get(key.matches.index()))
            .copied()
            .filter(|&identity| identity != MatchAnswerID::default())
        else {
            return Lookup::Missing(key);
        };
        let answer = catalog
            .answer(identity)
            .expect("a prefix answer identity must remain in its catalog");
        Lookup::Known((identity, answer))
    }

    pub(super) fn lane_remember(
        lane: &mut Column<Column<MatchAnswerID>>,
        nested_footprint: &mut MemoryLease,
        catalog: &mut MatchAnswerCatalog,
        program: ScopeProgramID,
        matches: PrefixMatchSetID,
        identity: MatchAnswerID,
    ) {
        let program_index = program.0 as usize;
        lane.ensure(program_index);
        let by_match_set = &mut lane[program_index];
        let match_set_index = matches.index();
        nested_footprint.grow_committed(by_match_set.ensure(match_set_index));
        let previous = by_match_set[match_set_index];
        if previous != identity {
            catalog.retain_prefix(identity);
            if previous != MatchAnswerID::default() {
                catalog.release_prefix(previous);
            }
            by_match_set[match_set_index] = identity;
        }
    }

    pub(super) fn prefix_contribution<'a>(
        &self,
        catalog: &'a MatchAnswerCatalog,
        key: PrefixContributionKey,
    ) -> Lookup<(MatchAnswerID, &'a Rc<[RetainedRuleMatch]>), PrefixContributionKey> {
        Self::lane_lookup(&self.prefix_contribution_by_match_set, catalog, key)
    }

    pub(super) fn exact_prefix<'a>(
        &self,
        catalog: &'a MatchAnswerCatalog,
        key: PrefixContributionKey,
    ) -> Lookup<(MatchAnswerID, &'a Rc<[RetainedRuleMatch]>), PrefixContributionKey> {
        Self::lane_lookup(&self.exact_prefix_by_match_set, catalog, key)
    }

    pub(super) fn remember_exact_prefix(
        &mut self,
        catalog: &mut MatchAnswerCatalog,
        program: ScopeProgramID,
        matches: PrefixMatchSetID,
        answer: Vec<RetainedRuleMatch>,
    ) -> MatchAnswerID {
        self.with_payload_accounting(catalog, |cache, catalog| {
            let identity = catalog.intern_prepared(answer);
            Self::lane_remember(
                &mut cache.exact_prefix_by_match_set,
                &mut cache.nested_footprint,
                catalog,
                program,
                matches,
                identity,
            );
            identity
        })
    }

    pub(super) fn remember_prefix_contribution(
        &mut self,
        catalog: &mut MatchAnswerCatalog,
        program: ScopeProgramID,
        matches: PrefixMatchSetID,
        answer: &[RuleMatch],
    ) -> MatchAnswerID {
        self.with_payload_accounting(catalog, |cache, catalog| {
            let identity = catalog.intern(answer);
            Self::lane_remember(
                &mut cache.prefix_contribution_by_match_set,
                &mut cache.nested_footprint,
                catalog,
                program,
                matches,
                identity,
            );
            identity
        })
    }

    pub(super) fn exact_answer(&self, key: PrefixAnswerKey) -> Lookup<MatchAnswerID, PrefixAnswerKey> {
        match self.exact_answers.get(&key) {
            Some(&answer) => Lookup::Known(answer),
            None => Lookup::Missing(key),
        }
    }

    pub(super) fn remember_exact_answer(
        &mut self,
        catalog: &mut MatchAnswerCatalog,
        key: PrefixAnswerKey,
        answer: MatchAnswerID,
    ) {
        self.with_payload_accounting(catalog, |cache, catalog| {
            catalog.retain_prefix(answer);
            if let Some(previous) = cache.exact_answers.insert(key, answer) {
                catalog.release_prefix(previous);
            } else {
                catalog.retain_prefix(key.non_prefix_matches);
            }
        });
    }

    pub(super) fn remember(
        &mut self,
        catalog: &mut MatchAnswerCatalog,
        key: PrefixAnswerKey,
        answer: &[RuleMatch],
        winner_group: Option<(u64, CascadeStateID)>,
        cascade_input: MatchAnswerID,
        cascade_winner_inventory_is_complete: bool,
    ) {
        self.with_payload_accounting(catalog, |cache, catalog| {
            let matches = catalog.intern(answer);
            catalog.retain_prefix(matches);
            if let Some(previous) = cache.answers.insert(
                key,
                PrefixAnswer {
                    matches,
                    winner_group,
                    cascade_input,
                    cascade_winner_inventory_is_complete,
                },
            ) {
                catalog.release_prefix(previous.matches);
            } else {
                catalog.retain_prefix(key.non_prefix_matches);
            }
        });
    }

    pub(super) fn settle_memory(&mut self, catalog: &MatchAnswerCatalog, memory: &mut MemoryController) {
        debug_assert!(!self.retained);
        let current = self.capacity_bytes(catalog);
        self.scratch_memory.resize_required_to(memory, current);
    }

    pub(super) fn release(&mut self, catalog: &mut MatchAnswerCatalog) {
        if self.retained {
            self.residency.release();
        } else {
            self.scratch_memory.release();
        }
        for lane in [&self.prefix_contribution_by_match_set, &self.exact_prefix_by_match_set] {
            for by_match_set in lane.iter() {
                for &identity in by_match_set.iter() {
                    if identity != MatchAnswerID::default() {
                        catalog.release_prefix(identity);
                    }
                }
            }
        }
        for (&key, answer) in &self.answers {
            catalog.release_prefix(key.non_prefix_matches);
            catalog.release_prefix(answer.matches);
        }
        for (&key, &answer) in &self.exact_answers {
            catalog.release_prefix(key.non_prefix_matches);
            catalog.release_prefix(answer);
        }
        catalog.compact_if_needed();
        self.prefix_contribution_by_match_set = Column::default();
        self.exact_prefix_by_match_set = Column::default();
        self.exact_answers = HashMap::default();
        self.answers = HashMap::default();
        self.nested_footprint.release();
        self.retained = false;
    }

    pub(super) fn retain(&mut self, memory: &mut MemoryController) -> bool {
        if self.retained {
            return true;
        }
        if !memory.is_tier3_admitting(MemoryCategory::PrefixAnswerCache) {
            return false;
        }
        self.residency.reconcile_committed(memory, self.scratch_memory.bytes());
        memory.finish_committed_acceleration_growth(MemoryCategory::PrefixAnswerCache);
        self.scratch_memory.release();
        self.retained = true;
        true
    }

    pub(super) fn make_scratch(&mut self, memory: &mut MemoryController) {
        if !self.retained {
            return;
        }
        self.scratch_memory.resize_required_to(memory, self.residency.bytes());
        self.residency.release();
        self.retained = false;
    }

    pub(super) fn remove_program(&mut self, catalog: &mut MatchAnswerCatalog, program: ScopeProgramID) {
        let program_index = program.0 as usize;
        if self
            .prefix_contribution_by_match_set
            .get(program_index)
            .is_some_and(|by_match_set| !by_match_set.is_empty())
            || self
                .exact_prefix_by_match_set
                .get(program_index)
                .is_some_and(|by_match_set| !by_match_set.is_empty())
        {
            // Canonical contributions are shared across programs. Dropping only this program's
            // lookup entries would leave unreachable contributions behind, so release the small,
            // evictable cache as one coherent unit.
            self.release(catalog);
        }
    }

    pub(super) fn capacity_bytes(&self, _catalog: &MatchAnswerCatalog) -> u64 {
        capacity_bytes! {
            shallow [
                self.prefix_contribution_by_match_set,
                self.exact_prefix_by_match_set,
                self.exact_answers,
                self.answers,
            ];
            cached [self.nested_footprint.bytes()];
            nested [];
            skip [self.scratch_memory, self.residency, self.retained];
        }
    }
}

impl PrefixAnswerCache {
    pub(super) fn lookup(&self, key: PrefixAnswerKey) -> Lookup<&PrefixAnswer, PrefixAnswerKey> {
        match self.answers.get(&key) {
            Some(answer) => Lookup::Known(answer),
            None => Lookup::Missing(key),
        }
    }
}

pub(super) fn hash_retained_rule_matches(matches: &[RetainedRuleMatch]) -> u64 {
    let mut hasher = fast_hasher();
    matches.hash(&mut hasher);
    hasher.finish()
}

/// Exact selector answers retained as the old side of a later plan.
///
/// Cascade compaction is declaration-dependent, so the answer catalog keeps this stable selector
/// payload and restores the current cascade rank when reading it. The per-node retained coverage is
/// Tier-3: losing it turns the next declaration edit into an ordinary cold match without changing
/// semantics.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(super) struct RetainedRuleMatch {
    pub(super) rule: RuleID,
    pub(super) program: SelectorProgramID,
    pub(super) entry: u32,
    pub(super) tree_scope: TreeScopeID,
    pub(super) scope_proximity: u32,
}

impl RetainedRuleMatch {
    pub(super) fn from_rule_match(entry: RuleMatch) -> Self {
        Self {
            rule: entry.rule,
            program: entry.program,
            entry: entry.entry,
            tree_scope: entry.tree_scope,
            scope_proximity: entry.scope_proximity,
        }
    }

    pub(super) fn materialize(
        self,
        node: StyleNodeID,
        programs: &SelectorPrograms,
        cascade_order: u32,
    ) -> Option<RuleMatch> {
        let entry = programs.get(self.program).entries().get(self.entry as usize)?;
        Some(RuleMatch {
            node,
            pseudo_element: entry.pseudo_element,
            rule: self.rule,
            program: self.program,
            entry: self.entry,
            cascade_order,
            specificity: entry.specificity,
            tree_scope: self.tree_scope,
            scope_proximity: self.scope_proximity,
        })
    }
}

pub(super) fn prepare_retained_match_answer(matches: impl Iterator<Item = RuleMatch>) -> Vec<RetainedRuleMatch> {
    let mut answer: Vec<_> = matches.map(RetainedRuleMatch::from_rule_match).collect();
    answer.sort_unstable();
    answer
}

pub(super) fn prepare_selector_truth_set(
    matches: &[RetainedRuleMatch],
    programs: &SelectorPrograms,
) -> Vec<SelectorTruth> {
    let mut truth = matches
        .iter()
        .map(|matched| SelectorTruth {
            entry: programs.entry_id(matched.program, matched.entry),
            tree_scope: matched.tree_scope,
            scope_proximity: matched.scope_proximity,
        })
        .collect::<Vec<_>>();
    truth.sort_unstable();
    truth.dedup();
    truth
}

pub(super) fn merge_retained_match_answers(answer: &mut Vec<RetainedRuleMatch>, suffix: &[RetainedRuleMatch]) {
    let mut answer_index = answer.len();
    let mut suffix_index = suffix.len();
    answer.reserve(suffix_index);
    answer.extend_from_slice(suffix);
    let mut destination = answer.len();
    while answer_index != 0 && suffix_index != 0 {
        destination -= 1;
        if answer[answer_index - 1] > suffix[suffix_index - 1] {
            answer_index -= 1;
            answer[destination] = answer[answer_index];
        } else {
            suffix_index -= 1;
            answer[destination] = suffix[suffix_index];
        }
    }
    if suffix_index != 0 {
        answer[..suffix_index].copy_from_slice(&suffix[..suffix_index]);
    }
}

pub(super) struct RetainedMatchAnswers {
    pub(super) column: Vec<MatchAnswerID>,
    pub(super) cascade_input_column: Vec<MatchAnswerID>,
    pub(super) cascade_input_memory: MemoryLease,
    pub(super) residency: MemoryLease,
}

/// Activation-independent selector incidence recovered from retained exact match answers.
///
/// Active match answers normally discard a rule when its condition becomes false. This inverse
/// relation preserves the selector side of that join, so a later condition flip can route through
/// exact selector truth without evaluating the selector again. It is Tier-3 state: incomplete
/// retained coverage or closed admission simply leaves program routing on its cold path.
pub(super) struct RetainedSelectorIncidences {
    pub(super) by_program: Vec<Option<Rc<[RetainedSelectorIncidence]>>>,
    pub(super) residency: MemoryLease,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub(super) struct RetainedSelectorIncidence {
    pub(super) node: StyleNodeID,
    pub(super) entry: u32,
}

impl Default for RetainedSelectorIncidences {
    fn default() -> Self {
        Self {
            by_program: Vec::new(),
            residency: MemoryLease::new(MemoryCategory::RetainedSelectorIncidence),
        }
    }
}

impl RetainedSelectorIncidences {
    pub(super) fn lookup(&self, program: SelectorProgramID) -> Option<Rc<[RetainedSelectorIncidence]>> {
        self.by_program
            .get(program.0 as usize)
            .and_then(Option::as_ref)
            .cloned()
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.by_program];
            cached [];
            nested [self
                .by_program
                .iter()
                .flatten()
                .map(|incidences| incidences.len() * size_of::<RetainedSelectorIncidence>())
                .sum::<usize>()];
            skip [self.residency];
        }
    }

    pub(super) fn remember(
        &mut self,
        program: SelectorProgramID,
        incidences: Vec<RetainedSelectorIncidence>,
        memory: &mut MemoryController,
    ) -> Option<Rc<[RetainedSelectorIncidence]>> {
        if self.by_program.get(program.0 as usize).is_none_or(Option::is_none)
            && !memory.is_tier3_admitting(MemoryCategory::RetainedSelectorIncidence)
        {
            return None;
        }
        let required_len = program.0 as usize + 1;
        let incidences: Rc<[RetainedSelectorIncidence]> = incidences.into();
        if self.by_program.len() < required_len {
            self.by_program.resize(required_len, None);
        }
        self.by_program[program.0 as usize] = Some(Rc::clone(&incidences));
        let bytes = self.capacity_bytes();
        self.residency.reconcile_committed(memory, bytes);
        memory.finish_committed_acceleration_growth(MemoryCategory::RetainedSelectorIncidence);
        Some(incidences)
    }

    pub(super) fn clear(&mut self) {
        self.by_program = Vec::new();
        self.residency.release();
    }
}

impl Default for RetainedMatchAnswers {
    fn default() -> Self {
        Self {
            column: Vec::new(),
            cascade_input_column: Vec::new(),
            cascade_input_memory: MemoryLease::new(MemoryCategory::MatchAnswerIdentity),
            residency: MemoryLease::new(MemoryCategory::RetainedMatchAnswer),
        }
    }
}

pub(super) struct RetainedAnswerPatchRule {
    pub(super) rule: RuleID,
    pub(super) program: SelectorProgramID,
}

pub(super) struct RetainedAnswerPatch {
    pub(super) rules: Vec<RetainedAnswerPatchRule>,
    /// Whether this transaction can reorder rules relative to each other (layer or sheet order).
    /// An unchanged match set can then still compact to a different winner, so the unchanged
    /// fast path must not conclude anything from set equality.
    pub(super) orders_shifted: bool,
    /// The (rule, program) identities of `rules`, sorted, as the batch matcher's rule filter.
    pub(super) rule_keys: Vec<(RuleID, SelectorProgramID)>,
    pub(super) scope_program: ScopeProgramID,
    pub(super) dispatch: Rc<RuleDispatch>,
    /// One shared match workspace for every node this patch visits, carrying the relation and
    /// sibling-prefix caches across them exactly as a matching traversal does.
    pub(super) match_workspace: MatchEvaluationWorkspace,
    pub(super) prefix_caches: Rc<RefCell<PrefixCaches>>,
    pub(super) dispatch_workspace: DispatchCandidateWorkspace,
    pub(super) always_emit: bool,
    /// A program join can make a rule contribute without producing a signed selector-truth delta
    /// for every node the join reaches. Those nodes must evaluate the affected rule set instead of
    /// treating deltas from concurrent element inputs as a complete patch.
    pub(super) requires_full_match: bool,
    pub(super) cascade_update_properties: Vec<u16>,
    pub(super) cascade_update_rules: Vec<RuleID>,
    pub(super) cascade_candidates: Vec<OrderedCascadeCandidate>,
    pub(super) cascade_compaction_workspace: ordering::CascadeCompactionWorkspace,
    pub(super) program_base_version: Option<ProgramVersion>,
    /// Memoized stopping delta transitions, shared by every node whose retained answer identity,
    /// compact cascade identity, and consolidated delta content are equal. The patch's cascade
    /// orders are fixed for the flush and the transition is node-independent for comparable
    /// document-scope answers, so a container of equal siblings pays the O(answer) derivation once
    /// and every further member is one column store.
    pub(super) delta_memo: HashMap<RetainedAnswerDeltaMemoKey, RetainedAnswerDeltaMemoEntry>,
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) struct RetainedAnswerDeltaMemoKey {
    pub(super) old_answer: MatchAnswerID,
    pub(super) old_cascade_input: MatchAnswerID,
    pub(super) delta_count: usize,
    pub(super) delta_digest: u64,
}

pub(super) struct RetainedAnswerDeltaMemoEntry {
    pub(super) deltas: Vec<(RuleID, EntryID, SetChange)>,
    pub(super) transition: RetainedAnswerDeltaTransition,
}

impl RetainedAnswerDeltaMemoEntry {
    pub(super) fn capacity_bytes(&self) -> u64 {
        (self.deltas.capacity() * size_of::<(RuleID, EntryID, SetChange)>()) as u64
    }
}

#[derive(Clone, Copy)]
pub(super) struct RetainedAnswerDeltaTransition {
    pub(super) new_answer: MatchAnswerID,
    /// The compact identity after the transition. Equal to the asker's old identity exactly when
    /// the transition stopped.
    pub(super) new_cascade_input: MatchAnswerID,
    /// The winner state the cohort's first member settled after applying the same deltas, with
    /// its program version, when one was retained. Emitting replays assign it by column store.
    pub(super) winner_state: Option<(CascadeStateID, ProgramVersion)>,
    /// Whether the first member's winner application reported an update, which decides whether
    /// replays hand the traversal an incremental cascade answer.
    pub(super) winners_updated: bool,
    pub(super) cascade_winners_are_complete: bool,
}

pub(super) struct RetainedAnswerPatchOutcome {
    pub(super) emit: bool,
    /// The patch proved the answer identity and winner state unchanged and emits only because
    /// the plan demands it; publication may confirm such a reaction away without any re-proof.
    pub(super) identity_preserved: bool,
    pub(super) incremental_cascade_answer: Option<IncrementalCascadeAnswer>,
}

pub(super) struct IncrementalCascadeAnswer {
    pub(super) node: StyleNodeID,
    pub(super) cascade_input: MatchAnswerID,
    pub(super) matches: Option<Box<[RuleMatch]>>,
    pub(super) cascade_winners_are_complete: bool,
}

impl RetainedAnswerPatch {
    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.rules,
                self.cascade_update_properties,
                self.cascade_update_rules,
                self.cascade_candidates,
                self.delta_memo,
            ];
            cached [];
            nested [
                self.dispatch_workspace.capacity_bytes(),
                self.cascade_compaction_workspace.capacity_bytes(),
                self.delta_memo
                    .values()
                    .map(RetainedAnswerDeltaMemoEntry::capacity_bytes)
                    .sum::<u64>(),
            ];
            skip [];
        }
    }
}

#[derive(Default)]
pub(super) struct RetainedAnswerPatchSelection {
    pub(super) affected: Vec<RetainedAnswerPatchSelectionRule>,
    pub(super) always_emit: bool,
    pub(super) orders_shifted: bool,
    pub(super) requires_full_match: bool,
    pub(super) cascade_update_properties: Vec<u16>,
    pub(super) cascade_update_rules: Vec<RuleID>,
    pub(super) program_base_version: Option<ProgramVersion>,
}

pub(super) struct RetainedAnswerPatchSelectionRule {
    pub(super) rule: RuleID,
    pub(super) program: SelectorProgramID,
    pub(super) evaluate: bool,
}

impl RetainedMatchAnswers {
    pub(super) fn answer_identity(&self, node: StyleNodeID) -> Option<MatchAnswerID> {
        let index = node.element_index()? as usize;
        self.column
            .get(index)
            .copied()
            .filter(|identity| *identity != MatchAnswerID::default())
    }

    fn cascade_input_capacity_bytes(&self, catalog: &MatchAnswerCatalog) -> u64 {
        capacity_bytes! {
            shallow [self.cascade_input_column];
            cached [catalog.cascade_payload_bytes];
            nested [];
            skip [];
        }
    }

    pub(super) fn release_swept_cascade_payloads(&mut self, bytes: u64) {
        self.cascade_input_memory.shrink_committed(bytes);
    }

    pub(super) fn remember_prepared(
        &mut self,
        catalog: &mut MatchAnswerCatalog,
        node: StyleNodeID,
        answer: Vec<RetainedRuleMatch>,
        memory: &mut MemoryController,
    ) -> Result<(), Vec<RetainedRuleMatch>> {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return Err(answer);
        };
        let answer_hash = hash_retained_rule_matches(&answer);
        let held_identity = catalog.identity(&answer, answer_hash);
        let identity_is_retained = held_identity.is_some_and(|identity| catalog.identity_is_retained(identity));
        let previous_identity = self
            .column
            .get(index)
            .map_or(MatchAnswerID::default(), |identity| *identity);
        if previous_identity == MatchAnswerID::default()
            && !memory.is_tier3_admitting(MemoryCategory::RetainedMatchAnswer)
        {
            return Err(answer);
        }

        let identity = match (held_identity, identity_is_retained) {
            (Some(identity), true) => {
                if identity != previous_identity {
                    catalog.retain_identity(identity);
                }
                identity
            }
            (Some(identity), false) => {
                catalog.retain_identity(identity);
                identity
            }
            (None, _) => catalog.insert_retained(answer, answer_hash),
        };
        if self.column.len() <= index {
            self.column.resize(index + 1, MatchAnswerID::default());
        }
        self.column[index] = identity;
        if previous_identity != MatchAnswerID::default() && previous_identity != identity {
            catalog.release_identity(previous_identity);
        }
        let current = self.capacity_bytes(catalog);
        self.residency.reconcile_committed(memory, current);
        memory.finish_committed_acceleration_growth(MemoryCategory::RetainedMatchAnswer);
        Ok(())
    }

    /// Repoint one node's retained answer to an identity the catalog already holds. The column may
    /// grow within its existing capacity, but neither the identity nor the column may allocate.
    pub(super) fn set_interned_identity(
        &mut self,
        node: StyleNodeID,
        catalog: &mut MatchAnswerCatalog,
        identity: MatchAnswerID,
    ) -> bool {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return false;
        };
        if index >= self.column.capacity() || catalog.retained_answer(identity).is_none() {
            return false;
        }
        if self.column.len() <= index {
            self.column.resize(index + 1, MatchAnswerID::default());
        }
        let previous_identity = self.column[index];
        if previous_identity == identity {
            return true;
        }
        catalog.retain_identity(identity);
        self.column[index] = identity;
        if previous_identity != MatchAnswerID::default() {
            catalog.release_identity(previous_identity);
        }
        self.residency.shrink_to(self.capacity_bytes(catalog));
        true
    }

    pub(super) fn remember_cascade_input(
        &mut self,
        catalog: &mut MatchAnswerCatalog,
        node: StyleNodeID,
        cascade_input: MatchAnswerID,
        memory: &mut MemoryController,
    ) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        if self.cascade_input_column.len() <= index {
            self.cascade_input_column.resize(index + 1, MatchAnswerID::default());
            let current = self.cascade_input_capacity_bytes(catalog);
            self.cascade_input_memory.resize_required_to(memory, current);
        }
        let previous = std::mem::replace(&mut self.cascade_input_column[index], cascade_input);
        if previous == cascade_input {
            return;
        }
        if previous != MatchAnswerID::default() {
            catalog.release_cascade(previous);
        }
        catalog.retain_cascade(cascade_input);
        let current = self.cascade_input_capacity_bytes(catalog);
        self.cascade_input_memory.resize_required_to(memory, current);
    }

    pub(super) fn forget_cascade_input(&mut self, catalog: &mut MatchAnswerCatalog, node: StyleNodeID) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        let Some(slot) = self.cascade_input_column.get_mut(index) else {
            return;
        };
        let previous = std::mem::take(slot);
        if previous != MatchAnswerID::default() {
            catalog.release_cascade(previous);
            self.cascade_input_memory
                .shrink_to(self.cascade_input_capacity_bytes(catalog));
        }
    }

    pub(super) fn cascade_input_lookup(&self, node: StyleNodeID) -> Lookup<&MatchAnswerID, StyleNodeID> {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return Lookup::Missing(node);
        };
        match self.cascade_input_column.get(index) {
            Some(cascade_input) if *cascade_input != MatchAnswerID::default() => Lookup::Known(cascade_input),
            _ => Lookup::Missing(node),
        }
    }

    /// Retire the optional exact payload while preserving its compact semantic old side.
    pub(super) fn forget_answer(&mut self, catalog: &mut MatchAnswerCatalog, node: StyleNodeID) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        if let Some(slot) = self.column.get_mut(index) {
            let identity = std::mem::take(slot);
            if identity != MatchAnswerID::default() {
                catalog.release_identity(identity);
                self.residency.shrink_to(self.capacity_bytes(catalog));
            }
        }
    }

    pub(super) fn for_each_answer_node(&self, mut visit: impl FnMut(StyleNodeID)) {
        for (index, identity) in self.column.iter().enumerate().skip(1) {
            if *identity != MatchAnswerID::default() {
                visit(StyleNodeID::element(
                    u32::try_from(index).expect("retained answer column exceeds style node identity space"),
                ));
            }
        }
    }

    pub(super) fn for_each_answer_containing_any_rule(
        &self,
        catalog: &MatchAnswerCatalog,
        rules: &[RuleID],
        mut visit: impl FnMut(StyleNodeID),
    ) {
        debug_assert!(rules.is_sorted());
        self.for_each_answer_node(|node| {
            let index = node.element_index().unwrap() as usize;
            if catalog
                .retained_answer(self.column[index])
                .is_some_and(|answer| answer.iter().any(|matched| rules.binary_search(&matched.rule).is_ok()))
            {
                visit(node);
            }
        });
    }

    /// Drop one node's retained answer. A plan that names a node no longer vouches for the answer
    /// remembered before the change it routes, and a departed identity must not leave one behind
    /// for a reused dense slot. The interned payload stays shared with any other node holding it.
    pub(super) fn forget(&mut self, catalog: &mut MatchAnswerCatalog, node: StyleNodeID) {
        self.forget_answer(catalog, node);
        self.forget_cascade_input(catalog, node);
    }

    pub(super) fn capacity_bytes(&self, catalog: &MatchAnswerCatalog) -> u64 {
        capacity_bytes! {
            shallow [self.column];
            cached [catalog.retained_capacity_bytes()];
            nested [];
            skip [
                self.cascade_input_column,
                self.residency,
                self.cascade_input_memory,
            ];
        }
    }

    pub(super) fn evict(&mut self, catalog: &mut MatchAnswerCatalog) {
        catalog.evict_retained();
        self.column = Vec::new();
        self.residency.release();
    }
}

impl RetainedMatchAnswers {
    pub(super) fn lookup(&self, node: StyleNodeID) -> Lookup<&MatchAnswerID, StyleNodeID> {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return Lookup::Missing(node);
        };
        let Some(identity) = self
            .column
            .get(index)
            .filter(|&&identity| identity != MatchAnswerID::default())
        else {
            return Lookup::Missing(node);
        };
        Lookup::Known(identity)
    }
}

/// Matching scratch owned by one synchronous style traversal.
pub(super) struct BatchMatchingTraversal {
    pub(super) root: StyleNodeID,
    pub(super) batch: Option<MatchingFactBatch>,
    pub(super) topology: Option<TransactionTopology>,
    pub(super) reuse_retained_match_answers: bool,
    pub(super) retained_answer_dispatch: Option<Rc<RuleDispatch>>,
    pub(super) ancestor_requirements: AncestorRequirementsCache,
    pub(super) prefix_caches: Rc<RefCell<PrefixCaches>>,
    pub(super) match_workspace: MatchEvaluationWorkspace,
    pub(super) match_workspace_bytes: u64,
    pub(super) dispatch_workspace: DispatchCandidateWorkspace,
    pub(super) dispatch_workspace_bytes: u64,
    pub(super) cascade_compaction_workspace: ordering::CascadeCompactionWorkspace,
    pub(super) cascade_compaction_workspace_bytes: u64,
}

/// Current-side matching scratch produced by the transaction immediately before a style
/// traversal.
pub(super) struct PreparedBatchMatchingTraversal {
    pub(super) root: StyleNodeID,
    pub(super) batch: Option<MatchingFactBatch>,
    pub(super) topology: Option<TransactionTopology>,
    pub(super) reuse_retained_match_answers: bool,
    pub(super) match_workspace: MatchEvaluationWorkspace,
}

impl PreparedBatchMatchingTraversal {
    pub(super) fn new(root: StyleNodeID) -> Self {
        Self {
            root,
            batch: None,
            topology: None,
            reuse_retained_match_answers: false,
            match_workspace: MatchEvaluationWorkspace::default(),
        }
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [];
            nested [
                self.batch.as_ref().map_or(0, MatchingFactBatch::capacity_bytes),
                self.topology.as_ref().map_or(0, TransactionTopology::capacity_bytes),
                self.match_workspace.capacity_bytes(),
            ];
            skip [self.root, self.reuse_retained_match_answers];
        }
    }
}

/// Distinct retained cascade states per (dispatch key, winner-group generation), shared by the
/// route-pruning proofs of one routing pass. `None` records incomplete posting coverage, which is
/// a `false` verdict for every asker under that key.
pub(super) type RoutePruningStateCache = HashMap<(DispatchKey, u64), Option<Rc<Vec<CascadeStateID>>>>;

pub(super) struct PublishedMatchAnswer {
    pub(super) node: StyleNodeID,
    pub(super) cascade_input: Option<MatchAnswerID>,
    pub(super) matches: Option<Box<[RuleMatch]>>,
    pub(super) cascade_winners_are_complete: bool,
    pub(super) observed: bool,
}

pub(super) struct PublishedMatchAnswers {
    pub(super) entries: Vec<PublishedMatchAnswer>,
    pub(super) shared_payloads: HashMap<MatchAnswerID, Box<[RuleMatch]>>,
    pub(super) memory: MemoryLease,
    pub(super) match_element_calls_at_publication: u64,
    pub(super) discard_unobserved_retained_answers: bool,
}

impl Default for PublishedMatchAnswers {
    fn default() -> Self {
        Self {
            entries: Vec::new(),
            shared_payloads: HashMap::default(),
            memory: MemoryLease::new(MemoryCategory::BatchScratch),
            match_element_calls_at_publication: 0,
            discard_unobserved_retained_answers: false,
        }
    }
}

impl PublishedMatchAnswers {
    #[cfg(test)]
    pub(super) fn recompute_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.entries, self.shared_payloads];
            cached [];
            nested [
                self
                .entries
                .iter()
                .map(|entry| {
                    entry
                        .matches
                        .as_ref()
                        .map_or(0, |matches| (matches.len() * size_of::<RuleMatch>()) as u64)
                })
                .sum::<u64>(),
                self
                .shared_payloads
                .values()
                .map(|matches| (matches.len() * size_of::<RuleMatch>()) as u64)
                .sum::<u64>(),
            ];
            skip [
                self.memory,
                self.match_element_calls_at_publication,
                self.discard_unobserved_retained_answers,
            ];
        }
    }

    pub(super) fn push(
        &mut self,
        mut entry: PublishedMatchAnswer,
        memory: &mut MemoryController,
        counters: &mut Counters,
    ) {
        let entries_capacity_before = self.entries.capacity();
        let shared_payload_capacity_before = self.shared_payloads.capacity();
        let mut added_payload_bytes = 0;
        if let Some(cascade_input) = entry.cascade_input {
            if let Some(matches) = entry.matches.take() {
                match self.shared_payloads.entry(cascade_input) {
                    std::collections::hash_map::Entry::Occupied(_) => {
                        counters.bump(Counter::PublishedMatchAnswerSharedPayloadReuses);
                    }
                    std::collections::hash_map::Entry::Vacant(shared) => {
                        added_payload_bytes = matches.len() * size_of::<RuleMatch>();
                        shared.insert(matches);
                        counters.bump(Counter::PublishedMatchAnswerSharedPayloads);
                    }
                }
            }
        } else {
            added_payload_bytes = entry
                .matches
                .as_ref()
                .map_or(0, |matches| matches.len() * size_of::<RuleMatch>());
            counters.bump(Counter::PublishedMatchAnswerContextualPayloads);
        }
        self.entries.push(entry);
        let added_bytes = (self.entries.capacity() - entries_capacity_before) * size_of::<PublishedMatchAnswer>()
            + (self.shared_payloads.capacity() - shared_payload_capacity_before)
                * (size_of::<MatchAnswerID>() + size_of::<Box<[RuleMatch]>>() + 1)
            + added_payload_bytes;
        let added_bytes = added_bytes as u64;
        self.memory.grow_required(memory, added_bytes);
    }

    pub(super) fn sort(&mut self) {
        self.entries.sort_unstable_by_key(|entry| entry.node);
    }

    pub(super) fn lookup(&self, node: StyleNodeID) -> Option<&PublishedMatchAnswer> {
        let index = self.entries.binary_search_by_key(&node, |entry| entry.node).ok()?;
        self.entries.get(index)
    }

    pub(super) fn mark_observed(&mut self, node: StyleNodeID) {
        let Ok(index) = self.entries.binary_search_by_key(&node, |entry| entry.node) else {
            return;
        };
        self.entries[index].observed = true;
    }

    pub(super) fn matches_for<'a>(&'a self, answer: &'a PublishedMatchAnswer) -> Option<&'a [RuleMatch]> {
        match answer.cascade_input {
            Some(cascade_input) => self.shared_payloads.get(&cascade_input).map(Box::as_ref),
            None => answer.matches.as_deref(),
        }
    }

    pub(super) fn clear(&mut self) {
        self.entries = Vec::new();
        self.shared_payloads = HashMap::default();
        self.memory.release();
    }

    pub(super) fn release(mut self) {
        self.clear();
    }
}

pub(super) struct TransactionFactClassification {
    pub(super) prefix: Option<PrefixFactTransition>,
    pub(super) retained_truth_available: bool,
}

// A promoted batch costs one document row per element to build. Wait until local asks have already
// packed four times that many rows, then require another ask to prove reuse. Even if that ask is the
// traversal's last, the speculative rebuild keeps total packing below 1.25 times the local work.

/// The document-origin sheets a tree scope adds to its own ordered sheet set.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) enum DocumentSheetMode {
    None,
    NonAuthor,
    All,
}

/// Every input that can change a scope's immutable selector dispatch or its static cascade ranks.
///
/// Constructed sheets commonly attach the same ordered set of programs to hundreds of shadow
/// roots. The matches still belong to the concrete scope being evaluated, but the dispatch they
/// evaluate against contains no scope-local state and can be shared wherever this key is equal.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub(super) struct ScopeDispatchKey {
    pub(super) depth: u32,
    pub(super) document_sheet_mode: DocumentSheetMode,
    pub(super) sheets: Vec<(SheetID, u64)>,
    pub(super) layer_order: Vec<(CascadeLayerID, u32)>,
}

/// The selector-derived topology shared by scopes whose concrete sheets contain the same compiled
/// selector programs in the same order. Rule identities and cascade ranks remain scope-local.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub(super) struct ScopeDispatchShape(pub(super) Vec<(SelectorProgramID, bool)>);

/// Every part of static cascade ordering that is not a concrete rule or sheet identity.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub(super) struct ScopeCascadeShape {
    pub(super) dispatch: ScopeDispatchShape,
    pub(super) depth: u32,
    pub(super) document_sheet_mode: DocumentSheetMode,
    pub(super) rule_origins_and_layers: Vec<(u8, CascadeLayerID)>,
    pub(super) layer_order: Vec<(CascadeLayerID, u32)>,
}

/// The ordered ancestor keys whose dense indices define an ancestor-requirement table.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub(super) struct AncestorDispatchShape(pub(super) Vec<DispatchKey>);

define_id! {
    /// The immutable selector and static-cascade program shared by equivalent tree scopes.
    pub(super) struct ScopeProgramID(pub(super));
}

impl super::intern_table::InternIdentity for ScopeProgramID {
    fn index(self) -> usize {
        self.0 as usize
    }
}

pub(super) struct ScopeProgram {
    pub(super) key: ScopeDispatchKey,
    pub(super) dispatch: Rc<RuleDispatch>,
    pub(super) scope_count: u32,
}

pub(super) struct ReplacedStyleRule {
    pub(super) rule: RuleID,
    pub(super) version: RuleVersion,
    pub(super) declared: Vec<DeclaredProperty>,
    pub(super) declarations_are_complete: bool,
    pub(super) gated_by_container_query: bool,
}

pub(super) struct PendingRuleDeclarationChange {
    pub(super) rule: RuleID,
    pub(super) old_properties: Vec<u16>,
    pub(super) new_properties: Vec<u16>,
}

#[derive(Clone)]
pub(super) struct PendingRuleDeclarations {
    pub(super) declared: Vec<DeclaredProperty>,
    pub(super) complete: bool,
}

pub(super) struct SheetRuleReplacement {
    pub(super) sheet: SheetID,
    pub(super) rules: Vec<ReplacedStyleRule>,
    pub(super) reused: usize,
    pub(super) reuse_disabled: bool,
}

#[cfg(test)]
#[derive(Default)]
pub(super) struct DiagnosticPlanCapture {
    pub(super) nodes: Vec<u32>,
    pub(super) scoped: bool,
}

pub(crate) struct RecordedAtomMappings {
    pub atoms: Vec<(u64, u32)>,
    pub qualified_atoms: Vec<(u32, u32, u32)>,
}
