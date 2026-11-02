/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The writes a document's host makes to the projection of its elements, as the DOM changes: an element arriving with
//! its facts, a fact or a state moving, an element taking its place in the tree or leaving it.
//!
//! Every such write is the host's writer's alone, made on the host's thread as the DOM changes, and reaches the render
//! owner only as the writer publishes its rows at a barrier. What a transaction changed is read back from the published
//! rows, so a write here records no input beyond its count: the projection is the whole of what the host publishes
//! about its elements' facts and relations.

use super::bridge::ElementBoxKind;
use super::index::AttributeFact;
use super::index::FeatureValue;
use super::index::LocalFeatureKey;
use super::index::StateSet;
use super::index::StyleAtomID;
use super::input_types::InputKey;
use super::input_types::StateFact;
use super::input_types::TreeRelations;
use super::instrumentation::Counter;
use super::pipeline::projection::ArrivalFacts;
use super::pipeline::projection::ProjectionWriter;
use super::tree::StyleNodeID;

/// The host's writer of a document's projection, with what its writes mean.
pub(in crate::css::style) struct ProjectionWrites<'a> {
    pub(in crate::css::style) projection: &'a mut ProjectionWriter,
}

/// What one element arrives with beside its tree relations, every fact of it its selectors test.
pub(in crate::css::style) struct ElementArrivalFacts<'a> {
    pub(in crate::css::style) tag: StyleAtomID,
    pub(in crate::css::style) folded_tag: StyleAtomID,
    pub(in crate::css::style) namespace: StyleAtomID,
    pub(in crate::css::style) id: StyleAtomID,
    pub(in crate::css::style) language: StyleAtomID,
    pub(in crate::css::style) directionality: StyleAtomID,
    pub(in crate::css::style) heading_level: u8,
    pub(in crate::css::style) backing_pseudo_kind_plus_one: u8,
    pub(in crate::css::style) is_slot: bool,
    pub(in crate::css::style) has_text_content: bool,
    pub(in crate::css::style) states: StateSet,
    /// What the host publishes of the element beside the facts its selectors test.
    pub(in crate::css::style) adjustment_facts: u32,
    pub(in crate::css::style) construction_facts: u32,
    pub(in crate::css::style) box_kind: ElementBoxKind,
    /// The element's custom states, then its classes, then its attributes as name and value atom pairs.
    pub(in crate::css::style) atoms: &'a [u32],
    pub(in crate::css::style) custom_state_count: usize,
    pub(in crate::css::style) class_count: usize,
}

impl ProjectionWrites<'_> {
    /// Count one input the projection says, as the transaction's routing reads it from the rows.
    fn count(&self, key: InputKey) {
        let counters = &self.projection.counters;
        counters.bump(Counter::RawMutationRecords);
        if let Some(counter) = key.counter() {
            counters.bump(counter);
        }
        if let InputKey::LocalFeature(node, _) | InputKey::State(node, _) = key
            && self.projection.arrival_is_pending(node)
        {
            // Every fact of an arriving element is said by its arrival.
            counters.bump(Counter::ArrivingNodeFactsFolded);
        }
    }

    // -- Identities ----------------------------------------------------------------------------

    /// Make the element identities of `nodes`, which the host minted, live, with no links yet.
    pub(in crate::css::style) fn mint(&mut self, nodes: impl IntoIterator<Item = StyleNodeID>) {
        for node in nodes {
            if node.element_index().is_some() {
                self.projection.mint(node);
            }
        }
    }

    // -- Tree ----------------------------------------------------------------------------------

    /// Write a structural change into the projection. The relation tree settles it at the next transaction barrier.
    pub(in crate::css::style) fn record_tree_delta(
        &mut self,
        node: StyleNodeID,
        old: Option<TreeRelations>,
        new: Option<TreeRelations>,
    ) {
        if old != new {
            self.stage_tree_delta(node, old, new);
        }
    }

    /// Stage one structural delta and the neighbor rows it derives.
    fn stage_tree_delta(&mut self, node: StyleNodeID, old: Option<TreeRelations>, new: Option<TreeRelations>) {
        if let Some(old) = old {
            if let Some(previous) = old.previous_element_sibling {
                self.stage_connected_tree_row(previous, |relations| {
                    relations.next_element_sibling = old.next_element_sibling;
                });
            } else if let Some(parent) = old.parent {
                self.stage_first_child(parent, old.next_element_sibling);
            }
            if let Some(next) = old.next_element_sibling {
                self.stage_connected_tree_row(next, |relations| {
                    relations.previous_element_sibling = old.previous_element_sibling;
                });
            }
        }
        if let Some(new) = new {
            if let Some(previous) = new.previous_element_sibling {
                self.stage_connected_tree_row(previous, |relations| {
                    relations.next_element_sibling = Some(node);
                });
            } else if let Some(parent) = new.parent {
                self.stage_first_child(parent, Some(node));
            }
            if let Some(next) = new.next_element_sibling {
                self.stage_connected_tree_row(next, |relations| {
                    relations.previous_element_sibling = Some(node);
                });
            }
        }
        self.stage_tree_row(node, old, new);
    }

    #[inline]
    fn stage_tree_row(
        &mut self,
        node: StyleNodeID,
        old_if_unwritten: Option<TreeRelations>,
        new: Option<TreeRelations>,
    ) {
        let old = match self.projection.relations_were_written(node) {
            true => self.projection.written_relations(node),
            false => old_if_unwritten,
        };
        if old == new {
            return;
        }
        self.count(InputKey::TreeRelations(node));
        if old.is_some() && new.is_none() {
            self.projection.counters.bump(Counter::TreeDepartureDeltas);
        }
        self.projection.write_relations(node, new);
    }

    fn stage_connected_tree_row(&mut self, node: StyleNodeID, update: impl FnOnce(&mut TreeRelations)) {
        let Some(old) = self.projection.written_relations(node) else {
            debug_assert!(
                !self.projection.relations_were_written(node),
                "a pending neighbor must remain connected"
            );
            return;
        };
        // A subtree can preallocate sibling identities before publishing their insertions. Such
        // a neighbor has no parent yet; its own insertion will supply its links. Synthesizing a
        // connected before-row here would hide that arrival from transaction normalization.
        if old.parent.is_none() {
            return;
        }
        let mut new = old;
        update(&mut new);
        self.stage_tree_row(node, Some(old), Some(new));
    }

    pub(in crate::css::style) fn stage_first_child(&mut self, parent: StyleNodeID, child: Option<StyleNodeID>) {
        self.projection.set_first_element_child(parent, child);
    }

    /// Record that `host` hosts `shadow_root`, answering whether the host takes a root it did not have.
    pub(in crate::css::style) fn set_shadow_root(&mut self, host: StyleNodeID, shadow_root: StyleNodeID) -> bool {
        let attached = self.projection.shadow_root_of(host) != Some(shadow_root);
        self.projection.set_shadow_root(host, shadow_root);
        attached
    }

    /// Replace the ordered list of nodes a slot has assigned to it, text nodes included.
    pub(in crate::css::style) fn set_slot_assigned_nodes(&mut self, slot: StyleNodeID, nodes: &[StyleNodeID]) {
        self.projection.counters.bump(Counter::SlotAssignedNodeListsPublished);
        self.projection.set_assigned_nodes(slot, nodes);
    }

    /// Record what a language atom spells, so `:lang()` can compare its ranges against the tag.
    pub(in crate::css::style) fn set_language_text(&mut self, language: StyleAtomID, text: &[u16]) {
        self.projection.counters.bump(Counter::LanguageTextsPublished);
        self.projection.set_language_text(language, text);
    }

    // -- Facts ---------------------------------------------------------------------------------

    /// Write every fact an element arrives with in one edit of its row.
    pub(in crate::css::style) fn record_element_arrival(
        &mut self,
        node: StyleNodeID,
        arrival: &ElementArrivalFacts<'_>,
    ) {
        let (custom_states, rest) = arrival.atoms.split_at(arrival.custom_state_count);
        let (classes, attributes) = rest.split_at(arrival.class_count);
        let custom_states = StyleAtomID::slice_of_raw(custom_states);
        let classes = StyleAtomID::slice_of_raw(classes);
        let attributes = AttributeFact::slice_of_raw(attributes.as_chunks::<2>().0);
        // NB: Each fact the arrival carries counts as a delta folded onto it, as each was once published apiece.
        let folded = 2
            + u64::from(!arrival.id.is_none())
            + u64::from(!arrival.language.is_none())
            + u64::from(!arrival.directionality.is_none())
            + u64::from(arrival.heading_level != 0)
            + u64::from(arrival.folded_tag != arrival.tag)
            + (custom_states.len() + classes.len() + attributes.len()) as u64
            + u64::from(arrival.states.0.count_ones());
        self.projection.counters.add(Counter::ArrivingNodeFactsFolded, folded);
        self.projection.counters.add(Counter::RawMutationRecords, folded);
        self.projection.counters.add(Counter::LocalFeatureDeltas, folded);
        self.projection.write_arrival_facts(
            node,
            &ArrivalFacts {
                tag: arrival.tag,
                folded_tag: arrival.folded_tag,
                namespace: arrival.namespace,
                id: arrival.id,
                language: arrival.language,
                directionality: arrival.directionality,
                heading_level: arrival.heading_level,
                is_slot: arrival.is_slot,
                has_text_content: arrival.has_text_content,
                states: arrival.states,
                classes,
                attributes,
                custom_states,
            },
        );
        self.projection.edit_host_facts(node, |set| {
            set.adjustment_facts = arrival.adjustment_facts;
            set.construction_facts = arrival.construction_facts;
            set.set_box_kind(arrival.box_kind);
            set.associated_pseudo_kind_plus_one = arrival.backing_pseudo_kind_plus_one;
        });
    }

    /// Replace the element facts the style computation's adjustments read, answering the bits that moved.
    pub(in crate::css::style) fn set_element_adjustment_facts(&mut self, node: StyleNodeID, facts: u32) -> u32 {
        let previous = self
            .projection
            .edit_host_facts(node, |set| set.adjustment_facts = facts);
        previous.adjustment_facts ^ facts
    }

    /// Replace the element facts a layout row built for the element records.
    pub(in crate::css::style) fn set_element_construction_facts(&mut self, node: StyleNodeID, facts: u32) {
        self.projection
            .edit_host_facts(node, |set| set.construction_facts = facts);
    }

    /// Record which principal box the element asks for, answering whether that moved.
    pub(in crate::css::style) fn set_element_box_kind(&mut self, node: StyleNodeID, box_kind: ElementBoxKind) -> bool {
        self.projection
            .edit_host_facts(node, |set| set.set_box_kind(box_kind))
            .box_kind()
            != box_kind
    }

    /// Record one local fact of `node` moving from `old` to `new`, which an arriving element's arrival says.
    pub(in crate::css::style) fn record_feature(
        &mut self,
        node: StyleNodeID,
        feature: LocalFeatureKey,
        new: FeatureValue,
        arriving_node: bool,
    ) {
        if arriving_node {
            self.projection.counters.bump(Counter::ArrivingNodeFactsFolded);
            self.projection.counters.bump(Counter::RawMutationRecords);
            self.projection.counters.bump(Counter::LocalFeatureDeltas);
        } else {
            self.count(InputKey::LocalFeature(node, feature));
        }
        self.apply_feature(node, feature, new);
    }

    /// Record a state publication against the fact the projection holds, answering whether the state moved.
    ///
    /// State invalidators may conservatively republish a related group of pseudo-classes when one
    /// member changes. The written fact is therefore the old side; assuming every publication is
    /// a Boolean toggle would invent transitions for the unchanged members of that group. Such a
    /// member is no input at all.
    pub(in crate::css::style) fn record_state(
        &mut self,
        node: StyleNodeID,
        fact: StateFact,
        new_value: bool,
        arriving_node: bool,
    ) -> bool {
        let old_value = self
            .projection
            .written_facts(node)
            .is_some_and(|facts| facts.states().contains(fact));
        if old_value == new_value && !arriving_node {
            return false;
        }
        if arriving_node {
            self.projection.counters.bump(Counter::ArrivingNodeFactsFolded);
            self.projection.counters.bump(Counter::RawMutationRecords);
            self.projection.counters.bump(Counter::LocalFeatureDeltas);
        } else {
            self.count(InputKey::State(node, fact));
        }
        self.projection.set_state(node, fact, new_value);
        true
    }

    /// Write one local fact of `node` into its row.
    pub(in crate::css::style) fn apply_feature(
        &mut self,
        node: StyleNodeID,
        feature: LocalFeatureKey,
        value: FeatureValue,
    ) {
        let atom = |value: FeatureValue| match value {
            FeatureValue::Atom(atom) => atom,
            _ => StyleAtomID::NONE,
        };
        let projection = &mut *self.projection;
        match feature {
            LocalFeatureKey::TagName => {
                if let FeatureValue::Atom(atom) = value {
                    projection.set_tag(node, atom);
                }
            }
            LocalFeatureKey::PartExposure => projection.set_part_exposure(node, atom(value)),
            LocalFeatureKey::Language => projection.set_language(node, atom(value)),
            LocalFeatureKey::Directionality => projection.set_directionality(node, atom(value)),
            LocalFeatureKey::HeadingLevel => {
                let level = match value {
                    FeatureValue::Number(level) => level as u8,
                    _ => 0,
                };
                projection.set_heading_level(node, level);
            }
            LocalFeatureKey::FoldedTagName => projection.set_folded_tag(node, atom(value)),
            // Parts and custom states are published as complete sets after their individual
            // deltas have been recorded. Arrival is only a routing key.
            LocalFeatureKey::Part(_) | LocalFeatureKey::CustomState(_) => {}
            // A text node is not a style node, so nothing in the tree can say it is there.
            // `Present` on this key means the element is empty.
            LocalFeatureKey::Emptiness => projection.set_has_text_content(node, !value.holds()),
            LocalFeatureKey::Id => projection.set_id(node, atom(value)),
            LocalFeatureKey::Class(class) => projection.set_class(node, class, value.holds()),
            LocalFeatureKey::Attribute(name) => {
                // The value's atom rides on the same delta. Presence is what routing reads; the
                // value is what an exact test compares, and a cold pass has no DOM to ask.
                projection.set_attribute(node, name, atom(value), value.holds());
            }
        }
    }

    /// Record the shadow parts an element exposes.
    ///
    /// A part is a fact about the element like a class is: posted so `::part()` rules can be
    /// enumerated from it, and recorded so a change to the `part` attribute routes. The plain
    /// name set is derived here from the name-to-host pairs exact matching needs, so the two views
    /// cannot disagree.
    pub(in crate::css::style) fn set_element_parts(&mut self, node: StyleNodeID, pairs: &[(StyleAtomID, StyleNodeID)]) {
        let mut parts = Vec::new();
        for &(part, _) in pairs {
            if !parts.contains(&part) {
                parts.push(part);
            }
        }
        let written = self
            .projection
            .written_facts(node)
            .map_or(&[][..], |facts| facts.parts());
        if written != parts {
            let previous: Vec<StyleAtomID> = written.to_vec();
            for part in previous.iter().filter(|part| !parts.contains(part)) {
                self.count(InputKey::LocalFeature(node, LocalFeatureKey::Part(*part)));
            }
            for part in parts.iter().filter(|part| !previous.contains(part)) {
                self.count(InputKey::LocalFeature(node, LocalFeatureKey::Part(*part)));
            }
            self.projection.set_parts(node, &parts);
        }
        self.projection.set_part_hosts(node, pairs);
    }

    /// Write one atom-valued local fact of `node`, which says nothing where it holds that value already.
    fn set_atom_fact(
        &mut self,
        node: StyleNodeID,
        feature: LocalFeatureKey,
        previous: StyleAtomID,
        value: StyleAtomID,
    ) {
        if previous == value {
            return;
        }
        self.count(InputKey::LocalFeature(node, feature));
        let value = match value.is_none() {
            true => FeatureValue::Absent,
            false => FeatureValue::Atom(value),
        };
        self.apply_feature(node, feature, value);
    }

    /// Report the outermost host a `::part()` rule can address this element from.
    ///
    /// What an `exportparts` change moves is which scopes can name an element, not which names it
    /// carries - the forwarded name is usually the one it already had. So the exposure is the fact,
    /// and an element whose reach did not move says nothing.
    pub(in crate::css::style) fn set_element_part_exposure(&mut self, node: StyleNodeID, exposure: StyleAtomID) {
        let previous = self
            .projection
            .written_facts(node)
            .map_or(StyleAtomID::NONE, |facts| facts.part_exposure());
        self.set_atom_fact(node, LocalFeatureKey::PartExposure, previous, exposure);
    }

    /// Record the element-backed pseudo-element kind of its host the element stands for, one plus
    /// the kind, or zero for none.
    pub(in crate::css::style) fn set_element_backing_pseudo_kind(
        &mut self,
        node: StyleNodeID,
        pseudo_kind_plus_one: u8,
    ) {
        self.projection
            .edit_host_facts(node, |set| set.associated_pseudo_kind_plus_one = pseudo_kind_plus_one);
    }

    pub(in crate::css::style) fn set_element_heading_level(&mut self, node: StyleNodeID, level: u8) {
        let previous = self
            .projection
            .written_facts(node)
            .map_or(0, |facts| facts.heading_level());
        if previous == level {
            return;
        }
        self.count(InputKey::LocalFeature(node, LocalFeatureKey::HeadingLevel));
        self.apply_feature(
            node,
            LocalFeatureKey::HeadingLevel,
            FeatureValue::Number(u32::from(level)),
        );
    }

    pub(in crate::css::style) fn set_element_language(&mut self, node: StyleNodeID, language: StyleAtomID) {
        let previous = self
            .projection
            .written_facts(node)
            .map_or(StyleAtomID::NONE, |facts| facts.language());
        self.set_atom_fact(node, LocalFeatureKey::Language, previous, language);
    }

    /// Report the element's resolved directionality, which `:dir()` tests.
    pub(in crate::css::style) fn set_element_directionality(&mut self, node: StyleNodeID, directionality: StyleAtomID) {
        let previous = self
            .projection
            .written_facts(node)
            .map_or(StyleAtomID::NONE, |facts| facts.directionality());
        self.set_atom_fact(node, LocalFeatureKey::Directionality, previous, directionality);
    }

    /// Replace the custom states an element is in.
    ///
    /// A custom state is a named fact about one element, exactly like a class, so it is published as
    /// one: the names that arrived and the names that left are each a local feature moving, and
    /// `:state()` reaches its subjects through the same postings every other name does.
    pub(in crate::css::style) fn set_element_custom_states(&mut self, node: StyleNodeID, states: &[StyleAtomID]) {
        let written = self
            .projection
            .written_facts(node)
            .map_or(&[][..], |facts| facts.custom_states());
        if written == states {
            return;
        }
        let previous: Vec<StyleAtomID> = written.to_vec();
        for state in previous.iter().filter(|state| !states.contains(state)) {
            self.count(InputKey::LocalFeature(node, LocalFeatureKey::CustomState(*state)));
        }
        for state in states.iter().filter(|state| !previous.contains(state)) {
            self.count(InputKey::LocalFeature(node, LocalFeatureKey::CustomState(*state)));
        }
        self.projection.set_custom_states(node, states);
    }
}
