/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The plan of a fresh layout tree: which boxes the styled DOM makes, how they nest, and which
//! anonymous boxes wrap them, decided without touching the arena or the style engine.
//!
//! A build plans a subtree reading the style mirror and nothing else: it writes the row of each box
//! where the arena will keep it, in chunks the build owns, and links the rows by their slots as it
//! places them. Its child lists are built by the insertion rules of [`child_list`]. The arena then
//! takes the chunks over as they are.

pub(crate) mod child_list;
pub(crate) mod content_items;
pub(crate) mod counter_walk;
pub(crate) mod planner;
pub(crate) mod style_summary;
pub(crate) mod table_fixup;

use crate::css::style::tree::StyleNodeID;
use crate::layout::FfiDisplay;
use crate::layout::layout_node_arena::SLOTS_PER_CHUNK;

/// How many of the free slots of the arena a build claims at most at once. A walk claims one first, and twice as many
/// as it claimed last each time after, up to this.
// OPTIMIZATION: A walk that makes a few rows then claims a few slots, and sets up what finds their rows for those alone.
const FREE_SLOTS_PER_CLAIM: usize = 64;
use crate::layout::node_data::{NodeData, NodeFlag, NodeKind};
use libcompositing_rust::node_slot_id::NodeSlotId;

/// What the style of a planned box says about where it goes in its parent's child list, and where
/// its own children go. One bit per question the insertion rules ask of a box's style.
pub(crate) mod box_fact {
    /// Text, or a box whose outer display type is inline.
    pub(crate) const INLINE_OUTSIDE: u32 = 1 << 0;
    pub(crate) const FLOW_INSIDE: u32 = 1 << 1;
    pub(crate) const FLEX_OR_GRID_INSIDE: u32 = 1 << 2;
    pub(crate) const MATH_INSIDE: u32 = 1 << 3;
    pub(crate) const TABLE_INSIDE: u32 = 1 << 4;
    /// The display makes the box a tabular container: a table, a row group or a row.
    pub(crate) const TABULAR_CONTAINER: u32 = 1 << 5;
    pub(crate) const FLOATING: u32 = 1 << 6;
    pub(crate) const ABSOLUTELY_POSITIONED: u32 = 1 << 7;
    /// The display before blockification has an inline outer display type.
    pub(crate) const INLINE_OUTSIDE_BEFORE_BLOCKIFICATION: u32 = 1 << 8;
    /// The display is table-internal or a caption.
    pub(crate) const TABLE_NON_ROOT: u32 = 1 << 9;
    /// The display before blockification is table-internal or a caption.
    pub(crate) const TABLE_NON_ROOT_BEFORE_BLOCKIFICATION: u32 = 1 << 10;
    /// The box has a style of its own, which a text node lacks.
    pub(crate) const HAS_STYLE: u32 = 1 << 12;
    pub(crate) const LIST_STYLE_POSITION_INSIDE: u32 = 1 << 13;
    /// The children of the box are its flex items.
    pub(crate) const ESTABLISHES_FLEX_ITEMS: u32 = 1 << 14;
    /// The children of the box are its grid items.
    pub(crate) const ESTABLISHES_GRID_ITEMS: u32 = 1 << 15;
    /// The row is made by the builder's own steps for an element's box, rather than from the planned facts: its kind is
    /// one the element asks for, or the build adjusts its display.
    pub(crate) const STAMPED_BY_BUILDER: u32 = 1 << 16;
    /// The box replaces its element's contents with an image, whose provider it owns.
    pub(crate) const OWNS_CONTENT_REPLACEMENT_IMAGE: u32 = 1 << 17;
    /// The element published a style for a highlight pseudo-element, which its box notes.
    pub(crate) const HOLDS_HIGHLIGHT_STYLE: u32 = 1 << 18;
    /// A text whose parent element published a style for a highlight pseudo-element, which the text's box notes where
    /// the element has no box of its own.
    pub(crate) const PARENT_HOLDS_HIGHLIGHT_STYLE: u32 = 1 << 19;
    /// The row's ancestor facts and flex or grid item flags are derived as the commit attaches it, rather than by the
    /// build: its parent's style is one the commit gives it, or it is a pseudo-element's.
    pub(crate) const DERIVED_ON_COMMIT: u32 = 1 << 20;
    /// The children of the row are derived as the commit attaches them, see [`DERIVED_ON_COMMIT`].
    pub(crate) const CHILDREN_DERIVED_ON_COMMIT: u32 = 1 << 21;
    /// The row is derived as the commit attaches it, and its parent is not: the commit derives its subtree from it.
    pub(crate) const SUBTREE_DERIVED_ON_COMMIT: u32 = 1 << 22;
    /// The children of the row are derived as the commit attaches them whatever its parent: it is a `display: contents`
    /// text's style wrapper, or a replaced element's box the builder's own steps stamp.
    pub(crate) const OWN_CHILDREN_DERIVED_ON_COMMIT: u32 = 1 << 23;
    /// An anonymous box among the items of a flex container, which is a flex item once it holds a box that is not text.
    pub(crate) const FLEX_ITEM_UNLESS_ONLY_TEXT: u32 = 1 << 24;
    /// As [`FLEX_ITEM_UNLESS_ONLY_TEXT`], among the items of a grid container.
    pub(crate) const GRID_ITEM_UNLESS_ONLY_TEXT: u32 = 1 << 25;
    /// Some child of the box is inline-level or in flow.
    pub(crate) const HAS_INLINE_OR_IN_FLOW_CHILDREN: u32 = 1 << 26;
    /// Some child of the box is block-level and in flow.
    pub(crate) const HAS_IN_FLOW_BLOCK_CHILDREN: u32 = 1 << 27;
    /// The walk is in the child list of the box, see [`super::PlanTree::open_parent`].
    pub(crate) const OPEN_PARENT: u32 = 1 << 28;
    /// Some box in the child list of the box, or in that of an anonymous box in it, is a part of a table, which table
    /// fixup reads, see [`super::PlanTree::table_fixup_lists`].
    pub(crate) const HAS_TABLE_PART_CHILD: u32 = 1 << 29;
    /// The facts that make a box a part of a table that table fixup reads among its siblings.
    pub(crate) const TABLE_PART: u32 = TABLE_INSIDE | TABLE_NON_ROOT | TABLE_NON_ROOT_BEFORE_BLOCKIFICATION;
    /// The facts a box's children set.
    pub(crate) const OF_CHILDREN: u32 = HAS_INLINE_OR_IN_FLOW_CHILDREN | HAS_IN_FLOW_BLOCK_CHILDREN;
    /// The facts the display decides, as [`super::box_facts_of_display`] answers them.
    pub(crate) const OF_DISPLAY: u32 = INLINE_OUTSIDE
        | FLOW_INSIDE
        | FLEX_OR_GRID_INSIDE
        | MATH_INSIDE
        | TABLE_INSIDE
        | TABULAR_CONTAINER
        | TABLE_NON_ROOT;
}

/// A box the planner makes: what its row is stamped with at first, and the facts the insertion rules read of it.
#[derive(Clone, Copy, Debug)]
pub(crate) struct PlanNode {
    /// The flags the row is built with: construction flags, and the ones the insertion rules set.
    pub(crate) flags: u32,
    /// See [`box_fact`].
    pub(crate) facts: u32,
    pub(crate) kind: NodeKind,
    pub(crate) generated_for: u8,
    /// The style of the record an element published, which its row is stamped with.
    pub(crate) style: Option<crate::layout::node_data::LayoutStyleRef>,
}

impl PlanNode {
    pub(crate) fn new(kind: NodeKind, flags: u32, facts: u32) -> Self {
        Self {
            flags,
            facts,
            kind,
            generated_for: 0,
            style: None,
        }
    }
}

/// What the commit gives a row it commits one by one, besides what the walk wrote: see
/// [`PlanTree::rows_committed_one_by_one`].
#[derive(Clone, Copy, Debug)]
pub(crate) enum OneByOneRowSource {
    /// The principal box of an element, with the record it published, whose facts are `style_facts` (see
    /// `planned_style_facts_for_kind`).
    Element {
        element: StyleNodeID,
        record: u64,
        style_facts: u32,
    },
    /// The box of a text node.
    Text(StyleNodeID),
    /// The box of a pseudo-element of an element, which the walk decided as `decision`.
    PseudoElement {
        element: StyleNodeID,
        generated_for: u8,
        decision: crate::layout::tree_builder::PseudoElementDecision,
    },
    /// The anonymous inline box a text node under a `display: contents` element renders in, which takes its style from
    /// `record`.
    DisplayContentsTextWrapper { text: StyleNodeID, record: u64 },
    /// An anonymous block container, which takes its style from `style_parent`, the box it was made below: a wrapper
    /// a button's or a fieldset's contents go into later keeps the style it would have had there.
    AnonymousWrapper { style_parent: NodeSlotId },
    /// An anonymous box a button's or a fieldset's layout asks for around its contents, which takes its style from
    /// `style_parent`, the box of the button or the fieldset, as `style_kind` says.
    ContentsWrapper {
        style_parent: NodeSlotId,
        style_kind: crate::css::style::layout_style::AnonymousStyleKind,
    },
    /// An anonymous table box of `kind` table fixup made around boxes of the child list of `style_parent`, which it
    /// takes its style from.
    AnonymousTableBox {
        style_parent: NodeSlotId,
        kind: table_fixup::AnonymousTableBoxKind,
    },
    /// The table wrapper box of `table`, which takes the table's outer properties: the commit gives the table's box
    /// what is left of its style.
    TableWrapper { table: NodeSlotId },
    /// An image the generated content of the pseudo-element of kind `generated_for` of `generator` shows, as an inline
    /// box that takes its style from `style_box`, the pseudo-element's box or the list marker it nests.
    GeneratedImage {
        generator: StyleNodeID,
        generated_for: u8,
        style_box: NodeSlotId,
        image: GeneratedImage,
    },
    /// The list marker the list-item pseudo-element of kind `generated_for` of `generator` nests.
    NestedListMarker { generator: StyleNodeID, generated_for: u8 },
}

/// Which image a row of generated content shows, see [`OneByOneRowSource::GeneratedImage`].
#[derive(Clone, Copy, Debug)]
pub(crate) enum GeneratedImage {
    /// The `<image>` at this index of the `content` list.
    Content(u32),
    /// The `list-style-image` of the list marker box.
    ListStyleImage,
}

/// A row the commit gives what the lists of its plan do not, one by one.
#[derive(Clone, Copy, Debug)]
pub(crate) struct RowCommittedOneByOne {
    pub(crate) slot: NodeSlotId,
    /// See [`box_fact`].
    pub(crate) facts: u32,
    pub(crate) source: OneByOneRowSource,
}

/// A planned box: its row, as the build has written it so far, and its facts.
#[derive(Clone, Copy)]
pub(crate) struct PlannedBox<'a> {
    row: &'a NodeData,
    facts: u32,
}

impl<'a> PlannedBox<'a> {
    pub(crate) fn parent(&self) -> NodeSlotId {
        self.row.parent.get()
    }

    pub(crate) fn first_child(&self) -> NodeSlotId {
        self.row.first_child.get()
    }

    pub(crate) fn last_child(&self) -> NodeSlotId {
        self.row.last_child.get()
    }

    pub(crate) fn previous_sibling(&self) -> NodeSlotId {
        self.row.previous_sibling.get()
    }

    pub(crate) fn next_sibling(&self) -> NodeSlotId {
        self.row.next_sibling.get()
    }

    pub(crate) fn kind(&self) -> NodeKind {
        self.row.kind.get()
    }

    /// The row, as the build has written it so far.
    pub(crate) fn row(&self) -> &'a NodeData {
        self.row
    }

    pub(crate) fn ancestor_facts(&self) -> u8 {
        self.row.ancestor_facts
    }

    pub(crate) fn flags(&self) -> u32 {
        self.row.flags.get()
    }

    pub(crate) fn generated_for(&self) -> u8 {
        self.row.generated_for.get()
    }

    pub(crate) fn facts(&self) -> u32 {
        self.facts
    }

    pub(crate) fn has_flag(&self, flag: NodeFlag) -> bool {
        self.flags() & flag as u32 != 0
    }

    pub(crate) fn has_fact(&self, fact: u32) -> bool {
        self.facts & fact != 0
    }

    /// Whether the box is floated or absolutely positioned, which a float that is a flex or grid item is not.
    /// https://drafts.csswg.org/css-flexbox-1/#flex-items
    pub(crate) fn is_out_of_flow(&self) -> bool {
        self.has_fact(box_fact::HAS_STYLE)
            && ((self.has_fact(box_fact::FLOATING) && !self.has_flag(NodeFlag::IsFlexItem))
                || self.has_fact(box_fact::ABSOLUTELY_POSITIONED))
    }

    /// Whether the box's display, as table fixup reads it, is table-internal or a caption.
    pub(crate) fn is_table_non_root_for_table_fixup(&self) -> bool {
        if !self.is_out_of_flow()
            || self.has_flag(NodeFlag::IsReplacedElement)
            || self.has_flag(NodeFlag::Anonymous)
            || self.has_flag(NodeFlag::UsesButtonLayout)
        {
            self.has_fact(box_fact::TABLE_NON_ROOT)
        } else {
            self.has_fact(box_fact::TABLE_NON_ROOT_BEFORE_BLOCKIFICATION)
        }
    }
}

/// The facts of a box whose style shows `display`, floated and positioned as `style` says.
pub(crate) fn box_facts_of_style(style: crate::layout::ComputedValuesView<'_>) -> u32 {
    let display = style.display();
    let before_blockification = style.display_before_box_type_transformation();
    let mut facts = box_facts_of_display(display) | box_fact::HAS_STYLE;
    if style.is_floating() {
        facts |= box_fact::FLOATING;
    }
    if style.is_absolutely_positioned() {
        facts |= box_fact::ABSOLUTELY_POSITIONED;
    }
    if before_blockification.is_inline_outside() {
        facts |= box_fact::INLINE_OUTSIDE_BEFORE_BLOCKIFICATION;
    }
    if before_blockification.is_internal_table() || before_blockification.is_table_caption() {
        facts |= box_fact::TABLE_NON_ROOT_BEFORE_BLOCKIFICATION;
    }
    if style.list_style_position_is_inside() {
        facts |= box_fact::LIST_STYLE_POSITION_INSIDE;
    }
    facts
}

/// The facts `display` alone decides.
pub(crate) fn box_facts_of_display(display: FfiDisplay) -> u32 {
    let mut facts = 0;
    if display.is_inline_outside() {
        facts |= box_fact::INLINE_OUTSIDE;
    }
    if display.is_flow_inside() {
        facts |= box_fact::FLOW_INSIDE;
    }
    if display.is_flex_inside() || display.is_grid_inside() {
        facts |= box_fact::FLEX_OR_GRID_INSIDE;
    }
    if display.is_math_inside() {
        facts |= box_fact::MATH_INSIDE;
    }
    if display.is_table_inside() {
        facts |= box_fact::TABLE_INSIDE;
    }
    if display.is_table_inside()
        || display.is_table_row()
        || display.is_table_row_group()
        || display.is_table_header_group()
        || display.is_table_footer_group()
    {
        facts |= box_fact::TABULAR_CONTAINER;
    }
    if display.is_internal_table() || display.is_table_caption() {
        facts |= box_fact::TABLE_NON_ROOT;
    }
    facts
}

/// The facts of an anonymous block container the insertion rules make around inline-level boxes.
pub(crate) fn anonymous_wrapper_facts() -> u32 {
    box_facts_of_display(FfiDisplay::block()) | box_fact::HAS_STYLE
}

/// The slots of a fresh row, as a build hands them out: every chunk it fills is new, so every row in it is in its
/// first generation.
pub(crate) const FRESH_ROW_GENERATION: u8 = 1;

/// Hands out to the builds that fill them the free slots the arena gave a build, then the chunks builds stopped filling
/// with slots left, and then the chunks to come after the arena's, in the order the builds ask.
#[derive(Clone)]
pub(crate) struct ChunkClaims(std::sync::Arc<ChunkClaimsState>);

struct ChunkClaimsState {
    /// The first chunk builds claim, the first after the arena's.
    first_chunk: u32,
    next_chunk: std::sync::atomic::AtomicU32,
    /// Free list entries of the arena, see `layout_node_arena::free_slot_entry`.
    free_slots: std::sync::Mutex<crate::fork_vec::ForkVec<u32>>,
    /// The chunks builds stopped filling before every slot held a row.
    open_chunks: std::sync::Mutex<Vec<FilledChunk>>,
    /// How many boxes builds numbered their rows in pre-order below, see [`ChunkClaims::take_pre_order_roots`].
    pre_order_roots: std::sync::atomic::AtomicU32,
}

impl ChunkClaims {
    /// Claims for the free slots `free_slots`, then for chunks after the first `chunk_count` chunks the arena has.
    pub(crate) fn new(chunk_count: usize, free_slots: crate::fork_vec::ForkVec<u32>) -> Self {
        let first_chunk = u32::try_from(chunk_count).expect("the arena holds fewer than 2^32 chunks");
        Self(std::sync::Arc::new(ChunkClaimsState {
            first_chunk,
            next_chunk: std::sync::atomic::AtomicU32::new(first_chunk),
            free_slots: std::sync::Mutex::new(free_slots),
            open_chunks: std::sync::Mutex::new(Vec::new()),
            pre_order_roots: std::sync::atomic::AtomicU32::new(0),
        }))
    }

    /// Names `count` boxes a build numbers the rows below in pre-order, answering the name of the first: each row is
    /// numbered by the name of the box it is below and its position among the rows below it (see
    /// [`ChunkColumns::pre_order_positions`]).
    pub(crate) fn take_pre_order_roots(&self, count: usize) -> u32 {
        self.0.pre_order_roots.fetch_add(
            u32::try_from(count).expect("a build numbers fewer than 2^32 boxes"),
            std::sync::atomic::Ordering::Relaxed,
        )
    }

    /// How many boxes the builds numbered the rows below, once every build is over.
    pub(crate) fn pre_order_root_count(&self) -> usize {
        self.0.pre_order_roots.load(std::sync::atomic::Ordering::Relaxed) as usize
    }

    /// A chunk to fill from its first slot no row is in yet.
    fn claim_chunk(&self) -> FilledChunk {
        if let Some(chunk) = self
            .0
            .open_chunks
            .lock()
            .expect("no build panicked holding the open chunks")
            .pop()
        {
            return chunk;
        }
        FilledChunk {
            index: self.0.next_chunk.fetch_add(1, std::sync::atomic::Ordering::Relaxed),
            rows: Box::default(),
            columns: ChunkColumns::new_boxed(),
            used: 0,
        }
    }

    /// Takes back a chunk a build stopped filling before every slot held a row, for the next build that needs one.
    fn give_back_open_chunk(&self, chunk: FilledChunk) {
        self.0
            .open_chunks
            .lock()
            .expect("no build panicked holding the open chunks")
            .push(chunk);
    }

    /// Up to a claim's worth of free slots, each named for its next row.
    fn claim_free_slots(&self, at_most: usize) -> Vec<NodeSlotId> {
        let mut free_slots = self
            .0
            .free_slots
            .lock()
            .expect("no build panicked holding the free slots");
        let count = free_slots.len().min(at_most);
        (0..count)
            .map(|_| {
                crate::layout::layout_node_arena::free_slot_for_next_row(
                    free_slots.pop().expect("the free slots counted are there"),
                )
            })
            .collect()
    }

    fn give_back_free_slots(&self, slots: &[NodeSlotId]) {
        let mut free_slots = self
            .0
            .free_slots
            .lock()
            .expect("no build panicked holding the free slots");
        for &slot in slots.iter().rev() {
            free_slots.push(crate::layout::layout_node_arena::free_slot_entry(
                slot.slot_index(),
                slot.generation() - 1,
            ));
        }
    }

    /// The free slots no build took, once every build is over.
    pub(crate) fn take_free_slots(&self) -> crate::fork_vec::ForkVec<u32> {
        std::mem::take(
            &mut *self
                .0
                .free_slots
                .lock()
                .expect("no build panicked holding the free slots"),
        )
    }

    /// The chunks builds stopped filling before every slot held a row, once every build is over.
    pub(crate) fn take_open_chunks(&self) -> Vec<FilledChunk> {
        std::mem::take(
            &mut *self
                .0
                .open_chunks
                .lock()
                .expect("no build panicked holding the open chunks"),
        )
    }
}

impl Default for ChunkClaims {
    fn default() -> Self {
        Self::new(0, crate::fork_vec::ForkVec::default())
    }
}

/// What the arena keeps beside a row in its per-slot columns, as a build writes it for a row it stamps completely: the
/// style record the row names, which the arena holds a published pin on, the node the row carries, and the name the
/// document knows that node by.
#[derive(Clone, Copy, Default)]
pub(crate) struct RowColumns {
    /// Zero for a row the build leaves to the builder's own steps to stamp.
    pub(crate) style_record: u64,
    pub(crate) style_node: Option<crate::css::style::tree::StyleNodeID>,
    pub(crate) unique_node_id: i64,
    /// See [`ChunkColumns::pre_order_positions`].
    pub(crate) pre_order_position: u64,
    /// See [`ChunkColumns::box_facts`].
    pub(crate) box_facts: u32,
}

/// The [`RowColumns`] of each row of a chunk a build fills, a column at a time, for the arena to take over a column at
/// a time.
pub(crate) struct ChunkColumns {
    pub(crate) style_records: [u64; SLOTS_PER_CHUNK],
    pub(crate) style_nodes: [Option<crate::css::style::tree::StyleNodeID>; SLOTS_PER_CHUNK],
    pub(crate) unique_node_ids: [i64; SLOTS_PER_CHUNK],
    /// Where each row stands in the pre-order of the subtree the build made: the name of the box it is below, in the
    /// bits above the low 32 (see [`ChunkClaims::take_pre_order_roots`]), and its position among the rows below that box
    /// in the low 32 bits. The arena makes pre-order labels of them once it knows where the rows below each box start.
    pub(crate) pre_order_positions: [u64; SLOTS_PER_CHUNK],
    /// The [`box_fact`]s of each row, which the build reads as it places rows, and the arena leaves behind.
    box_facts: [u32; SLOTS_PER_CHUNK],
}

/// How many bits of a packed pre-order position hold the position among the rows numbered below the same name.
const PRE_ORDER_POSITION_BITS: u32 = 40;

/// How far apart the positions of the rows a walk appends are: a row put before another takes a position between that
/// row's and the one before it, halving the room there.
pub(crate) const PRE_ORDER_POSITION_STEP: u64 = 1 << 8;

/// The position of a row in the pre-order of a planned subtree as [`ChunkColumns::pre_order_positions`] holds it: the
/// name of the stretch of rows it is numbered in, and its position there. Zero for a row not numbered yet.
pub(crate) fn packed_pre_order_position(name: u32, position: u64) -> u64 {
    assert!(
        position < 1 << PRE_ORDER_POSITION_BITS,
        "a build makes fewer than 2^32 rows"
    );
    assert!(
        name < 1 << (64 - PRE_ORDER_POSITION_BITS),
        "a build numbers fewer than 2^24 stretches of rows"
    );
    (u64::from(name) << PRE_ORDER_POSITION_BITS) | position
}

/// The name and the position of [`packed_pre_order_position`].
pub(crate) fn unpacked_pre_order_position(packed: u64) -> (usize, u64) {
    (
        (packed >> PRE_ORDER_POSITION_BITS) as usize,
        packed & ((1 << PRE_ORDER_POSITION_BITS) - 1),
    )
}

/// What a box gives each of its children as the child list builder attaches them, the same for every child that is not
/// anonymous: see [`PlanTree::attach`].
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) struct ChildrenDerivation {
    /// Whether the box's children are derived as the commit attaches them.
    derived_on_commit: bool,
    /// Whether the box itself is.
    parent_is_derived_on_commit: bool,
    /// The ancestor facts of a child that is not anonymous.
    ancestor_facts: u8,
    /// The item flag of a child the box lays out as an item.
    item_flag: Option<NodeFlag>,
}

impl ChunkColumns {
    fn new_boxed() -> Box<Self> {
        Box::new(Self {
            style_records: [0; SLOTS_PER_CHUNK],
            style_nodes: [None; SLOTS_PER_CHUNK],
            unique_node_ids: [0; SLOTS_PER_CHUNK],
            pre_order_positions: [0; SLOTS_PER_CHUNK],
            box_facts: [0; SLOTS_PER_CHUNK],
        })
    }
}

/// A chunk of the arena's slots to come that builds fill: its rows, written where the arena keeps them, and what the
/// arena keeps beside them, which it takes over as they are.
pub(crate) struct FilledChunk {
    /// The index the chunk takes among the arena's chunks.
    pub(crate) index: u32,
    pub(crate) rows: Box<crate::layout::tree_shape::Chunk>,
    pub(crate) columns: Box<ChunkColumns>,
    /// How many slots, from the chunk's first, hold a row.
    pub(crate) used: usize,
}

/// Where the rows of a [`BuiltChunk`] are.
enum RowStorage {
    /// Rows the plan stands in for, or rows going in free slots of the arena's chunks.
    Own {
        rows: Vec<NodeData>,
        columns: Vec<RowColumns>,
    },
    /// Rows of a chunk to come.
    Chunk(FilledChunk),
    /// Rows of a chunk to come that the plan handed on to fill to the next build that claims a chunk, once it made
    /// every row of its own.
    HandedOn,
}

/// Rows a build made in one claim.
struct BuiltChunk {
    storage: RowStorage,
    /// The first slot of the chunk the build's rows are in, past those other builds filled.
    start: usize,
    /// How many rows the build made in the chunk.
    len: usize,
    /// The free slots of the arena the rows go in, if they go in such slots: the build claimed them all, and used as
    /// many as the chunk holds rows.
    free_slots: Option<Vec<NodeSlotId>>,
    /// The index of the arena's chunk the rows go in, or [`u32::MAX`] for rows the build does not own and for rows put
    /// in free slots of the arena's chunks.
    index: u32,
}

impl BuiltChunk {
    fn own(free_slots: Option<Vec<NodeSlotId>>) -> Self {
        let capacity = free_slots.as_ref().map_or(0, Vec::len);
        Self {
            storage: RowStorage::Own {
                rows: Vec::with_capacity(capacity),
                columns: Vec::with_capacity(capacity),
            },
            start: 0,
            len: 0,
            free_slots,
            index: u32::MAX,
        }
    }

    fn is_full(&self) -> bool {
        match (&self.storage, &self.free_slots) {
            (RowStorage::Own { .. }, Some(free_slots)) => self.len == free_slots.len(),
            (RowStorage::Own { .. }, None) => false,
            (RowStorage::Chunk(_), _) => self.start + self.len == SLOTS_PER_CHUNK,
            (RowStorage::HandedOn, _) => true,
        }
    }

    #[inline]
    fn row(&self, offset: usize) -> &NodeData {
        match &self.storage {
            RowStorage::Own { rows, .. } => &rows[offset],
            RowStorage::Chunk(chunk) => chunk.rows.slot(offset),
            RowStorage::HandedOn => unreachable!("a plan reads no row of a chunk it handed on"),
        }
    }

    #[inline]
    fn row_mut(&mut self, offset: usize) -> &mut NodeData {
        match &mut self.storage {
            RowStorage::Own { rows, .. } => &mut rows[offset],
            RowStorage::Chunk(chunk) => chunk.rows.unpublished_slot_mut(offset),
            RowStorage::HandedOn => unreachable!("a plan writes no row of a chunk it handed on"),
        }
    }

    #[inline]
    fn facts(&self, offset: usize) -> u32 {
        match &self.storage {
            RowStorage::Own { columns, .. } => columns[offset].box_facts,
            RowStorage::Chunk(chunk) => chunk.columns.box_facts[offset],
            RowStorage::HandedOn => unreachable!("a plan reads no row of a chunk it handed on"),
        }
    }

    #[inline]
    fn facts_mut(&mut self, offset: usize) -> &mut u32 {
        match &mut self.storage {
            RowStorage::Own { columns, .. } => &mut columns[offset].box_facts,
            RowStorage::Chunk(chunk) => &mut chunk.columns.box_facts[offset],
            RowStorage::HandedOn => unreachable!("a plan writes no row of a chunk it handed on"),
        }
    }

    fn set_columns(&mut self, offset: usize, row_columns: RowColumns) {
        match &mut self.storage {
            RowStorage::Own { columns, .. } => {
                columns[offset] = RowColumns {
                    pre_order_position: columns[offset].pre_order_position,
                    box_facts: columns[offset].box_facts,
                    ..row_columns
                }
            }
            RowStorage::Chunk(chunk) => {
                chunk.columns.style_records[offset] = row_columns.style_record;
                chunk.columns.style_nodes[offset] = row_columns.style_node;
                chunk.columns.unique_node_ids[offset] = row_columns.unique_node_id;
            }
            RowStorage::HandedOn => unreachable!("a plan writes no row of a chunk it handed on"),
        }
    }

    fn style_node(&self, offset: usize) -> Option<crate::css::style::tree::StyleNodeID> {
        match &self.storage {
            RowStorage::Own { columns, .. } => columns[offset].style_node,
            RowStorage::Chunk(chunk) => chunk.columns.style_nodes[offset],
            RowStorage::HandedOn => unreachable!("a plan reads no row of a chunk it handed on"),
        }
    }

    #[inline]
    fn pre_order_position(&self, offset: usize) -> u64 {
        match &self.storage {
            RowStorage::Own { columns, .. } => columns[offset].pre_order_position,
            RowStorage::Chunk(chunk) => chunk.columns.pre_order_positions[offset],
            RowStorage::HandedOn => unreachable!("a plan reads no row of a chunk it handed on"),
        }
    }

    #[inline]
    fn set_pre_order_position(&mut self, offset: usize, position: u64) {
        match &mut self.storage {
            RowStorage::Own { columns, .. } => columns[offset].pre_order_position = position,
            RowStorage::Chunk(chunk) => chunk.columns.pre_order_positions[offset] = position,
            RowStorage::HandedOn => unreachable!("a plan writes no row of a chunk it handed on"),
        }
    }

    /// Writes a row, with its facts, in the slot after the last one the build filled.
    #[inline]
    fn push_row(&mut self, row: NodeData, facts: u32) {
        let offset = self.start + self.len;
        match &mut self.storage {
            RowStorage::Own { rows, columns } => {
                debug_assert_eq!(rows.len(), offset);
                rows.push(row);
                columns.push(RowColumns {
                    box_facts: facts,
                    ..RowColumns::default()
                });
            }
            RowStorage::Chunk(chunk) => {
                *chunk.rows.unpublished_slot_mut(offset) = row;
                chunk.columns.box_facts[offset] = facts;
            }
            RowStorage::HandedOn => unreachable!("a plan writes no row of a chunk it handed on"),
        }
        self.len += 1;
    }
}

/// The positions in [`PlanTree::chunks`] of the stand-ins and of the rows going in free slots of the arena's chunks.
const STAND_INS: usize = 0;
const REUSED_ROWS: usize = 1;

/// Rows a build made in free slots of the arena, each going in its slot.
pub(crate) struct ReusedRows {
    pub(crate) rows: Vec<NodeData>,
    pub(crate) columns: Vec<RowColumns>,
    pub(crate) slots: Vec<NodeSlotId>,
}

/// The rows of the boxes a walk planned, each written as the walk decides it in a chunk the build owns, named by the
/// slot it takes in the arena, and linked by those slots: but for the boxes the plan is for the descendants of, which
/// are rows of the arena: rows of the plan stand in for them, and the commit writes what the build linked under them
/// into the arena's.
pub(crate) struct PlanTree {
    /// The rows the walk made: the stand-ins, the rows going in free slots of the arena's chunks, then the chunks to
    /// come it claimed, in the order it claimed them.
    chunks: Vec<BuiltChunk>,
    /// The position in `chunks` of the chunk the walk writes rows in now.
    current_chunk: usize,
    /// The position in `chunks` of each chunk to come the plan fills, by its index past the arena's.
    chunk_positions: Vec<u32>,
    first_claimed_chunk: usize,
    /// For each chunk of the arena, by its index, the table of [`Self::reused_row_offsets`] that says where the rows
    /// going in its free slots are, if the plan put rows there.
    reused_row_tables: Vec<u32>,
    /// Where each row going in a free slot of a chunk of the arena is among those rows, by the slot's offset in the
    /// chunk; [`u32::MAX`] for a slot the plan put no row in.
    reused_row_offsets: Vec<[u32; SLOTS_PER_CHUNK]>,
    /// Where among the stand-ins the row standing in for each arena row is.
    stand_in_offsets: crate::css::style::fast_hash::FastMap<NodeSlotId, u32>,
    /// The stand-in made last, below which the walk plans, and where it is among the stand-ins.
    latest_stand_in: (NodeSlotId, u32),
    /// The stand-in made before it: a child list planned again places each box it keeps after the one before.
    previous_stand_in: (NodeSlotId, u32),
    /// The arena row each stand-in stands in for, by its offset, and the offset a lookup tries first.
    stand_in_slots: Vec<NodeSlotId>,
    stand_in_hint: std::cell::Cell<u32>,
    /// The row the walk made last, and where it is: see [`Self::locate`].
    latest_row: (NodeSlotId, u32, u32),
    claims: ChunkClaims,
    /// How many free slots of the arena the walk claims next, see [`FREE_SLOTS_PER_CLAIM`].
    free_slots_to_claim: usize,
    /// The inline boxes block-level boxes were lifted out of, as `(box, inline box)`.
    pub(crate) lifted_out_of_inline_boxes: Vec<(NodeSlotId, NodeSlotId)>,
    /// The layout algorithm each box of a MathML element selects, for the boxes whose row the build stamped and the boxes
    /// the plan stands in for.
    pub(crate) math_layout_kinds:
        crate::css::style::fast_hash::FastMap<NodeSlotId, crate::layout::math_formatting_context::MathLayoutKind>,
    /// How many rows the plan made, its stand-ins aside.
    row_count: usize,
    /// The boxes whose child lists the walk is in, innermost last, each with what it gives its children.
    open_parents: Vec<(NodeSlotId, ChildrenDerivation, u32, u32)>,
    /// What the box the walk placed a child in last, while that is no box whose child list it is in, gives its
    /// children: an anonymous wrapper the walk appends one text or inline box after another to.
    edge_derivation: Option<(NodeSlotId, ChildrenDerivation)>,
    /// The name the rows appended now are numbered below in pre-order, and how many it numbers so far.
    pre_order_name: u32,
    pre_order_rows: u64,
    /// Whether the build put rows out of the order their pre-order positions say, which the arena then labels again.
    pre_order_positions_out_of_order: bool,
    /// The rows the commit gives what the lists of the plan do not one by one, in the order the walk made them: rows the
    /// builder's own steps stamp, and rows whose style or place in the tree asks for more.
    pub(crate) rows_committed_one_by_one: Vec<RowCommittedOneByOne>,
    /// The boxes whose child lists, with those of the anonymous boxes in them, table fixup goes over once the rows are
    /// in the arena: each pseudo-element box of a table display, whose generated content the commit makes, and the
    /// child list a walk plans again where a part of a table is in it. The walk fixes up every other list it plans as
    /// it closes it, see [`table_fixup`].
    pub(crate) table_fixup_lists: Vec<NodeSlotId>,
    /// The elements whose `::marker`, `::before` or `::after` the walk planned a new box for whose content opens or
    /// closes quotes.
    pub(crate) quote_generators: Vec<crate::css::style::tree::StyleNodeID>,
    /// The rows of the boxes table fixup removed from the child lists the walk planned, which the commit frees.
    pub(crate) dropped_rows: Vec<NodeSlotId>,
    /// The table roots the walk made table wrappers for, whose missing cells the commit makes.
    pub(crate) table_roots: Vec<NodeSlotId>,
    /// The display of each box without a style of its own that is a part of a table, which table fixup reads of it:
    /// the anonymous table boxes table fixup made, and the boxes of pseudo-elements of table displays.
    table_part_displays: crate::css::style::fast_hash::FastMap<NodeSlotId, FfiDisplay>,
    /// The boxes of pseudo-elements whose content makes nothing but white space, which the commit makes: table fixup
    /// reads whether a box holds only white space before the commit.
    white_space_pseudo_element_boxes: crate::css::style::fast_hash::FastSet<NodeSlotId>,
    /// Whether the walk moved a box standing in for an arena row into another box, as a block that goes among inline
    /// boxes does: the boxes of a child list planned from a window then reach before the window.
    pub(crate) moved_stand_in: bool,
}

impl Default for PlanTree {
    fn default() -> Self {
        Self::new(ChunkClaims::default())
    }
}

impl PlanTree {
    pub(crate) fn new(claims: ChunkClaims) -> Self {
        Self {
            chunks: vec![BuiltChunk::own(None), BuiltChunk::own(Some(Vec::new()))],
            current_chunk: REUSED_ROWS,
            chunk_positions: Vec::new(),
            first_claimed_chunk: claims.0.first_chunk as usize,
            reused_row_tables: Vec::new(),
            reused_row_offsets: Vec::new(),
            stand_in_offsets: Default::default(),
            latest_stand_in: (NodeSlotId::INVALID, 0),
            previous_stand_in: (NodeSlotId::INVALID, 0),
            stand_in_slots: Vec::new(),
            stand_in_hint: std::cell::Cell::new(0),
            latest_row: (NodeSlotId::INVALID, 0, 0),
            claims,
            free_slots_to_claim: 1,
            lifted_out_of_inline_boxes: Vec::new(),
            math_layout_kinds: Default::default(),
            row_count: 0,
            open_parents: Vec::new(),
            edge_derivation: None,
            pre_order_name: 0,
            pre_order_rows: 0,
            pre_order_positions_out_of_order: false,
            rows_committed_one_by_one: Vec::new(),
            table_fixup_lists: Vec::new(),
            quote_generators: Vec::new(),
            dropped_rows: Vec::new(),
            table_roots: Vec::new(),
            table_part_displays: Default::default(),
            white_space_pseudo_element_boxes: Default::default(),
            moved_stand_in: false,
        }
    }

    /// Where the row of `slot` is: its chunk's position in `chunks`, and its offset there.
    #[inline(always)]
    fn locate(&self, slot: NodeSlotId) -> (usize, usize) {
        // OPTIMIZATION: Placing a row reads it, the box it goes in and that box's children; the walk places most rows
        //               just after it made them, below the box whose child list it is in.
        if slot == self.latest_row.0 {
            return (self.latest_row.1 as usize, self.latest_row.2 as usize);
        }
        if let Some(open) = self.open_parents.last()
            && open.0 == slot
        {
            return (open.2 as usize, open.3 as usize);
        }
        if slot == self.latest_stand_in.0 {
            return (STAND_INS, self.latest_stand_in.1 as usize);
        }
        if slot == self.previous_stand_in.0 {
            return (STAND_INS, self.previous_stand_in.1 as usize);
        }
        let index = slot.slot_index() as usize;
        let (chunk, offset) = (index / SLOTS_PER_CHUNK, index % SLOTS_PER_CHUNK);
        if let Some(claimed) = chunk.checked_sub(self.first_claimed_chunk) {
            if let Some(&position) = self.chunk_positions.get(claimed)
                && position != u32::MAX
                && offset >= self.chunks[position as usize].start
            {
                return (position as usize, offset);
            }
        } else if let Some(&table) = self.reused_row_tables.get(chunk)
            && table != u32::MAX
        {
            let reused = self.reused_row_offsets[table as usize][offset];
            if reused != u32::MAX {
                return (REUSED_ROWS, reused as usize);
            }
        }
        self.locate_stand_in(slot)
    }

    #[cold]
    #[inline(never)]
    fn locate_stand_in(&self, slot: NodeSlotId) -> (usize, usize) {
        (
            STAND_INS,
            self.stand_in_offset(slot).expect("a planned row is one the plan made") as usize,
        )
    }

    /// Where among the stand-ins the one for the arena row `slot` is, if the plan has one.
    fn stand_in_offset(&self, slot: NodeSlotId) -> Option<u32> {
        // OPTIMIZATION: The stand-ins of a child list planned again are read in the order the walk made them.
        let hint = self.stand_in_hint.get();
        if self.stand_in_slots.get(hint as usize) == Some(&slot) {
            self.stand_in_hint.set(hint + 1);
            return Some(hint);
        }
        let offset = *self.stand_in_offsets.get(&slot)?;
        self.stand_in_hint.set(offset + 1);
        Some(offset)
    }

    #[inline]
    pub(crate) fn node(&self, slot: NodeSlotId) -> PlannedBox<'_> {
        self.box_at(self.locate(slot))
    }

    /// The box of the row at `(position, offset)`, see [`Self::locate`].
    #[inline]
    fn box_at(&self, (position, offset): (usize, usize)) -> PlannedBox<'_> {
        let chunk = &self.chunks[position];
        PlannedBox {
            row: chunk.row(offset),
            facts: chunk.facts(offset),
        }
    }

    #[inline]
    pub(crate) fn row_mut(&mut self, slot: NodeSlotId) -> &mut NodeData {
        let (position, offset) = self.locate(slot);
        self.chunks[position].row_mut(offset)
    }

    #[inline]
    fn facts_mut(&mut self, slot: NodeSlotId) -> &mut u32 {
        let (position, offset) = self.locate(slot);
        self.chunks[position].facts_mut(offset)
    }

    /// Writes what the arena keeps beside the row of `slot`, which is not a stand-in.
    pub(crate) fn set_columns(&mut self, slot: NodeSlotId, columns: RowColumns) {
        let (position, offset) = self.locate(slot);
        debug_assert_ne!(position, 0, "a stand-in row is the arena's");
        self.chunks[position].set_columns(offset, columns);
    }

    /// The node the row of `slot`, which is not a stand-in, is bound to, if any.
    pub(crate) fn style_node(&self, slot: NodeSlotId) -> Option<crate::css::style::tree::StyleNodeID> {
        let (position, offset) = self.locate(slot);
        self.chunks[position].style_node(offset)
    }

    /// Notes the display of `slot`, a box without a style of its own that is a part of a table, see
    /// [`Self::table_part_displays`].
    pub(crate) fn note_table_part_display(&mut self, slot: NodeSlotId, display: FfiDisplay) {
        self.table_part_displays.insert(slot, display);
    }

    /// Notes that the content of the pseudo-element whose box is `slot` makes nothing but white space, see
    /// [`Self::white_space_pseudo_element_boxes`].
    pub(crate) fn note_white_space_pseudo_element_box(&mut self, slot: NodeSlotId) {
        self.white_space_pseudo_element_boxes.insert(slot);
    }

    /// Whether `slot` is a stand-in: a row of the arena the plan's rows hang below.
    pub(crate) fn is_stand_in(&self, slot: NodeSlotId) -> bool {
        self.locate(slot).0 == STAND_INS
    }

    /// Whether the arena row `slot`, which the plan may not hold, has a row standing in for it.
    pub(crate) fn stands_in_for(&self, slot: NodeSlotId) -> bool {
        self.stand_in_offset(slot).is_some()
    }

    /// Makes a row standing in for the arena row `slot`: the box the build places the boxes it plans below, which has no
    /// children yet, or a box a child list planned again keeps, which the build places but plans nothing below.
    pub(crate) fn push_stand_in(&mut self, slot: NodeSlotId, node: PlanNode) {
        let chunk = &mut self.chunks[STAND_INS];
        let offset = chunk.len;
        self.stand_in_offsets.insert(slot, offset as u32);
        self.stand_in_slots.push(slot);
        self.previous_stand_in = std::mem::replace(&mut self.latest_stand_in, (slot, offset as u32));
        chunk.push_row(
            Self::new_row(&node, slot.generation()),
            node.facts & !(box_fact::OPEN_PARENT | box_fact::HAS_TABLE_PART_CHILD),
        );
    }

    #[inline]
    pub(crate) fn push(&mut self, node: PlanNode) -> NodeSlotId {
        if self.chunks[self.current_chunk].is_full() {
            self.claim_chunk();
        }
        let chunk = &mut self.chunks[self.current_chunk];
        let offset = chunk.start + chunk.len;
        let slot = match &chunk.free_slots {
            Some(free_slots) => free_slots[offset],
            None => NodeSlotId::new(
                u32::try_from(chunk.index as usize * SLOTS_PER_CHUNK + offset).expect("a slot index fits 32 bits"),
                FRESH_ROW_GENERATION,
            ),
        };
        chunk.push_row(Self::new_row(&node, slot.generation()), node.facts);
        self.latest_row = (slot, self.current_chunk as u32, offset as u32);
        self.row_count += 1;
        slot
    }

    fn new_row(node: &PlanNode, generation: u8) -> NodeData {
        let mut row = NodeData::default();
        *row.kind.get_mut() = node.kind;
        *row.flags.get_mut() = node.flags;
        *row.generated_for.get_mut() = node.generated_for;
        *row.slot_generation.get_mut() = generation;
        *row.style.get_mut() = node.style;
        row
    }

    /// How many rows the plan made so far, its stand-ins aside.
    pub(crate) fn row_count(&self) -> usize {
        self.row_count
    }

    fn claim_chunk(&mut self) {
        let free_slots = self.claims.claim_free_slots(self.free_slots_to_claim);
        self.free_slots_to_claim = (self.free_slots_to_claim * 2).min(FREE_SLOTS_PER_CLAIM);
        if !free_slots.is_empty() {
            let reused = &mut self.chunks[REUSED_ROWS];
            let reused_slots = reused.free_slots.as_mut().expect("the reused rows go in free slots");
            for &slot in &free_slots {
                let index = slot.slot_index() as usize;
                let (chunk, offset) = (index / SLOTS_PER_CHUNK, index % SLOTS_PER_CHUNK);
                if self.reused_row_tables.len() <= chunk {
                    self.reused_row_tables.resize(chunk + 1, u32::MAX);
                }
                if self.reused_row_tables[chunk] == u32::MAX {
                    self.reused_row_tables[chunk] = self.reused_row_offsets.len() as u32;
                    self.reused_row_offsets.push([u32::MAX; SLOTS_PER_CHUNK]);
                }
                self.reused_row_offsets[self.reused_row_tables[chunk] as usize][offset] = reused_slots.len() as u32;
                reused_slots.push(slot);
            }
            self.current_chunk = REUSED_ROWS;
            return;
        }
        let chunk = self.claims.claim_chunk();
        let (index, start) = (chunk.index, chunk.used);
        let local = index as usize - self.first_claimed_chunk;
        if self.chunk_positions.len() <= local {
            self.chunk_positions.resize(local + 1, u32::MAX);
        }
        self.chunk_positions[local] = self.chunks.len() as u32;
        self.current_chunk = self.chunks.len();
        self.chunks.push(BuiltChunk {
            storage: RowStorage::Chunk(chunk),
            start,
            len: 0,
            free_slots: None,
            index,
        });
    }

    /// Hands the chunk the plan filled last, if it has slots left, to the next build that claims a chunk, once the plan
    /// made, linked and derived every row of its own. Nothing of the plan reads or writes a row in it after.
    pub(crate) fn hand_on_open_chunk(&mut self) {
        let chunk = &mut self.chunks[self.current_chunk];
        if !matches!(chunk.storage, RowStorage::Chunk(_)) || chunk.is_full() {
            return;
        }
        let RowStorage::Chunk(mut filled) = std::mem::replace(&mut chunk.storage, RowStorage::HandedOn) else {
            unreachable!("the storage was a chunk");
        };
        filled.used = chunk.start + chunk.len;
        self.chunk_positions[chunk.index as usize - self.first_claimed_chunk] = u32::MAX;
        self.claims.give_back_open_chunk(filled);
    }

    /// Hands the rows over: the chunks the plan filled, and the rows it put in free slots of the arena. The free slots
    /// it took and did not use go back to the claims. The plan keeps its stand-ins.
    pub(crate) fn take_rows(&mut self) -> (Vec<FilledChunk>, Vec<ReusedRows>) {
        let mut filled = Vec::new();
        let mut reused = Vec::new();
        for chunk in &mut self.chunks[REUSED_ROWS..] {
            let used = chunk.start + chunk.len;
            match std::mem::replace(&mut chunk.storage, RowStorage::HandedOn) {
                RowStorage::Own { rows, columns } => {
                    let free_slots = chunk
                        .free_slots
                        .as_mut()
                        .expect("a plan's own rows past its stand-ins reuse slots");
                    self.claims.give_back_free_slots(&free_slots[used..]);
                    free_slots.truncate(used);
                    if !rows.is_empty() {
                        reused.push(ReusedRows {
                            rows,
                            columns,
                            slots: std::mem::take(free_slots),
                        });
                    }
                }
                RowStorage::Chunk(mut rows) => {
                    rows.used = used;
                    filled.push(rows);
                }
                RowStorage::HandedOn => {}
            }
        }
        self.chunk_positions.clear();
        self.reused_row_tables.clear();
        self.reused_row_offsets.clear();
        (filled, reused)
    }

    pub(crate) fn claims(&self) -> &ChunkClaims {
        &self.claims
    }

    /// Gives `node`, a stand-in, the children from `first_child` to `last_child`, which a job planned for a run of its
    /// children below the row standing in for it, linked after the children it has already, and the facts they set.
    pub(crate) fn append_children_of_stand_in(
        &mut self,
        node: NodeSlotId,
        first_child: NodeSlotId,
        last_child: NodeSlotId,
        children_facts: u32,
    ) {
        let row = self.row_mut(node);
        if row.first_child.get().is_invalid() {
            *row.first_child.get_mut() = first_child;
        }
        *row.last_child.get_mut() = last_child;
        *self.facts_mut(node) |= children_facts & box_fact::OF_CHILDREN;
    }

    /// Gives `node` the children a job linked below the row standing in for it in `job`'s plan, and the flag and facts
    /// they set.
    pub(crate) fn take_children_of_stand_in(&mut self, node: NodeSlotId, job: &PlanTree) {
        let stand_in = job.node(node);
        let (first_child, last_child) = (stand_in.first_child(), stand_in.last_child());
        let children_are_inline = stand_in.has_flag(NodeFlag::ChildrenAreInline);
        let children_facts = stand_in.facts() & box_fact::OF_CHILDREN;
        let row = self.row_mut(node);
        *row.first_child.get_mut() = first_child;
        *row.last_child.get_mut() = last_child;
        self.set_flag(node, NodeFlag::ChildrenAreInline, children_are_inline);
        let facts = self.facts_mut(node);
        *facts = (*facts & !box_fact::OF_CHILDREN) | children_facts;
    }

    pub(crate) fn set_flag(&mut self, slot: NodeSlotId, flag: NodeFlag, value: bool) {
        self.forget_edge_derivation_of(slot);
        let flags = self.row_mut(slot).flags.get_mut();
        if value {
            *flags |= flag as u32;
        } else {
            *flags &= !(flag as u32);
        }
    }

    #[cfg(test)]
    pub(crate) fn set_generated_for(&mut self, slot: NodeSlotId, generated_for: u8) {
        *self.row_mut(slot).generated_for.get_mut() = generated_for;
    }

    /// Makes `parent` the box the walk places the boxes it plans next below, until [`Self::close_parent`].
    pub(crate) fn open_parent(&mut self, parent: NodeSlotId) {
        let derivation = self.children_derivation(parent);
        let (position, offset) = self.locate(parent);
        let facts = self.chunks[position].facts_mut(offset);
        debug_assert_eq!(*facts & box_fact::OPEN_PARENT, 0, "the walk opens a child list once");
        *facts |= box_fact::OPEN_PARENT;
        self.open_parents
            .push((parent, derivation, position as u32, offset as u32));
    }

    pub(crate) fn close_parent(&mut self) {
        let (_, _, position, offset) = self.open_parents.pop().expect("the walk closes a child list it opened");
        *self.chunks[position as usize].facts_mut(offset as usize) &= !box_fact::OPEN_PARENT;
    }

    /// Forgets what [`Self::edge_derivation`] says `slot` gives its children, as `slot` changes.
    #[inline]
    fn forget_edge_derivation_of(&mut self, slot: NodeSlotId) {
        if self.edge_derivation.is_some_and(|(edge, _)| edge == slot) {
            self.edge_derivation = None;
        }
    }

    /// The box the walk places the boxes it plans below.
    pub(crate) fn current_parent(&self) -> NodeSlotId {
        self.open_parents
            .last()
            .expect("layout tree planner must have an insertion ancestor")
            .0
    }

    /// Whether the walk is in the child list of `node`.
    #[inline]
    pub(crate) fn is_open_parent(&self, node: NodeSlotId) -> bool {
        self.node(node).has_fact(box_fact::OPEN_PARENT)
    }

    /// The style a box is planned with: an element's, or the style of the box a stand-in stands in for.
    #[inline]
    pub(crate) fn style_of(&self, node: NodeSlotId) -> Option<crate::layout::ComputedValuesView<'_>> {
        // SAFETY: The record a planned row names is a published one, which outlives the build, as the records of the rows
        //         the plan stands in for do.
        self.node(node).row.style.get().map(|style| unsafe { style.view() })
    }

    /// Notes the layout algorithm `node`, a stand-in, selects, as the box it stands in for does.
    pub(crate) fn note_stand_in_math_layout_kind(
        &mut self,
        node: NodeSlotId,
        kind: crate::layout::math_formatting_context::MathLayoutKind,
    ) {
        if kind != crate::layout::math_formatting_context::MathLayoutKind::NotMathML {
            self.math_layout_kinds.insert(node, kind);
        }
    }

    /// The layout algorithm the box of `node` selects, if it is the box of a MathML element.
    pub(crate) fn math_layout_kind_of(
        &self,
        node: NodeSlotId,
    ) -> crate::layout::math_formatting_context::MathLayoutKind {
        self.math_layout_kinds
            .get(&node)
            .copied()
            .unwrap_or(crate::layout::math_formatting_context::MathLayoutKind::NotMathML)
    }

    /// What `parent` gives each of its children, see [`ChildrenDerivation`].
    /// The ancestor facts `parent`, whose box is `parent_box` and whose style is `parent_style`, gives a child, anonymous
    /// or not, as the arena derives them.
    #[inline]
    fn ancestor_facts_of_children(
        &self,
        parent: NodeSlotId,
        parent_box: PlannedBox<'_>,
        parent_style: Option<crate::layout::ComputedValuesView<'_>>,
        child_is_anonymous: bool,
    ) -> u8 {
        use crate::layout::node_data::AncestorFact;
        let mut facts = crate::layout::layout_node_arena::ancestor_facts_of_children(
            parent_box.row,
            parent_style,
            || self.math_layout_kind_of(parent),
            child_is_anonymous,
        );
        // NB: The commit makes the styles of the anonymous table boxes table fixup made and of table wrappers. Of what
        //     those give their children, only whether they are unfloated flow containers is not what a box without a
        //     style gives: a table or table-row is none, a table cell is one, and a table wrapper is one unless the
        //     table it took its outer properties from floats.
        if parent_style.is_none() && parent_box.has_flag(NodeFlag::Anonymous) {
            #[cold]
            fn is_unfloated_flow_container(parent_box: PlannedBox<'_>) -> bool {
                match parent_box.kind() {
                    NodeKind::Box => !parent_box.has_fact(box_fact::TABLE_PART),
                    NodeKind::TableWrapper => !parent_box.has_fact(box_fact::FLOATING),
                    _ => true,
                }
            }
            if !is_unfloated_flow_container(parent_box) {
                facts &= !(AncestorFact::ParentIsUnfloatedFlowContainer as u8);
            }
        }
        facts
    }

    fn children_derivation(&self, parent: NodeSlotId) -> ChildrenDerivation {
        use crate::layout::layout_node_arena::item_flag_of_children;
        let parent_box = self.node(parent);
        let parent_is_derived_on_commit = parent_box.has_fact(box_fact::DERIVED_ON_COMMIT);
        if parent_box.has_fact(box_fact::CHILDREN_DERIVED_ON_COMMIT) {
            return ChildrenDerivation {
                derived_on_commit: true,
                parent_is_derived_on_commit,
                ancestor_facts: 0,
                item_flag: None,
            };
        }
        let parent_style = self.style_of(parent);
        ChildrenDerivation {
            derived_on_commit: false,
            parent_is_derived_on_commit,
            ancestor_facts: self.ancestor_facts_of_children(parent, parent_box, parent_style, false),
            // NB: A box planned without a style, as only a test plans one, lays its children out as its facts say.
            item_flag: if parent_style.is_some() {
                item_flag_of_children(
                    parent_box.row,
                    parent_style,
                    // NB: The writing mode decides nothing for the children of a flex or grid container, the only
                    //     parents this is asked of.
                    || {
                        let grandparent = parent_box.parent();
                        if grandparent.is_invalid() {
                            return None;
                        }
                        self.style_of(grandparent).map(|style| style.writing_mode())
                    },
                )
            } else if parent_box.has_fact(box_fact::ESTABLISHES_FLEX_ITEMS) {
                Some(NodeFlag::IsFlexItem)
            } else if parent_box.has_fact(box_fact::ESTABLISHES_GRID_ITEMS) {
                Some(NodeFlag::IsGridItem)
            } else {
                None
            },
        }
    }

    #[inline]
    fn derivation_for(&mut self, parent: NodeSlotId) -> ChildrenDerivation {
        if let Some((edge, derivation)) = self.edge_derivation
            && edge == parent
        {
            return derivation;
        }
        if self.is_open_parent(parent) {
            let &(_, derivation, ..) = self
                .open_parents
                .iter()
                .rev()
                .find(|&&(open, ..)| open == parent)
                .expect("an open parent is on the stack");
            return derivation;
        }
        let derivation = self.children_derivation(parent);
        self.edge_derivation = Some((parent, derivation));
        derivation
    }

    /// Derives what the row of `child`, just attached below `parent`, takes from it, as the arena derives it for a row it
    /// attaches: ancestor facts and flex or grid item flags. A row whose parent takes its style or its facts from the
    /// commit, or a pseudo-element's, is left to the commit.
    fn derive_attached_row(
        &mut self,
        parent: NodeSlotId,
        child: NodeSlotId,
        child_at: (usize, usize),
        derivation: &ChildrenDerivation,
    ) {
        use crate::layout::layout_node_arena::anonymous_box_may_be_run_of_collapsible_whitespace;
        self.forget_edge_derivation_of(child);
        let planned = self.box_at(child_at);
        let mut facts = planned.facts()
            & !(box_fact::DERIVED_ON_COMMIT
                | box_fact::SUBTREE_DERIVED_ON_COMMIT
                | box_fact::CHILDREN_DERIVED_ON_COMMIT
                | box_fact::FLEX_ITEM_UNLESS_ONLY_TEXT
                | box_fact::GRID_ITEM_UNLESS_ONLY_TEXT);
        if derivation.derived_on_commit || planned.generated_for() != 0 {
            facts |= box_fact::DERIVED_ON_COMMIT | box_fact::CHILDREN_DERIVED_ON_COMMIT;
            if !derivation.parent_is_derived_on_commit {
                facts |= box_fact::SUBTREE_DERIVED_ON_COMMIT;
            }
            *self.chunks[child_at.0].facts_mut(child_at.1) = facts;
            return;
        }
        if facts & box_fact::OWN_CHILDREN_DERIVED_ON_COMMIT != 0 {
            facts |= box_fact::CHILDREN_DERIVED_ON_COMMIT;
        }
        let ancestor_facts = if planned.has_flag(NodeFlag::Anonymous) {
            self.ancestor_facts_of_children(parent, self.node(parent), self.style_of(parent), true)
        } else {
            derivation.ancestor_facts
        };
        let is_box = crate::layout::node_facts::kind_is_box(planned.kind());
        let mut item_flag = None;
        if is_box && let Some(children_item_flag) = derivation.item_flag {
            // NB: The style of a box the walk planned is absolutely positioned where its facts say so.
            //     A table wrapper, whose style the commit makes, is where its facts say so.
            let is_absolutely_positioned = planned.has_fact(box_fact::ABSOLUTELY_POSITIONED)
                && match self.style_of(child) {
                    Some(style) => style.is_absolutely_positioned(),
                    None => planned.kind() == NodeKind::TableWrapper,
                };
            if !is_absolutely_positioned {
                let tree = &*self;
                let mut grandchild = planned.first_child();
                let children = std::iter::from_fn(|| {
                    if grandchild.is_invalid() {
                        return None;
                    }
                    let grandchild_box = tree.node(grandchild);
                    grandchild = grandchild_box.next_sibling();
                    // NB: The text of a new row is not synced yet.
                    Some((grandchild_box.kind(), None))
                });
                if anonymous_box_may_be_run_of_collapsible_whitespace(planned.row, children) {
                    facts |= if children_item_flag == NodeFlag::IsFlexItem {
                        box_fact::FLEX_ITEM_UNLESS_ONLY_TEXT
                    } else {
                        box_fact::GRID_ITEM_UNLESS_ONLY_TEXT
                    };
                } else {
                    item_flag = Some(children_item_flag);
                }
            }
        }
        let chunk = &mut self.chunks[child_at.0];
        *chunk.facts_mut(child_at.1) = facts;
        let row = chunk.row_mut(child_at.1);
        row.ancestor_facts = ancestor_facts;
        if is_box {
            let flags = row.flags.get_mut();
            *flags &= !(NodeFlag::IsFlexItem as u32 | NodeFlag::IsGridItem as u32);
            if let Some(item_flag) = item_flag {
                *flags |= item_flag as u32;
            }
        }
    }

    /// Gives each row committed one by one the facts its box has once the walk is done, which a move of the box after
    /// the row was listed may have changed.
    pub(crate) fn refresh_facts_of_rows_committed_one_by_one(&mut self) {
        for index in 0..self.rows_committed_one_by_one.len() {
            let slot = self.rows_committed_one_by_one[index].slot;
            self.rows_committed_one_by_one[index].facts = self.node(slot).facts();
        }
    }

    /// Moves `child` to the end of the children of `new_parent`, deriving again what it takes from its parent, and what
    /// its descendants take from theirs where that changes.
    pub(crate) fn move_child(&mut self, child: NodeSlotId, new_parent: NodeSlotId) {
        if self.is_stand_in(child) {
            self.moved_stand_in = true;
        }
        let derivation_before = (!self.node(child).first_child().is_invalid()).then(|| self.children_derivation(child));
        self.detach(child);
        self.attach(new_parent, child, NodeSlotId::INVALID);
        if let Some(derivation_before) = derivation_before {
            self.derive_children_again_if_changed(child, derivation_before);
        }
    }

    fn derive_children_again_if_changed(&mut self, node: NodeSlotId, derivation_before: ChildrenDerivation) {
        let derivation = self.children_derivation(node);
        if derivation == derivation_before {
            return;
        }
        self.forget_edge_derivation_of(node);
        if let Some(open) = self.open_parents.iter_mut().find(|(open, ..)| *open == node) {
            open.1 = derivation;
        }
        let mut child = self.node(node).first_child();
        // NB: The rows jobs made below a box are the plan's no more once they ran; a walk moves no row after.
        while !child.is_invalid() {
            let child_derivation_before =
                (!self.node(child).first_child().is_invalid()).then(|| self.children_derivation(child));
            let child_at = self.locate(child);
            self.derive_attached_row(node, child, child_at, &derivation);
            if let Some(child_derivation_before) = child_derivation_before {
                self.derive_children_again_if_changed(child, child_derivation_before);
            }
            child = self.node(child).next_sibling();
        }
    }

    /// Starts numbering the rows appended from now on in pre-order below `name`, and answers how far the positions of
    /// the rows numbered below the name before reach.
    pub(crate) fn start_pre_order_name(&mut self, name: u32) -> u64 {
        let extent = (self.pre_order_rows + 1) * PRE_ORDER_POSITION_STEP;
        self.pre_order_name = name;
        self.pre_order_rows = 0;
        extent
    }

    /// Whether the build put rows out of the order their pre-order positions say.
    pub(crate) fn pre_order_positions_out_of_order(&self) -> bool {
        self.pre_order_positions_out_of_order
    }

    #[inline]
    fn pre_order_position(&self, slot: NodeSlotId) -> u64 {
        let (position, offset) = self.locate(slot);
        self.chunks[position].pre_order_position(offset)
    }

    /// The pre-order position of a row attached below `parent` before `before`, or last for an invalid one. A row
    /// appended where the walk is, after every row in pre-order, takes the next position of the current name; a row
    /// put anywhere else takes a position between those of the rows it goes between.
    fn pre_order_position_of_attached_row(&mut self, parent: NodeSlotId, before: NodeSlotId) -> u64 {
        let following = if before.is_invalid() {
            self.following_subtree(parent)
        } else {
            before
        };
        if following.is_invalid() {
            self.pre_order_rows += 1;
            return packed_pre_order_position(self.pre_order_name, self.pre_order_rows * PRE_ORDER_POSITION_STEP);
        }
        let preceding_sibling = if before.is_invalid() {
            self.node(parent).last_child()
        } else {
            self.node(before).previous_sibling()
        };
        let mut preceding = parent;
        if !preceding_sibling.is_invalid() {
            preceding = preceding_sibling;
            while !self.node(preceding).last_child().is_invalid() {
                preceding = self.node(preceding).last_child();
            }
        }
        // NB: Rows numbered below another name, and the rows jobs make, come before all rows numbered below the name of
        //     the following row.
        let (name, position) = unpacked_pre_order_position(self.pre_order_position(following));
        let floor = if self.is_stand_in(preceding) {
            0
        } else {
            let (preceding_name, preceding_position) = unpacked_pre_order_position(self.pre_order_position(preceding));
            if preceding_name == name { preceding_position } else { 0 }
        };
        let between = floor + (position - floor) / 2;
        assert!(between > floor, "a build puts few rows between the same two rows");
        packed_pre_order_position(name as u32, between)
    }

    /// The row after the subtree of `node` in pre-order, or none where the subtree is the last the walk made: that of a
    /// box whose child list it is in, or of the last child of one.
    fn following_subtree(&self, node: NodeSlotId) -> NodeSlotId {
        let mut current = node;
        loop {
            if self.is_open_parent(current) {
                return NodeSlotId::INVALID;
            }
            let current_box = self.node(current);
            if !current_box.next_sibling().is_invalid() {
                return current_box.next_sibling();
            }
            if current_box.parent().is_invalid() {
                return NodeSlotId::INVALID;
            }
            current = current_box.parent();
        }
    }

    /// Links `child`, which has no parent, into `parent`'s children before `before`, or last for an invalid one, and
    /// gives it what it takes from `parent` (see [`Self::derive_attached_row`]) and its pre-order position, unless it has
    /// one: a row moved keeps its place in pre-order.
    pub(crate) fn attach(&mut self, parent: NodeSlotId, child: NodeSlotId, before: NodeSlotId) {
        let top = self.open_parents.last().copied().filter(|&(open, ..)| open == parent);
        let child_at = self.locate(child);
        let parent_at = match top {
            Some((_, _, position, offset)) => (position as usize, offset as usize),
            None => self.locate(parent),
        };
        debug_assert!(self.box_at(child_at).parent().is_invalid());
        let moved = self.chunks[child_at.0].pre_order_position(child_at.1) != 0;
        if !moved {
            let position = if before.is_invalid() && top.is_some() {
                self.pre_order_rows += 1;
                packed_pre_order_position(self.pre_order_name, self.pre_order_rows * PRE_ORDER_POSITION_STEP)
            } else {
                self.pre_order_position_of_attached_row(parent, before)
            };
            self.chunks[child_at.0].set_pre_order_position(child_at.1, position);
        }
        let previous = if before.is_invalid() {
            self.box_at(parent_at).last_child()
        } else {
            debug_assert_eq!(self.node(before).parent(), parent);
            self.node(before).previous_sibling()
        };
        {
            let row = self.chunks[child_at.0].row_mut(child_at.1);
            *row.parent.get_mut() = parent;
            *row.previous_sibling.get_mut() = previous;
            *row.next_sibling.get_mut() = before;
        }
        if previous.is_invalid() {
            *self.chunks[parent_at.0].row_mut(parent_at.1).first_child.get_mut() = child;
        } else {
            *self.row_mut(previous).next_sibling.get_mut() = child;
        }
        if before.is_invalid() {
            *self.chunks[parent_at.0].row_mut(parent_at.1).last_child.get_mut() = child;
        } else {
            *self.row_mut(before).previous_sibling.get_mut() = child;
        }
        let derivation = match top {
            Some((_, derivation, ..)) => derivation,
            None => self.derivation_for(parent),
        };
        self.derive_attached_row(parent, child, child_at, &derivation);
        let child_box = self.box_at(child_at);
        if child_box.has_fact(box_fact::TABLE_PART) {
            self.note_table_part_child(parent_at);
        }
        let child_box = self.box_at(child_at);
        let is_inline = child_box.has_fact(box_fact::INLINE_OUTSIDE);
        let is_out_of_flow = child_box.is_out_of_flow();
        let is_text = crate::layout::node_facts::kind_is_text(child_box.kind());
        let parent_chunk = &mut self.chunks[parent_at.0];
        let parent_facts = parent_chunk.facts_mut(parent_at.1);
        if is_inline || !is_out_of_flow {
            *parent_facts |= box_fact::HAS_INLINE_OR_IN_FLOW_CHILDREN;
        }
        if !is_inline && !is_out_of_flow {
            *parent_facts |= box_fact::HAS_IN_FLOW_BLOCK_CHILDREN;
        }
        // NB: An anonymous box among the items of a flex or grid container is one once it holds a box that is not text.
        let awaited_item =
            *parent_facts & (box_fact::FLEX_ITEM_UNLESS_ONLY_TEXT | box_fact::GRID_ITEM_UNLESS_ONLY_TEXT);
        if awaited_item != 0 && !is_text {
            *parent_facts &= !awaited_item;
            if self.edge_derivation.is_some_and(|(edge, _)| edge == parent) {
                self.edge_derivation = None;
            }
            let parent_chunk = &mut self.chunks[parent_at.0];
            let item_flag = if awaited_item == box_fact::FLEX_ITEM_UNLESS_ONLY_TEXT {
                NodeFlag::IsFlexItem
            } else {
                NodeFlag::IsGridItem
            };
            *parent_chunk.row_mut(parent_at.1).flags.get_mut() |= item_flag as u32;
        }
    }

    /// Notes on the box at `parent_at`, and on the boxes whose child lists it is in through anonymous boxes, that a part
    /// of a table is among its children.
    #[cold]
    fn note_table_part_child(&mut self, parent_at: (usize, usize)) {
        let mut at = parent_at;
        loop {
            *self.chunks[at.0].facts_mut(at.1) |= box_fact::HAS_TABLE_PART_CHILD;
            let parent = self.box_at(at);
            let grandparent = parent.parent();
            if grandparent.is_invalid()
                || !parent.has_flag(NodeFlag::Anonymous)
                || parent.has_flag(NodeFlag::IsPseudoElementPrincipalBox)
            {
                return;
            }
            at = self.locate(grandparent);
        }
    }

    /// Whether table fixup goes over the child list of `node`, a box whose children the walk planned all of, see
    /// [`Self::table_fixup_lists`].
    pub(crate) fn note_table_fixup_list_if_needed(&mut self, node: NodeSlotId) {
        if self
            .node(node)
            .has_fact(box_fact::HAS_TABLE_PART_CHILD | box_fact::TABULAR_CONTAINER)
        {
            self.table_fixup_lists.push(node);
        }
    }

    /// Leaves what `node`, a row the walk attached, and every row below it take from their parents to the commit, which
    /// derives them once their anonymous styles are made.
    fn derive_subtree_on_commit(&mut self, node: NodeSlotId) {
        *self.facts_mut(node) |=
            box_fact::DERIVED_ON_COMMIT | box_fact::SUBTREE_DERIVED_ON_COMMIT | box_fact::CHILDREN_DERIVED_ON_COMMIT;
    }

    /// Unlinks `child` from its parent. The facts its parent's children set stay: a child moves only into a wrapper
    /// that takes its place.
    pub(crate) fn detach(&mut self, child: NodeSlotId) {
        self.forget_edge_derivation_of(child);
        let node = self.node(child);
        let (parent, previous, next) = (node.parent(), node.previous_sibling(), node.next_sibling());
        debug_assert!(!parent.is_invalid());
        if previous.is_invalid() {
            *self.row_mut(parent).first_child.get_mut() = next;
        } else {
            *self.row_mut(previous).next_sibling.get_mut() = next;
        }
        if next.is_invalid() {
            *self.row_mut(parent).last_child.get_mut() = previous;
        } else {
            *self.row_mut(next).previous_sibling.get_mut() = previous;
        }
        let row = self.row_mut(child);
        *row.parent.get_mut() = NodeSlotId::INVALID;
        *row.previous_sibling.get_mut() = NodeSlotId::INVALID;
        *row.next_sibling.get_mut() = NodeSlotId::INVALID;
    }
}
