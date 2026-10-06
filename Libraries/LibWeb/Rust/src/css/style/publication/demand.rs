/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Answering a read of one element's style, or one of its pseudo-elements', that the host has to
//! make before the next style update takes the document's pending changes.

use super::pseudo::PseudoSettlement;
use super::*;

/// A read the host makes before the next style update: of an element's style, or of one of its
/// pseudo-elements'.
#[derive(Clone, Copy, Debug)]
pub(crate) enum RecordDemand {
    Element(bridge::FfiRecordDemand),
    PseudoElement(bridge::FfiPseudoElementRecordDemand, bridge::FfiDemandedPseudoElement),
}

impl RecordDemand {
    /// Whether the demand leaves the engine as it was, its record only for the host to read.
    pub(crate) fn is_read_only(self) -> bool {
        use bridge::{FfiPseudoElementRecordDemand as Pseudo, FfiRecordDemand as Element};
        matches!(
            self,
            Self::Element(
                Element::ElementRead | Element::ElementReadWithoutInlineStyle | Element::ElementReadAgainstParent
            ) | Self::PseudoElement(Pseudo::ReadOnly, _)
        )
    }
}

/// What a record demand answers.
#[derive(Clone, Copy, Debug)]
pub(crate) enum RecordDemandAnswer {
    /// The element's record with the pseudo-element records settled beside it, or the
    /// pseudo-element's record, and whether it was computed with a substituted winner.
    Record {
        record: DemandedEngineRecord,
        uses_substitution: bool,
    },
    /// The pseudo-element asked for generates no box. Its rules still declare custom properties
    /// a read sees: the environment they resolve to, or zero where no rule styles it.
    Absent { custom_property_environment: u64 },
}

/// Leave to publish a driven row's winners into the retained groups, outside any matching
/// traversal, as a style update does. Every drive holds it but a read-only demand's: the rows that
/// drive reads are its private traversal's, dropped with everything published to it, while a row
/// it republished would outlive the demand.
#[derive(Clone, Copy)]
pub(in crate::css::style) struct WinnerRepublication(());

impl WinnerRepublication {
    /// The leave a style update's flush holds before it drives anything, which no demand runs.
    pub(in crate::css::style) fn for_flush() -> Self {
        Self(())
    }
}

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
    custom_declaration_reads: Option<u8>,
    container_effects: Option<container_queries::ContainerVerdict>,
    container_verdicts: Option<Vec<PublishedContainerVerdict>>,
    container_gate_unheld: bool,
    tree_counting: Option<u16>,
    rolled_back: Option<u64>,
    element_relative_substitutions: Option<ElementRelativeSubstitutions>,
    pseudo_style_mask: Option<u64>,
}

fn set_contains(set: &mut HashSet<StyleNodeID>, node: StyleNodeID, contains: bool) {
    if contains {
        set.insert(node);
    } else {
        set.remove(&node);
    }
}

fn set_entry<V>(map: &mut HashMap<StyleNodeID, V>, node: StyleNodeID, value: Option<V>) {
    match value {
        Some(value) => {
            map.insert(node, value);
        }
        None => {
            map.remove(&node);
        }
    }
}

impl RetainedState {
    fn save_for_private_demand(&self, node: StyleNodeID) -> PrivateDemandSaves {
        PrivateDemandSaves {
            uses_substitution: self.nodes_with_substituted_records.contains(&node),
            custom_declaration_reads: self.custom_declaration_reads.get(&node).copied(),
            container_effects: self.container_effects_for_host.get(&node).cloned(),
            container_verdicts: self.published_container_verdicts.get(&node).cloned(),
            container_gate_unheld: self.container_gates_unheld.contains(&node),
            tree_counting: self.nodes_with_tree_counting_records.get(&node).copied(),
            rolled_back: self.nodes_with_rolled_back_records.get(&node).copied(),
            element_relative_substitutions: self.nodes_with_element_relative_substitutions.get(&node).copied(),
            pseudo_style_mask: self.computed_group_sets.node_pseudo_style_mask(node),
        }
    }

    /// Put `node` back the way the private demand found it: the records the drive derived for it go
    /// back as an abandoned derivation's do, and so does everything else the drive wrote for it.
    fn restore_after_private_demand(
        &mut self,
        node: StyleNodeID,
        saves: PrivateDemandSaves,
        scratch: &mut EngineComputedRecordScratch,
    ) {
        self.put_back_engine_computed_records(node, scratch);
        set_contains(&mut self.nodes_with_substituted_records, node, saves.uses_substitution);
        set_entry(&mut self.custom_declaration_reads, node, saves.custom_declaration_reads);
        self.container_effects_for_host.set(node, saves.container_effects);
        set_entry(&mut self.published_container_verdicts, node, saves.container_verdicts);
        set_contains(&mut self.container_gates_unheld, node, saves.container_gate_unheld);
        set_entry(&mut self.nodes_with_tree_counting_records, node, saves.tree_counting);
        set_entry(&mut self.nodes_with_rolled_back_records, node, saves.rolled_back);
        set_entry(
            &mut self.nodes_with_element_relative_substitutions,
            node,
            saves.element_relative_substitutions,
        );
        self.computed_group_sets
            .set_node_pseudo_style_mask(node, saves.pseudo_style_mask);
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
    /// is settled against its element's installed record, which nothing may be about to move: no
    /// pending input, and no deferred one of the element's own or, below, of an ancestor's.
    fn record_demand_reads_current_inputs(
        &self,
        node: StyleNodeID,
        host: &HostState,
        read_only: bool,
        pseudo: bool,
    ) -> bool {
        if pseudo
            && (!host.journal.is_empty()
                || host.owes_element_style_input(node)
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

impl StyleEngine {
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
        demand: RecordDemand,
    ) -> Drive<RecordDemandAnswer> {
        use bridge::FfiRecordDemand as Element;
        let read_only = demand.is_read_only();
        let targeted = matches!(
            demand,
            RecordDemand::Element(
                Element::TargetedElement | Element::ElementReadWithoutInlineStyle | Element::ElementReadAgainstParent
            )
        );
        // Only a private read of an element leaves its inline style out: the record it answers is
        // no element's.
        let exclude_inline_style = matches!(demand, RecordDemand::Element(Element::ElementReadWithoutInlineStyle));
        let pseudo = match demand {
            RecordDemand::Element(_) => None,
            RecordDemand::PseudoElement(_, pseudo_element) => Some(pseudo_element as u8),
        };
        if !self
            .retained
            .record_demand_reads_current_inputs(node, &self.host, read_only, pseudo.is_some())
        {
            if pseudo.is_some() {
                self.retained.counters.bump(Counter::PseudoRecordDemandsLeftToHost);
            }
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
        // retries once its ancestors installed, reads its answer after this demand. The node is
        // matched in a traversal that materializes the facts its selectors read as they ask, so a
        // scan past the node's subtree (`:has()`, a sibling or an ancestor) widens the facts to the
        // range it reads and matches again.
        let batch_answers = std::mem::take(&mut self.retained.published_match_answers);
        self.retained.begin_adaptive_cold_matching_batch(node);
        let retained_dispatch = if read_only {
            self.retained.retained_answer_dispatch_for_traversal(true)
        } else {
            None
        };
        let answer = self
            .retained
            .complete_published_match_answer(node, retained_dispatch.as_deref());
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
                        &self.retained.counters,
                    );
                    let index = traversal.pending_published.entries.len() - 1;
                    traversal
                        .answer_effects
                        .note_published(node, index, &mut self.retained.memory);
                }
                Some((saves, batch_answers))
            }
            None => {
                self.retained.end_cold_matching_batch();
                self.retained.published_match_answers = batch_answers;
                None
            }
        };

        // A targeted update installs the record as a flush row does, applying the animation plan it
        // decides and running the transition step it owes.
        let mut scratch = EngineComputedRecordScratch {
            read_only,
            host_applies_animation_plans: matches!(demand, RecordDemand::Element(Element::TargetedElement)),
            ..EngineComputedRecordScratch::default()
        };
        let result = answer.map_err(|_| Unanswered::Refused).and_then(|answer| match pseudo {
            None => self.drive_demanded_record(node, answer, targeted, read_only, &mut scratch),
            Some(kind) => self.drive_demanded_pseudo_record(node, kind, answer, read_only, &mut scratch),
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
            self.retained.restore_after_private_demand(node, saves, &mut scratch);
            self.retained.published_match_answers = batch_answers;
        }
        if pseudo.is_some() && result.is_err() {
            self.retained.counters.bump(Counter::PseudoRecordDemandsLeftToHost);
        }
        result
    }

    /// Publish the answer a demand matched for `node`, as a batch publishes its rows' answers, and
    /// keep the pseudo-elements it has rules for, which a private demand puts back. Whether the
    /// node's winners are complete.
    fn publish_demanded_answer(&mut self, node: StyleNodeID, answer: PublishedMatchAnswer, read_only: bool) -> bool {
        let complete =
            answer.cascade_winners_are_complete || self.cascade_winners_are_complete_but_for_custom_properties(node);
        let pseudo_style_mask = self.retained.answer_pseudo_style_mask(&answer);
        self.retained
            .computed_group_sets
            .set_node_pseudo_style_mask(node, pseudo_style_mask);
        if !read_only {
            self.retained
                .computed_group_sets
                .set_node_answer_incomplete(node, !complete);
            self.retained
                .published_match_answers
                .publish(answer, &mut self.retained.memory, &self.retained.counters);
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
    ) -> Drive<RecordDemandAnswer> {
        let complete = self.publish_demanded_answer(node, answer, read_only);
        let parent_inputs_moved = ParentInputsMoved {
            inherited_style: targeted,
            display: targeted,
        };
        let mut suspended_memory = MemoryLease::new(MemoryCategory::BatchScratch);
        let (_, record) = loop {
            match self.engine_computed_record_delta(node, complete, None, parent_inputs_moved, None, scratch) {
                Err(Unanswered::Suspended(Suspension::Font)) => {
                    let request = scratch.font_drive.take_suspended_request();
                    suspended_memory.resize_required_to(&mut self.retained.memory, scratch.font_drive.capacity_bytes());
                    self.refill_font_request(node, request);
                }
                delta => break delta?,
            }
        };
        let mut answered = DemandedEngineRecord {
            style_record: record.raw(),
            explicitly_inherited_groups: scratch.element_explicitly_inherited_groups,
            ..DemandedEngineRecord::default()
        };
        for delta in &scratch.pseudo_deltas {
            let kind = usize::from(delta.kind);
            if kind < bridge::PSEUDO_RECORD_SLOTS {
                answered.pseudo_records_present |= 1 << kind;
                answered.pseudo_records[kind] = delta.new_style_record.raw();
            }
        }
        if scratch.host_applies_animation_plans {
            self.retained.note_what_installing_owes(node, &mut answered);
        }
        if !read_only {
            self.host.journal.acknowledge_node(node, &mut self.retained.memory);
            self.consume_element_style_input_but_descendants(node);
        }
        Ok(RecordDemandAnswer::Record {
            record: answered,
            uses_substitution: scratch.element_uses_substitution,
        })
    }

    /// Settle the one pseudo-element `kind` of `node` against the element's installed record, as a
    /// style update settles it: absent where it generates no box. A read-only read is answered with
    /// what the kind computes to, whether or not it generates a box, as is one of a kind an element
    /// in the shadow tree backs or a named view transition one, from the element's rules for it.
    fn drive_demanded_pseudo_record(
        &mut self,
        node: StyleNodeID,
        kind: u8,
        answer: PublishedMatchAnswer,
        read_only: bool,
        scratch: &mut EngineComputedRecordScratch,
    ) -> Drive<RecordDemandAnswer> {
        // A kind past the synthetic ones is only ever computed for a read-only read: no style
        // update settles it beside its element.
        if !read_only && u16::from(kind) > bridge::LAST_SYNTHETIC_PSEUDO_ELEMENT_KIND {
            return Err(Unanswered::Refused);
        }
        self.publish_demanded_answer(node, answer, read_only);
        let element = self
            .retained
            .computed_group_sets
            .assigned_style_record(node)
            .or_refused()?;
        let kinds_with_rules = self.retained.pseudo_style_mask_or_rematch(node);
        let element_is_list_item = self
            .retained
            .computed_group_sets
            .style_record_view(element.raw())
            .and_then(|view| unsafe { view.longhand_table.as_ref() })
            .is_some_and(|table| table.display_is_list_item());
        // A highlight pseudo-element without rules of its own inherits its ancestor's.
        let generated = kinds_with_rules & (1 << kind) != 0
            || (kind == pseudo_kind::MARKER && element_is_list_item)
            || (pseudo_kind::is_highlight(kind)
                && self
                    .retained
                    .retained_highlight_inheritance_parent_style_record(node, kind)
                    .is_some());
        if !generated && !read_only {
            return Ok(RecordDemandAnswer::Absent {
                custom_property_environment: 0,
            });
        }
        // Winners published while an ancestor's answer moved are published again from the node's
        // answer, which decides their gated rules over the containers as they stand. A read-only
        // read publishes nothing, and leaves such a read to the host.
        if !self.retained.pseudo_winners_are_complete(node) {
            let republication = scratch.winner_republication().or_refused()?;
            self.retained
                .republish_winners_from_answer(node, republication)
                .or_refused()?;
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
                None,
                scratch,
            ) {
                Ok(()) => break,
                Err(Unanswered::Suspended(Suspension::Font)) => {
                    let request = scratch.font_drive.take_suspended_request();
                    suspended_memory.resize_required_to(&mut self.retained.memory, scratch.font_drive.capacity_bytes());
                    self.refill_font_request(node, request);
                }
                Err(unanswered) => {
                    // Whatever was settled before the refusal goes back.
                    self.retained.put_back_engine_computed_records(node, scratch);
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
                record: DemandedEngineRecord {
                    style_record: record.raw(),
                    ..DemandedEngineRecord::default()
                },
                uses_substitution: scratch.pseudo_uses_substitution,
            },
            None => RecordDemandAnswer::Absent {
                custom_property_environment: scratch.boxless_read_environment,
            },
        })
    }

    /// The record of an element no rule reaches, such as one outside the document: the cascade of
    /// its own declarations alone, in cascade order, over the initial values. Its custom
    /// declarations resolve over no environment, and its other declarations substitute under what
    /// they resolve to. The element has no style node, so the drive is keyed by `subject`, the
    /// document's, which names no parent and no siblings. No row holds the record, which comes
    /// back pinned for the caller.
    pub(in crate::css::style) fn declared_only_record(
        &mut self,
        subject: StyleNodeID,
        facts: u32,
        declarations: &[(ElementDeclarationKind, &crate::css::declaration_block::DeclaredProperty)],
        custom_declarations: &[(CustomDeclaration, RetainedStyleValueData)],
    ) -> Drive<computed::FinalStyleRecordID> {
        if !self.computes_records() {
            return Err(Unanswered::Refused);
        }
        let inputs = self.retained.document_style_computation_inputs;
        let cascaded = || {
            custom_declarations
                .iter()
                .map(|(declared, written)| (*declared, written.clone_retained()))
                .collect::<Vec<_>>()
        };
        let registered_declarations = self
            .retained
            .declarations_name_a_registered_custom_property(custom_declarations, &inputs);
        let mut environment =
            self.retained
                .engine_custom_property_environment_over(subject, None, cascaded(), 0, &inputs, None)?;
        let mut store = self
            .retained
            .declared_only_winners(subject, declarations, environment)?;
        let drive_subject = DriveSubject {
            target: computed::ComputedStyleTarget::new(subject, u8::MAX),
            parent: None,
            facts: facts & !bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT,
        };
        let mut scratch = EngineComputedRecordScratch::default();
        let mut suspended_memory = MemoryLease::new(MemoryCategory::BatchScratch);
        let mut awaits_registered_context = registered_declarations;
        let driven = loop {
            match self.retained.engine_full_drive(
                drive_subject,
                None,
                &store,
                &inputs,
                &mut scratch.font_drive,
                FontDriveGoal::Complete,
                awaits_registered_context,
            ) {
                Err(Unanswered::Suspended(Suspension::Font)) => {
                    let request = scratch.font_drive.take_suspended_request();
                    suspended_memory.resize_required_to(&mut self.retained.memory, scratch.font_drive.capacity_bytes());
                    self.refill_font_request(subject, request);
                }
                // The registered custom properties compute against the font the drive settled,
                // and the declarations substitute what they computed to.
                Ok(FullDrive::AwaitsRegisteredContext(registered)) => {
                    environment = self.retained.engine_custom_property_environment_over(
                        subject,
                        None,
                        cascaded(),
                        0,
                        &inputs,
                        Some(&registered),
                    )?;
                    store = self
                        .retained
                        .declared_only_winners(subject, declarations, environment)?;
                    awaits_registered_context = false;
                }
                driven => break driven?,
            }
        };
        let FullDrive::Driven(DrivenTable {
            table, length, font, ..
        }) = driven
        else {
            unreachable!("a complete drive that waits for no registered context answers with its table");
        };
        let font = font.expect("a full drive resolves the font");
        let (record, _) = self.retained.assemble_and_publish_engine_record(
            None,
            None,
            table,
            &length,
            &font,
            environment,
            0,
            0,
            None,
            &mut scratch.computability,
        )?;
        self.retained.computed_group_sets.pin_style_record(record.raw());
        Ok(record)
    }
}

impl RetainedState {
    /// The winners of an element no rule reaches: its declarations in cascade order, normal before
    /// important, a later one replacing an earlier one for the same property. A value with `var()`
    /// references substitutes under `environment`, and one invalid at computed-value time is
    /// unset. A substituted `revert` leaves the property undeclared, and a substituted
    /// `revert-layer` leaves the earlier declaration standing, as the presentational hints sit
    /// below the inline style.
    fn declared_only_winners(
        &mut self,
        subject: StyleNodeID,
        declarations: &[(ElementDeclarationKind, &crate::css::declaration_block::DeclaredProperty)],
        environment: u64,
    ) -> Drive<WinnerStore> {
        use crate::css::style_value::retain_style_value;
        let retained = |value: &StyleValueData| unsafe {
            RetainedStyleValueData::from_retained_pointer(retain_style_value(value))
        };
        let mut style_query = None;
        let mut functions = None;
        let mut winners: Vec<WinnerDeclaration> = Vec::with_capacity(declarations.len());
        for important in [false, true] {
            for &(kind, declaration) in declarations {
                let property = declaration.property_id;
                if declaration.important != important
                    || property < crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
                {
                    continue;
                }
                // A longhand pending its shorthand's substitution takes its part of the
                // substituted shorthand, which the same declarations hold.
                let (substituted_property, written) = match declaration.value.as_ref() {
                    data @ StyleValueData::Shorthand { .. } => match shorthand_longhand_data(property, data) {
                        Some(longhand) => (None, retained(longhand)),
                        None => continue,
                    },
                    data @ StyleValueData::Unresolved { .. } => (Some(property), retained(data)),
                    StyleValueData::PendingSubstitution {
                        original_shorthand_value,
                    } => {
                        let shorthand = declarations.iter().find(|(_, shorthand)| {
                            std::ptr::eq(
                                std::sync::Arc::as_ptr(&shorthand.value),
                                original_shorthand_value.pointer().cast(),
                            )
                        });
                        match shorthand {
                            Some((_, shorthand)) => {
                                (Some(shorthand.property_id), original_shorthand_value.clone_retained())
                            }
                            None => (None, unset_value()),
                        }
                    }
                    data => (None, retained(data)),
                };
                let value = match substituted_property {
                    None => written,
                    Some(substituted_property) => {
                        let calls_functions = custom_property_cascade::value_calls_custom_functions(written.data());
                        if calls_functions && functions.is_none() {
                            functions = Some(self.prepare_custom_functions(subject, None).or_refused()?);
                        }
                        if style_query.is_none()
                            && (calls_functions || custom_property_cascade::value_reads_conditions(written.data()))
                        {
                            style_query =
                                Some(self.style_query_inputs(computed::ComputedStyleTarget::new(subject, u8::MAX)));
                        }
                        let inputs = custom_property_cascade::SubstitutionInputs {
                            document: &self.document_style_computation_inputs,
                            media: &self.document_media,
                            environment: custom_property_cascade::SubstitutionEnvironment {
                                own: environment,
                                inherited: 0,
                            },
                            attributes: None,
                            style_query: style_query.as_ref().and_then(Option::as_ref),
                            style_query_references: None,
                            functions: functions.as_ref(),
                        };
                        let substituted = Self::substitute_written_value(
                            &mut self.custom_property_environments,
                            &inputs,
                            substituted_property,
                            written,
                            &self.counters,
                        )?;
                        let substituted = match substituted_property == property {
                            true => invalid_as_unset(substituted),
                            false => match substituted.data() {
                                StyleValueData::GuaranteedInvalid => unset_value(),
                                _ => expanded_longhand_value(substituted_property, property, &substituted)
                                    .unwrap_or_else(unset_value),
                            },
                        };
                        match super::program_updates::declaration_operator(substituted.data()) {
                            CascadeOperator::Revert => {
                                winners.retain(|winner| winner.property != property);
                                continue;
                            }
                            CascadeOperator::RevertLayer => continue,
                            _ => substituted,
                        }
                    }
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
        Ok(WinnerStore::new(winners))
    }
}

impl RetainedState {
    /// What the host owes an element when it installs the record `settled` names over the one it
    /// holds, as it owes a flush row: the animation plan the record decides, the transition step,
    /// and whether it composes the record before anything inherits from it.
    fn note_what_installing_owes(&self, node: StyleNodeID, settled: &mut DemandedEngineRecord) {
        let held_style_record = self.held_style_records.get(&node).copied().unwrap_or(0);
        settled.owes_an_animation_plan = self.row_owes_an_animation_plan(node, held_style_record, settled.style_record);
        settled.owes_a_transition_step = self.row_owes_a_transition_step(node);
        settled.composed_by_the_host = self.host_composes_row(node, held_style_record, settled.style_record);
    }
}
