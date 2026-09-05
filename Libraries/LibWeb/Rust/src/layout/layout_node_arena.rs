/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::abspos_inputs::AbsposLayoutInputs;
use super::formatting_context::DerivedBaselines;
use super::formatting_context::LayoutMode;
use super::geometry::AvailableSize;
use super::geometry::AvailableSpace;
use super::rendered_text::{FfiTextFragments, FfiTextSourceRange, RenderedTextBoundary, TextContent};
use super::used_values::SizeConstraint;
use super::used_values::UsedValues;
use crate::css::style::fast_hash::{FastMap as HashMap, FastSet as HashSet};
use crate::layout::ComputedValuesView;
use crate::layout::CssPixels;
use crate::layout::FfiReplacedContentFacts;
use crate::layout::node_data::{
    FfiNodeConstructionFacts, FfiNodeLink, FfiStylePayloads, MAX_NODE_SLOT_COUNT, NodeData, NodeFlag, NodeKind,
    NodeSlotId,
};
use std::cell::Cell;
use std::cell::RefCell;
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

impl IntrinsicSizeCacheKey {
    fn with_percentage_block_bases_masked(self) -> Self {
        Self {
            percentage_basis_block_size: None,
            quirks_mode_percentage_basis_block_size: None,
            ..self
        }
    }

    fn with_percentage_inline_basis_masked(self) -> Self {
        Self {
            percentage_basis_inline_size: None,
            ..self
        }
    }

    fn masked_for(self, dependencies: IntrinsicMeasurementDependencies) -> Self {
        let mut key = self;
        if !dependencies.percentage_block_size {
            key = key.with_percentage_block_bases_masked();
        }
        if !dependencies.percentage_inline_basis {
            key = key.with_percentage_inline_basis_masked();
        }
        key
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct IntrinsicMeasurementDependencies {
    pub(crate) percentage_block_size: bool,
    pub(crate) percentage_inline_basis: bool,
}

pub(crate) trait IntrinsicMeasurement: Copy {
    fn dependencies(&self) -> IntrinsicMeasurementDependencies;
}

// Measurements are stored under their key with every basis they never observed masked out. A
// probe that had to mask a basis must skip entries that observed it: a measurement made without
// that basis shares the masked key's shape.
fn intrinsic_cache_lookup<V: IntrinsicMeasurement>(
    map: &HashMap<IntrinsicSizeCacheKey, V>,
    key: IntrinsicSizeCacheKey,
) -> Option<V> {
    if let Some(value) = map.get(&key) {
        return Some(*value);
    }
    let block_masked = key.with_percentage_block_bases_masked();
    let inline_masked = key.with_percentage_inline_basis_masked();
    let masking_block_changes_key = block_masked != key;
    let masking_inline_changes_key = inline_masked != key;
    let candidates = [
        (block_masked, masking_block_changes_key, true, false),
        (inline_masked, masking_inline_changes_key, false, true),
        (
            block_masked.with_percentage_inline_basis_masked(),
            masking_block_changes_key && masking_inline_changes_key,
            true,
            true,
        ),
    ];
    candidates.into_iter().filter(|(_, applies, _, _)| *applies).find_map(
        |(candidate, _, block_was_masked, inline_was_masked)| {
            let value = *map.get(&candidate)?;
            let dependencies = value.dependencies();
            let observed_a_masked_basis = (block_was_masked && dependencies.percentage_block_size)
                || (inline_was_masked && dependencies.percentage_inline_basis);
            (!observed_a_masked_basis).then_some(value)
        },
    )
}

fn intrinsic_cache_store<V: IntrinsicMeasurement>(
    map: &mut HashMap<IntrinsicSizeCacheKey, V>,
    key: IntrinsicSizeCacheKey,
    value: V,
) {
    map.insert(key.masked_for(value.dependencies()), value);
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
pub(crate) struct TableCellMeasurementKey {
    pub(crate) layout_mode: LayoutMode,
    pub(crate) available_space: AvailableSpace,
    pub(crate) content_inline_size: CssPixels,
    pub(crate) content_block_size: CssPixels,
    pub(crate) has_definite_inline_size: bool,
    pub(crate) has_definite_block_size: bool,
    pub(crate) inline_size_constraint: SizeConstraint,
    pub(crate) block_size_constraint: SizeConstraint,
    pub(crate) uses_collapsing_borders_model: bool,
    pub(crate) adopt_automatic_content_block_size: bool,
}

impl Hash for TableCellMeasurementKey {
    fn hash<H: Hasher>(&self, state: &mut H) {
        fn hash_available_size<H: Hasher>(size: AvailableSize, state: &mut H) {
            match size {
                AvailableSize::Definite(value) => {
                    0u8.hash(state);
                    value.raw_value().hash(state);
                }
                AvailableSize::Indefinite => 1u8.hash(state),
                AvailableSize::MinContent => 2u8.hash(state),
                AvailableSize::MaxContent => 3u8.hash(state),
            }
        }

        (self.layout_mode as u8).hash(state);
        hash_available_size(self.available_space.inline_size, state);
        hash_available_size(self.available_space.block_size, state);
        self.content_inline_size.raw_value().hash(state);
        self.content_block_size.raw_value().hash(state);
        self.has_definite_inline_size.hash(state);
        self.has_definite_block_size.hash(state);
        (self.inline_size_constraint as u8).hash(state);
        (self.block_size_constraint as u8).hash(state);
        self.uses_collapsing_borders_model.hash(state);
        self.adopt_automatic_content_block_size.hash(state);
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct TableCellMeasurement {
    pub(crate) automatic_content_block_size: CssPixels,
    pub(crate) baselines: DerivedBaselines,
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
    // A dedicated inline-size query does not produce block sizes or baselines.
    pub(crate) layout: Option<IntrinsicInlineMeasurementLayout>,
    pub(crate) depends_on_percentage_block_size: bool,
    pub(crate) depends_on_percentage_inline_basis: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct IntrinsicInlineMeasurementLayout {
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

impl IntrinsicMeasurement for IntrinsicInlineSizeMeasurement {
    fn dependencies(&self) -> IntrinsicMeasurementDependencies {
        IntrinsicMeasurementDependencies {
            percentage_block_size: self.depends_on_percentage_block_size,
            percentage_inline_basis: self.depends_on_percentage_inline_basis,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct IntrinsicBlockSizeMeasurement {
    pub(crate) size: CssPixels,
    pub(crate) depends_on_percentage_block_size: bool,
    pub(crate) depends_on_percentage_inline_basis: bool,
}

impl IntrinsicMeasurement for IntrinsicBlockSizeMeasurement {
    fn dependencies(&self) -> IntrinsicMeasurementDependencies {
        IntrinsicMeasurementDependencies {
            percentage_block_size: self.depends_on_percentage_block_size,
            percentage_inline_basis: self.depends_on_percentage_inline_basis,
        }
    }
}

#[derive(Default)]
struct IntrinsicSizeMaps {
    // This is a subtree fact and shares the intrinsic cache's epoch so descendant changes invalidate it.
    inline_size_depends_on_block_size: Option<bool>,
    min_content_inline_size: HashMap<IntrinsicSizeCacheKey, IntrinsicInlineSizeMeasurement>,
    max_content_inline_size: HashMap<IntrinsicSizeCacheKey, IntrinsicInlineSizeMeasurement>,
    min_content_block_size: HashMap<IntrinsicSizeCacheKey, IntrinsicBlockSizeMeasurement>,
    max_content_block_size: HashMap<IntrinsicSizeCacheKey, IntrinsicBlockSizeMeasurement>,
    table_cell_measurements: HashMap<TableCellMeasurementKey, TableCellMeasurement>,
}

impl IntrinsicSizeMaps {
    fn block_sizes(
        &self,
        kind: IntrinsicSizeCacheKind,
    ) -> Option<&HashMap<IntrinsicSizeCacheKey, IntrinsicBlockSizeMeasurement>> {
        match kind {
            IntrinsicSizeCacheKind::MinContentInline | IntrinsicSizeCacheKind::MaxContentInline => None,
            IntrinsicSizeCacheKind::MinContentBlock => Some(&self.min_content_block_size),
            IntrinsicSizeCacheKind::MaxContentBlock => Some(&self.max_content_block_size),
        }
    }

    fn block_sizes_mut(
        &mut self,
        kind: IntrinsicSizeCacheKind,
    ) -> Option<&mut HashMap<IntrinsicSizeCacheKey, IntrinsicBlockSizeMeasurement>> {
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

#[derive(Clone, Copy, Default)]
struct DefaultScrollShiftAnchorSlot {
    generation: u8,
    anchor: NodeSlotId,
}

#[derive(Default)]
struct TextNodeSlot {
    generation: u8,
    state: Option<Box<TextNodeState>>,
}

#[derive(Default)]
struct TextNodeState {
    source_range: Option<FfiTextSourceRange>,
    first_letter: NodeSlotId,
    content: Option<TextContent>,
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

pub(crate) struct CachedTextChunks {
    key: TextChunkCacheKey,
    _retained_font_cascade_list: libgfx_rust::font::RetainedFontCascadeList,
    chunks: Vec<super::text_chunker::TextChunk>,
}

impl std::ops::Deref for CachedTextChunks {
    type Target = [super::text_chunker::TextChunk];

    fn deref(&self) -> &Self::Target {
        &self.chunks
    }
}

#[derive(Default)]
struct TextChunkCacheSlot {
    generation: u8,
    entry: Option<Rc<CachedTextChunks>>,
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

#[derive(Clone, Copy, PartialEq, Eq)]
enum AncestorInvalidation {
    StructuralChange,
    ContentChange,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiAnonymousStyleKind {
    Wrapper,
    TableRow,
    TableCell,
    Table,
    InlineTable,
    MissingTableCell,
    TableWrapper,
    ButtonFlexWrapper,
    ButtonContentBox,
    FieldsetContentWrapper,
    InlineStyleWrapper,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiAnonymousStyleOverrides {
    pub inline_block_wrapper: bool,
    pub overflow_x: u8,
    pub overflow_y: u8,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiDerivedStyleRecord {
    pub record: u64,
    pub payloads: *const c_void,
}

type ShellFactory = (*mut c_void, unsafe extern "C" fn(*mut c_void, NodeSlotId, NodeKind));

fn style_insets_use_anchor_functions(style: ComputedValuesView<'_>) -> bool {
    let surround = style.surround();
    [
        &surround.top_anchor_inset,
        &surround.right_anchor_inset,
        &surround.bottom_anchor_inset,
        &surround.left_anchor_inset,
    ]
    .iter()
    .any(|handle| !handle.pointer.is_null())
        || [
            &surround.inset.top,
            &surround.inset.right,
            &surround.inset.bottom,
            &surround.inset.left,
        ]
        .iter()
        .any(|side| {
            side.length_percentage()
                .is_some_and(|value| value.contains_anchor_function())
        })
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStyleRecordHostCallbacks {
    pub context: *mut c_void,
    pub derive_anonymous_style_record: unsafe extern "C" fn(
        *mut c_void,
        u64,
        FfiAnonymousStyleKind,
        FfiAnonymousStyleOverrides,
    ) -> FfiDerivedStyleRecord,
    pub reinherit_anonymous_style_record: unsafe extern "C" fn(*mut c_void, u64, u64) -> FfiDerivedStyleRecord,
    pub unpin_style_record: unsafe extern "C" fn(*mut c_void, u64),
    pub reinherit_owned_anonymous_box_style: unsafe extern "C" fn(*mut c_void, *mut c_void, u64) -> bool,
    pub reset_table_box_style_used_by_wrapper: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub shell_style_changed: unsafe extern "C" fn(*mut c_void, *mut c_void),
}

fn style_payloads_equal_in_layout_affecting_groups(a: *const c_void, b: *const c_void) -> bool {
    if a == b {
        return true;
    }
    if a.is_null() || b.is_null() {
        return false;
    }
    // SAFETY: A non-null style pointer addresses the engine's group pointer array, which
    // FfiStylePayloads mirrors exactly.
    let (a, b) = unsafe { (&*a.cast::<FfiStylePayloads>(), &*b.cast::<FfiStylePayloads>()) };
    (0..a.groups.len()).all(|group_index| {
        !crate::css::computed_values::style_group_affects_layout(group_index)
            || a.groups[group_index] == b.groups[group_index]
            || crate::css::computed_values::style_group_payloads_equal(
                group_index,
                a.groups[group_index],
                b.groups[group_index],
            )
    })
}

#[must_use]
pub(crate) struct FreedSubtree {
    shells: Vec<*mut c_void>,
    paintable_row_resets: Vec<crate::painting::paintable_rows::PaintableRowReset>,
    arena_pinned_style_records: Vec<u64>,
    style_record_host: Option<FfiStyleRecordHostCallbacks>,
}

impl FreedSubtree {
    #[cfg(test)]
    pub(crate) fn shell_count(&self) -> usize {
        self.shells.len()
    }

    #[cfg(test)]
    pub(crate) fn arena_pinned_style_record_count(&self) -> usize {
        self.arena_pinned_style_records.len()
    }

    pub(crate) fn destroy_shells_and_invoke_callbacks(self) {
        for shell in self.shells {
            crate::layout::tree_mutation::destroy_shell(shell);
        }
        for reset in self.paintable_row_resets {
            reset.invoke_callback();
        }
        if let Some(host) = self.style_record_host {
            for style_record in self.arena_pinned_style_records {
                // SAFETY: Registration and unregistration keep the host context live.
                unsafe { (host.unpin_style_record)(host.context, style_record) };
            }
        }
    }
}

const MAXIMUM_PRE_ORDER_LABEL_STRIDE: u64 = 1 << 32;

pub(crate) struct LayoutNodeArena {
    chunks: Vec<Box<Chunk>>,
    chunks_by_address: Vec<ChunkAddress>,
    slot_metadata: Vec<SlotMetadata>,
    dom_nodes: Vec<Cell<*mut c_void>>,
    style_records: Vec<Cell<u64>>,
    style_records_pinned_by_arena: Vec<Cell<bool>>,
    style_record_host: Cell<Option<FfiStyleRecordHostCallbacks>>,
    shell_factory: Cell<Option<ShellFactory>>,
    pre_order_labels: Vec<Cell<u64>>,
    pre_order_relabel_count: Cell<u64>,
    free_list: Vec<u32>,
    next_index: u32,
    live_count: u32,
    intrinsic_size_caches: RefCell<Vec<IntrinsicSizeCacheSlot>>,
    table_cell_measurement_cache_misses: Cell<u64>,
    intrinsic_measurements: Cell<u64>,
    saved_abspos_layout_inputs: RefCell<Vec<SavedAbsposLayoutInputsSlot>>,
    default_scroll_shift_anchors: RefCell<Vec<DefaultScrollShiftAnchorSlot>>,
    any_default_scroll_shift_anchor_ever_stored: Cell<bool>,
    text_nodes: Vec<TextNodeSlot>,
    text_chunk_caches: RefCell<Vec<TextChunkCacheSlot>>,
    replaced_content_facts: Vec<ReplacedContentFactsSlot>,
    raw_table_column_spans: HashMap<NodeSlotId, u32>,
    run_used_records: RefCell<Vec<RunRecordSlot>>,
    next_run_nonce: Cell<u64>,
    fc_run_cache_store: super::fc_run_cache::FcRunCacheArenaStore,
    pub(crate) paintable_rows: crate::painting::paintable_rows::PaintableRowStore,
    paint_state: RefCell<crate::painting::paint_state::PaintState>,
    svg_pattern_referencing_nodes: RefCell<Vec<NodeSlotId>>,
    pub(crate) partial_relayout_boundary_roots: RefCell<Vec<NodeSlotId>>,
    pub(crate) boxes_needing_scrollable_overflow_recalculation: RefCell<Vec<NodeSlotId>>,
    pub(crate) needs_full_scrollable_overflow_recalculation: Cell<bool>,
    text_nodes_enrolled_for_content_sync: RefCell<HashSet<NodeSlotId>>,
    nodes_enrolled_for_replaced_content_facts_sync: RefCell<Vec<NodeSlotId>>,
    owner_thread: thread::ThreadId,
}

impl LayoutNodeArena {
    pub(crate) fn new() -> Self {
        Self {
            chunks: Vec::new(),
            chunks_by_address: Vec::new(),
            slot_metadata: Vec::new(),
            dom_nodes: Vec::new(),
            style_records: Vec::new(),
            style_records_pinned_by_arena: Vec::new(),
            style_record_host: Cell::new(None),
            shell_factory: Cell::new(None),
            pre_order_labels: Vec::new(),
            pre_order_relabel_count: Cell::new(0),
            free_list: Vec::new(),
            next_index: 0,
            live_count: 0,
            intrinsic_size_caches: RefCell::new(Vec::new()),
            table_cell_measurement_cache_misses: Cell::new(0),
            intrinsic_measurements: Cell::new(0),
            saved_abspos_layout_inputs: RefCell::new(Vec::new()),
            default_scroll_shift_anchors: RefCell::new(Vec::new()),
            any_default_scroll_shift_anchor_ever_stored: Cell::new(false),
            text_nodes: Vec::new(),
            text_chunk_caches: RefCell::new(Vec::new()),
            replaced_content_facts: Vec::new(),
            raw_table_column_spans: HashMap::default(),
            run_used_records: RefCell::new(Vec::new()),
            next_run_nonce: Cell::new(1),
            fc_run_cache_store: super::fc_run_cache::FcRunCacheArenaStore::default(),
            paintable_rows: crate::painting::paintable_rows::PaintableRowStore::default(),
            paint_state: RefCell::new(crate::painting::paint_state::PaintState::default()),
            svg_pattern_referencing_nodes: RefCell::new(Vec::new()),
            partial_relayout_boundary_roots: RefCell::new(Vec::new()),
            boxes_needing_scrollable_overflow_recalculation: RefCell::new(Vec::new()),
            needs_full_scrollable_overflow_recalculation: Cell::new(false),
            text_nodes_enrolled_for_content_sync: RefCell::new(HashSet::default()),
            nodes_enrolled_for_replaced_content_facts_sync: RefCell::new(Vec::new()),
            owner_thread: thread::current().id(),
        }
    }

    pub(crate) fn register_svg_pattern_referencing_node(&self, node: NodeSlotId) {
        let mut nodes = self.svg_pattern_referencing_nodes.borrow_mut();
        nodes.retain(|candidate| self.slot_is_live(*candidate));
        if nodes.contains(&node) {
            return;
        }
        nodes.push(node);
    }

    pub(crate) fn svg_pattern_referencing_nodes(&self) -> Vec<NodeSlotId> {
        let mut nodes = self.svg_pattern_referencing_nodes.borrow_mut();
        nodes.retain(|candidate| self.slot_is_live(*candidate));
        nodes.clone()
    }

    /// Drops one node's cached intrinsic sizes outright, for the wrap of its epoch:
    /// entries are stamped with the epoch they were measured under, so a stamp reused
    /// after a full lap would match a pre-wrap entry.
    pub(crate) fn drop_intrinsic_size_cache(&self, data: &NodeData) {
        let (index, _) = self.slot_for_data(data);
        if let Some(slot) = self.intrinsic_size_caches.borrow_mut().get_mut(index as usize) {
            *slot = IntrinsicSizeCacheSlot::default();
        }
    }

    pub(crate) fn reset_cached_intrinsic_sizes(&self, node: NodeSlotId) {
        let data = self.data(node);
        let bumped_epoch = data.intrinsic_cache_epoch.get().wrapping_add(1);
        data.intrinsic_cache_epoch.set(bumped_epoch);
        if bumped_epoch == 0 {
            self.drop_intrinsic_size_cache(data);
        }
    }

    pub(crate) fn fc_run_cache_store(&self) -> &super::fc_run_cache::FcRunCacheArenaStore {
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
            self.data(id).fragment_cache_epoch.get() == validity.fragment_cache_epoch
        });
    }

    pub(crate) fn assert_owner_thread(&self) {
        debug_assert_eq!(self.owner_thread, thread::current().id());
    }

    // Freshly created chunks are default-initialized and free() resets slots on release, so
    // allocate() always hands out clean NodeData without writing it again.
    pub(crate) fn allocate(&mut self, construction_facts: FfiNodeConstructionFacts) -> NodeSlotId {
        let slot = self.allocate_unbound(construction_facts.dom_node);
        self.bind_shell(slot, construction_facts);
        slot
    }

    pub(crate) fn allocate_unbound(&mut self, dom_node: *mut c_void) -> NodeSlotId {
        let slot = self.allocate_slot();
        self.dom_nodes[slot.slot_index() as usize].set(dom_node);
        slot
    }

    pub(crate) fn bind_shell(&self, slot: NodeSlotId, construction_facts: FfiNodeConstructionFacts) {
        assert!(
            self.slot_is_live(slot),
            "layout node arena bound a shell to a dead slot"
        );
        let data = self.data(slot);
        assert!(
            data.shell.get().is_null(),
            "layout node arena bound a second shell to a slot"
        );
        self.dom_nodes[slot.slot_index() as usize].set(construction_facts.dom_node);
        data.kind.set(construction_facts.kind);
        data.shell.set(construction_facts.shell);
        data.flags
            .set(super::node_facts::construction_flags(&construction_facts));
        self.enroll_node_for_replaced_content_facts_sync_if_eligible(slot);
    }

    #[cfg(test)]
    pub(crate) fn allocate_for_test(&mut self) -> NodeAllocation {
        NodeAllocation {
            slot: self.allocate_slot(),
        }
    }

    pub(crate) fn enroll_node_for_replaced_content_facts_sync_if_eligible(&self, node: NodeSlotId) {
        let data = self.data(node);
        if !super::node_facts::node_may_have_replaced_content_facts_including_size_containment(data) {
            return;
        }
        let mut enrolled_nodes = self.nodes_enrolled_for_replaced_content_facts_sync.borrow_mut();
        if !enrolled_nodes.contains(&node) {
            enrolled_nodes.push(node);
        }
    }

    fn allocate_slot(&mut self) -> NodeSlotId {
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
            self.dom_nodes.push(Cell::new(std::ptr::null_mut()));
            self.style_records.push(Cell::new(0));
            self.style_records_pinned_by_arena.push(Cell::new(false));
            self.pre_order_labels.push(Cell::new(0));
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
        self.data_mut(index).slot_generation.set(generation);

        NodeSlotId::new(index, generation)
    }

    pub(crate) fn free_subtree(&mut self, root: NodeSlotId) -> FreedSubtree {
        self.assert_owner_thread();

        assert!(!root.is_invalid(), "invalid layout node arena slot ID");
        self.assert_node_is_unlinked_from_parent(root);
        let mut slots_in_pre_order = Vec::new();
        self.for_each_node_in_layout_subtree_in_pre_order(root, |slot| slots_in_pre_order.push(slot));

        let mut shells = Vec::with_capacity(slots_in_pre_order.len());
        let mut paintable_row_resets = Vec::new();
        let mut arena_pinned_style_records = Vec::new();
        for slot in slots_in_pre_order {
            self.mark_descendant_subtree_caches_dirty_from_layout_node(slot);
            shells.push(self.data(slot).shell.get());
            if self.style_records_pinned_by_arena[slot.slot_index() as usize].get() {
                arena_pinned_style_records.push(self.style_records[slot.slot_index() as usize].get());
            }
            self.unlink_children_of_node_being_freed(slot);
            if let Some(reset) = self.free_unlinked_slot(slot) {
                paintable_row_resets.push(reset);
            }
        }
        FreedSubtree {
            shells,
            paintable_row_resets,
            arena_pinned_style_records,
            style_record_host: self.style_record_host.get(),
        }
    }

    fn assert_node_is_unlinked_from_parent(&self, id: NodeSlotId) {
        let data = self.data(id);
        assert!(
            data.parent.get().is_invalid()
                && data.previous_sibling.get().is_invalid()
                && data.next_sibling.get().is_invalid(),
            "layout node arena freed a slot that is still linked under a parent"
        );
    }

    fn unlink_children_of_node_being_freed(&self, id: NodeSlotId) {
        let data = self.data(id);
        loop {
            let child = data.first_child.get();
            if child.is_invalid() {
                break;
            }
            self.unlink_child(id, child);
        }
    }

    fn free_unlinked_slot(&mut self, id: NodeSlotId) -> Option<crate::painting::paintable_rows::PaintableRowReset> {
        let index = id.slot_index();
        let id_generation = id.generation();
        let should_reuse = {
            let metadata = self.metadata_mut(index);
            assert!(metadata.occupied, "layout node arena freed an unused slot");
            assert_eq!(
                metadata.generation, id_generation,
                "layout node arena freed a stale slot generation"
            );
            metadata.generation != u8::MAX
        };

        let paintable_row_reset = self.prepare_paintable_row_freed_reset(index);
        if let Some(reset) = paintable_row_reset {
            self.paintable_row_freed(reset);
        }
        self.pre_order_labels[index as usize].set(0);
        self.metadata_mut(index).occupied = false;
        self.dom_nodes[index as usize].set(std::ptr::null_mut());
        self.style_records[index as usize].set(0);
        self.style_records_pinned_by_arena[index as usize].set(false);

        if let Some(slot) = self.intrinsic_size_caches.get_mut().get_mut(index as usize) {
            *slot = IntrinsicSizeCacheSlot::default();
        }
        if let Some(slot) = self.saved_abspos_layout_inputs.get_mut().get_mut(index as usize) {
            *slot = SavedAbsposLayoutInputsSlot::default();
        }
        if let Some(slot) = self.default_scroll_shift_anchors.get_mut().get_mut(index as usize) {
            *slot = DefaultScrollShiftAnchorSlot::default();
        }
        self.paintable_rows.reset_committed_fragment_link_slot(index);
        if let Some(slot) = self.text_nodes.get_mut(index as usize) {
            *slot = TextNodeSlot::default();
        }
        self.text_nodes_enrolled_for_content_sync.get_mut().remove(&id);
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
        let data = self.data_mut(index);
        debug_assert!(
            data.parent.get().is_invalid()
                && data.first_child.get().is_invalid()
                && data.last_child.get().is_invalid()
                && data.previous_sibling.get().is_invalid()
                && data.next_sibling.get().is_invalid(),
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
        paintable_row_reset
    }

    pub(crate) fn data(&self, id: NodeSlotId) -> &NodeData {
        assert!(!id.is_invalid(), "invalid layout node arena slot ID");
        let index = id.slot_index() as usize;
        let chunk = self
            .chunks
            .get(index / SLOTS_PER_CHUNK)
            .expect("invalid layout node arena slot ID");
        let data = &chunk.slots[index % SLOTS_PER_CHUNK];
        assert_eq!(
            data.slot_generation.get(),
            id.generation(),
            "layout node arena read a stale or unused slot"
        );
        data
    }

    pub(crate) fn set_node_generated_for(&self, id: NodeSlotId, generated_for: u8) {
        self.assert_owner_thread();
        let data = self.data(id);
        data.generated_for.set(generated_for);
    }

    pub(crate) fn set_node_style(&self, id: NodeSlotId, style_record: u64, payloads: *const c_void) {
        self.assert_owner_thread();
        let data = self.data(id);
        data.style.set(payloads);
        self.style_records[id.slot_index() as usize].set(style_record);
        self.enroll_node_for_replaced_content_facts_sync_if_eligible(id);
    }

    pub(crate) fn node_style_record(&self, id: NodeSlotId) -> u64 {
        assert!(
            self.slot_is_live(id),
            "layout node arena read the style record of a dead slot"
        );
        self.style_records[id.slot_index() as usize].get()
    }

    pub(crate) fn node_style_record_is_pinned_by_arena(&self, id: NodeSlotId) -> bool {
        assert!(
            self.slot_is_live(id),
            "layout node arena read the style pin of a dead slot"
        );
        self.style_records_pinned_by_arena[id.slot_index() as usize].get()
    }

    pub(crate) fn set_style_record_host(&self, host: Option<FfiStyleRecordHostCallbacks>) {
        self.style_record_host.set(host);
    }

    fn style_record_host(&self) -> FfiStyleRecordHostCallbacks {
        self.style_record_host
            .get()
            .expect("layout node arena has no style record host")
    }

    pub(crate) fn derive_anonymous_style_record(
        &self,
        parent_style_record: u64,
        kind: FfiAnonymousStyleKind,
        overrides: FfiAnonymousStyleOverrides,
    ) -> FfiDerivedStyleRecord {
        assert!(parent_style_record != 0, "anonymous box parent has no style record");
        let host = self.style_record_host();
        // SAFETY: Registration and unregistration keep the host context live.
        unsafe { (host.derive_anonymous_style_record)(host.context, parent_style_record, kind, overrides) }
    }

    pub(crate) fn reinherit_anonymous_style_record(
        &self,
        style_record: u64,
        parent_style_record: u64,
    ) -> FfiDerivedStyleRecord {
        assert!(style_record != 0 && parent_style_record != 0);
        let host = self.style_record_host();
        // SAFETY: Registration and unregistration keep the host context live.
        unsafe { (host.reinherit_anonymous_style_record)(host.context, style_record, parent_style_record) }
    }

    pub(crate) fn reset_table_box_style_used_by_wrapper(&self, table_box: NodeSlotId) {
        let shell = self.node_shell(table_box);
        assert!(
            !shell.is_null(),
            "table box without a shell cannot reset the properties its wrapper took"
        );
        let host = self.style_record_host();
        // SAFETY: Registration and unregistration keep the host context live, and the shell is live.
        unsafe { (host.reset_table_box_style_used_by_wrapper)(host.context, shell) };
    }

    pub(crate) fn reinherit_anonymous_descendants(&self, node: NodeSlotId) {
        self.assert_owner_thread();
        if self.node_style_record(node) == 0 {
            return;
        }
        let parent = self.data(node).parent.get();
        let parent_is_table_wrapper_of_this_table_box = !parent.is_invalid()
            && self.data(parent).kind.get() == NodeKind::TableWrapper
            && self
                .style_payloads(node)
                .is_some_and(|payloads| ComputedValuesView::new(&payloads.groups).display().is_table_inside());
        if parent_is_table_wrapper_of_this_table_box {
            let derived = self.derive_anonymous_style_record(
                self.node_style_record(node),
                FfiAnonymousStyleKind::TableWrapper,
                FfiAnonymousStyleOverrides::default(),
            );
            self.apply_reinherited_style_record(parent, derived);
            self.reset_table_box_style_used_by_wrapper(node);
        }
        self.reinherit_anonymous_children(node, self.node_style_record(node));
    }

    fn reinherit_anonymous_children(&self, parent: NodeSlotId, parent_style_record: u64) {
        let mut child = self.data(parent).first_child.get();
        while !child.is_invalid() {
            let next_sibling = self.data(child).next_sibling.get();
            let data = self.data(child);
            let flags = data.flags.get();
            let is_anonymous_styled_child = flags & NodeFlag::Anonymous as u32 != 0
                && flags & NodeFlag::HasStyle as u32 != 0
                && data.kind.get() != NodeKind::TableWrapper;
            if is_anonymous_styled_child {
                if self.style_records_pinned_by_arena[child.slot_index() as usize].get() {
                    let derived =
                        self.reinherit_anonymous_style_record(self.node_style_record(child), parent_style_record);
                    self.apply_reinherited_style_record(child, derived);
                    self.reinherit_anonymous_children(child, derived.record);
                } else if !data.shell.get().is_null() {
                    let host = self.style_record_host();
                    // SAFETY: Registration and unregistration keep the host context live, and the shell is live.
                    let descendants_follow = unsafe {
                        (host.reinherit_owned_anonymous_box_style)(host.context, data.shell.get(), parent_style_record)
                    };
                    if descendants_follow {
                        self.reinherit_anonymous_children(child, self.node_style_record(child));
                    }
                }
            }
            child = next_sibling;
        }
    }

    fn apply_reinherited_style_record(&self, slot: NodeSlotId, derived: FfiDerivedStyleRecord) {
        let previous_payloads = self.data(slot).style.get();
        let changes_layout_affecting_style =
            !style_payloads_equal_in_layout_affecting_groups(previous_payloads, derived.payloads);
        self.replace_arena_pinned_style_record(slot, derived);
        if changes_layout_affecting_style {
            self.bump_fragment_cache_epoch_of_self_and_ancestors(slot);
            self.reset_cached_intrinsic_sizes_of_self_and_ancestors(slot);
        }
        let mut child = self.data(slot).first_child.get();
        while !child.is_invalid() {
            if super::node_facts::kind_is_text(self.data(child).kind.get()) {
                self.enroll_text_node_for_content_sync(child);
            }
            child = self.data(child).next_sibling.get();
        }
        let shell = self.data(slot).shell.get();
        if !shell.is_null() {
            let host = self.style_record_host();
            // SAFETY: Registration and unregistration keep the host context live, and the shell is live.
            unsafe { (host.shell_style_changed)(host.context, shell) };
        } else {
            self.refresh_insets_use_anchor_functions_flag(slot);
        }
    }

    pub(crate) fn enroll_text_node_for_content_sync(&self, node: NodeSlotId) {
        self.text_nodes_enrolled_for_content_sync.borrow_mut().insert(node);
    }

    pub(crate) fn stamp_anonymous_box(&self, slot: NodeSlotId, kind: NodeKind, derived: FfiDerivedStyleRecord) {
        self.assert_owner_thread();
        let data = self.data(slot);
        assert_eq!(
            data.kind.get(),
            NodeKind::Unset,
            "stamped an anonymous box onto a bound slot"
        );
        assert!(derived.record != 0 && !derived.payloads.is_null());
        data.kind.set(kind);
        data.flags
            .set(super::node_facts::construction_flags(&FfiNodeConstructionFacts {
                kind,
                shell: std::ptr::null_mut(),
                dom_node: std::ptr::null_mut(),
                is_anonymous: true,
                is_html_input_element: false,
                is_html_html_element: false,
                is_document_element: false,
                is_in_user_agent_shadow_tree: false,
                uses_button_layout: false,
                is_editing_host: false,
                is_body: false,
            }));
        self.style_records[slot.slot_index() as usize].set(derived.record);
        self.style_records_pinned_by_arena[slot.slot_index() as usize].set(true);
        data.style.set(derived.payloads);
        self.enroll_node_for_replaced_content_facts_sync_if_eligible(slot);
    }

    pub(crate) fn refresh_insets_use_anchor_functions_flag(&self, slot: NodeSlotId) {
        let insets_use_anchor_functions = self
            .style_payloads(slot)
            .is_some_and(|payloads| style_insets_use_anchor_functions(ComputedValuesView::new(&payloads.groups)));
        self.set_node_flag(slot, NodeFlag::InsetsUseAnchorFunctions, insets_use_anchor_functions);
    }

    pub(crate) fn set_shell_factory(&self, factory: Option<ShellFactory>) {
        self.shell_factory.set(factory);
    }

    fn materialize_shell(&self, id: NodeSlotId) -> *mut c_void {
        let Some((context, factory)) = self.shell_factory.get() else {
            return std::ptr::null_mut();
        };
        let data = self.data(id);
        if data.kind.get() == NodeKind::Unset || data.flags.get() & NodeFlag::Anonymous as u32 == 0 {
            return std::ptr::null_mut();
        }
        // SAFETY: Registration and unregistration keep the factory context live; the factory binds a
        // shell to this live slot and writes nothing but the slot's shell cell.
        unsafe { factory(context, id, data.kind.get()) };
        data.shell.get()
    }

    pub(crate) fn shell_count(&self) -> u32 {
        let mut count = 0;
        for (index, metadata) in self.slot_metadata.iter().enumerate() {
            if metadata.occupied
                && !self
                    .data(NodeSlotId::new(index as u32, metadata.generation))
                    .shell
                    .get()
                    .is_null()
            {
                count += 1;
            }
        }
        count
    }

    pub(crate) fn replace_arena_pinned_style_record(&self, slot: NodeSlotId, derived: FfiDerivedStyleRecord) {
        self.assert_owner_thread();
        assert!(
            self.node_style_record_is_pinned_by_arena(slot),
            "replaced a style record the arena does not pin"
        );
        assert!(derived.record != 0 && !derived.payloads.is_null());
        let previous_style_record = self.style_records[slot.slot_index() as usize].replace(derived.record);
        self.data(slot).style.set(derived.payloads);
        self.enroll_node_for_replaced_content_facts_sync_if_eligible(slot);
        if previous_style_record != derived.record {
            let host = self.style_record_host();
            // SAFETY: Registration and unregistration keep the host context live.
            unsafe { (host.unpin_style_record)(host.context, previous_style_record) };
        } else {
            let host = self.style_record_host();
            // SAFETY: As above; the derivation pinned the record a second time.
            unsafe { (host.unpin_style_record)(host.context, derived.record) };
        }
    }

    pub(crate) fn attach_shell(&self, slot: NodeSlotId, shell: *mut c_void) {
        self.assert_owner_thread();
        assert!(!shell.is_null());
        let data = self.data(slot);
        assert!(
            data.shell.get().is_null(),
            "layout node arena attached a second shell to a slot"
        );
        data.shell.set(shell);
    }

    pub(crate) fn set_node_flag(&self, id: NodeSlotId, flag: NodeFlag, value: bool) {
        self.assert_owner_thread();
        let data = self.data(id);
        let mut updated = data.flags.get();
        if value {
            updated |= flag as u32;
        } else {
            updated &= !(flag as u32);
        }
        data.flags.set(updated);
    }

    pub(crate) fn node_has_compositor_animation_frame(
        &self,
        id: NodeSlotId,
        kind: super::node_data::CompositorAnimationFrameKind,
    ) -> bool {
        self.data(id).compositor_animation_frame_kinds.get() & kind as u8 != 0
    }

    pub(crate) fn set_node_needs_compositor_animation_frame(
        &self,
        id: NodeSlotId,
        kind: super::node_data::CompositorAnimationFrameKind,
        value: bool,
    ) {
        self.assert_owner_thread();
        let frame_kinds = &self.data(id).compositor_animation_frame_kinds;
        let mut updated = frame_kinds.get();
        if value {
            updated |= kind as u8;
        } else {
            updated &= !(kind as u8);
        }
        frame_kinds.set(updated);
    }

    pub(crate) fn for_each_node_in_layout_subtree_in_pre_order(
        &self,
        root: NodeSlotId,
        mut callback: impl FnMut(NodeSlotId),
    ) {
        self.for_each_node_in_layout_subtree_in_pre_order_with_pruning(root, |node| {
            callback(node);
            true
        });
    }

    pub(crate) fn for_each_node_in_layout_subtree_in_pre_order_with_pruning(
        &self,
        root: NodeSlotId,
        mut visit_node_and_report_whether_to_descend: impl FnMut(NodeSlotId) -> bool,
    ) {
        let mut current = root;
        loop {
            let descend_into_children = visit_node_and_report_whether_to_descend(current);
            let data = self.data(current);
            let (parent, first_child, next_sibling) =
                { (data.parent.get(), data.first_child.get(), data.next_sibling.get()) };

            if descend_into_children && !first_child.is_invalid() {
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
                let data = self.data(current);
                let next_sibling = data.next_sibling.get();
                if !next_sibling.is_invalid() {
                    current = next_sibling;
                    break;
                }
                current = data.parent.get();
            }
            if current == root {
                break;
            }
        }
    }

    pub(crate) fn reset_layout_update_flags_in_subtree(&self, root: NodeSlotId) {
        self.assert_owner_thread();
        let flags_to_clear = NodeFlag::NeedsLayoutUpdate as u32 | NodeFlag::NeedsOwnGeometryUpdate as u32;
        self.for_each_node_in_layout_subtree_in_pre_order(root, |node| {
            let data = self.data(node);
            data.flags.set(data.flags.get() & !flags_to_clear);
        });
    }

    fn node_is_capable_of_forming_a_containing_block(&self, id: NodeSlotId) -> bool {
        let data = self.data(id);
        super::node_facts::node_forms_containing_block_for_children(data, self.node_style_if_live(id))
    }

    fn nearest_ancestor_capable_of_forming_a_containing_block(&self, node: NodeSlotId) -> NodeSlotId {
        let mut ancestor = self.data(node).parent.get();
        while !ancestor.is_invalid() {
            if self.node_is_capable_of_forming_a_containing_block(ancestor) {
                return ancestor;
            }
            ancestor = self.data(ancestor).parent.get();
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
        data.inline_containing_block.set(NodeSlotId::INVALID);

        let kind = data.kind.get();
        if super::node_facts::kind_is_text(kind) {
            let containing_block = self.nearest_ancestor_capable_of_forming_a_containing_block(node);
            data.containing_block.set(containing_block);
            return;
        }

        let position = self
            .node_style_if_live(node)
            .map_or(positioning::STATIC, |style| style.box_values().position);

        // https://drafts.csswg.org/css-position-3/#absolute-cb
        if position == positioning::ABSOLUTE {
            let mut ancestor = data.parent.get();
            while !ancestor.is_invalid() && !establishes_positioning_containing_blocks(self, ancestor).0 {
                ancestor = self.data(ancestor).parent.get();
            }
            data.containing_block.set(ancestor);
            if !ancestor.is_invalid() {
                // SAFETY: Both slots are live; the callback only reads DOM ancestry
                // and per-node facts through the shells and does not mutate the tree.
                let inline_containing_block = unsafe {
                    let node_shell = self.node_shell(node);
                    let ancestor_shell = self.node_shell(ancestor);
                    inline_cb_lookup(node_shell, ancestor_shell)
                };
                data.inline_containing_block.set(inline_containing_block);
            }
            return;
        }

        // https://drafts.csswg.org/css-position-3/#fixed-cb
        if position == positioning::FIXED {
            // The containing block is established by the nearest ancestor box that establishes an fixed positioning
            // containing block, with the bounds of the containing block determined identically to the absolute positioning
            // containing block.
            let mut last_visited = node;
            let mut ancestor = data.parent.get();
            while !ancestor.is_invalid() && !establishes_positioning_containing_blocks(self, ancestor).1 {
                last_visited = ancestor;
                ancestor = self.data(ancestor).parent.get();
            }
            // If no ancestor establishes one, the box's fixed positioning containing block is the initial fixed containing
            // block:
            //  - in continuous media, the layout viewport (whose size matches the dynamic viewport size); as a result,
            //    fixed boxes do not move when the document is scrolled.
            // FIXME: - in paged media, the page area of each page; fixed positioned boxes are thus replicated on every
            //   page. (They are fixed with respect to the page box only, and are not affected by being seen through a
            //   viewport; as in the case of print preview, for example.)
            let containing_block = if ancestor.is_invalid() { last_visited } else { ancestor };
            data.containing_block.set(containing_block);
            return;
        }

        let containing_block = self.nearest_ancestor_capable_of_forming_a_containing_block(node);
        data.containing_block.set(containing_block);
    }

    fn derive_abspos_escape_flags_for_node(&self, node: NodeSlotId) {
        let data = self.data(node);
        let kind = data.kind.get();
        if !super::node_facts::kind_is_box(kind) {
            return;
        }
        self.set_node_flag(node, NodeFlag::AbsposDescendantEscapes, false);
        if !self
            .node_style_if_live(node)
            .is_some_and(|style| style.is_absolutely_positioned())
        {
            return;
        }
        let containing_block = data.containing_block.get();
        let mut ancestor = data.parent.get();
        while !ancestor.is_invalid() && ancestor != containing_block {
            let ancestor_kind = self.data(ancestor).kind.get();
            if super::node_facts::kind_is_box(ancestor_kind) {
                self.set_node_flag(ancestor, NodeFlag::AbsposDescendantEscapes, true);
            }
            ancestor = self.data(ancestor).parent.get();
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
        self.for_each_node_in_layout_subtree_in_pre_order(root, |node| {
            self.recompute_containing_block_for_node(node, inline_cb_lookup);
            self.derive_abspos_escape_flags_for_node(node);
        });
    }

    fn slot_for_data(&self, data: &NodeData) -> (u32, SlotMetadata) {
        let data_address = std::ptr::from_ref(data) as usize;
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
        let generation = data.slot_generation.get();
        assert_eq!(
            generation, metadata.generation,
            "layout node arena access used a stale slot"
        );
        (index, metadata)
    }

    pub(crate) fn node_pre_order_label(&self, id: NodeSlotId) -> u64 {
        let _ = self.data(id);
        self.pre_order_labels[id.slot_index() as usize].get()
    }

    fn set_node_pre_order_label(&self, id: NodeSlotId, label: u64) {
        self.pre_order_labels[id.slot_index() as usize].set(label);
    }

    pub(crate) fn pre_order_relabel_count(&self) -> u64 {
        self.pre_order_relabel_count.get()
    }

    pub(crate) fn count_nodes_in_layout_subtree(&self, root: NodeSlotId) -> u64 {
        let mut count = 0u64;
        self.for_each_node_in_layout_subtree_in_pre_order(root, |_| count += 1);
        count
    }

    fn last_descendant_in_pre_order(&self, node: NodeSlotId) -> NodeSlotId {
        let mut current = node;
        loop {
            let last_child = self.data(current).last_child.get();
            if last_child.is_invalid() {
                return current;
            }
            current = last_child;
        }
    }

    fn pre_order_label_of_subtree_successor(&self, node: NodeSlotId) -> u64 {
        let mut current = node;
        loop {
            let data = self.data(current);
            let (parent, next_sibling) = { (data.parent.get(), data.next_sibling.get()) };
            if !next_sibling.is_invalid() {
                return self.node_pre_order_label(next_sibling);
            }
            if parent.is_invalid() {
                return u64::MAX;
            }
            current = parent;
        }
    }

    fn assign_pre_order_labels_to_inserted_subtree(&self, parent: NodeSlotId, child: NodeSlotId) {
        let child_data = self.data(child);
        let (previous_sibling, next_sibling) = { (child_data.previous_sibling.get(), child_data.next_sibling.get()) };
        let lower = if previous_sibling.is_invalid() {
            self.node_pre_order_label(parent)
        } else {
            self.node_pre_order_label(self.last_descendant_in_pre_order(previous_sibling))
        };
        let upper = if next_sibling.is_invalid() {
            self.pre_order_label_of_subtree_successor(parent)
        } else {
            self.node_pre_order_label(next_sibling)
        };
        debug_assert!(lower < upper, "pre-order labels lost their strict order");
        let inserted_node_count = self.count_nodes_in_layout_subtree(child);
        let stride = ((upper - lower) / (inserted_node_count + 1)).min(MAXIMUM_PRE_ORDER_LABEL_STRIDE);
        if stride >= 2 {
            // Placement is biased toward the insertion direction, so a one-directional hot
            // spot consumes the gap linearly instead of halving it.
            let mut position_in_subtree = 0u64;
            self.for_each_node_in_layout_subtree_in_pre_order(child, |node| {
                position_in_subtree += 1;
                let label = if next_sibling.is_invalid() {
                    lower + stride * position_in_subtree
                } else {
                    upper - stride * (inserted_node_count + 1 - position_in_subtree)
                };
                self.set_node_pre_order_label(node, label);
            });
            debug_assert!(lower < self.node_pre_order_label(child));
            debug_assert!(self.node_pre_order_label(self.last_descendant_in_pre_order(child)) < upper);
            return;
        }
        let mut ancestor = parent;
        loop {
            let ancestor_parent = self.data(ancestor).parent.get();
            if ancestor_parent.is_invalid() {
                self.set_node_pre_order_label(ancestor, 0);
                let spread_succeeded = self.spread_pre_order_labels_evenly_over_descendants(ancestor, 0, u64::MAX);
                assert!(spread_succeeded, "pre-order label space exhausted");
                return;
            }
            let ancestor_lower = self.node_pre_order_label(ancestor);
            let ancestor_upper = self.pre_order_label_of_subtree_successor(ancestor);
            if self.spread_pre_order_labels_evenly_over_descendants(ancestor, ancestor_lower, ancestor_upper) {
                return;
            }
            ancestor = ancestor_parent;
        }
    }

    fn spread_pre_order_labels_evenly_over_descendants(
        &self,
        subtree_root: NodeSlotId,
        lower: u64,
        upper: u64,
    ) -> bool {
        let descendant_count = self.count_nodes_in_layout_subtree(subtree_root) - 1;
        if descendant_count == 0 {
            return true;
        }
        let step = (upper - lower) / (descendant_count + 1);
        if step < 2 {
            return false;
        }
        let mut position_in_subtree = 0u64;
        self.for_each_node_in_layout_subtree_in_pre_order(subtree_root, |node| {
            if node == subtree_root {
                return;
            }
            position_in_subtree += 1;
            self.set_node_pre_order_label(node, lower + step * position_in_subtree);
        });
        self.pre_order_relabel_count.set(self.pre_order_relabel_count.get() + 1);
        true
    }

    #[cfg(debug_assertions)]
    fn nodes_share_a_layout_tree_root(&self, node: NodeSlotId, other: NodeSlotId) -> bool {
        let root_of = |mut slot: NodeSlotId| loop {
            let parent = self.data(slot).parent.get();
            if parent.is_invalid() {
                return slot;
            }
            slot = parent;
        };
        root_of(node) == root_of(other)
    }

    pub(crate) fn is_before(&self, node: &NodeData, other: &NodeData) -> bool {
        let (node_index, node_metadata) = self.slot_for_data(node);
        let (other_index, other_metadata) = self.slot_for_data(other);
        let node = NodeSlotId::new(node_index, node_metadata.generation);
        let other = NodeSlotId::new(other_index, other_metadata.generation);
        assert_ne!(node, other, "a layout node cannot precede itself");
        #[cfg(debug_assertions)]
        debug_assert!(
            self.nodes_share_a_layout_tree_root(node, other),
            "layout nodes belong to different trees"
        );
        self.node_pre_order_label(node) < self.node_pre_order_label(other)
    }

    pub(crate) fn intrinsic_block_size_cache_get(
        &self,
        data: &NodeData,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
    ) -> Option<IntrinsicBlockSizeMeasurement> {
        assert!(
            matches!(
                kind,
                IntrinsicSizeCacheKind::MinContentBlock | IntrinsicSizeCacheKind::MaxContentBlock
            ),
            "block size cache kind must use the block axis"
        );

        let (index, metadata) = self.slot_for_data(data);
        let caches = self.intrinsic_size_caches.borrow();
        let slot = caches.get(index as usize)?;
        if slot.generation != metadata.generation || slot.epoch != data.intrinsic_cache_epoch.get() {
            return None;
        }
        let map = slot
            .sizes
            .as_ref()?
            .block_sizes(kind)
            .expect("block size cache kind must use the block axis");
        intrinsic_cache_lookup(map, key)
    }

    fn with_intrinsic_size_maps_mut(&self, data: &NodeData, callback: impl FnOnce(&mut IntrinsicSizeMaps)) {
        let (index, metadata) = self.slot_for_data(data);
        let mut caches = self.intrinsic_size_caches.borrow_mut();
        if caches.len() <= index as usize {
            caches.resize_with(index as usize + 1, IntrinsicSizeCacheSlot::default);
        }
        let slot = &mut caches[index as usize];
        if slot.generation != metadata.generation || slot.epoch != data.intrinsic_cache_epoch.get() {
            *slot = IntrinsicSizeCacheSlot {
                generation: metadata.generation,
                epoch: data.intrinsic_cache_epoch.get(),
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
        value: IntrinsicBlockSizeMeasurement,
    ) {
        self.with_intrinsic_size_maps_mut(data, |maps| {
            let map = maps
                .block_sizes_mut(kind)
                .expect("block size cache kind must use the block axis");
            intrinsic_cache_store(map, key, value);
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

        let (index, metadata) = self.slot_for_data(data);
        let caches = self.intrinsic_size_caches.borrow();
        let slot = caches.get(index as usize)?;
        if slot.generation != metadata.generation || slot.epoch != data.intrinsic_cache_epoch.get() {
            return None;
        }
        let map = slot
            .sizes
            .as_ref()?
            .inline_measurements(kind)
            .expect("inline measurement cache kind must use the inline axis");
        intrinsic_cache_lookup(map, key)
    }

    pub(crate) fn intrinsic_inline_size_depends_on_block_size(
        &self,
        data: &NodeData,
        compute: impl FnOnce() -> bool,
    ) -> bool {
        let (index, metadata) = self.slot_for_data(data);
        {
            let caches = self.intrinsic_size_caches.borrow();
            if let Some(slot) = caches.get(index as usize)
                && slot.generation == metadata.generation
                && slot.epoch == data.intrinsic_cache_epoch.get()
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
            let map = maps
                .inline_measurements_mut(kind)
                .expect("inline measurement cache kind must use the inline axis");
            intrinsic_cache_store(map, key, value);
        });
    }

    pub(crate) fn table_cell_measurement_cache_get(
        &self,
        data: &NodeData,
        key: TableCellMeasurementKey,
    ) -> Option<TableCellMeasurement> {
        let (index, metadata) = self.slot_for_data(data);
        let caches = self.intrinsic_size_caches.borrow();
        let slot = caches.get(index as usize)?;
        if slot.generation != metadata.generation || slot.epoch != data.intrinsic_cache_epoch.get() {
            return None;
        }
        slot.sizes.as_ref()?.table_cell_measurements.get(&key).copied()
    }

    pub(crate) fn table_cell_measurement_cache_put(
        &self,
        data: &NodeData,
        key: TableCellMeasurementKey,
        value: TableCellMeasurement,
    ) {
        self.with_intrinsic_size_maps_mut(data, |maps| {
            maps.table_cell_measurements.insert(key, value);
        });
    }

    pub(crate) fn note_table_cell_measurement_cache_miss(&self) {
        self.table_cell_measurement_cache_misses
            .set(self.table_cell_measurement_cache_misses.get() + 1);
    }

    pub(crate) fn table_cell_measurement_cache_miss_count(&self) -> u64 {
        self.table_cell_measurement_cache_misses.get()
    }

    pub(crate) fn note_intrinsic_measurement(&self) {
        self.intrinsic_measurements.set(self.intrinsic_measurements.get() + 1);
    }

    pub(crate) fn intrinsic_measurement_count(&self) -> u64 {
        self.intrinsic_measurements.get()
    }

    pub(crate) fn saved_abspos_layout_inputs(&self, data: &NodeData) -> Option<AbsposLayoutInputs> {
        let (index, metadata) = self.slot_for_data(data);
        let slots = self.saved_abspos_layout_inputs.borrow();
        let inputs = slots
            .get(index as usize)
            .filter(|slot| slot.generation == metadata.generation)
            .and_then(|slot| slot.inputs.as_deref().copied());

        let flags = data.flags.get();
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

    pub(crate) fn set_saved_abspos_layout_inputs(&self, data: &NodeData, inputs: Option<AbsposLayoutInputs>) {
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
        let mut value = data.flags.get() & !saved_abspos_flags;
        if let Some(inputs) = inputs {
            value |= NodeFlag::HasSavedAbsposLayoutInputs as u32;
            if inputs.containing_block_info.derives_from_own_computed_values {
                value |= NodeFlag::SavedAbsposCbDerivesFromOwnComputedValues as u32;
            }
            if inputs.static_position_rect.alignment_derives_from_own_computed_values {
                value |= NodeFlag::SavedAbsposAlignmentDerivesFromOwnComputedValues as u32;
            }
        }
        data.flags.set(value);
    }

    pub(crate) fn set_default_scroll_shift(
        &self,
        id: NodeSlotId,
        anchor: NodeSlotId,
        compensates_for_horizontal_scroll: bool,
        compensates_for_vertical_scroll: bool,
    ) {
        let anchor_is_live = self.slot_is_live(anchor);
        {
            let mut slots = self.default_scroll_shift_anchors.borrow_mut();
            let index = id.slot_index() as usize;
            if anchor_is_live {
                if slots.len() <= index {
                    slots.resize_with(index + 1, DefaultScrollShiftAnchorSlot::default);
                }
                slots[index] = DefaultScrollShiftAnchorSlot {
                    generation: id.generation(),
                    anchor,
                };
            } else if let Some(slot) = slots.get_mut(index) {
                *slot = DefaultScrollShiftAnchorSlot::default();
            }
        }
        self.set_node_flag(
            id,
            NodeFlag::CompensatesForHorizontalScroll,
            anchor_is_live && compensates_for_horizontal_scroll,
        );
        self.set_node_flag(
            id,
            NodeFlag::CompensatesForVerticalScroll,
            anchor_is_live && compensates_for_vertical_scroll,
        );
        if anchor_is_live {
            self.any_default_scroll_shift_anchor_ever_stored.set(true);
        }
    }

    pub(crate) fn default_scroll_shift_anchor(&self, id: NodeSlotId) -> NodeSlotId {
        if !self.any_default_scroll_shift_anchor_ever_stored.get() {
            return NodeSlotId::INVALID;
        }
        let slots = self.default_scroll_shift_anchors.borrow();
        let Some(slot) = slots.get(id.slot_index() as usize) else {
            return NodeSlotId::INVALID;
        };
        if slot.generation != id.generation() || !self.slot_is_live(slot.anchor) {
            return NodeSlotId::INVALID;
        }
        slot.anchor
    }

    pub(crate) fn may_have_default_scroll_shift_anchor(&self) -> bool {
        self.any_default_scroll_shift_anchor_ever_stored.get()
    }

    pub(crate) fn committed_fragment_link(&self, data: &NodeData) -> Option<super::fragment_tree::FragmentLink> {
        let (index, metadata) = self.slot_for_data(data);
        let link = self
            .paintable_rows
            .committed_fragment_link_cloned(index, metadata.generation);

        let flags = data.flags.get();
        assert_eq!(
            flags & NodeFlag::HasCommittedFragmentLink as u32 != 0,
            link.is_some(),
            "committed fragment link presence flag disagrees with the arena side table"
        );
        link
    }

    pub(crate) fn set_committed_fragment_link(&self, data: &NodeData, link: super::fragment_tree::FragmentLink) {
        let (index, metadata) = self.slot_for_data(data);
        self.paintable_rows
            .set_committed_fragment_link(index, metadata.generation, link);

        data.flags
            .set(data.flags.get() | NodeFlag::HasCommittedFragmentLink as u32);
    }

    pub(crate) fn take_committed_fragment_link(&self, data: &NodeData) -> Option<super::fragment_tree::FragmentLink> {
        let (index, metadata) = self.slot_for_data(data);
        let link = self
            .paintable_rows
            .take_committed_fragment_link(index, metadata.generation);

        assert_eq!(
            data.flags.get() & NodeFlag::HasCommittedFragmentLink as u32 != 0,
            link.is_some(),
            "committed fragment link presence flag disagrees with the arena side table"
        );
        data.flags
            .set(data.flags.get() & !(NodeFlag::HasCommittedFragmentLink as u32));
        link
    }

    pub(crate) fn clear_committed_fragment_link(&self, id: NodeSlotId) {
        drop(self.take_committed_fragment_link(self.data(id)));
    }

    fn text_node_state_mut(&mut self, id: NodeSlotId) -> &mut TextNodeState {
        self.assert_owner_thread();
        self.data(id);
        let index = id.slot_index() as usize;
        if self.text_nodes.len() <= index {
            self.text_nodes.resize_with(index + 1, TextNodeSlot::default);
        }
        let slot = &mut self.text_nodes[index];
        if slot.generation != id.generation() {
            *slot = TextNodeSlot {
                generation: id.generation(),
                ..TextNodeSlot::default()
            };
        }
        slot.state.get_or_insert_with(Default::default)
    }

    fn text_node_state(&self, id: NodeSlotId) -> Option<&TextNodeState> {
        if !self.slot_is_live(id) {
            return None;
        }
        self.text_nodes
            .get(id.slot_index() as usize)
            .filter(|slot| slot.generation == id.generation())
            .and_then(|slot| slot.state.as_deref())
    }

    pub(crate) fn set_text_content(&mut self, id: NodeSlotId, content: TextContent) {
        let state = self.text_node_state_mut(id);
        if let Some(previous) = state.content.as_mut()
            && previous.has_same_content_as(&content)
        {
            previous.rendering_key = content.rendering_key;
            return;
        }
        state.content = Some(content);
        if let Some(slot) = self.text_chunk_caches.get_mut().get_mut(id.slot_index() as usize) {
            *slot = TextChunkCacheSlot::default();
        }
        // Publication can happen through a C++ text read before the enrolled
        // sync runs. Invalidate here so every publication invalidates layout,
        // including mapping-only changes with identical rendered code units.
        self.bump_fragment_cache_epoch_of_self_and_ancestors(id);
    }

    pub(super) fn invalidate_text_content(&mut self, id: NodeSlotId) {
        self.data(id);
        if let Some(slot) = self.text_nodes.get_mut(id.slot_index() as usize)
            && slot.generation == id.generation()
            && let Some(state) = slot.state.as_mut()
            && let Some(content) = state.content.as_mut()
        {
            content.rendering_key = None;
        }
        self.enroll_text_node_for_content_sync(id);
    }

    pub(super) fn finish_text_content_sync(&self, id: NodeSlotId) {
        self.text_nodes_enrolled_for_content_sync.borrow_mut().remove(&id);
    }

    pub(super) fn take_text_nodes_for_content_sync(&self) -> HashSet<NodeSlotId> {
        std::mem::take(&mut *self.text_nodes_enrolled_for_content_sync.borrow_mut())
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
        self.text_node_state(id)?.content.as_ref()
    }

    pub(super) fn set_first_letter_slices(
        &mut self,
        first_letter: NodeSlotId,
        remainder: NodeSlotId,
        letter_end: usize,
        source_length: usize,
    ) {
        assert_ne!(first_letter, remainder);
        assert_eq!(self.data(first_letter).kind.get(), NodeKind::TextSliceNode);
        assert_eq!(self.data(remainder).kind.get(), NodeKind::TextSliceNode);
        assert!(letter_end <= source_length);
        self.text_node_state_mut(first_letter).source_range = Some(FfiTextSourceRange {
            start: 0,
            length: letter_end,
        });
        let remainder_state = self.text_node_state_mut(remainder);
        remainder_state.source_range = Some(FfiTextSourceRange {
            start: letter_end,
            length: source_length - letter_end,
        });
        remainder_state.first_letter = first_letter;
        self.invalidate_text_content(first_letter);
        self.invalidate_text_content(remainder);
    }

    pub(crate) fn text_source_range(&self, id: NodeSlotId, source_length: usize) -> FfiTextSourceRange {
        self.data(id);
        self.text_node_state(id)
            .and_then(|state| state.source_range)
            .unwrap_or(FfiTextSourceRange {
                start: 0,
                length: source_length,
            })
    }

    pub(crate) fn text_fragments(&self, primary: NodeSlotId) -> FfiTextFragments {
        let mut fragments = FfiTextFragments {
            nodes: [NodeSlotId::INVALID; 2],
            length: 0,
        };
        if !self.slot_is_live(primary) || !super::node_facts::kind_is_text(self.data(primary).kind.get()) {
            return fragments;
        }
        if let Some(state) = self.text_node_state(primary)
            && self.slot_is_live(state.first_letter)
        {
            fragments.nodes[0] = state.first_letter;
            fragments.length = 1;
        }
        fragments.nodes[fragments.length] = primary;
        fragments.length += 1;
        fragments
    }

    pub(crate) fn text_fragment_containing(
        &self,
        primary: NodeSlotId,
        dom_offset: usize,
        source_length: usize,
    ) -> NodeSlotId {
        self.text_fragments(primary)
            .as_slice()
            .iter()
            .copied()
            .find(|&fragment| {
                let range = self.text_source_range(fragment, source_length);
                dom_offset >= range.start && dom_offset <= range.start + range.length
            })
            .unwrap_or(NodeSlotId::INVALID)
    }

    /// The node's group payload pointer array, read in place from the
    /// Rust-owned style container that NodeData.style addresses. The node's
    /// retained immutable ComputedValues owns the container, and the pointer
    /// is only replaced between passes, so the array stays valid for as long
    /// as the node occupies its arena slot.
    pub(crate) fn style_payloads(&self, id: NodeSlotId) -> Option<&FfiStylePayloads> {
        let style = self.data(id).style.get();
        // SAFETY: A non-null style pointer addresses the container's group
        // pointer array, which FfiStylePayloads mirrors exactly.
        (!style.is_null()).then(|| unsafe { &*style.cast::<FfiStylePayloads>() })
    }

    pub(crate) fn text_chunks(
        &self,
        id: NodeSlotId,
        key: TextChunkCacheKey,
        compute: impl FnOnce() -> Vec<super::text_chunker::TextChunk>,
    ) -> Rc<CachedTextChunks> {
        // data() validates that id names a live slot with a matching generation.
        self.data(id);
        let index = id.slot_index() as usize;
        {
            let slots = self.text_chunk_caches.borrow();
            if let Some(slot) = slots.get(index)
                && slot.generation == id.generation()
                && let Some(entry) = slot.entry.as_ref()
                && entry.key == key
            {
                return entry.clone();
            }
        }

        // A nested measurement can request a different key while an iterator
        // still uses the previous chunks. Keep both the chunks and their fonts
        // alive until that iterator finishes.
        let entry = Rc::new(CachedTextChunks {
            key,
            // SAFETY: The caller derives the key's cascade-list pointer from a live style snapshot.
            _retained_font_cascade_list: unsafe {
                libgfx_rust::font::RetainedFontCascadeList::retain(key.font_cascade_list)
            },
            chunks: compute(),
        });
        let mut slots = self.text_chunk_caches.borrow_mut();
        if slots.len() <= index {
            slots.resize_with(index + 1, TextChunkCacheSlot::default);
        }
        slots[index] = TextChunkCacheSlot {
            generation: id.generation(),
            entry: Some(entry.clone()),
        };
        entry
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
    // NB: Bumps can legitimately run while another document's layout pass is on the stack (a
    // parent pass sizing a child navigable's viewport invalidates the child document), so the
    // helpers must not assert against the process-global pass flag. A bump landing between a
    // run's probe and its store is handled by storing the probe-time validity, which turns it
    // into a fail-safe miss.
    fn invalidate_at_and_above(&self, mut node: NodeSlotId, invalidation: AncestorInvalidation) {
        let epochs_enabled =
            super::fc_run_cache::fc_run_cache_mode_from_environment() != super::fc_run_cache::FcRunCacheMode::Disabled;
        let paintable_rows = self.paintable_rows();
        while !node.is_invalid() {
            let data = self.data(node);
            if invalidation == AncestorInvalidation::StructuralChange {
                self.fc_run_cache_store.note_inline_layout_damage(node);
            }
            if epochs_enabled {
                data.fragment_cache_epoch
                    .set(data.fragment_cache_epoch.get().wrapping_add(1));
            }
            let (kind, parent) = (data.kind.get(), data.parent.get());
            if super::node_facts::kind_is_box(kind) {
                paintable_rows.clear_cached_overflow_data(node);
            }
            node = parent;
        }
    }

    pub(crate) fn note_structural_change_at_and_above(&self, node: NodeSlotId) {
        self.invalidate_at_and_above(node, AncestorInvalidation::StructuralChange);
    }

    pub(crate) fn bump_fragment_cache_epoch_of_self_and_ancestors(&self, node: NodeSlotId) {
        self.invalidate_at_and_above(node, AncestorInvalidation::ContentChange);
    }

    pub(crate) fn insert_child(&self, parent: NodeSlotId, child: NodeSlotId, before: NodeSlotId) {
        self.assert_owner_thread();
        assert_ne!(parent, child, "a layout node cannot become its own child");
        let parent_data = self.data(parent);
        let child_data = self.data(child);

        let child_parent = child_data.parent.get();
        let child_previous_sibling = child_data.previous_sibling.get();
        let child_next_sibling = child_data.next_sibling.get();
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
            parent_data.first_child.get().is_invalid(),
            parent_data.last_child.get().is_invalid(),
            "layout node child list endpoints disagree"
        );

        #[cfg(debug_assertions)]
        {
            let mut ancestor = parent;
            while !ancestor.is_invalid() {
                assert_ne!(ancestor, child, "layout node insertion would create a cycle");
                ancestor = self.data(ancestor).parent.get();
            }
        }

        let previous = if before.is_invalid() {
            parent_data.last_child.get()
        } else {
            assert_ne!(before, child, "a layout node cannot be inserted before itself");
            let before_data = self.data(before);
            assert_eq!(
                before_data.parent.get(),
                parent,
                "insertion reference is not a child of the parent"
            );
            before_data.previous_sibling.get()
        };

        child_data.parent.set(parent);
        child_data.previous_sibling.set(previous);
        child_data.next_sibling.set(before);
        if previous.is_invalid() {
            parent_data.first_child.set(child);
        } else {
            self.data(previous).next_sibling.set(child);
        }
        if before.is_invalid() {
            parent_data.last_child.set(child);
        } else {
            self.data(before).previous_sibling.set(child);
        }

        self.assign_pre_order_labels_to_inserted_subtree(parent, child);
        self.note_layout_subtree_attached(child);
        self.note_structural_change_at_and_above(parent);
    }

    pub(crate) fn remove_child(&self, parent: NodeSlotId, child: NodeSlotId) {
        self.unlink_child(parent, child);
        self.note_structural_change_at_and_above(parent);
    }

    fn unlink_child(&self, parent: NodeSlotId, child: NodeSlotId) {
        self.assert_owner_thread();
        let parent_data = self.data(parent);
        let child_data = self.data(child);

        let child_parent = child_data.parent.get();
        assert_eq!(child_parent, parent, "removed layout node is not a child of the parent");
        let previous = child_data.previous_sibling.get();
        let next = child_data.next_sibling.get();

        if previous.is_invalid() {
            let first_child = parent_data.first_child.get();
            assert_eq!(first_child, child, "layout node child list lost its first child");
            parent_data.first_child.set(next);
        } else {
            let previous_data = self.data(previous);
            let previous_next_sibling = previous_data.next_sibling.get();
            assert_eq!(
                previous_next_sibling, child,
                "layout node sibling chain is inconsistent"
            );
            previous_data.next_sibling.set(next);
        }

        if next.is_invalid() {
            let last_child = parent_data.last_child.get();
            assert_eq!(last_child, child, "layout node child list lost its last child");
            parent_data.last_child.set(previous);
        } else {
            let next_data = self.data(next);
            let next_previous_sibling = next_data.previous_sibling.get();
            assert_eq!(
                next_previous_sibling, child,
                "layout node sibling chain is inconsistent"
            );
            next_data.previous_sibling.set(previous);
        }

        child_data.parent.set(NodeSlotId::INVALID);
        child_data.previous_sibling.set(NodeSlotId::INVALID);
        child_data.next_sibling.set(NodeSlotId::INVALID);
    }

    pub(crate) fn paint_state(&self) -> &RefCell<crate::painting::paint_state::PaintState> {
        &self.paint_state
    }

    pub(crate) fn node_flags_if_live(&self, id: NodeSlotId) -> u32 {
        if !self.slot_is_live(id) {
            return 0;
        }
        self.data(id).flags.get()
    }

    pub(crate) fn node_is_generated_for_pseudo_element(&self, id: NodeSlotId) -> bool {
        if !self.slot_is_live(id) {
            return false;
        }
        self.data(id).generated_for.get() != 0
    }

    pub(crate) fn node_kind_if_live(&self, id: NodeSlotId) -> Option<NodeKind> {
        if !self.slot_is_live(id) {
            return None;
        }
        Some(self.data(id).kind.get())
    }

    pub(crate) fn node_parent_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        if !self.slot_is_live(id) {
            return None;
        }
        let parent = self.data(id).parent.get();
        (!parent.is_invalid()).then_some(parent)
    }

    pub(crate) fn node_first_child_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        if !self.slot_is_live(id) {
            return None;
        }
        let child = self.data(id).first_child.get();
        (!child.is_invalid()).then_some(child)
    }

    pub(crate) fn node_next_sibling_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        if !self.slot_is_live(id) {
            return None;
        }
        let sibling = self.data(id).next_sibling.get();
        (!sibling.is_invalid()).then_some(sibling)
    }

    pub(crate) fn node_data_if_live(&self, id: NodeSlotId) -> Option<&NodeData> {
        if !self.slot_is_live(id) {
            return None;
        }
        Some(self.data(id))
    }

    pub(crate) fn node_is_out_of_flow_if_live(&self, id: NodeSlotId) -> bool {
        self.node_data_if_live(id)
            .is_some_and(|data| super::node_facts::node_is_out_of_flow(data, self.node_style_if_live(id)))
    }

    pub(crate) fn node_containing_block_if_live(&self, id: NodeSlotId) -> Option<NodeSlotId> {
        if !self.slot_is_live(id) {
            return None;
        }
        let block = self.data(id).containing_block.get();
        (!block.is_invalid()).then_some(block)
    }

    pub(crate) fn node_style_if_live(
        &self,
        id: NodeSlotId,
    ) -> Option<crate::css::computed_value_views::ComputedValuesView<'_>> {
        if !self.slot_is_live(id) {
            return None;
        }
        let payloads = self.style_payloads(id)?;
        Some(crate::css::computed_value_views::ComputedValuesView::new(
            &payloads.groups,
        ))
    }

    pub(crate) fn slot_is_live(&self, id: NodeSlotId) -> bool {
        if id.is_invalid() {
            return false;
        }
        self.slot_metadata
            .get(id.slot_index() as usize)
            .is_some_and(|metadata| metadata.occupied && metadata.generation == id.generation())
    }

    pub(crate) fn visit_dom_nodes(&self, mut visit: impl FnMut(*mut c_void)) {
        for (metadata, dom_node) in self.slot_metadata.iter().zip(&self.dom_nodes) {
            let dom_node = dom_node.get();
            if metadata.occupied && !dom_node.is_null() {
                visit(dom_node);
            }
        }
    }

    pub(crate) fn node_dom_node(&self, id: NodeSlotId) -> *mut c_void {
        if !self.slot_is_live(id) {
            return std::ptr::null_mut();
        }
        self.dom_nodes[id.slot_index() as usize].get()
    }

    pub(crate) fn node_dom_node_is_element(&self, id: NodeSlotId) -> bool {
        if self.node_dom_node(id).is_null() {
            return false;
        }
        let kind = self.data(id).kind.get();
        kind != NodeKind::Viewport && !crate::layout::node_facts::kind_is_text(kind)
    }

    pub(crate) fn previous_dom_backed_or_generated_node(
        &self,
        start: NodeSlotId,
        previous_sibling_only: bool,
    ) -> NodeSlotId {
        let mut current = start;
        loop {
            let data = self.data(current);
            current = if previous_sibling_only {
                data.previous_sibling.get()
            } else if data.previous_sibling.get().is_invalid() {
                data.parent.get()
            } else {
                let mut deepest_last_descendant = data.previous_sibling.get();
                loop {
                    let last_child = self.data(deepest_last_descendant).last_child.get();
                    if last_child.is_invalid() {
                        break;
                    }
                    deepest_last_descendant = last_child;
                }
                deepest_last_descendant
            };
            if current.is_invalid() {
                return NodeSlotId::INVALID;
            }
            let has_dom_node = !self.dom_nodes[current.slot_index() as usize].get().is_null();
            if has_dom_node || self.data(current).generated_for.get() != 0 {
                return current;
            }
        }
    }

    pub(crate) fn enroll_text_children_for_content_sync(&self, parent: NodeSlotId) {
        let mut child = self.data(parent).first_child.get();
        while !child.is_invalid() {
            let data = self.data(child);
            if crate::layout::node_facts::kind_is_text(data.kind.get()) {
                self.enroll_text_node_for_content_sync(child);
            }
            child = data.next_sibling.get();
        }
    }

    pub(crate) fn live_slot_count(&self) -> u32 {
        self.live_count
    }

    pub(crate) fn shell_if_live(&self, id: NodeSlotId) -> *mut c_void {
        if !self.slot_is_live(id) {
            return std::ptr::null_mut();
        }
        self.node_shell(id)
    }

    pub(crate) fn node_link_slot(&self, id: NodeSlotId, link: FfiNodeLink) -> NodeSlotId {
        let data = self.data(id);
        match link {
            FfiNodeLink::Parent => data.parent.get(),
            FfiNodeLink::FirstChild => data.first_child.get(),
            FfiNodeLink::LastChild => data.last_child.get(),
            FfiNodeLink::PreviousSibling => data.previous_sibling.get(),
            FfiNodeLink::NextSibling => data.next_sibling.get(),
        }
    }

    pub(crate) fn node_link_shell(&self, id: NodeSlotId, link: FfiNodeLink) -> *mut c_void {
        let linked = self.node_link_slot(id, link);
        if linked.is_invalid() {
            return std::ptr::null_mut();
        }
        self.node_shell(linked)
    }

    pub(crate) fn node_containing_block_shell_if_live(&self, id: NodeSlotId) -> *mut c_void {
        let containing_block = self.data(id).containing_block.get();
        self.shell_if_live(containing_block)
    }

    pub(crate) fn node_flags(&self, id: NodeSlotId) -> u32 {
        self.data(id).flags.get()
    }

    pub(crate) fn node_generated_for(&self, id: NodeSlotId) -> u8 {
        self.data(id).generated_for.get()
    }

    pub(crate) fn node_shell(&self, id: NodeSlotId) -> *mut c_void {
        let shell = self.data(id).shell.get();
        if !shell.is_null() {
            return shell;
        }
        self.materialize_shell(id)
    }

    pub(crate) fn dom_offset_for_rendered_text_offset(
        &self,
        id: NodeSlotId,
        offset: usize,
        boundary: RenderedTextBoundary,
    ) -> usize {
        if !self.node_kind_if_live(id).is_some_and(super::node_facts::kind_is_text) {
            return offset;
        }
        self.text_content(id)
            .expect("text must be published before mapping rendered offsets")
            .dom_offset_for_rendered_text_offset(offset, boundary)
    }

    pub(crate) fn rendered_text_offset_for_dom_offset(
        &self,
        id: NodeSlotId,
        offset: usize,
        boundary: RenderedTextBoundary,
    ) -> usize {
        if !self.node_kind_if_live(id).is_some_and(super::node_facts::kind_is_text) {
            return offset;
        }
        self.text_content(id)
            .expect("text must be published before mapping DOM offsets")
            .rendered_text_offset_for_dom_offset(offset, boundary)
    }

    pub(crate) unsafe fn from_handle<'a>(arena: *mut c_void) -> &'a Self {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: Layout passes borrow the document's arena synchronously,
        // and the document keeps it alive for the duration of the pass.
        unsafe { &*arena.cast::<Self>() }
    }

    pub(crate) unsafe fn from_handle_mut<'a>(arena: *mut c_void) -> &'a mut Self {
        assert!(!arena.is_null(), "layout node arena handle is null");
        // SAFETY: The caller guarantees exclusive access to the arena for the
        // duration of the returned borrow.
        unsafe { &mut *arena.cast::<Self>() }
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

#[cfg(test)]
#[derive(Clone, Copy)]
pub(crate) struct NodeAllocation {
    pub(crate) slot: NodeSlotId,
}

#[unsafe(no_mangle)]
pub extern "C" fn layout_arena_create() -> *mut c_void {
    Box::into_raw(Box::new(LayoutNodeArena::new())).cast()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_destroy(arena: *mut c_void) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The handle came from layout_arena_create and ownership is
    // transferred back exactly once by the C++ RAII wrapper.
    let arena = unsafe { Box::from_raw(arena.cast::<LayoutNodeArena>()) };
    arena.assert_owner_thread();
    assert_eq!(arena.live_count, 0, "layout node arena destroyed with live slots");
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_allocate(
    arena: *mut c_void,
    construction_facts: FfiNodeConstructionFacts,
) -> NodeSlotId {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    unsafe { &mut *arena.cast::<LayoutNodeArena>() }.allocate(construction_facts)
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `root` must name a live node
/// in this arena that has no parent. Every C++-side detach preparation that walks the subtree
/// must already have run. Every shell in the subtree is destroyed before this returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_free_subtree(arena: *mut c_void, root: NodeSlotId) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and serializes all access on
    // the document thread.
    crate::layout::tree_mutation::free_subtree_and_destroy_shells(arena.cast::<LayoutNodeArena>(), root);
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `node` must name a live node
/// in this arena. Every C++-side detach preparation that walks the subtree must already have
/// run. The node and every shell in its subtree are destroyed before this returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_detach_and_free_subtree(arena: *mut c_void, node: NodeSlotId) -> bool {
    assert!(!arena.is_null(), "layout node arena handle is null");
    let arena = arena.cast::<LayoutNodeArena>();
    // SAFETY: The C++ wrapper keeps the arena alive for this call and serializes all access on
    // the document thread; the shared borrow ends before the subtree is freed.
    let was_attached = unsafe { &*arena }.detach_from_parent(node);
    crate::layout::tree_mutation::free_subtree_and_destroy_shells(arena, node);
    was_attached
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and the visitor must not
/// re-enter the arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_visit_dom_nodes(
    arena: *mut c_void,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, *mut c_void),
) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.visit_dom_nodes(|dom_node| {
        // SAFETY: Guaranteed by the entry point's contract.
        unsafe { visit(context, dom_node) }
    });
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `start` must name a live node
/// in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_previous_dom_backed_or_generated_node(
    arena: *mut c_void,
    start: NodeSlotId,
    previous_sibling_only: bool,
) -> NodeSlotId {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.previous_dom_backed_or_generated_node(start, previous_sibling_only)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_dom_node(arena: *mut c_void, node: NodeSlotId) -> *mut c_void {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.node_dom_node(node)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_live_slot_count(arena: *mut c_void) -> u32 {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.live_slot_count()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_fc_run_cache_hit_count(arena: *mut c_void) -> u64 {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    unsafe { &*arena.cast::<LayoutNodeArena>() }
        .fc_run_cache_store()
        .hit_count()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_table_cell_measurement_cache_miss_count(arena: *mut c_void) -> u64 {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.table_cell_measurement_cache_miss_count()
}

/// # Safety
///
/// The arena must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_intrinsic_measurement_count(arena: *mut c_void) -> u64 {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.intrinsic_measurement_count()
}

/// # Safety
///
/// The arena must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_pre_order_label_violation_count(arena: *mut c_void, root: NodeSlotId) -> u64 {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    let arena = unsafe { &*arena.cast::<LayoutNodeArena>() };
    if arena.shell_if_live(root).is_null() {
        return 0;
    }
    let mut violation_count = 0u64;
    let mut previous_label: Option<u64> = None;
    arena.for_each_node_in_layout_subtree_in_pre_order(root, |node| {
        let label = arena.node_pre_order_label(node);
        if previous_label.is_some_and(|previous| label <= previous) {
            violation_count += 1;
        }
        previous_label = Some(label);
    });
    violation_count
}

/// # Safety
///
/// The arena must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_pre_order_relabel_count(arena: *mut c_void) -> u64 {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.pre_order_relabel_count()
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `root` must
/// name the root of a live subtree in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_reset_layout_update_flags_in_subtree(arena: *mut c_void, root: NodeSlotId) {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.reset_layout_update_flags_in_subtree(root);
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
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.recompute_containing_blocks_in_subtree(root, inline_cb_lookup);
}

/// # Safety
///
/// The arena must remain valid for the duration of the call. `id` may be
/// invalid or stale; null is returned in that case.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_shell_if_live(arena: *mut c_void, id: NodeSlotId) -> *mut c_void {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.shell_if_live(id)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_link_shell(
    arena: *mut c_void,
    id: NodeSlotId,
    link: FfiNodeLink,
) -> *mut c_void {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.node_link_shell(id, link)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_link_slot(
    arena: *mut c_void,
    id: NodeSlotId,
    link: FfiNodeLink,
) -> NodeSlotId {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.node_link_slot(id, link)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_containing_block_shell_if_live(
    arena: *mut c_void,
    id: NodeSlotId,
) -> *mut c_void {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.node_containing_block_shell_if_live(id)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_flags(arena: *mut c_void, id: NodeSlotId) -> u32 {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.node_flags(id)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_has_compositor_animation_frame(
    arena: *mut c_void,
    id: NodeSlotId,
    kind: super::node_data::CompositorAnimationFrameKind,
) -> bool {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.node_has_compositor_animation_frame(id, kind)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_generated_for(arena: *mut c_void, id: NodeSlotId) -> u8 {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.node_generated_for(id)
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `node` must name a live node
/// in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_bump_fragment_cache_epoch_of_self_and_ancestors(
    arena: *mut c_void,
    node: NodeSlotId,
) {
    // SAFETY: The C++ caller keeps the arena alive for this synchronous call.
    unsafe { LayoutNodeArena::from_handle(arena) }.bump_fragment_cache_epoch_of_self_and_ancestors(node);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_replaced_content_facts(
    arena: *mut c_void,
    id: NodeSlotId,
    facts: FfiReplacedContentFacts,
) -> bool {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    unsafe { &mut *arena.cast::<LayoutNodeArena>() }.set_replaced_content_facts(id, facts)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_node_flag(arena: *mut c_void, id: NodeSlotId, flag: NodeFlag, value: bool) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.set_node_flag(id, flag, value);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_node_needs_compositor_animation_frame(
    arena: *mut c_void,
    id: NodeSlotId,
    kind: super::node_data::CompositorAnimationFrameKind,
    value: bool,
) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and serializes all access on the document thread.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.set_node_needs_compositor_animation_frame(id, kind, value);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_node_generated_for(arena: *mut c_void, id: NodeSlotId, generated_for: u8) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.set_node_generated_for(id, generated_for);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_node_style(
    arena: *mut c_void,
    id: NodeSlotId,
    style_record: u64,
    payloads: *const c_void,
) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.set_node_style(id, style_record, payloads);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_style_record(arena: *mut c_void, id: NodeSlotId) -> u64 {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.node_style_record(id)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_style_record_is_pinned_by_arena(arena: *mut c_void, id: NodeSlotId) -> bool {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.node_style_record_is_pinned_by_arena(id)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_replace_arena_pinned_style_record(
    arena: *mut c_void,
    id: NodeSlotId,
    style_record: u64,
    payloads: *const c_void,
) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.replace_arena_pinned_style_record(
        id,
        FfiDerivedStyleRecord {
            record: style_record,
            payloads,
        },
    );
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_reinherit_anonymous_descendants(arena: *mut c_void, node: NodeSlotId) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.reinherit_anonymous_descendants(node);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_node_style_payloads(arena: *mut c_void, id: NodeSlotId) -> *const c_void {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.data(id).style.get()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_shell_count(arena: *mut c_void) -> u32 {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.shell_count()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_shell_factory(
    arena: *mut c_void,
    context: *mut c_void,
    factory: unsafe extern "C" fn(*mut c_void, NodeSlotId, NodeKind),
) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.set_shell_factory(Some((context, factory)));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_clear_shell_factory(arena: *mut c_void) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.set_shell_factory(None);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_attach_shell(arena: *mut c_void, id: NodeSlotId, shell: *mut c_void) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.attach_shell(id, shell);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_style_record_host_callbacks(
    arena: *mut c_void,
    callbacks: FfiStyleRecordHostCallbacks,
) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.set_style_record_host(Some(callbacks));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_clear_style_record_host_callbacks(arena: *mut c_void) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.set_style_record_host(None);
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `parent` must name a live node
/// in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_enroll_text_children_for_content_sync(arena: *mut c_void, parent: NodeSlotId) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.enroll_text_children_for_content_sync(parent);
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `node` must name a live node
/// in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_enroll_node_for_replaced_content_facts_sync_if_eligible(
    arena: *mut c_void,
    node: NodeSlotId,
) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: As above.
    unsafe { &*arena.cast::<LayoutNodeArena>() }.enroll_node_for_replaced_content_facts_sync_if_eligible(node);
}

/// # Safety
///
/// The arena must remain valid for the duration of the call. The callbacks receive live layout
/// node shells; the text callback may publish text and re-enter the enroll entry points.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_sync_enrolled_content_for_layout(
    arena: *mut c_void,
    context: *mut c_void,
    sync_text_content: unsafe extern "C" fn(*mut c_void, *mut c_void),
    build_replaced_content_facts: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut FfiReplacedContentFacts),
) {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY (for every derive below): the C++ wrapper keeps the arena alive for this call
    // and serializes all access on the document thread; no shared borrow outlives a callback.
    let enrolled_text_nodes = unsafe { &*arena.cast::<LayoutNodeArena>() }.take_text_nodes_for_content_sync();
    // A node that is alive but detached keeps its enrollment: it cannot
    // resolve style-dependent text without a parent, and it may be reinserted
    // by a later tree update without another enrollment trigger.
    let mut still_detached_text_nodes = Vec::new();
    for node in enrolled_text_nodes {
        let shell = unsafe { &*arena.cast::<LayoutNodeArena>() }.shell_if_live(node);
        if shell.is_null() {
            continue;
        }
        // SAFETY: shell_if_live established a live slot of this generation.
        let parent = unsafe { &*arena.cast::<LayoutNodeArena>() }.data(node).parent.get();
        if parent.is_invalid() {
            still_detached_text_nodes.push(node);
            continue;
        }
        // SAFETY: The callback receives a live shell. Publishing its text also
        // invalidates affected layout caches before any pass can read it.
        unsafe { sync_text_content(context, shell) };
    }
    unsafe { &*arena.cast::<LayoutNodeArena>() }
        .text_nodes_enrolled_for_content_sync
        .borrow_mut()
        .extend(still_detached_text_nodes);

    let enrolled_replaced_nodes = unsafe { &*arena.cast::<LayoutNodeArena>() }
        .nodes_enrolled_for_replaced_content_facts_sync
        .borrow()
        .clone();
    let mut live_replaced_nodes = Vec::with_capacity(enrolled_replaced_nodes.len());
    for node in enrolled_replaced_nodes {
        let shell = unsafe { &*arena.cast::<LayoutNodeArena>() }.shell_if_live(node);
        if shell.is_null() {
            continue;
        }
        live_replaced_nodes.push(node);
        let mut facts = FfiReplacedContentFacts::default();
        // SAFETY: The callback receives a live shell and a valid out-pointer.
        unsafe { build_replaced_content_facts(context, shell, &raw mut facts) };
        // Changed facts invalidate cached formatting-context runs regardless of which
        // channel produced the change, including sources with no invalidation of their own.
        // SAFETY: As above; the shared borrows ended with their statements.
        if unsafe { &mut *arena.cast::<LayoutNodeArena>() }.set_replaced_content_facts(node, facts) {
            unsafe { &*arena.cast::<LayoutNodeArena>() }.bump_fragment_cache_epoch_of_self_and_ancestors(node);
        }
    }
    *unsafe { &*arena.cast::<LayoutNodeArena>() }
        .nodes_enrolled_for_replaced_content_facts_sync
        .borrow_mut() = live_replaced_nodes;
}

/// # Safety
///
/// The arena must remain valid for the duration of the call, and `id` must name a live node
/// in this arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_set_table_spans(
    arena: *mut c_void,
    id: NodeSlotId,
    column_span: u16,
    row_span: u16,
    raw_column_span: u32,
) -> bool {
    assert!(!arena.is_null(), "layout node arena handle is null");
    // SAFETY: The C++ wrapper keeps the arena alive for this call and
    // serializes all access on the document thread.
    let arena = unsafe { &mut *arena.cast::<LayoutNodeArena>() };
    let data = arena.data(id);
    let effective_spans_changed = data.table_column_span.get() != column_span || data.table_row_span.get() != row_span;
    data.table_column_span.set(column_span);
    data.table_row_span.set(row_span);
    let previous_raw_column_span = arena.set_raw_table_column_span(id, raw_column_span);
    effective_spans_changed || previous_raw_column_span != raw_column_span
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;

    use crate::layout::layout_node_arena::{
        Chunk, FfiDerivedStyleRecord, IntrinsicBlockSizeMeasurement, IntrinsicInlineSizeMeasurement,
        IntrinsicSizeCacheKey, IntrinsicSizeCacheKind, LayoutNodeArena, SLOTS_PER_CHUNK, TableCellMeasurement,
        TableCellMeasurementKey,
    };
    use crate::layout::node_data::{FfiNodeConstructionFacts, NodeFlag, NodeKind, NodeSlotId};
    use crate::layout::{CssPixels, fragment_tree, used_values};
    use std::ffi::c_void;

    #[test]
    fn text_chunk_users_survive_cache_replacement() {
        use super::TextChunkCacheKey;
        use crate::layout::text_chunker::TextChunk;
        use std::rc::Rc;

        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        let key = TextChunkCacheKey {
            should_wrap_lines: true,
            should_respect_linebreaks: false,
            unidirectional_ltr: true,
            white_space_collapse: 0,
            word_break: 0,
            font_variant_emoji: 0,
            // The standalone test binary stubs the C++ retain/release callbacks.
            font_cascade_list: std::ptr::dangling(),
        };
        let chunk = TextChunk {
            start: 0,
            length: 5,
            font: std::ptr::dangling(),
            has_breaking_newline: false,
            has_breaking_tab: false,
            is_all_whitespace: false,
            can_break_after: true,
            text_type: 0,
        };
        let original = arena.text_chunks(node, key, || vec![chunk]);
        let hit = arena.text_chunks(node, key, || panic!("matching chunks should be cached"));
        assert!(Rc::ptr_eq(&original, &hit));
        drop(hit);
        let original_weak = Rc::downgrade(&original);
        let replacement = arena.text_chunks(
            node,
            TextChunkCacheKey {
                should_wrap_lines: false,
                ..key
            },
            Vec::new,
        );
        assert!(replacement.is_empty());
        assert_eq!(&**original, &[chunk]);
        assert_eq!(Rc::strong_count(&original), 1);
        drop(original);
        assert!(original_weak.upgrade().is_none());
    }

    fn test_construction_facts(dom_node: *mut c_void) -> FfiNodeConstructionFacts {
        test_construction_facts_with_kind(dom_node, NodeKind::Box)
    }

    fn test_construction_facts_with_kind(dom_node: *mut c_void, kind: NodeKind) -> FfiNodeConstructionFacts {
        FfiNodeConstructionFacts {
            kind,
            shell: std::ptr::null_mut(),
            dom_node,
            is_anonymous: dom_node.is_null(),
            is_html_input_element: false,
            is_html_html_element: false,
            is_document_element: false,
            is_in_user_agent_shadow_tree: false,
            uses_button_layout: false,
            is_editing_host: false,
            is_body: false,
        }
    }

    #[test]
    fn an_unbound_slot_has_no_shell_until_a_shell_is_bound() {
        let mut arena = LayoutNodeArena::new();
        let mut dom_node_storage = 0u8;
        let dom_node = std::ptr::from_mut(&mut dom_node_storage).cast::<c_void>();
        let slot = arena.allocate_unbound(dom_node);
        assert!(arena.slot_is_live(slot));
        assert!(arena.node_shell(slot).is_null());
        assert_eq!(arena.data(slot).kind.get(), NodeKind::Unset);
        assert_eq!(arena.node_dom_node(slot), dom_node);

        let unbound_freed = arena.free_subtree(slot);
        assert_eq!(unbound_freed.shell_count(), 1);
        unbound_freed.destroy_shells_and_invoke_callbacks();
        assert!(!arena.slot_is_live(slot));

        let slot = arena.allocate_unbound(dom_node);
        arena.bind_shell(slot, test_construction_facts(dom_node));
        assert_eq!(arena.data(slot).kind.get(), NodeKind::Box);
        assert!(arena.data(slot).flags.get() & NodeFlag::HasStyle as u32 != 0);
        arena.free_subtree(slot).destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn an_anonymous_box_stamped_by_the_arena_keeps_its_style_record_until_freed() {
        let mut arena = LayoutNodeArena::new();
        let payloads = [std::ptr::null::<c_void>(); 1];
        let slot = arena.allocate_unbound(std::ptr::null_mut());
        arena.stamp_anonymous_box(
            slot,
            NodeKind::InlineNode,
            FfiDerivedStyleRecord {
                record: 7,
                payloads: payloads.as_ptr().cast(),
            },
        );
        assert_eq!(arena.data(slot).kind.get(), NodeKind::InlineNode);
        assert!(arena.data(slot).flags.get() & NodeFlag::Anonymous as u32 != 0);
        assert!(arena.data(slot).flags.get() & NodeFlag::HasStyle as u32 != 0);
        assert_eq!(arena.node_style_record(slot), 7);
        assert!(arena.node_style_record_is_pinned_by_arena(slot));

        let element = arena.allocate(test_construction_facts_with_kind(
            std::ptr::null_mut(),
            NodeKind::InlineNode,
        ));
        arena.set_node_style(element, 9, payloads.as_ptr().cast());
        assert_eq!(arena.node_style_record(element), 9);
        assert!(!arena.node_style_record_is_pinned_by_arena(element));

        let freed = arena.free_subtree(slot);
        assert_eq!(freed.arena_pinned_style_record_count(), 1);
        freed.destroy_shells_and_invoke_callbacks();
        let freed = arena.free_subtree(element);
        assert_eq!(freed.arena_pinned_style_record_count(), 0);
        freed.destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn previous_dom_backed_or_generated_node_skips_anonymous_slots() {
        let mut arena = LayoutNodeArena::new();
        let mut root_dom_node_storage = 0u8;
        let mut element_dom_node_storage = 0u8;
        let root_dom_node = std::ptr::from_mut(&mut root_dom_node_storage).cast::<c_void>();
        let element_dom_node = std::ptr::from_mut(&mut element_dom_node_storage).cast::<c_void>();
        let root = arena.allocate(test_construction_facts(root_dom_node));
        let anonymous_wrapper = arena.allocate(test_construction_facts(std::ptr::null_mut()));
        let nested_anonymous = arena.allocate(test_construction_facts(std::ptr::null_mut()));
        let element = arena.allocate(test_construction_facts(element_dom_node));
        arena.insert_child(root, anonymous_wrapper, NodeSlotId::INVALID);
        arena.insert_child(anonymous_wrapper, nested_anonymous, NodeSlotId::INVALID);
        arena.insert_child(root, element, NodeSlotId::INVALID);

        assert_eq!(arena.previous_dom_backed_or_generated_node(element, false), root);
        assert!(arena.previous_dom_backed_or_generated_node(element, true).is_invalid());
        assert!(arena.previous_dom_backed_or_generated_node(root, false).is_invalid());

        arena.data(nested_anonymous).generated_for.set(1);
        assert_eq!(
            arena.previous_dom_backed_or_generated_node(element, false),
            nested_anonymous
        );

        arena.free_subtree(root).destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn dom_nodes_are_reported_only_while_their_slot_is_live() {
        let mut arena = LayoutNodeArena::new();
        let mut dom_node_storage = 0u8;
        let dom_node = std::ptr::from_mut(&mut dom_node_storage).cast::<c_void>();
        let anonymous = arena.allocate(test_construction_facts(std::ptr::null_mut()));
        let element = arena.allocate(test_construction_facts(dom_node));
        assert!(arena.node_dom_node(anonymous).is_null());
        assert_eq!(arena.node_dom_node(element), dom_node);
        let mut visited = Vec::new();
        arena.visit_dom_nodes(|node| visited.push(node));
        assert_eq!(visited, vec![dom_node]);
        assert_eq!(arena.live_slot_count(), 2);

        arena.free_subtree(element).destroy_shells_and_invoke_callbacks();
        assert!(arena.node_dom_node(element).is_null());
        let reoccupant = arena.allocate_for_test();
        assert_eq!(reoccupant.slot.slot_index(), element.slot_index());
        assert!(arena.node_dom_node(element).is_null());
        assert!(arena.node_dom_node(reoccupant.slot).is_null());
        visited.clear();
        arena.visit_dom_nodes(|node| visited.push(node));
        assert!(visited.is_empty());

        arena.free_subtree(anonymous).destroy_shells_and_invoke_callbacks();
        arena
            .free_subtree(reoccupant.slot)
            .destroy_shells_and_invoke_callbacks();
        assert_eq!(arena.live_slot_count(), 0);
    }

    fn test_fragment_link(node: NodeSlotId) -> fragment_tree::FragmentLink {
        fragment_tree::FragmentLink {
            fragment: std::rc::Rc::new(fragment_tree::Fragment {
                identity: 1,
                node,
                content_inline_size: CssPixels::default(),
                content_block_size: CssPixels::default(),
                margin_left: CssPixels::default(),
                margin_right: CssPixels::default(),
                margin_top: CssPixels::default(),
                margin_bottom: CssPixels::default(),
                border_left: CssPixels::default(),
                border_right: CssPixels::default(),
                border_top: CssPixels::default(),
                border_bottom: CssPixels::default(),
                padding_left: CssPixels::default(),
                padding_right: CssPixels::default(),
                padding_top: CssPixels::default(),
                padding_bottom: CssPixels::default(),
                uses_collapsing_borders_model: false,
                collapsed_table_borders: None,
                line_data: None,
                grid_layout_data: None,
                flex_layout_data: None,
                used_grid_tracks: None,
                svg_viewport_transform: None,
                svg_viewport_size: None,
                svg_view_box: None,
                svg_viewport_percentage_basis: CssPixels::default(),
                computed_svg_path: None,
                has_line_clamp_point: false,
                is_invisible_for_line_clamp: false,
                children: Vec::new(),
            }),
            committed_offset: Default::default(),
            inset_left: CssPixels::default(),
            inset_right: CssPixels::default(),
            inset_top: CssPixels::default(),
            inset_bottom: CssPixels::default(),
            containing_line_box_index: None,
            abspos_layout_inputs: None,
        }
    }

    #[test]
    fn node_data_addresses_remain_stable_when_chunks_are_added() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate_for_test();
        let first_data_address = std::ptr::from_ref(arena.data(first.slot)) as usize;

        let mut allocations = Vec::new();
        for _ in 0..SLOTS_PER_CHUNK * 2 {
            allocations.push(arena.allocate_for_test());
        }

        assert_eq!(first_data_address, std::ptr::from_ref(arena.data(first.slot)) as usize);
        arena.data(first.slot).table_column_span.set(42);
        assert_eq!(arena.data(first.slot).table_column_span.get(), 42);
        arena.free_subtree(first.slot).destroy_shells_and_invoke_callbacks();
        for allocation in allocations {
            arena
                .free_subtree(allocation.slot)
                .destroy_shells_and_invoke_callbacks();
        }
    }

    #[test]
    fn node_data_slots_are_cache_line_aligned() {
        assert_eq!(align_of::<Chunk>() % 64, 0);
        let mut arena = LayoutNodeArena::new();
        let allocation = arena.allocate_for_test();
        assert_eq!(std::ptr::from_ref(arena.data(allocation.slot)) as usize % 64, 0);
        arena
            .free_subtree(allocation.slot)
            .destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn freed_slots_are_reused_with_a_new_generation() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate_for_test();
        arena.free_subtree(first.slot).destroy_shells_and_invoke_callbacks();

        let second = arena.allocate_for_test();
        assert_eq!(second.slot.slot_index(), first.slot.slot_index());
        assert_ne!(second.slot, first.slot);
        assert_ne!(second.slot.generation(), first.slot.generation());
        arena.free_subtree(second.slot).destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn default_scroll_shift_anchors_behave_like_weak_references() {
        let mut arena = LayoutNodeArena::new();
        let positioned = arena.allocate_for_test();
        let anchor = arena.allocate_for_test();

        arena.set_default_scroll_shift(positioned.slot, anchor.slot, true, false);
        assert!(arena.may_have_default_scroll_shift_anchor());
        assert_eq!(arena.default_scroll_shift_anchor(positioned.slot), anchor.slot);
        let flags = arena.data(positioned.slot).flags.get();
        assert_ne!(flags & NodeFlag::CompensatesForHorizontalScroll as u32, 0);
        assert_eq!(flags & NodeFlag::CompensatesForVerticalScroll as u32, 0);

        arena.free_subtree(anchor.slot).destroy_shells_and_invoke_callbacks();
        assert!(arena.default_scroll_shift_anchor(positioned.slot).is_invalid());

        let anchor_slot_reoccupant = arena.allocate_for_test();
        assert_eq!(anchor_slot_reoccupant.slot.slot_index(), anchor.slot.slot_index());
        assert!(arena.default_scroll_shift_anchor(positioned.slot).is_invalid());

        arena.set_default_scroll_shift(positioned.slot, anchor_slot_reoccupant.slot, true, true);
        assert_eq!(
            arena.default_scroll_shift_anchor(positioned.slot),
            anchor_slot_reoccupant.slot
        );
        arena.set_default_scroll_shift(positioned.slot, NodeSlotId::INVALID, false, false);
        assert!(arena.default_scroll_shift_anchor(positioned.slot).is_invalid());
        let cleared_flags = arena.data(positioned.slot).flags.get();
        assert_eq!(cleared_flags & NodeFlag::CompensatesForHorizontalScroll as u32, 0);
        assert_eq!(cleared_flags & NodeFlag::CompensatesForVerticalScroll as u32, 0);

        arena
            .free_subtree(positioned.slot)
            .destroy_shells_and_invoke_callbacks();
        let positioned_slot_reoccupant = arena.allocate_for_test();
        assert_eq!(
            positioned_slot_reoccupant.slot.slot_index(),
            positioned.slot.slot_index()
        );
        arena.set_default_scroll_shift(positioned_slot_reoccupant.slot, anchor_slot_reoccupant.slot, true, true);
        arena
            .free_subtree(positioned_slot_reoccupant.slot)
            .destroy_shells_and_invoke_callbacks();
        let next_reoccupant = arena.allocate_for_test();
        assert!(arena.default_scroll_shift_anchor(next_reoccupant.slot).is_invalid());

        arena
            .free_subtree(next_reoccupant.slot)
            .destroy_shells_and_invoke_callbacks();
        arena
            .free_subtree(anchor_slot_reoccupant.slot)
            .destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn layout_update_flags_are_reset_only_in_the_requested_subtree() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test();
        let child = arena.allocate_for_test();
        let detached = arena.allocate_for_test();
        arena.insert_child(root.slot, child.slot, NodeSlotId::INVALID);
        let update_flags = NodeFlag::NeedsLayoutUpdate as u32 | NodeFlag::NeedsOwnGeometryUpdate as u32;
        arena
            .data(root.slot)
            .flags
            .set(arena.data(root.slot).flags.get() | (update_flags));
        arena
            .data(child.slot)
            .flags
            .set(arena.data(child.slot).flags.get() | (update_flags));
        arena
            .data(detached.slot)
            .flags
            .set(arena.data(detached.slot).flags.get() | (update_flags));

        arena.reset_layout_update_flags_in_subtree(root.slot);

        assert_eq!(arena.data(root.slot).flags.get() & update_flags, 0);
        assert_eq!(arena.data(child.slot).flags.get() & update_flags, 0);
        assert_eq!(arena.data(detached.slot).flags.get() & update_flags, update_flags);
        arena.remove_child(root.slot, child.slot);
        arena.free_subtree(root.slot).destroy_shells_and_invoke_callbacks();
        arena.free_subtree(child.slot).destroy_shells_and_invoke_callbacks();
        arena.free_subtree(detached.slot).destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn stale_slot_ids_do_not_resolve_to_a_new_occupant() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate_for_test();
        arena.free_subtree(first.slot).destroy_shells_and_invoke_callbacks();
        let second = arena.allocate_for_test();

        let stale_read = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| arena.data(first.slot)));
        assert!(stale_read.is_err());
        arena.free_subtree(second.slot).destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn clearing_a_committed_box_evicts_its_fragment_link() {
        let mut arena = LayoutNodeArena::new();
        let allocation = arena.allocate_for_test();
        arena.set_committed_fragment_link(arena.data(allocation.slot), test_fragment_link(allocation.slot));
        assert!(arena.committed_fragment_link(arena.data(allocation.slot)).is_some());

        // SAFETY: arena is a live handle on this thread, and allocation names
        // a live slot in it.
        unsafe {
            crate::painting::ffi::layout_arena_paintable_cleared_from_node(
                std::ptr::from_mut(&mut arena).cast(),
                allocation.slot,
            );
        }

        assert!(arena.committed_fragment_link(arena.data(allocation.slot)).is_none());
        arena
            .free_subtree(allocation.slot)
            .destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn committed_fragment_links_move_between_slots() {
        let mut arena = LayoutNodeArena::new();
        let old = arena.allocate_for_test();
        let new = arena.allocate_for_test();
        let link = test_fragment_link(old.slot);
        let retained_fragment = link.fragment.clone();
        arena.set_committed_fragment_link(arena.data(old.slot), link);

        let moved = arena
            .take_committed_fragment_link(arena.data(old.slot))
            .expect("old slot must retain its committed fragment");
        assert!(std::rc::Rc::ptr_eq(&moved.fragment, &retained_fragment));
        arena.set_committed_fragment_link(arena.data(new.slot), moved);

        assert!(arena.committed_fragment_link(arena.data(old.slot)).is_none());
        let moved = arena
            .committed_fragment_link(arena.data(new.slot))
            .expect("new slot must receive the committed fragment");
        assert!(std::rc::Rc::ptr_eq(&moved.fragment, &retained_fragment));
        arena.free_subtree(old.slot).destroy_shells_and_invoke_callbacks();
        arena.free_subtree(new.slot).destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn intrinsic_size_cache_validates_epoch_and_generation() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate_for_test();
        let key = IntrinsicSizeCacheKey {
            measured_at_inline_size: Some(CssPixels::from_raw(64)),
            ..Default::default()
        };
        let value = IntrinsicBlockSizeMeasurement {
            size: CssPixels::from_raw(128),
            depends_on_percentage_block_size: false,
            depends_on_percentage_inline_basis: false,
        };
        let inline_measurement = IntrinsicInlineSizeMeasurement {
            automatic_content_inline_size: CssPixels::from_raw(192),
            min_content_inline_size_from_max_content_layout: Some(CssPixels::from_raw(96)),
            layout: Some(super::IntrinsicInlineMeasurementLayout {
                available_block_size: crate::layout::layout_node_arena::AvailableSize::MaxContent,
                content_inline_size: CssPixels::from_raw(192),
                content_block_size: CssPixels::from_raw(256),
                automatic_content_block_size: CssPixels::from_raw(320),
                uses_collapsing_borders_model: true,
                has_first_baseline: true,
                first_baseline: CssPixels::from_raw(64),
                has_last_baseline: true,
                last_baseline: CssPixels::from_raw(128),
            }),
            depends_on_percentage_block_size: false,
            depends_on_percentage_inline_basis: false,
        };
        let dependency_computations = Cell::new(0);

        let first_data = arena.data(first.slot);
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

        first_data
            .intrinsic_cache_epoch
            .set(first_data.intrinsic_cache_epoch.get() + 1);
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
        arena.free_subtree(first.slot).destroy_shells_and_invoke_callbacks();

        let second = arena.allocate_for_test();
        assert_eq!(second.slot.slot_index(), first.slot.slot_index());
        assert_ne!(second.slot, first.slot);
        let second_data = &*arena.data(second.slot);
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
        arena.free_subtree(second.slot).destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn intrinsic_size_cache_answers_masked_probes_only_from_independent_measurements() {
        let mut arena = LayoutNodeArena::new();
        let allocation = arena.allocate_for_test();
        let data = arena.data(allocation.slot);
        let key_with_basis = |basis: i32| IntrinsicSizeCacheKey {
            measured_at_inline_size: Some(CssPixels::from_raw(64)),
            percentage_basis_block_size: Some(CssPixels::from_raw(basis)),
            quirks_mode_percentage_basis_block_size: Some(CssPixels::from_raw(basis)),
            ..Default::default()
        };
        let key_without_basis = IntrinsicSizeCacheKey {
            measured_at_inline_size: Some(CssPixels::from_raw(64)),
            ..Default::default()
        };
        let max_content = IntrinsicSizeCacheKind::MaxContentBlock;
        let min_content = IntrinsicSizeCacheKind::MinContentBlock;

        let independent = IntrinsicBlockSizeMeasurement {
            size: CssPixels::from_raw(128),
            depends_on_percentage_block_size: false,
            depends_on_percentage_inline_basis: false,
        };
        arena.intrinsic_block_size_cache_put(data, max_content, key_with_basis(100), independent);
        assert_eq!(
            arena.intrinsic_block_size_cache_get(data, max_content, key_with_basis(100)),
            Some(independent)
        );
        assert_eq!(
            arena.intrinsic_block_size_cache_get(data, max_content, key_with_basis(200)),
            Some(independent)
        );
        assert_eq!(
            arena.intrinsic_block_size_cache_get(data, max_content, key_without_basis),
            Some(independent)
        );

        let dependent = IntrinsicBlockSizeMeasurement {
            size: CssPixels::from_raw(256),
            depends_on_percentage_block_size: true,
            depends_on_percentage_inline_basis: false,
        };
        arena.intrinsic_block_size_cache_put(data, min_content, key_without_basis, dependent);
        assert_eq!(
            arena.intrinsic_block_size_cache_get(data, min_content, key_without_basis),
            Some(dependent)
        );
        assert_eq!(
            arena.intrinsic_block_size_cache_get(data, min_content, key_with_basis(100)),
            None
        );
        arena.intrinsic_block_size_cache_put(data, min_content, key_with_basis(100), dependent);
        assert_eq!(
            arena.intrinsic_block_size_cache_get(data, min_content, key_with_basis(100)),
            Some(dependent)
        );
        assert_eq!(
            arena.intrinsic_block_size_cache_get(data, min_content, key_with_basis(200)),
            None
        );

        let key_at_another_inline_size_with_inline_basis = |basis: i32| IntrinsicSizeCacheKey {
            measured_at_inline_size: Some(CssPixels::from_raw(96)),
            percentage_basis_inline_size: Some(CssPixels::from_raw(basis)),
            ..key_with_basis(100)
        };
        let observes_inline_basis = IntrinsicBlockSizeMeasurement {
            size: CssPixels::from_raw(512),
            depends_on_percentage_block_size: false,
            depends_on_percentage_inline_basis: true,
        };
        arena.intrinsic_block_size_cache_put(
            data,
            max_content,
            key_at_another_inline_size_with_inline_basis(300),
            observes_inline_basis,
        );
        assert_eq!(
            arena.intrinsic_block_size_cache_get(data, max_content, key_at_another_inline_size_with_inline_basis(300)),
            Some(observes_inline_basis)
        );
        assert_eq!(
            arena.intrinsic_block_size_cache_get(
                data,
                max_content,
                IntrinsicSizeCacheKey {
                    percentage_basis_block_size: Some(CssPixels::from_raw(200)),
                    ..key_at_another_inline_size_with_inline_basis(300)
                }
            ),
            Some(observes_inline_basis)
        );
        assert_eq!(
            arena.intrinsic_block_size_cache_get(data, max_content, key_at_another_inline_size_with_inline_basis(400)),
            None
        );
        arena
            .free_subtree(allocation.slot)
            .destroy_shells_and_invoke_callbacks();
    }

    #[test]
    fn table_cell_measurements_follow_the_intrinsic_cache_epoch() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate_for_test();
        let key = TableCellMeasurementKey {
            layout_mode: crate::layout::layout_node_arena::LayoutMode::Normal,
            available_space: crate::layout::layout_node_arena::AvailableSpace {
                inline_size: crate::layout::layout_node_arena::AvailableSize::definite(CssPixels::from_raw(640)),
                block_size: crate::layout::layout_node_arena::AvailableSize::Indefinite,
            },
            content_inline_size: CssPixels::from_raw(640),
            content_block_size: CssPixels::default(),
            has_definite_inline_size: true,
            has_definite_block_size: false,
            inline_size_constraint: used_values::SizeConstraint::None,
            block_size_constraint: used_values::SizeConstraint::None,
            uses_collapsing_borders_model: true,
            adopt_automatic_content_block_size: true,
        };
        let value = TableCellMeasurement {
            automatic_content_block_size: CssPixels::from_raw(320),
            baselines: crate::layout::layout_node_arena::DerivedBaselines {
                first: Some(CssPixels::from_raw(64)),
                last: None,
            },
        };

        let first_data = arena.data(first.slot);
        assert_eq!(arena.table_cell_measurement_cache_get(first_data, key), None);
        arena.table_cell_measurement_cache_put(first_data, key, value);
        assert_eq!(arena.table_cell_measurement_cache_get(first_data, key), Some(value));
        let percentage_resolved_key = TableCellMeasurementKey {
            content_block_size: CssPixels::from_raw(512),
            has_definite_block_size: true,
            adopt_automatic_content_block_size: false,
            ..key
        };
        assert_eq!(
            arena.table_cell_measurement_cache_get(first_data, percentage_resolved_key),
            None
        );

        first_data
            .intrinsic_cache_epoch
            .set(first_data.intrinsic_cache_epoch.get() + 1);
        assert_eq!(arena.table_cell_measurement_cache_get(first_data, key), None);
        arena.free_subtree(first.slot).destroy_shells_and_invoke_callbacks();

        let second = arena.allocate_for_test();
        assert_eq!(second.slot.slot_index(), first.slot.slot_index());
        let second_data = &*arena.data(second.slot);
        assert_eq!(arena.table_cell_measurement_cache_get(second_data, key), None);
        arena.free_subtree(second.slot).destroy_shells_and_invoke_callbacks();
    }
}
