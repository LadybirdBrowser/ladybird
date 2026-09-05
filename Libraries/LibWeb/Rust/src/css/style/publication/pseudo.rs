/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

impl StyleEngine {
    /// Check pseudo winner availability before deriving an originating record that would have
    /// to be discarded. Marker generation additionally depends on the newly computed display
    /// and is checked when settling the pseudo records.
    pub(super) fn engine_pseudo_inputs_available(
        &mut self,
        node: StyleNodeID,
        record: Option<computed::FinalStyleRecordID>,
    ) -> bool {
        use pseudo_kind::{AFTER, BACKDROP, BEFORE, FIRST_LETTER, MARKER, SELECTION};

        let mut available = 0_u64;
        for (pseudo, version, _, priority_current) in self.winner_groups.pseudo_states(node) {
            let kind = usize::from(pseudo.kind.0);
            if kind >= pseudo_kind::SYNTHETIC_COUNT || kind == usize::from(BACKDROP) {
                continue;
            }
            if version != self.program.version() || !priority_current {
                self.counters.bump(Counter::EngineComputedRecordBailPseudoStale);
                return false;
            }
            available |= 1 << kind;
        }
        let Some(required) = self.pseudo_style_mask(node) else {
            self.counters.bump(Counter::EngineComputedRecordBailPseudoMask);
            return false;
        };
        if required & !available == 0 {
            return true;
        }
        let mut explicit_kinds = (1 << BEFORE) | (1 << AFTER) | (1 << FIRST_LETTER) | (1 << SELECTION);
        if record
            .and_then(|record| self.computed_group_sets.style_record_view(record.raw()))
            .and_then(|view| unsafe { view.longhand_table.as_ref() })
            .is_some_and(|table| table.display_is_list_item())
        {
            explicit_kinds |= 1 << MARKER;
        }
        if let Lookup::Known((_, state)) = self
            .winner_groups
            .token_for(WinnerGroupKey::current(node, self.program.version()))
            && let Some(winner) = self
                .winner_groups
                .winner_in_state(state, crate::css::property_metadata::property_id::DISPLAY)
                .and_then(|winner| self.winner_groups.resolved_winner(winner))
            && let Lookup::Known(value) = self.specified_values.retained_value(winner.key.value)
            && let StyleValueData::Display { raw } = value.data()
            && crate::css::display::FfiDisplay::from_raw(*raw).is_list_item()
        {
            explicit_kinds |= 1 << MARKER;
        }
        if required & explicit_kinds & !available != 0 {
            self.counters.bump(Counter::EngineComputedRecordBailPseudoRow);
            return false;
        }
        true
    }

    pub(super) fn engine_marker_font_supported(&mut self, node: StyleNodeID) -> bool {
        // Reject unsupported existing marker fonts before computing the originating element.
        // A later change to supported settings still computes correctly through C++ and makes
        // the next attempt eligible. The default marker's tabular numerals are not supported by
        // the engine font resolver yet.
        if let Some(marker) = self.computed_group_sets.pseudo_style_record(node, pseudo_kind::MARKER)
            && let Some(view) = self.computed_group_sets.style_record_view(marker.raw())
            && let Some(table) = unsafe { view.longhand_table.as_ref() }
        {
            let value = table
                .effective_value(
                    None,
                    crate::css::property_metadata::property_id::FONT_VARIANT_NUMERIC,
                    true,
                )
                .value;
            if !matches!(unsafe { value.cast::<StyleValueData>().as_ref() },
                Some(StyleValueData::Keyword { keyword }) if *keyword == crate::css::style_compute::keyword::NORMAL)
            {
                self.counters.bump(Counter::EngineComputedRecordBailFontPhase);
                return false;
            }
        }
        true
    }

    /// Settle the synthetic pseudo-elements of an element the engine derived a record for, the
    /// way the C++ computation refreshes them after the element's own: each kind the element has
    /// rules for, and the marker a list item generates, is driven against the element's new
    /// record; one that generates no box any more is removed; one whose cascade state did not
    /// move keeps its record. `None` is a pseudo-element the engine cannot settle.
    pub(super) fn engine_pseudo_records(
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
        let mut pseudo_uses_substitution = false;
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
                Some(state) => match scratch.pseudo_stores.get(&(kind, state, environment)) {
                    Some(store) => store.clone(),
                    None => {
                        let mut substituted = false;
                        let store = std::rc::Rc::new(self.cascaded_store_for_state(
                            node,
                            state,
                            Some(kind),
                            environment,
                            &mut substituted,
                        )?);
                        if substituted {
                            scratch.substituted_states.insert((state, environment));
                        }
                        scratch.pseudo_stores.insert((kind, state, environment), store.clone());
                        store
                    }
                },
                None => std::rc::Rc::new(CascadedPropertyStore::new()),
            };
            pseudo_uses_substitution |=
                state.is_some_and(|state| scratch.substituted_states.contains(&(state, environment)));
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
        if pseudo_uses_substitution {
            self.nodes_with_substituted_records.insert(node);
        }
        Some(())
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
    ) {
        self.counters.bump(Counter::EngineComputedPseudoRecords);
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
    pub(super) fn revert_engine_computed_pseudo_record(&mut self, pending: &PendingEngineComputedRecord) {
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

    pub(super) fn pseudo_style_mask(&self, node: StyleNodeID) -> Option<u64> {
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
}

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
