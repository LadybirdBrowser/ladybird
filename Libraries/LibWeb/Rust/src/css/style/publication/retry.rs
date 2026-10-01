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
        let Lookup::Known(cascade_state) = self
            .current_winner_groups()
            .token_for(WinnerGroupKey::current(node, self.program.version()))
        else {
            return Err(Unanswered::Refused);
        };
        if !scratch.font_drive.is_pending_for(node)
            && !self.engine_pseudo_inputs_available(
                node,
                self.computed_group_sets.assigned_style_record(node),
                counters,
            )
        {
            return Err(Unanswered::Refused);
        }
        if !scratch.font_drive.is_pending_for(node)
            && !self.node_declares_custom_properties(node)
            && let Some(old_style_record) = self.computed_group_sets.assigned_style_record(node)
            && let Some(parent) = self.tree.inheritance_parent(node)
            && let Some(parent_record) = self.computed_group_sets.assigned_style_record(parent)
            && let Some(environment) = self.computed_group_sets.custom_property_environment_identity(parent)
            && let Some(pseudo_styles) = self.pseudo_style_mask(node)
        {
            let cache_key = self
                .cold_record_parent(node, parent, parent_record, cascade_state.1)
                .map(|parent| ColdRecordKey {
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
                let pseudos =
                    self.engine_pseudo_records(node, Some(old_record), record, cascade_state.0, scratch, counters);
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
        let cascade_winners_are_complete = self
            .current_published_answer(node)
            .is_some_and(|answer| answer.cascade_winners_are_complete);
        let (_, record) = self.engine_computed_record_delta(
            node,
            cascade_winners_are_complete,
            None,
            ParentInputsMoved {
                inherited_style: true,
                display: true,
            },
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
        let mut scratch = EngineComputedRecordScratch {
            installed_ancestors: Some(InstalledAncestors(())),
            ..EngineComputedRecordScratch::default()
        };
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
