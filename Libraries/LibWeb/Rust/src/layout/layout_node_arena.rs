/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;
use crate::layout::AbsposLayoutInputs;
use crate::layout::AvailableSize;
use crate::layout::CssPixels;
use crate::layout::FfiReplacedContentFacts;
use crate::layout::UsedValues;
use crate::layout::node_data::{FfiStylePayloads, MAX_NODE_SLOT_COUNT, NodeData, NodeFlag, NodeKind, NodeSlotId};
use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::hash::{Hash, Hasher};
use std::rc::Rc;
use std::thread;

pub(crate) const SLOTS_PER_CHUNK: usize = 256;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct IntrinsicSizeCacheKey {
    pub(crate) measured_at_inline_size: Option<CssPixels>,
    pub(crate) measured_at_block_size: Option<CssPixels>,
    pub(crate) percentage_basis_inline_size: Option<CssPixels>,
    pub(crate) percentage_basis_block_size: Option<CssPixels>,
    pub(crate) quirks_mode_percentage_basis_block_size: Option<CssPixels>,
}

impl Hash for IntrinsicSizeCacheKey {
    fn hash<H: Hasher>(&self, state: &mut H) {
        fn hash_optional<H: Hasher>(value: Option<CssPixels>, state: &mut H) {
            match value {
                Some(value) => {
                    true.hash(state);
                    value.raw_value().hash(state);
                }
                None => false.hash(state),
            }
        }

        hash_optional(self.measured_at_inline_size, state);
        hash_optional(self.measured_at_block_size, state);
        hash_optional(self.percentage_basis_inline_size, state);
        hash_optional(self.percentage_basis_block_size, state);
        hash_optional(self.quirks_mode_percentage_basis_block_size, state);
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum IntrinsicSizeCacheKind {
    MinContentInline,
    MaxContentInline,
    MinContentBlock,
    MaxContentBlock,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct IntrinsicInlineSizeMeasurement {
    pub(crate) automatic_content_inline_size: CssPixels,
    pub(crate) min_content_inline_size_from_max_content_layout: Option<CssPixels>,
    pub(crate) available_block_size: AvailableSize,
    pub(crate) content_inline_size: CssPixels,
    pub(crate) content_block_size: CssPixels,
    pub(crate) automatic_content_block_size: CssPixels,
    pub(crate) uses_collapsing_borders_model: bool,
    pub(crate) has_first_baseline: bool,
    pub(crate) first_baseline: CssPixels,
    pub(crate) has_last_baseline: bool,
    pub(crate) last_baseline: CssPixels,
}

#[derive(Default)]
struct IntrinsicSizeMaps {
    // This is a subtree fact and shares the intrinsic cache's epoch so descendant changes invalidate it.
    inline_size_depends_on_block_size: Option<bool>,
    min_content_inline_size: HashMap<IntrinsicSizeCacheKey, IntrinsicInlineSizeMeasurement>,
    max_content_inline_size: HashMap<IntrinsicSizeCacheKey, IntrinsicInlineSizeMeasurement>,
    min_content_block_size: HashMap<IntrinsicSizeCacheKey, CssPixels>,
    max_content_block_size: HashMap<IntrinsicSizeCacheKey, CssPixels>,
}

impl IntrinsicSizeMaps {
    fn block_sizes(&self, kind: IntrinsicSizeCacheKind) -> Option<&HashMap<IntrinsicSizeCacheKey, CssPixels>> {
        match kind {
            IntrinsicSizeCacheKind::MinContentInline | IntrinsicSizeCacheKind::MaxContentInline => None,
            IntrinsicSizeCacheKind::MinContentBlock => Some(&self.min_content_block_size),
            IntrinsicSizeCacheKind::MaxContentBlock => Some(&self.max_content_block_size),
        }
    }

    fn block_sizes_mut(
        &mut self,
        kind: IntrinsicSizeCacheKind,
    ) -> Option<&mut HashMap<IntrinsicSizeCacheKey, CssPixels>> {
        match kind {
            IntrinsicSizeCacheKind::MinContentInline | IntrinsicSizeCacheKind::MaxContentInline => None,
            IntrinsicSizeCacheKind::MinContentBlock => Some(&mut self.min_content_block_size),
            IntrinsicSizeCacheKind::MaxContentBlock => Some(&mut self.max_content_block_size),
        }
    }

    fn inline_measurements(
        &self,
        kind: IntrinsicSizeCacheKind,
    ) -> Option<&HashMap<IntrinsicSizeCacheKey, IntrinsicInlineSizeMeasurement>> {
        match kind {
            IntrinsicSizeCacheKind::MinContentInline => Some(&self.min_content_inline_size),
            IntrinsicSizeCacheKind::MaxContentInline => Some(&self.max_content_inline_size),
            IntrinsicSizeCacheKind::MinContentBlock | IntrinsicSizeCacheKind::MaxContentBlock => None,
        }
    }

    fn inline_measurements_mut(
        &mut self,
        kind: IntrinsicSizeCacheKind,
    ) -> Option<&mut HashMap<IntrinsicSizeCacheKey, IntrinsicInlineSizeMeasurement>> {
        match kind {
            IntrinsicSizeCacheKind::MinContentInline => Some(&mut self.min_content_inline_size),
            IntrinsicSizeCacheKind::MaxContentInline => Some(&mut self.max_content_inline_size),
            IntrinsicSizeCacheKind::MinContentBlock | IntrinsicSizeCacheKind::MaxContentBlock => None,
        }
    }
}

#[derive(Default)]
struct IntrinsicSizeCacheSlot {
    generation: u8,
    epoch: u16,
    sizes: Option<Box<IntrinsicSizeMaps>>,
}

#[derive(Default)]
struct SavedAbsposLayoutInputsSlot {
    generation: u8,
    inputs: Option<Box<AbsposLayoutInputs>>,
}

#[derive(Default)]
struct SavedCommittedGeometrySlot {
    generation: u8,
    geometry: Option<Box<crate::layout::FfiPaintableGeometry>>,
}

#[derive(Default)]
pub(crate) struct TextContent {
    pub(crate) text: Vec<u16>,
    pub(crate) untransformed_text_is_ascii_whitespace: bool,
    pub(crate) may_require_bidi_processing: bool,
    pub(crate) dom_start_offset: usize,
    grapheme_segmenter: std::cell::OnceCell<crate::layout::GraphemeSegmenter>,
}

impl TextContent {
    pub(crate) fn grapheme_segmenter(&self) -> &crate::layout::GraphemeSegmenter {
        self.grapheme_segmenter
            .get_or_init(|| crate::layout::GraphemeSegmenter::new(&self.text))
    }
}

#[derive(Default)]
struct TextContentSlot {
    generation: u8,
    content: Option<Box<TextContent>>,
}

#[derive(Default)]
struct ReplacedContentFactsSlot {
    generation: u8,
    facts: Option<FfiReplacedContentFacts>,
}

#[derive(Clone, Copy, PartialEq)]
pub(crate) struct TextChunkCacheKey {
    pub(crate) should_wrap_lines: bool,
    pub(crate) should_respect_linebreaks: bool,
    pub(crate) unidirectional_ltr: bool,
    pub(crate) white_space_collapse: u8,
    pub(crate) word_break: u8,
    pub(crate) font_variant_emoji: u8,
    pub(crate) font_cascade_list: *const c_void,
}

struct TextChunkCacheEntry {
    key: TextChunkCacheKey,
    _retained_font_cascade_list: libgfx_rust::font::RetainedFontCascadeList,
    chunks: Vec<crate::layout::TextChunk>,
}

#[derive(Default)]
struct TextChunkCacheSlot {
    generation: u8,
    entry: Option<Box<TextChunkCacheEntry>>,
}

#[derive(Default)]
struct RunRecordSlot {
    nonce: u64, // 0 = vacant
    record: Option<Rc<UsedValues>>,
}

// NodeData is sized to one cache line; the aligned chunk keeps every densely-strided slot
// line-aligned, and per-slot bookkeeping lives in a parallel array so it stays that way.
#[repr(align(64))]
pub(crate) struct Chunk {
    slots: [NodeData; SLOTS_PER_CHUNK],
}

fn new_chunk() -> Box<Chunk> {
    // SAFETY: Every slot is written with NodeData::default() before the chunk is exposed. The
    // chunk is built in place on the heap because it is far too large for the stack.
    unsafe {
        let mut chunk = Box::<Chunk>::new_uninit();
        let slots = &raw mut (*chunk.as_mut_ptr()).slots;
        for offset in 0..SLOTS_PER_CHUNK {
            (&raw mut (*slots)[offset]).write(NodeData::default());
        }
        chunk.assume_init()
    }
}

#[derive(Clone, Copy, Default)]
struct SlotMetadata {
    generation: u8,
    occupied: bool,
}

#[derive(Clone, Copy)]
struct ChunkAddress {
    start: usize,
    chunk_index: usize,
}

pub(crate) struct LayoutNodeArena {
    chunks: Vec<Box<Chunk>>,
    chunks_by_address: Vec<ChunkAddress>,
    slot_metadata: Vec<SlotMetadata>,
    free_list: Vec<u32>,
    next_index: u32,
    live_count: u32,
    intrinsic_size_caches: RefCell<Vec<IntrinsicSizeCacheSlot>>,
    saved_abspos_layout_inputs: RefCell<Vec<SavedAbsposLayoutInputsSlot>>,
    saved_committed_geometries: RefCell<Vec<SavedCommittedGeometrySlot>>,
    text_contents: Vec<TextContentSlot>,
    text_chunk_caches: RefCell<Vec<TextChunkCacheSlot>>,
    replaced_content_facts: Vec<ReplacedContentFactsSlot>,
    raw_table_column_spans: HashMap<NodeSlotId, u32>,
    run_used_records: RefCell<Vec<RunRecordSlot>>,
    next_run_nonce: Cell<u64>,
    fc_run_cache_store: crate::layout::FcRunCacheArenaStore,
    paintables: RefCell<crate::painting::paintable_arena::PaintableArena>,
    svg_pattern_referencing_nodes: RefCell<Vec<NodeSlotId>>,
    owner_thread: thread::ThreadId,
}

impl LayoutNodeArena {
    pub(crate) fn new() -> Self {
        Self {
            chunks: Vec::new(),
            chunks_by_address: Vec::new(),
            slot_metadata: Vec::new(),
            free_list: Vec::new(),
            next_index: 0,
            live_count: 0,
            intrinsic_size_caches: RefCell::new(Vec::new()),
            saved_abspos_layout_inputs: RefCell::new(Vec::new()),
            saved_committed_geometries: RefCell::new(Vec::new()),
            text_contents: Vec::new(),
            text_chunk_caches: RefCell::new(Vec::new()),
            replaced_content_facts: Vec::new(),
            raw_table_column_spans: HashMap::new(),
            run_used_records: RefCell::new(Vec::new()),
            next_run_nonce: Cell::new(1),
            fc_run_cache_store: crate::layout::FcRunCacheArenaStore::default(),
            paintables: RefCell::new(crate::painting::paintable_arena::PaintableArena::new()),
            svg_pattern_referencing_nodes: RefCell::new(Vec::new()),
            owner_thread: thread::current().id(),
        }
    }

    pub(crate) fn register_svg_pattern_referencing_node(&self, node: NodeSlotId) {
        let mut nodes = self.svg_pattern_referencing_nodes.borrow_mut();
        nodes.retain(|candidate| !self.shell_if_live(*candidate).is_null());
        if nodes.contains(&node) {
            return;
        }
        nodes.push(node);
    }

    pub(crate) fn svg_pattern_referencing_nodes(&self) -> Vec<NodeSlotId> {
        let mut nodes = self.svg_pattern_referencing_nodes.borrow_mut();
        nodes.retain(|candidate| !self.shell_if_live(*candidate).is_null());
        nodes.clone()
    }

    pub(crate) fn fc_run_cache_store(&self) -> &crate::layout::FcRunCacheArenaStore {
        &self.fc_run_cache_store
    }

    /// Drops entries whose slot or epoch no longer matches.
    /// Runs at the end of every full pass so invalidated entries whose box
    /// never probes again do not accumulate for the document's lifetime.
    pub(crate) fn sweep_stale_fc_run_cache_entries(&self) {
        self.fc_run_cache_store.retain_entries(|slot, validity| {
            let Some(metadata) = self.slot_metadata.get(slot as usize) else {
                return false;
            };
            if !metadata.occupied || metadata.generation != validity.slot_generation {
                return false;
            }
            let id = NodeSlotId::new(slot, metadata.generation);
            // SAFETY: The slot is occupied at the matching generation, so the
            // pointer addresses a live NodeData.
            unsafe { (*self.data(id)).fragment_cache_epoch == validity.fragment_cache_epoch }
        });
    }

    fn assert_owner_thread(&self) {
        debug_assert_eq!(self.owner_thread, thread::current().id());
    }

    // Freshly created chunks are default-initialized and free() resets slots on release, so
    // allocate() always hands out clean NodeData without writing it again.
    pub(crate) fn allocate(&mut self) -> NodeAllocation {
        self.assert_owner_thread();

        let index = if let Some(index) = self.free_list.pop() {
            index
        } else {
            let index = self.next_index;
            assert!(
                index < MAX_NODE_SLOT_COUNT,
                "layout node arena exhausted its 24-bit slot index space"
            );
            if (index as usize).is_multiple_of(SLOTS_PER_CHUNK) {
                let chunk = new_chunk();
                let start = (&raw const chunk.slots) as usize;
                let chunk_index = self.chunks.len();
                let insertion_index = self.chunks_by_address.partition_point(|address| address.start < start);
                self.chunks_by_address
                    .insert(insertion_index, ChunkAddress { start, chunk_index });
                self.chunks.push(chunk);
            }
            self.slot_metadata.push(SlotMetadata::default());
            // Grown with the slot space up front: nearly every slot gets a run
            // record each layout pass, so register() never has to resize.
            self.run_used_records.get_mut().push(RunRecordSlot::default());
            self.next_index = self
                .next_index
                .checked_add(1)
                .expect("layout node arena exhausted its slot ID space");
            index
        };

        self.live_count = self
            .live_count
            .checked_add(1)
            .expect("layout node arena live count overflowed");

        let metadata = self.metadata_mut(index);
        assert!(!metadata.occupied, "layout node arena allocated a live slot");
        metadata.generation = metadata
            .generation
            .checked_add(1)
            .expect("retired layout node arena slot was reused");
        metadata.occupied = true;
        let generation = metadata.generation;
        self.data_mut(index).slot_generation = generation;

        NodeAllocation {
            slot: NodeSlotId::new(index, generation),
            data: self.data_mut(index),
            generation: u32::from(generation),
        }
    }

    pub(crate) fn free(&mut self, id: NodeSlotId, generation: u32) {
        self.assert_owner_thread();

        assert!(!id.is_invalid(), "invalid layout node arena slot ID");
        let index = id.slot_index();
        let id_generation = id.generation();
        assert_eq!(
            u32::from(id_generation),
            generation,
            "layout node arena slot ID and allocation generation disagree"
        );
        let metadata = self.metadata_mut(index);
        assert!(metadata.occupied, "layout node arena freed an unused slot");
        assert_eq!(
            metadata.generation, id_generation,
            "layout node arena freed a stale slot generation"
        );
        metadata.occupied = false;
        let should_reuse = metadata.generation != u8::MAX;

        if let Some(slot) = self.intrinsic_size_caches.get_mut().get_mut(index as usize) {
            *slot = IntrinsicSizeCacheSlot::default();
        }
        if let Some(slot) = self.saved_abspos_layout_inputs.get_mut().get_mut(index as usize) {
            *slot = SavedAbsposLayoutInputsSlot::default();
        }
        if let Some(slot) = self.saved_committed_geometries.get_mut().get_mut(index as usize) {
            *slot = SavedCommittedGeometrySlot::default();
        }
        if let Some(slot) = self.text_contents.get_mut(index as usize) {
            *slot = TextContentSlot::default();
        }
        if let Some(slot) = self.text_chunk_caches.get_mut().get_mut(index as usize) {
            *slot = TextChunkCacheSlot::default();
        }
        if let Some(slot) = self.replaced_content_facts.get_mut(index as usize) {
            *slot = ReplacedContentFactsSlot::default();
        }
        // free() never interleaves with a layout pass (C++ is blocked on the
        // synchronous FFI entry), so a live record here means a run leaked.
        if let Some(slot) = self.run_used_records.get_mut().get_mut(index as usize) {
            debug_assert!(
                slot.record.is_none(),
                "layout node arena freed a slot with a live run record"
            );
            *slot = RunRecordSlot::default();
        }
        self.fc_run_cache_store.remove_entry(index);
        self.raw_table_column_spans.remove(&id);
        self.paintables.get_mut().layout_node_freed(index);
        let data = self.data_mut(index);
        debug_assert!(
            data.parent.is_invalid()
                && data.first_child.is_invalid()
                && data.last_child.is_invalid()
                && data.previous_sibling.is_invalid()
                && data.next_sibling.is_invalid(),
            "layout node arena freed a slot that is still linked into a tree"
        );
        *data = NodeData::default();

        self.live_count = self
            .live_count
            .checked_sub(1)
            .expect("layout node arena live count underflowed");
        if should_reuse {
            self.free_list.push(index);
        }
    }

    pub(crate) fn data(&self, id: NodeSlotId) -> *mut NodeData {
        assert!(!id.is_invalid(), "invalid layout node arena slot ID");
        let index = id.slot_index() as usize;
        let chunk = self
            .chunks
            .get(index / SLOTS_PER_CHUNK)
            .expect("invalid layout node arena slot ID");
        let data = (&raw const chunk.slots[index % SLOTS_PER_CHUNK]).cast_mut();
        // SAFETY: The chunk bounds check above established that data addresses
        // an initialized NodeData slot.
        let generation = unsafe { (&raw const (*data).slot_generation).read() };
        assert_eq!(
            generation,
            id.generation(),
            "layout node arena read a stale or unused slot"
        );
        data
    }

    pub(crate) fn set_node_flag(&self, id: NodeSlotId, flag: NodeFlag, value: bool) {
        self.assert_owner_thread();
        let data = self.data(id);
        // SAFETY: data() validated that id names a live slot, and layout
        // serializes mutation on the arena's owner thread.
        unsafe {
            let flags = &raw mut (*data).flags;
            let mut updated = flags.read();
            if value {
                updated |= flag as u32;
            } else {
                updated &= !(flag as u32);
            }
            flags.write(updated);
        }
    }

    pub(crate) fn reset_layout_update_flags_in_subtree(&self, root: NodeSlotId) {
        self.assert_owner_thread();
        let flags_to_clear = NodeFlag::NeedsLayoutUpdate as u32 | NodeFlag::NeedsOwnGeometryUpdate as u32;
        let mut current = root;
        loop {
            let data = self.data(current);
            // SAFETY: data() validated that current names a live slot, and layout tree mutation
            // is serialized on the arena's owner thread.
            let (parent, first_child, next_sibling) = unsafe {
                let flags = &raw mut (*data).flags;
                flags.write(flags.read() & !flags_to_clear);
                (
                    (&raw const (*data).parent).read(),
                    (&raw const (*data).first_child).read(),
                    (&raw const (*data).next_sibling).read(),
                )
            };

            if !first_child.is_invalid() {
                current = first_child;
                continue;
            }
            if current == root {
                break;
            }
            if !next_sibling.is_invalid() {
                current = next_sibling;
                continue;
            }

            current = parent;
            while current != root {
                // SAFETY: Parent links from a live subtree node name live slots in the same tree.
                let data = self.data(current);
                let next_sibling = unsafe { (&raw const (*data).next_sibling).read() };
                if !next_sibling.is_invalid() {
                    current = next_sibling;
                    break;
                }
                // SAFETY: data() validated current and the topology is stable during this call.
                current = unsafe { (&raw const (*data).parent).read() };
            }
            if current == root {
                break;
            }
        }
    }

    fn node_is_capable_of_forming_a_containing_block(&self, id: NodeSlotId) -> bool {
        // SAFETY: data() validated that id names a live slot.
        let data = unsafe { &*self.data(id) };
        if crate::layout::kind_is_block_container(data.kind)
            && !crate::painting::fragment_ownership::node_is_fragmented_inline(self, id)
        {
            return true;
        }
        if let Some(style) = self.node_style_if_live(id) {
            let display = style.display();
            if display.is_flex_inside() || display.is_grid_inside() {
                return true;
            }
        }
        crate::layout::kind_is_replaced_box(data.kind) && crate::layout::node_can_have_children(data)
    }

    fn nearest_ancestor_capable_of_forming_a_containing_block(&self, node: NodeSlotId) -> NodeSlotId {
        // SAFETY: Callers pass a node validated by the subtree walk, and parent
        // links from a live tree node name live slots.
        let mut ancestor = unsafe { (&raw const (*self.data(node)).parent).read() };
        while !ancestor.is_invalid() {
            if self.node_is_capable_of_forming_a_containing_block(ancestor) {
                return ancestor;
            }
            // SAFETY: As above.
            ancestor = unsafe { (&raw const (*self.data(ancestor)).parent).read() };
        }
        NodeSlotId::INVALID
    }

    fn recompute_containing_block_for_node(
        &self,
        node: NodeSlotId,
        inline_cb_lookup: unsafe extern "C" fn(*mut c_void, *mut c_void) -> NodeSlotId,
    ) {
        use crate::css::css_enums::positioning;
        use crate::painting::style_queries::establishes_positioning_containing_blocks;

        let data = self.data(node);
        // Reset the inline containing block - we'll set it below if applicable.
        // SAFETY: data() validated that node names a live slot, and layout
        // serializes arena mutation on the owner thread.
        unsafe { (&raw mut (*data).inline_containing_block).write(NodeSlotId::INVALID) };

        // SAFETY: As above.
        let kind = unsafe { (&raw const (*data).kind).read() };
        if crate::layout::kind_is_text(kind) {
            let containing_block = self.nearest_ancestor_capable_of_forming_a_containing_block(node);
            // SAFETY: As above.
            unsafe { (&raw mut (*data).containing_block).write(containing_block) };
            return;
        }

        let position = self
            .node_style_if_live(node)
            .map_or(positioning::STATIC, |style| style.box_values().position);

        // https://drafts.csswg.org/css-position-3/#absolute-cb
        if position == positioning::ABSOLUTE {
            // SAFETY: As above; parent links from a live tree name live slots.
            let mut ancestor = unsafe { (&raw const (*data).parent).read() };
            while !ancestor.is_invalid() && !establishes_positioning_containing_blocks(self, ancestor).0 {
                // SAFETY: As above.
                ancestor = unsafe { (&raw const (*self.data(ancestor)).parent).read() };
            }
            // SAFETY: As above.
            unsafe { (&raw mut (*data).containing_block).write(ancestor) };
            if !ancestor.is_invalid() {
                // SAFETY: Both slots are live; the callback only reads DOM ancestry
                // and per-node facts through the shells and does not mutate the tree.
                let inline_containing_block = unsafe {
                    let node_shell = (&raw const (*data).shell).read();
                    let ancestor_shell = (&raw const (*self.data(ancestor)).shell).read();
                    inline_cb_lookup(node_shell, ancestor_shell)
                };
                // SAFETY: As above.
                unsafe { (&raw mut (*data).inline_containing_block).write(inline_containing_block) };
            }
            return;
        }

        // https://drafts.csswg.org/css-position-3/#fixed-cb
        if position == positioning::FIXED {
            // The containing block is established by the nearest ancestor box that establishes an fixed positioning
            // containing block, with the bounds of the containing block determined identically to the absolute positioning
            // containing block.
            let mut last_visited = node;
            // SAFETY: As above.
            let mut ancestor = unsafe { (&raw const (*data).parent).read() };
            while !ancestor.is_invalid() && !establishes_positioning_containing_blocks(self, ancestor).1 {
                last_visited = ancestor;
                // SAFETY: As above.
                ancestor = unsafe { (&raw const (*self.data(ancestor)).parent).read() };
            }
            // If no ancestor establishes one, the box's fixed positioning containing block is the initial fixed containing
            // block:
            //  - in continuous media, the layout viewport (whose size matches the dynamic viewport size); as a result,
            //    fixed boxes do not move when the document is scrolled.
            // FIXME: - in paged media, the page area of each page; fixed positioned boxes are thus replicated on every
            //   page. (They are fixed with respect to the page box only, and are not affected by being seen through a
            //   viewport; as in the case of print preview, for example.)
            let containing_block = if ancestor.is_invalid() { last_visited } else { ancestor };
            // SAFETY: As above.
            unsafe { (&raw mut (*data).containing_block).write(containing_block) };
            return;
        }

        let containing_block = self.nearest_ancestor_capable_of_forming_a_containing_block(node);
        // SAFETY: As above.
        unsafe { (&raw mut (*data).containing_block).write(containing_block) };
    }

    fn derive_abspos_escape_flags_for_node(&self, node: NodeSlotId) {
        let data = self.data(node);
        // SAFETY: data() validated that node names a live slot.
        let kind = unsafe { (&raw const (*data).kind).read() };
        if !crate::layout::kind_is_box(kind) {
            return;
        }
        self.set_node_flag(node, NodeFlag::AbsposDescendantEscapes, false);
        if !self
            .node_style_if_live(node)
            .is_some_and(|style| style.is_absolutely_positioned())
        {
            return;
        }
        // SAFETY: As above; parent links from a live tree name live slots.
        let containing_block = unsafe { (&raw const (*data).containing_block).read() };
        let mut ancestor = unsafe { (&raw const (*data).parent).read() };
        while !ancestor.is_invalid() && ancestor != containing_block {
            // SAFETY: As above.
            let ancestor_kind = unsafe { (&raw const (*self.data(ancestor)).kind).read() };
            if crate::layout::kind_is_box(ancestor_kind) {
                self.set_node_flag(ancestor, NodeFlag::AbsposDescendantEscapes, true);
            }
            // SAFETY: As above.
            ancestor = unsafe { (&raw const (*self.data(ancestor)).parent).read() };
        }
    }

    /// Recomputes `containing_block` and `inline_containing_block` and derives
    /// the `AbsposDescendantEscapes` flag for every node in the inclusive
    /// subtree of `root`. The pre-order traversal visits ancestors before the
    /// descendants that mark them, so clearing the flag on visit and marking
    /// upwards compose within one walk; the marking follows plain parent links
    /// and so reaches ancestors above `root` when the containing block lies
    /// outside the subtree.
    pub(crate) fn recompute_containing_blocks_in_subtree(
        &self,
        root: NodeSlotId,
        inline_cb_lookup: unsafe extern "C" fn(*mut c_void, *mut c_void) -> NodeSlotId,
    ) {
        self.assert_owner_thread();
        let mut current = root;
        loop {
            self.recompute_containing_block_for_node(current, inline_cb_lookup);
            self.derive_abspos_escape_flags_for_node(current);

            let data = self.data(current);
            // SAFETY: data() validated that current names a live slot, and the
            // topology is stable during this call.
            let (parent, first_child, next_sibling) = unsafe {
                (
                    (&raw const (*data).parent).read(),
                    (&raw const (*data).first_child).read(),
                    (&raw const (*data).next_sibling).read(),
                )
            };

            if !first_child.is_invalid() {
                current = first_child;
                continue;
            }
            if current == root {
                break;
            }
            if !next_sibling.is_invalid() {
                current = next_sibling;
                continue;
            }

            current = parent;
            while current != root {
                // SAFETY: Parent links from a live subtree node name live slots in the same tree.
                let data = self.data(current);
                let next_sibling = unsafe { (&raw const (*data).next_sibling).read() };
                if !next_sibling.is_invalid() {
                    current = next_sibling;
                    break;
                }
                // SAFETY: data() validated current and the topology is stable during this call.
                current = unsafe { (&raw const (*data).parent).read() };
            }
            if current == root {
                break;
            }
        }
    }

    fn slot_for_data(&self, data: *const NodeData) -> (u32, SlotMetadata) {
        assert!(!data.is_null(), "layout node arena data pointer is null");
        let data_address = data as usize;
        let slot_size = size_of::<NodeData>();

        let address_index = self
            .chunks_by_address
            .partition_point(|address| address.start <= data_address);
        assert_ne!(
            address_index, 0,
            "layout node data pointer does not belong to this arena"
        );
        let address = self.chunks_by_address[address_index - 1];
        let chunk_end = address.start + size_of::<[NodeData; SLOTS_PER_CHUNK]>();
        assert!(
            data_address < chunk_end,
            "layout node data pointer does not belong to this arena"
        );

        let offset = data_address - address.start;
        assert_eq!(offset % slot_size, 0, "unaligned layout node arena data pointer");
        let index = address.chunk_index * SLOTS_PER_CHUNK + offset / slot_size;
        let index = u32::try_from(index).expect("layout node arena slot index overflowed");
        let metadata = *self.metadata(index);
        assert!(metadata.occupied, "layout node arena access for an unused slot");
        // SAFETY: The range and alignment checks established that data points
        // to the indexed NodeData slot.
        let generation = unsafe { (&raw const (*data).slot_generation).read() };
        assert_eq!(
            generation, metadata.generation,
            "layout node arena access used a stale slot"
        );
        (index, metadata)
    }

    pub(crate) fn is_before(&self, node: &NodeData, other: &NodeData) -> bool {
        let (node_index, node_metadata) = self.slot_for_data(std::ptr::from_ref(node));
        let (other_index, other_metadata) = self.slot_for_data(std::ptr::from_ref(other));
        let mut node = NodeSlotId::new(node_index, node_metadata.generation);
        let mut other = NodeSlotId::new(other_index, other_metadata.generation);
        assert_ne!(node, other, "a layout node cannot precede itself");

        let depth = |mut slot: NodeSlotId| {
            let mut depth = 0usize;
            while !slot.is_invalid() {
                depth += 1;
                // SAFETY: data() validates the live generation, and the
                // topology links name slots in this arena.
                slot = unsafe { (*self.data(slot)).parent };
            }
            depth
        };
        let node_depth = depth(node);
        let other_depth = depth(other);

        for _ in other_depth..node_depth {
            // SAFETY: node is a validated live arena slot.
            node = unsafe { (*self.data(node)).parent };
        }
        for _ in node_depth..other_depth {
            // SAFETY: other is a validated live arena slot.
            other = unsafe { (*self.data(other)).parent };
        }
        if node == other {
            return node_depth < other_depth;
        }

        loop {
            // SAFETY: Both slots are live and have equal depth.
            let node_parent = unsafe { (*self.data(node)).parent };
            // SAFETY: Both slots are live and have equal depth.
            let other_parent = unsafe { (*self.data(other)).parent };
            assert_eq!(
                node_parent.is_invalid(),
                other_parent.is_invalid(),
                "layout nodes belong to different trees"
            );
            if node_parent == other_parent {
                break;
            }
            node = node_parent;
            other = other_parent;
        }

        while !other.is_invalid() {
            if node == other {
                return true;
            }
            // SAFETY: other is a validated live arena slot.
            other = unsafe { (*self.data(other)).previous_sibling };
        }
        false
    }

    pub(crate) fn intrinsic_block_size_cache_get(
        &self,
        data: &NodeData,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
    ) -> Option<CssPixels> {
        assert!(
            matches!(
                kind,
                IntrinsicSizeCacheKind::MinContentBlock | IntrinsicSizeCacheKind::MaxContentBlock
            ),
            "block size cache kind must use the block axis"
        );
        if data.intrinsic_cache_epoch == u16::MAX {
            return None;
        }

        let (index, metadata) = self.slot_for_data(std::ptr::from_ref(data));
        let caches = self.intrinsic_size_caches.borrow();
        let slot = caches.get(index as usize)?;
        if slot.generation != metadata.generation || slot.epoch != data.intrinsic_cache_epoch {
            return None;
        }
        slot.sizes
            .as_ref()?
            .block_sizes(kind)
            .expect("block size cache kind must use the block axis")
            .get(&key)
            .copied()
    }

    fn with_intrinsic_size_maps_mut(&self, data: &NodeData, callback: impl FnOnce(&mut IntrinsicSizeMaps)) {
        if data.intrinsic_cache_epoch == u16::MAX {
            return;
        }

        let (index, metadata) = self.slot_for_data(std::ptr::from_ref(data));
        let mut caches = self.intrinsic_size_caches.borrow_mut();
        if caches.len() <= index as usize {
            caches.resize_with(index as usize + 1, IntrinsicSizeCacheSlot::default);
        }
        let slot = &mut caches[index as usize];
        if slot.generation != metadata.generation || slot.epoch != data.intrinsic_cache_epoch {
            *slot = IntrinsicSizeCacheSlot {
                generation: metadata.generation,
                epoch: data.intrinsic_cache_epoch,
                sizes: Some(Box::default()),
            };
        }
        callback(slot.sizes.get_or_insert_with(Box::default));
    }

    pub(crate) fn intrinsic_block_size_cache_put(
        &self,
        data: &NodeData,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        value: CssPixels,
    ) {
        self.with_intrinsic_size_maps_mut(data, |maps| {
            maps.block_sizes_mut(kind)
                .expect("block size cache kind must use the block axis")
                .insert(key, value);
        });
    }

    pub(crate) fn intrinsic_inline_size_measurement_cache_get(
        &self,
        data: &NodeData,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
    ) -> Option<IntrinsicInlineSizeMeasurement> {
        assert!(
            matches!(
                kind,
                IntrinsicSizeCacheKind::MinContentInline | IntrinsicSizeCacheKind::MaxContentInline
            ),
            "inline measurement cache kind must use the inline axis"
        );
        if data.intrinsic_cache_epoch == u16::MAX {
            return None;
        }

        let (index, metadata) = self.slot_for_data(std::ptr::from_ref(data));
        let caches = self.intrinsic_size_caches.borrow();
        let slot = caches.get(index as usize)?;
        if slot.generation != metadata.generation || slot.epoch != data.intrinsic_cache_epoch {
            return None;
        }
        slot.sizes
            .as_ref()?
            .inline_measurements(kind)
            .expect("inline measurement cache kind must use the inline axis")
            .get(&key)
            .copied()
    }

    pub(crate) fn intrinsic_inline_size_depends_on_block_size(
        &self,
        data: &NodeData,
        compute: impl FnOnce() -> bool,
    ) -> bool {
        if data.intrinsic_cache_epoch == u16::MAX {
            return compute();
        }

        let (index, metadata) = self.slot_for_data(std::ptr::from_ref(data));
        {
            let caches = self.intrinsic_size_caches.borrow();
            if let Some(slot) = caches.get(index as usize)
                && slot.generation == metadata.generation
                && slot.epoch == data.intrinsic_cache_epoch
                && let Some(value) = slot
                    .sizes
                    .as_ref()
                    .and_then(|sizes| sizes.inline_size_depends_on_block_size)
            {
                return value;
            }
        }

        let value = compute();
        self.with_intrinsic_size_maps_mut(data, |maps| {
            maps.inline_size_depends_on_block_size = Some(value);
        });
        value
    }

    pub(crate) fn intrinsic_inline_size_measurement_cache_put(
        &self,
        data: &NodeData,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        value: IntrinsicInlineSizeMeasurement,
    ) {
        self.with_intrinsic_size_maps_mut(data, |maps| {
            maps.inline_measurements_mut(kind)
                .expect("inline measurement cache kind must use the inline axis")
                .insert(key, value);
        });
    }

    pub(crate) fn saved_abspos_layout_inputs(&self, data: *const NodeData) -> Option<AbsposLayoutInputs> {
        let (index, metadata) = self.slot_for_data(data);
        let slots = self.saved_abspos_layout_inputs.borrow();
        let inputs = slots
            .get(index as usize)
            .filter(|slot| slot.generation == metadata.generation)
            .and_then(|slot| slot.inputs.as_deref().copied());

        // SAFETY: slot_for_data() established that data points to a live slot
        // in this arena.
        let flags = unsafe { (&raw const (*data).flags).read() };
        assert_eq!(
            flags & NodeFlag::HasSavedAbsposLayoutInputs as u32 != 0,
            inputs.is_some(),
            "saved abspos input presence flag disagrees with the arena side table"
        );
        assert_eq!(
            flags & NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues as u32 != 0,
            inputs.is_some_and(|inputs| inputs.containing_block_info.derives_from_own_computed_values),
            "saved abspos containing-block flag disagrees with the arena side table"
        );
        assert_eq!(
            flags & NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues as u32 != 0,
            inputs.is_some_and(|inputs| { inputs.static_position_rect.alignment_derives_from_own_computed_values }),
            "saved abspos alignment flag disagrees with the arena side table"
        );
        inputs
    }

    pub(crate) fn set_saved_abspos_layout_inputs(&self, data: *mut NodeData, inputs: Option<AbsposLayoutInputs>) {
        let (index, metadata) = self.slot_for_data(data);
        let mut slots = self.saved_abspos_layout_inputs.borrow_mut();
        if slots.len() <= index as usize {
            slots.resize_with(index as usize + 1, SavedAbsposLayoutInputsSlot::default);
        }
        let slot = &mut slots[index as usize];
        if slot.generation != metadata.generation {
            *slot = SavedAbsposLayoutInputsSlot {
                generation: metadata.generation,
                inputs: inputs.map(Box::new),
            };
        } else if let Some(inputs) = inputs {
            if let Some(saved_inputs) = &mut slot.inputs {
                **saved_inputs = inputs;
            } else {
                slot.inputs = Some(Box::new(inputs));
            }
        } else {
            slot.inputs = None;
        }
        drop(slots);

        let saved_abspos_flags = NodeFlag::HasSavedAbsposLayoutInputs as u32
            | NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues as u32
            | NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues as u32;
        // SAFETY: slot_for_data() established that data points to a live slot
        // in this arena, and layout/tree building serialize mutation on the
        // arena's owner thread.
        unsafe {
            let flags = &raw mut (*data).flags;
            let mut value = flags.read() & !saved_abspos_flags;
            if let Some(inputs) = inputs {
                value |= NodeFlag::HasSavedAbsposLayoutInputs as u32;
                if inputs.containing_block_info.derives_from_own_computed_values {
                    value |= NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues as u32;
                }
                if inputs.static_position_rect.alignment_derives_from_own_computed_values {
                    value |= NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues as u32;
                }
            }
            flags.write(value);
        }
    }

    pub(crate) fn saved_committed_geometry(
        &self,
        data: *const NodeData,
    ) -> Option<crate::layout::FfiPaintableGeometry> {
        let (index, metadata) = self.slot_for_data(data);
        let slots = self.saved_committed_geometries.borrow();
        let geometry = slots
            .get(index as usize)
            .filter(|slot| slot.generation == metadata.generation)
            .and_then(|slot| slot.geometry.as_deref().copied());

        // SAFETY: slot_for_data() established that data points to a live slot
        // in this arena.
        let flags = unsafe { (&raw const (*data).flags).read() };
        assert_eq!(
            flags & NodeFlag::HasSavedCommittedGeometry as u32 != 0,
            geometry.is_some(),
            "saved committed geometry presence flag disagrees with the arena side table"
        );
        geometry
    }

    pub(crate) fn set_saved_committed_geometry(
        &self,
        data: *mut NodeData,
        geometry: crate::layout::FfiPaintableGeometry,
    ) {
        let (index, metadata) = self.slot_for_data(data);
        let mut slots = self.saved_committed_geometries.borrow_mut();
        if slots.len() <= index as usize {
            slots.resize_with(index as usize + 1, SavedCommittedGeometrySlot::default);
        }
        let slot = &mut slots[index as usize];
        if slot.generation != metadata.generation {
            *slot = SavedCommittedGeometrySlot {
                generation: metadata.generation,
                geometry: Some(Box::new(geometry)),
            };
        } else if let Some(saved_geometry) = &mut slot.geometry {
            **saved_geometry = geometry;
        } else {
            slot.geometry = Some(Box::new(geometry));
        }
        drop(slots);

        // SAFETY: slot_for_data() established that data points to a live slot
        // in this arena, and layout/tree building serialize mutation on the
        // arena's owner thread.
        unsafe {
            let flags = &raw mut (*data).flags;
            flags.write(flags.read() | NodeFlag::HasSavedCommittedGeometry as u32);
        }
    }

    pub(crate) fn set_text_content(
        &mut self,
        id: NodeSlotId,
        text: Vec<u16>,
        untransformed_text_is_ascii_whitespace: bool,
        may_require_bidi_processing: bool,
        dom_start_offset: usize,
    ) -> bool {
        self.assert_owner_thread();
        self.data(id);
        let index = id.slot_index() as usize;
        if self.text_contents.len() <= index {
            self.text_contents.resize_with(index + 1, TextContentSlot::default);
        }
        let previous = &self.text_contents[index];
        let changed = previous.generation != id.generation()
            || match &previous.content {
                Some(content) => {
                    content.text != text
                        || content.untransformed_text_is_ascii_whitespace != untransformed_text_is_ascii_whitespace
                        || content.may_require_bidi_processing != may_require_bidi_processing
                        || content.dom_start_offset != dom_start_offset
                }
                None => true,
            };
        if !changed {
            return false;
        }
        self.text_contents[index] = TextContentSlot {
            generation: id.generation(),
            content: Some(Box::new(TextContent {
                text,
                untransformed_text_is_ascii_whitespace,
                may_require_bidi_processing,
                dom_start_offset,
                grapheme_segmenter: std::cell::OnceCell::new(),
            })),
        };
        if let Some(slot) = self.text_chunk_caches.get_mut().get_mut(index) {
            *slot = TextChunkCacheSlot::default();
        }
        true
    }

    pub(crate) fn set_replaced_content_facts(&mut self, id: NodeSlotId, facts: FfiReplacedContentFacts) -> bool {
        self.assert_owner_thread();
        self.data(id);
        let index = id.slot_index() as usize;
        if self.replaced_content_facts.len() <= index {
            self.replaced_content_facts
                .resize_with(index + 1, ReplacedContentFactsSlot::default);
        }
        let previous = &self.replaced_content_facts[index];
        let changed = previous.generation != id.generation() || previous.facts != Some(facts);
        self.replaced_content_facts[index] = ReplacedContentFactsSlot {
            generation: id.generation(),
            facts: Some(facts),
        };
        changed
    }

    pub(crate) fn replaced_content_facts(&self, id: NodeSlotId) -> Option<FfiReplacedContentFacts> {
        assert!(!id.is_invalid(), "invalid layout node arena slot ID");
        self.replaced_content_facts
            .get(id.slot_index() as usize)
            .filter(|slot| slot.generation == id.generation())
            .and_then(|slot| slot.facts)
    }

    pub(crate) fn set_raw_table_column_span(&mut self, id: NodeSlotId, value: u32) -> u32 {
        self.assert_owner_thread();
        self.data(id);
        if value == 1 {
            self.raw_table_column_spans.remove(&id).unwrap_or(1)
        } else {
            self.raw_table_column_spans.insert(id, value).unwrap_or(1)
        }
    }

    pub(crate) fn raw_table_column_span(&self, id: NodeSlotId) -> u32 {
        // data() validates that id names a live slot with a matching generation.
        self.data(id);
        self.raw_table_column_spans.get(&id).copied().unwrap_or(1)
    }

    pub(crate) fn text_content(&self, id: NodeSlotId) -> Option<&TextContent> {
        assert!(!id.is_invalid(), "invalid layout node arena slot ID");
        self.text_contents
            .get(id.slot_index() as usize)
            .filter(|slot| slot.generation == id.generation())
            .and_then(|slot| slot.content.as_deref())
    }

    /// The node's group payload pointer array, read in place from the
    /// Rust-owned style container that NodeData.style addresses. The node's
    /// retained immutable ComputedValues owns the container, and the pointer
    /// is only replaced between passes, so the array stays valid for as long
    /// as the node occupies its arena slot.
    pub(crate) fn style_payloads(&self, id: NodeSlotId) -> Option<&FfiStylePayloads> {
        // SAFETY: data() generation-checks the slot and returns an
        // initialized NodeData.
        let style = unsafe { (&raw const (*self.data(id)).style).read() };
        // SAFETY: A non-null style pointer addresses the container's group
        // pointer array, which FfiStylePayloads mirrors exactly.
        (!style.is_null()).then(|| unsafe { &*style.cast::<FfiStylePayloads>() })
    }

    pub(crate) fn text_chunks(
        &self,
        id: NodeSlotId,
        key: TextChunkCacheKey,
        compute: impl FnOnce() -> Vec<crate::layout::TextChunk>,
    ) -> &'static [crate::layout::TextChunk] {
        // data() validates that id names a live slot with a matching generation.
        self.data(id);
        let index = id.slot_index() as usize;

        // SAFETY (for both laundered returns below): an entry is only replaced
        // when its key changes or its slot is freed, and every key input is
        // fixed for a given node within one layout pass while the arena
        // itself outlives the pass, so a slice handed out during a pass stays
        // valid for that pass.
        {
            let slots = self.text_chunk_caches.borrow();
            if let Some(slot) = slots.get(index)
                && slot.generation == id.generation()
                && let Some(entry) = slot.entry.as_deref()
                && entry.key == key
            {
                return unsafe { std::slice::from_raw_parts(entry.chunks.as_ptr(), entry.chunks.len()) };
            }
        }

        let chunks = compute();
        let mut slots = self.text_chunk_caches.borrow_mut();
        if slots.len() <= index {
            slots.resize_with(index + 1, TextChunkCacheSlot::default);
        }
        slots[index] = TextChunkCacheSlot {
            generation: id.generation(),
            entry: Some(Box::new(TextChunkCacheEntry {
                key,
                // SAFETY: The caller derives the key's cascade-list pointer
                // from a live style snapshot.
                _retained_font_cascade_list: unsafe {
                    libgfx_rust::font::RetainedFontCascadeList::retain(key.font_cascade_list)
                },
                chunks,
            })),
        };
        let entry = slots[index].entry.as_deref().expect("entry was just stored");
        unsafe { std::slice::from_raw_parts(entry.chunks.as_ptr(), entry.chunks.len()) }
    }

    pub(crate) fn allocate_run_nonce(&self) -> u64 {
        let nonce = self.next_run_nonce.get();
        self.next_run_nonce
            .set(nonce.checked_add(1).expect("layout run nonce space exhausted"));
        nonce
    }

    pub(crate) fn run_record(&self, slot_index: u32, run_nonce: u64) -> Option<Rc<UsedValues>> {
        let records = self.run_used_records.borrow();
        let slot = records.get(slot_index as usize)?;
        if slot.nonce != run_nonce {
            return None;
        }
        slot.record.clone()
    }

    pub(crate) fn replace_run_record(
        &self,
        slot_index: u32,
        run_nonce: u64,
        record: Rc<UsedValues>,
    ) -> Option<(u64, Rc<UsedValues>)> {
        let mut records = self.run_used_records.borrow_mut();
        let slot = records
            .get_mut(slot_index as usize)
            .expect("registered layout run record slot must exist");
        let previous = std::mem::replace(
            slot,
            RunRecordSlot {
                nonce: run_nonce,
                record: Some(record),
            },
        );
        previous.record.map(|record| (previous.nonce, record))
    }

    pub(crate) fn restore_run_record(&self, slot_index: u32, run_nonce: u64, previous: Option<(u64, Rc<UsedValues>)>) {
        let mut records = self.run_used_records.borrow_mut();
        let slot = records
            .get_mut(slot_index as usize)
            .expect("restored layout run record slot must exist");
        debug_assert_eq!(
            slot.nonce, run_nonce,
            "layout run records were not restored in LIFO order"
        );
        *slot = match previous {
            Some((nonce, record)) => RunRecordSlot {
                nonce,
                record: Some(record),
            },
            None => RunRecordSlot::default(),
        };
    }

    // OPTIMIZATION: The edit invalidates line data at its direct parent and every formatting
    // ancestor. Preserve the structural proof along the same unbounded path as the fragment
    // epoch bumps so each affected inline context can reuse its unchanged line prefix.
    pub(crate) fn note_inline_layout_damage_at_and_above(&self, mut box_: NodeSlotId) {
        while !box_.is_invalid() {
            let data = self.data(box_);
            self.fc_run_cache_store.note_inline_layout_damage(box_);
            // SAFETY: data() validated that box_ names a live slot, and the layout tree is stable
            // for the duration of this synchronous topology update.
            box_ = unsafe { (&raw const (*data).parent).read() };
        }
    }

    pub(crate) fn insert_child(&self, parent: NodeSlotId, child: NodeSlotId, before: NodeSlotId) {
        self.assert_owner_thread();
        assert_ne!(parent, child, "a layout node cannot become its own child");
        let parent_data = self.data(parent);
        let child_data = self.data(child);

        // SAFETY (for every raw access below): data() validated that the slot IDs name live
        // nodes, and layout tree mutation is serialized on the arena's owner thread.
        unsafe {
            let child_parent = (&raw const (*child_data).parent).read();
            let child_previous_sibling = (&raw const (*child_data).previous_sibling).read();
            let child_next_sibling = (&raw const (*child_data).next_sibling).read();
            assert!(
                child_parent.is_invalid(),
                "inserted layout node is still linked to a parent"
            );
            assert!(
                child_previous_sibling.is_invalid(),
                "inserted layout node is still linked to a previous sibling"
            );
            assert!(
                child_next_sibling.is_invalid(),
                "inserted layout node is still linked to a next sibling"
            );
            debug_assert_eq!(
                (&raw const (*parent_data).first_child).read().is_invalid(),
                (&raw const (*parent_data).last_child).read().is_invalid(),
                "layout node child list endpoints disagree"
            );
        }

        #[cfg(debug_assertions)]
        {
            let mut ancestor = parent;
            while !ancestor.is_invalid() {
                assert_ne!(ancestor, child, "layout node insertion would create a cycle");
                // SAFETY: data() validated that ancestor names a live slot, and parent links
                // only name live slots.
                ancestor = unsafe { (&raw const (*self.data(ancestor)).parent).read() };
            }
        }

        let before_data = if before.is_invalid() {
            std::ptr::null_mut()
        } else {
            assert_ne!(before, child, "a layout node cannot be inserted before itself");
            self.data(before)
        };
        let previous = if before_data.is_null() {
            // SAFETY: parent_data addresses a live node validated above.
            unsafe { (&raw const (*parent_data).last_child).read() }
        } else {
            // SAFETY: data() validated that before names a live node.
            unsafe {
                let before_parent = (&raw const (*before_data).parent).read();
                assert_eq!(
                    before_parent, parent,
                    "insertion reference is not a child of the parent"
                );
                (&raw const (*before_data).previous_sibling).read()
            }
        };

        // SAFETY: Every written slot was validated live above (previous comes from a validated
        // sibling or child-list link), and mutation is serialized on the owner thread.
        unsafe {
            (&raw mut (*child_data).parent).write(parent);
            (&raw mut (*child_data).previous_sibling).write(previous);
            (&raw mut (*child_data).next_sibling).write(before);
            if previous.is_invalid() {
                (&raw mut (*parent_data).first_child).write(child);
            } else {
                (&raw mut (*self.data(previous)).next_sibling).write(child);
            }
            if before_data.is_null() {
                (&raw mut (*parent_data).last_child).write(child);
            } else {
                (&raw mut (*before_data).previous_sibling).write(child);
            }
        }

        self.note_inline_layout_damage_at_and_above(parent);
    }

    pub(crate) fn remove_child(&self, parent: NodeSlotId, child: NodeSlotId) {
        self.assert_owner_thread();
        let parent_data = self.data(parent);
        let child_data = self.data(child);

        // SAFETY (for every raw access below): data() validated that the slot IDs name live
        // nodes, sibling and child-list links only name live slots, and layout tree mutation
        // is serialized on the arena's owner thread.
        unsafe {
            let child_parent = (&raw const (*child_data).parent).read();
            assert_eq!(child_parent, parent, "removed layout node is not a child of the parent");
            let previous = (&raw const (*child_data).previous_sibling).read();
            let next = (&raw const (*child_data).next_sibling).read();

            if previous.is_invalid() {
                let first_child = (&raw const (*parent_data).first_child).read();
                assert_eq!(first_child, child, "layout node child list lost its first child");
                (&raw mut (*parent_data).first_child).write(next);
            } else {
                let previous_data = self.data(previous);
                let previous_next_sibling = (&raw const (*previous_data).next_sibling).read();
                assert_eq!(
                    previous_next_sibling, child,
                    "layout node sibling chain is inconsistent"
                );
                (&raw mut (*previous_data).next_sibling).write(next);
            }

            if next.is_invalid() {
                let last_child = (&raw const (*parent_data).last_child).read();
                assert_eq!(last_child, child, "layout node child list lost its last child");
                (&raw mut (*parent_data).last_child).write(previous);
            } else {
                let next_data = self.data(next);
                let next_previous_sibling = (&raw const (*next_data).previous_sibling).read();
                assert_eq!(
                    next_previous_sibling, child,
                    "layout node sibling chain is inconsistent"
                );
                (&raw mut (*next_data).previous_sibling).write(previous);
            }

            (&raw mut (*child_data).parent).write(NodeSlotId::INVALID);
            (&raw mut (*child_data).previous_sibling).write(NodeSlotId::INVALID);
            (&raw mut (*child_data).next_sibling).write(NodeSlotId::INVALID);
        }

        self.note_inline_layout_damage_at_and_above(parent);
    }

    pub(crate) fn paintables(&self) -> &RefCell<crate::painting::paintable_arena::PaintableArena> {
        &self.paintables
    }

    pub(crate) fn node_flags_if_live(&self, id: NodeSlotId) -> u32 {
        if self.shell_if_live(id).is_null() {
            return 0;
        }
        // SAFETY: shell_if_live established a live slot of this generation.
        unsafe { (*self.data(id)).flags }
    }

    pub(crate) fn node_is_generated_for_pseudo_element(&self, id: NodeSlotId) -> bool {
        if self.shell_if_live(id).is_null() {
            return false;
        }
        // SAFETY: shell_if_live established a live slot of this generation.
        unsafe { (*self.data(id)).generated_for != 0 }
    }

    pub(crate) fn node_kind_if_live(&self, id: NodeSlotId) -> Option<NodeKind> {
        if self.shell_if_live(id).is_null() {
            return None;
        }
        // SAFETY: shell_if_live established a live slot of this generation.
        Some(unsafe { (*self.data(id)).kind })
    }

    pub(crate) fn node_parent_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        if self.shell_if_live(id).is_null() {
            return None;
        }
        // SAFETY: shell_if_live established a live slot of this generation.
        let parent = unsafe { (*self.data(id)).parent };
        (!parent.is_invalid()).then_some(parent)
    }

    pub(crate) fn node_first_child_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        if self.shell_if_live(id).is_null() {
            return None;
        }
        // SAFETY: shell_if_live established a live slot of this generation.
        let child = unsafe { (*self.data(id)).first_child };
        (!child.is_invalid()).then_some(child)
    }

    pub(crate) fn node_next_sibling_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        if self.shell_if_live(id).is_null() {
            return None;
        }
        // SAFETY: shell_if_live established a live slot of this generation.
        let sibling = unsafe { (*self.data(id)).next_sibling };
        (!sibling.is_invalid()).then_some(sibling)
    }

    pub(crate) fn node_is_out_of_flow_if_live(&self, id: NodeSlotId) -> bool {
        if self.shell_if_live(id).is_null() {
            return false;
        }
        // SAFETY: shell_if_live established a live slot of this generation.
        let data = unsafe { &*self.data(id) };
        crate::layout::node_is_out_of_flow(data, self.node_style_if_live(id))
    }

    pub(crate) fn node_containing_block_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        if self.shell_if_live(id).is_null() {
            return None;
        }
        // SAFETY: shell_if_live established a live slot of this generation.
        let block = unsafe { (*self.data(id)).containing_block };
        (!block.is_invalid()).then_some(block)
    }

    pub(crate) fn node_style_if_live(
        &self,
        id: NodeSlotId,
    ) -> Option<crate::css::computed_value_views::ComputedValuesView<'_>> {
        if self.shell_if_live(id).is_null() {
            return None;
        }
        let payloads = self.style_payloads(id)?;
        Some(crate::css::computed_value_views::ComputedValuesView::new(
            &payloads.groups,
        ))
    }

    pub(crate) fn shell_if_live(&self, id: NodeSlotId) -> *mut c_void {
        if id.is_invalid() {
            return std::ptr::null_mut();
        }
        let index = id.slot_index() as usize;
        let Some(metadata) = self.slot_metadata.get(index) else {
            return std::ptr::null_mut();
        };
        if !metadata.occupied || metadata.generation != id.generation() {
            return std::ptr::null_mut();
        }
        // SAFETY: The metadata check established a live slot of this generation.
        unsafe { (*self.data(id)).shell }
    }

    pub(crate) unsafe fn from_handle<'a>(arena: *mut c_void) -> &'a Self {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: Layout passes borrow the document's arena synchronously,
        // and the document keeps it alive for the duration of the pass.
        unsafe { &*arena.cast::<Self>() }
    }

    fn data_mut(&mut self, index: u32) -> &mut NodeData {
        let index = index as usize;
        let chunk = self
            .chunks
            .get_mut(index / SLOTS_PER_CHUNK)
            .expect("invalid layout node arena slot ID");
        &mut chunk.slots[index % SLOTS_PER_CHUNK]
    }

    fn metadata(&self, index: u32) -> &SlotMetadata {
        self.slot_metadata
            .get(index as usize)
            .expect("invalid layout node arena slot ID")
    }

    fn metadata_mut(&mut self, index: u32) -> &mut SlotMetadata {
        self.slot_metadata
            .get_mut(index as usize)
            .expect("invalid layout node arena slot ID")
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct NodeAllocation {
    pub slot: NodeSlotId,
    pub data: *mut NodeData,
    pub generation: u32,
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_arena_create() -> *mut c_void {
    abort_on_panic(|| Box::into_raw(Box::new(LayoutNodeArena::new())).cast())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_destroy(arena: *mut c_void) {
    abort_on_panic(|| {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: The handle came from layout_arena_create and ownership is
        // transferred back exactly once by the C++ RAII wrapper.
        let arena = unsafe { Box::from_raw(arena.cast::<LayoutNodeArena>()) };
        arena.assert_owner_thread();
        assert_eq!(arena.live_count, 0, "layout node arena destroyed with live slots");
    });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_allocate(arena: *mut c_void) -> NodeAllocation {
    abort_on_panic(|| {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: The C++ wrapper keeps the arena alive for this call and
        // serializes all access on the document thread.
        unsafe { &mut *arena.cast::<LayoutNodeArena>() }.allocate()
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_free(arena: *mut c_void, id: NodeSlotId, generation: u32) {
    abort_on_panic(|| {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: The C++ wrapper keeps the arena alive for this call and
        // serializes all access on the document thread.
        unsafe { &mut *arena.cast::<LayoutNodeArena>() }.free(id, generation);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_fc_run_cache_epochs_enabled() -> bool {
    crate::layout::fc_run_cache_mode_from_environment() != crate::layout::FcRunCacheMode::Disabled
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_fc_run_cache_hit_count(arena: *mut c_void) -> u64 {
    abort_on_panic(|| {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: The C++ wrapper keeps the arena alive for this call and
        // serializes all access on the document thread.
        unsafe { &*arena.cast::<LayoutNodeArena>() }
            .fc_run_cache_store()
            .hit_count()
    })
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `id` must
/// name a live node in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_data(arena: *mut c_void, id: NodeSlotId) -> *mut NodeData {
    abort_on_panic(|| {
        // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
        unsafe { LayoutNodeArena::from_handle(arena) }.data(id)
    })
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `root` must
/// name the root of a live subtree in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_reset_layout_update_flags_in_subtree(arena: *mut c_void, root: NodeSlotId) {
    abort_on_panic(|| {
        // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
        unsafe { LayoutNodeArena::from_handle(arena) }.reset_layout_update_flags_in_subtree(root);
    });
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `root` must
/// name the root of a live subtree in this arena. `inline_cb_lookup` is invoked
/// for each absolutely positioned node with a resolved containing block; it
/// receives the two nodes' shell pointers, must not mutate the layout tree or
/// its styles, and must return the slot of the intervening inline containing
/// block or the invalid slot id.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_recompute_containing_blocks(
    arena: *mut c_void,
    root: NodeSlotId,
    inline_cb_lookup: unsafe extern "C" fn(*mut c_void, *mut c_void) -> NodeSlotId,
) {
    abort_on_panic(|| {
        // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
        unsafe { LayoutNodeArena::from_handle(arena) }.recompute_containing_blocks_in_subtree(root, inline_cb_lookup);
    });
}

/// # Safety
///
/// The arena must remain valid for the duration of the call. `id` may be
/// invalid or stale; null is returned in that case.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_shell_if_live(arena: *mut c_void, id: NodeSlotId) -> *mut c_void {
    abort_on_panic(|| {
        // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
        unsafe { LayoutNodeArena::from_handle(arena) }.shell_if_live(id)
    })
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `parent`,
/// `child`, and a valid `before` must name live nodes in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_insert_child(
    arena: *mut c_void,
    parent: NodeSlotId,
    child: NodeSlotId,
    before: NodeSlotId,
) {
    abort_on_panic(|| {
        // SAFETY: The C++ wrapper keeps the arena alive for this call and
        // serializes all access on the document thread.
        unsafe { LayoutNodeArena::from_handle(arena) }.insert_child(parent, child, before);
    });
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `parent` and
/// `child` must name live nodes in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_remove_child(arena: *mut c_void, parent: NodeSlotId, child: NodeSlotId) {
    abort_on_panic(|| {
        // SAFETY: The C++ wrapper keeps the arena alive for this call and
        // serializes all access on the document thread.
        unsafe { LayoutNodeArena::from_handle(arena) }.remove_child(parent, child);
    });
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_text_content(
    arena: *mut c_void,
    id: NodeSlotId,
    ascii_text: *const u8,
    utf16_text: *const u16,
    length_in_code_units: usize,
    untransformed_text_is_ascii_whitespace: bool,
    may_require_bidi_processing: bool,
    dom_start_offset: usize,
) -> bool {
    abort_on_panic(|| {
        assert!(!arena.is_null(), "layout node arena handle is null");
        let text = if length_in_code_units == 0 {
            Vec::new()
        } else if !ascii_text.is_null() {
            // SAFETY: The C++ caller passes the live ASCII storage of the
            // node's rendered text for the duration of this synchronous call.
            unsafe { std::slice::from_raw_parts(ascii_text, length_in_code_units) }
                .iter()
                .map(|unit| u16::from(*unit))
                .collect()
        } else {
            assert!(!utf16_text.is_null(), "text content push carries no storage");
            // SAFETY: The C++ caller passes the live UTF-16 storage of the
            // node's rendered text for the duration of this synchronous call.
            unsafe { std::slice::from_raw_parts(utf16_text, length_in_code_units) }.to_vec()
        };
        // SAFETY: The C++ wrapper keeps the arena alive for this call and
        // serializes all access on the document thread.
        unsafe { &mut *arena.cast::<LayoutNodeArena>() }.set_text_content(
            id,
            text,
            untransformed_text_is_ascii_whitespace,
            may_require_bidi_processing,
            dom_start_offset,
        )
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_replaced_content_facts(
    arena: *mut c_void,
    id: NodeSlotId,
    facts: FfiReplacedContentFacts,
) -> bool {
    abort_on_panic(|| {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: The C++ wrapper keeps the arena alive for this call and
        // serializes all access on the document thread.
        unsafe { &mut *arena.cast::<LayoutNodeArena>() }.set_replaced_content_facts(id, facts)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_raw_table_column_span(
    arena: *mut c_void,
    id: NodeSlotId,
    raw_column_span: u32,
) -> u32 {
    abort_on_panic(|| {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: The C++ wrapper keeps the arena alive for this call and
        // serializes all access on the document thread.
        unsafe { &mut *arena.cast::<LayoutNodeArena>() }.set_raw_table_column_span(id, raw_column_span)
    })
}

#[cfg(test)]
mod tests {
    use crate::layout::layout_node_arena::{
        Chunk, IntrinsicInlineSizeMeasurement, IntrinsicSizeCacheKey, IntrinsicSizeCacheKind, LayoutNodeArena,
        SLOTS_PER_CHUNK,
    };
    use crate::layout::node_data::{NodeFlag, NodeSlotId};
    use crate::layout::{AvailableSize, CssPixels};

    #[test]
    fn node_data_addresses_remain_stable_when_chunks_are_added() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate();
        let first_data = first.data;

        let mut allocations = Vec::new();
        for _ in 0..SLOTS_PER_CHUNK * 2 {
            allocations.push(arena.allocate());
        }

        assert_eq!(first_data, arena.data(first.slot));
        // SAFETY: The first allocation is still live, and the comparison above
        // confirms that its pointer still addresses the arena slot.
        unsafe {
            (*first_data).table_column_span = 42;
            assert_eq!((*arena.data(first.slot)).table_column_span, 42);
        }
        arena.free(first.slot, first.generation);
        for allocation in allocations {
            arena.free(allocation.slot, allocation.generation);
        }
    }

    #[test]
    fn node_data_slots_are_cache_line_aligned() {
        assert_eq!(align_of::<Chunk>() % 64, 0);
        let mut arena = LayoutNodeArena::new();
        let allocation = arena.allocate();
        assert_eq!(allocation.data as usize % 64, 0);
        arena.free(allocation.slot, allocation.generation);
    }

    #[test]
    fn freed_slots_are_reused_with_a_new_generation() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate();
        arena.free(first.slot, first.generation);

        let second = arena.allocate();
        assert_eq!(second.slot.slot_index(), first.slot.slot_index());
        assert_ne!(second.slot, first.slot);
        assert_ne!(second.generation, first.generation);
        arena.free(second.slot, second.generation);
    }

    #[test]
    fn layout_update_flags_are_reset_only_in_the_requested_subtree() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate();
        let child = arena.allocate();
        let detached = arena.allocate();
        arena.insert_child(root.slot, child.slot, NodeSlotId::INVALID);
        let update_flags = NodeFlag::NeedsLayoutUpdate as u32 | NodeFlag::NeedsOwnGeometryUpdate as u32;
        // SAFETY: All three allocations remain live for the duration of the test.
        unsafe {
            (*root.data).flags |= update_flags;
            (*child.data).flags |= update_flags;
            (*detached.data).flags |= update_flags;
        }

        arena.reset_layout_update_flags_in_subtree(root.slot);

        // SAFETY: The allocations remain live and the arena keeps their addresses stable.
        unsafe {
            assert_eq!((*root.data).flags & update_flags, 0);
            assert_eq!((*child.data).flags & update_flags, 0);
            assert_eq!((*detached.data).flags & update_flags, update_flags);
        }
        arena.remove_child(root.slot, child.slot);
        arena.free(root.slot, root.generation);
        arena.free(child.slot, child.generation);
        arena.free(detached.slot, detached.generation);
    }

    #[test]
    fn stale_slot_ids_do_not_resolve_to_a_new_occupant() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate();
        arena.free(first.slot, first.generation);
        let second = arena.allocate();

        let stale_read = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| arena.data(first.slot)));
        assert!(stale_read.is_err());
        arena.free(second.slot, second.generation);
    }

    #[test]
    fn intrinsic_size_cache_validates_epoch_and_generation() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate();
        let key = IntrinsicSizeCacheKey {
            measured_at_inline_size: Some(CssPixels::from_raw(64)),
            ..Default::default()
        };
        let value = CssPixels::from_raw(128);
        let inline_measurement = IntrinsicInlineSizeMeasurement {
            automatic_content_inline_size: CssPixels::from_raw(192),
            min_content_inline_size_from_max_content_layout: Some(CssPixels::from_raw(96)),
            available_block_size: AvailableSize::MaxContent,
            content_inline_size: CssPixels::from_raw(192),
            content_block_size: CssPixels::from_raw(256),
            automatic_content_block_size: CssPixels::from_raw(320),
            uses_collapsing_borders_model: true,
            has_first_baseline: true,
            first_baseline: CssPixels::from_raw(64),
            has_last_baseline: true,
            last_baseline: CssPixels::from_raw(128),
        };
        let dependency_computations = Cell::new(0);

        // SAFETY: The allocation remains live until it is explicitly freed below.
        let first_data = unsafe { &mut *first.data };
        arena.intrinsic_block_size_cache_put(first_data, IntrinsicSizeCacheKind::MinContentBlock, key, value);
        arena.intrinsic_inline_size_measurement_cache_put(
            first_data,
            IntrinsicSizeCacheKind::MaxContentInline,
            key,
            inline_measurement,
        );
        assert_eq!(
            arena.intrinsic_block_size_cache_get(first_data, IntrinsicSizeCacheKind::MinContentBlock, key),
            Some(value)
        );
        assert_eq!(
            arena.intrinsic_inline_size_measurement_cache_get(
                first_data,
                IntrinsicSizeCacheKind::MaxContentInline,
                key
            ),
            Some(inline_measurement)
        );
        assert!(arena.intrinsic_inline_size_depends_on_block_size(first_data, || {
            dependency_computations.set(dependency_computations.get() + 1);
            true
        }));
        assert!(arena.intrinsic_inline_size_depends_on_block_size(first_data, || false));
        assert_eq!(dependency_computations.get(), 1);

        first_data.intrinsic_cache_epoch += 1;
        assert_eq!(
            arena.intrinsic_block_size_cache_get(first_data, IntrinsicSizeCacheKind::MinContentBlock, key),
            None
        );
        assert_eq!(
            arena.intrinsic_inline_size_measurement_cache_get(
                first_data,
                IntrinsicSizeCacheKind::MaxContentInline,
                key
            ),
            None
        );
        assert!(!arena.intrinsic_inline_size_depends_on_block_size(first_data, || {
            dependency_computations.set(dependency_computations.get() + 1);
            false
        }));
        assert_eq!(dependency_computations.get(), 2);
        arena.free(first.slot, first.generation);

        let second = arena.allocate();
        assert_eq!(second.slot.slot_index(), first.slot.slot_index());
        assert_ne!(second.slot, first.slot);
        // SAFETY: The second allocation is live and reuses the first allocation's slot.
        let second_data = unsafe { &*second.data };
        assert_eq!(
            arena.intrinsic_block_size_cache_get(second_data, IntrinsicSizeCacheKind::MinContentBlock, key),
            None
        );
        assert_eq!(
            arena.intrinsic_inline_size_measurement_cache_get(
                second_data,
                IntrinsicSizeCacheKind::MaxContentInline,
                key
            ),
            None
        );
        arena.free(second.slot, second.generation);
    }
}
