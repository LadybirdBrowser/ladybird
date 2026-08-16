/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

impl StyleEngine {
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
        self.counters.bump(Counter::ComputedReconstructionMetadataReused);
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
        if publication.computed_reconstruction_metadata_node_handle_changed {
            self.counters
                .bump(Counter::ComputedReconstructionMetadataNodeHandlesPublished);
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

    pub(crate) fn style_record_view(&self, style_record: u64) -> Option<computed::StyleRecordView<'_>> {
        self.computed_group_sets.style_record_view(style_record)
    }

    pub(crate) fn pin_style_record(&mut self, style_record: u64) {
        self.computed_group_sets.pin_style_record(style_record);
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
        self.computed_reconstruction_metadata_memory.resize_required_to(
            &mut self.memory,
            self.computed_group_sets.reconstruction_header_capacity_bytes(),
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
        self.settle_computed_memory();
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
        if !publication.new_computed_reconstruction_metadata {
            self.counters.bump(Counter::ComputedReconstructionMetadataReused);
        }
        if publication.computed_reconstruction_metadata_node_handle_changed {
            self.counters
                .bump(Counter::ComputedReconstructionMetadataNodeHandlesPublished);
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

    pub(crate) fn publish_exact_cascade_state(
        &mut self,
        target: computed::ComputedStyleTarget,
        store: &CascadedPropertyStore,
        inherited_style_groups: u8,
    ) -> (bridge::FfiExactCascadePublication, Vec<(u16, SpecifiedWinnerKey)>, bool) {
        let context = self.prepare_exact_cascade_publication(target);
        let had_previous = context.0.is_some();
        let exact_winners = store
            .winning_declarations()
            .map(|(property, value_pointer, origin)| {
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

        let has_logical_counterpart = |property| {
            (0..4).any(|writing_mode| {
                (0..3).any(|direction| {
                    crate::css::style_compute::map_logical_alias_to_physical(property, writing_mode, direction)
                        != property
                        || crate::css::style_compute::map_physical_to_logical_alias(property, writing_mode, direction)
                            != property
                })
            })
        };
        let mut assignments = Vec::new();
        for winner in self.winner_groups.winners_in_state(state) {
            let Some(winner) = self.winner_groups.resolved_winner(winner) else {
                continue;
            };
            if winner.property > crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID
                || (target.is_pseudo() && winner.property == crate::css::property_metadata::property_id::CONTENT)
                || (target.is_pseudo()
                    && !crate::css::property_metadata::pseudo_element_supports_property(
                        target.pseudo_kind(),
                        winner.property,
                    ))
                || (crate::css::property_metadata::property_id::ANIMATION_COMPOSITION
                    ..=crate::css::property_metadata::property_id::ANIMATION_TIMING_FUNCTION)
                    .contains(&winner.property)
                || (crate::css::property_metadata::property_id::TRANSITION_BEHAVIOR
                    ..=crate::css::property_metadata::property_id::TRANSITION_TIMING_FUNCTION)
                    .contains(&winner.property)
                || has_logical_counterpart(winner.property)
            {
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
                declaration.property_id == winner.property && declaration.important == winner.priority.is_important()
            });
            let Some(declaration) = source_declarations.next() else {
                continue;
            };
            if source_declarations.next().is_some()
                || !matches!(
                    unsafe { self.specified_values.identity_of(declaration.data.cast()) },
                    Lookup::Known(value) if value == winner.key.value
                )
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
                winner.priority.is_important(),
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
    ) -> (bridge::FfiExactCascadePublication, bool) {
        let context = self.prepare_exact_cascade_publication(target);
        let had_previous = context.0.is_some();
        (
            self.publish_exact_cascade_winners_with_context(target, exact_winners, inherited_style_groups, context),
            had_previous,
        )
    }

    pub(super) fn prepare_exact_cascade_publication(
        &mut self,
        target: computed::ComputedStyleTarget,
    ) -> (Option<CascadeStateID>, Option<CascadeStateID>) {
        let generation = self.winner_groups.generation();
        let previous =
            self.computed_group_sets
                .cascade_state(target)
                .and_then(|(previous_generation, previous_state)| {
                    (previous_generation == generation).then_some(previous_state)
                });
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
        (previous, lower_bound_state)
    }

    pub(super) fn publish_exact_cascade_winners_with_context(
        &mut self,
        target: computed::ComputedStyleTarget,
        exact_winners: &[(u16, SpecifiedWinnerKey)],
        inherited_style_groups: u8,
        (previous, lower_bound_state): (Option<CascadeStateID>, Option<CascadeStateID>),
    ) -> bridge::FfiExactCascadePublication {
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
                key,
                priority: CascadePriority::exact_output_placeholder(),
                source: WinnerSource::ExactCascade,
            }));
        }
        verify_cascade_winners(|| {
            let Some(lower_bound_state) = lower_bound_state else {
                return;
            };
            if !self
                .published_match_answers
                .lookup(target.node())
                .is_some_and(|answer| answer.cascade_winners_are_complete)
            {
                return;
            }
            let retained = Rc::clone(
                self.retained_match_answer(target.node())
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
                    self.programs
                        .get(matched.program)
                        .entries()
                        .get(matched.entry as usize)
                        .is_some_and(|entry| entry.pseudo_element == target.pseudo_element_target())
                }) {
                    self.program
                        .declared_properties_of(matched.rule)
                        .iter()
                        .for_each(&mut inspect);
                }
                if !target.is_pseudo() {
                    for kind in ElementDeclarationKind::ALL {
                        self.facts
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
        if !self.winner_groups.settle_memory(&mut self.memory) {
            return bridge::FfiExactCascadePublication {
                computed_group_mask: u32::MAX,
                computed_property_word_0: u64::MAX,
                computed_property_word_1: u64::MAX,
                computed_property_word_2: u64::MAX,
                computed_property_word_3: u64::MAX,
                computed_property_word_4: u64::MAX,
                computed_property_word_5: u64::MAX,
                computed_property_closure_is_exact: false,
                unchanged: false,
            };
        }
        let generation = self.winner_groups.generation();
        let delta = self.winner_groups.semantic_delta(previous, state);
        let unchanged = previous.is_some() && delta.is_empty();
        let mut computed_property_words = [0u64; 6];
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
            .then(|| self.computed_group_sets.current_color_dependency_mask(target));
        let current_color_dependency_properties = delta
            .properties()
            .contains(&crate::css::property_metadata::property_id::COLOR)
            .then(|| self.computed_group_sets.current_color_dependency_properties(target));
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
            .then(|| self.computed_group_sets.color_scheme_dependency_mask(target));
        let color_scheme_dependency_properties = delta
            .properties()
            .contains(&crate::css::property_metadata::property_id::COLOR_SCHEME)
            .then(|| self.computed_group_sets.color_scheme_dependency_properties(target));
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
                .then(|| self.computed_group_sets.font_dependency_mask(target))
        });
        let font_dependency_properties = font_group_mask.and_then(|font_group_mask| {
            delta
                .properties()
                .iter()
                .copied()
                .any(|property| computed_group_output_mask(property) == Some(font_group_mask))
                .then(|| self.computed_group_sets.font_dependency_properties(target))
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
        let inherited_current_color_dependency_mask = (inherited_property_closure_requested
            && inherited_style_groups & INHERITED_TEXT_GROUP != 0)
            .then(|| self.computed_group_sets.current_color_dependency_mask(target));
        let inherited_current_color_dependency_properties = (inherited_property_closure_requested
            && inherited_style_groups & INHERITED_TEXT_GROUP != 0)
            .then(|| self.computed_group_sets.current_color_dependency_properties(target));
        let inherited_color_scheme_dependency_mask = (inherited_property_closure_requested
            && inherited_style_groups & INHERITED_UI_GROUP != 0)
            .then(|| self.computed_group_sets.color_scheme_dependency_mask(target));
        let inherited_color_scheme_dependency_properties = (inherited_property_closure_requested
            && inherited_style_groups & INHERITED_UI_GROUP != 0)
            .then(|| self.computed_group_sets.color_scheme_dependency_properties(target));
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
        bridge::FfiExactCascadePublication {
            unchanged,
            computed_group_mask,
            computed_property_word_0: computed_property_words[0],
            computed_property_word_1: computed_property_words[1],
            computed_property_word_2: computed_property_words[2],
            computed_property_word_3: computed_property_words[3],
            computed_property_word_4: computed_property_words[4],
            computed_property_word_5: computed_property_words[5],
            computed_property_closure_is_exact,
        }
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
    }

    pub(crate) fn remove_computed_pseudo(
        &mut self,
        node: StyleNodeID,
        pseudo_kind: u8,
    ) -> Option<computed::FinalStyleRecordID> {
        let target = computed::ComputedStyleTarget::new(node, pseudo_kind);
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
