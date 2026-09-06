/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Impact regions: the transaction-local execution plan for one style flush.
//!
//! An impact region is the normalized union of everything the transaction's transpose programs can
//! reach. It is a plan, never a retained element-by-selector relation, and it exists to answer one
//! question: between the last committed state and the final live state, which style nodes can have
//! a different semantic output?
//!
//! The obligation is one-sided. A region may be wider than the truth, because every subject it
//! yields is checked exactly before anything downstream is emitted. It may never be narrower, so an
//! operator that cannot construct a proven region widens to the document. Unknown scope is never
//! treated as empty scope.
//!
//! Regions carry the tree relation they were named with. Coalescing two overlapping regions keeps
//! that relation rather than flattening both into a node set, because the relation is what a later
//! stage needs in order to enumerate the region without materializing it.

use super::capacity::capacity_bytes;
use super::column::BitColumn;
use super::column::PagedColumn;
use super::column::PagedColumnPage;
use super::instrumentation::Counter;
use super::instrumentation::Counters;
use super::program::EntryID;
use super::program::RuleID;
use super::selector::InverseStep;
use super::tree::StyleNodeID;
use super::tree::StyleNodeTree;
use super::tree::TreeScopeID;

/// One region of the impact plan, named by the tree relation that produced it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum ImpactRegion {
    /// No style node at all. A relation step can prove this: a host with no shadow root hosts
    /// nothing, and an unslotted node reaches no slot. Saying so is what keeps a route that leads
    /// nowhere from being indistinguishable from one that could not be proven.
    Empty,
    /// Exactly one style node.
    Node(StyleNodeID),
    /// A node's element children.
    Children(StyleNodeID),
    /// A node and everything below it.
    Subtree(StyleNodeID),
    /// Everything below a node, but not the node.
    ///
    /// `:host *` describes the host's descendants and not the host, and the difference is the whole
    /// of what such a rule reaches when the host is the only element it would otherwise add.
    StrictSubtree(StyleNodeID),
    /// The immediately following element sibling of a node.
    NextSibling(StyleNodeID),
    /// Every following element sibling of a node.
    FollowingSiblings(StyleNodeID),
    /// Every following element sibling of a node and each sibling's subtree.
    ///
    /// This bounds the forest produced by a sibling combinator followed by a descendant or child
    /// combinator. Keeping the forest named avoids widening it to the parent's whole subtree,
    /// which would include the origin and every preceding sibling as well.
    FollowingSiblingSubtrees(StyleNodeID),
    /// The whole child sequence a node belongs to.
    SiblingSequence(StyleNodeID),
    /// A node's ancestors: the possible anchors of a descendant `:has()` query.
    Ancestors(StyleNodeID),
    /// The immediately preceding element sibling: the anchor of an adjacent-sibling query.
    PreviousSibling(StyleNodeID),
    /// The preceding element siblings: the anchors of a following-sibling query.
    PrecedingSiblings(StyleNodeID),
    /// Every node of every shadow tree nested at or below the one a host hosts.
    ///
    /// Named by the host rather than by its shadow root, because what bounds the region is how many
    /// shadow roots stand between a node and that host: a `::part()` name forwarded outwards by
    /// `exportparts` crosses one per level, and the host's own light-DOM descendants cross none and
    /// are not in the region at all.
    HostedSubtrees(StyleNodeID),
    /// A complete tree scope, when no narrower region could be proven.
    TreeScope(TreeScopeID),
    /// The whole document, the widest an operator can go.
    Document,
}

impl ImpactRegion {
    /// Widen a region by one inverse relation step.
    ///
    /// Each case answers "where can this relation reach, starting from everything already in the
    /// region". Sibling steps applied to a region wider than a single node escape upwards, because
    /// the siblings of a subtree's root lie outside that subtree.
    #[must_use]
    pub fn step(self, step: InverseStep, tree: &StyleNodeTree) -> Self {
        match (self, step) {
            (Self::Empty, _) => Self::Empty,
            (Self::Document, _) => Self::Document,
            (Self::TreeScope(scope), _) => Self::TreeScope(scope),

            (Self::StrictSubtree(node), _) => Self::Subtree(node).step(step, tree),
            (Self::Node(node), InverseStep::Descendants) => Self::Subtree(node),
            (Self::Node(node), InverseStep::Children) => Self::Children(node),
            (Self::Node(node), InverseStep::NextSibling) => Self::NextSibling(node),
            (Self::Node(node), InverseStep::FollowingSiblings) => Self::FollowingSiblings(node),
            (Self::Node(node), InverseStep::SiblingSequence) => Self::SiblingSequence(node),

            // Relational anchors: from a possible witness back to the elements whose `:has()`
            // Boolean it can flip.
            (Self::Node(node), InverseStep::AnchorParent) => match tree.parent(node) {
                Some(parent) => Self::Node(parent),
                None => Self::Node(node),
            },
            (Self::Node(node), InverseStep::AnchorAncestors) => Self::Ancestors(node),
            (Self::Node(node), InverseStep::AnchorPreviousSibling) => Self::PreviousSibling(node),
            (Self::Node(node), InverseStep::AnchorPrecedingSiblings) => Self::PrecedingSiblings(node),

            // From anything wider, an anchor step can reach any ancestor of the region, so it
            // widens to the enclosing scope rather than guessing a narrower one.
            (
                _,
                InverseStep::AnchorParent
                | InverseStep::AnchorAncestors
                | InverseStep::AnchorPreviousSibling
                | InverseStep::AnchorPrecedingSiblings,
            ) => Self::Document,

            // Children of a node's children, and everything below them, stay inside its subtree.
            (Self::Children(node), InverseStep::Descendants | InverseStep::Children) => Self::Subtree(node),
            (Self::Subtree(node), InverseStep::Descendants | InverseStep::Children) => Self::Subtree(node),

            // A sibling step from anywhere inside a subtree can leave it through the root, so it
            // widens to the parent's subtree.
            (
                Self::Children(node) | Self::Subtree(node),
                InverseStep::NextSibling | InverseStep::FollowingSiblings | InverseStep::SiblingSequence,
            ) => match tree.parent(node) {
                Some(parent) => Self::Subtree(parent),
                None => Self::Subtree(node),
            },

            // The sibling-shaped regions are already a set of nodes at one level; widening them
            // means treating that level as the starting set.
            (Self::NextSibling(node), InverseStep::Descendants | InverseStep::Children) => {
                match tree.next_element_sibling(node) {
                    Some(next) => Self::Subtree(next),
                    None => Self::Empty,
                }
            }
            (Self::FollowingSiblings(node), InverseStep::Descendants | InverseStep::Children) => {
                Self::FollowingSiblingSubtrees(node)
            }
            (Self::SiblingSequence(node), InverseStep::Descendants | InverseStep::Children) => {
                match tree.parent(node) {
                    Some(parent) => Self::Subtree(parent),
                    None => Self::Subtree(node),
                }
            }
            (
                Self::NextSibling(node) | Self::FollowingSiblings(node) | Self::SiblingSequence(node),
                InverseStep::NextSibling | InverseStep::FollowingSiblings | InverseStep::SiblingSequence,
            ) => Self::SiblingSequence(node),

            (
                Self::FollowingSiblingSubtrees(node),
                InverseStep::Descendants
                | InverseStep::Children
                | InverseStep::NextSibling
                | InverseStep::FollowingSiblings,
            ) => Self::FollowingSiblingSubtrees(node),
            (Self::FollowingSiblingSubtrees(node), InverseStep::SiblingSequence) => match tree.parent(node) {
                Some(parent) => Self::Subtree(parent),
                None => Self::Subtree(node),
            },

            // A host reaches the tree it hosts, and the shadow root's own identity is what makes
            // that a subtree rather than a boundary. A host with no shadow root hosts nothing, so
            // the step reaches nothing at all rather than everything.
            (Self::Node(node), InverseStep::HostedTree) => match tree.shadow_root_of(node) {
                Some(shadow_root) => Self::Subtree(shadow_root),
                None => Self::Empty,
            },

            // A slotted node reaches its slot, and only its slot.
            (Self::Node(node), InverseStep::SlotAssignment) => match tree.assigned_slot_of(node) {
                Some(slot) => Self::Node(slot),
                None => Self::Empty,
            },

            // Assigned elements are light-DOM children of the shadow host. This region can include
            // unassigned children, which exact matching filters, but can never omit an assignee.
            (Self::Node(node), InverseStep::SlotAssignees) => match tree.shadow_host_of(node) {
                Some(host) => Self::Children(host),
                None => Self::Empty,
            },

            // A host reaches every tree nested below the one it hosts, because `exportparts` carries
            // a part name outwards through any number of hosts.
            (Self::Node(node), InverseStep::HostedTrees) => match tree.shadow_root_of(node) {
                Some(_) => Self::HostedSubtrees(node),
                None => Self::Empty,
            },

            // From a region wider than one node the relation is a set lookup per member, which the
            // plan cannot enumerate without walking the region it was trying to avoid.
            (
                _,
                InverseStep::HostedTree
                | InverseStep::HostedTrees
                | InverseStep::SlotAssignment
                | InverseStep::SlotAssignees,
            ) => Self::Document,

            // The anchor-shaped regions already span an ancestor path or a sibling range, and any
            // further step from them can leave whatever subtree bounded them. A region spanning
            // nested shadow trees is bounded by no single subtree either.
            (
                Self::Ancestors(_) | Self::PreviousSibling(_) | Self::PrecedingSiblings(_) | Self::HostedSubtrees(_),
                _,
            ) => Self::Document,
        }
    }

    /// Apply a whole transpose path, in application order.
    #[must_use]
    pub fn follow(node: StyleNodeID, path: &[InverseStep], tree: &StyleNodeTree) -> Self {
        path.iter()
            .fold(Self::Node(node), |region, &step| region.step(step, tree))
    }

    /// Whether this region contains everything `other` does.
    #[must_use]
    fn contains_with_topology(self, other: Self, tree: &StyleNodeTree, topology: Option<&TransactionTopology>) -> bool {
        if self == other {
            return true;
        }
        if other == Self::Empty {
            return true;
        }
        match self {
            Self::Empty => false,
            Self::Document => true,
            Self::Subtree(root) => match other {
                Self::Node(node) | Self::Subtree(node) | Self::StrictSubtree(node) => topology
                    .and_then(|topology| topology.is_in_subtree_of(node, root))
                    .unwrap_or_else(|| tree.is_in_subtree_of(node, root)),
                Self::Children(node) => topology
                    .and_then(|topology| topology.is_in_subtree_of(node, root))
                    .unwrap_or_else(|| tree.is_in_subtree_of(node, root)),
                _ => false,
            },
            Self::StrictSubtree(root) => match other {
                Self::Node(node) | Self::Subtree(node) | Self::StrictSubtree(node) => {
                    node != root
                        && topology
                            .and_then(|topology| topology.is_in_subtree_of(node, root))
                            .unwrap_or_else(|| tree.is_in_subtree_of(node, root))
                }
                Self::Children(node) => topology
                    .and_then(|topology| topology.is_in_subtree_of(node, root))
                    .unwrap_or_else(|| tree.is_in_subtree_of(node, root)),
                _ => false,
            },
            Self::FollowingSiblingSubtrees(_) => match other {
                Self::Node(node) | Self::Subtree(node) | Self::StrictSubtree(node) => {
                    self.contains_node_with_topology(node, tree, topology)
                }
                Self::Children(node) => self.contains_node_with_topology(node, tree, topology),
                _ => false,
            },
            _ => false,
        }
    }

    /// Whether `node` lies in this region.
    ///
    /// Proving membership walks the named relation, which is what a selective plan pays per
    /// candidate in exchange for not streaming the whole region.
    #[must_use]
    pub fn contains_node(self, node: StyleNodeID, tree: &StyleNodeTree) -> bool {
        self.contains_node_with_topology(node, tree, None)
    }

    #[must_use]
    fn contains_node_with_topology(
        self,
        node: StyleNodeID,
        tree: &StyleNodeTree,
        topology: Option<&TransactionTopology>,
    ) -> bool {
        match self {
            Self::Empty => false,
            Self::Node(other) => node == other,
            Self::Children(parent) => tree.parent(node) == Some(parent),
            Self::Subtree(root) => topology
                .and_then(|topology| topology.is_in_subtree_of(node, root))
                .unwrap_or_else(|| tree.is_in_subtree_of(node, root)),
            Self::StrictSubtree(root) => {
                node != root
                    && topology
                        .and_then(|topology| topology.is_in_subtree_of(node, root))
                        .unwrap_or_else(|| tree.is_in_subtree_of(node, root))
            }
            // Climbing out of a shadow tree goes through its host, so the number of roots crossed on
            // the way to `host` is what says whether the node is in a tree it hosts. Zero crossings
            // means the node is in the host's own light DOM, which this region does not cover.
            Self::HostedSubtrees(host) => {
                let mut current = node;
                let mut crossed_a_shadow_root = false;
                loop {
                    if current == host {
                        return crossed_a_shadow_root;
                    }
                    if let Some(parent) = tree.parent(current) {
                        current = parent;
                        continue;
                    }
                    match tree.host_of(current) {
                        Some(host_of_root) => {
                            crossed_a_shadow_root = true;
                            current = host_of_root;
                        }
                        None => return false,
                    }
                }
            }
            Self::NextSibling(anchor) => tree.next_element_sibling(anchor) == Some(node),
            Self::FollowingSiblings(anchor) => {
                let mut current = tree.next_element_sibling(anchor);
                while let Some(sibling) = current {
                    if sibling == node {
                        return true;
                    }
                    current = tree.next_element_sibling(sibling);
                }
                false
            }
            Self::FollowingSiblingSubtrees(anchor) => {
                let Some(parent) = tree.parent(anchor) else {
                    return false;
                };
                let mut branch = node;
                loop {
                    match tree.parent(branch) {
                        Some(branch_parent) if branch_parent == parent => break,
                        Some(ancestor) => branch = ancestor,
                        None => return false,
                    }
                }
                let mut sibling = tree.next_element_sibling(anchor);
                while let Some(current) = sibling {
                    if current == branch {
                        return true;
                    }
                    sibling = tree.next_element_sibling(current);
                }
                false
            }
            Self::SiblingSequence(member) => tree.parent(node) == tree.parent(member),
            Self::Ancestors(from) => topology
                .and_then(|topology| topology.is_in_subtree_of(from, node))
                .map_or_else(
                    || tree.ancestors(from).any(|ancestor| ancestor == node),
                    |inside| inside && from != node,
                ),
            Self::PreviousSibling(anchor) => tree.previous_element_sibling(anchor) == Some(node),
            Self::PrecedingSiblings(anchor) => {
                let Some(parent) = tree.parent(anchor) else {
                    return false;
                };
                for sibling in tree.children(parent) {
                    if sibling == anchor {
                        return false;
                    }
                    if sibling == node {
                        return true;
                    }
                }
                false
            }
            Self::TreeScope(scope) => tree.tree_scope(node) == scope,
            Self::Document => true,
        }
    }

    /// Stream the style nodes of the region. The relation the region was named with decides how it
    /// is walked, which is why coalescing preserves it.
    pub fn for_each(self, tree: &StyleNodeTree, mut visit: impl FnMut(StyleNodeID)) {
        match self {
            Self::Empty => {}
            Self::Node(node) => visit(node),
            Self::Children(node) => tree.children(node).for_each(visit),
            Self::Subtree(node) => tree.preorder(node).for_each(visit),
            Self::StrictSubtree(root) => tree.preorder(root).filter(|&node| node != root).for_each(visit),
            // A shadow root is not a descendant of its host, so the walk has to be seeded with each
            // one it reaches rather than falling out of a single preorder.
            Self::HostedSubtrees(host) => {
                let mut pending: Vec<StyleNodeID> = tree.shadow_root_of(host).into_iter().collect();
                while let Some(root) = pending.pop() {
                    for node in tree.preorder(root) {
                        if let Some(nested) = tree.shadow_root_of(node) {
                            pending.push(nested);
                        }
                        visit(node);
                    }
                }
            }
            Self::NextSibling(node) => {
                if let Some(next) = tree.next_element_sibling(node) {
                    visit(next);
                }
            }
            Self::FollowingSiblings(node) => {
                let mut current = tree.next_element_sibling(node);
                while let Some(sibling) = current {
                    visit(sibling);
                    current = tree.next_element_sibling(sibling);
                }
            }
            Self::FollowingSiblingSubtrees(node) => {
                let mut sibling = tree.next_element_sibling(node);
                while let Some(root) = sibling {
                    tree.preorder(root).for_each(&mut visit);
                    sibling = tree.next_element_sibling(root);
                }
            }
            Self::SiblingSequence(node) => match tree.parent(node) {
                Some(parent) => tree.children(parent).for_each(visit),
                None => visit(node),
            },
            Self::Ancestors(node) => tree.ancestors(node).for_each(visit),
            Self::PreviousSibling(node) => {
                if let Some(previous) = tree.previous_element_sibling(node) {
                    visit(previous);
                }
            }
            Self::PrecedingSiblings(node) => {
                if let Some(parent) = tree.parent(node) {
                    for sibling in tree.children(parent) {
                        if sibling == node {
                            break;
                        }
                        visit(sibling);
                    }
                }
            }
            // A document contains independent DOM preorders for its document and shadow-tree
            // scopes. Live identity order covers all of them without treating a shadow root as a
            // descendant of its host.
            Self::TreeScope(scope) => tree
                .live_nodes()
                .filter(|&node| tree.tree_scope(node) == scope)
                .for_each(visit),
            Self::Document => tree.live_nodes().for_each(visit),
        }
    }
}

/// Coalescing more than this many regions pairwise would cost more relation steps than the
/// widening it saves, so beyond it only exact duplicates are removed.
pub(super) const MAX_PAIRWISE_COALESCE: usize = 64;

const NO_PREORDER_POSITION: u32 = u32::MAX;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
struct PreorderInterval {
    start: u32,
    end: u32,
}

/// Dense transaction-local membership for emitted region identities.
///
/// Every element index owns one bit per node-shaped region kind. Pseudo-elements do not have an
/// element index and remain in a small linear tail, while tree scopes use the same compact bitset
/// shape. This replaces hashing integer identities and folds exact-node membership into the same
/// index.
const REGION_KIND_PAGE_BITS: usize = 8;
const REGION_KIND_PAGE_SIZE: usize = 1 << REGION_KIND_PAGE_BITS;

struct RegionKindPage {
    kinds: [u16; REGION_KIND_PAGE_SIZE],
}

impl Default for RegionKindPage {
    fn default() -> Self {
        Self {
            kinds: [0; REGION_KIND_PAGE_SIZE],
        }
    }
}

impl PagedColumnPage for RegionKindPage {
    type Value = u16;

    const SHIFT: usize = REGION_KIND_PAGE_BITS;

    fn get(&self, index: usize) -> Option<u16> {
        (self.kinds[index] != 0).then_some(self.kinds[index])
    }

    fn insert(&mut self, index: usize, kinds: u16) {
        self.kinds[index] = kinds;
    }
}

#[derive(Default)]
struct RegionKindPages(PagedColumn<RegionKindPage>);

impl RegionKindPages {
    fn insert(&mut self, element_index: u32, kind: u16) -> bool {
        let index = element_index as usize;
        let (page, _) = self.0.page_mut_or_default(index);
        let kinds = &mut page.kinds[index & (REGION_KIND_PAGE_SIZE - 1)];
        let was_present = *kinds & kind != 0;
        *kinds |= kind;
        !was_present
    }

    fn contains(&self, element_index: u32, kind: u16) -> bool {
        self.0
            .get(element_index as usize)
            .is_some_and(|kinds| kinds & kind != 0)
    }

    fn clear(&mut self) {
        for page in self.0.pages_mut() {
            page.kinds.fill(0);
        }
    }

    fn capacity_bytes(&self) -> u64 {
        self.0.capacity_bytes()
    }
}

#[derive(Default)]
struct ImpactRegionIndex {
    element_kinds: RegionKindPages,
    tree_scopes: BitColumn,
    document: bool,
}

impl ImpactRegionIndex {
    fn node_and_kind(region: ImpactRegion) -> Option<(StyleNodeID, u16)> {
        let (node, bit) = match region {
            ImpactRegion::Node(node) => (node, 0),
            ImpactRegion::Children(node) => (node, 1),
            ImpactRegion::Subtree(node) => (node, 2),
            ImpactRegion::StrictSubtree(node) => (node, 3),
            ImpactRegion::NextSibling(node) => (node, 4),
            ImpactRegion::FollowingSiblings(node) => (node, 5),
            ImpactRegion::FollowingSiblingSubtrees(node) => (node, 6),
            ImpactRegion::SiblingSequence(node) => (node, 7),
            ImpactRegion::Ancestors(node) => (node, 8),
            ImpactRegion::PreviousSibling(node) => (node, 9),
            ImpactRegion::PrecedingSiblings(node) => (node, 10),
            ImpactRegion::HostedSubtrees(node) => (node, 11),
            ImpactRegion::Empty | ImpactRegion::TreeScope(_) | ImpactRegion::Document => return None,
        };
        Some((node, 1 << bit))
    }

    fn insert(&mut self, region: ImpactRegion) -> bool {
        match region {
            ImpactRegion::Empty => false,
            ImpactRegion::Document => {
                let was_present = self.document;
                self.document = true;
                !was_present
            }
            ImpactRegion::TreeScope(scope) => {
                let index = scope.0 as usize;
                self.tree_scopes.set(index, true).0
            }
            _ => {
                let (node, kind) = Self::node_and_kind(region).unwrap();
                self.element_kinds.insert(node.element_index().unwrap(), kind)
            }
        }
    }

    fn contains(&self, region: ImpactRegion) -> bool {
        match region {
            ImpactRegion::Empty => false,
            ImpactRegion::Document => self.document,
            ImpactRegion::TreeScope(scope) => self.tree_scopes.contains(scope.0 as usize),
            _ => {
                let (node, kind) = Self::node_and_kind(region).unwrap();
                self.element_kinds.contains(node.element_index().unwrap(), kind)
            }
        }
    }

    fn clear(&mut self) {
        self.element_kinds.clear();
        self.tree_scopes.clear();
        self.document = false;
    }

    fn capacity_bytes(&self) -> u64 {
        self.tree_scopes.capacity_bytes() + self.element_kinds.capacity_bytes()
    }
}

/// A region union compiled for repeated iteration and membership tests.
///
/// Transactions with topology coordinates retain only disjoint preorder intervals. The fallback
/// stores sorted node identities so small transactions do not pay to build a complete topology.
enum ImpactRegionBatchStorage {
    PreorderIntervals(Box<[PreorderInterval]>),
    Nodes(Box<[StyleNodeID]>),
}

pub(super) struct ImpactRegionBatch(ImpactRegionBatchStorage);

impl ImpactRegionBatch {
    #[must_use]
    pub(super) fn node_count(&self) -> usize {
        match &self.0 {
            ImpactRegionBatchStorage::PreorderIntervals(intervals) => intervals
                .iter()
                .map(|interval| (interval.end - interval.start) as usize)
                .sum(),
            ImpactRegionBatchStorage::Nodes(nodes) => nodes.len(),
        }
    }

    #[must_use]
    pub(super) fn storage_bytes(&self) -> usize {
        match &self.0 {
            ImpactRegionBatchStorage::PreorderIntervals(intervals) => size_of_val(intervals.as_ref()),
            ImpactRegionBatchStorage::Nodes(nodes) => size_of_val(nodes.as_ref()),
        }
    }

    #[must_use]
    pub(super) fn interval_count(&self) -> Option<usize> {
        match &self.0 {
            ImpactRegionBatchStorage::PreorderIntervals(intervals) => Some(intervals.len()),
            ImpactRegionBatchStorage::Nodes(_) => None,
        }
    }
}

/// Transaction-local tree coordinates for repeated impact-region queries.
///
/// The live tree deliberately retains no document-order label. Once a transaction asks enough
/// region-membership questions to justify one pass over the tree, this workspace turns subtree
/// membership into an interval comparison without adding anything to the mandatory per-node state.
pub(super) struct TransactionTopology {
    nodes: Vec<StyleNodeID>,
    preorder_by_element_index: Vec<u32>,
    subtree_end_by_element_index: Vec<u32>,
}

impl TransactionTopology {
    #[must_use]
    fn new(tree: &StyleNodeTree, root: StyleNodeID) -> Self {
        let nodes: Vec<StyleNodeID> = tree.preorder(root).collect();
        let slot_count = nodes
            .iter()
            .filter_map(|node| node.element_index())
            .max()
            .map_or(0, |index| index as usize + 1);
        let mut preorder_by_element_index = vec![NO_PREORDER_POSITION; slot_count];
        let mut subtree_end_by_element_index = vec![NO_PREORDER_POSITION; slot_count];
        let mut open = Vec::new();

        for (position, &node) in nodes.iter().enumerate() {
            let parent = tree.parent(node);
            while open.last().copied().is_some_and(|candidate| Some(candidate) != parent) {
                let finished = open.pop().unwrap();
                subtree_end_by_element_index[finished.element_index().unwrap() as usize] =
                    u32::try_from(position).expect("transaction preorder space exhausted");
            }
            debug_assert!(node == root || open.last().copied() == parent);
            preorder_by_element_index[node.element_index().unwrap() as usize] =
                u32::try_from(position).expect("transaction preorder space exhausted");
            open.push(node);
        }
        let end = u32::try_from(nodes.len()).expect("transaction preorder space exhausted");
        for node in open {
            subtree_end_by_element_index[node.element_index().unwrap() as usize] = end;
        }

        Self {
            nodes,
            preorder_by_element_index,
            subtree_end_by_element_index,
        }
    }

    fn preorder_position(&self, node: StyleNodeID) -> Option<u32> {
        let position = *self.preorder_by_element_index.get(node.element_index()? as usize)?;
        (position != NO_PREORDER_POSITION).then_some(position)
    }

    fn subtree_interval(&self, root: StyleNodeID) -> Option<(u32, u32)> {
        let start = self.preorder_position(root)?;
        let end = *self.subtree_end_by_element_index.get(root.element_index()? as usize)?;
        (end != NO_PREORDER_POSITION).then_some((start, end))
    }

    #[must_use]
    fn is_in_subtree_of(&self, node: StyleNodeID, root: StyleNodeID) -> Option<bool> {
        let position = self.preorder_position(node)?;
        let (start, end) = self.subtree_interval(root)?;
        Some((start..end).contains(&position))
    }

    fn push_node_interval(&self, node: StyleNodeID, intervals: &mut Vec<PreorderInterval>) -> bool {
        let Some(start) = self.preorder_position(node) else {
            return false;
        };
        intervals.push(PreorderInterval { start, end: start + 1 });
        true
    }

    fn collect_region_intervals(
        &self,
        region: ImpactRegion,
        tree: &StyleNodeTree,
        document_root: Option<StyleNodeID>,
        intervals: &mut Vec<PreorderInterval>,
    ) -> bool {
        match region {
            ImpactRegion::Empty => true,
            // Nested shadow trees are not one preorder run, so there is no interval set to hand
            // back and the caller falls back to walking the region.
            ImpactRegion::HostedSubtrees(_) => false,
            ImpactRegion::Node(node) => self.push_node_interval(node, intervals),
            ImpactRegion::Children(parent) => {
                for child in tree.children(parent) {
                    if !self.push_node_interval(child, intervals) {
                        return false;
                    }
                }
                true
            }
            ImpactRegion::Subtree(root) => {
                let Some((start, end)) = self.subtree_interval(root) else {
                    return false;
                };
                intervals.push(PreorderInterval { start, end });
                true
            }
            ImpactRegion::StrictSubtree(root) => {
                let Some((start, end)) = self.subtree_interval(root) else {
                    return false;
                };
                if start + 1 < end {
                    intervals.push(PreorderInterval { start: start + 1, end });
                }
                true
            }
            ImpactRegion::NextSibling(anchor) => tree
                .next_element_sibling(anchor)
                .is_none_or(|node| self.push_node_interval(node, intervals)),
            ImpactRegion::FollowingSiblings(anchor) => {
                let mut sibling = tree.next_element_sibling(anchor);
                while let Some(node) = sibling {
                    if !self.push_node_interval(node, intervals) {
                        return false;
                    }
                    sibling = tree.next_element_sibling(node);
                }
                true
            }
            ImpactRegion::FollowingSiblingSubtrees(anchor) => {
                let Some(first) = tree.next_element_sibling(anchor) else {
                    return true;
                };
                let Some(parent) = tree.parent(anchor) else {
                    return false;
                };
                let Some(start) = self.preorder_position(first) else {
                    return false;
                };
                let Some((_, end)) = self.subtree_interval(parent) else {
                    return false;
                };
                intervals.push(PreorderInterval { start, end });
                true
            }
            ImpactRegion::SiblingSequence(member) => {
                let Some(parent) = tree.parent(member) else {
                    return self.push_node_interval(member, intervals);
                };
                for sibling in tree.children(parent) {
                    if !self.push_node_interval(sibling, intervals) {
                        return false;
                    }
                }
                true
            }
            ImpactRegion::Ancestors(node) => {
                for ancestor in tree.ancestors(node) {
                    if !self.push_node_interval(ancestor, intervals) {
                        return false;
                    }
                }
                true
            }
            ImpactRegion::PreviousSibling(anchor) => tree
                .previous_element_sibling(anchor)
                .is_none_or(|node| self.push_node_interval(node, intervals)),
            ImpactRegion::PrecedingSiblings(anchor) => {
                let Some(parent) = tree.parent(anchor) else {
                    return true;
                };
                for sibling in tree.children(parent) {
                    if sibling == anchor {
                        break;
                    }
                    if !self.push_node_interval(sibling, intervals) {
                        return false;
                    }
                }
                true
            }
            ImpactRegion::TreeScope(_) => false,
            ImpactRegion::Document => {
                if self.node_count() != tree.connected_element_count() as usize {
                    return false;
                }
                let Some(root) = document_root else {
                    return false;
                };
                let Some((start, end)) = self.subtree_interval(root) else {
                    return false;
                };
                intervals.push(PreorderInterval { start, end });
                true
            }
        }
    }

    fn canonical_intervals(
        &self,
        regions: &[ImpactRegion],
        tree: &StyleNodeTree,
        document_root: Option<StyleNodeID>,
    ) -> Option<Vec<PreorderInterval>> {
        let mut intervals = Vec::new();
        for &region in regions {
            if !self.collect_region_intervals(region, tree, document_root, &mut intervals) {
                return None;
            }
        }
        intervals.sort_unstable();
        let mut write = 0;
        for read in 0..intervals.len() {
            if write != 0 && intervals[read].start <= intervals[write - 1].end {
                intervals[write - 1].end = intervals[write - 1].end.max(intervals[read].end);
            } else {
                intervals[write] = intervals[read];
                write += 1;
            }
        }
        intervals.truncate(write);
        Some(intervals)
    }

    fn for_each_interval(&self, interval: PreorderInterval, visit: &mut impl FnMut(StyleNodeID)) {
        self.nodes[interval.start as usize..interval.end as usize]
            .iter()
            .copied()
            .for_each(visit);
    }

    #[must_use]
    pub(super) fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.nodes, self.preorder_by_element_index, self.subtree_end_by_element_index];
            cached [];
            nested [];
            skip [];
        }
    }

    #[must_use]
    fn node_count(&self) -> usize {
        self.nodes.len()
    }

    #[must_use]
    pub(super) fn nodes(&self) -> &[StyleNodeID] {
        &self.nodes
    }
}

/// The cursor state of one preorder walk over a `PatchCover`'s attribution intervals.
#[derive(Default)]
pub(super) struct AttributionSweep {
    cursor: usize,
    /// `(end, key index)` of every interval entered and not yet left, in entry order.
    active: Vec<(u32, u32)>,
    last_position: Option<u32>,
}

/// Compiled per-transaction patch coverage: which nodes must fully re-derive and which rules a
/// covered node's narrowed patch must include.
pub(super) struct PatchCover {
    pub(super) full: ImpactRegionBatch,
    /// Attributed extents as half-open preorder intervals `(start, end, key index)`, sorted.
    intervals: Vec<(u32, u32, u32)>,
    /// Running maximum interval end per sorted prefix, bounding the leftward stab walk.
    prefix_max_end: Vec<u32>,
    keys: Vec<(RuleID, EntryID)>,
    unindexed: Vec<(ImpactRegion, (RuleID, EntryID))>,
}

/// The normalized impact plan for one transaction.
#[derive(Default)]
pub struct ImpactRegions {
    regions: Vec<ImpactRegion>,
    region_index: ImpactRegionIndex,
    covered_subtrees: Vec<PreorderInterval>,
    /// The subtrees among `covered_subtrees` that were added without attribution and therefore
    /// force covered retained answers to a full re-derivation. Coverage by these alone justifies
    /// dropping a route's contribution outright; coverage by an attributed subtree does not.
    full_covered_subtrees: Vec<PreorderInterval>,
    topology: Option<TransactionTopology>,
    exact_node_generation: u64,
    exact_node_history_start_generation: u64,
    /// Every region added without rule attribution, in original extent: what forces a covered
    /// node's retained answer to a full re-derivation.
    unattributed: Vec<ImpactRegion>,
    /// Rule-attributed emissions in original extent. A route naming its exact entry can prove
    /// that within its region only that rule's truth moved, so a node covered exclusively by
    /// attributed regions patches against the union of its covering attributions.
    attributions: Vec<(ImpactRegion, (RuleID, EntryID))>,
}

impl ImpactRegions {
    #[must_use]
    pub fn new() -> Self {
        Self {
            regions: Vec::new(),
            region_index: ImpactRegionIndex::default(),
            covered_subtrees: Vec::new(),
            full_covered_subtrees: Vec::new(),
            topology: None,
            exact_node_generation: 0,
            exact_node_history_start_generation: 0,
            unattributed: Vec::new(),
            attributions: Vec::new(),
        }
    }

    #[must_use]
    pub fn with_topology(tree: &StyleNodeTree, root: StyleNodeID) -> Self {
        Self {
            topology: Some(TransactionTopology::new(tree, root)),
            ..Self::new()
        }
    }

    #[must_use]
    pub fn topology_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [];
            nested [self.topology.as_ref().map_or(0, TransactionTopology::capacity_bytes)];
            skip [];
        }
    }

    #[must_use]
    pub fn region_index_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [];
            cached [self.region_index.capacity_bytes()];
            nested [];
            skip [];
        }
    }

    #[must_use]
    pub fn topology_node_count(&self) -> usize {
        self.topology.as_ref().map_or(0, TransactionTopology::node_count)
    }

    pub(super) fn take_topology(&mut self) -> Option<TransactionTopology> {
        self.topology.take()
    }

    fn invalidate_exact_node_history(&mut self) {
        self.exact_node_generation = self.exact_node_generation.wrapping_add(1);
        self.exact_node_history_start_generation = self.exact_node_generation;
    }

    fn insert_covered_subtree(&mut self, root: StyleNodeID) {
        let Some(interval) = self
            .topology
            .as_ref()
            .and_then(|topology| topology.subtree_interval(root))
            .map(|(start, end)| PreorderInterval { start, end })
        else {
            return;
        };
        Self::insert_covered_interval(&mut self.covered_subtrees, interval);
    }

    fn insert_full_covered_subtree(&mut self, root: StyleNodeID) {
        let Some(interval) = self
            .topology
            .as_ref()
            .and_then(|topology| topology.subtree_interval(root))
            .map(|(start, end)| PreorderInterval { start, end })
        else {
            return;
        };
        Self::insert_covered_interval(&mut self.full_covered_subtrees, interval);
    }

    fn insert_covered_interval(covered: &mut Vec<PreorderInterval>, interval: PreorderInterval) {
        let first = covered.partition_point(|existing| existing.end < interval.start);
        let count = covered[first..].partition_point(|existing| existing.start <= interval.end);
        if count == 0 {
            covered.insert(first, interval);
            return;
        }

        let last = first + count;
        let merged = PreorderInterval {
            start: interval.start.min(covered[first].start),
            end: interval.end.max(covered[last - 1].end),
        };
        covered.splice(first..last, [merged]);
    }

    fn rebuild_indexes(&mut self) {
        self.region_index.clear();
        self.covered_subtrees.clear();
        self.invalidate_exact_node_history();
        for index in 0..self.regions.len() {
            let region = self.regions[index];
            self.region_index.insert(region);
            if let ImpactRegion::Subtree(root) = region {
                self.insert_covered_subtree(root);
            }
        }
    }

    pub fn add(&mut self, region: ImpactRegion) {
        if !matches!(region, ImpactRegion::Empty | ImpactRegion::Node(_)) {
            self.unattributed.push(region);
        }
        if let ImpactRegion::Subtree(root) = region {
            self.insert_full_covered_subtree(root);
        }
        self.add_recorded(region);
    }

    /// Add a region whose emitting route names the one rule whose truth can move within it.
    /// Recorded in original extent, before merging, so patch narrowing can union the covering
    /// attributions per node; duplicate adds of the same extent by unattributed routes still
    /// force full re-derivation because every unattributed add is recorded independently.
    pub fn add_attributed(&mut self, region: ImpactRegion, key: (RuleID, EntryID)) {
        self.attribute_extent(region, key);
        self.add_recorded(region);
    }

    /// Record a rule attribution over a region without planning the region. A route whose
    /// candidates are already planned owes the patch only the fact that its rule may have moved
    /// inside its extent; this carries that fact symbolically, one row per route instead of one
    /// row per (node, rule), and the per-node union is rebuilt lazily by `covering_attributions`.
    pub fn attribute_extent(&mut self, region: ImpactRegion, key: (RuleID, EntryID)) {
        if region != ImpactRegion::Empty {
            self.attributions.push((region, key));
        }
    }

    fn add_recorded(&mut self, region: ImpactRegion) {
        if region == ImpactRegion::Empty {
            return;
        }
        if region == ImpactRegion::Document {
            self.regions.clear();
            self.region_index.clear();
            self.covered_subtrees.clear();
            self.full_covered_subtrees.clear();
            self.invalidate_exact_node_history();
            self.regions.push(ImpactRegion::Document);
            self.region_index.insert(ImpactRegion::Document);
            return;
        }
        if self.regions.first() == Some(&ImpactRegion::Document) {
            return;
        }
        if !self.region_index.insert(region) {
            return;
        }
        if let ImpactRegion::Subtree(root) = region {
            self.insert_covered_subtree(root);
        }
        if matches!(region, ImpactRegion::Node(_)) {
            self.exact_node_generation = self.exact_node_generation.wrapping_add(1);
        }
        self.regions.push(region);
    }

    #[must_use]
    pub fn contains_node(&self, node: StyleNodeID) -> bool {
        self.region_index.contains(ImpactRegion::Node(node))
    }

    #[must_use]
    pub(super) fn exact_node_generation(&self) -> u64 {
        self.exact_node_generation
    }

    #[must_use]
    /// Visit a bounded suffix of exact-node additions when the region vector has remained in
    /// append order since `generation`. Returns false when the caller must inspect the full plan.
    pub(super) fn for_each_exact_node_added_after(
        &self,
        generation: u64,
        limit: usize,
        mut visit: impl FnMut(StyleNodeID),
    ) -> bool {
        let Some(count) = self.exact_node_generation.checked_sub(generation) else {
            return false;
        };
        if generation < self.exact_node_history_start_generation || count > limit as u64 {
            return false;
        }
        self.regions
            .iter()
            .rev()
            .filter_map(|region| match region {
                ImpactRegion::Node(node) => Some(*node),
                _ => None,
            })
            .take(count as usize)
            .for_each(&mut visit);
        true
    }

    /// Whether an already named subtree contains all of `region`.
    ///
    /// Arrival subtrees establish the transaction's dirty envelope before selector routing. Asking
    /// this through the region set keeps a route that stays inside that envelope from doing any
    /// candidate discovery or exact matching merely to emit a region normalization will discard.
    #[must_use]
    pub fn is_covered_by_subtree(&self, region: ImpactRegion, tree: &StyleNodeTree) -> bool {
        let (ImpactRegion::Node(node)
        | ImpactRegion::Children(node)
        | ImpactRegion::Subtree(node)
        | ImpactRegion::StrictSubtree(node)) = region
        else {
            return false;
        };
        if let Some(position) = self
            .topology
            .as_ref()
            .and_then(|topology| topology.preorder_position(node))
        {
            let index = self
                .covered_subtrees
                .partition_point(|interval| interval.end <= position);
            return self
                .covered_subtrees
                .get(index)
                .is_some_and(|interval| interval.start <= position);
        }
        self.region_index.contains(ImpactRegion::Subtree(node))
            || tree
                .ancestors(node)
                .any(|ancestor| self.region_index.contains(ImpactRegion::Subtree(ancestor)))
    }

    /// Whether a subtree that forces full re-derivation contains all of `region`. Coverage by an
    /// attributed subtree keeps a node planned but narrows its patch to the covering rules, so
    /// only this stronger cover justifies forgetting a route's contribution or a walk's diff
    /// outright.
    #[must_use]
    pub fn is_covered_by_full_subtree(&self, region: ImpactRegion, tree: &StyleNodeTree) -> bool {
        if self.covers_document() {
            return true;
        }
        let (ImpactRegion::Node(node)
        | ImpactRegion::Children(node)
        | ImpactRegion::Subtree(node)
        | ImpactRegion::StrictSubtree(node)) = region
        else {
            return false;
        };
        if let Some(position) = self
            .topology
            .as_ref()
            .and_then(|topology| topology.preorder_position(node))
        {
            let index = self
                .full_covered_subtrees
                .partition_point(|interval| interval.end <= position);
            return self
                .full_covered_subtrees
                .get(index)
                .is_some_and(|interval| interval.start <= position);
        }
        self.unattributed.contains(&ImpactRegion::Subtree(node))
            || tree
                .ancestors(node)
                .any(|ancestor| self.unattributed.contains(&ImpactRegion::Subtree(ancestor)))
    }

    #[must_use]
    pub fn region_contains_node(&self, region: ImpactRegion, node: StyleNodeID, tree: &StyleNodeTree) -> bool {
        region.contains_node_with_topology(node, tree, self.topology.as_ref())
    }

    /// Order pending nodes so popping the vector visits every parent before its descendants.
    ///
    /// A wide transaction already paid for preorder coordinates. Reverse preorder in the backing
    /// vector gives the same top-down pop order as sorting by depth, without walking every node's
    /// ancestors again.
    ///
    /// Small transactions have no coordinates, but a rightward-propagating walk still needs
    /// exact document order, so the pending set derives it directly: each node keys by its
    /// ancestor path of sibling ordinals, making an ancestor's key a strict prefix of its
    /// descendants' and ordering siblings by ordinal. The ordinal memo bounds sibling counting
    /// to one walk per distinct prefix, so the cost stays proportional to the pending set.
    ///
    /// Returns whether the order is exact document preorder within every root's subtree.
    pub(super) fn sort_nodes_for_top_down_walk(&self, nodes: &mut [StyleNodeID], tree: &StyleNodeTree) -> bool {
        if let Some(topology) = &self.topology
            && nodes.iter().all(|&node| topology.preorder_position(node).is_some())
        {
            nodes.sort_unstable_by_key(|&node| std::cmp::Reverse(topology.preorder_position(node).unwrap()));
            return true;
        }

        // The topology branch above implicitly proved every pending node live in the document
        // scope, because only those nodes have preorder positions. The derived order can only
        // claim exactness under the same precondition: a detached or shadow-scoped node has no
        // place in document order, and the sibling walk must not run over a set containing one.
        if !nodes
            .iter()
            .all(|&node| tree.is_live(node) && tree.tree_scope(node) == TreeScopeID::DOCUMENT)
        {
            nodes.sort_unstable_by_key(|&node| std::cmp::Reverse(tree.ancestors(node).count()));
            return false;
        }
        let mut ordinals: super::HashMap<StyleNodeID, u32> = super::HashMap::default();
        let mut run = Vec::new();
        let mut ordinal_of = |node: StyleNodeID| -> u32 {
            run.clear();
            let mut current = node;
            let mut ordinal = loop {
                if let Some(&known) = ordinals.get(&current) {
                    break known;
                }
                run.push(current);
                match tree.previous_element_sibling(current) {
                    Some(previous) => current = previous,
                    None => break u32::MAX,
                }
            };
            while let Some(reached) = run.pop() {
                ordinal = ordinal.wrapping_add(1);
                ordinals.insert(reached, ordinal);
            }
            ordinals[&node]
        };
        let mut keyed: Vec<(Vec<u32>, StyleNodeID)> = nodes
            .iter()
            .map(|&node| {
                let mut path: Vec<u32> = std::iter::once(node)
                    .chain(tree.ancestors(node))
                    .map(&mut ordinal_of)
                    .collect();
                path.reverse();
                (path, node)
            })
            .collect();
        keyed.sort_unstable_by(|left, right| right.0.cmp(&left.0));
        for (slot, (_, node)) in nodes.iter_mut().zip(keyed) {
            *slot = node;
        }
        true
    }

    /// Partition subtree roots into the outer roots and those already covered by one.
    ///
    /// The input order is scratch. Results retain identity order for callers that use binary
    /// search after the interval scan.
    pub(super) fn partition_subtree_roots(
        &self,
        nodes: &mut [StyleNodeID],
    ) -> Option<(Vec<StyleNodeID>, Vec<StyleNodeID>)> {
        let topology = self.topology.as_ref()?;
        if !nodes.iter().all(|&node| topology.subtree_interval(node).is_some()) {
            return None;
        }
        nodes.sort_unstable_by_key(|&node| topology.preorder_position(node).unwrap());

        let mut outer = Vec::new();
        let mut nested = Vec::new();
        let mut covered_end = 0;
        for &node in nodes.iter() {
            let (start, end) = topology.subtree_interval(node).unwrap();
            if start < covered_end {
                nested.push(node);
            } else {
                outer.push(node);
                covered_end = end;
            }
        }
        outer.sort_unstable();
        nested.sort_unstable();
        Some((outer, nested))
    }

    /// Collect the complement of the subtree intervals already covered by the plan.
    ///
    /// The topology's backing nodes are in preorder, so every uncovered range is one contiguous
    /// slice. Callers must first prove that the topology covers every tree they need to inspect.
    pub(super) fn nodes_outside_covered_subtrees(&self) -> Option<Vec<StyleNodeID>> {
        let topology = self.topology.as_ref()?;
        let covered_nodes: usize = self
            .covered_subtrees
            .iter()
            .map(|interval| (interval.end - interval.start) as usize)
            .sum();
        let mut nodes = Vec::with_capacity(topology.nodes.len().saturating_sub(covered_nodes));
        let mut cursor = 0;
        for interval in &self.covered_subtrees {
            nodes.extend_from_slice(&topology.nodes[cursor..interval.start as usize]);
            cursor = interval.end as usize;
        }
        nodes.extend_from_slice(&topology.nodes[cursor..]);
        Some(nodes)
    }

    pub fn add_if_not_covered(&mut self, region: ImpactRegion, tree: &StyleNodeTree) {
        if self.is_covered_by_subtree(region, tree) {
            return;
        }
        self.add(region);
    }

    /// Normalize: drop duplicates and regions contained in another. The relation each surviving
    /// region was named with is preserved.
    pub fn normalize(&mut self, tree: &StyleNodeTree) {
        self.regions.sort_unstable();
        self.regions.dedup();
        if self.regions.len() > MAX_PAIRWISE_COALESCE {
            self.invalidate_exact_node_history();
            return;
        }
        let mut kept: Vec<ImpactRegion> = Vec::with_capacity(self.regions.len());
        for &region in &self.regions {
            let absorbed = kept
                .iter()
                .any(|&other| other.contains_with_topology(region, tree, self.topology.as_ref()));
            if absorbed {
                continue;
            }
            kept.retain(|&other| !region.contains_with_topology(other, tree, self.topology.as_ref()));
            kept.push(region);
        }
        self.regions = kept;
        self.rebuild_indexes();
    }

    pub fn widen_to_document(&mut self, counters: &mut Counters) {
        counters.bump(Counter::DocumentWidenings);
        self.regions.clear();
        self.region_index.clear();
        self.covered_subtrees.clear();
        self.full_covered_subtrees.clear();
        self.invalidate_exact_node_history();
        self.regions.push(ImpactRegion::Document);
        self.region_index.insert(ImpactRegion::Document);
    }

    #[must_use]
    pub fn regions(&self) -> &[ImpactRegion] {
        &self.regions
    }

    /// Whether no narrower contribution can change the plan any further.
    #[must_use]
    pub fn covers_document(&self) -> bool {
        self.regions.first() == Some(&ImpactRegion::Document)
    }

    pub fn for_each(&self, tree: &StyleNodeTree, document_root: Option<StyleNodeID>, visit: impl FnMut(StyleNodeID)) {
        self.for_each_union(&self.regions, tree, document_root, visit);
    }

    /// Stream a union of regions once.
    ///
    /// A transaction topology compiles the union into disjoint preorder intervals, so a subtree is
    /// represented by two coordinates and overlapping routes disappear before any node is visited.
    /// Small transactions without a topology retain the relation-walking fallback.
    pub(super) fn for_each_union(
        &self,
        regions: &[ImpactRegion],
        tree: &StyleNodeTree,
        document_root: Option<StyleNodeID>,
        mut visit: impl FnMut(StyleNodeID),
    ) {
        let batch = self.compile_union(regions, tree, document_root);
        self.for_each_batch(&batch, &mut visit);
    }

    #[must_use]
    /// Compile the transaction's patch coverage: the unattributed regions as the full
    /// re-derivation trigger, and the attributed emissions as stabbable preorder intervals
    /// carrying rule keys. Extents without preorder coordinates retain their rule attribution
    /// and are queried through tree relationships.
    pub(super) fn compile_patch_cover(&self, tree: &StyleNodeTree, document_root: Option<StyleNodeID>) -> PatchCover {
        let mut keys: Vec<(RuleID, EntryID)> = Vec::new();
        let mut key_indices: super::HashMap<(RuleID, EntryID), u32> = super::HashMap::default();
        let mut intervals: Vec<(u32, u32, u32)> = Vec::new();
        let mut unindexed = Vec::new();
        let mut scratch: Vec<PreorderInterval> = Vec::new();
        if let Some(topology) = &self.topology {
            for &(region, key) in &self.attributions {
                scratch.clear();
                if !topology.collect_region_intervals(region, tree, document_root, &mut scratch) {
                    unindexed.push((region, key));
                    continue;
                }
                let key_index = match key_indices.get(&key).copied() {
                    Some(index) => index,
                    None => {
                        let index = u32::try_from(keys.len()).expect("patch attribution key space exhausted");
                        keys.push(key);
                        key_indices.insert(key, index);
                        index
                    }
                };
                for interval in &scratch {
                    intervals.push((interval.start, interval.end, key_index));
                }
            }
        } else {
            unindexed.extend_from_slice(&self.attributions);
        }
        intervals.sort_unstable();
        intervals.dedup();
        let mut prefix_max_end = Vec::with_capacity(intervals.len());
        let mut max_end = 0u32;
        for &(_, end, _) in &intervals {
            max_end = max_end.max(end);
            prefix_max_end.push(max_end);
        }
        PatchCover {
            full: self.compile_union(&self.unattributed, tree, document_root),
            intervals,
            prefix_max_end,
            keys,
            unindexed,
        }
    }

    /// Collect the rule keys of every attributed region covering `node` into `out`, deduplicated.
    /// Returns `false` when the node has no preorder position, in which case the caller must fall
    /// back to full re-derivation.
    ///
    /// The batch walk visits nodes in nondecreasing preorder position, so coverage is answered by
    /// a sweep that consumes each interval once: route-extent attributions can stack thousands of
    /// intervals per transaction, and a per-node backward stab would rescan the dead ones behind
    /// every long-lived interval. A node visited out of order falls back to a direct stab without
    /// disturbing the sweep.
    pub(super) fn covering_attributions(
        &self,
        cover: &PatchCover,
        tree: &StyleNodeTree,
        sweep: &mut AttributionSweep,
        node: StyleNodeID,
        out: &mut Vec<(RuleID, EntryID)>,
    ) -> bool {
        out.clear();
        out.extend(
            cover
                .unindexed
                .iter()
                .filter_map(|&(region, key)| self.region_contains_node(region, node, tree).then_some(key)),
        );
        if cover.intervals.is_empty() {
            out.sort_unstable();
            out.dedup();
            return true;
        }
        let Some(topology) = &self.topology else {
            return false;
        };
        let Some(position) = topology.preorder_position(node) else {
            return false;
        };
        if sweep.last_position.is_some_and(|last| position < last) {
            self.stab_covering_attributions(cover, position, out);
        } else {
            sweep.last_position = Some(position);
            while let Some(&(start, end, key_index)) = cover.intervals.get(sweep.cursor) {
                if start > position {
                    break;
                }
                if end > position {
                    sweep.active.push((end, key_index));
                }
                sweep.cursor += 1;
            }
            sweep.active.retain(|&(end, _)| end > position);
            out.extend(
                sweep
                    .active
                    .iter()
                    .map(|&(_, key_index)| cover.keys[key_index as usize]),
            );
        }
        // Route-extent attributions can stack many covering rules on one node, so dedup by sort
        // instead of a quadratic membership scan.
        out.sort_unstable();
        out.dedup();
        true
    }

    fn stab_covering_attributions(&self, cover: &PatchCover, position: u32, out: &mut Vec<(RuleID, EntryID)>) {
        let mut index = cover.intervals.partition_point(|&(start, _, _)| start <= position);
        while index > 0 {
            index -= 1;
            if cover.prefix_max_end[index] <= position {
                break;
            }
            let (start, end, key_index) = cover.intervals[index];
            if start <= position && position < end {
                out.push(cover.keys[key_index as usize]);
            }
        }
    }

    pub(super) fn compile_union(
        &self,
        regions: &[ImpactRegion],
        tree: &StyleNodeTree,
        document_root: Option<StyleNodeID>,
    ) -> ImpactRegionBatch {
        if let Some(topology) = &self.topology
            && let Some(intervals) = topology.canonical_intervals(regions, tree, document_root)
        {
            return ImpactRegionBatch(ImpactRegionBatchStorage::PreorderIntervals(intervals.into()));
        }

        let mut seen: Vec<StyleNodeID> = Vec::new();
        for &region in regions {
            region.for_each(tree, |node| seen.push(node));
        }
        seen.sort_unstable();
        seen.dedup();
        ImpactRegionBatch(ImpactRegionBatchStorage::Nodes(seen.into()))
    }

    pub(super) fn for_each_batch(&self, batch: &ImpactRegionBatch, mut visit: impl FnMut(StyleNodeID)) {
        match &batch.0 {
            ImpactRegionBatchStorage::PreorderIntervals(intervals) => {
                let topology = self
                    .topology
                    .as_ref()
                    .expect("preorder intervals require transaction topology");
                for &interval in intervals {
                    topology.for_each_interval(interval, &mut visit);
                }
            }
            ImpactRegionBatchStorage::Nodes(nodes) => nodes.iter().copied().for_each(visit),
        }
    }

    #[must_use]
    pub(super) fn batch_contains_node(&self, batch: &ImpactRegionBatch, node: StyleNodeID) -> bool {
        match &batch.0 {
            ImpactRegionBatchStorage::PreorderIntervals(intervals) => {
                let topology = self
                    .topology
                    .as_ref()
                    .expect("preorder intervals require transaction topology");
                let Some(position) = topology.preorder_position(node) else {
                    return false;
                };
                let index = intervals.partition_point(|interval| interval.end <= position);
                intervals.get(index).is_some_and(|interval| interval.start <= position)
            }
            ImpactRegionBatchStorage::Nodes(nodes) => nodes.binary_search(&node).is_ok(),
        }
    }

    #[must_use]
    pub(super) fn union_contains_node(
        &self,
        regions: &[ImpactRegion],
        node: StyleNodeID,
        tree: &StyleNodeTree,
    ) -> bool {
        regions
            .iter()
            .any(|&region| region.contains_node_with_topology(node, tree, self.topology.as_ref()))
    }
}

/// Which way to evaluate a region.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Plan {
    /// Drive from a candidate source of known exact size, proving region membership per candidate.
    Selective { candidates: usize },
    /// Sweep the region once, reading local facts directly.
    ExactBatch { nodes: usize },
    /// Keep the routed region because neither exact execution strategy is available.
    Conservative,
}

/// A selective plan is only worth its per-candidate membership proofs while the candidate source is
/// substantially smaller than the region. Past a quarter of the region, sweeping is cheaper.
pub const SELECTIVE_SHARE_DIVISOR: usize = 4;

/// Choose between a selective walk and an exact batch sweep.
///
/// `candidate_cardinality` is `None` when no exact count is available. That is deliberately not an
/// invitation to estimate: without an exact number an available batch plan is chosen, because
/// guessing low commits to a selective walk over a scope that a single sweep would have handled.
/// If neither exact strategy is executable, the routed region remains conservative.
#[must_use]
pub fn choose_plan(region_nodes: usize, candidate_cardinality: Option<usize>, exact_batch_available: bool) -> Plan {
    let Some(candidates) = candidate_cardinality else {
        return match exact_batch_available {
            true => Plan::ExactBatch { nodes: region_nodes },
            false => Plan::Conservative,
        };
    };
    if exact_batch_available && candidates.saturating_mul(SELECTIVE_SHARE_DIVISOR) > region_nodes {
        return Plan::ExactBatch { nodes: region_nodes };
    }
    Plan::Selective { candidates }
}

#[cfg(test)]
mod tests {
    use super::super::memory::DeviceClass;
    use super::super::memory::MemoryController;
    use super::*;

    /// `root > [a > [a1, a2], b > b1, c > c1]`
    struct Fixture {
        tree: StyleNodeTree,
        nodes: Vec<StyleNodeID>,
        counters: Counters,
    }

    impl Fixture {
        fn new() -> Self {
            let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
            let mut tree = StyleNodeTree::new(&mut memory);
            let nodes: Vec<StyleNodeID> = (0..8).map(|_| tree.allocate_element(&mut memory)).collect();
            // root=0, children a=1, b=2, c=3; a's children a1=4, a2=5
            tree.set_first_element_child(nodes[0], Some(nodes[1]));
            for index in 1..4 {
                tree.set_parent(nodes[index], Some(nodes[0]));
                tree.set_next_element_sibling(nodes[index], nodes.get(index + 1).filter(|_| index < 3).copied());
            }
            tree.set_first_element_child(nodes[1], Some(nodes[4]));
            tree.set_parent(nodes[4], Some(nodes[1]));
            tree.set_parent(nodes[5], Some(nodes[1]));
            tree.set_next_element_sibling(nodes[4], Some(nodes[5]));
            for (parent, child) in [(nodes[2], nodes[6]), (nodes[3], nodes[7])] {
                tree.set_first_element_child(parent, Some(child));
                tree.set_parent(child, Some(parent));
            }

            Self {
                tree,
                nodes,
                counters: Counters::new(),
            }
        }

        fn collect(&self, region: ImpactRegion) -> Vec<usize> {
            let mut visited = Vec::new();
            region.for_each(&self.tree, |node| {
                visited.push(self.nodes.iter().position(|other| *other == node).unwrap());
            });
            visited
        }
    }

    #[test]
    fn each_region_streams_the_relation_it_was_named_with() {
        let fixture = Fixture::new();
        assert_eq!(fixture.collect(ImpactRegion::Node(fixture.nodes[1])), vec![1]);
        assert_eq!(fixture.collect(ImpactRegion::Children(fixture.nodes[0])), vec![1, 2, 3]);
        assert_eq!(fixture.collect(ImpactRegion::Subtree(fixture.nodes[1])), vec![1, 4, 5]);
        assert_eq!(fixture.collect(ImpactRegion::NextSibling(fixture.nodes[1])), vec![2]);
        assert_eq!(
            fixture.collect(ImpactRegion::FollowingSiblings(fixture.nodes[1])),
            vec![2, 3]
        );
        assert_eq!(
            fixture.collect(ImpactRegion::FollowingSiblingSubtrees(fixture.nodes[1])),
            vec![2, 6, 3, 7]
        );
        assert_eq!(
            fixture.collect(ImpactRegion::SiblingSequence(fixture.nodes[2])),
            vec![1, 2, 3]
        );
        assert_eq!(
            fixture.collect(ImpactRegion::Document),
            vec![0, 1, 2, 3, 4, 5, 6, 7],
            "the document region streams every live identity"
        );
    }

    #[test]
    fn a_document_region_includes_shadow_tree_scopes() {
        let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
        let mut tree = StyleNodeTree::new(&mut memory);
        let document_root = tree.allocate_element(&mut memory);
        let host = tree.allocate_element(&mut memory);
        let shadow_root = tree.allocate_element(&mut memory);
        let shadow_child = tree.allocate_element(&mut memory);
        tree.set_first_element_child(document_root, Some(host));
        tree.set_parent(host, Some(document_root));
        tree.set_first_element_child(shadow_root, Some(shadow_child));
        tree.set_parent(shadow_child, Some(shadow_root));
        tree.enable_tree_scopes(&mut memory);
        tree.set_tree_scope(shadow_root, TreeScopeID(1));
        tree.set_tree_scope(shadow_child, TreeScopeID(1));
        tree.set_shadow_root(host, shadow_root, &mut memory);

        let mut visited = Vec::new();
        ImpactRegion::Document.for_each(&tree, |node| visited.push(node));
        assert_eq!(visited, vec![document_root, host, shadow_root, shadow_child]);

        visited.clear();
        ImpactRegion::TreeScope(TreeScopeID(1)).for_each(&tree, |node| visited.push(node));
        assert_eq!(visited, vec![shadow_root, shadow_child]);
    }

    #[test]
    fn a_transpose_path_folds_into_one_region() {
        let fixture = Fixture::new();
        let node = fixture.nodes[0];

        assert_eq!(
            ImpactRegion::follow(node, &[InverseStep::Descendants], &fixture.tree),
            ImpactRegion::Subtree(node)
        );
        assert_eq!(
            ImpactRegion::follow(node, &[InverseStep::Children], &fixture.tree),
            ImpactRegion::Children(node)
        );
        // `.a > .b .c`: children, then their descendants, which stays inside the subtree.
        assert_eq!(
            ImpactRegion::follow(node, &[InverseStep::Children, InverseStep::Descendants], &fixture.tree),
            ImpactRegion::Subtree(node)
        );
        // An empty path is the changed node itself.
        assert_eq!(ImpactRegion::follow(node, &[], &fixture.tree), ImpactRegion::Node(node));
    }

    #[test]
    fn descendants_of_following_siblings_keep_their_forest_boundary() {
        let fixture = Fixture::new();
        let region = ImpactRegion::follow(
            fixture.nodes[1],
            &[InverseStep::FollowingSiblings, InverseStep::Descendants],
            &fixture.tree,
        );
        assert_eq!(region, ImpactRegion::FollowingSiblingSubtrees(fixture.nodes[1]));
        assert!(region.contains_node(fixture.nodes[2], &fixture.tree));
        assert!(region.contains_node(fixture.nodes[3], &fixture.tree));
        assert!(region.contains_node(fixture.nodes[6], &fixture.tree));
        assert!(region.contains_node(fixture.nodes[7], &fixture.tree));
        assert!(!region.contains_node(fixture.nodes[1], &fixture.tree));
        assert!(!region.contains_node(fixture.nodes[4], &fixture.tree));
        assert!(region.contains_with_topology(ImpactRegion::Subtree(fixture.nodes[6]), &fixture.tree, None));
        assert!(!region.contains_with_topology(ImpactRegion::Subtree(fixture.nodes[4]), &fixture.tree, None));
        assert_eq!(
            region.step(InverseStep::FollowingSiblings, &fixture.tree),
            region,
            "a following-sibling step cannot leave the forest"
        );
        assert_eq!(
            region.step(InverseStep::SiblingSequence, &fixture.tree),
            ImpactRegion::Subtree(fixture.nodes[0]),
            "a whole-sequence step can cross the forest's leading boundary"
        );

        assert_eq!(
            ImpactRegion::follow(
                fixture.nodes[1],
                &[InverseStep::NextSibling, InverseStep::Descendants],
                &fixture.tree,
            ),
            ImpactRegion::Subtree(fixture.nodes[2])
        );
        assert!(
            ImpactRegion::Subtree(fixture.nodes[2]).contains_node(fixture.nodes[6], &fixture.tree),
            "the adjacent sibling's descendants remain reachable"
        );
    }

    #[test]
    fn a_sibling_step_out_of_a_subtree_widens_to_the_parent() {
        let fixture = Fixture::new();
        // Siblings of a descendant of `a` can lie outside `a`, so the region climbs.
        assert_eq!(
            ImpactRegion::follow(
                fixture.nodes[1],
                &[InverseStep::Descendants, InverseStep::FollowingSiblings],
                &fixture.tree
            ),
            ImpactRegion::Subtree(fixture.nodes[0])
        );
    }

    #[test]
    fn normalization_absorbs_contained_regions_and_keeps_the_relation() {
        let fixture = Fixture::new();
        let mut regions = ImpactRegions::new();
        regions.add(ImpactRegion::Subtree(fixture.nodes[1]));
        regions.add(ImpactRegion::Node(fixture.nodes[4]));
        regions.add(ImpactRegion::Node(fixture.nodes[2]));
        regions.add(ImpactRegion::Subtree(fixture.nodes[1]));
        regions.normalize(&fixture.tree);

        assert_eq!(regions.regions().len(), 2);
        assert!(regions.regions().contains(&ImpactRegion::Subtree(fixture.nodes[1])));
        assert!(regions.regions().contains(&ImpactRegion::Node(fixture.nodes[2])));
    }

    #[test]
    fn duplicate_nodes_are_discarded_as_the_plan_is_built() {
        let fixture = Fixture::new();
        let mut regions = ImpactRegions::new();
        regions.add(ImpactRegion::Node(fixture.nodes[1]));
        regions.add(ImpactRegion::Node(fixture.nodes[1]));

        assert_eq!(regions.regions(), &[ImpactRegion::Node(fixture.nodes[1])]);
        assert!(regions.contains_node(fixture.nodes[1]));
    }

    #[test]
    fn dense_region_index_distinguishes_region_kinds_for_one_node() {
        let fixture = Fixture::new();
        let mut regions = ImpactRegions::new();
        let node = fixture.nodes[1];
        let node_regions = [
            ImpactRegion::Node(node),
            ImpactRegion::Children(node),
            ImpactRegion::Subtree(node),
            ImpactRegion::StrictSubtree(node),
            ImpactRegion::NextSibling(node),
            ImpactRegion::FollowingSiblings(node),
            ImpactRegion::FollowingSiblingSubtrees(node),
            ImpactRegion::SiblingSequence(node),
            ImpactRegion::Ancestors(node),
            ImpactRegion::PreviousSibling(node),
            ImpactRegion::PrecedingSiblings(node),
        ];

        for region in node_regions {
            regions.add(region);
            regions.add(region);
        }

        assert_eq!(regions.regions(), node_regions);
    }

    #[test]
    fn dense_region_index_allocates_only_touched_element_pages() {
        let mut index = ImpactRegionIndex::default();
        let element = StyleNodeID::element(1_000_000);

        assert!(index.insert(ImpactRegion::Node(element)));
        assert!(index.insert(ImpactRegion::Subtree(element)));
        assert!(!index.insert(ImpactRegion::Node(element)));
        assert!(index.contains(ImpactRegion::Node(element)));
        assert!(index.contains(ImpactRegion::Subtree(element)));
        assert!(
            index.capacity_bytes() < 64 * 1024,
            "a sparse high identity must allocate one data page, not one row per preceding identity"
        );
    }

    #[test]
    fn a_document_region_absorbs_everything_after_it() {
        let fixture = Fixture::new();
        let mut regions = ImpactRegions::new();
        regions.add(ImpactRegion::Node(fixture.nodes[4]));
        regions.add(ImpactRegion::Document);
        regions.add(ImpactRegion::Subtree(fixture.nodes[1]));
        assert_eq!(regions.regions(), &[ImpactRegion::Document]);
    }

    #[test]
    fn widening_replaces_the_plan_rather_than_adding_to_it() {
        let mut fixture = Fixture::new();
        let mut regions = ImpactRegions::new();
        regions.add(ImpactRegion::Node(fixture.nodes[4]));
        regions.widen_to_document(&mut fixture.counters);
        assert_eq!(regions.regions(), &[ImpactRegion::Document]);
        assert_eq!(fixture.counters.get(Counter::DocumentWidenings), 1);
    }

    #[test]
    fn a_plan_is_selective_only_while_its_candidate_source_is_small() {
        assert_eq!(choose_plan(1000, Some(100), true), Plan::Selective { candidates: 100 });
        // A quarter of the region is the cutoff.
        assert_eq!(choose_plan(1000, Some(250), true), Plan::Selective { candidates: 250 });
        assert_eq!(choose_plan(1000, Some(251), true), Plan::ExactBatch { nodes: 1000 });
    }

    #[test]
    fn an_unknown_cardinality_chooses_the_batch_rather_than_guessing() {
        assert_eq!(choose_plan(1000, None, true), Plan::ExactBatch { nodes: 1000 });
    }

    #[test]
    fn an_unavailable_batch_plan_is_never_selected() {
        assert_eq!(choose_plan(1000, Some(999), false), Plan::Selective { candidates: 999 });
        assert_eq!(choose_plan(1000, None, false), Plan::Conservative);
    }

    #[test]
    fn transaction_topology_answers_subtree_queries_and_streams_regions() {
        let fixture = Fixture::new();
        let topology = TransactionTopology::new(&fixture.tree, fixture.nodes[0]);

        assert_eq!(topology.node_count(), 8);
        assert_eq!(
            topology.is_in_subtree_of(fixture.nodes[4], fixture.nodes[1]),
            Some(true)
        );
        assert_eq!(
            topology.is_in_subtree_of(fixture.nodes[2], fixture.nodes[1]),
            Some(false)
        );

        let regions = [
            ImpactRegion::Empty,
            ImpactRegion::Node(fixture.nodes[1]),
            ImpactRegion::Children(fixture.nodes[0]),
            ImpactRegion::Subtree(fixture.nodes[1]),
            ImpactRegion::StrictSubtree(fixture.nodes[1]),
            ImpactRegion::StrictSubtree(fixture.nodes[4]),
            ImpactRegion::NextSibling(fixture.nodes[1]),
            ImpactRegion::NextSibling(fixture.nodes[3]),
            ImpactRegion::FollowingSiblings(fixture.nodes[1]),
            ImpactRegion::FollowingSiblings(fixture.nodes[3]),
            ImpactRegion::FollowingSiblingSubtrees(fixture.nodes[1]),
            ImpactRegion::FollowingSiblingSubtrees(fixture.nodes[3]),
            ImpactRegion::SiblingSequence(fixture.nodes[2]),
            ImpactRegion::Ancestors(fixture.nodes[4]),
            ImpactRegion::Ancestors(fixture.nodes[0]),
            ImpactRegion::PreviousSibling(fixture.nodes[2]),
            ImpactRegion::PreviousSibling(fixture.nodes[1]),
            ImpactRegion::PrecedingSiblings(fixture.nodes[3]),
            ImpactRegion::PrecedingSiblings(fixture.nodes[1]),
            ImpactRegion::Document,
        ];
        for region in regions {
            let mut intervals = Vec::new();
            assert!(topology.collect_region_intervals(region, &fixture.tree, Some(fixture.nodes[0]), &mut intervals));
            let mut actual = Vec::new();
            for interval in intervals {
                topology.for_each_interval(interval, &mut |node| {
                    actual.push(fixture.nodes.iter().position(|other| *other == node).unwrap());
                });
            }
            let mut expected = fixture.collect(region);
            if region == ImpactRegion::Document {
                actual.sort_unstable();
                expected.sort_unstable();
            }
            assert_eq!(actual, expected, "{region:?}");
        }

        let mut intervals = Vec::new();
        assert!(!topology.collect_region_intervals(
            ImpactRegion::TreeScope(TreeScopeID::DOCUMENT),
            &fixture.tree,
            Some(fixture.nodes[0]),
            &mut intervals,
        ));
    }

    #[test]
    fn transaction_topology_orders_pending_nodes_without_depth_walks() {
        let fixture = Fixture::new();
        let regions = ImpactRegions::with_topology(&fixture.tree, fixture.nodes[0]);
        let mut pending = vec![
            fixture.nodes[7],
            fixture.nodes[1],
            fixture.nodes[4],
            fixture.nodes[0],
            fixture.nodes[2],
        ];

        regions.sort_nodes_for_top_down_walk(&mut pending, &fixture.tree);

        assert_eq!(
            pending.into_iter().rev().collect::<Vec<_>>(),
            vec![
                fixture.nodes[0],
                fixture.nodes[1],
                fixture.nodes[4],
                fixture.nodes[2],
                fixture.nodes[7],
            ]
        );
    }

    #[test]
    fn transaction_topology_partitions_nested_subtree_roots() {
        let fixture = Fixture::new();
        let regions = ImpactRegions::with_topology(&fixture.tree, fixture.nodes[0]);
        let mut roots = vec![fixture.nodes[7], fixture.nodes[4], fixture.nodes[2], fixture.nodes[1]];

        let (outer, nested) = regions.partition_subtree_roots(&mut roots).unwrap();

        assert_eq!(outer, vec![fixture.nodes[1], fixture.nodes[2], fixture.nodes[7]]);
        assert_eq!(nested, vec![fixture.nodes[4]]);
    }

    #[test]
    fn transaction_topology_collects_the_complement_of_covered_subtrees() {
        let fixture = Fixture::new();
        let mut regions = ImpactRegions::with_topology(&fixture.tree, fixture.nodes[0]);
        regions.add(ImpactRegion::Subtree(fixture.nodes[1]));
        regions.add(ImpactRegion::Subtree(fixture.nodes[7]));

        assert_eq!(
            regions.nodes_outside_covered_subtrees().unwrap(),
            vec![fixture.nodes[0], fixture.nodes[2], fixture.nodes[6], fixture.nodes[3]]
        );
    }

    #[test]
    fn transaction_topology_merges_overlapping_regions_into_intervals() {
        let fixture = Fixture::new();
        let topology = TransactionTopology::new(&fixture.tree, fixture.nodes[0]);
        let intervals = topology
            .canonical_intervals(
                &[
                    ImpactRegion::Subtree(fixture.nodes[1]),
                    ImpactRegion::Node(fixture.nodes[4]),
                    ImpactRegion::FollowingSiblingSubtrees(fixture.nodes[1]),
                ],
                &fixture.tree,
                Some(fixture.nodes[0]),
            )
            .unwrap();

        assert_eq!(intervals, vec![PreorderInterval { start: 1, end: 8 }]);
    }

    #[test]
    fn compiled_region_batches_iterate_and_test_membership_without_node_arrays() {
        let fixture = Fixture::new();
        let plan = ImpactRegions::with_topology(&fixture.tree, fixture.nodes[0]);
        let regions = [
            ImpactRegion::Subtree(fixture.nodes[1]),
            ImpactRegion::Node(fixture.nodes[4]),
            ImpactRegion::FollowingSiblingSubtrees(fixture.nodes[1]),
        ];
        let batch = plan.compile_union(&regions, &fixture.tree, Some(fixture.nodes[0]));

        assert_eq!(batch.interval_count(), Some(1));
        assert_eq!(batch.node_count(), 7);
        assert_eq!(batch.storage_bytes(), size_of::<PreorderInterval>());

        let mut actual = Vec::new();
        plan.for_each_batch(&batch, |node| actual.push(node));
        assert_eq!(
            actual,
            vec![
                fixture.nodes[1],
                fixture.nodes[4],
                fixture.nodes[5],
                fixture.nodes[2],
                fixture.nodes[6],
                fixture.nodes[3],
                fixture.nodes[7],
            ]
        );
        for &node in &fixture.nodes[1..] {
            assert!(plan.batch_contains_node(&batch, node));
        }
        assert!(!plan.batch_contains_node(&batch, fixture.nodes[0]));
    }

    #[test]
    fn impact_regions_use_transaction_topology_for_subtree_membership() {
        let fixture = Fixture::new();
        let regions = ImpactRegions::with_topology(&fixture.tree, fixture.nodes[0]);
        let region = ImpactRegion::Subtree(fixture.nodes[1]);

        assert!(regions.region_contains_node(region, fixture.nodes[4], &fixture.tree));
        assert!(!regions.region_contains_node(region, fixture.nodes[2], &fixture.tree));
    }

    #[test]
    fn covered_subtree_queries_use_coalesced_preorder_intervals() {
        let fixture = Fixture::new();
        let mut regions = ImpactRegions::with_topology(&fixture.tree, fixture.nodes[0]);
        regions.add(ImpactRegion::Subtree(fixture.nodes[1]));
        regions.add(ImpactRegion::Subtree(fixture.nodes[4]));

        assert_eq!(regions.covered_subtrees, [PreorderInterval { start: 1, end: 4 }]);

        assert!(regions.is_covered_by_subtree(ImpactRegion::Node(fixture.nodes[4]), &fixture.tree));
        assert!(regions.is_covered_by_subtree(ImpactRegion::Children(fixture.nodes[5]), &fixture.tree));
        assert!(!regions.is_covered_by_subtree(ImpactRegion::Subtree(fixture.nodes[2]), &fixture.tree));
    }
}
