/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Asking again for a record the engine declined while the host was still styling the rows before
//! it.

use super::*;

/// What only a retry knows: the host has applied every row before the one asked for, so a parent
/// without a record is one with no style at all, rather than one the host has yet to style. Only
/// the retry mints it, and only its holder may compute a record over a styleless parent.
pub(in crate::css::style) struct InstalledAncestors(());

impl EngineComputedRecordScratch {
    /// The scratch of a row the host asks for again, driven under what its flush moved.
    fn for_retry(moves: BatchMoves) -> Self {
        Self {
            document_environment_moved: moves.document_environment,
            viewport_moved: moves.viewport,
            root_font_inputs_changed: moves.root_font_inputs,
            installed_ancestors: Some(InstalledAncestors(())),
            host_applies_animation_plans: true,
            ..Self::default()
        }
    }
}

impl RetainedState {
    fn retry_engine_record_after_ancestor_step(
        &mut self,
        node: StyleNodeID,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) -> Drive<u64> {
        let facts = self.computed_group_sets.adjustment_facts(node);
        if facts & bridge::element_adjustment_fact::DISALLOW_DISPLAY_CONTENTS != 0 {
            return Err(Unanswered::Refused);
        }
        // An element standing for its host's pseudo-element is its host's to cascade, and is
        // driven in full against its parent as it is now, from an answer as complete as any.
        if self.backs_host_pseudo_element(node) {
            if !scratch.font_drive.is_pending_for(node) && self.computed_group_sets.node_answer_is_incomplete(node) {
                return Err(Unanswered::Refused);
            }
            let backing_answer_is_complete = self
                .current_published_answer(node)
                .is_some_and(|answer| answer.cascade_winners_are_complete);
            let parent_inputs_moved = ParentInputsMoved {
                inherited_style: true,
                display: true,
            };
            return self
                .engine_computed_record_delta(
                    node,
                    backing_answer_is_complete,
                    None,
                    parent_inputs_moved,
                    None,
                    scratch,
                    counters,
                )
                .map(|(_, record)| record.raw());
        }
        // Winners published while an ancestor was moving, or whose container verdicts the
        // records installed since move, are published again from the node's retained answer
        // before anything is derived from them.
        let republished_complete = if self.container_winners_are_stale(node) {
            let republication = scratch.winner_republication().or_refused()?;
            Some(
                self.republish_winners_from_answer(node, republication, counters)
                    .ok_or(Unanswered::Refused)?,
            )
        } else {
            None
        };
        let Lookup::Known(cascade_state) = self
            .current_winner_groups()
            .token_for(WinnerGroupKey::current(node, self.program.version()))
        else {
            return Err(Unanswered::Refused);
        };
        if !scratch.font_drive.is_pending_for(node)
            && !self.node_declares_custom_properties(node)
            && let Some(old_style_record) = self.computed_group_sets.assigned_style_record(node)
            && let Some(parent) = self.tree.inheritance_parent(node)
            && let Some(parent_record) = self.computed_group_sets.assigned_style_record(parent)
            && let Some(environment) = self.computed_group_sets.custom_property_environment_identity(parent)
            && let Some(pseudo_styles) = self.pseudo_style_mask(node)
        {
            // A record whose winners read beyond its environment is the element's alone.
            let cache_key = self
                .cold_record_parent(node, parent, parent_record, cascade_state.1)
                .filter(|_| !self.state_reads_beyond_environment(node, cascade_state.1))
                .map(|parent| ColdRecordKey {
                    monospace_recascaded_font_size: self
                        .monospace_cohort_key(computed::ComputedStyleTarget::new(node, u8::MAX), cascade_state.1),
                    sibling_position: self.sibling_position_key(node, cascade_state.1),
                    parent,
                    previous_style_record: old_style_record.raw(),
                    generation: cascade_state.0,
                    state: cascade_state.1,
                    facts: cold_record_facts(facts),
                    pseudo_styles,
                    environment,
                    font_environment_generation: self.document_style_computation_inputs.font_environment_generation,
                    root_font_inputs: RootFontInputs::from_document(&self.document_style_computation_inputs),
                });
            if let Some((old_record, record)) = self.assign_cached_cold_record(
                node,
                computed::ComputedStyleTarget::new(node, u8::MAX),
                cascade_state,
                cache_key,
                Some(parent),
                cascade_state.1,
                old_style_record,
                0,
                scratch,
                counters,
            ) {
                let pseudos = self.engine_pseudo_records_beside(
                    node,
                    (old_record, record),
                    None,
                    cascade_state.0,
                    None,
                    scratch,
                    counters,
                );
                if pseudos.is_ok() && scratch.pseudo_uses_substitution {
                    scratch.substitution_effects.push((node, true));
                }
                if let Err(unanswered) = pseudos {
                    // A pseudo-element the engine cannot settle sends the element to C++.
                    match unanswered {
                        Unanswered::Suspended(_) => {
                            scratch.pending_element = Some(drive::PendingElement::new(node, (old_record, record)));
                        }
                        Unanswered::Refused | Unanswered::AwaitsParent => {
                            counters.bump(Counter::RetryAfterAncestorPseudoAbandons);
                            self.abandon_engine_computed_record(node, scratch, counters);
                        }
                    }
                    self.apply_substitution_effects(scratch);
                    return Err(unanswered);
                }
                counters.bump(Counter::RetryAfterAncestorColdHits);
                self.apply_substitution_effects(scratch);
                return Ok(record.raw());
            }
        }
        if !scratch.font_drive.is_pending_for(node) && self.computed_group_sets.node_answer_is_incomplete(node) {
            return Err(Unanswered::Refused);
        }
        let cascade_winners_are_complete = republished_complete.unwrap_or_else(|| {
            self.current_published_answer(node)
                .is_some_and(|answer| answer.cascade_winners_are_complete)
        });
        let (_, record) = self.engine_computed_record_delta(
            node,
            cascade_winners_are_complete,
            None,
            ParentInputsMoved {
                inherited_style: true,
                display: true,
            },
            None,
            scratch,
            counters,
        )?;
        Ok(record.raw())
    }
}

impl StyleEngineState {
    /// Retry a record after C++ has installed earlier records in the same preorder batch. A record
    /// rejected while the batch was planned may become computable once its inheritance parent is
    /// authoritative.
    pub(crate) fn retry_engine_record_after_ancestor(
        &mut self,
        node: StyleNodeID,
        counters: &mut Counters,
    ) -> RetriedEngineRecord {
        let font_environment_generation = self
            .retained
            .document_style_computation_inputs
            .font_environment_generation;
        if let Some(resolver) = &mut self.retained.font_resolution {
            resolver.prepare(font_environment_generation);
        }
        counters.bump(Counter::RetryAfterAncestorCalls);
        let started_at = std::time::Instant::now();
        let mut scratch = EngineComputedRecordScratch::for_retry(self.host.batch_moves_for_retries);
        let mut suspended_memory = MemoryLease::new(MemoryCategory::BatchScratch);
        let style_record =
            self.retry_engine_record_after_ancestor_loop(node, &mut scratch, &mut suspended_memory, counters);
        counters.add(
            Counter::RetryAfterAncestorMicroseconds,
            u64::try_from(started_at.elapsed().as_micros()).unwrap_or(u64::MAX),
        );
        let mut retried = RetriedEngineRecord {
            style_record,
            ..RetriedEngineRecord::default()
        };
        if style_record != 0 {
            retried.explicitly_inherited_groups = scratch.element_explicitly_inherited_groups;
            let held_style_record = self.retained.held_style_records.get(&node).copied().unwrap_or(0);
            retried.owes_an_animation_plan =
                self.retained
                    .row_owes_an_animation_plan(node, held_style_record, style_record);
            retried.owes_a_transition_step = self.retained.row_owes_a_transition_step(node);
            retried.composed_by_the_host = self.retained.host_composes_row(node, held_style_record, style_record);
            for delta in &scratch.pseudo_deltas {
                let kind = usize::from(delta.kind);
                if kind < bridge::RETRY_PSEUDO_RECORD_SLOTS {
                    retried.pseudo_records_present |= 1 << kind;
                    retried.pseudo_records[kind] = delta.new_style_record.raw();
                }
            }
        }
        retried
    }

    fn retry_engine_record_after_ancestor_loop(
        &mut self,
        node: StyleNodeID,
        scratch: &mut EngineComputedRecordScratch,
        suspended_memory: &mut MemoryLease,
        counters: &mut Counters,
    ) -> u64 {
        loop {
            match self.retry_engine_record_after_ancestor_step(node, scratch, counters) {
                Ok(record) => {
                    if record != 0 {
                        counters.bump(Counter::RetryAfterAncestorSettled);
                    }
                    return record;
                }
                Err(Unanswered::Refused | Unanswered::AwaitsParent) => return 0,
                Err(Unanswered::Suspended(Suspension::Font)) => {
                    let request = scratch.font_drive.take_suspended_request();
                    suspended_memory.resize_required_to(&mut self.memory, scratch.font_drive.capacity_bytes());
                    self.refill_font_request(node, request, counters);
                }
            }
        }
    }
}
