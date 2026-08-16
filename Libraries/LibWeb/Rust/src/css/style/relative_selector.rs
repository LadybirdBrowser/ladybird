/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Relational selector query descriptors, inverse anchor enumeration, and the retained witnesses.
//!
//! A relative selector matches a witness along one axis from an anchor. Runtime matching enumerates
//! candidates along that axis, while invalidation walks the relationship backwards from a changed
//! witness to enumerate only the anchors whose Boolean result could have changed. For a simple
//! query, matching also retains the one witness it found, which routing uses to prove that an
//! anchor's Boolean cannot have flipped and drop it from the plan.

use super::fast_hash::FastMap as HashMap;

use super::capacity::capacity_bytes;
use super::index::LocalFeatureKey;
use super::partial_view::Lookup;
use super::program::SelectorProgramID;
use super::selector::Incomplete;
#[cfg(test)]
use super::selector::InverseStep;
use super::selector::SelectorNodeID;
use super::tree::StyleNodeID;
use super::tree::StyleNodeTree;

define_id! {
    /// Identity of a compiled relative query within a selector program.
    pub struct RelativeQueryID(pub);
}

/// The single axis a simple relative selector traverses from its anchor.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum RelativeAxis {
    /// `:has(.error)`
    Descendant,
    /// `:has(> .item)`
    Child,
    /// `:has(+ .selected)`
    NextSibling,
    /// `:has(~ .selected)`
    FollowingSibling,
    /// `:has(+ .row .cell)`: the witness is somewhere under the anchor's next sibling.
    NextSiblingSubtree,
    /// `:has(~ .row .cell)`: the witness is somewhere under one of the following siblings.
    FollowingSiblingSubtree,
}

impl RelativeAxis {
    /// The inverse step from a possible witness back to its possible anchors.
    #[cfg(test)]
    #[must_use]
    pub fn inverse_step(self) -> InverseStep {
        match self {
            Self::Descendant => InverseStep::AnchorAncestors,
            Self::Child => InverseStep::AnchorParent,
            Self::NextSibling | Self::NextSiblingSubtree => InverseStep::AnchorPreviousSibling,
            Self::FollowingSibling | Self::FollowingSiblingSubtree => InverseStep::AnchorPrecedingSiblings,
        }
    }
}

/// A compiled relative query.
///
/// `driving_feature` is the positive indexable feature of the witness compound. Its presence is
/// part of what makes a query simple, because it is what a replacement search drives from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct RelativeQuery {
    pub axis: RelativeAxis,
    pub compound: SelectorNodeID,
    pub driving_feature: Option<LocalFeatureKey>,
    /// False for a query that needs the direct evaluator: a selector list, more than one axis, an
    /// internal combinator, a structural or nested relational operator, or a scope-crossing
    /// construct. Complex is not unsupported; it is exact and retains no witness.
    pub simple: bool,
    /// Whether the witness has to be strictly below the element the leading axis reaches.
    ///
    /// `:has(+ div .test)` names a `.test` under the anchor's next sibling, not a next sibling that
    /// is itself a `.test`. The chain compiles to constraints on the witness, which cannot say that
    /// on its own, so a selector whose every step after the leading one goes downwards says it here.
    /// One that goes sideways again - `:has(+ .a + .b)` - reaches a witness beside the axis element
    /// rather than below it, and does not.
    pub witness_is_below_the_axis: bool,
    /// Whether the argument reads the tree the anchor hosts rather than the anchor's surroundings.
    ///
    /// A `:has()` sharing its compound with `:host` was written in a shadow tree's own stylesheet
    /// and is anchored on that tree's host, which stands outside the tree. Its argument is a
    /// selector of that sheet, so it sees the tree's nodes and nothing around the host:
    /// `:host:has(.x)` asks whether the shadow tree holds an `.x`, and an `.x` sitting beside the
    /// host in the light DOM does not answer it. Every axis is therefore walked from the shadow
    /// root, which is where `:host > .x` crosses to as well.
    pub match_in_shadow_tree: bool,
}

impl RelativeQuery {
    /// A query is simple when one axis leads to one local compound with a positive indexable
    /// driving feature. Everything else is answered by the direct evaluator.
    #[must_use]
    pub fn is_simple(&self) -> bool {
        self.simple && self.driving_feature.is_some()
    }
}

/// The node a query's axis is actually walked from.
///
/// For an in-shadow-tree query that is the tree the anchor hosts rather than the anchor itself, and
/// a host that opens no tree witnesses nothing at all. Translating here rather than inside each walk
/// is what lets `candidate_witnesses` and the chain's own anchor tie stay as they are: both then
/// read the shadow root wherever they would have read the host.
#[must_use]
pub fn traversal_anchor(anchor: StyleNodeID, match_in_shadow_tree: bool, tree: &StyleNodeTree) -> Option<StyleNodeID> {
    match match_in_shadow_tree {
        true => tree.shadow_root_of(anchor),
        false => Some(anchor),
    }
}

/// Possible anchors for a newly matching witness of an in-shadow-tree query.
///
/// The axis was walked from a shadow root, so walking it back arrives at that root and the anchor is
/// whatever hosts it. A witness therefore has at most one anchor however wide the axis is, and only
/// when the root the axis reached back to is a hosted one.
pub fn possible_hosting_anchors(
    axis: RelativeAxis,
    witness: StyleNodeID,
    tree: &StyleNodeTree,
    mut visit: impl FnMut(StyleNodeID),
) {
    let root = match axis {
        RelativeAxis::Child => tree.parent(witness),
        // The walk stops at the top of the witness's own tree, which is what confines a descendant
        // query to the one tree the sheet was written for: a nested shadow root is not a DOM child
        // of anything in it, so no walk from inside one arrives here.
        RelativeAxis::Descendant => {
            let mut current = witness;
            while let Some(parent) = tree.parent(current) {
                current = parent;
            }
            Some(current)
        }
        // A sibling axis leaves the hosted tree, so such a query witnesses nothing and is compiled
        // away rather than routed.
        RelativeAxis::NextSibling
        | RelativeAxis::FollowingSibling
        | RelativeAxis::NextSiblingSubtree
        | RelativeAxis::FollowingSiblingSubtree => None,
    };
    if let Some(host) = root.and_then(|root| tree.host_of(root)) {
        visit(host);
    }
}

/// Possible anchors for a newly matching witness, per the inverse of the query's axis.
///
/// This is the bound that makes false-to-true cheap: a child query has exactly one possible anchor,
/// an adjacent-sibling query has exactly one, and a descendant query has the ancestor path.
pub fn possible_anchors(
    axis: RelativeAxis,
    witness: StyleNodeID,
    tree: &StyleNodeTree,
    scope_boundary: Option<StyleNodeID>,
    mut visit: impl FnMut(StyleNodeID),
) {
    match axis {
        RelativeAxis::Child => {
            if let Some(parent) = tree.parent(witness) {
                visit(parent);
            }
        }
        RelativeAxis::Descendant => {
            for ancestor in tree.ancestors(witness) {
                visit(ancestor);
                if Some(ancestor) == scope_boundary {
                    return;
                }
            }
        }
        RelativeAxis::NextSibling => {
            if let Some(previous) = tree.previous_element_sibling(witness) {
                visit(previous);
            }
        }
        RelativeAxis::FollowingSibling => {
            let Some(parent) = tree.parent(witness) else {
                return;
            };
            for sibling in tree.children(parent) {
                if sibling == witness {
                    return;
                }
                visit(sibling);
            }
        }
        // The witness is under a sibling rather than being one, so the element the anchor is a
        // sibling of is the witness or one of its ancestors. Each of those contributes the
        // siblings before it.
        RelativeAxis::NextSiblingSubtree => {
            let mut node = Some(witness);
            while let Some(current) = node {
                if let Some(previous) = tree.previous_element_sibling(current) {
                    visit(previous);
                }
                if Some(current) == scope_boundary {
                    return;
                }
                node = tree.parent(current);
            }
        }
        RelativeAxis::FollowingSiblingSubtree => {
            let mut node = Some(witness);
            while let Some(current) = node {
                if let Some(parent) = tree.parent(current) {
                    for sibling in tree.children(parent) {
                        if sibling == current {
                            break;
                        }
                        visit(sibling);
                    }
                }
                if Some(current) == scope_boundary {
                    return;
                }
                node = tree.parent(current);
            }
        }
    }
}

/// One anchor's observation of one simple relational query.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct RelationalWitnessKey {
    pub program: SelectorProgramID,
    pub query: RelativeQueryID,
    pub anchor: StyleNodeID,
}

/// The exact missing or invalid dependency that prevents a retained witness proof from being used.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RelationalWitnessGap {
    MissingEntry(RelationalWitnessKey),
    MissingQuery(RelationalWitnessKey),
    RetiredWitness {
        key: RelationalWitnessKey,
        witness: StyleNodeID,
    },
    UnreachableAnchor(RelationalWitnessKey),
    StaleAxis {
        key: RelationalWitnessKey,
        witness: StyleNodeID,
    },
    SelectorMismatch {
        key: RelationalWitnessKey,
        witness: StyleNodeID,
    },
    IncompleteFacts {
        key: RelationalWitnessKey,
        witness: StyleNodeID,
        incomplete: Incomplete,
    },
}

/// How many anchors may retain a witness at once. The table is a shortcut, not state, so hitting
/// the cap costs recomputation and nothing else.
const MAX_RETAINED_WITNESSES: usize = 16384;

/// The retained witnesses of simple relational queries: at most one per observed anchor.
///
/// An entry says that `witness` was seen to satisfy the query's compound on the query's axis from
/// `anchor` by an evaluation of the current tree, and that the anchor's last completed evaluation
/// of the query answered true. It is a proof cache, never semantic state: every read re-verifies
/// the claim against the tree and facts as they stand before trusting it, so dropping any entry at
/// any time is legal and costs only the shortcut.
///
/// Soundness rests on the write discipline, not on the reads: an entry is written only by an
/// evaluation of the live tree and current facts that found the witness, and is cleared by any
/// such evaluation that completed with no witness. An evaluation of an overlaid or hypothetical
/// state must never write here, because that is the one way to plant an entry whose "was true"
/// half is false - the read-side re-verification only re-establishes the "is true now" half.
pub struct RelationalWitnesses {
    entries: HashMap<RelationalWitnessKey, StyleNodeID>,
    admitting: bool,
}

impl Default for RelationalWitnesses {
    fn default() -> Self {
        Self {
            entries: HashMap::default(),
            admitting: true,
        }
    }
}

impl RelationalWitnesses {
    /// Retain `witness` for `key`, returning whether it was stored. A full table sheds the
    /// entries whose anchor or witness identity has been retired before refusing.
    pub fn retain(&mut self, key: RelationalWitnessKey, witness: StyleNodeID, tree: &StyleNodeTree) -> bool {
        if !self.admitting && !self.entries.contains_key(&key) {
            return false;
        }
        if self.entries.len() >= MAX_RETAINED_WITNESSES && !self.entries.contains_key(&key) {
            self.entries
                .retain(|entry, retained| tree.is_live(entry.anchor) && tree.is_live(*retained));
            if self.entries.len() >= MAX_RETAINED_WITNESSES {
                return false;
            }
        }
        self.entries.insert(key, witness);
        true
    }

    pub fn clear(&mut self, key: RelationalWitnessKey) {
        self.entries.remove(&key);
    }

    pub fn clear_all(&mut self) {
        self.entries = HashMap::default();
    }

    pub(super) fn set_admitting(&mut self, admitting: bool) {
        self.admitting = admitting;
    }

    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.entries];
            cached [];
            nested [];
            skip [self.admitting];
        }
    }
}

impl RelationalWitnesses {
    pub(super) fn lookup(&self, key: RelationalWitnessKey) -> Lookup<&StyleNodeID, RelationalWitnessGap> {
        match self.entries.get(&key) {
            Some(witness) => Lookup::Known(witness),
            None => Lookup::Missing(RelationalWitnessGap::MissingEntry(key)),
        }
    }
}

/// Candidate witnesses for one anchor, along the query's axis.
pub fn candidate_witnesses(
    axis: RelativeAxis,
    below_the_axis: bool,
    anchor: StyleNodeID,
    tree: &StyleNodeTree,
    mut visit: impl FnMut(StyleNodeID) -> bool,
) {
    match axis {
        RelativeAxis::Child => {
            for child in tree.children(anchor) {
                if !visit(child) {
                    return;
                }
            }
        }
        RelativeAxis::Descendant => {
            for node in tree.preorder(anchor) {
                if node == anchor {
                    continue;
                }
                if !visit(node) {
                    return;
                }
            }
        }
        RelativeAxis::NextSibling => {
            if let Some(next) = tree.next_element_sibling(anchor) {
                visit(next);
            }
        }
        RelativeAxis::FollowingSibling => {
            let mut current = tree.next_element_sibling(anchor);
            while let Some(sibling) = current {
                if !visit(sibling) {
                    return;
                }
                current = tree.next_element_sibling(sibling);
            }
        }
        // The witness lies under the sibling rather than being it, so the sibling's whole subtree
        // is the candidate range.
        RelativeAxis::NextSiblingSubtree => {
            if let Some(next) = tree.next_element_sibling(anchor) {
                for node in tree.preorder(next) {
                    if below_the_axis && node == next {
                        continue;
                    }
                    if !visit(node) {
                        return;
                    }
                }
            }
        }
        RelativeAxis::FollowingSiblingSubtree => {
            let mut current = tree.next_element_sibling(anchor);
            while let Some(sibling) = current {
                for node in tree.preorder(sibling) {
                    if below_the_axis && node == sibling {
                        continue;
                    }
                    if !visit(node) {
                        return;
                    }
                }
                current = tree.next_element_sibling(sibling);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::index::StyleAtomID;
    use super::super::memory::DeviceClass;
    use super::super::memory::MemoryController;
    use super::*;

    const ERROR_CLASS: LocalFeatureKey = LocalFeatureKey::Class(StyleAtomID(1));

    /// `root > [card > [a, b, c], other]`
    struct Fixture {
        tree: StyleNodeTree,
        nodes: Vec<StyleNodeID>,
    }

    impl Fixture {
        fn new() -> Self {
            let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
            let mut tree = StyleNodeTree::new(&mut memory);
            let nodes: Vec<StyleNodeID> = (0..6).map(|_| tree.allocate_element(&mut memory)).collect();
            // 0 root, 1 card, 2 other; card's children 3, 4, 5
            tree.set_first_element_child(nodes[0], Some(nodes[1]));
            tree.set_parent(nodes[1], Some(nodes[0]));
            tree.set_parent(nodes[2], Some(nodes[0]));
            tree.set_next_element_sibling(nodes[1], Some(nodes[2]));
            tree.set_previous_element_sibling(nodes[2], Some(nodes[1]));
            tree.set_first_element_child(nodes[1], Some(nodes[3]));
            for index in 3..6 {
                tree.set_parent(nodes[index], Some(nodes[1]));
                tree.set_next_element_sibling(nodes[index], nodes.get(index + 1).copied());
                tree.set_previous_element_sibling(nodes[index], (index > 3).then(|| nodes[index - 1]));
            }

            Self { tree, nodes }
        }

        fn anchors(&self, axis: RelativeAxis, witness: usize) -> Vec<usize> {
            let mut found = Vec::new();
            possible_anchors(axis, self.nodes[witness], &self.tree, None, |anchor| {
                found.push(self.nodes.iter().position(|node| *node == anchor).unwrap());
            });
            found
        }
    }

    #[test]
    fn a_child_query_has_exactly_one_possible_anchor() {
        let fixture = Fixture::new();
        assert_eq!(fixture.anchors(RelativeAxis::Child, 4), vec![1]);
    }

    #[test]
    fn an_adjacent_sibling_query_has_exactly_one_possible_anchor() {
        let fixture = Fixture::new();
        assert_eq!(fixture.anchors(RelativeAxis::NextSibling, 4), vec![3]);
        assert_eq!(
            fixture.anchors(RelativeAxis::NextSibling, 3),
            Vec::<usize>::new(),
            "a first child has no preceding sibling to anchor a + query"
        );
    }

    #[test]
    fn a_descendant_query_walks_the_ancestor_path() {
        let fixture = Fixture::new();
        assert_eq!(fixture.anchors(RelativeAxis::Descendant, 4), vec![1, 0]);
    }

    #[test]
    fn a_descendant_query_stops_at_its_scope_boundary() {
        let fixture = Fixture::new();
        let mut found = Vec::new();
        possible_anchors(
            RelativeAxis::Descendant,
            fixture.nodes[4],
            &fixture.tree,
            Some(fixture.nodes[1]),
            |anchor| found.push(anchor),
        );
        assert_eq!(found, vec![fixture.nodes[1]]);
    }

    #[test]
    fn a_following_sibling_query_anchors_on_preceding_siblings() {
        let fixture = Fixture::new();
        assert_eq!(fixture.anchors(RelativeAxis::FollowingSibling, 5), vec![3, 4]);
    }

    #[test]
    fn the_axis_inverses_are_the_ones_the_transpose_table_names() {
        assert_eq!(RelativeAxis::Child.inverse_step(), InverseStep::AnchorParent);
        assert_eq!(RelativeAxis::Descendant.inverse_step(), InverseStep::AnchorAncestors);
        assert_eq!(
            RelativeAxis::NextSibling.inverse_step(),
            InverseStep::AnchorPreviousSibling
        );
        assert_eq!(
            RelativeAxis::FollowingSibling.inverse_step(),
            InverseStep::AnchorPrecedingSiblings
        );
    }

    #[test]
    fn a_simple_query_needs_a_positive_driving_feature() {
        let with_feature = RelativeQuery {
            axis: RelativeAxis::Descendant,
            compound: SelectorNodeID(0),
            driving_feature: Some(ERROR_CLASS),
            simple: true,
            witness_is_below_the_axis: false,
            match_in_shadow_tree: false,
        };
        assert!(with_feature.is_simple());

        let without = RelativeQuery {
            driving_feature: None,
            ..with_feature
        };
        assert!(!without.is_simple(), "nothing to drive a replacement search from");

        let complex = RelativeQuery {
            simple: false,
            ..with_feature
        };
        assert!(!complex.is_simple());
    }

    #[test]
    fn an_evicted_relational_witness_is_a_typed_missing_entry() {
        let fixture = Fixture::new();
        let key = RelationalWitnessKey {
            program: SelectorProgramID(1),
            query: RelativeQueryID(2),
            anchor: fixture.nodes[1],
        };
        let witness = fixture.nodes[4];
        let mut witnesses = RelationalWitnesses::default();

        assert!(matches!(
            witnesses.lookup(key),
            Lookup::Missing(RelationalWitnessGap::MissingEntry(missing)) if missing == key
        ));
        assert!(witnesses.retain(key, witness, &fixture.tree));
        assert!(matches!(witnesses.lookup(key), Lookup::Known(&retained) if retained == witness));

        witnesses.clear_all();
        assert!(matches!(
            witnesses.lookup(key),
            Lookup::Missing(RelationalWitnessGap::MissingEntry(missing)) if missing == key
        ));
    }
}
