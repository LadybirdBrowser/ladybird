/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

mod demand;
mod pseudo;

use super::pipeline::compute::RootFontInputs;
use super::*;
use crate::css::cascaded_properties::NO_PSEUDO_ELEMENT;
use crate::css::computed_longhand_table::IN_DISPLAY_NONE_SUBTREE;
use crate::css::computed_value_views::QuoteTransfer;
use animations::DeclarationScope;
pub(crate) use demand::{RecordDemand, RecordDemandAnswer};

/// What a subtree does to the generated-content state that the boxes after it read.
#[derive(Clone, Copy, Default)]
pub(crate) struct SubtreeGeneratedContentEffects {
    /// A `counter-reset` names a counter. The counter it creates reaches only the following siblings of the element
    /// that resets it, and their descendants.
    pub(crate) resets_counters: bool,
    /// A `counter-increment` or `counter-set` names a counter, or a list item below the root increments `list-item`.
    /// The counter can be one that an ancestor or an earlier box created, and that boxes outside the subtree read. An
    /// inserted `<li>` itself has the DOM renumber the list it joins.
    pub(crate) increments_or_sets_counters: bool,
    /// An element's own quotes leave the quote depth after it other than they found it: its `::before` and `::after`
    /// together do, or its `::marker` or its own `content` names a quote. The walk stops once it finds one.
    pub(crate) has_element_with_unbalanced_quotes: bool,
}

impl RetainedState {
    /// The record a highlight pseudo-element inherits from: the same pseudo-element's record on its
    /// nearest flat-tree ancestor holding one, as the engine has it assigned.
    pub(in crate::css::style) fn retained_highlight_inheritance_parent_style_record(
        &self,
        node: StyleNodeID,
        target: pipeline::PseudoElementTarget,
    ) -> Option<computed::FinalStyleRecordID> {
        let mut ancestor = self.projection.inheritance_parent(node);
        while let Some(candidate) = ancestor {
            if let Some(record) = self.installed_pseudo_style_record(candidate, target) {
                return Some(record);
            }
            ancestor = self.projection.inheritance_parent(candidate);
        }
        None
    }

    pub(in crate::css::style) fn installed_pseudo_style_record(
        &self,
        node: StyleNodeID,
        target: pipeline::PseudoElementTarget,
    ) -> Option<computed::FinalStyleRecordID> {
        self.computed_group_sets.pseudo_style_record_of(node, target)
    }

    #[cfg(test)]
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

    /// The identity of the counter-style registry of the style scope of `target`, the values a
    /// group build of whose style reads `effective_value` answers, where the style reads one.
    pub(super) fn values_counter_style_environment_identity<'v>(
        &self,
        target: computed::ComputedStyleTarget,
        effective_value: impl Fn(u16) -> &'v crate::css::style_value::RetainedStyleValueData,
    ) -> u64 {
        use crate::css::property_metadata::property_id as prop;
        let value = |property| effective_value(property).optional_data();
        if !crate::css::style_compute::style_reads_counter_style_environment(
            value(prop::CONTENT),
            value(prop::LIST_STYLE_TYPE),
            target.is_pseudo(),
        ) {
            return 0;
        }
        self.counter_style_environment_identity_of(target.node())
    }

    /// The identity of the counter-style registry `node`'s style scope has.
    pub(super) fn counter_style_environment_identity_of(&self, node: StyleNodeID) -> u64 {
        self.counter_style_environment_identities
            .get(&self.counter_style_scope(node))
            .copied()
            .unwrap_or(0)
    }

    /// The style scope whose counter-style registry a node's records name. An element of a shadow
    /// tree built from the document's style sheets has the document's style scope.
    fn counter_style_scope(&self, node: StyleNodeID) -> TreeScopeID {
        let tree_scope = self.projection.tree_scope(node);
        if self.rules.scope_uses_document_sheets(tree_scope) {
            TreeScopeID::DOCUMENT
        } else {
            tree_scope
        }
    }

    /// The scope the winning `animation-name` of `record`, the record the engine installed for an element, or its
    /// pseudo-element `pseudo_kind`, was declared in, or `None` where it installed none. It is read off the record
    /// because a record computed on demand is held by no pass.
    pub(crate) fn animation_name_declaration_scope_of(
        &self,
        node: StyleNodeID,
        pseudo_kind: u8,
        record: &super::pipeline::Record,
    ) -> Option<DeclarationScope> {
        self.computed_group_sets
            .holds_an_installed_record(computed::ComputedStyleTarget::new(node, pseudo_kind))
            .then(|| record.rare().animation_name_scope)
    }

    /// Whether the host owes an element the engine settled the animation plan its record decides,
    /// once it installs the record `new`: the record moved the declarations, or the element runs
    /// CSS animations the plan would change, as where their `@keyframes` moved without the
    /// declarations.
    pub(super) fn row_owes_an_animation_plan(
        &self,
        node: StyleNodeID,
        old: u64,
        new: u64,
        new_record: &std::sync::Arc<super::pipeline::Record>,
    ) -> bool {
        // An element the host holds no record for, its style cleared under display:none and its
        // animations with it, starts the animations its record names as a first record does.
        let old = if self.held_style_records.contains(node) { old } else { 0 };
        self.record_owes_an_animation_plan(old, new, || self.record_declares_animations(new))
            || (self.css_defined_animations.node_runs_a_css_animation(node)
                && self
                    .animation_plan_of_record(node, NO_PSEUDO_ELEMENT, new_record)
                    .is_some())
    }

    /// The animation plan `record` decides for an element, or for its pseudo-element `pseudo_kind`,
    /// where applying it would change the animations they run: none where it would not, or where
    /// the record installs no `animation-name`. The definitions borrow the record's values.
    pub(crate) fn animation_plan_of_record(
        &self,
        node: StyleNodeID,
        pseudo_kind: u8,
        record: &std::sync::Arc<super::pipeline::Record>,
    ) -> Option<smallvec::SmallVec<[crate::css::style_compute::FfiComputedAnimation; 1]>> {
        let longhands = StyleRecordView::of_host_record(record).longhands();
        let declaration_scope = self.animation_name_declaration_scope_of(node, pseudo_kind, record)?;
        let existing_animations =
            self.element_css_defined_animations(node, crate::css::style_compute::animation_slot(pseudo_kind));
        let element_tree_scope = self.element_tree_scope(node);
        let mut definitions = smallvec::SmallVec::new();
        crate::css::style_compute::build_computed_animation_list(
            longhands,
            existing_animations,
            |name| {
                self.animation_keyframes()
                    .resolve(declaration_scope, element_tree_scope, name)
            },
            &mut definitions,
        );
        (!animations::plan_changes_nothing(&definitions, existing_animations)).then_some(definitions)
    }

    /// Whether installing the `new` record the engine derived for an element, in place of the
    /// `old` one the host holds, owes the host the animation plan the new record decides: a first
    /// record that names an animation, a record that moves the declarations, or one that names an
    /// animation and leaves a display:none subtree, which starts the animations it names.
    /// `new_declares_animations` answers whether the new record names an animation.
    pub(super) fn record_owes_an_animation_plan(
        &self,
        old: u64,
        new: u64,
        new_declares_animations: impl Fn() -> bool,
    ) -> bool {
        match old {
            0 => new_declares_animations(),
            old => {
                self.animation_declarations_moved(old, new)
                    || (self.record_is_in_display_none_subtree(old)
                        && !self.record_is_in_display_none_subtree(new)
                        && new_declares_animations())
            }
        }
    }

    /// Whether a record's `animation-name` names any animation.
    pub(super) fn record_declares_animations(&self, record: u64) -> bool {
        self.computed_group_sets
            .style_record_view(record)
            .is_some_and(|view| view.longhands().declares_css_animations())
    }

    /// Whether a record's element is display:none or inherits from one that is.
    pub(super) fn record_is_in_display_none_subtree(&self, record: u64) -> bool {
        self.computed_group_sets
            .style_record_view(record)
            .is_some_and(|view| view.dependency_flags & IN_DISPLAY_NONE_SUBTREE != 0)
    }

    /// Whether moving an element from the `old` record to the `new` one moves the `animation-*`
    /// longhands declaring its CSS animations, or the timelines they name. Every such longhand is
    /// in the animation group, beside the `transition-*` ones: a group that moved with only
    /// transitions in it decides a plan that changes nothing.
    fn animation_declarations_moved(&self, old: u64, new: u64) -> bool {
        use crate::css::computed_value_types::{AnimationValues, ComputedStyleValueHandle};
        use crate::css::table_group_builder::group_index::ANIMATION;
        if old == new {
            return false;
        }
        let animation_group = |record| {
            self.computed_group_sets
                .style_record_payloads(record)
                .map(|payloads| payloads[ANIMATION].as_ptr())
        };
        let (old_group, new_group) = (animation_group(old), animation_group(new));
        if old_group == new_group {
            return false;
        }
        let (Some(old_group), Some(new_group)) = (old_group, new_group) else {
            return true;
        };
        // SAFETY: A live record's animation group payload is an `AnimationValues`.
        let (old_values, new_values) = unsafe {
            (
                &*old_group.cast::<AnimationValues>(),
                &*new_group.cast::<AnimationValues>(),
            )
        };
        fn declarations(values: &AnimationValues) -> [&ComputedStyleValueHandle; 16] {
            [
                &values.animation_name,
                &values.animation_composition,
                &values.animation_delay,
                &values.animation_direction,
                &values.animation_duration,
                &values.animation_fill_mode,
                &values.animation_iteration_count,
                &values.animation_play_state,
                &values.animation_timeline,
                &values.animation_timing_function,
                &values.scroll_timeline_name,
                &values.scroll_timeline_axis,
                &values.timeline_scope,
                &values.view_timeline_name,
                &values.view_timeline_axis,
                &values.view_timeline_inset,
            ]
        }
        declarations(old_values) != declarations(new_values)
    }

    /// Whether moving an element from the `old` record to the `new` one moves the `transition-*`
    /// longhands declaring its transitions.
    pub(super) fn transition_declarations_moved(&self, old: u64, new: u64) -> bool {
        use crate::css::computed_value_types::{AnimationValues, ComputedStyleValueHandle};
        use crate::css::table_group_builder::group_index::ANIMATION;
        if old == new {
            return false;
        }
        let animation_group = |record| {
            self.computed_group_sets
                .style_record_payloads(record)
                .map(|payloads| payloads[ANIMATION].as_ptr())
        };
        let (old_group, new_group) = (animation_group(old), animation_group(new));
        if old_group == new_group {
            return false;
        }
        let (Some(old_group), Some(new_group)) = (old_group, new_group) else {
            return true;
        };
        // SAFETY: A live record's animation group payload is an `AnimationValues`.
        let (old_values, new_values) = unsafe {
            (
                &*old_group.cast::<AnimationValues>(),
                &*new_group.cast::<AnimationValues>(),
            )
        };
        fn declarations(values: &AnimationValues) -> [&ComputedStyleValueHandle; 5] {
            [
                &values.transition_property,
                &values.transition_duration,
                &values.transition_timing_function,
                &values.transition_delay,
                &values.transition_behavior,
            ]
        }
        declarations(old_values) != declarations(new_values)
    }

    /// Whether the host composes animations over a record, which the engine cannot drive from: a
    /// record it cannot read counts as one.
    pub(super) fn record_holds_an_animation_overlay(&self, record: computed::FinalStyleRecordID) -> bool {
        self.computed_group_sets
            .style_record_view(record.raw())
            .is_none_or(|view| view.animated_overlay.is_some())
    }

    /// Whether a record's transition declarations name a longhand a change of which starts a
    /// transition.
    pub(super) fn record_declares_transitions(&self, record: computed::FinalStyleRecordID) -> bool {
        self.computed_group_sets
            .style_record_view(record.raw())
            .is_some_and(|view| view.longhands().declares_transitions())
    }

    /// Whether a row moving `node` off `underlying_style_record`, the record beneath any
    /// composition, owes the host the transition step: the host holds a style to start transitions
    /// from, and the record declares transitions or the row moves their declarations. The host runs
    /// the step once it installs the new record, against the record the row moved away from, and
    /// composes the record before anything inherits from it, so its children and pseudo-elements
    /// read the values the step started from.
    pub(super) fn decide_transition_step(
        &self,
        node: StyleNodeID,
        underlying_style_record: computed::FinalStyleRecordID,
        moves_transition_declarations: bool,
    ) -> bool {
        // An element whose style the host cleared on entering display:none, or never computed, has
        // no before-change style.
        self.held_style_records.contains(node)
            && (moves_transition_declarations || self.record_declares_transitions(underlying_style_record))
    }

    pub(super) fn root_font_inputs_from_record(&self, record: computed::FinalStyleRecordID) -> Option<RootFontInputs> {
        let view = self.computed_group_sets.style_record_view(record.raw())?;
        Some(RootFontInputs::from_payloads(view.payloads, view.dependency_flags))
    }

    /// The host boundary publishes root inputs after materializing a document element.
    /// Cache entries name these inputs; publication does not clear them as a side effect.
    pub(super) fn prepare_root_font_metrics_from_record(&mut self, record: computed::FinalStyleRecordID) {
        let Some(root_inputs) = self.root_font_inputs_from_record(record) else {
            return;
        };
        root_inputs.apply_to(&mut self.document_style_computation_inputs);
    }

    /// What the node and its DOM descendants do to the two pieces of generated-content state that run
    /// along the whole tree rather than staying inside one box, themselves or through a `::before`,
    /// `::after` or `::marker`. Inserting such a subtree can change what the boxes after it show.
    #[must_use]
    pub(crate) fn subtree_generated_content_effects(&self, root: StyleNodeID) -> SubtreeGeneratedContentEffects {
        let mut effects = SubtreeGeneratedContentEffects::default();
        let mut pending = vec![root];
        while let Some(node) = pending.pop() {
            if node.element_index().is_some() {
                let records = [
                    self.computed_group_sets.assigned_style_record(node),
                    self.computed_group_sets.pseudo_style_record(node, pseudo_kind::BEFORE),
                    self.computed_group_sets.pseudo_style_record(node, pseudo_kind::AFTER),
                    self.computed_group_sets.pseudo_style_record(node, pseudo_kind::MARKER),
                ];
                for (index, record) in records.into_iter().enumerate() {
                    let Some(view) = self.published_style_record_view(record) else {
                        continue;
                    };
                    let is_root_element = index == 0 && node == root;
                    effects.resets_counters |= !view.counter_reset().is_empty();
                    effects.increments_or_sets_counters |= !view.counter_increment().is_empty()
                        || !view.counter_set().is_empty()
                        || (view.display().is_list_item() && !is_root_element);
                }
                let [element_record, before_record, after_record, marker_record] = records;
                let names_quotes = |record| {
                    self.published_style_record_view(record)
                        .is_some_and(|view| view.content_opens_or_closes_quotes())
                };
                let generated_quote_transfer = |record| {
                    self.published_style_record_view(record)
                        .filter(|view| !view.display().is_none())
                        .map_or(QuoteTransfer::IDENTITY, |view| view.content_quote_transfer())
                };
                if names_quotes(element_record)
                    || names_quotes(marker_record)
                    || !generated_quote_transfer(before_record)
                        .then(generated_quote_transfer(after_record))
                        .is_identity()
                {
                    effects.has_element_with_unbalanced_quotes = true;
                    return effects;
                }
            }
            pending.extend(self.tree.dom_children(node));
        }
        effects
    }

    /// Whether the node's published style holds a `::first-letter` record. The tree build asks a
    /// block this before it goes looking for the letter to style, and again of each block it
    /// descends into, which stops the search where a nested block styles its own first letter.
    #[must_use]
    pub(crate) fn has_published_first_letter_style(&self, node: StyleNodeID) -> bool {
        self.published_pseudo_record(node, pseudo_kind::FIRST_LETTER).is_some()
    }

    /// Whether publishing sampled custom properties requires another style computation.
    /// The sample has already resolved the target's custom declarations that read animated values,
    /// which `moved` holds with the animated ones.
    pub(crate) fn animated_custom_properties_trigger_restyle(
        &self,
        node: StyleNodeID,
        pseudo: Option<u8>,
        moved: &[(
            crate::css::retained_fly_string::RetainedUtf16FlyString,
            RetainedStyleValueData,
        )],
        has_non_custom_property_animation: bool,
        uncached_interval_values_may_need_substitution: bool,
        host_custom_property_registration_generation: u64,
    ) -> bool {
        // Animations of non-custom properties may read the sampled custom properties or change the font context
        // their registered readers compute against.
        if has_non_custom_property_animation {
            return true;
        }

        let has_unresolved_keyframe_easing = self
            .element_animation_effects(node, pseudo.map_or(0, |kind| kind + 1))
            .iter()
            .any(|effect| {
                effect.keyframes().iter().any(|keyframe| {
                    keyframe
                        .easing_value
                        .as_ref()
                        .is_some_and(|value| matches!(value.data(), StyleValueData::Unresolved { .. }))
                })
            });

        // Unresolved keyframe easing may read the sampled custom properties.
        if has_unresolved_keyframe_easing {
            return true;
        }

        // Retained discrete intervals keep their substituted values. Fresh intervals may need
        // another sample against the custom properties this sample publishes.
        if uncached_interval_values_may_need_substitution {
            return true;
        }

        // Animation sampling precedes the transaction that consumes pending registration changes.
        // In that case, the sample used the engine's outdated registrations and underlying values.
        if host_custom_property_registration_generation != self.custom_property_registration_generation() {
            return true;
        }

        let target = computed::ComputedStyleTarget::new(node, pseudo.unwrap_or(u8::MAX));

        // Without an assigned style record, we cannot establish that sampling covers all consumers.
        let Some(record) = self.computed_group_sets.assigned_final_style_record(target) else {
            return true;
        };

        // The record must still be available so we can inspect the target's current style.
        let Some(view) = self.computed_group_sets.style_record_view(record.raw()) else {
            return true;
        };

        if pseudo.is_none() && self.has_pseudo_styles_to_recompute(node, &view) {
            return true;
        }

        // The engine keeps the custom properties the longhands of each record it holds substitute.
        let Some(live) = self.live_style.as_ref() else {
            return true;
        };
        live.custom_property_reads_of(node, pseudo).is_none_or(|reads| {
            reads.read_any_of(|read| moved.iter().any(|(name, _)| name.to_utf16().as_ref() == read))
        })
    }

    /// Whether pseudo styles need updating when the element's custom properties change.
    fn has_pseudo_styles_to_recompute(&self, node: StyleNodeID, view: &computed::StyleRecordView<'_>) -> bool {
        // Existing pseudo-element styles may inherit the sampled values and need to be recomputed.
        if self.computed_group_sets.assigned_pseudo_kinds(node).next().is_some() {
            return true;
        }

        // Ignore rules for pseudo-elements that generate no box.
        let pseudo_kinds = view.pseudo_element_styles;
        let marker_is_live = view.longhands().display_is_list_item();
        self.filter_non_generating_pseudo_kinds(node, pseudo_kinds, marker_is_live) != 0
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
        if !self.projection.is_live(node) {
            return None;
        }
        let Some(parent) = self
            .projection
            .parent(node)
            .filter(|&parent| self.projection.host_of(parent).is_none())
        else {
            return Some(SiblingPosition { count: 1, index: 1 });
        };
        // OPTIMIZATION: Each element of a run of many siblings asks for its place as a style pass reaches it, which
        //               counting the run every time would make quadratic: the places of a long run are kept until
        //               the tree's DOM order changes.
        let mut cache = self
            .sibling_positions
            .lock()
            .expect("the sibling position memo is never held across a panic");
        cache.forget_unless_at(self.tree.dom_order_version());
        if let Some(position) = cache.position_of(parent, node) {
            return Some(position);
        }
        let children: Vec<StyleNodeID> = self
            .tree
            .dom_children(parent)
            .filter(|child| child.text_index().is_none())
            .collect();
        let count = u32::try_from(children.len()).expect("sibling count space exhausted");
        if children.len() >= SiblingPositionCache::MIN_CHILDREN {
            cache.insert(parent, &children);
        }
        let index = children
            .iter()
            .position(|&child| child == node)
            .map_or(0, |index| index as u32 + 1);
        Some(SiblingPosition { count, index })
    }

    /// Install a record of `table`'s values, or of the initial ones where it has none, holding
    /// `payloads`, or the default ones where none are given, styling the pseudo-elements of the
    /// bits of `pseudo_element_styles`, and assign it to `target`.
    #[cfg(test)]
    pub(crate) fn install_test_record(
        &mut self,
        target: computed::ComputedStyleTarget,
        value_of: Option<&dyn Fn(u16) -> RetainedStyleValueData>,
        payloads: Option<&crate::css::computed_values::StyleGroups>,
        pseudo_element_styles: u64,
    ) -> computed::FinalStyleRecordID {
        let initial = |property| crate::css::style_compute::initial_value_handle(property).clone();
        let source = super::pipeline::Record::of_test_values(|property| {
            value_of.map_or_else(|| initial(property), |value_of| value_of(property))
        });
        let payloads = payloads
            .cloned()
            .unwrap_or_else(|| self.default_groups().groups().clone());
        let record = std::sync::Arc::new(super::pipeline::Record::derived(&source, &payloads))
            .with_pseudo_element_styles(super::pipeline::PseudoElementStyles::from_bits(pseudo_element_styles));
        let identity = self.computed_group_sets.install_derived_record(&record, 0, || {
            computed::record_dependency_flags(&record.facts(), &record.payloads)
        });
        match target.is_pseudo() {
            true => {
                self.computed_group_sets
                    .assign_installed_pseudo_record(target.node(), target.pseudo_kind(), identity)
            }
            false => self
                .computed_group_sets
                .assign_installed_record(target.node(), identity),
        }
        identity
    }

    pub(crate) fn style_record_payloads(&self, style_record: u64) -> Option<&crate::css::computed_values::StyleGroups> {
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

    /// The record the host holds for the live record `style_record`.
    pub(crate) fn host_record(&self, style_record: u64) -> Option<&std::sync::Arc<super::pipeline::Record>> {
        self.computed_group_sets.host_record(style_record)
    }

    pub(crate) fn take_released_animation_overlay_records(&mut self) -> computed::ReleasedAnimationOverlayRecords {
        self.computed_group_sets.take_released_animation_overlay_records()
    }

    pub(crate) fn pin_style_record(&mut self, style_record: u64) {
        self.computed_group_sets.pin_style_record(style_record);
    }

    pub(crate) fn has_animation_overlay_records(&self) -> bool {
        self.computed_group_sets.live_animation_overlay_records() != 0
            || self.computed_group_sets.retired_animation_overlay_records() != 0
    }

    pub(crate) fn lease_hover_frame_style_records(
        &mut self,
        nodes: crate::cow_column::ColumnSnapshot<
            crate::layout::node_data::PaintNode,
            { crate::layout::tree_shape::PUBLISHED_ROWS_PER_CHUNK },
        >,
    ) -> std::sync::Arc<computed::HoverFrameLease> {
        self.computed_group_sets.lease_hover_frame_style_records(nodes)
    }

    pub(crate) fn style_record_is_held(&self, record: u64) -> bool {
        self.computed_group_sets.style_record_is_held(record)
    }

    pub(crate) fn lease_style_records(&self) -> computed::StyleRecordLease {
        self.computed_group_sets.lease_style_records()
    }

    pub(crate) fn begin_style_record_view_epoch(&mut self) {
        self.style_record_view_epoch_depth = self
            .style_record_view_epoch_depth
            .checked_add(1)
            .expect("style-record view epoch depth overflow");
    }

    pub(crate) fn unpin_style_record(&mut self, style_record: u64) {
        self.computed_group_sets.unpin_style_record(style_record);
    }

    pub(crate) fn current_color_dependent_group_mask(longhands: ViewLonghands<'_>) -> Option<u32> {
        let dependencies = computed::ComputedGroupSets::current_color_dependency_mask(longhands);
        let caret_color_group = computed_group_output_mask(crate::css::property_metadata::property_id::CARET_COLOR)?;
        let accent_color_group = computed_group_output_mask(crate::css::property_metadata::property_id::ACCENT_COLOR)?;
        Some(dependencies | caret_color_group | accent_color_group)
    }

    pub(crate) fn remove_computed_pseudo(
        &mut self,
        node: StyleNodeID,
        pseudo_kind: u8,
    ) -> Option<computed::FinalStyleRecordID> {
        self.remove_computed_pseudo_of(node, pipeline::PseudoElementTarget::of_kind(pseudo_kind))
    }

    pub(in crate::css::style) fn remove_computed_pseudo_of(
        &mut self,
        node: StyleNodeID,
        target: pipeline::PseudoElementTarget,
    ) -> Option<computed::FinalStyleRecordID> {
        let live_animation_overlays_before = self.computed_group_sets.live_animation_overlay_records();
        let removed_style_record = self.computed_group_sets.remove_pseudo_of(node, target);
        let live_animation_overlays_after = self.computed_group_sets.live_animation_overlay_records();
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

impl StyleEngine {
    /// Frees what the engine kept for the leases of published frames, once none is held. The host calls it as it
    /// takes a recording in, so that a document that styles nothing more still frees what its recording kept.
    pub(crate) fn free_style_records_kept_for_leases(&mut self) {
        self.retained.computed_group_sets.free_retired_animation_overlays();
        self.retained.computed_group_sets.sweep_at_next_reclamation();
        self.reclaim_unreachable_computed_records();
    }

    /// Frees the records the samples of a clock tick let go of, as a style update does: a fork of the render state sees
    /// no style update for as long as a task keeps the host from one.
    pub(crate) fn free_style_records_after_tick(&mut self) {
        self.reclaim_unreachable_computed_records();
    }

    /// Frees the installed records nothing holds, where nothing views them, and once enough were freed reclaims the
    /// custom-property environments no record names any more. Answers whether it reclaimed them.
    pub(super) fn reclaim_unreachable_computed_records(&mut self) -> bool {
        let records_are_viewed = self.retained.style_record_view_epoch_depth != 0;
        if !self
            .retained
            .computed_group_sets
            .reclaim_unreachable_if_needed(records_are_viewed)
        {
            return false;
        }
        // An animation overlay is named by no record, only by the element or pseudo-element holding it.
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
            .chain(
                self.retained
                    .pseudo_element_custom_property_data
                    .values()
                    .flat_map(|environments| environments.iter().map(|(_, held)| held.identity)),
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
        payloads: Option<&crate::css::computed_values::StyleGroups>,
    ) -> Option<computed::AnimationOverlayUpdate> {
        // A pseudo-element inherits from its element.
        let parent = if target.is_pseudo() {
            Some(target.node())
        } else {
            self.retained.projection.inheritance_parent(target.node())
        };
        let parent_in_display_none_subtree = parent
            .and_then(|parent| self.retained.computed_group_sets.assigned_style_record(parent))
            .and_then(|record| {
                self.retained
                    .computed_group_sets
                    .style_record_dependency_flags(record.raw())
            })
            .is_some_and(|flags| flags & computed::IN_DISPLAY_NONE_SUBTREE != 0);
        // The children of an element style styles inherit the composition of its
        // animations, which they compute again where it moves.
        let held_composition = match target.is_pseudo() {
            true => None,
            false => self.hold_composition(target.node()),
        };
        let publication = self.retained.computed_group_sets.publish_animation_overlay(
            target,
            source_identity,
            animated_overlay,
            payloads,
            parent_in_display_none_subtree,
        )?;
        if publication.slot_allocated {
            self.retained.counters.bump(Counter::AnimationOverlaySlotsAllocated);
        }
        if publication.slot_released {
            self.retained.counters.bump(Counter::AnimationOverlaySlotsReleased);
        }
        if publication.record_updated {
            self.retained.counters.bump(Counter::AnimationOverlayRecordsUpdated);
        }
        self.retained
            .counters
            .set(Counter::LiveAnimationOverlayRecords, publication.live_records as u64);
        if let Some(held_composition) = held_composition {
            self.note_composition_moved(target.node(), held_composition);
        }
        Some(publication)
    }
}

/// The places of the element children of parents with many of them, as
/// [`RetainedState::sibling_position`] counts them, at one version of the tree's DOM order.
#[derive(Default)]
pub(super) struct SiblingPositionCache {
    dom_order_version: u64,
    /// How many element children each parent counted has.
    counts: super::fast_hash::FastMap<StyleNodeID, u32>,
    /// The place of each counted child among its parent's element children, counted from one.
    indices: super::fast_hash::FastMap<StyleNodeID, u32>,
}

impl SiblingPositionCache {
    /// How many element children a parent has before their places are kept: fewer are counted again
    /// as quickly as they are looked up.
    const MIN_CHILDREN: usize = 32;

    /// Forgets the places counted at a version of the DOM order other than `dom_order_version`.
    fn forget_unless_at(&mut self, dom_order_version: u64) {
        if self.dom_order_version != dom_order_version {
            self.counts.clear();
            self.indices.clear();
            self.dom_order_version = dom_order_version;
        }
    }

    fn position_of(&self, parent: StyleNodeID, node: StyleNodeID) -> Option<SiblingPosition> {
        Some(SiblingPosition {
            count: *self.counts.get(&parent)?,
            index: *self.indices.get(&node)?,
        })
    }

    fn insert(&mut self, parent: StyleNodeID, children: &[StyleNodeID]) {
        self.counts.insert(parent, children.len() as u32);
        for (index, &child) in children.iter().enumerate() {
            self.indices.insert(child, index as u32 + 1);
        }
    }
}

/// Where an element stands among its element siblings, counted from one: what `sibling-count()`
/// and `sibling-index()` read.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub(super) struct SiblingPosition {
    pub(super) count: u32,
    pub(super) index: u32,
}

/// What a demand for an element's record settles: the record with the groups it explicitly
/// inherits, and the pseudo-element records the engine settled beside it, one slot per synthetic
/// kind with a present bit each; a present slot holding zero is a removal.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct DemandedEngineRecord {
    pub(crate) style_record: u64,
    pub(crate) pseudo_records_present: u16,
    pub(crate) pseudo_records: [u64; bridge::PSEUDO_RECORD_SLOTS],
}

const _: () = assert!(pseudo_kind::SYNTHETIC_COUNT == bridge::PSEUDO_RECORD_SLOTS);

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
    pub(in crate::css::style) const SPELLING_ERROR: u8 = 9;
    pub(in crate::css::style) const SYNTHETIC_COUNT: usize = 11;
    pub(in crate::css::style) const HIGHLIGHT: u8 = crate::css::selector::PseudoElementType::Highlight as u8;

    /// https://drafts.csswg.org/css-pseudo-4/#highlight-pseudos
    pub(in crate::css::style) const fn is_highlight(kind: u8) -> bool {
        matches!(
            kind,
            SEARCH_TEXT | SEARCH_TEXT_CURRENT | SELECTION | SPELLING_ERROR | HIGHLIGHT
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn retained_highlight_inheritance_parent_uses_nearest_ancestor_pseudo_record() {
        let mut engine = StyleEngine::new();
        let mut raw_nodes = [0; 4];
        engine.allocate_style_nodes(&mut raw_nodes);
        let [root, parent, child, slot] = raw_nodes.map(|node| StyleNodeID::from_raw(node).unwrap());
        let projection = &mut engine.retained.projection;
        projection.set_parent(parent, Some(root));
        projection.set_parent(child, Some(parent));
        projection.set_assigned_slot(child, Some(slot));
        projection.set_parent(slot, Some(root));
        projection.commit();
        let publish = |engine: &mut StyleEngine, node, pseudo_kind| {
            engine.install_test_record(
                computed::ComputedStyleTarget::new(node, pseudo_kind),
                None,
                None,
                // NB: Told apart by node, so that each node's record is its own.
                u64::from(node.raw()),
            )
        };
        let root_record = publish(&mut engine, root, 0);
        assert_eq!(
            engine
                .retained_highlight_inheritance_parent_style_record(parent, pipeline::PseudoElementTarget::of_kind(0)),
            Some(root_record)
        );
        assert_eq!(
            engine.retained_highlight_inheritance_parent_style_record(child, pipeline::PseudoElementTarget::of_kind(0)),
            Some(root_record)
        );
        let slot_record = publish(&mut engine, slot, 0);
        assert_eq!(
            engine.retained_highlight_inheritance_parent_style_record(child, pipeline::PseudoElementTarget::of_kind(0)),
            Some(slot_record)
        );
    }
}

impl StyleEngine {
    pub(crate) fn end_style_record_view_epoch(&mut self) {
        assert!(
            self.retained.style_record_view_epoch_depth > 0,
            "style-record view epoch underflow"
        );
        self.retained.style_record_view_epoch_depth -= 1;
        self.reclaim_unreachable_computed_records();
    }
}

impl StyleEngine {
    pub(super) fn refill_font_request(&mut self, node: StyleNodeID, request: font_resolution::FontRequest) {
        let resolver = self.host.font_resolver.as_ref().expect("a request has a font resolver");
        self.retained.refill_font_request(resolver, node, request);
    }
}

impl RetainedState {
    /// The resolution of `request`: the one the cache holds, or, on a thread of a pass that resolves as it goes, one of
    /// the pass.
    pub(super) fn resolved_font(&self, request: bridge::FfiFontResolutionRequest) -> Option<bridge::FfiResolvedFont> {
        self.font_resolution
            .as_ref()
            .and_then(|resolutions| resolutions.lookup(request))
            .or_else(|| self.pass_fonts.as_ref().map(|pass_fonts| pass_fonts.resolve(request)))
    }

    /// Resolve `request` through `resolver` into the font resolution cache, between passes or the
    /// rounds of one, on the thread that runs them.
    pub(super) fn refill_font_request(
        &mut self,
        resolver: &font_resolution::FontResolverHost,
        node: StyleNodeID,
        request: font_resolution::FontRequest,
    ) {
        self.counters.bump(Counter::FontRefillRounds);
        self.counters.bump(Counter::FontResolutionRequests);
        // NB: Use the selector-tree depth for this diagnostic. It is not a flat-tree dependency-span proof. A refill
        //     round is rare, so its walk of the ancestors is not on any hot path.
        self.counters.set(
            Counter::FontRefillBlockedDepth,
            self.counters
                .get(Counter::FontRefillBlockedDepth)
                .max(u64::from(self.element_depth(node)) + 1),
        );
        let resolutions = self
            .font_resolution
            .as_mut()
            .expect("a request has a font resolution cache");
        resolver.refill(resolutions, request);
    }
}
