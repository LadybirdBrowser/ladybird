/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::computed_longhand_table::ComputedLonghandTable;

/// Another element's published style that a first-time computation may build over: the element
/// whose cascade state stands in for the previous one, and the record it must still hold.
#[derive(Clone, Copy, Debug)]
pub(crate) struct ExactCascadeDonor {
    pub node: StyleNodeID,
    pub style_record: u64,
}

pub(super) struct ExactCascadeContext {
    previous: Option<CascadeStateID>,
    lower_bound_state: Option<CascadeStateID>,
    dependency_target: computed::ComputedStyleTarget,
    donor_used: bool,
}

impl StyleEngine {
    pub(super) fn retained_store_supports_property(target: computed::ComputedStyleTarget, property: u16) -> bool {
        if property > crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID
            || (crate::css::property_metadata::property_id::ANIMATION_COMPOSITION
                ..=crate::css::property_metadata::property_id::ANIMATION_TIMING_FUNCTION)
                .contains(&property)
            || (crate::css::property_metadata::property_id::TRANSITION_BEHAVIOR
                ..=crate::css::property_metadata::property_id::TRANSITION_TIMING_FUNCTION)
                .contains(&property)
            || crate::css::property_metadata::property_is_in_logical_group(property)
        {
            return false;
        }
        !target.is_pseudo()
            || (property != crate::css::property_metadata::property_id::CONTENT
                && crate::css::property_metadata::pseudo_element_supports_property(target.pseudo_kind(), property))
    }

    #[allow(dead_code)]
    pub(crate) fn engine_constructed_cascade_store(
        &self,
        target: computed::ComputedStyleTarget,
    ) -> Option<CascadedPropertyStore> {
        let answer = self.published_match_answers.lookup(target.node())?;
        if !answer.cascade_winners_are_complete {
            return None;
        }
        let key = target.pseudo_element_target().map_or_else(
            || WinnerGroupKey::current(target.node(), self.program.version()),
            |pseudo| WinnerGroupKey::current_pseudo(target.node(), pseudo, self.program.version()),
        );
        let Lookup::Known((_, state)) = self.winner_groups.token_for(key) else {
            return None;
        };

        let mut store = CascadedPropertyStore::new();
        for winner in self.winner_groups.winners_in_state(state) {
            let winner = self.winner_groups.resolved_winner(winner)?;
            if !Self::retained_store_supports_property(target, winner.property) {
                return None;
            }
            let Lookup::Known(value) = self.specified_values.retained_value(winner.key.value) else {
                return None;
            };
            if crate::css::style_compute::external_value_dependencies(value.data())
                .may_need_style_sheet_resource_context
            {
                return None;
            }
            store.seed_retained_property(winner.property, value, winner.important, false);
        }
        Some(store)
    }

    /// Derive the record a published-style reaction moves `node` to, when the engine can compute
    /// it exactly: the winners that moved compute in the drive's remaining phase from the values
    /// their declarations were written with, against the record's own font, the document's
    /// computation inputs, and the parent's record. Every node of one cohort - the same old
    /// record moved to the same winner state - derives the same record, so the second and later
    /// members take the first one's answer.
    pub(super) fn engine_computed_record_delta(
        &mut self,
        node: StyleNodeID,
        cascade_winners_are_complete: bool,
        exact_flipped_rules: Option<&[FlippedRule]>,
        parent_inputs_moved: ParentInputsMoved,
        scratch: &mut EngineComputedRecordScratch,
    ) -> Option<(computed::FinalStyleRecordID, computed::FinalStyleRecordID)> {
        scratch.pseudo_deltas.clear();
        scratch.flipped_pseudo_rules.clear();
        scratch.flipped_pseudo_rules.extend(
            exact_flipped_rules
                .into_iter()
                .flatten()
                .filter(|flip| flip.pseudo_kind.is_some()),
        );
        let delta = self.engine_computed_element_record_delta(
            node,
            cascade_winners_are_complete,
            exact_flipped_rules,
            parent_inputs_moved,
            scratch,
        )?;
        // The element's pseudo-elements are settled beside its record, as the C++ computation
        // refreshes them after the element's own; a pseudo-element the engine cannot settle
        // sends the whole element to C++.
        let old_style_record = (delta.0 != computed::FinalStyleRecordID::NONE).then_some(delta.0);
        let generation = self.winner_groups.generation();
        if self
            .engine_pseudo_records(node, old_style_record, delta.1, generation, scratch)
            .is_none()
        {
            self.abandon_engine_computed_record(node, scratch);
            return None;
        }
        Some(delta)
    }

    /// `exact_flipped_rules` are the rules that flipped for the node when the reaction is exactly
    /// those flips and nothing else the record depends on moved. `parent_inputs_moved` says which
    /// of the parent's inputs may have moved under the record.
    fn engine_computed_element_record_delta(
        &mut self,
        node: StyleNodeID,
        cascade_winners_are_complete: bool,
        exact_flipped_rules: Option<&[FlippedRule]>,
        parent_inputs_moved: ParentInputsMoved,
        scratch: &mut EngineComputedRecordScratch,
    ) -> Option<(computed::FinalStyleRecordID, computed::FinalStyleRecordID)> {
        use crate::css::computed_value_types::{
            STYLE_GROUP_INDEX_ANCHOR, STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_SURROUND,
        };
        use crate::css::computed_values::computed_group_dependency_mask;
        use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LONGHAND_WORD_COUNT};

        let target = computed::ComputedStyleTarget::new(node, u8::MAX);
        if !cascade_winners_are_complete {
            self.counters.bump(Counter::EngineComputedRecordBailIncompleteWinners);
            return None;
        }
        // The winners the record was computed from, against the winners the node holds now: the
        // same comparison a C++ publication makes to select what it recomputes.
        let (generation, state) = match self
            .winner_groups
            .token_for(WinnerGroupKey::current(node, self.program.version()))
        {
            Lookup::Known(token) => token,
            Lookup::Missing(gap) => {
                self.counters.bump(match gap {
                    cascade::WinnerGroupGap::MissingNode(_) => Counter::EngineComputedRecordBailWinnerMissingNode,
                    cascade::WinnerGroupGap::StaleProgram { .. } => Counter::EngineComputedRecordBailWinnerStaleProgram,
                    cascade::WinnerGroupGap::StalePriority(_) => Counter::EngineComputedRecordBailWinnerStalePriority,
                });
                return None;
            }
            Lookup::KnownAbsent => {
                self.counters.bump(Counter::EngineComputedRecordBailWinner);
                return None;
            }
        };
        // Presentational hints reach the cascade through the C++ computation, where a changed
        // attribute may have moved one the winner state does not carry yet, and an element's
        // animations compose into its style there.
        if self.computed_group_sets.adjustment_facts(node)
            & (bridge::element_adjustment_fact::HAS_PRESENTATIONAL_HINTS
                | bridge::element_adjustment_fact::HAS_ANIMATIONS)
            != 0
        {
            self.counters.bump(Counter::EngineComputedRecordBailWinnerElement);
            return None;
        }
        let Some(old_style_record) = self.computed_group_sets.assigned_style_record(node) else {
            return self.engine_cold_record(node, (generation, state), scratch);
        };
        // A record C++ computed holds no cascade state; when the reaction moved none of the
        // node's own rules its winners are the ones the record was computed from, and a full
        // drive against the moved parent inputs binds the state.
        let winners_unchanged =
            exact_flipped_rules.is_some_and(|flipped| flipped.iter().all(|flip| flip.pseudo_kind.is_some()));
        let delta = match self.computed_group_sets.cascade_state(target) {
            Some((previous_generation, previous_state)) => {
                if previous_generation != generation {
                    self.counters.bump(Counter::EngineComputedRecordBailStaleCascadeState);
                    return None;
                }
                self.winner_groups.semantic_delta(Some(previous_state), state)
            }
            None if winners_unchanged && parent_inputs_moved.any() => {
                self.winner_groups.semantic_delta(Some(state), state)
            }
            None => {
                self.counters.bump(Counter::EngineComputedRecordBailNoCascadeState);
                return None;
            }
        };
        if delta.is_empty() {
            // The winners the record was computed from are the winners now. When everything else
            // the record was computed from is as it was too - the document environment, the rules
            // that flipped (custom properties are no winners), the parent's inherited style and
            // custom-property environment - the record stands, and the reaction may still move a
            // pseudo-element. The state has to hold the flips: a row this flush published holds
            // the cascade of the node's current answer. Anything else recomputes in C++.
            let flips_are_reflected = exact_flipped_rules.is_some_and(|flipped| {
                !flipped.iter().any(|flip| flip.pseudo_kind.is_none())
                    || self.winner_groups.row_stamp(node) == Some(self.flush_stamp)
            });
            if !flips_are_reflected {
                self.counters.bump(Counter::EngineComputedRecordBailUnchangedWinners);
                return None;
            }
            // The winners stand while the parent's inherited style or display moved under the
            // record: it is driven again in full against the parent as it is now. The record
            // does not say which parent display it was transformed under, and a winner's own
            // value may read the parent (a relative length, an inherit keyword).
            if !parent_inputs_moved.any() {
                if !self.record_inherits_from_current_parent(node, state) {
                    self.counters.bump(Counter::EngineComputedRecordBailUnchangedWinners);
                    return None;
                }
                self.counters.bump(Counter::EngineComputedRecordUnchangedWinners);
                self.counters.bump(Counter::CascadeWinnerDeltaStops);
                self.note_engine_computed_record(node, (old_style_record, old_style_record), (generation, state), 0, 0);
                return Some((old_style_record, old_style_record));
            }
        }
        // A moved font-phase longhand reaches every value the font feeds, so the record is driven
        // through every phase and every group is rebuilt. A moved box-type transformation input
        // takes the same route, as does a record whose parent inputs moved: the transformation
        // and the inheritance are part of the full drive.
        let full_drive = parent_inputs_moved.any()
            || delta.properties().iter().any(|&property| {
                use crate::css::property_metadata::property_id as prop;
                !property_computes_in_remaining_phase(property)
                    || matches!(property, prop::DISPLAY | prop::POSITION | prop::FLOAT)
            });
        let delta_property_count = delta.properties().len() as u64;
        // A full drive reads the parent's record, so alike derivations share only under alike
        // parents.
        let parent_record = if full_drive {
            self.tree
                .flat_tree_parent(node)
                .and_then(|parent| self.computed_group_sets.assigned_style_record(parent))
                .map_or(0, |record| record.raw())
        } else {
            0
        };
        let cohort = (
            old_style_record.raw(),
            state,
            self.computed_group_sets.adjustment_facts(node),
            parent_record,
        );
        if let Some(&new_style_record) = scratch.cohorts.get(&cohort) {
            let delta =
                self.computed_group_sets
                    .assign_engine_computed_record(node, old_style_record, new_style_record)?;
            if delta.0 == delta.1 {
                self.counters.bump(Counter::ComputedWinnerPropagationStops);
            }
            self.note_engine_computed_record(node, delta, (generation, state), delta_property_count, 0);
            self.counters.bump(Counter::EngineComputedRecordCohortHits);
            return Some(delta);
        }
        let Some(inputs) = self.document_style_computation_inputs else {
            self.counters.bump(Counter::EngineComputedRecordBailNoEnvironment);
            return None;
        };

        // The moved properties, the groups they feed, and the drive selection. A moved member of
        // a logical property group takes its counterpart along: which of the pair the other
        // derives from is a cascade decision the drive makes for both.
        let mut groups_to_rebuild = 0_u32;
        let mut selected = [0_u64; LONGHAND_WORD_COUNT];
        let mut select = |property: u16| {
            let index = usize::from(property - FIRST_LONGHAND_PROPERTY_ID);
            selected[index / 64] |= 1 << (index % 64);
        };
        let (writing_mode, direction) = {
            let Some(view) = self.computed_group_sets.style_record_view(old_style_record.raw()) else {
                self.counters.bump(Counter::EngineComputedRecordBailRecord);
                return None;
            };
            let inherited_box = unsafe {
                &*view.payloads[crate::css::computed_value_types::STYLE_GROUP_INDEX_INHERITED_BOX]
                    .cast::<crate::css::computed_values::InheritedBoxValues>()
            };
            (inherited_box.writing_mode, inherited_box.direction)
        };
        for &property in delta.properties() {
            // Animations and transitions start from the C++ computation, and the counter-style
            // environment behind `content` and `list-style-type` is resolved there.
            if property_starts_animation_or_counter_environment(property) {
                self.counters.bump(Counter::EngineComputedRecordBailProperty);
                return None;
            }
            let groups = match computed_group_dependency_mask(property) {
                Some(groups) => groups,
                // A longhand the font resolution selects by feeds no group of its own; the full
                // drive it takes rebuilds every group.
                None if full_drive && font_resolution_selects_by(property) => 0,
                None => {
                    self.counters.bump(Counter::EngineComputedRecordBailProperty);
                    return None;
                }
            };
            groups_to_rebuild |= groups;
            select(property);
            let bits = crate::css::style_compute::table_row_bits(property);
            let counterpart = if bits & crate::css::style_compute::LOGICAL_ALIAS_BIT != 0 {
                crate::css::style_compute::map_logical_alias_to_physical(property, writing_mode, direction)
            } else if bits & crate::css::style_compute::PHYSICAL_TO_LOGICAL_BIT != 0 {
                crate::css::style_compute::map_physical_to_logical_alias(property, writing_mode, direction)
            } else {
                property
            };
            if counterpart != property {
                let Some(groups) = computed_group_dependency_mask(counterpart) else {
                    self.counters.bump(Counter::EngineComputedRecordBailProperty);
                    return None;
                };
                groups_to_rebuild |= groups;
                select(counterpart);
            }
        }
        if groups_to_rebuild & (1 << STYLE_GROUP_INDEX_ANCHOR) != 0 {
            groups_to_rebuild |= 1 << STYLE_GROUP_INDEX_SURROUND;
        }
        // A moved `color` reaches every group holding a value resolved against currentcolor.
        if delta
            .properties()
            .contains(&crate::css::property_metadata::property_id::COLOR)
        {
            let Some(dependencies) = self.computed_group_sets.current_color_dependency_mask(target) else {
                self.counters.bump(Counter::EngineComputedRecordBailRecord);
                return None;
            };
            groups_to_rebuild |= dependencies;
            // The dependents compute again from their specified values, so the table spells them
            // the way a fresh computation does, not the way an inherited-group swap resolved them.
            let Some(dependent_properties) = self.computed_group_sets.current_color_dependency_properties(target)
            else {
                self.counters.bump(Counter::EngineComputedRecordBailRecord);
                return None;
            };
            for (word, &bits) in dependent_properties.iter().enumerate() {
                let mut bits = bits;
                while bits != 0 {
                    let bit = bits.trailing_zeros() as usize;
                    bits &= bits - 1;
                    let property = FIRST_LONGHAND_PROPERTY_ID + (word * 64 + bit) as u16;
                    select(property);
                    if crate::css::style_compute::table_row_bits(property)
                        & crate::css::style_compute::LOGICAL_ALIAS_BIT
                        != 0
                    {
                        select(crate::css::style_compute::map_logical_alias_to_physical(
                            property,
                            writing_mode,
                            direction,
                        ));
                    }
                }
            }
        }
        if full_drive {
            groups_to_rebuild = (1 << crate::css::table_group_builder::group_index::COUNT) - 1;
        } else if groups_to_rebuild & (1 << STYLE_GROUP_INDEX_FONT) != 0 {
            self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
            return None;
        }

        let store = match scratch.stores.get(&state) {
            Some(store) => store.clone(),
            None => {
                let store = std::rc::Rc::new(self.cascaded_store_for_state(node, state, None)?);
                scratch.stores.insert(state, store.clone());
                store
            }
        };
        let (table, length, longhand_evaluations, font) = if full_drive {
            let subject = self.element_drive_subject(node)?;
            self.engine_full_drive(subject, Some(old_style_record), &store, &inputs)?
        } else {
            self.engine_driven_table(node, old_style_record, &store, &selected, &inputs)?
        };
        let parent_in_display_none_subtree = self
            .tree
            .flat_tree_parent(node)
            .and_then(|parent| self.computed_group_sets.assigned_style_record(parent))
            .and_then(|record| self.computed_group_sets.style_record_view(record.raw()))
            .is_some_and(|view| view.dependency_flags & (1 << 2) != 0);
        let Some(assembly) = self.computed_group_sets.replace_engine_computed_table(
            node,
            table,
            groups_to_rebuild,
            &length,
            font.as_ref(),
            parent_in_display_none_subtree,
        ) else {
            self.counters.bump(Counter::EngineComputedRecordBailAssemble);
            return None;
        };
        self.settle_computed_memory();
        self.counters.add(
            Counter::ComputedOutputGroupsCanonicalized,
            u64::from(assembly.canonicalized_groups),
        );
        if assembly.group_set_unchanged {
            self.counters.bump(Counter::ComputedWinnerPropagationStops);
        }
        let delta = assembly.delta;
        self.note_engine_computed_record(
            node,
            delta,
            (generation, state),
            delta_property_count,
            longhand_evaluations,
        );
        scratch.cohorts.insert(cohort, delta.1);
        Some(delta)
    }

    /// Account for a record the engine derived and leave its commitment to C++'s acknowledgement.
    fn note_engine_computed_record(
        &mut self,
        node: StyleNodeID,
        delta: (computed::FinalStyleRecordID, computed::FinalStyleRecordID),
        cascade_state: (u64, CascadeStateID),
        delta_property_count: u64,
        longhand_evaluations: u32,
    ) {
        self.counters
            .add(Counter::CascadeWinnerDeltaProperties, delta_property_count);
        self.counters
            .add(Counter::ComputedWinnerDeltaPropertiesConsumed, delta_property_count);
        self.counters.bump(Counter::EngineComputedRecordDeltas);
        self.engine_computed_records_pending.push(PendingEngineComputedRecord {
            node,
            pseudo_kind: u8::MAX,
            old_style_record: delta.0,
            new_style_record: delta.1,
            cascade_state: Some(cascade_state),
            longhand_evaluations,
        });
    }

    /// C++ installed the record the engine derived for `node`: the winner state it was computed
    /// from becomes the node's cascade state, and the answer counts as consumed.
    pub(crate) fn acknowledge_engine_computed_record(&mut self, node: StyleNodeID) {
        let mut acknowledged = false;
        let mut index = 0;
        while index < self.engine_computed_records_pending.len() {
            if self.engine_computed_records_pending[index].node != node {
                index += 1;
                continue;
            }
            let pending = self.engine_computed_records_pending.swap_remove(index);
            acknowledged = true;
            let target = computed::ComputedStyleTarget::new(node, pending.pseudo_kind);
            self.remove_pending_style_computation_selection(target);
            // A pseudo-element settled as gone is removed now that C++ has cleared its style.
            if pending.pseudo_kind != u8::MAX && pending.new_style_record == computed::FinalStyleRecordID::NONE {
                self.remove_computed_pseudo(node, pending.pseudo_kind);
                continue;
            }
            self.computed_group_sets.take_pending_cascade_state(target);
            if let Some(cascade_state) = pending.cascade_state {
                self.computed_group_sets.bind_cascade_state(target, cascade_state);
            }
            // A pseudo-element's record was computed from this very state, as the retained
            // cascade would have observed had C++ computed it.
            if pending.pseudo_kind != u8::MAX {
                self.computed_group_sets
                    .observe_pseudo_retained_cascade_state(target, pending.cascade_state);
            }
            self.counters.add(
                Counter::EngineComputedLonghandEvaluations,
                u64::from(pending.longhand_evaluations),
            );
        }
        if acknowledged {
            self.published_match_answers.mark_observed(node);
        }
    }

    /// The transaction's outputs are gone: every derived record C++ did not install goes back to
    /// the record the node held, unless a publication has moved the node on since.
    pub(super) fn discard_engine_computed_records(&mut self) {
        for pending in std::mem::take(&mut self.engine_computed_records_pending) {
            if pending.pseudo_kind != u8::MAX {
                self.revert_engine_computed_pseudo_record(&pending);
                continue;
            }
            self.computed_group_sets.revert_engine_computed_record(
                pending.node,
                pending.new_style_record,
                pending.old_style_record,
            );
        }
    }

    /// Derive a node's first record: every winner of its state driven through every phase, every
    /// group built against the parent's payloads, and the record published the way a C++ first
    /// computation publishes it, with the parent's custom-property environment. A node alike in
    /// everything a first record is computed from takes the record an earlier node got, whether
    /// in this flush or one before it.
    fn engine_cold_record(
        &mut self,
        node: StyleNodeID,
        cascade_state: (u64, CascadeStateID),
        scratch: &mut EngineComputedRecordScratch,
    ) -> Option<(computed::FinalStyleRecordID, computed::FinalStyleRecordID)> {
        use crate::css::computed_values::computed_group_dependency_mask;

        let target = computed::ComputedStyleTarget::new(node, u8::MAX);
        let (_, state) = cascade_state;
        let Some(inputs) = self.document_style_computation_inputs else {
            self.counters.bump(Counter::EngineComputedRecordBailNoEnvironment);
            return None;
        };
        let delta = self.winner_groups.semantic_delta(None, state);
        // A longhand the font resolution selects the font by feeds no group of its own: the
        // full drive resolves the font and rebuilds every group from it. The other font-phase
        // longhands without a group carry feature and variation data the resolution does not
        // pass on yet.
        for &property in delta.properties() {
            if property_starts_animation_or_counter_environment(property)
                || (computed_group_dependency_mask(property).is_none() && !font_resolution_selects_by(property))
            {
                self.counters.bump(Counter::EngineComputedRecordBailProperty);
                return None;
            }
        }
        let delta_property_count = delta.properties().len() as u64;
        let facts = self.computed_group_sets.adjustment_facts(node);
        let parent = self.tree.flat_tree_parent(node);
        // Only the document element is styled without a flat-tree parent: it inherits from the
        // initial values.
        if parent.is_none() && facts & bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT == 0 {
            self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
            return None;
        }
        let parent_record = match parent {
            Some(parent) => match self.computed_group_sets.assigned_style_record(parent) {
                Some(parent_record) => Some(parent_record),
                None => {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return None;
                }
            },
            None => None,
        };
        // The state has to be one the engine can compute from before any record is shared under
        // it: a record C++ computed for a per-element value, such as a `random()` draw, is that
        // element's alone.
        let store = match scratch.stores.get(&state) {
            Some(store) => store.clone(),
            None => {
                let store = self.cascaded_store_for_state(node, state, None);
                self.remember_state_admission(cascade_state, store.is_some());
                let store = std::rc::Rc::new(store?);
                scratch.stores.insert(state, store.clone());
                store
            }
        };
        let Some(pseudo_styles) = self.pseudo_style_mask(node) else {
            self.counters.bump(Counter::EngineComputedRecordBailWinner);
            return None;
        };
        let cache_key = parent
            .zip(parent_record)
            .and_then(|(parent, parent_record)| self.cold_record_parent(node, parent, parent_record, state))
            .map(|parent| ColdRecordKey {
                parent,
                generation: cascade_state.0,
                state,
                facts,
                pseudo_styles,
                font_environment_generation: inputs.font_environment_generation,
            });
        // A key names the parent's inherited groups by an identity the record store may have
        // reused since the record was derived, so a hit stands only for a live record that
        // inherits what this parent holds now.
        let own_groups = self.state_owned_inherited_groups(state);
        let derived_under_parent = |engine: &Self, record: ColdRecord| {
            parent.is_some_and(|parent| {
                engine
                    .computed_group_sets
                    .final_style_record_is_live(record.record.raw())
                    && engine.computed_group_sets.style_record_inherits_from_node(
                        record.record.raw(),
                        parent,
                        own_groups,
                    )
            })
        };
        let shared = cache_key.and_then(|cache_key| {
            scratch
                .cold_cohorts
                .get(&cache_key)
                .copied()
                .filter(|&record| derived_under_parent(self, record))
                .map(|record| (record, false))
                .or_else(|| {
                    let record = *self.engine_cold_record_cache.get(&cache_key)?;
                    derived_under_parent(self, record).then_some((record, true))
                })
        });
        if let Some((ColdRecord { record, swap_eligible }, from_cache)) = shared {
            self.computed_group_sets
                .set_pending_cascade_state(target, cascade_state);
            let publication = self.assign_shared_style_record(
                target,
                record.raw(),
                computed::ENGINE_INHERITED_GROUP_COUNT,
                swap_eligible,
            );
            let delta = (computed::FinalStyleRecordID::NONE, publication.style_record_identity);
            self.note_engine_computed_record(node, delta, cascade_state, delta_property_count, 0);
            self.counters.bump(if from_cache {
                Counter::EngineComputedRecordSharedHits
            } else {
                Counter::EngineComputedRecordCohortHits
            });
            return Some(delta);
        }
        let subject = DriveSubject { parent, facts };
        let (table, length, longhand_evaluations, font) = self.engine_full_drive(subject, None, &store, &inputs)?;
        let font = font.expect("a full drive resolves the font");
        // The document element's environment is its own, which is nothing without declarations.
        let Some(environment) = parent.map_or(Some(0), |parent| {
            self.computed_group_sets.custom_property_environment_identity(parent)
        }) else {
            self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
            return None;
        };
        let (new_style_record, swap_eligible) = self.assemble_and_publish_engine_record(
            target,
            parent_record,
            table,
            &length,
            &font,
            environment,
            pseudo_styles,
            Some(cascade_state),
        )?;
        let delta = (computed::FinalStyleRecordID::NONE, new_style_record);
        // The publication itself kept the record for later transactions; alike elements in this
        // one take it from the cohort.
        if let Some(cache_key) = cache_key {
            scratch.cold_cohorts.insert(
                cache_key,
                ColdRecord {
                    record: delta.1,
                    swap_eligible,
                },
            );
        }
        self.note_engine_computed_record(node, delta, cascade_state, delta_property_count, longhand_evaluations);
        Some(delta)
    }

    /// Build a driven table's groups against the parent record's payloads and publish the record
    /// for `target` the way a C++ computation publishes one; the record's swap eligibility comes
    /// back beside its identity.
    #[allow(clippy::too_many_arguments)]
    fn assemble_and_publish_engine_record(
        &mut self,
        target: computed::ComputedStyleTarget,
        parent_record: Option<computed::FinalStyleRecordID>,
        mut table: ComputedLonghandTable,
        length: &crate::css::style_compute::FfiLengthResolutionContext,
        font: &crate::css::table_group_builder::FfiFontGroupBuildInputs,
        environment: u64,
        pseudo_styles: u64,
        cascade_state: Option<(u64, CascadeStateID)>,
    ) -> Option<(computed::FinalStyleRecordID, bool)> {
        use crate::css::computed_value_types::STYLE_GROUP_INDEX_FONT;
        use crate::css::table_group_builder::group_index;

        // The document element's groups build against no parent payloads.
        let (parent_payloads, parent_in_display_none_subtree) = match parent_record {
            Some(parent_record) => {
                let Some(parent_view) = self.computed_group_sets.style_record_view(parent_record.raw()) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return None;
                };
                (
                    parent_view.payloads.to_vec(),
                    parent_view.dependency_flags & (1 << 2) != 0,
                )
            }
            None => (vec![std::ptr::null(); group_index::COUNT], false),
        };
        let Ok(used_color_scheme) = u8::try_from(table.effective_color_scheme()) else {
            self.counters.bump(Counter::EngineComputedRecordBailDrive);
            return None;
        };
        let display_is_none = crate::css::style_compute::effective_display(&table, None).is_none();
        table.set_in_display_none_subtree(parent_in_display_none_subtree || display_is_none);
        table.freeze();
        let swap_eligible = table.property_inheritance_is_standard()
            && !table.display_is_list_item()
            && crate::css::style_compute::active_transition_properties(&table).is_empty();
        let table = table.into_raw_shared();
        let release_table = |table: *const ComputedLonghandTable| unsafe {
            crate::css::computed_longhand_table::rust_computed_longhand_table_release(table.cast_mut());
        };
        let Some(current_color) =
            crate::css::table_group_builder::own_color_from_table(unsafe { &*table }, used_color_scheme, Some(length))
        else {
            release_table(table);
            self.counters.bump(Counter::EngineComputedRecordBailAssemble);
            return None;
        };
        let mut payloads = Vec::with_capacity(group_index::COUNT);
        for (group, &parent_payload) in parent_payloads.iter().enumerate().take(group_index::COUNT) {
            let payload = if group == STYLE_GROUP_INDEX_FONT {
                unsafe { crate::css::table_group_builder::rebuild_font_group_from_table(&*table, font, parent_payload) }
            } else {
                unsafe {
                    crate::css::table_group_builder::rebuild_group_from_table(
                        &*table,
                        group,
                        parent_payload,
                        current_color,
                        used_color_scheme,
                        Some(length),
                    )
                }
            };
            let Some(payload) = payload else {
                for (group, payload) in payloads.into_iter().enumerate() {
                    crate::css::computed_values::release_group_payload(group, payload);
                }
                release_table(table);
                self.counters.bump(Counter::EngineComputedRecordBailAssemble);
                return None;
            };
            payloads.push(payload);
        }
        let holds_image_values = crate::css::computed_values::style_group_payloads_hold_image_values(&payloads);
        let dependency_flags = unsafe { &*table }.publication_dependency_flags()
            | (u8::from(swap_eligible) * computed::INHERITED_GROUP_SWAP_ELIGIBLE)
            | (u8::from(holds_image_values) * computed::HOLDS_IMAGE_VALUES);
        let metadata_input = computed::ComputedMetadataInput {
            pseudo_element_styles: pseudo_styles,
            dependency_flags,
            counter_style_environment_identity: 0,
            animation_overlay_identity: 0,
            animated_overlay: std::ptr::null(),
            animation_overlay_payloads: &[],
            longhand_table: table,
        };
        if let Some(cascade_state) = cascade_state {
            self.computed_group_sets
                .set_pending_cascade_state(target, cascade_state);
        }
        let publication = self.publish_computed_groups_impl(
            Some(target),
            &payloads,
            computed::ENGINE_INHERITED_GROUP_COUNT,
            environment,
            metadata_input,
        );
        for (group, payload) in payloads.into_iter().enumerate() {
            crate::css::computed_values::release_group_payload(group, payload);
        }
        release_table(table);
        Some((publication.style_record_identity, swap_eligible))
    }

    /// Settle the synthetic pseudo-elements of an element the engine derived a record for, the
    /// way the C++ computation refreshes them after the element's own: each kind the element has
    /// rules for, and the marker a list item generates, is driven against the element's new
    /// record; one that generates no box any more is removed; one whose cascade state did not
    /// move keeps its record. `None` is a pseudo-element the engine cannot settle.
    fn engine_pseudo_records(
        &mut self,
        node: StyleNodeID,
        old_element_record: Option<computed::FinalStyleRecordID>,
        new_element_record: computed::FinalStyleRecordID,
        generation: u64,
        scratch: &mut EngineComputedRecordScratch,
    ) -> Option<()> {
        use pseudo_kind::{AFTER, BACKDROP, BEFORE, FIRST_LETTER, MARKER, SELECTION};

        let Some(inputs) = self.document_style_computation_inputs else {
            self.counters.bump(Counter::EngineComputedRecordBailNoEnvironment);
            return None;
        };
        let program_version = self.program.version();
        let mut states: [Option<CascadeStateID>; pseudo_kind::SYNTHETIC_COUNT] = [None; pseudo_kind::SYNTHETIC_COUNT];
        for (pseudo, version, state, priority_current) in self.winner_groups.pseudo_states(node) {
            let Ok(kind) = u8::try_from(pseudo.kind.0) else {
                continue;
            };
            if usize::from(kind) >= pseudo_kind::SYNTHETIC_COUNT {
                continue;
            }
            // A ::backdrop is materialized for a top-layer element only, which C++ decides; the
            // rules for it match every element. A stale row is no answer.
            if kind == BACKDROP {
                continue;
            }
            if version != program_version || !priority_current {
                self.counters.bump(Counter::EngineComputedRecordBailPseudoStale);
                return None;
            }
            states[usize::from(kind)] = Some(state);
        }
        // An element holding a backdrop style is in the top layer: its backdrop is C++'s.
        if self
            .computed_group_sets
            .assigned_pseudo_kinds(node)
            .any(|kind| kind == BACKDROP)
        {
            self.counters.bump(Counter::EngineComputedRecordBailPseudoBackdrop);
            return None;
        }
        let display_is_list_item = |engine: &Self, record: computed::FinalStyleRecordID| -> Option<bool> {
            let view = engine.computed_group_sets.style_record_view(record.raw())?;
            let table = unsafe { view.longhand_table.as_ref() }?;
            Some(table.display_is_list_item())
        };
        let Some(new_is_list_item) = display_is_list_item(self, new_element_record) else {
            self.counters.bump(Counter::EngineComputedRecordBailRecord);
            return None;
        };
        let Some(new_view_dependency_flags) = self
            .computed_group_sets
            .style_record_view(new_element_record.raw())
            .map(|view| view.dependency_flags)
        else {
            self.counters.bump(Counter::EngineComputedRecordBailRecord);
            return None;
        };
        let old_is_list_item = match old_element_record {
            Some(record) => {
                let Some(list_item) = display_is_list_item(self, record) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecord);
                    return None;
                };
                list_item
            }
            None => false,
        };
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
        let Some(environment) = self.computed_group_sets.custom_property_environment_identity(node) else {
            self.counters.bump(Counter::EngineComputedRecordBailRecord);
            return None;
        };
        // The kinds the node's match answer has rules for: a winner row is published for each
        // the engine cascaded itself, and a kind with rules but no row is not decided.
        let Some(kinds_with_rules) = self.pseudo_style_mask(node) else {
            self.counters.bump(Counter::EngineComputedRecordBailPseudoMask);
            return None;
        };
        for kind in [BEFORE, AFTER, FIRST_LETTER, SELECTION, MARKER] {
            let target = computed::ComputedStyleTarget::new(node, kind);
            let old = self.computed_group_sets.pseudo_style_record(node, kind);
            if let Some(old) = old {
                let Some(view) = self.computed_group_sets.style_record_view(old.raw()) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecord);
                    return None;
                };
                let transitioning = (unsafe { view.longhand_table.as_ref() })
                    .is_some_and(|table| !crate::css::style_compute::active_transition_properties(table).is_empty());
                if !view.animated_overlay.is_null() || transitioning {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return None;
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
            if has_rules && state.is_none() {
                self.counters.bump(Counter::EngineComputedRecordBailPseudoRow);
                return None;
            }
            // The row has to hold the rules that flipped for this kind: one this flush published
            // holds the cascade of the node's current answer.
            if state.is_some()
                && scratch
                    .flipped_pseudo_rules
                    .iter()
                    .any(|flip| flip.pseudo_kind == Some(u16::from(kind)))
                && self.winner_groups.pseudo_row_stamp(
                    node,
                    tree::PseudoElementTarget::new(tree::PseudoElementKind(u16::from(kind))),
                ) != Some(self.flush_stamp)
            {
                self.counters.bump(Counter::EngineComputedRecordBailPseudoFlip);
                return None;
            }
            let old_record = old.unwrap_or(computed::FinalStyleRecordID::NONE);
            let remove = |engine: &mut Self, scratch: &mut EngineComputedRecordScratch| {
                if old.is_some() {
                    engine.note_engine_computed_pseudo_record(
                        node,
                        kind,
                        old_record,
                        computed::FinalStyleRecordID::NONE,
                        None,
                        0,
                        scratch,
                    );
                }
            };
            if !has_rules && !implicit {
                remove(self, scratch);
                continue;
            }
            // Reuse only when the originating element preserves every input the pseudo reads,
            // including display transformation and explicit inheritance of non-inherited values.
            if old.is_some()
                && originating_inputs_unchanged
                && (old_element_record == Some(new_element_record)
                    || !state.is_some_and(|state| self.state_explicitly_inherits_non_inherited_property(node, state)))
            {
                let bound = self
                    .computed_group_sets
                    .cascade_state(target)
                    .or_else(|| self.computed_group_sets.pseudo_retained_cascade_state(node, kind));
                let unchanged = match (state, bound) {
                    (Some(state), Some((bound_generation, bound_state))) => {
                        bound_generation == generation
                            && self.winner_groups.semantic_delta(Some(bound_state), state).is_empty()
                    }
                    (None, None) => true,
                    _ => false,
                } && (kind != MARKER || old_is_list_item == new_is_list_item);
                if unchanged {
                    continue;
                }
            }
            let store = match state {
                Some(state) => match scratch.pseudo_stores.get(&(kind, state)) {
                    Some(store) => store.clone(),
                    None => {
                        let store = std::rc::Rc::new(self.cascaded_store_for_state(node, state, Some(kind))?);
                        scratch.pseudo_stores.insert((kind, state), store.clone());
                        store
                    }
                },
                None => std::rc::Rc::new(CascadedPropertyStore::new()),
            };
            if pseudo_content_generates_nothing(&store, kind) {
                remove(self, scratch);
                continue;
            }
            // What the record is derived from: the element's inherited style, display and
            // environment, and the element's record itself only when the state inherits a
            // non-inherited property from it.
            let key = self
                .computed_group_sets
                .node_inherited_groups_identity(node)
                .zip(self.box_type_parent_display(node))
                .map(|(inherited_groups, parent_display)| PseudoCohortKey {
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
                    );
                    self.counters.bump(Counter::EngineComputedRecordCohortHits);
                    (publication.style_record_identity, 0)
                }
                None => {
                    let subject = DriveSubject {
                        parent: Some(node),
                        facts,
                    };
                    let (table, length, longhand_evaluations, font) =
                        self.engine_full_drive(subject, None, &store, &inputs)?;
                    let font = font.expect("a full drive resolves the font");
                    let (record, _) = self.assemble_and_publish_engine_record(
                        target,
                        Some(new_element_record),
                        table,
                        &length,
                        &font,
                        environment,
                        0,
                        cascade_state,
                    )?;
                    if let Some(key) = key {
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
            );
        }
        Some(())
    }

    /// Account for a pseudo-element record the engine settled (a removal when `new_style_record`
    /// is none) and leave its commitment to C++'s acknowledgement of the element.
    #[allow(clippy::too_many_arguments)]
    fn note_engine_computed_pseudo_record(
        &mut self,
        node: StyleNodeID,
        pseudo_kind: u8,
        old_style_record: computed::FinalStyleRecordID,
        new_style_record: computed::FinalStyleRecordID,
        cascade_state: Option<(u64, CascadeStateID)>,
        longhand_evaluations: u32,
        scratch: &mut EngineComputedRecordScratch,
    ) {
        self.counters.bump(Counter::EngineComputedPseudoRecords);
        self.engine_computed_records_pending.push(PendingEngineComputedRecord {
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

    /// A derivation that could not be completed: everything derived for `node` this flush goes
    /// back to what the node held, and nothing in the flush may share it.
    fn abandon_engine_computed_record(&mut self, node: StyleNodeID, scratch: &mut EngineComputedRecordScratch) {
        let mut index = 0;
        while index < self.engine_computed_records_pending.len() {
            if self.engine_computed_records_pending[index].node != node {
                index += 1;
                continue;
            }
            let pending = self.engine_computed_records_pending.swap_remove(index);
            let derived = pending.new_style_record;
            if pending.pseudo_kind == u8::MAX {
                let target = computed::ComputedStyleTarget::new(node, u8::MAX);
                self.computed_group_sets.take_pending_cascade_state(target);
                self.computed_group_sets
                    .revert_engine_computed_record(node, derived, pending.old_style_record);
                scratch.cohorts.retain(|_, record| *record != derived);
                scratch.cold_cohorts.retain(|_, record| record.record != derived);
                self.engine_cold_record_cache
                    .retain(|_, record| record.record != derived);
            } else {
                self.revert_engine_computed_pseudo_record(&pending);
                scratch.pseudo_cohorts.retain(|_, record| *record != derived);
                self.engine_pseudo_record_cache.retain(|_, record| *record != derived);
            }
        }
        scratch.pseudo_deltas.clear();
        self.settle_computed_memory();
        self.counters.bump(Counter::EngineComputedRecordsAbandoned);
    }

    /// Put a pseudo-element back the way it was before the engine settled it, unless a
    /// publication has moved it on since.
    fn revert_engine_computed_pseudo_record(&mut self, pending: &PendingEngineComputedRecord) {
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
            );
        } else {
            self.computed_group_sets
                .remove_pseudo(pending.node, pending.pseudo_kind);
        }
    }

    /// Whether a node's record inherits from the parent it has now: each inherited group its
    /// state declares nothing in is the parent's own, no non-inherited property is inherited
    /// explicitly, and its custom-property environment is the parent's.
    fn record_inherits_from_current_parent(&self, node: StyleNodeID, state: CascadeStateID) -> bool {
        use crate::css::property_metadata::property_style_group_index;
        let Some(parent) = self.tree.flat_tree_parent(node) else {
            return false;
        };
        if self.state_explicitly_inherits_non_inherited_property(node, state) {
            return false;
        }
        let own_groups = self
            .winner_groups
            .winners_in_state(state)
            .filter_map(|winner| property_style_group_index(winner.property))
            .fold(0_u32, |mask, group| mask | (1 << group));
        self.computed_group_sets
            .inherited_groups_follow_parent(node, parent, own_groups)
            .unwrap_or(false)
            && self
                .computed_group_sets
                .custom_property_environment_identity(node)
                .is_some()
            && self.computed_group_sets.custom_property_environment_identity(node)
                == self.computed_group_sets.custom_property_environment_identity(parent)
    }

    /// The inherited groups a state's winners rebuild for their element: the groups its
    /// declarations land in, and the ones holding colors when it declares `color`, which their
    /// currentcolor-dependent values resolve against.
    fn state_owned_inherited_groups(&self, state: CascadeStateID) -> u32 {
        use crate::css::computed_value_types::{
            STYLE_GROUP_INDEX_INHERITED_SVG, STYLE_GROUP_INDEX_INHERITED_TEXT, STYLE_GROUP_INDEX_INHERITED_UI,
        };
        use crate::css::property_metadata::{property_id as prop, property_style_group_index};
        self.winner_groups.winners_in_state(state).fold(0_u32, |mask, winner| {
            let mask = mask | property_style_group_index(winner.property).map_or(0, |group| 1 << group);
            if winner.property == prop::COLOR {
                mask | (1 << STYLE_GROUP_INDEX_INHERITED_UI)
                    | (1 << STYLE_GROUP_INDEX_INHERITED_SVG)
                    | (1 << STYLE_GROUP_INDEX_INHERITED_TEXT)
            } else {
                mask
            }
        })
    }

    /// The document element's record settled: the root font metrics the drives after it resolve
    /// `rem` against are its font's, the way C++ refreshes them after computing it.
    pub(super) fn refresh_root_font_metrics_from_record(&mut self, record: computed::FinalStyleRecordID) {
        use crate::css::computed_value_types::STYLE_GROUP_INDEX_FONT;
        let Some(view) = self.computed_group_sets.style_record_view(record.raw()) else {
            return;
        };
        let font =
            unsafe { &*view.payloads[STYLE_GROUP_INDEX_FONT].cast::<crate::css::computed_value_types::FontValues>() };
        let Some(inputs) = self.document_style_computation_inputs.as_mut() else {
            return;
        };
        inputs.root_font_size = font.font_size.to_double();
        inputs.root_font_x_height = drive_font_metric(font.font_x_height);
        inputs.root_font_cap_height = drive_font_metric(font.font_ascent);
        inputs.root_font_zero_advance = drive_font_metric(font.font_zero_advance);
        inputs.root_line_height = font.line_height_used.to_double();
        inputs.root_font_metrics_depend_on_viewport_metrics = view.dependency_flags & (1 << 1) != 0;
    }

    fn element_drive_subject(&mut self, node: StyleNodeID) -> Option<DriveSubject> {
        let facts = self.computed_group_sets.adjustment_facts(node);
        let parent = self.tree.flat_tree_parent(node);
        // Only the document element is styled without a flat-tree parent.
        if parent.is_none() && facts & bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT == 0 {
            self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
            return None;
        }
        // A child inherits the parent's animated values, which C++ composes over the record.
        if let Some(parent) = parent
            && self.computed_group_sets.node_has_animation_overlay(parent)
        {
            self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return None;
        }
        Some(DriveSubject { parent, facts })
    }

    /// A later element alike in what a first record is computed from takes this record, the way a
    /// C++ computation shares across transactions. The cache is small and bounded.
    fn remember_cold_record(&mut self, key: ColdRecordKey, record: ColdRecord) {
        if self.engine_cold_record_cache.len() >= COLD_RECORD_CACHE_LIMIT {
            self.engine_cold_record_cache.clear();
        }
        self.engine_cold_record_cache.insert(key, record);
    }

    /// Whether a winner state is one the engine computes records from: every winner a plain rule
    /// declaration with a written value that needs no document context. Decided once per state.
    fn state_is_engine_computable(&mut self, node: StyleNodeID, cascade_state: (u64, CascadeStateID)) -> bool {
        if let Some(&admitted) = self.engine_computable_states.get(&cascade_state) {
            return admitted;
        }
        let admitted = self.cascaded_store_for_state(node, cascade_state.1, None).is_some();
        self.remember_state_admission(cascade_state, admitted);
        admitted
    }

    fn remember_state_admission(&mut self, cascade_state: (u64, CascadeStateID), admitted: bool) {
        if self.engine_computable_states.len() >= COLD_RECORD_CACHE_LIMIT {
            self.engine_computable_states.clear();
        }
        self.engine_computable_states.insert(cascade_state, admitted);
    }

    /// The parent-side half of a first record's sharing key, or nothing when the parent's style
    /// is one no first record may be shared under.
    fn cold_record_parent(
        &self,
        node: StyleNodeID,
        parent: StyleNodeID,
        parent_record: computed::FinalStyleRecordID,
        state: CascadeStateID,
    ) -> Option<ColdRecordParent> {
        let view = self.computed_group_sets.style_record_view(parent_record.raw())?;
        if !view.animated_overlay.is_null() {
            return None;
        }
        let dependency_flags = view.dependency_flags;
        let environment = self.computed_group_sets.custom_property_environment_identity(parent)?;
        let inherited_groups = self.computed_group_sets.node_inherited_groups_identity(parent)?;
        let parent_display = self.box_type_parent_display(parent)?;
        let record = if self.state_explicitly_inherits_non_inherited_property(node, state) {
            parent_record.raw()
        } else {
            0
        };
        Some(ColdRecordParent {
            record,
            inherited_groups,
            environment,
            dependency_flags,
            parent_display,
        })
    }

    /// The display the box-type transformation reads as the parent's for a child of the parent:
    /// the parent's own, past any display:contents ancestor, packed into one word.
    fn box_type_parent_display(&self, parent: StyleNodeID) -> Option<u32> {
        let mut ancestor = Some(parent);
        while let Some(current) = ancestor {
            let record = self.computed_group_sets.assigned_style_record(current)?;
            let view = self.computed_group_sets.style_record_view(record.raw())?;
            let table = unsafe { view.longhand_table.as_ref() }?;
            let display = crate::css::style_compute::effective_display(table, None);
            // C++ styles the children of a display:none element on demand, past the engine's
            // view of the parent, so no first record is computed under one.
            if display.is_none() {
                return None;
            }
            if !display.is_contents() {
                return Some(
                    u32::from(display.tag)
                        | u32::from(display.outside) << 8
                        | u32::from(display.inside) << 16
                        | u32::from(display.internal) << 24
                        | u32::from(display.list_item) << 3,
                );
            }
            ancestor = self.tree.flat_tree_parent(current);
        }
        None
    }

    /// Whether a winner state declares `inherit` for a non-inherited property, or carries a value
    /// the engine cannot see the spelling of.
    fn state_explicitly_inherits_non_inherited_property(&self, node: StyleNodeID, state: CascadeStateID) -> bool {
        let is_inherit_keyword = |value: Option<crate::css::style_value::RetainedStyleValueData>| {
            value.is_none_or(|value| {
                matches!(value.data(), crate::css::style_value::StyleValueData::Keyword { keyword }
                    if *keyword == crate::css::style_compute::keyword::INHERIT)
            })
        };
        self.winner_groups.winners_in_state(state).any(|winner| {
            if crate::css::property_metadata::property_is_inherited(winner.property) {
                return false;
            }
            let Some(winner) = self.winner_groups.resolved_winner(winner) else {
                return false;
            };
            match winner.source {
                WinnerSource::Rule(rule) => is_inherit_keyword(self.program.written_winner_value(
                    rule,
                    winner.property,
                    winner.important,
                    winner.key.value,
                )),
                WinnerSource::Element(kind) => {
                    let (declared, _) = self.facts.element_declared_properties(node, kind);
                    let written = self.facts.element_written_declared_values(node, kind);
                    let index = declared.iter().rposition(|declared| {
                        declared.property == winner.property
                            && declared.important == winner.important
                            && declared.value == winner.key.value
                    });
                    is_inherit_keyword(
                        index
                            .and_then(|index| written.get(index))
                            .map(|value| value.clone_retained()),
                    )
                }
                WinnerSource::ExactCascade => true,
            }
        })
    }

    /// Keep a record C++ published for an element as a first record a later alike element can
    /// take, when it was computed from nothing but what the engine keys first records on: the
    /// parent's inherited style and environment, a winner state the engine can compute from, the
    /// element facts and the pseudo-elements it has rules for.
    fn remember_cold_record_candidate(
        &mut self,
        target: computed::ComputedStyleTarget,
        cascade_state: (u64, CascadeStateID),
        custom_property_environment: u64,
        pseudo_styles: u64,
        style_record: computed::FinalStyleRecordID,
        is_base_record: bool,
    ) {
        if target.is_pseudo() || !is_base_record {
            return;
        }
        let Some(inputs) = self.document_style_computation_inputs else {
            return;
        };
        let node = target.node();
        let facts = self.computed_group_sets.adjustment_facts(node);
        // A record C++ computed for an element with hints or animations is not what its winner
        // state alone describes.
        if facts
            & (bridge::element_adjustment_fact::HAS_PRESENTATIONAL_HINTS
                | bridge::element_adjustment_fact::HAS_ANIMATIONS)
            != 0
        {
            return;
        }
        let Some(parent) = self.tree.flat_tree_parent(node) else {
            return;
        };
        let Some(parent_record) = self.computed_group_sets.assigned_style_record(parent) else {
            return;
        };
        if self.computed_group_sets.custom_property_environment_identity(parent) != Some(custom_property_environment) {
            return;
        }
        if !self.state_is_engine_computable(node, cascade_state) {
            return;
        }
        let Some(parent) = self.cold_record_parent(node, parent, parent_record, cascade_state.1) else {
            return;
        };
        let swap_eligible = self.computed_group_sets.node_inherited_group_swap_eligible(node);
        let key = ColdRecordKey {
            parent,
            generation: cascade_state.0,
            state: cascade_state.1,
            facts,
            pseudo_styles,
            font_environment_generation: inputs.font_environment_generation,
        };
        self.remember_cold_record(
            key,
            ColdRecord {
                record: style_record,
                swap_eligible,
            },
        );
    }

    /// The synthetic pseudo-elements the node has matching rules for, as the bits a C++ record
    /// carries in its pseudo-style mask: derived from the node's match answer the way C++ derives
    /// its own, since pseudo winner rows exist only for answers the engine cascaded itself. `None`
    /// when the engine holds no answer for the node.
    /// The synthetic pseudo-elements the node has rules for, as a C++ record's pseudo-style mask,
    /// or no bits when the engine holds no answer for the node.
    pub(crate) fn published_pseudo_style_mask(&self, node: StyleNodeID) -> u64 {
        self.pseudo_style_mask(node).unwrap_or(0)
    }

    fn pseudo_style_mask(&self, node: StyleNodeID) -> Option<u64> {
        let bit = |pseudo: Option<tree::PseudoElementTarget>| {
            pseudo
                .map(|pseudo| pseudo.kind.0)
                .filter(|&kind| kind <= bridge::LAST_SYNTHETIC_PSEUDO_ELEMENT_KIND)
                .map_or(0, |kind| 1u64 << kind)
        };
        if let Some(answer) = self.published_match_answers.lookup(node)
            && let Some(matches) = self.published_match_answers.matches_for(answer)
        {
            return Some(
                matches
                    .iter()
                    .fold(0, |mask, rule_match| mask | bit(rule_match.pseudo_element)),
            );
        }
        let Lookup::Known(answer) = self.retained_match_answer(node) else {
            return None;
        };
        Some(answer.iter().fold(0, |mask, rule_match| {
            let entry = &self.programs.get(rule_match.program).entries()[rule_match.entry as usize];
            mask | bit(entry.pseudo_element)
        }))
    }

    /// The cascade a winner state describes, as the drive consumes it: every winner's written
    /// value, seeded in cascade order so a logical property pair resolves the way it cascaded.
    /// `None` when a winner is not a plain rule declaration the engine can compute from.
    fn cascaded_store_for_state(
        &mut self,
        node: StyleNodeID,
        state: CascadeStateID,
        pseudo_kind: Option<u8>,
    ) -> Option<CascadedPropertyStore> {
        use crate::css::property_metadata::property_id as prop;
        let winners: Vec<PropertyWinner> = self.winner_groups.winners_in_state(state).collect();
        // Seeded in cascade order, and within one rule in declaration order, since a logical
        // property and its physical associate resolve by order of appearance.
        let mut declarations = Vec::with_capacity(winners.len());
        for winner in winners {
            // A revert whose continuation resumes at nothing leaves the property undeclared.
            let Some(winner) = self.winner_groups.resolved_winner(winner) else {
                continue;
            };
            if winner.key.animation_relevance != 0 {
                self.counters.bump(Counter::EngineComputedRecordBailWinnerAnimated);
                return None;
            }
            // A pseudo-element's cascade keeps the properties its kind supports; its `content`
            // computes in the drive when the value needs no element or counter environment.
            if let Some(kind) = pseudo_kind {
                if !crate::css::property_metadata::pseudo_element_supports_property(kind, winner.property) {
                    continue;
                }
                if winner.property != prop::CONTENT && property_starts_animation_or_counter_environment(winner.property)
                {
                    self.counters.bump(Counter::EngineComputedRecordBailProperty);
                    return None;
                }
            }
            // The drive computes from the spelling the declaration was written in, which the
            // cascade's canonical identity may have rewritten; the rule keeps it.
            let declaration = match winner.source {
                WinnerSource::Rule(rule) => {
                    self.program
                        .written_winner_declaration(rule, winner.property, winner.important, winner.key.value)
                }
                // An element's own declarations keep their written values beside the facts the
                // way rules keep theirs; the element they belong to is the one being computed,
                // since its state names them.
                WinnerSource::Element(kind) => {
                    let (declared, complete) = self.facts.element_declared_properties(node, kind);
                    let written = self.facts.element_written_declared_values(node, kind);
                    if !complete || written.len() != declared.len() {
                        self.counters.bump(Counter::EngineComputedRecordBailWinnerElement);
                        return None;
                    }
                    declared
                        .iter()
                        .rposition(|declared| {
                            declared.property == winner.property
                                && declared.important == winner.important
                                && declared.value == winner.key.value
                        })
                        .map(|index| (index, written[index].clone_retained()))
                }
                WinnerSource::ExactCascade => {
                    self.counters.bump(Counter::EngineComputedRecordBailWinnerOperator);
                    return None;
                }
            };
            let Some((index, value)) = declaration else {
                self.counters.bump(Counter::EngineComputedRecordBailWinnerSpelling);
                return None;
            };
            // A longhand declared through a shorthand keeps the whole shorthand as its written
            // value; the store takes the longhand's own part of it.
            let value = match value.data() {
                crate::css::style_value::StyleValueData::Shorthand { .. } => {
                    let Some(value) = shorthand_longhand_value(winner.property, value.data()) else {
                        self.counters.bump(Counter::EngineComputedRecordBailWinnerSpelling);
                        return None;
                    };
                    value
                }
                _ => value,
            };
            if !value_computes_without_document_context(value.data())
                || (pseudo_kind.is_some()
                    && winner.property == prop::CONTENT
                    && !content_value_is_engine_computable(value.data()))
            {
                self.counters.bump(Counter::EngineComputedRecordBailValue);
                return None;
            }
            declarations.push((winner.priority, index, winner.property, winner.important, value));
        }
        declarations.sort_by_key(|(priority, index, ..)| (*priority, *index));
        let mut store = CascadedPropertyStore::new();
        for (_, _, property, important, value) in declarations {
            store.seed_retained_property(property, value, important, false);
        }
        Some(store)
    }

    /// Run the drive's remaining phase for the selected longhands over a copy of the node's
    /// current table, against the record's own font metrics, the document's computation inputs
    /// and the parent's record. The required driver inputs recompute on every drive and their
    /// post-compute adjustments read element facts this context does not carry, so the table
    /// stands only when they came out exactly as before.
    fn engine_driven_table(
        &mut self,
        node: StyleNodeID,
        old_style_record: computed::FinalStyleRecordID,
        store: &CascadedPropertyStore,
        selected: &[u64],
        inputs: &bridge::FfiDocumentStyleComputationInputs,
    ) -> Option<(
        ComputedLonghandTable,
        crate::css::style_compute::FfiLengthResolutionContext,
        u32,
        Option<crate::css::table_group_builder::FfiFontGroupBuildInputs>,
    )> {
        use crate::css::computed_value_types::{STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_INHERITED_BOX};
        use crate::css::style_compute::{
            FfiBoxTypeTransformationInput, FfiEffectiveColorSchemeInput, FfiFontMetrics, FfiLengthResolutionContext,
            FfiStyleComputationEnvironment, LONGHAND_DRIVE_PHASE_REMAINING, drive_property_computation,
            empty_longhand_driver_results, is_required_driver_input, keyword, parent_snapshot_for_style_record,
            property_computation_order_for_phase,
        };

        let Some(view) = self.computed_group_sets.style_record_view(old_style_record.raw()) else {
            self.counters.bump(Counter::EngineComputedRecordBailRecord);
            return None;
        };
        if !view.animated_overlay.is_null() {
            self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return None;
        }
        let Some(old_table) = (unsafe { view.longhand_table.as_ref() }) else {
            self.counters.bump(Counter::EngineComputedRecordBailRecordTable);
            return None;
        };
        // A record under display:none may no longer be the style C++ holds, and a property change
        // on an element with active transitions starts one in the C++ computation.
        if view.dependency_flags & (1 << 2) != 0
            || !crate::css::style_compute::active_transition_properties(old_table).is_empty()
        {
            self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return None;
        }
        let snapshot = match self.tree.flat_tree_parent(node) {
            None => None,
            Some(parent) => match self.computed_group_sets.assigned_style_record(parent) {
                Some(record) => Some(parent_snapshot_for_style_record(self, record.raw(), None)),
                None => {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return None;
                }
            },
        };
        let font =
            unsafe { &*view.payloads[STYLE_GROUP_INDEX_FONT].cast::<crate::css::computed_value_types::FontValues>() };
        let inherited_box = unsafe {
            &*view.payloads[STYLE_GROUP_INDEX_INHERITED_BOX].cast::<crate::css::computed_values::InheritedBoxValues>()
        };
        let mut resolved_viewport_relative_length = false;
        let length = FfiLengthResolutionContext {
            viewport_width: inputs.viewport_width,
            viewport_height: inputs.viewport_height,
            font_metrics: FfiFontMetrics {
                font_size: font.font_size.to_double(),
                x_height: drive_font_metric(font.font_x_height),
                // The C++ metrics approximate the cap height with the ascent.
                cap_height: drive_font_metric(font.font_ascent),
                zero_advance: drive_font_metric(font.font_zero_advance),
                line_height: font.line_height_used.to_double(),
            },
            root_font_metrics: FfiFontMetrics {
                font_size: inputs.root_font_size,
                x_height: inputs.root_font_x_height,
                cap_height: inputs.root_font_cap_height,
                zero_advance: inputs.root_font_zero_advance,
                line_height: inputs.root_line_height,
            },
            font_metrics_depend_on_viewport_metrics: view.dependency_flags & (1 << 1) != 0,
            root_font_metrics_depend_on_viewport_metrics: inputs.root_font_metrics_depend_on_viewport_metrics,
            has_container_width_basis: false,
            has_container_height_basis: false,
            container_width_basis: 0.0,
            container_height_basis: 0.0,
            container_width_basis_depends_on_viewport_metrics: false,
            container_height_basis_depends_on_viewport_metrics: false,
            subject_inline_axis_is_horizontal: inherited_box.writing_mode
                == crate::css::css_enums::writing_mode::HORIZONTAL_TB,
            resolved_viewport_relative_length: &raw mut resolved_viewport_relative_length,
        };
        // No element fact reaches the remaining phase through this environment: the moved
        // properties were checked not to need one, and the required driver inputs are compared
        // against the record below.
        let environment = FfiStyleComputationEnvironment {
            box_type_input: FfiBoxTypeTransformationInput {
                display: crate::css::display::FfiDisplay::inline(),
                position: keyword::STATIC,
                float_value: keyword::NONE,
                is_br_element: false,
                is_document_element: false,
                is_mathml_element: false,
                is_mathml_mtable: false,
                is_mathml_mtr: false,
                is_mathml_mtd: false,
                has_parent_display: false,
                parent_display: crate::css::display::FfiDisplay::block(),
                is_wbr_element: false,
                disallow_display_contents: false,
                rewrite_inline_flow: false,
                is_button_element: false,
                force_line_height_normal: false,
                check_input_line_height: false,
                hide_audio_without_controls: false,
                is_table_element: false,
                force_position_static: false,
                force_symbol_display_inline: false,
                webkit_box_layout_transformation_applies: false,
            },
            color_scheme_input: FfiEffectiveColorSchemeInput {
                preferred_color_scheme: 0,
                has_document_supported_schemes: false,
                document_supported_scheme_codes: std::ptr::null(),
                document_supported_scheme_count: 0,
            },
            is_th_element: false,
            has_new_font_size: false,
            has_tree_counting_context: false,
            sibling_count: 0,
            sibling_index: 0,
            random_base_values: std::ptr::null(),
            random_base_value_count: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            style_sheet_resource_contexts: std::ptr::null(),
            style_sheet_resource_context_count: 0,
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
            initial_font_size_raw: inputs.initial_font_size_raw,
            default_font_size_raw: inputs.default_font_size_raw,
        };
        let mut table = view.longhand_table_for_partial_drive();
        let mut results = empty_longhand_driver_results();
        let mut effective_color_scheme = old_table.effective_color_scheme();
        unsafe {
            drive_property_computation(
                &raw mut table,
                std::ptr::null_mut(),
                store,
                snapshot.as_ref(),
                &raw const environment,
                u32::MAX,
                selected.as_ptr(),
                LONGHAND_DRIVE_PHASE_REMAINING,
                &raw const length,
                std::ptr::null(),
                std::ptr::null(),
                &raw mut results,
                &mut effective_color_scheme,
                true,
            );
        }
        if results.explicitly_inherited_non_inherited_style_groups != 0
            || results.uses_tree_counting_function
            || table.display_before_box_type_transformation() != old_table.display_before_box_type_transformation()
        {
            self.counters.bump(Counter::EngineComputedRecordBailDrive);
            return None;
        }
        let old_values = old_table.value_pointers();
        for &property in property_computation_order_for_phase(LONGHAND_DRIVE_PHASE_REMAINING) {
            if !is_required_driver_input(property) {
                continue;
            }
            let slot = usize::from(property - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID);
            let old_value = old_values[slot];
            let new_value = table.value_pointers()[slot];
            if old_value == new_value {
                continue;
            }
            let equal = unsafe {
                match (
                    old_value.cast::<StyleValueData>().as_ref(),
                    new_value.cast::<StyleValueData>().as_ref(),
                ) {
                    (Some(old_value), Some(new_value)) => old_value == new_value,
                    _ => false,
                }
            };
            if !equal {
                self.counters.bump(Counter::EngineComputedRecordBailDrive);
                return None;
            }
            table.copy_slot_from(old_table, property);
        }
        // The group builders resolve against the same context; they report no viewport dependence
        // of their own.
        let length = FfiLengthResolutionContext {
            resolved_viewport_relative_length: std::ptr::null_mut(),
            ..length
        };
        Some((table, length, results.longhand_evaluations, None))
    }

    /// Drive a record through every phase: the font phase against the parent's metrics, the
    /// element's font resolved through the document's resolver, line-height and color-scheme
    /// against that font, and the remaining phase with the element facts the box-type
    /// transformation reads. Elements whose font family selects the monospace default size, and
    /// the document element, still compute in C++.
    #[allow(clippy::too_many_lines)]
    fn engine_full_drive(
        &mut self,
        subject: DriveSubject,
        old_style_record: Option<computed::FinalStyleRecordID>,
        store: &CascadedPropertyStore,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
    ) -> Option<(
        ComputedLonghandTable,
        crate::css::style_compute::FfiLengthResolutionContext,
        u32,
        Option<crate::css::table_group_builder::FfiFontGroupBuildInputs>,
    )> {
        use crate::css::computed_value_types::{STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_INHERITED_BOX};
        use crate::css::css_pixels::CssPixels;
        use crate::css::property_metadata::property_id as prop;
        use crate::css::style_compute::{
            FfiBoxTypeTransformationInput, FfiEffectiveColorSchemeInput, FfiFontMetrics, FfiInputLineHeightMetrics,
            FfiLengthResolutionContext, FfiStyleComputationEnvironment, LONGHAND_DRIVE_PHASE_COLOR_SCHEME,
            LONGHAND_DRIVE_PHASE_FONT, LONGHAND_DRIVE_PHASE_LINE_HEIGHT, LONGHAND_DRIVE_PHASE_REMAINING,
            drive_property_computation, effective_display, empty_longhand_driver_results, font_family_is_monospace,
            keyword,
        };
        use crate::css::table_group_builder::FfiFontGroupBuildInputs;
        use bridge::element_adjustment_fact as fact;

        let DriveSubject { parent, facts } = subject;
        let has = |bit: u32| facts & bit != 0;
        let is_document_element = has(fact::IS_DOCUMENT_ELEMENT);
        // An element with animations composes its style with their effects in C++.
        if facts & fact::HAS_ANIMATIONS != 0 {
            self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return None;
        }
        if self.font_resolver.is_none() {
            self.counters.bump(Counter::EngineComputedRecordBailNoEnvironment);
            return None;
        }
        if store
            .winning_declaration(prop::FONT_FAMILY)
            .is_some_and(|(value, ..)| font_family_is_monospace(unsafe { &*value.cast::<StyleValueData>() }))
        {
            self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
            return None;
        }
        let old_table = match old_style_record {
            Some(old_style_record) => {
                let Some(view) = self.computed_group_sets.style_record_view(old_style_record.raw()) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecord);
                    return None;
                };
                if !view.animated_overlay.is_null() {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return None;
                }
                let Some(old_table) = (unsafe { view.longhand_table.as_ref() }) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordTable);
                    return None;
                };
                if view.dependency_flags & (1 << 2) != 0
                    || !crate::css::style_compute::active_transition_properties(old_table).is_empty()
                {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
                    return None;
                }
                Some(old_table)
            }
            None => None,
        };
        let parent_view = match parent {
            Some(parent) => {
                let Some(parent_record) = self.computed_group_sets.assigned_style_record(parent) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return None;
                };
                let Some(parent_view) = self.computed_group_sets.style_record_view(parent_record.raw()) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return None;
                };
                Some(parent_view)
            }
            None => None,
        };
        // The document element inherits from the initial values and resolves its font against
        // the document's initial font, the way C++'s document resolution context does.
        let initial_metrics = FfiFontMetrics {
            font_size: inputs.initial_font_size,
            x_height: inputs.initial_font_x_height,
            cap_height: inputs.initial_font_cap_height,
            zero_advance: inputs.initial_font_zero_advance,
            line_height: 0.0,
        };
        let (parent_metrics, parent_font_metrics_depend_on_viewport_metrics, parent_line_height_used) =
            match &parent_view {
                Some(parent_view) => {
                    let parent_font = unsafe {
                        &*parent_view.payloads[STYLE_GROUP_INDEX_FONT]
                            .cast::<crate::css::computed_value_types::FontValues>()
                    };
                    (
                        FfiFontMetrics {
                            font_size: parent_font.font_size.to_double(),
                            x_height: drive_font_metric(parent_font.font_x_height),
                            cap_height: drive_font_metric(parent_font.font_ascent),
                            zero_advance: drive_font_metric(parent_font.font_zero_advance),
                            line_height: parent_font.line_height_used.to_double(),
                        },
                        parent_view.dependency_flags & (1 << 1) != 0,
                        parent_font.line_height_used.to_double(),
                    )
                }
                None => (initial_metrics, false, 0.0),
            };
        // C++ computes no style under a display:none ancestor.
        if old_table.is_none()
            && parent_view
                .as_ref()
                .is_some_and(|parent_view| parent_view.dependency_flags & (1 << 2) != 0)
        {
            self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
            return None;
        }
        // The parent's display, past any display:contents ancestor, is what the box-type
        // transformation reads.
        let mut parent_display = None;
        let mut ancestor = parent;
        while let Some(current) = ancestor {
            let Some(record) = self.computed_group_sets.assigned_style_record(current) else {
                break;
            };
            let Some(ancestor_view) = self.computed_group_sets.style_record_view(record.raw()) else {
                break;
            };
            let Some(ancestor_table) = (unsafe { ancestor_view.longhand_table.as_ref() }) else {
                break;
            };
            let display = effective_display(ancestor_table, None);
            if !display.is_contents() {
                parent_display = Some(display);
                break;
            }
            ancestor = self.tree.flat_tree_parent(current);
        }
        let snapshot = match &parent_view {
            Some(parent_view) => {
                let Some(parent_table) = (unsafe { parent_view.longhand_table.as_ref() }) else {
                    self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return None;
                };
                Some(crate::css::style_compute::ParentSnapshot::new(
                    parent_table,
                    unsafe { parent_view.animated_overlay.as_ref() },
                    parent_font_metrics_depend_on_viewport_metrics,
                    parent_view.dependency_flags & (1 << 2) != 0,
                ))
            }
            None => None,
        };
        // The subject axis is the element's own writing mode when it has one, else its parent's;
        // the initial writing mode is horizontal.
        let inherited_box_payload = match old_style_record {
            Some(old_style_record) => Some(
                self.computed_group_sets
                    .style_record_view(old_style_record.raw())?
                    .payloads[STYLE_GROUP_INDEX_INHERITED_BOX],
            ),
            None => parent_view
                .as_ref()
                .map(|parent_view| parent_view.payloads[STYLE_GROUP_INDEX_INHERITED_BOX]),
        };
        let subject_inline_axis_is_horizontal = inherited_box_payload.is_none_or(|payload| {
            let inherited_box = unsafe { &*payload.cast::<crate::css::computed_values::InheritedBoxValues>() };
            inherited_box.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB
        });
        let document_root_font_metrics = FfiFontMetrics {
            font_size: inputs.root_font_size,
            x_height: inputs.root_font_x_height,
            cap_height: inputs.root_font_cap_height,
            zero_advance: inputs.root_font_zero_advance,
            line_height: inputs.root_line_height,
        };
        let environment = FfiStyleComputationEnvironment {
            box_type_input: FfiBoxTypeTransformationInput {
                display: crate::css::display::FfiDisplay::inline(),
                position: keyword::STATIC,
                float_value: keyword::NONE,
                is_br_element: has(fact::IS_BR),
                is_document_element,
                is_mathml_element: has(fact::IS_MATHML),
                is_mathml_mtable: has(fact::IS_MATHML_MTABLE),
                is_mathml_mtr: has(fact::IS_MATHML_MTR),
                is_mathml_mtd: has(fact::IS_MATHML_MTD),
                has_parent_display: parent_display.is_some(),
                parent_display: parent_display.unwrap_or_else(crate::css::display::FfiDisplay::block),
                is_wbr_element: has(fact::IS_WBR),
                disallow_display_contents: has(fact::DISALLOW_DISPLAY_CONTENTS),
                rewrite_inline_flow: has(fact::REWRITE_INLINE_FLOW),
                is_button_element: has(fact::IS_BUTTON),
                force_line_height_normal: has(fact::FORCE_LINE_HEIGHT_NORMAL),
                check_input_line_height: has(fact::CHECK_INPUT_LINE_HEIGHT),
                hide_audio_without_controls: has(fact::HIDE_AUDIO_WITHOUT_CONTROLS),
                is_table_element: has(fact::IS_TABLE),
                force_position_static: has(fact::FORCE_POSITION_STATIC),
                force_symbol_display_inline: has(fact::FORCE_SYMBOL_DISPLAY_INLINE),
                webkit_box_layout_transformation_applies: false,
            },
            color_scheme_input: FfiEffectiveColorSchemeInput {
                preferred_color_scheme: inputs.preferred_color_scheme,
                has_document_supported_schemes: inputs.has_document_supported_schemes,
                document_supported_scheme_codes: inputs.document_supported_scheme_codes.as_ptr(),
                document_supported_scheme_count: usize::from(inputs.document_supported_scheme_count),
            },
            is_th_element: has(fact::IS_TH),
            has_new_font_size: false,
            has_tree_counting_context: false,
            sibling_count: 0,
            sibling_index: 0,
            random_base_values: std::ptr::null(),
            random_base_value_count: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            style_sheet_resource_contexts: std::ptr::null(),
            style_sheet_resource_context_count: 0,
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
            initial_font_size_raw: inputs.initial_font_size_raw,
            default_font_size_raw: inputs.default_font_size_raw,
        };
        let mut resolved_viewport_relative_length = false;
        let resolved_viewport_relative_length_pointer = &raw mut resolved_viewport_relative_length;
        // The root metrics a phase resolves against: the document's, except for the document
        // element itself, whose font phase reads the initial font and whose line-height phase
        // reads its own font; its remaining phase reads the document's metrics as they stand,
        // which C++ refreshes only after computing it.
        let length_context =
            |font_metrics: FfiFontMetrics,
             font_metrics_depend_on_viewport_metrics: bool,
             root_font_metrics: FfiFontMetrics,
             root_font_metrics_depend_on_viewport_metrics: bool| FfiLengthResolutionContext {
                viewport_width: inputs.viewport_width,
                viewport_height: inputs.viewport_height,
                font_metrics,
                root_font_metrics,
                font_metrics_depend_on_viewport_metrics,
                root_font_metrics_depend_on_viewport_metrics,
                has_container_width_basis: false,
                has_container_height_basis: false,
                container_width_basis: 0.0,
                container_height_basis: 0.0,
                container_width_basis_depends_on_viewport_metrics: false,
                container_height_basis_depends_on_viewport_metrics: false,
                subject_inline_axis_is_horizontal,
                resolved_viewport_relative_length: resolved_viewport_relative_length_pointer,
            };
        let mut table = old_table.map_or_else(ComputedLonghandTable::new, ComputedLonghandTable::copied_for_drive);
        let mut results = empty_longhand_driver_results();
        let mut effective_color_scheme: i16 = -1;
        let drive = |table: &mut ComputedLonghandTable,
                     results: &mut crate::css::style_compute::FfiLonghandDriverResults,
                     effective_color_scheme: &mut i16,
                     phase: u8,
                     length: *const FfiLengthResolutionContext,
                     input_line_height_metrics: *const FfiInputLineHeightMetrics,
                     line_height_before: *const std::ffi::c_void| unsafe {
            drive_property_computation(
                std::ptr::from_mut(table),
                std::ptr::null_mut(),
                store,
                snapshot.as_ref(),
                &raw const environment,
                u32::MAX,
                std::ptr::null(),
                phase,
                length,
                input_line_height_metrics,
                line_height_before,
                std::ptr::from_mut(results),
                effective_color_scheme,
                true,
            );
        };
        let font_length = if is_document_element {
            length_context(initial_metrics, false, initial_metrics, false)
        } else {
            length_context(
                parent_metrics,
                parent_font_metrics_depend_on_viewport_metrics,
                document_root_font_metrics,
                inputs.root_font_metrics_depend_on_viewport_metrics,
            )
        };
        drive(
            &mut table,
            &mut results,
            &mut effective_color_scheme,
            LONGHAND_DRIVE_PHASE_FONT,
            &raw const font_length,
            std::ptr::null(),
            std::ptr::null(),
        );

        // The element's own font, resolved as the C++ font computer would for these values.
        let value_of = |table: &ComputedLonghandTable, property: u16| -> Option<&StyleValueData> {
            unsafe {
                table
                    .effective_value(None, property, true)
                    .value
                    .cast::<StyleValueData>()
                    .as_ref()
            }
        };
        // The font resolver supplies default feature and variation settings. Check computed
        // values here because non-default settings can also come from inheritance.
        for (property, default_keyword) in [
            (prop::FONT_FEATURE_SETTINGS, keyword::NORMAL),
            (prop::FONT_VARIATION_SETTINGS, keyword::NORMAL),
            (prop::FONT_VARIANT_ALTERNATES, keyword::NORMAL),
            (prop::FONT_VARIANT_CAPS, keyword::NORMAL),
            (prop::FONT_VARIANT_EAST_ASIAN, keyword::NORMAL),
            (prop::FONT_VARIANT_EMOJI, keyword::NORMAL),
            (prop::FONT_VARIANT_LIGATURES, keyword::NORMAL),
            (prop::FONT_VARIANT_NUMERIC, keyword::NORMAL),
            (prop::FONT_VARIANT_POSITION, keyword::NORMAL),
            (prop::FONT_KERNING, keyword::AUTO),
            (prop::TEXT_RENDERING, keyword::AUTO),
        ] {
            if !matches!(value_of(&table, property), Some(StyleValueData::Keyword { keyword }) if *keyword == default_keyword)
            {
                self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
                return None;
            }
        }
        // The font size the element's own lengths resolve against is the C++ working set's, a
        // CSSPixels value, not the computed value's double.
        let font_size = match value_of(&table, prop::FONT_SIZE) {
            Some(StyleValueData::Length { value, unit }) if *unit == crate::css::style_compute::px_length_unit() => {
                CssPixels::nearest_value_for(*value).to_double()
            }
            _ => {
                self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
                return None;
            }
        };
        let font_size_raw = CssPixels::nearest_value_for(font_size).raw_value();
        let font_family = table.effective_value(None, prop::FONT_FAMILY, true).value;
        let font_slope = match value_of(&table, prop::FONT_STYLE) {
            Some(StyleValueData::FontStyle { font_style, .. }) => match *font_style {
                crate::css::css_enums::font_style_keyword::ITALIC => 1,
                crate::css::css_enums::font_style_keyword::OBLIQUE => 2,
                _ => 0,
            },
            _ => 0,
        };
        let (font_weight, font_width) = match (value_of(&table, prop::FONT_WEIGHT), value_of(&table, prop::FONT_WIDTH))
        {
            (Some(StyleValueData::Number { value: weight }), Some(StyleValueData::Percentage { value: width })) => {
                (*weight, *width)
            }
            _ => {
                self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
                return None;
            }
        };
        let font_optical_sizing = match value_of(&table, prop::FONT_OPTICAL_SIZING) {
            Some(StyleValueData::Keyword { keyword }) => {
                crate::css::css_enums::keyword_to_font_optical_sizing(*keyword).unwrap_or(0)
            }
            _ => 0,
        };
        let request = bridge::FfiFontResolutionRequest {
            font_family: font_family.cast(),
            font_size_raw,
            font_slope,
            font_weight,
            font_width,
            font_optical_sizing,
            font_environment_generation: inputs.font_environment_generation,
        };
        let Some(resolved) = self
            .font_resolver
            .as_mut()
            .and_then(|resolver| resolver.resolve(request))
        else {
            self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
            return None;
        };
        let own_metrics = |line_height: f64| FfiFontMetrics {
            font_size,
            x_height: drive_font_metric(resolved.x_height),
            cap_height: drive_font_metric(resolved.ascent),
            zero_advance: drive_font_metric(resolved.zero_advance),
            line_height,
        };

        let line_height_length = if is_document_element {
            length_context(
                own_metrics(parent_line_height_used),
                results.font_metrics_depend_on_viewport_metrics,
                own_metrics(parent_line_height_used),
                results.font_metrics_depend_on_viewport_metrics,
            )
        } else {
            length_context(
                own_metrics(parent_line_height_used),
                results.font_metrics_depend_on_viewport_metrics,
                document_root_font_metrics,
                inputs.root_font_metrics_depend_on_viewport_metrics,
            )
        };
        drive(
            &mut table,
            &mut results,
            &mut effective_color_scheme,
            LONGHAND_DRIVE_PHASE_LINE_HEIGHT,
            &raw const line_height_length,
            std::ptr::null(),
            std::ptr::null(),
        );
        drive(
            &mut table,
            &mut results,
            &mut effective_color_scheme,
            LONGHAND_DRIVE_PHASE_COLOR_SCHEME,
            std::ptr::null(),
            std::ptr::null(),
            std::ptr::null(),
        );
        effective_color_scheme = table.effective_color_scheme();

        // The used line height, as the C++ working set reads it from the computed value.
        let normal_line_height = f64::from(resolved.ascent.round() as i32 + resolved.descent.round() as i32);
        let line_height_used = |table: &ComputedLonghandTable| -> Option<f64> {
            match value_of(table, prop::LINE_HEIGHT)? {
                StyleValueData::Keyword { keyword } if *keyword == keyword::NORMAL => Some(normal_line_height),
                StyleValueData::Length { value, unit } if *unit == crate::css::style_compute::px_length_unit() => {
                    Some(CssPixels::nearest_value_for(*value).to_double())
                }
                StyleValueData::Number { value } => Some(CssPixels::nearest_value_for(value * font_size).to_double()),
                _ => None,
            }
        };
        let Some(line_height_before_adjustments) = line_height_used(&table) else {
            self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
            return None;
        };
        let remaining_length = length_context(
            own_metrics(line_height_before_adjustments),
            results.font_metrics_depend_on_viewport_metrics,
            document_root_font_metrics,
            inputs.root_font_metrics_depend_on_viewport_metrics,
        );
        let input_line_height_metrics = if has(fact::CHECK_INPUT_LINE_HEIGHT) {
            FfiInputLineHeightMetrics {
                current_line_height: line_height_before_adjustments,
                minimum_line_height: normal_line_height,
            }
        } else {
            FfiInputLineHeightMetrics {
                current_line_height: 0.0,
                minimum_line_height: 0.0,
            }
        };
        let line_height_value = table.effective_value(None, prop::LINE_HEIGHT, true).value;
        drive(
            &mut table,
            &mut results,
            &mut effective_color_scheme,
            LONGHAND_DRIVE_PHASE_REMAINING,
            &raw const remaining_length,
            &raw const input_line_height_metrics,
            line_height_value,
        );
        if results.explicitly_inherited_non_inherited_style_groups != 0 || results.uses_tree_counting_function {
            self.counters.bump(Counter::EngineComputedRecordBailDrive);
            return None;
        }
        let Some(line_height_used_after) = line_height_used(&table) else {
            self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
            return None;
        };
        let keyword_code = |property: u16, map: fn(u16) -> Option<u8>| match value_of(&table, property) {
            Some(StyleValueData::Keyword { keyword }) => map(*keyword).unwrap_or(0),
            _ => 0,
        };
        let math_depth = match value_of(&table, prop::MATH_DEPTH) {
            Some(StyleValueData::Integer { value }) => *value,
            _ => 0,
        };
        let font = FfiFontGroupBuildInputs {
            font_size_raw,
            line_height_used_raw: CssPixels::nearest_value_for(line_height_used_after).raw_value(),
            font_variant_emoji: keyword_code(
                prop::FONT_VARIANT_EMOJI,
                crate::css::css_enums::keyword_to_font_variant_emoji,
            ),
            font_ascent: resolved.ascent,
            font_descent: resolved.descent,
            font_x_height: resolved.x_height,
            font_zero_advance: resolved.zero_advance,
            first_available_font: resolved.first_available_font,
            font_cascade_list: resolved.font_cascade_list,
            font_weight,
            font_width,
            math_shift: keyword_code(prop::MATH_SHIFT, crate::css::css_enums::keyword_to_math_shift),
            math_style: keyword_code(prop::MATH_STYLE, crate::css::css_enums::keyword_to_math_style),
            math_depth,
        };
        let length = FfiLengthResolutionContext {
            resolved_viewport_relative_length: std::ptr::null_mut(),
            ..remaining_length
        };
        Some((table, length, results.longhand_evaluations, Some(font)))
    }

    /// Whether every cascade winner that moved on this node since its record was computed is a
    /// longhand the engine computes itself. Such a change cannot reach the node's descendants:
    /// nothing inherited moves, and no custom property does, so a descendant's engine-computed
    /// record stays exact even though this ancestor changes in the same batch.
    pub(super) fn winner_delta_is_engine_confined(&self, node: StyleNodeID) -> bool {
        let target = computed::ComputedStyleTarget::new(node, u8::MAX);
        let Lookup::Known((generation, state)) = self
            .winner_groups
            .token_for(WinnerGroupKey::current(node, self.program.version()))
        else {
            return false;
        };
        let Some((previous_generation, previous_state)) = self.computed_group_sets.cascade_state(target) else {
            return false;
        };
        if previous_generation != generation {
            return false;
        }
        // A descendant's engine-computed record is assembled before C++ applies the ancestor, so
        // nothing the descendant inherits may move; custom properties inherit unless registered
        // otherwise, and a child's box-type transformation reads its parent's display.
        self.winner_groups
            .semantic_delta(Some(previous_state), state)
            .properties()
            .iter()
            .all(|&property| {
                property != crate::css::property_metadata::property_id::CUSTOM
                    && property != crate::css::property_metadata::property_id::DISPLAY
                    && !crate::css::property_metadata::property_is_inherited(property)
            })
    }

    /// Publish the immutable computed-group payloads of one element's base style. This assigns
    /// dense identities to shared payloads and their ordered tuple, so an equal handle proves equal
    /// groups and downstream operators can consume one node handle.
    pub(crate) fn publish_computed_groups(
        &mut self,
        target: computed::ComputedStyleTarget,
        payloads: &[*const std::ffi::c_void],
        inherited_group_count: usize,
        custom_property_environment: u64,
        metadata_input: computed::ComputedMetadataInput<'_>,
    ) -> computed::ComputedGroupPublication {
        self.publish_computed_groups_impl(
            Some(target),
            payloads,
            inherited_group_count,
            custom_property_environment,
            metadata_input,
        )
    }

    pub(crate) fn assign_shared_style_record(
        &mut self,
        target: computed::ComputedStyleTarget,
        style_record: u64,
        inherited_group_count: usize,
        inherited_group_swap_eligible: bool,
    ) -> computed::ComputedGroupPublication {
        let group_count = self
            .computed_group_sets
            .style_record_payloads(style_record)
            .expect("a shared style record must name a live base record")
            .len();
        let current_cascade_state = self.computed_group_sets.take_pending_cascade_state(target);
        let publication = self
            .computed_group_sets
            .assign_shared_style_record(
                target,
                style_record,
                inherited_group_count,
                inherited_group_swap_eligible,
            )
            .expect("a shared style record must name a live base record");
        if let Some((current_generation, current_cascade_state)) = current_cascade_state {
            let previous_cascade_state = self
                .computed_group_sets
                .bind_cascade_state(target, (current_generation, current_cascade_state))
                .and_then(|(previous_generation, previous_state)| {
                    (previous_generation == current_generation).then_some(previous_state)
                });
            let delta = self
                .winner_groups
                .semantic_delta(previous_cascade_state, current_cascade_state);
            if delta.is_empty() {
                self.counters.bump(Counter::CascadeWinnerDeltaStops);
            } else {
                self.counters
                    .add(Counter::CascadeWinnerDeltaProperties, delta.properties().len() as u64);
                self.counters.add(
                    Counter::ComputedWinnerDeltaPropertiesConsumed,
                    delta.properties().len() as u64,
                );
                if !publication.node_handle_changed {
                    self.counters.bump(Counter::ComputedWinnerPropagationStops);
                }
            }
        } else {
            self.computed_group_sets.clear_cascade_state(target);
        }
        self.settle_computed_memory();
        self.counters.add(Counter::ComputedGroupsReused, group_count as u64);
        self.counters.bump(Counter::ComputedGroupSetsReused);
        self.counters.bump(Counter::InheritedGroupSetsReused);
        self.counters.bump(Counter::CustomPropertyEnvironmentsReused);
        self.counters.bump(Counter::ComputedFixedMetadataReused);
        self.counters.bump(Counter::StyleRecordsReused);
        if publication.node_handle_changed {
            self.counters.bump(Counter::ComputedGroupNodeHandlesPublished);
        }
        if publication.inherited_node_handle_changed {
            self.counters.bump(Counter::InheritedGroupNodeHandlesPublished);
        }
        if publication.custom_property_environment_node_handle_changed {
            self.counters
                .bump(Counter::CustomPropertyEnvironmentNodeHandlesPublished);
        }
        if publication.computed_fixed_metadata_node_handle_changed {
            self.counters.bump(Counter::ComputedFixedMetadataNodeHandlesPublished);
        }
        if publication.style_record_node_handle_changed {
            self.counters.bump(Counter::StyleRecordNodeHandlesPublished);
        }
        if publication.animation_overlay_slot_released {
            self.counters.bump(Counter::AnimationOverlaySlotsReleased);
        }
        self.counters.set(
            Counter::LiveAnimationOverlayRecords,
            publication.live_animation_overlay_records as u64,
        );
        if publication.is_pseudo && publication.style_record_node_handle_changed {
            self.counters.bump(Counter::ComputedPseudoAssignmentsPublished);
        }
        publication
    }

    /// Intern the immutable computed-group payloads of a style which has no live StyleEngine target.
    pub(crate) fn intern_computed_groups(
        &mut self,
        payloads: &[*const std::ffi::c_void],
        inherited_group_count: usize,
        custom_property_environment: u64,
        metadata_input: computed::ComputedMetadataInput<'_>,
    ) -> computed::ComputedGroupPublication {
        self.publish_computed_groups_impl(
            None,
            payloads,
            inherited_group_count,
            custom_property_environment,
            metadata_input,
        )
    }

    pub(crate) fn style_record_payloads(&self, style_record: u64) -> Option<&[*const std::ffi::c_void]> {
        self.computed_group_sets.style_record_payloads(style_record)
    }

    pub(crate) fn style_record_dependency_flags(&self, style_record: u64) -> Option<u8> {
        self.computed_group_sets.style_record_dependency_flags(style_record)
    }

    pub(crate) fn recording_computed_group_identities(&self, style_record: u64) -> Option<Vec<u32>> {
        #[cfg(feature = "style-recording")]
        return self.computed_group_sets.recording_group_identities(style_record);
        #[cfg(not(feature = "style-recording"))]
        {
            let _ = style_record;
            None
        }
    }

    pub(crate) fn recording_computed_group_retained_bytes(&self, style_record: u64) -> Option<Vec<u64>> {
        #[cfg(feature = "style-recording")]
        return self.computed_group_sets.recording_group_retained_bytes(style_record);
        #[cfg(not(feature = "style-recording"))]
        {
            let _ = style_record;
            None
        }
    }

    pub(crate) fn recording_computed_longhand_table(
        &self,
        style_record: u64,
    ) -> Option<(u32, &[*const std::ffi::c_void])> {
        #[cfg(feature = "style-recording")]
        return self.computed_group_sets.recording_longhand_table(style_record);
        #[cfg(not(feature = "style-recording"))]
        {
            let _ = style_record;
            None
        }
    }

    pub(crate) fn style_record_view(&self, style_record: u64) -> Option<computed::StyleRecordView<'_>> {
        self.computed_group_sets.style_record_view(style_record)
    }

    pub(crate) fn pin_style_record(&mut self, style_record: u64) {
        self.computed_group_sets.pin_style_record(style_record);
    }

    pub(crate) fn begin_style_record_view_epoch(&mut self) {
        self.computed_group_sets.begin_style_record_view_epoch();
    }

    pub(crate) fn end_style_record_view_epoch(&mut self) {
        self.computed_group_sets.end_style_record_view_epoch();
        self.reclaim_computed_memory_if_needed();
    }

    pub(super) fn settle_computed_memory(&mut self) {
        self.computed_group_sets.settle_nested_memory(&mut self.memory);
        self.computed_group_set_memory.resize_required_to(
            &mut self.memory,
            self.computed_group_sets.group_set_header_capacity_bytes(),
        );
        self.custom_property_environment_memory.resize_required_to(
            &mut self.memory,
            self.computed_group_sets.custom_property_environment_capacity_bytes(),
        );
        self.computed_fixed_metadata_memory.resize_required_to(
            &mut self.memory,
            self.computed_group_sets.computed_fixed_metadata_capacity_bytes(),
        );
        self.computed_longhand_table_memory.resize_required_to(
            &mut self.memory,
            self.computed_group_sets.longhand_table_header_capacity_bytes(),
        );
        self.style_record_memory
            .resize_required_to(&mut self.memory, self.computed_group_sets.style_record_capacity_bytes());
        self.animation_overlay_memory.resize_required_to(
            &mut self.memory,
            self.computed_group_sets.animation_overlay_header_capacity_bytes(),
        );
        self.computed_pseudo_assignment_memory.resize_required_to(
            &mut self.memory,
            self.computed_group_sets.pseudo_assignment_header_capacity_bytes(),
        );
    }

    pub(super) fn reclaim_computed_memory_if_needed(&mut self) {
        // Recording dictionaries are keyed by computed identities. Reusing an identity for new
        // semantics would make later events refer to the first definition replay saw for it.
        if self.recording_id().is_none()
            && let Some(retention) = self.computed_group_sets.reclaim_unreachable_if_needed()
        {
            self.counters
                .set(Counter::ComputedGroupsRetained, retention.retained as u64);
            self.counters
                .set(Counter::ComputedGroupsReachable, retention.reachable as u64);
        }
        self.settle_computed_memory();
    }

    pub(crate) fn unpin_style_record(&mut self, style_record: u64) {
        self.computed_group_sets.unpin_style_record(style_record);
    }

    pub(super) fn publish_computed_groups_impl(
        &mut self,
        target: Option<computed::ComputedStyleTarget>,
        payloads: &[*const std::ffi::c_void],
        inherited_group_count: usize,
        custom_property_environment: u64,
        metadata_input: computed::ComputedMetadataInput<'_>,
    ) -> computed::ComputedGroupPublication {
        let current_cascade_state =
            target.and_then(|target| self.computed_group_sets.take_pending_cascade_state(target));
        let is_base_record = metadata_input.animation_overlay_identity == 0;
        let pseudo_styles = metadata_input.pseudo_element_styles;
        let publication = self.computed_group_sets.publish(
            target,
            payloads,
            inherited_group_count,
            custom_property_environment,
            metadata_input,
        );
        if let Some((current_generation, current_cascade_state)) = current_cascade_state {
            let target = target.expect("only a target has pending cascade state");
            let previous_cascade_state = self
                .computed_group_sets
                .bind_cascade_state(target, (current_generation, current_cascade_state))
                .and_then(|(previous_generation, previous_state)| {
                    (previous_generation == current_generation).then_some(previous_state)
                });
            self.remember_cold_record_candidate(
                target,
                (current_generation, current_cascade_state),
                custom_property_environment,
                pseudo_styles,
                publication.style_record_identity,
                is_base_record,
            );
            let delta = self
                .winner_groups
                .semantic_delta(previous_cascade_state, current_cascade_state);
            if delta.is_empty() {
                self.counters.bump(Counter::CascadeWinnerDeltaStops);
            } else {
                self.counters
                    .add(Counter::CascadeWinnerDeltaProperties, delta.properties().len() as u64);
                self.counters.add(
                    Counter::ComputedWinnerDeltaPropertiesConsumed,
                    delta.properties().len() as u64,
                );
                if !publication.node_handle_changed {
                    self.counters.bump(Counter::ComputedWinnerPropagationStops);
                }
            }
        } else if let Some(target) = target {
            self.computed_group_sets.clear_cascade_state(target);
        }
        self.settle_computed_memory();
        self.counters.add(
            Counter::ComputedOutputGroupsCanonicalized,
            publication.canonical_output_groups_reused as u64,
        );
        self.counters.add(
            Counter::ComputedGroupsReused,
            (payloads.len() - publication.new_groups) as u64,
        );
        if !publication.new_group_set {
            self.counters.bump(Counter::ComputedGroupSetsReused);
        }
        if !publication.new_inherited_group_set {
            self.counters.bump(Counter::InheritedGroupSetsReused);
        }
        if publication.node_handle_changed {
            self.counters.bump(Counter::ComputedGroupNodeHandlesPublished);
        }
        if publication.inherited_node_handle_changed {
            self.counters.bump(Counter::InheritedGroupNodeHandlesPublished);
        }
        if !publication.new_custom_property_environment {
            self.counters.bump(Counter::CustomPropertyEnvironmentsReused);
        }
        if publication.custom_property_environment_node_handle_changed {
            self.counters
                .bump(Counter::CustomPropertyEnvironmentNodeHandlesPublished);
        }
        match publication.new_computed_fixed_metadata {
            true => self.counters.bump(Counter::ComputedFixedMetadataInterned),
            false => self.counters.bump(Counter::ComputedFixedMetadataReused),
        }
        if publication.computed_fixed_metadata_node_handle_changed {
            self.counters.bump(Counter::ComputedFixedMetadataNodeHandlesPublished);
        }
        match publication.new_style_record {
            true => self.counters.bump(Counter::StyleRecordsInterned),
            false => self.counters.bump(Counter::StyleRecordsReused),
        }
        if publication.style_record_node_handle_changed {
            self.counters.bump(Counter::StyleRecordNodeHandlesPublished);
        }
        if publication.animation_overlay_slot_allocated {
            self.counters.bump(Counter::AnimationOverlaySlotsAllocated);
        }
        if publication.animation_overlay_slot_released {
            self.counters.bump(Counter::AnimationOverlaySlotsReleased);
        }
        if publication.animation_overlay_record_updated {
            self.counters.bump(Counter::AnimationOverlayRecordsUpdated);
        }
        self.counters.set(
            Counter::LiveAnimationOverlayRecords,
            publication.live_animation_overlay_records as u64,
        );
        if publication.is_pseudo && publication.style_record_node_handle_changed {
            self.counters.bump(Counter::ComputedPseudoAssignmentsPublished);
        }
        publication
    }

    pub(super) fn publish_animation_overlay_impl(
        &mut self,
        target: computed::ComputedStyleTarget,
        source_identity: u64,
        animated_overlay: *const crate::css::animated_overlay::AnimatedOverlay,
        payloads: &[*const std::ffi::c_void],
    ) -> Option<computed::AnimationOverlayUpdate> {
        if self.recording_id().is_some() {
            return None;
        }
        let publication =
            self.computed_group_sets
                .publish_animation_overlay(target, source_identity, animated_overlay, payloads)?;
        self.settle_computed_memory();
        if publication.slot_allocated {
            self.counters.bump(Counter::AnimationOverlaySlotsAllocated);
        }
        if publication.slot_released {
            self.counters.bump(Counter::AnimationOverlaySlotsReleased);
        }
        if publication.record_updated {
            self.counters.bump(Counter::AnimationOverlayRecordsUpdated);
        }
        self.counters
            .set(Counter::LiveAnimationOverlayRecords, publication.live_records as u64);
        Some(publication)
    }

    pub(crate) fn publish_exact_cascade_state(
        &mut self,
        target: computed::ComputedStyleTarget,
        store: &CascadedPropertyStore,
        inherited_style_groups: u8,
        donor: Option<ExactCascadeDonor>,
    ) -> (bridge::FfiExactCascadePublication, Vec<(u16, SpecifiedWinnerKey)>, bool) {
        let context = self.prepare_exact_cascade_publication(target, donor);
        let had_previous = context.previous.is_some();
        let exact_winners = store
            .winning_declarations()
            .map(|(property, value_pointer, origin, important)| {
                let value = unsafe { self.intern_exact_specified_value(value_pointer) };
                (
                    property,
                    SpecifiedWinnerKey {
                        value,
                        operator: unsafe { Self::cascade_operator_of_style_value(value_pointer) },
                        continuation: cascade::CascadeContinuationID::default(),
                        animation_relevance: match origin {
                            CascadeOrigin::Animation => 1,
                            CascadeOrigin::Transition => 2,
                            _ => 0,
                        },
                        important,
                    },
                )
            })
            .collect::<Vec<_>>();
        let publication =
            self.publish_exact_cascade_winners_with_context(target, &exact_winners, inherited_style_groups, context);
        (publication, exact_winners, had_previous)
    }

    pub(crate) fn materialize_retained_cascade_state(
        &mut self,
        target: computed::ComputedStyleTarget,
        store: &mut CascadedPropertyStore,
        blocks: &[FfiCascadeBlock],
    ) -> Vec<FfiSourceSlotAssignment> {
        let Some(answer) = self.published_match_answers.lookup(target.node()) else {
            return Vec::new();
        };
        if !answer.cascade_winners_are_complete {
            return Vec::new();
        }
        let key = target.pseudo_element_target().map_or_else(
            || WinnerGroupKey::current(target.node(), self.program.version()),
            |pseudo| WinnerGroupKey::current_pseudo(target.node(), pseudo, self.program.version()),
        );
        let Lookup::Known((_, state)) = self.winner_groups.token_for(key) else {
            return Vec::new();
        };

        let mut assignments = Vec::new();
        for winner in self.winner_groups.winners_in_state(state) {
            let Some(winner) = self.winner_groups.resolved_winner(winner) else {
                continue;
            };
            if !Self::retained_store_supports_property(target, winner.property) {
                continue;
            }
            let block = match winner.source {
                WinnerSource::Rule(rule) => {
                    let Some(block) = blocks.iter().find(|block| block.style_engine_rule_id == rule.0 + 1) else {
                        continue;
                    };
                    block
                }
                WinnerSource::Element(ElementDeclarationKind::InlineStyle) => {
                    let Some(block) = blocks.iter().find(|block| block.is_inline_style) else {
                        continue;
                    };
                    block
                }
                WinnerSource::Element(
                    ElementDeclarationKind::PresentationalHint | ElementDeclarationKind::SvgPresentationAttribute,
                ) => {
                    let Some(block) = blocks
                        .iter()
                        .find(|block| block.origin == CascadeOrigin::AuthorPresentationalHint)
                    else {
                        continue;
                    };
                    block
                }
                WinnerSource::ExactCascade => continue,
            };
            let declarations = unsafe { std::slice::from_raw_parts(block.declarations, block.declaration_count) };
            let mut source_declarations = declarations.iter().filter(|declaration| {
                declaration.property_id == winner.property && declaration.important == winner.important
            });
            let Some(declaration) = source_declarations.next() else {
                continue;
            };
            if source_declarations.next().is_some()
                || !unsafe {
                    self.specified_values
                        .ensure_identity(declaration.data.cast(), winner.key.value, &mut self.memory)
                }
            {
                continue;
            }
            let value = unsafe { &*(declaration.data as *const StyleValueData) };
            if matches!(
                value,
                StyleValueData::Shorthand { .. }
                    | StyleValueData::Unresolved { .. }
                    | StyleValueData::PendingSubstitution { .. }
            ) {
                continue;
            }
            let retained = unsafe {
                RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(
                    declaration.data.cast(),
                ))
            };
            let slot = store.seed_retained_property(
                winner.property,
                retained,
                winner.important,
                declaration.has_style_sheet_context,
            );
            assignments.push(FfiSourceSlotAssignment {
                slot,
                source_id: block.source_id,
            });
        }
        assignments
    }

    pub(crate) fn exact_cascade_generation_snapshot(
        &self,
        target: computed::ComputedStyleTarget,
    ) -> (u64, Option<u64>) {
        (
            self.winner_groups.generation(),
            self.computed_group_sets
                .cascade_state(target)
                .map(|(generation, _)| generation),
        )
    }

    #[cfg(feature = "style-recording")]
    pub(crate) fn publish_exact_cascade_winners(
        &mut self,
        target: computed::ComputedStyleTarget,
        exact_winners: &[(u16, SpecifiedWinnerKey)],
        inherited_style_groups: u8,
        donor: Option<ExactCascadeDonor>,
    ) -> (bridge::FfiExactCascadePublication, bool) {
        let context = self.prepare_exact_cascade_publication(target, donor);
        let had_previous = context.previous.is_some();
        (
            self.publish_exact_cascade_winners_with_context(target, exact_winners, inherited_style_groups, context),
            had_previous,
        )
    }

    pub(super) fn prepare_exact_cascade_publication(
        &mut self,
        target: computed::ComputedStyleTarget,
        donor: Option<ExactCascadeDonor>,
    ) -> ExactCascadeContext {
        let generation = self.winner_groups.generation();
        let mut previous =
            self.computed_group_sets
                .cascade_state(target)
                .and_then(|(previous_generation, previous_state)| {
                    (previous_generation == generation).then_some(previous_state)
                });
        let mut dependency_target = target;
        let mut donor_used = false;
        if !target.is_pseudo()
            && let Some(donor) = donor
            && self
                .computed_group_sets
                .assigned_style_record(donor.node)
                .is_some_and(|record| record.raw() == donor.style_record)
        {
            let donor_target = computed::ComputedStyleTarget::new(donor.node, u8::MAX);
            if let Some(state) = self
                .computed_group_sets
                .cascade_state(donor_target)
                .and_then(|(donor_generation, donor_state)| (donor_generation == generation).then_some(donor_state))
            {
                previous = Some(state);
                dependency_target = donor_target;
                donor_used = true;
            }
        }
        let winner_key = target.pseudo_element_target().map_or_else(
            || WinnerGroupKey::current(target.node(), self.program.version()),
            |pseudo| WinnerGroupKey::current_pseudo(target.node(), pseudo, self.program.version()),
        );
        let lower_bound_state = self
            .winner_groups
            .token_for(winner_key)
            .sparse()
            .ok()
            .map(|(_, state)| state);
        if target.is_pseudo() {
            self.computed_group_sets
                .observe_pseudo_retained_cascade_state(target, lower_bound_state.map(|state| (generation, state)));
            self.settle_computed_memory();
        }
        ExactCascadeContext {
            previous,
            lower_bound_state,
            dependency_target,
            donor_used,
        }
    }

    pub(super) fn publish_exact_cascade_winners_with_context(
        &mut self,
        target: computed::ComputedStyleTarget,
        exact_winners: &[(u16, SpecifiedWinnerKey)],
        inherited_style_groups: u8,
        context: ExactCascadeContext,
    ) -> bridge::FfiExactCascadePublication {
        let ExactCascadeContext {
            previous,
            lower_bound_state,
            dependency_target,
            donor_used,
        } = context;
        let mut winners = Vec::with_capacity(exact_winners.len());
        for &(property, key) in exact_winners {
            let lower_bound_winner = lower_bound_state
                .and_then(|state| self.winner_groups.winner_in_state(state, property))
                .filter(|winner| {
                    self.winner_groups.resolved_winner(*winner).is_some_and(|resolved| {
                        resolved.key == key
                            && matches!(self.specified_values.value(resolved.key.value), Lookup::Known(_))
                    })
                });
            winners.push(lower_bound_winner.unwrap_or(PropertyWinner {
                property,
                important: false,
                key,
                priority: CascadePriority::exact_output_placeholder(),
                source: WinnerSource::ExactCascade,
            }));
        }
        verify_cascade_winners(self, |verifier| {
            let Some(lower_bound_state) = lower_bound_state else {
                return;
            };
            if !verifier
                .published_match_answers
                .lookup(target.node())
                .is_some_and(|answer| answer.cascade_winners_are_complete)
            {
                return;
            }
            let retained = Rc::clone(
                verifier
                    .retained_match_answer(target.node())
                    .sparse()
                    .expect("a complete published answer retains its exact input"),
            );
            for &(property, exact_key) in exact_winners {
                let Some(maintained_winner) = self.winner_groups.winner_in_state(lower_bound_state, property) else {
                    continue;
                };
                let resolved_winner = self.winner_groups.resolved_winner(maintained_winner);
                if exact_key.animation_relevance != 0 || resolved_winner.is_some_and(|winner| winner.key == exact_key) {
                    continue;
                }
                if matches!(
                    maintained_winner.key.operator,
                    CascadeOperator::Revert | CascadeOperator::RevertLayer
                ) && maintained_winner.key.continuation != cascade::CascadeContinuationID::default()
                    && resolved_winner.is_none()
                    && matches!(exact_key.operator, CascadeOperator::Initial | CascadeOperator::Inherit)
                {
                    continue;
                }
                let mut saw_declaration = false;
                let mut declarations_are_unanimous = true;
                let mut inspect = |declared: &DeclaredProperty| {
                    if declared.property == property {
                        saw_declaration = true;
                        declarations_are_unanimous &=
                            declared.value == exact_key.value && declared.operator == exact_key.operator;
                    }
                };
                for matched in retained.iter().filter(|matched| {
                    verifier
                        .programs
                        .get(matched.program)
                        .entries()
                        .get(matched.entry as usize)
                        .is_some_and(|entry| entry.pseudo_element == target.pseudo_element_target())
                }) {
                    verifier
                        .program
                        .declared_properties_of(matched.rule)
                        .iter()
                        .for_each(&mut inspect);
                }
                if !target.is_pseudo() {
                    for kind in ElementDeclarationKind::ALL {
                        verifier
                            .facts
                            .element_declared_properties(target.node(), kind)
                            .0
                            .iter()
                            .for_each(&mut inspect);
                    }
                }
                let unresolved_continuation = matches!(
                    maintained_winner.key.operator,
                    CascadeOperator::Revert | CascadeOperator::RevertLayer
                ) && maintained_winner.key.continuation
                    == cascade::CascadeContinuationID::default();
                let unanimous_declared_mismatch = saw_declaration
                    && declarations_are_unanimous
                    && resolved_winner.is_some_and(|winner| winner.key.operator == CascadeOperator::Declared);
                if !unresolved_continuation && !unanimous_declared_mismatch {
                    continue;
                }
                panic!(
                    "maintained cascade winner {maintained_winner:?}, resolved as {resolved_winner:?}, differs from exact legacy input {exact_key:?} for {target:?}, property {property}; retained input has {} rules",
                    retained.len()
                );
            }
        });
        let state = self.intern_cascade_state(&winners, previous);
        self.winner_groups.settle_memory(&mut self.memory);
        let generation = self.winner_groups.generation();
        let delta = self.winner_groups.semantic_delta(previous, state);
        let unchanged = previous.is_some() && delta.is_empty() && !donor_used;
        let mut computed_property_words = [0u64; crate::css::property_metadata::LONGHAND_WORD_COUNT];
        for property in delta.properties().iter().copied() {
            let Some(index) = property
                .checked_sub(crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID)
                .map(usize::from)
                .filter(|&index| index < crate::css::property_metadata::NUMBER_OF_LONGHAND_PROPERTIES)
            else {
                computed_property_words.fill(u64::MAX);
                break;
            };
            computed_property_words[index / 64] |= 1 << (index % 64);
        }
        let current_color_dependency_mask = delta
            .properties()
            .contains(&crate::css::property_metadata::property_id::COLOR)
            .then(|| {
                self.computed_group_sets
                    .current_color_dependency_mask(dependency_target)
            });
        let current_color_dependency_properties = delta
            .properties()
            .contains(&crate::css::property_metadata::property_id::COLOR)
            .then(|| {
                self.computed_group_sets
                    .current_color_dependency_properties(dependency_target)
            });
        let mut computed_property_closure_is_exact = delta.properties().len() == 1
            && current_color_dependency_properties.is_some_and(|properties| properties.is_some());
        if let Some(Some(dependencies)) = current_color_dependency_properties {
            for (word, dependencies) in computed_property_words.iter_mut().zip(dependencies) {
                *word |= dependencies;
            }
        }
        // caret-color and accent-color bake used values resolved against the element's own color
        // into their group fields, even for their initial `auto`. A color change re-evaluates them
        // whether or not either property is declared anywhere.
        if delta
            .properties()
            .contains(&crate::css::property_metadata::property_id::COLOR)
        {
            for property in [
                crate::css::property_metadata::property_id::CARET_COLOR,
                crate::css::property_metadata::property_id::ACCENT_COLOR,
            ] {
                let index = usize::from(property - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID);
                computed_property_words[index / 64] |= 1 << (index % 64);
            }
        }
        let color_scheme_dependency_mask = delta
            .properties()
            .contains(&crate::css::property_metadata::property_id::COLOR_SCHEME)
            .then(|| self.computed_group_sets.color_scheme_dependency_mask(dependency_target));
        let color_scheme_dependency_properties = delta
            .properties()
            .contains(&crate::css::property_metadata::property_id::COLOR_SCHEME)
            .then(|| {
                self.computed_group_sets
                    .color_scheme_dependency_properties(dependency_target)
            });
        if delta.properties().len() == 1 {
            computed_property_closure_is_exact |=
                color_scheme_dependency_properties.is_some_and(|properties| properties.is_some());
        }
        if let Some(Some(dependencies)) = color_scheme_dependency_properties {
            for (word, dependencies) in computed_property_words.iter_mut().zip(dependencies) {
                *word |= dependencies;
            }
        }
        let font_group_mask = computed_group_output_mask(crate::css::property_metadata::property_id::FONT_SIZE);
        let font_dependency_mask = font_group_mask.and_then(|font_group_mask| {
            delta
                .properties()
                .iter()
                .copied()
                .any(|property| computed_group_output_mask(property) == Some(font_group_mask))
                .then(|| self.computed_group_sets.font_dependency_mask(dependency_target))
        });
        let font_dependency_properties = font_group_mask.and_then(|font_group_mask| {
            delta
                .properties()
                .iter()
                .copied()
                .any(|property| computed_group_output_mask(property) == Some(font_group_mask))
                .then(|| self.computed_group_sets.font_dependency_properties(dependency_target))
        });
        if delta.properties().len() == 1 {
            computed_property_closure_is_exact |=
                font_dependency_properties.is_some_and(|properties| properties.is_some());
        }
        if let Some(Some(dependencies)) = font_dependency_properties {
            for (word, dependencies) in computed_property_words.iter_mut().zip(dependencies) {
                *word |= dependencies;
            }
        }
        const INHERITED_STATIC_GROUPS: u8 = (1 << 0) | (1 << 1) | (1 << 3);
        const INHERITED_UI_GROUP: u8 = 1 << 2;
        const INHERITED_TEXT_GROUP: u8 = 1 << 4;
        const INHERITED_GROUPS_WITH_COMPUTED_CLOSURE: u8 =
            INHERITED_STATIC_GROUPS | INHERITED_UI_GROUP | INHERITED_TEXT_GROUP;
        let inherited_property_closure_requested = delta.is_empty()
            && inherited_style_groups != 0
            && inherited_style_groups & !INHERITED_GROUPS_WITH_COMPUTED_CLOSURE == 0;
        let inherited_current_color_dependency_mask =
            (inherited_property_closure_requested && inherited_style_groups & INHERITED_TEXT_GROUP != 0).then(|| {
                self.computed_group_sets
                    .current_color_dependency_mask(dependency_target)
            });
        let inherited_current_color_dependency_properties =
            (inherited_property_closure_requested && inherited_style_groups & INHERITED_TEXT_GROUP != 0).then(|| {
                self.computed_group_sets
                    .current_color_dependency_properties(dependency_target)
            });
        let inherited_color_scheme_dependency_mask = (inherited_property_closure_requested
            && inherited_style_groups & INHERITED_UI_GROUP != 0)
            .then(|| self.computed_group_sets.color_scheme_dependency_mask(dependency_target));
        let inherited_color_scheme_dependency_properties =
            (inherited_property_closure_requested && inherited_style_groups & INHERITED_UI_GROUP != 0).then(|| {
                self.computed_group_sets
                    .color_scheme_dependency_properties(dependency_target)
            });
        let inherited_property_closure_is_exact = inherited_property_closure_requested
            && (inherited_style_groups & INHERITED_TEXT_GROUP == 0
                || inherited_current_color_dependency_properties.is_some_and(|properties| properties.is_some()))
            && (inherited_style_groups & INHERITED_UI_GROUP == 0
                || inherited_color_scheme_dependency_properties.is_some_and(|properties| properties.is_some()));
        if inherited_property_closure_is_exact {
            for property in crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
                ..=crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID
            {
                if computed_group_output_mask(property)
                    .is_some_and(|groups| groups & u32::from(inherited_style_groups) != 0)
                {
                    let index = usize::from(property - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID);
                    computed_property_words[index / 64] |= 1 << (index % 64);
                }
            }
            for dependencies in [
                inherited_current_color_dependency_properties,
                inherited_color_scheme_dependency_properties,
            ]
            .into_iter()
            .flatten()
            .flatten()
            {
                for (word, dependencies) in computed_property_words.iter_mut().zip(dependencies) {
                    *word |= dependencies;
                }
            }
            computed_property_closure_is_exact = true;
        }
        let inherited_computed_group_mask = if inherited_property_closure_is_exact {
            [
                inherited_current_color_dependency_mask,
                inherited_color_scheme_dependency_mask,
            ]
            .into_iter()
            .flatten()
            .flatten()
            .fold(u32::from(inherited_style_groups), |mask, dependencies| {
                mask | dependencies
            })
        } else {
            0
        };
        let computed_group_mask = previous.map_or(u32::MAX, |_| {
            // NB: Inherited groups the closure cannot answer exactly forbid narrowing: the caller
            //     passes every changed inherited group whenever its group swap may decline, and a
            //     mask that silently dropped an unanswered group would let the masked recompute
            //     keep stale values for it.
            if inherited_style_groups != 0 && !inherited_property_closure_is_exact {
                return u32::MAX;
            }
            delta
                .properties()
                .iter()
                .copied()
                .try_fold(0, |mask, property| {
                    let groups = computed_group_dependency_mask(property)?;
                    let dynamic_groups = if property == crate::css::property_metadata::property_id::COLOR {
                        current_color_dependency_mask.flatten()?
                            | computed_group_output_mask(crate::css::property_metadata::property_id::CARET_COLOR)
                                .unwrap_or(u32::MAX)
                    } else if property == crate::css::property_metadata::property_id::COLOR_SCHEME {
                        color_scheme_dependency_mask.flatten()?
                    } else if Some(groups) == font_group_mask {
                        font_dependency_mask.flatten()?
                    } else {
                        0
                    };
                    Some(mask | groups | dynamic_groups)
                })
                .unwrap_or(u32::MAX)
                | inherited_computed_group_mask
        });
        self.computed_group_sets
            .set_pending_cascade_state(target, (generation, state));
        let selection = StyleComputationSelection {
            computed_property_words,
            computed_property_closure_is_exact,
        };
        if target.is_pseudo() {
            let selections = self
                .pending_pseudo_style_computation_selections
                .entry(target.node())
                .or_default();
            match selections.iter_mut().find(|(kind, _)| *kind == target.pseudo_kind()) {
                Some((_, existing)) => *existing = selection,
                None => selections.push((target.pseudo_kind(), selection)),
            }
        } else {
            self.pending_element_style_computation_selections
                .insert(target.node(), selection);
        }
        bridge::FfiExactCascadePublication {
            unchanged,
            computed_group_mask,
            donor_used,
        }
    }

    pub(crate) fn pending_style_computation_selection(
        &self,
        node: StyleNodeID,
        pseudo_kind: u8,
    ) -> Option<StyleComputationSelection> {
        let target = computed::ComputedStyleTarget::new(node, pseudo_kind);
        if !target.is_pseudo() {
            return self.pending_element_style_computation_selections.get(&node).copied();
        }
        self.pending_pseudo_style_computation_selections
            .get(&node)?
            .iter()
            .find(|(kind, _)| *kind == pseudo_kind)
            .map(|(_, selection)| *selection)
    }

    unsafe fn cascade_operator_of_style_value(value: *const StyleValueData) -> CascadeOperator {
        let StyleValueData::Keyword { keyword } = (unsafe { &*value }) else {
            return CascadeOperator::Declared;
        };
        match *keyword {
            crate::css::style_compute::keyword::INHERIT => CascadeOperator::Inherit,
            crate::css::style_compute::keyword::INITIAL => CascadeOperator::Initial,
            crate::css::style_compute::keyword::UNSET => CascadeOperator::Unset,
            crate::css::style_compute::keyword::REVERT => CascadeOperator::Revert,
            crate::css::style_compute::keyword::REVERT_LAYER => CascadeOperator::RevertLayer,
            _ => CascadeOperator::Declared,
        }
    }

    /// Carry the exact winner state behind a style an element hands back unchanged into its next
    /// publication. Reuse is granted only when the record's inputs are the ones the cascade ran
    /// on, so the state bound by that cascade still describes the element's winners.
    pub(crate) fn retain_exact_cascade_state(&mut self, node: StyleNodeID) {
        let target = computed::ComputedStyleTarget::new(node, u8::MAX);
        let Some((generation, state)) = self.computed_group_sets.cascade_state(target) else {
            return;
        };
        verify_cascade_winners(self, |engine| {
            if let Lookup::Known(current) = engine
                .winner_groups
                .token_for(WinnerGroupKey::current(node, engine.program.version()))
            {
                assert_eq!(
                    current,
                    (generation, state),
                    "a reused style must keep the winner state its cascade bound"
                );
            }
        });
        self.computed_group_sets
            .set_pending_cascade_state(target, (generation, state));
    }

    /// Bind an exact winner state to a style-sharing publication which consumes the same complete
    /// cascade input without running the C++ cascade.
    pub(crate) fn prepare_shared_exact_cascade_state(&mut self, node: StyleNodeID) {
        let published_answer_is_complete = self
            .published_match_answers
            .lookup(node)
            .is_some_and(|answer| answer.cascade_winners_are_complete);
        let retained_answer_is_complete = || {
            self.batch_matching_traversal.as_ref()?;
            let retained = match self.retained_match_answer(node) {
                Lookup::Known(answer) => answer,
                Lookup::KnownAbsent | Lookup::Missing(_) => return None,
            };
            // Completeness depends only on rule inventory and scope. Cascade rank,
            // specificity, and the materialized node never participate.
            for entry in retained.iter() {
                self.programs.get(entry.program).entries().get(entry.entry as usize)?;
                if self.program.rule_is_gated_by_container_query(entry.rule)
                    || !self.program.declarations_are_complete_for(entry.rule)
                    || entry.tree_scope != TreeScopeID::DOCUMENT
                {
                    return Some(false);
                }
            }
            Some(
                ElementDeclarationKind::ALL
                    .iter()
                    .all(|&kind| self.facts.element_declared_properties(node, kind).1),
            )
        };
        if !published_answer_is_complete && retained_answer_is_complete() != Some(true) {
            return;
        }
        let (generation, state) = match self
            .winner_groups
            .token_for(WinnerGroupKey::current(node, self.program.version()))
        {
            Lookup::Known(state) => state,
            Lookup::KnownAbsent | Lookup::Missing(_) => return,
        };
        self.computed_group_sets
            .set_pending_cascade_state(computed::ComputedStyleTarget::new(node, u8::MAX), (generation, state));
    }

    pub(crate) fn discard_pending_exact_cascade_state(&mut self, target: computed::ComputedStyleTarget) {
        self.computed_group_sets.take_pending_cascade_state(target);
        self.remove_pending_style_computation_selection(target);
    }

    fn remove_pending_style_computation_selection(&mut self, target: computed::ComputedStyleTarget) {
        if !target.is_pseudo() {
            self.pending_element_style_computation_selections.remove(&target.node());
            return;
        }
        let Some(selections) = self.pending_pseudo_style_computation_selections.get_mut(&target.node()) else {
            return;
        };
        selections.retain(|(kind, _)| *kind != target.pseudo_kind());
        if selections.is_empty() {
            self.pending_pseudo_style_computation_selections.remove(&target.node());
        }
    }

    pub(crate) fn remove_computed_pseudo(
        &mut self,
        node: StyleNodeID,
        pseudo_kind: u8,
    ) -> Option<computed::FinalStyleRecordID> {
        let target = computed::ComputedStyleTarget::new(node, pseudo_kind);
        self.remove_pending_style_computation_selection(target);
        if let Some(state) = self.computed_group_sets.take_pending_cascade_state(target) {
            self.computed_group_sets
                .observe_absent_pseudo_cascade_state(target, state);
        }
        let live_animation_overlays_before = self.computed_group_sets.live_animation_overlay_records();
        let removed_style_record = self.computed_group_sets.remove_pseudo(node, pseudo_kind);
        let live_animation_overlays_after = self.computed_group_sets.live_animation_overlay_records();
        self.settle_computed_memory();
        let removed_style_record = removed_style_record?;
        self.counters.bump(Counter::ComputedPseudoAssignmentsRemoved);
        self.counters.bump(Counter::StyleRecordNodeHandlesPublished);
        self.counters.add(
            Counter::AnimationOverlaySlotsReleased,
            (live_animation_overlays_before - live_animation_overlays_after) as u64,
        );
        self.counters.set(
            Counter::LiveAnimationOverlayRecords,
            live_animation_overlays_after as u64,
        );
        Some(removed_style_record)
    }
}

/// What a first record was derived from: the parent's side of the computation, the winner state
/// (with the generation its identity belongs to), the element facts, the pseudo-elements the
/// element has rules for, and the font environment.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) struct ColdRecordKey {
    parent: ColdRecordParent,
    generation: u64,
    state: CascadeStateID,
    facts: u32,
    /// The pseudo-elements the element has rules for: the record's metadata says which, and
    /// C++ computes their styles beside it.
    pseudo_styles: u64,
    font_environment_generation: u64,
}

/// What a first record reads of the parent's style: its inherited groups, its custom-property
/// environment, its dependency flags, and the display the box-type transformation takes as the
/// parent's. A state that explicitly inherits a non-inherited property reads the parent's whole
/// table, so it keys on the parent's record instead.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct ColdRecordParent {
    record: u64,
    inherited_groups: u32,
    environment: u64,
    dependency_flags: u8,
    parent_display: u32,
}

/// A first record the engine keeps for reuse, with the swap eligibility its assignment carries.
#[derive(Clone, Copy)]
pub(super) struct ColdRecord {
    record: computed::FinalStyleRecordID,
    swap_eligible: bool,
}

/// First records the engine keeps for reuse; cleared wholesale past this many entries.
const COLD_RECORD_CACHE_LIMIT: usize = 4096;

/// A record the engine derived for a published reaction, awaiting C++'s installation.
pub(super) struct PendingEngineComputedRecord {
    node: StyleNodeID,
    /// The pseudo-element the record is for, or `u8::MAX` for the element's own.
    pseudo_kind: u8,
    old_style_record: computed::FinalStyleRecordID,
    new_style_record: computed::FinalStyleRecordID,
    /// The winner state the record was derived from; an implicit marker without rules has none.
    cascade_state: Option<(u64, CascadeStateID)>,
    /// Longhands the drive evaluated for the record; counted once C++ installs it.
    longhand_evaluations: u32,
}

/// What one flush accumulates while deriving engine-computed records: the record each cohort
/// (an old record moved to a winner state) derived, and the cascade each winner state describes.
/// Which of a parent's inputs to its children's records may have moved under a record.
#[derive(Clone, Copy, Default)]
pub(super) struct ParentInputsMoved {
    /// The parent's inherited style groups.
    pub(super) inherited_style: bool,
    /// The parent's display, which the children's box-type transformation reads.
    pub(super) display: bool,
}

impl ParentInputsMoved {
    pub(super) fn any(self) -> bool {
        self.inherited_style || self.display
    }
}

#[derive(Default)]
pub(super) struct EngineComputedRecordScratch {
    pub(super) cohorts: HashMap<(u64, CascadeStateID, u32, u64), computed::FinalStyleRecordID>,
    /// The nodes whose record this flush settled: what their descendants inherit from is in
    /// place.
    pub(super) settled_nodes: HashSet<StyleNodeID>,
    /// First records derived this flush, by what they were derived from.
    pub(super) cold_cohorts: HashMap<ColdRecordKey, ColdRecord>,
    pub(super) stores: HashMap<CascadeStateID, std::rc::Rc<CascadedPropertyStore>>,
    /// Pseudo-element records derived this flush, by what they were derived from.
    pub(super) pseudo_cohorts: HashMap<PseudoCohortKey, computed::FinalStyleRecordID>,
    pub(super) pseudo_stores: HashMap<(u8, CascadeStateID), std::rc::Rc<CascadedPropertyStore>>,
    /// The pseudo-element records settled beside the element derived last.
    pub(super) pseudo_deltas: Vec<PseudoRecordDelta>,
    /// The pseudo-element rules that flipped for the element being derived.
    pub(super) flipped_pseudo_rules: Vec<FlippedRule>,
}

/// A rule that came to match or stopped matching a node, by the pseudo-element it decides for,
/// if any.
#[derive(Clone, Copy)]
pub(super) struct FlippedRule {
    pub(super) pseudo_kind: Option<u16>,
}

impl EngineComputedRecordScratch {
    pub(super) fn capacity_bytes(&self) -> u64 {
        ((self.cohorts.capacity() + self.cold_cohorts.capacity())
            * size_of::<((u64, CascadeStateID), computed::FinalStyleRecordID)>()
            + self.pseudo_cohorts.capacity() * size_of::<(PseudoCohortKey, computed::FinalStyleRecordID)>()
            + (self.stores.capacity() + self.pseudo_stores.capacity())
                * size_of::<((u8, CascadeStateID), std::rc::Rc<CascadedPropertyStore>)>()
            + self.pseudo_deltas.capacity() * size_of::<PseudoRecordDelta>()
            + self.flipped_pseudo_rules.capacity() * size_of::<FlippedRule>()) as u64
    }
}

/// What a drive computes for: the node whose record it inherits from (the flat-tree parent, or
/// the originating element of a pseudo-element) and the element facts the computation's
/// adjustments read.
#[derive(Clone, Copy)]
pub(super) struct DriveSubject {
    /// The flat-tree parent the element inherits from; the document element has none and
    /// inherits from the initial values.
    parent: Option<StyleNodeID>,
    facts: u32,
}

/// A pseudo-element record the engine settled beside its originating element's; a removal when
/// the new record is none.
#[derive(Clone, Copy)]
pub(super) struct PseudoRecordDelta {
    pub(super) kind: u8,
    pub(super) old_style_record: computed::FinalStyleRecordID,
    pub(super) new_style_record: computed::FinalStyleRecordID,
}

/// What a pseudo-element record is derived from: the originating element's inherited style,
/// display, dependency flags and custom-property environment (and its record, when the state
/// inherits a non-inherited property from it), the pseudo-element's winner state and the
/// element facts and font environment the drive reads.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) struct PseudoCohortKey {
    parent_record: u64,
    inherited_groups: u32,
    parent_display: u32,
    dependency_flags: u8,
    environment: u64,
    kind: u8,
    generation: u64,
    state: Option<CascadeStateID>,
    facts: u32,
    font_environment_generation: u64,
}

/// The synthetic pseudo-element kinds, as the C++ `PseudoElement` enumeration numbers them.
mod pseudo_kind {
    pub(super) const AFTER: u8 = 0;
    pub(super) const BACKDROP: u8 = 1;
    pub(super) const BEFORE: u8 = 2;
    pub(super) const FIRST_LETTER: u8 = 3;
    pub(super) const MARKER: u8 = 5;
    pub(super) const SELECTION: u8 = 6;
    pub(super) const SYNTHETIC_COUNT: usize = 8;
}

/// The element facts a pseudo-element's computation reads: the C++ adjustments for what the
/// originating element is stay off for its pseudo-elements, past the ones about the element's
/// place in the document and its markup language.
const PSEUDO_ELEMENT_ADJUSTMENT_FACTS: u32 = {
    use bridge::element_adjustment_fact as fact;
    fact::IS_MATHML
        | fact::IS_MATHML_MTABLE
        | fact::IS_MATHML_MTR
        | fact::IS_MATHML_MTD
        | fact::IS_TH
        | fact::IS_DOCUMENT_ELEMENT
        | fact::HAS_ANIMATIONS
};

/// Whether a pseudo-element's winning `content` generates no box: `none` for every kind, and
/// `normal` (the initial value, so also an absent one) for ::before and ::after.
fn pseudo_content_generates_nothing(store: &CascadedPropertyStore, kind: u8) -> bool {
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

/// Whether a `content` value computes without the element or its counter environment: keywords,
/// and lists of strings and keywords; counters, attributes and images resolve in C++.
fn content_value_is_engine_computable(value: &StyleValueData) -> bool {
    fn plain(value: &StyleValueData) -> bool {
        match value {
            StyleValueData::Keyword { .. } | StyleValueData::String { .. } => true,
            StyleValueData::ValueList { values, .. } => values
                .as_slice()
                .iter()
                .all(|value| value.optional_data().is_none_or(plain)),
            _ => false,
        }
    }
    match value {
        StyleValueData::Keyword { .. } => true,
        StyleValueData::Content { content, alt_text } => {
            content.optional_data().is_none_or(plain) && alt_text.optional_data().is_none_or(plain)
        }
        _ => false,
    }
}

/// The value a shorthand value carries for one of its longhands, through nested shorthands.
fn shorthand_longhand_value(
    property: u16,
    data: &crate::css::style_value::StyleValueData,
) -> Option<crate::css::style_value::RetainedStyleValueData> {
    let crate::css::style_value::StyleValueData::Shorthand {
        sub_properties, values, ..
    } = data
    else {
        return None;
    };
    for (&sub_property, sub_value) in sub_properties.as_slice().iter().zip(values.as_slice()) {
        let sub_data = unsafe { &*sub_value.pointer().cast::<crate::css::style_value::StyleValueData>() };
        if sub_property == property {
            return Some(unsafe {
                crate::css::style_value::RetainedStyleValueData::from_retained_pointer(
                    crate::css::style_value::retain_style_value(sub_data),
                )
            });
        }
        if let Some(found) = shorthand_longhand_value(property, sub_data) {
            return Some(found);
        }
    }
    None
}

/// Whether a longhand computes in the drive's remaining phase: after the font, line-height and
/// color-scheme stages, whose outputs the engine does not derive itself yet.
fn property_computes_in_remaining_phase(property: u16) -> bool {
    use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LONGHAND_WORD_COUNT};
    use crate::css::style_compute::{LONGHAND_DRIVE_PHASE_REMAINING, property_computation_order_for_phase};
    static REMAINING: std::sync::OnceLock<[u64; LONGHAND_WORD_COUNT]> = std::sync::OnceLock::new();
    let words = REMAINING.get_or_init(|| {
        let mut words = [0_u64; LONGHAND_WORD_COUNT];
        for &property in property_computation_order_for_phase(LONGHAND_DRIVE_PHASE_REMAINING) {
            let index = usize::from(property - FIRST_LONGHAND_PROPERTY_ID);
            words[index / 64] |= 1 << (index % 64);
        }
        words
    });
    let Some(index) = property.checked_sub(FIRST_LONGHAND_PROPERTY_ID).map(usize::from) else {
        return false;
    };
    index / 64 < words.len() && words[index / 64] & (1 << (index % 64)) != 0
}

/// Whether a longhand's new value would start an animation or a transition in the C++
/// computation, register anchor names there, or feed the counter-style environment identity it
/// resolves.
/// The longhands the drive's font resolution selects a font by, which its request carries.
fn font_resolution_selects_by(property: u16) -> bool {
    use crate::css::property_metadata::property_id as prop;
    matches!(
        property,
        prop::FONT_FAMILY | prop::FONT_STYLE | prop::FONT_WEIGHT | prop::FONT_WIDTH | prop::FONT_OPTICAL_SIZING
    )
}

fn property_starts_animation_or_counter_environment(property: u16) -> bool {
    use crate::css::property_metadata::{
        FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, property_id as prop, property_style_group_index,
    };
    if !(FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property) {
        return true;
    }
    // A view transition name is a plain computed value; it starts nothing.
    matches!(property, prop::CONTENT | prop::LIST_STYLE_TYPE | prop::ANCHOR_NAME)
        || (property != prop::VIEW_TRANSITION_NAME
            && property_style_group_index(property)
                .is_some_and(|group| usize::from(group) == crate::css::table_group_builder::group_index::ANIMATION))
}

/// A font's pixel metric as the drive resolves font-relative units against it: the C++ length
/// resolution context carries the metrics as `CSSPixels`, so an `ex` resolves against the
/// fixed-point x-height rather than the font's raw floating-point one.
fn drive_font_metric(value: f32) -> f64 {
    crate::css::css_pixels::CssPixels::nearest_value_for_f32(value).to_double()
}

/// Whether a written value computes from the record, the parent and the document's computation
/// inputs alone: no custom-property substitution, and none of the element or sheet facts the C++
/// computation gathers per drive.
fn value_computes_without_document_context(value: &StyleValueData) -> bool {
    if matches!(value, StyleValueData::Unresolved { .. })
        || crate::css::style_compute::value_is_computationally_independent(value).is_none()
    {
        return false;
    }
    let dependencies = crate::css::style_compute::external_value_dependencies(value);
    !dependencies.uses_tree_counting_function
        && dependencies.container_relative_length_unit_mask == 0
        && !dependencies.has_unfixed_random_sharing
        && !dependencies.uses_random_function
        && !dependencies.needs_document_base_url
        && !dependencies.may_need_style_sheet_resource_context
}
