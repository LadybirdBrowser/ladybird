/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What installing a style record has the main thread do beside swapping the element's style,
//! decided on the render side from the element's old record, its new one and the facts the engine
//! holds of the element, which the host acts on without reading the records itself.

use super::super::bridge::{
    FfiStyleInvalidationField, LayoutTreeRebuildRoot, MainEffects, element_adjustment_fact, element_construction_fact,
};
use super::super::publication::pseudo_kind::{MARKER, SEARCH_TEXT, SEARCH_TEXT_CURRENT, SELECTION, SPELLING_ERROR};
use super::super::style_invalidation::RecordView;
use super::super::*;
use crate::css::computed_value_types::AnimationValues;
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::{overflow, scroll_snap_strictness};
use crate::css::table_group_builder::group_index;

/// The main-side effects of installing one element's style record, which the main thread performs after it swapped
/// in the element's styles.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct InstalledElementEffects {
    style_node: u32,
    pub(crate) effects: MainEffects,
    /// Where the layout tree rebuild starts, with `MainEffects::REBUILD`.
    pub(crate) rebuild_root: LayoutTreeRebuildRoot,
    /// What the moves of the element and its pseudo-elements damage, merged into one `FfiStyleInvalidationField` word.
    damage: u32,
    /// The record the element moves from, where the main thread held one, which the effects compare the new one with:
    /// the record the host holds for it, which the installation pins until the host ends the transaction.
    old_record: *const std::ffi::c_void,
    /// The pseudo-elements whose records the host composes once it swapped them in, each a bit at its kind: those
    /// that owe the plan of their animations or the transition step, and those of an element with animations. The
    /// host applies the plan a record decides as it composes it.
    pseudo_elements_composed: u16,
    /// Of those, the ones that owe the transition step, which the host runs against the record each held before.
    pseudo_element_transition_steps: u16,
    /// The swaps of the element's styles, which the host performs before its effects, from the first to one past the
    /// last, by index among the swaps of the installation.
    pub(crate) swaps_begin: u32,
    pub(crate) swaps_end: u32,
}

// SAFETY: The pointer only borrows the immutable record the installation pins until the host ended the transaction.
unsafe impl Send for InstalledElementEffects {}
// SAFETY: As above; nothing writes through the pointer.
unsafe impl Sync for InstalledElementEffects {}

impl InstalledElementEffects {
    pub(crate) fn node(&self) -> StyleNodeID {
        StyleNodeID::from_raw(self.style_node).expect("an effect names an element")
    }

    /// What the moves of the element and its pseudo-elements damage, merged.
    pub(crate) fn damage(&self) -> u32 {
        self.damage
    }

    /// Whether the element moves from a record the main thread held.
    pub(crate) fn moves_from_a_record(&self) -> bool {
        !self.old_record.is_null()
    }

    /// Whether the host has anything to do for the element beside swapping in its styles.
    pub(crate) fn has_host_work(&self) -> bool {
        self.effects != MainEffects::NONE || self.pseudo_elements_composed != 0
    }
}

/// What the moves of the records of an element's pseudo-elements have the main thread do.
#[derive(Default)]
pub(super) struct PseudoElementFacts {
    /// Whether any of them moved.
    pub(super) moved: bool,
    /// What each move damages, a counter-style rebuild of its box included.
    pub(super) damages: smallvec::SmallVec<[u32; 2]>,
    /// Whether the custom-property environment of any of them moved.
    pub(super) environment_moved: bool,
    /// Whether the record any of them moves from or to holds an image.
    pub(super) holds_images: bool,
    /// The ones the host composes, each a bit at its kind.
    pub(super) composed: u16,
    /// The ones that owe the transition step.
    pub(super) transition_steps: u16,
}

/// What installing the record of a pseudo-element owes the host for its animations and transitions.
#[derive(Clone, Copy, Default)]
pub(super) struct PseudoElementComposition {
    pub(super) composed_by_the_host: bool,
    pub(super) owes_an_animation_plan: bool,
    pub(super) owes_a_transition_step: bool,
}

/// The facts of an installation the main-side effects read beside the two records.
pub(super) struct InstallationFacts {
    /// The host composes the element's animations over the record.
    pub(super) composed_by_the_host: bool,
    /// The element owes the plan of its animations, which the host applies before it composes them.
    pub(super) owes_an_animation_plan: bool,
    /// The element owes the transition step, which the host runs against the record it moves from
    /// once it applied the plan.
    pub(super) owes_a_transition_step: bool,
    /// The animations of the element's parent sample custom properties, which the host composes
    /// into what the element inherits.
    pub(super) inherits_sampled_custom_properties: bool,
    /// The custom-property environment the element moves from, and the one it moves to.
    pub(super) old_environment: Option<u64>,
    pub(super) new_environment: u64,
    /// What the element's move damages, where the main thread holds the record it moves from.
    pub(super) element_damage: Option<u32>,
    /// The record the element moves from, where the main thread holds it.
    pub(super) old_record: Option<computed::FinalStyleRecordID>,
    pub(super) pseudo_elements: PseudoElementFacts,
}

impl RetainedState {
    /// Notes in `facts` what moving the `kind` pseudo-element of `node` between the records of
    /// `records`, the one it moves from, the one it moves to and its element's new one, has the main
    /// thread do, the move damaging `damage`. Below an element that was display:none, the main
    /// thread holds no record the pseudo-element moves from.
    pub(super) fn note_pseudo_element_move(
        &mut self,
        facts: &mut PseudoElementFacts,
        node: StyleNodeID,
        kind: u8,
        records: (u64, u64, u64),
        damage: u32,
        below_display_none: bool,
    ) {
        let (old, new, originating) = records;
        facts.moved = true;
        facts.damages.push(damage);
        // The host compares the counter styles a box that stays was built with, and rebuilds it
        // where they moved: a marker's, or those its content names. They resolve from the record's
        // `content` and its list item's `list-style-type`, whose changes the moves damage already,
        // against the counter-style registry the record names, so only a move between registries
        // can move them.
        let reads_counter_styles = kind == MARKER
            || self
                .computed_group_sets
                .style_record_payloads(new)
                .and_then(|payloads| ComputedValuesView::of(payloads).content_value())
                .is_some_and(crate::css::style_compute::content_reads_counter_style_environment);
        let counter_style_registry = |record: u64| {
            self.computed_group_sets
                .style_record_view(record)
                .map(|view| view.counter_style_environment_identity)
        };
        if old != 0 && new != 0 && reads_counter_styles && counter_style_registry(old) != counter_style_registry(new) {
            facts
                .damages
                .push(self.pseudo_element_record_damage(node, kind, old, new, originating, true));
        }
        let sets = &self.computed_group_sets;
        let environment = |record: u64| match record {
            0 => None,
            record => Some(sets.style_record_custom_property_environment(record).unwrap_or(0)),
        };
        let old_environment = if below_display_none { None } else { environment(old) };
        facts.environment_moved |= old_environment != environment(new);
        let holds_images = |record: u64| {
            record != 0
                && sets
                    .style_record_dependency_flags(record)
                    .is_some_and(|flags| flags & computed::HOLDS_IMAGE_VALUES != 0)
        };
        facts.holds_images |= holds_images(old) || holds_images(new);
    }

    /// What installing `new`, the record of the `kind` pseudo-element of `node`, in place of `old`,
    /// the one the main thread holds for it, if any, owes the host for its animations and
    /// transitions, noted in `facts`: the plan its record decides, as an element's does, and the
    /// transition step where the record beneath its composition moves. The host composes it where
    /// it owes either, where its element has animations, which compose over the new record, or
    /// where `old` is a composition, which installing the new record lets go of.
    pub(super) fn note_pseudo_element_composition(
        &self,
        facts: &mut PseudoElementFacts,
        node: StyleNodeID,
        kind: u8,
        old: computed::FinalStyleRecordID,
        new: computed::FinalStyleRecordID,
    ) -> PseudoElementComposition {
        let owes_an_animation_plan = self
            .record_owes_an_animation_plan(old.raw(), new.raw(), || self.record_declares_animations(new.raw()))
            || !self
                .css_defined_animations
                .list(node, super::super::animations::AnimationSlot::from(kind) + 1)
                .is_empty();
        let owes_a_transition_step = old != computed::FinalStyleRecordID::NONE
            && self
                .computed_group_sets
                .underlying_style_record(old)
                .is_some_and(|underlying| {
                    underlying != new
                        && self.decide_transition_step(
                            node,
                            underlying,
                            self.transition_declarations_moved(underlying.raw(), new.raw()),
                        )
                });
        let composed_by_the_host = owes_an_animation_plan
            || owes_a_transition_step
            || old.is_animation_overlay()
            || self.projection.adjustment_facts(node) & element_adjustment_fact::HAS_ANIMATIONS != 0;
        let bit = 1_u16 << kind;
        if composed_by_the_host {
            facts.composed |= bit;
        }
        if owes_a_transition_step {
            facts.transition_steps |= bit;
        }
        PseudoElementComposition {
            composed_by_the_host,
            owes_an_animation_plan,
            owes_a_transition_step,
        }
    }

    /// Notes in `facts` that the main thread takes `record`, the record the `kind` pseudo-element of
    /// `node` already had, again, as it holds no style for the pseudo-element: it installs the
    /// pseudo-element's environment, and its box the images the record holds.
    pub(super) fn note_pseudo_element_reinstall(&self, facts: &mut PseudoElementFacts, record: u64) {
        facts.moved = true;
        facts.environment_moved = true;
        facts.holds_images |= self
            .computed_group_sets
            .style_record_dependency_flags(record)
            .is_some_and(|flags| flags & computed::HOLDS_IMAGE_VALUES != 0);
    }

    /// The main-side effects of moving `node` from `old`, if it had a record, to `new`.
    pub(super) fn main_effects(
        &self,
        node: StyleNodeID,
        old: Option<RecordView<'_>>,
        new: RecordView<'_>,
        facts: InstallationFacts,
    ) -> InstalledElementEffects {
        let adjustment_facts = self.projection.adjustment_facts(node);
        let construction_facts = self.element_construction_facts(node);
        let new_values = view_values(new);
        let old_values = old.map(view_values);
        let is_none = |values: ComputedValuesView<'_>| values.display().is_none();
        let names_anchors = |values: ComputedValuesView<'_>| !values.anchor().anchor_names.as_slice().is_empty();
        let animation = |record: RecordView<'_>| {
            // SAFETY: A live record's animation group payload is a live AnimationValues.
            unsafe {
                &*record.values.payloads[group_index::ANIMATION]
                    .as_ptr()
                    .cast::<AnimationValues>()
            }
        };
        let declares_timelines = |record: RecordView<'_>| {
            crate::css::style_compute::rust_animation_values_declare_timelines(animation(record))
        };
        let timelines_differ = match old {
            None => declares_timelines(new),
            Some(old) => {
                let (old_declares, new_declares) = (declares_timelines(old), declares_timelines(new));
                (old_declares || new_declares)
                    && (old_declares != new_declares
                        || !crate::css::style_compute::rust_animation_values_timeline_declarations_equal(
                            animation(old),
                            animation(new),
                        ))
            }
        };
        let was_none = old_values.is_none_or(is_none);
        let (damage_effects, rebuild_root) = super::super::style_invalidation::main_effects_of_damage(
            facts.element_damage,
            facts.pseudo_elements.damages.iter().copied(),
        );
        let damage = super::super::style_invalidation::merged_damage(
            facts.element_damage,
            facts.pseudo_elements.damages.iter().copied(),
        );
        let sets = &self.computed_group_sets;
        let holds_images = |record: RecordView<'_>| {
            record
                .identity
                .and_then(|identity| sets.style_record_dependency_flags(identity.raw()))
                .is_some_and(|flags| flags & computed::HOLDS_IMAGE_VALUES != 0)
        };
        let custom_properties = facts.old_environment != Some(facts.new_environment);
        let container_effects = facts.composed_by_the_host || self.container_effects_for_host.get(&node).is_some();
        // A box that is a scroll container registers as a scroll snap container, or forgets what it
        // snapped to, and one whose style snaps marks the document as having snap areas.
        let scroll_snaps = |values: ComputedValuesView<'_>| {
            let makes_scroll_container = |overflow: u8| overflow != overflow::VISIBLE && overflow != overflow::CLIP;
            makes_scroll_container(values.overflow_x())
                || makes_scroll_container(values.overflow_y())
                || values.misc_interaction().scroll_snap_strictness != scroll_snap_strictness::NONE
        };
        let effects = damage_effects
            .with(
                MainEffects::SCROLL_SNAP,
                adjustment_facts & element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0
                    || scroll_snaps(new_values)
                    || old_values.is_some_and(scroll_snaps),
            )
            .with(MainEffects::CONTAINER_EFFECTS, container_effects)
            .with(
                MainEffects::IMAGES,
                holds_images(new) || old.is_some_and(holds_images) || facts.pseudo_elements.holds_images,
            )
            .with(
                MainEffects::HIGHLIGHTS,
                self.holds_pseudo_element_records_of(node, &HIGHLIGHT_PSEUDO_ELEMENTS)
                    || self.computed_group_sets.custom_highlight_records(node).next().is_some(),
            )
            .with(
                MainEffects::ANIMATED,
                facts.composed_by_the_host || facts.inherits_sampled_custom_properties,
            )
            .with(MainEffects::ANIMATION_PLAN, facts.owes_an_animation_plan)
            .with(MainEffects::TRANSITION_STEP, facts.owes_a_transition_step)
            .with(
                MainEffects::DISPLAY_NONE_FLIP,
                old_values.is_some_and(|old| is_none(old) != is_none(new_values)),
            )
            .with(
                MainEffects::CLEAR_HIDDEN_DESCENDANTS,
                self.clears_hidden_descendants(
                    node,
                    old_values.map(is_none),
                    is_none(new_values),
                    (damage >> FfiStyleInvalidationField::InheritedGroupsShift as u32)
                        & FfiStyleInvalidationField::InheritedGroupsMask as u32,
                    custom_properties,
                ),
            )
            .with(
                MainEffects::VISIBILITY_FLIP,
                old_values.is_some_and(|old| old.visibility() != new_values.visibility()),
            )
            // NB: An element's environment is the store its record holds. The host has something to do only where
            //     another lies over that: what animations sample, on the element or a parent it inherits them from.
            .with(
                MainEffects::CUSTOM_PROPERTIES,
                (custom_properties || facts.pseudo_elements.environment_moved)
                    && (facts.inherits_sampled_custom_properties
                        || self.element_custom_property_data.contains_key(&node)
                        || self.pseudo_element_custom_property_data.contains_key(&node)),
            )
            .with(
                MainEffects::ANCHOR_NAMES,
                old_values.is_some_and(names_anchors) || names_anchors(new_values),
            )
            .with(
                MainEffects::ROOT_FONT_METRICS,
                adjustment_facts & element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0,
            )
            .with(MainEffects::REVEAL_WAITING, was_none && !is_none(new_values))
            .with(
                MainEffects::SVG_REFERENCES,
                adjustment_facts & element_adjustment_fact::IS_SVG_ELEMENT != 0,
            )
            .with(MainEffects::TIMELINES, timelines_differ)
            .with(
                MainEffects::FORM_CONTROL,
                construction_facts
                    & (element_construction_fact::IS_HTML_INPUT_ELEMENT
                        | element_construction_fact::IS_HTML_SELECT_ELEMENT)
                    != 0,
            );
        InstalledElementEffects {
            style_node: node.raw(),
            effects,
            rebuild_root,
            damage,
            old_record: facts.old_record.map_or(std::ptr::null(), |old| {
                std::sync::Arc::as_ptr(
                    self.computed_group_sets
                        .host_record(old.raw())
                        .expect("the installation pins the record an element moves from"),
                )
                .cast()
            }),
            pseudo_elements_composed: facts.pseudo_elements.composed,
            pseudo_element_transition_steps: facts.pseudo_elements.transition_steps,
            swaps_begin: 0,
            swaps_end: 0,
        }
    }

    /// Whether `node` holds a record for a pseudo-element of one of `kinds`.
    fn holds_pseudo_element_records_of(&self, node: StyleNodeID, kinds: &[u8]) -> bool {
        kinds
            .iter()
            .any(|&kind| self.computed_group_sets.pseudo_style_record(node, kind).is_some())
    }
}

/// The highlight pseudo-elements, whose styles the host pushes to paint whenever the element's box
/// takes a record.
const HIGHLIGHT_PSEUDO_ELEMENTS: [u8; 4] = [SELECTION, SEARCH_TEXT, SEARCH_TEXT_CURRENT, SPELLING_ERROR];

/// Whether a style move with `damage`, an `FfiStyleInvalidationField` word, has its box hit-tested again, as the host's
/// `invalidates_hit_test_display_list()` decides for a record it installs: where the move says so of the properties it
/// changes, lays the box out again, or rebuilds the stacking contexts or visual contexts the box takes part in. A move
/// of a paint-only property, as of `color`, repaints the box alone.
pub(crate) fn record_damage_hit_tests_again(damage: u32) -> bool {
    use super::super::bridge::FfiStyleInvalidationField;
    use super::super::bridge::RELAYOUT_LEVEL;
    const VISUAL_CONTEXT_REBUILD: u32 = 2;
    let level = damage & FfiStyleInvalidationField::LevelMask as u32;
    let visual_contexts =
        (damage >> FfiStyleInvalidationField::VisualContextShift as u32) & FfiStyleInvalidationField::LevelMask as u32;
    damage & FfiStyleInvalidationField::AffectsHitTesting as u32 != 0
        || level >= RELAYOUT_LEVEL
        || damage & FfiStyleInvalidationField::RebuildStackingContext as u32 != 0
        || visual_contexts == VISUAL_CONTEXT_REBUILD
}

fn view_values(record: RecordView<'_>) -> ComputedValuesView<'_> {
    ComputedValuesView::of(record.values.payloads)
}

#[cfg(test)]
mod tests {
    use super::super::test_support::*;
    use super::{MARKER, record_damage_hit_tests_again};
    use crate::css::style::bridge::{FfiStyleInvalidationField, MainEffects};
    use crate::css::style::tree::{StyleNodeID, TreeScopeID};

    /// Whether the last style installation rebuilds the layout tree for `node`'s move. The main
    /// thread then holds the records it installed for `node` and its marker, as the host does once
    /// it took the installation.
    fn installation_rebuilds(engine: &mut crate::css::style::StyleEngine, node: StyleNodeID) -> bool {
        let installation = engine.take_installation();
        let state = &mut engine.retained;
        let held =
            |record: Option<crate::css::style::computed::FinalStyleRecordID>| record.map_or(0, |record| record.raw());
        let element_record = held(state.computed_group_sets.assigned_style_record(node));
        let marker_record = held(state.computed_group_sets.pseudo_style_record(node, MARKER));
        let element_held = state.computed_group_sets.host_record(element_record).cloned();
        state.set_held_style_record(node, element_held);
        state.set_held_pseudo_style_record(node, super::super::PseudoElementTarget::of_kind(MARKER), marker_record);
        installation
            .effects_of(node)
            .is_some_and(|effects| effects.effects.intersects(MainEffects::REBUILD))
    }

    #[test]
    fn a_marker_recolor_keeps_the_list_item_box() {
        let (mut engine, nodes) = linear_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(
            &mut engine,
            sheet,
            ".item { display: list-item; } .item::marker { color: rgb(255, 0, 0); } \
             .item.recolored::marker { color: rgb(0, 0, 255); } .item.roman { list-style-type: lower-roman; }",
        );
        engine
            .retained
            .set_counter_style_environment_identity(TreeScopeID::DOCUMENT, 1);
        start_live(&mut engine, nodes[0]);
        // The list item's marker box appears, which builds it.
        add_class(&mut engine, nodes[2], "item");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(installation_rebuilds(&mut engine, nodes[2]));

        add_class(&mut engine, nodes[2], "recolored");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(!installation_rebuilds(&mut engine, nodes[2]));

        // A marker that moves to another counter-style registry may resolve its counter style
        // differently.
        engine
            .retained
            .set_counter_style_environment_identity(TreeScopeID::DOCUMENT, 2);
        remove_class(&mut engine, nodes[2], "recolored");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(installation_rebuilds(&mut engine, nodes[2]));

        // The marker string comes from the list item's own list-style-type, which rebuilds it.
        add_class(&mut engine, nodes[2], "roman");
        run_style_update(&mut engine, nodes[0]);
        assert_live_matches_cold(&mut engine, nodes[0]);
        assert!(installation_rebuilds(&mut engine, nodes[2]));
    }

    /// An element entering display:none has the main thread clear the styles of its descendants, which the render
    /// side decides and notes as the host takes the installation, without the host telling it.
    #[test]
    fn entering_display_none_clears_what_descendants_hold() {
        let (mut engine, nodes) = nested_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, ".hidden { display: none; }");
        start_live(&mut engine, nodes[0]);
        run_style_update(&mut engine, nodes[0]);
        let _ = engine.take_installation();
        for &node in &nodes {
            let record = engine.retained.computed_group_sets.assigned_style_record(node);
            let held = record.and_then(|record| engine.retained.computed_group_sets.host_record(record.raw()).cloned());
            engine.retained.set_held_style_record(node, held);
        }
        assert!(engine.retained.held_style_records.contains(nodes[3]));

        add_class(&mut engine, nodes[1], "hidden");
        run_style_update(&mut engine, nodes[0]);
        let installation = engine.take_installation();
        assert!(
            installation
                .effects_of(nodes[1])
                .is_some_and(|effects| { effects.effects.intersects(MainEffects::CLEAR_HIDDEN_DESCENDANTS) })
        );
        installation.note_held_by_the_host(&mut engine.retained);
        assert!(engine.retained.held_style_records.contains(nodes[1]));
        assert!(!engine.retained.held_style_records.contains(nodes[2]));
        assert!(!engine.retained.held_style_records.contains(nodes[3]));
    }

    #[test]
    fn only_moves_that_change_hit_testing_hit_test_again() {
        let repaint = 1;
        let relayout = 2;
        let rebuild = 3;
        let visual_contexts = |level: u32| level << FfiStyleInvalidationField::VisualContextShift as u32;
        let engine_computed = FfiStyleInvalidationField::EngineComputed as u32;
        assert!(!record_damage_hit_tests_again(0));
        assert!(!record_damage_hit_tests_again(repaint | engine_computed));
        assert!(!record_damage_hit_tests_again(
            repaint | visual_contexts(1) | FfiStyleInvalidationField::AnyComputedValueChanged as u32
        ));
        assert!(!record_damage_hit_tests_again(
            repaint | FfiStyleInvalidationField::RepaintTextDecorations as u32
        ));
        assert!(record_damage_hit_tests_again(
            repaint | FfiStyleInvalidationField::AffectsHitTesting as u32
        ));
        assert!(record_damage_hit_tests_again(relayout));
        assert!(record_damage_hit_tests_again(rebuild));
        assert!(record_damage_hit_tests_again(
            repaint | FfiStyleInvalidationField::RebuildStackingContext as u32
        ));
        assert!(record_damage_hit_tests_again(repaint | visual_contexts(2)));
    }
}
