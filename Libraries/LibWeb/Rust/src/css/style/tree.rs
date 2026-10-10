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
use smallvec::SmallVec;
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

/// What an element's published style record says about whether, and as what, it is a query
/// container. Kept apart from the selector facts, since it moves only when a record is published.
#[derive(Clone, Default)]
pub(super) struct ContainerQueryInputRow {
    pub(super) style_record: u64,
    pub(super) names: Vec<Vec<u16>>,
    pub(super) is_size_container: bool,
    pub(super) is_inline_size_container: bool,
    pub(super) is_scroll_state_container: bool,
    pub(super) writing_mode: u8,
    pub(super) direction: u8,
}

#[derive(Clone, Default)]
pub(super) struct ContainerQueryInputColumns {
    rows: Vec<Option<ContainerQueryInputRow>>,
}

impl ContainerQueryInputColumns {
    pub(super) fn set(&mut self, node: StyleNodeID, row: ContainerQueryInputRow) {
        let Some(index) = node.element_index().map(|index| index as usize) else {
            return;
        };
        if self.rows.len() <= index {
            self.rows.resize_with(index + 1, || None);
        }
        self.rows[index] = Some(row);
    }

    pub(super) fn clear(&mut self, node: StyleNodeID) {
        if let Some(row) = node.element_index().and_then(|index| self.rows.get_mut(index as usize)) {
            *row = None;
        }
    }

    pub(super) fn get(&self, node: StyleNodeID) -> Option<&ContainerQueryInputRow> {
        self.rows.get(node.element_index()? as usize).and_then(Option::as_ref)
    }
}

/// Document-local identity of an element or a text node.
///
/// The top bit says which kind of node it names, and the rest is a dense index into that kind's own
/// columns. Text nodes outnumber elements on most pages and have none of their facts or styles, so
/// sharing one index space would make every element column span them too.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct StyleNodeID(NonZeroU32);

const TEXT_STYLE_NODE_BIT: u32 = 1 << 31;

impl StyleNodeID {
    /// `index` is a dense element index starting at 1.
    #[must_use]
    pub fn element(index: u32) -> Self {
        assert!(index != 0, "element index 0 is reserved");
        assert!(index < TEXT_STYLE_NODE_BIT, "element index space exhausted");
        Self(NonZeroU32::new(index).unwrap())
    }

    /// `index` is a dense text index starting at 1. The all-ones identity stays out of reach, as the
    /// boundary gives it a meaning of its own.
    #[must_use]
    pub fn text(index: u32) -> Self {
        assert!(index != 0, "text index 0 is reserved");
        assert!(index < TEXT_STYLE_NODE_BIT - 1, "text index space exhausted");
        Self(NonZeroU32::new(index | TEXT_STYLE_NODE_BIT).unwrap())
    }

    /// The dense element index, or `None` for a text node.
    #[must_use]
    pub fn element_index(self) -> Option<u32> {
        (self.0.get() & TEXT_STYLE_NODE_BIT == 0).then_some(self.0.get())
    }

    /// The slot the node has in columns keyed by element index. A text node's identity has the
    /// top bit set, which puts its slot past the end of every element column: indexing one with it
    /// fails the bounds check, and looking it up finds nothing, without testing the kind first.
    #[must_use]
    pub fn element_slot(self) -> usize {
        self.0.get() as usize
    }

    /// The dense text index, or `None` for an element.
    #[must_use]
    pub fn text_index(self) -> Option<u32> {
        (self.0.get() & TEXT_STYLE_NODE_BIT != 0).then_some(self.0.get() & !TEXT_STYLE_NODE_BIT)
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

#[derive(Clone)]

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
#[derive(Clone)]
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
#[derive(Clone, Default)]
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
#[derive(Clone, Default)]
struct ShadowRelations {
    /// A slotted element's slot.
    assigned_slot: SegmentedNodeColumn<StyleNodeID>,
    /// A slotted text node's slot. Text identities have no relation columns of their own, and a
    /// slotted text node is rare enough that a map costs less than a second column would.
    text_assigned_slot: HashMap<StyleNodeID, StyleNodeID>,
    /// A slot's assigned nodes, text nodes included, in the order the DOM assigned them.
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
                if assigned.text_index().is_some() {
                    if self.text_assigned_slot.get(&assigned) == Some(&node) {
                        self.text_assigned_slot.remove(&assigned);
                    }
                } else if self.assigned_slot.get(assigned) == Some(node) {
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

    fn retire_text(&mut self, node: StyleNodeID) {
        if let Some(slot) = self.text_assigned_slot.remove(&node)
            && let Some(nodes) = self.assigned_nodes.get_mut(&slot)
        {
            nodes.retain(|&assigned| assigned != node);
        }
    }

    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.text_assigned_slot, self.assigned_nodes, self.part_hosts];
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

/// The inverse of the element id column: which elements answer to an id name.
///
/// The name is the id as it is written, not the atom a selector is compiled against: a quirks-mode
/// document folds an id selector's name to lowercase, while `getElementById` is case-sensitive in
/// every mode. The two therefore cannot share one atom, and this index keeps the unfolded one.
///
/// A name is not keyed by tree scope. An element carries its scope in a column that a move or an
/// adoption already maintains, so keying by it here would mean maintaining it twice; the scope is
/// settled at the lookup instead, where the candidate list is almost always one element long.
#[derive(Clone, Default)]
struct ElementIdIndex {
    /// The name each element answers to, which is the key a change or a retirement removes under.
    name_of_node: HashMap<StyleNodeID, StyleAtomID>,
    /// The elements answering to a name, in the order they took it rather than in tree order.
    /// Nearly every name is unique, so the list that holds one stays inline.
    nodes_by_name: HashMap<StyleAtomID, SmallVec<[StyleNodeID; 1]>>,
    /// What the spilled candidate lists hold, carried rather than summed: every element that takes
    /// an id writes here, and walking one list per name would make a page of ids quadratic.
    candidate_bytes: usize,
}

impl ElementIdIndex {
    fn set(&mut self, node: StyleNodeID, name: StyleAtomID) {
        if let Some(previous) = self.name_of_node.remove(&node)
            && let Some(nodes) = self.nodes_by_name.get_mut(&previous)
        {
            let before = Self::candidate_bytes_of(nodes);
            nodes.retain(|&mut candidate| candidate != node);
            let empty = nodes.is_empty();
            let after = if empty { 0 } else { Self::candidate_bytes_of(nodes) };
            self.candidate_bytes -= before - after;
            if empty {
                self.nodes_by_name.remove(&previous);
            }
        }
        if name.is_none() {
            return;
        }
        self.name_of_node.insert(node, name);
        let nodes = self.nodes_by_name.entry(name).or_default();
        let before = Self::candidate_bytes_of(nodes);
        nodes.push(node);
        self.candidate_bytes += Self::candidate_bytes_of(nodes) - before;
    }

    /// What a candidate list holds beyond its inline room.
    fn candidate_bytes_of(nodes: &SmallVec<[StyleNodeID; 1]>) -> usize {
        if nodes.spilled() {
            nodes.capacity() * size_of::<StyleNodeID>()
        } else {
            0
        }
    }

    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [self.name_of_node, self.nodes_by_name];
            cached [];
            nested [self.candidate_bytes];
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

/// The spans a table cell or table column takes from its attributes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TableSpans {
    pub column_span: u16,
    pub row_span: u16,
    pub raw_column_span: u32,
}

impl Default for TableSpans {
    fn default() -> Self {
        Self {
            column_span: 1,
            row_span: 1,
            raw_column_span: 1,
        }
    }
}

/// The Rust-owned projection of the tree relations selectors navigate.
///
/// Element columns are indexed by element index, with slot 0 unused so that a `StyleNodeID` indexes
/// its own column entry directly.
#[derive(Clone)]
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
    /// Identities that stand in the tree without being styled: the document, whose children the
    /// DOM child sequence hangs from. A selector never names one and nothing publishes features
    /// for one, so a style pass that reaches one must pass it by rather than ask it to match.
    relation_only: BitColumn,
    connected_element_count: u32,
    /// Identities retired in the current epoch. They cannot be reused until the epoch that could
    /// still observe them has retired.
    pending_reuse: Vec<u32>,
    /// The identities a test mints from, standing in for the host's.
    #[cfg(test)]
    test_identities: super::identities::StyleNodeIdAllocator,

    /// Allocated only once a shadow tree exists.
    shadow: Option<Box<ShadowRelations>>,

    /// Allocated only once an element carries an id.
    ids: Option<Box<ElementIdIndex>>,

    // The DOM child sequence, text nodes included. Elements keep these beside their element-only
    // links, which every selector walk reads; text nodes have nothing else.
    //
    // Unlike the element-only links, these are spliced when the DOM changes rather than staged,
    // because no transaction plans from them.
    first_child: Vec<Option<StyleNodeID>>,
    next_sibling: Vec<Option<StyleNodeID>>,
    previous_sibling: Vec<Option<StyleNodeID>>,
    text: TextRows,

    /// What a row built for the node is painted and hit-tested with: whether the node is inert,
    /// editable or an editing host, inside a blocking wheel event handler, or a navigable container
    /// holding a navigable. Nearly every node holds none of them, which is the absence of an entry.
    /// Element, text and document identities publish here, as each gets a row.
    dom_paint_facts: HashMap<StyleNodeID, u8>,
    /// The unique node id the document names the element by, published where the identity arrives
    /// and constant for as long as the element holds it. A box built for the element answers by
    /// it, and so does a box built for one of the element's pseudo-elements, which is why the
    /// render side needs it for an element that has no box of its own. The document's identity
    /// carries the document's.
    unique_node_ids: Vec<i64>,
    /// The spans a table cell's or table column's attributes give it: the effective column and row
    /// span, and the column span attribute's unclamped value, which only the table formatting
    /// context's column handling reads. Every other element spans one of each, which is what the
    /// absence of an entry means.
    table_spans: HashMap<StyleNodeID, TableSpans>,
    /// The document's top layer, in the order its members were added. The order is the order their
    /// boxes are built in and belongs to the document, so no per-element fact can carry it.
    top_layer: Vec<StyleNodeID>,

    capacity_bytes: u64,
    /// Counts the changes to the DOM child sequences, to the parents and shadow roots that place a
    /// node among its siblings, and to which nodes are live. See [`Self::dom_order_version`].
    dom_order_version: u64,

    #[cfg(test)]
    depth_recompute_visits: usize,
}

impl StyleNodeTree {
    pub(super) fn collect_atoms(&self, atoms: &mut HashSet<StyleAtomID>) -> u64 {
        let mut visited = 0_u64;
        if let Some(shadow) = &self.shadow {
            for pairs in shadow.part_hosts.values() {
                visited += u64::try_from(pairs.len()).expect("part host count exceeds u64");
                atoms.extend(pairs.iter().map(|&(atom, _)| atom));
            }
        }
        // An id name is the id as written, which in a quirks-mode document no id fact holds: those
        // keep the folded form. The index roots its own names rather than rely on other state to.
        if let Some(ids) = &self.ids {
            visited += u64::try_from(ids.nodes_by_name.len()).expect("id name count exceeds u64");
            atoms.extend(ids.nodes_by_name.keys().copied());
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
            relation_only: BitColumn::default(),
            connected_element_count: 0,
            pending_reuse: Vec::new(),
            #[cfg(test)]
            test_identities: Default::default(),
            shadow: None,
            ids: None,
            first_child: Vec::new(),
            next_sibling: Vec::new(),
            previous_sibling: Vec::new(),
            text: TextRows::default(),
            dom_paint_facts: HashMap::default(),
            unique_node_ids: Vec::new(),
            table_spans: HashMap::default(),
            top_layer: Vec::new(),
            capacity_bytes: 0,
            dom_order_version: 0,
            #[cfg(test)]
            depth_recompute_visits: 0,
        };
        // Slot 0 is never a valid identity; reserving it keeps column indexing direct.
        tree.parent.push(None);
        tree.first_element_child.push(None);
        tree.next_element_sibling.push(None);
        tree.previous_element_sibling.push(None);
        tree.depth.push(0);
        tree.first_child.push(None);
        tree.next_sibling.push(None);
        tree.previous_sibling.push(None);
        tree.text.parent.push(None);
        tree.text.next_sibling.push(None);
        tree.text.previous_sibling.push(None);
        tree.text.data.push(ak::Utf16String::default());
        tree.capacity_bytes = tree.recompute_capacity_bytes();
        memory.reserve_required(MemoryCategory::RelationColumns, tree.capacity_bytes);
        tree
    }

    /// Connected styleable elements, which is the count the document memory budget is written in.
    #[must_use]
    pub fn connected_element_count(&self) -> u32 {
        self.connected_element_count
    }

    /// Mark an identity as standing in the tree without being styled. See `relation_only`.
    pub fn mark_relation_only(&mut self, node: StyleNodeID, memory: &mut MemoryController) {
        let Some(index) = node.element_index() else {
            return;
        };
        let before = self.identity_capacity_bytes();
        let (changed, _) = self.relation_only.set(index as usize, true);
        let current = self.identity_capacity_bytes();
        self.record_capacity_change(memory, before, current);
        // The count is the number of elements a style pass has to answer for, and this is not one.
        if changed {
            self.connected_element_count -= 1;
        }
    }

    /// Whether the identity stands in the tree without being styled. See `relation_only`.
    #[must_use]
    pub fn is_relation_only(&self, node: StyleNodeID) -> bool {
        node.element_index()
            .is_some_and(|index| self.relation_only.contains(index as usize))
    }

    #[must_use]
    pub fn is_live(&self, node: StyleNodeID) -> bool {
        match node.text_index() {
            Some(index) => self.text.live.contains(index as usize),
            None => self.live.contains(node.element_slot()),
        }
    }

    /// Every live element-kind identity, including the synthetic roots of shadow trees.
    pub fn live_nodes(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        (1..self.parent.len()).filter_map(|index| {
            let index = u32::try_from(index).expect("style node index space exhausted");
            self.live.contains(index as usize).then(|| StyleNodeID::element(index))
        })
    }

    // -- Identity lifecycle ------------------------------------------------------------------

    /// Make the element identity `node`, which the host minted, live. The host mints past every
    /// identity it minted before, or one this tree released, which no reader can still name.
    pub fn mint_element(&mut self, node: StyleNodeID, memory: &mut MemoryController) {
        self.dom_order_version += 1;
        let index = node.element_index().expect("mint_element requires an element identity");
        let capacity_before_growth = if (index as usize) < self.parent.len() {
            assert!(
                !self.live.contains(index as usize),
                "minting an element identity that is live"
            );
            self.parent[index as usize] = None;
            self.first_element_child[index as usize] = None;
            self.next_element_sibling[index as usize] = None;
            self.previous_element_sibling[index as usize] = None;
            self.depth[index as usize] = 0;
            self.first_child[index as usize] = None;
            self.next_sibling[index as usize] = None;
            self.previous_sibling[index as usize] = None;
            if let Some(column) = self.tree_scope.as_mut() {
                column[index as usize] = TreeScopeID::DOCUMENT;
            }
            None
        } else {
            assert_eq!(
                index as usize,
                self.parent.len(),
                "the host mints new element identities in order"
            );
            let capacity_before_growth = self.identity_capacity_bytes();
            self.parent.push(None);
            self.first_element_child.push(None);
            self.next_element_sibling.push(None);
            self.previous_element_sibling.push(None);
            self.depth.push(0);
            self.first_child.push(None);
            self.next_sibling.push(None);
            self.previous_sibling.push(None);
            if let Some(column) = self.tree_scope.as_mut() {
                column.push(TreeScopeID::DOCUMENT);
            }
            Some(capacity_before_growth)
        };
        self.live.set(index as usize, true);
        if let Some(capacity_before_growth) = capacity_before_growth {
            let current = self.identity_capacity_bytes();
            self.record_capacity_change(memory, capacity_before_growth, current);
        }
        self.connected_element_count += 1;
    }

    /// Mint an identity from the tree's own space, which stands in for the host's in a test.
    #[cfg(test)]
    pub fn mint_test_identity(&mut self, text: bool) -> StyleNodeID {
        if text {
            self.test_identities.mint_text()
        } else {
            self.test_identities.mint_element()
        }
    }

    /// Mint an element identity from the tree's own space, for a test.
    #[cfg(test)]
    pub fn allocate_element(&mut self, memory: &mut MemoryController) -> StyleNodeID {
        let node = self.mint_test_identity(false);
        self.mint_element(node, memory);
        node
    }

    /// Retire an element identity. The slot stays reserved until [`Self::release_retired_identities`]
    /// runs at epoch retirement, so no reader can observe a reused identity.
    #[cfg(test)]
    pub fn retire_element(&mut self, node: StyleNodeID, memory: &mut MemoryController) {
        self.retire_elements(&[node], memory);
    }

    /// Retire a batch of element identities, accounting once for the capacity they release.
    pub fn retire_elements(&mut self, nodes: &[StyleNodeID], memory: &mut MemoryController) {
        self.dom_order_version += 1;
        let before = self.retirement_capacity_bytes();
        for &node in nodes {
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
            if let Some(ids) = &mut self.ids {
                ids.set(node, StyleAtomID::NONE);
            }
            self.live.set(index as usize, false);
            self.dom_paint_facts.remove(&node);
            if let Some(unique_node_id) = self.unique_node_ids.get_mut(index as usize) {
                *unique_node_id = 0;
            }
            self.table_spans.remove(&node);
            self.top_layer.retain(|&member| member != node);
            if !self.relation_only.set(index as usize, false).0 {
                self.connected_element_count -= 1;
            }
            self.parent[index as usize] = None;
            self.first_element_child[index as usize] = None;
            self.next_element_sibling[index as usize] = None;
            self.previous_element_sibling[index as usize] = None;
            self.depth[index as usize] = 0;
            self.first_child[index as usize] = None;
            self.next_sibling[index as usize] = None;
            self.previous_sibling[index as usize] = None;
            self.pending_reuse.push(index);
        }
        let current = self.retirement_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// Called once the read epoch that could still name the retired identities has retired. Answers
    /// the identities it releases, elements then text nodes, each in the order they retired, for the
    /// host to mint again.
    pub fn release_retired_identities(&mut self, memory: &mut MemoryController) -> Vec<u32> {
        let before = self.reuse_capacity_bytes() + self.text.capacity_bytes();
        let released: Vec<u32> = self
            .pending_reuse
            .drain(..)
            .map(|index| StyleNodeID::element(index).raw())
            .chain(
                self.text
                    .pending_reuse
                    .drain(..)
                    .map(|index| StyleNodeID::text(index).raw()),
            )
            .collect();
        let current = self.reuse_capacity_bytes() + self.text.capacity_bytes();
        self.record_capacity_change(memory, before, current);
        #[cfg(test)]
        self.test_identities.release(&released);
        released
    }

    #[must_use]
    #[cfg(test)]
    pub fn retired_identities_pending_release(&self) -> usize {
        self.pending_reuse.len()
    }

    /// Make the text identity `node`, which the host minted, live. Like an element's, it is new or
    /// one this tree released.
    pub fn mint_text(&mut self, node: StyleNodeID, memory: &mut MemoryController) {
        self.dom_order_version += 1;
        let index = node.text_index().expect("mint_text requires a text identity");
        let before = self.text.capacity_bytes();
        if (index as usize) < self.text.parent.len() {
            assert!(
                !self.text.live.contains(index as usize),
                "minting a text identity that is live"
            );
        } else {
            assert_eq!(
                index as usize,
                self.text.parent.len(),
                "the host mints new text identities in order"
            );
            self.text.parent.push(None);
            self.text.next_sibling.push(None);
            self.text.previous_sibling.push(None);
            self.text.data.push(ak::Utf16String::default());
        }
        self.text.live.set(index as usize, true);
        let current = self.text.capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// Mint a text identity from the tree's own space, for a test.
    #[cfg(test)]
    pub fn allocate_text(&mut self, memory: &mut MemoryController) -> StyleNodeID {
        let node = self.mint_test_identity(true);
        self.mint_text(node, memory);
        node
    }

    /// Retire text identities as their nodes disconnect. Nothing selects or styles a text node, so
    /// it has no relations to stage, and its slot waits for [`Self::release_retired_identities`]
    /// like an element's.
    pub fn retire_texts(&mut self, nodes: impl IntoIterator<Item = StyleNodeID>, memory: &mut MemoryController) {
        self.dom_order_version += 1;
        let before = self.text.capacity_bytes() + self.shadow_capacity_bytes();
        // Every column is named, so a new one cannot leave a retired identity's value behind for
        // the next text node issued the index.
        let TextRows {
            parent,
            next_sibling,
            previous_sibling,
            live,
            is_ascii_whitespace,
            is_in_user_agent_shadow_tree,
            is_password_input,
            data,
            pending_reuse,
        } = &mut self.text;
        for node in nodes {
            let index = node.text_index().expect("retire_texts requires a text identity");
            let (was_live, _) = live.set(index as usize, false);
            assert!(was_live, "retiring a text identity that is not live");
            is_ascii_whitespace.set(index as usize, false);
            is_in_user_agent_shadow_tree.set(index as usize, false);
            is_password_input.set(index as usize, false);
            self.dom_paint_facts.remove(&node);
            // Lets go of the reference the mirror held to the document's string.
            data[index as usize] = ak::Utf16String::default();
            parent[index as usize] = None;
            next_sibling[index as usize] = None;
            previous_sibling[index as usize] = None;
            pending_reuse.push(index);
            if let Some(shadow) = self.shadow.as_mut() {
                shadow.retire_text(node);
            }
        }
        let current = self.text.capacity_bytes() + self.shadow_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// Whether the text node's data is nothing but ASCII whitespace. Only a text node has data, so
    /// every other identity answers no.
    #[must_use]
    pub fn text_is_ascii_whitespace(&self, node: StyleNodeID) -> bool {
        node.text_index()
            .is_some_and(|index| self.text.is_ascii_whitespace.contains(index as usize))
    }

    /// Record what the text node's data now spells, as its whitespace-only state.
    pub fn set_text_is_ascii_whitespace(&mut self, node: StyleNodeID, value: bool, memory: &mut MemoryController) {
        let Some(index) = node.text_index() else {
            return;
        };
        let before = self.text.capacity_bytes();
        self.text.is_ascii_whitespace.set(index as usize, value);
        let current = self.text.capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// Whether the text node sits in a user agent shadow tree. Only a text node is asked; an
    /// element records the fact among its construction facts.
    #[must_use]
    pub fn text_is_in_user_agent_shadow_tree(&self, node: StyleNodeID) -> bool {
        node.text_index()
            .is_some_and(|index| self.text.is_in_user_agent_shadow_tree.contains(index as usize))
    }

    /// Record which kind of tree the text node arrived in.
    pub fn set_text_is_in_user_agent_shadow_tree(
        &mut self,
        node: StyleNodeID,
        value: bool,
        memory: &mut MemoryController,
    ) {
        let Some(index) = node.text_index() else {
            return;
        };
        let before = self.text.capacity_bytes();
        self.text.is_in_user_agent_shadow_tree.set(index as usize, value);
        let current = self.text.capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// Whether the text node holds the value of a password input. Only a text node is asked; every
    /// other identity answers no.
    #[must_use]
    pub fn text_is_password_input(&self, node: StyleNodeID) -> bool {
        node.text_index()
            .is_some_and(|index| self.text.is_password_input.contains(index as usize))
    }

    /// Record whether the text node holds the value of a password input.
    pub fn set_text_is_password_input(&mut self, node: StyleNodeID, value: bool, memory: &mut MemoryController) {
        let Some(index) = node.text_index() else {
            return;
        };
        let before = self.text.capacity_bytes();
        self.text.is_password_input.set(index as usize, value);
        let current = self.text.capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// Record the unique node id the document names the element by. Only an element identity, the
    /// document's included, holds one.
    pub fn set_unique_node_id(&mut self, node: StyleNodeID, unique_node_id: i64, memory: &mut MemoryController) {
        let Some(index) = node.element_index() else {
            return;
        };
        let index = index as usize;
        let before = self.identity_capacity_bytes();
        if self.unique_node_ids.len() <= index {
            self.unique_node_ids.resize(index + 1, 0);
        }
        self.unique_node_ids[index] = unique_node_id;
        let current = self.identity_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// The unique node id the document names the element by, or zero for anything else.
    #[must_use]
    pub fn unique_node_id(&self, node: StyleNodeID) -> i64 {
        node.element_index()
            .and_then(|index| self.unique_node_ids.get(index as usize).copied())
            .unwrap_or(0)
    }

    /// The spans a row built for the element takes from its attributes.
    #[must_use]
    pub fn table_spans(&self, node: StyleNodeID) -> TableSpans {
        self.table_spans.get(&node).copied().unwrap_or_default()
    }

    /// Record the spans a row built for the element takes from its attributes. Spanning one of
    /// each is the absence of an entry.
    pub fn set_table_spans(&mut self, node: StyleNodeID, spans: TableSpans, memory: &mut MemoryController) {
        let before = self.identity_capacity_bytes();
        if spans == TableSpans::default() {
            self.table_spans.remove(&node);
        } else {
            self.table_spans.insert(node, spans);
        }
        let current = self.identity_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// What a row built for the node is painted and hit-tested with. An identity with nothing
    /// published holds none of the facts.
    #[must_use]
    pub fn dom_paint_facts(&self, node: StyleNodeID) -> u8 {
        self.dom_paint_facts.get(&node).copied().unwrap_or(0)
    }

    /// Record what a row built for the node is painted and hit-tested with. Holding none of the
    /// facts is the absence of an entry.
    pub fn set_dom_paint_facts(&mut self, node: StyleNodeID, facts: u8, memory: &mut MemoryController) {
        let before = self.identity_capacity_bytes();
        if facts == 0 {
            self.dom_paint_facts.remove(&node);
        } else {
            self.dom_paint_facts.insert(node, facts);
        }
        let current = self.identity_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// The document's top layer, in the order its members were added.
    #[must_use]
    pub fn top_layer(&self) -> &[StyleNodeID] {
        &self.top_layer
    }

    /// Replace the document's top layer.
    pub fn set_top_layer(&mut self, members: &[StyleNodeID], memory: &mut MemoryController) {
        let before = self.identity_capacity_bytes();
        members.clone_into(&mut self.top_layer);
        let current = self.identity_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// The characters the text node holds, or none for any other identity.
    #[must_use]
    pub fn text_data(&self, node: StyleNodeID) -> Option<&ak::Utf16String> {
        self.text.data.get(node.text_index()? as usize)
    }

    /// Record the characters the text node now holds. The string is the document's, shared rather
    /// than copied.
    pub fn set_text_data(&mut self, node: StyleNodeID, data: ak::Utf16String) {
        if let Some(index) = node.text_index() {
            self.text.data[index as usize] = data;
        }
    }

    // -- DOM child sequence ------------------------------------------------------------------

    /// Splice `node` into `parent`'s child sequence right after `previous`, or first when there is
    /// none. A node with no parent is left unlinked: the document's own children are not a
    /// sequence anything reads.
    pub fn link_in_dom_order(&mut self, node: StyleNodeID, parent: Option<StyleNodeID>, previous: Option<StyleNodeID>) {
        self.dom_order_version += 1;
        if !self.is_live(node) {
            return;
        }
        let previous = previous.filter(|&previous| self.is_live(previous));
        let Some(parent_index) = parent
            .filter(|&parent| self.is_live(parent))
            .and_then(StyleNodeID::element_index)
        else {
            *self.next_sibling_mut(node) = None;
            *self.previous_sibling_mut(node) = None;
            if let Some(index) = node.text_index() {
                self.text.parent[index as usize] = None;
            }
            return;
        };
        let parent_index = parent_index as usize;
        let next = match previous {
            Some(previous) => self.next_sibling_in_dom_order(previous),
            None => self.first_child[parent_index],
        };
        *self.previous_sibling_mut(node) = previous;
        *self.next_sibling_mut(node) = next;
        match previous {
            Some(previous) => *self.next_sibling_mut(previous) = Some(node),
            None => self.first_child[parent_index] = Some(node),
        }
        if let Some(next) = next {
            *self.previous_sibling_mut(next) = Some(node);
        }
        if let Some(index) = node.text_index() {
            self.text.parent[index as usize] = parent;
        }
    }

    /// Take `node` out of the child sequence of `parent`, the parent it was linked under.
    pub fn unlink_from_dom_order(&mut self, node: StyleNodeID, parent: Option<StyleNodeID>) {
        self.dom_order_version += 1;
        if !self.is_live(node) {
            return;
        }
        let previous = self.previous_sibling_mut(node).take();
        let next = self.next_sibling_mut(node).take();
        match previous {
            Some(previous) => {
                if self.is_live(previous) {
                    *self.next_sibling_mut(previous) = next;
                }
            }
            None => {
                if let Some(parent_index) = parent
                    .filter(|&parent| self.is_live(parent))
                    .and_then(StyleNodeID::element_index)
                    && self.first_child[parent_index as usize] == Some(node)
                {
                    self.first_child[parent_index as usize] = next;
                }
            }
        }
        if let Some(next) = next
            && self.is_live(next)
        {
            *self.previous_sibling_mut(next) = previous;
        }
        if let Some(index) = node.text_index() {
            self.text.parent[index as usize] = None;
        }
    }

    /// A version of what places a node among its siblings: the DOM child sequences, the parents and
    /// shadow roots, and which nodes are live. Whatever reads none of it since it read the version
    /// reads the same places again.
    #[must_use]
    pub fn dom_order_version(&self) -> u64 {
        self.dom_order_version
    }

    /// The children of `node` in DOM order, text nodes included.
    #[must_use]
    pub fn dom_children(&self, node: StyleNodeID) -> DomChildren<'_> {
        DomChildren {
            tree: self,
            next: node.element_index().and_then(|index| self.first_child[index as usize]),
        }
    }

    /// The parent a text node is linked under.
    #[must_use]
    pub fn text_parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.text.parent[node.text_index()? as usize]
    }

    #[must_use]
    pub fn next_sibling_in_dom_order(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        match node.text_index() {
            Some(index) => self.text.next_sibling[index as usize],
            None => self.next_sibling[node.element_slot()],
        }
    }

    #[must_use]
    pub fn previous_sibling_in_dom_order(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        match node.text_index() {
            Some(index) => self.text.previous_sibling[index as usize],
            None => self.previous_sibling[node.element_slot()],
        }
    }

    fn next_sibling_mut(&mut self, node: StyleNodeID) -> &mut Option<StyleNodeID> {
        match node.text_index() {
            Some(index) => &mut self.text.next_sibling[index as usize],
            None => &mut self.next_sibling[node.element_slot()],
        }
    }

    fn previous_sibling_mut(&mut self, node: StyleNodeID) -> &mut Option<StyleNodeID> {
        match node.text_index() {
            Some(index) => &mut self.text.previous_sibling[index as usize],
            None => &mut self.previous_sibling[node.element_slot()],
        }
    }

    // -- Relation maintenance ----------------------------------------------------------------

    pub fn set_parent(&mut self, node: StyleNodeID, parent: Option<StyleNodeID>) {
        self.dom_order_version += 1;
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
        self.dom_order_version += 1;
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

    // -- Element ids -------------------------------------------------------------------------

    /// Record the id an element answers to, or clear it with atom zero. The name is the id as
    /// written; see [`ElementIdIndex`] for why that is not the atom a selector is compiled against.
    pub fn set_element_id_name(&mut self, node: StyleNodeID, name: StyleAtomID, memory: &mut MemoryController) {
        if name.is_none()
            && self
                .ids
                .as_ref()
                .is_none_or(|index| !index.name_of_node.contains_key(&node))
        {
            return;
        }
        let before = self.id_capacity_bytes();
        self.ids.get_or_insert_with(Box::default).set(node, name);
        let current = self.id_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// The first element in tree order that answers to `name` inside `tree_scope`, which is what
    /// `getElementById` answers with.
    ///
    /// Duplicate ids are legal, so the candidates are ordered here rather than at the write: an
    /// element's place in the tree moves without its id moving, so an index kept in tree order
    /// would have to be resorted by every insertion. A name shared by k elements costs k tree
    /// order comparisons, each a climb to the common ancestor and a walk along its children, which
    /// is fine for the handful of duplicates real pages have.
    #[must_use]
    pub fn element_by_id(&self, tree_scope: TreeScopeID, name: StyleAtomID) -> Option<StyleNodeID> {
        let nodes = self.ids.as_ref()?.nodes_by_name.get(&name)?;
        let mut first = None;
        for &node in nodes {
            let Some(index) = node.element_index() else {
                continue;
            };
            if !self.live.contains(index as usize) || self.tree_scope(node) != tree_scope {
                continue;
            }
            first = match first {
                Some(current) if !self.precedes_in_tree_order(node, current) => Some(current),
                _ => Some(node),
            };
        }
        first
    }

    /// Whether `a` comes before `b` in the tree order of the scope they share.
    fn precedes_in_tree_order(&self, a: StyleNodeID, b: StyleNodeID) -> bool {
        if a == b {
            return false;
        }
        // Climb the deeper of the two to the other's level. Arriving at the other node says it is
        // an ancestor, and an ancestor always comes first.
        let (mut left, mut right) = (a, b);
        for _ in self.depth(b)..self.depth(a) {
            let Some(parent) = self.parent(left) else {
                return false;
            };
            left = parent;
        }
        if left == b {
            return false;
        }
        for _ in self.depth(a)..self.depth(b) {
            let Some(parent) = self.parent(right) else {
                return false;
            };
            right = parent;
        }
        if right == a {
            return true;
        }
        while self.parent(left) != self.parent(right) {
            let (Some(next_left), Some(next_right)) = (self.parent(left), self.parent(right)) else {
                return false;
            };
            left = next_left;
            right = next_right;
        }
        // Siblings now, so whichever the child sequence reaches first comes first.
        let mut sibling = self.next_element_sibling(left);
        while let Some(node) = sibling {
            if node == right {
                return true;
            }
            sibling = self.next_element_sibling(node);
        }
        false
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
        self.dom_order_version += 1;
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

    /// The node's parent in the tree it is in, or a shadow root's host: the host's
    /// `parent_or_shadow_host()`, so a slotted element continues through its light-tree parent.
    #[must_use]
    pub fn parent_or_shadow_host(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.parent(node).or_else(|| self.host_of(node))
    }

    /// Assign `node` to `slot`. Passing `None` removes the assignment.
    ///
    /// Slot assignment changes flat-tree identity even when the DOM parent does not move, which is
    /// why it is its own relation rather than a derived view of the DOM tree.
    pub fn set_assigned_slot(&mut self, node: StyleNodeID, slot: Option<StyleNodeID>, memory: &mut MemoryController) {
        if slot.is_none()
            && self
                .shadow
                .as_ref()
                .is_none_or(|shadow| shadow.assigned_slot.get(node).is_none())
        {
            return;
        }
        let before = self.shadow_capacity_bytes();
        let shadow = self.shadow_mut();
        shadow.assigned_slot.remove(node);
        if let Some(slot) = slot {
            shadow.assigned_slot.insert(node, slot);
        }
        let current = self.shadow_capacity_bytes();
        self.record_capacity_change(memory, before, current);
    }

    /// Replace the ordered list of nodes `slot` has assigned to it.
    ///
    /// The list is published whole rather than assembled from the per-element assignments above,
    /// because neither of the two things it has to be can be recovered from them. A text node is a
    /// slottable but holds no relation row, so its assignment cannot be staged beside an element's;
    /// and the order is the DOM's, not the order assignments arrive in: a manual assignment orders
    /// its nodes the way `assign()` named them, and a reorder among a slot's own assignees changes
    /// no node's slot at all.
    pub fn set_assigned_nodes(&mut self, slot: StyleNodeID, nodes: &[StyleNodeID], memory: &mut MemoryController) {
        if nodes.is_empty()
            && self
                .shadow
                .as_ref()
                .is_none_or(|shadow| !shadow.assigned_nodes.contains_key(&slot))
        {
            return;
        }
        let before = self.shadow_capacity_bytes();
        let shadow = self.shadow_mut();
        let mut assigned = shadow.assigned_nodes.remove(&slot).unwrap_or_default();
        // A tree-wide assignment can have moved one of the departing text nodes to another slot
        // already, and this slot must not take that newer assignment away again.
        for node in &assigned {
            if shadow.text_assigned_slot.get(node) == Some(&slot) {
                shadow.text_assigned_slot.remove(node);
            }
        }
        assigned.clear();
        assigned.extend_from_slice(nodes);
        for &node in &assigned {
            if node.text_index().is_some() {
                shadow.text_assigned_slot.insert(node, slot);
            }
        }
        if !assigned.is_empty() {
            shadow.assigned_nodes.insert(slot, assigned);
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
        let shadow = self.shadow.as_ref()?;
        if node.text_index().is_some() {
            return shadow.text_assigned_slot.get(&node).copied();
        }
        shadow.assigned_slot.get(node)
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

    /// The element whose computed values this element inherits. Unlike [`Self::flat_tree_parent`],
    /// this keeps the DOM parent of an element excluded from the flat tree: such an element can
    /// still have its style requested through CSSOM and inherits from that parent when it does.
    #[must_use]
    pub fn inheritance_parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        if let Some(slot) = self.assigned_slot_of(node) {
            return Some(slot);
        }
        let parent = self.parent(node)?;
        Some(self.host_of(parent).unwrap_or(parent))
    }

    /// The inheritance relations and editable identities an immutable hover reads for cursor selection.
    pub(crate) fn hover_cursor_relations(&self) -> (Vec<Option<StyleNodeID>>, Vec<StyleNodeID>) {
        let mut parents = self.parent.clone();
        if self.shadow.is_some() {
            for (index, parent) in parents.iter_mut().enumerate().skip(1) {
                *parent = self.inheritance_parent(StyleNodeID::element(index as u32));
            }
        }
        let editable = self
            .dom_paint_facts
            .iter()
            .filter_map(|(&node, &facts)| {
                (facts & crate::layout::node_data::DomPaintFact::EditableOrEditingHost as u8 != 0).then_some(node)
            })
            .collect();
        (parents, editable)
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

    /// Rank a batch in the same dependency order as `compare_style_reaction_order`.
    /// Each ancestor is visited once, even for a deep chain of reacting descendants.
    pub fn style_reaction_order_ranks(
        &self,
        nodes: impl IntoIterator<Item = StyleNodeID>,
    ) -> HashMap<StyleNodeID, usize> {
        let mut seen = HashSet::default();
        let mut children: HashMap<StyleNodeID, Vec<(u8, StyleNodeID)>> = HashMap::default();
        let mut roots = Vec::new();
        for mut node in nodes {
            while seen.insert(node) {
                if let Some((parent, branch)) = self.style_reaction_parent(node) {
                    children.entry(parent).or_default().push((branch, node));
                    node = parent;
                } else {
                    roots.push(node);
                    break;
                }
            }
        }
        // The existing order uses identity within each branch, not DOM sibling order.
        roots.sort_unstable_by(|first, second| second.cmp(first));
        for children in children.values_mut() {
            children.sort_unstable_by(|first, second| second.cmp(first));
        }
        let mut ranks = HashMap::default();
        let mut pending = roots;
        while let Some(node) = pending.pop() {
            ranks.insert(node, ranks.len());
            if let Some(children) = children.get(&node) {
                pending.extend(children.iter().map(|&(_, child)| child));
            }
        }
        ranks
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

    /// Whether `node` is `root` or lies below it in the shadow-including tree: the climb out of a
    /// shadow tree continues at the host rather than stopping there.
    ///
    /// `root` names an element or a shadow root. A climb that runs out of parents has reached a
    /// child of the document, which no element contains, so it answers false rather than taking a
    /// document identity to compare against.
    #[must_use]
    pub fn is_in_shadow_including_subtree_of(&self, node: StyleNodeID, root: StyleNodeID) -> bool {
        if node == root {
            return true;
        }
        if root.text_index().is_some() {
            return false;
        }
        // Only an element owns a child sequence, so a text node is answered for by the element it
        // is linked under.
        let mut candidate = match node.text_index() {
            Some(_) => match self.text_parent(node) {
                Some(parent) => parent,
                None => return false,
            },
            None => node,
        };
        loop {
            if candidate == root {
                return true;
            }
            candidate = match self.host_of(candidate) {
                Some(host) => host,
                None => match self.parent(candidate) {
                    Some(parent) => parent,
                    None => return false,
                },
            };
        }
    }

    // -- Accounting --------------------------------------------------------------------------

    /// Exact capacity of every column, charged to Tier 1.
    #[must_use]
    #[cfg(test)]
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
                self.first_child,
                self.next_sibling,
                self.previous_sibling,
                self.dom_paint_facts,
                self.unique_node_ids,
                self.table_spans,
                self.top_layer,
            ];
            cached [];
            nested [
                self.tree_scope
                    .as_ref()
                    .map_or(0, |column| column.capacity() * size_of::<TreeScopeID>()),
                self.live.capacity_bytes(),
                self.relation_only.capacity_bytes(),
            ];
            skip [];
        }
    }

    fn reuse_capacity_bytes(&self) -> u64 {
        (self.pending_reuse.capacity() * size_of::<u32>()) as u64
    }

    fn shadow_capacity_bytes(&self) -> u64 {
        self.shadow.as_ref().map_or(0, |relations| relations.capacity_bytes())
    }

    fn id_capacity_bytes(&self) -> u64 {
        self.ids.as_ref().map_or(0, |index| index.capacity_bytes())
    }

    fn retirement_capacity_bytes(&self) -> u64 {
        (self.pending_reuse.capacity() * size_of::<u32>()) as u64
            + self.shadow_capacity_bytes()
            + self.id_capacity_bytes()
    }

    fn recompute_capacity_bytes(&self) -> u64 {
        self.identity_capacity_bytes()
            + self.reuse_capacity_bytes()
            + self.shadow_capacity_bytes()
            + self.id_capacity_bytes()
            + self.text.capacity_bytes()
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
        debug_assert!(
            node.element_index().is_some(),
            "tree relations are keyed by element identity"
        );
        node.element_slot()
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

/// The rows of text identities, indexed by text index with slot 0 unused. A text node owns no
/// element relations, only its place in the DOM child sequence. A retired index waits in
/// `pending_reuse` until its epoch retires, as an element's does.
#[derive(Clone, Default)]
struct TextRows {
    parent: Vec<Option<StyleNodeID>>,
    next_sibling: Vec<Option<StyleNodeID>>,
    previous_sibling: Vec<Option<StyleNodeID>>,
    live: BitColumn,
    /// Whether the node's data is nothing but ASCII whitespace, which is what decides whether the
    /// layout tree build can collapse it away rather than give it a box of its own.
    is_ascii_whitespace: BitColumn,
    /// Whether the node sits in a user agent shadow tree. An element records the same fact among
    /// its construction facts; a text node has no element columns, so it records it here.
    is_in_user_agent_shadow_tree: BitColumn,
    /// Whether the node is the text of a password input, which renders as replacement characters.
    is_password_input: BitColumn,
    /// The characters the node holds, sharing the document's string rather than copying it. The
    /// layout tree build reads them to render a text box, so they are published where the node
    /// arrives and wherever its data is replaced.
    data: Vec<ak::Utf16String>,
    pending_reuse: Vec<u32>,
}

impl TextRows {
    fn capacity_bytes(&self) -> u64 {
        capacity_bytes! {
            shallow [
                self.parent,
                self.next_sibling,
                self.previous_sibling,
                self.data,
                self.pending_reuse,
            ];
            cached [];
            nested [
                self.live.capacity_bytes(),
                self.is_ascii_whitespace.capacity_bytes(),
                self.is_in_user_agent_shadow_tree.capacity_bytes(),
                self.is_password_input.capacity_bytes(),
            ];
            skip [];
        }
    }
}

pub struct DomChildren<'a> {
    tree: &'a StyleNodeTree,
    next: Option<StyleNodeID>,
}

impl Iterator for DomChildren<'_> {
    type Item = StyleNodeID;

    fn next(&mut self) -> Option<StyleNodeID> {
        let current = self.next?;
        self.next = self.tree.next_sibling_in_dom_order(current);
        Some(current)
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
    use super::*;

    #[test]
    fn element_and_text_identities_index_their_own_kinds() {
        let element = StyleNodeID::element(7);
        let text = StyleNodeID::text(7);
        assert_ne!(element, text);
        assert_eq!(element.element_index(), Some(7));
        assert_eq!(element.text_index(), None);
        assert_eq!(text.element_index(), None);
        assert_eq!(text.text_index(), Some(7));
        assert_eq!(StyleNodeID::from_raw(text.raw()), Some(text));
    }

    #[test]
    fn radix_sorts_style_node_identities() {
        let mut nodes = vec![
            StyleNodeID::element(i32::MAX as u32),
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
                StyleNodeID::element(i32::MAX as u32),
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
            let mut memory = MemoryController::new();
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
    fn an_id_names_the_first_of_its_elements_in_tree_order() {
        let mut fixture = TreeFixture::new();
        let root = fixture.element();
        let first = fixture.element();
        let second = fixture.element();
        let nested = fixture.element();
        fixture.attach_children(root, &[first, second]);
        fixture.attach_children(first, &[nested]);
        let name = StyleAtomID(7);

        // The order the elements take the name in is not tree order.
        fixture.tree.set_element_id_name(second, name, &mut fixture.memory);
        fixture.tree.set_element_id_name(nested, name, &mut fixture.memory);
        assert_eq!(fixture.tree.element_by_id(TreeScopeID::DOCUMENT, name), Some(nested));

        fixture.tree.set_element_id_name(first, name, &mut fixture.memory);
        assert_eq!(fixture.tree.element_by_id(TreeScopeID::DOCUMENT, name), Some(first));

        fixture
            .tree
            .set_element_id_name(first, StyleAtomID::NONE, &mut fixture.memory);
        assert_eq!(fixture.tree.element_by_id(TreeScopeID::DOCUMENT, name), Some(nested));
        assert_eq!(fixture.tree.element_by_id(TreeScopeID::DOCUMENT, StyleAtomID(8)), None);
    }

    #[test]
    fn a_retired_element_leaves_the_id_index() {
        let mut fixture = TreeFixture::new();
        let root = fixture.element();
        let children = [fixture.element(), fixture.element(), fixture.element()];
        fixture.attach_children(root, &children);
        // Three elements sharing a name spill its candidate list out of line.
        let name = StyleAtomID(7);
        for child in children {
            fixture.tree.set_element_id_name(child, name, &mut fixture.memory);
        }
        assert_eq!(
            fixture.tree.element_by_id(TreeScopeID::DOCUMENT, name),
            Some(children[0])
        );
        assert!(fixture.tree.ids.as_ref().unwrap().candidate_bytes > 0);
        assert_eq!(
            fixture.memory.bytes_in_category(MemoryCategory::RelationColumns),
            fixture.tree.capacity_bytes()
        );

        for child in children {
            fixture.tree.set_parent(child, None);
        }
        fixture.tree.set_first_element_child(root, None);
        fixture.tree.retire_elements(&children, &mut fixture.memory);
        assert_eq!(fixture.tree.element_by_id(TreeScopeID::DOCUMENT, name), None);
        // The spilled list went with the name, and the controller was told.
        assert_eq!(fixture.tree.ids.as_ref().unwrap().candidate_bytes, 0);
        assert_eq!(
            fixture.memory.bytes_in_category(MemoryCategory::RelationColumns),
            fixture.tree.capacity_bytes()
        );
    }

    #[test]
    fn id_names_are_roots_of_the_atom_sweep() {
        let mut fixture = TreeFixture::new();
        let element = fixture.element();
        let name = StyleAtomID(7);
        fixture.tree.set_element_id_name(element, name, &mut fixture.memory);
        let mut atoms = HashSet::default();
        fixture.tree.collect_atoms(&mut atoms);
        assert!(atoms.contains(&name));
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
    fn text_identities_are_reused_only_after_the_epoch_retires() {
        let mut fixture = TreeFixture::new();
        let element = fixture.element();
        let first = fixture.tree.allocate_text(&mut fixture.memory);
        let second = fixture.tree.allocate_text(&mut fixture.memory);
        assert_eq!(first, StyleNodeID::text(1));
        assert_eq!(second, StyleNodeID::text(2));
        assert!(fixture.tree.is_live(first) && fixture.tree.is_live(element));
        assert_eq!(fixture.tree.connected_element_count(), 1);

        fixture.tree.retire_texts([first], &mut fixture.memory);
        assert!(!fixture.tree.is_live(first));
        assert!(fixture.tree.is_live(element));

        let third = fixture.tree.allocate_text(&mut fixture.memory);
        assert_eq!(third, StyleNodeID::text(3));

        fixture.tree.release_retired_identities(&mut fixture.memory);
        let reused = fixture.tree.allocate_text(&mut fixture.memory);
        assert_eq!(reused, first);
        assert!(fixture.tree.is_live(reused));
        assert_eq!(fixture.tree.live_nodes().collect::<Vec<_>>(), vec![element]);
    }

    #[test]
    fn a_retired_identity_holds_no_table_spans_when_it_is_reused() {
        let mut fixture = TreeFixture::new();
        let element = fixture.element();
        let spans = TableSpans {
            column_span: 2,
            row_span: 3,
            raw_column_span: 2,
        };
        fixture.tree.set_table_spans(element, spans, &mut fixture.memory);
        assert_eq!(fixture.tree.table_spans(element), spans);

        fixture.tree.retire_element(element, &mut fixture.memory);
        fixture.tree.release_retired_identities(&mut fixture.memory);
        assert_eq!(fixture.tree.allocate_element(&mut fixture.memory), element);
        assert_eq!(fixture.tree.table_spans(element), TableSpans::default());
    }

    #[test]
    fn a_retired_identity_leaves_the_top_layer_in_order() {
        let mut fixture = TreeFixture::new();
        let first = fixture.element();
        let second = fixture.element();
        let third = fixture.element();
        fixture.tree.set_top_layer(&[third, first, second], &mut fixture.memory);
        assert_eq!(fixture.tree.top_layer(), [third, first, second]);

        fixture.tree.retire_element(first, &mut fixture.memory);
        assert_eq!(fixture.tree.top_layer(), [third, second]);
        fixture.tree.release_retired_identities(&mut fixture.memory);
        assert_eq!(fixture.tree.allocate_element(&mut fixture.memory), first);
        assert_eq!(fixture.tree.top_layer(), [third, second]);
        assert_eq!(
            fixture.memory.bytes_in_category(MemoryCategory::RelationColumns),
            fixture.tree.capacity_bytes()
        );
    }

    #[test]
    fn a_retired_identity_holds_no_paint_facts_when_it_is_reused() {
        let mut fixture = TreeFixture::new();
        let element = fixture.element();
        let text = fixture.tree.allocate_text(&mut fixture.memory);
        fixture.tree.set_dom_paint_facts(element, 3, &mut fixture.memory);
        fixture.tree.set_dom_paint_facts(text, 4, &mut fixture.memory);
        assert_eq!(fixture.tree.dom_paint_facts(element), 3);
        assert_eq!(fixture.tree.dom_paint_facts(text), 4);
        fixture.tree.set_dom_paint_facts(element, 0, &mut fixture.memory);
        assert_eq!(fixture.tree.dom_paint_facts(element), 0);
        fixture.tree.set_dom_paint_facts(element, 1, &mut fixture.memory);

        fixture.tree.retire_element(element, &mut fixture.memory);
        fixture.tree.retire_texts([text], &mut fixture.memory);
        fixture.tree.release_retired_identities(&mut fixture.memory);
        assert_eq!(fixture.tree.allocate_element(&mut fixture.memory), element);
        assert_eq!(fixture.tree.allocate_text(&mut fixture.memory), text);
        assert_eq!(fixture.tree.dom_paint_facts(element), 0);
        assert_eq!(fixture.tree.dom_paint_facts(text), 0);
    }

    #[test]
    fn text_nodes_take_places_in_the_dom_child_sequence_beside_elements() {
        let mut fixture = TreeFixture::new();
        let parent = fixture.element();
        let first_text = fixture.tree.allocate_text(&mut fixture.memory);
        let element = fixture.element();
        let last_text = fixture.tree.allocate_text(&mut fixture.memory);
        fixture.tree.link_in_dom_order(first_text, Some(parent), None);
        fixture.tree.link_in_dom_order(element, Some(parent), Some(first_text));
        fixture.tree.link_in_dom_order(last_text, Some(parent), Some(element));

        assert_eq!(
            fixture.tree.dom_children(parent).collect::<Vec<_>>(),
            vec![first_text, element, last_text]
        );
        assert_eq!(fixture.tree.text_parent(last_text), Some(parent));
        assert_eq!(fixture.tree.children(parent).count(), 0);
        assert_eq!(fixture.tree.connected_element_count(), 2);

        // A move is an unlink followed by a link at the new place.
        fixture.tree.unlink_from_dom_order(last_text, Some(parent));
        fixture.tree.link_in_dom_order(last_text, Some(parent), None);
        assert_eq!(
            fixture.tree.dom_children(parent).collect::<Vec<_>>(),
            vec![last_text, first_text, element]
        );

        fixture.tree.unlink_from_dom_order(first_text, Some(parent));
        fixture.tree.retire_texts([first_text], &mut fixture.memory);
        assert!(!fixture.tree.is_live(first_text));
        assert_eq!(
            fixture.tree.dom_children(parent).collect::<Vec<_>>(),
            vec![last_text, element]
        );
        assert_eq!(fixture.tree.previous_sibling_in_dom_order(element), Some(last_text));

        // Like an element's, a retired text identity is reused only once its epoch retires.
        let replacement = fixture.tree.allocate_text(&mut fixture.memory);
        assert_ne!(replacement, first_text);
        fixture.tree.release_retired_identities(&mut fixture.memory);
        let reused = fixture.tree.allocate_text(&mut fixture.memory);
        assert_eq!(reused, first_text);
        assert_eq!(fixture.tree.text_parent(reused), None);
        assert_eq!(fixture.tree.next_sibling_in_dom_order(reused), None);
    }

    #[test]
    fn a_text_slottable_holds_a_place_in_its_slots_assigned_list() {
        let mut fixture = TreeFixture::new();
        let slot = fixture.element();
        let element = fixture.element();
        let text = fixture.tree.allocate_text(&mut fixture.memory);
        fixture
            .tree
            .set_assigned_nodes(slot, &[text, element], &mut fixture.memory);
        assert_eq!(fixture.tree.assigned_nodes_of(slot), &[text, element]);
        assert_eq!(fixture.tree.assigned_slot_of(text), Some(slot));

        fixture.tree.retire_texts([text], &mut fixture.memory);
        assert_eq!(fixture.tree.assigned_nodes_of(slot), &[element]);
        assert_eq!(
            fixture.memory.bytes_in_category(MemoryCategory::RelationColumns),
            fixture.tree.capacity_bytes()
        );
    }

    #[test]
    fn a_relation_only_identity_roots_the_dom_child_sequence_without_counting_as_an_element() {
        let mut fixture = TreeFixture::new();
        let document = fixture.element();
        fixture.tree.mark_relation_only(document, &mut fixture.memory);
        let document_element = fixture.element();
        fixture.tree.link_in_dom_order(document_element, Some(document), None);

        assert!(fixture.tree.is_relation_only(document));
        assert!(!fixture.tree.is_relation_only(document_element));
        assert_eq!(fixture.tree.connected_element_count(), 1);
        assert_eq!(
            fixture.tree.dom_children(document).collect::<Vec<_>>(),
            vec![document_element]
        );
        assert_eq!(fixture.tree.parent(document_element), None);

        fixture
            .tree
            .retire_elements(&[document, document_element], &mut fixture.memory);
        assert_eq!(fixture.tree.connected_element_count(), 0);
        fixture.tree.release_retired_identities(&mut fixture.memory);
        let reused = fixture.element();
        assert!(!fixture.tree.is_relation_only(reused));
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
        fixture.tree.set_assigned_nodes(slot, &[slotted], &mut fixture.memory);
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
    fn the_shadow_including_subtree_test_climbs_out_of_a_shadow_tree_to_its_host() {
        let mut fixture = TreeFixture::new();
        let root_element = fixture.element();
        let host = fixture.element();
        let light_child = fixture.element();
        let shadow_root = fixture.element();
        let shadow_child = fixture.element();
        let nested_host = fixture.element();
        let nested_shadow_root = fixture.element();
        let nested_shadow_child = fixture.element();
        let outside = fixture.element();
        let text = fixture.tree.allocate_text(&mut fixture.memory);
        fixture.attach_children(root_element, &[host, outside]);
        fixture.attach_children(host, &[light_child]);
        fixture.attach_children(shadow_root, &[shadow_child, nested_host]);
        fixture.tree.set_shadow_root(host, shadow_root, &mut fixture.memory);
        fixture.attach_children(nested_shadow_root, &[nested_shadow_child]);
        fixture
            .tree
            .set_shadow_root(nested_host, nested_shadow_root, &mut fixture.memory);
        fixture.tree.link_in_dom_order(text, Some(shadow_child), None);

        let is_in = |node, root| fixture.tree.is_in_shadow_including_subtree_of(node, root);
        assert!(is_in(host, host), "the test is inclusive");
        assert!(is_in(light_child, host), "a light child lies below its host");
        assert!(is_in(shadow_child, host), "and so does what its shadow tree holds");
        assert!(is_in(text, host), "text inside that tree included");
        assert!(
            is_in(nested_shadow_child, host),
            "a shadow tree nested inside that one included"
        );
        assert!(
            is_in(shadow_child, shadow_root),
            "a shadow root still holds its own tree"
        );
        assert!(!is_in(outside, host), "a sibling subtree lies outside");
        assert!(!is_in(host, light_child), "a child is not an ancestor of its parent");
        assert!(!is_in(host, text), "a text node holds nothing but itself");
        assert!(
            is_in(light_child, root_element),
            "the climb reaches the element above the host"
        );
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
        fixture
            .tree
            .set_assigned_nodes(slot, &[first, second], &mut fixture.memory);
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
        fixture.tree.set_assigned_nodes(slot, &[assigned], &mut fixture.memory);

        let mut reactions = vec![light_child, assigned, fallback, slot, host];
        reactions.sort_unstable_by(|first, second| fixture.tree.compare_style_reaction_order(*first, *second));

        assert_eq!(reactions, vec![host, slot, fallback, assigned, light_child]);
        let ranks = fixture.tree.style_reaction_order_ranks(reactions.iter().copied());
        for &first in &reactions {
            for &second in &reactions {
                assert_eq!(
                    ranks[&first].cmp(&ranks[&second]),
                    fixture.tree.compare_style_reaction_order(first, second)
                );
            }
        }
    }

    #[test]
    fn reaction_ranks_cover_sparse_batches_and_deep_chains() {
        let mut fixture = TreeFixture::new();
        let first_root = fixture.element();
        let second_root = fixture.element();
        let mut chain = vec![first_root];
        for _ in 0..4096 {
            let child = fixture.element();
            fixture.attach_children(*chain.last().unwrap(), &[child]);
            chain.push(child);
        }
        let leaf = *chain.last().unwrap();
        let ranks = fixture.tree.style_reaction_order_ranks([second_root, leaf, leaf]);
        assert_eq!(ranks.len(), chain.len() + 1);
        for pair in chain.windows(2) {
            assert!(ranks[&pair[0]] < ranks[&pair[1]]);
        }
        assert!(ranks[&leaf] < ranks[&second_root]);
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
        fixture
            .tree
            .set_assigned_nodes(first_slot, &[node], &mut fixture.memory);
        assert_eq!(fixture.tree.assigned_nodes_of(first_slot), &[node]);

        fixture
            .tree
            .set_assigned_slot(node, Some(second_slot), &mut fixture.memory);
        fixture.tree.set_assigned_nodes(first_slot, &[], &mut fixture.memory);
        fixture
            .tree
            .set_assigned_nodes(second_slot, &[node], &mut fixture.memory);
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
        fixture.tree.set_assigned_nodes(second_slot, &[], &mut fixture.memory);
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
        fixture.tree.set_assigned_nodes(slot, &[slotted], &mut fixture.memory);
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
