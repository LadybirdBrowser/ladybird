/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Style node identity and the tree relations StyleEngine navigates.
//!
//! A [`StyleNodeID`] is a document-local `u32`: sufficient for one document, and half the size of a
//! native pointer in every index and handle that mentions it.
//!
//! The parent, first-element-child, and next-element-sibling columns are dense, required, and
//! Rust-owned. That is not an acceleration choice. A transpose program starting from a changed
//! input has to traverse inverse selector relations to reach possible subjects - "descendants that
//! can satisfy `B`" is not in a tree delta - and `StyleNodeID` is deliberately not a tree-order
//! label, so proving that a posting candidate lies inside a subtree costs relation steps per
//! candidate. If those steps left the evaluator, the hot path would degenerate into per-element FFI
//! or reverse cold requests. Keeping the columns resident is what makes the selective plan viable.
//!
//! The columns are **not** semantically authoritative. They are a derived projection that must agree
//! with the live tree at every epoch boundary, maintained from the same tree delta that reports the
//! mutation.

use super::fast_hash::FastMap as HashMap;
use super::fast_hash::FastSet as HashSet;
use std::cmp::Ordering;
use std::num::NonZeroU32;

use super::capacity::capacity_bytes;
use super::column::BitColumn;
use super::column::PagedColumn;
use super::column::PagedColumnPage;
use super::column::RemovablePagedColumnPage;
use super::index::StyleAtomID;
use super::memory::MemoryCategory;
use super::memory::MemoryController;
use super::transaction::TreeRelations;

/// Document-local identity of an element.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct StyleNodeID(NonZeroU32);

impl StyleNodeID {
    /// `index` is a dense element index starting at 1.
    #[must_use]
    pub fn element(index: u32) -> Self {
        assert!(index != 0, "element index 0 is reserved");
        Self(NonZeroU32::new(index).unwrap())
    }

    /// The dense element index.
    #[must_use]
    pub fn element_index(self) -> Option<u32> {
        Some(self.0.get())
    }

    #[must_use]
    pub fn raw(self) -> u32 {
        self.0.get()
    }

    #[must_use]
    pub fn from_raw(raw: u32) -> Option<Self> {
        NonZeroU32::new(raw).map(Self)
    }
}

define_id! {
    /// Identity of a tree scope: the document tree, or a shadow root's tree.
    pub struct TreeScopeID(pub);
}

impl TreeScopeID {
    pub const DOCUMENT: Self = Self(0);
}

/// A pseudo-element kind, holding the generated CSS pseudo-element enum value.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct PseudoElementKind(pub u16);

/// Which pseudo-element a selector entry targets.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct PseudoElementTarget {
    pub kind: PseudoElementKind,
}

impl PseudoElementTarget {
    #[must_use]
    pub fn new(kind: PseudoElementKind) -> Self {
        Self { kind }
    }
}

const SEGMENTED_NODE_COLUMN_PAGE_SHIFT: usize = 6;
const SEGMENTED_NODE_COLUMN_PAGE_SIZE: usize = 1 << SEGMENTED_NODE_COLUMN_PAGE_SHIFT;

struct SegmentedNodePage<T: Copy> {
    values: [Option<T>; SEGMENTED_NODE_COLUMN_PAGE_SIZE],
}

impl<T: Copy> Default for SegmentedNodePage<T> {
    fn default() -> Self {
        Self {
            values: [None; SEGMENTED_NODE_COLUMN_PAGE_SIZE],
        }
    }
}

impl<T: Copy> PagedColumnPage for SegmentedNodePage<T> {
    type Value = T;

    const SHIFT: usize = SEGMENTED_NODE_COLUMN_PAGE_SHIFT;

    fn get(&self, index: usize) -> Option<T> {
        self.values[index]
    }

    fn insert(&mut self, index: usize, value: T) {
        self.values[index] = Some(value);
    }
}

impl<T: Copy> RemovablePagedColumnPage for SegmentedNodePage<T> {
    fn remove(&mut self, index: usize) -> Option<T> {
        self.values[index].take()
    }
}

/// A conditional column addressed directly by the dense element part of `StyleNodeID`.
///
/// The page directory makes an absent column segment cost one pointer rather than one value per
/// document node.
pub(super) struct SegmentedNodeColumn<T: Copy>(PagedColumn<SegmentedNodePage<T>>);

impl<T: Copy> Default for SegmentedNodeColumn<T> {
    fn default() -> Self {
        Self(PagedColumn::default())
    }
}

impl<T: Copy> SegmentedNodeColumn<T> {
    pub(super) fn get(&self, node: StyleNodeID) -> Option<T> {
        let index = node.element_index()? as usize;
        self.0.get(index)
    }

    pub(super) fn insert(&mut self, node: StyleNodeID, value: T) -> Option<T> {
        let index = node
            .element_index()
            .expect("conditional tree relations connect DOM nodes") as usize;
        self.0.insert(index, value).0
    }

    pub(super) fn remove(&mut self, node: StyleNodeID) -> Option<T> {
        let index = node.element_index()? as usize;
        self.0.remove(index)
    }

    fn capacity_bytes(&self) -> u64 {
        self.0.capacity_bytes()
    }
}

#[derive(Clone, Copy)]
struct StagedTreeValue<T: Copy> {
    before: T,
    after: T,
    dirty: bool,
}

/// Transaction-local before/after rows for the tree relation family.
///
/// Pages are addressed by dense element identity. The touched lists exist only to drain populated
/// rows without scanning the document-wide page directory at the commit barrier.
#[derive(Default)]
pub(super) struct TreeRelationStaging {
    rows: SegmentedNodeColumn<StagedTreeValue<Option<TreeRelations>>>,
    touched_rows: Vec<StyleNodeID>,
    dirty_rows: Vec<StyleNodeID>,
    first_children: SegmentedNodeColumn<StagedTreeValue<Option<StyleNodeID>>>,
    touched_first_children: Vec<StyleNodeID>,
    dirty_first_children: Vec<StyleNodeID>,
    applied: bool,
}

type StagedTreeRows = Vec<(StyleNodeID, Option<TreeRelations>, Option<TreeRelations>)>;

fn radix_sort_style_node_ids(mut nodes: Vec<StyleNodeID>) -> Vec<StyleNodeID> {
    if nodes.is_sorted() {
        return nodes;
    }
    let mut scratch = vec![nodes[0]; nodes.len()];
    let significant_bits = u32::BITS - nodes.iter().map(|node| node.raw()).max().unwrap().leading_zeros();
    for shift in (0..significant_bits).step_by(u8::BITS as usize) {
        let mut offsets = [0_usize; 1 << u8::BITS];
        for node in &nodes {
            offsets[((node.raw() >> shift) & u8::MAX as u32) as usize] += 1;
        }
        let mut offset = 0;
        for count in &mut offsets {
            let next_offset = offset + *count;
            *count = offset;
            offset = next_offset;
        }
        for &node in &nodes {
            let bucket = ((node.raw() >> shift) & u8::MAX as u32) as usize;
            scratch[offsets[bucket]] = node;
            offsets[bucket] += 1;
        }
        std::mem::swap(&mut nodes, &mut scratch);
    }
    nodes
}

impl TreeRelationStaging {
    pub(super) fn is_empty(&self) -> bool {
        self.touched_rows.is_empty() && self.touched_first_children.is_empty()
    }

    pub(super) fn is_applied(&self) -> bool {
        self.applied
    }

    pub(super) fn current_row(&self, node: StyleNodeID, unstaged: Option<TreeRelations>) -> Option<TreeRelations> {
        self.rows.get(node).map_or(unstaged, |pair| pair.after)
    }

    pub(super) fn stage_row(&mut self, node: StyleNodeID, before: Option<TreeRelations>, after: Option<TreeRelations>) {
        self.applied = false;
        match self.rows.get(node) {
            Some(mut pair) => {
                pair.after = after;
                if !pair.dirty {
                    pair.dirty = true;
                    self.dirty_rows.push(node);
                }
                self.rows.insert(node, pair);
            }
            None => {
                self.rows.insert(
                    node,
                    StagedTreeValue {
                        before,
                        after,
                        dirty: true,
                    },
                );
                self.touched_rows.push(node);
                self.dirty_rows.push(node);
            }
        }
    }

    pub(super) fn stage_first_child(
        &mut self,
        parent: StyleNodeID,
        before: Option<StyleNodeID>,
        after: Option<StyleNodeID>,
    ) {
        self.applied = false;
        match self.first_children.get(parent) {
            Some(mut pair) => {
                pair.after = after;
                if !pair.dirty {
                    pair.dirty = true;
                    self.dirty_first_children.push(parent);
                }
                self.first_children.insert(parent, pair);
            }
            None => {
                self.first_children.insert(
                    parent,
                    StagedTreeValue {
                        before,
                        after,
                        dirty: true,
                    },
                );
                self.touched_first_children.push(parent);
                self.dirty_first_children.push(parent);
            }
        }
    }

    pub(super) fn rows(
        &self,
    ) -> impl Iterator<Item = (StyleNodeID, Option<TreeRelations>, Option<TreeRelations>)> + '_ {
        self.touched_rows.iter().copied().map(|node| {
            let pair = self.rows.get(node).expect("touched tree row must be staged");
            (node, pair.before, pair.after)
        })
    }

    pub(super) fn first_children(
        &self,
    ) -> impl Iterator<Item = (StyleNodeID, Option<StyleNodeID>, Option<StyleNodeID>)> + '_ {
        self.touched_first_children.iter().copied().map(|parent| {
            let pair = self
                .first_children
                .get(parent)
                .expect("touched first-child row must be staged");
            (parent, pair.before, pair.after)
        })
    }

    pub(super) fn dirty_rows(&self) -> StagedTreeRows {
        radix_sort_style_node_ids(self.dirty_rows.clone())
            .into_iter()
            .map(|node| {
                let pair = self.rows.get(node).expect("dirty tree row must be staged");
                (node, pair.before, pair.after)
            })
            .collect()
    }

    pub(super) fn dirty_first_children(
        &self,
    ) -> impl Iterator<Item = (StyleNodeID, Option<StyleNodeID>, Option<StyleNodeID>)> + '_ {
        self.dirty_first_children.iter().copied().map(|parent| {
            let pair = self
                .first_children
                .get(parent)
                .expect("dirty first-child row must be staged");
            (parent, pair.before, pair.after)
        })
    }

    pub(super) fn before_relations(&self, node: StyleNodeID, resident: Option<TreeRelations>) -> Option<TreeRelations> {
        self.rows.get(node).map_or(resident, |pair| pair.before)
    }

    pub(super) fn before_first_child(&self, parent: StyleNodeID, resident: Option<StyleNodeID>) -> Option<StyleNodeID> {
        self.first_children.get(parent).map_or(resident, |pair| pair.before)
    }

    pub(super) fn mark_applied(&mut self) {
        for &node in &self.dirty_rows {
            let mut pair = self.rows.get(node).expect("touched tree row must be staged");
            pair.dirty = false;
            self.rows.insert(node, pair);
        }
        for &parent in &self.dirty_first_children {
            let mut pair = self
                .first_children
                .get(parent)
                .expect("touched first-child row must be staged");
            pair.dirty = false;
            self.first_children.insert(parent, pair);
        }
        self.dirty_rows.clear();
        self.dirty_first_children.clear();
        self.applied = true;
    }

    pub(super) fn clear(&mut self) {
        *self = Self::default();
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        self.rows.capacity_bytes()
            + self.first_children.capacity_bytes()
            + (self.touched_rows.capacity() * size_of::<StyleNodeID>()) as u64
            + (self.dirty_rows.capacity() * size_of::<StyleNodeID>()) as u64
            + (self.touched_first_children.capacity() * size_of::<StyleNodeID>()) as u64
            + (self.dirty_first_children.capacity() * size_of::<StyleNodeID>()) as u64
    }
}

/// Sparse shadow relations, allocated only for documents that have shadow trees.
///
/// These are the facts the flat tree is derived from rather than a second child list. Storing
/// flat-tree children directly would cost two more words per node and duplicate information the
/// slot and host relations already carry; deriving costs one lookup at the two places the flat tree
/// actually diverges from the DOM tree.
#[derive(Default)]
struct ShadowRelations {
    /// A slotted node's slot.
    assigned_slot: SegmentedNodeColumn<StyleNodeID>,
    /// A slot's assigned nodes, in assignment order.
    assigned_nodes: HashMap<StyleNodeID, Vec<StyleNodeID>>,
    /// A shadow host's shadow root.
    shadow_root: SegmentedNodeColumn<StyleNodeID>,
    /// A shadow root's host.
    host: SegmentedNodeColumn<StyleNodeID>,
    /// Every name an element is addressable by paired with the host of the level that name reaches.
    ///
    /// `exportparts` forwards a name outwards under a name of the host's choosing, so a name and a
    /// host only answer a `::part()` rule together: the flattened name set says an element answers
    /// to some name somewhere, which is what candidate discovery needs, while a rule matches only
    /// when one level exposes the name it writes and the host of that same level is the element its
    /// outer compound describes.
    part_hosts: HashMap<StyleNodeID, Vec<(StyleAtomID, StyleNodeID)>>,
}

impl ShadowRelations {
    fn retire_node(&mut self, node: StyleNodeID) {
        if let Some(slot) = self.assigned_slot.remove(node)
            && let Some(nodes) = self.assigned_nodes.get_mut(&slot)
        {
            nodes.retain(|&assigned| assigned != node);
        }
        if let Some(nodes) = self.assigned_nodes.remove(&node) {
            for assigned in nodes {
                if self.assigned_slot.get(assigned) == Some(node) {
                    self.assigned_slot.remove(assigned);
                }
            }
        }
        if let Some(root) = self.shadow_root.remove(node) {
            self.host.remove(root);
        }
        if let Some(host) = self.host.remove(node) {
            self.shadow_root.remove(host);
        }
        self.part_hosts.remove(&node);
    }

    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.assigned_nodes, self.part_hosts];
            cached [];
            nested [
                self.assigned_slot.capacity_bytes(),
                self.shadow_root.capacity_bytes(),
                self.host.capacity_bytes(),
                self.assigned_nodes
                    .values()
                    .map(|nodes| nodes.capacity() * size_of::<StyleNodeID>())
                    .sum::<usize>(),
                self.part_hosts
                    .values()
                    .map(|pairs| pairs.capacity() * size_of::<(StyleAtomID, StyleNodeID)>())
                    .sum::<usize>(),
            ];
            skip [];
        }
    }
}

/// Flat-tree children of one node.
pub enum FlatTreeChildren<'a> {
    /// The node's DOM children, which is the common case and the whole story for a document with
    /// no shadow trees.
    Dom(Children<'a>),
    /// A slot's assigned nodes, or a shadow host's shadow-root children.
    Assigned(std::slice::Iter<'a, StyleNodeID>),
}

impl Iterator for FlatTreeChildren<'_> {
    type Item = StyleNodeID;

    fn next(&mut self) -> Option<StyleNodeID> {
        match self {
            Self::Dom(children) => children.next(),
            Self::Assigned(nodes) => nodes.next().copied(),
        }
    }
}

/// The Rust-owned projection of the tree relations selectors navigate.
///
/// Element columns are indexed by element index, with slot 0 unused so that a `StyleNodeID` indexes
/// its own column entry directly.
pub struct StyleNodeTree {
    // Required, dense.
    parent: Vec<Option<StyleNodeID>>,
    first_element_child: Vec<Option<StyleNodeID>>,
    next_element_sibling: Vec<Option<StyleNodeID>>,
    previous_element_sibling: Vec<Option<StyleNodeID>>,
    depth: Vec<u32>,

    // Conditional: allocated only for documents that need them.
    tree_scope: Option<Vec<TreeScopeID>>,

    live: BitColumn,
    connected_element_count: u32,
    /// Identities retired in the current epoch. They cannot be reused until the epoch that could
    /// still observe them has retired.
    pending_reuse: Vec<u32>,
    free_element_indexes: Vec<u32>,

    /// Allocated only once a shadow tree exists.
    shadow: Option<Box<ShadowRelations>>,

    capacity_bytes: u64,

    #[cfg(test)]
    depth_recompute_visits: usize,
}

impl StyleNodeTree {
    pub(super) fn collect_atoms(&self, atoms: &mut HashSet<StyleAtomID>) -> u64 {
        let Some(shadow) = &self.shadow else {
            return 0;
        };
        let mut visited = 0_u64;
        for pairs in shadow.part_hosts.values() {
            visited += u64::try_from(pairs.len()).expect("part host count exceeds u64");
            atoms.extend(pairs.iter().map(|&(atom, _)| atom));
        }
        visited
    }

    #[must_use]
    pub fn new(memory: &mut MemoryController) -> Self {
        let mut tree = Self {
            parent: Vec::new(),
            first_element_child: Vec::new(),
            next_element_sibling: Vec::new(),
            previous_element_sibling: Vec::new(),
            depth: Vec::new(),
            tree_scope: None,
            live: BitColumn::default(),
            connected_element_count: 0,
            pending_reuse: Vec::new(),
            free_element_indexes: Vec::new(),
            shadow: None,
            capacity_bytes: 0,
            #[cfg(test)]
            depth_recompute_visits: 0,
        };
        // Slot 0 is never a valid identity; reserving it keeps column indexing direct.
        tree.parent.push(None);
        tree.first_element_child.push(None);
        tree.next_element_sibling.push(None);
        tree.previous_element_sibling.push(None);
        tree.depth.push(0);
        tree.capacity_bytes = tree.recompute_capacity_bytes();
        memory.reserve_required(MemoryCategory::RelationColumns, tree.capacity_bytes);
        tree
    }

    /// Connected styleable elements, which is the count the document memory budget is written in.
    #[must_use]
    pub fn connected_element_count(&self) -> u32 {
        self.connected_element_count
    }

    #[must_use]
    pub fn is_live(&self, node: StyleNodeID) -> bool {
        self.live.contains(node.element_index().unwrap() as usize)
    }

    /// Every live style-tree identity, including the synthetic roots of shadow trees.
    pub fn live_nodes(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        (1..self.parent.len()).filter_map(|index| {
            let index = u32::try_from(index).expect("style node index space exhausted");
            self.live.contains(index as usize).then(|| StyleNodeID::element(index))
        })
    }

    // -- Identity lifecycle ------------------------------------------------------------------

    /// Allocate an element identity. Reuses a slot only once the epoch that could still observe its
    /// previous occupant has retired.
    pub fn allocate_element(&mut self, memory: &mut MemoryController) -> StyleNodeID {
        let (index, capacity_before_growth) = match self.free_element_indexes.pop() {
            Some(index) => {
                self.parent[index as usize] = None;
                self.first_element_child[index as usize] = None;
                self.next_element_sibling[index as usize] = None;
                self.previous_element_sibling[index as usize] = None;
                self.depth[index as usize] = 0;
                if let Some(column) = self.tree_scope.as_mut() {
                    column[index as usize] = TreeScopeID::DOCUMENT;
                }
                (index, None)
            }
            None => {
                let capacity_before_growth = self.identity_capacity_bytes();
                let index = u32::try_from(self.parent.len()).expect("element index space exhausted");
                self.parent.push(None);
                self.first_element_child.push(None);
                self.next_element_sibling.push(None);
                self.previous_element_sibling.push(None);
                self.depth.push(0);
                if let Some(column) = self.tree_scope.as_mut() {
                    column.push(TreeScopeID::DOCUMENT);
                }
                (index, Some(capacity_before_growth))
            }
        };
        self.live.set(index as usize, true);
        if let Some(capacity_before_growth) = capacity_before_growth {
            let current = self.identity_capacity_bytes();
            self.record_capacity_change(memory, capacity_before_growth, current);
        }
        self.connected_element_count += 1;
        StyleNodeID::element(index)
    }

    /// Retire an element identity. The slot stays reserved until [`Self::release_retired_identities`]
    /// runs at epoch retirement, so no reader can observe a reused identity.
    pub fn retire_element(&mut self, node: StyleNodeID, memory: &mut MemoryController) {
        let before = self.retirement_capacity_bytes();
        let index = node
            .element_index()
            .expect("retire_element requires an element identity");
        assert!(
            self.live.contains(index as usize),
            "retiring an identity that is not live"
        );
        if let Some(shadow) = &mut self.shadow {
            shadow.retire_node(node);
        }
        self.live.set(index as usize, false);
        self.parent[index as usize] = None;
        self.first_element_child[index as usize] = None;
        self.next_element_sibling[index as usize] = None;
        self.previous_element_sibling[index as usize] = None;
        self.depth[index as usize] = 0;
        self.connected_element_count -= 1;
        self.pending_reuse.push(index);
        let current = self.retirement_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// Called once the read epoch that could still name the retired identities has retired.
    pub fn release_retired_identities(&mut self, memory: &mut MemoryController) {
        let before = self.reuse_capacity_bytes();
        self.free_element_indexes.append(&mut self.pending_reuse);
        let current = self.reuse_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    #[must_use]
    #[cfg(test)]
    pub fn retired_identities_pending_release(&self) -> usize {
        self.pending_reuse.len()
    }

    // -- Relation maintenance ----------------------------------------------------------------

    pub fn set_parent(&mut self, node: StyleNodeID, parent: Option<StyleNodeID>) {
        let depth = parent.map_or(0, |parent| {
            self.depth(parent).checked_add(1).expect("style tree depth exhausted")
        });
        self.set_subtree_depth(node, depth);
        let index = self.live_element_index(node);
        self.parent[index] = parent;
    }

    fn set_subtree_depth(&mut self, node: StyleNodeID, depth: u32) {
        let index = self.live_element_index(node);
        let previous_depth = self.depth[index];
        if depth != previous_depth {
            let adjustment = i64::from(depth) - i64::from(previous_depth);
            let mut next = Some(node);
            while let Some(descendant) = next {
                let descendant_index = self.live_element_index(descendant);
                self.depth[descendant_index] = u32::try_from(i64::from(self.depth[descendant_index]) + adjustment)
                    .expect("style tree depth exhausted");
                next = self.first_element_child[descendant_index].or_else(|| {
                    let mut candidate = descendant;
                    loop {
                        if candidate == node {
                            return None;
                        }
                        let candidate_index = self.element_index(candidate);
                        if let Some(sibling) = self.next_element_sibling[candidate_index] {
                            return Some(sibling);
                        }
                        candidate = self.parent[candidate_index]?;
                    }
                });
            }
        }
    }

    pub(super) fn set_parent_without_updating_depth(&mut self, node: StyleNodeID, parent: Option<StyleNodeID>) {
        let index = self.live_element_index(node);
        self.parent[index] = parent;
    }

    /// Update one final staged subtree after all parent and sibling columns are installed.
    pub(super) fn recompute_subtree_depth(&mut self, root: StyleNodeID) {
        let mut next = Some(root);
        while let Some(node) = next {
            #[cfg(test)]
            {
                self.depth_recompute_visits += 1;
            }
            let index = self.live_element_index(node);
            self.depth[index] = self.parent[index].map_or(0, |parent| {
                self.depth(parent).checked_add(1).expect("style tree depth exhausted")
            });
            next = self.first_element_child[index].or_else(|| {
                let mut candidate = node;
                loop {
                    if candidate == root {
                        return None;
                    }
                    let candidate_index = self.element_index(candidate);
                    if let Some(sibling) = self.next_element_sibling[candidate_index] {
                        return Some(sibling);
                    }
                    candidate = self.parent[candidate_index]?;
                }
            });
        }
    }

    #[cfg(test)]
    pub(super) fn take_depth_recompute_visits(&mut self) -> usize {
        core::mem::take(&mut self.depth_recompute_visits)
    }

    pub fn set_first_element_child(&mut self, node: StyleNodeID, child: Option<StyleNodeID>) {
        let index = self.live_element_index(node);
        self.first_element_child[index] = child;
    }

    pub fn set_next_element_sibling(&mut self, node: StyleNodeID, sibling: Option<StyleNodeID>) {
        let index = self.live_element_index(node);
        self.next_element_sibling[index] = sibling;
    }

    pub fn set_previous_element_sibling(&mut self, node: StyleNodeID, sibling: Option<StyleNodeID>) {
        let index = self.live_element_index(node);
        self.previous_element_sibling[index] = sibling;
    }

    /// Allocate the tree-scope column. A single-scope document never pays for it.
    pub fn enable_tree_scopes(&mut self, memory: &mut MemoryController) {
        if self.tree_scope.is_some() {
            return;
        }
        self.tree_scope = Some(vec![TreeScopeID::DOCUMENT; self.parent.len()]);
        let current = self
            .tree_scope
            .as_ref()
            .map_or(0, |column| column.capacity() * size_of::<TreeScopeID>());
        self.record_capacity_change(memory, 0, current as u64);
    }

    #[must_use]
    pub fn has_tree_scopes(&self) -> bool {
        self.tree_scope.is_some()
    }

    pub fn set_tree_scope(&mut self, node: StyleNodeID, scope: TreeScopeID) {
        let index = self.live_element_index(node);
        let column = self
            .tree_scope
            .as_mut()
            .expect("set_tree_scope requires the tree-scope column");
        column[index] = scope;
    }

    // -- Shadow relations --------------------------------------------------------------------

    fn shadow_mut(&mut self) -> &mut ShadowRelations {
        self.shadow.get_or_insert_with(Box::default)
    }

    #[must_use]
    #[cfg(test)]
    pub fn has_shadow_relations(&self) -> bool {
        self.shadow.is_some()
    }

    /// Record that `host` hosts `shadow_root`.
    pub fn set_shadow_root(&mut self, host: StyleNodeID, shadow_root: StyleNodeID, memory: &mut MemoryController) {
        let before = self.shadow_capacity_bytes();
        let shadow = self.shadow_mut();
        if let Some(previous_root) = shadow.shadow_root.insert(host, shadow_root)
            && previous_root != shadow_root
            && shadow.host.get(previous_root) == Some(host)
        {
            shadow.host.remove(previous_root);
        }
        if let Some(previous_host) = shadow.host.insert(shadow_root, host)
            && previous_host != host
            && shadow.shadow_root.get(previous_host) == Some(shadow_root)
        {
            shadow.shadow_root.remove(previous_host);
        }
        let current = self.shadow_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    #[must_use]
    pub fn shadow_root_of(&self, host: StyleNodeID) -> Option<StyleNodeID> {
        self.shadow.as_ref()?.shadow_root.get(host)
    }

    #[must_use]
    pub fn host_of(&self, shadow_root: StyleNodeID) -> Option<StyleNodeID> {
        self.shadow.as_ref()?.host.get(shadow_root)
    }

    /// Assign `node` to `slot`. Passing `None` removes the assignment.
    ///
    /// Slot assignment changes flat-tree identity even when the DOM parent does not move, which is
    /// why it is its own relation rather than a derived view of the DOM tree.
    pub fn set_assigned_slot(&mut self, node: StyleNodeID, slot: Option<StyleNodeID>, memory: &mut MemoryController) {
        let before = self.shadow_capacity_bytes();
        let shadow = self.shadow_mut();
        if let Some(previous) = shadow.assigned_slot.remove(node)
            && let Some(nodes) = shadow.assigned_nodes.get_mut(&previous)
        {
            nodes.retain(|assigned| *assigned != node);
        }
        if let Some(slot) = slot {
            shadow.assigned_slot.insert(node, slot);
            shadow.assigned_nodes.entry(slot).or_default().push(node);
        }
        let current = self.shadow_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// The shadow host of the tree `node` is in, if it is in one.
    ///
    /// A shadow root is the parent its top-level children name, so the root of a node's parent
    /// chain is the shadow root when there is one, and that root knows its host.
    #[must_use]
    pub fn shadow_host_of(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        let shadow = self.shadow.as_ref()?;
        let mut root = node;
        while let Some(parent) = self.parent(root) {
            root = parent;
        }
        shadow.host.get(root)
    }

    #[must_use]
    pub fn assigned_slot_of(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.shadow.as_ref()?.assigned_slot.get(node)
    }

    #[must_use]
    pub fn assigned_nodes_of(&self, slot: StyleNodeID) -> &[StyleNodeID] {
        self.shadow
            .as_ref()
            .and_then(|shadow| shadow.assigned_nodes.get(&slot))
            .map_or(&[], Vec::as_slice)
    }

    pub fn set_part_hosts(
        &mut self,
        node: StyleNodeID,
        pairs: &[(StyleAtomID, StyleNodeID)],
        memory: &mut MemoryController,
    ) {
        let before = self.shadow_capacity_bytes();
        let shadow = self.shadow_mut();
        if pairs.is_empty() {
            shadow.part_hosts.remove(&node);
        } else {
            pairs.clone_into(shadow.part_hosts.entry(node).or_default());
        }
        let current = self.shadow_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// Every (name, host) pair the element answers a `::part()` rule under.
    #[must_use]
    pub fn part_hosts_of(&self, node: StyleNodeID) -> &[(StyleAtomID, StyleNodeID)] {
        self.shadow
            .as_ref()
            .and_then(|shadow| shadow.part_hosts.get(&node))
            .map_or(&[], Vec::as_slice)
    }

    /// The flat-tree children of `node`: what inheritance and `::slotted()` actually walk.
    ///
    /// The flat tree diverges from the DOM tree in exactly two places. A shadow host's flat-tree
    /// children are its shadow root's children, and a slot's are its assigned nodes - falling back
    /// to its DOM children when nothing is assigned, which is what fallback content means.
    #[must_use]
    pub fn flat_tree_children(&self, node: StyleNodeID) -> FlatTreeChildren<'_> {
        let Some(shadow) = self.shadow.as_ref() else {
            return FlatTreeChildren::Dom(self.children(node));
        };
        if let Some(shadow_root) = shadow.shadow_root.get(node) {
            return FlatTreeChildren::Dom(self.children(shadow_root));
        }
        match shadow.assigned_nodes.get(&node) {
            Some(nodes) if !nodes.is_empty() => FlatTreeChildren::Assigned(nodes.iter()),
            _ => FlatTreeChildren::Dom(self.children(node)),
        }
    }

    /// The flat-tree parent whose inherited style an element consumes.
    #[must_use]
    pub fn flat_tree_parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        if let Some(slot) = self.assigned_slot_of(node) {
            return Some(slot);
        }
        let parent = self.parent(node)?;
        if let Some(host) = self.host_of(parent) {
            return Some(host);
        }
        let shadow = self.shadow.as_ref();
        if shadow.is_some_and(|shadow| shadow.shadow_root.get(parent).is_some()) {
            return None;
        }
        if shadow
            .and_then(|shadow| shadow.assigned_nodes.get(&parent))
            .is_some_and(|assigned| !assigned.is_empty())
        {
            return None;
        }
        Some(parent)
    }

    /// Compare nodes in the order C++ must apply style reactions.
    ///
    /// This is preorder over the style-inheritance tree, extended to keep shadow-tree children
    /// before a host's light-tree children and slot fallback before assigned slottables. Comparing
    /// relation columns directly avoids materializing an ancestor path for every reaction.
    #[must_use]
    pub fn compare_style_reaction_order(&self, first: StyleNodeID, second: StyleNodeID) -> Ordering {
        if first == second {
            return Ordering::Equal;
        }

        let relation_depth = |mut node| {
            let mut depth = 0u32;
            while let Some((parent, _)) = self.style_reaction_parent(node) {
                node = parent;
                depth += 1;
            }
            depth
        };

        let mut first_node = first;
        let mut second_node = second;
        let mut first_depth = relation_depth(first);
        let mut second_depth = relation_depth(second);
        while first_depth > second_depth {
            first_node = self
                .style_reaction_parent(first_node)
                .expect("a non-root reaction node must have a parent")
                .0;
            first_depth -= 1;
            if first_node == second_node {
                return Ordering::Greater;
            }
        }
        while second_depth > first_depth {
            second_node = self
                .style_reaction_parent(second_node)
                .expect("a non-root reaction node must have a parent")
                .0;
            second_depth -= 1;
            if second_node == first_node {
                return Ordering::Less;
            }
        }

        loop {
            let first_parent = self.style_reaction_parent(first_node);
            let second_parent = self.style_reaction_parent(second_node);
            if first_parent.map(|(parent, _)| parent) == second_parent.map(|(parent, _)| parent) {
                let first_branch = first_parent.map_or(0, |(_, branch)| branch);
                let second_branch = second_parent.map_or(0, |(_, branch)| branch);
                return (first_branch, first_node.raw()).cmp(&(second_branch, second_node.raw()));
            }
            first_node = first_parent
                .expect("different reaction roots must meet at the virtual root")
                .0;
            second_node = second_parent
                .expect("different reaction roots must meet at the virtual root")
                .0;
        }
    }

    fn style_reaction_parent(&self, node: StyleNodeID) -> Option<(StyleNodeID, u8)> {
        if let Some(slot) = self.assigned_slot_of(node) {
            return Some((slot, 2));
        }
        let parent = self.parent(node)?;
        if let Some(host) = self.host_of(parent) {
            return Some((host, 0));
        }
        let branch = u8::from(self.shadow_root_of(parent).is_some());
        Some((parent, branch))
    }

    // -- Navigation --------------------------------------------------------------------------

    #[must_use]
    pub fn parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.parent[self.element_index(node)]
    }

    #[must_use]
    pub fn first_element_child(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.first_element_child[self.element_index(node)]
    }

    #[must_use]
    pub fn next_element_sibling(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.next_element_sibling[self.element_index(node)]
    }

    #[must_use]
    pub fn tree_scope(&self, node: StyleNodeID) -> TreeScopeID {
        match self.tree_scope.as_ref() {
            Some(column) => column[self.element_index(node)],
            None => TreeScopeID::DOCUMENT,
        }
    }

    #[must_use]
    pub fn depth(&self, node: StyleNodeID) -> u32 {
        self.depth[self.element_index(node)]
    }

    /// The preceding element sibling, served from a resident column.
    ///
    /// This column earned its four bytes per element by measurement: the scan it replaced walked
    /// the child sequence per backward hop, and waypoint checks walking sibling steps backwards
    /// were paying it once per candidate, 19 percent of sibling-heavy style updates.
    #[must_use]
    pub fn previous_element_sibling(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.previous_element_sibling[self.element_index(node)]
    }

    #[must_use]
    pub fn children(&self, node: StyleNodeID) -> Children<'_> {
        Children {
            tree: self,
            next: self.first_element_child(node),
        }
    }

    #[must_use]
    pub fn ancestors(&self, node: StyleNodeID) -> Ancestors<'_> {
        Ancestors {
            tree: self,
            next: self.parent(node),
        }
    }

    /// Preorder stream of `root` and its descendants. Broad regions are streamed this way rather
    /// than joined against postings.
    #[must_use]
    pub fn preorder(&self, root: StyleNodeID) -> Preorder<'_> {
        Preorder {
            tree: self,
            root,
            next: Some(root),
        }
    }

    /// Whether `node` lies inside the subtree rooted at `root`. Because `StyleNodeID` is not a
    /// tree-order label, a subtree impact region is not a numeric interval. The depth column rejects
    /// impossible membership immediately and bounds the remaining parent walk exactly.
    #[must_use]
    pub fn is_in_subtree_of(&self, node: StyleNodeID, root: StyleNodeID) -> bool {
        let node_depth = self.depth(node);
        let root_depth = self.depth(root);
        if node_depth < root_depth {
            return false;
        }
        let mut candidate = node;
        for _ in root_depth..node_depth {
            let Some(parent) = self.parent(candidate) else {
                return false;
            };
            candidate = parent;
        }
        candidate == root
    }

    // -- Accounting --------------------------------------------------------------------------

    /// Exact capacity of every column, charged to Tier 1.
    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        self.capacity_bytes
    }

    fn identity_capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.parent,
                self.first_element_child,
                self.next_element_sibling,
                self.previous_element_sibling,
                self.depth,
            ];
            cached [];
            nested [
                self.tree_scope
                    .as_ref()
                    .map_or(0, |column| column.capacity() * size_of::<TreeScopeID>()),
                self.live.capacity_bytes(),
            ];
            skip [];
        }
    }

    fn reuse_capacity_bytes(&self) -> u64 {
        ((self.pending_reuse.capacity() + self.free_element_indexes.capacity()) * size_of::<u32>()) as u64
    }

    fn shadow_capacity_bytes(&self) -> u64 {
        self.shadow.as_ref().map_or(0, |relations| relations.capacity_bytes())
    }

    fn retirement_capacity_bytes(&self) -> u64 {
        (self.pending_reuse.capacity() * size_of::<u32>()) as u64 + self.shadow_capacity_bytes()
    }

    fn recompute_capacity_bytes(&self) -> u64 {
        self.identity_capacity_bytes() + self.reuse_capacity_bytes() + self.shadow_capacity_bytes()
    }

    fn record_capacity_change(&mut self, memory: &mut MemoryController, previous: u64, current: u64) {
        if current > previous {
            let growth = current - previous;
            self.capacity_bytes += growth;
            memory.reserve_required(MemoryCategory::RelationColumns, growth);
        } else if previous > current {
            let shrinkage = previous - current;
            self.capacity_bytes -= shrinkage;
            memory.release(MemoryCategory::RelationColumns, shrinkage);
        }
        #[cfg(test)]
        assert_eq!(self.capacity_bytes, self.recompute_capacity_bytes());
    }

    fn element_index(&self, node: StyleNodeID) -> usize {
        node.element_index()
            .expect("tree relations are keyed by element identity") as usize
    }

    fn live_element_index(&self, node: StyleNodeID) -> usize {
        let index = node
            .element_index()
            .expect("tree relations are keyed by element identity");
        assert!(
            self.live.contains(index as usize),
            "mutating relations of a retired identity"
        );
        index as usize
    }
}

pub struct Children<'a> {
    tree: &'a StyleNodeTree,
    next: Option<StyleNodeID>,
}

impl Iterator for Children<'_> {
    type Item = StyleNodeID;

    fn next(&mut self) -> Option<StyleNodeID> {
        let current = self.next?;
        self.next = self.tree.next_element_sibling(current);
        Some(current)
    }
}

pub struct Ancestors<'a> {
    tree: &'a StyleNodeTree,
    next: Option<StyleNodeID>,
}

impl Iterator for Ancestors<'_> {
    type Item = StyleNodeID;

    fn next(&mut self) -> Option<StyleNodeID> {
        let current = self.next?;
        self.next = self.tree.parent(current);
        Some(current)
    }
}

pub struct Preorder<'a> {
    tree: &'a StyleNodeTree,
    root: StyleNodeID,
    next: Option<StyleNodeID>,
}

impl Iterator for Preorder<'_> {
    type Item = StyleNodeID;

    fn next(&mut self) -> Option<StyleNodeID> {
        let current = self.next?;
        self.next = self.tree.first_element_child(current).or_else(|| {
            let mut node = current;
            loop {
                if node == self.root {
                    return None;
                }
                if let Some(sibling) = self.tree.next_element_sibling(node) {
                    return Some(sibling);
                }
                node = self.tree.parent(node)?;
            }
        });
        Some(current)
    }
}

#[cfg(test)]
mod tests {
    use super::super::memory::DeviceClass;
    use super::*;

    #[test]
    fn radix_sorts_style_node_identities() {
        let mut nodes = vec![
            StyleNodeID::element(u32::MAX),
            StyleNodeID::element(256),
            StyleNodeID::element(65_536),
            StyleNodeID::element(255),
            StyleNodeID::element(1),
        ];

        nodes = radix_sort_style_node_ids(nodes);

        assert_eq!(
            nodes,
            vec![
                StyleNodeID::element(1),
                StyleNodeID::element(255),
                StyleNodeID::element(256),
                StyleNodeID::element(65_536),
                StyleNodeID::element(u32::MAX),
            ]
        );
    }

    #[test]
    fn tree_staging_keeps_exact_before_and_after_rows_across_apply() {
        let node = StyleNodeID::element(1);
        let parent = StyleNodeID::element(2);
        let final_first_child = StyleNodeID::element(4);
        let mut first_after = TreeRelations::detached(TreeScopeID::DOCUMENT);
        let before = Some(first_after);
        first_after.parent = Some(parent);
        let mut final_after = first_after;
        final_after.assigned_slot = Some(StyleNodeID::element(3));
        let mut staging = TreeRelationStaging::default();

        staging.stage_row(node, before, Some(first_after));
        staging.stage_first_child(parent, None, Some(node));
        staging.mark_applied();
        assert!(staging.dirty_rows().is_empty());
        assert!(staging.dirty_first_children().next().is_none());
        staging.stage_row(node, Some(first_after), Some(final_after));
        staging.stage_first_child(parent, Some(node), Some(final_first_child));

        assert_eq!(staging.current_row(node, None), Some(final_after));
        assert_eq!(staging.before_relations(node, Some(final_after)), before);
        assert_eq!(staging.before_first_child(parent, Some(final_first_child)), None);
        assert!(!staging.is_applied());
        let rows: Vec<_> = staging.rows().collect();
        let first_children: Vec<_> = staging.first_children().collect();
        assert_eq!(rows, vec![(node, before, Some(final_after))]);
        assert_eq!(first_children, vec![(parent, None, Some(final_first_child))]);
        assert!(!staging.is_empty());
    }

    #[test]
    fn dirty_tree_rows_are_sorted_by_node_identity() {
        let low = StyleNodeID::element(1);
        let high = StyleNodeID::element(70);
        let relations = Some(TreeRelations::detached(TreeScopeID::DOCUMENT));
        let mut staging = TreeRelationStaging::default();

        staging.stage_row(high, None, relations);
        staging.stage_row(low, None, relations);

        assert_eq!(
            staging.dirty_rows(),
            vec![(low, None, relations), (high, None, relations)]
        );
    }

    /// Builds `parent -> [children]` shapes without repeating relation bookkeeping in every test.
    struct TreeFixture {
        memory: MemoryController,
        tree: StyleNodeTree,
    }

    impl TreeFixture {
        fn new() -> Self {
            let mut memory = MemoryController::new(DeviceClass::ForegroundDesktop);
            let tree = StyleNodeTree::new(&mut memory);
            Self { memory, tree }
        }

        fn element(&mut self) -> StyleNodeID {
            self.tree.allocate_element(&mut self.memory)
        }

        fn attach_children(&mut self, parent: StyleNodeID, children: &[StyleNodeID]) {
            self.tree.set_first_element_child(parent, children.first().copied());
            for (index, &child) in children.iter().enumerate() {
                self.tree.set_parent(child, Some(parent));
                self.tree
                    .set_next_element_sibling(child, children.get(index + 1).copied());
                self.tree
                    .set_previous_element_sibling(child, index.checked_sub(1).map(|index| children[index]));
            }
        }
    }

    #[test]
    fn conditional_node_columns_allocate_only_touched_segments() {
        let mut column = SegmentedNodeColumn::default();
        let first = StyleNodeID::element(1);
        let same_page = StyleNodeID::element(63);
        let next_page = StyleNodeID::element(64);
        let first_value = StyleNodeID::element(10);
        let replacement = StyleNodeID::element(11);

        assert_eq!(column.get(first), None);
        assert_eq!(column.insert(first, first_value), None);
        assert_eq!(column.insert(same_page, first_value), None);
        assert_eq!(column.0.page_count(), 1);
        assert_eq!(column.insert(next_page, first_value), None);
        assert_eq!(column.0.page_count(), 2);
        assert_eq!(column.insert(first, replacement), Some(first_value));
        assert_eq!(column.get(first), Some(replacement));
        assert_eq!(column.remove(first), Some(replacement));
        assert_eq!(column.get(first), None);
        assert_eq!(
            column.capacity_bytes(),
            (column.0.directory_capacity() * size_of::<Option<Box<SegmentedNodePage<StyleNodeID>>>>()
                + column.0.page_count() * size_of::<SegmentedNodePage<StyleNodeID>>()) as u64
        );
    }

    #[test]
    fn traversal_walks_the_resident_columns() {
        let mut fixture = TreeFixture::new();
        let root = fixture.element();
        let first = fixture.element();
        let second = fixture.element();
        let third = fixture.element();
        let grandchild = fixture.element();
        fixture.attach_children(root, &[first, second, third]);
        fixture.attach_children(second, &[grandchild]);

        assert_eq!(
            fixture.tree.children(root).collect::<Vec<_>>(),
            vec![first, second, third]
        );
        assert_eq!(
            fixture.tree.ancestors(grandchild).collect::<Vec<_>>(),
            vec![second, root]
        );
        assert_eq!(
            fixture.tree.preorder(root).collect::<Vec<_>>(),
            vec![root, first, second, grandchild, third]
        );
        assert_eq!(
            fixture.tree.preorder(second).collect::<Vec<_>>(),
            vec![second, grandchild]
        );

        assert_eq!(fixture.tree.previous_element_sibling(first), None);
        assert_eq!(fixture.tree.previous_element_sibling(third), Some(second));

        assert_eq!(fixture.tree.depth(root), 0);
        assert_eq!(fixture.tree.depth(second), 1);
        assert_eq!(fixture.tree.depth(grandchild), 2);

        assert!(fixture.tree.is_in_subtree_of(grandchild, root));
        assert!(fixture.tree.is_in_subtree_of(root, root));
        assert!(!fixture.tree.is_in_subtree_of(first, second));
    }

    #[test]
    fn moving_a_subtree_updates_every_descendant_depth() {
        let mut fixture = TreeFixture::new();
        let root = fixture.element();
        let first = fixture.element();
        let second = fixture.element();
        let child = fixture.element();
        let grandchild = fixture.element();
        fixture.attach_children(root, &[first, second]);
        fixture.attach_children(first, &[child]);
        fixture.attach_children(child, &[grandchild]);

        fixture.attach_children(second, &[first]);

        assert_eq!(fixture.tree.depth(first), 2);
        assert_eq!(fixture.tree.depth(child), 3);
        assert_eq!(fixture.tree.depth(grandchild), 4);
        assert!(fixture.tree.is_in_subtree_of(grandchild, second));
        assert!(!fixture.tree.is_in_subtree_of(second, first));
    }

    #[test]
    fn staged_parent_changes_recompute_depth_after_final_links_are_installed() {
        let mut fixture = TreeFixture::new();
        let root = fixture.element();
        let parent = fixture.element();
        let child = fixture.element();
        fixture.attach_children(root, &[parent]);
        fixture.attach_children(parent, &[child]);

        fixture.tree.set_parent_without_updating_depth(child, Some(root));
        fixture.tree.set_parent_without_updating_depth(parent, Some(child));
        fixture.tree.set_first_element_child(root, Some(child));
        fixture.tree.set_first_element_child(child, Some(parent));
        fixture.tree.set_first_element_child(parent, None);
        fixture.tree.set_next_element_sibling(parent, None);
        fixture.tree.set_next_element_sibling(child, None);
        fixture.tree.recompute_subtree_depth(child);

        assert_eq!(fixture.tree.depth(root), 0);
        assert_eq!(fixture.tree.depth(child), 1);
        assert_eq!(fixture.tree.depth(parent), 2);
    }

    #[test]
    fn a_retired_identity_is_not_reused_before_the_epoch_retires() {
        let mut fixture = TreeFixture::new();
        let first = fixture.element();
        let second = fixture.element();
        assert_eq!(fixture.tree.connected_element_count(), 2);

        fixture.tree.retire_element(first, &mut fixture.memory);
        assert!(!fixture.tree.is_live(first));
        assert_eq!(fixture.tree.connected_element_count(), 1);
        assert_eq!(fixture.tree.retired_identities_pending_release(), 1);

        let third = fixture.element();
        assert_ne!(third, first);
        assert_ne!(third, second);

        fixture.tree.release_retired_identities(&mut fixture.memory);
        let fourth = fixture.element();
        assert_eq!(fourth, first);
        assert!(fixture.tree.is_live(fourth));
    }

    #[test]
    fn a_reused_identity_starts_in_the_document_tree_scope() {
        let mut fixture = TreeFixture::new();
        let node = fixture.element();
        fixture.tree.enable_tree_scopes(&mut fixture.memory);
        fixture.tree.set_tree_scope(node, TreeScopeID(1));
        fixture.tree.retire_element(node, &mut fixture.memory);
        fixture.tree.release_retired_identities(&mut fixture.memory);

        let reused = fixture.element();
        assert_eq!(reused, node);
        assert_eq!(fixture.tree.tree_scope(reused), TreeScopeID::DOCUMENT);
    }

    #[test]
    fn reused_element_identities_do_not_inherit_shadow_relations() {
        let mut fixture = TreeFixture::new();
        let host = fixture.element();
        let root = fixture.element();
        let slot = fixture.element();
        let slotted = fixture.element();
        let part = StyleAtomID(1);
        fixture.tree.set_shadow_root(host, root, &mut fixture.memory);
        fixture.tree.set_assigned_slot(slotted, Some(slot), &mut fixture.memory);
        fixture.tree.set_part_hosts(host, &[(part, host)], &mut fixture.memory);

        fixture.tree.retire_element(host, &mut fixture.memory);
        fixture.tree.retire_element(slot, &mut fixture.memory);
        assert_eq!(fixture.tree.host_of(root), None);
        assert_eq!(fixture.tree.assigned_slot_of(slotted), None);

        fixture.tree.release_retired_identities(&mut fixture.memory);
        let reused_slot = fixture.element();
        let reused_host = fixture.element();
        assert_eq!(reused_slot, slot);
        assert_eq!(reused_host, host);
        assert_eq!(fixture.tree.shadow_root_of(reused_host), None);
        assert_eq!(fixture.tree.assigned_nodes_of(reused_slot), &[]);
        assert_eq!(fixture.tree.part_hosts_of(reused_host), &[]);
    }

    #[test]
    fn column_capacity_is_charged_to_tier_one_and_released_with_the_columns() {
        let mut fixture = TreeFixture::new();
        for _ in 0..1000 {
            fixture.element();
        }
        let charged = fixture.memory.bytes_in_category(MemoryCategory::RelationColumns);
        assert_eq!(charged, fixture.tree.capacity_bytes());
        assert!(charged >= 1000 * 3 * 4);
    }

    #[test]
    fn a_document_with_no_shadow_tree_pays_nothing_for_the_flat_tree() {
        let mut fixture = TreeFixture::new();
        let parent = fixture.element();
        let child = fixture.element();
        fixture.attach_children(parent, &[child]);

        assert!(!fixture.tree.has_shadow_relations());
        assert_eq!(fixture.tree.flat_tree_children(parent).collect::<Vec<_>>(), vec![child]);
        assert_eq!(fixture.tree.assigned_nodes_of(parent), &[]);
    }

    #[test]
    fn a_shadow_host_takes_its_flat_tree_children_from_its_shadow_root() {
        let mut fixture = TreeFixture::new();
        let host = fixture.element();
        let light_child = fixture.element();
        let shadow_root = fixture.element();
        let shadow_child = fixture.element();
        fixture.attach_children(host, &[light_child]);
        fixture.attach_children(shadow_root, &[shadow_child]);
        fixture.tree.set_shadow_root(host, shadow_root, &mut fixture.memory);

        assert_eq!(
            fixture.tree.flat_tree_children(host).collect::<Vec<_>>(),
            vec![shadow_child],
            "the light child reaches the flat tree only through a slot"
        );
        assert_eq!(fixture.tree.shadow_root_of(host), Some(shadow_root));
        assert_eq!(fixture.tree.host_of(shadow_root), Some(host));
        assert_eq!(fixture.tree.flat_tree_parent(shadow_child), Some(host));
        assert_eq!(fixture.tree.flat_tree_parent(light_child), None);
    }

    #[test]
    fn replacing_a_shadow_root_unlinks_the_previous_root() {
        let mut fixture = TreeFixture::new();
        let host = fixture.element();
        let previous_root = fixture.element();
        let current_root = fixture.element();

        fixture.tree.set_shadow_root(host, previous_root, &mut fixture.memory);
        fixture.tree.set_shadow_root(host, current_root, &mut fixture.memory);

        assert_eq!(fixture.tree.shadow_root_of(host), Some(current_root));
        assert_eq!(fixture.tree.host_of(previous_root), None);
        assert_eq!(fixture.tree.host_of(current_root), Some(host));

        fixture.tree.retire_element(previous_root, &mut fixture.memory);
        assert_eq!(fixture.tree.shadow_root_of(host), Some(current_root));
        assert_eq!(fixture.tree.host_of(current_root), Some(host));
    }

    #[test]
    fn a_slot_takes_its_flat_tree_children_from_its_assignment() {
        let mut fixture = TreeFixture::new();
        let slot = fixture.element();
        let fallback = fixture.element();
        let first = fixture.element();
        let second = fixture.element();
        fixture.attach_children(slot, &[fallback]);

        // With nothing assigned, the slot's own children are its fallback content.
        assert_eq!(
            fixture.tree.flat_tree_children(slot).collect::<Vec<_>>(),
            vec![fallback]
        );

        fixture.tree.set_assigned_slot(first, Some(slot), &mut fixture.memory);
        fixture.tree.set_assigned_slot(second, Some(slot), &mut fixture.memory);
        assert_eq!(
            fixture.tree.flat_tree_children(slot).collect::<Vec<_>>(),
            vec![first, second],
            "assigned nodes replace fallback content"
        );
        assert_eq!(fixture.tree.assigned_slot_of(first), Some(slot));
        assert_eq!(fixture.tree.flat_tree_parent(first), Some(slot));
        assert_eq!(fixture.tree.flat_tree_parent(fallback), None);
    }

    #[test]
    fn style_reaction_order_covers_shadow_light_fallback_and_assigned_branches() {
        let mut fixture = TreeFixture::new();
        let document = fixture.element();
        let host = fixture.element();
        let light_child = fixture.element();
        let shadow_root = fixture.element();
        let slot = fixture.element();
        let fallback = fixture.element();
        let assigned = fixture.element();
        fixture.attach_children(document, &[host]);
        fixture.attach_children(host, &[light_child, assigned]);
        fixture.attach_children(shadow_root, &[slot]);
        fixture.attach_children(slot, &[fallback]);
        fixture.tree.set_shadow_root(host, shadow_root, &mut fixture.memory);
        fixture
            .tree
            .set_assigned_slot(assigned, Some(slot), &mut fixture.memory);

        let mut reactions = vec![light_child, assigned, fallback, slot, host];
        reactions.sort_unstable_by(|first, second| fixture.tree.compare_style_reaction_order(*first, *second));

        assert_eq!(reactions, vec![host, slot, fallback, assigned, light_child]);
    }

    #[test]
    fn reassigning_a_slot_moves_the_node_without_touching_the_dom_tree() {
        let mut fixture = TreeFixture::new();
        let first_slot = fixture.element();
        let second_slot = fixture.element();
        let node = fixture.element();
        let dom_parent = fixture.element();
        fixture.attach_children(dom_parent, &[node]);

        fixture
            .tree
            .set_assigned_slot(node, Some(first_slot), &mut fixture.memory);
        assert_eq!(fixture.tree.assigned_nodes_of(first_slot), &[node]);

        fixture
            .tree
            .set_assigned_slot(node, Some(second_slot), &mut fixture.memory);
        assert_eq!(
            fixture.tree.assigned_nodes_of(first_slot),
            &[],
            "the old slot no longer lists it"
        );
        assert_eq!(fixture.tree.assigned_nodes_of(second_slot), &[node]);
        assert_eq!(
            fixture.tree.parent(node),
            Some(dom_parent),
            "the DOM parent is unchanged, which is exactly why slotting is its own relation"
        );

        fixture.tree.set_assigned_slot(node, None, &mut fixture.memory);
        assert_eq!(fixture.tree.assigned_nodes_of(second_slot), &[]);
        assert_eq!(fixture.tree.assigned_slot_of(node), None);
    }

    #[test]
    fn part_exposure_is_its_own_sparse_relation() {
        let mut fixture = TreeFixture::new();
        let element = fixture.element();
        assert_eq!(fixture.tree.part_hosts_of(element), &[]);

        let pairs = [(StyleAtomID(7), element), (StyleAtomID(8), element)];
        fixture.tree.set_part_hosts(element, &pairs, &mut fixture.memory);
        assert_eq!(fixture.tree.part_hosts_of(element), &pairs);
        let mut atoms = HashSet::default();
        assert_eq!(fixture.tree.collect_atoms(&mut atoms), 2);
        assert_eq!(atoms, [StyleAtomID(7), StyleAtomID(8)].into_iter().collect());

        fixture.tree.set_part_hosts(element, &[], &mut fixture.memory);
        assert_eq!(fixture.tree.part_hosts_of(element), &[]);
        atoms.clear();
        assert_eq!(fixture.tree.collect_atoms(&mut atoms), 0);
        assert!(atoms.is_empty());
    }

    #[test]
    fn shadow_relations_are_charged_only_where_they_exist() {
        let mut fixture = TreeFixture::new();
        for _ in 0..100 {
            fixture.element();
        }
        let without_shadow = fixture.tree.capacity_bytes();

        let host = StyleNodeID::element(1);
        let root = StyleNodeID::element(2);
        let slot = StyleNodeID::element(3);
        let slotted = StyleNodeID::element(4);
        fixture.tree.set_shadow_root(host, root, &mut fixture.memory);
        assert!(fixture.tree.capacity_bytes() > without_shadow);
        assert!(fixture.tree.has_shadow_relations());
        assert_eq!(
            fixture.memory.bytes_in_category(MemoryCategory::RelationColumns),
            fixture.tree.capacity_bytes()
        );

        fixture.tree.set_assigned_slot(slotted, Some(slot), &mut fixture.memory);
        assert_eq!(
            fixture.memory.bytes_in_category(MemoryCategory::RelationColumns),
            fixture.tree.capacity_bytes()
        );

        fixture
            .tree
            .set_part_hosts(host, &[(StyleAtomID(1), host)], &mut fixture.memory);
        assert_eq!(
            fixture.memory.bytes_in_category(MemoryCategory::RelationColumns),
            fixture.tree.capacity_bytes()
        );
    }

    #[test]
    fn per_node_relation_state_fits_the_mandatory_budget() {
        // Rust stores four relation columns, depth, and the style-record handle per node. The DOM
        // owns the style-node identity mapping counted by the documented surface budget.
        const STYLE_RECORD_ID_BYTES: usize = 4;
        const STYLE_NODE_ID_BYTES: usize = 4;
        let required = 4 * size_of::<Option<StyleNodeID>>() + size_of::<u32>() + STYLE_RECORD_ID_BYTES;
        assert_eq!(size_of::<Option<StyleNodeID>>(), 4);
        assert!(required <= 24, "mandatory engine node bytes exceeded: {required}");

        let conditional = size_of::<TreeScopeID>();
        assert!(
            STYLE_NODE_ID_BYTES + required + conditional <= 32,
            "mandatory node surface bytes exceeded: {}",
            STYLE_NODE_ID_BYTES + required + conditional
        );
    }
}
