/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

mod drive;
mod pseudo;

use super::*;
use crate::css::computed_longhand_table::ComputedLonghandTable;
use drive::drive_font_metric;

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
    fn shared_style_record_key(
        &self,
        node: StyleNodeID,
        parent_record: u64,
        environment: u64,
        shape: [u64; 4],
    ) -> Option<computed::SharedStyleRecordKey> {
        if !self.has_no_element_declarations(node) || self.node_declares_custom_properties(node) {
            return None;
        }
        let cascade_input = self.published_match_answers.lookup(node)?.cascade_input?;
        Some(computed::SharedStyleRecordKey {
            cascade_input: cascade_input.0,
            tree_scope: self.tree.tree_scope(node).0,
            inherited_groups: self
                .computed_group_sets
                .inherited_groups_for_shared_style(parent_record)?,
            environment,
            shape,
        })
    }

    pub(crate) fn lookup_shared_style_record(
        &mut self,
        node: StyleNodeID,
        parent_record: u64,
        environment: u64,
        shape: [u64; 4],
    ) -> Option<u64> {
        let key = self.shared_style_record_key(node, parent_record, environment, shape)?;
        let shared = self.computed_group_sets.shared_style_record(key)?;
        let inherited_group_count = self
            .computed_group_sets
            .inherited_group_count_for_shared_style(shared.record)?;
        self.published_match_answers.mark_observed(node);
        self.prepare_shared_exact_cascade_state(node);
        let publication = self.assign_shared_style_record(
            computed::ComputedStyleTarget::new(node, u8::MAX),
            shared.record,
            inherited_group_count,
            shared.inherited_group_swap_eligible,
        );
        let record = publication.style_record_identity.raw();
        self.computed_group_sets.remember_shared_computation_context(
            node,
            computed::SharedComputationContext {
                parent_record,
                record,
                key,
            },
        );
        self.settle_computed_memory();
        self.counters.bump(Counter::SharedStyleRecordHits);
        Some(record)
    }

    pub(crate) fn take_shared_computation_context(
        &mut self,
        node: StyleNodeID,
        parent_record: u64,
        environment: u64,
        shape: [u64; 4],
        pseudo_elements: u64,
    ) -> Option<u64> {
        let context = self.computed_group_sets.take_shared_computation_context(node)?;
        if context.key.environment != environment
            || context.key.tree_scope != self.tree.tree_scope(node).0
            || context.key.shape[..3] != shape[..3]
            || self.computed_group_sets.assigned_style_record(node)?.raw() != context.record
        {
            return None;
        }
        let previous_inherited = self
            .computed_group_sets
            .inherited_groups_for_shared_style(context.parent_record)?;
        let current_inherited = self
            .computed_group_sets
            .inherited_groups_for_shared_style(parent_record)?;
        if previous_inherited != current_inherited
            || self
                .computed_group_sets
                .style_record_view(context.record)?
                .pseudo_element_styles
                != pseudo_elements
        {
            return None;
        }
        Some(context.record)
    }

    pub(crate) fn remember_shared_style_record(
        &mut self,
        node: StyleNodeID,
        parent_record: u64,
        environment: u64,
        shape: [u64; 4],
        record: u64,
    ) {
        let Some(key) = self.shared_style_record_key(node, parent_record, environment, shape) else {
            return;
        };
        self.computed_group_sets.remember_shared_style_record(node, key, record);
        self.settle_computed_memory();
    }

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
        if parent_inputs_moved.inherited_style && !self.engine_marker_font_supported(node) {
            return None;
        }
        if !self.engine_pseudo_inputs_available(node, self.computed_group_sets.assigned_style_record(node)) {
            return None;
        }
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
        let element_uses_substitution = self.nodes_with_substituted_records.contains(&node);
        let pseudo_states: Vec<_> = self
            .winner_groups
            .pseudo_states(node)
            .filter_map(|(_, version, state, priority_current)| {
                (version == self.program.version() && priority_current).then_some(state)
            })
            .collect();
        let pseudo_uses_substitution = pseudo_states
            .into_iter()
            .any(|state| self.state_has_substitutions(node, state));
        if element_uses_substitution || pseudo_uses_substitution {
            self.nodes_with_substituted_records.insert(node);
        } else {
            self.nodes_with_substituted_records.remove(&node);
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
        mut parent_inputs_moved: ParentInputsMoved,
        scratch: &mut EngineComputedRecordScratch,
    ) -> Option<(computed::FinalStyleRecordID, computed::FinalStyleRecordID)> {
        use crate::css::computed_value_types::{
            STYLE_GROUP_INDEX_ANCHOR, STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_SURROUND,
        };
        use crate::css::computed_values::computed_group_dependency_mask;
        use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LONGHAND_WORD_COUNT};

        let target = computed::ComputedStyleTarget::new(node, u8::MAX);
        // A custom property the cascade declares is no winner the columns hold; the engine
        // computes the environment it decides itself.
        if !cascade_winners_are_complete && !self.cascade_winners_are_complete_but_for_custom_properties(node) {
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
        // An element's animations compose into its style in the C++ computation, an element
        // standing for its host's pseudo-element takes the style C++ computes for that
        // pseudo-element, and a hint mapped from another element's attributes moves without
        // anything recorded on the element.
        let facts = self.computed_group_sets.adjustment_facts(node);
        if facts
            & (bridge::element_adjustment_fact::HAS_ANIMATIONS
                | bridge::element_adjustment_fact::IS_SHADOW_HOST_PSEUDO_ELEMENT
                | bridge::element_adjustment_fact::HAS_DERIVED_PRESENTATIONAL_HINTS)
            != 0
        {
            self.counters.bump(Counter::EngineComputedRecordBailWinnerElement);
            return None;
        }
        let Some(old_style_record) = self.computed_group_sets.assigned_style_record(node) else {
            // Presentational hints are mapped from the attributes by the C++ computation, which
            // publishes them as the element's declarations: a first record waits for that
            // computation, and an attribute change asks for it through a recorded input, so a
            // later record's winners carry the hints.
            if facts & bridge::element_adjustment_fact::HAS_PRESENTATIONAL_HINTS != 0 {
                self.counters.bump(Counter::EngineComputedRecordBailWinnerElement);
                return None;
            }
            return self.engine_cold_record(node, (generation, state), scratch);
        };
        if self.record_requires_cpp_animation(old_style_record) {
            self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return None;
        }
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
        let Some(inputs) = self.document_style_computation_inputs else {
            self.counters.bump(Counter::EngineComputedRecordBailNoEnvironment);
            return None;
        };
        // The environment the node's own custom declarations resolve to over the parent's. A node
        // declaring none keeps its record's, which is the parent's; a moved environment
        // republishes the record under the new one.
        let environment = {
            let parent_environment = match self.tree.flat_tree_parent(node) {
                Some(parent) => {
                    let Some(parent_environment) =
                        self.computed_group_sets.custom_property_environment_identity(parent)
                    else {
                        self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
                        return None;
                    };
                    parent_environment
                }
                None => 0,
            };
            let Some(environment) = self.engine_custom_property_environment(node, parent_environment, &inputs) else {
                self.counters.bump(Counter::EngineComputedRecordBailCustomProperties);
                return None;
            };
            let Some(old_environment) = self
                .computed_group_sets
                .style_record_custom_property_environment(old_style_record.raw())
            else {
                self.counters.bump(Counter::EngineComputedRecordBailRecord);
                return None;
            };
            (environment != old_environment).then_some(environment)
        };
        let Some(current_environment) = environment.or_else(|| {
            self.computed_group_sets
                .style_record_custom_property_environment(old_style_record.raw())
        }) else {
            self.counters.bump(Counter::EngineComputedRecordBailRecord);
            return None;
        };
        // A moved environment reaches every winner written with a substitution: such a record
        // is driven again in full under the new one.
        let environment_moved_under_substitutions = environment.is_some() && self.state_has_substitutions(node, state);
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
            if !parent_inputs_moved.any() && !environment_moved_under_substitutions {
                // A declaration in an inherited payload group does not prove that the other
                // properties in that group still inherit from the current parent. Re-drive the
                // record in full when its payloads cannot prove the relationship.
                if self.record_inherits_from_current_parent(node, state, 0) {
                    // Only the environment moved: the record keeps its groups and takes the new one.
                    if let Some(environment) = environment {
                        let Some(delta) = self
                            .computed_group_sets
                            .republish_engine_record_with_environment(node, environment)
                        else {
                            self.counters.bump(Counter::EngineComputedRecordBailAssemble);
                            return None;
                        };
                        self.counters.bump(Counter::EngineComputedRecordUnchangedWinners);
                        self.note_engine_computed_record(node, delta, (generation, state), 0, 0);
                        return Some(delta);
                    }
                    self.counters.bump(Counter::EngineComputedRecordUnchangedWinners);
                    self.counters.bump(Counter::CascadeWinnerDeltaStops);
                    self.note_engine_computed_record(
                        node,
                        (old_style_record, old_style_record),
                        (generation, state),
                        0,
                        0,
                    );
                    return Some((old_style_record, old_style_record));
                }
                let owned_groups = self
                    .winner_groups
                    .winners_in_state(state)
                    .filter_map(|winner| crate::css::property_metadata::property_style_group_index(winner.property))
                    .fold(0_u32, |mask, group| mask | (1 << group));
                if !self.record_inherits_from_current_parent(node, state, owned_groups) {
                    self.counters.bump(Counter::EngineComputedRecordBailUnchangedWinners);
                    return None;
                }
                parent_inputs_moved.inherited_style = true;
            }
        }
        // A moved font-phase longhand reaches every value the font feeds, so the record is driven
        // through every phase and every group is rebuilt. A moved box-type transformation input
        // takes the same route, as does a record whose parent inputs moved: the transformation
        // and the inheritance are part of the full drive.
        let full_drive = parent_inputs_moved.any()
            || environment_moved_under_substitutions
            || delta.properties().iter().any(|&property| {
                use crate::css::property_metadata::property_id as prop;
                !property_computes_in_remaining_phase(property)
                    || matches!(property, prop::DISPLAY | prop::POSITION | prop::FLOAT)
            });
        let delta_property_count = delta.properties().len() as u64;
        // Both drive paths may inherit values from the parent. A partial drive does so when a
        // previously declared longhand becomes undeclared, so alike derivations share only under
        // alike parents.
        let parent_record = self
            .tree
            .flat_tree_parent(node)
            .and_then(|parent| self.computed_group_sets.assigned_style_record(parent))
            .map_or(0, |record| record.raw());
        let cohort = (
            old_style_record.raw(),
            state,
            facts,
            parent_record,
            environment.unwrap_or(0),
        );
        if let Some(&new_style_record) = scratch.cohorts.get(&cohort) {
            self.note_node_substitution(node, scratch, state, current_environment);
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

        let store = match scratch.stores.get(&(state, current_environment)) {
            Some(store) => store.clone(),
            None => {
                let mut substituted = false;
                let store = std::rc::Rc::new(self.cascaded_store_for_state(
                    node,
                    state,
                    None,
                    current_environment,
                    &mut substituted,
                )?);
                scratch.stores.insert((state, current_environment), store.clone());
                if substituted {
                    scratch.substituted_states.insert((state, current_environment));
                }
                store
            }
        };
        self.note_node_substitution(node, scratch, state, current_environment);
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
            old_style_record,
            old_style_record,
            table,
            groups_to_rebuild,
            &length,
            font.as_ref(),
            parent_in_display_none_subtree,
            environment,
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
    /// Move a node's record to the environment C++ refreshed its inherited custom-property data
    /// to, without recomputing anything: what an inherited-custom-properties reaction C++ settled
    /// by refreshing the data alone publishes. The new record's identity, or nothing when the node
    /// holds no base record to move.
    pub(crate) fn republish_record_environment(&mut self, node: StyleNodeID, environment: u64) -> Option<u64> {
        let (_, new_style_record) = self
            .computed_group_sets
            .republish_engine_record_with_environment(node, environment)?;
        Some(new_style_record.raw())
    }

    /// Note whether the node's record was computed with a substituted winner, for C++ to record
    /// the node as a reader of custom properties when it installs the record.
    fn note_node_substitution(
        &mut self,
        node: StyleNodeID,
        scratch: &EngineComputedRecordScratch,
        state: CascadeStateID,
        environment: u64,
    ) {
        if scratch.substituted_states.contains(&(state, environment)) {
            self.nodes_with_substituted_records.insert(node);
        } else {
            self.nodes_with_substituted_records.remove(&node);
        }
    }

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
        self.engine_computed_records_pending
            .entry(node)
            .or_default()
            .push(PendingEngineComputedRecord {
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
        if let Some(pending_records) = self.engine_computed_records_pending.remove(&node) {
            for pending in pending_records {
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
        }
        self.published_match_answers.mark_observed(node);
    }

    /// The transaction's outputs are gone: every derived record C++ did not install goes back to
    /// the record the node held, unless a publication has moved the node on since.
    pub(super) fn discard_engine_computed_records(&mut self) {
        for pending in std::mem::take(&mut self.engine_computed_records_pending)
            .into_values()
            .flatten()
        {
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
        // The document element's environment is its own, which is nothing without declarations;
        // any other node's is its declarations resolved over the parent's.
        let Some(parent_environment) = parent.map_or(Some(0), |parent| {
            self.computed_group_sets.custom_property_environment_identity(parent)
        }) else {
            self.counters.bump(Counter::EngineComputedRecordBailRecordParent);
            return None;
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
                previous_style_record: 0,
                generation: cascade_state.0,
                state,
                facts,
                pseudo_styles,
                environment: parent_environment,
                font_environment_generation: inputs.font_environment_generation,
            });
        let delta = self.winner_groups.semantic_delta(None, state);
        let delta_property_count = delta.properties().len() as u64;
        if !self.node_declares_custom_properties(node)
            && let Some(delta) = self.assign_cached_cold_record(
                node,
                target,
                cascade_state,
                cache_key,
                parent,
                state,
                computed::FinalStyleRecordID::NONE,
                delta_property_count,
                scratch,
            )
        {
            return Some(delta);
        }
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
        let Some(environment) = self.engine_custom_property_environment(node, parent_environment, &inputs) else {
            self.counters.bump(Counter::EngineComputedRecordBailCustomProperties);
            return None;
        };
        // The state has to be one the engine can compute from before any record is shared under
        // it: a record C++ computed for a per-element value, such as a `random()` draw, is that
        // element's alone. A store with substituted values is the environment's as well as the
        // state's, and admits nothing for the state alone.
        let store = match scratch.stores.get(&(state, environment)) {
            Some(store) => store.clone(),
            None => {
                let mut substituted = false;
                let store = self.cascaded_store_for_state(node, state, None, environment, &mut substituted);
                self.remember_state_admission(
                    (
                        cascade_state.0,
                        state,
                        environment,
                        inputs.custom_property_registration_generation,
                    ),
                    store.is_some(),
                );
                let store = std::rc::Rc::new(store?);
                scratch.stores.insert((state, environment), store.clone());
                if substituted {
                    scratch.substituted_states.insert((state, environment));
                }
                store
            }
        };
        self.note_node_substitution(node, scratch, state, environment);
        let cache_key = parent
            .zip(parent_record)
            .and_then(|(parent, parent_record)| self.cold_record_parent(node, parent, parent_record, state))
            .map(|parent| ColdRecordKey {
                parent,
                previous_style_record: 0,
                generation: cascade_state.0,
                state,
                facts,
                pseudo_styles,
                environment,
                font_environment_generation: inputs.font_environment_generation,
            });
        if let Some(delta) = self.assign_cached_cold_record(
            node,
            target,
            cascade_state,
            cache_key,
            parent,
            state,
            computed::FinalStyleRecordID::NONE,
            delta_property_count,
            scratch,
        ) {
            return Some(delta);
        }
        let donor = cache_key.and_then(|key| {
            let donor_key = ColdRecordDonorKey {
                parent: key.parent,
                generation: key.generation,
                property_shape_hash: self.winner_groups.property_shape_hash(state),
                facts: key.facts,
                pseudo_styles: key.pseudo_styles,
                environment: key.environment,
                font_environment_generation: key.font_environment_generation,
            };
            self.engine_cold_record_donors
                .get(&donor_key)?
                .iter()
                .rev()
                .copied()
                .filter(|donor| {
                    self.winner_groups.property_shapes_are_equal(donor.state, state)
                        && self
                            .computed_group_sets
                            .final_style_record_is_live(donor.record.record.raw())
                })
                .min_by_key(|donor| {
                    self.winner_groups
                        .semantic_delta(Some(donor.state), state)
                        .properties()
                        .len()
                })
        });
        if let Some(donor) = donor {
            let donor_delta = self.winner_groups.semantic_delta(Some(donor.state), state);
            if let Some((groups_to_rebuild, selected)) =
                self.cold_record_donor_selection(donor, donor_delta.properties())
                && let Some((table, length, longhand_evaluations, _)) =
                    self.engine_driven_table(node, donor.record.record, &store, &selected, &inputs)
            {
                let parent_in_display_none_subtree = parent_record
                    .and_then(|record| self.computed_group_sets.style_record_view(record.raw()))
                    .is_some_and(|view| view.dependency_flags & (1 << 2) != 0);
                if let Some(assembly) = self.computed_group_sets.replace_engine_computed_table(
                    node,
                    donor.record.record,
                    computed::FinalStyleRecordID::NONE,
                    table,
                    groups_to_rebuild,
                    &length,
                    None,
                    parent_in_display_none_subtree,
                    Some(environment),
                ) {
                    self.computed_group_sets
                        .set_pending_cascade_state(target, cascade_state);
                    self.settle_computed_memory();
                    self.note_node_substitution(node, scratch, state, environment);
                    self.note_engine_computed_record(
                        node,
                        assembly.delta,
                        cascade_state,
                        delta_property_count,
                        longhand_evaluations,
                    );
                    if let Some(cache_key) = cache_key {
                        let record = ColdRecord {
                            record: assembly.delta.1,
                            swap_eligible: self.computed_group_sets.node_inherited_group_swap_eligible(node),
                        };
                        scratch.cold_cohorts.insert(cache_key, record);
                        self.remember_cold_record(cache_key, record);
                    }
                    return Some(assembly.delta);
                }
            }
        }
        let subject = DriveSubject { parent, facts };
        let (table, length, longhand_evaluations, font) = self.engine_full_drive(subject, None, &store, &inputs)?;
        let font = font.expect("a full drive resolves the font");
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
            let record = ColdRecord {
                record: delta.1,
                swap_eligible,
            };
            scratch.cold_cohorts.insert(cache_key, record);
            self.remember_cold_record(cache_key, record);
        }
        self.note_engine_computed_record(node, delta, cascade_state, delta_property_count, longhand_evaluations);
        Some(delta)
    }

    #[allow(clippy::too_many_arguments)]
    fn assign_cached_cold_record(
        &mut self,
        node: StyleNodeID,
        target: computed::ComputedStyleTarget,
        cascade_state: (u64, CascadeStateID),
        cache_key: Option<ColdRecordKey>,
        parent: Option<StyleNodeID>,
        state: CascadeStateID,
        old_style_record: computed::FinalStyleRecordID,
        delta_property_count: u64,
        scratch: &mut EngineComputedRecordScratch,
    ) -> Option<(computed::FinalStyleRecordID, computed::FinalStyleRecordID)> {
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
        let (ColdRecord { record, swap_eligible }, from_cache) = cache_key.and_then(|cache_key| {
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
        })?;
        // A record with transitions will need C++ on its next change. Keep its initial
        // computation in C++ too, so that fallback retains the input record and can select
        // only the changed groups instead of rebuilding the entire style.
        if self.record_requires_cpp_animation(record) {
            self.counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return None;
        }
        if !self.engine_pseudo_inputs_available(node, Some(record)) {
            return None;
        }
        self.computed_group_sets
            .set_pending_cascade_state(target, cascade_state);
        let publication = self.assign_shared_style_record(
            target,
            record.raw(),
            computed::ENGINE_INHERITED_GROUP_COUNT,
            swap_eligible,
        );
        let delta = (old_style_record, publication.style_record_identity);
        self.note_engine_computed_record(node, delta, cascade_state, delta_property_count, 0);
        self.counters.bump(if from_cache {
            Counter::EngineComputedRecordSharedHits
        } else {
            Counter::EngineComputedRecordCohortHits
        });
        Some(delta)
    }

    fn record_requires_cpp_animation(&self, record: computed::FinalStyleRecordID) -> bool {
        self.computed_group_sets
            .style_record_view(record.raw())
            .is_none_or(|view| {
                !view.animated_overlay.is_null()
                    || (unsafe { view.longhand_table.as_ref() })
                        .is_none_or(|table| !crate::css::style_compute::active_transition_properties(table).is_empty())
            })
    }

    /// Select the remaining-phase properties and output groups needed to derive a record from a
    /// donor. Dependencies belong to the donor's published record, since the new node has no
    /// computed row yet.
    fn cold_record_donor_selection(
        &mut self,
        donor: ColdRecordDonor,
        properties: &[u16],
    ) -> Option<(u32, [u64; crate::css::property_metadata::LONGHAND_WORD_COUNT])> {
        use crate::css::computed_value_types::{
            STYLE_GROUP_INDEX_ANCHOR, STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_SURROUND,
        };
        use crate::css::computed_values::computed_group_dependency_mask;
        use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LONGHAND_WORD_COUNT};

        if properties.is_empty()
            || properties.iter().any(|&property| {
                use crate::css::property_metadata::property_id as prop;
                !property_computes_in_remaining_phase(property)
                    || matches!(property, prop::DISPLAY | prop::POSITION | prop::FLOAT)
            })
        {
            return None;
        }
        let mut groups_to_rebuild = 0_u32;
        let mut selected = [0_u64; LONGHAND_WORD_COUNT];
        let mut select = |property: u16| {
            let index = usize::from(property - FIRST_LONGHAND_PROPERTY_ID);
            selected[index / 64] |= 1 << (index % 64);
        };
        let view = self.computed_group_sets.style_record_view(donor.record.record.raw())?;
        let inherited_box = unsafe {
            &*view.payloads[crate::css::computed_value_types::STYLE_GROUP_INDEX_INHERITED_BOX]
                .cast::<crate::css::computed_values::InheritedBoxValues>()
        };
        for &property in properties {
            if property_starts_animation_or_counter_environment(property) {
                return None;
            }
            let groups = computed_group_dependency_mask(property)?;
            groups_to_rebuild |= groups;
            select(property);
            let bits = crate::css::style_compute::table_row_bits(property);
            let counterpart = if bits & crate::css::style_compute::LOGICAL_ALIAS_BIT != 0 {
                crate::css::style_compute::map_logical_alias_to_physical(
                    property,
                    inherited_box.writing_mode,
                    inherited_box.direction,
                )
            } else if bits & crate::css::style_compute::PHYSICAL_TO_LOGICAL_BIT != 0 {
                crate::css::style_compute::map_physical_to_logical_alias(
                    property,
                    inherited_box.writing_mode,
                    inherited_box.direction,
                )
            } else {
                property
            };
            if counterpart != property {
                groups_to_rebuild |= computed_group_dependency_mask(counterpart)?;
                select(counterpart);
            }
        }
        if groups_to_rebuild & (1 << STYLE_GROUP_INDEX_ANCHOR) != 0 {
            groups_to_rebuild |= 1 << STYLE_GROUP_INDEX_SURROUND;
        }
        if properties.contains(&crate::css::property_metadata::property_id::COLOR) {
            let (dependent_groups, dependent_properties) = self
                .computed_group_sets
                .record_current_color_dependencies(donor.record.record)?;
            groups_to_rebuild |= dependent_groups;
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
                            inherited_box.writing_mode,
                            inherited_box.direction,
                        ));
                    }
                }
            }
        }
        if groups_to_rebuild & (1 << STYLE_GROUP_INDEX_FONT) != 0 {
            return None;
        }
        Some((groups_to_rebuild, selected))
    }

    /// Retry a record after C++ has installed earlier records in the same preorder batch. A record
    /// rejected while the batch was planned may become computable once its inheritance parent is
    /// authoritative.
    pub(crate) fn retry_engine_record_after_ancestor(&mut self, node: StyleNodeID) -> u64 {
        let facts = self.computed_group_sets.adjustment_facts(node);
        if facts & bridge::element_adjustment_fact::DISALLOW_DISPLAY_CONTENTS != 0 {
            return 0;
        }
        let Lookup::Known(cascade_state) = self
            .winner_groups
            .token_for(WinnerGroupKey::current(node, self.program.version()))
        else {
            return 0;
        };
        let mut scratch = EngineComputedRecordScratch::default();
        if !self.engine_pseudo_inputs_available(node, self.computed_group_sets.assigned_style_record(node)) {
            return 0;
        }
        if !self.node_declares_custom_properties(node)
            && let Some(old_style_record) = self.computed_group_sets.assigned_style_record(node)
            && let Some(inputs) = self.document_style_computation_inputs
            && let Some(parent) = self.tree.flat_tree_parent(node)
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
                    facts,
                    pseudo_styles,
                    environment,
                    font_environment_generation: inputs.font_environment_generation,
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
                &mut scratch,
            ) {
                if self
                    .engine_pseudo_records(node, Some(old_record), record, cascade_state.0, &mut scratch)
                    .is_none()
                    || !scratch.pseudo_deltas.is_empty()
                {
                    // The retry result carries only the originating element's record. Let C++
                    // materialize when pseudo-element records must settle alongside it.
                    self.abandon_engine_computed_record(node, &mut scratch);
                    return 0;
                }
                return record.raw();
            }
        }
        if self.computed_group_sets.node_answer_is_incomplete(node) {
            return 0;
        }
        let cascade_winners_are_complete = self
            .published_match_answers
            .lookup(node)
            .is_some_and(|answer| answer.cascade_winners_are_complete);
        let record = self.engine_computed_record_delta(
            node,
            cascade_winners_are_complete,
            None,
            ParentInputsMoved {
                inherited_style: true,
                display: true,
            },
            &mut scratch,
        );
        let Some((_, record)) = record else {
            return 0;
        };
        if !scratch.pseudo_deltas.is_empty() {
            // The retry result carries only the originating element's record. Let C++ materialize
            // when pseudo-element records must settle alongside it.
            self.abandon_engine_computed_record(node, &mut scratch);
            return 0;
        }
        record.raw()
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

    /// A derivation that could not be completed: everything derived for `node` this flush goes
    /// back to what the node held, and nothing in the flush may share it.
    fn abandon_engine_computed_record(&mut self, node: StyleNodeID, scratch: &mut EngineComputedRecordScratch) {
        for pending in self.engine_computed_records_pending.remove(&node).into_iter().flatten() {
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
                self.engine_cold_record_donors.retain(|_, donors| {
                    donors.retain(|donor| donor.record.record != derived);
                    !donors.is_empty()
                });
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

    /// Whether a node's record inherits from the parent it has now: each inherited group outside
    /// `owned_groups` is the parent's own, no non-inherited property is inherited explicitly, and
    /// its custom-property environment is the parent's.
    fn record_inherits_from_current_parent(&self, node: StyleNodeID, state: CascadeStateID, owned_groups: u32) -> bool {
        let Some(parent) = self.tree.flat_tree_parent(node) else {
            return false;
        };
        if self.state_explicitly_inherits_non_inherited_property(node, state) {
            return false;
        }
        self.computed_group_sets
            .inherited_groups_follow_parent(node, parent, owned_groups)
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
    pub(super) fn refresh_root_font_metrics_from_record(&mut self, record: computed::FinalStyleRecordID) -> bool {
        use crate::css::computed_value_types::STYLE_GROUP_INDEX_FONT;
        let Some(view) = self.computed_group_sets.style_record_view(record.raw()) else {
            return false;
        };
        let font =
            unsafe { &*view.payloads[STYLE_GROUP_INDEX_FONT].cast::<crate::css::computed_value_types::FontValues>() };
        let Some(inputs) = self.document_style_computation_inputs.as_mut() else {
            return false;
        };
        let previous_inputs = *inputs;
        inputs.root_font_size = font.font_size.to_double();
        inputs.root_font_x_height = drive_font_metric(font.font_x_height);
        inputs.root_font_cap_height = drive_font_metric(font.font_ascent);
        inputs.root_font_zero_advance = drive_font_metric(font.font_zero_advance);
        inputs.root_line_height = font.line_height_used.to_double();
        inputs.root_font_metrics_depend_on_viewport_metrics = view.dependency_flags & (1 << 1) != 0;
        let changed = *inputs != previous_inputs;
        if changed {
            self.engine_cold_record_cache.clear();
            self.engine_cold_record_donors.clear();
        }
        changed
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
            self.engine_cold_record_donors.clear();
        }
        self.engine_cold_record_cache.insert(key, record);
        if key.previous_style_record == 0 {
            let donor_key = ColdRecordDonorKey {
                parent: key.parent,
                generation: key.generation,
                property_shape_hash: self.winner_groups.property_shape_hash(key.state),
                facts: key.facts,
                pseudo_styles: key.pseudo_styles,
                environment: key.environment,
                font_environment_generation: key.font_environment_generation,
            };
            let donors = self.engine_cold_record_donors.entry(donor_key).or_default();
            if let Some(existing) = donors.iter_mut().find(|donor| donor.state == key.state) {
                existing.record = record;
                return;
            }
            if donors.len() == MAXIMUM_COLD_RECORD_DONORS_PER_KEY {
                donors.remove(0);
            }
            donors.push(ColdRecordDonor {
                state: key.state,
                record,
            });
        }
    }

    /// Whether a winner state is one the engine computes records from: every winner a plain rule
    /// declaration with a written value that needs no document context. Substituted declarations
    /// also depend on the custom-property environment and the registry used to parse them.
    fn state_is_engine_computable(&mut self, node: StyleNodeID, cascade_state: (u64, CascadeStateID)) -> bool {
        let environment = self
            .computed_group_sets
            .custom_property_environment_identity(node)
            .unwrap_or(0);
        let registration_generation = self
            .document_style_computation_inputs
            .map_or(0, |inputs| inputs.custom_property_registration_generation);
        let key = (cascade_state.0, cascade_state.1, environment, registration_generation);
        if let Some(&admitted) = self.engine_computable_states.get(&key) {
            return admitted;
        }
        let mut substituted = false;
        let admitted = self
            .cascaded_store_for_state(node, cascade_state.1, None, environment, &mut substituted)
            .is_some();
        self.remember_state_admission(key, admitted);
        admitted
    }

    /// Whether C++ may publish one record as the answer for another element with this winner
    /// state. Values which read per-element or external context are never shared opaquely.
    fn state_is_opaque_record_shareable(&mut self, node: StyleNodeID, state: CascadeStateID) -> bool {
        let winners = self
            .winner_groups
            .winners_in_state(state)
            .filter_map(|winner| self.winner_groups.resolved_winner(winner))
            .collect::<Vec<_>>();
        winners.into_iter().all(|winner| {
            self.written_winner_value(node, &winner)
                .is_some_and(|(_, value)| value_computes_without_document_context(value.data()))
        })
    }

    fn remember_state_admission(&mut self, key: (u64, CascadeStateID, u64, u64), admitted: bool) {
        if self.engine_computable_states.len() >= COLD_RECORD_CACHE_LIMIT {
            self.engine_computable_states.clear();
        }
        self.engine_computable_states.insert(key, admitted);
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
                        | u32::from(display.list_item) << 3
                        | u32::from(display.box_value) << 4,
                );
            }
            ancestor = self.tree.flat_tree_parent(current);
        }
        None
    }

    /// Whether a winner state declares `inherit` for a non-inherited property, or carries a value
    /// the engine cannot see the spelling of.
    pub(super) fn node_explicitly_inherits_non_inherited_property(&self, node: StyleNodeID) -> bool {
        let Lookup::Known((_, state)) = self
            .winner_groups
            .token_for(WinnerGroupKey::current(node, self.program.version()))
        else {
            return false;
        };
        self.state_explicitly_inherits_non_inherited_property(node, state)
    }

    fn state_explicitly_inherits_non_inherited_property(&self, node: StyleNodeID, state: CascadeStateID) -> bool {
        let is_inherit_keyword = |value: Option<&crate::css::style_value::RetainedStyleValueData>| {
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
                    is_inherit_keyword(index.and_then(|index| written.get(index)))
                }
                WinnerSource::ExactCascade => true,
            }
        })
    }

    /// Keep a record C++ published for an element as a first record a later alike element can
    /// take, when it was computed from nothing but what the engine keys first records on: the
    /// parent's inherited style and environment, a winner state the engine can compute from, the
    /// element facts and the pseudo-elements it has rules for.
    #[allow(clippy::too_many_arguments)]
    fn remember_cold_record_candidate(
        &mut self,
        target: computed::ComputedStyleTarget,
        cascade_state: (u64, CascadeStateID),
        custom_property_environment: u64,
        pseudo_styles: u64,
        previous_style_record: Option<computed::FinalStyleRecordID>,
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
        if self.node_declares_custom_properties(node) {
            return;
        }
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
        if !self.state_is_engine_computable(node, cascade_state)
            && !self.state_is_opaque_record_shareable(node, cascade_state.1)
        {
            return;
        }
        let Some(parent) = self.cold_record_parent(node, parent, parent_record, cascade_state.1) else {
            return;
        };
        let swap_eligible = self.computed_group_sets.node_inherited_group_swap_eligible(node);
        let key = ColdRecordKey {
            parent,
            previous_style_record: previous_style_record.map_or(0, computed::FinalStyleRecordID::raw),
            generation: cascade_state.0,
            state: cascade_state.1,
            facts,
            pseudo_styles,
            environment: custom_property_environment,
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

    /// The synthetic pseudo-elements the node has rules for, as a C++ record's pseudo-style mask,
    /// or no bits when the engine holds no answer for the node.
    pub(crate) fn published_pseudo_style_mask(&self, node: StyleNodeID) -> u64 {
        self.pseudo_style_mask(node).unwrap_or(0)
    }

    /// The value a winner's declaration was written with, and the declaration's index in its
    /// block: the drive computes from the spelling the declaration was written in, which the
    /// cascade's canonical identity may have rewritten. A rule keeps its written values beside its
    /// declarations, and an element's own declarations keep theirs beside the facts.
    fn written_winner_value(
        &mut self,
        node: StyleNodeID,
        winner: &PropertyWinner,
    ) -> Option<(usize, crate::css::style_value::RetainedStyleValueData)> {
        match winner.source {
            WinnerSource::Rule(rule) => self
                .program
                .written_winner_declaration(rule, winner.property, winner.important, winner.key.value)
                .map(|(index, value)| (index, value.clone_retained())),
            WinnerSource::Element(kind) => {
                let (declared, _) = self.facts.element_declared_properties(node, kind);
                let complete = self
                    .facts
                    .element_declarations_are_complete_but_for_custom_properties(node, kind);
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
                None
            }
        }
    }

    /// Whether a winner's declaration was written with a substitution the engine resolves itself:
    /// var() references of its own, or a longhand pending a shorthand written with them. A value
    /// reading anything else - a custom function, an attribute, a style query - is C++'s, and what
    /// it computes to can move without any winner moving.
    fn winner_is_written_with_substitution(&self, node: StyleNodeID, winner: &PropertyWinner) -> bool {
        let written = match winner.source {
            WinnerSource::Rule(rule) => self
                .program
                .written_winner_declaration(rule, winner.property, winner.important, winner.key.value)
                .map(|(_, value)| value),
            WinnerSource::Element(kind) => {
                let (declared, _) = self.facts.element_declared_properties(node, kind);
                let written = self.facts.element_written_declared_values(node, kind);
                if written.len() != declared.len() {
                    return false;
                }
                declared
                    .iter()
                    .rposition(|declared| {
                        declared.property == winner.property
                            && declared.important == winner.important
                            && declared.value == winner.key.value
                    })
                    .map(|index| &written[index])
            }
            WinnerSource::ExactCascade => None,
        };
        written.is_some_and(|value| match value.data() {
            unresolved @ crate::css::style_value::StyleValueData::Unresolved { .. } => {
                custom_property_cascade::value_is_engine_resolvable_substitution(unresolved)
            }
            crate::css::style_value::StyleValueData::PendingSubstitution {
                original_shorthand_value,
            } => custom_property_cascade::value_is_engine_resolvable_substitution(original_shorthand_value.data()),
            _ => false,
        })
    }

    /// The shorthand declared in a winner's block with the written value a pending longhand
    /// names: which shorthand it is, and that written value.
    fn shorthand_declaration_written_as(
        &self,
        node: StyleNodeID,
        source: WinnerSource,
        written_value: *const crate::css::style_value::StyleValueData,
    ) -> Option<(u16, crate::css::style_value::RetainedStyleValueData)> {
        let (declared, written): (&[DeclaredProperty], &[crate::css::style_value::RetainedStyleValueData]) =
            match source {
                WinnerSource::Rule(rule) => (
                    self.program.declared_properties_of(rule),
                    self.program.written_values_of(rule),
                ),
                WinnerSource::Element(kind) => (
                    self.facts.element_declared_properties(node, kind).0,
                    self.facts.element_written_declared_values(node, kind),
                ),
                WinnerSource::ExactCascade => return None,
            };
        if written.len() != declared.len() {
            return None;
        }
        declared
            .iter()
            .zip(written)
            .find(|(declared, written)| {
                declared.property < crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
                    && std::ptr::eq(written.pointer(), written_value)
            })
            .map(|(declared, written)| (declared.property, written.clone_retained()))
    }

    /// Whether any winner of a state was written with a substitution, so the record computed
    /// from it reads the node's custom-property environment.
    pub(super) fn state_has_substitutions(&self, node: StyleNodeID, state: CascadeStateID) -> bool {
        self.winner_groups.winners_in_state(state).any(|winner| {
            let Some(winner) = self.winner_groups.resolved_winner(winner) else {
                return false;
            };
            if winner.property < crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID {
                return false;
            }
            let value = match winner.source {
                WinnerSource::Rule(rule) => {
                    self.program
                        .written_winner_value(rule, winner.property, winner.important, winner.key.value)
                }
                WinnerSource::Element(kind) => {
                    let (declared, _) = self.facts.element_declared_properties(node, kind);
                    let written = self.facts.element_written_declared_values(node, kind);
                    declared
                        .iter()
                        .rposition(|declared| {
                            declared.property == winner.property
                                && declared.important == winner.important
                                && declared.value == winner.key.value
                        })
                        .and_then(|index| written.get(index))
                }
                WinnerSource::ExactCascade => None,
            };
            value.is_some_and(|value| {
                matches!(
                    value.data(),
                    crate::css::style_value::StyleValueData::Unresolved { .. }
                        | crate::css::style_value::StyleValueData::PendingSubstitution { .. }
                )
            })
        })
    }

    /// The cascade a winner state describes, as the drive consumes it: every winner's written
    /// value, seeded in cascade order so a logical property pair resolves the way it cascaded.
    /// `None` when a winner is not a plain rule declaration the engine can compute from.
    fn cascaded_store_for_state(
        &mut self,
        node: StyleNodeID,
        state: CascadeStateID,
        pseudo_kind: Option<u8>,
        environment: u64,
        substituted: &mut bool,
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
            // A shorthand written with a substitution is declared beside the longhands it
            // pends; those carry it.
            if winner.property < crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID {
                continue;
            }
            let Some((index, value)) = self.written_winner_value(node, &winner) else {
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
                // A value with var() references substitutes under the node's environment, as the
                // C++ cascade substitutes it; a value invalid at computed-value time is unset.
                crate::css::style_value::StyleValueData::Unresolved { .. } => {
                    *substituted = true;
                    let value = self.substitute_written_value(environment, winner.property, &value)?;
                    invalid_as_unset(value)
                }
                // A longhand pending its shorthand's substitution takes its part of the
                // substituted shorthand.
                crate::css::style_value::StyleValueData::PendingSubstitution {
                    original_shorthand_value,
                } => {
                    *substituted = true;
                    let Some((shorthand, written)) =
                        self.shorthand_declaration_written_as(node, winner.source, original_shorthand_value.pointer())
                    else {
                        self.counters.bump(Counter::EngineComputedRecordBailWinnerSpelling);
                        return None;
                    };
                    let resolved = self.substitute_written_value(environment, shorthand, &written)?;
                    match resolved.data() {
                        crate::css::style_value::StyleValueData::GuaranteedInvalid => unset_value(),
                        _ => expanded_longhand_value(shorthand, winner.property, &resolved).unwrap_or_else(unset_value),
                    }
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
            self.computed_group_sets.custom_property_environment_capacity_bytes()
                + self.custom_property_environments.capacity_bytes(),
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
            let live: super::fast_hash::FastSet<u64> =
                self.computed_group_sets.live_custom_property_environments().collect();
            self.custom_property_environments
                .retain_only(|identity| live.contains(&identity));
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
        if let Some(target) = target
            && !target.is_pseudo()
            && self.computed_group_sets.adjustment_facts(target.node())
                & bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT
                != 0
        {
            self.refresh_root_font_metrics_from_record(publication.style_record_identity);
        }
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
                publication.previous_style_record_identity,
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
            // The engine's own winner stands for the exact one when it is the same declaration,
            // or a declaration written with a substitution: the exact value is what that
            // substitutes to, which the engine computes for itself.
            let lower_bound_winner = lower_bound_state
                .and_then(|state| self.winner_groups.winner_in_state(state, property))
                .filter(|winner| {
                    self.winner_groups.resolved_winner(*winner).is_some_and(|resolved| {
                        (resolved.key == key || self.winner_is_written_with_substitution(target.node(), &resolved))
                            && matches!(self.specified_values.value(resolved.key.value), Lookup::Known(_))
                    })
                });
            winners.push(lower_bound_winner.unwrap_or(PropertyWinner {
                property,
                important: key.important,
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
                if exact_key.animation_relevance != 0
                    || resolved_winner.is_some_and(|winner| {
                        winner.key == exact_key || verifier.winner_is_written_with_substitution(target.node(), &winner)
                    })
                {
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
        // The background longhands form coordinated repeatable lists. A changed layer count in
        // any one of them changes the computed representation of every other list even when its
        // specified winner is unchanged.
        let background_group = 1 << crate::css::computed_value_types::STYLE_GROUP_INDEX_BACKGROUND;
        if delta
            .properties()
            .iter()
            .copied()
            .any(|property| computed_group_output_mask(property).is_some_and(|groups| groups & background_group != 0))
        {
            for property in crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
                ..=crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID
            {
                if computed_group_output_mask(property).is_some_and(|groups| groups & background_group != 0) {
                    let index = usize::from(property - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID);
                    computed_property_words[index / 64] |= 1 << (index % 64);
                }
            }
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
                    || !self
                        .program
                        .declarations_are_complete_but_for_custom_properties(entry.rule)
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
    previous_style_record: u64,
    generation: u64,
    state: CascadeStateID,
    facts: u32,
    /// The pseudo-elements the element has rules for: the record's metadata says which, and
    /// C++ computes their styles beside it.
    pseudo_styles: u64,
    /// The custom-property environment the record is published with.
    environment: u64,
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

/// The value-independent half of a first-record key. Records under the same key may seed one
/// another, with the semantic cascade delta selecting the values which must be recomputed.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) struct ColdRecordDonorKey {
    parent: ColdRecordParent,
    generation: u64,
    property_shape_hash: u64,
    facts: u32,
    pseudo_styles: u64,
    environment: u64,
    font_environment_generation: u64,
}

#[derive(Clone, Copy)]
pub(super) struct ColdRecordDonor {
    state: CascadeStateID,
    record: ColdRecord,
}

const MAXIMUM_COLD_RECORD_DONORS_PER_KEY: usize = 4;

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
    pub(super) cohorts: HashMap<(u64, CascadeStateID, u32, u64, u64), computed::FinalStyleRecordID>,
    /// The nodes whose record this flush settled: what their descendants inherit from is in
    /// place.
    pub(super) settled_nodes: HashSet<StyleNodeID>,
    /// First records derived this flush, by what they were derived from.
    pub(super) cold_cohorts: HashMap<ColdRecordKey, ColdRecord>,
    pub(super) stores: HashMap<(CascadeStateID, u64), std::rc::Rc<CascadedPropertyStore>>,
    /// The states whose store, under an environment, substituted a custom property into a winner.
    pub(super) substituted_states: HashSet<(CascadeStateID, u64)>,
    /// Pseudo-element records derived this flush, by what they were derived from.
    pub(super) pseudo_cohorts: HashMap<PseudoCohortKey, computed::FinalStyleRecordID>,
    pub(super) pseudo_stores: HashMap<(u8, CascadeStateID, u64), std::rc::Rc<CascadedPropertyStore>>,
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
/// The `unset` keyword, which a declaration invalid at computed-value time computes as.
fn unset_value() -> crate::css::style_value::RetainedStyleValueData {
    crate::css::style_value::RetainedStyleValueData::from_owned(crate::css::style_value::StyleValueData::Keyword {
        keyword: crate::css::style_compute::keyword::UNSET,
    })
}

fn invalid_as_unset(
    value: crate::css::style_value::RetainedStyleValueData,
) -> crate::css::style_value::RetainedStyleValueData {
    match value.data() {
        crate::css::style_value::StyleValueData::GuaranteedInvalid => unset_value(),
        _ => value,
    }
}

/// The longhand's part of a substituted shorthand value, as the cascade expands it.
fn expanded_longhand_value(
    shorthand: u16,
    property: u16,
    value: &crate::css::style_value::RetainedStyleValueData,
) -> Option<crate::css::style_value::RetainedStyleValueData> {
    let mut found = None;
    crate::css::style_compute::expand_shorthands_with(
        shorthand,
        value.pointer().cast(),
        false,
        &mut |longhand, data, _| {
            if longhand == property && found.is_none() {
                found = Some(unsafe {
                    crate::css::style_value::RetainedStyleValueData::from_retained_pointer(
                        crate::css::style_value::retain_style_value(data.cast()),
                    )
                });
            }
        },
    );
    found
}

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

/// Whether a written value computes from the record, the parent and the document's computation
/// inputs alone: no custom-property substitution, and none of the element or sheet facts the C++
/// computation gathers per drive.
fn value_computes_without_document_context(value: &StyleValueData) -> bool {
    // A longhand a shorthand written with a substitution declares holds a pending substitution
    // until the shorthand resolves; both compute in C++.
    if matches!(
        value,
        StyleValueData::Unresolved { .. } | StyleValueData::PendingSubstitution { .. }
    ) || crate::css::style_compute::value_is_computationally_independent(value).is_none()
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn acknowledgement_without_pending_records_observes_published_answer() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let mut raw_nodes = [0; 2];
        engine.allocate_style_nodes(&mut raw_nodes);
        let [first, second] = raw_nodes.map(|node| StyleNodeID::from_raw(node).unwrap());
        for node in [first, second] {
            engine.published_match_answers.push(
                PublishedMatchAnswer {
                    node,
                    cascade_input: None,
                    matches: None,
                    cascade_winners_are_complete: true,
                    observed: false,
                },
                &mut engine.memory,
                &mut engine.counters,
            );
        }
        engine.published_match_answers.sort();
        assert!(engine.engine_computed_records_pending.is_empty());

        engine.acknowledge_engine_computed_record(second);
        assert!(engine.published_match_answers.lookup(second).unwrap().observed);
        assert!(!engine.published_match_answers.lookup(first).unwrap().observed);
        engine.acknowledge_engine_computed_record(second);
        assert!(engine.published_match_answers.lookup(second).unwrap().observed);
        assert_eq!(engine.counters.get(Counter::EngineComputedLonghandEvaluations), 0);
    }

    #[test]
    fn pending_records_acknowledge_and_rollback_independently_by_node() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let mut raw_nodes = [0; 3];
        engine.allocate_style_nodes(&mut raw_nodes);
        let [first, second, third] = raw_nodes.map(|node| StyleNodeID::from_raw(node).unwrap());
        let mut scratch = EngineComputedRecordScratch::default();
        let publish = |engine: &mut StyleEngine, node, pseudo_kind| {
            let record = engine
                .publish_computed_groups(
                    computed::ComputedStyleTarget::new(node, pseudo_kind),
                    &[],
                    0,
                    0,
                    computed::ComputedMetadataInput {
                        pseudo_element_styles: 0,
                        dependency_flags: 0,
                        counter_style_environment_identity: 0,
                        animation_overlay_identity: 0,
                        animated_overlay: std::ptr::null(),
                        animation_overlay_payloads: &[],
                        longhand_table: std::ptr::null(),
                    },
                )
                .style_record_identity;
            engine
                .engine_computed_records_pending
                .entry(node)
                .or_default()
                .push(PendingEngineComputedRecord {
                    node,
                    pseudo_kind,
                    old_style_record: computed::FinalStyleRecordID::NONE,
                    new_style_record: record,
                    cascade_state: None,
                    longhand_evaluations: 1,
                });
            record
        };
        let first_record = publish(&mut engine, first, u8::MAX);
        publish(&mut engine, third, u8::MAX);
        let second_record = publish(&mut engine, second, u8::MAX);
        let first_pseudo = publish(&mut engine, first, 0);
        publish(&mut engine, third, 0);

        // Acknowledge out of publication order, including a repeated acknowledgement.
        engine.acknowledge_engine_computed_record(second);
        engine.acknowledge_engine_computed_record(second);
        assert_eq!(engine.counters.get(Counter::EngineComputedLonghandEvaluations), 1);
        engine.acknowledge_engine_computed_record(first);
        assert_eq!(engine.counters.get(Counter::EngineComputedLonghandEvaluations), 3);

        engine.abandon_engine_computed_record(third, &mut scratch);
        assert_eq!(engine.computed_group_sets.assigned_style_record(third), None);
        assert_eq!(engine.computed_group_sets.pseudo_style_record(third, 0), None);
        assert!(engine.engine_computed_records_pending.is_empty());

        // A later batch can use the same node, and discarding it leaves installed nodes alone.
        publish(&mut engine, third, u8::MAX);
        publish(&mut engine, third, 0);
        engine.discard_engine_computed_records();
        engine.acknowledge_engine_computed_record(third);
        assert!(engine.engine_computed_records_pending.is_empty());
        assert_eq!(engine.counters.get(Counter::EngineComputedLonghandEvaluations), 3);
        assert_eq!(engine.computed_group_sets.assigned_style_record(third), None);
        assert_eq!(engine.computed_group_sets.pseudo_style_record(third, 0), None);
        assert_eq!(
            engine.computed_group_sets.assigned_style_record(first),
            Some(first_record)
        );
        assert_eq!(
            engine.computed_group_sets.assigned_style_record(second),
            Some(second_record)
        );
        assert_eq!(
            engine.computed_group_sets.pseudo_style_record(first, 0),
            Some(first_pseudo)
        );
    }
}
