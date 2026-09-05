/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::batch_matcher::{append_selector_truth_matches, insert_scope_rule};
use super::selector::{AttributeCase, AttributeOperator};
use super::*;

const MIN_SHARED_CASCADE_COMPLETION_SAVINGS: usize = 8;

#[derive(Default)]
pub(crate) struct SelectorQueryCache {
    attribute_value_catalog_version: u64,
    attribute_values: Vec<Option<Vec<StyleAtomID>>>,
}

#[derive(Clone, Copy)]
pub(crate) struct SelectorQueryContext {
    pub(crate) root: StyleNodeID,
    pub(crate) include_root: bool,
    pub(crate) scope_root: Option<StyleNodeID>,
    pub(crate) shadow_root: Option<StyleNodeID>,
    pub(crate) has_document_root: bool,
}

impl SelectorQueryCache {
    #[must_use]
    pub(crate) fn capacity_bytes(&self) -> u64 {
        (self.attribute_values.capacity() * size_of::<Option<Vec<StyleAtomID>>>()
            + self
                .attribute_values
                .iter()
                .flatten()
                .map(|values| values.capacity() * size_of::<StyleAtomID>())
                .sum::<usize>()) as u64
    }
}

fn verify_match_answer_against_cold(
    engine: &mut StyleEngine,
    answer: &[RuleMatch],
    node: StyleNodeID,
    description: &str,
) {
    verify_style_answer_patch(engine, |verifier| {
        verifier.verify_match_answer(answer, node, description);
    });
}

fn verify_cascade_answer_against_cold(
    engine: &mut StyleEngine,
    answer: &[RuleMatch],
    node: StyleNodeID,
    description: &str,
) {
    verify_style_answer_patch(engine, |verifier| {
        verifier.verify_cascade_answer(answer, node, description);
    });
}

impl StyleEngine {
    fn retained_answer_delta_memo_key(
        old_answer: MatchAnswerID,
        old_cascade_input: MatchAnswerID,
        deltas: &[SelectorTruthDelta],
    ) -> RetainedAnswerDeltaMemoKey {
        let mut hasher = fast_hash::fast_hasher();
        for delta in deltas {
            (delta.rule, delta.entry, delta.change).hash(&mut hasher);
        }
        RetainedAnswerDeltaMemoKey {
            old_answer,
            old_cascade_input,
            delta_count: deltas.len(),
            delta_digest: hasher.finish(),
        }
    }

    fn retained_answer_delta_memo_entry_matches(
        entry: &RetainedAnswerDeltaMemoEntry,
        deltas: &[SelectorTruthDelta],
    ) -> bool {
        entry
            .deltas
            .iter()
            .copied()
            .eq(deltas.iter().map(|delta| (delta.rule, delta.entry, delta.change)))
    }

    /// The pseudo-element winner states a node settled this flush for the kinds the engine
    /// settles pseudo-elements from, to travel with the node's winner state: ::before, ::after,
    /// ::first-letter and ::selection. The rules for the other kinds match every element, and a
    /// row for each of them on every element would cost the memory the winner groups have.
    fn settled_pseudo_winner_states(&self, node: StyleNodeID) -> Rc<[(tree::PseudoElementTarget, CascadeStateID)]> {
        self.winner_groups
            .pseudo_states(node)
            .filter(|&(pseudo, version, _, priority_current)| {
                matches!(pseudo.kind.0, 0 | 2 | 3 | 6) && version == self.program.version() && priority_current
            })
            .map(|(pseudo, _, state, _)| (pseudo, state))
            .collect()
    }

    fn remember_retained_answer_delta_transition(
        patch: &mut RetainedAnswerPatch,
        key: RetainedAnswerDeltaMemoKey,
        deltas: &[SelectorTruthDelta],
        transition: RetainedAnswerDeltaTransition,
    ) {
        use std::collections::hash_map::Entry;

        match patch.delta_memo.entry(key) {
            Entry::Vacant(entry) => {
                entry.insert(RetainedAnswerDeltaMemoEntry {
                    deltas: deltas
                        .iter()
                        .map(|delta| (delta.rule, delta.entry, delta.change))
                        .collect(),
                    transition,
                });
            }
            Entry::Occupied(mut entry) if Self::retained_answer_delta_memo_entry_matches(entry.get(), deltas) => {
                entry.get_mut().transition = transition;
            }
            // A digest collision only forfeits this memo opportunity. It must not replace or use
            // the unrelated transition already stored under the fixed-size probe key.
            Entry::Occupied(_) => {}
        }
    }

    pub fn prepare_selector_query(&mut self) {
        let has_staged_structure = !(self.tree_staging.is_empty() || self.tree_staging.is_applied());
        self.apply_staged_tree_deltas();
        self.discard_prepared_batch_matching_traversal();
        self.facts.prepare_selector_query(&mut self.memory);
        // Every candidate of the coming query shares one tree — so its sibling positions are computed once, and reused.
        // They stay valid until something changes: Every mutation reaches this engine either as a staged-tree delta or
        // as a non-empty style transaction — and the second is what advances the transaction version. A run of queries
        // with neither in between — the querySelector-in-a-loop shape — keeps one workspace for the whole run.
        let settled_version = self.next_style_transaction_version;
        if has_staged_structure || settled_version != self.query_settled_transaction_version {
            self.selector_query_generation = self.selector_query_generation.wrapping_add(1);
            self.query_settled_transaction_version = settled_version;
        }
    }

    pub(crate) fn selector_query_matches(
        &mut self,
        program: &SelectorProgram,
        node: StyleNodeID,
        scope_root: Option<StyleNodeID>,
        shadow_root: Option<StyleNodeID>,
        has_document_root: bool,
    ) -> Result<bool, Incomplete> {
        let share_sibling_geometry = program.has_positional_test();
        if share_sibling_geometry && self.query_workspace_generation != self.selector_query_generation {
            self.query_match_workspace = MatchEvaluationWorkspace::for_selector_query();
            self.query_workspace_generation = self.selector_query_generation;
        }
        let mut evaluator = MatchEvaluator::new(&self.tree, self.facts.primary());
        if share_sibling_geometry {
            evaluator = evaluator.with_match_workspace(&self.query_match_workspace, MatchEvaluationSide::Current);
        }
        if !has_document_root {
            evaluator = evaluator.without_document_root();
        }
        if let Some(scope_root) = scope_root {
            evaluator = evaluator.with_scope_root(scope_root);
        }
        if let Some(shadow_root) = shadow_root {
            evaluator = evaluator.in_shadow_tree(shadow_root);
        }
        for entry in program.entries() {
            if entry.pseudo_element.is_none() && evaluator.matches_entry(program, entry, node, &mut self.counters)? {
                return Ok(true);
            }
        }
        Ok(false)
    }

    pub(crate) fn selector_query_all(
        &mut self,
        program: &SelectorProgram,
        cache: &mut SelectorQueryCache,
        context: SelectorQueryContext,
    ) -> Result<Vec<StyleNodeID>, Incomplete> {
        let matches = self.evaluate_selector_query(program, cache, context)?;
        if matches.len() <= 1 {
            return Ok(matches.into_iter().collect());
        }

        let match_count = matches.len();
        let mut ordered_matches = Vec::with_capacity(match_count);
        for node in self.tree.preorder(context.root) {
            if matches.contains(&node) {
                ordered_matches.push(node);
                if ordered_matches.len() == match_count {
                    break;
                }
            }
        }
        debug_assert_eq!(ordered_matches.len(), match_count);
        Ok(ordered_matches)
    }

    /// The first match in tree order, or None. One engine call serves a whole querySelector:
    /// candidate enumeration, evaluation, and tree ordering all stay on this side of the
    /// boundary — instead of one boundary crossing per walked element.
    ///
    /// When every entry's subject carries posting-backed dispatch keys, only posted candidates
    /// are evaluated, in tree order, stopping at the first hit. Otherwise, the subtree is walked
    /// in tree order, and each element evaluated — still one boundary crossing for the query.
    pub(crate) fn selector_query_first(
        &mut self,
        program: &SelectorProgram,
        context: SelectorQueryContext,
    ) -> Result<Option<StyleNodeID>, Incomplete> {
        let share_sibling_geometry = program.has_positional_test();
        if share_sibling_geometry && self.query_workspace_generation != self.selector_query_generation {
            self.query_match_workspace = MatchEvaluationWorkspace::for_selector_query();
            self.query_workspace_generation = self.selector_query_generation;
        }
        let candidates_are_known = self.ensure_sorted_query_candidates(program, context.root);

        let mut evaluator = MatchEvaluator::new(&self.tree, self.facts.primary());
        if share_sibling_geometry {
            evaluator = evaluator.with_match_workspace(&self.query_match_workspace, MatchEvaluationSide::Current);
        }
        if !context.has_document_root {
            evaluator = evaluator.without_document_root();
        }
        if let Some(scope_root) = context.scope_root {
            evaluator = evaluator.with_scope_root(scope_root);
        }
        if let Some(shadow_root) = context.shadow_root {
            evaluator = evaluator.in_shadow_tree(shadow_root);
        }

        if candidates_are_known {
            // With a single dispatch key, membership in its posting proved that feature on every
            // candidate — but only for key kinds where the posting means exactly what the feature
            // tests. An id or class key does: the posting holds the elements carrying that very
            // atom. A tag-name key is folded while the feature test is case-sensitive for foreign
            // elements, and an attribute-name key proves presence while the feature may compare a
            // value — those must still be evaluated.
            let proven_key = match &self.query_sorted_candidates_stamp {
                Some(stamp)
                    if stamp.keys.len() == 1 && matches!(stamp.keys[0], DispatchKey::Id(_) | DispatchKey::Class(_)) =>
                {
                    Some(stamp.keys[0])
                }
                _ => None,
            };
            for index in 0..self.query_sorted_candidates.len() {
                let node = self.query_sorted_candidates[index];
                if !context.include_root && node == context.root {
                    continue;
                }
                for entry in program.entries() {
                    if entry.pseudo_element.is_some() {
                        continue;
                    }
                    let matched = match proven_key {
                        Some(key) => {
                            evaluator.matches_entry_after_dispatch(program, entry, key, node, &mut self.counters)?
                        }
                        None => evaluator.matches_entry(program, entry, node, &mut self.counters)?,
                    };
                    if matched {
                        return Ok(Some(node));
                    }
                }
            }
            return Ok(None);
        }

        for node in self.tree.preorder(context.root) {
            if !context.include_root && node == context.root {
                continue;
            }
            for entry in program.entries() {
                if entry.pseudo_element.is_none()
                    && evaluator.matches_entry(program, entry, node, &mut self.counters)?
                {
                    return Ok(Some(node));
                }
            }
        }
        Ok(None)
    }

    /// Refresh `query_sorted_candidates` for this program's subject keys under `root` — or report
    /// that postings can't name every entry's candidates, and the caller has to walk.
    fn ensure_sorted_query_candidates(&mut self, program: &SelectorProgram, root: StyleNodeID) -> bool {
        let mut keys: Vec<DispatchKey> = Vec::new();
        for (entry_index, entry) in program.entries().iter().enumerate() {
            if entry.pseudo_element.is_some() {
                continue;
            }
            let dispatch_keys = program.subject_dispatch_keys(entry_index);
            if dispatch_keys.is_empty() {
                return false;
            }
            for &key in dispatch_keys {
                if !key.has_selector_posting() {
                    return false;
                }
                keys.push(key);
            }
        }
        if keys.is_empty() {
            return false;
        }
        keys.sort_unstable();
        keys.dedup();
        let generation = self.selector_query_generation;
        if let Some(stamp) = &self.query_sorted_candidates_stamp
            && stamp.generation == generation
            && stamp.root == root
            && stamp.keys == keys
        {
            return true;
        }
        // The union of every key's posting is a superset of every entry's true matches. Missing
        // means the facts haven't materialized that posting; the walk is the correct fallback —
        // rather than forcing a materialization from the query path.
        let mut candidates: Vec<StyleNodeID> = Vec::new();
        for &key in &keys {
            match self.facts.postings().lookup(key) {
                Lookup::Known(posting) => candidates.extend(posting.candidates()),
                Lookup::KnownAbsent => {}
                Lookup::Missing(_) => return false,
            }
        }
        self.ensure_query_preorder_ranks(root);
        let ranks = &self.query_preorder_ranks;
        // A candidate without a rank lies outside the queried subtree.
        let mut ranked: Vec<(u32, StyleNodeID)> = candidates
            .into_iter()
            .filter_map(|node| ranks.get(&node).map(|&rank| (rank, node)))
            .collect();
        ranked.sort_unstable();
        ranked.dedup();
        self.query_sorted_candidates.clear();
        self.query_sorted_candidates
            .extend(ranked.into_iter().map(|(_, node)| node));
        self.query_sorted_candidates_stamp = Some(QuerySortedCandidatesStamp { generation, root, keys });
        true
    }

    fn ensure_query_preorder_ranks(&mut self, root: StyleNodeID) {
        let generation = self.selector_query_generation;
        if self.query_preorder_ranks_stamp == Some((generation, root)) {
            return;
        }
        self.query_preorder_ranks.clear();
        for (rank, node) in self.tree.preorder(root).enumerate() {
            self.query_preorder_ranks
                .insert(node, u32::try_from(rank).unwrap_or(u32::MAX));
        }
        self.query_preorder_ranks_stamp = Some((generation, root));
    }

    fn evaluate_selector_query(
        &mut self,
        program: &SelectorProgram,
        cache: &mut SelectorQueryCache,
        context: SelectorQueryContext,
    ) -> Result<HashSet<StyleNodeID>, Incomplete> {
        let attribute_value_catalog_version = self.facts.attribute_value_catalog_version();
        if cache.attribute_value_catalog_version != attribute_value_catalog_version
            || cache.attribute_values.len() != program.entries().len()
        {
            cache.attribute_values.clear();
            cache.attribute_values.reserve(program.entries().len());
            for entry_index in 0..program.entries().len() {
                let values = program
                    .subject_attribute_value_test(entry_index)
                    .filter(|test| test.operator != AttributeOperator::Presence)
                    .map(|test| {
                        if test.operator != AttributeOperator::Exact || test.case != AttributeCase::Sensitive {
                            self.counters.bump(Counter::SelectorQueryAttributeValueCatalogScans);
                        }
                        self.facts
                            .matching_attribute_values(test, program.literal(test.value_offset, test.value_length))
                    });
                cache.attribute_values.push(values);
            }
            cache.attribute_value_catalog_version = attribute_value_catalog_version;
        } else {
            self.counters.add(
                Counter::SelectorQueryAttributeValuePlanHits,
                u64::try_from(cache.attribute_values.iter().flatten().count()).unwrap_or(u64::MAX),
            );
        }

        // The same tree serves every candidate of this query, so sibling geometry and positional
        // answers are shared across the candidate loop below - exactly as the per-node entry
        // point shares them across the calls of one query.
        let share_sibling_geometry = program.has_positional_test();
        if share_sibling_geometry && self.query_workspace_generation != self.selector_query_generation {
            self.query_match_workspace = MatchEvaluationWorkspace::for_selector_query();
            self.query_workspace_generation = self.selector_query_generation;
        }
        let mut matches = HashSet::default();
        let mut evaluator = MatchEvaluator::new(&self.tree, self.facts.primary());
        if share_sibling_geometry {
            evaluator = evaluator.with_match_workspace(&self.query_match_workspace, MatchEvaluationSide::Current);
        }
        if !context.has_document_root {
            evaluator = evaluator.without_document_root();
        }
        if let Some(scope_root) = context.scope_root {
            evaluator = evaluator.with_scope_root(scope_root);
        }
        if let Some(shadow_root) = context.shadow_root {
            evaluator = evaluator.in_shadow_tree(shadow_root);
        }

        for (entry_index, entry) in program.entries().iter().enumerate() {
            if entry.pseudo_element.is_some() {
                continue;
            }

            let dispatch_keys = program.subject_dispatch_keys(entry_index);
            let mut candidates = Vec::new();
            let mut used_attribute_value_posting = false;
            if let Some(values) = &cache.attribute_values[entry_index]
                && let Some(value_candidates) = self.facts.attribute_value_candidates(values)
            {
                candidates.extend(value_candidates);
                used_attribute_value_posting = true;
            }
            let mut use_all_nodes = !used_attribute_value_posting && dispatch_keys.is_empty();
            if !used_attribute_value_posting && !use_all_nodes {
                for &key in dispatch_keys {
                    if !key.has_selector_posting() {
                        use_all_nodes = true;
                        break;
                    }
                    match self.facts.postings().lookup(key) {
                        Lookup::Known(posting) => {
                            candidates.extend(posting.candidates());
                        }
                        Lookup::KnownAbsent => {}
                        Lookup::Missing(_) => {
                            use_all_nodes = true;
                            break;
                        }
                    }
                }
            }
            if use_all_nodes {
                candidates.clear();
                candidates.extend(
                    self.tree
                        .preorder(context.root)
                        .filter(|&node| context.include_root || node != context.root),
                );
            }
            candidates.retain(|&node| {
                (context.include_root || node != context.root) && self.tree.is_in_subtree_of(node, context.root)
            });
            candidates.sort_unstable();
            candidates.dedup();
            self.counters.add(
                Counter::SelectorQueryCandidateRows,
                u64::try_from(candidates.len()).unwrap_or(u64::MAX),
            );

            for node in candidates {
                if matches.contains(&node) {
                    continue;
                }
                self.counters.bump(Counter::SelectorQueryEvaluations);
                if evaluator.matches_entry(program, entry, node, &mut self.counters)? {
                    matches.insert(node);
                }
            }
        }
        Ok(matches)
    }

    pub(super) fn materialize_cold_matching_batch(
        &mut self,
        root: StyleNodeID,
        topology: Option<&TransactionTopology>,
    ) -> Option<MatchingFactBatch> {
        let fallback_nodes;
        let nodes = if let Some(topology) = topology
            && topology.nodes().len() == self.tree.connected_element_count() as usize
        {
            topology.nodes()
        } else {
            fallback_nodes = self.elements_under(root);
            &fallback_nodes
        };
        let mut batch = StyleNodeFacts::new();
        self.facts.materialize(nodes.iter().copied(), &mut batch);
        let mut expected_row_count = nodes.len();
        for scope_root in self.scope_roots.iter().flatten().copied() {
            if self.tree.is_live(scope_root) && !nodes.contains(&scope_root) {
                self.facts.materialize_missing(std::iter::once(scope_root), &mut batch);
                expected_row_count += 1;
            }
        }
        if batch.row_count() != expected_row_count {
            self.counters.bump(Counter::ColdMatchingBatchMissingRows);
            return None;
        }

        let bytes = batch.capacity_bytes();
        self.memory.reserve_required(MemoryCategory::BatchScratch, bytes);
        self.counters.add(
            Counter::ColdMatchingBatchRows,
            u64::try_from(batch.row_count()).unwrap_or(u64::MAX),
        );
        Some(batch.into())
    }

    pub(super) fn discard_prepared_batch_matching_traversal(&mut self) {
        if let Some(prepared) = self.prepared_batch_matching_traversal.take() {
            self.memory
                .release(MemoryCategory::BatchScratch, prepared.capacity_bytes());
        }
    }

    pub(super) fn discard_published_match_answers(&mut self) {
        let published = std::mem::take(&mut self.published_match_answers);
        verify_published_style_transaction(self, |verifier| {
            assert!(
                published.entries.is_empty()
                    || verifier
                        .counters
                        .get(Counter::MatchElementCallsDuringPublishedStyleTransaction)
                        == published.match_element_calls_at_publication,
                "a style transaction called match_element() after publishing complete match answers"
            );
        });
        if published.discard_unobserved_retained_answers {
            for answer in published.entries.iter().filter(|answer| !answer.observed) {
                self.retained_match_answers.forget(&mut self.match_answers, answer.node);
            }
        }
        published.release();
    }

    pub(super) fn discard_retained_prefix_caches(&mut self) {
        let mut caches = self.prefix_caches.borrow_mut();
        caches.states.release();
        caches.answers.release(&mut self.match_answers);
    }

    pub(super) fn retain_prefix_states(&mut self) {
        if !self
            .prefix_caches
            .borrow_mut()
            .states
            .retain(&mut self.memory, &mut self.counters)
        {
            self.prefix_caches.borrow_mut().states.release();
        }
    }

    pub(super) fn take_prepared_batch_matching_traversal(
        &mut self,
        root: StyleNodeID,
    ) -> Option<PreparedBatchMatchingTraversal> {
        let prepared = self.prepared_batch_matching_traversal.take()?;
        if prepared.root == root {
            return Some(prepared);
        }
        self.memory
            .release(MemoryCategory::BatchScratch, prepared.capacity_bytes());
        None
    }

    pub(super) fn retained_answer_dispatch_for_traversal(
        &mut self,
        reuse_retained_match_answers: bool,
    ) -> Option<Rc<RuleDispatch>> {
        reuse_retained_match_answers.then(|| self.ranked_scope_program(TreeScopeID::DOCUMENT).1)
    }

    /// Share current facts and selector work while a scoped plan completes typed answer misses.
    ///
    /// Retained prefix caches describe the previous transaction, so this batch deliberately starts
    /// empty and is released before publication. Repeated local asks may still promote to one
    /// complete current fact batch after their accumulated reconstruction cost justifies it.
    pub(super) fn begin_published_match_answer_completion_batch(
        &mut self,
        root: StyleNodeID,
        prefer_complete_batch: bool,
    ) {
        debug_assert!(self.batch_matching_traversal.is_none());
        // Each completion batch may ask for exact answers again after a quota boundary reopened
        // retained-answer admission.
        self.completion_exactness_exhausted = false;
        let batch = prefer_complete_batch
            .then(|| {
                self.prepared_batch_matching_traversal
                    .as_mut()
                    .and_then(|prepared| prepared.batch.take())
                    .or_else(|| self.materialize_cold_matching_batch(root, None))
            })
            .flatten();
        // The walk that just converged left the retained states describing THIS transaction,
        // so the completion batch can extend the warm automaton instead of re-deriving every
        // upquery spine. Without a current walk the retained states describe the previous
        // transaction and the batch starts empty as before.
        {
            let mut caches = self.prefix_caches.borrow_mut();
            if !caches.states.is_current() {
                caches.states.release();
                caches.answers.release(&mut self.match_answers);
            }
            caches.states.make_scratch(&mut self.memory);
            caches.answers.make_scratch(&mut self.memory);
        }
        self.batch_matching_traversal = Some(Box::new(BatchMatchingTraversal {
            root,
            batch,
            topology: None,
            reuse_retained_match_answers: false,
            retained_answer_dispatch: None,
            ancestor_requirements: AncestorRequirementsCache::default(),
            prefix_caches: Rc::clone(&self.prefix_caches),
            match_workspace: MatchEvaluationWorkspace::default(),
            match_workspace_bytes: 0,
            dispatch_workspace: DispatchCandidateWorkspace::default(),
            dispatch_workspace_bytes: 0,
            cascade_compaction_workspace: ordering::CascadeCompactionWorkspace::default(),
            cascade_compaction_workspace_bytes: 0,
        }));
    }

    pub(super) fn end_published_match_answer_completion_batch(&mut self) {
        let Some(mut traversal) = self.batch_matching_traversal.take() else {
            return;
        };
        traversal.ancestor_requirements.release(&mut self.memory);
        // The batch extended states describing this transaction's facts, so retain their shared
        // owner instead of discarding them: the next flush's walk completes over
        // warm spines. The sparse origin flag keeps that walk from letting the cache speak for
        // nodes the batch never visited. Unlike the walk's bootstrap retention, this is an
        // opportunistic give-back. If admission already closed, both scratch caches are released.
        {
            let mut caches = self.prefix_caches.borrow_mut();
            caches.states.mark_sparse();
            if caches.states.retain(&mut self.memory, &mut self.counters) {
                caches.states.mark_current();
                if !caches.answers.retain(&mut self.memory) {
                    caches.answers.release(&mut self.match_answers);
                }
            } else {
                caches.states.release();
                caches.answers.release(&mut self.match_answers);
            }
        }
        self.memory
            .release(MemoryCategory::BatchScratch, traversal.match_workspace_bytes);
        self.memory
            .release(MemoryCategory::BatchScratch, traversal.dispatch_workspace_bytes);
        self.memory.release(
            MemoryCategory::BatchScratch,
            traversal.cascade_compaction_workspace_bytes,
        );
        let released_cascade_payload_bytes = self.match_answers.sweep_unreferenced();
        self.retained_match_answers
            .release_swept_cascade_payloads(released_cascade_payload_bytes);
        self.match_answers.compact_if_needed();
        if let Some(batch) = traversal.batch.take() {
            match &mut self.prepared_batch_matching_traversal {
                Some(prepared) if prepared.batch.is_none() => prepared.batch = Some(batch),
                None => {
                    let mut prepared = PreparedBatchMatchingTraversal::new(traversal.root);
                    prepared.batch = Some(batch);
                    self.prepared_batch_matching_traversal = Some(prepared);
                }
                Some(_) => self
                    .memory
                    .release(MemoryCategory::BatchScratch, batch.capacity_bytes()),
            }
        }
    }

    pub(super) fn traversal_with_cold_matching_batch(
        &mut self,
        root: StyleNodeID,
        batch: MatchingFactBatch,
        topology: Option<TransactionTopology>,
        reuse_retained_match_answers: bool,
        match_workspace: MatchEvaluationWorkspace,
    ) -> Box<BatchMatchingTraversal> {
        let match_workspace_bytes = match_workspace.capacity_bytes();
        {
            let mut caches = self.prefix_caches.borrow_mut();
            caches.states.make_scratch(&mut self.memory);
            caches.answers.make_scratch(&mut self.memory);
        }
        let retained_answer_dispatch = self.retained_answer_dispatch_for_traversal(reuse_retained_match_answers);
        Box::new(BatchMatchingTraversal {
            root,
            batch: Some(batch),
            topology,
            reuse_retained_match_answers,
            retained_answer_dispatch,
            ancestor_requirements: AncestorRequirementsCache::default(),
            prefix_caches: Rc::clone(&self.prefix_caches),
            match_workspace,
            match_workspace_bytes,
            dispatch_workspace: DispatchCandidateWorkspace::default(),
            dispatch_workspace_bytes: 0,
            cascade_compaction_workspace: ordering::CascadeCompactionWorkspace::default(),
            cascade_compaction_workspace_bytes: 0,
        })
    }

    /// Materialize the document's facts once for a synchronous broad style traversal.
    ///
    /// The caller brackets one traversal explicitly, so engine mutations cannot leave a retained
    /// answer stale. If the batch exceeds the document's Tier-4 budget, drop it and let each
    /// element use the ordinary exact batch path.
    pub fn begin_cold_matching_batch(&mut self, root: StyleNodeID) -> bool {
        self.end_cold_matching_batch();

        let mut prepared = self.take_prepared_batch_matching_traversal(root);
        let Some(batch) = prepared
            .as_mut()
            .and_then(|prepared| prepared.batch.take())
            .or_else(|| {
                self.materialize_cold_matching_batch(
                    root,
                    prepared.as_ref().and_then(|prepared| prepared.topology.as_ref()),
                )
            })
        else {
            if let Some(prepared) = prepared {
                self.memory
                    .release(MemoryCategory::BatchScratch, prepared.capacity_bytes());
            }
            return false;
        };
        let (topology, reuse_retained_match_answers, match_workspace) = prepared.map_or_else(
            || (None, false, MatchEvaluationWorkspace::default()),
            |prepared| {
                (
                    prepared.topology,
                    prepared.reuse_retained_match_answers,
                    prepared.match_workspace,
                )
            },
        );
        self.batch_matching_traversal = Some(self.traversal_with_cold_matching_batch(
            root,
            batch,
            topology,
            reuse_retained_match_answers,
            match_workspace,
        ));
        true
    }

    /// Begin a synchronous selective traversal without paying for broad facts up front.
    ///
    /// Local asks remain local instead of paying for broad facts up front.
    pub fn begin_adaptive_cold_matching_batch(&mut self, root: StyleNodeID) {
        self.end_cold_matching_batch();
        let mut prepared = self.take_prepared_batch_matching_traversal(root);
        if let Some(batch) = prepared.as_mut().and_then(|prepared| prepared.batch.take()) {
            let prepared = prepared.unwrap();
            self.batch_matching_traversal = Some(self.traversal_with_cold_matching_batch(
                root,
                batch,
                prepared.topology,
                prepared.reuse_retained_match_answers,
                prepared.match_workspace,
            ));
            return;
        }
        let (topology, reuse_retained_match_answers, match_workspace) = prepared.map_or_else(
            || (None, false, MatchEvaluationWorkspace::default()),
            |prepared| {
                (
                    prepared.topology,
                    prepared.reuse_retained_match_answers,
                    prepared.match_workspace,
                )
            },
        );
        let match_workspace_bytes = match_workspace.capacity_bytes();
        {
            let mut caches = self.prefix_caches.borrow_mut();
            caches.states.make_scratch(&mut self.memory);
            caches.answers.make_scratch(&mut self.memory);
        }
        let retained_answer_dispatch = self.retained_answer_dispatch_for_traversal(reuse_retained_match_answers);
        self.batch_matching_traversal = Some(Box::new(BatchMatchingTraversal {
            root,
            batch: None,
            topology,
            reuse_retained_match_answers,
            retained_answer_dispatch,
            ancestor_requirements: AncestorRequirementsCache::default(),
            prefix_caches: Rc::clone(&self.prefix_caches),
            match_workspace,
            match_workspace_bytes,
            dispatch_workspace: DispatchCandidateWorkspace::default(),
            dispatch_workspace_bytes: 0,
            cascade_compaction_workspace: ordering::CascadeCompactionWorkspace::default(),
            cascade_compaction_workspace_bytes: 0,
        }));
    }

    pub fn end_cold_matching_batch(&mut self) {
        if let Some(mut traversal) = self.batch_matching_traversal.take() {
            if let Some(batch) = &traversal.batch
                && self.tree.tree_scope(traversal.root) == TreeScopeID::DOCUMENT
                && self.tree.parent(traversal.root).is_none()
            {
                let (scope_program, dispatch) = self.ranked_scope_program(TreeScopeID::DOCUMENT);
                if !dispatch.prefixes().is_empty() {
                    let evaluator = MatchEvaluator::new(&self.tree, batch);
                    let evaluation = PrefixEvaluation::new(
                        dispatch.prefixes(),
                        &self.tree,
                        batch,
                        &self.programs,
                        &evaluator,
                        None,
                        None,
                    );
                    let prefix_caches = Rc::clone(&self.prefix_caches);
                    let mut caches = prefix_caches.borrow_mut();
                    caches.states.make_scratch(&mut self.memory);
                    let states = caches
                        .states
                        .get_or_insert(scope_program, batch.generation(), batch.row_count());
                    // The batch just matched every node it iterates here, so completing the
                    // transitions matching did not touch is the cheap tail of work already paid
                    // for: each one is a chain walk over memoized dependencies. A complete cache
                    // is what lets a sibling-bearing automaton's convergence maintain itself
                    // through later flushes instead of upquerying its way back to the planner:
                    // the rightward walk cannot seed missing left context the way the downward
                    // walk seeds through ancestors, so only sibling automata earn the uncapped
                    // walk, and only when the pool can plausibly retain what the walk builds.
                    // Completing a cache with no available Tier-3 headroom is work paid and thrown
                    // away after admission has already closed.
                    let headroom = self
                        .memory
                        .tier3_limit()
                        .saturating_sub(self.memory.bytes_in_tier(memory::Tier::Acceleration));
                    #[cfg(test)]
                    let force_bounded = self.force_bounded_prefix_completion;
                    #[cfg(not(test))]
                    let force_bounded = false;
                    let completion_budget = match !force_bounded
                        && dispatch.prefixes().has_sibling_steps()
                        && headroom >= states.capacity_bytes()
                    {
                        true => usize::MAX,
                        false => PREFIX_TRANSITION_CACHE_COMPLETION_BUDGET,
                    };
                    if let Some(topology) = &traversal.topology {
                        states.complete_nodes_with_budget(
                            &evaluation,
                            topology
                                .nodes()
                                .iter()
                                .copied()
                                .filter(|&node| self.tree.tree_scope(node) == TreeScopeID::DOCUMENT),
                            completion_budget,
                            &mut self.counters,
                        );
                    } else {
                        states.complete_nodes_with_budget(
                            &evaluation,
                            self.tree
                                .preorder(traversal.root)
                                .filter(|&node| self.tree.tree_scope(node) == TreeScopeID::DOCUMENT),
                            completion_budget,
                            &mut self.counters,
                        );
                    }
                    let _ = states;
                    caches.states.settle_memory(&mut self.memory);
                }
            }
            self.retain_prefix_states();
            let mut caches = self.prefix_caches.borrow_mut();
            let retained_prefix_states = caches.states.is_retained();
            if retained_prefix_states {
                caches.states.mark_full();
            }
            if !retained_prefix_states || !caches.answers.retain(&mut self.memory) {
                caches.answers.release(&mut self.match_answers);
            }
            drop(caches);
            let released_cascade_payload_bytes = self.match_answers.sweep_unreferenced();
            self.retained_match_answers
                .release_swept_cascade_payloads(released_cascade_payload_bytes);
            self.match_answers.compact_if_needed();
            if let Some(batch) = traversal.batch {
                self.memory
                    .release(MemoryCategory::BatchScratch, batch.capacity_bytes());
            }
            if let Some(topology) = traversal.topology {
                self.memory
                    .release(MemoryCategory::BatchScratch, topology.capacity_bytes());
            }
            traversal.ancestor_requirements.release(&mut self.memory);
            self.memory
                .release(MemoryCategory::BatchScratch, traversal.match_workspace_bytes);
            self.memory
                .release(MemoryCategory::BatchScratch, traversal.dispatch_workspace_bytes);
            self.memory.release(
                MemoryCategory::BatchScratch,
                traversal.cascade_compaction_workspace_bytes,
            );
            self.discard_published_match_answers();
        }
    }

    /// Every element the document holds, shadow trees included, in tree order per tree.
    ///
    /// A host's shadow tree is not below it in the style tree, so a preorder from the document
    /// element does not reach it. Each tree is walked from its own root. A shadow root is a parent
    /// in the style tree without being an element, so it is not one of the nodes: it holds no facts
    /// and nothing decides anything for it.
    #[must_use]
    pub(super) fn elements_under(&self, root: StyleNodeID) -> Vec<StyleNodeID> {
        let mut nodes: Vec<StyleNodeID> = Vec::new();
        let mut roots = vec![root];
        while let Some(next) = roots.pop() {
            for node in self.tree.preorder(next) {
                if self.tree.host_of(node).is_none() {
                    nodes.push(node);
                }
                if let Some(shadow_root) = self.tree.shadow_root_of(node) {
                    roots.push(shadow_root);
                }
            }
        }
        nodes
    }

    /// Every element under `root` that is not inside one of the named subtrees.
    ///
    /// Program changes arriving with a new subtree need to inspect only the old resident tree.
    /// Walking every new descendant and then asking its ancestors whether it belongs to that
    /// subtree makes initial style proportional to the content already covered by the plan.
    #[must_use]
    pub(super) fn elements_under_excluding_subtrees(
        &self,
        root: StyleNodeID,
        excluded_roots: &[StyleNodeID],
    ) -> Vec<StyleNodeID> {
        let mut nodes = Vec::new();
        let mut pending = vec![root];
        while let Some(node) = pending.pop() {
            if excluded_roots.binary_search(&node).is_ok() {
                continue;
            }
            if self.tree.host_of(node).is_none() {
                nodes.push(node);
            }
            pending.extend(self.tree.children(node));
            if let Some(shadow_root) = self.tree.shadow_root_of(node) {
                pending.push(shadow_root);
            }
        }
        nodes
    }

    /// Every scope a `::part()` rule can address this element from, nearest host first.
    ///
    /// One per level of `exportparts` forwarding: the tree each forwarding host stands in holds
    /// rules that can name the element under the name that host exposed it by. Duplicates are
    /// dropped because two levels can stand in one tree.
    pub(super) fn part_exposure_scopes(&self, node: StyleNodeID) -> impl Iterator<Item = TreeScopeID> + '_ {
        let mut seen: Vec<TreeScopeID> = Vec::new();
        self.tree
            .part_hosts_of(node)
            .iter()
            .map(|&(_, host)| self.tree.tree_scope(host))
            .filter(move |scope| match seen.contains(scope) {
                true => false,
                false => {
                    seen.push(*scope);
                    true
                }
            })
    }

    /// Build one scope's selector dispatch and rank its static cascade priorities once.
    pub(super) fn build_ranked_scope_dispatch(&mut self, scope: TreeScopeID) -> Rc<RuleDispatch> {
        let (shape, rules) = scope_dispatch_shape_and_rules(&self.program, &self.programs, scope);
        let document_sheet_mode = if scope == TreeScopeID::DOCUMENT {
            DocumentSheetMode::None
        } else if self.program.scope_uses_document_sheets(scope) {
            DocumentSheetMode::All
        } else {
            DocumentSheetMode::NonAuthor
        };
        let cascade_shape = ScopeCascadeShape {
            dispatch: shape.clone(),
            depth: self.tree_scope_depth(scope),
            document_sheet_mode,
            rule_origins_and_layers: rules
                .iter()
                .map(|&rule| {
                    let sheet = self.program.rule_sheet(rule);
                    (
                        self.program.sheet_origin(sheet) as u8,
                        self.program.rule_version(rule).layer,
                    )
                })
                .collect(),
            layer_order: self.program.layer_order_key(scope),
        };
        let cascade_template = self.scope_cascade_templates.get(&cascade_shape).cloned();
        let exact_template = self.scope_dispatch_templates.get(&shape).cloned();
        let extension_template = exact_template.is_none().then(|| {
            // Extending a template copies its topology first. Require the retained prefix to cover
            // at least half the resulting entries so copying cannot dominate a small cold build.
            self.scope_dispatch_templates
                .iter()
                .filter(|(candidate, _)| {
                    !candidate.0.is_empty() && candidate.0.len() < shape.0.len() && shape.0.starts_with(&candidate.0)
                })
                .filter(|(_, template)| template.entry_count() >= rules.len().div_ceil(2))
                .max_by_key(|(_, template)| template.entry_count())
                .map(|(candidate, template)| (candidate.0.len(), Rc::clone(template)))
        });
        let mut dispatch = match (exact_template, extension_template.flatten()) {
            (Some(template), _) => RuleDispatch::rebind_rules(&template, &rules),
            (None, Some((prefix_len, template))) => {
                let mut dispatch =
                    RuleDispatch::rebind_rules_for_extension(&template, &rules[..template.entry_count()]);
                let mut rule_index = template.entry_count();
                for &(selector_program, author) in &shape.0[prefix_len..] {
                    insert_scope_rule(
                        &mut dispatch,
                        &self.programs,
                        rules[rule_index],
                        selector_program,
                        author,
                    );
                    rule_index = dispatch.entry_count();
                }
                assert_eq!(rule_index, rules.len());
                dispatch.finish_prefixes();
                dispatch
            }
            (None, None) => {
                let mut dispatch = build_scope_dispatch(&self.program, &self.programs, scope);
                let ancestor_shape = dispatch.ancestor_dispatch_shape();
                if let Some(template) = self.ancestor_dispatch_templates.get(&ancestor_shape) {
                    dispatch.share_ancestor_topology_with(template);
                }
                dispatch
            }
        };
        let ancestor_shape = dispatch.ancestor_dispatch_shape();
        if let Some(template) = cascade_template {
            dispatch.reuse_cascade_order(&template);
        } else {
            dispatch.assign_cascade_order(|candidate| {
                let entry = &self.programs.get(candidate.program).entries()[candidate.entry as usize];
                self.cascade_priority_of(candidate.rule, scope, entry.specificity, u32::MAX, false)
            });
        }
        dispatch.assign_cascade_properties(
            |candidate| {
                self.program.rule_is_gated_by_container_query(candidate.rule)
                    || !self.program.declarations_are_complete_for(candidate.rule)
            },
            |candidate| {
                let entry = &self.programs.get(candidate.program).entries()[candidate.entry as usize];
                let sheet = self.program.rule_sheet(candidate.rule);
                if scope != TreeScopeID::DOCUMENT
                    || self.program.sheet_origin(sheet) != CascadeOrigin::Author
                    || self.program.rule_is_gated_by_container_query(candidate.rule)
                    || !self.program.declarations_are_complete_for(candidate.rule)
                    || entry.scope_root.is_some()
                {
                    return None;
                }
                let declared = self.program.declared_properties_of(candidate.rule);
                if declared.iter().any(|property| property.important) {
                    return None;
                }
                Some(declared.iter().map(|property| property.property).collect())
            },
        );
        dispatch.settle_memory(&mut self.memory);
        let dispatch = Rc::new(dispatch);
        self.scope_cascade_templates
            .entry(cascade_shape)
            .or_insert_with(|| Rc::clone(&dispatch));
        self.scope_dispatch_templates
            .entry(shape)
            .or_insert_with(|| Rc::clone(&dispatch));
        self.ancestor_dispatch_templates
            .entry(ancestor_shape)
            .or_insert_with(|| Rc::clone(&dispatch));
        dispatch
    }

    pub(super) fn scope_dispatch_key(&self, scope: TreeScopeID) -> ScopeDispatchKey {
        let document_sheet_mode = if scope == TreeScopeID::DOCUMENT {
            DocumentSheetMode::None
        } else if self.program.scope_uses_document_sheets(scope) {
            DocumentSheetMode::All
        } else {
            DocumentSheetMode::NonAuthor
        };
        let mut sheets = self.program.sheets_in_scope(scope);
        if scope != TreeScopeID::DOCUMENT {
            sheets.extend(
                self.program
                    .sheets_in_scope(TreeScopeID::DOCUMENT)
                    .into_iter()
                    .filter(|&sheet| {
                        document_sheet_mode == DocumentSheetMode::All
                            || self.program.sheet_origin(sheet) != CascadeOrigin::Author
                    }),
            );
        }
        ScopeDispatchKey {
            depth: self.tree_scope_depth(scope),
            document_sheet_mode,
            sheets: sheets
                .into_iter()
                .map(|sheet| (sheet, self.program.sheet_dispatch_version(sheet)))
                .collect(),
            layer_order: self.program.layer_order_key(scope),
        }
    }

    pub(super) fn scope_program(&self, id: ScopeProgramID) -> &ScopeProgram {
        self.scope_programs[id]
            .as_ref()
            .expect("a retained scope names a live program")
    }

    pub(super) fn release_scope_program(&mut self, id: ScopeProgramID) {
        let program = self.scope_programs[id]
            .as_mut()
            .expect("a retained scope names a live program");
        assert_ne!(program.scope_count, 0);
        program.scope_count -= 1;
        if program.scope_count != 0 {
            return;
        }
        let mut caches = self.prefix_caches.borrow_mut();
        caches.states.remove(id);
        caches.answers.remove_program(&mut self.match_answers, id);
        let program = self.scope_programs[id].take().unwrap();
        let hash = intern_table::content_hash(&program.key);
        self.scope_programs.remove_identity(hash, id);
        self.vacant_scope_programs.push(id);
    }

    pub(super) fn intern_scope_program(&mut self, scope: TreeScopeID, key: ScopeDispatchKey) -> ScopeProgramID {
        let hash = intern_table::content_hash(&key);
        if let Some(id) = self.scope_programs.find(hash, |_id, program| {
            program.as_ref().is_some_and(|program| program.key == key)
        }) {
            let count = &mut self.scope_programs[id].as_mut().unwrap().scope_count;
            *count = count.checked_add(1).expect("scope program reference count overflow");
            return id;
        }

        let id = self.vacant_scope_programs.pop().unwrap_or_else(|| {
            ScopeProgramID(u32::try_from(self.scope_programs.len()).expect("scope program identity space exhausted"))
        });
        let dispatch = self.build_ranked_scope_dispatch(scope);
        let program = ScopeProgram {
            key,
            dispatch,
            scope_count: 1,
        };
        self.scope_programs.insert(hash, id, Some(program));
        id
    }

    /// Resolve the immutable selector program a concrete scope evaluates against.
    pub(super) fn ranked_scope_program(&mut self, scope: TreeScopeID) -> (ScopeProgramID, Rc<RuleDispatch>) {
        let depth = self.tree_scope_depth(scope);
        if let Some((held_scope, held_depth, id)) = self.held_scope_program
            && held_scope == scope
            && held_depth == depth
        {
            return (id, Rc::clone(&self.scope_program(id).dispatch));
        }

        let scope_index = scope.0 as usize;
        if let Some(Some((held_depth, id))) = self.scope_program_by_scope.get(scope_index)
            && *held_depth == depth
        {
            self.held_scope_program = Some((scope, depth, *id));
            return (*id, Rc::clone(&self.scope_program(*id).dispatch));
        }

        if let Some((_, previous)) = self.scope_program_by_scope.get_mut(scope_index).and_then(Option::take) {
            self.release_scope_program(previous);
        }
        let key = self.scope_dispatch_key(scope);
        let id = self.intern_scope_program(scope, key);
        self.scope_program_by_scope.insert(scope_index, Some((depth, id)));
        self.held_scope_program = Some((scope, depth, id));
        (id, Rc::clone(&self.scope_program(id).dispatch))
    }

    /// Ask one scope's rules of one node against the dispatch the scope keeps.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn match_node_in_kept_scope(
        &mut self,
        node: StyleNodeID,
        scope: TreeScopeID,
        facts: &StyleNodeFacts,
        dispatch_workspace: &mut DispatchCandidateWorkspace,
        matches: &mut RuleMatches,
        shared_ancestor_requirements: Option<&mut AncestorRequirementsCache>,
        shared_prefix_caches: Option<&Rc<RefCell<PrefixCaches>>>,
        match_workspace: Option<&MatchEvaluationWorkspace>,
        mut retry: BatchMatchRetry<'_>,
    ) -> Result<(), Incomplete> {
        let (scope_program, dispatch) = self.ranked_scope_program(scope);
        // The same summary the document pass builds, over the one element being asked. It answers
        // only for a node in its own scope; a host, a slotted element and a part are each asked the
        // rules of a scope that is not theirs, and their ancestry is not the one those rules mean.
        let mut local_requirements = None;
        let requirements = if self.tree.tree_scope(node) != scope {
            None
        } else if let Some(cache) = shared_ancestor_requirements {
            Some(cache.get_or_build_for_node(&self.tree, facts, &dispatch, node, &mut self.memory))
        } else {
            local_requirements = AncestorRequirements::build_for_node(&self.tree, facts, &dispatch, node);
            local_requirements.as_ref()
        };
        let scratch_bytes = match local_requirements.is_some() {
            true => AncestorRequirements::required_bytes_for_one_row(dispatch.ancestor_key_count()),
            false => 0,
        };
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, scratch_bytes);
        if let Some(completed) = retry.completed.as_deref_mut() {
            completed.resize(dispatch.entry_count(), false);
        }
        let mut local_prefix_states = None;
        let mut shared_prefix_caches = shared_prefix_caches.map(|caches| caches.borrow_mut());
        let mut shared_prefix_states = None;
        let prefix_states = if self.tree.tree_scope(node) != scope || dispatch.prefixes().is_empty() {
            None
        } else if let Some(caches) = shared_prefix_caches.as_deref_mut() {
            shared_prefix_states = Some(caches.states.get_or_insert(
                scope_program,
                facts.generation(),
                facts.row_count(),
            ));
            shared_prefix_states.as_deref_mut()
        } else {
            local_prefix_states = Some(PrefixStates::new(facts.row_count()));
            local_prefix_states.as_mut()
        };
        let result = self.match_node_in_scope(
            node,
            scope,
            &dispatch,
            facts,
            dispatch_workspace,
            requirements,
            prefix_states,
            match_workspace,
            BatchMatchAttempt {
                matches,
                requests: retry.requests,
                completed: retry.completed.map(Vec::as_mut_slice),
                deferred_prefix_matches: retry.deferred_prefix_matches,
                answer_is_exact: retry.answer_is_exact,
                cascade_only: retry.cascade_only,
            },
        );
        let _ = shared_prefix_states;
        if let Some(caches) = shared_prefix_caches.as_deref_mut() {
            caches.states.settle_memory(&mut self.memory);
        }
        if let Some(prefix_states) = local_prefix_states {
            let bytes = prefix_states.capacity_bytes();
            self.memory.reserve_required(MemoryCategory::BatchScratch, bytes);
            self.memory.release(MemoryCategory::BatchScratch, bytes);
        }
        self.memory.release(MemoryCategory::BatchScratch, scratch_bytes);
        result
    }

    /// Ask one scope's rules of one node, appending what matches.
    ///
    /// The dispatch and the ancestor summary are passed in because the two callers get them
    /// differently: a pass over the whole document builds one of each per scope and drops them,
    /// while a per-element ask reads the dispatch the scope keeps and summarizes one element.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn match_node_in_scope(
        &mut self,
        node: StyleNodeID,
        scope: TreeScopeID,
        dispatch: &RuleDispatch,
        facts: &StyleNodeFacts,
        dispatch_workspace: &mut DispatchCandidateWorkspace,
        ancestor_requirements: Option<&AncestorRequirements>,
        prefix_states: Option<&mut PrefixStates>,
        match_workspace: Option<&MatchEvaluationWorkspace>,
        attempt: BatchMatchAttempt<'_>,
    ) -> Result<(), Incomplete> {
        let mut interpreter = BatchMatcher::new(&self.tree, facts, dispatch, &self.programs, &self.program)
            .in_scope(scope)
            .observing_witnesses(&self.relational_witnesses);
        if attempt.cascade_only {
            interpreter = interpreter.for_cascade();
        }
        if self.tree.tree_scope(node) == scope
            && let Some(requirements) = ancestor_requirements
        {
            interpreter = interpreter.with_ancestor_requirements(requirements);
        }
        if let Some(shadow_root) = self.scope_root(scope) {
            interpreter = interpreter.in_shadow_tree(shadow_root);
        }
        if let Some(match_workspace) = match_workspace {
            interpreter = interpreter.with_match_workspace(match_workspace);
        }
        if self.tree.tree_scope(node) != scope {
            if self.scopes_slotted_into(node).any(|slotted| slotted == scope) {
                interpreter = interpreter.for_a_node_slotted_in();
            } else if self.part_exposure_scopes(node).any(|exposed| exposed == scope) {
                interpreter = interpreter.for_a_part_exposed_here();
            } else if self
                .tree
                .shadow_root_of(node)
                .is_some_and(|root| self.scope_root(scope) == Some(root))
            {
                interpreter = interpreter.for_the_host_of_this_tree();
            }
        }
        let cascade_rejections_before = self.counters.get(Counter::CascadeCandidatesRejectedByWinner);
        let result = interpreter.match_node_collecting_requests(
            node,
            attempt.matches,
            &mut self.counters,
            BatchMatchState {
                dispatch_workspace,
                requests: attempt.requests,
                completed: attempt.completed,
                prefix_states,
                deferred_prefix_matches: attempt.deferred_prefix_matches,
            },
        );
        if self.counters.get(Counter::CascadeCandidatesRejectedByWinner) != cascade_rejections_before
            && let Some(answer_is_exact) = attempt.answer_is_exact
        {
            *answer_is_exact = false;
        }
        self.settle_relational_witness_memory();
        result
    }

    /// Keep the retained-witness charge in step with the table. Capacity is already committed
    /// at this boundary, so pressure keeps the table usable for this period and schedules the whole
    /// category for eviction at the next boundary.
    pub(super) fn settle_relational_witness_memory(&mut self) {
        let bytes = self.relational_witnesses.borrow().capacity_bytes();
        self.relational_witness_residency
            .reconcile_committed(&mut self.memory, bytes);
        self.memory
            .finish_committed_acceleration_growth(MemoryCategory::RetainedWitness);
        if !self.memory.is_tier3_admitting(MemoryCategory::RetainedWitness) {
            self.relational_witnesses.borrow_mut().set_admitting(false);
        }
    }

    /// Return the retained witness proving that this anchor's Boolean cannot have flipped.
    ///
    /// The entry only proves that the anchor's last completed evaluation of the query answered
    /// true; that the witness still witnesses is re-established here against the live tree and
    /// the current facts. Anything unprovable - no entry, a retired or moved witness, a fact row
    /// the check cannot see, a query the program no longer answers for - routes conservatively,
    /// and a witness caught no longer witnessing is dropped so the next ask fails one lookup
    /// earlier.
    pub(super) fn retained_witness_for_anchor(
        &mut self,
        program_id: SelectorProgramID,
        query_id: RelativeQueryID,
        anchor: StyleNodeID,
    ) -> Lookup<StyleNodeID, RelationalWitnessGap> {
        let key = RelationalWitnessKey {
            program: program_id,
            query: query_id,
            anchor,
        };
        let witness = match self.relational_witnesses.borrow().lookup(key).sparse() {
            Ok(&witness) => witness,
            Err(gap) => return Lookup::Missing(gap),
        };
        let Some(query) = self.programs.get(program_id).retainable_relative_query(query_id) else {
            return Lookup::Missing(RelationalWitnessGap::MissingQuery(key));
        };
        if !self.tree.is_live(witness) {
            self.relational_witnesses.borrow_mut().clear(key);
            return Lookup::Missing(RelationalWitnessGap::RetiredWitness { key, witness });
        }
        let Some(walk_anchor) = traversal_anchor(anchor, query.match_in_shadow_tree, &self.tree) else {
            self.relational_witnesses.borrow_mut().clear(key);
            return Lookup::Missing(RelationalWitnessGap::UnreachableAnchor(key));
        };
        let still_on_the_axis = match query.axis {
            RelativeAxis::Child => self.tree.parent(witness) == Some(walk_anchor),
            RelativeAxis::NextSibling => self.tree.previous_element_sibling(witness) == Some(walk_anchor),
            RelativeAxis::Descendant => self.tree.ancestors(witness).any(|ancestor| ancestor == walk_anchor),
            // Walked backwards from the witness with a step budget, so the check stays constant
            // however long the sequence is; an anchor further back than that routes conservatively.
            RelativeAxis::FollowingSibling => {
                let mut steps = 0;
                let mut current = self.tree.previous_element_sibling(witness);
                loop {
                    match current {
                        Some(sibling) if sibling == walk_anchor => break true,
                        Some(sibling) if steps < RETAINED_WITNESS_SIBLING_STEPS => {
                            steps += 1;
                            current = self.tree.previous_element_sibling(sibling);
                        }
                        _ => break false,
                    }
                }
            }
            // A multi-compound argument is never simple, so no witness of it is ever retained.
            RelativeAxis::NextSiblingSubtree | RelativeAxis::FollowingSiblingSubtree => false,
        };
        if !still_on_the_axis {
            self.relational_witnesses.borrow_mut().clear(key);
            return Lookup::Missing(RelationalWitnessGap::StaleAxis { key, witness });
        }
        // The compound of a retainable query reads only facts the witness itself publishes, so one
        // current-side row decides it exactly. A node the store has no row for reports a miss, and
        // the miss routes conservatively rather than deciding anything.
        let resident_facts = self.facts.primary();
        let transaction_fact_view = self.transaction_fact_view.as_ref();
        let current_row = transaction_fact_view
            .and_then(|view| view.row_of(TransactionFactSide::After, resident_facts, witness))
            .or_else(|| resident_facts.row_of(witness).map(|row| (resident_facts, row)));
        if current_row.is_none() {
            return Lookup::Missing(RelationalWitnessGap::IncompleteFacts {
                key,
                witness,
                incomplete: Incomplete::MissingFacts(witness),
            });
        }
        let program = self.programs.get(program_id);
        let mut evaluator = MatchEvaluator::new(&self.tree, resident_facts);
        if let Some(view) = transaction_fact_view {
            evaluator = evaluator.with_transaction_fact_view(view, TransactionFactSide::After);
        }
        match evaluator.matches_selector_node(program, query.compound, witness, &mut self.counters) {
            Ok(true) => Lookup::Known(witness),
            Ok(false) => {
                self.relational_witnesses.borrow_mut().clear(key);
                Lookup::Missing(RelationalWitnessGap::SelectorMismatch { key, witness })
            }
            Err(incomplete) => Lookup::Missing(RelationalWitnessGap::IncompleteFacts {
                key,
                witness,
                incomplete,
            }),
        }
    }

    pub(super) fn append_descendant_fact_window(
        &self,
        covered: &mut Vec<StyleNodeID>,
        root: StyleNodeID,
        first: StyleNodeID,
        mut remaining: usize,
    ) -> bool {
        let mut reached_first = false;
        for node in self.tree.preorder(root) {
            reached_first |= node == first;
            if reached_first && !covered.contains(&node) {
                covered.push(node);
                remaining -= 1;
                if remaining == 0 {
                    break;
                }
            }
        }
        reached_first
    }

    /// Add the fact range an interrupted evaluator can still read, up to `window` rows of it.
    ///
    /// Returning a node means it was already requested but the resident store could not supply a
    /// row for it, so another retry cannot make progress.
    ///
    /// A sibling or descendant scan names the whole remaining range it can read, but how much of
    /// it the scan really reads is not known until it runs. Taking the whole range on the first
    /// miss makes a bounded query materialize rows it may never inspect, while taking one row per
    /// retry makes a query that reads the whole range quadratic. The range is therefore taken a
    /// window at a time, and the window doubles per retry: a scan that stops early pays for what it
    /// read, one that runs to the end converges in a logarithmic number of restarts, and neither
    /// asks for more than twice what it needs.
    pub(super) fn widen_fact_coverage(
        &self,
        covered: &mut Vec<StyleNodeID>,
        incomplete: Incomplete,
        window: &mut usize,
    ) -> Result<(), StyleNodeID> {
        let first = incomplete.first_missing_node();
        if covered.contains(&first) {
            return Err(first);
        }
        if self.append_fact_window(covered, incomplete, *window)? {
            *window = window.saturating_mul(2);
        }
        Ok(())
    }

    pub(super) fn append_fact_window(
        &self,
        covered: &mut Vec<StyleNodeID>,
        incomplete: Incomplete,
        window: usize,
    ) -> Result<bool, StyleNodeID> {
        match incomplete {
            Incomplete::MissingFacts(missing) => {
                if !covered.contains(&missing) {
                    covered.push(missing);
                }
                Ok(false)
            }
            Incomplete::MissingDescendantFacts { root, first } => self
                .append_descendant_fact_window(covered, root, first, window)
                .then_some(true)
                .ok_or(first),
            Incomplete::MissingSiblingFacts { first, last_exclusive } => {
                let mut remaining = window;
                let mut current = Some(first);
                while current != last_exclusive && remaining > 0 {
                    let Some(sibling) = current else {
                        break;
                    };
                    if !covered.contains(&sibling) {
                        covered.push(sibling);
                        remaining -= 1;
                    }
                    current = self.tree.next_element_sibling(sibling);
                }
                Ok(true)
            }
        }
    }

    /// Add every exact fact range one cold candidate pass requested.
    ///
    /// `previously_covered` separates a duplicate request in this batch from a row that was already
    /// materialized for the pass and still could not be supplied. The former is harmless; the
    /// latter cannot make progress on another retry.
    pub(super) fn widen_fact_coverage_for_requests(
        &self,
        covered: &mut Vec<StyleNodeID>,
        requests: &[Incomplete],
        window: &mut usize,
    ) -> Result<(), StyleNodeID> {
        let previously_covered = covered.len();
        let mut widened_a_range = false;
        for &request in requests {
            let first = request.first_missing_node();
            if covered[..previously_covered].contains(&first) {
                return Err(first);
            }
            widened_a_range |= self.append_fact_window(covered, request, *window)?;
        }
        if widened_a_range {
            *window = window.saturating_mul(2);
        }
        Ok(())
    }

    /// Every rule that decides for one element, in the order the cascade applies them.
    ///
    /// This is the question a style recompute asks. The document pass below answers it for every
    /// element at once and exists to be compared against another matcher; this one is what styling
    /// reads.
    #[must_use]
    pub fn match_element_signature(&mut self, node: StyleNodeID) -> Option<u32> {
        if !self.match_answer_is_comparable_across_elements(node) {
            return None;
        }
        self.retained_match_answers
            .cascade_input_lookup(node)
            .sparse()
            .ok()
            .map(|cascade_input| cascade_input.0)
    }

    pub(super) fn match_answer_is_comparable_across_elements(&self, node: StyleNodeID) -> bool {
        self.tree.tree_scope(node) == TreeScopeID::DOCUMENT && self.match_answer_is_retainable(node)
    }

    pub(super) fn match_answer_is_retainable(&self, node: StyleNodeID) -> bool {
        self.tree.shadow_root_of(node).is_none()
            && !self
                .scopes_slotted_into(node)
                .any(|scope| scope != TreeScopeID::DOCUMENT)
            && !self
                .part_exposure_scopes(node)
                .any(|scope| scope != TreeScopeID::DOCUMENT)
    }

    pub(super) fn publish_cascade_input(&mut self, node: StyleNodeID, cascade_input: MatchAnswerID) {
        if !self.match_answer_is_comparable_across_elements(node) {
            self.retained_match_answers
                .forget_cascade_input(&mut self.match_answers, node);
            return;
        }
        self.retained_match_answers.remember_cascade_input(
            &mut self.match_answers,
            node,
            cascade_input,
            &mut self.memory,
        );
    }

    pub(super) fn remember_retained_match_answer(&mut self, node: StyleNodeID, matches: &[RuleMatch]) {
        let answer = prepare_retained_match_answer(matches.iter().copied());
        self.remember_prepared_retained_match_answer(node, answer);
    }

    pub(super) fn remember_prepared_retained_match_answer(
        &mut self,
        node: StyleNodeID,
        answer: Vec<RetainedRuleMatch>,
    ) {
        self.remember_prepared_retained_match_answer_with_truth(node, answer, None);
    }

    pub(super) fn remember_prepared_retained_match_answer_with_truth(
        &mut self,
        node: StyleNodeID,
        answer: Vec<RetainedRuleMatch>,
        selector_truth: Option<Vec<SelectorTruth>>,
    ) {
        if !self.match_answer_is_retainable(node) {
            return;
        }
        let tree_scope = self.tree.tree_scope(node);
        if answer.iter().any(|entry| entry.tree_scope != tree_scope) {
            return;
        }
        let reused_derived_answer = verify_selector_truth_derivation_is_enabled().then(|| {
            let truth = selector_truth.unwrap_or_else(|| prepare_selector_truth_set(&answer, &self.programs));
            let truth_rows = truth.len();
            let (identity, reused_truth) = self.selector_truth_sets.intern_prepared(truth);
            self.counters.bump(if reused_truth {
                Counter::SelectorTruthSetHits
            } else {
                Counter::SelectorTruthSetMisses
            });
            if !reused_truth {
                self.counters.add(
                    Counter::SelectorTruthSetRows,
                    u64::try_from(truth_rows).expect("selector truth row count exceeds u64"),
                );
            }
            let truth = Rc::clone(self.selector_truth_sets.get(identity));
            let dispatch = self.build_ranked_scope_dispatch(tree_scope);
            let mut derived = RuleMatches::new();
            append_selector_truth_matches(
                &mut derived,
                node,
                &self.program,
                &self.programs,
                &dispatch,
                &truth,
                &mut self.counters,
            );
            let derived = prepare_retained_match_answer(derived.as_slice().iter().copied());
            let mut retained = RuleMatches::new();
            append_retained_matches(&mut retained, node, tree_scope, &self.programs, &dispatch, &answer)
                .expect("a retained answer must still exist in its ranked dispatch");
            let retained = prepare_retained_match_answer(retained.as_slice().iter().copied());
            assert_eq!(retained, derived, "selector truth derivation differs for {node:?}");
            self.selector_truth_sets
                .verify_derived_answer(identity, tree_scope, self.program.version(), &retained)
        });
        if let Some(reused) = reused_derived_answer {
            self.counters.bump(if reused {
                Counter::SelectorTruthDerivedAnswerHits
            } else {
                Counter::SelectorTruthDerivedAnswerMisses
            });
        }
        if self
            .retained_match_answers
            .remember_prepared(&mut self.match_answers, node, answer, &mut self.memory)
            .is_ok()
        {
            return;
        }
        if matches!(self.retained_match_answers.lookup(node), Lookup::Missing(_)) {
            self.counters.bump(Counter::RetainedMatchAnswerRefusals);
            if !self.completion_exactness_exhausted
                && self.counters.get(Counter::Tier3RefusalRetainedMatchAnswerBytes) == 0
            {
                self.counters.set(
                    Counter::Tier3RefusalRetainedMatchAnswerBytes,
                    self.memory.bytes_in_category(MemoryCategory::RetainedMatchAnswer),
                );
            }
            self.completion_exactness_exhausted = true;
        }
    }

    pub(super) fn retained_match_answer(&self, node: StyleNodeID) -> Lookup<&Rc<[RetainedRuleMatch]>, StyleNodeID> {
        let identity = match self.retained_match_answers.lookup(node).sparse() {
            Ok(identity) => *identity,
            Err(gap) => return Lookup::Missing(gap),
        };
        match self.match_answers.retained_answer(identity) {
            Some(answer) => Lookup::Known(answer),
            None => Lookup::Missing(node),
        }
    }

    /// Materialize one selector program's exact subject relation from the active side retained by
    /// the previous style transaction. Every connected element must have an exact answer: a sparse
    /// column cannot prove that the missing elements do not match.
    pub(super) fn retained_selector_incidence(
        &mut self,
        program: SelectorProgramID,
        document_root: StyleNodeID,
    ) -> Option<Rc<[RetainedSelectorIncidence]>> {
        if let Some(incidences) = self.retained_selector_incidences.lookup(program) {
            return Some(incidences);
        }
        let mut incidences = Vec::new();
        for node in self.tree.preorder(document_root) {
            let answer = self.retained_match_answer(node).sparse().ok()?;
            for matched in answer.iter().filter(|matched| matched.program == program) {
                incidences.push(RetainedSelectorIncidence {
                    node,
                    entry: matched.entry,
                });
            }
        }
        incidences.sort_unstable();
        incidences.dedup();
        self.retained_selector_incidences
            .remember(program, incidences, &mut self.memory)
    }

    /// Materialize several active programs' selector incidences in one retained-answer pass.
    ///
    /// A media-query change commonly flips many independently gated rules at once. Walking every
    /// retained match answer once per rule makes planning proportional to rules times elements,
    /// even though each answer already contains all of those rules together.
    pub(super) fn retain_selector_incidences(&mut self, programs: &[SelectorProgramID], document_root: StyleNodeID) {
        let mut missing: Vec<_> = programs
            .iter()
            .copied()
            .filter(|program| self.retained_selector_incidences.lookup(*program).is_none())
            .collect();
        missing.sort_unstable();
        missing.dedup();
        if missing.is_empty() {
            return;
        }
        self.counters
            .add(Counter::RetainedSelectorIncidenceBatchPrograms, missing.len() as u64);

        let mut incidences = vec![Vec::new(); missing.len()];
        let mut rows = 0;
        for node in self.tree.preorder(document_root) {
            let Ok(answer) = self.retained_match_answer(node).sparse() else {
                self.counters.bump(Counter::RetainedSelectorIncidenceBatchMissingRows);
                return;
            };
            rows += 1;
            for matched in answer.iter() {
                let Ok(index) = missing.binary_search(&matched.program) else {
                    continue;
                };
                incidences[index].push(RetainedSelectorIncidence {
                    node,
                    entry: matched.entry,
                });
            }
        }
        self.counters.add(Counter::RetainedSelectorIncidenceBatchRows, rows);
        for (program, mut incidences) in missing.into_iter().zip(incidences) {
            incidences.sort_unstable();
            incidences.dedup();
            self.retained_selector_incidences
                .remember(program, incidences, &mut self.memory);
        }
    }

    /// Materialize selector incidence from current facts when no active retained answer names it.
    pub(super) fn materialize_current_selector_incidence(
        &mut self,
        program: SelectorProgramID,
    ) -> Option<Rc<[RetainedSelectorIncidence]>> {
        if let Some(incidences) = self.retained_selector_incidences.lookup(program) {
            return Some(incidences);
        }
        let (scope_program, dispatch) = self.ranked_scope_program(TreeScopeID::DOCUMENT);
        let compiled = self.programs.get(program);
        if compiled.can_leave_its_scope() {
            return None;
        }
        let mut incidences = Vec::new();
        for (entry_index, entry) in compiled.entries().iter().enumerate() {
            let posting_key = compiled.dispatch_key(entry);
            if !posting_key.has_selector_posting() {
                return None;
            }
            let candidates: Vec<_> = match self.facts.postings().lookup(posting_key) {
                Lookup::Known(posting) => posting.candidates().collect(),
                Lookup::KnownAbsent => continue,
                Lookup::Missing(_) => return None,
            };
            let subject_required = compiled.subject_required_keys(entry_index);
            let position = compiled.subject_position(entry_index);
            for node in candidates {
                if !self.node_is_within_subject_position(node, position) {
                    continue;
                }
                let mut carries_required = true;
                for required in subject_required {
                    if !required.has_selector_posting() {
                        continue;
                    }
                    match self.facts.postings().lookup(*required) {
                        Lookup::Known(posting) => carries_required &= posting.contains(node),
                        Lookup::KnownAbsent => carries_required = false,
                        Lookup::Missing(_) => return None,
                    }
                    if !carries_required {
                        break;
                    }
                }
                if !carries_required {
                    continue;
                }
                if entry.has_prefix_chain()
                    && dispatch.prefixes().contains_entry(self.programs.entry_id(
                        program,
                        u32::try_from(entry_index).expect("selector entry identity space exhausted"),
                    ))
                {
                    let retained_prefix_match = {
                        let caches = self.prefix_caches.borrow();
                        caches.states.lookup(scope_program).sparse().ok().and_then(|states| {
                            states.retained_matches_for(node).map(|matches| {
                                matches
                                    .iter()
                                    .any(|&matched| matched == self.programs.entry_id(program, entry_index as u32))
                            })
                        })
                    };
                    if let Some(matches) = retained_prefix_match {
                        if matches {
                            incidences.push(RetainedSelectorIncidence {
                                node,
                                entry: u32::try_from(entry_index).expect("selector entry identity space exhausted"),
                            });
                        }
                        continue;
                    }
                }
                match MatchEvaluator::new(&self.tree, self.facts.primary()).matches_entry_after_dispatch(
                    compiled,
                    entry,
                    compiled.dispatch_key(entry),
                    node,
                    &mut self.counters,
                ) {
                    Ok(true) => incidences.push(RetainedSelectorIncidence {
                        node,
                        entry: u32::try_from(entry_index).expect("selector entry identity space exhausted"),
                    }),
                    Ok(false) => {}
                    Err(_) => return None,
                }
            }
        }
        incidences.sort_unstable();
        incidences.dedup();
        let mut selected = Vec::<RetainedSelectorIncidence>::new();
        for incidence in incidences {
            let entry = &compiled.entries()[incidence.entry as usize];
            let existing = selected
                .iter_mut()
                .rev()
                .take_while(|existing| existing.node == incidence.node)
                .find(|existing| compiled.entries()[existing.entry as usize].pseudo_element == entry.pseudo_element);
            if let Some(existing) = existing {
                if entry.specificity > compiled.entries()[existing.entry as usize].specificity {
                    *existing = incidence;
                }
            } else {
                selected.push(incidence);
            }
        }
        selected.sort_unstable();
        self.retained_selector_incidences
            .remember(program, selected, &mut self.memory)
    }

    /// Read one selector entry's pre-transaction truth from the retained exact answer.
    ///
    /// A selector list retains only the entry which supplied the rule's winning specificity. If a
    /// different entry of the same program is present, absence of `entry` is therefore not proof
    /// that it did not match, and exact narrowing must fall back conservatively.
    pub(super) fn retained_entry_matches(
        &self,
        node: StyleNodeID,
        program: SelectorProgramID,
        entry: u32,
    ) -> Option<bool> {
        let answer = self.retained_match_answer(node).sparse().ok()?;
        let mut matched_program = false;
        for matched in answer.iter() {
            if matched.program != program {
                continue;
            }
            if matched.entry == entry {
                return Some(true);
            }
            matched_program = true;
        }
        (!matched_program).then_some(false)
    }

    pub(super) fn append_catalog_answer(
        &mut self,
        identity: MatchAnswerID,
        node: StyleNodeID,
        tree_scope_override: Option<TreeScopeID>,
        out: &mut Vec<RuleMatch>,
    ) -> Option<()> {
        let answer = Rc::clone(self.match_answers.answer(identity)?);
        out.reserve_exact(answer.len());
        for &stored_entry in answer.iter() {
            let mut entry = stored_entry;
            if let Some(tree_scope) = tree_scope_override {
                entry.tree_scope = tree_scope;
            }
            let (_, dispatch) = self.ranked_scope_program(entry.tree_scope);
            let cascade_order = dispatch.cascade_order_for_entry(entry.rule, entry.program, entry.entry)?;
            out.push(entry.materialize(node, &self.programs, cascade_order)?);
        }
        Some(())
    }

    /// Reclaim one controller-selected acceleration category without changing semantic state.
    /// The controller selected this complete category at the quota boundary. This owner-side step
    /// keeps allocation accounting exact while releasing it.
    pub(super) fn evict_tier3_category(&mut self, category: MemoryCategory) -> bool {
        let before = self.memory.bytes_in_category(category);
        match category {
            MemoryCategory::RetainedWitness => {
                self.relational_witnesses.borrow_mut().clear_all();
                self.relational_witness_residency.release();
            }
            MemoryCategory::FeaturePosting => self.facts.postings_mut().evict_all(),
            MemoryCategory::SpecifiedValueTable => self.specified_values.evict(),
            MemoryCategory::CascadeWinnerGroup => self.winner_groups.evict(),
            MemoryCategory::RetainedSelectorIncidence => self.retained_selector_incidences.clear(),
            MemoryCategory::RetainedMatchAnswer => self.retained_match_answers.evict(&mut self.match_answers),
            MemoryCategory::PrefixTransitionCache => {
                self.prefix_caches.borrow_mut().states.release();
            }
            MemoryCategory::PrefixAnswerCache => {
                self.prefix_caches.borrow_mut().answers.release(&mut self.match_answers);
            }
            _ => unreachable!("only Tier-3 categories are eviction candidates"),
        }
        self.memory.bytes_in_category(category) < before
    }

    pub(super) fn sync_tier3_benefit_observations(&mut self) {
        let (posting_hits, posting_misses) = self.facts.postings().take_benefit_lookups();
        self.memory
            .record_benefit_lookups(MemoryCategory::FeaturePosting, posting_hits, posting_misses);
        let witness_hits = self.counters.get(Counter::RelationalAnchorsSkippedByWitness);
        let witness_misses = self
            .counters
            .get(Counter::RelationalAnchorsConsidered)
            .saturating_sub(witness_hits);
        self.memory
            .record_benefit_totals(MemoryCategory::RetainedWitness, witness_hits, witness_misses);
        self.memory.record_benefit_totals(
            MemoryCategory::FeaturePosting,
            self.counters.get(Counter::RemainingPostingReuses),
            self.counters.get(Counter::RemainingPostingBuilds),
        );
        self.memory.record_benefit_totals(
            MemoryCategory::SpecifiedValueTable,
            self.counters.get(Counter::SpecifiedValuesReused),
            0,
        );
        self.memory.record_benefit_totals(
            MemoryCategory::CascadeWinnerGroup,
            self.counters.get(Counter::CascadeCandidatesRejectedByWinner),
            self.counters.get(Counter::CascadeNodeHandlesPublished),
        );
        self.memory
            .record_benefit_totals(MemoryCategory::RetainedSelectorIncidence, 0, 0);
        self.memory.record_benefit_totals(
            MemoryCategory::RetainedMatchAnswer,
            self.counters
                .get(Counter::RetainedMatchAnswerPatches)
                .saturating_add(self.counters.get(Counter::RetainedMatchAnswerDeltaPatches)),
            self.counters.get(Counter::RetainedMatchAnswerPatchMisses),
        );
        self.memory.record_benefit_totals(
            MemoryCategory::PrefixTransitionCache,
            self.counters
                .get(Counter::PrefixTransitionCacheHits)
                .saturating_add(self.counters.get(Counter::PrefixTransitionCacheMatchHits)),
            self.counters.get(Counter::PrefixTransitionCacheMatchMisses),
        );
        self.memory.record_benefit_totals(
            MemoryCategory::PrefixAnswerCache,
            self.counters.get(Counter::PrefixAnswerCacheHits),
            self.counters.get(Counter::PrefixAnswerCacheMisses),
        );
    }

    /// The selector identities whose answers can change in a patchable transaction.
    ///
    /// A retained answer keeps only the greatest matching selector-list entry per rule and pseudo
    /// target. Re-evaluating one routed entry is therefore not enough when that entry was the
    /// winner: another entry of the same rule may still match. The patch unit is consequently one
    /// routed rule version. Program edits name the old version for removal and the current version
    /// for evaluation, while every identity no input reached remains byte-for-byte unchanged.
    pub(super) fn rules_for_retained_answer_patch(
        &self,
        transaction: &StyleTransaction,
    ) -> Option<RetainedAnswerPatchSelection> {
        if !transaction.markers.is_empty() {
            return None;
        }

        let mut affected = Vec::new();
        let mut always_emit = false;
        let mut orders_shifted = false;
        let mut requires_full_match = false;
        let mut tree_mutation_rules_selected = false;
        let mut affected_routing_keys = HashSet::default();
        let mut affected_current_rules = HashSet::default();
        let mut cascade_update_properties = Vec::new();
        let mut cascade_update_rules = Vec::new();
        for input in &transaction.inputs {
            // Retained cascade inputs are document-scope identities. A local fact inside a shadow
            // tree may route to both that tree and an outer host, but its published answers cannot
            // be completed from the document cascade order used by a retained patch.
            if input
                .key
                .style_node()
                .is_some_and(|node| self.tree.tree_scope(node) != TreeScopeID::DOCUMENT)
            {
                return None;
            }
            // Assigning a slottable moves it between the cascade programs of shadow scopes even
            // though the slottable itself remains in the document scope. A document-scope patch
            // cannot name those rules, so treating this as an ordinary tree mutation can preserve
            // a stale retained answer and suppress the reaction entirely.
            if let (
                InputKey::TreeRelations(_),
                InputValue::TreeRelations(Some(old)),
                InputValue::TreeRelations(Some(new)),
            ) = (input.key, input.old, input.new)
                && old.assigned_slot != new.assigned_slot
            {
                return None;
            }
            let join_deltas = transaction.program_joins_for(input.key);
            if !join_deltas.is_empty() {
                if matches!(
                    input.key,
                    InputKey::SheetAttachment(_, tree_scope)
                        | InputKey::CascadeTopology(
                            TopologyAxis::LayerOrder(tree_scope) | TopologyAxis::SheetOrder(tree_scope)
                        ) if tree_scope != TreeScopeID::DOCUMENT
                ) {
                    return None;
                }
                for delta in join_deltas {
                    match delta.kind {
                        ProgramJoinDeltaKind::Declarations => always_emit = true,
                        ProgramJoinDeltaKind::Priority => {
                            orders_shifted = true;
                            cascade_update_rules.push(delta.rule);
                            cascade_update_properties.extend(
                                self.program
                                    .declared_properties_of(delta.rule)
                                    .iter()
                                    .map(|declared| declared.property),
                            );
                        }
                        ProgramJoinDeltaKind::ActiveRuleMatch => {
                            requires_full_match = true;
                            let reevaluate_current = matches!(
                                input.key,
                                InputKey::RuleField(_, RuleField::Activation) | InputKey::SheetActivation(_)
                            );
                            if reevaluate_current && let Some(program) = delta.after_program {
                                affected.push(RetainedAnswerPatchSelectionRule {
                                    rule: delta.rule,
                                    program,
                                    evaluate: true,
                                });
                            }
                            let before_program = delta.before_contributes.then_some(delta.before_program).flatten();
                            let after_program = delta.after_contributes.then_some(delta.after_program).flatten();
                            if !reevaluate_current {
                                if before_program == after_program {
                                    if let Some(program) = after_program {
                                        affected.push(RetainedAnswerPatchSelectionRule {
                                            rule: delta.rule,
                                            program,
                                            evaluate: true,
                                        });
                                    }
                                } else {
                                    if let Some(program) = before_program {
                                        affected.push(RetainedAnswerPatchSelectionRule {
                                            rule: delta.rule,
                                            program,
                                            evaluate: false,
                                        });
                                    }
                                    if let Some(program) = after_program {
                                        affected.push(RetainedAnswerPatchSelectionRule {
                                            rule: delta.rule,
                                            program,
                                            evaluate: true,
                                        });
                                    }
                                }
                            }
                            // Named rules and custom functions have consumers outside selector
                            // answers. A program-less selector replacement does not: absence on
                            // that field means there is no selector contribution on that side.
                            if !matches!(input.key, InputKey::RuleField(_, RuleField::Selector))
                                && (delta.before_contributes || delta.after_contributes)
                                && delta.before_program.is_none()
                                && delta.after_program.is_none()
                            {
                                always_emit = true;
                            }
                        }
                    }
                }
                continue;
            }
            let keys = match input.key {
                InputKey::LocalFeature(_, LocalFeatureKey::PartExposure) => return None,
                InputKey::TreeRelations(_) | InputKey::LocalFeature(_, LocalFeatureKey::ArrivingFacts) => {
                    // A subtree arriving, leaving, or moving cannot change a resident answer
                    // through a descendant or child compound: a non-subject compound matching
                    // inside the moved subtree puts the subject inside it too, and the plan
                    // forgets the moved subtree's own answers so those nodes match cold. What
                    // reaches residents is the sibling axis, relational queries, and positional,
                    // emptiness, or heading truth, so those rule families are the affected set.
                    if !tree_mutation_rules_selected {
                        tree_mutation_rules_selected = true;
                        let tree_mutation_routes = self
                            .routing
                            .sibling_first_routes()
                            .iter()
                            .chain(self.routing.relational_routes())
                            .chain(self.routing.routes_for(RoutingKey::Structural));
                        for &route in tree_mutation_routes {
                            let rule = self.routing.rule_of(route);
                            let point = self.routing.route(route);
                            let (program, _) = self.programs.entry_location(point.entry);
                            if self.program.rule_can_decide(rule)
                                && self.program.rule_version(rule).selector_program == Some(program)
                            {
                                affected_current_rules.insert((rule, program));
                            }
                        }
                    }
                    continue;
                }
                InputKey::LocalFeature(_, LocalFeatureKey::Attribute(name)) => {
                    let mut keys = routing_keys_for_input(input);
                    for other in self.facts.attribute_name_keys(name) {
                        if other != name {
                            keys.push(RoutingKey::AttributeName(other));
                        }
                    }
                    keys
                }
                InputKey::LocalFeature(..) | InputKey::State(..) => routing_keys_for_input(input),
                InputKey::ElementDeclaration(..) => {
                    always_emit = true;
                    continue;
                }
                InputKey::ElementStyleInput(..) => {
                    always_emit = true;
                    continue;
                }
                InputKey::CustomPropertyRegistration(..) => {
                    always_emit = true;
                    continue;
                }
                _ => return None,
            };
            affected_routing_keys.extend(keys);
        }
        for key in affected_routing_keys {
            for &route in self.routing.routes_for(key) {
                let rule = self.routing.rule_of(route);
                let point = self.routing.route(route);
                let (program, _) = self.programs.entry_location(point.entry);
                if self.program.rule_can_decide(rule)
                    && self.program.rule_version(rule).selector_program == Some(program)
                {
                    affected_current_rules.insert((rule, program));
                }
            }
        }
        affected.extend(
            affected_current_rules
                .into_iter()
                .map(|(rule, program)| RetainedAnswerPatchSelectionRule {
                    rule,
                    program,
                    evaluate: true,
                }),
        );
        let removed_programs: Vec<SelectorProgramID> = affected
            .iter()
            .filter_map(|affected| (!affected.evaluate).then_some(affected.program))
            .collect();
        for program in removed_programs {
            affected.extend(
                self.program
                    .sheets_in_scope(TreeScopeID::DOCUMENT)
                    .into_iter()
                    .flat_map(|sheet| self.program.rules_in_sheet(sheet))
                    .filter_map(|current_rule| {
                        (self.program.rule_version(current_rule).selector_program == Some(program)).then_some(
                            RetainedAnswerPatchSelectionRule {
                                rule: current_rule,
                                program,
                                evaluate: true,
                            },
                        )
                    }),
            );
        }
        affected.sort_unstable_by_key(|affected| (affected.rule, affected.program));
        let mut merged: Vec<RetainedAnswerPatchSelectionRule> = Vec::with_capacity(affected.len());
        for affected in affected {
            if let Some(previous) = merged.last_mut()
                && (previous.rule, previous.program) == (affected.rule, affected.program)
            {
                previous.evaluate |= affected.evaluate;
            } else {
                merged.push(affected);
            }
        }
        cascade_update_properties.extend(
            transaction
                .rule_declaration_changes
                .iter()
                .flat_map(|change| change.old_properties.iter().chain(&change.new_properties).copied()),
        );
        cascade_update_rules.extend(transaction.rule_declaration_changes.iter().map(|change| change.rule));
        Some(RetainedAnswerPatchSelection {
            affected: merged,
            always_emit,
            orders_shifted,
            requires_full_match,
            cascade_update_properties,
            cascade_update_rules,
            program_base_version: transaction.program_base_version,
        })
    }

    pub(super) fn prepare_retained_answer_patch(
        &mut self,
        selection: RetainedAnswerPatchSelection,
    ) -> RetainedAnswerPatch {
        let (scope_program, dispatch) = self.ranked_scope_program(TreeScopeID::DOCUMENT);
        let rules: Vec<RetainedAnswerPatchRule> = selection
            .affected
            .into_iter()
            .map(|affected| RetainedAnswerPatchRule {
                rule: affected.rule,
                program: affected.program,
            })
            .collect();
        let dispatch_workspace = DispatchCandidateWorkspace::with_entry_capacity(dispatch.entry_count());
        let rule_keys: Vec<(RuleID, SelectorProgramID)> =
            rules.iter().map(|affected| (affected.rule, affected.program)).collect();
        {
            let mut caches = self.prefix_caches.borrow_mut();
            caches.states.make_scratch(&mut self.memory);
            caches.states.prepare_to_mutate(&mut self.memory);
        }
        let mut cascade_update_properties = selection.cascade_update_properties;
        cascade_update_properties.sort_unstable();
        cascade_update_properties.dedup();
        let mut cascade_update_rules = selection.cascade_update_rules;
        cascade_update_rules.sort_unstable();
        cascade_update_rules.dedup();
        RetainedAnswerPatch {
            rules,
            rule_keys,
            scope_program,
            dispatch,
            match_workspace: MatchEvaluationWorkspace::default(),
            prefix_caches: Rc::clone(&self.prefix_caches),
            dispatch_workspace,
            always_emit: selection.always_emit,
            orders_shifted: selection.orders_shifted,
            requires_full_match: selection.requires_full_match,
            cascade_update_properties,
            cascade_update_rules,
            cascade_candidates: Vec::new(),
            cascade_compaction_workspace: ordering::CascadeCompactionWorkspace::default(),
            program_base_version: selection.program_base_version,
            delta_memo: HashMap::default(),
        }
    }

    /// Repair declaration or priority edits from retained selector truth and the previous compact
    /// answer.
    ///
    /// Element winners are updated only for properties declared by the edited or reprioritized
    /// rules. Pseudo-element winners are not retained yet, so an affected rule that matched a
    /// pseudo target uses the exact fallback. Every other pseudo entry is unchanged and can be
    /// copied from the previous compact answer.
    pub(super) fn apply_retained_cascade_updates(
        &mut self,
        node: StyleNodeID,
        patch: &mut RetainedAnswerPatch,
        retained: &[RetainedRuleMatch],
        old_cascade_input: MatchAnswerID,
    ) -> Option<RetainedAnswerPatchOutcome> {
        if patch.cascade_update_properties.is_empty() || patch.requires_full_match {
            return None;
        }
        let base_version = patch.program_base_version?;
        let (_, previous) = self
            .winner_groups
            .token_for(WinnerGroupKey::retained(node, base_version))
            .sparse()
            .ok()?;
        let mut exact_answer: Vec<RuleMatch> = retained
            .iter()
            .copied()
            .map(|entry| {
                let cascade_order = patch
                    .dispatch
                    .cascade_order_for_entry(entry.rule, entry.program, entry.entry)?;
                entry.materialize(node, &self.programs, cascade_order)
            })
            .collect::<Option<_>>()?;
        exact_answer = self.in_cascade_order(exact_answer, false);
        let cascade_winners_are_complete = self.cascade_winner_inventory_is_complete(&exact_answer, Some(node));
        if exact_answer.iter().any(|entry| {
            entry.pseudo_element.is_some() && patch.cascade_update_rules.binary_search(&entry.rule).is_ok()
        }) {
            return None;
        }
        let old_compact = Rc::clone(self.match_answers.answer(old_cascade_input)?);
        let updates = self.exact_cascade_winner_updates_for_properties_with_scratch(
            node,
            &exact_answer,
            None,
            &patch.cascade_update_properties,
            &mut patch.cascade_candidates,
        )?;
        let (state, delta) =
            self.with_cascade_interning_counters(|groups| groups.apply_property_updates(previous, &updates));
        let published = self.winner_groups.set(node, state, self.program.version());
        self.winner_groups.settle_memory(&mut self.memory);
        if published {
            self.counters.bump(Counter::CascadeNodeHandlesPublished);
        }

        let mut winning_rules: Vec<RuleID> = self
            .winner_groups
            .winners_in_state(state)
            .filter_map(|winner| match winner.source {
                WinnerSource::Rule(rule) => Some(rule),
                WinnerSource::Element(_) | WinnerSource::ExactCascade => None,
            })
            .collect();
        winning_rules.sort_unstable();
        winning_rules.dedup();
        self.counters
            .add(Counter::CascadeMatchesBeforeCompaction, exact_answer.len() as u64);
        exact_answer.retain(|entry| {
            if entry.pseudo_element.is_some() {
                return old_compact
                    .iter()
                    .any(|old| (old.rule, old.program, old.entry) == (entry.rule, entry.program, entry.entry));
            }
            self.program.sheet_origin(self.program.rule_sheet(entry.rule)) != CascadeOrigin::Author
                || winning_rules.binary_search(&entry.rule).is_ok()
        });
        let new_cascade_input = self.intern_cascade_input(&exact_answer);
        if new_cascade_input != old_cascade_input {
            self.counters.bump(Counter::MatchAnswerChanges);
        }
        self.publish_cascade_input(node, new_cascade_input);
        let emit = !delta.is_empty();
        if !emit {
            self.counters.bump(Counter::RetainedMatchAnswerPatchStops);
        }
        Some(RetainedAnswerPatchOutcome {
            identity_preserved: !emit,
            emit,
            incremental_cascade_answer: Some(IncrementalCascadeAnswer {
                node,
                cascade_input: new_cascade_input,
                matches: Some(exact_answer.into_boxed_slice()),
                cascade_winners_are_complete,
            }),
        })
    }

    /// Whether signed element match changes cannot alter the compact cascade answer.
    ///
    /// Keep the exact retained answer current even when every changed rule loses to an
    /// unaffected retained winner, but avoid materializing and reducing that whole answer again.
    pub(super) fn retained_match_deltas_cannot_change_cascade(
        &self,
        node: StyleNodeID,
        retained: &[RetainedRuleMatch],
        deltas: &[SelectorTruthDelta],
    ) -> bool {
        let Some((_, previous)) = self
            .winner_groups
            .token_for(WinnerGroupKey::current(node, self.program.version()))
            .sparse()
            .ok()
        else {
            return false;
        };
        if ElementDeclarationKind::ALL
            .iter()
            .any(|&kind| !self.facts.element_declared_properties(node, kind).1)
        {
            return false;
        }
        for retained_entry in retained {
            let Some(entry) = self
                .programs
                .get(retained_entry.program)
                .entries()
                .get(retained_entry.entry as usize)
            else {
                return false;
            };
            if entry.pseudo_element.is_none()
                && (retained_entry.tree_scope != TreeScopeID::DOCUMENT
                    || self.program.rule_is_gated_by_container_query(retained_entry.rule)
                    || !self.program.declarations_are_complete_for(retained_entry.rule))
            {
                return false;
            }
        }
        for delta in deltas {
            let entry = self.programs.entry(delta.entry).1;
            if !self.rule_has_complete_element_winners(delta.rule, entry) {
                return false;
            }
            for declared in self.program.declared_properties_of(delta.rule) {
                if matches!(
                    declared.operator,
                    CascadeOperator::Revert | CascadeOperator::RevertLayer
                ) {
                    return false;
                }
                let Some(winner) = self.winner_groups.winner_in_state(previous, declared.property) else {
                    return false;
                };
                if self.winner_groups.continuation(winner.key.continuation).is_some() {
                    return false;
                }
                match delta.change {
                    SetChange::Removed if winner.source == WinnerSource::Rule(delta.rule) => return false,
                    SetChange::Added => {
                        if winner.source == WinnerSource::ExactCascade {
                            return false;
                        }
                        let priority = self.cascade_priority_of(
                            delta.rule,
                            TreeScopeID::DOCUMENT,
                            entry.specificity,
                            u32::MAX,
                            declared.important,
                        );
                        if priority > winner.priority {
                            return false;
                        }
                    }
                    SetChange::Removed => {}
                }
            }
        }
        true
    }

    /// Apply signed entry truth to the compact retained relation without publishing it.
    ///
    /// Mixed patches use this as their exact base, then ask the matcher only about the remaining
    /// refreshes and attributed rules. The same selector-list guards as the direct path keep an
    /// entry delta from hiding a different winning entry of the rule.
    fn retained_answer_after_deltas(
        &self,
        node: StyleNodeID,
        patch: &RetainedAnswerPatch,
        retained: &[RetainedRuleMatch],
        deltas: &[SelectorTruthDelta],
    ) -> Option<(Vec<RetainedRuleMatch>, u64)> {
        if !patch.requires_full_match {
            for delta in deltas {
                let (program, selector_entry) = self.programs.entry_location(delta.entry);
                let entries = self.programs.get(program).entries();
                let changed_entry = entries.get(selector_entry as usize)?;
                let retained_winner = retained.iter().find(|retained_entry| {
                    retained_entry.rule == delta.rule
                        && self
                            .programs
                            .get(retained_entry.program)
                            .entries()
                            .get(retained_entry.entry as usize)
                            .is_some_and(|entry| entry.pseudo_element == changed_entry.pseudo_element)
                });
                match delta.change {
                    SetChange::Added
                        if retained_winner.is_some_and(|winner| {
                            self.programs.entry_id(winner.program, winner.entry) != delta.entry
                        }) =>
                    {
                        return None;
                    }
                    SetChange::Removed
                        if retained_winner.is_some_and(|winner| {
                            self.programs.entry_id(winner.program, winner.entry) == delta.entry
                        }) && entries.iter().enumerate().any(|(index, entry)| {
                            index != selector_entry as usize && entry.pseudo_element == changed_entry.pseudo_element
                        }) =>
                    {
                        return None;
                    }
                    _ => {}
                }
            }
        }

        let retained_key = |entry: &RetainedRuleMatch| (entry.rule, self.programs.entry_id(entry.program, entry.entry));
        debug_assert!(retained.is_sorted_by_key(retained_key));
        let mut answer = Vec::with_capacity(retained.len().saturating_add(deltas.len()));
        let mut retained_index = 0;
        let mut delta_index = 0;
        let mut applied = 0_u64;
        while delta_index < deltas.len() {
            let delta = deltas[delta_index];
            debug_assert_eq!(delta.node, node);
            let key = (delta.rule, delta.entry);
            let mut weight = 0_i32;
            while delta_index < deltas.len() && (deltas[delta_index].rule, deltas[delta_index].entry) == key {
                weight += match deltas[delta_index].change {
                    SetChange::Added => 1,
                    SetChange::Removed => -1,
                };
                delta_index += 1;
            }
            if weight == 0 {
                continue;
            }
            while retained_index < retained.len() && retained_key(&retained[retained_index]) < key {
                answer.push(retained[retained_index]);
                retained_index += 1;
            }
            let held = retained.get(retained_index).filter(|entry| retained_key(entry) == key);
            if weight < 0 {
                held?;
                retained_index += 1;
            } else {
                if held.is_some() {
                    return None;
                }
                let (program, selector_entry) = self.programs.entry_location(delta.entry);
                let entry = self.programs.get(program).entries().get(selector_entry as usize)?;
                if entry.scope_root.is_some() {
                    return None;
                }
                answer.push(RetainedRuleMatch {
                    rule: delta.rule,
                    program,
                    entry: selector_entry,
                    tree_scope: TreeScopeID::DOCUMENT,
                    scope_proximity: u32::MAX,
                });
            }
            applied += 1;
        }
        answer.extend_from_slice(&retained[retained_index..]);
        Some((answer, applied))
    }

    /// Apply a complete signed match delta directly to one retained exact answer.
    ///
    /// Entries with dynamic scope proximity still use the cold patch: their match row contains a
    /// value that the prefix match-set delta does not carry yet.
    pub(super) fn apply_retained_match_answer_deltas(
        &mut self,
        node: StyleNodeID,
        patch: &mut RetainedAnswerPatch,
        old_identity: MatchAnswerID,
        retained: &[RetainedRuleMatch],
        old_cascade_input: MatchAnswerID,
        deltas: &[SelectorTruthDelta],
    ) -> Option<RetainedAnswerPatchOutcome> {
        if !patch.cascade_update_properties.is_empty() {
            if !deltas.is_empty() {
                return None;
            }
            let outcome = self.apply_retained_cascade_updates(node, patch, retained, old_cascade_input)?;
            self.counters.bump(Counter::RetainedMatchAnswerDeltaPatches);
            return Some(outcome);
        }
        // Empty signed truth preserves the exact answer. Unless cascade order itself moved, its
        // compact identity is also unchanged, so stop without rebuilding either representation.
        if deltas.is_empty() && !patch.orders_shifted {
            self.publish_cascade_input(node, old_cascade_input);
            self.counters.bump(Counter::RetainedMatchAnswerDeltaPatches);
            let emit = patch.always_emit;
            if !emit {
                self.counters.bump(Counter::RetainedMatchAnswerPatchStops);
            }
            return Some(RetainedAnswerPatchOutcome {
                identity_preserved: true,
                emit,
                incremental_cascade_answer: None,
            });
        }

        // Nodes sharing the same retained identity, compact identity, and consolidated delta
        // content share the entire transition: the answer content, the guards below, the compact
        // reduction, and the stop verdict are all node-independent for comparable document-scope
        // answers under one patch's fixed cascade orders. A stopping transition therefore runs
        // once per cohort; every further member is one column store.
        let memo_key = (!patch.always_emit && !patch.orders_shifted && !self.node_has_element_declaration_input(node))
            .then(|| Self::retained_answer_delta_memo_key(old_identity, old_cascade_input, deltas));
        if let Some(key) = &memo_key
            && let Some(entry) = patch.delta_memo.get(key)
            && Self::retained_answer_delta_memo_entry_matches(entry, deltas)
            && let transition = entry.transition.clone()
            && self
                .retained_match_answers
                .set_interned_identity(node, &mut self.match_answers, transition.new_answer)
        {
            self.counters.bump(Counter::RetainedMatchAnswerDeltaMemoHits);
            let stopped = transition.new_cascade_input == old_cascade_input;
            if let Some((state, version)) = transition.winner_state {
                let _ = self.winner_groups.set(node, state, version);
            }
            // Pseudo-element rows are independent of the element's sparse winner row.
            for &(pseudo, state) in transition.pseudo_winner_states.iter() {
                let _ = self
                    .winner_groups
                    .set_pseudo(node, pseudo, state, self.program.version());
            }
            self.publish_cascade_input(node, transition.new_cascade_input);
            self.counters.bump(Counter::RetainedMatchAnswerDeltaPatches);
            if stopped {
                self.counters.bump(Counter::RetainedMatchAnswerPatchStops);
                return Some(RetainedAnswerPatchOutcome {
                    identity_preserved: true,
                    emit: false,
                    incremental_cascade_answer: None,
                });
            }
            self.counters.bump(Counter::MatchAnswerChanges);
            let incremental_cascade_answer = transition.winners_updated.then_some(IncrementalCascadeAnswer {
                node,
                cascade_input: transition.new_cascade_input,
                matches: None,
                cascade_winners_are_complete: transition.cascade_winners_are_complete,
            });
            return Some(RetainedAnswerPatchOutcome {
                identity_preserved: false,
                emit: true,
                incremental_cascade_answer,
            });
        }

        let (answer, applied) = self.retained_answer_after_deltas(node, patch, retained, deltas)?;

        if !patch.orders_shifted && self.retained_match_deltas_cannot_change_cascade(node, retained, deltas) {
            self.remember_prepared_retained_match_answer(node, answer);
            self.publish_cascade_input(node, old_cascade_input);
            self.counters.bump(Counter::RetainedMatchAnswerDeltaPatches);
            self.counters.add(Counter::RetainedMatchAnswerDeltaEntries, applied);
            if !patch.always_emit {
                self.counters.bump(Counter::RetainedMatchAnswerPatchStops);
            }
            if let Some(key) = memo_key
                && let Lookup::Known(new_identity) = self.retained_match_answers.lookup(node)
            {
                let winner_state = match self
                    .winner_groups
                    .lookup(WinnerGroupKey::current(node, self.program.version()))
                {
                    Lookup::Known(&(state, version)) => Some((state, version)),
                    Lookup::KnownAbsent | Lookup::Missing(_) => None,
                };
                let pseudo_winner_states = self.settled_pseudo_winner_states(node);
                Self::remember_retained_answer_delta_transition(
                    patch,
                    key,
                    deltas,
                    RetainedAnswerDeltaTransition {
                        new_answer: *new_identity,
                        new_cascade_input: old_cascade_input,
                        winner_state,
                        pseudo_winner_states,
                        winners_updated: false,
                        cascade_winners_are_complete: false,
                    },
                );
            }
            return Some(RetainedAnswerPatchOutcome {
                identity_preserved: true,
                emit: patch.always_emit,
                incremental_cascade_answer: None,
            });
        }

        let materialized: Vec<RuleMatch> = answer
            .iter()
            .copied()
            .map(|entry| {
                let cascade_order = patch
                    .dispatch
                    .cascade_order_for_entry(entry.rule, entry.program, entry.entry)?;
                entry.materialize(node, &self.programs, cascade_order)
            })
            .collect::<Option<_>>()?;
        let materialized = self.in_cascade_order(materialized, false);
        let cascade_winners_are_complete = self.cascade_winner_inventory_is_complete(&materialized, Some(node));

        verify_match_answer_against_cold(self, &materialized, node, "a retained match answer delta");

        self.remember_prepared_retained_match_answer(node, answer);
        let cascade_winners_updated =
            self.apply_cascade_winner_match_deltas(node, &materialized, deltas, &mut patch.cascade_candidates);
        let new_input =
            self.matches_for_cascade_with_scratch(materialized, false, None, &mut patch.cascade_compaction_workspace);
        let new_cascade_input = self.intern_cascade_input(&new_input);
        let changed = old_cascade_input != new_cascade_input;
        if changed {
            self.counters.bump(Counter::MatchAnswerChanges);
        }
        self.publish_cascade_input(node, new_cascade_input);
        self.counters.bump(Counter::RetainedMatchAnswerDeltaPatches);
        self.counters.add(Counter::RetainedMatchAnswerDeltaEntries, applied);
        let emit = patch.always_emit || changed;
        if !emit {
            self.counters.bump(Counter::RetainedMatchAnswerPatchStops);
        }
        // The whole transition is node-independent for the cohort: remember the new answer
        // identity, the compact identity, and the settled winner state, so every further member
        // replays by column stores whether the transition stopped or emitted.
        if let Some(key) = memo_key
            && let Lookup::Known(new_identity) = self.retained_match_answers.lookup(node)
        {
            let new_identity = *new_identity;
            let winner_state = match self
                .winner_groups
                .lookup(WinnerGroupKey::current(node, self.program.version()))
            {
                Lookup::Known(&(state, version)) => Some((state, version)),
                Lookup::KnownAbsent | Lookup::Missing(_) => None,
            };
            let pseudo_winner_states = self.settled_pseudo_winner_states(node);
            Self::remember_retained_answer_delta_transition(
                patch,
                key,
                deltas,
                RetainedAnswerDeltaTransition {
                    new_answer: new_identity,
                    new_cascade_input,
                    winner_state,
                    pseudo_winner_states,
                    winners_updated: cascade_winners_updated,
                    cascade_winners_are_complete,
                },
            );
        }
        Some(RetainedAnswerPatchOutcome {
            identity_preserved: false,
            emit,
            incremental_cascade_answer: (emit && cascade_winners_updated).then(|| IncrementalCascadeAnswer {
                node,
                cascade_input: new_cascade_input,
                matches: Some(new_input.into_boxed_slice()),
                cascade_winners_are_complete,
            }),
        })
    }

    /// Re-derive the affected rules' matches at one node through the shared filtered
    /// interpreter: the shared prefix states answer every prefix-covered entry from one
    /// transition per node, the shared workspace carries relation and sibling-prefix caches
    /// across nodes, and the dispatch narrows the rest. `None` means exact evaluation could not
    /// complete, which retires the answer.
    pub(super) fn filtered_patch_replacement(
        &mut self,
        node: StyleNodeID,
        patch: &mut RetainedAnswerPatch,
        narrowed_keys: Option<&[(RuleID, SelectorProgramID)]>,
    ) -> Option<Vec<RuleMatch>> {
        let affected_keys: &[(RuleID, SelectorProgramID)] = narrowed_keys.unwrap_or(&patch.rule_keys);
        self.counters.bump(Counter::SelectorTruthRepairUpqueries);
        let mut matches = RuleMatches::new();
        let prefix_caches = Rc::clone(&patch.prefix_caches);
        let mut caches = prefix_caches.borrow_mut();
        let mut states = caches.states.get_or_insert(
            patch.scope_program,
            self.facts.primary().generation(),
            self.facts.primary().row_count(),
        );
        let interpreter = BatchMatcher::new(
            &self.tree,
            self.facts.primary(),
            &patch.dispatch,
            &self.programs,
            &self.program,
        )
        .with_rule_filter(affected_keys)
        .with_match_workspace(&patch.match_workspace);
        let result = interpreter.match_node_collecting_requests(
            node,
            &mut matches,
            &mut self.counters,
            BatchMatchState {
                dispatch_workspace: &mut patch.dispatch_workspace,
                requests: None,
                completed: None,
                prefix_states: Some(&mut states),
                deferred_prefix_matches: None,
            },
        );
        let _ = states;
        caches.states.settle_memory(&mut self.memory);
        if result.is_err() {
            matches.settle_memory(&mut self.memory);
            matches.release(&mut self.memory);
            return None;
        }
        let replacement = matches.take(&mut self.memory);
        Some(replacement)
    }

    /// Patch one retained exact answer by re-evaluating only the rules reached by this transaction.
    ///
    /// `None` means the optional old side was absent or exact evaluation could not complete. The
    /// caller then retires the answer and publishes an ordinary exact reaction.
    pub(super) fn patch_retained_match_answer(
        &mut self,
        node: StyleNodeID,
        patch: &mut RetainedAnswerPatch,
        truth_patch: SelectorTruthPatch<'_>,
    ) -> Option<RetainedAnswerPatchOutcome> {
        let old_identity = *self.retained_match_answers.lookup(node).sparse().ok()?;
        let retained = Rc::clone(self.match_answers.retained_answer(old_identity)?);
        let old_cascade_input = *self.retained_match_answers.cascade_input_lookup(node).sparse().ok()?;
        if !self.match_answer_is_comparable_across_elements(node) {
            return None;
        }
        if let SelectorTruthPatch::Direct(deltas) = truth_patch
            && let Some(changed) =
                self.apply_retained_match_answer_deltas(node, patch, old_identity, &retained, old_cascade_input, deltas)
        {
            return Some(changed);
        }
        let mixed_deltas = match truth_patch {
            SelectorTruthPatch::Refresh { deltas, .. } | SelectorTruthPatch::Attributed { deltas, .. } => Some(deltas),
            SelectorTruthPatch::Full | SelectorTruthPatch::Direct(_) => None,
        };
        // Materializing the adjusted base has a fixed compact-answer copy cost. Require at least
        // two avoided matcher queries so small mixed patches keep their cheaper existing path.
        let delta_base = if patch.cascade_update_properties.is_empty() {
            mixed_deltas.filter(|deltas| deltas.len() >= 2).and_then(|deltas| {
                self.retained_answer_after_deltas(node, patch, &retained, deltas)
                    .map(|(answer, _)| answer)
            })
        } else {
            None
        };
        // The routes that planned this node named exactly these affected rules, so only they can
        // have changed truth here; every other affected rule was proven unchanged on this node by
        // its own narrowing. The filtered evaluation shrinks accordingly.
        let narrowed_keys: Option<Vec<(RuleID, SelectorProgramID)>> = match truth_patch {
            SelectorTruthPatch::Full => None,
            SelectorTruthPatch::Direct(deltas) => Some(
                deltas
                    .iter()
                    .map(|delta| (delta.rule, self.programs.entry_location(delta.entry).0))
                    .collect(),
            ),
            SelectorTruthPatch::Refresh { deltas, refreshes } => Some(match delta_base.is_some() {
                true => refreshes
                    .iter()
                    .filter_map(|refresh| {
                        refresh
                            .rule
                            .map(|(rule, entry)| (rule, self.programs.entry_location(entry).0))
                    })
                    .collect(),
                false => deltas
                    .iter()
                    .map(|delta| (delta.rule, self.programs.entry_location(delta.entry).0))
                    .chain(refreshes.iter().filter_map(|refresh| {
                        refresh
                            .rule
                            .map(|(rule, entry)| (rule, self.programs.entry_location(entry).0))
                    }))
                    .collect(),
            }),
            SelectorTruthPatch::Attributed {
                deltas,
                refreshes,
                rules,
            } => Some(match delta_base.is_some() {
                true => refreshes
                    .iter()
                    .filter_map(|refresh| {
                        refresh
                            .rule
                            .map(|(rule, entry)| (rule, self.programs.entry_location(entry).0))
                    })
                    .chain(
                        rules
                            .iter()
                            .map(|&(rule, entry)| (rule, self.programs.entry_location(entry).0)),
                    )
                    .collect(),
                false => deltas
                    .iter()
                    .map(|delta| (delta.rule, self.programs.entry_location(delta.entry).0))
                    .chain(refreshes.iter().filter_map(|refresh| {
                        refresh
                            .rule
                            .map(|(rule, entry)| (rule, self.programs.entry_location(entry).0))
                    }))
                    .chain(
                        rules
                            .iter()
                            .map(|&(rule, entry)| (rule, self.programs.entry_location(entry).0)),
                    )
                    .collect(),
            }),
        }
        .map(|mut keys: Vec<_>| {
            keys.retain(|key| patch.rule_keys.binary_search(key).is_ok());
            keys.sort_unstable();
            keys.dedup();
            keys
        });
        if let Some(keys) = narrowed_keys.as_deref() {
            self.counters.bump(Counter::RetainedMatchAnswerFilteredPatches);
            if keys.is_empty() {
                if let Some(deltas) = mixed_deltas
                    && delta_base.is_some()
                {
                    return self.apply_retained_match_answer_deltas(
                        node,
                        patch,
                        old_identity,
                        &retained,
                        old_cascade_input,
                        deltas,
                    );
                }
                // Nothing this transaction affects reached the node through a rule-naming route.
                self.publish_cascade_input(node, old_cascade_input);
                self.counters.bump(Counter::RetainedMatchAnswerPatches);
                if !patch.always_emit {
                    self.counters.bump(Counter::RetainedMatchAnswerPatchStops);
                }
                return Some(RetainedAnswerPatchOutcome {
                    identity_preserved: true,
                    emit: patch.always_emit,
                    incremental_cascade_answer: None,
                });
            }
        }

        self.facts.primary().row_of(node)?;
        // One filtered cold match answers the affected rules through the same machinery a full
        // match uses; both the unchanged-set stop and the full patch below consume its result.
        let replacement = self.filtered_patch_replacement(node, patch, narrowed_keys.as_deref())?;
        let affected_keys: &[(RuleID, SelectorProgramID)] = narrowed_keys.as_deref().unwrap_or(&patch.rule_keys);
        let retained_base: &[RetainedRuleMatch] = delta_base.as_deref().unwrap_or(retained.as_ref());

        // Reuse the replacement allocation and append only old entries which survive into the
        // repaired answer. The compact retained slice is the old side for delta derivation below,
        // so no materialized old snapshot is needed.
        // Both inputs are ordered by (rule, program), so count survivors with one merge walk.
        let mut affected_index = 0;
        let unaffected_count = retained_base
            .iter()
            .filter(|entry| {
                let key = (entry.rule, entry.program);
                while affected_keys
                    .get(affected_index)
                    .is_some_and(|affected| *affected < key)
                {
                    affected_index += 1;
                }
                affected_keys.get(affected_index) != Some(&key)
            })
            .count();
        let mut patched_answer = replacement;
        patched_answer.reserve_exact(unaffected_count);
        for &entry in retained_base {
            if affected_keys.binary_search(&(entry.rule, entry.program)).is_ok() {
                continue;
            }
            let cascade_order = patch
                .dispatch
                .cascade_order_for_entry(entry.rule, entry.program, entry.entry)?;
            patched_answer.push(entry.materialize(node, &self.programs, cascade_order)?);
        }
        // A refresh is a typed request for exact old/new truth, not an alternate match-answer
        // update path. Turn the repaired relation into signed entry deltas and apply those through
        // the same authoritative operator as routes which had complete facts during planning.
        let repair_deltas = repaired_selector_truth_deltas(node, &retained, &mut patched_answer, &self.programs);
        if let Some((repair_deltas, changed)) = repair_deltas.as_deref().and_then(|repair_deltas| {
            self.apply_retained_match_answer_deltas(
                node,
                patch,
                old_identity,
                &retained,
                old_cascade_input,
                repair_deltas,
            )
            .map(|changed| (repair_deltas, changed))
        }) {
            for delta in repair_deltas {
                self.counters.bump(match delta.change {
                    SetChange::Added => Counter::SelectorTruthRepairAdditions,
                    SetChange::Removed => Counter::SelectorTruthRepairRemovals,
                });
            }
            return Some(changed);
        }

        let patched_answer = self.in_cascade_order(patched_answer, false);

        verify_match_answer_against_cold(self, &patched_answer, node, "a retained match answer patch");

        // Most patches end where they began: the filtered match returns exactly the entries it
        // displaced. An identical answer keeps its stored cascade input, so skip re-deriving and
        // re-interning its cascade expansion.
        let matches_retained_answer = patched_answer.len() == retained.len()
            && patched_answer.iter().all(|entry| {
                retained
                    .binary_search(&RetainedRuleMatch::from_rule_match(*entry))
                    .is_ok()
            });
        if matches_retained_answer && !patch.orders_shifted {
            self.publish_cascade_input(node, old_cascade_input);
            self.counters.bump(Counter::RetainedMatchAnswerPatches);
            let emit = patch.always_emit;
            if !emit {
                self.counters.bump(Counter::RetainedMatchAnswerPatchStops);
            }
            return Some(RetainedAnswerPatchOutcome {
                identity_preserved: true,
                emit,
                incremental_cascade_answer: None,
            });
        }

        self.remember_retained_match_answer(node, &patched_answer);
        let new_input =
            self.matches_for_cascade_with_scratch(patched_answer, false, None, &mut patch.cascade_compaction_workspace);
        let new_cascade_input = self.intern_cascade_input(&new_input);
        let changed = old_cascade_input != new_cascade_input;
        if changed {
            self.counters.bump(Counter::MatchAnswerChanges);
        }
        self.publish_cascade_input(node, new_cascade_input);
        self.counters.bump(Counter::RetainedMatchAnswerPatches);
        let emit = patch.always_emit || changed;
        if !emit {
            self.counters.bump(Counter::RetainedMatchAnswerPatchStops);
        }
        Some(RetainedAnswerPatchOutcome {
            identity_preserved: !changed,
            emit,
            incremental_cascade_answer: None,
        })
    }

    /// Name one ask's answer so the cascade can share its expansion within this transaction.
    pub(super) fn remember_cascade_input(&mut self, node: StyleNodeID, matches: &[RuleMatch]) {
        if !self.match_answer_is_comparable_across_elements(node) {
            self.retained_match_answers
                .forget_cascade_input(&mut self.match_answers, node);
            return;
        }
        let cascade_input = self.intern_cascade_input(matches);
        self.publish_cascade_input(node, cascade_input);
    }

    pub(super) fn intern_cascade_input(&mut self, matches: &[RuleMatch]) -> MatchAnswerID {
        // The catalog converts matches to RetainedRuleMatch before canonicalizing them. That
        // representation already omits the element identity and absolute cascade rank, so cloning,
        // normalizing and sorting RuleMatch here would canonicalize fields the catalog discards.
        let identity = self.match_answers.intern(matches);
        let reused = self.match_answers.has_cascade_reference(identity);
        self.counters.bump(if reused {
            Counter::MatchAnswerSignatureReuses
        } else {
            Counter::MatchAnswerSignatures
        });
        identity
    }

    pub fn match_element(&mut self, node: StyleNodeID) -> Result<Vec<RuleMatch>, Incomplete> {
        self.match_element_for_purpose(node, false)
    }

    /// Match one element and discard rules that cannot contribute to its cascade.
    pub fn match_element_for_cascade(&mut self, node: StyleNodeID) -> Result<Vec<RuleMatch>, Incomplete> {
        self.match_element_for_purpose(node, true)
    }

    pub(super) fn match_element_for_purpose(
        &mut self,
        node: StyleNodeID,
        compact_for_cascade: bool,
    ) -> Result<Vec<RuleMatch>, Incomplete> {
        self.match_element_for_purpose_with_compact_answer(node, compact_for_cascade, None, None)
    }

    pub(super) fn complete_published_match_answer(
        &mut self,
        node: StyleNodeID,
        retained_answer_dispatch: Option<&RuleDispatch>,
    ) -> Result<PublishedMatchAnswer, Incomplete> {
        if !self.match_answer_is_retainable(node) {
            self.retained_match_answers.forget_answer(&mut self.match_answers, node);
        }
        let tree_scope = self.tree.tree_scope(node);
        let scoped_dispatch = (tree_scope != TreeScopeID::DOCUMENT).then(|| self.ranked_scope_program(tree_scope).1);
        let retained_answer_dispatch = scoped_dispatch.as_deref().or(retained_answer_dispatch);
        let retained_answer = retained_answer_dispatch.and_then(|dispatch| {
            let retained = Rc::clone(self.retained_match_answer(node).sparse().ok()?);
            let exact_answer = retained
                .iter()
                .copied()
                .map(|entry| {
                    let cascade_order = dispatch.cascade_order_for_entry(entry.rule, entry.program, entry.entry)?;
                    entry.materialize(node, &self.programs, cascade_order)
                })
                .collect::<Option<Vec<_>>>()?;
            let cascade_winners_are_complete = self.cascade_winner_inventory_is_complete(&exact_answer, Some(node));
            let answer = self.matches_for_cascade(exact_answer, false, Some(node));
            Some((answer, cascade_winners_are_complete))
        });
        let (matches, cascade_winners_are_complete, compact_answer) =
            if let Some((answer, cascade_winners_are_complete)) = retained_answer {
                self.remember_cascade_input(node, &answer);
                self.counters.bump(Counter::RetainedMatchAnswerReuses);
                (answer, cascade_winners_are_complete, None)
            } else {
                // Ask for an exact, retainable answer rather than a winner-pruned one: pruning is
                // cheaper once, but the pruned answer cannot enter the retained relation, and this
                // node will then cold-match again on every flush that plans it. Once retained-answer
                // admission closes, later new nodes go back to the cheap pruned form, so exactness
                // is only paid while answers can still enter the retained relation.
                self.complete_answers_exactly = !self.completion_exactness_exhausted;
                let mut compact_answer = None;
                let mut cascade_winners_are_complete = false;
                let answer = self.match_element_for_purpose_with_compact_answer(
                    node,
                    true,
                    Some(&mut compact_answer),
                    Some(&mut cascade_winners_are_complete),
                );
                self.complete_answers_exactly = false;
                let answer = answer?;
                (answer, cascade_winners_are_complete, compact_answer)
            };
        let cascade_input = self
            .retained_match_answers
            .cascade_input_lookup(node)
            .sparse()
            .ok()
            .copied();
        Ok(PublishedMatchAnswer {
            node,
            cascade_input,
            matches: compact_answer.is_none().then(|| matches.into_boxed_slice()),
            cascade_winners_are_complete,
            observed: false,
        })
    }

    /// Complete a node from a matching node's already compacted cascade input and winner rows.
    pub(super) fn complete_published_match_answer_from_cascade_input(
        &mut self,
        node: StyleNodeID,
        source: StyleNodeID,
        cascade_input: MatchAnswerID,
        cascade_winners_are_complete: bool,
    ) -> Option<PublishedMatchAnswer> {
        // Both nodes use this completion batch's document dispatch. The source already proved
        // this answer, and the catalog can materialize it if a consumer needs individual matches.
        // Most consumers use the cascade identity directly, so keep it shared until then.
        self.match_answers.answer(cascade_input)?;
        let published_rows =
            self.winner_groups
                .copy_node_rows(source, node, self.program.version(), &mut self.memory)?;
        self.winner_groups.settle_memory(&mut self.memory);
        self.counters
            .add(Counter::CascadeNodeHandlesPublished, published_rows as u64);
        self.publish_cascade_input(node, cascade_input);
        Some(PublishedMatchAnswer {
            node,
            cascade_input: Some(cascade_input),
            matches: None,
            cascade_winners_are_complete,
            observed: false,
        })
    }

    pub(super) fn has_no_element_declarations(&self, node: StyleNodeID) -> bool {
        ElementDeclarationKind::ALL.iter().all(|&kind| {
            let (declarations, complete) = self.facts.element_declared_properties(node, kind);
            complete && declarations.is_empty()
        })
    }

    /// Whether sharing this compaction avoids enough declaration candidates to cover its fixed
    /// identity lookup and winner-row copy costs.
    pub(super) fn shared_cascade_completion_is_profitable(
        &self,
        answer: MatchAnswerID,
        cascade_input: MatchAnswerID,
    ) -> bool {
        let Some(full) = self.match_answers.answer(answer) else {
            return false;
        };
        let Some(compact) = self.match_answers.answer(cascade_input) else {
            return false;
        };
        let declaration_count = |answer: &[RetainedRuleMatch]| {
            answer
                .iter()
                .map(|matched| self.program.declared_properties_of(matched.rule).len())
                .sum::<usize>()
        };
        declaration_count(full) >= declaration_count(compact).saturating_add(MIN_SHARED_CASCADE_COMPLETION_SAVINGS)
    }

    /// Complete the transaction output over nodes added by the style consumer's inheritance
    /// closure. The active traversal owns the current cascade orders for retained answers, so lend
    /// them to the same completion path used before publication. A retained miss runs exact
    /// matching here, before the closure node is handed to style computation.
    pub(super) fn retained_closure_cascade_input(&self, node: StyleNodeID) -> Option<MatchAnswerID> {
        let cascade_input = *self.retained_match_answers.cascade_input_lookup(node).sparse().ok()?;
        let retained = self.match_answers.answer(cascade_input)?;
        if !matches!(
            self.winner_groups
                .token_for(WinnerGroupKey::current(node, self.program.version())),
            Lookup::Known(_)
        ) {
            return None;
        }
        for matched in retained.iter() {
            if matched.tree_scope != TreeScopeID::DOCUMENT
                || self.program.rule_is_gated_by_container_query(matched.rule)
                || !self.program.declarations_are_complete_for(matched.rule)
            {
                return None;
            }
            let entry = self
                .programs
                .get(matched.program)
                .entries()
                .get(matched.entry as usize)?;
            if entry.scope_root.is_some() {
                return None;
            }
        }
        Some(cascade_input)
    }

    pub(super) fn retained_cascade_input_is_exact(&mut self, node: StyleNodeID, cascade_input: MatchAnswerID) -> bool {
        let Ok((full_answer, verification_winner_groups)) = self.exact_cascade_answer_for_verification(node) else {
            return false;
        };
        let prepared_answer = prepare_retained_match_answer(full_answer.into_iter());
        let answer_is_equal = self
            .match_answers
            .answer(cascade_input)
            .is_some_and(|retained_answer| retained_answer.as_ref() == prepared_answer);
        let winner_rows_are_equal = self.winner_groups.node_rows_are_semantically_equal(
            &verification_winner_groups,
            node,
            self.program.version(),
        );
        answer_is_equal && winner_rows_are_equal
    }

    /// Whether any entry of the answer observes sibling or positional relations, whose truth is
    /// maintained state in the prefix automaton.
    pub(super) fn answer_observes_sibling_relations(&self, input: MatchAnswerID) -> bool {
        let Some(rows) = self.match_answers.answer(input) else {
            return true;
        };
        rows.iter().any(|row| {
            self.programs
                .get(row.program)
                .entries()
                .get(row.entry as usize)
                .is_none_or(|entry| entry.observes_sibling_relation())
        })
    }

    /// Prove that an added-only transition between two compact answers cannot change any cascade
    /// winner, by checking every added rule's declared properties against the node's previously
    /// published winner rows: O(delta) instead of a cold re-derivation, so it holds even where
    /// the winner inventory is incomplete. Any gap answers false: a removal, a pseudo-targeted or
    /// scoped or container-gated added rule, an added rule whose declaration list is incomplete
    /// (custom properties), or provenance the previous state cannot answer for.
    pub(super) fn answer_transition_cannot_change_cascade(
        &mut self,
        node: StyleNodeID,
        previous_input: MatchAnswerID,
        current_input: MatchAnswerID,
    ) -> bool {
        // Identity equality is NOT a proof: a stale retained answer compares equal to itself.
        if previous_input == current_input {
            return false;
        }
        let target = computed::ComputedStyleTarget::new(node, u8::MAX);
        let Some((previous_generation, previous_state)) = self.computed_group_sets.cascade_state(target) else {
            self.counters.bump(Counter::TransitionProofNoPreviousState);
            return false;
        };
        if previous_generation != self.winner_groups.generation() {
            self.counters.bump(Counter::TransitionProofGenerationGap);
            return false;
        }
        let Some(previous_rows) = self.match_answers.answer(previous_input) else {
            self.counters.bump(Counter::TransitionProofMissingAnswer);
            return false;
        };
        let Some(current_rows) = self.match_answers.answer(current_input) else {
            self.counters.bump(Counter::TransitionProofMissingAnswer);
            return false;
        };
        if ElementDeclarationKind::ALL
            .iter()
            .any(|&kind| !self.facts.element_declared_properties(node, kind).1)
        {
            self.counters.bump(Counter::TransitionProofElementDeclarations);
            return false;
        }
        let (mut i, mut j) = (0, 0);
        while i < previous_rows.len() || j < current_rows.len() {
            let added = match (previous_rows.get(i), current_rows.get(j)) {
                (Some(previous), Some(current)) if previous == current => {
                    i += 1;
                    j += 1;
                    continue;
                }
                (Some(previous), Some(current)) if previous < current => {
                    let _ = previous;
                    self.counters.bump(Counter::TransitionProofRemoval);
                    return false;
                }
                (Some(_), Some(current)) => {
                    j += 1;
                    *current
                }
                (Some(_), None) => {
                    self.counters.bump(Counter::TransitionProofRemoval);
                    return false;
                }
                (None, Some(current)) => {
                    j += 1;
                    *current
                }
                (None, None) => unreachable!(),
            };
            if added.tree_scope != TreeScopeID::DOCUMENT
                || self.program.rule_is_gated_by_container_query(added.rule)
                || !self.program.declarations_are_complete_for(added.rule)
            {
                self.counters.bump(Counter::TransitionProofUnsafeRule);
                return false;
            }
            let Some(entry) = self.programs.get(added.program).entries().get(added.entry as usize) else {
                self.counters.bump(Counter::TransitionProofUnsafeRule);
                return false;
            };
            if entry.pseudo_element.is_some() || entry.scope_root.is_some() {
                self.counters.bump(Counter::TransitionProofPseudoOrScope);
                return false;
            }
            if !self.rule_has_complete_element_winners(added.rule, entry) {
                self.counters.bump(Counter::TransitionProofElementWinnerGap);
                return false;
            }
            for declared in self.program.declared_properties_of(added.rule) {
                if matches!(
                    declared.operator,
                    CascadeOperator::Revert | CascadeOperator::RevertLayer
                ) {
                    self.counters.bump(Counter::TransitionProofOperatorOrContinuation);
                    return false;
                }
                let Some(winner) = self.winner_groups.winner_in_state(previous_state, declared.property) else {
                    self.counters.bump(Counter::TransitionProofWinnerGap);
                    return false;
                };
                if self.winner_groups.continuation(winner.key.continuation).is_some() {
                    self.counters.bump(Counter::TransitionProofOperatorOrContinuation);
                    return false;
                }
                if winner.source == WinnerSource::ExactCascade {
                    self.counters.bump(Counter::TransitionProofWinnerGap);
                    return false;
                }
                let priority = self.cascade_priority_of(
                    added.rule,
                    added.tree_scope,
                    entry.specificity,
                    added.scope_proximity,
                    declared.important,
                );
                if priority >= winner.priority {
                    self.counters.bump(Counter::TransitionProofPriorityWin);
                    return false;
                }
            }
        }
        self.counters.bump(Counter::TransitionProofConfirmed);
        true
    }

    pub(super) fn verify_retained_cascade_input(&mut self, node: StyleNodeID, cascade_input: MatchAnswerID) {
        assert!(
            self.retained_cascade_input_is_exact(node, cascade_input),
            "retained cascade identity stop diverged from exact matching"
        );
    }

    pub fn complete_published_match_answers_for_closure(&mut self, nodes: &[StyleNodeID]) -> Result<(), Incomplete> {
        let retained_answer_dispatch = self
            .batch_matching_traversal
            .as_ref()
            .and_then(|traversal| traversal.retained_answer_dispatch.clone());
        (|| {
            let mut completed = 0;
            for &node in nodes {
                if self.published_match_answers.lookup(node).is_some() {
                    continue;
                }
                let answer = if let Some(cascade_input) = self.retained_closure_cascade_input(node) {
                    self.counters.bump(Counter::PublishedClosureRetainedIdentityStops);
                    // NB: Verify mode proves the identity stop against the full completion on the
                    // side instead of disabling it; the check's own work must not disturb engine
                    // counters, so they are restored around it.
                    verify_style_answer_patch(self, |verifier| {
                        verifier.verify_retained_cascade_input(node, cascade_input);
                    });
                    PublishedMatchAnswer {
                        node,
                        cascade_input: Some(cascade_input),
                        matches: None,
                        cascade_winners_are_complete: false,
                        observed: false,
                    }
                } else {
                    self.complete_published_match_answer(node, retained_answer_dispatch.as_deref())?
                };
                self.published_match_answers
                    .push(answer, &mut self.memory, &mut self.counters);
                completed += 1;
            }
            self.published_match_answers.sort();
            self.counters
                .add(Counter::PublishedMatchAnswerClosureCompletions, completed);
            Ok(())
        })()
    }

    /// Compare the complete element cascade behind the published base style with the current exact
    /// selector transaction. Pseudo-element rows are compared independently before the caller
    /// stops the reaction.
    pub(super) fn exact_cascade_output_is_unchanged(&self, node: StyleNodeID) -> bool {
        let target = computed::ComputedStyleTarget::new(node, u8::MAX);
        let Some((previous_generation, previous)) = self.computed_group_sets.cascade_state(target) else {
            return false;
        };
        let current = match self
            .winner_groups
            .token_for(WinnerGroupKey::current(node, self.program.version()))
        {
            Lookup::Known((current_generation, current)) if current_generation == previous_generation => current,
            Lookup::Known(_) | Lookup::KnownAbsent | Lookup::Missing(_) => return false,
        };
        self.winner_groups.semantic_delta(Some(previous), current).is_empty()
    }

    /// Consume the complete current answer which the immediately preceding style plan retained.
    ///
    /// A miss is not an incomplete selector answer. It means this transaction did not publish an
    /// answer for the node, so the caller may ask the ordinary exact matcher instead.
    pub fn consume_published_match_answer(&mut self, node: StyleNodeID) -> Option<Vec<RuleMatch>> {
        let (mut matches, cascade_input) = self
            .published_match_answers
            .lookup(node)
            .map_or((None, None), |answer| {
                (
                    self.published_match_answers
                        .matches_for(answer)
                        .map(|matches| matches.to_vec()),
                    answer.cascade_input,
                )
            });
        if matches.is_none()
            && let Some(cascade_input) = cascade_input
        {
            let mut answer = Vec::new();
            if self
                .append_catalog_answer(cascade_input, node, None, &mut answer)
                .is_some()
            {
                // The catalog canonicalizes rule identities independently of cascade rank.
                self.order_matches_in_cascade(&mut answer, false);
                matches = Some(answer);
            }
        }
        if let Some(mut matches) = matches {
            self.published_match_answers.mark_observed(node);
            for entry in &mut matches {
                entry.node = node;
            }
            self.counters.bump(Counter::PublishedMatchAnswerConsumptions);
            return Some(matches);
        }
        if !self
            .batch_matching_traversal
            .as_ref()
            .is_some_and(|traversal| traversal.reuse_retained_match_answers)
        {
            return None;
        }
        let exact_answer = self.retained_match_answer(node).sparse().ok().and_then(|answer| {
            let dispatch = self
                .batch_matching_traversal
                .as_ref()
                .expect("a retained answer is consumed only inside a traversal")
                .retained_answer_dispatch
                .as_deref()?;
            answer
                .iter()
                .copied()
                .map(|entry| {
                    let cascade_order = dispatch.cascade_order_for_entry(entry.rule, entry.program, entry.entry)?;
                    entry.materialize(node, &self.programs, cascade_order)
                })
                .collect::<Option<Vec<_>>>()
        });
        let Some(exact_answer) = exact_answer else {
            self.retained_match_answers.forget_answer(&mut self.match_answers, node);
            return None;
        };
        let answer = self.matches_for_cascade(exact_answer, false, Some(node));
        verify_cascade_answer_against_cold(self, &answer, node, "a retained match answer");
        self.remember_cascade_input(node, &answer);
        self.counters.bump(Counter::RetainedMatchAnswerReuses);
        self.counters.bump(Counter::PublishedMatchAnswerConsumptions);
        Some(answer)
    }

    /// Stream a published answer into its consumer. A materialized payload needs no copy; an
    /// identity-only payload is restored in cascade order before it crosses the bridge.
    pub(super) fn consume_published_match_answer_with(
        &mut self,
        node: StyleNodeID,
        capacity: usize,
        mut consume: impl FnMut(
            usize,
            StyleNodeID,
            RuleID,
            SemanticDeclarationID,
            Option<tree::PseudoElementTarget>,
            u32,
            u32,
        ),
    ) -> Option<usize> {
        if let Some(published) = self.published_match_answers.lookup(node) {
            let cascade_input = published.cascade_input;
            let materialized_len = self
                .published_match_answers
                .matches_for(published)
                .map(<[RuleMatch]>::len);
            let compact_len = cascade_input
                .filter(|_| materialized_len.is_none())
                .and_then(|identity| self.match_answers.answer(identity))
                .map(|matches| matches.len());
            if let Some(len) = materialized_len {
                self.published_match_answers.mark_observed(node);
                self.counters.bump(Counter::PublishedMatchAnswerConsumptions);
                if len > capacity {
                    return Some(len);
                }
                let published = self
                    .published_match_answers
                    .lookup(node)
                    .expect("a published answer remains live until the transaction ends");
                let matches = self
                    .published_match_answers
                    .matches_for(published)
                    .expect("a materialized published answer remains live until the transaction ends");
                for (index, matched) in matches.iter().enumerate() {
                    consume(
                        index,
                        node,
                        matched.rule,
                        self.program.ensure_semantic_declaration(matched.rule),
                        matched.pseudo_element,
                        self.cascade_context_host(matched.rule, matched.tree_scope),
                        matched.scope_proximity,
                    );
                }
                return Some(len);
            }
            if let Some(len) = compact_len
                && len > capacity
            {
                self.published_match_answers.mark_observed(node);
                self.counters.bump(Counter::PublishedMatchAnswerConsumptions);
                return Some(len);
            }
        }

        let matches = self.consume_published_match_answer(node)?;
        let len = matches.len();
        if len <= capacity {
            for (index, matched) in matches.iter().enumerate() {
                consume(
                    index,
                    node,
                    matched.rule,
                    self.program.ensure_semantic_declaration(matched.rule),
                    matched.pseudo_element,
                    self.cascade_context_host(matched.rule, matched.tree_scope),
                    matched.scope_proximity,
                );
            }
        }
        Some(len)
    }

    /// Read the shareable identity of one answer from the immediately preceding style transaction.
    ///
    /// A contextual answer has no identity and must still consume its complete payload. A shared
    /// identity lets a downstream cache answer before copying that payload across the bridge.
    pub fn published_match_answer_signature(&mut self, node: StyleNodeID) -> Option<u32> {
        let cascade_input = self.published_match_answers.lookup(node)?.cascade_input?;
        self.published_match_answers.mark_observed(node);
        self.counters.bump(Counter::PublishedMatchAnswerIdentityReads);
        Some(cascade_input.0)
    }

    pub fn pseudo_cascade_states_are_unchanged(&self, node: StyleNodeID) -> bool {
        self.pseudo_cascade_states_are_unchanged_in(node, &self.winner_groups)
    }

    fn pseudo_cascade_states_are_unchanged_in(&self, node: StyleNodeID, winner_groups: &WinnerGroups) -> bool {
        let generation = winner_groups.generation();
        // A held pseudo style the engine has no record of cannot be vouched for: winner rows and
        // computed cascade records are the only witnesses the two arms below can judge, and a
        // Tier-3 eviction can strip both while the consumer still holds the computed style. With
        // no witness the answer must be a recompute, never a vacuous pass.
        let held_kinds_are_witnessed = self.computed_group_sets.assigned_pseudo_kinds(node).all(|kind| {
            self.computed_group_sets
                .pseudo_retained_cascade_state(node, kind)
                .is_some()
                || winner_groups
                    .pseudo_state(
                        node,
                        tree::PseudoElementTarget::new(tree::PseudoElementKind(u16::from(kind))),
                    )
                    .is_some()
        });
        let observed_are_current =
            self.computed_group_sets
                .pseudo_retained_cascade_states(node)
                .all(|(pseudo_kind, computed)| {
                    let pseudo = tree::PseudoElementTarget::new(tree::PseudoElementKind(u16::from(pseudo_kind)));
                    match winner_groups.token_for(WinnerGroupKey::current_pseudo(node, pseudo, self.program.version()))
                    {
                        Lookup::Known((retained_generation, state)) if retained_generation == computed.0 => {
                            winner_groups.semantic_delta(Some(computed.1), state).is_empty()
                        }
                        Lookup::Known(_) | Lookup::KnownAbsent | Lookup::Missing(_) => false,
                    }
                });
        held_kinds_are_witnessed
            && observed_are_current
            && winner_groups
                .pseudo_states(node)
                .filter(|(pseudo, _, _, _)| {
                    matches!(pseudo.kind.0, 0 | 2 | 3 | 6)
                        || u8::try_from(pseudo.kind.0).ok().is_some_and(|kind| {
                            self.computed_group_sets
                                .assigned_pseudo_kinds(node)
                                .any(|assigned| assigned == kind)
                        })
                })
                .all(|(pseudo, version, state, priority_current)| {
                    version == self.program.version()
                        && priority_current
                        && u8::try_from(pseudo.kind.0).ok().is_some_and(|pseudo_kind| {
                            self.computed_group_sets
                                .pseudo_retained_cascade_states(node)
                                .find(|(observed_kind, _)| *observed_kind == pseudo_kind)
                                .map(|(_, state)| state)
                                .is_some_and(|computed| {
                                    computed.0 == generation
                                        && winner_groups.semantic_delta(Some(computed.1), state).is_empty()
                                })
                        })
                })
    }

    pub(super) fn append_exact_matches_in_scope(
        &mut self,
        node: StyleNodeID,
        scope: TreeScopeID,
        context: ExactMatchContext,
        matches: &mut Vec<exact_matcher::ExactRuleMatch>,
    ) -> Result<(), Incomplete> {
        let shadow_root = self.scope_root(scope);
        let mut matcher = ExactMatcher::new(&self.tree, self.facts.primary(), &self.programs, &self.program)
            .in_scope(scope)
            .with_context(context);
        if let Some(shadow_root) = shadow_root {
            matcher = matcher.in_shadow_tree(shadow_root);
        }
        matcher.match_node(node, matches, &mut self.counters)
    }

    /// Match one element from committed facts without consulting derived matching state.
    #[cfg(test)]
    pub(super) fn match_element_with_exact_matcher(
        &mut self,
        node: StyleNodeID,
        compact_for_cascade: bool,
    ) -> Result<Vec<RuleMatch>, Incomplete> {
        let exact_answer = self.exact_match_answer(node)?;
        let answer = match compact_for_cascade {
            true => {
                self.remember_retained_match_answer(node, &exact_answer);
                self.matches_for_cascade(exact_answer, false, Some(node))
            }
            false => exact_answer,
        };
        self.remember_cascade_input(node, &answer);
        Ok(answer)
    }

    fn exact_match_answer(&mut self, node: StyleNodeID) -> Result<Vec<RuleMatch>, Incomplete> {
        let scope = self.tree.tree_scope(node);
        let inner_scope = self
            .tree
            .shadow_root_of(node)
            .and_then(|shadow_root| self.scope_by_root.get(shadow_root).filter(|&inner| inner != scope));
        let slotted_scopes: Vec<_> = self
            .scopes_slotted_into(node)
            .filter(|&slotted| slotted != scope && Some(slotted) != inner_scope)
            .collect();
        let part_scopes: Vec<_> = self
            .part_exposure_scopes(node)
            .filter(|&exposed| exposed != scope && Some(exposed) != inner_scope && !slotted_scopes.contains(&exposed))
            .collect();

        let mut exact = Vec::new();
        for_each_matching_scope(scope, inner_scope, &slotted_scopes, &part_scopes, |scope, context| {
            self.append_exact_matches_in_scope(node, scope, context, &mut exact)
        })?;

        let mut matches = Vec::with_capacity(exact.len());
        for matched in exact {
            let (_, dispatch) = self.ranked_scope_program(matched.tree_scope);
            let cascade_order = dispatch
                .cascade_order_for_entry(matched.rule, matched.program, matched.entry)
                .expect("every exact rule entry has a cascade rank");
            matches.push(RuleMatch {
                node: matched.node,
                pseudo_element: matched.pseudo_element,
                rule: matched.rule,
                program: matched.program,
                entry: matched.entry,
                cascade_order,
                specificity: matched.specificity,
                tree_scope: matched.tree_scope,
                scope_proximity: matched.scope_proximity,
            });
        }

        let can_have_scope_duplicates = inner_scope.is_some() || !slotted_scopes.is_empty() || !part_scopes.is_empty();
        Ok(self.in_cascade_order(matches, can_have_scope_duplicates))
    }

    pub(super) fn exact_match_answer_for_verification(
        &mut self,
        node: StyleNodeID,
    ) -> Result<Vec<RuleMatch>, Incomplete> {
        let counters_before_verification = self.counters.clone();
        let answer = self.exact_match_answer(node);
        self.counters = counters_before_verification;
        answer
    }

    pub(super) fn exact_cascade_answer_for_verification(
        &mut self,
        node: StyleNodeID,
    ) -> Result<(Vec<RuleMatch>, WinnerGroups), Incomplete> {
        let counters_before_verification = self.counters.clone();
        let exact_answer = match self.exact_match_answer(node) {
            Ok(answer) => answer,
            Err(incomplete) => {
                self.counters = counters_before_verification;
                return Err(incomplete);
            }
        };
        let mut verification_winner_groups = self.winner_groups.verification_copy();
        let mut verification_memory = self.memory.verification_copy();
        std::mem::swap(&mut self.winner_groups, &mut verification_winner_groups);
        std::mem::swap(&mut self.memory, &mut verification_memory);
        let answer = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            self.matches_for_cascade(exact_answer, false, Some(node))
        }));
        std::mem::swap(&mut self.memory, &mut verification_memory);
        std::mem::swap(&mut self.winner_groups, &mut verification_winner_groups);
        self.counters = counters_before_verification;
        match answer {
            Ok(answer) => Ok((answer, verification_winner_groups)),
            Err(payload) => std::panic::resume_unwind(payload),
        }
    }

    pub(super) fn match_element_for_purpose_with_compact_answer(
        &mut self,
        node: StyleNodeID,
        compact_for_cascade: bool,
        mut compact_answer: Option<&mut Option<MatchAnswerID>>,
        mut cascade_winners_are_complete: Option<&mut bool>,
    ) -> Result<Vec<RuleMatch>, Incomplete> {
        if compact_for_cascade
            && self.published_match_answers.lookup(node).is_some()
            && let Some(complete) = cascade_winners_are_complete.as_mut()
        {
            **complete = self
                .published_match_answers
                .lookup(node)
                .is_some_and(|answer| answer.cascade_winners_are_complete);
        }
        if compact_for_cascade && let Some(answer) = self.consume_published_match_answer(node) {
            return Ok(answer);
        }
        if self.published_match_answers.lookup(node).is_some() {
            self.counters
                .bump(Counter::MatchElementCallsDuringPublishedStyleTransaction);
        }

        self.counters.bump(Counter::MatchAnswerUpqueries);

        let scope = self.tree.tree_scope(node);
        // A host stands outside the tree its own shadow root opens, and `:host` inside that tree
        // names it, so the tree's own rules are asked of it as well.
        let inner_scope = self
            .tree
            .shadow_root_of(node)
            .and_then(|shadow_root| self.scope_by_root.get(shadow_root).filter(|&inner| inner != scope));
        // A slotted element stands outside the tree it is slotted into, and `::slotted()` inside
        // that tree names it, so that tree's rules are asked of it as well. A slot is itself a
        // slottable, so an element can be re-slotted through several trees, and each of them names
        // it: what stands in the flat tree at the end of the chain is the element, not the slots.
        let slotted_scopes: Vec<TreeScopeID> = self
            .scopes_slotted_into(node)
            .filter(|&slotted| slotted != scope && Some(slotted) != inner_scope)
            .collect();
        // A part is inside a tree the addressing scope does not decide in, and `::part()` in that
        // scope names it. `exportparts` forwards a name outwards one host at a time, and a rule in
        // every tree along that chain can address the element - under the name that level exposes it
        // by - so each of those scopes is asked, not only the outermost one the exposure names.
        let part_scopes: Vec<TreeScopeID> = self
            .part_exposure_scopes(node)
            .filter(|&exposed| exposed != scope && Some(exposed) != inner_scope && !slotted_scopes.contains(&exposed))
            .collect();

        // A reaction derived while applying the published transaction can extend the C++ pass
        // beyond the region whose facts this transaction prepared. Keep using the shared batch
        // for covered nodes, but let an unprepared node take the exact adaptive path below.
        let prepared_batch_contains_node = self
            .batch_matching_traversal
            .as_ref()
            .and_then(|traversal| traversal.batch.as_ref())
            .is_some_and(|batch| batch.row_of(node).is_some());
        if prepared_batch_contains_node {
            // The box moves out of its slot so its fields can be borrowed while `self` stays
            // mutably borrowable; what moves is one pointer, not the batch.
            let mut held_traversal = self.batch_matching_traversal.take().unwrap();
            let traversal = &mut *held_traversal;
            let prefix_caches = Rc::clone(&traversal.prefix_caches);
            let facts = traversal.batch.as_ref().unwrap();
            let mut matches = RuleMatches::new();
            if compact_for_cascade && verify_selector_truth_derivation_is_enabled() {
                matches.enable_selector_truth();
            }
            // OPTIMIZATION: Identical sheet sets share a scope program, so the prefix automaton's
            // answer can be reused across otherwise independent shadow trees.
            let can_defer_prefix_matches =
                compact_for_cascade && inner_scope.is_none() && slotted_scopes.is_empty() && part_scopes.is_empty();
            let mut deferred_prefix_matches = None;
            let mut retained_match_answer_is_exact = compact_for_cascade;
            let result = for_each_matching_scope(
                scope,
                inner_scope,
                &slotted_scopes,
                &part_scopes,
                |asked_scope, context| {
                    let is_primary = context == ExactMatchContext::Ordinary;
                    self.match_node_in_kept_scope(
                        node,
                        asked_scope,
                        facts,
                        &mut traversal.dispatch_workspace,
                        &mut matches,
                        Some(&mut traversal.ancestor_requirements),
                        is_primary.then_some(&prefix_caches),
                        Some(&traversal.match_workspace),
                        BatchMatchRetry {
                            requests: None,
                            completed: None,
                            deferred_prefix_matches: (is_primary && can_defer_prefix_matches)
                                .then_some(&mut deferred_prefix_matches),
                            answer_is_exact: Some(&mut retained_match_answer_is_exact),
                            cascade_only: is_primary && can_defer_prefix_matches && !self.complete_answers_exactly,
                        },
                    )
                },
            );
            let can_have_scope_duplicates =
                inner_scope.is_some() || !slotted_scopes.is_empty() || !part_scopes.is_empty();
            let mut cascade_input = None;
            let mut retained_match_answer = None;
            let mut retained_selector_truth = None;
            let mut retained_match_answer_reused = false;
            let mut retained_match_answer_key_to_remember = None;
            let mut cached_cascade_winner_inventory_is_complete = None;
            let all = match (result, deferred_prefix_matches) {
                (Ok(()), Some(prefix_matches)) => {
                    if retained_match_answer_is_exact {
                        retained_selector_truth = matches.prepared_selector_truth();
                    }
                    let (scope_program, dispatch) = self.ranked_scope_program(scope);
                    let non_prefix_matches =
                        PrefixAnswerCache::non_prefix_identity(&mut self.match_answers, matches.as_slice());
                    let contribution_key = PrefixContributionKey {
                        program: scope_program,
                        matches: prefix_matches,
                    };
                    let mut newly_materialized_exact_prefix = None;
                    if retained_match_answer_is_exact && scope == TreeScopeID::DOCUMENT {
                        let known_exact_prefix = match prefix_caches
                            .borrow()
                            .answers
                            .exact_prefix(&self.match_answers, contribution_key)
                        {
                            Lookup::Known((identity, answer)) => Some((identity, Rc::clone(answer))),
                            Lookup::KnownAbsent => {
                                unreachable!("exact prefix answers are sparse, never known absent")
                            }
                            Lookup::Missing(_) => None,
                        };
                        let (exact_prefix_identity, exact_prefix) = match known_exact_prefix {
                            Some(answer) => answer,
                            None => {
                                let mut prefix_rules = RuleMatches::new();
                                let mut caches = prefix_caches.borrow_mut();
                                match caches.states.lookup_mut(scope_program) {
                                    Lookup::Known(states) => append_prefix_matches(
                                        &mut prefix_rules,
                                        node,
                                        scope,
                                        &dispatch,
                                        &self.program,
                                        &self.programs,
                                        states.matches_in(prefix_matches),
                                        &mut self.counters,
                                        CountRuleMatchEmission::No,
                                    ),
                                    Lookup::KnownAbsent | Lookup::Missing(_) => {
                                        unreachable!("matching retained the scope's prefix states")
                                    }
                                }
                                drop(caches);
                                let prepared = prepare_retained_match_answer(prefix_rules.as_slice().iter().copied());
                                let identity = prefix_caches.borrow_mut().answers.remember_exact_prefix(
                                    &mut self.match_answers,
                                    scope_program,
                                    prefix_matches,
                                    prepared,
                                );
                                newly_materialized_exact_prefix = Some(prefix_rules);
                                (
                                    identity,
                                    Rc::clone(
                                        self.match_answers
                                            .answer(identity)
                                            .expect("a remembered exact prefix answer must remain live"),
                                    ),
                                )
                            }
                        };
                        let exact_answer_key = PrefixAnswerKey {
                            prefix_contribution: exact_prefix_identity,
                            non_prefix_matches,
                        };
                        let known_exact_answer = match prefix_caches.borrow().answers.exact_answer(exact_answer_key) {
                            Lookup::Known(answer) => Some(answer),
                            Lookup::KnownAbsent => {
                                unreachable!("exact answers are sparse, never known absent")
                            }
                            Lookup::Missing(_) => None,
                        };
                        retained_match_answer_reused = known_exact_answer.is_some_and(|answer| {
                            self.retained_match_answers
                                .set_interned_identity(node, &mut self.match_answers, answer)
                        });
                        if !retained_match_answer_reused {
                            let mut retained = prepare_retained_match_answer(matches.as_slice().iter().copied());
                            merge_retained_match_answers(&mut retained, &exact_prefix);
                            retained_match_answer = Some(retained);
                            if known_exact_answer.is_none() {
                                retained_match_answer_key_to_remember = Some(exact_answer_key);
                            }
                        }
                    } else if retained_match_answer_is_exact {
                        // Exact retained answers carry a concrete tree scope. Materialize that
                        // cheap payload locally while still sharing the selector evaluation.
                        let mut prefix_rules = RuleMatches::new();
                        let mut caches = prefix_caches.borrow_mut();
                        match caches.states.lookup_mut(scope_program) {
                            Lookup::Known(states) => append_prefix_matches(
                                &mut prefix_rules,
                                node,
                                scope,
                                &dispatch,
                                &self.program,
                                &self.programs,
                                states.matches_in(prefix_matches),
                                &mut self.counters,
                                CountRuleMatchEmission::No,
                            ),
                            Lookup::KnownAbsent | Lookup::Missing(_) => {
                                unreachable!("matching retained the scope's prefix states")
                            }
                        }
                        drop(caches);
                        let mut retained = prepare_retained_match_answer(matches.as_slice().iter().copied());
                        let exact_prefix = prepare_retained_match_answer(prefix_rules.as_slice().iter().copied());
                        merge_retained_match_answers(&mut retained, &exact_prefix);
                        retained_match_answer = Some(retained);
                        newly_materialized_exact_prefix = Some(prefix_rules);
                    }
                    // OPTIMIZATION: A prefix rule which loses every declaration to another prefix
                    // rule cannot become a winner after adding non-prefix contenders. Canonicalize
                    // that stable contribution before combining the two sets.
                    let known_contribution = match prefix_caches
                        .borrow()
                        .answers
                        .prefix_contribution(&self.match_answers, contribution_key)
                    {
                        Lookup::Known((identity, _)) => Some(identity),
                        Lookup::KnownAbsent => {
                            unreachable!("prefix contributions are sparse, never known absent")
                        }
                        Lookup::Missing(_) => None,
                    };
                    let prefix_contribution = match known_contribution {
                        Some(identity) => identity,
                        None => {
                            let mut prefix_rules = match newly_materialized_exact_prefix.take() {
                                Some(prefix_rules) => {
                                    self.counters
                                        .add(Counter::RuleMatchesEmitted, prefix_rules.len() as u64);
                                    prefix_rules
                                }
                                None => {
                                    let mut prefix_rules = RuleMatches::new();
                                    let mut caches = prefix_caches.borrow_mut();
                                    match caches.states.lookup_mut(scope_program) {
                                        Lookup::Known(states) => append_prefix_matches(
                                            &mut prefix_rules,
                                            node,
                                            scope,
                                            &dispatch,
                                            &self.program,
                                            &self.programs,
                                            states.matches_in(prefix_matches),
                                            &mut self.counters,
                                            CountRuleMatchEmission::Yes,
                                        ),
                                        Lookup::KnownAbsent | Lookup::Missing(_) => {
                                            unreachable!("matching retained the scope's prefix states")
                                        }
                                    }
                                    drop(caches);
                                    prefix_rules
                                }
                            };
                            self.compact_matches_for_cascade_with_scratch(
                                prefix_rules.as_mut_vec(),
                                false,
                                None,
                                &mut traversal.cascade_compaction_workspace,
                            );
                            let contribution = prefix_rules.take(&mut self.memory);
                            prefix_caches.borrow_mut().answers.remember_prefix_contribution(
                                &mut self.match_answers,
                                scope_program,
                                prefix_matches,
                                &contribution,
                            )
                        }
                    };
                    if let Some(mut prefix_rules) = newly_materialized_exact_prefix {
                        prefix_rules.settle_memory(&mut self.memory);
                        prefix_rules.release(&mut self.memory);
                    }
                    let key = PrefixAnswerKey {
                        prefix_contribution,
                        non_prefix_matches,
                    };
                    let answer = if self.node_has_element_declaration_input(node) {
                        append_retained_matches(
                            &mut matches,
                            node,
                            scope,
                            &self.programs,
                            &dispatch,
                            self.match_answers
                                .answer(prefix_contribution)
                                .expect("a prefix contribution must remain live"),
                        )
                        .expect("a prefix contribution must reference live selector entries");
                        self.compact_matches_for_cascade_with_scratch(
                            matches.as_mut_vec(),
                            false,
                            Some(node),
                            &mut traversal.cascade_compaction_workspace,
                        );
                        let answer = matches.take(&mut self.memory);
                        cascade_input = Some(self.intern_cascade_input(&answer));
                        answer
                    } else {
                        let cached_answer = prefix_caches.borrow().answers.lookup(key).sparse().ok().map(|answer| {
                            (
                                answer.matches,
                                answer.winner_group,
                                answer
                                    .pseudo_winner_groups
                                    .as_ref()
                                    .map(|(generation, states)| (*generation, Rc::clone(states))),
                                answer.cascade_input,
                                answer.cascade_winner_inventory_is_complete,
                            )
                        });
                        match cached_answer {
                            Some((
                                answer_matches,
                                winner_group,
                                pseudo_winner_groups,
                                answer_cascade_input,
                                cascade_winner_inventory_is_complete,
                            )) => {
                                self.counters.bump(Counter::PrefixAnswerCacheHits);
                                self.counters.bump(Counter::MatchAnswerSignatureReuses);
                                cascade_input = Some(answer_cascade_input);
                                cached_cascade_winner_inventory_is_complete =
                                    Some(cascade_winner_inventory_is_complete);
                                let mut matches = if scope == TreeScopeID::DOCUMENT
                                    && let Some(compact_answer) = compact_answer.as_mut()
                                {
                                    **compact_answer = Some(answer_cascade_input);
                                    Vec::new()
                                } else {
                                    let mut matches = Vec::new();
                                    self.append_catalog_answer(answer_matches, node, Some(scope), &mut matches)
                                        .expect("a cached prefix answer must reference live selector entries");
                                    matches
                                };
                                if scope != TreeScopeID::DOCUMENT {
                                    // The shared answer stores the scope which first populated it.
                                    // Rebind and compact against this node's contextual declarations.
                                    self.compact_matches_for_cascade(&mut matches, false, Some(node));
                                    cascade_input = Some(self.intern_cascade_input(&matches));
                                    cached_cascade_winner_inventory_is_complete = None;
                                } else {
                                    self.order_matches_in_cascade(&mut matches, false);
                                }
                                let mut published_winner_rows = false;
                                if scope == TreeScopeID::DOCUMENT
                                    && let Some((generation, group)) = winner_group
                                    && self
                                        .winner_groups
                                        .set_from_token(node, generation, group, self.program.version())
                                        .is_ok()
                                {
                                    published_winner_rows = true;
                                }
                                if scope == TreeScopeID::DOCUMENT
                                    && let Some((generation, pseudo_winner_groups)) = pseudo_winner_groups
                                    && generation == self.winner_groups.generation()
                                {
                                    for &(pseudo, state) in pseudo_winner_groups.iter() {
                                        let _ =
                                            self.winner_groups
                                                .set_pseudo(node, pseudo, state, self.program.version());
                                    }
                                    published_winner_rows = true;
                                }
                                if published_winner_rows {
                                    self.winner_groups.settle_memory(&mut self.memory);
                                    self.counters.bump(Counter::CascadeNodeHandlesPublished);
                                }
                                matches
                            }
                            None => {
                                self.counters.bump(Counter::PrefixAnswerCacheMisses);
                                append_retained_matches(
                                    &mut matches,
                                    node,
                                    scope,
                                    &self.programs,
                                    &dispatch,
                                    self.match_answers
                                        .answer(prefix_contribution)
                                        .expect("a prefix contribution must remain live"),
                                )
                                .expect("a prefix contribution must reference live selector entries");
                                self.compact_matches_for_cascade_with_scratch(
                                    matches.as_mut_vec(),
                                    false,
                                    Some(node),
                                    &mut traversal.cascade_compaction_workspace,
                                );
                                let answer = matches.take(&mut self.memory);
                                let winner_group = match self
                                    .winner_groups
                                    .token_for(WinnerGroupKey::current(node, self.program.version()))
                                {
                                    Lookup::Known(token) => Some(token),
                                    Lookup::KnownAbsent => {
                                        unreachable!("winner groups are sparse, never known absent")
                                    }
                                    Lookup::Missing(_) => None,
                                };
                                // The rows the engine settles pseudo-elements from travel with
                                // the group: ::before, ::after, ::first-letter and ::selection.
                                // The rules for ::marker, ::backdrop and the element-backed
                                // pseudo-elements match every element, and a row for every
                                // element would only cost the memory the winner groups have.
                                let pseudo_winner_groups = self.settled_pseudo_winner_states(node);
                                let pseudo_winner_groups = (!pseudo_winner_groups.is_empty())
                                    .then(|| (self.winner_groups.generation(), pseudo_winner_groups));
                                let answer_cascade_input = self.intern_cascade_input(&answer);
                                let cascade_winner_inventory_is_complete =
                                    self.cascade_winner_inventory_is_complete(&answer, Some(node));
                                prefix_caches.borrow_mut().answers.remember(
                                    &mut self.match_answers,
                                    key,
                                    &answer,
                                    winner_group,
                                    pseudo_winner_groups,
                                    answer_cascade_input,
                                    cascade_winner_inventory_is_complete,
                                );
                                cascade_input = Some(answer_cascade_input);
                                answer
                            }
                        }
                    };
                    prefix_caches
                        .borrow_mut()
                        .answers
                        .settle_memory(&self.match_answers, &mut self.memory);
                    Ok(answer)
                }
                (Ok(()), None) => {
                    if retained_match_answer_is_exact {
                        retained_match_answer = Some(prepare_retained_match_answer(matches.as_slice().iter().copied()));
                        retained_selector_truth = matches.prepared_selector_truth();
                    }
                    if compact_for_cascade {
                        self.compact_matches_for_cascade_with_scratch(
                            matches.as_mut_vec(),
                            can_have_scope_duplicates,
                            Some(node),
                            &mut traversal.cascade_compaction_workspace,
                        );
                    } else if !retained_match_answer_is_exact {
                        self.order_matches_in_cascade(matches.as_mut_vec(), can_have_scope_duplicates);
                    }
                    Ok(matches.take(&mut self.memory))
                }
                (Err(incomplete), _) => Err(incomplete),
            };
            matches.settle_memory(&mut self.memory);
            matches.release(&mut self.memory);
            let match_workspace_bytes = traversal.match_workspace.capacity_bytes();
            self.memory.reserve_required(
                MemoryCategory::BatchScratch,
                match_workspace_bytes - traversal.match_workspace_bytes,
            );
            traversal.match_workspace_bytes = match_workspace_bytes;
            let dispatch_workspace_bytes = traversal.dispatch_workspace.capacity_bytes();
            self.memory.reserve_required(
                MemoryCategory::BatchScratch,
                dispatch_workspace_bytes - traversal.dispatch_workspace_bytes,
            );
            traversal.dispatch_workspace_bytes = dispatch_workspace_bytes;
            let cascade_compaction_workspace_bytes = traversal.cascade_compaction_workspace.capacity_bytes();
            self.memory.reserve_required(
                MemoryCategory::BatchScratch,
                cascade_compaction_workspace_bytes - traversal.cascade_compaction_workspace_bytes,
            );
            traversal.cascade_compaction_workspace_bytes = cascade_compaction_workspace_bytes;
            self.batch_matching_traversal = Some(held_traversal);
            if let Ok(matches) = &all {
                if let Some(retained_match_answer) = retained_match_answer {
                    self.remember_prepared_retained_match_answer_with_truth(
                        node,
                        retained_match_answer,
                        retained_selector_truth,
                    );
                    if let Some(key) = retained_match_answer_key_to_remember
                        && let Lookup::Known(&identity) = self.retained_match_answers.lookup(node)
                    {
                        prefix_caches.borrow_mut().answers.remember_exact_answer(
                            &mut self.match_answers,
                            key,
                            identity,
                        );
                        prefix_caches
                            .borrow_mut()
                            .answers
                            .settle_memory(&self.match_answers, &mut self.memory);
                    }
                } else if compact_for_cascade && !retained_match_answer_reused {
                    // A cascade-only shortcut can answer the current style without proving the
                    // complete selector answer. Do not leave an older exact answer resident.
                    self.retained_match_answers.forget(&mut self.match_answers, node);
                }
                if let Some(complete) = cascade_winners_are_complete.as_mut() {
                    **complete = retained_match_answer_is_exact
                        && matches!(self.retained_match_answer(node), Lookup::Known(_))
                        && cached_cascade_winner_inventory_is_complete
                            .unwrap_or_else(|| self.cascade_winner_inventory_is_complete(matches, Some(node)));
                }
                match cascade_input {
                    Some(cascade_input) => self.publish_cascade_input(node, cascade_input),
                    None => self.remember_cascade_input(node, matches),
                }
            }
            return all;
        }

        // Which other nodes a selector reads is a property of the selectors, not of the element:
        // a descendant combinator climbs, a sibling combinator walks back, `:has()` descends. So
        // the batch grows by exactly the node each miss names. A sibling, positional or descendant
        // scan names the rest of the range it can read instead, and the batch takes a doubling
        // window of that range, so a scan that reads its whole range needs a logarithmic number of
        // restarts rather than one per row. Both requests converge because the tree is finite and
        // cover only nodes the interrupted operator can still read.
        //
        // The ancestors are seeded because a descendant or child combinator reads them and one of
        // those is in almost every stylesheet: without this a page of depth eight discovers one
        // ancestor per retry. Other exact requests are collected across the candidate set, and a
        // completion bit keeps candidates that already answered from being replayed while those
        // requests are materialized.
        let mut covered = vec![node];
        covered.extend(self.tree.ancestors(node));
        // The unified automaton's rightward states read the node's left context the same way its
        // down axes read the ancestry, so a sibling-bearing automaton seeds the preceding
        // siblings of the node and of each ancestor up front rather than discovering them one
        // restart at a time.
        if self.ranked_scope_program(scope).1.prefixes().has_sibling_steps() {
            let spine_len = covered.len();
            for level_index in 0..spine_len {
                let mut previous = self.tree.previous_element_sibling(covered[level_index]);
                while let Some(sibling) = previous {
                    covered.push(sibling);
                    previous = self.tree.previous_element_sibling(sibling);
                }
            }
        }
        let mut sibling_window = INITIAL_SIBLING_FACT_WINDOW;
        let mut matches = RuleMatches::new();
        if compact_for_cascade && verify_selector_truth_derivation_is_enabled() {
            matches.enable_selector_truth();
        }
        let mut completed_by_scope: Column<Vec<bool>> = Column::default();
        let mut dispatch_workspace = DispatchCandidateWorkspace::default();
        let prefix_caches_return_to_completion_batch = self.batch_matching_traversal.is_some();
        let prefix_caches = self
            .batch_matching_traversal
            .as_ref()
            .map(|traversal| Rc::clone(&traversal.prefix_caches))
            .unwrap_or_else(|| Rc::new(RefCell::new(PrefixCaches::default())));
        let mut completion_scratch_bytes = 0;
        let mut retained_match_answer_is_exact = compact_for_cascade;
        let mut facts = StyleNodeFacts::new();
        let mut requests = Vec::new();
        loop {
            self.facts.materialize(covered.iter().copied(), &mut facts);
            requests.clear();
            let result = for_each_matching_scope(
                scope,
                inner_scope,
                &slotted_scopes,
                &part_scopes,
                |asked_scope, context| {
                    let is_primary = context == ExactMatchContext::Ordinary;
                    self.match_node_in_kept_scope(
                        node,
                        asked_scope,
                        &facts,
                        &mut dispatch_workspace,
                        &mut matches,
                        None,
                        is_primary.then_some(&prefix_caches),
                        None,
                        BatchMatchRetry {
                            requests: Some(&mut requests),
                            completed: Some(completed_by_scope.entry(asked_scope.0 as usize)),
                            deferred_prefix_matches: None,
                            answer_is_exact: Some(&mut retained_match_answer_is_exact),
                            cascade_only: is_primary
                                && compact_for_cascade
                                && !self.complete_answers_exactly
                                && scope == TreeScopeID::DOCUMENT
                                && inner_scope.is_none()
                                && slotted_scopes.is_empty()
                                && part_scopes.is_empty(),
                        },
                    )
                },
            );
            let current_completion_scratch_bytes = (completed_by_scope.capacity() * size_of::<Vec<bool>>()
                + completed_by_scope
                    .iter()
                    .map(|completed| completed.capacity().div_ceil(8))
                    .sum::<usize>()
                + requests.capacity() * size_of::<Incomplete>())
                as u64
                + dispatch_workspace.capacity_bytes();
            if current_completion_scratch_bytes > completion_scratch_bytes {
                self.memory.reserve_required(
                    MemoryCategory::BatchScratch,
                    current_completion_scratch_bytes - completion_scratch_bytes,
                );
            } else {
                self.memory.release(
                    MemoryCategory::BatchScratch,
                    completion_scratch_bytes - current_completion_scratch_bytes,
                );
            }
            completion_scratch_bytes = current_completion_scratch_bytes;
            let can_have_scope_duplicates =
                inner_scope.is_some() || !slotted_scopes.is_empty() || !part_scopes.is_empty();

            match result {
                Ok(()) => {
                    if retained_match_answer_is_exact {
                        self.order_matches_in_cascade(matches.as_mut_vec(), can_have_scope_duplicates);
                        let retained = prepare_retained_match_answer(matches.as_slice().iter().copied());
                        self.remember_prepared_retained_match_answer_with_truth(
                            node,
                            retained,
                            matches.prepared_selector_truth(),
                        );
                    } else if compact_for_cascade {
                        // A cascade-only shortcut can answer the current style without proving the
                        // complete selector answer. Do not leave an older exact answer resident.
                        self.retained_match_answers.forget(&mut self.match_answers, node);
                    }
                    if compact_for_cascade {
                        self.compact_matches_for_cascade(matches.as_mut_vec(), can_have_scope_duplicates, Some(node));
                    } else if !retained_match_answer_is_exact {
                        self.order_matches_in_cascade(matches.as_mut_vec(), can_have_scope_duplicates);
                    }
                    let all = matches.take(&mut self.memory);
                    self.remember_cascade_input(node, &all);
                    if let Some(complete) = cascade_winners_are_complete.as_mut() {
                        **complete = retained_match_answer_is_exact
                            && matches!(self.retained_match_answer(node), Lookup::Known(_))
                            && self.cascade_winner_inventory_is_complete(&all, Some(node));
                    }
                    self.memory
                        .release(MemoryCategory::BatchScratch, completion_scratch_bytes);
                    if !prefix_caches_return_to_completion_batch {
                        prefix_caches.borrow_mut().states.release();
                    }
                    return Ok(all);
                }
                Err(incomplete) => {
                    let widened = match requests.is_empty() {
                        true => self.widen_fact_coverage(&mut covered, incomplete, &mut sibling_window),
                        false => self.widen_fact_coverage_for_requests(&mut covered, &requests, &mut sibling_window),
                    };
                    if let Err(missing) = widened {
                        // The store has no row for it at all, which widening cannot fix.
                        matches.settle_memory(&mut self.memory);
                        matches.release(&mut self.memory);
                        self.memory
                            .release(MemoryCategory::BatchScratch, completion_scratch_bytes);
                        if !prefix_caches_return_to_completion_batch {
                            prefix_caches.borrow_mut().states.release();
                        }
                        return Err(Incomplete::MissingFacts(missing));
                    }
                }
            }
        }
    }
}
