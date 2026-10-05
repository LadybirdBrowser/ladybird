/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

mod demand;
mod drive;
mod pseudo;
pub(super) use drive::drive_font_metric;
mod winner_store;

use winner_store::{WinnerDeclaration, WinnerStore, WinnerValue, shorthand_longhand_data};

use super::container_queries::VerdictTargets;
use super::*;
use crate::css::computed_longhand_table::{
    ComputedLonghandTable, DEPENDS_ON_VIEWPORT_METRICS, FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS,
    IN_DISPLAY_NONE_SUBTREE,
};
use crate::css::computed_values::StyleGroupMasks;
use animations::DeclarationScope;
pub(super) use demand::WinnerRepublication;
pub(crate) use demand::{RecordDemand, RecordDemandAnswer};
pub(super) use drive::{Drive, OrRefused, Suspension, Unanswered};
use drive::{DrivenTable, FontDriveGoal, FullDrive, PartialDrive};

/// Root-relative computation reads these inputs independently of its inheritance parent.
/// Bitwise keys keep equality exact without using an invalidation generation as a substitute
/// for the values. The viewport-dependence bit matters even when today's metrics agree.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(super) struct RootFontInputs {
    metrics: [u64; 5],
    depends_on_viewport: bool,
}

/// An element's old and new style records.
pub(super) type RecordDelta = (computed::FinalStyleRecordID, computed::FinalStyleRecordID);

/// What the computation of an element's record answers: its record delta, or, for the root-input
/// probe, the document element's font inputs where the probe proved them.
pub(super) enum ElementAnswer {
    Delta(RecordDelta),
    RootInputs(Option<RootFontInputs>),
}

/// How much of an element's held record a row drives again.
#[derive(Clone, Copy, PartialEq, Eq)]
enum DriveScope {
    /// Nothing the record was computed from moved: it stands.
    Stands,
    /// Only the custom-property environment the record resolves under moved: it takes this one.
    Environment(u64),
    /// The moved winners are driven again over the record.
    Partial,
    /// Every phase is driven again from the current inputs, against the parent as it is now.
    Full,
}

impl ElementAnswer {
    /// The delta of a computation that was no root-input probe.
    fn delta(self) -> RecordDelta {
        match self {
            Self::Delta(delta) => delta,
            Self::RootInputs(_) => unreachable!("only the root-input probe answers with root inputs"),
        }
    }
}

impl RootFontInputs {
    /// The metrics a `rem` resolves against, and whether they read the viewport.
    pub(super) fn font_metrics(self) -> (crate::css::style_compute::FfiFontMetrics, bool) {
        let [font_size, x_height, cap_height, zero_advance, line_height] = self.metrics.map(f64::from_bits);
        (
            crate::css::style_compute::FfiFontMetrics {
                font_size,
                x_height,
                cap_height,
                zero_advance,
                line_height,
            },
            self.depends_on_viewport,
        )
    }

    fn apply_to(self, inputs: &mut bridge::FfiDocumentStyleComputationInputs) {
        inputs.root_font_size = f64::from_bits(self.metrics[0]);
        inputs.root_font_x_height = f64::from_bits(self.metrics[1]);
        inputs.root_font_cap_height = f64::from_bits(self.metrics[2]);
        inputs.root_font_zero_advance = f64::from_bits(self.metrics[3]);
        inputs.root_line_height = f64::from_bits(self.metrics[4]);
        inputs.root_font_metrics_depend_on_viewport_metrics = self.depends_on_viewport;
    }

    pub(super) fn from_document(inputs: &bridge::FfiDocumentStyleComputationInputs) -> Self {
        Self {
            metrics: [
                inputs.root_font_size.to_bits(),
                inputs.root_font_x_height.to_bits(),
                inputs.root_font_cap_height.to_bits(),
                inputs.root_font_zero_advance.to_bits(),
                inputs.root_line_height.to_bits(),
            ],
            depends_on_viewport: inputs.root_font_metrics_depend_on_viewport_metrics,
        }
    }
}

impl RetainedState {
    pub(super) fn pseudo_element_style_is_deferred(&self, kind: tree::PseudoElementKind) -> bool {
        u32::from(kind.0) < u64::BITS && self.deferred_pseudo_elements & (1 << kind.0) != 0
    }

    /// The record a highlight pseudo-element inherits from: the same pseudo-element's record on its
    /// nearest flat-tree ancestor holding one, as the engine has it assigned.
    pub(crate) fn retained_highlight_inheritance_parent_style_record(
        &self,
        node: StyleNodeID,
        pseudo_kind: u8,
    ) -> Option<computed::FinalStyleRecordID> {
        let mut ancestor = self.tree.inheritance_parent(node);
        while let Some(candidate) = ancestor {
            if let Some(record) = self.computed_group_sets.pseudo_style_record(candidate, pseudo_kind) {
                return Some(record);
            }
            ancestor = self.tree.inheritance_parent(candidate);
        }
        None
    }

    /// The inheritance parent a node's record is computed from, none for the document element. A
    /// parent without a record is one the host styles before it applies the row, which waits for
    /// it.
    pub(super) fn record_inheritance_parent(&self, node: StyleNodeID) -> Drive<Option<StyleNodeID>> {
        let Some(parent) = self.tree.inheritance_parent(node) else {
            return Ok(None);
        };
        if self.computed_group_sets.assigned_style_record(parent).is_none() {
            return Err(Unanswered::AwaitsParent);
        }
        Ok(Some(parent))
    }

    /// The custom-property environment of a node the drive reads as an inheritance parent, which
    /// holds a record. Every record is assigned with its environment, so it holds one; should it
    /// not, the row waits for the parent as for one without a record.
    ///
    /// What the node's animations sampled into its custom properties is what it hands down, over
    /// the record's environment. The host lays the samples over a record as it installs it, so a
    /// child of a record moved beneath them waits for the parent.
    fn held_custom_property_environment(&self, node: StyleNodeID, counters: &mut Counters) -> Drive<u64> {
        let environment = self.computed_group_sets.custom_property_environment_identity(node);
        debug_assert!(environment.is_some(), "an inheritance parent without an environment");
        let Some(environment) = environment else {
            counters.bump(Counter::EngineComputedRecordBailRecordParent);
            return Err(Unanswered::AwaitsParent);
        };
        let Some(held) = self
            .element_custom_property_data
            .get(&node)
            .filter(|held| held.sampled_over.is_some())
        else {
            return Ok(environment);
        };
        // The record names the overlay once the host composed the samples into it, or the
        // environment beneath them before.
        if environment == held.identity || held.sampled_over == Some(environment) {
            Ok(held.identity)
        } else {
            Err(Unanswered::AwaitsParent)
        }
    }

    /// A node's custom-property environment as this flush leaves it. A row this flush settles
    /// holds its new environment already, but an ancestor between it and a first record may be one
    /// the host refreshes only once the batch applies, so the first record would resolve over the
    /// environment that ancestor held before. Each ancestor's is resolved again over its parent's
    /// as it is now, and kept for the rows after it. A node whose animations compose keeps the
    /// environment it holds. `None` for a node holding no environment.
    fn current_custom_property_environment(
        &mut self,
        node: StyleNodeID,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        scratch: &mut EngineComputedRecordScratch,
    ) -> Option<u64> {
        if let Some(&current) = scratch.current_custom_property_environments.get(&node) {
            return Some(current);
        }
        self.computed_group_sets.custom_property_environment_identity(node)?;
        let held = self
            .held_custom_property_environment(node, &mut Counters::default())
            .ok()?;
        // An ancestor outside the batch can have no retained answer for the current rule program.
        // That does not mean it declares no custom properties: keep its own resolved values when
        // refreshing what it inherits from a parent that moved.
        let has_match_answer = self
            .try_for_each_answer_match(node, None, |_, _, _, _| std::ops::ControlFlow::<()>::Continue(()))
            .is_some();
        let animates =
            self.computed_group_sets.adjustment_facts(node) & bridge::element_adjustment_fact::HAS_ANIMATIONS != 0
                || self.element_samples_custom_properties(node);
        let parent = self.tree.inheritance_parent(node).filter(|_| !animates);
        let current = match parent.and_then(|parent| {
            Some((
                parent,
                self.current_custom_property_environment(parent, inputs, scratch)?,
            ))
        }) {
            Some((parent, parent_environment))
                if !has_match_answer
                    && self.computed_group_sets.custom_property_environment_identity(parent)
                        != Some(parent_environment) =>
            {
                let inherited = self.custom_property_environments.inheritable(parent_environment);
                let previously_inherited = self
                    .computed_group_sets
                    .custom_property_environment_identity(parent)
                    .map(|environment| self.custom_property_environments.inheritable(environment));
                if previously_inherited == Some(held) {
                    inherited
                } else {
                    let store = self.custom_property_environments.store(held)?;
                    let parent_store = self
                        .custom_property_environments
                        .store(inherited)
                        .unwrap_or(std::ptr::null());
                    // SAFETY: The catalog keeps both stores alive. The rebuilt store transfers its
                    //         one Arc reference to the engine environment.
                    unsafe {
                        let store = &*store.cast::<crate::css::custom_properties::CustomPropertyStore>();
                        let values = store
                            .declared_names
                            .iter()
                            .map(|&name| (name, store.own_values[&name].value.clone_retained()))
                            .collect();
                        let rebuilt = store.resolved_child(parent_store, values);
                        self.custom_property_environments.adopt_engine_environment(
                            rebuilt,
                            inherited,
                            parent_store,
                            inputs.custom_property_registry(),
                        )
                    }
                }
            }
            // A node declaring none inherits its parent's environment as it is now.
            Some((_, parent_environment)) if has_match_answer && !self.node_declares_custom_properties(node) => {
                self.custom_property_environments.inheritable(parent_environment)
            }
            // A node declaring custom properties was resolved over its parent's environment as the
            // parent holds it, so it is stale only beneath a parent that moved.
            Some((parent, parent_environment))
                if has_match_answer
                    && self.computed_group_sets.custom_property_environment_identity(parent)
                        != Some(parent_environment) =>
            {
                self.engine_custom_property_environment(
                    node,
                    parent_environment,
                    inputs,
                    None,
                    &mut Counters::default(),
                )
                .unwrap_or(held)
            }
            _ => held,
        };
        scratch.current_custom_property_environments.insert(node, current);
        Some(current)
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
        let answer = self.current_published_answer(target.node())?;
        if !answer.cascade_winners_are_complete {
            return None;
        }
        let key = target.pseudo_element_target().map_or_else(
            || WinnerGroupKey::current(target.node(), self.program.version()),
            |pseudo| WinnerGroupKey::current_pseudo(target.node(), pseudo, self.program.version()),
        );
        let Lookup::Known((_, state)) = self.current_winner_groups().token_for(key) else {
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
    /// members take the first one's answer. `full_drive_reason` says why the row's reaction
    /// drives the record again in full whatever its winners did, if it does.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn engine_computed_record_delta(
        &mut self,
        node: StyleNodeID,
        cascade_winners_are_complete: bool,
        exact_flipped_rules: Option<FlippedRules>,
        parent_inputs_moved: ParentInputsMoved,
        full_drive_reason: Option<FullDriveReason>,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) -> Drive<RecordDelta> {
        // A child of an element the host composes once it installs the element's record inherits
        // that composition, which the host has made by the next wave of the pass.
        if self
            .tree
            .inheritance_parent(node)
            .is_some_and(|parent| scratch.nodes_composed_by_the_host.contains(&parent))
        {
            counters.bump(Counter::EngineComputedRecordBailRecordParent);
            return Err(Unanswered::AwaitsParent);
        }
        let delta = self.decide_engine_computed_record_delta(
            node,
            cascade_winners_are_complete,
            exact_flipped_rules,
            parent_inputs_moved,
            full_drive_reason,
            scratch,
            counters,
        );
        self.apply_substitution_effects(scratch);
        // A record that does not move keeps the composition the children read already, unless the
        // step it owes moves it.
        if let Ok((old, new)) = delta
            && (old != new || self.row_owes_a_transition_step(node))
            && self.host_composes_row(node, old.raw(), new.raw())
        {
            scratch.nodes_composed_by_the_host.insert(node);
        }
        delta
    }

    /// Whether a document hosts this engine. The host installs its font resolver when it creates the engine, and
    /// publishes the document's inputs and every table a record is computed against before it asks for any row. An
    /// engine no document hosts, such as a unit test's, computes no records. Only test builds can
    /// create such an engine, so the browser build has no unhosted path at all.
    #[cfg(test)]
    pub(super) fn computes_records(&self) -> bool {
        self.font_resolution.is_some()
    }

    /// A browser build's engine always has a document to host it.
    #[cfg(not(test))]
    pub(super) const fn computes_records(&self) -> bool {
        true
    }

    /// The step itself, which decides the substituted-record facts rather than writing them.
    #[allow(clippy::too_many_arguments)]
    fn decide_engine_computed_record_delta(
        &mut self,
        node: StyleNodeID,
        cascade_winners_are_complete: bool,
        exact_flipped_rules: Option<FlippedRules>,
        parent_inputs_moved: ParentInputsMoved,
        full_drive_reason: Option<FullDriveReason>,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) -> Drive<RecordDelta> {
        if !self.computes_records() {
            counters.bump(Counter::EngineComputedRecordBailUnhosted);
            return Err(Unanswered::Refused);
        }
        let pending_element = drive::PendingElement::take_for(&mut scratch.pending_element, node);
        if pending_element.is_none() {
            scratch.pseudo_deltas.clear();
            scratch.next_pseudo = 0;
            scratch.pseudo_uses_substitution = false;
            scratch.noted_substitution = None;
            scratch.element_explicitly_inherited_groups = 0;
            scratch.flipped_pseudo_rules = exact_flipped_rules.map_or(0, |flipped| flipped.pseudos);
        }
        let delta = match pending_element {
            Some(delta) => delta,
            None => self
                .engine_computed_element_record_delta(
                    node,
                    cascade_winners_are_complete,
                    exact_flipped_rules,
                    parent_inputs_moved,
                    full_drive_reason,
                    scratch,
                    FontDriveGoal::Complete,
                    counters,
                )?
                .delta(),
        };
        // A pseudo-element's container conditions ask about its originating element first, whose
        // record may have just made it another container: pseudo-element winners whose verdicts
        // moved with it are published again from the node's retained answer. What the verdicts
        // read of the element as a container is known only now, and the host takes it with the
        // record.
        if self.container_verdicts_moved(node) {
            if scratch.winner_republication().is_none_or(|republication| {
                self.republish_pseudo_winners_from_retained_answer(node, republication, counters)
                    .is_none()
            }) || !self.container_verdicts_stand(node, VerdictTargets::ElementAndPseudoElements)
            {
                counters.bump(Counter::EngineComputedRecordBailIncompleteWinners);
                self.abandon_engine_computed_record(node, scratch, counters);
                return Err(Unanswered::Refused);
            }
        } else {
            self.note_pseudo_container_effects_for_host(node);
        }
        // The element's pseudo-elements are settled beside its record, as the C++ computation
        // refreshes them after the element's own; a pseudo-element the engine cannot settle
        // sends the whole element to C++.
        let generation = self.winner_groups.generation();
        if let Err(unanswered) =
            self.engine_pseudo_records_beside(node, delta, None, generation, full_drive_reason, scratch, counters)
        {
            match unanswered {
                Unanswered::Suspended(_) => scratch.pending_element = Some(drive::PendingElement::new(node, delta)),
                Unanswered::Refused | Unanswered::AwaitsParent => {
                    self.abandon_engine_computed_record(node, scratch, counters);
                }
            }
            return Err(unanswered);
        }
        // What this element's record was computed from decides the fact when the computation
        // noted it; a record that stands unchanged keeps the fact the node already carries.
        let uses_substitution = scratch
            .noted_substitution
            .unwrap_or_else(|| self.nodes_with_substituted_records.contains(&node))
            || scratch.pseudo_uses_substitution
            || self
                .current_winner_groups()
                .pseudo_states(node)
                .any(|(_, version, state, priority_current)| {
                    version == self.program.version() && priority_current && self.state_has_substitutions(node, state)
                });
        scratch.element_uses_substitution = uses_substitution;
        scratch.substitution_effects.push((node, uses_substitution));
        Ok(delta)
    }

    /// The winners a driven element's record is computed from. A republish admits the row whatever
    /// the memory budget, and an answer that published nothing is matched again; a row that holds
    /// no winners even then is the host's, as is one a drive without the leave to republish holds
    /// no winners for.
    fn driven_element_winners(
        &mut self,
        node: StyleNodeID,
        winner_key: WinnerGroupKey,
        republication: Option<WinnerRepublication>,
        counters: &mut Counters,
    ) -> Option<(u64, CascadeStateID)> {
        if let Lookup::Known(token) = self.current_winner_groups().token_for(winner_key) {
            return Some(token);
        }
        self.rematch_driven_winners(node, republication?, counters);
        let token = match self.current_winner_groups().token_for(winner_key) {
            Lookup::Known(token) => Some(token),
            _ => None,
        };
        debug_assert!(
            token.is_some(),
            "a driven element holds no winners after matching again"
        );
        token
    }

    /// `exact_flipped_rules` are the rules that flipped for the node when the reaction is exactly
    /// those flips and nothing else the record depends on moved. `parent_inputs_moved` says which
    /// of the parent's inputs may have moved under the record.
    #[allow(clippy::too_many_arguments)]
    fn engine_computed_element_record_delta(
        &mut self,
        node: StyleNodeID,
        mut cascade_winners_are_complete: bool,
        exact_flipped_rules: Option<FlippedRules>,
        parent_inputs_moved: ParentInputsMoved,
        full_drive_reason: Option<FullDriveReason>,
        scratch: &mut EngineComputedRecordScratch,
        goal: FontDriveGoal,
        counters: &mut Counters,
    ) -> Drive<ElementAnswer> {
        use crate::css::computed_value_types::{
            STYLE_GROUP_INDEX_ANCHOR, STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_SURROUND,
        };
        use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LONGHAND_WORD_COUNT};

        let target = computed::ComputedStyleTarget::new(node, u8::MAX);
        // An element standing for its host's pseudo-element is cascaded from the host's rules.
        if let Some(backed) = self.backed_host_pseudo_element(node) {
            return self
                .engine_backing_element_record(node, backed, cascade_winners_are_complete, scratch, counters)
                .map(ElementAnswer::Delta);
        }
        // A row left out of winner publication can still carry a retained selector answer.
        // Rebuild its winners before comparing them with the record's cascade state: otherwise
        // an empty delta can describe yesterday's answer after this flush flipped a rule. So does
        // a row that holds no winners for the current program at all. Exact flips of
        // pseudo-element rules alone leave the element row as it was: the pseudo rows are
        // refreshed where the pseudo-elements settle.
        let winner_key = WinnerGroupKey::current(node, self.program.version());
        let stale_element_winners = (scratch.answer_or_declarations_moved
            && exact_flipped_rules.is_none_or(|flipped| flipped.element)
            && self.current_winner_groups().row_stamp(node) != Some(self.flush_stamp))
            || !matches!(self.current_winner_groups().token_for(winner_key), Lookup::Known(_));
        if stale_element_winners {
            // A read-only demand's drive reads the rows its private traversal published. One it
            // would have to republish is the host's: the republished row would outlive the demand.
            let Some(republication) = scratch.winner_republication() else {
                counters.bump(Counter::EngineComputedRecordBailWinner);
                return Err(Unanswered::Refused);
            };
            cascade_winners_are_complete = self.republish_driven_winners(node, republication, counters);
        }
        // The winners hold a gated rule where its container conditions held when they were
        // published; they answer for the node while every one decides as it did, over containers
        // its settled ancestors published. One a declined ancestor may still move waits for the
        // host to install that ancestor's record, as a row waits for its parent's. Winners
        // published while an ancestor was moving, or whose verdicts moved since (a container
        // resized, or an ancestor whose container type moved or that left display:none, whose
        // record this batch settled before the node), are published again from the node's
        // retained answer, over the containers as they stand now.
        if self.node_holds_container_gates(node) {
            if self.container_ancestor_is_unsettled(node, scratch) {
                counters.bump(Counter::EngineComputedRecordBailRecordParent);
                return Err(Unanswered::AwaitsParent);
            }
            if self.container_winners_are_stale(node) {
                let Some(complete) = scratch
                    .winner_republication()
                    .and_then(|republication| self.republish_winners_from_answer(node, republication, counters))
                else {
                    counters.bump(Counter::EngineComputedRecordBailIncompleteWinners);
                    return Err(Unanswered::Refused);
                };
                cascade_winners_are_complete = complete;
            }
            // A container the host styles in this update decides the verdicts once it is installed.
            // Those of the pseudo-elements are decided over the element's record, once it is settled.
            if !self.container_verdicts_stand(node, VerdictTargets::Element) {
                counters.bump(Counter::EngineComputedRecordBailRecordParent);
                return Err(Unanswered::AwaitsParent);
            }
        }
        // A custom property the cascade declares is no winner the columns hold; the engine
        // computes the environment it decides itself.
        if !cascade_winners_are_complete && !self.cascade_winners_are_complete_but_for_custom_properties(node) {
            counters.bump(Counter::EngineComputedRecordBailIncompleteWinners);
            return Err(Unanswered::Refused);
        }
        // The winners the record was computed from, against the winners the node holds now: the
        // same comparison a C++ publication makes to select what it recomputes.
        let Some((generation, state)) =
            self.driven_element_winners(node, winner_key, scratch.winner_republication(), counters)
        else {
            counters.bump(Counter::EngineComputedRecordBailWinner);
            return Err(Unanswered::Refused);
        };
        let facts = self.computed_group_sets.adjustment_facts(node);
        // Moved root inputs reach every row below the root. The root's font the root-input probe
        // drove is left pending for the root's own row, which resumes it in full the same way.
        let root_inputs_moved = if facts & bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0 {
            scratch.font_drive.is_pending_for(node)
        } else {
            scratch.root_font_inputs_changed
        };
        // An element's animations compose over the record its winners decide, and the host samples
        // them over the record it installs. A record demand installs none of its own.
        if facts & bridge::element_adjustment_fact::HAS_ANIMATIONS != 0 && !scratch.host_applies_animation_plans {
            counters.bump(Counter::EngineComputedRecordBailWinnerElement);
            return Err(Unanswered::Refused);
        }
        let Some(old_style_record) = self.computed_group_sets.assigned_style_record(node) else {
            return self.engine_cold_record(node, (generation, state), scratch, goal, counters);
        };
        // A row derives the record beneath the composition the element's animations laid over it:
        // the delta starts at the record the winners decided, and the composition stays the record
        // the row moves the element away from.
        let Some(underlying_style_record) = self.computed_group_sets.underlying_style_record(old_style_record) else {
            counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return Err(Unanswered::Refused);
        };
        let composes_animations = underlying_style_record != old_style_record;
        if composes_animations && !scratch.host_applies_animation_plans {
            counters.bump(Counter::EngineComputedRecordBailRecordOverlay);
            return Err(Unanswered::Refused);
        }
        // A record C++ computed holds no cascade state, so there is no earlier state to take a
        // delta from, and the state of a record from a replaced rule program names winners of
        // another generation: either record is driven again in full from the node's winners,
        // which binds the state. The old record still supplies the before-change values.
        let (delta, holds_no_current_cascade_state) = match self.computed_group_sets.cascade_state(target) {
            Some((previous_generation, previous_state)) if previous_generation == generation => {
                (self.winner_groups.semantic_delta(Some(previous_state), state), false)
            }
            Some(_) | None => (self.winner_groups.semantic_delta(Some(state), state), true),
        };
        // No delta names the winners such a record is driven from, so they are checked as a first
        // record's are: one starting an animation keeps the record in C++, unless it declares CSS
        // animations and the host can be handed the plan the record decides.
        if holds_no_current_cascade_state
            && self
                .winner_groups
                .semantic_delta_properties(None, state)
                .any(|property| {
                    self.first_record_winner_needs_cpp(property)
                        && !(property_declares_css_animations(property)
                            && self.may_plan_css_animations(node, state, scratch))
                })
        {
            counters.bump(Counter::EngineComputedRecordBailProperty);
            return Err(Unanswered::Refused);
        }
        // A record driven from its winners alone takes its transition declarations from them. A
        // moved display is applied after the step, as C++ applies it after the step it runs. The row
        // carries what is decided here to the host, for a record that moves.
        let moves_transition_declarations = if holds_no_current_cascade_state {
            self.winner_groups
                .semantic_delta_properties(None, state)
                .any(property_declares_transitions)
        } else {
            delta
                .properties()
                .iter()
                .any(|&property| property_declares_transitions(property))
        };
        let owes_a_transition_step = self.decide_transition_step(
            node,
            underlying_style_record,
            moves_transition_declarations,
            scratch.host_applies_animation_plans,
            counters,
        )?;
        // A step decides over a moved base record, or over one an ancestor's moved style reaches
        // without moving it: the base the record holds an inherited animated value of.
        let owes_a_transition_step_to = |new_style_record| {
            owes_a_transition_step
                && (new_style_record != underlying_style_record || parent_inputs_moved.inherited_style)
        };
        let mut inputs = self.document_style_computation_inputs;
        if let Some((root, root_inputs)) = scratch.root_element_inputs
            && root == node
        {
            root_inputs.apply_to(&mut inputs);
        }
        // The environment the node's own custom declarations resolve to over the parent's. A node
        // declaring none keeps its record's, which is the parent's; a moved environment
        // republishes the record under the new one. A registered name declared here computes
        // against the record's font until a drive settles the font anew.
        let has_registered_declarations = self.declares_registered_custom_property(node, None, &inputs);
        // Every installed record is published with an environment; a record without is
        // republished as moved.
        let old_environment = self
            .computed_group_sets
            .style_record_custom_property_environment(underlying_style_record.raw());
        debug_assert!(
            old_environment.is_some(),
            "an installed record was published with a custom-property environment"
        );
        let (mut environment, mut current_environment, parent_environment) = {
            let parent = self
                .record_inheritance_parent(node)
                .inspect_err(|_| counters.bump(Counter::EngineComputedRecordBailRecordParent))?;
            let parent_environment = match parent {
                Some(parent) => self.held_custom_property_environment(parent, counters)?,
                None => 0,
            };
            // A function's container condition the engine cannot decide yet waits, as the row does.
            let environment = self
                .engine_custom_property_environment(node, parent_environment, &inputs, None, counters)
                .inspect_err(|&unanswered| {
                    if unanswered == Unanswered::Refused {
                        counters.bump(Counter::EngineComputedRecordBailCustomProperties);
                    }
                })?;
            // An unmoved environment is the one the record holds, so the current one is always
            // what the declarations resolved to.
            (
                (old_environment != Some(environment)).then_some(environment),
                environment,
                parent_environment,
            )
        };
        // A moved environment reaches every winner written with a substitution, and so does a
        // moved custom-property registry. A winner written with `attr()` computes to what the
        // element's attributes hold now, one written with `inherit()` to what the parent's
        // environment holds now, one written with `if()` to what its conditions say now, one
        // calling a custom function to what the function's definition says now, one reading the
        // element's place among its siblings to where it stands now, and one written with a
        // container-relative length to what its containers measure now, which no winner delta
        // shows. Such a record is driven again in full, as is one holding no current cascade
        // state, one under a moved document environment, one whose reaction stands for inputs no
        // winner shows, and one that rolled a property back below a substituted revert keyword,
        // to declarations no winner names.
        let drive_in_full = holds_no_current_cascade_state
            || scratch.document_environment_moved
            || (scratch.viewport_moved && self.record_reads_the_viewport(underlying_style_record))
            || full_drive_reason.is_some()
            // A composition holds what the element inherits of its parent's animations beside what
            // its own sample: the record beneath it is driven in full, inheriting from the parent's
            // composition, and the host samples the element's own animations over it.
            || composes_animations
            || ((environment.is_some() || self.custom_property_registrations_changed)
                && self.state_has_substitutions(node, state))
            || self.state_reads_beyond_environment(node, state)
            || self.record_reads_sibling_position(node, state)
            || self.record_rolls_back_substitution(node);
        // The winners the record was computed from are the winners now when the delta is empty;
        // the flips the reaction holds are reflected in them once the row holds the cascade of
        // the node's current answer (custom properties are no winners).
        let flips_are_reflected = exact_flipped_rules.is_some_and(|flipped| {
            !flipped.element || self.current_winner_groups().row_stamp(node) == Some(self.flush_stamp)
        });
        // The document element's own record stays with C++ when nothing says why it is asked
        // for, but nothing says its font inputs moved either: keep the host's root-metric route
        // rather than declaring the root's computation unsupported, which takes every descendant
        // with it.
        if goal == FontDriveGoal::RootInputs && delta.is_empty() && !flips_are_reflected {
            return Ok(ElementAnswer::RootInputs(None));
        }
        let scope = self.record_drive_scope(
            node,
            state,
            &delta,
            flips_are_reflected,
            parent_inputs_moved.any() || root_inputs_moved || drive_in_full,
            environment,
            has_registered_declarations,
            scratch.pseudo_inputs_alone,
        );
        match scope {
            DriveScope::Stands | DriveScope::Environment(_) if goal == FontDriveGoal::RootInputs => {
                // NB: This proof covers the retained font, without publishing the root's
                //     remaining properties or custom-property environment during preparation.
                return Ok(ElementAnswer::RootInputs(Some(
                    self.root_font_inputs_from_record(old_style_record).or_refused()?,
                )));
            }
            // A record reading its place among its siblings is driven in full, so one that stands
            // reads none.
            DriveScope::Stands => {
                counters.bump(Counter::EngineComputedRecordUnchangedWinners);
                counters.bump(Counter::CascadeWinnerDeltaStops);
                self.note_engine_computed_record(
                    node,
                    (old_style_record, old_style_record),
                    (generation, state),
                    false,
                    0,
                    0,
                    counters,
                );
                return Ok(ElementAnswer::Delta((old_style_record, old_style_record)));
            }
            DriveScope::Environment(environment) => {
                let delta = self
                    .computed_group_sets
                    .republish_engine_record_with_environment(node, environment)
                    .expect("an assigned record without an overlay moves to any environment");
                counters.bump(Counter::EngineComputedRecordUnchangedWinners);
                self.note_engine_computed_record(node, delta, (generation, state), false, 0, 0, counters)
                    .owes_a_transition_step = owes_a_transition_step_to(delta.1);
                return Ok(ElementAnswer::Delta(delta));
            }
            // Standing winners driven in full for a reason they do not show still stop the
            // cascade's winner delta.
            DriveScope::Full if delta.is_empty() => counters.bump(Counter::CascadeWinnerDeltaStops),
            DriveScope::Partial | DriveScope::Full => {}
        }
        let full_drive = scope == DriveScope::Full;
        let delta_property_count = delta.properties().len() as u64;
        // Partial drives can share across parents whose inherited inputs agree. Keep the full
        // parent record in the key when a non-inherited property explicitly inherits, including
        // through substitution, or when a full drive may read more of the parent's style.
        let parent = self.tree.inheritance_parent(node);
        let parent_record = parent.and_then(|parent| self.computed_group_sets.assigned_style_record(parent));
        let mut cohort_parent = RecordDeltaParent::Exact(parent_record.map_or(0, |record| record.raw()));
        if !full_drive
            && let (Some(parent), Some(parent_record)) = (parent, parent_record)
            && !self.state_has_substitutions(node, state)
            && let Some(inputs) = self.cold_record_parent(node, parent, parent_record, state)
        {
            cohort_parent = RecordDeltaParent::Inputs(inputs);
        }
        // A record whose winners read beyond its environment is the element's alone, as is one
        // driven in full under registered custom properties, which its own font decides.
        let record_is_the_elements_alone =
            self.state_reads_beyond_environment(node, state) || (has_registered_declarations && full_drive);
        let cohort = (!record_is_the_elements_alone).then(|| {
            (
                underlying_style_record.raw(),
                state,
                facts,
                cohort_parent,
                // An unchanged environment is not necessarily empty. Distinguish it from a
                // record moving to the empty environment when both started with the same style.
                current_environment,
                RootFontInputs::from_document(&inputs),
                self.monospace_cohort_key(computed::ComputedStyleTarget::new(node, u8::MAX), state),
                self.sibling_position_key(node, state),
            )
        });
        if let Some(&(new_style_record, explicitly_inherited_groups, reads_sibling_position)) = cohort
            .as_ref()
            .and_then(|cohort| scratch.cohorts.get(cohort))
            .filter(|&&(record, ..)| self.record_answers_counter_styles_for(record, node))
        {
            self.note_node_substitution(node, scratch, state, current_environment);
            // The mark is per node: an element taking the record owes its own parent the mark.
            scratch.element_explicitly_inherited_groups = explicitly_inherited_groups;
            let detached_composition = composes_animations
                .then(|| self.computed_group_sets.detach_composition(node))
                .flatten();
            let Some(delta) =
                self.computed_group_sets
                    .assign_engine_computed_record(node, underlying_style_record, new_style_record)
            else {
                if let Some(detached) = detached_composition {
                    self.computed_group_sets.reattach_composition(node, detached);
                }
                return Err(Unanswered::Refused);
            };
            let delta = (old_style_record, delta.1);
            if delta.0 == delta.1 {
                counters.bump(Counter::ComputedWinnerPropagationStops);
            }
            let pending = self.note_engine_computed_record(
                node,
                delta,
                (generation, state),
                reads_sibling_position,
                delta_property_count,
                0,
                counters,
            );
            pending.detached_composition = detached_composition;
            pending.owes_a_transition_step = owes_a_transition_step_to(delta.1);
            counters.bump(Counter::EngineComputedRecordCohortHits);
            return Ok(ElementAnswer::Delta(delta));
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
        // The record being driven again has a view. A record without one cannot say what a
        // partial drive would leave standing: it is driven in full.
        let mut old_record_is_unreadable = false;
        let (writing_mode, direction) = match self
            .computed_group_sets
            .style_record_view(underlying_style_record.raw())
        {
            Some(view) => {
                let inherited_box = unsafe {
                    view.payloads[crate::css::computed_value_types::STYLE_GROUP_INDEX_INHERITED_BOX]
                        .cast::<crate::css::computed_values::InheritedBoxValues>()
                        .deref()
                };
                (inherited_box.writing_mode, inherited_box.direction)
            }
            None => {
                debug_assert!(false, "the record being driven again has a view");
                old_record_is_unreadable = true;
                (
                    crate::css::css_enums::writing_mode::HORIZONTAL_TB,
                    crate::css::css_enums::direction::LTR,
                )
            }
        };
        for &property in delta.properties() {
            // Any animation the engine cannot hand the host a plan for starts from the C++
            // computation; a transition declaration is the step's to act on.
            if property_starts_animation(property)
                && !property_declares_transitions(property)
                && !(property_declares_css_animations(property) && self.may_plan_css_animations(node, state, scratch))
            {
                counters.bump(Counter::EngineComputedRecordBailProperty);
                return Err(Unanswered::Refused);
            }
            groups_to_rebuild |= moved_longhand_groups(self.style_groups, property);
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
                groups_to_rebuild |= moved_longhand_groups(self.style_groups, counterpart);
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
            // A record holding no table cannot say which values read currentcolor: it is driven
            // in full, as a partial drive of it is.
            let dependencies = self.computed_group_sets.current_color_dependency_mask(target);
            let dependent_properties = self.computed_group_sets.current_color_dependency_properties(target);
            old_record_is_unreadable |= dependencies.is_none() || dependent_properties.is_none();
            groups_to_rebuild |= dependencies.unwrap_or(0);
            // The dependents compute again from their specified values, so the table spells them
            // the way a fresh computation does, not the way an inherited-group swap resolved them.
            let dependent_properties = dependent_properties.unwrap_or_default();
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
        // A partial delta that reaches the font group reaches every value the font feeds, so it is
        // driven in full, as a partial drive whose driver inputs moved is below, and so is one of
        // a record that cannot say what it would leave standing.
        let mut in_full_after_all =
            !full_drive && (old_record_is_unreadable || groups_to_rebuild & (1 << STYLE_GROUP_INDEX_FONT) != 0);
        if goal == FontDriveGoal::RootInputs && !full_drive && !in_full_after_all {
            // NB: No font property moved, but borrowing the retained font still needs the
            //     proof that only the named rule flips changed the computation's inputs.
            if exact_flipped_rules.is_some() {
                return Ok(ElementAnswer::RootInputs(Some(
                    self.root_font_inputs_from_record(old_style_record).or_refused()?,
                )));
            }
            // Without that proof the root's font is computed rather than borrowed.
            in_full_after_all = true;
        }
        if full_drive || in_full_after_all {
            groups_to_rebuild = (1 << crate::css::table_group_builder::group_index::COUNT) - 1;
        }

        // A store whose values substitute `attr()` or `inherit()` holds this element's attributes
        // or its parent's values, and is the element's alone.
        let element_alone = self.state_reads_beyond_environment(node, state);
        let mut store = match scratch
            .stores
            .get(&(state, current_environment))
            .filter(|_| !element_alone)
        {
            Some(store) => store.clone(),
            None => {
                let mut substituted = false;
                let store = std::sync::Arc::new(self.cascaded_store_for_state(
                    node,
                    state,
                    None,
                    custom_property_cascade::SubstitutionEnvironment {
                        own: current_environment,
                        inherited: parent_environment,
                    },
                    &mut substituted,
                    counters,
                )?);
                scratch.store_capacity_bytes += store.capacity_bytes();
                if !element_alone && !self.records_are_the_elements_alone(node) {
                    scratch.stores.insert((state, current_environment), store.clone());
                }
                if substituted {
                    scratch.substituted_states.insert((state, current_environment));
                }
                store
            }
        };
        self.note_node_substitution(node, scratch, state, current_environment);
        let partial = if full_drive || in_full_after_all {
            None
        } else {
            Some(self.engine_driven_table(node, underlying_style_record, &store, &selected, &inputs, counters)?)
        };
        let driver_input_moved = matches!(partial, Some(PartialDrive::DriverInputMoved));
        let DrivenTable {
            table,
            length,
            longhand_evaluations,
            font,
            explicitly_inherited_groups,
        } = match partial {
            Some(PartialDrive::Driven(partial)) => partial,
            // A partial drive whose driver inputs moved reaches values it did not select, so the
            // record is driven in full and every group is rebuilt.
            Some(PartialDrive::DriverInputMoved) | None => {
                if driver_input_moved {
                    groups_to_rebuild = (1 << crate::css::table_group_builder::group_index::COUNT) - 1;
                }
                let subject = self.element_drive_subject(node, counters)?;
                let mut driven = self.engine_full_drive(
                    subject,
                    Some(underlying_style_record),
                    &store,
                    &inputs,
                    &mut scratch.font_drive,
                    goal,
                    has_registered_declarations,
                    counters,
                )?;
                // The registered custom properties compute against the font the drive settled,
                // and the winners substitute what they computed to.
                if let FullDrive::AwaitsRegisteredContext(registered) = driven {
                    let (settled, settled_store, _) = self.store_over_registered_context(
                        subject.target,
                        state,
                        parent_environment,
                        |engine, counters| {
                            engine
                                .engine_custom_property_environment(
                                    node,
                                    parent_environment,
                                    &inputs,
                                    Some(&registered),
                                    counters,
                                )
                                .inspect_err(|&unanswered| {
                                    if unanswered == Unanswered::Refused {
                                        counters.bump(Counter::EngineComputedRecordBailCustomProperties);
                                    }
                                })
                        },
                        scratch,
                        counters,
                    )?;
                    current_environment = settled;
                    environment = (old_environment != Some(settled)).then_some(settled);
                    store = std::sync::Arc::new(settled_store);
                    self.note_node_substitution(node, scratch, state, current_environment);
                    driven = self.engine_full_drive(
                        subject,
                        Some(underlying_style_record),
                        &store,
                        &inputs,
                        &mut scratch.font_drive,
                        goal,
                        false,
                        counters,
                    )?;
                }
                match driven {
                    FullDrive::Driven(driven) => driven,
                    FullDrive::RootInputs(root_inputs) => return Ok(ElementAnswer::RootInputs(Some(root_inputs))),
                    FullDrive::AwaitsRegisteredContext(_) => {
                        unreachable!("a drive resumed with its registered context waits for nothing")
                    }
                }
            }
        };
        let parent_in_display_none_subtree = self
            .tree
            .inheritance_parent(node)
            .and_then(|parent| self.computed_group_sets.assigned_style_record(parent))
            .and_then(|record| self.computed_group_sets.style_record_view(record.raw()))
            .is_some_and(|view| view.dependency_flags & (1 << 2) != 0);
        // The record names the counter-style registry its `content` or `list-style-type` reads,
        // which a moved `content` may have started or stopped reading.
        let counter_style_registry = self.table_counter_style_environment_identity(target, &table);
        let detached_composition = composes_animations
            .then(|| self.computed_group_sets.detach_composition(node))
            .flatten();
        let assembly = self.computed_group_sets.replace_engine_computed_table(
            node,
            underlying_style_record,
            old_style_record,
            table,
            groups_to_rebuild,
            &length,
            font.as_ref(),
            parent_in_display_none_subtree,
            environment,
            Some(counter_style_registry),
        );
        self.settle_computed_memory();
        let Some(assembly) = assembly else {
            if let Some(detached) = detached_composition {
                self.computed_group_sets.reattach_composition(node, detached);
            }
            return Err(Unanswered::Refused);
        };
        counters.add(
            Counter::ComputedOutputGroupsCanonicalized,
            u64::from(assembly.canonicalized_groups),
        );
        if assembly.group_set_unchanged {
            counters.bump(Counter::ComputedWinnerPropagationStops);
        }
        let delta = assembly.delta;
        // The record reads the element's place among its siblings where its state's winners do, as
        // the store substitutes them.
        let reads_sibling_position = store.uses_tree_counting_function(self);
        let pending = self.note_engine_computed_record(
            node,
            delta,
            (generation, state),
            reads_sibling_position,
            delta_property_count,
            longhand_evaluations,
            counters,
        );
        pending.detached_composition = detached_composition;
        pending.owes_a_transition_step = owes_a_transition_step_to(delta.1);
        // A record driven in full stands for a cohort keyed by the parent's inherited inputs only
        // when the drive was partial.
        if !driver_input_moved
            && !self.records_are_the_elements_alone(node)
            && let Some(cohort) = cohort
        {
            scratch
                .cohorts
                .insert(cohort, (delta.1, explicitly_inherited_groups, reads_sibling_position));
        }
        scratch.element_explicitly_inherited_groups = explicitly_inherited_groups;
        Ok(ElementAnswer::Delta(delta))
    }

    /// Move the record the host holds for a node to the environment C++ refreshed its inherited
    /// custom-property data to, without recomputing anything: what an inherited-custom-properties
    /// reaction C++ settled by refreshing the data alone publishes. The host installs it in the
    /// middle of a batch, so it is never a record the engine assigned for a row the host has yet to
    /// apply, which moves more than the environment. The new record's identity, or nothing when the
    /// node holds no base record to move.
    pub(crate) fn republish_record_environment(&mut self, node: StyleNodeID, environment: u64) -> Option<u64> {
        let held_style_record = computed::FinalStyleRecordID::from_raw(*self.held_style_records.get(&node)?)?;
        self.computed_group_sets
            .republish_style_record_with_environment(node, held_style_record, environment)
            .map(|style_record| style_record.raw())
    }

    /// Note whether the node's record was computed with a substituted winner, for C++ to record
    /// the node as a reader of custom properties when it installs the record. The step records
    /// the fact; the boundary that installs the record applies it.
    fn note_node_substitution(
        &self,
        node: StyleNodeID,
        scratch: &mut EngineComputedRecordScratch,
        state: CascadeStateID,
        environment: u64,
    ) {
        let substituted = scratch.substituted_states.contains(&(state, environment));
        scratch.noted_substitution = Some(substituted);
        scratch.substitution_effects.push((node, substituted));
    }

    /// The environment and store a full drive resumes with once it settled the context the
    /// target's registered custom properties compute against: `resolve_environment` resolves the
    /// target's custom declarations again with that context, and the state's winners substitute
    /// what they computed to. Returns the settled environment, the store and whether it
    /// substituted.
    fn store_over_registered_context(
        &mut self,
        target: computed::ComputedStyleTarget,
        state: CascadeStateID,
        inherited_environment: u64,
        resolve_environment: impl FnOnce(&mut Self, &mut Counters) -> Drive<u64>,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) -> Drive<(u64, WinnerStore, bool)> {
        let environment = resolve_environment(self, counters)?;
        let mut substituted = false;
        let store = self.cascaded_store_for_state(
            target.node(),
            state,
            target.is_pseudo().then_some(target.pseudo_kind()),
            custom_property_cascade::SubstitutionEnvironment {
                own: environment,
                inherited: inherited_environment,
            },
            &mut substituted,
            counters,
        )?;
        scratch.store_capacity_bytes += store.capacity_bytes();
        if substituted {
            scratch.substituted_states.insert((state, environment));
        }
        Ok((environment, store, substituted))
    }

    /// Write the substituted-record facts the step decided. The set is a retained per-node fact
    /// C++ reads when it installs a record, so the step decides it once and the record's
    /// installation boundary writes it once, rather than the step reading its own writes back.
    fn apply_substitution_effects(&mut self, scratch: &mut EngineComputedRecordScratch) {
        for (node, uses_substitution) in scratch.substitution_effects.drain(..) {
            if uses_substitution {
                self.nodes_with_substituted_records.insert(node);
            } else {
                self.nodes_with_substituted_records.remove(&node);
            }
        }
    }

    /// Account for an element record the engine derived, which reads the node's place among its
    /// siblings as `reads_sibling_position` says, in place of whatever the record it replaces read.
    #[allow(clippy::too_many_arguments)]
    fn note_engine_computed_record(
        &mut self,
        node: StyleNodeID,
        delta: (computed::FinalStyleRecordID, computed::FinalStyleRecordID),
        cascade_state: (u64, CascadeStateID),
        reads_sibling_position: bool,
        delta_property_count: u64,
        longhand_evaluations: u32,
        counters: &mut Counters,
    ) -> &mut PendingEngineComputedRecord {
        counters.add(Counter::CascadeWinnerDeltaProperties, delta_property_count);
        counters.add(Counter::ComputedWinnerDeltaPropertiesConsumed, delta_property_count);
        counters.bump(Counter::EngineComputedRecordDeltas);
        // The containers the node's descendants ask about are the ones its settled record
        // describes, as the host publishes them when it installs a record: a descendant this batch
        // derives after it reads the container as the host will leave it.
        self.set_element_container_query_inputs(node, delta.1.raw());
        self.note_sibling_position_reads(node, u8::MAX, reads_sibling_position);
        self.note_container_unit_effects_for_host(node, false, cascade_state.1, delta.0);
        let records = self.engine_computed_records_pending.entry(node).or_default();
        records.push(PendingEngineComputedRecord {
            node,
            pseudo_kind: u8::MAX,
            old_style_record: delta.0,
            new_style_record: delta.1,
            cascade_state: Some(cascade_state),
            longhand_evaluations,
            owes_a_transition_step: false,
            detached_composition: None,
        });
        let last = records.len() - 1;
        &mut records[last]
    }

    /// C++ installed the record the engine derived for `node`: the winner state it was computed
    /// from becomes the node's cascade state, and the answer counts as consumed.
    pub(crate) fn acknowledge_engine_computed_record(&mut self, node: StyleNodeID, counters: &mut Counters) {
        if let Some(pending_records) = self.engine_computed_records_pending.remove(&node) {
            for pending in pending_records {
                let target = computed::ComputedStyleTarget::new(node, pending.pseudo_kind);
                // A pseudo-element settled as gone is removed now that C++ has cleared its style.
                if pending.pseudo_kind != u8::MAX && pending.new_style_record == computed::FinalStyleRecordID::NONE {
                    self.remove_computed_pseudo(node, pending.pseudo_kind, counters);
                    continue;
                }
                self.computed_group_sets.take_pending_cascade_state(target);
                if let Some(cascade_state) = pending.cascade_state {
                    self.computed_group_sets.bind_cascade_state(target, cascade_state);
                }
                // The host holds the record that replaced the composition now.
                if let Some(detached) = pending.detached_composition {
                    self.computed_group_sets.release_detached_composition(detached);
                }
                // A pseudo-element's record was computed from this very state, as the retained
                // cascade would have observed had C++ computed it.
                if pending.pseudo_kind != u8::MAX {
                    self.computed_group_sets
                        .observe_pseudo_retained_cascade_state(target, pending.cascade_state);
                }
                counters.add(
                    Counter::EngineComputedLonghandEvaluations,
                    u64::from(pending.longhand_evaluations),
                );
            }
        }
        self.mark_published_answer_observed(node);
    }

    /// The host computed the target's record itself: the record the engine derived for it is not
    /// installed, and a composition the derivation detached gives way to what the host published.
    pub(super) fn forget_engine_computed_record(&mut self, target: computed::ComputedStyleTarget) {
        let Some(pending_records) = self.engine_computed_records_pending.get_mut(&target.node()) else {
            return;
        };
        let computed_group_sets = &mut self.computed_group_sets;
        pending_records.retain(|pending| {
            if pending.pseudo_kind != target.pseudo_kind() {
                return true;
            }
            if let Some(detached) = pending.detached_composition.take() {
                computed_group_sets.release_detached_composition(detached);
            }
            false
        });
        if pending_records.is_empty() {
            self.engine_computed_records_pending.remove(&target.node());
        }
    }

    /// The transaction's outputs are gone: every derived record C++ did not install goes back to
    /// the record the node held, unless a publication has moved the node on since.
    pub(super) fn discard_engine_computed_records(&mut self, counters: &mut Counters) {
        for pending in std::mem::take(&mut self.engine_computed_records_pending)
            .into_values()
            .flatten()
        {
            if pending.pseudo_kind != u8::MAX {
                self.revert_engine_computed_pseudo_record(pending, counters);
                continue;
            }
            self.revert_engine_computed_element_record(pending);
        }
    }

    /// Put an element back on the record it held before the engine derived one for it, unless a
    /// publication has moved it on since, with the composition the derivation detached composing
    /// over it again, and its container query inputs back on the record the host holds: the
    /// derived record they were read from goes with the derivation, and may be reclaimed.
    fn revert_engine_computed_element_record(&mut self, pending: PendingEngineComputedRecord) {
        let previous = pending
            .detached_composition
            .as_ref()
            .map_or(pending.old_style_record, |detached| detached.base);
        self.computed_group_sets
            .revert_engine_computed_record(pending.node, pending.new_style_record, previous);
        if let Some(detached) = pending.detached_composition {
            self.computed_group_sets.reattach_composition(pending.node, detached);
        }
        let held_style_record = self.held_style_records.get(&pending.node).copied().unwrap_or(0);
        self.set_element_container_query_inputs(pending.node, held_style_record);
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
        goal: FontDriveGoal,
        counters: &mut Counters,
    ) -> Drive<ElementAnswer> {
        if !self.computes_records() {
            counters.bump(Counter::EngineComputedRecordBailUnhosted);
            return Err(Unanswered::Refused);
        }
        let target = computed::ComputedStyleTarget::new(node, u8::MAX);
        let (_, state) = cascade_state;
        let mut inputs = self.document_style_computation_inputs;
        if let Some((root, root_inputs)) = scratch.root_element_inputs
            && root == node
        {
            root_inputs.apply_to(&mut inputs);
        }
        let facts = self.computed_group_sets.adjustment_facts(node);
        // The document element inherits from the initial values.
        let parent = self
            .record_inheritance_parent(node)
            .inspect_err(|_| counters.bump(Counter::EngineComputedRecordBailRecordParent))?;
        let parent_record = parent.and_then(|parent| self.computed_group_sets.assigned_style_record(parent));
        // The document element's environment is its own, which is nothing without declarations;
        // any other node's is its declarations resolved over the parent's.
        let parent_environment = match parent {
            Some(parent) => {
                let held = self.held_custom_property_environment(parent, counters)?;
                self.current_custom_property_environment(parent, &inputs, scratch)
                    .unwrap_or(held)
            }
            None => 0,
        };
        let pseudo_styles = self.pseudo_style_mask_or_rematch(node, counters);
        // A record whose winners read beyond its environment is the element's alone.
        let cache_key = parent
            .zip(parent_record)
            .filter(|_| !self.state_reads_beyond_environment(node, state))
            .and_then(|(parent, parent_record)| self.cold_record_parent(node, parent, parent_record, state))
            .map(|parent| ColdRecordKey {
                monospace_recascaded_font_size: self
                    .monospace_cohort_key(computed::ComputedStyleTarget::new(node, u8::MAX), state),
                sibling_position: self.sibling_position_key(node, state),
                parent,
                previous_style_record: 0,
                generation: cascade_state.0,
                state,
                facts: cold_record_facts(facts),
                pseudo_styles,
                environment: self.custom_property_environments.inheritable(parent_environment),
                font_environment_generation: inputs.font_environment_generation,
                root_font_inputs: RootFontInputs::from_document(&inputs),
            });
        // A winner that starts an animation keeps the record in C++, unless it declares CSS
        // animations and the host can be handed the plan the record decides, which is asked of every
        // node, a shared record's too.
        for property in self.winner_groups.semantic_delta_properties(None, state) {
            if self.first_record_winner_needs_cpp(property)
                && !(property_declares_css_animations(property) && self.may_plan_css_animations(node, state, scratch))
            {
                counters.bump(Counter::EngineComputedRecordBailProperty);
                return Err(Unanswered::Refused);
            }
        }
        let delta_property_count = self.winner_groups.winner_count_in_state(state) as u64;
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
                counters,
            )
        {
            return Ok(ElementAnswer::Delta(delta));
        }
        // A registered name declared here computes provisionally against the parent's font until
        // the drive settles the element's own.
        let has_registered_declarations = self.declares_registered_custom_property(node, None, &inputs);
        // A function's container condition the engine cannot decide yet waits, as the row does.
        let mut environment = self
            .engine_custom_property_environment(node, parent_environment, &inputs, None, counters)
            .inspect_err(|&unanswered| {
                if unanswered == Unanswered::Refused {
                    counters.bump(Counter::EngineComputedRecordBailCustomProperties);
                }
            })?;
        // The state has to be one the engine can compute from before any record is shared under
        // it: a record C++ computed for a per-element value, such as a `random()` draw, is that
        // element's alone. A store with substituted values is the environment's as well as the
        // state's, and admits nothing for the state alone.
        let element_alone = self.state_reads_beyond_environment(node, state);
        let mut store = match scratch.stores.get(&(state, environment)).filter(|_| !element_alone) {
            Some(store) => store.clone(),
            None => {
                let mut substituted = false;
                let store = self.cascaded_store_for_state(
                    node,
                    state,
                    None,
                    custom_property_cascade::SubstitutionEnvironment {
                        own: environment,
                        inherited: parent_environment,
                    },
                    &mut substituted,
                    counters,
                );
                scratch.computability.remember(
                    (
                        node,
                        cascade_state.0,
                        state,
                        environment,
                        inputs.custom_property_registration_generation,
                    ),
                    store.as_ref().ok().map(|store| store.record_reads(self)),
                );
                let store = std::sync::Arc::new(store?);
                scratch.store_capacity_bytes += store.capacity_bytes();
                if !element_alone && !self.records_are_the_elements_alone(node) {
                    scratch.stores.insert((state, environment), store.clone());
                }
                if substituted {
                    scratch.substituted_states.insert((state, environment));
                }
                store
            }
        };
        self.note_node_substitution(node, scratch, state, environment);
        // A record whose winners read beyond its environment is the element's alone, as is one
        // declaring registered custom properties, which its own font decides.
        let cache_key = parent
            .zip(parent_record)
            .filter(|_| !element_alone && !has_registered_declarations)
            .and_then(|(parent, parent_record)| self.cold_record_parent(node, parent, parent_record, state))
            .map(|parent| ColdRecordKey {
                monospace_recascaded_font_size: self
                    .monospace_cohort_key(computed::ComputedStyleTarget::new(node, u8::MAX), state),
                sibling_position: self.sibling_position_key(node, state),
                parent,
                previous_style_record: 0,
                generation: cascade_state.0,
                state,
                facts: cold_record_facts(facts),
                pseudo_styles,
                environment,
                font_environment_generation: inputs.font_environment_generation,
                root_font_inputs: RootFontInputs::from_document(&inputs),
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
            counters,
        ) {
            return Ok(ElementAnswer::Delta(delta));
        }
        // The record reads the element's place among its siblings where its state's winners do, as
        // the store substitutes them.
        let reads_sibling_position = store.uses_tree_counting_function(self);
        let no_resource_contexts = store.reads_no_resource_contexts(self);
        let donor = cache_key.and_then(|key| {
            let donor_key = ColdRecordDonorKey {
                parent: key.parent,
                generation: key.generation,
                property_shape_hash: self.winner_groups.property_shape_hash(state),
                facts: key.facts,
                pseudo_styles: key.pseudo_styles,
                environment: key.environment,
                font_environment_generation: key.font_environment_generation,
                root_font_inputs: key.root_font_inputs,
                sibling_position: key.sibling_position,
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
                        .semantic_delta_properties(Some(donor.state), state)
                        .count()
                })
        });
        if !scratch.font_drive.is_pending_for(node)
            && let Some(donor) = donor
        {
            let donor_delta = self.winner_groups.semantic_delta(Some(donor.state), state);
            if let Some((groups_to_rebuild, selected)) =
                self.cold_record_donor_selection(donor, donor_delta.properties())
                && let Ok(PartialDrive::Driven(DrivenTable {
                    table,
                    length,
                    longhand_evaluations,
                    explicitly_inherited_groups,
                    ..
                })) = self.engine_driven_table(node, donor.record.record, &store, &selected, &inputs, counters)
            {
                let parent_in_display_none_subtree = parent_record
                    .and_then(|record| self.computed_group_sets.style_record_view(record.raw()))
                    .is_some_and(|view| view.dependency_flags & (1 << 2) != 0);
                let counter_style_registry = self.table_counter_style_environment_identity(target, &table);
                let assembly = self
                    .computed_group_sets
                    .replace_engine_computed_table(
                        node,
                        donor.record.record,
                        computed::FinalStyleRecordID::NONE,
                        table,
                        groups_to_rebuild,
                        &length,
                        None,
                        parent_in_display_none_subtree,
                        Some(environment),
                        Some(counter_style_registry),
                    )
                    .or_refused()?;
                self.computed_group_sets
                    .set_pending_cascade_state(target, cascade_state);
                self.settle_computed_memory();
                self.note_node_substitution(node, scratch, state, environment);
                self.note_engine_computed_record(
                    node,
                    assembly.delta,
                    cascade_state,
                    reads_sibling_position,
                    delta_property_count,
                    longhand_evaluations,
                    counters,
                );
                // The key names the parent's environment: a record whose own declarations
                // resolved another is no answer for an element declaring none. Nor is one that is
                // the element's alone.
                if let Some(cache_key) =
                    cache_key.filter(|key| key.environment == environment && !self.records_are_the_elements_alone(node))
                {
                    let record = ColdRecord {
                        record: assembly.delta.1,
                        swap_eligible: self.computed_group_sets.node_inherited_group_swap_eligible(node),
                        explicitly_inherited_groups,
                        reads_sibling_position,
                    };
                    scratch.cold_cohorts.insert(cache_key, record);
                    if let Some(no_resource_contexts) = no_resource_contexts {
                        self.remember_cold_record(cache_key, record, no_resource_contexts);
                    }
                }
                scratch.element_explicitly_inherited_groups = explicitly_inherited_groups;
                return Ok(ElementAnswer::Delta(assembly.delta));
            }
        }
        let subject = DriveSubject { target, parent, facts };
        let mut driven = self.engine_full_drive(
            subject,
            None,
            &store,
            &inputs,
            &mut scratch.font_drive,
            goal,
            has_registered_declarations,
            counters,
        )?;
        // The registered custom properties compute against the font the drive settled, and the
        // winners substitute what they computed to.
        if let FullDrive::AwaitsRegisteredContext(registered) = driven {
            let (settled, settled_store, _) = self.store_over_registered_context(
                subject.target,
                state,
                parent_environment,
                |engine, counters| {
                    engine
                        .engine_custom_property_environment(
                            node,
                            parent_environment,
                            &inputs,
                            Some(&registered),
                            counters,
                        )
                        .inspect_err(|&unanswered| {
                            if unanswered == Unanswered::Refused {
                                counters.bump(Counter::EngineComputedRecordBailCustomProperties);
                            }
                        })
                },
                scratch,
                counters,
            )?;
            environment = settled;
            store = std::sync::Arc::new(settled_store);
            self.note_node_substitution(node, scratch, state, environment);
            driven = self.engine_full_drive(
                subject,
                None,
                &store,
                &inputs,
                &mut scratch.font_drive,
                goal,
                false,
                counters,
            )?;
        }
        let DrivenTable {
            table,
            length,
            longhand_evaluations,
            font,
            explicitly_inherited_groups,
        } = match driven {
            FullDrive::Driven(driven) => driven,
            FullDrive::RootInputs(root_inputs) => return Ok(ElementAnswer::RootInputs(Some(root_inputs))),
            FullDrive::AwaitsRegisteredContext(_) => {
                unreachable!("a drive resumed with its registered context waits for nothing")
            }
        };
        let font = font.expect("a full drive resolves the font");
        let counter_style_registry = self.table_counter_style_environment_identity(target, &table);
        let (new_style_record, swap_eligible) = self.assemble_and_publish_engine_record(
            Some(target),
            parent_record,
            table,
            &length,
            &font,
            environment,
            pseudo_styles,
            counter_style_registry,
            Some(cascade_state),
            &mut scratch.computability,
            counters,
        )?;
        let delta = (computed::FinalStyleRecordID::NONE, new_style_record);
        // The publication itself kept the record for later transactions; alike elements in this
        // one take it from the cohort. The key names the parent's environment, so a record whose
        // own declarations resolved another is kept for no one, nor is one that is the element's
        // alone.
        if let Some(cache_key) =
            cache_key.filter(|key| key.environment == environment && !self.records_are_the_elements_alone(node))
        {
            let record = ColdRecord {
                record: delta.1,
                swap_eligible,
                explicitly_inherited_groups,
                reads_sibling_position,
            };
            scratch.cold_cohorts.insert(cache_key, record);
            if let Some(no_resource_contexts) = no_resource_contexts {
                self.remember_cold_record(cache_key, record, no_resource_contexts);
            }
        }
        scratch.element_explicitly_inherited_groups = explicitly_inherited_groups;
        self.note_engine_computed_record(
            node,
            delta,
            cascade_state,
            reads_sibling_position,
            delta_property_count,
            longhand_evaluations,
            counters,
        );
        Ok(ElementAnswer::Delta(delta))
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
        counters: &mut Counters,
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
                    && engine.record_answers_counter_styles_for(record.record, node)
            })
        };
        let (
            ColdRecord {
                record,
                swap_eligible,
                explicitly_inherited_groups,
                reads_sibling_position,
            },
            from_cache,
        ) = cache_key.and_then(|cache_key| {
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
        self.computed_group_sets
            .set_pending_cascade_state(target, cascade_state);
        let publication = self.assign_shared_style_record(
            target,
            record.raw(),
            computed::ENGINE_INHERITED_GROUP_COUNT,
            swap_eligible,
            counters,
        );
        let delta = (old_style_record, publication.style_record_identity);
        scratch.element_explicitly_inherited_groups = explicitly_inherited_groups;
        self.note_engine_computed_record(
            node,
            delta,
            cascade_state,
            reads_sibling_position,
            delta_property_count,
            0,
            counters,
        );
        counters.bump(if from_cache {
            Counter::EngineComputedRecordSharedHits
        } else {
            Counter::EngineComputedRecordCohortHits
        });
        Some(delta)
    }

    /// Whether a first record's winner keeps the record's computation in C++: a property that
    /// starts an animation. A transition declaration is computed into the record like any other
    /// value: a first record has no before-change style to start a transition from, and a record
    /// driven in full that replaces one leaves the transition step to the host.
    fn first_record_winner_needs_cpp(&self, property: u16) -> bool {
        property_starts_animation(property) && !property_declares_transitions(property)
    }

    /// The counter-style registry a record computed for `target` from `table` names, as C++ stamps
    /// it: its style scope's registry whenever its `content` holds a counter in a named counter
    /// style, or its `list-style-type` names a counter style that registry may define, which for a
    /// pseudo-element is any named one. Zero otherwise.
    fn table_counter_style_environment_identity(
        &self,
        target: computed::ComputedStyleTarget,
        table: &ComputedLonghandTable,
    ) -> u64 {
        use crate::css::property_metadata::property_id as prop;
        let value = |property| unsafe {
            table
                .effective_value(None, property, true)
                .value
                .cast::<StyleValueData>()
                .as_ref()
        };
        let names_a_counter_style = value(prop::CONTENT)
            .is_some_and(crate::css::style_compute::content_reads_counter_style_environment)
            || match value(prop::LIST_STYLE_TYPE) {
                Some(StyleValueData::CounterStyle {
                    is_symbols: false,
                    name,
                    ..
                }) => target.is_pseudo() || !counter_style_name_is_non_overridable(name.units()),
                _ => false,
            };
        if !names_a_counter_style {
            return 0;
        }
        self.counter_style_environment_identities
            .get(&self.counter_style_scope(target.node()))
            .copied()
            .unwrap_or(0)
    }

    /// Whether a record settled for another element answers for `node` as far as counter styles
    /// go: it names no counter-style registry, or the one `node`'s style scope has.
    fn record_answers_counter_styles_for(&self, record: computed::FinalStyleRecordID, node: StyleNodeID) -> bool {
        self.computed_group_sets
            .style_record_view(record.raw())
            .is_some_and(|view| {
                view.counter_style_environment_identity == 0
                    || self
                        .counter_style_environment_identities
                        .get(&self.counter_style_scope(node))
                        == Some(&view.counter_style_environment_identity)
            })
    }

    /// The style scope whose counter-style registry a node's records name. An element of a shadow
    /// tree built from the document's style sheets has the document's style scope.
    fn counter_style_scope(&self, node: StyleNodeID) -> TreeScopeID {
        let tree_scope = self.tree.tree_scope(node);
        if self.program.scope_uses_document_sheets(tree_scope) {
            TreeScopeID::DOCUMENT
        } else {
            tree_scope
        }
    }

    /// Whether the cascade state's winning `font-family` is monospace, which is what makes the
    /// font-size recascade against a 13px default. It is the target's own declaration that decides
    /// this, not what it inherits.
    fn font_family_winner_is_monospace(&self, state: CascadeStateID) -> bool {
        self.winner_groups
            .winner_in_state(state, crate::css::property_metadata::property_id::FONT_FAMILY)
            .and_then(|winner| self.winner_groups.resolved_winner(winner))
            .is_some_and(|winner| match self.specified_values.value(winner.key.value) {
                Lookup::Known(data) => crate::css::style_compute::font_family_is_monospace(data),
                _ => true,
            })
    }

    /// What a record for this target under this cascade state owes the monospace recascade, as a
    /// cohort key carries it: zero for a font-family that is not monospace. Two targets whose
    /// parents hold equal records can still sit under different cascaded font-size chains, and the
    /// recascade reads the chain rather than the records, so a cohort that ignored this would hand
    /// one target the other's font size.
    fn monospace_cohort_key(&self, target: computed::ComputedStyleTarget, state: CascadeStateID) -> i32 {
        if !self.font_family_winner_is_monospace(state) {
            return 0;
        }
        match self.monospace_recascaded_font_size(target, &self.document_style_computation_inputs) {
            Some(drive::MonospaceRecascade::Size(size, _)) => size,
            // The drive this key is for waits for the same font, and takes the key again after; or
            // it is left to C++.
            Some(drive::MonospaceRecascade::AwaitsFont(_)) | None => i32::MIN,
        }
    }

    /// Whether the host can be handed the animation plan of a record the engine derives for `node`
    /// from `state`, decided from that record once it is installed: the host applies it, and the
    /// winners say which scope the winning `animation-name` was declared in.
    fn may_plan_css_animations(
        &self,
        node: StyleNodeID,
        state: CascadeStateID,
        scratch: &EngineComputedRecordScratch,
    ) -> bool {
        scratch.host_applies_animation_plans && self.animation_name_declaration_scope(node, state).is_ok()
    }

    /// The tree scope the winning `animation-name` declaration of `state` was written in, where its
    /// `@keyframes` are looked for first, refused where C++'s exact cascade decided the winner. An
    /// author rule's is the scope its sheet is attached to, and for a sheet several scopes adopt,
    /// the one among them the winner's priority places among the element's encapsulation contexts.
    fn animation_name_declaration_scope(
        &self,
        node: StyleNodeID,
        state: CascadeStateID,
    ) -> Result<DeclarationScope, Unanswered> {
        let Some(winner) = self
            .winner_groups
            .winner_in_state(state, crate::css::property_metadata::property_id::ANIMATION_NAME)
        else {
            return Ok(DeclarationScope::Unscoped);
        };
        // A winner the cascade rolled back past every declaration (`revert`, `revert-layer`) takes
        // the initial or inherited value, which no scope declared.
        let Some(winner) = self.winner_groups.resolved_winner(winner) else {
            return Ok(DeclarationScope::Unscoped);
        };
        let rule = match winner.source {
            cascade::WinnerSource::Rule(rule) => rule,
            cascade::WinnerSource::Element(_) => return Ok(DeclarationScope::Unscoped),
            cascade::WinnerSource::ExactCascade => return Err(Unanswered::Refused),
        };
        let sheet = self.program.rule_sheet(rule);
        if self.program.sheet_origin(sheet) != crate::css::cascaded_properties::CascadeOrigin::Author {
            return Ok(DeclarationScope::Unscoped);
        }
        let scope = match self.program.sheet_scopes(sheet).as_slice() {
            &[scope] => scope,
            // The cascade weighs a match at the context of the scope it decided in, and each of the
            // element's contexts stands at its own depth.
            scopes => {
                let depth = winner.priority.author_context_depth();
                *scopes
                    .iter()
                    .find(|&&scope| self.author_context_index(node, scope) == depth)
                    .expect("an author rule wins at one of the element's contexts its sheet is attached to")
            }
        };
        Ok(match scope {
            TreeScopeID::DOCUMENT => DeclarationScope::Unscoped,
            scope => DeclarationScope::Shadow(scope),
        })
    }

    /// The scope the winning `animation-name` of the cascade state the record an element, or its
    /// pseudo-element `pseudo_kind`, is installed with was declared in, as
    /// [`Self::animation_name_declaration_scope`] answers it, or `None` where it refuses or the
    /// record is bound to no state.
    pub(crate) fn animation_name_declaration_scope_of(
        &self,
        node: StyleNodeID,
        pseudo_kind: u8,
    ) -> Option<DeclarationScope> {
        let (_, state) = self
            .computed_group_sets
            .installing_cascade_state(computed::ComputedStyleTarget::new(node, pseudo_kind))?;
        self.animation_name_declaration_scope(node, state).ok()
    }

    /// Whether the host composes the record the engine settled for `node`, moving it from `old` to
    /// `new`, before anything inherits from it: it applies the record's animation plan and samples
    /// the element's animations over it, even over a record that does not move. The element's
    /// pseudo-elements are settled over the composition afterwards, and its children wait for one
    /// that moves.
    pub(super) fn host_composes_row(&self, node: StyleNodeID, old: u64, new: u64) -> bool {
        self.computed_group_sets.adjustment_facts(node) & bridge::element_adjustment_fact::HAS_ANIMATIONS != 0
            || self.row_owes_an_animation_plan(node, old, new)
            || self.row_owes_a_transition_step(node)
    }

    /// Settle the pseudo-elements of an element the engine moves along `delta` beside its record,
    /// as `engine_pseudo_records` does, unless the host composes that record first: the host asks
    /// for them once the composition they inherit stands.
    #[allow(clippy::too_many_arguments)]
    fn engine_pseudo_records_beside(
        &mut self,
        node: StyleNodeID,
        delta: (computed::FinalStyleRecordID, computed::FinalStyleRecordID),
        old_is_list_item: Option<bool>,
        generation: u64,
        full_drive_reason: Option<FullDriveReason>,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) -> Drive<()> {
        if self.host_composes_row(node, delta.0.raw(), delta.1.raw()) {
            return Ok(());
        }
        let old_style_record = (delta.0 != computed::FinalStyleRecordID::NONE).then_some(delta.0);
        self.engine_pseudo_records(
            node,
            old_style_record,
            old_is_list_item,
            delta.1,
            generation,
            full_drive_reason,
            scratch,
            counters,
        )
    }

    /// Whether the host owes an element the engine settled the animation plan its record decides,
    /// once it installs the record: the record moved the declarations, or the element runs CSS
    /// animations, whose `@keyframes` may have moved without the declarations.
    pub(super) fn row_owes_an_animation_plan(&self, node: StyleNodeID, old: u64, new: u64) -> bool {
        // An element the host holds no record for, its style cleared under display:none and its
        // animations with it, starts the animations its record names as a first record does.
        let old = if self.held_style_records.contains_key(&node) {
            old
        } else {
            0
        };
        self.record_owes_an_animation_plan(old, new) || self.css_defined_animations.node_runs_a_css_animation(node)
    }

    /// Whether installing the `new` record the engine derived for an element, in place of the
    /// `old` one the host holds, owes the host the animation plan the new record decides: a first
    /// record that names an animation, a record that moves the declarations, or one that names an
    /// animation and leaves a display:none subtree, which starts the animations it names.
    pub(super) fn record_owes_an_animation_plan(&self, old: u64, new: u64) -> bool {
        match old {
            0 => self.record_declares_animations(new),
            old => {
                self.animation_declarations_moved(old, new)
                    || (self.record_is_in_display_none_subtree(old)
                        && !self.record_is_in_display_none_subtree(new)
                        && self.record_declares_animations(new))
            }
        }
    }

    /// Whether a record's `animation-name` names any animation.
    fn record_declares_animations(&self, record: u64) -> bool {
        self.computed_group_sets
            .style_record_view(record)
            .and_then(|view| unsafe { view.longhand_table.as_ref() })
            .is_some_and(crate::css::style_compute::table_declares_css_animations)
    }

    /// Whether a record's element is display:none or inherits from one that is.
    pub(super) fn record_is_in_display_none_subtree(&self, record: u64) -> bool {
        self.computed_group_sets
            .style_record_view(record)
            .is_some_and(|view| view.dependency_flags & IN_DISPLAY_NONE_SUBTREE != 0)
    }

    /// Whether moving an element from the `old` record to the `new` one moves the `animation-*`
    /// longhands declaring its CSS animations. Every such longhand is in the animation group; a
    /// group that moved with only transitions in it decides a plan that changes nothing.
    fn animation_declarations_moved(&self, old: u64, new: u64) -> bool {
        use crate::css::table_group_builder::group_index::ANIMATION;
        let animation_group = |record| {
            self.computed_group_sets
                .style_record_payloads(record)
                .and_then(|payloads| payloads.get(ANIMATION))
                .map(|payload| payload.as_ptr())
        };
        old != new && animation_group(old) != animation_group(new)
    }

    /// Whether the host composes animations over a record, which the engine cannot drive from: a
    /// record it cannot read counts as one.
    pub(super) fn record_holds_an_animation_overlay(&self, record: computed::FinalStyleRecordID) -> bool {
        self.computed_group_sets
            .style_record_view(record.raw())
            .is_none_or(|view| !view.animated_overlay.is_null())
    }

    /// Whether a record's transition declarations name a longhand a change of which starts a
    /// transition.
    pub(super) fn record_declares_transitions(&self, record: computed::FinalStyleRecordID) -> bool {
        self.computed_group_sets
            .style_record_view(record.raw())
            .and_then(|view| unsafe { view.longhand_table.as_ref() })
            .is_some_and(crate::css::style_compute::has_active_transition_properties)
    }

    /// Whether a row moving `node` off `underlying_style_record`, the record beneath any
    /// composition, owes the host the transition step: the host holds a style to start transitions
    /// from, and the record declares transitions or the row moves their declarations. The host runs
    /// the step once it installs the new record, against the record the row moved away from, and
    /// composes the record before anything inherits from it, so its children and pseudo-elements
    /// read the values the step started from. A row no host installs is refused.
    pub(super) fn decide_transition_step(
        &self,
        node: StyleNodeID,
        underlying_style_record: computed::FinalStyleRecordID,
        moves_transition_declarations: bool,
        host_applies_animation_plans: bool,
        counters: &mut Counters,
    ) -> Result<bool, Unanswered> {
        // An element whose style the host cleared on entering display:none, or never computed, has
        // no before-change style.
        let owes_a_transition_step = self.held_style_records.contains_key(&node)
            && (moves_transition_declarations || self.record_declares_transitions(underlying_style_record));
        if owes_a_transition_step && !host_applies_animation_plans {
            counters.bump(Counter::EngineComputedRecordBailProperty);
            return Err(Unanswered::Refused);
        }
        Ok(owes_a_transition_step)
    }

    /// Whether the host owes the element the transition step once it installs the record the engine
    /// settled for it, as deciding the row found.
    pub(super) fn row_owes_a_transition_step(&self, node: StyleNodeID) -> bool {
        self.engine_computed_records_pending
            .get(&node)
            .and_then(|records| records.iter().rev().find(|pending| pending.pseudo_kind == u8::MAX))
            .is_some_and(|pending| pending.owes_a_transition_step)
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
        use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LONGHAND_WORD_COUNT};

        if properties.is_empty()
            || properties.iter().any(|&property| {
                !property_computes_in_remaining_phase(property) || property_feeds_box_type_transformation(property)
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
            view.payloads[crate::css::computed_value_types::STYLE_GROUP_INDEX_INHERITED_BOX]
                .cast::<crate::css::computed_values::InheritedBoxValues>()
                .deref()
        };
        for &property in properties {
            if property_starts_animation(property) {
                return None;
            }
            groups_to_rebuild |= moved_longhand_groups(self.style_groups, property);
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
                groups_to_rebuild |= moved_longhand_groups(self.style_groups, counterpart);
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

    /// Build a driven table's groups against the parent record's payloads and publish the record
    /// for `target` the way a C++ computation publishes one; the record's swap eligibility comes
    /// back beside its identity. Without a target, no row holds the record.
    #[allow(clippy::too_many_arguments)]
    fn assemble_and_publish_engine_record(
        &mut self,
        target: Option<computed::ComputedStyleTarget>,
        parent_record: Option<computed::FinalStyleRecordID>,
        mut table: ComputedLonghandTable,
        length: &crate::css::style_compute::FfiLengthResolutionContext,
        font: &crate::css::table_group_builder::FfiFontGroupBuildInputs,
        environment: u64,
        pseudo_styles: u64,
        counter_style_registry: u64,
        cascade_state: Option<(u64, CascadeStateID)>,
        scratch: &mut EngineComputabilityScratch,
        counters: &mut Counters,
    ) -> Drive<(computed::FinalStyleRecordID, bool)> {
        use crate::css::table_group_builder::group_index;

        // The document element's groups build against no parent payloads.
        let (parent_payloads, parent_in_display_none_subtree) = match parent_record {
            Some(parent_record) => {
                let Some(parent_view) = self.computed_group_sets.style_record_view(parent_record.raw()) else {
                    debug_assert!(false, "an assigned parent record has a view");
                    counters.bump(Counter::EngineComputedRecordBailRecordParent);
                    return Err(Unanswered::Refused);
                };
                (parent_view.payloads, parent_view.dependency_flags & (1 << 2) != 0)
            }
            None => (&[SharedPayload::null(); group_index::COUNT][..], false),
        };
        let display_is_none = crate::css::style_compute::effective_display(&table, None).is_none();
        table.set_in_display_none_subtree(parent_in_display_none_subtree || display_is_none);
        table.freeze();
        let swap_eligible = table.property_inheritance_is_standard()
            && !table.display_is_list_item()
            && !crate::css::style_compute::has_active_transition_properties(&table);
        let color_inputs = crate::css::table_group_builder::assembly_color_inputs(&table, length);
        let table = table.into_raw_shared();
        let payloads = parent_payloads
            .iter()
            .enumerate()
            .take(group_index::COUNT)
            .map(|(group, &parent_payload)| {
                SharedPayload::new(unsafe {
                    crate::css::table_group_builder::assemble_group_from_table(
                        &*table,
                        group,
                        Some(font),
                        parent_payload.as_ptr(),
                        color_inputs,
                        length,
                    )
                })
            })
            .collect::<Vec<_>>();
        let holds_image_values = crate::css::computed_values::style_group_payloads_hold_image_values(
            HostShared::as_pointer_slice(&payloads),
        );
        let dependency_flags = unsafe { &*table }.publication_dependency_flags()
            | (u8::from(swap_eligible) * computed::INHERITED_GROUP_SWAP_ELIGIBLE)
            | (u8::from(holds_image_values) * computed::HOLDS_IMAGE_VALUES);
        let metadata_input = computed::ComputedMetadataInput {
            pseudo_element_styles: pseudo_styles,
            dependency_flags,
            counter_style_environment_identity: counter_style_registry,
            animation_overlay_identity: 0,
            animated_overlay: HostShared::null(),
            animation_overlay_payloads: &[],
            longhand_table: HostShared::new(table),
        };
        if let Some(target) = target
            && let Some(cascade_state) = cascade_state
        {
            self.computed_group_sets
                .set_pending_cascade_state(target, cascade_state);
        }
        // The drive built every payload and the table, and holds the only reference to each:
        // hand them to the catalog rather than have it retain a second one per published payload.
        let owned = computed::PendingRecordOwnership {
            groups: u32::try_from((1_u64 << payloads.len()) - 1).expect("a style group index fits the ownership mask"),
            table: true,
        };
        let publication = self.publish_computed_groups_impl(
            target,
            &payloads,
            computed::ENGINE_INHERITED_GROUP_COUNT,
            environment,
            metadata_input,
            owned,
            scratch,
            counters,
        );
        let transferred = publication.transferred;
        for (group, payload) in payloads.into_iter().enumerate() {
            if transferred.groups & (1 << group) == 0 {
                crate::css::computed_values::release_group_payload(group, payload.as_ptr());
            }
        }
        if !transferred.table {
            unsafe {
                crate::css::computed_longhand_table::rust_computed_longhand_table_release(table.cast_mut());
            }
        }
        Ok((publication.style_record_identity, swap_eligible))
    }

    /// C++ computes `node` itself rather than install what a record demand derived for it: the
    /// derived records go back as an abandoned derivation's do, and the demand's own cohorts went
    /// with it.
    pub(crate) fn abandon_demanded_records(&mut self, node: StyleNodeID, counters: &mut Counters) {
        self.abandon_engine_computed_record(node, &mut EngineComputedRecordScratch::default(), counters);
    }

    /// A derivation that could not be completed: everything derived for `node` this flush goes
    /// back to what the node held, and nothing in the flush may share it.
    pub(super) fn abandon_engine_computed_record(
        &mut self,
        node: StyleNodeID,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) {
        self.put_back_engine_computed_records(node, scratch, counters);
        counters.bump(Counter::EngineComputedRecordsAbandoned);
    }

    /// Put back every record derived for `node` that the host has not taken: the node goes back to
    /// the records it held, the cascade state a derived record waits to bind goes with it, and no
    /// cohort of the drive or cache across drives keeps a derived record to share.
    pub(super) fn put_back_engine_computed_records(
        &mut self,
        node: StyleNodeID,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) {
        for pending in self.engine_computed_records_pending.remove(&node).into_iter().flatten() {
            let derived = pending.new_style_record;
            if pending.pseudo_kind == u8::MAX {
                let target = computed::ComputedStyleTarget::new(node, u8::MAX);
                self.computed_group_sets.take_pending_cascade_state(target);
                self.revert_engine_computed_element_record(pending);
                scratch.cohorts.retain(|_, (record, ..)| *record != derived);
                scratch.cold_cohorts.retain(|_, record| record.record != derived);
                self.engine_cold_record_cache
                    .retain(|_, record| record.record != derived);
                self.engine_cold_record_donors.retain(|_, donors| {
                    donors.retain(|donor| donor.record.record != derived);
                    !donors.is_empty()
                });
            } else {
                self.revert_engine_computed_pseudo_record(pending, counters);
                scratch.pseudo_cohorts.retain(|_, record| *record != derived);
                self.engine_pseudo_record_cache.retain(|_, record| *record != derived);
            }
        }
        scratch.pseudo_deltas.clear();
        self.settle_computed_memory();
    }

    /// Whether a node's winners hold gated rules, decided where they were published or left for
    /// the record drive to decide once its containers settle.
    pub(super) fn node_holds_container_gates(&self, node: StyleNodeID) -> bool {
        self.published_container_verdicts.contains_key(&node) || self.container_gates_unheld.contains(&node)
    }

    /// How much of an element's held record a row drives again, given the winner `delta` since
    /// the record and whether anything else it was computed from moved (`inputs_moved`: the
    /// parent's inherited style or display, the root's inputs, or an input that drives in full).
    #[allow(clippy::too_many_arguments)]
    fn record_drive_scope(
        &self,
        node: StyleNodeID,
        state: CascadeStateID,
        delta: &cascade::CascadeWinnerDelta,
        flips_are_reflected: bool,
        inputs_moved: bool,
        environment: Option<u64>,
        has_registered_declarations: bool,
        pseudo_inputs_alone: bool,
    ) -> DriveScope {
        if delta.is_empty() {
            // Standing winners leave the record standing when the reaction is the rules that flipped
            // and nothing else moved, while its inherited groups provably follow the current parent
            // or all that moved is what the pseudo-elements read. Any other reaction stands for an
            // input no winner shows, a host's forced recompute (an attribute, a state, an input
            // type) among them: the record is driven in full from the inputs as they are now.
            return if flips_are_reflected
                && !inputs_moved
                && (pseudo_inputs_alone || self.record_inherits_from_current_parent(node, state))
            {
                environment.map_or(DriveScope::Stands, DriveScope::Environment)
            } else {
                DriveScope::Full
            };
        }
        // A moved font-phase longhand reaches every value the font feeds, so the record is driven
        // through every phase and every group is rebuilt. A moved box-type transformation input
        // takes the same route: the transformation is part of the full drive, and so is a moved
        // environment under registered custom properties, which compute against the font the drive
        // settles.
        if inputs_moved
            || (has_registered_declarations && environment.is_some())
            || delta.properties().iter().any(|&property| {
                !property_computes_in_remaining_phase(property) || property_feeds_box_type_transformation(property)
            })
        {
            DriveScope::Full
        } else {
            DriveScope::Partial
        }
    }

    /// Whether a node's record inherits from the parent it has now: each inherited group is the
    /// parent's own, no non-inherited property is inherited explicitly, and its custom-property
    /// environment is the parent's.
    fn record_inherits_from_current_parent(&self, node: StyleNodeID, state: CascadeStateID) -> bool {
        let Some(parent) = self.tree.inheritance_parent(node) else {
            return false;
        };
        if self.state_explicitly_inherits_non_inherited_property(node, state) {
            return false;
        }
        self.computed_group_sets
            .inherited_groups_follow_parent(node, parent)
            .unwrap_or(false)
            && self
                .computed_group_sets
                .custom_property_environment_identity(node)
                .is_some_and(|environment| self.inherited_custom_property_environment(parent) == Some(environment))
    }

    /// The inherited groups a state's winners rebuild for their element: the groups its
    /// declarations land in, the font group when it declares a longhand that group carries
    /// without landing in it, and the ones holding colors when it declares `color`, which their
    /// currentcolor-dependent values resolve against.
    fn state_owned_inherited_groups(&self, state: CascadeStateID) -> u32 {
        use crate::css::computed_value_types::{
            STYLE_GROUP_INDEX_FONT, STYLE_GROUP_INDEX_INHERITED_SVG, STYLE_GROUP_INDEX_INHERITED_TEXT,
            STYLE_GROUP_INDEX_INHERITED_UI,
        };
        use crate::css::property_metadata::{property_id as prop, property_style_group_index};
        self.winner_groups
            .properties_in_state(state)
            .fold(0_u32, |mask, property| {
                let mask = mask | property_style_group_index(property).map_or(0, |group| 1 << group);
                // A font longhand the group carries rather than holds, such as
                // `font-variant-numeric`, has no group of its own, so the group it does rebuild
                // has to be named here or nothing names it.
                let mask = if font_group_carries_longhand(property) {
                    mask | (1 << STYLE_GROUP_INDEX_FONT)
                } else {
                    mask
                };
                if property == prop::COLOR {
                    mask | (1 << STYLE_GROUP_INDEX_INHERITED_UI)
                        | (1 << STYLE_GROUP_INDEX_INHERITED_SVG)
                        | (1 << STYLE_GROUP_INDEX_INHERITED_TEXT)
                } else {
                    mask
                }
            })
    }

    pub(super) fn root_font_inputs_from_record(&self, record: computed::FinalStyleRecordID) -> Option<RootFontInputs> {
        use crate::css::computed_value_types::STYLE_GROUP_INDEX_FONT;
        let view = self.computed_group_sets.style_record_view(record.raw())?;
        let font = unsafe {
            view.payloads[STYLE_GROUP_INDEX_FONT]
                .cast::<crate::css::computed_value_types::FontValues>()
                .deref()
        };
        Some(RootFontInputs {
            metrics: [
                font.font_size.to_double().to_bits(),
                drive_font_metric(font.font_x_height).to_bits(),
                drive_font_metric(font.font_ascent).to_bits(),
                drive_font_metric(font.font_zero_advance).to_bits(),
                font.line_height_used.to_double().to_bits(),
            ],
            depends_on_viewport: view.dependency_flags & FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS != 0,
        })
    }

    /// The host boundary publishes root inputs after materializing a document element.
    /// Cache entries name these inputs; publication does not clear them as a side effect.
    fn prepare_root_font_metrics_from_record(&mut self, record: computed::FinalStyleRecordID) {
        let Some(root_inputs) = self.root_font_inputs_from_record(record) else {
            return;
        };
        root_inputs.apply_to(&mut self.document_style_computation_inputs);
    }

    fn element_drive_subject(&mut self, node: StyleNodeID, counters: &mut Counters) -> Drive<DriveSubject> {
        let facts = self.computed_group_sets.adjustment_facts(node);
        // The document element inherits from the initial values.
        let parent = self
            .record_inheritance_parent(node)
            .inspect_err(|_| counters.bump(Counter::EngineComputedRecordBailRecordParent))?;
        Ok(DriveSubject {
            target: computed::ComputedStyleTarget::new(node, u8::MAX),
            parent,
            facts,
        })
    }

    /// A later element alike in what a first record is computed from takes this record, the way a
    /// C++ computation shares across transactions. The cache is small and bounded.
    fn remember_cold_record(&mut self, key: ColdRecordKey, record: ColdRecord, _: ReadsNoResourceContexts) {
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
                root_font_inputs: key.root_font_inputs,
                sibling_position: key.sibling_position,
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

    /// What a record the engine computes from a winner state reads beside its winners, or `None`
    /// when the state is not one the engine computes records from: every
    /// winner a plain rule declaration with a written value that needs no document context.
    /// Substituted declarations also depend on the custom-property environment and the registry
    /// used to parse them.
    fn computable_state_record_reads(
        &mut self,
        node: StyleNodeID,
        cascade_state: (u64, CascadeStateID),
        scratch: &mut EngineComputabilityScratch,
        counters: &mut Counters,
    ) -> Option<StateRecordReads> {
        let environment = self
            .computed_group_sets
            .custom_property_environment_identity(node)
            .unwrap_or(0);
        let registration_generation = self
            .document_style_computation_inputs
            .custom_property_registration_generation;
        let key = (
            node,
            cascade_state.0,
            cascade_state.1,
            environment,
            registration_generation,
        );
        if let Some(&admitted) = scratch.states.get(&key) {
            return admitted;
        }
        let inherited = self
            .tree
            .inheritance_parent(node)
            .and_then(|parent| self.computed_group_sets.custom_property_environment_identity(parent))
            .unwrap_or(0);
        let mut substituted = false;
        let admitted = self
            .cascaded_store_for_state_in(
                node,
                cascade_state.1,
                None,
                custom_property_cascade::SubstitutionEnvironment {
                    own: environment,
                    inherited,
                },
                &mut substituted,
                StoreUse::Admission,
                counters,
            )
            .ok()
            .map(|store| store.record_reads(self));
        scratch.remember(key, admitted);
        admitted
    }

    /// Whether C++ may publish one record as the answer for another element with this winner
    /// state. Values which read per-element or external context are never shared opaquely, and
    /// no context-free value resolves a `url()`.
    fn opaque_record_shareable_state(
        &mut self,
        node: StyleNodeID,
        state: CascadeStateID,
        counters: &mut Counters,
    ) -> Option<ReadsNoResourceContexts> {
        for winner in self
            .winner_groups
            .winners_in_state(state)
            .filter_map(|winner| self.winner_groups.resolved_winner(winner))
        {
            match self.written_winner_value(node, &winner) {
                Ok(Some((_, _, checks))) if checks.whole_context_free => {}
                Err(counter) => {
                    counters.bump(counter);
                    return None;
                }
                _ => return None,
            }
        }
        Some(ReadsNoResourceContexts(()))
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
        let environment = self.inherited_custom_property_environment(parent)?;
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
            ancestor = self.tree.inheritance_parent(current);
        }
        None
    }

    /// Whether a winner state declares `inherit` for a non-inherited property, or carries a value
    /// the engine cannot see the spelling of.
    pub(super) fn node_explicitly_inherits_non_inherited_property(&self, node: StyleNodeID) -> bool {
        let Lookup::Known((_, state)) = self
            .current_winner_groups()
            .token_for(WinnerGroupKey::current(node, self.program.version()))
        else {
            return false;
        };
        self.state_explicitly_inherits_non_inherited_property(node, state)
    }

    fn state_explicitly_inherits_non_inherited_property(&self, node: StyleNodeID, state: CascadeStateID) -> bool {
        self.state_explicitly_inherited_group_masks(node, state)
            .next()
            .is_some()
    }

    /// The non-inherited style groups a winner state reads straight from the parent: what C++
    /// marks the parent with when it computes a record from the state.
    fn state_explicitly_inherited_groups(&self, node: StyleNodeID, state: CascadeStateID) -> u32 {
        self.state_explicitly_inherited_group_masks(node, state)
            .fold(0, |groups, mask| groups | mask)
    }

    /// The style groups of each non-inherited property a winner state declares `inherit` for, and
    /// every group for a value the engine cannot see the spelling of.
    fn state_explicitly_inherited_group_masks(
        &self,
        node: StyleNodeID,
        state: CascadeStateID,
    ) -> impl Iterator<Item = u32> + '_ {
        let is_inherit_keyword = |value: Option<&crate::css::style_value::RetainedStyleValueData>| {
            value.is_none_or(|value| {
                matches!(value.data(), crate::css::style_value::StyleValueData::Keyword { keyword }
                    if *keyword == crate::css::style_compute::keyword::INHERIT)
            })
        };
        self.winner_groups.winners_in_state(state).filter_map(move |winner| {
            if crate::css::property_metadata::property_is_inherited(winner.property) {
                return None;
            }
            let winner = self.winner_groups.resolved_winner(winner)?;
            let group_mask =
                || crate::css::computed_values::computed_group_output_mask(winner.property).unwrap_or(u32::MAX);
            match winner.source {
                WinnerSource::Rule(rule) => is_inherit_keyword(self.program.written_winner_value(
                    rule,
                    winner.property,
                    winner.important,
                    winner.key.value,
                ))
                .then(group_mask),
                WinnerSource::Element(kind) => {
                    is_inherit_keyword(self.facts.element_declarations(node, kind).and_then(|declarations| {
                        declarations.winner_value(winner.property, winner.important, winner.key.value)
                    }))
                    .then(group_mask)
                }
                WinnerSource::ExactCascade => Some(u32::MAX),
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
        scratch: &mut EngineComputabilityScratch,
        counters: &mut Counters,
    ) {
        if target.is_pseudo() || !is_base_record {
            return;
        }
        let inputs = self.document_style_computation_inputs;
        let node = target.node();
        let facts = self.computed_group_sets.adjustment_facts(node);
        if self.node_declares_custom_properties(node) {
            return;
        }
        // A record C++ computed for an element with animations is not what its winner state
        // alone describes.
        if facts & bridge::element_adjustment_fact::HAS_ANIMATIONS != 0 {
            return;
        }
        // A state minted before the winner groups were evicted names nothing in the table now.
        if cascade_state.0 != self.winner_groups.generation() {
            return;
        }
        let Some(parent) = self.tree.inheritance_parent(node) else {
            return;
        };
        let Some(parent_record) = self.computed_group_sets.assigned_style_record(parent) else {
            return;
        };
        if self.inherited_custom_property_environment(parent) != Some(custom_property_environment) {
            return;
        }
        // An opaque record is shared only when every winner is context-free, which no winner
        // reading the element's place among its siblings is.
        let Some(reads) = self
            .computable_state_record_reads(node, cascade_state, scratch, counters)
            .or_else(|| {
                self.opaque_record_shareable_state(node, cascade_state.1, counters)
                    .map(|no_resource_contexts| StateRecordReads {
                        sibling_position: false,
                        no_resource_contexts: Some(no_resource_contexts),
                    })
            })
        else {
            return;
        };
        // The element holds the record whether or not a later element takes it, and a change
        // among its siblings has to drive one reading its place again in full.
        self.note_sibling_position_reads(node, u8::MAX, reads.sibling_position);
        let Some(no_resource_contexts) = reads.no_resource_contexts else {
            return;
        };
        // A record whose winners read beyond its environment is the element's alone, as is one
        // `records_are_the_elements_alone` says is.
        if self.state_reads_beyond_environment(node, cascade_state.1) || self.records_are_the_elements_alone(node) {
            return;
        }
        let Some(parent) = self.cold_record_parent(node, parent, parent_record, cascade_state.1) else {
            return;
        };
        let swap_eligible = self.computed_group_sets.node_inherited_group_swap_eligible(node);
        let key = ColdRecordKey {
            monospace_recascaded_font_size: self
                .monospace_cohort_key(computed::ComputedStyleTarget::new(node, u8::MAX), cascade_state.1),
            sibling_position: self.sibling_position_key(node, cascade_state.1),
            parent,
            previous_style_record: previous_style_record.map_or(0, computed::FinalStyleRecordID::raw),
            generation: cascade_state.0,
            state: cascade_state.1,
            facts: cold_record_facts(facts),
            pseudo_styles,
            environment: custom_property_environment,
            font_environment_generation: inputs.font_environment_generation,
            root_font_inputs: RootFontInputs::from_document(&inputs),
        };
        self.remember_cold_record(
            key,
            ColdRecord {
                record: style_record,
                swap_eligible,
                explicitly_inherited_groups: self.state_explicitly_inherited_groups(node, cascade_state.1),
                reads_sibling_position: reads.sibling_position,
            },
            no_resource_contexts,
        );
    }

    /// The synthetic pseudo-elements the node has rules for, as a C++ record's pseudo-style mask,
    /// or no bits when the engine holds no answer for the node.
    pub(crate) fn published_pseudo_style_mask(&self, node: StyleNodeID) -> u64 {
        self.pseudo_style_mask(node).unwrap_or(0)
    }

    /// Whether the style record answers for a counter or a quote: the two things whose state runs
    /// along the whole tree rather than staying inside one box.
    fn style_record_affects_generated_content_state(&self, style_record: Option<computed::FinalStyleRecordID>) -> bool {
        self.published_style_record_view(style_record)
            .is_some_and(crate::css::computed_value_views::ComputedValuesView::affects_generated_content_state)
    }

    /// Whether the node or any of its DOM descendants styles a counter or a quote, itself or
    /// through a `::before`, `::after` or `::marker`. Inserting such a subtree renumbers what
    /// follows it, so the layout tree build has to rebuild rather than splice it in.
    #[must_use]
    pub fn subtree_affects_generated_content_state(&self, node: StyleNodeID) -> bool {
        let mut pending = vec![node];
        while let Some(node) = pending.pop() {
            if node.element_index().is_some()
                && (self
                    .style_record_affects_generated_content_state(self.computed_group_sets.assigned_style_record(node))
                    || [pseudo_kind::BEFORE, pseudo_kind::AFTER, pseudo_kind::MARKER]
                        .into_iter()
                        .any(|kind| {
                            self.style_record_affects_generated_content_state(
                                self.computed_group_sets.pseudo_style_record(node, kind),
                            )
                        }))
            {
                return true;
            }
            pending.extend(self.tree.dom_children(node));
        }
        false
    }

    /// Whether the node's published style holds a `::first-letter` record. The tree build asks a
    /// block this before it goes looking for the letter to style, and again of each block it
    /// descends into, which stops the search where a nested block styles its own first letter.
    #[must_use]
    pub(crate) fn has_published_first_letter_style(&self, node: StyleNodeID) -> bool {
        self.computed_group_sets
            .pseudo_style_record(node, pseudo_kind::FIRST_LETTER)
            .is_some()
    }

    /// The value a winner's declaration was written with, and the declaration's index in its
    /// block: the drive computes from the spelling the declaration was written in, which the
    /// cascade's canonical identity may have rewritten. A rule keeps its written values beside its
    /// declarations, and an element's own declarations keep theirs beside the facts.
    fn written_winner_value(
        &self,
        node: StyleNodeID,
        winner: &PropertyWinner,
    ) -> Result<
        Option<(
            usize,
            &crate::css::style_value::RetainedStyleValueData,
            WrittenValueChecks,
        )>,
        Counter,
    > {
        match winner.source {
            WinnerSource::Rule(rule) => Ok(self
                .program
                .written_winner_declaration(rule, winner.property, winner.important, winner.key.value)
                .map(|(index, value)| (index, value, self.program.written_value_checks(rule, index)))),
            WinnerSource::Element(kind) => Ok(self.facts.element_declarations(node, kind).and_then(|declarations| {
                let index = declarations.winner_index(winner.property, winner.important, winner.key.value)?;
                Some((index, declarations.written(index), declarations.checks(index)))
            })),
            WinnerSource::ExactCascade => Err(Counter::EngineComputedRecordBailWinnerOperator),
        }
    }

    /// The outermost shorthand declared in a winner's block for the written value a pending
    /// longhand names, and that shorthand's written value. A shorthand nested in another, such as
    /// `border-width` in `border`, is itself declared pending the outer one.
    fn shorthand_declaration_written_as(
        &self,
        node: StyleNodeID,
        source: WinnerSource,
        written_value: *const crate::css::style_value::StyleValueData,
    ) -> Option<(u16, crate::css::style_value::RetainedStyleValueData)> {
        let declarations = match source {
            WinnerSource::Rule(rule) => {
                let declared = self.program.declared_properties_of(rule);
                let written = self.program.written_values_of(rule);
                if written.len() != declared.len() {
                    return None;
                }
                declared.iter().zip(written)
            }
            WinnerSource::Element(kind) => self.facts.element_declarations(node, kind)?.iter(),
            WinnerSource::ExactCascade => return None,
        };
        let mut written_value = written_value;
        while let Some((shorthand, value)) = declarations.clone().find(|(declared, written)| {
            declared.property < crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
                && std::ptr::eq(written.pointer(), written_value)
        }) {
            match value.data() {
                crate::css::style_value::StyleValueData::PendingSubstitution {
                    original_shorthand_value,
                } => written_value = original_shorthand_value.pointer(),
                _ => return Some((shorthand.property, value.clone_retained())),
            }
        }

        // Inline declarations retain the expanded pending longhands without a separate shorthand
        // declaration. The longhands pending the same original value are the leaves of the
        // shorthand whose grammar parses the substituted source, through any shorthand nested in
        // it (`border-width` in `border`).
        let mut original = None;
        let mut pending_longhands = Vec::new();
        for (declared, written) in declarations {
            if let crate::css::style_value::StyleValueData::PendingSubstitution {
                original_shorthand_value,
            } = written.data()
                && std::ptr::eq(original_shorthand_value.pointer(), written_value)
            {
                pending_longhands.push(declared.property);
                original = Some(original_shorthand_value);
            }
        }
        let original = original?;
        pending_longhands.sort_unstable();
        pending_longhands.dedup();
        fn leaf_longhands(property: u16, leaves: &mut Vec<u16>) {
            for &longhand in crate::css::property_metadata::longhands_for_shorthand(property) {
                if longhand < crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID {
                    leaf_longhands(longhand, leaves);
                } else {
                    leaves.push(longhand);
                }
            }
        }
        let mut leaves = Vec::with_capacity(pending_longhands.len());
        let shorthand = (crate::css::property_metadata::FIRST_SHORTHAND_PROPERTY_ID
            ..=crate::css::property_metadata::LAST_SHORTHAND_PROPERTY_ID)
            .find(|&candidate| {
                leaves.clear();
                leaf_longhands(candidate, &mut leaves);
                leaves.sort_unstable();
                leaves.dedup();
                leaves == pending_longhands
            })?;
        Some((shorthand, original.clone_retained()))
    }

    /// Whether any winner of a state was written with a substitution, so the record computed
    /// from it reads the node's custom-property environment.
    pub(super) fn state_has_substitutions(&self, node: StyleNodeID, state: CascadeStateID) -> bool {
        self.state_substitution_values(node, state).next().is_some()
    }

    /// What a state's winners read beyond the cascade, as `cascade::STATE_READS_*` bits, decided
    /// once for each state.
    pub(super) fn state_reads(&self, node: StyleNodeID, state: CascadeStateID) -> u8 {
        if let Some(reads) = self.winner_groups.state_reads(state) {
            return reads;
        }
        let mut reads = 0;
        for value in self.state_substitution_values(node, state) {
            if custom_property_cascade::value_reads_attributes(value.data()) {
                reads |= cascade::STATE_READS_ATTRIBUTES;
            }
            if custom_property_cascade::value_reads_inherited_values(value.data()) {
                reads |= cascade::STATE_READS_INHERIT_FUNCTION;
            }
            if custom_property_cascade::value_reads_conditions(value.data()) {
                reads |= cascade::STATE_READS_IF_FUNCTION;
            }
            if custom_property_cascade::value_calls_custom_functions(value.data()) {
                reads |= cascade::STATE_READS_CUSTOM_FUNCTION;
            }
        }
        for (_, value, _) in self
            .winner_groups
            .winners_in_state(state)
            .filter_map(|winner| self.winner_groups.resolved_winner(winner))
            .filter_map(|winner| self.written_winner_value(node, &winner).ok().flatten())
        {
            let dependencies = crate::css::style_compute::collect_external_value_dependencies(value.data());
            if dependencies.uses_tree_counting_function {
                reads |= cascade::STATE_READS_SIBLING_POSITION;
            }
            if dependencies.has_unfixed_random_sharing && value_draws_element_random_base(value.data()) {
                reads |= cascade::STATE_READS_ELEMENT_RANDOM_BASE;
            }
            if dependencies.container_relative_length_unit_mask != 0 {
                reads |= cascade::STATE_READS_CONTAINER_UNITS;
            }
        }
        self.winner_groups.note_state_reads(state, reads);
        reads
    }

    /// The container-relative units a state's winners are written with, as a
    /// `container_relative_length_unit_mask`.
    pub(super) fn state_container_unit_mask(&self, node: StyleNodeID, state: CascadeStateID) -> u8 {
        self.winner_groups
            .winners_in_state(state)
            .filter_map(|winner| self.winner_groups.resolved_winner(winner))
            .filter_map(|winner| self.written_winner_value(node, &winner).ok().flatten())
            .fold(0, |mask, (_, value, _)| {
                mask | crate::css::style_compute::collect_external_value_dependencies(value.data())
                    .container_relative_length_unit_mask
            })
    }

    /// Whether any of a state's longhand winners, itself or through the shorthand it is pending,
    /// substitutes what the node's own custom-property environment does not decide: the element's
    /// attributes through `attr()`, the parent's environment through `inherit()`, the document's
    /// media features and the element's lengths through `if()`, or the `@function` definitions
    /// through a custom function call. What such a state substitutes to is the element's alone,
    /// as is what a state computes to whose winners draw a random base value for the element or
    /// resolve a container-relative length against the element's query containers, or substitute
    /// under the custom properties the element's animations sample.
    pub(super) fn state_reads_beyond_environment(&self, node: StyleNodeID, state: CascadeStateID) -> bool {
        self.state_reads(node, state)
            & (cascade::STATE_READS_ATTRIBUTES
                | cascade::STATE_READS_INHERIT_FUNCTION
                | cascade::STATE_READS_IF_FUNCTION
                | cascade::STATE_READS_CUSTOM_FUNCTION
                | cascade::STATE_READS_ELEMENT_RANDOM_BASE
                | cascade::STATE_READS_CONTAINER_UNITS)
            != 0
            || (self.element_samples_custom_properties(node) && self.state_has_substitutions(node, state))
    }

    /// Whether a record holds a value resolved against the viewport, its own or its font's, which
    /// the record's publication flags carry, or a registered custom property's, which its
    /// environment does.
    pub(super) fn record_reads_the_viewport(&self, record: computed::FinalStyleRecordID) -> bool {
        self.computed_group_sets
            .style_record_view(record.raw())
            .is_some_and(|view| {
                view.dependency_flags & (DEPENDS_ON_VIEWPORT_METRICS | FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS) != 0
            })
            || self
                .computed_group_sets
                .style_record_custom_property_environment(record.raw())
                .is_some_and(|environment| self.custom_property_environments.reads_viewport(environment))
    }

    /// Whether a record of the node rolled a property back below a revert keyword a substitution
    /// produced, to a declaration its winners do not name: its records are the node's alone, and
    /// are computed again whatever its winners say.
    pub(super) fn record_rolls_back_substitution(&self, node: StyleNodeID) -> bool {
        self.nodes_with_rolled_back_records.contains_key(&node)
    }

    /// Whether the node's records are its alone whatever its winners and environment say, so no
    /// record cache keeps them: one rolled a property back below a substituted revert keyword, or
    /// substituted a value that resolves against the element.
    pub(super) fn records_are_the_elements_alone(&self, node: StyleNodeID) -> bool {
        self.record_rolls_back_substitution(node) || self.nodes_with_element_relative_substitutions.contains_key(&node)
    }

    /// The element whose attributes `attr()` reads for a node's record or one of its
    /// pseudo-elements': an element standing for its shadow host's pseudo-element is computed as
    /// that pseudo-element, so it reads the host's, as C++ does for all but its ::first-letter.
    pub(super) fn substitution_attribute_element(&self, node: StyleNodeID, pseudo_kind: Option<u8>) -> StyleNodeID {
        if pseudo_kind == Some(pseudo_kind::FIRST_LETTER) {
            return node;
        }
        self.backed_host_pseudo_element(node).map_or(node, |(_, host)| host)
    }

    /// Where `node` stands among its element siblings, as the tree-counting functions count: an
    /// element whose parent is a shadow root has no parent element, and is an only child. `None`
    /// for a node the retained tree does not hold.
    pub(super) fn sibling_position(&self, node: StyleNodeID) -> Option<SiblingPosition> {
        if !self.tree.is_live(node) {
            return None;
        }
        let Some(parent) = self
            .tree
            .parent(node)
            .filter(|&parent| self.tree.host_of(parent).is_none())
        else {
            return Some(SiblingPosition { count: 1, index: 1 });
        };
        let mut position = SiblingPosition::default();
        for child in self
            .tree
            .dom_children(parent)
            .filter(|child| child.text_index().is_none())
        {
            position.count += 1;
            if child == node {
                position.index = position.count;
            }
        }
        Some(position)
    }

    /// Whether any of a state's winners is written with a tree-counting function.
    pub(super) fn state_has_written_tree_counting(&self, node: StyleNodeID, state: CascadeStateID) -> bool {
        self.state_reads(node, state) & cascade::STATE_READS_SIBLING_POSITION != 0
    }

    /// Whether the record `node` holds reads its place among its siblings, written or substituted,
    /// as its installation noted, or as a winner of `state` written with a tree-counting function
    /// says of a record whose installation noted nothing.
    pub(super) fn record_reads_sibling_position(&self, node: StyleNodeID, state: CascadeStateID) -> bool {
        self.nodes_with_tree_counting_records
            .get(&node)
            .is_some_and(|&bits| bits & ELEMENT_READS_SIBLING_POSITION != 0)
            || self.state_has_written_tree_counting(node, state)
    }

    /// Note whether the record the engine derived for `node`, or for its pseudo-element of
    /// `pseudo_kind` (`u8::MAX` for the element's own), reads the node's place among its siblings,
    /// in place of what the record it replaces read.
    fn note_sibling_position_reads(&mut self, node: StyleNodeID, pseudo_kind: u8, reads: bool) {
        let bit = match pseudo_kind {
            u8::MAX => ELEMENT_READS_SIBLING_POSITION,
            kind if usize::from(kind) < pseudo_kind::SYNTHETIC_COUNT => 1 << kind,
            // A kind past the synthetic ones is only a read's, which no style update installs.
            _ => return,
        };
        if reads {
            *self.nodes_with_tree_counting_records.entry(node).or_default() |= bit;
        } else if let Some(bits) = self.nodes_with_tree_counting_records.get_mut(&node) {
            *bits &= !bit;
            if *bits == 0 {
                self.nodes_with_tree_counting_records.remove(&node);
            }
        }
    }

    /// Where `node` stands among its siblings, for the record caches, when a winner of `state` is
    /// written with a tree-counting function, or a substitution may produce one.
    pub(super) fn sibling_position_key(&self, node: StyleNodeID, state: CascadeStateID) -> Option<SiblingPosition> {
        (self.state_has_written_tree_counting(node, state) || self.state_has_substitutions(node, state))
            .then(|| self.sibling_position(node))
            .flatten()
    }

    /// The written values of a state's longhand winners that substitute: `var()`, `attr()` and
    /// the like, or a longhand pending its shorthand's substitution.
    fn state_substitution_values(
        &self,
        node: StyleNodeID,
        state: CascadeStateID,
    ) -> impl Iterator<Item = &crate::css::style_value::RetainedStyleValueData> {
        self.winner_groups.winners_in_state(state).filter_map(move |winner| {
            let winner = self.winner_groups.resolved_winner(winner)?;
            if winner.property < crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID {
                return None;
            }
            let value = match winner.source {
                WinnerSource::Rule(rule) => {
                    self.program
                        .written_winner_value(rule, winner.property, winner.important, winner.key.value)
                }
                WinnerSource::Element(kind) => self.facts.element_declarations(node, kind).and_then(|declarations| {
                    declarations.winner_value(winner.property, winner.important, winner.key.value)
                }),
                WinnerSource::ExactCascade => None,
            }?;
            matches!(
                value.data(),
                crate::css::style_value::StyleValueData::Unresolved { .. }
                    | crate::css::style_value::StyleValueData::PendingSubstitution { .. }
            )
            .then_some(value)
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
        environment: custom_property_cascade::SubstitutionEnvironment,
        substituted: &mut bool,
        counters: &mut Counters,
    ) -> Drive<WinnerStore> {
        self.cascaded_store_for_state_in(
            node,
            state,
            pseudo_kind,
            environment,
            substituted,
            StoreUse::Drive,
            counters,
        )
    }

    /// `cascaded_store_for_state`, for a drive or for the admission that proves a drive can
    /// read every winner's written declaration: admission refuses a winner the drive asserts it
    /// finds.
    #[allow(clippy::too_many_arguments)]
    fn cascaded_store_for_state_in(
        &mut self,
        node: StyleNodeID,
        state: CascadeStateID,
        pseudo_kind: Option<u8>,
        environment: custom_property_cascade::SubstitutionEnvironment,
        substituted: &mut bool,
        store_use: StoreUse,
        counters: &mut Counters,
    ) -> Drive<WinnerStore> {
        // A winner whose written declaration is not where its cascade found it.
        let unwritten = |counters: &mut Counters, invariant: &str| -> Drive<()> {
            if store_use == StoreUse::Admission {
                counters.bump(Counter::EngineComputedRecordBailWinner);
                return Err(Unanswered::Refused);
            }
            debug_assert!(false, "{invariant}");
            Ok(())
        };
        crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::WinnerStoreBuilds);
        // An element's own values read what its animations sampled into its custom properties.
        let environment = match pseudo_kind {
            None => custom_property_cascade::SubstitutionEnvironment {
                own: self.sampled_custom_property_environment(node, environment.own),
                ..environment
            },
            Some(_) => environment,
        };
        // Seeded in cascade order, and within one rule in declaration order, since a logical
        // property and its physical associate resolve by order of appearance.
        let mut declarations = Vec::with_capacity(self.winner_groups.winner_count_in_state(state));
        // What an `attr()` reads, gathered once for the first winner that reads it. A
        // pseudo-element's reads its originating element's.
        let mut attributes = None;
        let attribute_element = self.substitution_attribute_element(node, pseudo_kind);
        // What a `style()` query in an `if()` resolves against, and the custom functions a call
        // reaches, likewise.
        let mut style_query = None;
        let style_query_references = std::cell::RefCell::new(None);
        let mut functions: Option<custom_property_cascade::PreparedCustomFunctions> = None;
        // Whether a property rolled back below a substituted revert keyword: what it rolled back to
        // is decided by declarations the winners do not name.
        let mut rolled_back = false;
        // Whether a substitution produced a value that resolves against the element, and the
        // container-relative units it did.
        let mut element_relative = false;
        let mut substituted_container_units = 0;
        'winners: for winner in self.winner_groups.winners_in_state(state) {
            // A revert whose continuation resumes at nothing leaves the property undeclared.
            let Some(mut winner) = self.winner_groups.resolved_winner(winner) else {
                continue;
            };
            if winner.key.animation_relevance != 0 {
                counters.bump(Counter::EngineComputedRecordBailWinnerAnimated);
                return Err(Unanswered::Refused);
            }
            // A pseudo-element's cascade keeps the properties its kind supports. Its animations
            // start, and its transition step runs, where the host installs its record. Its anchor
            // name is a plain computed value: the host registers an element's alone.
            if let Some(kind) = pseudo_kind
                && !crate::css::property_metadata::pseudo_element_supports_property(kind, winner.property)
            {
                continue;
            }
            // A shorthand written with a substitution is declared beside the longhands it
            // pends; those carry it.
            if winner.property < crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID {
                continue;
            }
            // A substitution that produces `revert` or `revert-layer` rolls the property back below
            // the declaration that held it, to a lower declaration that is substituted in turn,
            // as the C++ cascade does. The declarations it rolled back, with the keyword each
            // substituted to.
            let mut reverted = Vec::new();
            let (index, checks, value, borrowed) = loop {
                // A winner's declaration is written in its source. One the engine cannot find again
                // is invalid at computed-value time, and the property is left undeclared: it
                // computes as `unset`.
                let (index, value, checks) = match self.written_winner_value(node, &winner) {
                    Ok(Some(written)) => written,
                    Ok(None) => {
                        unwritten(counters, "a winner's declaration is written in its source")?;
                        continue 'winners;
                    }
                    Err(counter) => {
                        counters.bump(counter);
                        return Err(Unanswered::Refused);
                    }
                };
                // A longhand declared through a shorthand keeps the whole shorthand as its
                // written value; the store takes the longhand's own part of it.
                let location = WinnerValue::Written {
                    node,
                    source: winner.source,
                    index,
                };
                let calls_functions = custom_property_cascade::value_calls_custom_functions(value.data());
                if calls_functions && functions.is_none() {
                    // A container condition the engine cannot decide asks about an ancestor the
                    // host styles in this update: the row waits for it.
                    let Some(prepared) = self.prepare_custom_functions(node, pseudo_kind) else {
                        counters.bump(Counter::EngineComputedRecordBailRecordParent);
                        return Err(Unanswered::AwaitsParent);
                    };
                    functions = Some(prepared);
                }
                // A function's declarations may hold an `if()` as well.
                if style_query.is_none()
                    && (calls_functions || custom_property_cascade::value_reads_conditions(value.data()))
                {
                    style_query =
                        Some(self.style_query_inputs(computed::ComputedStyleTarget::new(
                            node,
                            pseudo_kind.unwrap_or(u8::MAX),
                        )));
                }
                // An `attr()` reads the element's attributes, written in a longhand or in the
                // shorthand it pends, or in a function's declarations. A state that reads them
                // keeps its store the element's alone.
                let reads_attributes = custom_property_cascade::value_reads_attributes(value.data())
                    || (calls_functions && functions.as_ref().is_some_and(|functions| functions.reads_attributes));
                if reads_attributes && attributes.is_none() {
                    attributes = Some(custom_property_cascade::SubstitutionAttributes::of(
                        &self.facts,
                        attribute_element,
                        self.html_element_namespace,
                    ));
                }
                let (value, borrowed) = match value.data() {
                    crate::css::style_value::StyleValueData::Shorthand { .. } => {
                        let Some(value) = shorthand_longhand_data(winner.property, value.data()) else {
                            unwritten(
                                counters,
                                "a shorthand written for a longhand winner carries that longhand",
                            )?;
                            continue 'winners;
                        };
                        (location, Some(value))
                    }
                    // A value with var() references substitutes under the node's environment, as
                    // the C++ cascade substitutes it; a value invalid at computed-value time is
                    // unset.
                    crate::css::style_value::StyleValueData::Unresolved { .. } => {
                        *substituted = true;
                        let value = value.clone_retained();
                        let inputs = custom_property_cascade::SubstitutionInputs {
                            document: &self.document_style_computation_inputs,
                            media: &self.document_media,
                            environment,
                            attributes: attributes.as_ref().filter(|_| reads_attributes),
                            style_query: style_query.as_ref().and_then(Option::as_ref),
                            style_query_references: Some(&style_query_references),
                            functions: functions.as_ref(),
                        };
                        let value = Self::substitute_written_value(
                            &mut self.custom_property_environments,
                            &inputs,
                            winner.property,
                            value,
                            counters,
                        )?;
                        (
                            WinnerValue::Substituted {
                                value: invalid_as_unset(value),
                                source: winner.source,
                            },
                            None,
                        )
                    }
                    // A longhand pending its shorthand's substitution takes its part of the
                    // substituted shorthand.
                    crate::css::style_value::StyleValueData::PendingSubstitution {
                        original_shorthand_value,
                    } => {
                        *substituted = true;
                        let Some((shorthand, written)) = self.shorthand_declaration_written_as(
                            node,
                            winner.source,
                            original_shorthand_value.pointer(),
                        ) else {
                            unwritten(counters, "a pending longhand's shorthand is written in its source")?;
                            continue 'winners;
                        };
                        let inputs = custom_property_cascade::SubstitutionInputs {
                            document: &self.document_style_computation_inputs,
                            media: &self.document_media,
                            environment,
                            attributes: attributes.as_ref().filter(|_| reads_attributes),
                            style_query: style_query.as_ref().and_then(Option::as_ref),
                            style_query_references: Some(&style_query_references),
                            functions: functions.as_ref(),
                        };
                        let resolved = Self::substitute_written_value(
                            &mut self.custom_property_environments,
                            &inputs,
                            shorthand,
                            written,
                            counters,
                        )?;
                        let value = match resolved.data() {
                            crate::css::style_value::StyleValueData::GuaranteedInvalid => unset_value(),
                            _ => expanded_longhand_value(shorthand, winner.property, &resolved)
                                .unwrap_or_else(unset_value),
                        };
                        (
                            WinnerValue::Substituted {
                                value,
                                source: winner.source,
                            },
                            None,
                        )
                    }
                    _ => (location, Some(value.data())),
                };
                if let WinnerValue::Substituted {
                    value: substituted_value,
                    ..
                } = &value
                    && let operator @ (CascadeOperator::Revert | CascadeOperator::RevertLayer) =
                        super::program_updates::declaration_operator(substituted_value.data())
                {
                    reverted.push((winner.priority, operator));
                    // An element standing for its host's pseudo-element is cascaded from the host's
                    // rules, which its own answer does not hold.
                    let below = match self.backed_host_pseudo_element(node) {
                        Some(_) => Err(()),
                        None => self.winner_below_substitution(node, pseudo_kind, winner.property, &reverted),
                    };
                    rolled_back = true;
                    match below {
                        Ok(Some(lower)) => {
                            winner = lower;
                            continue;
                        }
                        // Nothing below declares the property: it is undeclared.
                        Ok(None) => continue 'winners,
                        Err(()) => {
                            counters.bump(Counter::EngineComputedRecordBailSubstitution);
                            return Err(Unanswered::Refused);
                        }
                    }
                }
                break (index, checks, value, borrowed);
            };
            let data = match &value {
                WinnerValue::Substituted { value, .. } => value.data(),
                WinnerValue::Written { .. } => borrowed.expect("written declaration is borrowed"),
            };
            // A `url()` resolves against the sheet its rule came from, written or substituted, or
            // the document's base URL for an element's own declaration.
            let resources_are_known = match &value {
                WinnerValue::Written {
                    source: WinnerSource::Rule(rule),
                    ..
                }
                | WinnerValue::Substituted {
                    source: WinnerSource::Rule(rule),
                    ..
                } => self
                    .rule_source_identity(*rule)
                    .is_some_and(|source| self.document_resource_contexts.for_source(source).is_some()),
                WinnerValue::Written {
                    source: WinnerSource::Element(_),
                    ..
                }
                | WinnerValue::Substituted {
                    source: WinnerSource::Element(_),
                    ..
                } => true,
                _ => false,
            };
            // A value of a shape the computation does not know never reaches a winner, and the
            // drive holds everything else a value may read beyond the record, the parent and the
            // document's computation inputs, in any mix. A random function or a container-relative
            // length that appears through a substitution resolves against the element, which the
            // record caches key a state's written winners on but not what the environment
            // substitutes: the record is the element's alone.
            let computable = checks.longhand_context_free == Some(true)
                || crate::css::style_compute::value_is_computationally_independent(data).is_some_and(|_| {
                    let dependencies = crate::css::style_compute::external_value_dependencies(data);
                    if matches!(value, WinnerValue::Substituted { .. }) {
                        element_relative |= value_resolves_against_the_element(&dependencies);
                        substituted_container_units |= dependencies.container_relative_length_unit_mask;
                    }
                    drive_holds_value_inputs(&dependencies, resources_are_known, || {
                        self.sibling_position(node).is_some()
                    })
                });
            debug_assert!(
                computable || crate::css::style_compute::value_is_computationally_independent(data).is_some(),
                "a cascade winner computes to a value of a known shape"
            );
            if !computable {
                counters.bump(Counter::EngineComputedRecordBailValue);
                return Err(Unanswered::Refused);
            }
            if matches!(value, WinnerValue::Substituted { .. }) {
                crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::WinnerStoreValueRetains);
            }
            declarations.push((
                winner.priority,
                index,
                WinnerDeclaration::new(winner.property, winner.important, value),
            ));
        }
        // What a function's container conditions read is recorded with the node's record, as a
        // gated rule's is.
        if let Some(effects) = functions.and_then(|functions| functions.container_effects) {
            self.note_container_effects_for_host(node, effects);
        }
        declarations.sort_by_key(|(priority, index, ..)| (*priority, *index));
        let store = WinnerStore::new(
            declarations
                .into_iter()
                .map(|(_, _, declaration)| declaration)
                .collect(),
        );
        // A store built to drive a record says whether that record rolls back; one built to admit a
        // row only adds to what the node's records are known to do.
        let bit = rolled_back_bit(pseudo_kind);
        if rolled_back {
            *self.nodes_with_rolled_back_records.entry(node).or_default() |= bit;
        } else if store_use == StoreUse::Drive
            && let Some(bits) = self.nodes_with_rolled_back_records.get_mut(&node)
        {
            *bits &= !bit;
            if *bits == 0 {
                self.nodes_with_rolled_back_records.remove(&node);
            }
        }
        // So does one whose substitutions resolve against the element.
        if element_relative {
            let substitutions = self.nodes_with_element_relative_substitutions.entry(node).or_default();
            substitutions.records |= bit;
            substitutions.container_units |= substituted_container_units;
        } else if store_use == StoreUse::Drive
            && let Some(substitutions) = self.nodes_with_element_relative_substitutions.get_mut(&node)
        {
            substitutions.records &= !bit;
            if substitutions.records == 0 {
                self.nodes_with_element_relative_substitutions.remove(&node);
            }
        }
        // The custom properties its `style()` queries read are the element's style query
        // references, which the host records with its record.
        if store_use == StoreUse::Drive
            && let Some(references) = style_query_references.into_inner()
        {
            self.note_container_effects_for_host(
                node,
                super::container_queries::ContainerVerdict {
                    style_query_references: Some(references),
                    ..Default::default()
                },
            );
        }
        if store_use == StoreUse::Drive {
            store.draw_random_base_values(self, node);
        }
        Ok(store)
    }

    /// Whether every cascade winner that moved on this node since its record was computed is a
    /// longhand the engine computes itself. Such a change cannot reach the node's descendants:
    /// nothing inherited moves, and no custom property does, so a descendant's engine-computed
    /// record stays exact even though this ancestor changes in the same batch.
    pub(super) fn winner_delta_is_engine_confined(&self, node: StyleNodeID) -> bool {
        let target = computed::ComputedStyleTarget::new(node, u8::MAX);
        let Lookup::Known((generation, state)) = self
            .current_winner_groups()
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
        let mut moves_non_inherited = false;
        let confined = self
            .winner_groups
            .semantic_delta_properties(Some(previous_state), state)
            .all(|property| {
                moves_non_inherited = true;
                property != crate::css::property_metadata::property_id::CUSTOM
                    && property != crate::css::property_metadata::property_id::DISPLAY
                    && !crate::css::property_metadata::property_is_inherited(property)
            });
        // A child that explicitly inherits a non-inherited property inherits it like any other,
        // and passes it on to a descendant whose record is assembled from the child's.
        confined && !(moves_non_inherited && self.children_explicitly_inherit_non_inherited_properties(node))
    }

    /// Whether any of the node's children, as its descendants inherit through them, explicitly
    /// inherits a non-inherited property.
    fn children_explicitly_inherit_non_inherited_properties(&self, node: StyleNodeID) -> bool {
        // A text slottable holds a place among a slot's assigned nodes, but inherits nothing on.
        self.tree
            .flat_tree_children(node)
            .filter(|child| child.element_index().is_some())
            .any(|child| self.node_explicitly_inherits_non_inherited_property(child))
    }

    /// Publish the immutable computed-group payloads of one element's base style. This assigns
    /// dense identities to shared payloads and their ordered tuple, so an equal handle proves equal
    /// groups and downstream operators can consume one node handle.
    pub(crate) fn publish_computed_groups(
        &mut self,
        target: computed::ComputedStyleTarget,
        payloads: &[SharedPayload],
        inherited_group_count: usize,
        custom_property_environment: u64,
        metadata_input: computed::ComputedMetadataInput<'_>,
        counters: &mut Counters,
    ) -> computed::ComputedGroupPublication {
        let mut scratch = EngineComputabilityScratch::default();
        let publication = self.publish_computed_groups_impl(
            Some(target),
            payloads,
            inherited_group_count,
            custom_property_environment,
            metadata_input,
            computed::PendingRecordOwnership::default(),
            &mut scratch,
            counters,
        );
        let bytes = scratch.capacity_bytes();
        self.memory.reserve_required(MemoryCategory::BatchScratch, bytes);
        drop(scratch);
        self.memory.release(MemoryCategory::BatchScratch, bytes);
        publication
    }

    pub(crate) fn assign_shared_style_record(
        &mut self,
        target: computed::ComputedStyleTarget,
        style_record: u64,
        inherited_group_count: usize,
        inherited_group_swap_eligible: bool,
        counters: &mut Counters,
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
        if let Some(current_cascade_state) = current_cascade_state {
            self.bind_published_cascade_state(target, current_cascade_state, publication.node_handle_changed, counters);
        } else {
            self.computed_group_sets.clear_cascade_state(target);
        }
        self.settle_computed_memory();
        counters.add(Counter::ComputedGroupsReused, group_count as u64);
        self.note_identity_mints(counters);
        counters.bump(Counter::ComputedGroupSetsReused);
        counters.bump(Counter::InheritedGroupSetsReused);
        counters.bump(Counter::CustomPropertyEnvironmentsReused);
        counters.bump(Counter::ComputedFixedMetadataReused);
        counters.bump(Counter::StyleRecordsReused);
        if publication.node_handle_changed {
            counters.bump(Counter::ComputedGroupNodeHandlesPublished);
        }
        if publication.inherited_node_handle_changed {
            counters.bump(Counter::InheritedGroupNodeHandlesPublished);
        }
        if publication.custom_property_environment_node_handle_changed {
            counters.bump(Counter::CustomPropertyEnvironmentNodeHandlesPublished);
        }
        if publication.computed_fixed_metadata_node_handle_changed {
            counters.bump(Counter::ComputedFixedMetadataNodeHandlesPublished);
        }
        if publication.style_record_node_handle_changed {
            counters.bump(Counter::StyleRecordNodeHandlesPublished);
        }
        if publication.animation_overlay_slot_released {
            counters.bump(Counter::AnimationOverlaySlotsReleased);
        }
        counters.set(
            Counter::LiveAnimationOverlayRecords,
            publication.live_animation_overlay_records as u64,
        );
        if publication.is_pseudo && publication.style_record_node_handle_changed {
            counters.bump(Counter::ComputedPseudoAssignmentsPublished);
        }
        publication
    }

    /// Intern the immutable computed-group payloads of a style which has no live StyleEngine target.
    pub(crate) fn intern_computed_groups(
        &mut self,
        payloads: &[SharedPayload],
        inherited_group_count: usize,
        custom_property_environment: u64,
        metadata_input: computed::ComputedMetadataInput<'_>,
        counters: &mut Counters,
    ) -> computed::ComputedGroupPublication {
        self.publish_computed_groups_impl(
            None,
            payloads,
            inherited_group_count,
            custom_property_environment,
            metadata_input,
            computed::PendingRecordOwnership::default(),
            &mut EngineComputabilityScratch::default(),
            counters,
        )
    }

    pub(crate) fn style_record_payloads(&self, style_record: u64) -> Option<&[SharedPayload]> {
        self.computed_group_sets.style_record_payloads(style_record)
    }

    pub(crate) fn style_record_dependency_flags(&self, style_record: u64) -> Option<u8> {
        self.computed_group_sets.style_record_dependency_flags(style_record)
    }

    /// Whether `style_record` holds an image a box loads. A record that is gone holds none.
    pub(crate) fn style_record_holds_image_values(&self, style_record: u64) -> bool {
        self.style_record_dependency_flags(style_record)
            .is_some_and(|flags| flags & computed::HOLDS_IMAGE_VALUES != 0)
    }

    pub(crate) fn style_record_view(&self, style_record: u64) -> Option<computed::StyleRecordView<'_>> {
        self.computed_group_sets.style_record_view(style_record)
    }

    pub(crate) fn pin_style_record(&mut self, style_record: u64) {
        self.computed_group_sets.pin_style_record(style_record);
    }

    pub(crate) fn lease_style_records(&self) -> computed::StyleRecordLease {
        self.computed_group_sets.lease_style_records()
    }

    pub(crate) fn begin_style_record_view_epoch(&mut self) {
        self.computed_group_sets.begin_style_record_view_epoch();
    }

    /// Publishes how many identities each catalog has minted. These count the sharing partition
    /// a run produces; the reuse counters beside them credit whichever publication interned an
    /// identity first, which is an execution-order decision.
    fn note_identity_mints(&mut self, counters: &mut Counters) {
        let mints = self.computed_group_sets.identity_mints();
        counters.set(Counter::ComputedGroupIdentitiesMinted, mints.groups);
        counters.set(Counter::ComputedGroupSetIdentitiesMinted, mints.group_sets);
        counters.set(Counter::InheritedGroupSetIdentitiesMinted, mints.inherited_group_sets);
        counters.set(Counter::StyleRecordIdentitiesMinted, mints.style_records);
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

    pub(crate) fn unpin_style_record(&mut self, style_record: u64) {
        self.computed_group_sets.unpin_style_record(style_record);
    }

    fn bind_published_cascade_state(
        &mut self,
        target: computed::ComputedStyleTarget,
        (current_generation, current_cascade_state): (u64, CascadeStateID),
        node_handle_changed: bool,
        counters: &mut Counters,
    ) {
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
            counters.bump(Counter::CascadeWinnerDeltaStops);
            return;
        }
        counters.add(Counter::CascadeWinnerDeltaProperties, delta.properties().len() as u64);
        counters.add(
            Counter::ComputedWinnerDeltaPropertiesConsumed,
            delta.properties().len() as u64,
        );
        if !node_handle_changed {
            counters.bump(Counter::ComputedWinnerPropagationStops);
        }
    }

    #[allow(clippy::too_many_arguments)]
    /// `owned` says which payloads, and whether the table, the caller hands to the catalog; the
    /// publication says which of those references the catalog took.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn publish_computed_groups_impl(
        &mut self,
        target: Option<computed::ComputedStyleTarget>,
        payloads: &[SharedPayload],
        inherited_group_count: usize,
        custom_property_environment: u64,
        metadata_input: computed::ComputedMetadataInput<'_>,
        owned: computed::PendingRecordOwnership,
        scratch: &mut EngineComputabilityScratch,
        counters: &mut Counters,
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
            owned,
        );
        if let Some(target) = target
            && !target.is_pseudo()
            && self.computed_group_sets.adjustment_facts(target.node())
                & bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT
                != 0
        {
            self.prepare_root_font_metrics_from_record(publication.style_record_identity);
        }
        if let Some(current_cascade_state) = current_cascade_state {
            let target = target.expect("only a target has pending cascade state");
            self.bind_published_cascade_state(target, current_cascade_state, publication.node_handle_changed, counters);
            self.remember_cold_record_candidate(
                target,
                current_cascade_state,
                custom_property_environment,
                pseudo_styles,
                publication.previous_style_record_identity,
                publication.style_record_identity,
                is_base_record,
                scratch,
                counters,
            );
        } else if let Some(target) = target {
            self.computed_group_sets.clear_cascade_state(target);
        }
        self.settle_computed_memory();
        counters.add(
            Counter::ComputedOutputGroupsCanonicalized,
            publication.canonical_output_groups_reused as u64,
        );
        counters.add(
            Counter::ComputedGroupsReused,
            (payloads.len() - publication.new_groups) as u64,
        );
        self.note_identity_mints(counters);
        if !publication.new_group_set {
            counters.bump(Counter::ComputedGroupSetsReused);
        }
        if !publication.new_inherited_group_set {
            counters.bump(Counter::InheritedGroupSetsReused);
        }
        if publication.node_handle_changed {
            counters.bump(Counter::ComputedGroupNodeHandlesPublished);
        }
        if publication.inherited_node_handle_changed {
            counters.bump(Counter::InheritedGroupNodeHandlesPublished);
        }
        if !publication.new_custom_property_environment {
            counters.bump(Counter::CustomPropertyEnvironmentsReused);
        }
        if publication.custom_property_environment_node_handle_changed {
            counters.bump(Counter::CustomPropertyEnvironmentNodeHandlesPublished);
        }
        match publication.new_computed_fixed_metadata {
            true => counters.bump(Counter::ComputedFixedMetadataInterned),
            false => counters.bump(Counter::ComputedFixedMetadataReused),
        }
        if publication.computed_fixed_metadata_node_handle_changed {
            counters.bump(Counter::ComputedFixedMetadataNodeHandlesPublished);
        }
        match publication.new_style_record {
            true => counters.bump(Counter::StyleRecordsInterned),
            false => counters.bump(Counter::StyleRecordsReused),
        }
        if publication.style_record_node_handle_changed {
            counters.bump(Counter::StyleRecordNodeHandlesPublished);
        }
        if publication.animation_overlay_slot_allocated {
            counters.bump(Counter::AnimationOverlaySlotsAllocated);
        }
        if publication.animation_overlay_slot_released {
            counters.bump(Counter::AnimationOverlaySlotsReleased);
        }
        if publication.animation_overlay_record_updated {
            counters.bump(Counter::AnimationOverlayRecordsUpdated);
        }
        counters.set(
            Counter::LiveAnimationOverlayRecords,
            publication.live_animation_overlay_records as u64,
        );
        if publication.is_pseudo && publication.style_record_node_handle_changed {
            counters.bump(Counter::ComputedPseudoAssignmentsPublished);
        }
        publication
    }

    pub(crate) fn current_color_dependent_group_mask(&self, node: StyleNodeID, pseudo_kind: u8) -> Option<u32> {
        let target = computed::ComputedStyleTarget::new(node, pseudo_kind);
        let dependencies = self.computed_group_sets.current_color_dependency_mask(target)?;
        let caret_color_group = computed_group_output_mask(crate::css::property_metadata::property_id::CARET_COLOR)?;
        let accent_color_group = computed_group_output_mask(crate::css::property_metadata::property_id::ACCENT_COLOR)?;
        Some(dependencies | caret_color_group | accent_color_group)
    }

    pub(crate) fn remove_computed_pseudo(
        &mut self,
        node: StyleNodeID,
        pseudo_kind: u8,
        counters: &mut Counters,
    ) -> Option<computed::FinalStyleRecordID> {
        let target = computed::ComputedStyleTarget::new(node, pseudo_kind);
        // A record the engine derived for the pseudo-element goes with it, uninstalled: the host removed the
        // pseudo-element instead, as where its element left the top layer beside the transaction that derived it.
        self.forget_engine_computed_record(target);
        if let Some(state) = self.computed_group_sets.take_pending_cascade_state(target) {
            self.computed_group_sets
                .observe_absent_pseudo_cascade_state(target, state);
        }
        let live_animation_overlays_before = self.computed_group_sets.live_animation_overlay_records();
        let removed_style_record = self.computed_group_sets.remove_pseudo(node, pseudo_kind);
        let live_animation_overlays_after = self.computed_group_sets.live_animation_overlay_records();
        self.settle_computed_memory();
        let removed_style_record = removed_style_record?;
        counters.bump(Counter::ComputedPseudoAssignmentsRemoved);
        counters.bump(Counter::StyleRecordNodeHandlesPublished);
        counters.add(
            Counter::AnimationOverlaySlotsReleased,
            (live_animation_overlays_before - live_animation_overlays_after) as u64,
        );
        counters.set(
            Counter::LiveAnimationOverlayRecords,
            live_animation_overlays_after as u64,
        );
        Some(removed_style_record)
    }
}

impl StyleEngineState {
    pub(super) fn reclaim_computed_memory_if_needed(&mut self, counters: &mut Counters) {
        self.reclaim_unreachable_computed_records(counters);
        self.settle_computed_memory();
    }

    /// Frees what the engine kept for the leases of published frames, once none is held. The host calls it as it
    /// takes a recording in, so that a document that styles nothing more still frees what its recording kept.
    pub(crate) fn free_style_records_kept_for_leases(&mut self, counters: &mut Counters) {
        let freed_overlays = self.retained.computed_group_sets.free_retired_animation_overlays();
        if self.reclaim_unreachable_computed_records(counters) || freed_overlays {
            self.settle_computed_memory();
        }
    }

    /// Reclaims the unreachable computed records, if they are due and nothing views them, and answers whether it did.
    fn reclaim_unreachable_computed_records(&mut self, counters: &mut Counters) -> bool {
        let Some(retention) = self.retained.computed_group_sets.reclaim_unreachable_if_needed() else {
            return false;
        };
        counters.set(Counter::ComputedGroupsRetained, retention.retained as u64);
        counters.set(Counter::ComputedGroupsReachable, retention.reachable as u64);
        // An element's animation overlay is named by no record, only by the element holding it.
        let live: super::fast_hash::FastSet<u64> = self
            .retained
            .computed_group_sets
            .live_custom_property_environments()
            .chain(
                self.retained
                    .element_custom_property_data
                    .values()
                    .map(|held| held.identity),
            )
            .collect();
        self.retained
            .custom_property_environments
            .retain_only(|identity| live.contains(&identity));
        true
    }

    pub(super) fn publish_animation_overlay_impl(
        &mut self,
        target: computed::ComputedStyleTarget,
        source_identity: u64,
        animated_overlay: HostShared<crate::css::animated_overlay::AnimatedOverlay>,
        payloads: &[SharedPayload],
        counters: &mut Counters,
    ) -> Option<computed::AnimationOverlayUpdate> {
        // A pseudo-element inherits from its element.
        let parent = if target.is_pseudo() {
            Some(target.node())
        } else {
            self.retained.tree.inheritance_parent(target.node())
        };
        let parent_in_display_none_subtree = parent
            .and_then(|parent| self.retained.computed_group_sets.assigned_style_record(parent))
            .and_then(|record| {
                self.retained
                    .computed_group_sets
                    .style_record_dependency_flags(record.raw())
            })
            .is_some_and(|flags| flags & computed::IN_DISPLAY_NONE_SUBTREE != 0);
        let publication = self.retained.computed_group_sets.publish_animation_overlay(
            target,
            source_identity,
            animated_overlay,
            payloads,
            parent_in_display_none_subtree,
        )?;
        self.settle_computed_memory();
        if publication.slot_allocated {
            counters.bump(Counter::AnimationOverlaySlotsAllocated);
        }
        if publication.slot_released {
            counters.bump(Counter::AnimationOverlaySlotsReleased);
        }
        if publication.record_updated {
            counters.bump(Counter::AnimationOverlayRecordsUpdated);
        }
        counters.set(Counter::LiveAnimationOverlayRecords, publication.live_records as u64);
        Some(publication)
    }
}

/// What the records of a node substitute that resolves against the element itself, which makes
/// them the element's alone.
#[derive(Clone, Copy, Default)]
pub(super) struct ElementRelativeSubstitutions {
    /// The records that do, a bit each as `rolled_back_bit` numbers them.
    records: u64,
    /// The container-relative units they substituted, as a `container_relative_length_unit_mask`.
    pub(super) container_units: u8,
}

/// The bit of `nodes_with_rolled_back_records` that stands for a node's element record, or for one
/// of its pseudo-element records.
fn rolled_back_bit(pseudo_kind: Option<u8>) -> u64 {
    match pseudo_kind {
        Some(kind) => {
            debug_assert!(kind < 63, "a pseudo-element kind leaves the element's bit free");
            1 << kind
        }
        None => 1 << 63,
    }
}

/// The element facts a cold record is keyed by: every fact but the ones only the layout tree build
/// or a record move's damage reads, which no style depends on.
fn cold_record_facts(facts: u32) -> u32 {
    use bridge::element_adjustment_fact::{LAYOUT_TREE_FACTS, RECORD_DAMAGE_FACTS};
    facts & !(LAYOUT_TREE_FACTS | RECORD_DAMAGE_FACTS)
}

/// The bit a node's own record holds among what its records read of its place among its siblings,
/// above one bit per synthetic pseudo-element kind.
const ELEMENT_READS_SIBLING_POSITION: u16 = 1 << pseudo_kind::SYNTHETIC_COUNT;

/// Where an element stands among its element siblings, counted from one: what `sibling-count()`
/// and `sibling-index()` read.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub(super) struct SiblingPosition {
    pub(super) count: u32,
    pub(super) index: u32,
}

/// What a first record was derived from: the parent's side of the computation, the winner state
/// (with the generation its identity belongs to), the element facts, the pseudo-elements the
/// element has rules for, and the font environment.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(super) struct ColdRecordKey {
    /// What the monospace font-size recascade gives this node, or zero when its winning
    /// font-family is not monospace. The recascade reads the whole cascaded chain, which equal
    /// parent records do not pin down.
    monospace_recascaded_font_size: i32,
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
    root_font_inputs: RootFontInputs,
    /// Where the element stands among its siblings, when its winners read it.
    sibling_position: Option<SiblingPosition>,
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

/// What a warm record is derived from: the record it replaces, the winner state, the element
/// facts, the parent, the custom-property environment, the root's font inputs, the monospace
/// recascade and the sibling position a tree-counting function reads. A record whose winners read
/// `attr()` has none: it is the element's alone.
type RecordCohortKey = (
    u64,
    CascadeStateID,
    u32,
    RecordDeltaParent,
    u64,
    RootFontInputs,
    i32,
    Option<SiblingPosition>,
);

/// A first record the engine keeps for reuse, with the swap eligibility its assignment carries,
/// the style groups it read straight from the parent through an explicit `inherit`, which every
/// element it answers for marks its own parent with, and whether it reads the element's place
/// among its siblings, which every element it answers for is noted as reading.
#[derive(Clone, Copy)]
pub(super) struct ColdRecord {
    record: computed::FinalStyleRecordID,
    swap_eligible: bool,
    explicitly_inherited_groups: u32,
    reads_sibling_position: bool,
}

/// The proof that a record reads no resource context: no value its winners give it resolves a
/// `url()` against a style sheet's or the document's base URL. The engine keeps only such records
/// across transactions, so a move of those base URLs, which every `history.pushState()` makes
/// without a `<base>`, leaves nothing it keeps to forget.
#[derive(Clone, Copy)]
pub(super) struct ReadsNoResourceContexts(());

/// What a record computed from a winner state reads beside the state's winners: the element's
/// place among its siblings, and whether a `url()` resolves against a resource context.
#[derive(Clone, Copy)]
pub(super) struct StateRecordReads {
    pub(super) sibling_position: bool,
    pub(super) no_resource_contexts: Option<ReadsNoResourceContexts>,
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
    root_font_inputs: RootFontInputs,
    sibling_position: Option<SiblingPosition>,
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
    /// Whether the host owes the element the transition step once it installs the record: the
    /// record moves, and either the one it replaces declares transitions or the move changes the
    /// declarations.
    owes_a_transition_step: bool,
    /// The composition the element's animations laid over the record this one replaces, detached
    /// and pinned until C++ installs the record, computes one itself, or the derivation is reverted.
    detached_composition: Option<computed::DetachedComposition>,
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

/// Why a row's record is driven again in full, its pseudo-elements with it, while its winners
/// stand: inputs no winner shows moved under it.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum FullDriveReason {
    /// A descendant recompute, for the root's font metrics or an ancestor's direction, writing
    /// mode or container type, or an ancestor leaving display:none, which cleared the element's
    /// style on the way in.
    AncestorChange,
    /// A `@function` the element calls, or a counter style its record or a pseudo-element's
    /// resolved, moved.
    NamedRuleContext,
    /// A face the element's font cascade names became available or failed, or the document's
    /// `@font-feature-values` moved.
    FontEnvironment,
    /// A container moved that the element's or a pseudo-element's values measure, through a
    /// container-relative length or a substitution.
    ContainerMoved,
    /// A style pass gave the row up, and with it what moved under the record that only the pass
    /// saw, such as the viewport.
    PassGivenUp,
}

/// Static checks attached to the original declaration spelling at input preparation.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) struct WrittenValueChecks {
    whole_context_free: bool,
    longhand_context_free: Option<bool>,
}

impl WrittenValueChecks {
    pub(super) fn prepare(property: u16, value: &crate::css::style_value::RetainedStyleValueData) -> Self {
        use crate::css::style_value::StyleValueData;
        let whole_context_free = value_computes_without_document_context(value.data());
        let longhand_context_free = match value.data() {
            StyleValueData::Unresolved { .. } | StyleValueData::PendingSubstitution { .. } => None,
            StyleValueData::Shorthand { .. } => Some(
                shorthand_longhand_value(property, value.data())
                    .is_some_and(|value| value_computes_without_document_context(value.data())),
            ),
            _ => Some(whole_context_free),
        };
        Self {
            whole_context_free,
            longhand_context_free,
        }
    }
}

#[derive(Default)]
pub(super) struct EngineComputabilityScratch {
    /// What a record computed from the state reads beside its winners, or `None` when the engine
    /// computes none from it.
    // NB: Equal winner states can have different per-node written declaration inputs.
    states: HashMap<(StyleNodeID, u64, CascadeStateID, u64, u64), Option<StateRecordReads>>,
}

impl EngineComputabilityScratch {
    fn capacity_bytes(&self) -> u64 {
        capacity::capacity_bytes! {
            shallow [self.states];
            cached [];
            nested [];
            skip [];
        }
    }

    fn remember(&mut self, key: (StyleNodeID, u64, CascadeStateID, u64, u64), admitted: Option<StateRecordReads>) {
        if self.states.len() >= COLD_RECORD_CACHE_LIMIT {
            self.states.clear();
        }
        self.states.insert(key, admitted);
    }
}

#[derive(Default)]
pub(super) struct EngineComputedRecordScratch {
    pub(super) font_drive: drive::FontDriveScratch,
    /// Whether this flush carries a document environment change. A record's winners stand
    /// through one while the values they computed to may not, so such a record is driven again
    /// in full against the document's inputs rather than kept.
    pub(super) document_environment_moved: bool,
    /// Whether the viewport moved since the last flush drove records. A record holding a value
    /// resolved against it cannot stand, whatever its winners did.
    pub(super) viewport_moved: bool,
    /// Whether the row being derived had its selector answer or its declarations move this flush
    /// without its winners necessarily being published again.
    pub(super) answer_or_declarations_moved: bool,
    /// Whether the row's only reaction is that the element's pseudo-element inputs may have
    /// changed (a deferred ::selection becoming observable): its own record stands where its
    /// winners do.
    pub(super) pseudo_inputs_alone: bool,
    pub(super) prepared_root_font: Option<(StyleNodeID, ParentInputsMoved, drive::FontDriveScratch)>,
    // NB: Preserve the root's existing remaining-phase context after preparing consumer inputs.
    root_element_inputs: Option<(StyleNodeID, RootFontInputs)>,
    root_font_inputs_changed: bool,
    pending_element: Option<drive::PendingElement>,
    next_pseudo: usize,
    pseudo_uses_substitution: bool,
    /// What the element being derived noted about substituted winners, when its computation
    /// reached the point of deciding. A record that stands unchanged notes nothing.
    noted_substitution: Option<bool>,
    /// Whether the element derived last was computed with a substituted winner: what the flush
    /// publishes beside its record, decided by the step rather than read back out of the
    /// retained set.
    pub(super) element_uses_substitution: bool,
    /// The non-inherited style groups the element being derived read straight from its parent
    /// through an explicit `inherit`, and those its pseudo-elements read from it, which the row
    /// carries for C++ to mark the parent with.
    pub(super) element_explicitly_inherited_groups: u32,
    /// Whether the host applies the animation plan of a record this scratch derives once it has
    /// installed it: a style update's batch and its retries do, a record demand does not.
    pub(super) host_applies_animation_plans: bool,
    /// The elements whose rows this flush settled that the host composes once it installs their
    /// records: their animations, and those the records' plans start, sampled over them.
    nodes_composed_by_the_host: HashSet<StyleNodeID>,
    /// The nodes whose substituted-record fact the step decided, in the order it decided them.
    /// The boundary that installs the record applies them.
    substitution_effects: Vec<(StyleNodeID, bool)>,
    /// Warm records derived this flush, by what they were derived from, with the style groups
    /// they explicitly inherit and whether they read the element's place among its siblings.
    cohorts: HashMap<RecordCohortKey, (computed::FinalStyleRecordID, u32, bool)>,
    computability: EngineComputabilityScratch,
    /// What each node the walk has reached tells its children: whether the chain above it is
    /// confined, and whether it resolved the record its children inherit from. A column with
    /// touched-page allocation, because the walk writes a row for every node it processes and
    /// reads one for every node's parent.
    pub(super) derived_child_inputs: column::PagedColumn<column::PagedValuePage<DerivedChildInputs>>,
    /// First records derived this flush, by what they were derived from.
    pub(super) cold_cohorts: HashMap<ColdRecordKey, ColdRecord>,
    store_capacity_bytes: u64,
    pub(super) stores: HashMap<(CascadeStateID, u64), std::sync::Arc<WinnerStore>>,
    /// The states whose store, under an environment, substituted a custom property into a winner.
    pub(super) substituted_states: HashSet<(CascadeStateID, u64)>,
    /// Pseudo-element records derived this flush, by what they were derived from.
    pub(super) pseudo_cohorts: HashMap<PseudoCohortKey, computed::FinalStyleRecordID>,
    pub(super) pseudo_stores: HashMap<(u8, CascadeStateID, u64), std::sync::Arc<WinnerStore>>,
    /// The nodes whose custom-property environment this flush has brought up to date.
    current_custom_property_environments: HashMap<StyleNodeID, u64>,
    /// The pseudo-element records settled beside the element derived last.
    pub(super) pseudo_deltas: Vec<PseudoRecordDelta>,
    /// The custom-property environment of the pseudo-element a read settled without a box.
    pub(super) boxless_read_environment: u64,
    /// The pseudo-element rules that flipped for the element being derived.
    pub(super) flipped_pseudo_rules: u64,
    /// Whether the drive answers a read-only demand, which holds no leave to republish winners.
    read_only: bool,
}

/// What one node tells its flat-tree children, decided where the node settles and read by the
/// children in the same pass: whether the node resolved the record its children inherit from, and
/// the accumulated proof about the chain above it, so a child reads one row instead of walking to
/// the document element for every gate it has to pass.
#[derive(Clone, Copy, Default)]
pub(super) struct DerivedChildInputs {
    /// Whether the node's record settled this flush: what its descendants inherit from is in place.
    pub(super) settled: bool,
    /// Whether the node is a row of this flush whose record the engine left to the host: the
    /// record, and the container inputs it publishes, are installed after the flush. A row the
    /// walk keeps only for its chain is no such row.
    pub(super) declined: bool,
    /// Whether the node is a row of this flush answered as hidden, in a display:none subtree, as
    /// is everything that inherits from it.
    pub(super) hidden: bool,
    /// Whether the node took an inherited-style reaction and resolved no record of its own, so
    /// its immediate children cannot take the direct inherited-group path. Deliberately separate
    /// from the chain proof: that is the accumulated confinement argument, this is the immediate
    /// parent's own unresolved fact.
    pub(super) inheritance_unresolved: bool,
    /// The proof about everything from this node upwards, folded on the first ask a child makes
    /// and kept for the asks after it.
    pub(super) chain: Option<AncestorChain>,
}

/// The published chain from one node to the root, as a child's gate reads it.
#[derive(Clone, Copy)]
pub(super) struct AncestorChain {
    /// Whether any published node from this one to the root publishes a change the engine cannot
    /// prove confined. Once an unsettled node sits below one of those, no descendant's record is
    /// exact.
    unconfined_above: bool,
    /// `None` when an unsettled gap sits below an unconfined published ancestor, and otherwise
    /// whether a child relies on an ancestor this flush settled.
    proof: Option<bool>,
}

impl AncestorChain {
    /// What the document element's parent says: nothing above it, and nothing unsettled.
    pub(super) const ROOT: Self = Self {
        unconfined_above: false,
        proof: Some(false),
    };

    /// The proof a child of this node needs.
    pub(super) fn ancestors_are_confined(self) -> Option<bool> {
        self.proof
    }

    /// Fold one node onto what its parent says. `published` says the flush published a reaction
    /// for the node, `unconfined` that the published change may move something a descendant
    /// inherits, and `settled` that the node's record is in place.
    ///
    /// This is the upward walk written as a recurrence. The walk carries "something closer to the
    /// child is unsettled" downward and stops at the first unconfined published ancestor once it
    /// is set, which is exactly `unconfined_above` read from the unsettled node.
    pub(super) fn fold(parent: Self, published: bool, unconfined: bool, settled: bool) -> Self {
        let unconfined_here = published && unconfined;
        let proof = if unconfined_here && !settled {
            None
        } else if settled {
            parent.proof.map(|relied| unconfined_here || relied)
        } else if parent.unconfined_above {
            None
        } else {
            Some(unconfined_here)
        };
        Self {
            unconfined_above: unconfined_here || parent.unconfined_above,
            proof,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
enum RecordDeltaParent {
    Exact(u64),
    Inputs(ColdRecordParent),
}

/// Which element and pseudo winner rows must reflect this update's exact rule flips.
#[derive(Clone, Copy, Default)]
pub(super) struct FlippedRules {
    element: bool,
    pseudos: u64,
}

impl FromIterator<Option<u16>> for FlippedRules {
    fn from_iter<T: IntoIterator<Item = Option<u16>>>(kinds: T) -> Self {
        let mut result = Self::default();
        for kind in kinds {
            match kind {
                None => result.element = true,
                Some(kind) => {
                    // NB: Only the existing synthetic pseudo inventory consumes these bits.
                    if let Some(bit) = 1_u64.checked_shl(u32::from(kind)) {
                        result.pseudos |= bit;
                    }
                }
            }
        }
        result
    }
}

impl EngineComputedRecordScratch {
    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity::capacity_bytes! {
            shallow [self.computability.states, self.cohorts, self.derived_child_inputs, self.cold_cohorts, self.stores,
                self.substituted_states, self.pseudo_cohorts, self.pseudo_stores,
                self.pseudo_deltas, self.substitution_effects, self.current_custom_property_environments];
            cached [self.store_capacity_bytes, self.font_drive.capacity_bytes(),
                self.prepared_root_font.as_ref().map_or(0, |(_, _, drive)| drive.capacity_bytes())];
            nested [];
            skip [];
        }
    }
}

/// What a drive computes for: the node whose record it inherits from (the flat-tree parent, or
/// the originating element of a pseudo-element) and the element facts the computation's
/// adjustments read.
#[derive(Clone, Copy)]
pub(super) struct DriveSubject {
    /// What is being driven, element or pseudo-element. A drive that suspends to wait for a font
    /// names it, so the suspended drive can only be resumed by the one it belongs to.
    target: computed::ComputedStyleTarget,
    /// The flat-tree parent the element inherits from; the document element has none and
    /// inherits from the initial values.
    parent: Option<StyleNodeID>,
    facts: u32,
}

/// What a demand for an element's record settles: the record with the groups it explicitly
/// inherits, and the pseudo-element records the engine settled beside it, one slot per synthetic
/// kind with a present bit each; a present slot holding zero is a removal.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct DemandedEngineRecord {
    pub(crate) style_record: u64,
    pub(crate) explicitly_inherited_groups: u32,
    pub(crate) owes_an_animation_plan: bool,
    pub(crate) owes_a_transition_step: bool,
    pub(crate) composed_by_the_host: bool,
    pub(crate) pseudo_records_present: u16,
    pub(crate) pseudo_records: [u64; bridge::PSEUDO_RECORD_SLOTS],
}

const _: () = assert!(pseudo_kind::SYNTHETIC_COUNT == bridge::PSEUDO_RECORD_SLOTS);

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
    /// What the monospace font-size recascade gives the pseudo-element, as for an element's cohort.
    monospace_recascaded_font_size: i32,
    parent_record: u64,
    /// For a highlight pseudo-element, the record of the same pseudo-element of its nearest
    /// ancestor it inherits from.
    highlight_parent_record: u64,
    inherited_groups: u32,
    parent_display: u32,
    dependency_flags: u8,
    environment: u64,
    kind: u8,
    generation: u64,
    state: Option<CascadeStateID>,
    facts: u32,
    font_environment_generation: u64,
    root_font_inputs: RootFontInputs,
    /// Where the originating element stands among its siblings, when the pseudo-element's winners
    /// read it.
    sibling_position: Option<SiblingPosition>,
}

/// The synthetic pseudo-element kinds, as the C++ `PseudoElement` enumeration numbers them.
pub(super) mod pseudo_kind {
    pub(in crate::css::style) const AFTER: u8 = 0;
    pub(in crate::css::style) const BACKDROP: u8 = 1;
    pub(in crate::css::style) const BEFORE: u8 = 2;
    pub(in crate::css::style) const FIRST_LETTER: u8 = 3;
    pub(in crate::css::style) const FIRST_LINE: u8 = 4;
    pub(in crate::css::style) const MARKER: u8 = 5;
    pub(in crate::css::style) const SEARCH_TEXT: u8 = 6;
    pub(in crate::css::style) const SEARCH_TEXT_CURRENT: u8 = 7;
    pub(in crate::css::style) const SELECTION: u8 = 8;
    pub(in crate::css::style) const VIEW_TRANSITION: u8 = 9;
    pub(in crate::css::style) const SYNTHETIC_COUNT: usize = 10;
    /// Every kind, in the order a settlement goes through them: first the ones a style update
    /// settles, then the ones only a read asks for.
    pub(in crate::css::style) const SETTLEMENT_ORDER: [u8; SYNTHETIC_COUNT] = [
        BEFORE,
        AFTER,
        FIRST_LETTER,
        SELECTION,
        SEARCH_TEXT,
        SEARCH_TEXT_CURRENT,
        BACKDROP,
        MARKER,
        FIRST_LINE,
        VIEW_TRANSITION,
    ];
    /// The kinds an element in the host's shadow tree backs, as a mask.
    pub(in crate::css::style) const ELEMENT_REFERENCE_KINDS: u64 =
        ((1 << (super::bridge::LAST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND + 1)) - 1)
            & !((1 << super::bridge::FIRST_ELEMENT_REFERENCE_PSEUDO_ELEMENT_KIND) - 1);

    /// https://drafts.csswg.org/css-pseudo-4/#highlight-pseudos
    pub(in crate::css::style) const fn is_highlight(kind: u8) -> bool {
        matches!(kind, SEARCH_TEXT | SEARCH_TEXT_CURRENT | SELECTION)
    }
}

/// The element facts a pseudo-element's computation reads: the C++ adjustments for what the
/// originating element is stay off for its pseudo-elements, past the ones about its markup
/// language.
const PSEUDO_ELEMENT_ADJUSTMENT_FACTS: u32 = {
    use bridge::element_adjustment_fact as fact;
    fact::IS_MATHML
        | fact::IS_MATHML_MTABLE
        | fact::IS_MATHML_MTR
        | fact::IS_MATHML_MTD
        | fact::IS_TH
        | fact::HAS_ANIMATIONS
};

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

/// Whether a longhand is read by the box-type transformation, which rewrites the computed display.
/// A `-webkit-box` becomes a block container while its `-webkit-box-orient` and `continue` say
/// that `-webkit-line-clamp` applies to it.
fn property_feeds_box_type_transformation(property: u16) -> bool {
    use crate::css::property_metadata::property_id as prop;
    matches!(
        property,
        prop::DISPLAY | prop::POSITION | prop::FLOAT | prop::_WEBKIT_BOX_ORIENT | prop::CONTINUE
    )
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

/// The computed style groups a moved longhand reaches. One the font group carries feeds no group of
/// its own and reaches every value the font feeds, through the font group: a delta moving it is
/// driven in full, as is one moving a longhand no registered group names.
fn moved_longhand_groups(style_groups: &StyleGroupMasks, property: u16) -> u32 {
    use crate::css::computed_value_types::STYLE_GROUP_INDEX_FONT;
    match style_groups.dependencies(property) {
        0 if font_group_carries_longhand(property) => 1 << STYLE_GROUP_INDEX_FONT,
        0 => (1 << crate::css::table_group_builder::group_index::COUNT) - 1,
        groups => groups,
    }
}

/// The font-phase longhands the font group carries without a group binding of their own.
fn font_group_carries_longhand(property: u16) -> bool {
    use crate::css::property_metadata::property_id as prop;
    font_resolution_selects_by(property)
        || matches!(
            property,
            prop::FONT_FEATURE_SETTINGS
                | prop::FONT_KERNING
                | prop::FONT_LANGUAGE_OVERRIDE
                | prop::FONT_VARIANT_ALTERNATES
                | prop::FONT_VARIANT_CAPS
                | prop::FONT_VARIANT_EAST_ASIAN
                | prop::FONT_VARIANT_EMOJI
                | prop::FONT_VARIANT_LIGATURES
                | prop::FONT_VARIANT_NUMERIC
                | prop::FONT_VARIANT_POSITION
                | prop::FONT_VARIATION_SETTINGS
                | prop::MATH_DEPTH
                | prop::MATH_SHIFT
                | prop::MATH_STYLE
                | prop::TEXT_RENDERING
        )
}

/// The counter-style names no @counter-style rule overrides: decimal, disc, square, circle,
/// disclosure-open and disclosure-closed.
fn counter_style_name_is_non_overridable(name: &[u16]) -> bool {
    [
        "decimal",
        "disc",
        "square",
        "circle",
        "disclosure-open",
        "disclosure-closed",
    ]
    .iter()
    .any(|candidate| {
        candidate.len() == name.len()
            && candidate
                .bytes()
                .zip(name)
                .all(|(expected, &unit)| unit < 128 && (unit as u8).eq_ignore_ascii_case(&expected))
    })
}

/// What a winner store is built for: a drive reads the winners admission proved.
#[derive(Clone, Copy, PartialEq, Eq)]
enum StoreUse {
    Admission,
    Drive,
}

/// Whether the property is one of the `animation-*` longhands that declare an element's CSS
/// animations, which the plan a record decides starts, retimes and cancels.
fn property_declares_css_animations(property: u16) -> bool {
    use crate::css::property_metadata::property_id as prop;
    matches!(
        property,
        prop::ANIMATION_COMPOSITION
            | prop::ANIMATION_DELAY
            | prop::ANIMATION_DIRECTION
            | prop::ANIMATION_DURATION
            | prop::ANIMATION_FILL_MODE
            | prop::ANIMATION_ITERATION_COUNT
            | prop::ANIMATION_NAME
            | prop::ANIMATION_PLAY_STATE
            | prop::ANIMATION_TIMELINE
            | prop::ANIMATION_TIMING_FUNCTION
    )
}

/// Whether the property is one of the `transition-*` longhands that declare which changes of an
/// element's values start transitions.
fn property_declares_transitions(property: u16) -> bool {
    use crate::css::property_metadata::property_id as prop;
    matches!(
        property,
        prop::TRANSITION_BEHAVIOR
            | prop::TRANSITION_DELAY
            | prop::TRANSITION_DURATION
            | prop::TRANSITION_PROPERTY
            | prop::TRANSITION_TIMING_FUNCTION
    )
}

/// Whether a winner of the property keeps its record in C++, which starts animations and
/// transitions from what it computes: a property of the animation group, and anything that is not a
/// longhand.
fn property_starts_animation(property: u16) -> bool {
    use crate::css::property_metadata::{
        FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, property_id as prop, property_style_group_index,
    };
    if !(FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property) {
        return true;
    }
    // A view transition name is a plain computed value; it starts nothing. So is a named timeline:
    // what finds it is an animation that names it in `animation-timeline`, which reads it from
    // whichever record the element holds when the animation starts.
    !matches!(
        property,
        prop::VIEW_TRANSITION_NAME
            | prop::SCROLL_TIMELINE_NAME
            | prop::SCROLL_TIMELINE_AXIS
            | prop::TIMELINE_SCOPE
            | prop::VIEW_TIMELINE_NAME
            | prop::VIEW_TIMELINE_AXIS
            | prop::VIEW_TIMELINE_INSET
    ) && property_style_group_index(property)
        .is_some_and(|group| usize::from(group) == crate::css::table_group_builder::group_index::ANIMATION)
}

/// Whether the drive holds what a value with these dependencies reads beyond the record, the
/// parent and the document's computation inputs: the base URLs a `url()` resolves against when
/// `resources_are_known`, the element's place among its siblings when the retained tree knows
/// it, and for an element-keyed value, the random base values the store draws for it and the
/// bases its container-relative lengths resolve against.
fn drive_holds_value_inputs(
    dependencies: &crate::css::style_compute::ExternalValueDependencies,
    resources_are_known: bool,
    sibling_position_is_known: impl FnOnce() -> bool,
) -> bool {
    (resources_are_known
        || (!dependencies.needs_document_base_url && !dependencies.may_need_style_sheet_resource_context))
        && (!dependencies.uses_tree_counting_function || sibling_position_is_known())
}

/// Whether a value resolves against the element itself: a `random()` it draws, or a
/// container-relative length its query containers measure.
fn value_resolves_against_the_element(dependencies: &crate::css::style_compute::ExternalValueDependencies) -> bool {
    dependencies.uses_random_function
        || dependencies.has_unfixed_random_sharing
        || dependencies.container_relative_length_unit_mask != 0
}

/// Whether a value holds a random function whose random caching key names the element.
fn value_draws_element_random_base(value: &StyleValueData) -> bool {
    let mut sharings = Vec::new();
    crate::css::style_compute::collect_unfixed_random_sharings_in_value(value, &mut sharings);
    sharings.into_iter().any(|sharing| {
        // SAFETY: The value retains every sharing in it.
        !crate::css::style_compute::random_caching_key(unsafe { &*sharing }).1
    })
}

/// Whether a written value computes from the record, the parent and the document's computation
/// inputs alone: no custom-property substitution, and none of the element or sheet facts the C++
/// computation gathers per drive.
fn value_computes_without_document_context(value: &StyleValueData) -> bool {
    // A longhand a shorthand written with a substitution declares holds a pending substitution
    // until the shorthand resolves.
    !matches!(
        value,
        StyleValueData::Unresolved { .. } | StyleValueData::PendingSubstitution { .. }
    ) && crate::css::style_compute::value_is_computationally_independent(value).is_some_and(|_| {
        let dependencies = crate::css::style_compute::external_value_dependencies(value);
        !value_resolves_against_the_element(&dependencies) && drive_holds_value_inputs(&dependencies, false, || false)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cold_ancestor_refreshes_inheritance_without_losing_own_custom_properties() {
        use crate::css::custom_properties::CustomPropertyStore;
        use crate::css::style_value::RetainedStyleValueData;
        use std::sync::Arc;

        let inherited_name = ak::Utf16FlyString::from_utf16(&"--inherited".encode_utf16().collect::<Vec<_>>());
        let own_name = ak::Utf16FlyString::from_utf16(&"--own".encode_utf16().collect::<Vec<_>>());
        let make_store = |parent, name: &ak::Utf16FlyString, text: &str, value| {
            let value = RetainedStyleValueData::from_owned(StyleValueData::Number { value });
            // SAFETY: The parent is null or retained by this test, and both the name and value
            //         remain live while cascaded_child retains them.
            let raw = unsafe {
                CustomPropertyStore::cascaded_child(
                    parent,
                    vec![(
                        name.raw_identity(),
                        text.encode_utf16().collect::<Vec<_>>().into(),
                        false,
                        value.pointer().cast(),
                    )],
                )
            };
            // SAFETY: cascaded_child transfers one Arc reference to its caller.
            unsafe { Arc::from_raw(raw.cast::<CustomPropertyStore>()) }
        };
        let old_parent = make_store(std::ptr::null(), &inherited_name, "--inherited", 1.0);
        let new_parent = make_store(std::ptr::null(), &inherited_name, "--inherited", 2.0);
        let own = make_store(Arc::as_ptr(&old_parent).cast(), &own_name, "--own", 42.0);
        let mut engine = StyleEngine::new();
        let mut raw_nodes = [0; 3];
        engine.allocate_style_nodes(&mut raw_nodes);
        let [root, ancestor, inheritor] = raw_nodes.map(|node| StyleNodeID::from_raw(node).unwrap());
        engine.tree.set_parent(ancestor, Some(root));
        engine.tree.set_parent(inheritor, Some(root));
        for (identity, store) in [(1, &old_parent), (2, &new_parent), (3, &own)] {
            // SAFETY: The test keeps these stores alive while the catalog retains them.
            unsafe {
                engine
                    .state
                    .retained
                    .custom_property_environments
                    .retain(identity, Arc::as_ptr(store).cast())
            };
        }
        for (node, environment) in [(root, 1), (ancestor, 3), (inheritor, 1)] {
            engine.publish_computed_groups(
                computed::ComputedStyleTarget::new(node, u8::MAX),
                &[],
                0,
                environment,
                computed::ComputedMetadataInput {
                    pseudo_element_styles: 0,
                    dependency_flags: 0,
                    counter_style_environment_identity: 0,
                    animation_overlay_identity: 0,
                    animated_overlay: HostShared::null(),
                    animation_overlay_payloads: &[],
                    longhand_table: HostShared::null(),
                },
            );
        }
        let mut scratch = EngineComputedRecordScratch::default();
        scratch.current_custom_property_environments.insert(root, 2);
        let retained = &mut engine.state.retained;
        let current = retained
            .current_custom_property_environment(ancestor, &Default::default(), &mut scratch)
            .unwrap();
        let store = retained.custom_property_environments.store(current).unwrap();
        // SAFETY: The catalog retains the refreshed environment.
        let store = unsafe { &*store.cast::<CustomPropertyStore>() };
        assert!(matches!(
            store.get(inherited_name.raw_identity()).unwrap().value.data(),
            StyleValueData::Number { value: 2.0 }
        ));
        assert!(matches!(
            store.get(own_name.raw_identity()).unwrap().value.data(),
            StyleValueData::Number { value: 42.0 }
        ));
        assert_eq!(
            retained.current_custom_property_environment(inheritor, &Default::default(), &mut scratch),
            Some(2)
        );
    }

    #[test]
    fn retained_highlight_inheritance_parent_uses_nearest_ancestor_pseudo_record() {
        let mut engine = StyleEngine::new();
        let mut raw_nodes = [0; 4];
        engine.allocate_style_nodes(&mut raw_nodes);
        let [root, parent, child, slot] = raw_nodes.map(|node| StyleNodeID::from_raw(node).unwrap());
        engine.tree.set_parent(parent, Some(root));
        engine.tree.set_parent(child, Some(parent));
        let retained = &mut engine.state.retained;
        retained.tree.set_assigned_slot(child, Some(slot), &mut retained.memory);
        retained.tree.set_parent(slot, Some(root));
        let publish = |engine: &mut StyleEngine, node, pseudo_kind| {
            engine
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
                        animated_overlay: HostShared::null(),
                        animation_overlay_payloads: &[],
                        longhand_table: HostShared::null(),
                    },
                )
                .style_record_identity
        };
        let root_record = publish(&mut engine, root, 0);
        assert_eq!(
            engine.retained_highlight_inheritance_parent_style_record(parent, 0),
            Some(root_record)
        );
        assert_eq!(
            engine.retained_highlight_inheritance_parent_style_record(child, 0),
            Some(root_record)
        );
        let slot_record = publish(&mut engine, slot, 0);
        assert_eq!(
            engine.retained_highlight_inheritance_parent_style_record(child, 0),
            Some(slot_record)
        );
    }

    #[test]
    fn pending_shorthand_lookup_returns_the_outermost_shorthand() {
        use crate::css::property_metadata::property_id;
        use crate::css::style_value::{RetainedStyleValueData, StyleValueData};

        let mut engine = StyleEngineState::new();
        let sheet = engine.program.add_sheet(StyleSheetObjectID(1), CascadeOrigin::Author);
        for (shorthand, nested_shorthand) in [
            (property_id::BACKGROUND, property_id::BACKGROUND_POSITION),
            (property_id::BORDER, property_id::BORDER_WIDTH),
            (property_id::FONT, property_id::FONT_VARIANT),
        ] {
            let rule = engine.program.append_rule(sheet, None, RuleKind::Style);
            let original = RetainedStyleValueData::from_owned(StyleValueData::Number { value: 1.0 });
            let pending = RetainedStyleValueData::from_owned(StyleValueData::PendingSubstitution {
                original_shorthand_value: original.clone_retained(),
            });
            let declared = |property| DeclaredProperty {
                property,
                important: false,
                operator: CascadeOperator::Declared,
                value: SpecifiedValueID(1),
            };
            engine.program.set_rule_declared_properties(
                rule,
                vec![declared(shorthand), declared(nested_shorthand)],
                vec![original.clone_retained(), pending.clone_retained()],
                vec![],
                vec![],
            );
            let (found_property, found_value) = engine
                .shorthand_declaration_written_as(StyleNodeID::element(1), WinnerSource::Rule(rule), pending.pointer())
                .unwrap();
            assert_eq!(found_property, shorthand);
            assert!(std::ptr::eq(found_value.pointer(), original.pointer()));
        }
    }

    #[test]
    fn computability_scratch_keeps_element_declaration_inputs_separate() {
        use crate::css::style_value::RetainedStyleValueData;

        let mut engine = StyleEngine::new();
        let mut raw_nodes = [0; 2];
        engine.allocate_style_nodes(&mut raw_nodes);
        let [first, second] = raw_nodes.map(|node| StyleNodeID::from_raw(node).unwrap());
        let declaration = DeclaredProperty {
            property: crate::css::property_metadata::property_id::OPACITY,
            important: false,
            operator: CascadeOperator::Declared,
            value: SpecifiedValueID(1),
        };
        let kind = ElementDeclarationKind::InlineStyle;
        let written = RetainedStyleValueData::from_owned(StyleValueData::Number { value: 0.5 });
        engine
            .facts
            .set_element_declared_properties(first, kind, vec![(declaration, written)]);
        // The second element declares nothing, so the winner names no declaration of its own.
        let winner = PropertyWinner {
            property: declaration.property,
            important: false,
            key: SpecifiedWinnerKey {
                value: declaration.value,
                operator: declaration.operator,
                continuation: cascade::CascadeContinuationID::default(),
                animation_relevance: 0,
                important: false,
            },
            priority: CascadePriority::exact_output_placeholder(),
            source: WinnerSource::Element(kind),
        };
        let state = engine.winner_groups.intern_sorted(&[winner], None);
        let cascade_state = (0, state);
        for order in [[first, second], [second, first]] {
            let mut scratch = EngineComputabilityScratch::default();
            for node in order {
                assert_eq!(
                    engine
                        .state
                        .computable_state_record_reads(node, cascade_state, &mut scratch, &mut engine.counters)
                        .is_some(),
                    node == first,
                );
            }
        }
    }

    #[test]
    fn acknowledgement_without_pending_records_observes_published_answer() {
        let mut engine = StyleEngine::new();
        let mut raw_nodes = [0; 2];
        engine.allocate_style_nodes(&mut raw_nodes);
        let [first, second] = raw_nodes.map(|node| StyleNodeID::from_raw(node).unwrap());
        for node in [first, second] {
            engine.state.retained.published_match_answers.push(
                PublishedMatchAnswer {
                    node,
                    cascade_input: None,
                    matches: None,
                    cascade_winners_are_complete: true,
                    observed: false,
                },
                &mut engine.state.retained.memory,
                &mut engine.counters,
            );
        }
        engine.published_match_answers.sort();
        assert!(engine.engine_computed_records_pending.is_empty());

        engine.acknowledge_engine_computed_record(second);
        let effects = std::mem::take(&mut engine.published_match_answers.answer_effects);
        engine.state.install_answer_effects(effects);
        assert!(engine.published_match_answers.lookup(second).unwrap().observed);
        assert!(!engine.published_match_answers.lookup(first).unwrap().observed);
        engine.acknowledge_engine_computed_record(second);
        let effects = std::mem::take(&mut engine.published_match_answers.answer_effects);
        engine.state.install_answer_effects(effects);
        assert!(engine.published_match_answers.lookup(second).unwrap().observed);
        assert_eq!(engine.counters.get(Counter::EngineComputedLonghandEvaluations), 0);
    }

    #[test]
    fn pending_records_acknowledge_and_rollback_independently_by_node() {
        let mut engine = StyleEngine::new();
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
                        animated_overlay: HostShared::null(),
                        animation_overlay_payloads: &[],
                        longhand_table: HostShared::null(),
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
                    owes_a_transition_step: false,
                    detached_composition: None,
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

        engine
            .state
            .abandon_engine_computed_record(third, &mut scratch, &mut engine.counters);
        assert_eq!(engine.computed_group_sets.assigned_style_record(third), None);
        assert_eq!(engine.computed_group_sets.pseudo_style_record(third, 0), None);
        assert!(engine.engine_computed_records_pending.is_empty());

        // A later batch can use the same node, and discarding it leaves installed nodes alone.
        publish(&mut engine, third, u8::MAX);
        publish(&mut engine, third, 0);
        engine.state.discard_engine_computed_records(&mut engine.counters);
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

    #[test]
    fn a_record_the_host_computes_instead_releases_the_detached_composition() {
        let mut engine = StyleEngine::new();
        let mut raw_node = [0];
        engine.allocate_style_nodes(&mut raw_node);
        let node = StyleNodeID::from_raw(raw_node[0]).unwrap();
        let target = computed::ComputedStyleTarget::new(node, u8::MAX);
        let animated_overlay = crate::css::animated_overlay::AnimatedOverlay::default();
        let composition = engine
            .publish_computed_groups(
                target,
                &[],
                0,
                0,
                computed::ComputedMetadataInput {
                    pseudo_element_styles: 0,
                    dependency_flags: 0,
                    counter_style_environment_identity: 0,
                    animation_overlay_identity: 1,
                    animated_overlay: HostShared::new(std::ptr::from_ref(&animated_overlay)),
                    animation_overlay_payloads: &[],
                    longhand_table: HostShared::null(),
                },
            )
            .style_record_identity;
        let detached = engine.computed_group_sets.detach_composition(node);
        assert!(detached.is_some());
        assert!(
            engine
                .computed_group_sets
                .underlying_style_record(composition)
                .is_some()
        );
        engine
            .engine_computed_records_pending
            .entry(node)
            .or_default()
            .push(PendingEngineComputedRecord {
                node,
                pseudo_kind: u8::MAX,
                old_style_record: composition,
                new_style_record: computed::FinalStyleRecordID::NONE,
                cascade_state: None,
                longhand_evaluations: 0,
                owes_a_transition_step: true,
                detached_composition: detached,
            });

        engine.forget_engine_computed_record(target);
        assert!(engine.engine_computed_records_pending.is_empty());
        assert!(
            engine
                .computed_group_sets
                .underlying_style_record(composition)
                .is_none()
        );
    }

    #[test]
    fn a_pseudo_element_composition_detaches_and_reattaches() {
        let mut engine = StyleEngine::new();
        let mut raw_node = [0];
        engine.allocate_style_nodes(&mut raw_node);
        let node = StyleNodeID::from_raw(raw_node[0]).unwrap();
        let target = computed::ComputedStyleTarget::new(node, pseudo_kind::BEFORE);
        let animated_overlay = crate::css::animated_overlay::AnimatedOverlay::default();
        let metadata = |animated_overlay: HostShared<crate::css::animated_overlay::AnimatedOverlay>| {
            computed::ComputedMetadataInput {
                pseudo_element_styles: 0,
                dependency_flags: 0,
                counter_style_environment_identity: 0,
                animation_overlay_identity: u64::from(!animated_overlay.is_null()),
                animated_overlay,
                animation_overlay_payloads: &[],
                longhand_table: HostShared::null(),
            }
        };
        let composition = engine
            .publish_computed_groups(
                target,
                &[],
                0,
                0,
                metadata(HostShared::new(std::ptr::from_ref(&animated_overlay))),
            )
            .style_record_identity;

        // A record settled beneath the composition takes its place, while the composition stays
        // live for the host that holds it.
        let detached = engine.computed_group_sets.detach_composition_of(target).unwrap();
        let settled = engine
            .publish_computed_groups(target, &[], 0, 0, metadata(HostShared::null()))
            .style_record_identity;
        assert_eq!(settled, detached.base);
        assert!(
            engine
                .computed_group_sets
                .underlying_style_record(composition)
                .is_some()
        );

        // The host never installed it: the composition is laid over the pseudo-element's record
        // again.
        engine.computed_group_sets.reattach_composition_of(target, detached);
        assert_eq!(
            engine
                .computed_group_sets
                .pseudo_style_record(node, pseudo_kind::BEFORE),
            Some(composition)
        );
    }
}

impl StyleEngineState {
    pub(crate) fn end_style_record_view_epoch(&mut self, counters: &mut Counters) {
        self.retained.computed_group_sets.end_style_record_view_epoch();
        self.reclaim_computed_memory_if_needed(counters);
    }
}

impl StyleEngineState {
    pub(super) fn refill_font_request(
        &mut self,
        node: StyleNodeID,
        request: font_resolution::FontRequest,
        counters: &mut Counters,
    ) {
        counters.bump(Counter::FontRefillRounds);
        counters.bump(Counter::FontResolutionRequests);
        // NB: Use resident selector-tree depth for this diagnostic. It is not a flat-tree
        //     dependency-span proof and must not buy an ancestor traversal just for counting.
        counters.set(
            Counter::FontRefillBlockedDepth,
            counters
                .get(Counter::FontRefillBlockedDepth)
                .max(u64::from(self.tree.depth(node)) + 1),
        );
        let resolver = self.host.font_resolver.as_ref().expect("a request has a font resolver");
        let resolutions = self
            .retained
            .font_resolution
            .as_mut()
            .expect("a request has a font resolution cache");
        resolver.refill(resolutions, request);
    }
}

impl StyleEngineState {
    /// Establish the document element's font input before the consumer pass. The root's
    /// remaining properties and pseudos complete in their normal canonical position.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn prepare_root_font_inputs(
        &mut self,
        node: StyleNodeID,
        cascade_winners_are_complete: bool,
        exact_flipped_rules: Option<FlippedRules>,
        parent_inputs_moved: ParentInputsMoved,
        full_drive_reason: Option<FullDriveReason>,
        scratch: &mut EngineComputedRecordScratch,
        counters: &mut Counters,
    ) {
        let inputs = self.document_style_computation_inputs;
        scratch.root_element_inputs = Some((node, RootFontInputs::from_document(&inputs)));
        let probe = |state: &mut Self, scratch: &mut EngineComputedRecordScratch, counters: &mut Counters| {
            state.engine_computed_element_record_delta(
                node,
                cascade_winners_are_complete,
                exact_flipped_rules,
                parent_inputs_moved,
                full_drive_reason,
                scratch,
                FontDriveGoal::RootInputs,
                counters,
            )
        };
        let mut answer = probe(self, scratch, counters);
        if let Err(Unanswered::Suspended(Suspension::Font)) = answer {
            // NB: A root font miss completes at this preparation boundary. Consumers need
            //     current metrics even when their first records install in this same pass.
            let request = scratch.font_drive.take_suspended_request();
            self.refill_font_request(node, request, counters);
            answer = probe(self, scratch, counters);
        }
        // An unproven or refused probe keeps the host's root-metric route.
        let prepared = match answer {
            Ok(ElementAnswer::RootInputs(prepared)) => prepared,
            Ok(ElementAnswer::Delta(_)) | Err(_) => None,
        };
        if let Some(root_inputs) = prepared {
            scratch.root_font_inputs_changed = RootFontInputs::from_document(&inputs) != root_inputs;
            root_inputs.apply_to(&mut self.document_style_computation_inputs);
            counters.bump(Counter::RootFontInputsPrepared);
        } else {
            // NB: Preserve the current host root-metric route. Unproven font inputs do not
            //     make every descendant wait for the host.
            counters.bump(Counter::RootFontInputsUnprovenFallbacks);
        }
        if scratch.font_drive.is_pending_for(node) {
            scratch.prepared_root_font = Some((node, parent_inputs_moved, std::mem::take(&mut scratch.font_drive)));
        }
        self.apply_substitution_effects(scratch);
    }
}

impl EngineComputedRecordScratch {
    /// The scratch of the next wave of a style pass, which drives its rows under what the pass
    /// moved under every row. What it cached of the rows the host installed since is gone.
    pub(super) fn for_next_wave(&self) -> Self {
        Self {
            document_environment_moved: self.document_environment_moved,
            viewport_moved: self.viewport_moved,
            root_font_inputs_changed: self.root_font_inputs_changed,
            host_applies_animation_plans: true,
            ..Self::default()
        }
    }
}

/// A walk's scratch is movable to the worker that owns it for the length of that walk, and comes
/// back at the join. Nothing in it is shared while the walk runs, so `Send` is the whole bound:
/// the values a half-built record carries are borrowed through `HostShared`, and a frozen
/// `ComputedLonghandTable` fills its lazy memos atomically.
const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<EngineComputedRecordScratch>();
};
