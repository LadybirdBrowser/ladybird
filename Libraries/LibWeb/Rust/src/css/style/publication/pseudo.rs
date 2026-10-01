/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

impl RetainedState {
    /// The pseudo winner rows a settlement reads, republished from the node's answer wherever one
    /// predates it, is missing, or predates the rules that flipped for its kind: settling then
    /// reads a current row for every kind it generates. The kinds are those the answer has rules
    /// for that the engine settles, less a deferred kind and a marker no list item generates,
    /// which generate no box whatever their rows say.
    fn refresh_pseudo_winner_rows(
        &mut self,
        node: StyleNodeID,
        new_is_list_item: bool,
        old_is_list_item: bool,
        flipped_pseudo_rules: u64,
        counters: &mut Counters,
    ) {
        use pseudo_kind::{AFTER, BACKDROP, BEFORE, FIRST_LETTER, MARKER};

        let mut required = self.pseudo_style_mask_or_rematch(node, counters)
            & [BEFORE, AFTER, FIRST_LETTER, MARKER]
                .into_iter()
                .fold(0_u64, |kinds, kind| kinds | (1 << kind));
        if let Some(deferred) = self.deferred_pseudo_element {
            required &= !(1_u64 << deferred.0);
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
            return;
        }
        // The element row may already be compared with the record derived from it in this flush:
        // keep it and publish only the pseudo rows. An evicted answer is matched again.
        let republished = self.current_winner_groups().row_stamp(node) == Some(self.flush_stamp)
            && self
                .republish_pseudo_winners_from_retained_answer(node, counters)
                .is_some();
        if !republished {
            let rematched = self.republish_winners_from_answer(node, counters);
            debug_assert!(rematched.is_some(), "a settled node's pseudo winners republish");
        }
    }

    /// Settle the synthetic pseudo-elements of an element the engine derived a record for, the
    /// way the C++ computation refreshes them after the element's own: each kind the element has
    /// rules for, and the marker a list item generates, is driven against the element's new
    /// record; one that generates no box any more is removed; one whose cascade state did not
    /// move keeps its record. A refusal is a pseudo-element the engine cannot settle.
    /// `old_is_list_item` says whether the element was a list item when the record it held before
    /// is not `old_element_record`: C++ computed the new one over it.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn engine_pseudo_records(
        &mut self,
        node: StyleNodeID,
        old_element_record: Option<computed::FinalStyleRecordID>,
        old_is_list_item: Option<bool>,
        new_element_record: computed::FinalStyleRecordID,
        generation: u64,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) -> Drive<()> {
        use pseudo_kind::{AFTER, BACKDROP, BEFORE, FIRST_LETTER, MARKER, SELECTION};

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
        // An element holding a backdrop style is in the top layer: its backdrop is C++'s.
        if self
            .computed_group_sets
            .assigned_pseudo_kinds(node)
            .any(|kind| kind == BACKDROP)
        {
            counters.bump(Counter::EngineComputedRecordBailPseudoBackdrop);
            return Err(Unanswered::Refused);
        }
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
            new_is_list_item,
            old_is_list_item,
            scratch.flipped_pseudo_rules,
            counters,
        );
        let program_version = self.program.version();
        // A row still stale after the refresh is one of a kind that generates no box, or one whose
        // rules the cascade could not order, which is checked where the kind is settled.
        let mut states: [Option<CascadeStateID>; pseudo_kind::SYNTHETIC_COUNT] = [None; pseudo_kind::SYNTHETIC_COUNT];
        for (pseudo, version, state, priority_current) in self.current_winner_groups().pseudo_states(node) {
            if version != program_version || !priority_current {
                continue;
            }
            if let Some(slot) = states.get_mut(usize::from(pseudo.kind.0)) {
                *slot = Some(state);
            }
        }
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
        let originating_inputs_unchanged = inherited_inputs_unchanged
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
        let kinds_with_rules = self.pseudo_style_mask_or_rematch(node, counters);
        let mut pseudo_uses_substitution = scratch.pseudo_uses_substitution;
        for (pseudo_index, kind) in [BEFORE, AFTER, FIRST_LETTER, SELECTION, MARKER]
            .into_iter()
            .enumerate()
            .skip(scratch.next_pseudo)
        {
            scratch.next_pseudo = pseudo_index + 1;
            if self.deferred_pseudo_element == Some(tree::PseudoElementKind(u16::from(kind)))
                || pseudo_kind::is_highlight(usize::from(kind))
            {
                continue;
            }
            let target = computed::ComputedStyleTarget::new(node, kind);
            let old = self.computed_group_sets.pseudo_style_record(node, kind);
            // An installed pseudo-element record has a view; one without holds no composition.
            let old_view = old.and_then(|old| self.computed_group_sets.style_record_view(old.raw()));
            debug_assert!(
                old.is_none() || old_view.is_some(),
                "an installed pseudo-element record has a view"
            );
            if let Some(view) = old_view {
                let transitioning = (unsafe { view.longhand_table.as_ref() })
                    .is_some_and(crate::css::style_compute::has_active_transition_properties);
                if !view.animated_overlay.is_null() || transitioning {
                    counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return Err(Unanswered::Refused);
                }
            }
            // A marker is generated for a list item (and refreshed once more for an element that
            // stops being one), whatever rules match ::marker; other kinds are generated by their
            // rules, which the answer names: a winner row outlives the last rule as an empty
            // state, and a rule without declarations is an empty state that generates.
            let implicit = kind == MARKER && (new_is_list_item || old_is_list_item);
            if kind == MARKER && !implicit {
                continue;
            }
            let has_rules = kinds_with_rules & (1 << kind) != 0;
            let state = states[usize::from(kind)].filter(|_| has_rules);
            // Rules whose cascade order the row could not settle, as `:host::before` rules from
            // the host's shadow tree are, leave the kind to the host.
            if has_rules && state.is_none() {
                counters.bump(Counter::EngineComputedRecordBailIncompleteWinners);
                return Err(Unanswered::Refused);
            }
            let old_record = old.unwrap_or(computed::FinalStyleRecordID::NONE);
            let remove = |engine: &mut Self, scratch: &mut EngineComputedRecordScratch, counters: &mut Counters| {
                if old.is_some() {
                    engine.note_engine_computed_pseudo_record(
                        node,
                        kind,
                        old_record,
                        computed::FinalStyleRecordID::NONE,
                        None,
                        0,
                        scratch,
                        counters,
                    );
                }
            };
            if !has_rules && !implicit {
                remove(self, scratch, counters);
                continue;
            }
            // Reuse only when the originating element preserves every input the pseudo reads,
            // including display transformation and explicit inheritance of non-inherited values.
            // A moved registry reaches a substitution, and attributes reach an `attr()`, without
            // moving the state.
            if old.is_some()
                && originating_inputs_unchanged
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
                            && !self.state_reads_attributes(node, state)
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
            // as an element's resolve over its parent's.
            let environment =
                self.engine_custom_property_environment_of(node, Some(kind), element_environment, &inputs, counters)?;
            // A store substituting `attr()` holds the element's attributes, which no other element
            // shares.
            let reads_attributes = state.is_some_and(|state| self.state_reads_attributes(node, state));
            let store = match state {
                Some(state) => match scratch
                    .pseudo_stores
                    .get(&(kind, state, environment))
                    .filter(|_| !reads_attributes)
                {
                    Some(store) => store.clone(),
                    None => {
                        let mut substituted = false;
                        let store = std::sync::Arc::new(self.cascaded_store_for_state(
                            node,
                            state,
                            Some(kind),
                            environment,
                            &mut substituted,
                            counters,
                        )?);
                        if substituted {
                            scratch.substituted_states.insert((state, environment));
                        }
                        scratch.store_capacity_bytes += store.capacity_bytes();
                        if !reads_attributes {
                            scratch.pseudo_stores.insert((kind, state, environment), store.clone());
                        }
                        store
                    }
                },
                None => std::sync::Arc::new(WinnerStore::default()),
            };
            pseudo_uses_substitution |=
                state.is_some_and(|state| scratch.substituted_states.contains(&(state, environment)));
            if pseudo_content_generates_nothing(&store.view(self), kind) {
                remove(self, scratch, counters);
                continue;
            }
            // What the record is derived from: the element's inherited style, display and
            // environment, and the element's record itself only when the state inherits a
            // non-inherited property from it. A record whose winners read the originating
            // element's attributes is that element's alone.
            let key = self
                .computed_group_sets
                .node_inherited_groups_identity(node)
                .zip(self.box_type_parent_display(node))
                .filter(|_| !reads_attributes)
                .map(|(inherited_groups, parent_display)| PseudoCohortKey {
                    monospace_recascaded_font_size: state.map_or(0, |state| self.monospace_cohort_key(target, state)),
                    parent_record: if state
                        .is_some_and(|state| self.state_explicitly_inherits_non_inherited_property(node, state))
                    {
                        new_element_record.raw()
                    } else {
                        0
                    },
                    inherited_groups,
                    parent_display,
                    dependency_flags: new_view_dependency_flags,
                    environment,
                    kind,
                    generation,
                    state,
                    facts,
                    font_environment_generation: inputs.font_environment_generation,
                    root_font_inputs: RootFontInputs::from_document(&inputs),
                });
            let cascade_state = state.map(|state| (generation, state));
            let own_groups = state.map_or(0, |state| self.state_owned_inherited_groups(state));
            let derived_under_element = |engine: &Self, record: computed::FinalStyleRecordID| {
                engine.computed_group_sets.final_style_record_is_live(record.raw())
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
            let (new_style_record, longhand_evaluations) = match shared {
                Some(record) => {
                    if let Some(cascade_state) = cascade_state {
                        self.computed_group_sets
                            .set_pending_cascade_state(target, cascade_state);
                    }
                    let publication = self.assign_shared_style_record(
                        target,
                        record.raw(),
                        computed::ENGINE_INHERITED_GROUP_COUNT,
                        false,
                        counters,
                    );
                    counters.bump(Counter::EngineComputedRecordCohortHits);
                    (publication.style_record_identity, 0)
                }
                None => {
                    let subject = DriveSubject {
                        target,
                        parent: Some(node),
                        facts,
                    };
                    let driven = self.engine_full_drive(
                        subject,
                        None,
                        &store,
                        &inputs,
                        &mut scratch.font_drive,
                        FontDriveGoal::Complete,
                        counters,
                    );
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
                        unreachable!("only the root-input probe answers with root inputs");
                    };
                    // C++ marks the originating element's parent when a pseudo-element explicitly
                    // inherits a non-inherited property, as it does for the element itself.
                    if kind != SELECTION {
                        scratch.element_explicitly_inherited_groups |= explicitly_inherited_groups;
                    }
                    let font = font.expect("a full drive resolves the font");
                    // A marker renders its list-style-type through the counter style a registry
                    // defines, so C++ stamps the marker's record with its tree scope's registry,
                    // and a new registry makes a new record. An @counter-style rule joining or
                    // leaving the program, by its sheet's activation or attachment too, closes the
                    // flush's named-rule gate to every node whose records name a registry, and C++
                    // computes them: the registry the marker's own record names is the current one
                    // whenever the engine settles the element. It is the only one the engine can
                    // name.
                    let registry = if kind == MARKER && table_names_a_counter_style(&table) {
                        match old.and_then(|old| self.computed_group_sets.style_record_view(old.raw())) {
                            Some(view) if view.counter_style_environment_identity != 0 => {
                                view.counter_style_environment_identity
                            }
                            _ => {
                                counters.bump(Counter::EngineComputedRecordBailProperty);
                                return Err(Unanswered::Refused);
                            }
                        }
                    } else {
                        0
                    };
                    let (record, _) = self.assemble_and_publish_engine_record(
                        target,
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
                    )?;
                    // Another element's marker may sit in another tree scope, so a record naming a
                    // registry answers for this one alone.
                    if let Some(key) = key.filter(|_| registry == 0) {
                        scratch.pseudo_cohorts.insert(key, record);
                        if self.engine_pseudo_record_cache.len() >= COLD_RECORD_CACHE_LIMIT {
                            self.engine_pseudo_record_cache.clear();
                        }
                        self.engine_pseudo_record_cache.insert(key, record);
                    }
                    (record, longhand_evaluations)
                }
            };
            self.note_engine_computed_pseudo_record(
                node,
                kind,
                old_record,
                new_style_record,
                cascade_state,
                longhand_evaluations,
                scratch,
                counters,
            );
        }
        scratch.pseudo_uses_substitution = pseudo_uses_substitution;
        Ok(())
    }

    /// Account for a pseudo-element record the engine settled (a removal when `new_style_record`
    /// is none) and leave its commitment to C++'s acknowledgement of the element.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn note_engine_computed_pseudo_record(
        &mut self,
        node: StyleNodeID,
        pseudo_kind: u8,
        old_style_record: computed::FinalStyleRecordID,
        new_style_record: computed::FinalStyleRecordID,
        cascade_state: Option<(u64, CascadeStateID)>,
        longhand_evaluations: u32,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) {
        counters.bump(Counter::EngineComputedPseudoRecords);
        self.engine_computed_records_pending
            .entry(node)
            .or_default()
            .push(PendingEngineComputedRecord {
                node,
                pseudo_kind,
                old_style_record,
                new_style_record,
                cascade_state,
                longhand_evaluations,
            });
        scratch.pseudo_deltas.push(PseudoRecordDelta {
            kind: pseudo_kind,
            old_style_record,
            new_style_record,
        });
    }

    /// Put a pseudo-element back the way it was before the engine settled it, unless a
    /// publication has moved it on since.
    pub(super) fn revert_engine_computed_pseudo_record(
        &mut self,
        pending: &PendingEngineComputedRecord,
        counters: &mut Counters,
    ) {
        // A removal is applied only on acknowledgement.
        if pending.new_style_record == computed::FinalStyleRecordID::NONE {
            return;
        }
        let target = computed::ComputedStyleTarget::new(pending.node, pending.pseudo_kind);
        if self
            .computed_group_sets
            .pseudo_style_record(pending.node, pending.pseudo_kind)
            != Some(pending.new_style_record)
        {
            return;
        }
        self.computed_group_sets.take_pending_cascade_state(target);
        if pending.old_style_record != computed::FinalStyleRecordID::NONE
            && self
                .computed_group_sets
                .final_style_record_is_live(pending.old_style_record.raw())
        {
            self.assign_shared_style_record(
                target,
                pending.old_style_record.raw(),
                computed::ENGINE_INHERITED_GROUP_COUNT,
                false,
                counters,
            );
        } else {
            self.computed_group_sets
                .remove_pseudo(pending.node, pending.pseudo_kind);
        }
    }

    /// The kinds the node's match answer has rules for, matching the element again when that
    /// answer was evicted. A match that cannot complete for want of a fact generates no
    /// pseudo-element.
    pub(super) fn pseudo_style_mask_or_rematch(&mut self, node: StyleNodeID, counters: &mut Counters) -> u64 {
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

    /// Whether the node holds a match answer its pseudo-elements' winners are proven from: the
    /// one this transaction published, or the retained one.
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
        let bit = synthetic_pseudo_bit;
        if let Some((owner, answer)) = Self::published_answer_lookup(
            &self.published_match_answers,
            self.batch_matching_traversal.as_deref(),
            node,
        ) {
            if let Some(identity) = answer.cascade_input {
                return self.match_answers.synthetic_pseudo_mask(identity);
            }
            if let Some(matches) = owner.matches_for(answer) {
                return Some(
                    matches
                        .iter()
                        .fold(0, |mask, rule_match| mask | bit(rule_match.pseudo_element)),
                );
            }
        }
        let identity = self.current_answer_identity(node)?;
        self.match_answers.answer(identity)?;
        self.match_answers.synthetic_pseudo_mask(identity)
    }

    /// One attempt at the synthetic pseudo-elements of an element whose record C++ computed: the
    /// engine settles them against that record exactly as it settles them beside one of its own.
    /// A resumed attempt has already passed the entry checks.
    fn settle_pseudo_records_after_host_record_step(
        &mut self,
        node: StyleNodeID,
        old_is_list_item: bool,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) -> Drive<computed::FinalStyleRecordID> {
        // The host installed the element's record before it asked.
        let record = self.computed_group_sets.assigned_style_record(node);
        debug_assert!(
            record.is_some(),
            "the pseudo settle follows the element's installed record"
        );
        let record = record.or_refused()?;
        if !scratch.font_drive.is_pending_for(node) {
            // A record the engine derived for the element itself carries its pseudo-elements, and
            // an element standing for its host's pseudo-element is that pseudo-element.
            if self.engine_computed_records_pending.contains_key(&node)
                || self.computed_group_sets.adjustment_facts(node)
                    & bridge::element_adjustment_fact::IS_SHADOW_HOST_PSEUDO_ELEMENT
                    != 0
            {
                return Err(Unanswered::Refused);
            }
            // The pseudo-elements of an element whose animations C++ composes inherit the
            // composed values, which the record does not hold.
            if self.host_composes_style(node) {
                counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                return Err(Unanswered::Refused);
            }
            // Every declaration the pseudo-elements' rules make has to be a winner the engine
            // holds, as it has for any record it derives. What the element's own declarations
            // make is in the record C++ computed. An answer released between the element's
            // installation and this settle is matched again, exactly: that answer publishes the
            // winners and says whether they are complete.
            let winners_are_complete = if self.holds_pseudo_match_answer(node) {
                self.pseudo_winners_are_complete(node)
            } else {
                self.republish_winners_from_answer(node, counters) == Some(true)
            };
            if !winners_are_complete {
                counters.bump(Counter::EngineComputedRecordBailIncompleteWinners);
                return Err(Unanswered::Refused);
            }
        }
        let generation = self.winner_groups.generation();
        if let Err(unanswered) = self.engine_pseudo_records(
            node,
            None,
            Some(old_is_list_item),
            record,
            generation,
            scratch,
            counters,
        ) {
            if unanswered != Unanswered::Suspended(Suspension::Font) {
                // The element's record is C++'s and stays; only what was settled beside it goes.
                for pending in self.engine_computed_records_pending.remove(&node).into_iter().flatten() {
                    let derived = pending.new_style_record;
                    self.revert_engine_computed_pseudo_record(&pending, counters);
                    scratch.pseudo_cohorts.retain(|_, record| *record != derived);
                    self.engine_pseudo_record_cache.retain(|_, record| *record != derived);
                }
                scratch.pseudo_deltas.clear();
                self.settle_computed_memory();
            }
            return Err(unanswered);
        }
        Ok(record)
    }

    /// Whether every rule the node's answer matches for a pseudo-element declares only what the
    /// winner columns hold, and custom properties, which the engine resolves into the
    /// pseudo-element's own environment, with no container query deciding it.
    fn pseudo_winners_are_complete(&self, node: StyleNodeID) -> bool {
        let rule_is_complete = |rule: RuleID| {
            !self.program.rule_is_gated_by_container_query(rule)
                && self.program.declarations_are_complete_but_for_custom_properties(rule)
        };
        if let Some((published, answer)) = Self::published_answer_lookup(
            &self.published_match_answers,
            self.batch_matching_traversal.as_deref(),
            node,
        ) && let Some(matches) = published.matches_for(answer)
        {
            return matches
                .iter()
                .filter(|entry| entry.pseudo_element.is_some())
                .all(|entry| entry.tree_scope == TreeScopeID::DOCUMENT && rule_is_complete(entry.rule));
        }
        let Lookup::Known(answer) = self.retained_match_answer(node) else {
            return false;
        };
        answer.iter().all(|rule_match| {
            self.programs.get(rule_match.program).entries()[rule_match.entry as usize]
                .pseudo_element
                .is_none()
                || (rule_match.tree_scope == TreeScopeID::DOCUMENT && rule_is_complete(rule_match.rule))
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

/// Whether a marker's record names a counter style a registry may define, as C++ decides when it
/// stamps the record with its scope's registry: through a named `list-style-type`. A `content`
/// counter never reaches an engine record.
fn table_names_a_counter_style(table: &ComputedLonghandTable) -> bool {
    let value = table
        .effective_value(None, crate::css::property_metadata::property_id::LIST_STYLE_TYPE, true)
        .value;
    matches!(unsafe { value.cast::<StyleValueData>().as_ref() },
        Some(StyleValueData::CounterStyle { is_symbols, .. }) if !*is_symbols)
}

impl StyleEngineState {
    /// Settle the synthetic pseudo-elements of an element whose record C++ has just computed and
    /// installed, so C++ installs the engine's records for them instead of computing each one:
    /// their inputs are the element's record and the published winner states, all current once
    /// the element's own computation has published its record. `old_is_list_item` is whether the
    /// element generated a marker before. A zero `style_record` leaves the pseudo-elements to C++;
    /// the flag says whether a settled one substituted custom properties.
    pub(crate) fn settle_pseudo_records_after_host_record(
        &mut self,
        node: StyleNodeID,
        old_is_list_item: bool,
        counters: &mut Counters,
    ) -> (RetriedEngineRecord, bool) {
        let font_environment_generation = self
            .retained
            .document_style_computation_inputs
            .font_environment_generation;
        if let Some(resolver) = &mut self.retained.font_resolution {
            resolver.prepare(font_environment_generation);
        }
        let mut scratch = EngineComputedRecordScratch::default();
        let mut suspended_memory = MemoryLease::new(MemoryCategory::BatchScratch);
        let record = loop {
            match self.retained.settle_pseudo_records_after_host_record_step(
                node,
                old_is_list_item,
                &mut scratch,
                counters,
            ) {
                Err(Unanswered::Suspended(Suspension::Font)) => {
                    let request = scratch.font_drive.take_suspended_request();
                    suspended_memory.resize_required_to(&mut self.memory, scratch.font_drive.capacity_bytes());
                    self.refill_font_request(node, request, counters);
                }
                record => break record,
            }
        };
        let mut settled = RetriedEngineRecord::default();
        let Ok(record) = record else {
            return (settled, false);
        };
        settled.style_record = record.raw();
        settled.explicitly_inherited_groups = scratch.element_explicitly_inherited_groups;
        for delta in &scratch.pseudo_deltas {
            let kind = usize::from(delta.kind);
            if kind < bridge::RETRY_PSEUDO_RECORD_SLOTS {
                settled.pseudo_records_present |= 1 << kind;
                settled.pseudo_records[kind] = delta.new_style_record.raw();
            }
        }
        (settled, scratch.pseudo_uses_substitution)
    }
}

fn synthetic_pseudo_bit(pseudo: Option<tree::PseudoElementTarget>) -> u64 {
    pseudo
        .map(|pseudo| pseudo.kind.0)
        .filter(|&kind| kind <= bridge::LAST_SYNTHETIC_PSEUDO_ELEMENT_KIND)
        .map_or(0, |kind| 1u64 << kind)
}
