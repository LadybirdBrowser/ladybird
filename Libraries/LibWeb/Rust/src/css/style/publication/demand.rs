/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Answering a read of one element's style, or one of its pseudo-elements', that the host has to
//! make before the next style update takes the document's pending changes.

use super::pseudo::PseudoSettlement;
use super::*;

/// What a record demand answers.
#[derive(Clone, Copy, Debug)]
pub(crate) enum RecordDemandAnswer {
    /// The element's record with the pseudo-element records settled beside it, or the
    /// pseudo-element's record, and whether it was computed with a substituted winner.
    Record {
        record: RetriedEngineRecord,
        uses_substitution: bool,
    },
    /// The pseudo-element asked for generates no box.
    Absent,
}

/// The pseudo-elements a demand may ask for: those the engine settles beside their element in a
/// style update. A highlight pseudo-element's read is the host's.
const DEMANDED_PSEUDO_KINDS: [u8; 5] = [
    pseudo_kind::BEFORE,
    pseudo_kind::AFTER,
    pseudo_kind::FIRST_LETTER,
    pseudo_kind::MARKER,
    pseudo_kind::BACKDROP,
];

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

    /// Let go of the records private reads of `node`'s styles were answered with.
    pub(super) fn release_demand_records(&mut self, node: StyleNodeID) {
        let computed_group_sets = &mut self.computed_group_sets;
        self.demand_records.retain(|target, record| {
            let released = target.node() == node;
            if released {
                computed_group_sets.unpin_style_record(record.raw());
            }
            !released
        });
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
    /// inherits from owes a computation. A read under pending input is the host's. A pseudo-element
    /// is settled against its element's installed record, which nothing may be about to move.
    fn record_demand_reads_current_inputs(
        &self,
        node: StyleNodeID,
        host: &HostState,
        read_only: bool,
        pseudo: bool,
    ) -> bool {
        if pseudo
            && (!host.journal.is_empty()
                || !host.deferred_element_style_inputs.is_empty()
                || self.engine_computed_records_pending.contains_key(&node)
                || self.computed_group_sets.assigned_style_record(node).is_none())
        {
            return false;
        }
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
    /// Answer a read of `node`'s style, or of one of its pseudo-elements', that the host makes
    /// before the next style update, from the document as it is now and without taking the
    /// document's pending transaction. The node is matched again, so no winner row an earlier
    /// batch published answers inputs that moved since. A targeted demand drives the element's
    /// record in full against the parent as it is now; a pseudo-element is settled against the
    /// element's installed record.
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
            pseudo_kind_plus_one,
        } = demand;
        let pseudo = pseudo_kind_plus_one.checked_sub(1);
        // Only a private read of an element may leave its inline style out: the record it
        // answers is no element's.
        if (exclude_inline_style && (!read_only || pseudo.is_some()))
            || pseudo.is_some_and(|kind| !DEMANDED_PSEUDO_KINDS.contains(&kind))
            || !self
                .retained
                .record_demand_reads_current_inputs(node, &self.host, read_only, pseudo.is_some())
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
        let result = answer.map_err(|_| Unanswered::Refused).and_then(|answer| match pseudo {
            None => self.drive_demanded_record(node, answer, targeted, read_only, &mut scratch, counters),
            Some(kind) => self.drive_demanded_pseudo_record(node, kind, answer, read_only, &mut scratch, counters),
        });

        if let Some(hidden) = hidden_inline_declarations {
            self.retained
                .facts
                .restore_inline_declarations_after_demand(node, hidden);
        }
        if let Some((saves, batch_answers)) = private {
            if let Ok(RecordDemandAnswer::Record { record, .. }) = &result
                && let Some(record) = computed::FinalStyleRecordID::from_raw(record.style_record)
            {
                let target = computed::ComputedStyleTarget::new(node, pseudo.unwrap_or(u8::MAX));
                self.retained.hold_demand_record(target, record);
            }
            self.retained
                .restore_after_private_demand(node, saves, &mut scratch, counters);
            self.retained.published_match_answers = batch_answers;
        }
        result
    }

    /// Publish the answer a demand matched for `node`, as a batch publishes its rows' answers.
    /// Whether the node's winners are complete.
    fn publish_demanded_answer(
        &mut self,
        node: StyleNodeID,
        answer: PublishedMatchAnswer,
        read_only: bool,
        counters: &mut Counters,
    ) -> bool {
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
        complete
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
        let complete = self.publish_demanded_answer(node, answer, read_only, counters);
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
        let mut answered = RetriedEngineRecord {
            style_record: record.raw(),
            explicitly_inherited_groups: scratch.element_explicitly_inherited_groups,
            ..RetriedEngineRecord::default()
        };
        for delta in &scratch.pseudo_deltas {
            let kind = usize::from(delta.kind);
            if kind < bridge::RETRY_PSEUDO_RECORD_SLOTS {
                answered.pseudo_records_present |= 1 << kind;
                answered.pseudo_records[kind] = delta.new_style_record.raw();
            }
        }
        if !read_only {
            self.host.journal.acknowledge_node(node, &mut self.retained.memory);
            self.consume_element_style_input(node);
            self.retained.style_input_nodes_for_cpp.remove(&node);
        }
        Ok(RecordDemandAnswer::Record {
            record: answered,
            uses_substitution: scratch.element_uses_substitution,
        })
    }

    /// Settle the one pseudo-element `kind` of `node` against the element's installed record, as a
    /// style update settles it: absent where it generates no box. A read-only read is answered with
    /// what the kind computes to, whether or not it generates a box.
    fn drive_demanded_pseudo_record(
        &mut self,
        node: StyleNodeID,
        kind: u8,
        answer: PublishedMatchAnswer,
        read_only: bool,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) -> Drive<RecordDemandAnswer> {
        self.publish_demanded_answer(node, answer, read_only, counters);
        // A style update removes the backdrop of an element outside the top layer, which a
        // record installed for a read would outlive.
        if kind == pseudo_kind::BACKDROP
            && !read_only
            && self.retained.computed_group_sets.adjustment_facts(node)
                & bridge::element_adjustment_fact::RENDERED_IN_TOP_LAYER
                == 0
        {
            return Err(Unanswered::Refused);
        }
        let element = self
            .retained
            .computed_group_sets
            .assigned_style_record(node)
            .or_refused()?;
        let kinds_with_rules = self.retained.pseudo_style_mask(node).or_refused()?;
        let element_is_list_item = self
            .retained
            .computed_group_sets
            .style_record_view(element.raw())
            .and_then(|view| unsafe { view.longhand_table.as_ref() })
            .is_some_and(|table| table.display_is_list_item());
        let generated = kinds_with_rules & (1 << kind) != 0 || (kind == pseudo_kind::MARKER && element_is_list_item);
        if !generated && !read_only {
            return Ok(RecordDemandAnswer::Absent);
        }
        if !self.retained.pseudo_winners_are_complete(node) {
            return Err(Unanswered::Refused);
        }
        let settlement = if read_only {
            PseudoSettlement::Computed(kind)
        } else {
            PseudoSettlement::Read(kind)
        };
        let generation = self.retained.winner_groups.generation();
        let mut suspended_memory = MemoryLease::new(MemoryCategory::BatchScratch);
        loop {
            match self.retained.settle_pseudo_records(
                node,
                Some(element),
                None,
                element,
                generation,
                settlement,
                scratch,
                counters,
            ) {
                Ok(()) => break,
                Err(Unanswered::Suspended(Suspension::Font)) => {
                    let request = scratch.font_drive.take_suspended_request();
                    suspended_memory.resize_required_to(&mut self.retained.memory, scratch.font_drive.capacity_bytes());
                    self.refill_font_request(node, request, counters);
                }
                Err(unanswered) => {
                    // Whatever was settled before the refusal goes back.
                    self.retained.put_back_engine_computed_records(node, scratch, counters);
                    return Err(unanswered);
                }
            }
        }
        let record = match scratch.pseudo_deltas.iter().rev().find(|delta| delta.kind == kind) {
            Some(delta) => Some(delta.new_style_record).filter(|&record| record != computed::FinalStyleRecordID::NONE),
            None => self.retained.computed_group_sets.pseudo_style_record(node, kind),
        };
        Ok(match record {
            Some(record) => RecordDemandAnswer::Record {
                record: RetriedEngineRecord {
                    style_record: record.raw(),
                    ..RetriedEngineRecord::default()
                },
                uses_substitution: scratch.pseudo_uses_substitution,
            },
            None => RecordDemandAnswer::Absent,
        })
    }

    /// The record of an element no rule reaches, such as one outside the document: the cascade of
    /// its own declarations alone, in cascade order, over the initial values. The element has no
    /// style node, so the drive is keyed by `subject`, the document's, which names no parent and
    /// no siblings. A value that would substitute is C++'s. No row holds the record, which comes
    /// back pinned for the caller.
    pub(in crate::css::style) fn declared_only_record(
        &mut self,
        subject: StyleNodeID,
        facts: u32,
        declarations: &[(ElementDeclarationKind, &crate::css::declaration_block::DeclaredProperty)],
        counters: &mut Counters,
    ) -> Drive<computed::FinalStyleRecordID> {
        use crate::css::style_value::{RetainedStyleValueData, retain_style_value};
        if !self.computes_records() {
            return Err(Unanswered::Refused);
        }
        let retained = |value: &StyleValueData| unsafe {
            RetainedStyleValueData::from_retained_pointer(retain_style_value(value))
        };
        let mut winners: Vec<WinnerDeclaration> = Vec::with_capacity(declarations.len());
        for important in [false, true] {
            for &(kind, declaration) in declarations {
                let property = declaration.property_id;
                if declaration.important != important
                    || property < crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
                {
                    continue;
                }
                let value = match declaration.value.as_ref() {
                    data @ StyleValueData::Shorthand { .. } => match shorthand_longhand_data(property, data) {
                        Some(longhand) => retained(longhand),
                        None => continue,
                    },
                    StyleValueData::Unresolved { .. } | StyleValueData::PendingSubstitution { .. } => {
                        return Err(Unanswered::Refused);
                    }
                    data => retained(data),
                };
                winners.retain(|winner| winner.property != property);
                winners.push(WinnerDeclaration::new(
                    property,
                    important,
                    WinnerValue::Substituted {
                        value: invalid_as_unset(value),
                        source: WinnerSource::Element(kind),
                    },
                ));
            }
        }
        let store = WinnerStore::new(winners);
        let inputs = self.retained.document_style_computation_inputs;
        let subject = DriveSubject {
            target: computed::ComputedStyleTarget::new(subject, u8::MAX),
            parent: None,
            facts: facts & !bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT,
        };
        let mut scratch = EngineComputedRecordScratch::default();
        let mut suspended_memory = MemoryLease::new(MemoryCategory::BatchScratch);
        let driven = loop {
            match self.retained.engine_full_drive(
                subject,
                None,
                &store,
                &inputs,
                &mut scratch.font_drive,
                FontDriveGoal::Complete,
                counters,
            ) {
                Err(Unanswered::Suspended(Suspension::Font)) => {
                    let request = scratch.font_drive.take_suspended_request();
                    suspended_memory.resize_required_to(&mut self.retained.memory, scratch.font_drive.capacity_bytes());
                    self.refill_font_request(subject.target.node(), request, counters);
                }
                driven => break driven?,
            }
        };
        let FullDrive::Driven(DrivenTable {
            table, length, font, ..
        }) = driven
        else {
            unreachable!("only the root-input probe answers with root inputs");
        };
        let font = font.expect("a full drive resolves the font");
        let (record, _) = self.retained.assemble_and_publish_engine_record(
            None,
            None,
            table,
            &length,
            &font,
            0,
            0,
            0,
            None,
            &mut scratch.computability,
            counters,
        )?;
        self.retained.computed_group_sets.pin_style_record(record.raw());
        Ok(record)
    }
}
