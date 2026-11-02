/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the inputs the host has yet to settle may change, which the host asks to skip settling
//! them where nothing it reads can move: a geometry read reuses the committed layout where no
//! pending input may move geometry, and a hover reuses the read-only frame where no element can
//! match a compound testing `:hover`.

use super::super::*;
use super::invalidation::{InvalidationMap, Trigger};
use super::live::Live;
use super::projection::{ElementChange, ProjectionGeneration};
use crate::fast_hash::FastSet;

/// The last answer of whether a hover needs the style engine, with what it read to answer: the
/// projection's generation, the tree's version, the invalidation map and the hover's root. An
/// answer read over the same holds again, so a rendering update over a document nothing changed
/// in asks nothing of its elements.
#[derive(Clone, Default)]
pub(in crate::css::style) struct HoverSealMemo {
    last: Option<HoverSealAnswer>,
    candidates: Option<HoverSealCandidates>,
    cursor_facts: Option<HoverCursorFactsAnswer>,
}

/// The elements whose rows may make a hover need the style engine, with the generation of the rows they were read
/// from and the invalidation map they were matched against: a seal over other rows reads again only the rows written
/// since.
#[derive(Clone)]
struct HoverSealCandidates {
    projection: ProjectionGeneration,
    invalidation: Option<Arc<InvalidationMap>>,
    /// The elements whose written facts say they are disabled.
    disabled: FastSet<StyleNodeID>,
    hover_keyed: HoverKeyedElements,
}

/// The elements that may match a compound testing `:hover`, by their keys.
#[derive(Clone)]
enum HoverKeyedElements {
    /// Every element may: there is no map describing the rules, or a query tests `:hover`.
    Every,
    /// Those carrying the keys of a compound testing `:hover`.
    Carrying(FastSet<StyleNodeID>),
}

/// The cursor facts published last, with the generations of what they read: the projection's, the tree's paint facts,
/// and the records the elements are assigned.
#[derive(Clone)]
struct HoverCursorFactsAnswer {
    projection: ProjectionGeneration,
    dom_paint_facts_version: u64,
    installed_column_version: u64,
    facts: Arc<super::super::hover_lane::HoverCursorFacts>,
    editable: Arc<FastSet<StyleNodeID>>,
}

#[derive(Clone)]
struct HoverSealAnswer {
    projection: ProjectionGeneration,
    dom_order_version: u64,
    invalidation: Option<Arc<InvalidationMap>>,
    root: StyleNodeID,
    needs_style_state: bool,
}

/// Whether `a` and `b` are the same invalidation map, or both none.
fn same_map(a: Option<&Arc<InvalidationMap>>, b: Option<&Arc<InvalidationMap>>) -> bool {
    match (a, b) {
        (Some(a), Some(b)) => Arc::ptr_eq(a, b),
        (None, None) => true,
        _ => false,
    }
}

impl StyleEngine {
    fn hover_seal_invalidation(&self) -> Option<&Arc<InvalidationMap>> {
        self.retained.live_style.as_deref()?.current_invalidation_shared()
    }

    /// The answer the hover seal gave last for `root`, where nothing it read moved since.
    pub(in super::super) fn memoized_hover_seal(&self, root: StyleNodeID) -> Option<bool> {
        let last = self.host.hover_seal.last.as_ref()?;
        (last.root == root
            && same_map(last.invalidation.as_ref(), self.hover_seal_invalidation())
            && last.dom_order_version == self.tree().dom_order_version()
            && self.retained.projection.is_at_generation(&last.projection))
        .then_some(last.needs_style_state)
    }

    /// The cursor facts built last, where nothing they read moved since.
    pub(in super::super) fn memoized_hover_cursor_facts(
        &self,
    ) -> Option<Arc<super::super::hover_lane::HoverCursorFacts>> {
        let last = self.host.hover_seal.cursor_facts.as_ref()?;
        (last.dom_paint_facts_version == self.tree().dom_paint_facts_version()
            && last.installed_column_version == self.computed_group_sets.installed_column_version()
            && self.retained.projection.is_at_generation(&last.projection))
        .then(|| last.facts.clone())
    }

    /// The editable elements as they stand, shared with the cursor facts published last while the tree's paint facts
    /// did not move.
    pub(in super::super) fn hover_editable_nodes(&self) -> Arc<FastSet<StyleNodeID>> {
        match &self.host.hover_seal.cursor_facts {
            Some(last) if last.dom_paint_facts_version == self.tree().dom_paint_facts_version() => {
                last.editable.clone()
            }
            _ => Arc::new(self.tree().editable_nodes().collect()),
        }
    }

    /// Keep `facts`, the cursor facts as they stand, with what they read.
    pub(in super::super) fn memoize_hover_cursor_facts(
        &mut self,
        facts: Arc<super::super::hover_lane::HoverCursorFacts>,
    ) {
        self.host.hover_seal.cursor_facts = Some(HoverCursorFactsAnswer {
            projection: self.retained.projection.generation(),
            dom_paint_facts_version: self.tree().dom_paint_facts_version(),
            installed_column_version: self.computed_group_sets.installed_column_version(),
            editable: facts.editable(),
            facts,
        });
    }

    /// Keep `needs_style_state`, the hover seal's answer for `root`, with what it read.
    pub(in super::super) fn memoize_hover_seal(&mut self, root: StyleNodeID, needs_style_state: bool) {
        self.host.hover_seal.last = Some(HoverSealAnswer {
            projection: self.retained.projection.generation(),
            dom_order_version: self.tree().dom_order_version(),
            invalidation: self.hover_seal_invalidation().cloned(),
            root,
            needs_style_state,
        });
    }

    /// The engine of the document and its invalidation map, where the map describes the
    /// rules and elements as they stand.
    fn current_invalidation(&self) -> Option<(&Live, &InvalidationMap)> {
        let live = self.retained.live_style.as_deref()?;
        let map = live.current_invalidation()?;
        Some((live, map))
    }

    /// Whether settling the pending element inputs may move layout geometry: those the projection
    /// holds no row for always may, and so may a change made to
    /// the projection since the last barrier or geometry read seal that does by
    /// [`Self::change_may_affect_layout_geometry`].
    pub(in super::super) fn pending_inputs_may_affect_layout_geometry(&self) -> bool {
        if !self.host.element_inputs.is_empty() {
            return true;
        }
        let current = self.current_invalidation();
        self.retained.projection.any_change_since_seal(|node, change| {
            current.is_none_or(|(live, map)| self.change_may_affect_layout_geometry(live, map, node, change))
        })
    }

    /// Whether settling `change`, a pending change to one element's facts or relations, may move
    /// layout geometry: whether the elements it restyles may come to match, or stop matching, a
    /// rule that may move geometry, or substitute the element's attributes.
    fn change_may_affect_layout_geometry(
        &self,
        live: &Live,
        map: &InvalidationMap,
        node: StyleNodeID,
        change: ElementChange,
    ) -> bool {
        let program = &self.retained.rules;
        let rule_moves = |entry: u32| program.rule_may_affect_layout_geometry(live.index.rule_of(entry));
        let moves = |trigger: Trigger| {
            map.effects(&trigger).any(|effect| rule_moves(effect.entry))
                || map.lone_entries(&trigger).any(rule_moves)
                || map
                    .queries_testing(&trigger)
                    .any(|(query, _)| rule_moves(map.relative_entry(query)))
        };
        let feature = match change {
            ElementChange::State(fact) => return moves(Trigger::State(fact)),
            ElementChange::Id(old, new) => {
                return [old, new].into_iter().any(|id| !id.is_none() && moves(Trigger::Id(id)));
            }
            ElementChange::Feature(feature) => feature,
            ElementChange::Relations(..) | ElementChange::ArrivingFacts => return true,
        };
        match feature {
            LocalFeatureKey::Class(class) => moves(Trigger::Class(class)),
            LocalFeatureKey::Attribute(name) => {
                self.retained
                    .projection
                    .attribute_name_keys(name)
                    .into_iter()
                    .any(|name| !name.is_none() && moves(Trigger::Attribute(name)))
                    // https://drafts.csswg.org/css-values-5/#attr-notation
                    || std::iter::once(node)
                        .chain(self.retained.backing_elements(node, u64::MAX))
                        .any(|reader| {
                            live.records.get(reader).is_some_and(|record| record.rare().reads.reads_attributes)
                                || live
                                    .records
                                    .pseudo_element_records(reader)
                                    .iter()
                                    .any(|(_, record)| record.rare().reads.reads_attributes)
                        })
            }
            LocalFeatureKey::CustomState(state) => moves(Trigger::CustomState(state)),
            LocalFeatureKey::Emptiness => moves(Trigger::Emptiness),
            LocalFeatureKey::Language => moves(Trigger::Language),
            LocalFeatureKey::Directionality => moves(Trigger::Directionality),
            LocalFeatureKey::HeadingLevel => moves(Trigger::Heading),
            LocalFeatureKey::Part(part) => moves(Trigger::Part(part)),
            LocalFeatureKey::Id
            | LocalFeatureKey::PartExposure
            | LocalFeatureKey::TagName
            | LocalFeatureKey::FoldedTagName => true,
        }
    }

    /// Whether a hover under `root` needs the style engine: whether an element in its shadow-including subtree is a
    /// disabled control, or may come to match, or stop matching, a compound testing `:hover`. Reads again only the
    /// rows written since the last answer, where the rules are the same.
    pub(in super::super) fn hover_seal_needs_style_state(&mut self, root: StyleNodeID) -> bool {
        self.update_hover_seal_candidates();
        let candidates = self
            .host
            .hover_seal
            .candidates
            .as_ref()
            .expect("the hover seal's candidates were just read");
        let projection = &self.retained.projection;
        let tree = self.tree();
        let under_root =
            |node: &StyleNodeID| tree.is_live(*node) && projection.is_in_shadow_including_subtree_of(*node, root);
        candidates.disabled.iter().any(under_root)
            || match &candidates.hover_keyed {
                HoverKeyedElements::Every => true,
                HoverKeyedElements::Carrying(elements) => elements.iter().any(under_root),
            }
    }

    /// Bring the hover seal's candidates up to the rows as they stand: read the rows written since they were read
    /// last, or every row where there are none yet, or the rules moved.
    fn update_hover_seal_candidates(&mut self) {
        if self.host.hover_seal.candidates.as_ref().is_some_and(|candidates| {
            self.retained.projection.is_at_generation(&candidates.projection)
                && same_map(candidates.invalidation.as_ref(), self.hover_seal_invalidation())
        }) {
            return;
        }
        let generation = self.retained.projection.generation();
        let invalidation = self.hover_seal_invalidation().cloned();
        let (mut candidates, first_read) = match self.host.hover_seal.candidates.take() {
            Some(candidates) => (candidates, false),
            None => (
                HoverSealCandidates {
                    projection: generation.clone(),
                    invalidation: None,
                    disabled: FastSet::default(),
                    hover_keyed: HoverKeyedElements::Every,
                },
                true,
            ),
        };
        let projection = &self.retained.projection;
        let every_slot: Vec<_> = std::iter::once(0..projection.element_slot_count()).collect();
        let element_at = |slot: usize| StyleNodeID::element(u32::try_from(slot).expect("element slot exceeds u32"));
        let disabled_slots = match first_read {
            true => every_slot.clone(),
            false => generation.written_slots_changed_since(&candidates.projection),
        };
        for node in disabled_slots
            .into_iter()
            .flatten()
            .filter(|&slot| slot != 0)
            .map(element_at)
        {
            if projection
                .written_facts(node)
                .is_some_and(|facts| facts.states().contains(StateFact::Disabled))
            {
                candidates.disabled.insert(node);
            } else {
                candidates.disabled.remove(&node);
            }
        }
        let hover = Trigger::State(StateFact::Hover);
        candidates.hover_keyed = match self.current_invalidation() {
            Some((live, map)) if map.queries_testing(&hover).next().is_none() => {
                let rules_moved = first_read || !same_map(candidates.invalidation.as_ref(), invalidation.as_ref());
                let (mut elements, keyed_slots) =
                    match std::mem::replace(&mut candidates.hover_keyed, HoverKeyedElements::Every) {
                        HoverKeyedElements::Carrying(elements) if !rules_moved => (
                            elements,
                            generation.committed_slots_changed_since(&candidates.projection),
                        ),
                        _ => (FastSet::default(), every_slot),
                    };
                if map.has_effects(&hover) {
                    for node in keyed_slots
                        .into_iter()
                        .flatten()
                        .filter(|&slot| slot != 0)
                        .map(element_at)
                    {
                        let carries_keys = projection.row_of(node).is_some_and(|row| {
                            map.effects(&hover)
                                .any(|effect| map.may_match_by_keys(&live.index, effect, row))
                        });
                        if carries_keys {
                            elements.insert(node);
                        } else {
                            elements.remove(&node);
                        }
                    }
                } else {
                    elements.clear();
                }
                HoverKeyedElements::Carrying(elements)
            }
            _ => HoverKeyedElements::Every,
        };
        candidates.projection = generation;
        candidates.invalidation = invalidation;
        self.host.hover_seal.candidates = Some(candidates);
    }
}

#[cfg(test)]
mod tests {
    use super::super::test_support::{
        add_class, add_rules, add_sheet, linear_document, remove_class, set_tag, start_live,
    };
    use super::*;

    fn document_with_rules(source: &str) -> (StyleEngine, Vec<StyleNodeID>) {
        let (mut engine, nodes) = linear_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, source);
        start_live(&mut engine, nodes[0]);
        (engine, nodes)
    }

    #[test]
    fn only_inputs_reaching_rules_that_move_geometry_may_affect_layout_geometry() {
        let (mut engine, nodes) = document_with_rules(
            ".paint { background-color: green; } .wide > * { width: 10px; } .moved { transform: scale(2); }",
        );
        add_class(&mut engine, nodes[1], "paint");
        add_class(&mut engine, nodes[2], "unstyled");
        assert!(engine.has_pending_transaction());
        assert!(!engine.pending_transaction_may_affect_layout_geometry());

        add_class(&mut engine, nodes[0], "wide");
        assert!(engine.pending_transaction_may_affect_layout_geometry());

        let (mut engine, nodes) = document_with_rules(".moved { transform: scale(2); }");
        add_class(&mut engine, nodes[3], "moved");
        assert!(engine.pending_transaction_may_affect_layout_geometry());
    }

    #[test]
    fn inputs_a_has_argument_reads_may_affect_layout_geometry_through_its_anchor() {
        let (mut engine, nodes) = document_with_rules(".anchor:has(.inner) { width: 1px; }");
        add_class(&mut engine, nodes[2], "inner");
        assert!(engine.pending_transaction_may_affect_layout_geometry());

        let (mut engine, nodes) = document_with_rules(".anchor:has(.inner) { background-color: green; }");
        add_class(&mut engine, nodes[2], "inner");
        assert!(!engine.pending_transaction_may_affect_layout_geometry());
    }

    #[test]
    fn hover_restyles_only_where_an_element_may_match_a_hover_compound() {
        let (mut engine, nodes) = document_with_rules(".menu:hover .item { color: red; }");
        assert!(!engine.hover_needs_style_state(nodes[0]));

        add_class(&mut engine, nodes[1], "menu");
        start_live(&mut engine, nodes[0]);
        assert!(engine.hover_needs_style_state(nodes[0]));

        let (mut engine, nodes) = document_with_rules(":is(button, .action):hover { color: red; }");
        assert!(!engine.hover_needs_style_state(nodes[0]));
        set_tag(&mut engine, nodes[2], "button");
        start_live(&mut engine, nodes[0]);
        assert!(engine.hover_needs_style_state(nodes[0]));
    }

    #[test]
    fn hover_restyles_through_a_type_selector_in_any_case() {
        let (mut engine, nodes) = document_with_rules("A:hover { color: red; }");
        assert!(!engine.hover_needs_style_state(nodes[0]));
        set_tag(&mut engine, nodes[2], "a");
        start_live(&mut engine, nodes[0]);
        assert!(engine.hover_needs_style_state(nodes[0]));
    }

    #[test]
    fn hover_restyles_through_what_no_compound_of_its_own_tests() {
        let (mut engine, nodes) = document_with_rules(":nth-child(1 of :hover) { color: red; }");
        assert!(engine.hover_needs_style_state(nodes[0]));
    }

    #[test]
    fn hover_restyles_a_backed_pseudo_element_only_through_the_element_backing_it() {
        let (mut engine, nodes) = document_with_rules("input::file-selector-button:hover { color: red; }");
        assert!(!engine.hover_needs_style_state(nodes[0]));
    }

    #[test]
    fn a_hover_seal_follows_the_rows_written_since_it_answered() {
        let (mut engine, nodes) = document_with_rules(".menu:hover { color: red; }");
        assert!(!engine.hover_needs_style_state(nodes[0]));
        add_class(&mut engine, nodes[2], "menu");
        start_live(&mut engine, nodes[0]);
        assert!(engine.hover_needs_style_state(nodes[0]));
        assert!(!engine.hover_needs_style_state(nodes[3]));
        remove_class(&mut engine, nodes[2], "menu");
        start_live(&mut engine, nodes[0]);
        assert!(!engine.hover_needs_style_state(nodes[0]));
        add_class(&mut engine, nodes[3], "menu");
        start_live(&mut engine, nodes[0]);
        assert!(engine.hover_needs_style_state(nodes[3]));
        assert!(engine.hover_needs_style_state(nodes[0]));
    }
}
