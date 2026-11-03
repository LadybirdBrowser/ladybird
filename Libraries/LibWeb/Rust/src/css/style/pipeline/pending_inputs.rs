/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the inputs the host has yet to settle may change, which the host asks to skip settling
//! them where nothing it reads can move: a geometry read reuses the committed layout where no
//! pending input may move geometry, and a hover reuses the read-only frame where no element can
//! match a compound testing `:hover`.
//!
//! The hover's question is asked of every frame and the host waits for its answer, so no element is read to answer it.
//! The compounds testing `:hover` are compiled once for the rules into the keys of the elements that may match them, and
//! a transaction notes the elements whose keys among those it moved.

use super::super::*;
use super::invalidation::{HoverCompounds, InvalidationMap, Trigger};
use super::live::Live;
use super::projection::{ElementChange, ProjectionGeneration};
use crate::fast_hash::FastSet;

/// What the hover seal keeps between frames.
#[derive(Default)]
pub(in crate::css::style) struct HoverSealMemo {
    carriers: Option<HoverCarriers>,
    cursor_facts: Option<HoverCursorFactsAnswer>,
}

// NB: A fork of the engine seals no frame, and has no use for what the host's seal kept.
impl Clone for HoverSealMemo {
    fn clone(&self) -> Self {
        Self::default()
    }
}

/// The elements that may match a compound testing `:hover`, which the transactions keep up as they move the keys of the
/// compounds of `map`.
struct HoverCarriers {
    map: Arc<InvalidationMap>,
    compounds: HoverCompounds,
    elements: FastSet<StyleNodeID>,
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

impl StyleEngine {
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
    /// disabled control, or may come to match, or stop matching, a compound testing `:hover`. Reads only the elements
    /// a transaction moved the keys of those compounds of.
    pub(in super::super) fn hover_seal_needs_style_state(&mut self, root: StyleNodeID) -> bool {
        let projection = &self.retained.projection;
        let tree = self.tree();
        let under_root =
            |node: &StyleNodeID| tree.is_live(*node) && projection.is_in_shadow_including_subtree_of(*node, root);
        if projection.written_disabled().iter().any(under_root) || !self.update_hover_carriers() {
            return true;
        }
        let projection = &self.retained.projection;
        let tree = self.tree();
        let under_root =
            |node: &StyleNodeID| tree.is_live(*node) && projection.is_in_shadow_including_subtree_of(*node, root);
        self.host
            .hover_seal
            .carriers
            .as_ref()
            .expect("the hover seal's carriers were just brought up")
            .elements
            .iter()
            .any(under_root)
    }

    /// Bring the elements that may match a compound testing `:hover` up to the rows and the rules as they stand,
    /// answering whether they say which elements may. They do not where no map describes the rules, or where any element
    /// may.
    fn update_hover_carriers(&mut self) -> bool {
        let Some(live) = self.retained.live_style.as_deref() else {
            return false;
        };
        let Some(map) = live.current_invalidation_shared() else {
            return false;
        };
        let projection = &mut self.retained.projection;
        let known = self
            .host
            .hover_seal
            .carriers
            .take()
            .filter(|carriers| projection.watches_keys(carriers.compounds.keys()));
        let mut carriers = match known {
            Some(carriers) if Arc::ptr_eq(&carriers.map, map) => carriers,
            known => {
                let compounds = map.hover_compounds(&live.index);
                let elements = match known {
                    // NB: The rules moved around the same compounds: no key moved, and no element is read again.
                    Some(carriers) if carriers.compounds.are_the_same_as(&compounds) => carriers.elements,
                    _ => {
                        let writes = projection.take_watched_key_writes();
                        projection.reuse_watched_key_writes(writes);
                        projection
                            .committed_carriers_of(compounds.keys())
                            .filter(|&node| {
                                projection
                                    .row_of(node)
                                    .is_some_and(|row| compounds.may_match(map, &live.index, row))
                            })
                            .collect()
                    }
                };
                projection.watch_keys(compounds.keys().clone());
                HoverCarriers {
                    map: map.clone(),
                    compounds,
                    elements,
                }
            }
        };
        let writes = projection.take_watched_key_writes();
        for &node in &writes {
            let carries = projection
                .row_of(node)
                .is_some_and(|row| carriers.compounds.may_match(&carriers.map, &live.index, row));
            if carries {
                carriers.elements.insert(node);
            } else {
                carriers.elements.remove(&node);
            }
        }
        projection.reuse_watched_key_writes(writes);
        let says = !carriers.compounds.every_element();
        self.host.hover_seal.carriers = Some(carriers);
        says
    }
}

#[cfg(test)]
mod tests {
    use super::super::test_support::{
        add_class, add_rules, add_sheet, linear_document, relations, remove_class, run_style_update, set_tag,
        start_live,
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

    #[test]
    fn hover_compounds_select_what_reading_every_compound_selects() {
        for selector in [
            "A:hover",
            ".a.b:hover",
            ".a.c:hover",
            "#i:hover",
            "span.a:hover",
            "div.c:hover",
            ":is(.a, span):hover",
            ":is(.c, em):hover",
            ":where(:is(.b)):hover",
            ":is(.c):is(.a):hover",
        ] {
            let (mut engine, nodes) = document_with_rules(&format!("{selector} {{ color: red; }}"));
            add_class(&mut engine, nodes[1], "a");
            add_class(&mut engine, nodes[1], "b");
            set_tag(&mut engine, nodes[2], "span");
            add_class(&mut engine, nodes[3], "a");
            start_live(&mut engine, nodes[0]);
            let (live, map) = engine.current_invalidation().expect("the map describes the rules");
            let compounds = map.hover_compounds(&live.index);
            assert!(!compounds.every_element(), "{selector}");
            let hover = Trigger::State(StateFact::Hover);
            for &node in &nodes {
                let row = engine.retained.projection.row_of(node).expect("the element has facts");
                assert_eq!(
                    compounds.may_match(map, &live.index, row),
                    map.effects(&hover)
                        .any(|effect| map.may_match_by_keys(&live.index, effect, row)),
                    "{selector}"
                );
            }
        }
    }

    #[test]
    fn hover_compounds_with_no_key_answer_for_every_element() {
        for selector in ["*:hover", ":hover", ":not(:hover)", "[data-x]:hover", ":not(.a):hover"] {
            let (engine, _) = document_with_rules(&format!("{selector} {{ color: red; }}"));
            let (live, map) = engine.current_invalidation().expect("the map describes the rules");
            assert!(map.hover_compounds(&live.index).every_element(), "{selector}");
        }
    }

    #[test]
    fn a_hover_seal_follows_each_key_of_a_compound_as_transactions_move_them() {
        let (mut engine, nodes) = document_with_rules(".a.b:hover { color: red; }");
        assert!(!engine.hover_needs_style_state(nodes[0]));
        add_class(&mut engine, nodes[2], "a");
        run_style_update(&mut engine, nodes[0]);
        assert!(!engine.hover_needs_style_state(nodes[0]));
        add_class(&mut engine, nodes[2], "b");
        run_style_update(&mut engine, nodes[0]);
        assert!(engine.hover_needs_style_state(nodes[0]));
        remove_class(&mut engine, nodes[2], "a");
        run_style_update(&mut engine, nodes[0]);
        assert!(!engine.hover_needs_style_state(nodes[0]));
    }

    #[test]
    fn a_hover_seal_hears_of_no_element_but_those_whose_watched_keys_moved() {
        let (mut engine, nodes) = document_with_rules(".menu:hover { color: red; }");
        assert!(!engine.hover_needs_style_state(nodes[0]));
        add_class(&mut engine, nodes[1], "other");
        run_style_update(&mut engine, nodes[0]);
        assert!(engine.retained.projection.take_watched_key_writes().is_empty());
        add_class(&mut engine, nodes[1], "menu");
        add_class(&mut engine, nodes[2], "other");
        run_style_update(&mut engine, nodes[0]);
        assert_eq!(engine.retained.projection.take_watched_key_writes(), [nodes[1]]);
        add_class(&mut engine, nodes[1], "another");
        run_style_update(&mut engine, nodes[0]);
        assert_eq!(engine.retained.projection.take_watched_key_writes(), [nodes[1]]);
    }

    #[test]
    fn a_hover_seal_reads_no_element_for_rules_that_leave_its_compounds_alone() {
        let (mut engine, nodes) = document_with_rules(".menu:hover { color: red; }");
        assert!(!engine.hover_needs_style_state(nodes[0]));
        // NB: Nothing carries the key of the compound: an element read again would be found not to carry it.
        let carriers = engine.host.hover_seal.carriers.as_mut().expect("the seal answered");
        carriers.elements.insert(nodes[3]);
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, ".other { color: green; }");
        run_style_update(&mut engine, nodes[0]);
        assert!(engine.hover_needs_style_state(nodes[0]));
    }

    #[test]
    fn a_hover_seal_finds_the_elements_a_rule_added_later_may_match() {
        let (mut engine, nodes) = document_with_rules(".other { color: green; }");
        add_class(&mut engine, nodes[2], "menu");
        run_style_update(&mut engine, nodes[0]);
        assert!(!engine.hover_needs_style_state(nodes[0]));
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, ".menu:hover { color: red; }");
        run_style_update(&mut engine, nodes[0]);
        assert!(engine.hover_needs_style_state(nodes[0]));
    }

    #[test]
    fn a_hover_seal_lets_go_of_an_element_that_leaves() {
        let (mut engine, nodes) = document_with_rules(".menu:hover { color: red; }");
        add_class(&mut engine, nodes[3], "menu");
        run_style_update(&mut engine, nodes[0]);
        assert!(engine.hover_needs_style_state(nodes[0]));
        engine.record_tree_delta(nodes[3], Some(relations(Some(nodes[0]), None, Some(nodes[2]))), None);
        run_style_update(&mut engine, nodes[0]);
        assert!(!engine.hover_needs_style_state(nodes[0]));
    }
}
