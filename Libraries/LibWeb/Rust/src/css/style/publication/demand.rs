/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Answering a read of one element's style that the host has to make before the next style
//! update takes the document's pending changes.

use super::*;

/// What a record demand answers: the element's record with the pseudo-element records settled
/// beside it, and whether it was computed with a substituted winner.
#[derive(Clone, Copy, Debug)]
pub(crate) struct RecordDemandAnswer {
    pub(crate) record: RetriedEngineRecord,
    pub(crate) uses_substitution: bool,
}

/// Leave to publish a driven row's winners into the retained groups, outside any matching
/// traversal, as a style update does. Every drive holds it but a read-only demand's: the rows that
/// drive reads are its private traversal's, dropped with everything published to it, while a row
/// it republished would outlive the demand.
#[derive(Clone, Copy)]
pub(in crate::css::style) struct WinnerRepublication(());

impl EngineComputedRecordScratch {
    /// The leave to republish winners, which only a read-only demand's drive is without.
    pub(super) fn winner_republication(&self) -> Option<WinnerRepublication> {
        (!self.read_only).then_some(WinnerRepublication(()))
    }
}

/// What a private demand writes for its node beside the record it derives, kept to be put back:
/// a private answer leaves what the next style update reads as it found it.
struct PrivateDemandSaves {
    uses_substitution: bool,
    custom_declarations_read_attributes: bool,
    container_effects: Option<container_queries::ContainerVerdict>,
    container_verdicts: Option<Vec<(RuleID, Option<bool>)>>,
    container_gate_unheld: bool,
    tree_counting: Option<u16>,
}

fn set_contains(set: &mut HashSet<StyleNodeID>, node: StyleNodeID, contains: bool) {
    if contains {
        set.insert(node);
    } else {
        set.remove(&node);
    }
}

impl RetainedState {
    fn save_for_private_demand(&self, node: StyleNodeID) -> PrivateDemandSaves {
        PrivateDemandSaves {
            uses_substitution: self.nodes_with_substituted_records.contains(&node),
            custom_declarations_read_attributes: self.custom_declarations_reading_attributes.contains(&node),
            container_effects: self.container_effects_for_host.get(&node).cloned(),
            container_verdicts: self.published_container_verdicts.get(&node).cloned(),
            container_gate_unheld: self.container_gates_unheld.contains(&node),
            tree_counting: self.nodes_with_tree_counting_records.get(&node).copied(),
        }
    }

    /// Put `node` back the way the private demand found it: the records the drive derived for it go
    /// back as an abandoned derivation's do, and so does everything else the drive wrote for it.
    fn restore_after_private_demand(
        &mut self,
        node: StyleNodeID,
        saves: PrivateDemandSaves,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) {
        self.put_back_engine_computed_records(node, scratch, counters);
        set_contains(&mut self.nodes_with_substituted_records, node, saves.uses_substitution);
        set_contains(
            &mut self.custom_declarations_reading_attributes,
            node,
            saves.custom_declarations_read_attributes,
        );
        match saves.container_effects {
            Some(effects) => {
                self.container_effects_for_host.insert(node, effects);
            }
            None => {
                self.container_effects_for_host.remove(&node);
            }
        }
        match saves.container_verdicts {
            Some(verdicts) => {
                self.published_container_verdicts.insert(node, verdicts);
            }
            None => {
                self.published_container_verdicts.remove(&node);
            }
        }
        set_contains(&mut self.container_gates_unheld, node, saves.container_gate_unheld);
        match saves.tree_counting {
            Some(reads) => {
                self.nodes_with_tree_counting_records.insert(node, reads);
            }
            None => {
                self.nodes_with_tree_counting_records.remove(&node);
            }
        }
        self.discard_private_record_demand_matching_batch();
    }

    /// Keep a private answer's record alive for the host, which reads it once the demand has
    /// returned. The record the target's previous demand held is let go.
    fn hold_demand_record(&mut self, target: computed::ComputedStyleTarget, record: computed::FinalStyleRecordID) {
        self.computed_group_sets.pin_style_record(record.raw());
        if let Some(previous) = self.demand_records.insert(target, record) {
            self.computed_group_sets.unpin_style_record(previous.raw());
        }
    }

    /// Whether everything a demand for `node` reads is current: the tree, the rules and the node's
    /// own facts hold no change the next transaction has yet to take, and no ancestor the node
    /// inherits from owes a computation. A read under pending input is the host's.
    fn record_demand_reads_current_inputs(&self, node: StyleNodeID, host: &HostState, read_only: bool) -> bool {
        if !self.tree.is_live(node)
            || !host.tree_staging.is_empty()
            || host.program_staging.is_dirty()
            || host.sheet_rule_replacement.is_some()
            || !host.journal.markers().is_empty()
            || host.computed_record_verification_counters.is_some()
            || host.journal.inputs().any(|input| {
                input.key.style_node().is_none()
                    || matches!(input.key, InputKey::TreeRelations(_))
                    || matches!(input.key, InputKey::LocalFeature(changed, _) | InputKey::State(changed, _) if changed == node)
            })
        {
            return false;
        }
        // A private read of a row the host is still installing would read a record it has not
        // taken yet.
        if read_only && self.engine_computed_records_pending.contains_key(&node) {
            return false;
        }
        // The nodes owing input are gathered once, so a chain of demands reads the journal once per
        // demand, not once per ancestor.
        let owing: HashSet<StyleNodeID> = host
            .journal
            .inputs()
            .chain(host.deferred_element_style_inputs.iter().copied())
            .filter_map(|input| input.key.style_node())
            .collect();
        owing.is_empty()
            || !std::iter::successors(self.tree.flat_tree_parent(node), |&parent| {
                self.tree.flat_tree_parent(parent)
            })
            .any(|ancestor| owing.contains(&ancestor))
    }
}

impl StyleEngineState {
    /// Answer a read of `node`'s style that the host makes before the next style update, from the
    /// document as it is now and without taking the document's pending transaction. The node is
    /// matched again, so no winner row an earlier batch published answers inputs that moved since.
    /// A targeted demand drives the record in full against the parent as it is now.
    ///
    /// A read-only demand leaves the published rows, the node's records and its pending inputs as
    /// they were: it reuses the node's retained match answer or matches in a private traversal, and
    /// the record it answers is held only for the host to read. Any other demand publishes what it
    /// derives as the next style update would have: the host installs the record and acknowledges
    /// it, and the node's pending inputs are answered.
    pub(in crate::css::style) fn answer_record_demand(
        &mut self,
        node: StyleNodeID,
        demand: bridge::FfiRecordDemand,
        counters: &mut Counters,
    ) -> Drive<RecordDemandAnswer> {
        let bridge::FfiRecordDemand {
            targeted,
            read_only,
            exclude_inline_style,
        } = demand;
        // Only a private read may leave the inline style out: the record it answers is no
        // element's.
        if (exclude_inline_style && !read_only)
            || !self
                .retained
                .record_demand_reads_current_inputs(node, &self.host, read_only)
        {
            return Err(Unanswered::Refused);
        }
        let font_environment_generation = self
            .retained
            .document_style_computation_inputs
            .font_environment_generation;
        if let Some(resolver) = &mut self.retained.font_resolution {
            resolver.prepare(font_environment_generation);
        }
        let private_saves = read_only.then(|| self.retained.save_for_private_demand(node));
        let hidden_inline_declarations = if exclude_inline_style {
            self.retained.facts.hide_inline_declarations_for_demand(node)
        } else {
            None
        };
        if !read_only {
            self.retained.forget_node_match_answer_for_demand(node);
        }
        // The batch's answers for its other rows stay theirs: a row the host still applies, or
        // retries once its ancestors installed, reads its answer after this demand.
        let batch_answers = std::mem::take(&mut self.retained.published_match_answers);
        if read_only || !self.retained.begin_cold_matching_batch(node, counters) {
            self.retained.begin_adaptive_cold_matching_batch(node, counters);
        }
        let retained_dispatch = if read_only {
            self.retained.retained_answer_dispatch_for_traversal(true)
        } else {
            None
        };
        let answer = self
            .retained
            .complete_published_match_answer(node, retained_dispatch.as_deref(), counters);
        let private = match private_saves {
            Some(saves) => {
                // The answer is published to the private traversal only, where the drive reads it.
                if let Ok(answer) = &answer
                    && let Some(traversal) = self.retained.batch_matching_traversal.as_deref_mut()
                {
                    traversal.pending_published.push(
                        PublishedMatchAnswer {
                            node,
                            cascade_input: answer.cascade_input,
                            matches: answer.matches.clone(),
                            cascade_winners_are_complete: answer.cascade_winners_are_complete,
                            observed: false,
                        },
                        &mut self.retained.memory,
                        counters,
                    );
                    let index = traversal.pending_published.entries.len() - 1;
                    traversal
                        .answer_effects
                        .note_published(node, index, &mut self.retained.memory);
                }
                Some((saves, batch_answers))
            }
            None => {
                self.retained.end_cold_matching_batch(counters);
                self.retained.published_match_answers = batch_answers;
                None
            }
        };

        let mut scratch = EngineComputedRecordScratch {
            read_only,
            ..EngineComputedRecordScratch::default()
        };
        let result = answer
            .map_err(|_| Unanswered::Refused)
            .and_then(|answer| self.drive_demanded_record(node, answer, targeted, read_only, &mut scratch, counters));

        if let Some(hidden) = hidden_inline_declarations {
            self.retained
                .facts
                .restore_inline_declarations_after_demand(node, hidden);
        }
        if let Some((saves, batch_answers)) = private {
            if let Ok(answer) = &result
                && let Some(record) = computed::FinalStyleRecordID::from_raw(answer.record.style_record)
            {
                self.retained
                    .hold_demand_record(computed::ComputedStyleTarget::new(node, u8::MAX), record);
            }
            self.retained
                .restore_after_private_demand(node, saves, &mut scratch, counters);
            self.retained.published_match_answers = batch_answers;
        }
        result
    }

    fn drive_demanded_record(
        &mut self,
        node: StyleNodeID,
        answer: PublishedMatchAnswer,
        targeted: bool,
        read_only: bool,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) -> Drive<RecordDemandAnswer> {
        let complete =
            answer.cascade_winners_are_complete || self.cascade_winners_are_complete_but_for_custom_properties(node);
        if !read_only {
            self.retained
                .computed_group_sets
                .set_node_answer_incomplete(node, !complete);
            self.retained
                .published_match_answers
                .push(answer, &mut self.retained.memory, counters);
            self.retained.published_match_answers.sort();
        }
        let parent_inputs_moved = ParentInputsMoved {
            inherited_style: targeted,
            display: targeted,
        };
        let mut suspended_memory = MemoryLease::new(MemoryCategory::BatchScratch);
        let (_, record) = loop {
            match self.engine_computed_record_delta(node, complete, None, parent_inputs_moved, scratch, counters) {
                Err(Unanswered::Suspended(Suspension::Font)) => {
                    let request = scratch.font_drive.take_suspended_request();
                    suspended_memory.resize_required_to(&mut self.retained.memory, scratch.font_drive.capacity_bytes());
                    self.refill_font_request(node, request, counters);
                }
                delta => break delta?,
            }
        };
        let mut answered = RecordDemandAnswer {
            record: RetriedEngineRecord {
                style_record: record.raw(),
                explicitly_inherited_groups: scratch.element_explicitly_inherited_groups,
                ..RetriedEngineRecord::default()
            },
            uses_substitution: scratch.element_uses_substitution,
        };
        for delta in &scratch.pseudo_deltas {
            let kind = usize::from(delta.kind);
            if kind < bridge::RETRY_PSEUDO_RECORD_SLOTS {
                answered.record.pseudo_records_present |= 1 << kind;
                answered.record.pseudo_records[kind] = delta.new_style_record.raw();
            }
        }
        if !read_only {
            self.host.journal.acknowledge_node(node, &mut self.retained.memory);
            self.consume_element_style_input(node);
            self.retained.style_input_nodes_for_cpp.remove(&node);
        }
        Ok(answered)
    }
}
