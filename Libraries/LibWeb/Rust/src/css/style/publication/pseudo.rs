/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// Which of an element's pseudo-elements a settlement settles.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum PseudoSettlement {
    /// Every kind the element generates, as a style update settles them.
    Generated,
    /// The one kind a style read asks for, settled as a style update would settle it.
    Read(u8),
    /// The one kind a read-only style read asks for: what it computes to, whether or not it
    /// generates a box, for the read alone.
    Computed(u8),
}

impl PseudoSettlement {
    /// The kinds the settlement goes through, in order: a style update's every synthetic kind, a
    /// read's the one it asks for.
    fn kinds(&self) -> &[u8] {
        match self {
            Self::Generated => &pseudo_kind::SETTLEMENT_ORDER,
            Self::Read(kind) | Self::Computed(kind) => std::slice::from_ref(kind),
        }
    }

    /// Whether the settlement settles `kind`: a style update every kind that generates a box with
    /// the element but the ones the host defers, a read the one it asks for, deferred or not.
    fn selects(self, kind: u8, deferred: u64) -> bool {
        match self {
            Self::Generated => {
                !matches!(kind, pseudo_kind::FIRST_LINE | pseudo_kind::VIEW_TRANSITION) && deferred & (1 << kind) == 0
            }
            Self::Read(selected) | Self::Computed(selected) => selected == kind,
        }
    }
}

impl RetainedState {
    /// The pseudo winner rows a settlement reads, republished from the node's answer wherever one
    /// predates it, is missing, or predates the rules that flipped for its kind: settling then
    /// reads a current row for every kind it settles. The kinds are those the answer has rules for
    /// that `settlement` selects, less, in a style update, a backdrop outside the top layer and a
    /// marker no list item generates, which generate no box whatever their rows say. A row a drive
    /// without the leave to republish would have to refresh leaves the settlement to the host.
    #[allow(clippy::too_many_arguments)]
    fn refresh_pseudo_winner_rows(
        &mut self,
        node: StyleNodeID,
        settlement: PseudoSettlement,
        new_is_list_item: bool,
        old_is_list_item: bool,
        flipped_pseudo_rules: u64,
        republication: Option<WinnerRepublication>,
        counters: &Counters,
    ) -> Drive<()> {
        use pseudo_kind::{AFTER, BACKDROP, BEFORE, MARKER};

        let mut required = self.pseudo_kinds_with_rules(node, settlement, counters)?
            & settlement
                .kinds()
                .iter()
                .filter(|&&kind| settlement.selects(kind, self.deferred_pseudo_elements))
                .fold(0_u64, |kinds, &kind| kinds | (1 << kind));
        if settlement == PseudoSettlement::Generated {
            if self.computed_group_sets.adjustment_facts(node) & bridge::element_adjustment_fact::RENDERED_IN_TOP_LAYER
                == 0
            {
                required &= !(1_u64 << BACKDROP);
            }
            let marker_is_live = new_is_list_item
                || old_is_list_item
                || [BEFORE, AFTER, BACKDROP]
                    .into_iter()
                    .filter_map(|kind| self.computed_group_sets.pseudo_style_record(node, kind))
                    .filter_map(|record| self.computed_group_sets.style_record_view(record.raw()))
                    .filter_map(|view| unsafe { view.longhand_table.as_ref() })
                    .any(|table| table.display_is_list_item());
            if !marker_is_live {
                required &= !(1_u64 << MARKER);
            }
        }
        let program_version = self.program.version();
        for (pseudo, version, _, priority_current) in self.current_winner_groups().pseudo_states(node) {
            let kind = pseudo.kind.0;
            if kind >= 64 || required & (1_u64 << kind) == 0 {
                continue;
            }
            let unflipped = flipped_pseudo_rules & (1_u64 << kind) != 0
                && self.current_winner_groups().pseudo_row_stamp(node, pseudo) != Some(self.flush_stamp);
            if version == program_version && priority_current && !unflipped {
                required &= !(1_u64 << kind);
            }
        }
        if required == 0 {
            return Ok(());
        }
        let republication = republication.or_refused()?;
        // The element row may already be compared with the record derived from it in this flush:
        // keep it and publish only the pseudo rows. An evicted answer is matched again.
        let republished = self.current_winner_groups().row_stamp(node) == Some(self.flush_stamp)
            && self
                .republish_pseudo_winners_from_retained_answer(node, republication, counters)
                .is_some();
        if !republished {
            let rematched = self.republish_winners_from_answer(node, republication, counters);
            debug_assert!(rematched.is_some(), "a settled node's pseudo winners republish");
        }
        Ok(())
    }

    /// Settle the synthetic pseudo-elements of an element the engine derived a record for, the
    /// way the C++ computation refreshes them after the element's own: each kind the element has
    /// rules for, and the marker a list item generates, is driven against the element's new
    /// record; one that generates no box any more is removed; one whose cascade state did not
    /// move keeps its record. A refusal is a pseudo-element the engine cannot settle.
    /// `old_is_list_item` says whether the element was a list item when the record it held before
    /// is not `old_element_record`: C++ computed the new one over it. `full_drive_reason` is the
    /// element's: a record driven again in full has its pseudo-elements derived afresh too.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn engine_pseudo_records(
        &mut self,
        node: StyleNodeID,
        old_element_record: Option<computed::FinalStyleRecordID>,
        old_is_list_item: Option<bool>,
        new_element_record: computed::FinalStyleRecordID,
        generation: u64,
        full_drive_reason: Option<FullDriveReason>,
        scratch: &mut EngineComputedRecordScratch,
        counters: &Counters,
    ) -> Drive<()> {
        // The records earlier reads of the node's styles were answered with are no style's now.
        self.release_demand_records(node);
        self.settle_pseudo_records(
            node,
            old_element_record,
            old_is_list_item,
            new_element_record,
            generation,
            PseudoSettlement::Generated,
            full_drive_reason,
            scratch,
            counters,
        )
    }

    /// Settle the pseudo-elements `settlement` selects; see `engine_pseudo_records`.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn settle_pseudo_records(
        &mut self,
        node: StyleNodeID,
        old_element_record: Option<computed::FinalStyleRecordID>,
        old_is_list_item: Option<bool>,
        new_element_record: computed::FinalStyleRecordID,
        generation: u64,
        settlement: PseudoSettlement,
        full_drive_reason: Option<FullDriveReason>,
        scratch: &mut EngineComputedRecordScratch,
        counters: &Counters,
    ) -> Drive<()> {
        use pseudo_kind::{BACKDROP, MARKER};

        // Only an element's settled record leads here, which an unhosted engine never computes.
        debug_assert!(self.computes_records());
        let mut inputs = self.document_style_computation_inputs;
        // NB: Root pseudos use the originating record's current font, independently of
        //     the document context used for the root's own remaining properties.
        if self.computed_group_sets.adjustment_facts(node) & bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0 {
            self.root_font_inputs_from_record(new_element_record)
                .or_refused()?
                .apply_to(&mut inputs);
        }
        let in_top_layer = self.computed_group_sets.adjustment_facts(node)
            & bridge::element_adjustment_fact::RENDERED_IN_TOP_LAYER
            != 0;
        // Every installed record has a view. One without, or one holding no table, is read as a
        // list item, so its marker is considered rather than dropped.
        let display_is_list_item = |engine: &Self, record: computed::FinalStyleRecordID| -> bool {
            let view = engine.computed_group_sets.style_record_view(record.raw());
            debug_assert!(view.is_some(), "an installed record has a view");
            view.and_then(|view| unsafe { view.longhand_table.as_ref() })
                .is_none_or(|table| table.display_is_list_item())
        };
        let new_is_list_item = display_is_list_item(self, new_element_record);
        let new_view_dependency_flags = self
            .computed_group_sets
            .style_record_view(new_element_record.raw())
            .map(|view| view.dependency_flags);
        debug_assert!(new_view_dependency_flags.is_some(), "an element record has a view");
        let new_view_dependency_flags = new_view_dependency_flags.unwrap_or(0);
        let old_is_list_item = old_is_list_item
            .unwrap_or_else(|| old_element_record.is_some_and(|record| display_is_list_item(self, record)));
        self.refresh_pseudo_winner_rows(
            node,
            settlement,
            new_is_list_item,
            old_is_list_item,
            scratch.flipped_pseudo_rules,
            scratch.winner_republication(),
            counters,
        )?;
        let program_version = self.program.version();
        // A row still stale after the refresh is one of a kind that generates no box, or one whose
        // rules the cascade could not order, which is checked where the kind is settled.
        let states: SmallVec<[(u16, CascadeStateID); pseudo_kind::SYNTHETIC_COUNT]> = self
            .current_winner_groups()
            .pseudo_states(node)
            .filter(|&(_, version, _, priority_current)| version == program_version && priority_current)
            .map(|(pseudo, _, state, _)| (pseudo.kind.0, state))
            .collect();
        // What a pseudo-element inherits from its element: an element record that kept its
        // inherited groups left them alone.
        let inherited_inputs_unchanged = match old_element_record {
            Some(old) if old == new_element_record => true,
            Some(old) => {
                let old_identity = self.computed_group_sets.style_record_inherited_groups_identity(old);
                let new_identity = self
                    .computed_group_sets
                    .style_record_inherited_groups_identity(new_element_record);
                old_identity.is_some() && old_identity == new_identity
            }
            None => false,
        };
        let facts = self.computed_group_sets.adjustment_facts(node) & PSEUDO_ELEMENT_ADJUSTMENT_FACTS;
        // A read-only read derives its record afresh, as does a reaction that drives its element
        // in full: an element revealed from display:none holds no pseudo-element styles, and a
        // pseudo-element resolves a font cascade and named rules of its own.
        let originating_inputs_unchanged = !matches!(settlement, PseudoSettlement::Computed(_))
            && inherited_inputs_unchanged
            && full_drive_reason.is_none()
            && !scratch.root_font_inputs_changed
            && !scratch.document_environment_moved
            && old_element_record.is_some_and(|old| {
                let Some(old_view) = self.computed_group_sets.style_record_view(old.raw()) else {
                    return false;
                };
                let Some(new_view) = self.computed_group_sets.style_record_view(new_element_record.raw()) else {
                    return false;
                };
                let (Some(old_table), Some(new_table)) = (unsafe { old_view.longhand_table.as_ref() }, unsafe {
                    new_view.longhand_table.as_ref()
                }) else {
                    return false;
                };
                let old_display = crate::css::style_compute::effective_display(old_table, None);
                let new_display = crate::css::style_compute::effective_display(new_table, None);
                old_view.dependency_flags == new_view.dependency_flags
                    && old_display == new_display
                    && !new_display.is_contents()
                    && self
                        .computed_group_sets
                        .style_record_custom_property_environment(old.raw())
                        == self
                            .computed_group_sets
                            .style_record_custom_property_environment(new_element_record.raw())
            });
        // The element's new record names the environment its pseudo-elements resolve against,
        // whether or not the host installed it yet.
        let element_environment = self
            .computed_group_sets
            .style_record_custom_property_environment(new_element_record.raw());
        debug_assert!(element_environment.is_some(), "an element record holds an environment");
        let element_environment = element_environment.unwrap_or(0);
        // The kinds the node's match answer has rules for: a winner row is published for each
        // the engine cascaded itself, and a kind with rules but no row is not decided.
        let kinds_with_rules = self.pseudo_kinds_with_rules(node, settlement, counters)?;
        let mut pseudo_uses_substitution = scratch.pseudo_uses_substitution;
        for (pseudo_index, &kind) in settlement.kinds().iter().enumerate().skip(scratch.next_pseudo) {
            scratch.next_pseudo = pseudo_index + 1;
            if !settlement.selects(kind, self.deferred_pseudo_elements) {
                continue;
            }
            // The backdrop of a node outside the top layer generates no box, whatever its rules;
            // a read of it computes it as its rules say. The host holds the record it installed
            // until it installs the removal.
            if kind == BACKDROP && !in_top_layer && settlement == PseudoSettlement::Generated {
                if let Some(old) = self.computed_group_sets.pseudo_style_record(node, kind) {
                    counters.bump(Counter::EngineComputedPseudoRecords);
                    self.note_engine_computed_pseudo_record(
                        node,
                        kind,
                        old,
                        computed::FinalStyleRecordID::NONE,
                        None,
                        false,
                        0,
                        None,
                        scratch,
                    );
                }
                continue;
            }
            let target = computed::ComputedStyleTarget::new(node, kind);
            let old = self.computed_group_sets.pseudo_style_record(node, kind);
            // A marker is generated for a list item (and refreshed once more for an element that
            // stops being one), whatever rules match ::marker; other kinds are generated by their
            // rules, which the answer names: a winner row outlives the last rule as an empty
            // state, and a rule without declarations is an empty state that generates.
            let implicit = kind == MARKER && (new_is_list_item || old_is_list_item);
            let has_rules = kinds_with_rules & (1 << kind) != 0;
            // A read of a marker computes it, list item or not.
            if kind == MARKER && !implicit && settlement == PseudoSettlement::Generated {
                continue;
            }
            // A highlight pseudo-element with no rules of its own still inherits its ancestor's.
            let highlight_parent_record = pseudo_kind::is_highlight(kind)
                .then(|| self.retained_highlight_inheritance_parent_style_record(node, kind))
                .flatten();
            let state = states
                .iter()
                .find(|&&(row_kind, _)| row_kind == u16::from(kind))
                .map(|&(_, state)| state)
                .filter(|_| has_rules);
            // Rules whose cascade order the row could not settle, as `:host::before` rules from
            // the host's shadow tree are, leave the kind to the host.
            if has_rules && state.is_none() {
                counters.bump(Counter::EngineComputedRecordBailIncompleteWinners);
                return Err(Unanswered::Refused);
            }
            let old_record = old.unwrap_or(computed::FinalStyleRecordID::NONE);
            let remove = |engine: &mut Self, scratch: &mut EngineComputedRecordScratch, counters: &Counters| {
                if old.is_some() {
                    if settlement == PseudoSettlement::Generated {
                        counters.bump(Counter::EngineComputedPseudoRecords);
                    }
                    engine.note_engine_computed_pseudo_record(
                        node,
                        kind,
                        old_record,
                        computed::FinalStyleRecordID::NONE,
                        None,
                        false,
                        0,
                        None,
                        scratch,
                    );
                }
            };
            if !has_rules
                && !implicit
                && highlight_parent_record.is_none()
                && !matches!(settlement, PseudoSettlement::Computed(_))
            {
                remove(self, scratch, counters);
                continue;
            }
            // Reuse only when the originating element preserves every input the pseudo reads,
            // including display transformation and explicit inheritance of non-inherited values.
            // A moved registry reaches a substitution, attributes reach an `attr()` and the
            // element's place among its siblings a tree-counting function, without moving the
            // state.
            // A highlight pseudo-element reads its ancestor's as well, which nothing here proves
            // unchanged.
            if !pseudo_kind::is_highlight(kind)
                && old.is_some()
                && originating_inputs_unchanged
                && !(scratch.viewport_moved && self.record_reads_the_viewport(old_record))
                && !state.is_some_and(|state| self.sibling_position_key(node, state).is_some())
                && (old_element_record == Some(new_element_record)
                    || !state.is_some_and(|state| self.state_explicitly_inherits_non_inherited_property(node, state)))
            {
                let bound = self
                    .computed_group_sets
                    .cascade_state(target)
                    .or_else(|| self.computed_group_sets.pseudo_retained_cascade_state(node, kind));
                // Custom declarations resolve against registrations that move without the state.
                let unchanged = match (state, bound) {
                    (Some(state), Some((bound_generation, bound_state))) => {
                        bound_generation == generation
                            && self.winner_groups.custom_declarations_of(state) == Default::default()
                            && !(self.custom_property_registrations_changed
                                && self.state_has_substitutions(node, state))
                            && !self.state_reads_beyond_environment(node, state)
                            && !self.records_are_the_elements_alone(node)
                            && self.winner_groups.states_are_semantically_equal(bound_state, state)
                    }
                    (None, None) => true,
                    _ => false,
                } && (kind != MARKER || old_is_list_item == new_is_list_item);
                if unchanged {
                    continue;
                }
            }
            // A pseudo-element's own custom declarations resolve over its element's environment,
            // as an element's resolve over its parent's. A registered name declared here computes
            // provisionally against the element's font until the drive settles the pseudo-element's
            // own.
            let has_registered_declarations = self.declares_registered_custom_property(node, Some(kind), &inputs);
            let mut environment = self.engine_custom_property_environment_of(
                node,
                Some(kind),
                element_environment,
                &inputs,
                None,
                counters,
            )?;
            // A store substituting `attr()` or `inherit()` holds the element's attributes or its
            // parent's values, which no other element shares.
            let element_alone = state.is_some_and(|state| self.state_reads_beyond_environment(node, state));
            let mut store = match state {
                Some(state) => match scratch
                    .pseudo_stores
                    .get(&(kind, state, environment))
                    .filter(|_| !element_alone)
                {
                    Some(store) => store.clone(),
                    None => {
                        let mut substituted = false;
                        let store = std::sync::Arc::new(self.cascaded_store_for_state(
                            node,
                            state,
                            Some(kind),
                            custom_property_cascade::SubstitutionEnvironment {
                                own: environment,
                                inherited: element_environment,
                            },
                            &mut substituted,
                            counters,
                        )?);
                        if substituted {
                            scratch.substituted_states.insert((state, environment));
                        }
                        scratch.store_capacity_bytes += store.capacity_bytes();
                        if !element_alone && !self.records_are_the_elements_alone(node) {
                            scratch.pseudo_stores.insert((kind, state, environment), store.clone());
                        }
                        store
                    }
                },
                None => std::sync::Arc::new(WinnerStore::default()),
            };
            pseudo_uses_substitution |=
                state.is_some_and(|state| scratch.substituted_states.contains(&(state, environment)));
            if !matches!(settlement, PseudoSettlement::Computed(_))
                && pseudo_content_generates_nothing(&store.view(self), kind)
            {
                // A read of a kind its rules style without a box still reads the custom properties
                // they declare.
                if matches!(settlement, PseudoSettlement::Read(_)) {
                    scratch.boxless_read_environment = environment;
                }
                remove(self, scratch, counters);
                continue;
            }
            // What the record is derived from: the element's inherited style, display and
            // environment, and the element's record itself only when the state inherits a
            // non-inherited property from it. A record whose winners read beyond its environment
            // is the originating element's alone, as is one declaring registered custom
            // properties, which its own font decides.
            let key = self
                .computed_group_sets
                .node_inherited_groups_identity(node)
                .zip(self.box_type_parent_display(node))
                .filter(|_| !element_alone && !has_registered_declarations)
                .map(|(inherited_groups, parent_display)| PseudoCohortKey {
                    monospace_recascaded_font_size: state.map_or(0, |state| self.monospace_cohort_key(target, state)),
                    parent_record: if pseudo_kind::is_highlight(kind)
                        || kind == BACKDROP
                        || state.is_some_and(|state| self.state_explicitly_inherits_non_inherited_property(node, state))
                    {
                        new_element_record.raw()
                    } else {
                        0
                    },
                    highlight_parent_record: highlight_parent_record.map_or(0, computed::FinalStyleRecordID::raw),
                    inherited_groups,
                    parent_display,
                    dependency_flags: new_view_dependency_flags,
                    environment,
                    kind,
                    generation,
                    state,
                    facts,
                    font_environment_generation: inputs.font_environment_generation,
                    sibling_position: state.and_then(|state| self.sibling_position_key(node, state)),
                    root_font_inputs: RootFontInputs::from_document(&inputs),
                });
            let cascade_state = state.map(|state| (generation, state));
            let own_groups = state.map_or(0, |state| self.state_owned_inherited_groups(state));
            let derived_under_element = |engine: &Self, record: computed::FinalStyleRecordID| {
                engine.computed_group_sets.final_style_record_is_live(record.raw())
                    && engine.record_answers_counter_styles_for(record, node)
                    && engine
                        .computed_group_sets
                        .style_record_inherits_from_node(record.raw(), node, own_groups)
            };
            let shared = key.and_then(|key| {
                scratch
                    .pseudo_cohorts
                    .get(&key)
                    .copied()
                    .filter(|&record| derived_under_element(self, record))
                    .or_else(|| {
                        let record = *self.engine_pseudo_record_cache.get(&key)?;
                        derived_under_element(self, record).then_some(record)
                    })
            });
            // A composition laid over the record the pseudo-element held gives way to the record
            // derived beneath it, over which the host samples the pseudo-element's animations once
            // it installs it.
            let (new_style_record, longhand_evaluations, detached_composition) = match shared {
                Some(record) => {
                    if let Some(cascade_state) = cascade_state {
                        self.computed_group_sets
                            .set_pending_cascade_state(target, cascade_state);
                    }
                    let detached_composition = self.computed_group_sets.detach_composition_of(target);
                    let publication = self.assign_shared_style_record(
                        target,
                        record.raw(),
                        computed::ENGINE_INHERITED_GROUP_COUNT,
                        false,
                        counters,
                    );
                    counters.bump(Counter::EngineComputedRecordCohortHits);
                    (publication.style_record_identity, 0, detached_composition)
                }
                None => {
                    let subject = DriveSubject {
                        target,
                        parent: Some(node),
                        facts,
                    };
                    let mut driven = self.engine_full_drive(
                        subject,
                        None,
                        &store,
                        &inputs,
                        &mut scratch.font_drive,
                        FontDriveGoal::Complete,
                        has_registered_declarations,
                        counters,
                    );
                    // The registered custom properties compute against the font the drive
                    // settled, and the winners substitute what they computed to.
                    if let Ok(FullDrive::AwaitsRegisteredContext(registered)) = driven {
                        let (settled, settled_store, substituted) = self.store_over_registered_context(
                            target,
                            state.or_refused()?,
                            element_environment,
                            |engine, counters| {
                                engine.engine_custom_property_environment_of(
                                    node,
                                    Some(kind),
                                    element_environment,
                                    &inputs,
                                    Some(&registered),
                                    counters,
                                )
                            },
                            scratch,
                            counters,
                        )?;
                        environment = settled;
                        store = std::sync::Arc::new(settled_store);
                        pseudo_uses_substitution |= substituted;
                        driven = self.engine_full_drive(
                            subject,
                            None,
                            &store,
                            &inputs,
                            &mut scratch.font_drive,
                            FontDriveGoal::Complete,
                            false,
                            counters,
                        );
                    }
                    if let Err(Unanswered::Suspended(_)) = driven {
                        scratch.next_pseudo = pseudo_index;
                        scratch.pseudo_uses_substitution = pseudo_uses_substitution;
                    }
                    let FullDrive::Driven(DrivenTable {
                        table,
                        length,
                        longhand_evaluations,
                        font,
                        explicitly_inherited_groups,
                    }) = driven?
                    else {
                        unreachable!("a complete drive that waits for no registered context answers with its table");
                    };
                    // C++ marks the originating element's parent when a pseudo-element explicitly
                    // inherits a non-inherited property, as it does for the element itself.
                    if !pseudo_kind::is_highlight(kind) {
                        scratch.element_explicitly_inherited_groups |= explicitly_inherited_groups;
                    }
                    let font = font.expect("a full drive resolves the font");
                    // A marker renders its list-style-type through the counter style a registry
                    // defines, so its record names its tree scope's registry, as C++ stamps it.
                    let registry = self.table_counter_style_environment_identity(target, &table);
                    let detached_composition = self.computed_group_sets.detach_composition_of(target);
                    let published = self.assemble_and_publish_engine_record(
                        Some(target),
                        Some(new_element_record),
                        table,
                        &length,
                        &font,
                        environment,
                        0,
                        registry,
                        cascade_state,
                        &mut scratch.computability,
                        counters,
                    );
                    let (record, _) = match published {
                        Ok(published) => published,
                        Err(unanswered) => {
                            if let Some(detached) = detached_composition {
                                self.computed_group_sets.reattach_composition_of(target, detached);
                            }
                            return Err(unanswered);
                        }
                    };
                    // A record that is the element's alone answers for no other pseudo-element.
                    if let Some(key) = key.filter(|_| !self.records_are_the_elements_alone(node)) {
                        scratch.pseudo_cohorts.insert(key, record);
                        if let Some(no_resource_contexts) = store.reads_no_resource_contexts(self) {
                            self.remember_pseudo_record(key, record, no_resource_contexts);
                        }
                    }
                    (record, longhand_evaluations, detached_composition)
                }
            };
            // A read's record is the read's, which no style update installs.
            if settlement == PseudoSettlement::Generated {
                counters.bump(Counter::EngineComputedPseudoRecords);
            }
            let reads_sibling_position = store.uses_tree_counting_function(self);
            self.note_engine_computed_pseudo_record(
                node,
                kind,
                old_record,
                new_style_record,
                cascade_state,
                reads_sibling_position,
                longhand_evaluations,
                detached_composition,
                scratch,
            );
        }
        scratch.pseudo_uses_substitution = pseudo_uses_substitution;
        Ok(())
    }

    /// A later pseudo-element alike in what this record is computed from takes it in a later
    /// transaction. The cache is small and bounded.
    fn remember_pseudo_record(
        &mut self,
        key: PseudoCohortKey,
        record: computed::FinalStyleRecordID,
        _: ReadsNoResourceContexts,
    ) {
        if self.engine_pseudo_record_cache.len() >= COLD_RECORD_CACHE_LIMIT {
            self.engine_pseudo_record_cache.clear();
        }
        self.engine_pseudo_record_cache.insert(key, record);
    }

    /// Account for a pseudo-element record the engine settled (a removal when `new_style_record`
    /// is none) and leave its commitment to C++'s acknowledgement of the element. The record reads
    /// the element's place among its siblings as `reads_sibling_position` says, in place of
    /// whatever the record it replaces read. A composition the settlement detached from the record
    /// it replaces stays pinned until then.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn note_engine_computed_pseudo_record(
        &mut self,
        node: StyleNodeID,
        pseudo_kind: u8,
        old_style_record: computed::FinalStyleRecordID,
        new_style_record: computed::FinalStyleRecordID,
        cascade_state: Option<(u64, CascadeStateID)>,
        reads_sibling_position: bool,
        longhand_evaluations: u32,
        detached_composition: Option<computed::DetachedComposition>,
        scratch: &mut EngineComputedRecordScratch,
    ) {
        self.note_sibling_position_reads(node, pseudo_kind, reads_sibling_position);
        // The host records what a pseudo-element's container units read as its element's own.
        if let Some((_, state)) = cascade_state
            && new_style_record != computed::FinalStyleRecordID::NONE
        {
            self.note_container_unit_effects_for_host(node, true, state, old_style_record);
        }
        self.engine_computed_records_pending
            .entry(node)
            .or_default()
            .push(PendingEngineComputedRecord {
                node,
                pseudo_kind,
                old_style_record,
                new_style_record,
                replaced: None,
                cascade_state,
                longhand_evaluations,
                owes_a_transition_step: false,
                detached_composition,
            });
        scratch.pseudo_deltas.push(PseudoRecordDelta {
            kind: pseudo_kind,
            old_style_record,
            new_style_record,
        });
    }

    /// Put a pseudo-element back the way it was before the engine settled it, unless a
    /// publication has moved it on since, with the composition the settlement detached composing
    /// over it again.
    pub(super) fn revert_engine_computed_pseudo_record(
        &mut self,
        pending: PendingEngineComputedRecord,
        counters: &Counters,
    ) {
        let target = computed::ComputedStyleTarget::new(pending.node, pending.pseudo_kind);
        // A removal is applied only on acknowledgement.
        let moved_on = pending.new_style_record == computed::FinalStyleRecordID::NONE
            || self
                .computed_group_sets
                .pseudo_style_record(pending.node, pending.pseudo_kind)
                != Some(pending.new_style_record);
        if moved_on {
            if let Some(detached) = pending.detached_composition {
                self.computed_group_sets.release_detached_composition(detached);
            }
            return;
        }
        self.computed_group_sets.take_pending_cascade_state(target);
        let previous = pending
            .detached_composition
            .as_ref()
            .map_or(pending.old_style_record, |detached| detached.base);
        if previous != computed::FinalStyleRecordID::NONE
            && self.computed_group_sets.final_style_record_is_live(previous.raw())
        {
            self.assign_shared_style_record(
                target,
                previous.raw(),
                computed::ENGINE_INHERITED_GROUP_COUNT,
                false,
                counters,
            );
        } else {
            self.computed_group_sets
                .remove_pseudo(pending.node, pending.pseudo_kind);
        }
        if let Some(detached) = pending.detached_composition {
            self.computed_group_sets.reattach_composition_of(target, detached);
        }
    }

    /// The kinds `settlement` may settle that the node's match answer has rules for: those its
    /// pseudo-style mask names, or a read's kind past the synthetic ones the mask carries where the
    /// answer has rules for it. Refused when the node has no answer to read.
    fn pseudo_kinds_with_rules(
        &mut self,
        node: StyleNodeID,
        settlement: PseudoSettlement,
        counters: &Counters,
    ) -> Drive<u64> {
        match settlement {
            PseudoSettlement::Computed(kind) if u16::from(kind) > bridge::LAST_SYNTHETIC_PSEUDO_ELEMENT_KIND => {
                let target = tree::PseudoElementTarget::new(tree::PseudoElementKind(u16::from(kind)));
                let matches = self
                    .backing_element_rule_matches(node, target, true, counters)
                    .or_refused()?;
                Ok(u64::from(!matches.is_empty()) << kind)
            }
            _ => Ok(self.pseudo_style_mask_or_rematch(node, counters)),
        }
    }

    /// The kinds the node's match answer has rules for, matching the element again when that
    /// answer was evicted. A match that cannot complete for want of a fact generates no
    /// pseudo-element.
    pub(super) fn pseudo_style_mask_or_rematch(&mut self, node: StyleNodeID, counters: &Counters) -> u64 {
        if let Some(mask) = self.pseudo_style_mask(node) {
            return mask;
        }
        match self.match_element_for_cascade(node, counters) {
            Ok(matches) => matches.iter().fold(0, |mask, rule_match| {
                mask | synthetic_pseudo_bit(rule_match.pseudo_element)
            }),
            Err(_) => {
                debug_assert!(false, "an element's match reports missing facts");
                0
            }
        }
    }

    /// The pseudo-element kind an element stands for (the element a `::placeholder` or a slider part
    /// is), whose style is that pseudo-element's, cascaded from the host's rules, and the shadow
    /// host it stands for it on. The host publishes the kind with the element's facts, and an
    /// element in no shadow tree stands for nothing.
    pub(super) fn backed_host_pseudo_element(&self, node: StyleNodeID) -> Option<(u8, StyleNodeID)> {
        let kind = self.computed_group_sets.associated_pseudo_kind(node)?;
        Some((kind, self.tree.shadow_host_of(node)?))
    }

    /// The host's matches for one of its pseudo-elements, such as the one an element stands for:
    /// those the transaction publishes for the host, the ones it holds, or a fresh match of the
    /// host. A backing element's own published answer proves the inventory its style is computed
    /// from, the host's pseudo-element rules included; `None` when it does not.
    fn backing_element_rule_matches(
        &mut self,
        host: StyleNodeID,
        target: tree::PseudoElementTarget,
        backing_answer_is_complete: bool,
        counters: &Counters,
    ) -> Option<Vec<RuleMatch>> {
        if !backing_answer_is_complete {
            return None;
        }
        let for_target = |matches: &[RuleMatch]| -> Vec<RuleMatch> {
            matches
                .iter()
                .filter(|entry| entry.pseudo_element == Some(target))
                .copied()
                .collect()
        };
        if let Some(matches) = self.batch_backing_pseudo_matches.get(&host) {
            return Some(for_target(matches));
        }
        let published = Self::published_answer_lookup(
            &self.published_match_answers,
            self.batch_matching_traversal.as_deref(),
            host,
        )
        .and_then(|(published, answer)| match published.matches_for(answer) {
            Some(matches) => Some(for_target(matches)),
            None => self
                .match_answers
                .answer(answer.cascade_input?)?
                .iter()
                .filter(|entry| {
                    self.programs.get(entry.program).entries()[entry.entry as usize].pseudo_element == Some(target)
                })
                .map(|entry| entry.materialize(host, &self.programs, 0))
                .collect::<Option<Vec<_>>>(),
        });
        if published.is_some() {
            return published;
        }
        match self.retained_match_answer(host) {
            Lookup::Known(answer) => answer
                .iter()
                .filter(|entry| {
                    self.programs.get(entry.program).entries()[entry.entry as usize].pseudo_element == Some(target)
                })
                .map(|entry| entry.materialize(host, &self.programs, 0))
                .collect(),
            _ => Some(for_target(&self.exact_match_answer(host, counters).ok()?)),
        }
    }

    /// The matches for element-backed pseudo-elements in the answer a transaction publishes for a
    /// node, read before that answer is installed. `None` when it has none.
    pub(in crate::css::style) fn batch_backing_pseudo_matches_of(
        &self,
        node: StyleNodeID,
        published: &PublishedMatchAnswers,
        answer: &PublishedMatchAnswer,
    ) -> Option<Vec<RuleMatch>> {
        let is_backed = |pseudo: Option<tree::PseudoElementTarget>| {
            pseudo.is_some_and(|pseudo| {
                (u16::from(bridge::FIRST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND)
                    ..=u16::from(bridge::LAST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND))
                    .contains(&pseudo.kind.0)
            })
        };
        let matches: Vec<RuleMatch> = match published.matches_for(answer) {
            Some(matches) => matches
                .iter()
                .filter(|entry| is_backed(entry.pseudo_element))
                .copied()
                .collect(),
            None => self
                .match_answers
                .answer(answer.cascade_input?)?
                .iter()
                .filter(|entry| {
                    is_backed(self.programs.get(entry.program).entries()[entry.entry as usize].pseudo_element)
                })
                .map(|entry| entry.materialize(node, &self.programs, 0))
                .collect::<Option<_>>()?,
        };
        (!matches.is_empty()).then_some(matches)
    }

    /// The record of an element standing for its shadow host's pseudo-element, the way C++
    /// computes one: the host's rules for that pseudo-element, held to the properties the
    /// pseudo-element supports, cascaded with the element's own declarations, the rules' custom
    /// declarations resolved over the environment of the parent the record inherits from, with the
    /// element's own box adjustments.
    pub(super) fn engine_backing_element_record(
        &mut self,
        node: StyleNodeID,
        (kind, host): (u8, StyleNodeID),
        backing_answer_is_complete: bool,
        scratch: &mut EngineComputedRecordScratch,
        counters: &Counters,
    ) -> Drive<RecordDelta> {
        use bridge::element_adjustment_fact as fact;
        let facts = self.computed_group_sets.adjustment_facts(node);
        // The host samples the element's animations over the record it installs, as it does for
        // any element, and a record demand installs none of its own. A CSS animation needs a plan,
        // which this drive does not decide.
        let animates = facts & fact::HAS_ANIMATIONS != 0;
        if animates
            && (!scratch.host_applies_animation_plans || self.css_defined_animations.node_runs_a_css_animation(node))
        {
            counters.bump(Counter::EngineComputedRecordBailWinnerElement);
            return Err(Unanswered::Refused);
        }
        // A record it already holds is replaced by a full drive. A composition laid over it is
        // the element's own effects, sampled again over the new base.
        let old_record = self.computed_group_sets.assigned_style_record(node);
        if old_record.is_some_and(|old| !animates && self.record_holds_an_animation_overlay(old)) {
            counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return Err(Unanswered::Refused);
        }
        let target = tree::PseudoElementTarget::new(tree::PseudoElementKind(u16::from(kind)));
        // The host's rules for the pseudo-element, cascaded as the element's own with its own
        // declarations, as C++ cascades them for it.
        let Some(mut matches) = self.backing_element_rule_matches(host, target, backing_answer_is_complete, counters)
        else {
            counters.bump(Counter::EngineComputedRecordBailIncompleteWinners);
            return Err(Unanswered::Refused);
        };
        // The pseudo-element's custom declarations cascade from these matches too: a shadow host
        // retains no answer to read them from.
        let Some(custom_declarations) = self.cascade_custom_declarations(host, Some(kind), Some(&matches)) else {
            counters.bump(Counter::EngineComputedRecordBailCustomProperties);
            return Err(Unanswered::Refused);
        };
        for entry in &mut matches {
            entry.node = node;
            entry.pseudo_element = None;
        }
        let mut winners = self.resolved_cascade_winners_for_properties(node, &matches, None, None);
        // A pseudo-element takes from its rules only the properties it supports; the element's
        // own declarations are not held to that. An own declaration a dropped rule winner hid
        // wins among the element's own declarations alone, since no rule declares it for this
        // pseudo-element.
        let declared_properties = self
            .facts
            .element_declared_properties(node, ElementDeclarationKind::InlineStyle);
        let own_declared: Vec<u16> = declared_properties.iter().map(|declared| declared.property).collect();
        let mut hidden_own_declarations = Vec::new();
        winners.retain(|winner| {
            let supported = !matches!(winner.source, WinnerSource::Rule(_))
                || crate::css::property_metadata::pseudo_element_supports_property(kind, winner.property);
            if !supported && own_declared.contains(&winner.property) {
                hidden_own_declarations.push(winner.property);
            }
            supported
        });
        if !hidden_own_declarations.is_empty() {
            hidden_own_declarations.sort_unstable();
            hidden_own_declarations.dedup();
            winners.extend(self.resolved_cascade_winners_for_properties(
                node,
                &[],
                None,
                Some(&hidden_own_declarations),
            ));
            winners.sort_unstable_by_key(|winner| winner.property);
        }
        let state = self.with_cascade_interning_counters(|groups| groups.intern_sorted(&winners, None), counters);
        for property in self.winner_groups.semantic_delta_properties(None, state) {
            if self.first_record_winner_needs_cpp(property) {
                counters.bump(Counter::EngineComputedRecordBailProperty);
                return Err(Unanswered::Refused);
            }
        }
        // A record that replaces one owes the host the transition step as an element's row does,
        // with every winner moved, since the drive takes them all.
        let owes_a_transition_step = match old_record {
            Some(old) => {
                let Some(underlying_style_record) = self.computed_group_sets.underlying_style_record(old) else {
                    counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return Err(Unanswered::Refused);
                };
                let moves_transition_declarations = self
                    .winner_groups
                    .semantic_delta_properties(None, state)
                    .any(super::property_declares_transitions);
                self.decide_transition_step(
                    node,
                    underlying_style_record,
                    moves_transition_declarations,
                    scratch.host_applies_animation_plans,
                    counters,
                )?
            }
            None => false,
        };
        let mut inputs = self.document_style_computation_inputs;
        if let Some((root, root_inputs)) = scratch.root_element_inputs
            && root == node
        {
            root_inputs.apply_to(&mut inputs);
        }
        let subject = self.element_drive_subject(node, counters)?;
        let parent_record = subject
            .parent
            .and_then(|parent| self.computed_group_sets.assigned_style_record(parent));
        let parent_environment = match subject.parent {
            Some(parent) => self.held_custom_property_environment(parent, counters)?,
            None => 0,
        };
        self.note_custom_declaration_reads(node, None, &custom_declarations);
        // A registered name declared here computes provisionally until the drive settles the
        // font, and then again over the same declarations.
        let registered_declarations = self
            .declarations_name_a_registered_custom_property(&custom_declarations, &inputs)
            .then(|| custom_declarations.clone());
        let mut environment = self.engine_custom_property_environment_over(
            host,
            None,
            custom_declarations,
            parent_environment,
            &inputs,
            None,
            counters,
        )?;
        let mut substituted = false;
        let mut store = self.cascaded_store_for_state(
            node,
            state,
            None,
            custom_property_cascade::SubstitutionEnvironment {
                own: environment,
                inherited: parent_environment,
            },
            &mut substituted,
            counters,
        )?;
        let pseudo_styles = self.pseudo_style_mask_or_rematch(node, counters);
        let mut driven = self.engine_full_drive(
            subject,
            None,
            &store,
            &inputs,
            &mut scratch.font_drive,
            FontDriveGoal::Complete,
            registered_declarations.is_some(),
            counters,
        )?;
        if let FullDrive::AwaitsRegisteredContext(registered) = driven
            && let Some(custom_declarations) = registered_declarations
        {
            (environment, store, substituted) = self.store_over_registered_context(
                subject.target,
                state,
                parent_environment,
                |engine, counters| {
                    engine.engine_custom_property_environment_over(
                        host,
                        None,
                        custom_declarations,
                        parent_environment,
                        &inputs,
                        Some(&registered),
                        counters,
                    )
                },
                scratch,
                counters,
            )?;
            driven = self.engine_full_drive(
                subject,
                None,
                &store,
                &inputs,
                &mut scratch.font_drive,
                FontDriveGoal::Complete,
                false,
                counters,
            )?;
        }
        let FullDrive::Driven(DrivenTable {
            table,
            length,
            font,
            explicitly_inherited_groups,
            ..
        }) = driven
        else {
            unreachable!("a complete drive that waits for no registered context answers with its table");
        };
        let font = font.expect("a full drive resolves the font");
        let counter_style_registry =
            self.table_counter_style_environment_identity(computed::ComputedStyleTarget::new(node, u8::MAX), &table);
        // The composition stays pinned for the host's step, which reads it as the before-change
        // style, until the host installs the new base or the derivation is reverted.
        let detached_composition = animates
            .then(|| self.computed_group_sets.detach_composition(node))
            .flatten();
        let replaced = self.computed_group_sets.replaced_columns(node);
        let record = match self.assemble_and_publish_engine_record(
            Some(computed::ComputedStyleTarget::new(node, u8::MAX)),
            parent_record,
            table,
            &length,
            &font,
            environment,
            pseudo_styles,
            counter_style_registry,
            None,
            &mut scratch.computability,
            counters,
        ) {
            Ok((record, _)) => record,
            Err(unanswered) => {
                if let Some(detached) = detached_composition {
                    self.computed_group_sets.reattach_composition(node, detached);
                }
                return Err(unanswered);
            }
        };
        scratch.element_explicitly_inherited_groups = explicitly_inherited_groups;
        scratch.noted_substitution = Some(substituted);
        let reads_sibling_position = store.uses_tree_counting_function(self);
        self.note_sibling_position_reads(node, u8::MAX, reads_sibling_position);
        let old_record = old_record.unwrap_or(computed::FinalStyleRecordID::NONE);
        let owes_a_transition_step = owes_a_transition_step && old_record != record;
        if owes_a_transition_step || detached_composition.is_some() {
            self.engine_computed_records_pending
                .entry(node)
                .or_default()
                .push(PendingEngineComputedRecord {
                    node,
                    pseudo_kind: u8::MAX,
                    old_style_record: old_record,
                    new_style_record: record,
                    replaced: Some(replaced),
                    cascade_state: None,
                    longhand_evaluations: 0,
                    owes_a_transition_step,
                    detached_composition,
                });
        }
        Ok((old_record, record))
    }

    /// Whether the node holds a match answer its pseudo-elements' winners are proven from: the
    /// one this transaction published, or the retained one.
    /// The synthetic pseudo-elements a published answer has rules for, where it can say.
    pub(in crate::css::style) fn answer_pseudo_style_mask(&self, answer: &PublishedMatchAnswer) -> Option<u64> {
        match answer.cascade_input {
            Some(identity) => self.match_answers.synthetic_pseudo_mask(identity),
            None => answer.matches.as_deref().map(|matches| {
                matches.iter().fold(0, |mask, rule_match| {
                    mask | synthetic_pseudo_bit(rule_match.pseudo_element)
                })
            }),
        }
    }

    fn holds_pseudo_match_answer(&self, node: StyleNodeID) -> bool {
        Self::published_answer_lookup(
            &self.published_match_answers,
            self.batch_matching_traversal.as_deref(),
            node,
        )
        .is_some_and(|(published, answer)| published.matches_for(answer).is_some())
            || matches!(self.retained_match_answer(node), Lookup::Known(_))
    }

    pub(super) fn pseudo_style_mask(&self, node: StyleNodeID) -> Option<u64> {
        if let Some(mask) = self.computed_group_sets.node_pseudo_style_mask(node) {
            return Some(mask);
        }
        if let Some((_, answer)) = Self::published_answer_lookup(
            &self.published_match_answers,
            self.batch_matching_traversal.as_deref(),
            node,
        ) && let Some(mask) = self.answer_pseudo_style_mask(answer)
        {
            return Some(mask);
        }
        let identity = self.current_answer_identity(node)?;
        self.match_answers.answer(identity)?;
        self.match_answers.synthetic_pseudo_mask(identity)
    }

    /// One attempt at the synthetic pseudo-elements of an element whose record the host installed:
    /// the engine settles them against that record exactly as it settles them beside one of its
    /// own. A resumed attempt has already brought the winners up to date. A refusal is a value the
    /// engine cannot compute, or a container condition it cannot decide.
    fn settle_pseudo_records_after_host_record_step(
        &mut self,
        node: StyleNodeID,
        old_is_list_item: bool,
        republication: WinnerRepublication,
        scratch: &mut EngineComputedRecordScratch,
        counters: &Counters,
    ) -> Drive<()> {
        // The host installed the element's record before it asked.
        let record = self.computed_group_sets.assigned_style_record(node);
        debug_assert!(
            record.is_some(),
            "the pseudo settle follows the element's installed record"
        );
        let record = record.or_refused()?;
        if !scratch.font_drive.is_pending_for(node)
            && !self.refresh_pseudo_winners_beside_host_record(node, republication, scratch, counters)
        {
            counters.bump(Counter::EngineComputedRecordBailIncompleteWinners);
            return Err(Unanswered::Refused);
        }
        let generation = self.winner_groups.generation();
        if let Err(unanswered) = self.engine_pseudo_records(
            node,
            None,
            Some(old_is_list_item),
            record,
            generation,
            None,
            scratch,
            counters,
        ) {
            if unanswered != Unanswered::Suspended(Suspension::Font) {
                self.put_back_pseudo_records_beside_host_record(node, scratch, counters);
            }
            return Err(unanswered);
        }
        Ok(())
    }

    /// The kinds among `::before` and `::after` with rules for `node` that a settle beside any record of it leaves
    /// alone: it holds no record of the kind, and the kind's current winners declare no `content`, so they generate no
    /// box, and substitute nothing.
    pub(crate) fn inert_pseudo_kinds(&self, node: StyleNodeID) -> u64 {
        use crate::css::property_metadata::property_id as prop;
        use pseudo_kind::{AFTER, BEFORE};

        let program_version = self.program.version();
        let mut inert = 0;
        for (pseudo, version, state, priority_current) in self.current_winner_groups().pseudo_states(node) {
            let Ok(kind) = u8::try_from(pseudo.kind.0) else {
                continue;
            };
            if (kind == BEFORE || kind == AFTER)
                && version == program_version
                && priority_current
                && self.computed_group_sets.pseudo_style_record(node, kind).is_none()
                && self.winner_groups.winner_in_state(state, prop::CONTENT).is_none()
                && self.winner_groups.custom_declarations_of(state) == Default::default()
                && !self.state_has_substitutions(node, state)
            {
                inert |= 1 << kind;
            }
        }
        inert
    }

    /// Bring the winners the pseudo-elements of an element are settled from up to date beside the
    /// record C++ just installed for it, as a style update leaves them beside one of its own.
    /// Whether the engine decides every container condition their rules ask.
    fn refresh_pseudo_winners_beside_host_record(
        &mut self,
        node: StyleNodeID,
        republication: WinnerRepublication,
        scratch: &mut EngineComputedRecordScratch,
        counters: &Counters,
    ) -> bool {
        // What the engine settled for the node in a style update whose record the host computed
        // instead was settled beside a record the host never installed, and goes back.
        self.put_back_pseudo_records_beside_host_record(node, scratch, counters);
        // Every declaration the pseudo-elements' rules make has to be a winner the engine holds, as
        // it has for any record it derives; what the element's own declarations make is in the
        // record C++ computed. Winners that are missing, or were published while an ancestor's
        // answer moved, are published again from the node's answer, which decides their gated
        // rules over the containers as the installed records leave them. So do winners whose
        // verdicts the installed record moved by making the element a container, which the
        // pseudo-elements' conditions ask about first.
        let republished = if !self.holds_pseudo_match_answer(node) || !self.pseudo_winners_are_complete(node) {
            self.republish_winners_from_answer(node, republication, counters)
                .is_some()
        } else {
            !self.container_verdicts_moved(node)
                || self
                    .republish_pseudo_winners_from_retained_answer(node, republication, counters)
                    .is_some()
                || self
                    .republish_winners_from_answer(node, republication, counters)
                    .is_some()
        };
        // Matching the element again only fails for want of facts the host publishes before it
        // styles the element.
        debug_assert!(republished, "an element the host styled matches");
        debug_assert!(
            !republished || self.pseudo_winners_are_complete(node),
            "winners published on their own hold the node's gated rules"
        );
        republished && self.container_verdicts_stand(node, VerdictTargets::ElementAndPseudoElements)
    }

    /// Put back every pseudo-element record settled for `node` that the host has not taken. The
    /// element's own record is the one the host computed, which stays: a record the engine derived
    /// for it is merely not installed.
    fn put_back_pseudo_records_beside_host_record(
        &mut self,
        node: StyleNodeID,
        scratch: &mut EngineComputedRecordScratch,
        counters: &Counters,
    ) {
        for pending in self.engine_computed_records_pending.remove(&node).into_iter().flatten() {
            if pending.pseudo_kind == u8::MAX {
                let target = computed::ComputedStyleTarget::new(node, u8::MAX);
                self.computed_group_sets.take_pending_cascade_state(target);
                if let Some(detached) = pending.detached_composition {
                    self.computed_group_sets.release_detached_composition(detached);
                }
                continue;
            }
            let derived = pending.new_style_record;
            self.revert_engine_computed_pseudo_record(pending, counters);
            scratch.pseudo_cohorts.retain(|_, record| *record != derived);
            self.engine_pseudo_record_cache.retain(|_, record| *record != derived);
        }
        scratch.pseudo_deltas.clear();
        self.settle_computed_memory();
    }

    /// Whether the winners hold every match the node's answer has for a pseudo-element, as
    /// `match_is_complete_but_for_custom_properties` reads a pseudo-element's: custom properties
    /// are resolved into the pseudo-element's own environment.
    pub(super) fn pseudo_winners_are_complete(&self, node: StyleNodeID) -> bool {
        let is_complete = |rule: RuleID| self.match_is_complete_but_for_custom_properties(node, rule);
        if let Some((published, answer)) = Self::published_answer_lookup(
            &self.published_match_answers,
            self.batch_matching_traversal.as_deref(),
            node,
        ) && let Some(matches) = published.matches_for(answer)
        {
            return matches
                .iter()
                .filter(|entry| entry.pseudo_element.is_some())
                .all(|entry| is_complete(entry.rule));
        }
        let Lookup::Known(answer) = self.retained_match_answer(node) else {
            return false;
        };
        answer.iter().all(|rule_match| {
            self.programs.get(rule_match.program).entries()[rule_match.entry as usize]
                .pseudo_element
                .is_none()
                || is_complete(rule_match.rule)
        })
    }
}

/// Whether a pseudo-element's winning `content` generates no box: `none` for every kind, and
/// `normal` (the initial value, so also an absent one) for ::before and ::after.
fn pseudo_content_generates_nothing(store: &impl crate::css::cascaded_properties::CascadedValues, kind: u8) -> bool {
    use crate::css::property_metadata::property_id as prop;
    use crate::css::style_compute::keyword;
    let generated = matches!(kind, pseudo_kind::BEFORE | pseudo_kind::AFTER);
    match store
        .winning_declaration(prop::CONTENT)
        .map(|(value, ..)| unsafe { &*value.cast::<StyleValueData>() })
    {
        None => generated,
        Some(StyleValueData::Keyword { keyword }) => {
            *keyword == keyword::NONE || (*keyword == keyword::NORMAL && generated)
        }
        Some(_) => false,
    }
}

impl StyleEngineState {
    /// Settle the synthetic pseudo-elements of an element whose record the host has just installed
    /// or composed, so the host installs the engine's records for them: their inputs are the
    /// element's record and the published winner states. `old_is_list_item` is whether the element
    /// generated a marker before. Where the engine cannot compute one of them, it refuses them all.
    pub(crate) fn settle_pseudo_records_after_host_record(
        &mut self,
        node: StyleNodeID,
        old_is_list_item: bool,
        counters: &Counters,
    ) -> bridge::FfiSettledPseudoRecords {
        let font_environment_generation = self
            .retained
            .document_style_computation_inputs
            .font_environment_generation;
        if let Some(resolver) = &mut self.retained.font_resolution {
            resolver.prepare(font_environment_generation);
        }
        let mut scratch = EngineComputedRecordScratch::default();
        let mut suspended_memory = MemoryLease::new(MemoryCategory::BatchScratch);
        // The settle runs in the host's style update, which publishes winners as a flush does.
        let republication = WinnerRepublication::for_flush();
        let attempt = loop {
            match self.retained.settle_pseudo_records_after_host_record_step(
                node,
                old_is_list_item,
                republication,
                &mut scratch,
                counters,
            ) {
                Err(Unanswered::Suspended(Suspension::Font)) => {
                    let request = scratch.font_drive.take_suspended_request();
                    suspended_memory.resize_required_to(&mut self.memory, scratch.font_drive.capacity_bytes());
                    self.refill_font_request(node, request, counters);
                }
                attempt => break attempt,
            }
        };
        // The step put back what it settled before it refused.
        let mut settled = bridge::FfiSettledPseudoRecords {
            refused: attempt.is_err(),
            ..Default::default()
        };
        if settled.refused {
            return settled;
        }
        settled.uses_substitution = scratch.pseudo_uses_substitution;
        settled.explicitly_inherited_groups = scratch.element_explicitly_inherited_groups;
        for delta in &scratch.pseudo_deltas {
            let kind = usize::from(delta.kind);
            if kind < bridge::PSEUDO_RECORD_SLOTS {
                settled.pseudo_records_present |= 1 << kind;
                settled.pseudo_records[kind] = delta.new_style_record.raw();
            }
        }
        settled
    }
}

fn synthetic_pseudo_bit(pseudo: Option<tree::PseudoElementTarget>) -> u64 {
    pseudo
        .map(|pseudo| pseudo.kind.0)
        .filter(|&kind| kind <= bridge::LAST_SYNTHETIC_PSEUDO_ELEMENT_KIND)
        .map_or(0, |kind| 1u64 << kind)
}
