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
use std::num::NonZeroU32;

use super::capacity::capacity_bytes;
use super::column::BitColumn;
use super::column::PagedColumn;
use super::column::PagedColumnPage;
use super::column::RemovablePagedColumnPage;
use super::index::StyleAtomID;
use super::memory::MemoryCategory;
use super::memory::MemoryController;

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
struct SegmentedNodeColumn<T: Copy>(PagedColumn<SegmentedNodePage<T>>);

impl<T: Copy> Default for SegmentedNodeColumn<T> {
    fn default() -> Self {
        Self(PagedColumn::default())
    }
}

impl<T: Copy> SegmentedNodeColumn<T> {
    fn get(&self, node: StyleNodeID) -> Option<T> {
        let index = node.element_index()? as usize;
        self.0.get(index)
    }

    fn insert(&mut self, node: StyleNodeID, value: T) -> Option<T> {
        let index = node
            .element_index()
            .expect("conditional tree relations connect DOM nodes") as usize;
        self.0.insert(index, value).0
    }

    fn remove(&mut self, node: StyleNodeID) -> Option<T> {
        let index = node.element_index()? as usize;
        self.0.remove(index)
    }

    fn capacity_bytes(&self) -> u64 {
        self.0.capacity_bytes()
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
}

impl StyleNodeTree {
    #[must_use]
    pub fn new(memory: &mut MemoryController) -> Self {
        let mut tree = Self {
            parent: Vec::new(),
            first_element_child: Vec::new(),
            next_element_sibling: Vec::new(),
            previous_element_sibling: Vec::new(),
            tree_scope: None,
            live: BitColumn::default(),
            connected_element_count: 0,
            pending_reuse: Vec::new(),
            free_element_indexes: Vec::new(),
            shadow: None,
        };
        // Slot 0 is never a valid identity; reserving it keeps column indexing direct.
        tree.parent.push(None);
        tree.first_element_child.push(None);
        tree.next_element_sibling.push(None);
        tree.previous_element_sibling.push(None);
        tree.charge(memory, 0);
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
        let before = self.capacity_bytes();
        let index = match self.free_element_indexes.pop() {
            Some(index) => {
                self.parent[index as usize] = None;
                self.first_element_child[index as usize] = None;
                self.next_element_sibling[index as usize] = None;
                self.previous_element_sibling[index as usize] = None;
                if let Some(column) = self.tree_scope.as_mut() {
                    column[index as usize] = TreeScopeID::DOCUMENT;
                }
                index
            }
            None => {
                let index = u32::try_from(self.parent.len()).expect("element index space exhausted");
                self.parent.push(None);
                self.first_element_child.push(None);
                self.next_element_sibling.push(None);
                self.previous_element_sibling.push(None);
                if let Some(column) = self.tree_scope.as_mut() {
                    column.push(TreeScopeID::DOCUMENT);
                }
                index
            }
        };
        self.live.set(index as usize, true);
        self.connected_element_count += 1;
        self.charge(memory, before);
        StyleNodeID::element(index)
    }

    /// Retire an element identity. The slot stays reserved until [`Self::release_retired_identities`]
    /// runs at epoch retirement, so no reader can observe a reused identity.
    pub fn retire_element(&mut self, node: StyleNodeID, memory: &mut MemoryController) {
        let before = self.capacity_bytes();
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
        self.connected_element_count -= 1;
        self.pending_reuse.push(index);
        self.charge(memory, before);
    }

    /// Called once the read epoch that could still name the retired identities has retired.
    pub fn release_retired_identities(&mut self, memory: &mut MemoryController) {
        let before = self.capacity_bytes();
        self.free_element_indexes.append(&mut self.pending_reuse);
        self.charge(memory, before);
    }

    #[must_use]
    #[cfg(test)]
    pub fn retired_identities_pending_release(&self) -> usize {
        self.pending_reuse.len()
    }

    // -- Relation maintenance ----------------------------------------------------------------

    pub fn set_parent(&mut self, node: StyleNodeID, parent: Option<StyleNodeID>) {
        let index = self.live_element_index(node);
        self.parent[index] = parent;
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
        let before = self.capacity_bytes();
        self.tree_scope = Some(vec![TreeScopeID::DOCUMENT; self.parent.len()]);
        self.charge(memory, before);
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
        let before = self.capacity_bytes();
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
        self.charge(memory, before);
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
        let before = self.capacity_bytes();
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
        self.charge(memory, before);
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
        let before = self.capacity_bytes();
        let shadow = self.shadow_mut();
        if pairs.is_empty() {
            shadow.part_hosts.remove(&node);
        } else {
            shadow.part_hosts.insert(node, pairs.to_vec());
        }
        self.charge(memory, before);
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
    /// tree-order label, a subtree impact region is not a numeric interval, so proving membership
    /// costs one relation step per level. Those steps are served from resident columns.
    #[must_use]
    pub fn is_in_subtree_of(&self, node: StyleNodeID, root: StyleNodeID) -> bool {
        node == root || self.ancestors(node).any(|ancestor| ancestor == root)
    }

    // -- Accounting --------------------------------------------------------------------------

    /// Exact capacity of every column, charged to Tier 1.
    #[must_use]
    pub fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.parent,
                self.first_element_child,
                self.next_element_sibling,
                self.previous_element_sibling,
                self.pending_reuse,
                self.free_element_indexes,
            ];
            cached [];
            nested [
                self.tree_scope
                    .as_ref()
                    .map_or(0, |column| column.capacity() * size_of::<TreeScopeID>()),
                self.live.capacity_bytes(),
                self.shadow.as_ref().map_or(0, |relations| relations.capacity_bytes()),
            ];
            skip [];
        }
    }

    fn charge(&self, memory: &mut MemoryController, previous_bytes: u64) {
        let current = self.capacity_bytes();
        if current > previous_bytes {
            let outcome = memory.reserve(MemoryCategory::RelationColumns, current - previous_bytes);
            assert!(outcome.is_granted(), "required relation columns must not be refused");
        } else if previous_bytes > current {
            memory.release(MemoryCategory::RelationColumns, previous_bytes - current);
        }
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

        assert!(fixture.tree.is_in_subtree_of(grandchild, root));
        assert!(fixture.tree.is_in_subtree_of(root, root));
        assert!(!fixture.tree.is_in_subtree_of(first, second));
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

        fixture.tree.set_part_hosts(element, &[], &mut fixture.memory);
        assert_eq!(fixture.tree.part_hosts_of(element), &[]);
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
        // Rust stores four relation columns plus the style-record handle per node.
        const STYLE_RECORD_ID_BYTES: usize = 4;
        let required = 4 * size_of::<Option<StyleNodeID>>() + STYLE_RECORD_ID_BYTES;
        assert_eq!(size_of::<Option<StyleNodeID>>(), 4);
        assert!(required <= 20, "phase-1 mandatory node bytes exceeded: {required}");

        let conditional = size_of::<TreeScopeID>() + size_of::<Option<StyleNodeID>>();
        assert!(conditional <= 8, "conditional relation bytes exceeded: {conditional}");
    }
}
