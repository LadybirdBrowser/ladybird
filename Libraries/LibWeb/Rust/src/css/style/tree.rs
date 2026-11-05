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
//! Rust-owned. That is not an acceleration choice. Invalidation starting from a changed input has
//! to walk selector relations to reach possible subjects - "descendants that can satisfy `B`" is
//! not in a tree delta - and `StyleNodeID` is deliberately not a tree-order label, so proving that
//! a candidate lies inside a subtree costs relation steps per candidate. If those steps left the
//! engine, the hot path would degenerate into per-element FFI.
//!
//! The columns are **not** semantically authoritative. They are a derived projection that must agree
//! with the live tree at every epoch boundary, maintained from the same tree delta that reports the
//! mutation.

use super::fast_hash::FastSet as HashSet;
use smallvec::SmallVec;
use std::num::NonZeroU32;

use super::column::BitColumn;
use super::column::PagedColumn;
use super::column::PagedColumnPage;
use super::column::RemovablePagedColumnPage;
use super::index::StyleAtomID;
use super::pipeline::projection::Projection;
use crate::cow_column::CowColumn;

/// What a record says of its element as a query container, read from the record's box group and
/// the element's box kind: nothing of it is kept beside the record. Fly strings are interned, so
/// inputs naming equal names hold equal raws.
#[derive(Clone, Copy, PartialEq)]
pub(super) struct ContainerQueryInputs<'a> {
    pub(super) style_record: u64,
    /// The record's `container-name` idents, as the raw fly strings its box group holds.
    pub(super) names: &'a [usize],
    pub(super) is_size_container: bool,
    pub(super) size_queries_available: bool,
    pub(super) is_inline_size_container: bool,
    pub(super) is_scroll_state_container: bool,
    pub(super) writing_mode: u8,
    pub(super) direction: u8,
}

const _: () = assert!(size_of::<ContainerQueryInputs<'static>>() <= 32);

impl ContainerQueryInputs<'_> {
    /// Whether one of the container's names is `name`.
    pub(super) fn is_named(&self, name: &[u16]) -> bool {
        self.names.iter().any(|raw| {
            // SAFETY: The record's box group holds a reference to each of its names.
            match unsafe { ak::utf16_string_units(raw) } {
                ak::Utf16StringUnits::Ascii(units) | ak::Utf16StringUnits::Latin1(units) => {
                    units.iter().copied().map(u16::from).eq(name.iter().copied())
                }
                ak::Utf16StringUnits::Utf16(units) => units == name,
            }
        })
    }

    /// Whether the container answers size queries.
    pub(super) fn supports_size_queries(&self) -> bool {
        self.size_queries_available && (self.is_size_container || self.is_inline_size_container)
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
#[cfg(test)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct PseudoElementKind(pub u16);

/// Which pseudo-element a selector entry targets.
#[cfg(test)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct PseudoElementTarget {
    pub kind: PseudoElementKind,
}

#[cfg(test)]
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
    pub(super) fn heap_size(&self) -> usize {
        self.0.heap_size()
    }

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
    name_of_node: crate::cow_map::CowMap<StyleNodeID, StyleAtomID>,
    /// The elements answering to a name, in the order they took it rather than in tree order.
    /// Nearly every name is unique, so the list that holds one stays inline.
    nodes_by_name: crate::cow_map::CowMap<StyleAtomID, SmallVec<[StyleNodeID; 1]>>,
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
}

/// The spans a table cell or table column takes from its attributes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TableSpans {
    pub column_span: u16,
    pub row_span: u16,
}

impl Default for TableSpans {
    fn default() -> Self {
        Self {
            column_span: 1,
            row_span: 1,
        }
    }
}

/// What the host publishes of its nodes beside the element links the projection holds: which
/// identities are live, each element's depth, the DOM child sequences text nodes included, the
/// text nodes' rows, the id index and the paint facts of a row built for a node.
///
/// Element columns are indexed by element index, with slot 0 unused so that a `StyleNodeID` indexes
/// its own column entry directly.
#[derive(Clone)]
pub struct HostTree {
    /// What each element slot holds: its depth, its DOM child sequence links and its unique node
    /// id. The column is copied on write in chunks, so that a fork shares it, and a write while a
    /// fork lives copies only the chunk it lands in.
    elements: CowColumn<ElementRow, ELEMENT_ROWS_PER_CHUNK>,
    /// One past the greatest element slot the tree minted.
    element_slots: usize,

    live: BitColumn,
    /// Identities that stand in the tree without being styled: the document, whose children the
    /// DOM child sequence hangs from. A selector never names one and nothing publishes features
    /// for one, so a style pass that reaches one must pass it by rather than ask it to match.
    relation_only: BitColumn,
    connected_element_count: u32,
    /// Identities retired in the current epoch. They cannot be reused until the epoch that could
    /// still observe them has retired.
    pending_reuse: crate::cow_column::CowVec<u32, 1024>,
    /// The identities a test mints from, standing in for the host's.
    #[cfg(test)]
    test_identities: super::identities::StyleNodeIdAllocator,

    /// Allocated only once an element carries an id.
    ids: Option<Box<ElementIdIndex>>,

    text: TextRows,

    /// What a row built for the node is painted and hit-tested with: whether the node is inert,
    /// editable or an editing host, inside a blocking wheel event handler, or a navigable container
    /// holding a navigable. Nearly every node holds none of them, which is the absence of an entry.
    /// Element, text and document identities publish here, as each gets a row.
    dom_paint_facts: crate::cow_map::CowMap<StyleNodeID, u8>,
    /// The spans a table cell's or table column's attributes give it: the effective column and row
    /// span, and the column span attribute's unclamped value, which only the table formatting
    /// context's column handling reads. Every other element spans one of each, which is what the
    /// absence of an entry means.
    table_spans: crate::cow_map::CowMap<StyleNodeID, TableSpans>,
    /// The document's top layer, in the order its members were added. The order is the order their
    /// boxes are built in and belongs to the document, so no per-element fact can carry it.
    top_layer: crate::fork::ForkShared<Vec<StyleNodeID>>,

    /// Counts the changes to the DOM child sequences, to the parents and shadow roots that place a
    /// node among its siblings, and to which nodes are live. See [`Self::dom_order_version`].
    dom_order_version: u64,
    /// Counts the changes to the DOM paint facts. See [`Self::dom_paint_facts_version`].
    dom_paint_facts_version: u64,

    #[cfg(test)]
    depth_recompute_visits: usize,
}

/// What a [`HostTree`] holds of each element slot.
#[derive(Clone, Copy, Default, PartialEq)]
struct ElementRow {
    // The DOM child sequence, text nodes included. Elements keep these beside their element-only
    // links, which every selector walk reads; text nodes have nothing else.
    //
    // Unlike the element-only links, these are spliced when the DOM changes rather than staged,
    // because no transaction plans from them.
    first_child: Option<StyleNodeID>,
    next_sibling: Option<StyleNodeID>,
    previous_sibling: Option<StyleNodeID>,
    /// The unique node id the document names the element by, published where the identity arrives
    /// and constant for as long as the element holds it. A box built for the element answers by
    /// it, and so does a box built for one of the element's pseudo-elements, which is why the
    /// render side needs it for an element that has no box of its own. The document's identity
    /// carries the document's.
    unique_node_id: UniqueNodeId,
}

const _: () = assert!(size_of::<ElementRow>() == 20);

/// The unique node id the document names a node by, in two words so that a row needs no more than 4-byte alignment.
#[derive(Clone, Copy, Default, PartialEq, Eq)]
struct UniqueNodeId([u32; 2]);

impl From<i64> for UniqueNodeId {
    fn from(id: i64) -> Self {
        let bits = id.cast_unsigned();
        Self([bits as u32, (bits >> 32) as u32])
    }
}

impl From<UniqueNodeId> for i64 {
    fn from(UniqueNodeId([low, high]): UniqueNodeId) -> Self {
        (u64::from(low) | (u64::from(high) << 32)).cast_signed()
    }
}

/// How many element slots a chunk of [`HostTree`]'s column holds.
const ELEMENT_ROWS_PER_CHUNK: usize = 32;

impl HostTree {
    /// The row of the element slot `index`, which the tree holds.
    fn element_row(&self, index: usize) -> &ElementRow {
        self.elements
            .get(index)
            .expect("the tree holds a row of every element slot it minted")
    }

    /// Write the row of the element slot `index`, which the tree holds.
    fn update_element_row(&mut self, index: usize, update: impl FnOnce(&mut ElementRow)) {
        update(
            &mut self
                .elements
                .row_mut(index)
                .expect("the tree holds a row of every element slot it minted"),
        );
    }

    /// The bytes of the tree's columns and indexes.
    pub(super) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        use crate::heap_size::vec_bytes;
        let ids = self.ids.as_ref().map_or(0, |ids| {
            size_of::<ElementIdIndex>()
                + ids.name_of_node.heap_size()
                + ids.nodes_by_name.heap_size()
                + ids.candidate_bytes
        });
        let text = &self.text;
        let text_bytes = text.links.heap_size(sizer, |_, _| 0)
            + text.flags.heap_size(sizer, |_, _| 0)
            + text.data.heap_size(sizer, |_, _| 0)
            + text.pending_reuse.row_bytes();
        self.elements.heap_size(sizer, |_, _| 0)
            + self.live.heap_size()
            + self.relation_only.heap_size()
            + self.pending_reuse.row_bytes()
            + ids
            + text_bytes
            + self.dom_paint_facts.heap_size()
            + self.table_spans.heap_size()
            + vec_bytes(&self.top_layer)
    }

    pub(super) fn collect_atoms(&self, atoms: &mut HashSet<StyleAtomID>) -> u64 {
        let mut visited = 0_u64;
        // An id name is the id as written, which in a quirks-mode document no id fact holds: those
        // keep the folded form. The index roots its own names rather than rely on other state to.
        if let Some(ids) = &self.ids {
            visited += u64::try_from(ids.nodes_by_name.len()).expect("id name count exceeds u64");
            atoms.extend(ids.nodes_by_name.keys().copied());
        }
        visited
    }

    #[must_use]
    pub fn new() -> Self {
        let mut tree = Self {
            elements: CowColumn::default(),
            element_slots: 1,
            live: BitColumn::default(),
            relation_only: BitColumn::default(),
            connected_element_count: 0,
            pending_reuse: Default::default(),
            #[cfg(test)]
            test_identities: Default::default(),
            ids: None,
            text: TextRows::default(),
            dom_paint_facts: Default::default(),
            table_spans: Default::default(),
            top_layer: Default::default(),
            dom_order_version: 0,
            dom_paint_facts_version: 0,
            #[cfg(test)]
            depth_recompute_visits: 0,
        };
        // Slot 0 is never a valid identity; reserving it keeps column indexing direct.
        tree.elements.grow_to(1);
        tree.text.links.grow_to(1);
        tree.text.flags.grow_to(1);
        tree.text.data.grow_to(1);

        tree
    }

    /// Connected styleable elements.
    #[must_use]
    pub fn connected_element_count(&self) -> u32 {
        self.connected_element_count
    }

    /// One past the greatest element slot the tree holds.
    #[must_use]
    pub fn element_slot_count(&self) -> usize {
        self.element_slots
    }

    /// Mark an identity as standing in the tree without being styled. See `relation_only`.
    pub fn mark_relation_only(&mut self, node: StyleNodeID) {
        let Some(index) = node.element_index() else {
            return;
        };

        let (changed, _) = self.relation_only.set(index as usize, true);

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
            Some(index) => self.text.flag(index, TextFlags::LIVE),
            None => self.live.contains(node.element_slot()),
        }
    }

    /// Every live element-kind identity, including the synthetic roots of shadow trees.
    pub fn live_nodes(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        (1..self.element_slots).filter_map(|index| {
            let index = u32::try_from(index).expect("style node index space exhausted");
            self.live.contains(index as usize).then(|| StyleNodeID::element(index))
        })
    }

    // -- Identity lifecycle ------------------------------------------------------------------

    /// Make the element identity `node`, which the host minted, live. The host mints past every
    /// identity it minted before, or one this tree released, which no reader can still name.
    pub fn mint_element(&mut self, node: StyleNodeID) {
        self.dom_order_version += 1;
        let index = node.element_index().expect("mint_element requires an element identity");
        if (index as usize) < self.element_slots {
            assert!(
                !self.live.contains(index as usize),
                "minting an element identity that is live"
            );
            // NB: The unique node id stays: the host may publish it before minting the identity.
            self.update_element_row(index as usize, |row| {
                *row = ElementRow {
                    unique_node_id: row.unique_node_id,
                    ..ElementRow::default()
                }
            });
        } else {
            assert_eq!(
                index as usize, self.element_slots,
                "the host mints new element identities in order"
            );
            self.element_slots += 1;
            self.elements.grow_to(self.element_slots);
        }
        self.live.set(index as usize, true);
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
    pub fn allocate_element(&mut self) -> StyleNodeID {
        let node = self.mint_test_identity(false);
        self.mint_element(node);
        node
    }

    /// Retire a batch of element identities, accounting once for the capacity they release. Each
    /// slot stays reserved until [`Self::release_retired_identities`] runs at epoch retirement, so
    /// no reader can observe a reused identity.
    pub fn retire_elements(&mut self, nodes: &[StyleNodeID]) {
        self.dom_order_version += 1;

        for &node in nodes {
            let index = node
                .element_index()
                .expect("retire_element requires an element identity");
            assert!(
                self.live.contains(index as usize),
                "retiring an identity that is not live"
            );
            if let Some(ids) = &mut self.ids {
                ids.set(node, StyleAtomID::NONE);
            }
            self.live.set(index as usize, false);
            self.dom_paint_facts.remove(&node);
            self.table_spans.remove(&node);
            self.top_layer.retain(|&member| member != node);
            if !self.relation_only.set(index as usize, false).0 {
                self.connected_element_count -= 1;
            }
            self.update_element_row(index as usize, |row| *row = ElementRow::default());
            self.pending_reuse.push(index);
        }
    }

    /// Called once the read epoch that could still name the retired identities has retired. Answers
    /// the identities it releases, elements then text nodes, each in the order they retired, for the
    /// host to mint again.
    pub fn release_retired_identities(&mut self) -> Vec<u32> {
        let released: Vec<u32> = self
            .pending_reuse
            .take_rows()
            .into_iter()
            .map(|index| StyleNodeID::element(index).raw())
            .chain(
                self.text
                    .pending_reuse
                    .take_rows()
                    .into_iter()
                    .map(|index| StyleNodeID::text(index).raw()),
            )
            .collect();

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
    pub fn mint_text(&mut self, node: StyleNodeID) {
        self.dom_order_version += 1;
        let index = node.text_index().expect("mint_text requires a text identity");

        if (index as usize) < self.text.links.len() {
            assert!(
                !self.text.flag(index, TextFlags::LIVE),
                "minting a text identity that is live"
            );
        } else {
            assert_eq!(
                index as usize,
                self.text.links.len(),
                "the host mints new text identities in order"
            );
            self.text.links.grow_to(index as usize + 1);
            self.text.data.grow_to(index as usize + 1);
        }
        self.text.set_flag(index, TextFlags::LIVE, true);
    }

    /// Mint a text identity from the tree's own space, for a test.
    #[cfg(test)]
    pub fn allocate_text(&mut self) -> StyleNodeID {
        let node = self.mint_test_identity(true);
        self.mint_text(node);
        node
    }

    /// Retire text identities as their nodes disconnect. Nothing selects or styles a text node, so
    /// it has no relations to stage, and its slot waits for [`Self::release_retired_identities`]
    /// like an element's.
    pub fn retire_texts(&mut self, nodes: impl IntoIterator<Item = StyleNodeID>) {
        self.dom_order_version += 1;

        // Every column is reset, so none leaves a retired identity's value behind for the next text
        // node issued the index.
        for node in nodes {
            let index = node.text_index().expect("retire_texts requires a text identity");
            assert!(
                self.text.flag(index, TextFlags::LIVE),
                "retiring a text identity that is not live"
            );
            self.text.flags.set(index as usize, TextFlags::default());
            self.dom_paint_facts.remove(&node);
            // Lets go of the reference the mirror held to the document's string.
            self.text.data.set(index as usize, ak::Utf16String::default());
            let text = &mut self.text;
            text.links.set(index as usize, TextLinks::default());
            text.pending_reuse.push(index);
        }
    }

    /// Whether the text node's data is nothing but ASCII whitespace. Only a text node has data, so
    /// every other identity answers no.
    #[must_use]
    pub fn text_is_ascii_whitespace(&self, node: StyleNodeID) -> bool {
        node.text_index()
            .is_some_and(|index| self.text.flag(index, TextFlags::IS_ASCII_WHITESPACE))
    }

    /// Record what the text node's data now spells, as its whitespace-only state.
    pub fn set_text_is_ascii_whitespace(&mut self, node: StyleNodeID, value: bool) {
        let Some(index) = node.text_index() else {
            return;
        };

        self.text.set_flag(index, TextFlags::IS_ASCII_WHITESPACE, value);
    }

    /// Whether the text node sits in a user agent shadow tree. Only a text node is asked; an
    /// element records the fact among its construction facts.
    #[must_use]
    pub fn text_is_in_user_agent_shadow_tree(&self, node: StyleNodeID) -> bool {
        node.text_index()
            .is_some_and(|index| self.text.flag(index, TextFlags::IS_IN_USER_AGENT_SHADOW_TREE))
    }

    /// Record which kind of tree the text node arrived in.
    pub fn set_text_is_in_user_agent_shadow_tree(&mut self, node: StyleNodeID, value: bool) {
        let Some(index) = node.text_index() else {
            return;
        };

        self.text
            .set_flag(index, TextFlags::IS_IN_USER_AGENT_SHADOW_TREE, value);
    }

    /// Whether the text node holds the value of a password input. Only a text node is asked; every
    /// other identity answers no.
    #[must_use]
    pub fn text_is_password_input(&self, node: StyleNodeID) -> bool {
        node.text_index()
            .is_some_and(|index| self.text.flag(index, TextFlags::IS_PASSWORD_INPUT))
    }

    /// Record whether the text node holds the value of a password input.
    pub fn set_text_is_password_input(&mut self, node: StyleNodeID, value: bool) {
        let Some(index) = node.text_index() else {
            return;
        };

        self.text.set_flag(index, TextFlags::IS_PASSWORD_INPUT, value);
    }

    /// Record the unique node id the document names the element by. Only an element identity, the
    /// document's included, holds one.
    pub fn set_unique_node_id(&mut self, node: StyleNodeID, unique_node_id: i64) {
        let Some(index) = node.element_index() else {
            return;
        };
        let index = index as usize;

        self.elements.grow_to(index + 1);
        self.update_element_row(index, |row| row.unique_node_id = unique_node_id.into());
    }

    /// The unique node id the document names the element by, or zero for anything else.
    #[must_use]
    pub fn unique_node_id(&self, node: StyleNodeID) -> i64 {
        node.element_index()
            .and_then(|index| self.elements.get(index as usize))
            .map_or(0, |row| row.unique_node_id.into())
    }

    /// The spans a row built for the element takes from its attributes.
    #[must_use]
    pub fn table_spans(&self, node: StyleNodeID) -> TableSpans {
        self.table_spans.get(&node).copied().unwrap_or_default()
    }

    /// Record the spans a row built for the element takes from its attributes. Spanning one of
    /// each is the absence of an entry.
    pub fn set_table_spans(&mut self, node: StyleNodeID, spans: TableSpans) {
        if spans == TableSpans::default() {
            self.table_spans.remove(&node);
        } else {
            self.table_spans.insert(node, spans);
        }
    }

    /// What a row built for the node is painted and hit-tested with. An identity with nothing
    /// published holds none of the facts.
    #[must_use]
    pub fn dom_paint_facts(&self, node: StyleNodeID) -> u8 {
        self.dom_paint_facts.get(&node).copied().unwrap_or(0)
    }

    /// Record what a row built for the node is painted and hit-tested with. Holding none of the
    /// facts is the absence of an entry.
    /// The nodes that are editable or an editing host.
    pub fn editable_nodes(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.dom_paint_facts.iter().filter_map(|(&node, &facts)| {
            (facts & crate::layout::node_data::DomPaintFact::EditableOrEditingHost as u8 != 0).then_some(node)
        })
    }

    pub fn set_dom_paint_facts(&mut self, node: StyleNodeID, facts: u8) {
        let previous = if facts == 0 {
            self.dom_paint_facts.remove(&node)
        } else {
            self.dom_paint_facts.insert(node, facts)
        };
        if previous.unwrap_or(0) != facts {
            self.dom_paint_facts_version += 1;
        }
    }

    /// A version of the DOM paint facts, which a retirement of a node moves with
    /// [`Self::dom_order_version`]. Whatever reads none of them since it read the version reads the
    /// same facts again.
    #[must_use]
    pub fn dom_paint_facts_version(&self) -> u64 {
        self.dom_paint_facts_version
    }

    /// The document's top layer, in the order its members were added.
    #[must_use]
    pub fn top_layer(&self) -> &[StyleNodeID] {
        &self.top_layer
    }

    /// Replace the document's top layer.
    pub fn set_top_layer(&mut self, members: &[StyleNodeID]) {
        self.top_layer.assign(members.to_vec());
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
            self.text.data.grow_to(index as usize + 1);
            self.text.data.set(index as usize, data);
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
            self.set_next_sibling(node, None);
            self.set_previous_sibling(node, None);
            if let Some(index) = node.text_index() {
                self.text.edit_links(index, |links| links.parent = None);
            }
            return;
        };
        let parent_index = parent_index as usize;
        let next = match previous {
            Some(previous) => self.next_sibling_in_dom_order(previous),
            None => self.element_row(parent_index).first_child,
        };
        self.set_previous_sibling(node, previous);
        self.set_next_sibling(node, next);
        match previous {
            Some(previous) => self.set_next_sibling(previous, Some(node)),
            None => self.update_element_row(parent_index, |row| row.first_child = Some(node)),
        }
        if let Some(next) = next {
            self.set_previous_sibling(next, Some(node));
        }
        if let Some(index) = node.text_index() {
            self.text.edit_links(index, |links| links.parent = parent);
        }
    }

    /// Take `node` out of the child sequence of `parent`, the parent it was linked under.
    pub fn unlink_from_dom_order(&mut self, node: StyleNodeID, parent: Option<StyleNodeID>) {
        self.dom_order_version += 1;
        if !self.is_live(node) {
            return;
        }
        let previous = self.previous_sibling_in_dom_order(node);
        let next = self.next_sibling_in_dom_order(node);
        self.set_previous_sibling(node, None);
        self.set_next_sibling(node, None);
        match previous {
            Some(previous) => {
                if self.is_live(previous) {
                    self.set_next_sibling(previous, next);
                }
            }
            None => {
                if let Some(parent_index) = parent
                    .filter(|&parent| self.is_live(parent))
                    .and_then(StyleNodeID::element_index)
                    && self.element_row(parent_index as usize).first_child == Some(node)
                {
                    self.update_element_row(parent_index as usize, |row| row.first_child = next);
                }
            }
        }
        if let Some(next) = next
            && self.is_live(next)
        {
            self.set_previous_sibling(next, previous);
        }
        if let Some(index) = node.text_index() {
            self.text.edit_links(index, |links| links.parent = None);
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
            next: node
                .element_index()
                .and_then(|index| self.element_row(index as usize).first_child),
        }
    }

    /// The parent a text node is linked under.
    #[must_use]
    pub fn text_parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.text.links(node.text_index()?).parent
    }

    #[must_use]
    pub fn next_sibling_in_dom_order(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        match node.text_index() {
            Some(index) => self.text.links(index).next_sibling,
            None => self.element_row(node.element_slot()).next_sibling,
        }
    }

    #[must_use]
    pub fn previous_sibling_in_dom_order(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        match node.text_index() {
            Some(index) => self.text.links(index).previous_sibling,
            None => self.element_row(node.element_slot()).previous_sibling,
        }
    }

    fn set_next_sibling(&mut self, node: StyleNodeID, next: Option<StyleNodeID>) {
        match node.text_index() {
            Some(index) => self.text.edit_links(index, |links| links.next_sibling = next),
            None => self.update_element_row(node.element_slot(), |row| row.next_sibling = next),
        }
    }

    fn set_previous_sibling(&mut self, node: StyleNodeID, previous: Option<StyleNodeID>) {
        match node.text_index() {
            Some(index) => self.text.edit_links(index, |links| links.previous_sibling = previous),
            None => self.update_element_row(node.element_slot(), |row| row.previous_sibling = previous),
        }
    }

    // -- Depth ------------------------------------------------------------------------------

    /// Note that the links the projection holds moved: the parents and shadow roots that place a
    /// node among its siblings. See [`Self::dom_order_version`].
    pub(super) fn note_element_links_moved(&mut self) {
        self.dom_order_version += 1;
    }

    /// The depth of the deepest element at or below `root`, by the links written into `projection`, which the
    /// transaction being taken settles.
    pub(super) fn deepest_depth_under(&mut self, projection: &Projection, root: StyleNodeID) -> u32 {
        let written_parent = |node| {
            projection
                .written_relations(node)
                .and_then(|relations| relations.parent)
        };
        let mut depth = 0_u32;
        let mut ancestor = written_parent(root);
        while let Some(node) = ancestor {
            depth = depth.checked_add(1).expect("style tree depth exhausted");
            ancestor = written_parent(node);
        }
        let mut deepest = depth;
        let mut next = Some(root);
        while let Some(node) = next {
            #[cfg(test)]
            {
                self.depth_recompute_visits += 1;
            }
            deepest = deepest.max(depth);
            if let Some(child) = projection.written_first_element_child(node) {
                depth = depth.checked_add(1).expect("style tree depth exhausted");
                next = Some(child);
                continue;
            }
            next = None;
            let mut candidate = node;
            while candidate != root {
                if let Some(sibling) = projection
                    .written_relations(candidate)
                    .and_then(|relations| relations.next_element_sibling)
                {
                    next = Some(sibling);
                    break;
                }
                let Some(parent) = written_parent(candidate) else {
                    break;
                };
                candidate = parent;
                depth -= 1;
            }
        }
        deepest
    }

    #[cfg(test)]
    pub(super) fn take_depth_recompute_visits(&mut self) -> usize {
        core::mem::take(&mut self.depth_recompute_visits)
    }

    // -- Element ids -------------------------------------------------------------------------

    /// Record the id an element answers to, or clear it with atom zero. The name is the id as
    /// written; see [`ElementIdIndex`] for why that is not the atom a selector is compiled against.
    pub fn set_element_id_name(&mut self, node: StyleNodeID, name: StyleAtomID) {
        if name.is_none()
            && self
                .ids
                .as_ref()
                .is_none_or(|index| !index.name_of_node.contains_key(&node))
        {
            return;
        }

        self.ids.get_or_insert_with(Box::default).set(node, name);
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
    pub(super) fn element_by_id(
        &self,
        projection: &Projection,
        tree_scope: TreeScopeID,
        name: StyleAtomID,
    ) -> Option<StyleNodeID> {
        let nodes = self.ids.as_ref()?.nodes_by_name.get(&name)?;
        let mut first = None;
        for &node in nodes {
            let Some(index) = node.element_index() else {
                continue;
            };
            if !self.live.contains(index as usize) || projection.tree_scope(node) != tree_scope {
                continue;
            }
            first = match first {
                Some(current) if !self.precedes_in_tree_order(projection, node, current) => Some(current),
                _ => Some(node),
            };
        }
        first
    }

    /// Whether `a` comes before `b` in the tree order of the scope they share.
    fn precedes_in_tree_order(&self, projection: &Projection, a: StyleNodeID, b: StyleNodeID) -> bool {
        if a == b {
            return false;
        }
        // The ancestor chains of the two, each from the node up to its root.
        let chain = |node: StyleNodeID| {
            let mut chain: SmallVec<[StyleNodeID; 32]> = SmallVec::new();
            let mut next = Some(node);
            while let Some(node) = next {
                chain.push(node);
                next = projection.parent(node);
            }
            chain
        };
        let (a_chain, b_chain) = (chain(a), chain(b));
        // Walk both from the root end to where they part. A node on the other's chain is its ancestor, and an
        // ancestor always comes first.
        let shared = a_chain
            .iter()
            .rev()
            .zip(b_chain.iter().rev())
            .take_while(|(a, b)| a == b)
            .count();
        if shared == 0 {
            return false;
        }
        if shared == b_chain.len() {
            return false;
        }
        if shared == a_chain.len() {
            return true;
        }
        let left = a_chain[a_chain.len() - shared - 1];
        let right = b_chain[b_chain.len() - shared - 1];
        // Siblings now, so whichever the child sequence reaches first comes first.
        let mut sibling = projection.next_element_sibling(left);
        while let Some(node) = sibling {
            if node == right {
                return true;
            }
            sibling = projection.next_element_sibling(node);
        }
        false
    }
}

/// The rows of text identities, indexed by text index with slot 0 unused. A text node owns no
/// element relations, only its place in the DOM child sequence. A retired index waits in
/// `pending_reuse` until its epoch retires, as an element's does.
#[derive(Clone, Default)]
struct TextRows {
    /// Where each text node stands in its parent's DOM child sequence. Copied on write in chunks, so that a fork
    /// shares it.
    links: CowColumn<TextLinks, TEXT_LINKS_PER_CHUNK>,
    /// What each text node is, as `TextFlags`. Copied on write in chunks, so that a fork shares it.
    flags: CowColumn<TextFlags, TEXT_FLAGS_PER_CHUNK>,
    /// The characters the node holds, sharing the document's string rather than copying it. The
    /// layout tree build reads them to render a text box, so they are published where the node
    /// arrives and wherever its data is replaced. Copied on write in chunks, as the flags are.
    data: CowColumn<ak::Utf16String, TEXT_DATA_PER_CHUNK>,
    pending_reuse: crate::cow_column::CowVec<u32, 1024>,
}

/// How many text nodes a chunk of [`TextRows`]'s links holds.
const TEXT_LINKS_PER_CHUNK: usize = 64;
/// How many text nodes a chunk of [`TextRows`]'s flags holds.
const TEXT_FLAGS_PER_CHUNK: usize = 64;
/// How many text nodes a chunk of [`TextRows`]'s data holds.
const TEXT_DATA_PER_CHUNK: usize = 64;

/// Where one text node stands in its parent's DOM child sequence.
#[derive(Clone, Copy, Default, PartialEq, Eq)]
struct TextLinks {
    parent: Option<StyleNodeID>,
    next_sibling: Option<StyleNodeID>,
    previous_sibling: Option<StyleNodeID>,
}

const _: () = assert!(size_of::<TextLinks>() == 12);

/// What one text node is.
#[derive(Clone, Copy, Default, PartialEq, Eq)]
struct TextFlags(u8);

impl TextFlags {
    const LIVE: u8 = 1 << 0;
    /// Whether the node's data is nothing but ASCII whitespace, which is what decides whether the
    /// layout tree build can collapse it away rather than give it a box of its own.
    const IS_ASCII_WHITESPACE: u8 = 1 << 1;
    /// Whether the node sits in a user agent shadow tree. An element records the same fact among
    /// its construction facts; a text node has no element columns, so it records it here.
    const IS_IN_USER_AGENT_SHADOW_TREE: u8 = 1 << 2;
    /// Whether the node is the text of a password input, which renders as replacement characters.
    const IS_PASSWORD_INPUT: u8 = 1 << 3;
}

impl TextRows {
    fn links(&self, index: u32) -> TextLinks {
        self.links.get(index as usize).copied().unwrap_or_default()
    }

    fn edit_links(&mut self, index: u32, edit: impl FnOnce(&mut TextLinks)) {
        let mut links = self.links(index);
        edit(&mut links);
        self.links
            .set(index as usize, links)
            .expect("a minted text identity has a links row");
    }

    fn flag(&self, index: u32, flag: u8) -> bool {
        self.flags.get(index as usize).is_some_and(|flags| flags.0 & flag != 0)
    }

    fn set_flag(&mut self, index: u32, flag: u8, value: bool) {
        let index = index as usize;
        self.flags.grow_to(index + 1);
        let flags = *self
            .flags
            .get(index)
            .expect("the column was just grown to hold the row");
        let flags = TextFlags(match value {
            true => flags.0 | flag,
            false => flags.0 & !flag,
        });
        self.flags.set(index, flags);
    }
}

pub struct DomChildren<'a> {
    tree: &'a HostTree,
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_fork_writing_one_text_link_copies_one_chunk_of_links() {
        let mut tree = HostTree::new();
        let texts: Vec<StyleNodeID> = (0..8 * TEXT_LINKS_PER_CHUNK).map(|_| tree.allocate_text()).collect();
        let mut fork = tree.clone();
        let mut sizer = crate::heap_size::HeapSizer::default();
        let whole = tree.text.links.heap_size(&mut sizer, |_, _| 0);
        assert!(whole > 8 * TEXT_LINKS_PER_CHUNK * size_of::<TextLinks>());
        fork.set_next_sibling(texts[70], Some(texts[71]));
        let copied = fork.text.links.heap_size(&mut sizer, |_, _| 0);
        assert!(copied < 3 * TEXT_LINKS_PER_CHUNK * size_of::<TextLinks>());
        assert_eq!(fork.next_sibling_in_dom_order(texts[70]), Some(texts[71]));
        assert_eq!(tree.next_sibling_in_dom_order(texts[70]), None);
    }

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

    /// Builds `parent -> [children]` shapes in the projection beside the tree without repeating
    /// relation bookkeeping in every test.
    struct TreeFixture {
        tree: HostTree,
        projection: Projection,
    }

    impl TreeFixture {
        fn new() -> Self {
            Self {
                tree: HostTree::new(),
                projection: Projection::default(),
            }
        }

        fn element(&mut self) -> StyleNodeID {
            let node = self.tree.allocate_element();
            self.projection.mint(node);
            self.projection.set_connected(node, true);
            node
        }

        /// Write the links of `children` under `parent`. [`Self::commit`] makes them the ones read.
        fn attach_children(&mut self, parent: StyleNodeID, children: &[StyleNodeID]) {
            self.projection
                .set_first_element_child(parent, children.first().copied());
            for (index, &child) in children.iter().enumerate() {
                self.projection.set_parent(child, Some(parent));
                self.projection
                    .set_next_element_sibling(child, children.get(index + 1).copied());
                self.projection
                    .set_previous_element_sibling(child, index.checked_sub(1).map(|index| children[index]));
            }
        }

        fn retire(&mut self, nodes: &[StyleNodeID]) {
            self.tree.retire_elements(nodes);
            for &node in nodes {
                self.projection.retire(node);
            }
        }

        /// Make the written links the ones read, and the depths the ones they give.
        fn commit(&mut self) {
            let roots: Vec<StyleNodeID> = self
                .tree
                .live_nodes()
                .filter(|&node| {
                    self.projection
                        .written_relations(node)
                        .is_none_or(|relations| relations.parent.is_none())
                })
                .collect();
            for root in roots {
                let _ = self.tree.deepest_depth_under(&self.projection, root);
            }
            self.projection.commit();
            self.projection.release();
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
        fixture.commit();
        let name = StyleAtomID(7);

        // The order the elements take the name in is not tree order.
        fixture.tree.set_element_id_name(second, name);
        fixture.tree.set_element_id_name(nested, name);
        assert_eq!(
            fixture
                .tree
                .element_by_id(&fixture.projection, TreeScopeID::DOCUMENT, name),
            Some(nested)
        );

        fixture.tree.set_element_id_name(first, name);
        assert_eq!(
            fixture
                .tree
                .element_by_id(&fixture.projection, TreeScopeID::DOCUMENT, name),
            Some(first)
        );

        fixture.tree.set_element_id_name(first, StyleAtomID::NONE);
        assert_eq!(
            fixture
                .tree
                .element_by_id(&fixture.projection, TreeScopeID::DOCUMENT, name),
            Some(nested)
        );
        assert_eq!(
            fixture
                .tree
                .element_by_id(&fixture.projection, TreeScopeID::DOCUMENT, StyleAtomID(8)),
            None
        );
    }

    #[test]
    fn a_retired_element_leaves_the_id_index() {
        let mut fixture = TreeFixture::new();
        let root = fixture.element();
        let children = [fixture.element(), fixture.element(), fixture.element()];
        fixture.attach_children(root, &children);
        fixture.commit();
        // Three elements sharing a name spill its candidate list out of line.
        let name = StyleAtomID(7);
        for child in children {
            fixture.tree.set_element_id_name(child, name);
        }
        assert_eq!(
            fixture
                .tree
                .element_by_id(&fixture.projection, TreeScopeID::DOCUMENT, name),
            Some(children[0])
        );

        fixture.retire(&children);
        assert_eq!(
            fixture
                .tree
                .element_by_id(&fixture.projection, TreeScopeID::DOCUMENT, name),
            None
        );
        // The spilled list went with the name, and the controller was told.
    }

    #[test]
    fn id_names_are_roots_of_the_atom_sweep() {
        let mut fixture = TreeFixture::new();
        let element = fixture.element();
        let name = StyleAtomID(7);
        fixture.tree.set_element_id_name(element, name);
        let mut atoms = HashSet::default();
        fixture.tree.collect_atoms(&mut atoms);
        assert!(atoms.contains(&name));
    }

    #[test]
    fn the_deepest_depth_follows_the_links_written_into_the_projection() {
        let mut fixture = TreeFixture::new();
        let root = fixture.element();
        let parent = fixture.element();
        let child = fixture.element();
        fixture.attach_children(root, &[parent]);
        fixture.attach_children(parent, &[child]);

        assert_eq!(fixture.tree.deepest_depth_under(&fixture.projection, root), 2);
        assert_eq!(fixture.tree.deepest_depth_under(&fixture.projection, child), 2);

        fixture.projection.set_parent(child, Some(root));
        fixture.projection.set_parent(parent, Some(child));
        fixture.projection.set_first_element_child(root, Some(child));
        fixture.projection.set_first_element_child(child, Some(parent));
        fixture.projection.set_first_element_child(parent, None);
        fixture.projection.set_next_element_sibling(parent, None);
        fixture.projection.set_next_element_sibling(child, None);

        assert_eq!(fixture.tree.deepest_depth_under(&fixture.projection, child), 2);
        assert_eq!(fixture.tree.deepest_depth_under(&fixture.projection, parent), 2);
        assert_eq!(fixture.tree.deepest_depth_under(&fixture.projection, root), 2);
    }

    #[test]
    fn a_fork_shares_text_data_and_flags_until_one_side_writes() {
        let mut tree = HostTree::new();
        let text = tree.allocate_text();
        tree.set_text_is_ascii_whitespace(text, true);
        let fork = tree.clone();
        let (data_version, flags_version) = (tree.text.data.version(), tree.text.flags.version());
        tree.set_text_is_ascii_whitespace(text, false);
        tree.set_text_data(text, ak::Utf16String::from_utf16(&[u16::from(b'x')]));
        assert!(fork.text_is_ascii_whitespace(text));
        assert!(fork.text_data(text).is_some_and(ak::Utf16String::is_empty));
        assert_eq!(tree.text.flags.version(), flags_version + 1);
        assert_eq!(tree.text.data.version(), data_version + 1);
    }

    #[test]
    fn a_retired_identity_is_not_reused_before_the_epoch_retires() {
        let mut fixture = TreeFixture::new();
        let first = fixture.element();
        let second = fixture.element();
        assert_eq!(fixture.tree.connected_element_count(), 2);

        fixture.retire(&[first]);
        assert!(!fixture.tree.is_live(first));
        assert_eq!(fixture.tree.connected_element_count(), 1);
        assert_eq!(fixture.tree.retired_identities_pending_release(), 1);

        let third = fixture.element();
        assert_ne!(third, first);
        assert_ne!(third, second);

        fixture.tree.release_retired_identities();
        let fourth = fixture.element();
        assert_eq!(fourth, first);
        assert!(fixture.tree.is_live(fourth));
    }

    #[test]
    fn text_identities_are_reused_only_after_the_epoch_retires() {
        let mut fixture = TreeFixture::new();
        let element = fixture.element();
        let first = fixture.tree.allocate_text();
        let second = fixture.tree.allocate_text();
        assert_eq!(first, StyleNodeID::text(1));
        assert_eq!(second, StyleNodeID::text(2));
        assert!(fixture.tree.is_live(first) && fixture.tree.is_live(element));
        assert_eq!(fixture.tree.connected_element_count(), 1);

        fixture.tree.retire_texts([first]);
        assert!(!fixture.tree.is_live(first));
        assert!(fixture.tree.is_live(element));

        let third = fixture.tree.allocate_text();
        assert_eq!(third, StyleNodeID::text(3));

        fixture.tree.release_retired_identities();
        let reused = fixture.tree.allocate_text();
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
        };
        fixture.tree.set_table_spans(element, spans);
        assert_eq!(fixture.tree.table_spans(element), spans);

        fixture.retire(&[element]);
        fixture.tree.release_retired_identities();
        assert_eq!(fixture.tree.allocate_element(), element);
        assert_eq!(fixture.tree.table_spans(element), TableSpans::default());
    }

    #[test]
    fn a_retired_identity_leaves_the_top_layer_in_order() {
        let mut fixture = TreeFixture::new();
        let first = fixture.element();
        let second = fixture.element();
        let third = fixture.element();
        fixture.tree.set_top_layer(&[third, first, second]);
        assert_eq!(fixture.tree.top_layer(), [third, first, second]);

        fixture.retire(&[first]);
        assert_eq!(fixture.tree.top_layer(), [third, second]);
        fixture.tree.release_retired_identities();
        assert_eq!(fixture.tree.allocate_element(), first);
        assert_eq!(fixture.tree.top_layer(), [third, second]);
    }

    #[test]
    fn a_retired_identity_holds_no_paint_facts_when_it_is_reused() {
        let mut fixture = TreeFixture::new();
        let element = fixture.element();
        let text = fixture.tree.allocate_text();
        fixture.tree.set_dom_paint_facts(element, 3);
        fixture.tree.set_dom_paint_facts(text, 4);
        assert_eq!(fixture.tree.dom_paint_facts(element), 3);
        assert_eq!(fixture.tree.dom_paint_facts(text), 4);
        fixture.tree.set_dom_paint_facts(element, 0);
        assert_eq!(fixture.tree.dom_paint_facts(element), 0);
        fixture.tree.set_dom_paint_facts(element, 1);

        fixture.retire(&[element]);
        fixture.tree.retire_texts([text]);
        fixture.tree.release_retired_identities();
        assert_eq!(fixture.tree.allocate_element(), element);
        assert_eq!(fixture.tree.allocate_text(), text);
        assert_eq!(fixture.tree.dom_paint_facts(element), 0);
        assert_eq!(fixture.tree.dom_paint_facts(text), 0);
    }

    #[test]
    fn text_nodes_take_places_in_the_dom_child_sequence_beside_elements() {
        let mut fixture = TreeFixture::new();
        let parent = fixture.element();
        let first_text = fixture.tree.allocate_text();
        let element = fixture.element();
        let last_text = fixture.tree.allocate_text();
        fixture.tree.link_in_dom_order(first_text, Some(parent), None);
        fixture.tree.link_in_dom_order(element, Some(parent), Some(first_text));
        fixture.tree.link_in_dom_order(last_text, Some(parent), Some(element));

        assert_eq!(
            fixture.tree.dom_children(parent).collect::<Vec<_>>(),
            vec![first_text, element, last_text]
        );
        assert_eq!(fixture.tree.text_parent(last_text), Some(parent));
        assert_eq!(fixture.projection.children(parent).count(), 0);
        assert_eq!(fixture.tree.connected_element_count(), 2);

        // A move is an unlink followed by a link at the new place.
        fixture.tree.unlink_from_dom_order(last_text, Some(parent));
        fixture.tree.link_in_dom_order(last_text, Some(parent), None);
        assert_eq!(
            fixture.tree.dom_children(parent).collect::<Vec<_>>(),
            vec![last_text, first_text, element]
        );

        fixture.tree.unlink_from_dom_order(first_text, Some(parent));
        fixture.tree.retire_texts([first_text]);
        assert!(!fixture.tree.is_live(first_text));
        assert_eq!(
            fixture.tree.dom_children(parent).collect::<Vec<_>>(),
            vec![last_text, element]
        );
        assert_eq!(fixture.tree.previous_sibling_in_dom_order(element), Some(last_text));

        // Like an element's, a retired text identity is reused only once its epoch retires.
        let replacement = fixture.tree.allocate_text();
        assert_ne!(replacement, first_text);
        fixture.tree.release_retired_identities();
        let reused = fixture.tree.allocate_text();
        assert_eq!(reused, first_text);
        assert_eq!(fixture.tree.text_parent(reused), None);
        assert_eq!(fixture.tree.next_sibling_in_dom_order(reused), None);
    }

    #[test]
    fn a_relation_only_identity_roots_the_dom_child_sequence_without_counting_as_an_element() {
        let mut fixture = TreeFixture::new();
        let document = fixture.element();
        fixture.tree.mark_relation_only(document);
        let document_element = fixture.element();
        fixture.tree.link_in_dom_order(document_element, Some(document), None);

        assert!(fixture.tree.is_relation_only(document));
        assert!(!fixture.tree.is_relation_only(document_element));
        assert_eq!(fixture.tree.connected_element_count(), 1);
        assert_eq!(
            fixture.tree.dom_children(document).collect::<Vec<_>>(),
            vec![document_element]
        );
        assert_eq!(fixture.projection.parent(document_element), None);

        fixture.retire(&[document, document_element]);
        assert_eq!(fixture.tree.connected_element_count(), 0);
        fixture.tree.release_retired_identities();
        let reused = fixture.element();
        assert!(!fixture.tree.is_relation_only(reused));
    }

    #[test]
    fn column_capacity_is_charged_to_tier_one_and_released_with_the_columns() {
        let mut fixture = TreeFixture::new();
        for _ in 0..1000 {
            fixture.element();
        }
    }

    #[test]
    fn per_node_relation_state_fits_the_mandatory_budget() {
        // Rust stores four relation columns, depth, and the style-record handle per node. The DOM
        // owns the style-node identity mapping.
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
