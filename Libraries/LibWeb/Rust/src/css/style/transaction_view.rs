/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Physical views used while evaluating one normalized transaction.

use super::capacity::capacity_bytes;
use super::index::DispatchKey;
use super::index::StyleNodeFacts;
use super::selector::SiblingSequenceGeometry;
use super::tree::StyleNodeID;

const FEATURE_FLUX_PAGE_BITS: usize = u64::BITS as usize;

/// Transaction-local feature changes indexed by dense style-node identity.
///
/// The occupancy words retain the fast dense negative gate. A hit selects one flat node span
/// through a parallel page start and bit rank, so neither nodes nor their keys need a hash table or
/// per-page allocations.
#[derive(Clone, Copy)]
struct FeatureFluxNode {
    key_offset: u32,
    key_count: u32,
}

#[derive(Default)]
pub(super) struct FeatureFluxColumn {
    occupied: Vec<u64>,
    node_starts: Vec<u32>,
    nodes: Vec<FeatureFluxNode>,
    keys: Vec<DispatchKey>,
    nodes_by_key: Vec<(DispatchKey, StyleNodeID)>,
}

impl FeatureFluxColumn {
    pub(super) fn from_entries(mut entries: Vec<(StyleNodeID, DispatchKey)>) -> Self {
        entries.sort_unstable();
        entries.dedup();
        let mut nodes_by_key: Vec<_> = entries.iter().map(|&(node, key)| (key, node)).collect();
        nodes_by_key.sort_unstable();
        let mut features = Self {
            nodes_by_key,
            ..Self::default()
        };
        let mut start = 0;
        while start < entries.len() {
            let node = entries[start].0;
            let mut end = start + 1;
            while end < entries.len() && entries[end].0 == node {
                end += 1;
            }
            features.push_node(node, entries[start..end].iter().map(|&(_, key)| key));
            start = end;
        }
        features
    }

    fn push_node(&mut self, node: StyleNodeID, keys: impl IntoIterator<Item = DispatchKey>) {
        self.push(node.element_index().unwrap() as usize, keys);
    }

    fn push(&mut self, index: usize, keys: impl IntoIterator<Item = DispatchKey>) {
        let page_index = index / FEATURE_FLUX_PAGE_BITS;
        if self.occupied.len() <= page_index {
            self.occupied.resize(page_index + 1, 0);
            self.node_starts.resize(page_index + 1, 0);
        }
        let slot = index % FEATURE_FLUX_PAGE_BITS;
        let mask = 1_u64 << slot;
        let occupied = &mut self.occupied[page_index];
        assert_eq!(*occupied & mask, 0, "feature flux node inserted twice");
        assert!(
            *occupied < mask,
            "feature flux nodes must be inserted in identity order"
        );
        if *occupied == 0 {
            self.node_starts[page_index] = u32::try_from(self.nodes.len()).expect("feature flux node space exhausted");
        }
        *occupied |= mask;
        let key_offset = u32::try_from(self.keys.len()).expect("feature flux key space exhausted");
        self.keys.extend(keys);
        let key_end = u32::try_from(self.keys.len()).expect("feature flux key space exhausted");
        self.nodes.push(FeatureFluxNode {
            key_offset,
            key_count: key_end - key_offset,
        });
    }

    fn keys_of(&self, index: usize) -> &[DispatchKey] {
        let page_index = index / FEATURE_FLUX_PAGE_BITS;
        let Some(&occupied) = self.occupied.get(page_index) else {
            return &[];
        };
        let slot = index % FEATURE_FLUX_PAGE_BITS;
        let mask = 1_u64 << slot;
        if occupied & mask == 0 {
            return &[];
        }
        let rank = (occupied & mask.wrapping_sub(1)).count_ones() as usize;
        let node = self.nodes[self.node_starts[page_index] as usize + rank];
        let start = node.key_offset as usize;
        let end = start + node.key_count as usize;
        &self.keys[start..end]
    }

    pub(super) fn keys_of_node(&self, node: StyleNodeID) -> &[DispatchKey] {
        self.keys_of(node.element_index().unwrap() as usize)
    }

    #[cfg(test)]
    pub(super) fn all_nodes(&self, mut predicate: impl FnMut(StyleNodeID, &[DispatchKey]) -> bool) -> bool {
        for (page_index, &occupied) in self.occupied.iter().enumerate() {
            let mut remaining = occupied;
            let node_start = self.node_starts[page_index] as usize;
            for rank in 0..occupied.count_ones() as usize {
                let slot = remaining.trailing_zeros() as usize;
                remaining &= remaining - 1;
                let node = self.nodes[node_start + rank];
                let key_start = node.key_offset as usize;
                let key_end = key_start + node.key_count as usize;
                let index = page_index * FEATURE_FLUX_PAGE_BITS + slot;
                if !predicate(
                    StyleNodeID::element(u32::try_from(index).expect("element identity space exhausted")),
                    &self.keys[key_start..key_end],
                ) {
                    return false;
                }
            }
        }
        true
    }

    pub(super) fn all_nodes_with_key(&self, key: DispatchKey, mut predicate: impl FnMut(StyleNodeID) -> bool) -> bool {
        let start = self.nodes_by_key.partition_point(|&(candidate, _)| candidate < key);
        self.nodes_by_key[start..]
            .iter()
            .take_while(|&&(candidate, _)| candidate == key)
            .all(|&(_, node)| predicate(node))
    }

    pub(super) fn capacity_bytes(&self) -> usize {
        (capacity_bytes! {
            shallow [self.occupied, self.node_starts, self.nodes, self.keys, self.nodes_by_key];
            cached [];
            nested [];
            skip [];
        }) as usize
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum TransactionFactSide {
    Before,
    After,
}

/// The local facts and tree relations available while planning one transaction.
///
/// One semantic side remains in the resident arrangement. The other is a sparse batch grown to
/// the rows evaluation touches. Tree and program transactions currently plan after commit and
/// reconstruct their before side; eligible local facts plan before commit and materialize their
/// staged after side instead.
pub(super) struct TransactionFactView {
    pub(super) root: StyleNodeID,
    /// Every (node, key) local feature moved by this transaction, including all attribute name
    /// forms. Routing consults this delta beside the resident after-side facts so that neither a
    /// removed nor an added conjunct can reject the invalidation that will settle it.
    pub(super) moved_features: FeatureFluxColumn,
    pub(super) before_sibling_geometry: SiblingSequenceGeometry,
    pub(super) before_sibling_sequence_by_parent: Vec<(StyleNodeID, u32)>,
    pub(super) before_sibling_parents_by_sequence: Vec<StyleNodeID>,
    pub(super) before_absent_nodes: Vec<StyleNodeID>,
    pub(super) before_sibling_relations_available: bool,
    pub(super) prefix: Option<PrefixFactTransition>,
    pub(super) retained_truth_available: bool,
    /// Which semantic side is still resident in the engine's primary fact arrangement.
    pub(super) resident_side: TransactionFactSide,
    /// Whether both semantic sides share the resident local-fact arrangement.
    pub(super) local_facts_are_shared: bool,
    /// Shared before-side exact-evaluation batch when the after side is resident.
    pub(super) before: Option<StyleNodeFacts>,
    /// Shared after-side exact-evaluation batch when the before side is resident.
    pub(super) after: Option<StyleNodeFacts>,
    pub(super) opposite_fully_materialized: bool,
}

impl TransactionFactView {
    #[must_use]
    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.before_sibling_sequence_by_parent,
                self.before_sibling_parents_by_sequence,
                self.before_absent_nodes,
            ];
            cached [];
            nested [
                self.moved_features.capacity_bytes(),
                self.before_sibling_geometry.capacity_bytes(),
                self.prefix.as_ref().map_or(0, PrefixFactTransition::capacity_bytes),
                self.before.as_ref().map_or(0, StyleNodeFacts::capacity_bytes),
                self.after.as_ref().map_or(0, StyleNodeFacts::capacity_bytes),
            ];
            skip [
                self.root,
                self.before_sibling_relations_available,
                self.retained_truth_available,
                self.resident_side,
                self.local_facts_are_shared,
                self.opposite_fully_materialized,
            ];
        }
    }

    #[must_use]
    pub(super) fn facts<'a>(
        &'a self,
        side: TransactionFactSide,
        resident: &'a StyleNodeFacts,
    ) -> Option<&'a StyleNodeFacts> {
        if side == self.resident_side || self.local_facts_are_shared {
            return Some(resident);
        }
        match side {
            TransactionFactSide::Before => self.before.as_ref(),
            TransactionFactSide::After => self.after.as_ref(),
        }
    }
}

pub(super) struct PrefixFactTransition {
    pub(super) roots: Vec<StyleNodeID>,
    pub(super) departures: Vec<StyleNodeID>,
    pub(super) tree_relations_changed: bool,
    /// Live nodes whose rightward context a tree delta moved: reordered nodes, arrivals, and the
    /// nodes that followed an arrival, departure, or reorder in its old and new sequences. A
    /// sibling-bearing automaton walks these to convergence; a sibling-free one ignores them.
    pub(super) sibling_frontier: Vec<StyleNodeID>,
}

impl PrefixFactTransition {
    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.roots, self.departures, self.sibling_frontier];
            cached [];
            nested [];
            skip [self.tree_relations_changed];
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::index::StyleAtomID;
    use super::*;

    #[test]
    fn feature_flux_packs_element_pages() {
        let first_class = DispatchKey::Class(StyleAtomID(200));
        let second_class = DispatchKey::Class(StyleAtomID(201));
        let features = FeatureFluxColumn::from_entries(vec![
            (StyleNodeID::element(70), second_class),
            (StyleNodeID::element(1), first_class),
            (StyleNodeID::element(70), first_class),
            (StyleNodeID::element(70), first_class),
        ]);

        assert_eq!(features.keys_of_node(StyleNodeID::element(1)), &[first_class]);
        assert_eq!(
            features.keys_of_node(StyleNodeID::element(70)),
            &[first_class, second_class]
        );
        assert!(features.keys_of_node(StyleNodeID::element(69)).is_empty());
        let mut visited = Vec::new();
        assert!(features.all_nodes(|node, _| {
            visited.push(node);
            true
        }));
        assert_eq!(visited, [StyleNodeID::element(1), StyleNodeID::element(70)]);
        assert_eq!(features.nodes.len(), 2);
        assert_eq!(features.occupied, [2, 64]);
        assert_eq!(features.node_starts, [0, 1]);
        let mut first_class_nodes = Vec::new();
        assert!(features.all_nodes_with_key(first_class, |node| {
            first_class_nodes.push(node);
            true
        }));
        assert_eq!(first_class_nodes, [StyleNodeID::element(1), StyleNodeID::element(70)]);
        let mut second_class_nodes = Vec::new();
        assert!(features.all_nodes_with_key(second_class, |node| {
            second_class_nodes.push(node);
            true
        }));
        assert_eq!(second_class_nodes, [StyleNodeID::element(70)]);
    }

    #[test]
    fn an_unchanged_local_fact_arrangement_serves_both_semantic_sides() {
        let resident = StyleNodeFacts::new();
        let view = TransactionFactView {
            root: StyleNodeID::element(1),
            moved_features: FeatureFluxColumn::default(),
            before_sibling_geometry: SiblingSequenceGeometry::default(),
            before_sibling_sequence_by_parent: Vec::new(),
            before_sibling_parents_by_sequence: Vec::new(),
            before_absent_nodes: Vec::new(),
            before_sibling_relations_available: false,
            prefix: None,
            retained_truth_available: false,
            resident_side: TransactionFactSide::After,
            local_facts_are_shared: true,
            before: None,
            after: None,
            opposite_fully_materialized: false,
        };

        assert!(std::ptr::eq(
            view.facts(TransactionFactSide::Before, &resident).unwrap(),
            &resident
        ));
        assert!(std::ptr::eq(
            view.facts(TransactionFactSide::After, &resident).unwrap(),
            &resident
        ));
    }
}
