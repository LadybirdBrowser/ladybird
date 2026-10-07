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
use super::layout_changes::{LayoutChange, queue};
use super::rendered_text::{FfiTextSourceRange, PublishedTextSlot, RenderedTextBoundary, TextContent, TextFragments};
use super::shell_reads::read_arena;
use super::tree_builder::FfiLayoutTreeBuildOutcome;
use super::tree_shape::{Chunk, PUBLISHED_ROWS_PER_CHUNK, ShapeWriter, ShapeWrites, TreeShape};
use super::update_layout::FfiLayoutTreeBuildStats;
use super::used_values::SizeConstraint;
use crate::cow_column::{ColumnSnapshot, CowColumn};
use crate::css::css_pixels::{CssPixelPoint, FfiCssPixelPoint};
use crate::css::style::bridge::ElementBoxKind;
use crate::css::style::fast_hash::{FastMap as HashMap, FastSet as HashSet};
use crate::css::style::flight_style_rows::{Decline, FlightStyleRow, REBUILD_LEVEL, RELAYOUT_LEVEL};
use crate::css::style::tree::{StyleNodeID, TableSpans};
use crate::css::style::{
    PublishedBoxFacts, PublishedTextSource, StyleEngine, TextStyleParentFacts,
    engine_sample::NeedsHost,
    layout_style::{AnonymousStyleKind, AnonymousStyleOverrides, DerivedStyleRecord, LayoutStyle},
};
use crate::layout::ComputedValuesView;
use crate::layout::CssPixels;
use crate::layout::FfiReplacedContentFacts;
use crate::layout::node_data::{
    AncestorFact, DomPaintFact, FfiStylePayloads, GENERATED_FOR_AFTER, GENERATED_FOR_LAST_SYNTHETIC,
    MAX_NODE_SLOT_COUNT, NodeData, NodeFlag, NodeKind, NodeSlotId, StylePayloadsRef, pseudo_kind_of,
};
use crate::layout::tree_mutation::HostCalls;
use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::render_state::DocumentHost;
use crate::stage::MainThread;
use std::cell::Cell;
use std::cell::RefCell;
use std::ffi::c_void;
use std::hash::{Hash, Hasher};
use std::ops::{Deref, DerefMut};
use std::sync::Arc;

crate::render_state::held_node_entries!();

mod main_thread_entries;

pub(crate) use main_thread_entries::MainThreadFfiEntry;

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
    pub(crate) depends_on_percentage_block_size: bool,
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
    pub(crate) is_collapsed_borders_table_box: bool,
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

/// What an intrinsic size cache entry was measured for: the row in a slot, and the row's
/// intrinsic cache epoch, which every change that can move the row's intrinsic sizes bumps.
#[derive(Clone, Copy)]
pub(crate) struct IntrinsicSizeCacheStamp {
    index: u32,
    generation: u8,
    epoch: u16,
}

/// The intrinsic sizes layout measured, kept from one pass to the next. Only layout reads or
/// writes them, so they are the layout stage's scratch; the arena only notes the slots whose
/// entries must go, which the stage drops before its next run reads anything.
#[derive(Default)]
pub(crate) struct IntrinsicSizeCaches {
    slots: RefCell<Vec<IntrinsicSizeCacheSlot>>,
}

impl IntrinsicSizeCaches {
    /// Drops the entries of `slots`, which were freed or whose rows' cache epoch wrapped.
    pub(crate) fn drop_slots(&self, slots: Vec<u32>) {
        let mut caches = self.slots.borrow_mut();
        for index in slots {
            if let Some(slot) = caches.get_mut(index as usize) {
                *slot = IntrinsicSizeCacheSlot::default();
            }
        }
    }

    /// Answers `answer` from the maps of the row `stamp` names, where they hold what was measured for it.
    fn with_maps<R>(
        &self,
        stamp: IntrinsicSizeCacheStamp,
        answer: impl FnOnce(&IntrinsicSizeMaps) -> Option<R>,
    ) -> Option<R> {
        let caches = self.slots.borrow();
        let slot = caches.get(stamp.index as usize)?;
        if slot.generation != stamp.generation || slot.epoch != stamp.epoch {
            return None;
        }
        answer(slot.sizes.as_ref()?)
    }

    pub(crate) fn intrinsic_block_size_cache_get(
        &self,
        stamp: IntrinsicSizeCacheStamp,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
    ) -> Option<IntrinsicBlockSizeMeasurement> {
        self.with_maps(stamp, |maps| {
            let map = maps
                .block_sizes(kind)
                .expect("block size cache kind must use the block axis");
            intrinsic_cache_lookup(map, key)
        })
    }

    fn with_maps_mut(&self, stamp: IntrinsicSizeCacheStamp, callback: impl FnOnce(&mut IntrinsicSizeMaps)) {
        let IntrinsicSizeCacheStamp {
            index,
            generation,
            epoch,
        } = stamp;
        let mut caches = self.slots.borrow_mut();
        if caches.len() <= index as usize {
            caches.resize_with(index as usize + 1, IntrinsicSizeCacheSlot::default);
        }
        let slot = &mut caches[index as usize];
        if slot.generation != generation || slot.epoch != epoch {
            *slot = IntrinsicSizeCacheSlot {
                generation,
                epoch,
                sizes: Some(Box::default()),
            };
        }
        callback(slot.sizes.get_or_insert_with(Box::default));
    }

    pub(crate) fn intrinsic_block_size_cache_put(
        &self,
        stamp: IntrinsicSizeCacheStamp,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        value: IntrinsicBlockSizeMeasurement,
    ) {
        self.with_maps_mut(stamp, |maps| {
            let map = maps
                .block_sizes_mut(kind)
                .expect("block size cache kind must use the block axis");
            intrinsic_cache_store(map, key, value);
        });
    }

    pub(crate) fn intrinsic_inline_size_measurement_cache_get(
        &self,
        stamp: IntrinsicSizeCacheStamp,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
    ) -> Option<IntrinsicInlineSizeMeasurement> {
        self.with_maps(stamp, |maps| {
            let map = maps
                .inline_measurements(kind)
                .expect("inline measurement cache kind must use the inline axis");
            intrinsic_cache_lookup(map, key)
        })
    }

    pub(crate) fn intrinsic_inline_size_depends_on_block_size(
        &self,
        stamp: IntrinsicSizeCacheStamp,
        compute: impl FnOnce() -> bool,
    ) -> bool {
        if let Some(value) = self.with_maps(stamp, |maps| maps.inline_size_depends_on_block_size) {
            return value;
        }
        let value = compute();
        self.with_maps_mut(stamp, |maps| {
            maps.inline_size_depends_on_block_size = Some(value);
        });
        value
    }

    pub(crate) fn intrinsic_inline_size_measurement_cache_put(
        &self,
        stamp: IntrinsicSizeCacheStamp,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        value: IntrinsicInlineSizeMeasurement,
    ) {
        self.with_maps_mut(stamp, |maps| {
            let map = maps
                .inline_measurements_mut(kind)
                .expect("inline measurement cache kind must use the inline axis");
            intrinsic_cache_store(map, key, value);
        });
    }

    pub(crate) fn table_cell_measurement_cache_get(
        &self,
        stamp: IntrinsicSizeCacheStamp,
        key: TableCellMeasurementKey,
    ) -> Option<TableCellMeasurement> {
        self.with_maps(stamp, |maps| maps.table_cell_measurements.get(&key).copied())
    }

    pub(crate) fn table_cell_measurement_cache_put(
        &self,
        stamp: IntrinsicSizeCacheStamp,
        key: TableCellMeasurementKey,
        value: TableCellMeasurement,
    ) {
        self.with_maps_mut(stamp, |maps| {
            maps.table_cell_measurements.insert(key, value);
        });
    }
}

#[derive(Clone, Copy, Default)]
struct DefaultScrollShiftAnchorSlot {
    generation: u8,
    anchor: NodeSlotId,
}

#[derive(Clone, Copy)]
struct TextNodeSlot {
    generation: u8,
    /// Where the slot's state is in [`TextSlots::states`], or [`TextNodeSlot::NO_STATE`].
    state: u32,
}

impl TextNodeSlot {
    const NO_STATE: u32 = u32::MAX;
}

impl Default for TextNodeSlot {
    fn default() -> Self {
        Self {
            generation: 0,
            state: Self::NO_STATE,
        }
    }
}

/// Each text row's state, and what it publishes for the paint side. A slot is written only through
/// a [`TextStateMut`], which republishes the slot when it drops, or by [`TextSlots::reset`], so
/// the published column cannot fall behind the slots. The states live side by side rather than
/// each in an allocation of its own, which keeps a copy of the slots to one allocation.
#[derive(Clone, Default)]
struct TextSlots {
    slots: Vec<TextNodeSlot>,
    states: Vec<TextNodeState>,
    /// The places in `states` no slot uses.
    vacant_states: Vec<u32>,
    published: CowColumn<PublishedTextSlot, SLOTS_PER_CHUNK>,
}

impl TextSlots {
    fn state(&self, id: NodeSlotId) -> Option<&TextNodeState> {
        self.slots
            .get(id.slot_index() as usize)
            .filter(|slot| slot.generation == id.generation() && slot.state != TextNodeSlot::NO_STATE)
            .map(|slot| &self.states[slot.state as usize])
    }

    /// The state of `id`'s slot, for writing. A slot last used by another generation starts over.
    fn state_mut(&mut self, id: NodeSlotId) -> TextStateMut<'_> {
        let index = id.slot_index() as usize;
        if self.slots.len() <= index {
            self.slots.resize_with(index + 1, TextNodeSlot::default);
        }
        if self.slots[index].generation != id.generation() {
            self.release_state(index);
            self.slots[index].generation = id.generation();
        }
        if self.slots[index].state == TextNodeSlot::NO_STATE {
            self.slots[index].state = match self.vacant_states.pop() {
                Some(state) => state,
                None => {
                    self.states.push(TextNodeState::default());
                    u32::try_from(self.states.len() - 1).expect("text states fit in u32")
                }
            };
        }
        TextStateMut { slots: self, index }
    }

    /// Empties the slot at `index`, if there is one.
    fn reset(&mut self, index: usize) {
        if index < self.slots.len() {
            self.release_state(index);
            self.slots[index] = TextNodeSlot::default();
            self.republish(index);
        }
    }

    /// Lets go of the state of the slot at `index`, if it has one.
    fn release_state(&mut self, index: usize) {
        let state = std::mem::replace(&mut self.slots[index].state, TextNodeSlot::NO_STATE);
        if state != TextNodeSlot::NO_STATE {
            self.states[state as usize] = TextNodeState::default();
            self.vacant_states.push(state);
        }
    }

    /// Brings what the slot at `index` publishes in step with it.
    fn republish(&mut self, index: usize) {
        let published = self
            .slots
            .get(index)
            .filter(|slot| slot.state != TextNodeSlot::NO_STATE)
            .map(|slot| {
                let state = &self.states[slot.state as usize];
                PublishedTextSlot {
                    generation: slot.generation,
                    first_letter: state.first_letter,
                    rendered: state.content.as_ref().map(|content| content.rendered().clone()),
                }
            })
            .unwrap_or_default();
        if self.published.get(index).is_none() {
            if published.rendered.is_none() && published.first_letter.is_invalid() {
                return;
            }
            self.published.grow_to(index + 1);
        }
        self.published.set(index, published).expect("the column was grown");
    }
}

/// A text slot's state, for writing. Dropping it republishes the slot.
struct TextStateMut<'a> {
    slots: &'a mut TextSlots,
    index: usize,
}

impl Deref for TextStateMut<'_> {
    type Target = TextNodeState;

    fn deref(&self) -> &TextNodeState {
        &self.slots.states[self.slots.slots[self.index].state as usize]
    }
}

impl DerefMut for TextStateMut<'_> {
    fn deref_mut(&mut self) -> &mut TextNodeState {
        &mut self.slots.states[self.slots.slots[self.index].state as usize]
    }
}

impl Drop for TextStateMut<'_> {
    fn drop(&mut self) {
        self.slots.republish(self.index);
    }
}

#[derive(Clone, Default)]
struct TextNodeState {
    source_range: Option<FfiTextSourceRange>,
    first_letter: NodeSlotId,
    content: Option<TextContent>,
    /// What a generated text row renders. Generated content has no DOM text node whose characters
    /// the style mirror could hold, so the row keeps the characters it was made with.
    generated_text: Option<ak::Utf16String>,
}

/// How far a document's rows have been written. See [`LayoutNodeArena::rows_version`].
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub(crate) struct RowsVersion {
    writes: u64,
    /// The part of `writes` that changed what a row is or the row a node is bound to. See
    /// [`LayoutNodeArena::rows_identity_version`].
    identity: u64,
    /// The part of `writes` that gave a row a style record (see [`super::tree_shape::ShapeWrites::style`]).
    style: u64,
    /// The part of `writes` that populated or reset a paintable row.
    population: u64,
    /// Where the paint fact tables, the image maps and the visual context tree are, which moves
    /// when a write copies one a publication shares or replaces the tree.
    tables: [usize; 5],
}

impl RowsVersion {
    /// Whether rows published at this version answer what each row is, and the row each node is bound to, as the
    /// arena does at the identity version `identity`.
    pub(crate) fn has_identity_version(&self, identity: u64) -> bool {
        self.identity == identity
    }

    /// See [`LayoutNodeArena::rows_identity_version`].
    pub(crate) fn identity(&self) -> u64 {
        self.identity
    }

    /// Whether rows published at this version answer what each row is and the style record it has as the arena does at
    /// `version`.
    pub(crate) fn has_styles_of(&self, version: RowsVersion) -> bool {
        self.identity == version.identity && self.style == version.style
    }

    /// Whether rows published at this version answer which rows are populated as the arena does at `version`.
    pub(crate) fn has_population_of(&self, version: RowsVersion) -> bool {
        self.identity == version.identity && self.population == version.population
    }
}

#[derive(Clone, Default)]
struct ReplacedContentFactsSlot {
    generation: u8,
    facts: Option<FfiReplacedContentFacts>,
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

pub(crate) type ShellFactory = (*mut c_void, unsafe extern "C" fn(*mut c_void, NodeSlotId, NodeKind));

/// How the host learns what boxes a DOM node has. The identity is 0 for the document, which has
/// none of its own. The callback must not reenter the arena: it runs while the arena is changing
/// the bindings it would read.
#[derive(Clone, Copy)]
pub(crate) struct BoxPresenceHost(*mut c_void, unsafe extern "C" fn(*mut c_void, u32, u8));

// SAFETY: The arena calls the host only outside a job of its render state, which queues what the host is to hear and
// pays it on the host's thread once it is over (see [`LayoutNodeArena::queue_box_presence`]).
unsafe impl Send for BoxPresenceHost {}

/// What the host is to hear of the boxes the nodes a change changed have: each node's identity, 0 for the document,
/// and what boxes it has, as the change left them.
#[derive(Default)]
pub(crate) struct DueBoxPresence {
    host: Option<BoxPresenceHost>,
    nodes: Vec<(u32, u8)>,
}

impl DueBoxPresence {
    pub(crate) fn is_empty(&self) -> bool {
        self.nodes.is_empty()
    }

    /// Tells the host, if it listens.
    pub(crate) fn tell(self, _: &MainThread) {
        let Some(BoxPresenceHost(context, callback)) = self.host else {
            return;
        };
        for (style_node, bits) in self.nodes {
            // SAFETY: Registration and unregistration keep the host context live, and the host does not reenter the
            // arena.
            unsafe { callback(context, style_node, bits) };
        }
    }
}

/// A row is bound to the node.
pub const BOX_PRESENCE_HAS_LAYOUT_BOX: u8 = 1 << 0;
/// The bound row has a committed box.
pub const BOX_PRESENCE_HAS_COMMITTED_BOX: u8 = 1 << 1;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStyleRecordHostCallbacks {
    pub context: *mut c_void,
    pub shell_style_changed: unsafe extern "C" fn(*mut c_void, *mut c_void, u64, *const c_void, bool, bool),
}

/// How the host learns that a shell's row took a new style: the shell, its new record and
/// payloads, whether the arena derived the record, and whether the shell should attach the style's resources.
pub(crate) type ShellStyleChangedHost = (
    *mut c_void,
    unsafe extern "C" fn(*mut c_void, *mut c_void, u64, *const c_void, bool, bool),
);

fn style_payloads_equal_in_layout_affecting_groups(a: StylePayloadsRef, b: StylePayloadsRef) -> bool {
    style_payloads_equal_in_groups(a, b, crate::css::computed_values::style_group_affects_layout)
}

/// Whether `a` and `b` hold equal payloads in the inherited groups, which are all an anonymous box inherits.
fn style_payloads_equal_in_inherited_groups(a: StylePayloadsRef, b: StylePayloadsRef) -> bool {
    style_payloads_equal_in_groups(a, b, |group_index| {
        group_index < crate::css::style::ENGINE_INHERITED_GROUP_COUNT
    })
}

fn style_payloads_equal_in_groups(a: StylePayloadsRef, b: StylePayloadsRef, compares: impl Fn(usize) -> bool) -> bool {
    if a == b {
        return true;
    }
    if a.is_null() || b.is_null() {
        return false;
    }
    // SAFETY: A row's non-null style addresses the payload array of the record pinned for it.
    let (a, b) = unsafe { (a.deref(), b.deref()) };
    (0..a.groups.len()).all(|group_index| {
        !compares(group_index)
            || a.groups[group_index] == b.groups[group_index]
            || crate::css::computed_values::style_group_payloads_equal(
                group_index,
                a.groups[group_index],
                b.groups[group_index],
            )
    })
}

/// The image resources a tree build owes the host for a row it stamped, which the host attaches
/// once the layout update the build ran in is over.
#[derive(Clone, Copy)]
pub(crate) enum OwedImageResources {
    /// The row's style resources: the images its style names, and the paint facts that follow,
    /// with whether the row owns the provider of the image its content is replaced with.
    StyleResources { owns_content_replacement_image: bool },
    /// The provider of the image a pseudo-element's generated content names, which the image box
    /// owns, and the box's style resources.
    GeneratedImage {
        generator: StyleNodeID,
        pseudo_element: super::tree_builder::FfiPseudoElement,
        image: super::tree_builder::FfiGeneratedImage,
    },
}

impl OwedImageResources {
    /// Whether the box owns the provider of the image it shows.
    pub(crate) fn owns_provider(self) -> bool {
        match self {
            Self::StyleResources {
                owns_content_replacement_image,
            } => owns_content_replacement_image,
            Self::GeneratedImage { .. } => true,
        }
    }
}

/// Where an image box that owns the provider of the image it shows stands with it.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum OwnedImageProvider {
    /// The host hands the box its provider once the layout update the build ran in is over. Until then the box has no
    /// image.
    Awaited,
    /// The box shows the provider's image, never its element's.
    HandedOver,
}

#[must_use]
pub(crate) struct FreedSubtree {
    /// Each freed row, as it was named before it was freed.
    rows: Vec<NodeSlotId>,
    paintable_row_resets: Vec<crate::painting::paintable_rows::PaintableRowReset>,
    arena_pinned_style_records: Vec<u64>,
    style_engine: crate::css::style::StyleEngineHandle,
}

/// The node a layout row can be bound to: an element or text node, named by its identity, a
/// pseudo-element, which has no identity of its own and is named by its generator's identity and
/// its kind, or the document, which is bound to a viewport row.
#[derive(Clone, Copy, PartialEq, Eq)]
enum BoundNode {
    Identity(StyleNodeID),
    PseudoElement(StyleNodeID, u8),
    Document,
}

impl BoundNode {
    /// The node a row carrying `style_node` can be bound to: the node itself, or the pseudo-element
    /// the row was generated for.
    fn of(style_node: StyleNodeID, generated_for: u8) -> Self {
        if generated_for == 0 {
            BoundNode::Identity(style_node)
        } else {
            BoundNode::PseudoElement(style_node, generated_for)
        }
    }
}

impl FreedSubtree {
    #[cfg(test)]
    pub(crate) fn row_count(&self) -> usize {
        self.rows.len()
    }

    #[cfg(test)]
    pub(crate) fn arena_pinned_style_record_count(&self) -> usize {
        self.arena_pinned_style_records.len()
    }

    /// Destroys the freed rows' shells, then what the host holds for the rows by slot.
    pub(crate) fn destroy_shells_and_invoke_callbacks(self, main_thread: &crate::stage::MainThread) {
        if let Some(host_tables) = main_thread.host_tables() {
            for &slot in &self.rows {
                let shell = host_tables.shells.borrow_mut().remove(&slot);
                if let Some(shell) = shell {
                    crate::layout::tree_mutation::destroy_shell(main_thread, shell.as_ptr());
                }
            }
            for &slot in &self.rows {
                host_tables.rows_with_layer_image_paint_facts.borrow_mut().remove(&slot);
                let provider = host_tables.owned_image_providers.borrow_mut().remove(&slot);
                if let Some(provider) = provider {
                    crate::layout::tree_mutation::destroy_owned_image_provider(main_thread, provider);
                }
                let observers = host_tables.replace_image_observers(slot, std::ptr::null_mut());
                crate::layout::tree_mutation::destroy_image_observers(main_thread, observers);
            }
        }
        for reset in self.paintable_row_resets {
            reset.tell(main_thread);
        }
        if !self.style_engine.is_null() {
            for style_record in self.arena_pinned_style_records {
                // SAFETY: Registration and unregistration keep the engine live.
                unsafe { self.style_engine.get_mut() }.unpin_layout_style_record(style_record);
            }
        }
    }
}

const MAXIMUM_PRE_ORDER_LABEL_STRIDE: u64 = 1 << 32;

/// How many bound rows a chunk of a published bound row column holds.
const BOUND_ROWS_PER_CHUNK: usize = 64;

/// The row each node is bound to, kept so that the host reads it from published rows: by the dense
/// index of an element or text identity, by generator and kind for a pseudo-element, and the
/// viewport row for the document.
#[derive(Clone, Default)]
pub(crate) struct BoundRows {
    elements: CowColumn<NodeSlotId, BOUND_ROWS_PER_CHUNK>,
    texts: CowColumn<NodeSlotId, BOUND_ROWS_PER_CHUNK>,
    pseudo_elements: Arc<HashMap<(StyleNodeID, u8), NodeSlotId>>,
    viewport: NodeSlotId,
    /// Advanced by every write to the pseudo-element or viewport rows.
    writes: u64,
}

/// The bound rows as they were published.
pub(crate) struct PublishedBoundRows {
    elements: ColumnSnapshot<NodeSlotId, BOUND_ROWS_PER_CHUNK>,
    texts: ColumnSnapshot<NodeSlotId, BOUND_ROWS_PER_CHUNK>,
    pseudo_elements: Arc<HashMap<(StyleNodeID, u8), NodeSlotId>>,
    viewport: NodeSlotId,
}

impl BoundRows {
    fn column(&self, style_node: StyleNodeID) -> (&CowColumn<NodeSlotId, BOUND_ROWS_PER_CHUNK>, usize) {
        match style_node.text_index() {
            Some(index) => (&self.texts, index as usize),
            None => (&self.elements, style_node.element_slot()),
        }
    }

    fn row(&self, style_node: StyleNodeID) -> NodeSlotId {
        let (column, index) = self.column(style_node);
        column.get(index).copied().unwrap_or(NodeSlotId::INVALID)
    }

    /// Binds `style_node` to `row`, and answers the row it was bound to.
    fn set_row(&mut self, style_node: StyleNodeID, row: NodeSlotId) -> NodeSlotId {
        let (column, index) = match style_node.text_index() {
            Some(index) => (&mut self.texts, index as usize),
            None => (&mut self.elements, style_node.element_slot()),
        };
        let previous = column.get(index).copied().unwrap_or(NodeSlotId::INVALID);
        if previous != row {
            column.grow_to(index + 1);
            column.set(index, row).expect("the column was grown");
        }
        previous
    }

    fn pseudo_element_row(&self, generator: StyleNodeID, generated_for: u8) -> NodeSlotId {
        self.pseudo_elements
            .get(&(generator, generated_for))
            .copied()
            .unwrap_or(NodeSlotId::INVALID)
    }

    /// Binds the pseudo-element to `row`, or unbinds it for an invalid one, and answers the row it was bound to.
    fn set_pseudo_element_row(&mut self, generator: StyleNodeID, generated_for: u8, row: NodeSlotId) -> NodeSlotId {
        let previous = self.pseudo_element_row(generator, generated_for);
        if previous != row {
            self.writes += 1;
            let rows = Arc::make_mut(&mut self.pseudo_elements);
            if row.is_invalid() {
                rows.remove(&(generator, generated_for));
            } else {
                rows.insert((generator, generated_for), row);
            }
        }
        previous
    }

    fn set_viewport_row(&mut self, row: NodeSlotId) -> NodeSlotId {
        if self.viewport != row {
            self.writes += 1;
        }
        std::mem::replace(&mut self.viewport, row)
    }

    fn version(&self) -> u64 {
        self.elements.version() + self.texts.version() + self.writes
    }

    pub(crate) fn publish(&mut self) -> PublishedBoundRows {
        PublishedBoundRows {
            elements: self.elements.publish(),
            texts: self.texts.publish(),
            pseudo_elements: self.pseudo_elements.clone(),
            viewport: self.viewport,
        }
    }
}

impl PublishedBoundRows {
    /// The row the element or text node with `style_node` was bound to.
    pub(crate) fn row(&self, style_node: StyleNodeID) -> NodeSlotId {
        let row = match style_node.text_index() {
            Some(index) => self.texts.get(index as usize),
            None => self.elements.get(style_node.element_slot()),
        };
        row.copied().unwrap_or(NodeSlotId::INVALID)
    }

    pub(crate) fn pseudo_element_row(&self, generator: StyleNodeID, generated_for: u8) -> NodeSlotId {
        self.pseudo_elements
            .get(&(generator, generated_for))
            .copied()
            .unwrap_or(NodeSlotId::INVALID)
    }

    pub(crate) fn viewport_row(&self) -> NodeSlotId {
        self.viewport
    }
}

/// One row for each StyleNodeID, indexed by the identity's dense index within its kind, so element
/// and text identities each cost one entry per node of their own kind.
#[derive(Clone, Default)]
struct RowsByStyleNode {
    elements: Vec<NodeSlotId>,
    texts: Vec<NodeSlotId>,
}

impl RowsByStyleNode {
    fn head(&self, style_node: StyleNodeID) -> NodeSlotId {
        let (rows, index) = match style_node.text_index() {
            Some(index) => (&self.texts, index as usize),
            None => (&self.elements, style_node.element_slot()),
        };
        rows.get(index).copied().unwrap_or(NodeSlotId::INVALID)
    }

    fn head_mut(&mut self, style_node: StyleNodeID) -> &mut NodeSlotId {
        let (rows, index) = match style_node.text_index() {
            Some(index) => (&mut self.texts, index as usize),
            None => (&mut self.elements, style_node.element_slot()),
        };
        if rows.len() <= index {
            rows.resize(index + 1, NodeSlotId::INVALID);
        }
        &mut rows[index]
    }
}

/// What one step of the shadow-including walk that clears stale layout boxes needs to know about a
/// node: whether its boxes belong to someone else, and where the walk goes next.
#[derive(Clone, Copy)]
pub(crate) struct StaleWalkFacts {
    pub(crate) rendered_in_top_layer: bool,
    pub(crate) shadow_root: Option<StyleNodeID>,
    pub(crate) first_dom_child: Option<StyleNodeID>,
    pub(crate) next_dom_sibling: Option<StyleNodeID>,
}

/// How many interned names one SVG element's attribute publication can name.
const PUBLISHED_REFERENCE_ATOM_COUNT: usize = 5;

/// What the arena keeps under a style node identity, besides the rows carrying it. Identities are
/// reissued, so every table here must let go of a retired one: `LayoutNodeArena::forget_style_node`
/// names each field, and a table added here does not compile until it says how it forgets.
#[derive(Clone, Default)]
struct StyleNodeTables {
    /// The CSS counters set of every element and pseudo-element the tree build resolved one for.
    counters_sets: RefCell<super::counters::CountersSets>,
    /// What the tree build recorded about the generated content of each pseudo-element it built.
    generated_content: RefCell<super::generated_content::GeneratedContent>,
    /// The parsed SVG attributes each SVG element published, keyed by its style node: an
    /// element that draws nothing itself has no row, and a mask, clip or pattern has one row per
    /// element that references it.
    ///
    /// The maps are borrowed for writing only by the publication entry points, which can run while a
    /// tree build is under way: an element styled on the build's behalf replaces its style record,
    /// which republishes the resources its style names. No reader holds a borrow across a host call.
    svg_attribute_facts: RefCell<HashMap<StyleNodeID, super::svg_formatting_context::FfiSvgAttributeFacts>>,
    /// The `points` list each <polyline> and <polygon> published beside its facts.
    svg_points: RefCell<HashMap<StyleNodeID, std::sync::Arc<[super::svg_formatting_context::FfiFloatPoint]>>>,
    /// The elements carrying each anchor name, in tree order. The DOM's registry republishes a
    /// name's list whenever it changes.
    anchor_name_elements: RefCell<HashMap<ScopedAnchorName, Vec<StyleNodeID>>>,
    /// The nodes sitting in the user agent shadow tree of the focused text control, which is what
    /// a caret is painted inside. At most one control is focused, so this holds one control's
    /// shadow tree and is empty the rest of the time.
    identities_in_focused_text_control: RefCell<HashSet<StyleNodeID>>,
    /// What each element has scrolled to, held against its identity because the element's box is
    /// replaced whenever its subtree is rebuilt. The element still stores the offset it answers
    /// `scrollTop` with, and publishes it here as it changes, arrives and changes identity. Zero is
    /// the absence of an entry, which is nearly every element.
    element_scroll_offsets: RefCell<HashMap<StyleNodeID, CssPixelPoint>>,
    /// What each element's pseudo-elements have scrolled to, held against the element's identity,
    /// since a pseudo-element has none of its own, and published the way the element's offset is.
    pseudo_element_scroll_offsets: RefCell<HashMap<StyleNodeID, PseudoElementScrollOffsets>>,
}

/// What an element's synthetic pseudo-elements have scrolled to, by kind.
type PseudoElementScrollOffsets = [CssPixelPoint; (GENERATED_FOR_LAST_SYNTHETIC - GENERATED_FOR_AFTER + 1) as usize];

/// Where a synthetic pseudo-element of kind `generated_for` keeps its offset among its element's.
fn pseudo_element_scroll_offset_index(generated_for: u8) -> usize {
    assert!(
        (GENERATED_FOR_AFTER..=GENERATED_FOR_LAST_SYNTHETIC).contains(&generated_for),
        "only a synthetic pseudo-element scrolls"
    );
    usize::from(generated_for - GENERATED_FOR_AFTER)
}

/// An anchor name as a tree scope registers it.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct ScopedAnchorName {
    /// The shadow host of the scope's root, or none for the document tree.
    scope_host: Option<StyleNodeID>,
    /// The name's interned string.
    name: usize,
}

/// Why the arena holds a pin on the style record a row names.
#[derive(Clone, Copy, PartialEq, Eq)]
enum ArenaStylePin {
    None,
    /// The arena derived the record for the row: an anonymous box's, or a style layout adjusted.
    Derived,
    /// The record the row's node published when the build stamped the row, held until the row
    /// takes another.
    Published,
    /// A sample of the row's element's animations a clock tick installed, held until the style the
    /// host installed is restored.
    Sampled,
}

/// What a sample a clock tick shows in a box samples: the animations the host runs on the box's element, or transitions
/// a hover beside the host started on it, which leave a record the host pinned for its own readers theirs until the
/// host starts the transitions in its turn.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum SampleKind {
    Animation,
    Transition,
}

/// The style a clock tick took from a box to show a sample of its element's animations in its place:
/// the record the host installed for the box, and the pin the arena held it by. Only
/// [`LayoutNodeArena::restore_host_style`] gives it back, so the host never reads a sample.
#[must_use = "the host's style is restored before the host reads the box"]
pub(crate) struct HostStyle {
    record: u64,
    pin: ArenaStylePin,
}

impl HostStyle {
    /// The record the host installed for the box.
    pub(crate) fn record(&self) -> u64 {
        self.record
    }
}

#[derive(Clone)]
pub(crate) struct LayoutNodeArena {
    chunks: Vec<Box<Chunk>>,
    tree_shape: TreeShape,
    /// Advanced by every write to a node's shape, or to the style record or node kept beside it, which
    /// publication takes into the published rows.
    shape_writes: ShapeWrites,
    chunks_by_address: Vec<ChunkAddress>,
    slot_metadata: Vec<SlotMetadata>,
    style_records: Vec<Cell<u64>>,
    style_record_pins: Vec<Cell<ArenaStylePin>>,
    /// The style record the host pinned for its readers of a row that outlive the row's place in
    /// the tree (a detached box is read until its row is freed), or zero. A pin is counted, so this
    /// is a pin of its own beside the arena's, and it names the record it took rather than whichever
    /// record the row holds when it goes.
    style_records_pinned_by_host: Vec<Cell<u64>>,
    /// The StyleNodeID of the element or text node each row is bound to, or of the element it is
    /// generated for. Rows carrying one identity are chained through `next_rows_with_same_style_node`
    /// from `first_rows_by_style_node`, so retiring an identity reaches every row that carries it,
    /// bound or not, before the identity can be reused.
    style_nodes: Vec<Cell<Option<StyleNodeID>>>,
    next_rows_with_same_style_node: Vec<Cell<NodeSlotId>>,
    first_rows_by_style_node: RefCell<RowsByStyleNode>,
    /// The row each element or text node is bound to: the row its layout node is. Carrying the
    /// identity does not make a row bound, since first-letter slices and rows awaiting a rebuild
    /// carry it too. The principal box each pseudo-element is bound to is keyed by its generator's
    /// identity and its kind; the generated content inside the box carries the same pair but is
    /// never bound. The document, which has no identity of its own, is bound to the viewport row.
    bound_rows: RefCell<BoundRows>,
    /// What the navigable has scrolled the viewport to, which the viewport's row holds.
    viewport_scroll_offset: Cell<CssPixelPoint>,
    /// The style node of the document the last layout tree build was for. The style mirror names
    /// the document element as its first DOM child.
    document_style_node: Cell<Option<StyleNodeID>>,
    /// The style engine the document registered, which owns the records rows are built from;
    /// null when it registered none, as in a layout test.
    style_engine: Cell<crate::css::style::StyleEngineHandle>,
    box_presence_host: Cell<Option<BoxPresenceHost>>,
    /// The nodes whose boxes a running tree build or layout write changed, which the host hears of
    /// once it is over, or `None` while none runs.
    queued_box_presence: RefCell<Option<Vec<BoundNode>>>,
    /// Whether the document is an SVG file decoded as an image, which is fixed for its lifetime.
    document_is_decoded_svg: Cell<bool>,
    /// Depth of synchronous layout passes, including their commits, on the stack.
    active_layout_pass_depth: Cell<u32>,
    /// Whether any fragment-cache epoch changed during the outermost active layout pass. Geometry that the pass laid
    /// out before the change may not match the box's current epoch.
    fragment_cache_epoch_changed_during_layout_pass: Cell<bool>,
    /// The viewport the last layout tree build placed, invalid once that box is freed.
    layout_root: Cell<NodeSlotId>,
    /// The subtree roots the last layout tree build rebuilt, waiting for the partial relayout
    /// plan that follows it. A full layout pass covers every one of them, so its commit clears
    /// them.
    pending_rebuilt_subtree_roots: RefCell<Vec<NodeSlotId>>,
    pending_layout_tree_update_escaped_rebuild_roots: Cell<bool>,
    /// Every box must be recreated by the next layout tree build; set when the tree is torn down
    /// or a build finds a box it cannot place among rebuilt roots, cleared by the full pass.
    needs_full_layout_tree_update: Cell<bool>,
    /// The nodes whose container-relative lengths a pass that could not ask the host resolved without their container.
    unresolved_container_lengths: RefCell<Vec<NodeSlotId>>,
    partial_layout_count: Cell<u64>,
    full_layout_count: Cell<u64>,
    layout_tree_build_stats: Cell<FfiLayoutTreeBuildStats>,
    pre_order_labels: Vec<Cell<u64>>,
    pre_order_relabel_count: Cell<u64>,
    free_list: Vec<u32>,
    next_index: u32,
    live_count: u32,
    /// The slots whose intrinsic size cache entries the layout stage drops before its next run.
    intrinsic_size_cache_drops: RefCell<Vec<u32>>,
    table_cell_measurement_cache_misses: Cell<u64>,
    intrinsic_measurements: Cell<u64>,
    intrinsic_inline_measurements: Cell<u64>,
    default_scroll_shift_anchors: RefCell<Vec<DefaultScrollShiftAnchorSlot>>,
    any_default_scroll_shift_anchor_ever_stored: Cell<bool>,
    text_slots: TextSlots,
    pub(super) searchable_text: Option<Vec<super::text_queries::MappedText>>,
    replaced_content_facts: Vec<ReplacedContentFactsSlot>,
    raw_table_column_spans: RefCell<HashMap<NodeSlotId, u32>>,
    replaced_paint_facts: RefCell<Arc<crate::painting::replaced_paint_facts::ReplacedPaintFactsTable>>,
    layer_image_paint_facts: RefCell<Arc<crate::painting::layer_image_paint_facts::LayerImagePaintFactsTable>>,
    svg_paint_resources: crate::painting::svg_paint_resources::SvgPaintResources,
    /// The rows built for one DOM node, chained into a ring through the rows themselves; a row that
    /// is the only one built for its node links to nothing. The chain lives on the rows rather than
    /// under a key so that it survives the node's identity being retired and re-issued.
    next_rows_built_for_same_node: Vec<Cell<NodeSlotId>>,
    fc_run_cache_store: super::fc_run_cache::FcRunCacheArenaStore,
    pub(super) layout_trace: super::trace::LayoutTrace,
    pub(super) innermost_run: Cell<(NodeSlotId, NodeSlotId)>,
    pub(crate) paintable_rows: crate::painting::paintable_rows::PaintableRowStore,
    paint_state: RefCell<crate::painting::paint_state::PaintState>,
    // Hit testing can measure overflow and invalidate painting state while querying this list.
    // Reuse workspace allocations without making recording scratch part of the committed paint state.
    pub(crate) scrollable_overflow: crate::painting::scrollable_overflow::ScrollableOverflowState,
    pub(crate) partial_relayout_boundary_roots: RefCell<super::partial_relayout::PartialRelayoutBoundaryRoots>,
    nodes_with_layout_update_flags: RefCell<Vec<NodeSlotId>>,
    layout_update_flag_node_indices: RefCell<HashMap<NodeSlotId, usize>>,
    pub(super) pending_attached_subtree_roots: RefCell<Vec<NodeSlotId>>,
    /// Boxes whose child lists gained children since the last layout tree build, held back from
    /// layout invalidation until the build shows what the new children are.
    pub(crate) deferred_child_list_insertion_parents: RefCell<Vec<(NodeSlotId, NodeSlotId)>>,
    /// Layout inputs for new absolutely positioned boxes that the build confined to themselves.
    /// They stand in for the committed inputs such a box does not have yet.
    pub(crate) confined_abspos_layout_inputs: RefCell<HashMap<NodeSlotId, AbsposLayoutInputs>>,
    pub(crate) inline_boxes_lifted_out_of: RefCell<HashMap<NodeSlotId, NodeSlotId>>,
    pub(crate) out_of_flow_positioning_contained: RefCell<HashMap<NodeSlotId, u32>>,
    /// Attribution of pending updates for partial relayout. Invariant: every update recorded
    /// since the last layout pass is either attributed to a boundary in the root set above, or
    /// this escape bit is set. Partial relayout may only run while the bit is clear; a full
    /// layout pass re-derives every fact boundary qualification depends on, so it clears the bit.
    pub(crate) pending_updates_escape_partial_relayout: Cell<bool>,
    pub(crate) boxes_needing_scrollable_overflow_recalculation: RefCell<Vec<NodeSlotId>>,
    pub(crate) needs_full_scrollable_overflow_recalculation: Cell<bool>,
    text_nodes_enrolled_for_content_sync: RefCell<HashSet<NodeSlotId>>,
    /// The scroll containers a running tree build gave a style, which may be where scroll
    /// snapping happens once the build is over.
    built_scroll_containers: RefCell<Vec<NodeSlotId>>,
    /// The image resources the tree builds owe the host for the rows they stamped, in the order
    /// the builds came to owe them, until the layout update the builds ran in is over.
    image_resources_owed_to_host: RefCell<Vec<(NodeSlotId, OwedImageResources)>>,
    /// The image boxes among the rows that own the provider of the image they show.
    owned_image_providers: RefCell<HashMap<NodeSlotId, OwnedImageProvider>>,
    /// Whether a row has ever been given a style with `content-visibility: auto`.
    may_have_auto_content_visibility: Cell<bool>,
    /// Whether a row has ever been given a style with a scroll snap type.
    may_have_scroll_snap_areas: Cell<bool>,
    nodes_enrolled_for_replaced_content_facts_sync: RefCell<Vec<NodeSlotId>>,
    /// The natural size of the image each image box that owns its image's provider shows
    /// (`content: url(...)`), as the provider publishes it: zero while the image is not available.
    /// Such a box shows an image of its own rather than its element's.
    owned_image_natural_sizes: RefCell<HashMap<NodeSlotId, crate::css::style::NaturalSize>>,
    /// The natural size layout last negotiated for the box of an `<svg>` document element, and that
    /// box. An `<object>` showing the document is sized from it.
    document_svg_root_natural_size: Cell<Option<(NodeSlotId, crate::css::style::NaturalSize)>>,
    /// The document element's natural size as it was last handed to the `<object>` showing the
    /// document, if it has been.
    handed_over_document_svg_root_natural_size: Cell<Option<crate::css::style::NaturalSize>>,
    /// What the running pass has to tell the document, waiting for the commit that delivers it.
    /// The rows the layout commit in progress gathers for the style engine's container queries.
    pub(crate) layout_style_snapshot_commit: RefCell<Vec<super::style_snapshot::CommittedGeometry>>,
    /// What the DOM has asked the next layout tree build to rebuild, by style node identity.
    layout_tree_update_marks: RefCell<super::tree_update_marks::LayoutTreeUpdateMarks>,
    /// The counter styles each tree scope registers. The rule cache that settles them is C++'s,
    /// and the document owns the result because a fallback chain is followed from the scope the
    /// counter is used in, not from the scope the style was written in.
    counter_styles: RefCell<crate::css::counter_representation::CounterStyleRegistry>,
    style_node_tables: StyleNodeTables,
}

impl LayoutNodeArena {
    pub(crate) fn new() -> Self {
        Self {
            chunks: Vec::new(),
            tree_shape: TreeShape::default(),
            shape_writes: ShapeWrites::default(),
            chunks_by_address: Vec::new(),
            slot_metadata: Vec::new(),
            style_records: Vec::new(),
            style_record_pins: Vec::new(),
            style_records_pinned_by_host: Vec::new(),
            style_nodes: Vec::new(),
            next_rows_with_same_style_node: Vec::new(),
            first_rows_by_style_node: RefCell::new(RowsByStyleNode::default()),
            bound_rows: RefCell::default(),
            viewport_scroll_offset: Cell::new(CssPixelPoint::default()),
            document_style_node: Cell::new(None),
            style_engine: Cell::new(crate::css::style::StyleEngineHandle::null()),
            box_presence_host: Cell::new(None),
            queued_box_presence: RefCell::new(None),
            document_is_decoded_svg: Cell::new(false),
            active_layout_pass_depth: Cell::new(0),
            fragment_cache_epoch_changed_during_layout_pass: Cell::new(false),
            layout_root: Cell::new(NodeSlotId::INVALID),
            pending_rebuilt_subtree_roots: RefCell::new(Vec::new()),
            pending_layout_tree_update_escaped_rebuild_roots: Cell::new(false),
            needs_full_layout_tree_update: Cell::new(false),
            unresolved_container_lengths: RefCell::new(Vec::new()),
            partial_layout_count: Cell::new(0),
            full_layout_count: Cell::new(0),
            layout_tree_build_stats: Cell::new(FfiLayoutTreeBuildStats::default()),
            pre_order_labels: Vec::new(),
            pre_order_relabel_count: Cell::new(0),
            free_list: Vec::new(),
            next_index: 0,
            live_count: 0,
            intrinsic_size_cache_drops: RefCell::new(Vec::new()),
            table_cell_measurement_cache_misses: Cell::new(0),
            intrinsic_measurements: Cell::new(0),
            intrinsic_inline_measurements: Cell::new(0),
            default_scroll_shift_anchors: RefCell::new(Vec::new()),
            any_default_scroll_shift_anchor_ever_stored: Cell::new(false),
            text_slots: TextSlots::default(),
            searchable_text: None,
            replaced_content_facts: Vec::new(),
            raw_table_column_spans: RefCell::default(),
            replaced_paint_facts: RefCell::default(),
            layer_image_paint_facts: RefCell::default(),
            svg_paint_resources: crate::painting::svg_paint_resources::SvgPaintResources::default(),
            next_rows_built_for_same_node: Vec::new(),
            fc_run_cache_store: super::fc_run_cache::FcRunCacheArenaStore::default(),
            layout_trace: super::trace::LayoutTrace::default(),
            innermost_run: Cell::new((NodeSlotId::INVALID, NodeSlotId::INVALID)),
            paintable_rows: crate::painting::paintable_rows::PaintableRowStore::default(),
            paint_state: RefCell::new(crate::painting::paint_state::PaintState::default()),
            scrollable_overflow: Default::default(),
            partial_relayout_boundary_roots: RefCell::default(),
            nodes_with_layout_update_flags: RefCell::new(Vec::new()),
            layout_update_flag_node_indices: RefCell::new(HashMap::default()),
            pending_attached_subtree_roots: RefCell::new(Vec::new()),
            deferred_child_list_insertion_parents: RefCell::new(Vec::new()),
            confined_abspos_layout_inputs: RefCell::new(HashMap::default()),
            inline_boxes_lifted_out_of: RefCell::new(HashMap::default()),
            out_of_flow_positioning_contained: RefCell::new(HashMap::default()),
            pending_updates_escape_partial_relayout: Cell::new(false),
            boxes_needing_scrollable_overflow_recalculation: RefCell::new(Vec::new()),
            needs_full_scrollable_overflow_recalculation: Cell::new(false),
            text_nodes_enrolled_for_content_sync: RefCell::new(HashSet::default()),
            built_scroll_containers: RefCell::default(),
            image_resources_owed_to_host: RefCell::default(),
            owned_image_providers: RefCell::default(),
            may_have_auto_content_visibility: Cell::new(false),
            may_have_scroll_snap_areas: Cell::new(false),
            nodes_enrolled_for_replaced_content_facts_sync: RefCell::new(Vec::new()),
            owned_image_natural_sizes: RefCell::new(HashMap::default()),
            document_svg_root_natural_size: Cell::new(None),
            handed_over_document_svg_root_natural_size: Cell::new(None),
            layout_style_snapshot_commit: RefCell::new(Vec::new()),
            layout_tree_update_marks: RefCell::default(),
            counter_styles: RefCell::default(),
            style_node_tables: StyleNodeTables::default(),
        }
    }

    /// Drops one node's cached intrinsic sizes outright, for the wrap of its epoch:
    /// entries are stamped with the epoch they were measured under, so a stamp reused
    /// after a full lap would match a pre-wrap entry.
    pub(crate) fn drop_intrinsic_size_cache(&self, data: &NodeData) {
        let (index, _) = self.slot_for_data(data);
        self.intrinsic_size_cache_drops.borrow_mut().push(index);
    }

    /// The slots whose intrinsic size cache entries must go, for the layout stage to drop before
    /// its next run reads any.
    pub(crate) fn take_intrinsic_size_cache_drops(&self) -> Vec<u32> {
        self.intrinsic_size_cache_drops.take()
    }

    /// What an intrinsic size cache entry measured for the row `id` now is valid for.
    pub(crate) fn intrinsic_size_cache_stamp(&self, id: NodeSlotId) -> IntrinsicSizeCacheStamp {
        IntrinsicSizeCacheStamp {
            index: id.slot_index(),
            generation: id.generation(),
            epoch: self.data(id).intrinsic_cache_epoch.get(),
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

    pub(crate) fn end_layout_pass(&self) {
        self.sweep_stale_fc_run_cache_entries();
    }

    /// Drops entries whose slot or epoch no longer matches.
    /// Checks only entries invalidated or stored since the previous sweep. Stale entries
    /// survive until commit so inline layout can reuse their undamaged line prefixes.
    fn sweep_stale_fc_run_cache_entries(&self) {
        self.fc_run_cache_store.sweep_pending_entries(|slot, validity| {
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

    /// Checks that the calling thread may reach the arena: the StyleLayout thread, which the arena's render state lives
    /// on, or the host's, while the StyleLayout thread waits for its next message.
    // Freshly created chunks are default-initialized and free() resets slots on release, so
    // allocate() always hands out clean NodeData without writing it again.
    #[cfg(test)]
    pub(crate) fn allocate(&mut self, construction_facts: super::node_data::NodeConstructionFacts) -> NodeSlotId {
        let slot = self.allocate_unbound();
        self.bind_shell(slot, construction_facts);
        slot
    }

    #[cfg(test)]
    pub(crate) fn bind_shell(&self, slot: NodeSlotId, construction_facts: super::node_data::NodeConstructionFacts) {
        assert!(
            self.slot_is_live(slot),
            "layout node arena bound a shell to a dead slot"
        );
        let data = self.write_shape(slot);
        data.set_kind(construction_facts.kind);
        data.set_flags(super::node_facts::construction_flags(
            construction_facts.kind,
            construction_facts.is_anonymous,
            super::node_facts::construction_fact_word(&construction_facts),
        ));
        data.set_dom_paint_facts(construction_facts.dom_paint_facts);
        self.set_node_style_node(slot, StyleNodeID::from_raw(construction_facts.style_node));
        self.enroll_node_for_replaced_content_facts_sync_if_eligible(slot);
    }

    #[cfg(test)]
    pub(crate) fn allocate_for_test(&mut self) -> NodeAllocation {
        NodeAllocation {
            slot: self.allocate_unbound(),
        }
    }

    #[cfg(test)]
    pub(crate) fn set_style_node_for_test(&self, slot: NodeSlotId, style_node: Option<StyleNodeID>) {
        self.set_node_style_node(slot, style_node);
    }

    /// Derives the replaced content facts of every enrolled node from what its element, or the
    /// image provider the node owns, published, and lets go of the nodes that died.
    fn sync_enrolled_replaced_content_facts(&mut self) {
        let mut enrolled_nodes = std::mem::take(self.nodes_enrolled_for_replaced_content_facts_sync.get_mut());
        enrolled_nodes.retain(|&node| self.slot_is_live(node));
        for &node in &enrolled_nodes {
            // A box waiting for the provider it owns shows no image yet, as the provider says while
            // its image is not available.
            let input = if self.owned_image_providers.get_mut().get(&node) == Some(&OwnedImageProvider::Awaited) {
                crate::css::style::ReplacedContentInput::NaturalSize(crate::css::style::NaturalSize {
                    width: Some(0),
                    height: Some(0),
                    aspect_ratio: None,
                })
            } else if let Some(&natural_size) = self.owned_image_natural_sizes.get_mut().get(&node) {
                crate::css::style::ReplacedContentInput::NaturalSize(natural_size)
            } else if let Some(input) = self.replaced_content_input(node) {
                input
            } else {
                // A row outlives the element it was built for until the host frees it: the rows of a removed
                // subtree, and those of the tree a build replaced, which the layout that follows the build still
                // syncs. Such a row keeps the facts it has.
                continue;
            };
            let facts = super::node_facts::derived_replaced_content_facts(self.data(node), input);
            // Changed facts invalidate cached formatting-context runs regardless of which channel
            // produced the change, including sources with no invalidation of their own.
            if self.set_replaced_content_facts(node, facts) {
                self.bump_fragment_cache_epoch_of_self_and_ancestors(node);
            }
        }
        *self.nodes_enrolled_for_replaced_content_facts_sync.get_mut() = enrolled_nodes;
    }

    pub(crate) fn enroll_node_for_replaced_content_facts_sync_if_eligible(&self, node: NodeSlotId) {
        let data = self.data(node);
        // The pass negotiates an <svg> root's natural size itself.
        if !super::node_facts::node_may_have_replaced_content_facts_including_size_containment(data)
            || data.kind.get() == NodeKind::SVGSVGBox
        {
            return;
        }
        let mut enrolled_nodes = self.nodes_enrolled_for_replaced_content_facts_sync.borrow_mut();
        if !enrolled_nodes.contains(&node) {
            enrolled_nodes.push(node);
        }
    }

    pub(crate) fn allocate_unbound(&mut self) -> NodeSlotId {
        let index = if let Some(index) = self.free_list.pop() {
            index
        } else {
            let index = self.next_index;
            assert!(
                index < MAX_NODE_SLOT_COUNT,
                "layout node arena exhausted its 24-bit slot index space"
            );
            if (index as usize).is_multiple_of(SLOTS_PER_CHUNK) {
                let chunk = Chunk::new();
                let start = chunk.slots_address();
                let chunk_index = self.chunks.len();
                let insertion_index = self.chunks_by_address.partition_point(|address| address.start < start);
                self.chunks_by_address
                    .insert(insertion_index, ChunkAddress { start, chunk_index });
                self.chunks.push(chunk);
                self.shape_writes.add_chunk(chunk_index);
            }
            self.slot_metadata.push(SlotMetadata::default());
            self.style_records.push(Cell::new(0));
            self.style_record_pins.push(Cell::new(ArenaStylePin::None));
            self.style_records_pinned_by_host.push(Cell::new(0));
            self.style_nodes.push(Cell::new(None));
            self.next_rows_with_same_style_node.push(Cell::new(NodeSlotId::INVALID));
            self.next_rows_built_for_same_node.push(Cell::new(NodeSlotId::INVALID));
            self.pre_order_labels.push(Cell::new(0));
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
        *self.data_mut(index).slot_generation.get_mut() = generation;

        NodeSlotId::new(index, generation)
    }

    pub(crate) fn free_subtree(&mut self, root: NodeSlotId) -> FreedSubtree {
        assert!(!root.is_invalid(), "invalid layout node arena slot ID");
        self.assert_node_is_unlinked_from_parent(root);
        if self.layout_root.get() == root {
            self.layout_root.set(NodeSlotId::INVALID);
            self.pending_rebuilt_subtree_roots.get_mut().clear();
            self.pending_layout_tree_update_escaped_rebuild_roots.set(false);
        }
        if !self.scrollable_overflow.non_child_boxes.borrow().is_empty() {
            self.scrollable_overflow.contained_boxes_dirty.set(true);
        }
        if self.scrollable_overflow.viewport.get() == Some(root) {
            self.scrollable_overflow.viewport.set(None);
            self.scrollable_overflow.non_child_boxes.borrow_mut().clear();
            self.scrollable_overflow.full_layout_commit.set(false);
            self.scrollable_overflow.geometry_changed.set(false);
            self.scrollable_overflow.scrollability_changed.set(false);
            self.boxes_needing_scrollable_overflow_recalculation
                .borrow_mut()
                .clear();
            self.needs_full_scrollable_overflow_recalculation.set(false);
        }
        let mut rows = Vec::new();
        self.for_each_node_in_layout_subtree_in_pre_order(root, |slot| rows.push(slot));

        let mut paintable_row_resets = Vec::new();
        let mut arena_pinned_style_records = Vec::new();
        for &slot in &rows {
            if self.style_record_pins[slot.slot_index() as usize].get() != ArenaStylePin::None {
                arena_pinned_style_records.push(self.style_records[slot.slot_index() as usize].get());
            }
            let host_pinned_style_record = self.style_records_pinned_by_host[slot.slot_index() as usize].get();
            if host_pinned_style_record != 0 {
                arena_pinned_style_records.push(host_pinned_style_record);
            }
            self.unlink_children_of_node_being_freed(slot);
            if let Some(reset) = self.free_unlinked_slot(slot) {
                paintable_row_resets.push(reset);
            }
        }
        FreedSubtree {
            rows,
            paintable_row_resets,
            arena_pinned_style_records,
            style_engine: self.style_engine.get(),
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
        self.searchable_text = None;
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
        self.inline_boxes_lifted_out_of.get_mut().remove(&id);
        self.out_of_flow_positioning_contained.get_mut().remove(&id);
        self.pre_order_labels[index as usize].set(0);
        self.partial_relayout_boundary_roots.get_mut().note_freed(id);
        self.metadata_mut(index).occupied = false;
        self.forget_row_sharing_dom_node(id);
        self.unbind_row(id);
        self.set_node_style_node(id, None);
        self.style_records[index as usize].set(0);
        self.style_record_pins[index as usize].set(ArenaStylePin::None);
        self.style_records_pinned_by_host[index as usize].set(0);

        self.intrinsic_size_cache_drops.get_mut().push(index);
        if let Some(slot) = self.default_scroll_shift_anchors.get_mut().get_mut(index as usize) {
            *slot = DefaultScrollShiftAnchorSlot::default();
        }
        self.paintable_rows.reset_committed_fragment_link_slot(index);
        self.text_slots.reset(index as usize);
        self.text_nodes_enrolled_for_content_sync.get_mut().remove(&id);
        if let Some(slot) = self.replaced_content_facts.get_mut(index as usize) {
            *slot = ReplacedContentFactsSlot::default();
        }
        self.owned_image_natural_sizes.get_mut().remove(&id);
        self.owned_image_providers.get_mut().remove(&id);
        self.fc_run_cache_store.remove_entry(index);
        self.remove_layout_update_flag_node(id);
        self.raw_table_column_spans.get_mut().remove(&id);
        if self.replaced_paint_facts.get_mut().contains_key(&id) {
            Arc::make_mut(self.replaced_paint_facts.get_mut()).remove(&id);
        }
        if self.layer_image_paint_facts.get_mut().contains_key(&id) {
            Arc::make_mut(self.layer_image_paint_facts.get_mut()).remove(&id);
        }
        self.svg_paint_resources.forget_slot(id);
        for highlight in crate::painting::selection::HighlightPseudoElement::ALL {
            let styles = self.paint_state.get_mut().highlight_pseudo_styles_mut(highlight);
            if styles.contains_key(&id) {
                Arc::make_mut(styles).remove(&id);
            }
        }
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
        let data = chunk.slot(index % SLOTS_PER_CHUNK);
        assert_eq!(
            data.slot_generation.get(),
            id.generation(),
            "layout node arena read a stale or unused slot"
        );
        data
    }

    /// The node's shape, for writing. See [`ShapeWriter`].
    pub(crate) fn write_shape(&self, id: NodeSlotId) -> ShapeWriter<'_> {
        assert!(!id.is_invalid(), "invalid layout node arena slot ID");
        let index = id.slot_index() as usize;
        let shape = self
            .chunks
            .get(index / SLOTS_PER_CHUNK)
            .expect("invalid layout node arena slot ID")
            .write_shape(index / SLOTS_PER_CHUNK, index % SLOTS_PER_CHUNK, &self.shape_writes);
        assert_eq!(
            shape.slot_generation.get(),
            id.generation(),
            "layout node arena wrote a stale or unused slot"
        );
        shape
    }

    /// What the paint side reads of every node, as it is now. The arena's nodes go on being written,
    /// and the next publication copies only the nodes written since this one.
    pub(crate) fn publish_paint_tree(
        &mut self,
    ) -> crate::cow_column::ColumnSnapshot<super::node_data::PaintNode, PUBLISHED_ROWS_PER_CHUNK> {
        self.tree_shape.publish(
            &self.chunks,
            &self.style_records,
            &self.style_nodes,
            |index| {
                self.style_record_pins
                    .get(index)
                    .is_some_and(|pin| pin.get() == ArenaStylePin::Derived)
            },
            &self.shape_writes,
        )
    }

    /// The shape of the node whose data `data` is, for writing.
    fn write_shape_of(&self, data: &NodeData) -> ShapeWriter<'_> {
        let (index, metadata) = self.slot_for_data(data);
        self.write_shape(NodeSlotId::new(index, metadata.generation))
    }

    pub(crate) fn set_node_generated_for(&self, id: NodeSlotId, generated_for: u8, generator: Option<StyleNodeID>) {
        self.write_shape(id).set_generated_for(generated_for);
        self.set_node_style_node(id, generator);
        // A row generated for a pseudo-element answers by its generator's name. Reading it from
        // the generator's own box would answer nothing for a `display: contents` element, which
        // has no box and still has pseudo-elements.
        if let Some(generator) = generator.filter(|_| generated_for != 0) {
            let unique_node_id = self.with_style_store(|engine| engine.element_unique_node_id(generator));
            self.unique_node_ids().publish(id, unique_node_id);
        }
    }

    pub(crate) fn node_style_node(&self, id: NodeSlotId) -> Option<StyleNodeID> {
        if !self.slot_is_live(id) {
            return None;
        }
        self.style_nodes[id.slot_index() as usize].get()
    }

    /// The style node of the DOM node a row stands for. An anonymous row, which includes a
    /// pseudo-element's, stands for no DOM node, even though it carries its generator's style node.
    pub(crate) fn dom_node_style_node(&self, id: NodeSlotId) -> Option<StyleNodeID> {
        if !self.slot_is_live(id) || super::node_facts::has_flag(self.data(id), NodeFlag::Anonymous) {
            return None;
        }
        self.node_style_node(id)
    }

    /// Names the DOM node a row stands for the way a commit message does: by its style node, or by
    /// 0 for the document, which the viewport row stands for. An anonymous row stands for no DOM
    /// node, and there is nothing to tell the document about it.
    pub(crate) fn commit_message_style_node(&self, id: NodeSlotId) -> Option<u32> {
        if self.slot_is_live(id) && self.data(id).kind.get() == NodeKind::Viewport {
            return Some(0);
        }
        self.dom_node_style_node(id).map(StyleNodeID::raw)
    }

    pub(crate) fn layout_tree_update_marks(&self) -> &RefCell<super::tree_update_marks::LayoutTreeUpdateMarks> {
        &self.layout_tree_update_marks
    }

    /// Retires the layout tree update marks of a node whose stale box was cleared: its own, and its
    /// host's when the node is a shadow root whose own mark was what marked the host, as clearing a
    /// mark through the DOM does, and its children's summary.
    pub(crate) fn retire_layout_tree_update_marks_of_cleared_node(&self, node: StyleNodeID) {
        let mut marks = self.layout_tree_update_marks().borrow_mut();
        let mut current = node;
        while marks.merge(current, false, 0) {
            let Some(host) = self.with_style_store(|engine| engine.tree().host_of(current)) else {
                break;
            };
            current = host;
        }
        marks.set_child_needs(node, false);
    }

    /// Retire the layout tree update marks `style_node` holds, own and child alike.
    pub(crate) fn clear_layout_tree_update_marks(&self, style_node: Option<StyleNodeID>) {
        if let Some(style_node) = style_node {
            self.layout_tree_update_marks.borrow_mut().clear(style_node);
        }
    }

    /// Whether `style_node` itself holds a layout tree update mark.
    pub(crate) fn needs_layout_tree_update(&self, style_node: StyleNodeID) -> bool {
        self.layout_tree_update_marks.borrow().needs(style_node)
    }

    /// Which narrower rebuilds the layout tree update marks `style_node` collected still permit.
    pub(crate) fn layout_tree_update_reuse_reasons(&self, style_node: StyleNodeID) -> u8 {
        self.layout_tree_update_marks.borrow().reuse_reasons(style_node)
    }

    /// Whether a flat-tree descendant of `style_node` holds a layout tree update mark.
    pub(crate) fn child_needs_layout_tree_update(&self, style_node: Option<StyleNodeID>) -> bool {
        style_node.is_some_and(|style_node| self.layout_tree_update_marks.borrow().child_needs(style_node))
    }

    fn set_node_style_node(&self, id: NodeSlotId, style_node: Option<StyleNodeID>) {
        let index = id.slot_index() as usize;
        let previous = self.style_nodes[index].get();
        if previous == style_node {
            return;
        }
        let generated_for = self.data(id).generated_for.get();
        let was_bound = previous.is_some_and(|previous| {
            self.replace_bound_row(BoundNode::of(previous, generated_for), id, NodeSlotId::INVALID)
        });
        let mut first_rows = self.first_rows_by_style_node.borrow_mut();
        if let Some(previous) = previous {
            let next = self.next_rows_with_same_style_node[index].replace(NodeSlotId::INVALID);
            let head = first_rows.head_mut(previous);
            if *head == id {
                *head = next;
            } else {
                let mut row = *head;
                loop {
                    let link = &self.next_rows_with_same_style_node[row.slot_index() as usize];
                    if link.get() == id {
                        link.set(next);
                        break;
                    }
                    row = link.get();
                }
            }
        }
        self.style_nodes[index].set(style_node);
        self.write_shape(id).mark_identity();
        if let Some(style_node) = style_node {
            let head = first_rows.head_mut(style_node);
            self.next_rows_with_same_style_node[index].set(*head);
            *head = id;
            if was_bound {
                self.set_bound_row(BoundNode::of(style_node, generated_for), id);
            }
        }
    }

    /// The row the element or text node with `style_node` is bound to, if any.
    pub(crate) fn bound_row(&self, style_node: StyleNodeID) -> NodeSlotId {
        self.bound_rows.borrow().row(style_node)
    }

    pub(crate) fn bound_rows_mut(&mut self) -> &mut BoundRows {
        self.bound_rows.get_mut()
    }

    pub(crate) fn bound_viewport_row(&self) -> NodeSlotId {
        self.bound_rows.borrow().viewport
    }

    /// The style node of the document the last layout tree build was for.
    pub(crate) fn document_style_node(&self) -> Option<StyleNodeID> {
        self.document_style_node.get()
    }

    pub(crate) fn set_document_style_node(&self, document_style_node: StyleNodeID) {
        self.document_style_node.set(Some(document_style_node));
    }

    /// The row the pseudo-element of kind `generated_for` on the element with `generator` is bound
    /// to, if any.
    pub(crate) fn bound_pseudo_element_row(&self, generator: StyleNodeID, generated_for: u8) -> NodeSlotId {
        self.bound_rows.borrow().pseudo_element_row(generator, generated_for)
    }

    /// The node a row can be bound to, if any.
    fn bound_node_of(&self, id: NodeSlotId) -> Option<BoundNode> {
        if let Some(style_node) = self.style_nodes[id.slot_index() as usize].get() {
            return Some(BoundNode::of(style_node, self.data(id).generated_for.get()));
        }
        if self.data(id).kind.get() == NodeKind::Viewport {
            return Some(BoundNode::Document);
        }
        None
    }

    /// Makes `id` the row `node` is bound to. Every change to a binding goes through here or
    /// `replace_bound_row`, which tell the host when the bound row changes.
    fn set_bound_row(&self, node: BoundNode, id: NodeSlotId) {
        let mut bound_rows = self.bound_rows.borrow_mut();
        let previous = match node {
            BoundNode::Identity(style_node) => bound_rows.set_row(style_node, id),
            BoundNode::PseudoElement(generator, generated_for) => {
                bound_rows.set_pseudo_element_row(generator, generated_for, id)
            }
            BoundNode::Document => bound_rows.set_viewport_row(id),
        };
        drop(bound_rows);
        if previous != id {
            self.notify_box_presence(node);
        }
    }

    /// Rebinds `node` to `replacement` if it is bound to `id`, and returns whether it was.
    fn replace_bound_row(&self, node: BoundNode, id: NodeSlotId, replacement: NodeSlotId) -> bool {
        if id.is_invalid() || self.bound_row_of(node) != id {
            return false;
        }
        let mut bound_rows = self.bound_rows.borrow_mut();
        match node {
            BoundNode::Identity(style_node) => bound_rows.set_row(style_node, replacement),
            BoundNode::PseudoElement(generator, generated_for) => {
                bound_rows.set_pseudo_element_row(generator, generated_for, replacement)
            }
            BoundNode::Document => bound_rows.set_viewport_row(replacement),
        };
        drop(bound_rows);
        if replacement != id {
            self.notify_box_presence(node);
        }
        true
    }

    /// The row `node` is bound to, if any.
    fn bound_row_of(&self, node: BoundNode) -> NodeSlotId {
        match node {
            BoundNode::Identity(style_node) => self.bound_row(style_node),
            BoundNode::PseudoElement(generator, generated_for) => {
                self.bound_pseudo_element_row(generator, generated_for)
            }
            BoundNode::Document => self.bound_viewport_row(),
        }
    }

    /// What a row the build stamped for a DOM node owes the node's other rows, and the node itself.
    /// The row bound before joins the ring of rows built for the same node and keeps its style
    /// readable for as long as the host holds it; the stamped row becomes the node's.
    pub(crate) fn take_over_rows_of_bound_node(&self, slot: NodeSlotId) {
        let Some(node) = self.bound_node_of(slot) else {
            return;
        };
        let previously_bound = self.bound_row_of(node);
        if !previously_bound.is_invalid() {
            self.note_rows_share_dom_node(previously_bound, slot);
            self.pin_style_record_for_detachment(previously_bound);
        }
        self.bind_row(slot);
    }

    /// Binds the node `id` belongs to to `id`, replacing any row bound to it before.
    pub(crate) fn bind_row(&self, id: NodeSlotId) {
        if let Some(node) = self.bound_node_of(id) {
            self.set_bound_row(node, id);
        }
    }

    /// Leaves the node `id` is bound to without a bound row.
    pub(crate) fn unbind_row(&self, id: NodeSlotId) {
        if let Some(node) = self.bound_node_of(id) {
            self.replace_bound_row(node, id, NodeSlotId::INVALID);
        }
    }

    /// Records a new identity for the node bound to `id`, on every row that shares its DOM node.
    pub(crate) fn set_style_node_of_rows_sharing_dom_node_with(&self, id: NodeSlotId, style_node: Option<StyleNodeID>) {
        for row in self.rows_sharing_dom_node_with(id) {
            self.set_node_style_node(row, style_node);
        }
    }

    /// Moves the rows of the node bound under `old_style_node`, and the binding with them, to `new_style_node`.
    pub(crate) fn move_bound_rows_to_style_node(&self, old_style_node: StyleNodeID, new_style_node: StyleNodeID) {
        let bound_row = self.bound_row(old_style_node);
        if !bound_row.is_invalid() {
            self.set_style_node_of_rows_sharing_dom_node_with(bound_row, Some(new_style_node));
        }
    }

    /// Records a new identity for the generator of a pseudo-element box, on the box and the
    /// generated content inside it.
    pub(crate) fn set_style_node_of_generated_subtree(&self, root: NodeSlotId, style_node: Option<StyleNodeID>) {
        self.for_each_node_in_layout_subtree_in_pre_order(root, |row| {
            if self.data(row).generated_for.get() != 0 && !self.node_is_dom_backed(row) {
                self.set_node_style_node(row, style_node);
            }
        });
    }

    /// Moves the box of the pseudo-element of kind `generated_for` bound under `old_generator`, the
    /// generated content inside it and the binding to `new_generator`.
    pub(crate) fn move_bound_pseudo_element_rows_to_style_node(
        &self,
        old_generator: StyleNodeID,
        generated_for: u8,
        new_generator: StyleNodeID,
    ) {
        let bound_row = self.bound_pseudo_element_row(old_generator, generated_for);
        if !bound_row.is_invalid() {
            self.set_style_node_of_generated_subtree(bound_row, Some(new_generator));
        }
    }

    /// Replaces the elements registered under `anchor_name` in the tree scope hosted by
    /// `scope_host`, in tree order. An empty list forgets the name.
    pub(crate) fn set_anchor_name_elements(
        &self,
        scope_host: Option<StyleNodeID>,
        anchor_name: usize,
        elements: impl Iterator<Item = StyleNodeID>,
    ) {
        let mut names = self.style_node_tables.anchor_name_elements.borrow_mut();
        let key = ScopedAnchorName {
            scope_host,
            name: anchor_name,
        };
        let entry = names.entry(key).or_default();
        entry.clear();
        entry.extend(elements);
        if entry.is_empty() {
            names.remove(&key);
        }
    }

    /// The last element in tree order registered under `anchor_name` in the tree scope hosted by
    /// `scope_host` that `is_acceptable` accepts.
    pub(crate) fn last_element_with_anchor_name(
        &self,
        scope_host: Option<StyleNodeID>,
        anchor_name: usize,
        is_acceptable: impl FnMut(&StyleNodeID) -> bool,
    ) -> Option<StyleNodeID> {
        let names = self.style_node_tables.anchor_name_elements.borrow();
        names
            .get(&ScopedAnchorName {
                scope_host,
                name: anchor_name,
            })?
            .iter()
            .rev()
            .copied()
            .find(is_acceptable)
    }

    /// The shadow host of the shadow tree `element` is in, or none in the document tree. A shadow
    /// root has no layout row, so the tree scope is named by its host.
    pub(crate) fn tree_scope_host(&self, element: StyleNodeID) -> Option<StyleNodeID> {
        self.with_style_store(|engine| engine.tree().shadow_host_of(element))
    }

    /// What the row is scrolled to: the viewport's row holds the navigable's offset, a
    /// pseudo-element's box the pseudo-element's, and a row built for an element the element's.
    /// Every other row holds none, including the generated content inside a pseudo-element's box
    /// and a pseudo-element's box that has been replaced by another.
    pub(crate) fn row_scroll_offset(&self, slot: NodeSlotId) -> CssPixelPoint {
        if !self.slot_is_live(slot) {
            return CssPixelPoint::default();
        }
        let data = self.data(slot);
        if data.kind.get() == NodeKind::Viewport {
            return self.viewport_scroll_offset.get();
        }
        let Some(style_node) = self.node_style_node(slot) else {
            return CssPixelPoint::default();
        };
        let generated_for = data.generated_for.get();
        let offset = if generated_for != 0 {
            if self.bound_pseudo_element_row(style_node, generated_for) != slot {
                return CssPixelPoint::default();
            }
            self.style_node_tables
                .pseudo_element_scroll_offsets
                .borrow()
                .get(&style_node)
                .map(|offsets| offsets[pseudo_element_scroll_offset_index(generated_for)])
        } else if super::node_facts::has_flag(data, NodeFlag::Anonymous) {
            None
        } else {
            self.style_node_tables
                .element_scroll_offsets
                .borrow()
                .get(&style_node)
                .copied()
        };
        offset.unwrap_or_default()
    }

    /// Sets the row's `HasScrollOffset` flag from what it is scrolled to, as a row is stamped,
    /// bound or unbound, and as the offset it holds changes. The viewport's row is measured eagerly
    /// anyway, so it never carries the flag.
    fn refresh_has_scroll_offset_flag(&self, slot: NodeSlotId) {
        let has_scroll_offset = self.data(slot).kind.get() != NodeKind::Viewport
            && self.row_scroll_offset(slot) != CssPixelPoint::default();
        self.set_node_flag(slot, NodeFlag::HasScrollOffset, has_scroll_offset);
    }

    /// Records what the element has scrolled to, for every row built for it. A rebuild can leave
    /// an old row and its replacement both built for the element until the commit that retires the
    /// old one, and both answer for the element meanwhile.
    pub(crate) fn set_element_scroll_offset(&self, element: StyleNodeID, offset: CssPixelPoint) {
        {
            let mut offsets = self.style_node_tables.element_scroll_offsets.borrow_mut();
            if offset == CssPixelPoint::default() {
                offsets.remove(&element);
            } else {
                offsets.insert(element, offset);
            }
        }
        let bound_row = self.bound_row(element);
        if !bound_row.is_invalid() {
            for row in self.rows_sharing_dom_node_with(bound_row) {
                self.refresh_has_scroll_offset_flag(row);
            }
        }
    }

    /// Records what the pseudo-element of kind `generated_for` on `generator` has scrolled to.
    pub(crate) fn set_pseudo_element_scroll_offset(
        &self,
        generator: StyleNodeID,
        generated_for: u8,
        offset: CssPixelPoint,
    ) {
        {
            let index = pseudo_element_scroll_offset_index(generated_for);
            let mut offsets = self.style_node_tables.pseudo_element_scroll_offsets.borrow_mut();
            if offset != CssPixelPoint::default() {
                offsets.entry(generator).or_default()[index] = offset;
            } else if let Some(element_offsets) = offsets.get_mut(&generator) {
                element_offsets[index] = offset;
                if element_offsets.iter().all(|&offset| offset == CssPixelPoint::default()) {
                    offsets.remove(&generator);
                }
            }
        }
        let bound_row = self.bound_pseudo_element_row(generator, generated_for);
        if !bound_row.is_invalid() {
            self.refresh_has_scroll_offset_flag(bound_row);
        }
    }

    /// Records what the navigable has scrolled the viewport to.
    pub(crate) fn set_viewport_scroll_offset(&self, offset: CssPixelPoint) {
        self.viewport_scroll_offset.set(offset);
    }

    /// Record whether the node sits in the user agent shadow tree of the focused text control.
    pub(crate) fn set_identity_in_focused_text_control(&self, node: StyleNodeID, value: bool) {
        let mut identities = self.style_node_tables.identities_in_focused_text_control.borrow_mut();
        if value {
            identities.insert(node);
        } else {
            identities.remove(&node);
        }
    }

    /// Whether the row's node sits in the user agent shadow tree of a text control that is focused
    /// right now. Only a row in a user agent shadow tree can be, which its construction flags
    /// answer, so the published set is asked about almost no row at all.
    pub(crate) fn node_is_in_focused_text_control(&self, id: NodeSlotId) -> bool {
        self.node_flags_if_live(id) & NodeFlag::IsInUserAgentShadowTree as u32 != 0
            && self.dom_node_style_node(id).is_some_and(|style_node| {
                self.style_node_tables
                    .identities_in_focused_text_control
                    .borrow()
                    .contains(&style_node)
            })
    }

    /// Clears a retired identity from every row still carrying it, and from every table keyed by
    /// it, including rows of a removed subtree that outlive the element's disconnection.
    pub(crate) fn forget_style_node(&self, style_node: StyleNodeID) {
        let StyleNodeTables {
            counters_sets,
            generated_content,
            svg_attribute_facts,
            svg_points,
            anchor_name_elements,
            identities_in_focused_text_control,
            element_scroll_offsets,
            pseudo_element_scroll_offsets,
        } = &self.style_node_tables;
        counters_sets.borrow_mut().forget(style_node);
        generated_content.borrow_mut().forget(style_node);
        // The SVG facts let go of the reference atoms they hold as they leave.
        let removed = svg_attribute_facts.borrow_mut().remove(&style_node);
        if let Some(removed) = removed {
            self.retain_published_reference_atoms(
                [0; PUBLISHED_REFERENCE_ATOM_COUNT],
                Self::published_reference_atoms(&removed),
            );
        }
        svg_points.borrow_mut().remove(&style_node);
        identities_in_focused_text_control.borrow_mut().remove(&style_node);
        element_scroll_offsets.borrow_mut().remove(&style_node);
        pseudo_element_scroll_offsets.borrow_mut().remove(&style_node);
        // A retired shadow host takes its tree scope with it. The registry withdraws no names from a
        // scope it can no longer name.
        anchor_name_elements
            .borrow_mut()
            .retain(|name, _| name.scope_host != Some(style_node));
        loop {
            let row = self.first_rows_by_style_node.borrow().head(style_node);
            if row.is_invalid() {
                return;
            }
            self.set_node_style_node(row, None);
        }
    }

    pub(crate) fn set_node_style(&self, id: NodeSlotId, style_record: u64, payloads: StylePayloadsRef) -> bool {
        self.write_shape(id).set_style(payloads);
        self.note_row_style(id);
        self.set_node_flag(id, NodeFlag::FollowsPrincipalStyle, false);
        self.invalidate_overflow_after_style_change(id);
        let previous = self.style_records[id.slot_index() as usize].replace(style_record);
        self.shape_writes.note_style();
        self.write_shape(id).mark();
        if self.style_record_pins[id.slot_index() as usize].replace(ArenaStylePin::None) != ArenaStylePin::None {
            self.with_style_engine(|engine| engine.unpin_layout_style_record(previous));
        }
        self.enroll_text_children_for_content_sync(id);
        self.enroll_node_for_replaced_content_facts_sync_if_eligible(id);
        previous != style_record
    }

    /// Applies `rows`, the rows of a style transaction a frame applies ahead of the host, to the boxes of their elements,
    /// as the host's install of each row does: the row's record, the styles the box's anonymous descendants inherit, and
    /// the relayout the move asks for. Answers the rows it applied, by element, or why it applied none: a row's box is
    /// one the host styles in a way of its own. The host installs the rows on the elements once the frame has landed.
    pub(crate) fn apply_flight_style_rows(
        &self,
        host_calls: HostCalls<'_>,
        rows: Vec<FlightStyleRow>,
    ) -> Result<Vec<FlightStyleRow>, Decline> {
        let mut applied = Vec::with_capacity(rows.len());
        for row in rows {
            let slot = self.bound_row(row.style_node);
            if slot.is_invalid() {
                continue;
            }
            // Replaced content, form controls, list items, tables and SVG take facts from their element the host
            // publishes as it styles them, and a box that holds a style of the arena's or one the host pins is styled
            // in a way of its own. So is a table box, whose wrapper may take properties of it and so give it a style of
            // the arena's, which its layout node hears of only as the host pays the frame, after the host installs the
            // row.
            let parent = self.data(slot).parent.get();
            if !matches!(
                self.data(slot).kind.get(),
                NodeKind::Box | NodeKind::BlockContainer | NodeKind::InlineNode
            ) || self.style_records[slot.slot_index() as usize].get() != row.old_style_record
                || self.node_style_record_is_derived(slot)
                || self.node_style_record_pinned_by_host(slot) != 0
                || (!parent.is_invalid() && self.data(parent).kind.get() == NodeKind::TableWrapper)
            {
                return Err(Decline::LayoutNode);
            }
            applied.push(row);
        }
        for row in &applied {
            let slot = self.bound_row(row.style_node);
            let previous_payloads = self.data(slot).style.get();
            let payloads = self.with_style_engine(|engine| {
                engine
                    .style_record_payloads(row.new_style_record)
                    .map_or(std::ptr::null(), <[_]>::as_ptr)
            });
            let payloads = StylePayloadsRef::new(payloads.cast());
            if self.set_node_style(slot, row.new_style_record, payloads) {
                self.refresh_style_flags(slot);
            }
            self.publish_new_size_container_geometry(slot);
            self.enroll_node_for_svg_paint_resources_sync(slot);
            self.set_node_flag(slot, NodeFlag::HasAnimatedOpacityOrTransform, false);
            if !style_payloads_equal_in_layout_affecting_groups(previous_payloads, payloads) {
                self.bump_fragment_cache_epoch_of_self_and_ancestors(slot);
                self.reset_cached_intrinsic_sizes_of_self_and_ancestors(slot);
            }
            self.reinherit_anonymous_descendants(HostCalls(host_calls.0), slot);
            if row.relayout {
                self.mark_row_for_relayout_after_style_change(row.style_node, slot);
            }
        }
        applied.sort_unstable_by_key(|row| row.style_node);
        Ok(applied)
    }

    /// The box of the element or pseudo-element `row` of a hover's style transaction styles, `generated_for` naming the
    /// pseudo-element, or 0 for the element.
    fn hover_row_box(&self, row: &crate::css::style::bridge::FfiStyleDelta, generated_for: u8) -> NodeSlotId {
        let Some(node) = StyleNodeID::from_raw(row.style_node) else {
            return NodeSlotId::INVALID;
        };
        match generated_for {
            0 => self.bound_row(node),
            generated_for => self.bound_pseudo_element_row(node, generated_for),
        }
    }

    /// Whether the box of the element or pseudo-element `row` of a hover's style transaction moves takes the row's
    /// record as the host's install of it would, without the host: the move rebuilds no box, and a box it styles is a
    /// plain one that shows the record the row moves from. List items, images and SVG boxes, which take facts from their
    /// element as the host styles them, take a move that only repaints them.
    /// Whether the move of `row` builds boxes again, which the box takes only from a build.
    pub(crate) fn hover_row_builds_boxes_again(&self, row: &crate::css::style::bridge::FfiStyleDelta) -> bool {
        row.record_damage & crate::css::style::bridge::FfiStyleInvalidationField::LevelMask as u32 >= REBUILD_LEVEL
    }

    pub(crate) fn takes_hover_row(&self, row: &crate::css::style::bridge::FfiStyleDelta, generated_for: u8) -> bool {
        self.hover_row_box_refusal(row, generated_for).is_none()
    }

    /// Why the box of the element or pseudo-element `row` of a hover's style transaction moves cannot take the row's
    /// record without the host, or none where it can (see [`Self::takes_hover_row`]).
    pub(crate) fn hover_row_box_refusal(
        &self,
        row: &crate::css::style::bridge::FfiStyleDelta,
        generated_for: u8,
    ) -> Option<String> {
        use crate::css::style::bridge::FfiStyleInvalidationField;
        let level = row.record_damage & FfiStyleInvalidationField::LevelMask as u32;
        if level >= REBUILD_LEVEL {
            return Some("a move that builds boxes again".into());
        }
        let slot = self.hover_row_box(row, generated_for);
        // A row without a new record moves only what its element's children inherit.
        if slot.is_invalid() || row.old_style_record == row.new_style_record || row.new_style_record == 0 {
            return None;
        }
        let kind = self.data(slot).kind.get();
        let repaints_only = level < RELAYOUT_LEVEL;
        let plain = matches!(kind, NodeKind::Box | NodeKind::BlockContainer | NodeKind::InlineNode)
            || (repaints_only
                && matches!(
                    kind,
                    NodeKind::ListItemBox
                        | NodeKind::ListItemMarkerBox
                        | NodeKind::ImageBox
                        | NodeKind::VideoBox
                        | NodeKind::SVGSVGBox
                        | NodeKind::SVGGeometryBox
                        | NodeKind::SVGGraphicsBox
                        | NodeKind::SVGTextBox
                ));
        if !plain {
            return Some(format!("a {kind:?} box at damage level {level}"));
        }
        // A record the host pinned for its own readers stays theirs until the host installs the row, beside the one
        // the box shows.
        if self.style_records[slot.slot_index() as usize].get() != row.old_style_record {
            return Some("a box that shows another record".into());
        }
        if self.style_record_pins[slot.slot_index() as usize].get() == ArenaStylePin::Sampled {
            return Some("a box that shows an animation sample".into());
        }
        if self.node_style_record_is_derived(slot) {
            return Some("a box whose record the arena derived".into());
        }
        let parent = self.data(slot).parent.get();
        (!parent.is_invalid() && self.data(parent).kind.get() == NodeKind::TableWrapper).then(|| "a table box".into())
    }

    /// Whether the box of the element or pseudo-element `row` of a hover's style transaction styles shows the record the
    /// row moves to already, or there is no box.
    pub(crate) fn hover_row_box_shows_its_record(
        &self,
        row: &crate::css::style::bridge::FfiStyleDelta,
        generated_for: u8,
    ) -> bool {
        let slot = self.hover_row_box(row, generated_for);
        slot.is_invalid() || self.style_records[slot.slot_index() as usize].get() == row.new_style_record
    }

    /// Installs the record `row` of a hover's style transaction moves to in the box of its element or pseudo-element,
    /// which [`Self::takes_hover_row`] took, as the host's install of the row does: the style, the styles the box's
    /// anonymous descendants inherit, and the relayout, the visual contexts and the repaint the move asks for.
    pub(crate) fn install_hover_row(
        &self,
        host_calls: HostCalls<'_>,
        row: &crate::css::style::bridge::FfiStyleDelta,
        generated_for: u8,
    ) {
        use crate::css::style::bridge::FfiStyleInvalidationField;
        let slot = self.hover_row_box(row, generated_for);
        if slot.is_invalid() || row.old_style_record == row.new_style_record || row.new_style_record == 0 {
            return;
        }
        let previous_payloads = self.data(slot).style.get();
        let payloads = self.with_style_engine(|engine| {
            engine
                .style_record_payloads(row.new_style_record)
                .map_or(std::ptr::null(), <[_]>::as_ptr)
        });
        let payloads = StylePayloadsRef::new(payloads.cast());
        if self.set_node_style(slot, row.new_style_record, payloads) {
            self.refresh_style_flags(slot);
        }
        self.publish_new_size_container_geometry(slot);
        self.enroll_node_for_svg_paint_resources_sync(slot);
        if !style_payloads_equal_in_layout_affecting_groups(previous_payloads, payloads) {
            self.bump_fragment_cache_epoch_of_self_and_ancestors(slot);
            self.reset_cached_intrinsic_sizes_of_self_and_ancestors(slot);
        }
        self.reinherit_anonymous_descendants(HostCalls(host_calls.0), slot);
        let level = row.record_damage & FfiStyleInvalidationField::LevelMask as u32;
        if level >= RELAYOUT_LEVEL
            && let Some(style_node) = self.node_style_node(slot)
        {
            self.mark_row_for_relayout_after_style_change(style_node, slot);
        }
        self.note_style_visual_context_moves(slot, row.record_damage);
        self.push_paint_damage_for_repaint(slot, crate::painting::record::damage::PaintDamage::ALL_PRODUCERS);
    }

    /// Notes the visual contexts a style move with `damage`, an `FfiStyleInvalidationField` word, moves for `slot`'s
    /// box, which the next recording builds again.
    pub(crate) fn note_style_visual_context_moves(&self, slot: NodeSlotId, damage: u32) {
        use crate::css::style::bridge::FfiStyleInvalidationField;
        use crate::painting::visual_context::dirty::VisualContextBoxDirtyKind;
        let visual_contexts = (damage >> FfiStyleInvalidationField::VisualContextShift as u32)
            & FfiStyleInvalidationField::LevelMask as u32;
        if visual_contexts == 0 || !crate::painting::paint_read::GeometryRead::paintable_row_is_populated(self, slot) {
            return;
        }
        let kind = match visual_contexts {
            1 => VisualContextBoxDirtyKind::StyleValueChange,
            _ => VisualContextBoxDirtyKind::StyleStructuralChange,
        };
        self.note_visual_context_box_dirty(slot, kind);
    }

    /// Whether the box `row` shows `sample`, a sample of `kind` of the effects of its element, where
    /// [`Self::install_sample`] would. A box the host styles in a way of its own shows no sample, and neither does one
    /// whose sample moves the style of an anonymous box: its layout node would have to hear of a style no host reads.
    pub(crate) fn takes_sample(&self, row: NodeSlotId, sample: &DerivedStyleRecord, kind: SampleKind) -> bool {
        matches!(
            self.data(row).kind.get(),
            NodeKind::Box | NodeKind::BlockContainer | NodeKind::InlineNode
        ) && self.style_record_pins[row.slot_index() as usize].get() != ArenaStylePin::Derived
            && (kind == SampleKind::Transition || self.node_style_record_pinned_by_host(row) == 0)
            && self.node_style_node(row).is_some()
            && !self.sample_moves_anonymous_box_style(row, sample.payloads)
    }

    /// Shows `sample`, a sample of `kind` of the effects of the element whose box `row` is, in the box in place of the
    /// record the host installed, with the relayout the move asks for, where the box takes it (see
    /// [`Self::takes_sample`]). Answers the host's style where the box held it, or nothing where it held a sample
    /// already, which `sample` replaces.
    pub(crate) fn install_sample(
        &self,
        row: NodeSlotId,
        sample: DerivedStyleRecord,
        kind: SampleKind,
    ) -> Result<Option<HostStyle>, NeedsHost> {
        if !self.takes_sample(row, &sample, kind) {
            self.with_style_engine(|engine| engine.unpin_layout_style_record(sample.record));
            return Err(NeedsHost);
        }
        let index = row.slot_index() as usize;
        let host_style = match self.style_record_pins[index].get() {
            ArenaStylePin::Sampled => {
                let previous = self.style_records[index].get();
                self.with_style_engine(|engine| engine.unpin_layout_style_record(previous));
                None
            }
            pin => Some(HostStyle {
                record: self.style_records[index].get(),
                pin,
            }),
        };
        self.style_record_pins[index].set(ArenaStylePin::Sampled);
        self.show_style(row, self.node_style_node(row), sample);
        Ok(host_style)
    }

    /// Whether showing a style with `payloads` in `row` moves the style of an anonymous box: the table wrapper's, which
    /// takes properties of its table box of its own, or an anonymous child's, which inherits the inherited groups.
    fn sample_moves_anonymous_box_style(&self, row: NodeSlotId, payloads: StylePayloadsRef) -> bool {
        let parent = self.data(row).parent.get();
        if !parent.is_invalid() && self.data(parent).kind.get() == NodeKind::TableWrapper {
            return true;
        }
        let anonymous_with_style = NodeFlag::Anonymous as u32 | NodeFlag::HasStyle as u32;
        let mut child = self.data(row).first_child.get();
        while !child.is_invalid() {
            if self.data(child).flags.get() & anonymous_with_style == anonymous_with_style {
                return !style_payloads_equal_in_inherited_groups(self.data(row).style.get(), payloads);
            }
            child = self.data(child).next_sibling.get();
        }
        false
    }

    /// Gives `row` back the style the host installed for it, which a clock tick took to show a sample in its place.
    pub(crate) fn restore_host_style(&self, row: NodeSlotId, host_style: HostStyle) {
        let index = row.slot_index() as usize;
        assert!(
            self.style_record_pins[index].get() == ArenaStylePin::Sampled,
            "a box shows a sample until its host's style is restored"
        );
        let sample = self.style_records[index].get();
        self.with_style_engine(|engine| engine.unpin_layout_style_record(sample));
        let HostStyle { record, pin } = host_style;
        let payloads = self.with_style_engine(|engine| {
            engine
                .style_record_payloads(record)
                .expect("the host's style record is live while a box shows a sample")
                .as_ptr()
        });
        self.style_record_pins[index].set(pin);
        self.show_style(
            row,
            self.node_style_node(row),
            DerivedStyleRecord {
                record,
                payloads: StylePayloadsRef::new(payloads.cast()),
            },
        );
    }

    /// Shows `style` in `row`, whose pin the caller set, as the host's install of a row of a box that styles no anonymous
    /// box does.
    fn show_style(&self, row: NodeSlotId, style_node: Option<StyleNodeID>, style: DerivedStyleRecord) {
        let previous_payloads = self.data(row).style.get();
        self.style_records[row.slot_index() as usize].set(style.record);
        let shape = self.write_shape(row);
        shape.set_style(style.payloads);
        shape.mark();
        self.note_row_style(row);
        self.refresh_style_flags(row);
        self.invalidate_overflow_after_style_change(row);
        self.enroll_text_children_for_content_sync(row);
        self.enroll_node_for_svg_paint_resources_sync(row);
        if style_payloads_equal_in_layout_affecting_groups(previous_payloads, style.payloads) {
            return;
        }
        self.bump_fragment_cache_epoch_of_self_and_ancestors(row);
        self.reset_cached_intrinsic_sizes_of_self_and_ancestors(row);
        if let Some(style_node) = style_node {
            self.mark_row_for_relayout_after_style_change(style_node, row);
        }
    }

    /// Marks `slot`, the box of the element `style_node` names, for the relayout a style change asks for, as the host
    /// does. Only a full layout pass propagates the viewport's overflow, writing mode and direction from their elements
    /// again, so the relayout of one of them does not finish as a partial relayout. A relayout of an absolutely
    /// positioned partial relayout boundary stays confined to it, as the box contributes nothing to ancestor layout;
    /// a rendered ::backdrop keeps it from being confined, as its box is the element box's sibling.
    fn mark_row_for_relayout_after_style_change(&self, style_node: StyleNodeID, slot: NodeSlotId) {
        let viewport_propagation_source =
            self.data(slot).flags.get() & (NodeFlag::IsDocumentElement as u32 | NodeFlag::IsBody as u32) != 0;
        if viewport_propagation_source {
            self.record_partial_relayout_escape();
        }
        let confined = !viewport_propagation_source
            && self
                .style_payloads(slot)
                .is_some_and(|payloads| ComputedValuesView::new(&payloads.groups).is_absolutely_positioned())
            && self.node_is_partial_relayout_boundary(slot)
            && self
                .bound_pseudo_element_row(style_node, super::node_data::GENERATED_FOR_BACKDROP)
                .is_invalid();
        if confined {
            self.set_node_flag(slot, NodeFlag::NeedsOwnGeometryUpdate, true);
        }
        self.set_needs_layout_update(slot, !confined);
    }

    pub(crate) fn enroll_node_for_svg_paint_resources_sync(&self, id: NodeSlotId) {
        use crate::painting::svg_paint_resources::SvgPaintResourceKind;
        let Some(style) = self.node_style_if_live(id) else {
            return;
        };
        let effects = style.effects();
        let mut kinds = 0;
        if crate::painting::css_filter::contains_url(&effects.filter) {
            kinds |= SvgPaintResourceKind::Filter.bit();
        }
        if crate::painting::css_filter::contains_url(&effects.backdrop_filter) {
            kinds |= SvgPaintResourceKind::BackdropFilter.bit();
        }
        if crate::painting::node_painting::is_svg_path(self.data(id).kind.get()) {
            let svg = style.inherited_svg();
            if svg.fill.kind == crate::css::computed_value_types::SVG_PAINT_URL {
                kinds |= SvgPaintResourceKind::Fill.bit();
            }
            if svg.stroke.kind == crate::css::computed_value_types::SVG_PAINT_URL {
                kinds |= SvgPaintResourceKind::Stroke.bit();
            }
        }
        self.svg_paint_resources.set_enrolled_kinds(id, kinds);
    }

    pub(crate) fn svg_paint_resources(&self) -> &crate::painting::svg_paint_resources::SvgPaintResources {
        &self.svg_paint_resources
    }

    /// A fork of the arena, linked to `engine`, the fork of its style engine: its chunks have addresses of their own,
    /// and the flags the arena shares with the host are its own. See [`crate::fork`].
    pub(crate) fn fork(&self, engine: crate::css::style::StyleEngineHandle) -> Self {
        let mut fork = self.clone();
        fork.chunks_by_address = fork
            .chunks
            .iter()
            .enumerate()
            .map(|(chunk_index, chunk)| ChunkAddress {
                start: chunk.slots_address(),
                chunk_index,
            })
            .collect();
        fork.chunks_by_address.sort_unstable_by_key(|address| address.start);
        fork.style_engine.set(engine);
        fork.svg_paint_resources.detach_enrolled_flag_for_fork();
        fork
    }

    /// See [`crate::painting::svg_paint_resources::SvgPaintResources::share_enrolled_flag`].
    pub(crate) fn share_svg_paint_resources_enrolled(&mut self, flag: std::sync::Arc<std::sync::atomic::AtomicBool>) {
        self.svg_paint_resources.share_enrolled_flag(flag);
    }

    pub(crate) fn node_style_record(&self, id: NodeSlotId) -> u64 {
        assert!(
            self.slot_is_live(id),
            "layout node arena read the style record of a dead slot"
        );
        self.style_records[id.slot_index() as usize].get()
    }

    /// Whether the row's record is one the arena derived for it rather than one its node published.
    pub(crate) fn node_style_record_is_derived(&self, id: NodeSlotId) -> bool {
        assert!(
            self.slot_is_live(id),
            "layout node arena read the style pin of a dead slot"
        );
        self.style_record_pins[id.slot_index() as usize].get() == ArenaStylePin::Derived
    }

    /// Pins the style record of a row that is leaving the layout tree for the host's readers, which
    /// read it until the row is freed. A text row has no record of its own.
    pub(crate) fn pin_style_record_for_detachment(&self, row: NodeSlotId) {
        if !super::tree_builder::node_kind_is_node_with_style(self.data(row).kind.get()) {
            return;
        }
        let style_record = self.style_records[row.slot_index() as usize].get();
        if style_record != 0 {
            self.pin_node_style_record_for_host(row, style_record);
        }
    }

    /// Pins `record` for the host's readers of `id`. A row holds at most one such pin; asking again
    /// while one is held keeps the one it has.
    pub(crate) fn pin_node_style_record_for_host(&self, id: NodeSlotId, record: u64) {
        assert!(record != 0, "a row pinned a null style record for its host");
        let pinned = &self.style_records_pinned_by_host[id.slot_index() as usize];
        if pinned.get() != 0 {
            return;
        }
        pinned.set(record);
        self.with_style_engine(|engine| engine.pin_layout_style_record(record));
    }

    /// Releases the pin the host holds on `id`'s style record, if it holds one.
    pub(crate) fn release_node_style_record_pin_for_host(&self, id: NodeSlotId) {
        let record = self.style_records_pinned_by_host[id.slot_index() as usize].replace(0);
        if record != 0 {
            self.with_style_engine(|engine| engine.unpin_layout_style_record(record));
        }
    }

    /// The style record the host pinned for `id`, or zero.
    pub(crate) fn node_style_record_pinned_by_host(&self, id: NodeSlotId) -> u64 {
        self.style_records_pinned_by_host[id.slot_index() as usize].get()
    }

    pub(crate) fn set_style_engine(&self, style_engine: crate::css::style::StyleEngineHandle) {
        self.style_engine.set(style_engine);
    }

    pub(crate) fn set_document_is_decoded_svg(&self, is_decoded_svg: bool) {
        self.document_is_decoded_svg.set(is_decoded_svg);
    }

    pub(crate) fn document_is_decoded_svg(&self) -> bool {
        self.document_is_decoded_svg.get()
    }

    /// True while a synchronous layout pass, including its commit, is on the stack. Computed
    /// values must never be replaced in that window: the pass caches decoded style and borrows
    /// payload pointers that a replacement would invalidate under it.
    pub(crate) fn layout_pass_is_running(&self) -> bool {
        self.active_layout_pass_depth.get() > 0
    }

    pub(crate) fn begin_active_layout_pass(&self) {
        let depth = self.active_layout_pass_depth.get();
        if depth == 0 {
            self.fragment_cache_epoch_changed_during_layout_pass.set(false);
        }
        self.active_layout_pass_depth.set(depth + 1);
    }

    /// Leaves a pass, and answers whether it was the outermost one.
    pub(crate) fn leave_active_layout_pass(&self) -> bool {
        let depth = self.active_layout_pass_depth.get();
        assert!(depth > 0, "layout pass depth underflow");
        self.active_layout_pass_depth.set(depth - 1);
        depth == 1
    }

    pub(crate) fn set_layout_root(&self, viewport: NodeSlotId) {
        self.layout_root.set(viewport);
    }

    pub(crate) fn layout_root(&self) -> NodeSlotId {
        self.layout_root.get()
    }

    /// Whether `id` is the live box of an `<svg>` document element.
    pub(crate) fn is_document_svg_root_box(&self, id: NodeSlotId) -> bool {
        let layout_root = self.layout_root();
        self.slot_is_live(id)
            && !layout_root.is_invalid()
            && self.data(id).kind.get() == NodeKind::SVGSVGBox
            && self.data(id).parent.get() == layout_root
    }

    /// Note the natural size layout negotiated for the box of an `<svg>` document element.
    pub(crate) fn note_document_svg_root_natural_size(
        &self,
        root_box: NodeSlotId,
        natural_size: crate::css::style::NaturalSize,
    ) {
        self.document_svg_root_natural_size.set(Some((root_box, natural_size)));
    }

    /// The natural size of the document's `<svg>` document element as its last layout negotiated
    /// it, unless it is what the last call handed over. The size is all absent if the document
    /// element is no `<svg>` with a box.
    pub(crate) fn take_changed_document_svg_root_natural_size(&self) -> Option<crate::css::style::NaturalSize> {
        let current = self
            .document_svg_root_natural_size
            .get()
            .filter(|&(root_box, _)| self.is_document_svg_root_box(root_box))
            .map(|(_, natural_size)| natural_size)
            .unwrap_or_default();
        (self.handed_over_document_svg_root_natural_size.replace(Some(current)) != Some(current)).then_some(current)
    }

    /// Publish the natural size of the image the image box `id` owns the provider of, which the
    /// box's replaced content facts are derived from.
    pub(crate) fn set_owned_image_natural_size(&self, id: NodeSlotId, natural_size: crate::css::style::NaturalSize) {
        assert_eq!(self.data(id).kind.get(), NodeKind::ImageBox);
        self.owned_image_natural_sizes.borrow_mut().insert(id, natural_size);
    }

    /// Whether the tree the arena holds already reflects every pending update. A document without
    /// a layout root needs one built, so it is never up to date; the DOM-side half of the answer
    /// comes in as `document_needs_layout_tree_build`.
    pub(crate) fn layout_is_up_to_date(&self, document_needs_layout_tree_build: bool) -> bool {
        let layout_root = self.layout_root();
        if layout_root.is_invalid() {
            return false;
        }
        !self.node_needs_layout_update(layout_root)
            && !document_needs_layout_tree_build
            && !self.needs_full_layout_tree_update()
            && !self.has_partial_relayout_boundary_roots()
    }

    pub(crate) fn set_pending_rebuilt_subtree_roots(
        &self,
        roots: Vec<NodeSlotId>,
        layout_tree_update_escaped_rebuild_roots: bool,
    ) {
        *self.pending_rebuilt_subtree_roots.borrow_mut() = roots;
        self.pending_layout_tree_update_escaped_rebuild_roots
            .set(layout_tree_update_escaped_rebuild_roots);
    }

    pub(crate) fn take_pending_rebuilt_subtree_roots(&self) -> (Vec<NodeSlotId>, bool) {
        (
            std::mem::take(&mut *self.pending_rebuilt_subtree_roots.borrow_mut()),
            self.pending_layout_tree_update_escaped_rebuild_roots.replace(false),
        )
    }

    /// The DOM nodes the subtrees the last build rebuilt and left live stand for. An anonymous root stands for none.
    pub(crate) fn pending_rebuilt_dom_roots(&self) -> Vec<crate::painting::host::FfiNodeIdentity> {
        self.pending_rebuilt_subtree_roots
            .borrow()
            .iter()
            .filter(|&&root| self.node_is_dom_backed(root))
            .map(|&root| crate::painting::hit_test::resolve::row_node_identity(self, root, false))
            .collect()
    }

    pub(crate) fn clear_pending_rebuilt_subtree_roots(&self) {
        self.pending_rebuilt_subtree_roots.borrow_mut().clear();
        self.pending_layout_tree_update_escaped_rebuild_roots.set(false);
    }

    pub(crate) fn note_partial_layout(&self) {
        self.partial_layout_count.set(self.partial_layout_count.get() + 1);
    }

    pub(crate) fn note_full_layout(&self) {
        self.full_layout_count.set(self.full_layout_count.get() + 1);
    }

    pub(crate) fn partial_layout_count(&self) -> u64 {
        self.partial_layout_count.get()
    }

    pub(crate) fn full_layout_count(&self) -> u64 {
        self.full_layout_count.get()
    }

    pub(crate) fn record_layout_tree_build(&self, outcome: &FfiLayoutTreeBuildOutcome) {
        let stats = self.layout_tree_build_stats.get();
        self.layout_tree_build_stats.set(FfiLayoutTreeBuildStats {
            builds: stats.builds + 1,
            last_build_rebuilt_subtree_roots: outcome.rebuilt_subtree_root_count as u64,
            last_build_escaped_rebuild_roots: outcome.layout_tree_update_escaped_rebuild_roots,
        });
    }

    pub(crate) fn layout_tree_build_stats(&self) -> FfiLayoutTreeBuildStats {
        self.layout_tree_build_stats.get()
    }

    pub(crate) fn needs_full_layout_tree_update(&self) -> bool {
        self.needs_full_layout_tree_update.get()
    }

    /// Notes that a pass that could not ask the host resolved a container-relative length of `node` without its
    /// container.
    pub(crate) fn note_unresolved_container_lengths(&self, node: NodeSlotId) {
        let mut nodes = self.unresolved_container_lengths.borrow_mut();
        if nodes.last() != Some(&node) {
            nodes.push(node);
        }
    }

    /// Marks the nodes whose container-relative lengths a pass resolved without their container since this was asked
    /// last for a layout update, ancestors included, so that the next layout lays them out again rather than reusing
    /// what the pass laid out. Answers whether there were any.
    pub(crate) fn mark_unresolved_container_lengths_for_layout(&self) -> bool {
        let nodes = self.unresolved_container_lengths.take();
        for &node in &nodes {
            self.set_needs_layout_update(node, true);
        }
        !nodes.is_empty()
    }

    pub(crate) fn set_needs_full_layout_tree_update(&self, value: bool) {
        self.needs_full_layout_tree_update.set(value);
    }

    fn style_engine(&self) -> crate::css::style::StyleEngineHandle {
        let style_engine = self.style_engine.get();
        assert!(!style_engine.is_null(), "layout node arena has no style engine");
        style_engine
    }

    /// Reads the style mirror. As with `with_style_engine`, no host callback runs while the borrow
    /// is active.
    /// Replace what one tree scope registers. C++ rebuilds a scope's counter styles whole, so the
    /// publication does too.
    pub(crate) fn publish_counter_styles(
        &self,
        tree_scope: u32,
        scope: crate::css::counter_representation::CounterStyleScope,
    ) {
        self.counter_styles.borrow_mut().publish_scope(tree_scope, scope);
    }

    pub(crate) fn counters_sets(&self) -> &RefCell<super::counters::CountersSets> {
        &self.style_node_tables.counters_sets
    }

    pub(crate) fn generated_content(&self) -> &RefCell<super::generated_content::GeneratedContent> {
        &self.style_node_tables.generated_content
    }

    pub(crate) fn svg_attribute_facts(&self, id: NodeSlotId) -> super::svg_formatting_context::FfiSvgAttributeFacts {
        match self.node_style_node(id) {
            Some(style_node) => self.style_node_svg_attribute_facts(style_node),
            None => Default::default(),
        }
    }

    /// The parsed SVG attributes an element published, named by its style node. An element
    /// the document never published for, anything that is not an SVG element, answers with the
    /// default facts.
    pub(crate) fn style_node_svg_attribute_facts(
        &self,
        style_node: StyleNodeID,
    ) -> super::svg_formatting_context::FfiSvgAttributeFacts {
        self.style_node_tables
            .svg_attribute_facts
            .borrow()
            .get(&style_node)
            .copied()
            .unwrap_or_default()
    }

    /// The `points` list a <polyline> or <polygon> parsed, shared rather than borrowed: a
    /// publication can replace it while a reader still uses it.
    pub(crate) fn svg_points(
        &self,
        id: NodeSlotId,
    ) -> Option<std::sync::Arc<[super::svg_formatting_context::FfiFloatPoint]>> {
        self.style_node_svg_points(self.node_style_node(id)?)
    }

    pub(crate) fn style_node_svg_points(
        &self,
        style_node: StyleNodeID,
    ) -> Option<std::sync::Arc<[super::svg_formatting_context::FfiFloatPoint]>> {
        self.style_node_tables.svg_points.borrow().get(&style_node).cloned()
    }

    /// The element an SVG reference resolves to, named by the atom its URL fragment interned to.
    /// `SVGGraphicsElement::resolve_fragment_identifier_to_element` asks the document first and the
    /// shadow tree the referring element sits in second, so the lookup is made in that order.
    pub(crate) fn element_by_svg_reference(&self, referrer: StyleNodeID, name: u32) -> Option<StyleNodeID> {
        let name = crate::css::style::index::StyleAtomID(name);
        if name.is_none() {
            return None;
        }
        self.with_style_store(|engine| {
            let scope = engine.tree().tree_scope(referrer);
            engine
                .element_by_id(crate::css::style::tree::TreeScopeID::DOCUMENT, name)
                .or_else(|| {
                    (scope != crate::css::style::tree::TreeScopeID::DOCUMENT)
                        .then(|| engine.element_by_id(scope, name))
                        .flatten()
                })
        })
    }

    /// The published computed style of an element the style tree names, which a box's own style
    /// pointer cannot reach: a `<defs>` builds no box, so nothing under it has a row.
    ///
    /// The group pointers are copied out rather than borrowed, since the style store's borrow ends
    /// with the query while the record they address is retained for the pass.
    pub(crate) fn style_node_style_payloads(&self, style_node: StyleNodeID) -> Option<FfiStylePayloads> {
        self.with_style_store(|engine| {
            let groups = engine.element_published_style_payloads(style_node)?;
            let mut payloads = FfiStylePayloads::default();
            payloads.groups.copy_from_slice(groups);
            Some(payloads)
        })
    }

    pub(crate) fn set_style_node_svg_attribute_facts(
        &self,
        style_node: StyleNodeID,
        facts: super::svg_formatting_context::FfiSvgAttributeFacts,
        points: &[super::svg_formatting_context::FfiFloatPoint],
    ) {
        let retained = Self::published_reference_atoms(&facts);
        let replaced = self
            .style_node_tables
            .svg_attribute_facts
            .borrow_mut()
            .insert(style_node, facts)
            .map_or([0; PUBLISHED_REFERENCE_ATOM_COUNT], |published| {
                Self::published_reference_atoms(&published)
            });
        self.retain_published_reference_atoms(retained, replaced);
        let mut published_points = self.style_node_tables.svg_points.borrow_mut();
        if points.is_empty() {
            published_points.remove(&style_node);
        } else {
            published_points.insert(style_node, points.into());
        }
    }

    /// Replace only the four names a graphics element's style carries. An element that has not
    /// published its attributes yet has no place to put them, and will carry them itself when it
    /// does: the publication is made when the style tree names the element.
    pub(crate) fn set_style_node_svg_style_references(&self, style_node: StyleNodeID, references: [u32; 4]) {
        let mut published = self.style_node_tables.svg_attribute_facts.borrow_mut();
        let Some(facts) = published.get_mut(&style_node) else {
            return;
        };
        let replaced = Self::published_reference_atoms(facts);
        [
            facts.mask_reference_atom,
            facts.clip_path_reference_atom,
            facts.fill_reference_atom,
            facts.stroke_reference_atom,
        ] = references;
        let retained = Self::published_reference_atoms(facts);
        drop(published);
        self.retain_published_reference_atoms(retained, replaced);
    }

    /// The element the document's id index holds for the atom `name`, which is what
    /// `Document::get_element_by_id` answers with. A reference that resolves in the document scope
    /// alone, an SVG `href` chain, asks for this rather than for `element_by_svg_reference`.
    pub(crate) fn element_by_document_id(&self, name: u32) -> Option<StyleNodeID> {
        let name = crate::css::style::index::StyleAtomID(name);
        if name.is_none() {
            return None;
        }
        self.with_style_store(|engine| engine.element_by_id(crate::css::style::tree::TreeScopeID::DOCUMENT, name))
    }

    /// Whether the element has an element child, which is what `childElementCount` counts.
    pub(crate) fn has_dom_element_children(&self, style_node: StyleNodeID) -> bool {
        style_node.element_index().is_some()
            && self.with_style_store(|engine| engine.tree().first_element_child(style_node).is_some())
    }

    /// The names a publication holds a sweep retention on: the one an `href` names, and the four
    /// a graphics element's style names.
    fn published_reference_atoms(
        facts: &super::svg_formatting_context::FfiSvgAttributeFacts,
    ) -> [u32; PUBLISHED_REFERENCE_ATOM_COUNT] {
        [
            facts.reference_fragment_atom,
            facts.mask_reference_atom,
            facts.clip_path_reference_atom,
            facts.fill_reference_atom,
            facts.stroke_reference_atom,
        ]
    }

    /// Hand the retention a publication's SVG references hold from the names they used to carry to
    /// the names they carry now, so the style engine's atom sweep cannot reissue either number
    /// while a publication still reads it. The atom an id names is otherwise rooted only by the
    /// element answering to it, and a reference to an id that is in no document has no such
    /// element.
    fn retain_published_reference_atoms(
        &self,
        retained: [u32; PUBLISHED_REFERENCE_ATOM_COUNT],
        released: [u32; PUBLISHED_REFERENCE_ATOM_COUNT],
    ) {
        if retained == released {
            return;
        }
        // A document being torn down drops its style record host before the last publication is
        // cleared. The engine it named is going with it, so there is nothing left to retain for.
        if !self.has_style_engine() {
            return;
        }
        // SAFETY: As with `with_style_store`, the engine outlives the arena's live nodes, and no
        // reader keeps a borrow of the engine across the host call this publication arrives in.
        let engine = unsafe { self.style_engine().get_mut() };
        for atom in retained {
            engine.retain_published_atom(crate::css::style::index::StyleAtomID(atom));
        }
        for atom in released {
            engine.release_published_atom(crate::css::style::index::StyleAtomID(atom));
        }
    }

    pub(crate) fn with_counter_style_registry<T>(
        &self,
        callback: impl FnOnce(&crate::css::counter_representation::CounterStyleRegistry) -> T,
    ) -> T {
        callback(&self.counter_styles.borrow())
    }

    pub(crate) fn with_style_store<T>(&self, query: impl FnOnce(&StyleEngine) -> T) -> T {
        query(unsafe { self.style_engine().get() })
    }

    /// The first child the style mirror's DOM child sequence holds for `style_node`, text nodes
    /// included. Only an element, a shadow root and the document own a sequence; a text node owns
    /// none. Nodes that can never have a box (a comment, a doctype, a processing instruction) hold
    /// no place in it.
    pub(crate) fn first_dom_child(&self, style_node: StyleNodeID) -> Option<StyleNodeID> {
        self.with_style_store(|engine| engine.tree().dom_children(style_node).next())
    }

    /// Everything one step of the stale-subtree walk reads out of the style mirror, in one borrow.
    pub(crate) fn stale_walk_facts(&self, style_node: StyleNodeID) -> StaleWalkFacts {
        self.with_style_store(|engine| {
            let tree = engine.tree();
            let owns_children = style_node.text_index().is_none();
            StaleWalkFacts {
                rendered_in_top_layer: owns_children
                    && engine.element_adjustment_facts(style_node)
                        & crate::css::style::bridge::element_adjustment_fact::RENDERED_IN_TOP_LAYER
                        != 0,
                shadow_root: owns_children.then(|| tree.shadow_root_of(style_node)).flatten(),
                first_dom_child: owns_children.then(|| tree.dom_children(style_node).next()).flatten(),
                next_dom_sibling: tree.next_sibling_in_dom_order(style_node),
            }
        })
    }

    /// The shadow root the element `host` hosts, if any. The style mirror names a root the moment it
    /// is attached to a host in the document.
    pub(crate) fn shadow_root_of(&self, host: StyleNodeID) -> Option<StyleNodeID> {
        self.with_style_store(|engine| engine.tree().shadow_root_of(host))
    }

    /// The node after `style_node` in its parent's DOM child sequence.
    pub(crate) fn next_dom_sibling(&self, style_node: StyleNodeID) -> Option<StyleNodeID> {
        self.with_style_store(|engine| engine.tree().next_sibling_in_dom_order(style_node))
    }

    /// The element facts the style mirror holds for `style_node`, as
    /// `bridge::element_adjustment_fact` names them. A text node, an anonymous row and the document
    /// hold none, and answer zero.
    pub(crate) fn element_adjustment_facts(&self, style_node: Option<StyleNodeID>) -> u32 {
        style_node.map_or(0, |style_node| {
            self.with_style_store(|engine| engine.element_adjustment_facts(style_node))
        })
    }

    /// The box facts the element's published style record holds. A text node, an anonymous row and
    /// the document have no record and answer nothing.
    pub(crate) fn published_box_facts(&self, style_node: Option<StyleNodeID>) -> Option<PublishedBoxFacts> {
        let style_node = style_node?;
        self.with_style_store(|engine| engine.element_published_box_facts(style_node))
    }

    /// The record an element's box is built from, the one [`Self::stamp_published_style`] gives its
    /// row, with the box facts the record holds. A text node, an anonymous row and the document have
    /// no record and answer nothing.
    pub(crate) fn element_box_style_record(&self, style_node: Option<StyleNodeID>) -> Option<(u64, PublishedBoxFacts)> {
        let style_node = style_node?;
        self.with_style_store(|engine| {
            let record = engine.element_published_style_record(style_node)?;
            Some((record, engine.style_record_box_facts(record)?))
        })
    }

    /// Whether the element's published style record holds a `::first-letter`. A text node, an
    /// anonymous row that names no element and the document have no record and answer no.
    pub(crate) fn has_published_first_letter_style(&self, style_node: Option<StyleNodeID>) -> bool {
        style_node.is_some_and(|style_node| {
            self.with_style_store(|engine| engine.has_published_first_letter_style(style_node))
        })
    }

    /// What the element a row is built for gives the natural size of its replaced content, as the
    /// style mirror publishes it, and nothing for an anonymous row, which stands for no element. A
    /// row that outlived its element has no answer.
    pub(crate) fn replaced_content_input(&self, id: NodeSlotId) -> Option<crate::css::style::ReplacedContentInput> {
        if super::node_facts::has_flag(self.data(id), NodeFlag::Anonymous) {
            return Some(crate::css::style::ReplacedContentInput::None);
        }
        let style_node = self.node_style_node(id)?;
        Some(self.with_style_store(|engine| engine.element_replaced_content_input(style_node)))
    }

    /// Which principal box the element asks for, as the style mirror publishes it.
    pub(crate) fn element_box_kind(&self, style_node: Option<StyleNodeID>) -> ElementBoxKind {
        style_node.map_or(ElementBoxKind::FromDisplay, |style_node| {
            self.with_style_store(|engine| engine.element_box_kind(style_node))
        })
    }

    /// Whether the element's published style record replaces its contents with a single image,
    /// which is what makes its box a replaced box rather than a container for its children.
    pub(crate) fn published_content_is_single_image(&self, style_node: Option<StyleNodeID>) -> bool {
        style_node.is_some_and(|style_node| {
            self.with_style_store(|engine| engine.element_content_is_single_image(style_node))
        })
    }

    /// The node above `style_node` that DOM code calls its flat-tree parent: the slot it is
    /// assigned to, else its parent, where a shadow root stands for its host. The document is not
    /// in the element relations, so the document element has none.
    pub(crate) fn flat_tree_parent(&self, style_node: StyleNodeID) -> Option<StyleNodeID> {
        self.with_style_store(|engine| {
            let tree = engine.tree();
            if let Some(slot) = tree.assigned_slot_of(style_node) {
                return Some(slot);
            }
            let parent = tree.parent(style_node)?;
            Some(tree.host_of(parent).unwrap_or(parent))
        })
    }

    /// Whether the text node's data is nothing but ASCII whitespace. Anything that is not a text
    /// node has no data and answers no.
    pub(crate) fn text_is_ascii_whitespace(&self, style_node: Option<StyleNodeID>) -> bool {
        style_node.is_some_and(|style_node| self.with_style_store(|engine| engine.text_is_ascii_whitespace(style_node)))
    }

    /// What the element above the text node in the flat tree publishes, in one borrow.
    pub(crate) fn text_style_parent_facts(&self, style_node: Option<StyleNodeID>) -> TextStyleParentFacts {
        style_node.map_or_else(TextStyleParentFacts::default, |style_node| {
            self.with_style_store(|engine| engine.text_style_parent_facts(style_node))
        })
    }

    /// How many nodes the style mirror holds assigned to the slot `style_node` names. Anything that
    /// is not a slot with assigned nodes answers zero.
    pub(crate) fn assigned_node_count(&self, style_node: Option<StyleNodeID>) -> usize {
        style_node.map_or(0, |style_node| {
            self.with_style_store(|engine| engine.tree().assigned_nodes_of(style_node).len())
        })
    }

    /// The node assigned to the slot `style_node` names at `index`, in flat-tree order.
    pub(crate) fn assigned_node_at(&self, style_node: StyleNodeID, index: usize) -> StyleNodeID {
        self.with_style_store(|engine| engine.tree().assigned_nodes_of(style_node)[index])
    }

    /// The document's top layer member at `index`, in the order the members were added, or nothing
    /// past the last one.
    pub(crate) fn top_layer_element(&self, index: usize) -> Option<StyleNodeID> {
        self.with_style_store(|engine| engine.tree().top_layer().get(index).copied())
    }

    /// Whether a style engine hosts this arena's records; a layout test's arena has none.
    pub(crate) fn has_style_engine(&self) -> bool {
        !self.style_engine.get().is_null()
    }

    // The engine outlives the arena's live nodes. No host callback runs while this
    // native style-store borrow is active; shell notifications follow publication.
    pub(crate) fn with_style_engine<T>(&self, callback: impl FnOnce(&mut StyleEngine) -> T) -> T {
        unsafe { callback(self.style_engine().get_mut()) }
    }

    pub(crate) fn derive_anonymous_style_record(
        &self,
        parent: u64,
        kind: AnonymousStyleKind,
        overrides: AnonymousStyleOverrides,
    ) -> DerivedStyleRecord {
        self.with_style_engine(|engine| LayoutStyle::anonymous(engine, parent, kind, overrides).intern(engine))
    }

    /// `record` with its display replaced by `display`, pinned for a row of the arena.
    pub(crate) fn derive_style_record_with_display(
        &self,
        record: u64,
        display: crate::layout::FfiDisplay,
    ) -> DerivedStyleRecord {
        self.with_style_engine(|engine| {
            let mut style = LayoutStyle::from_record(engine, record);
            style.set_display(display);
            if style.is_unchanged() {
                return DerivedStyleRecord::pin(engine, record);
            }
            style.intern(engine)
        })
    }

    pub(crate) fn reinherit_anonymous_style_record(&self, record: u64, parent: u64) -> DerivedStyleRecord {
        self.with_style_engine(|engine| {
            let mut style = LayoutStyle::from_record(engine, record);
            style.inherit_from(engine, parent);
            style.intern(engine)
        })
    }

    pub(crate) fn update_layout_style(
        &self,
        host_calls: HostCalls<'_>,
        node: NodeSlotId,
        update: impl FnOnce(&mut LayoutStyle),
    ) {
        let derived = self.with_style_engine(|engine| {
            let mut style = LayoutStyle::from_record(engine, self.node_style_record(node));
            update(&mut style);
            if style.is_unchanged() {
                return None;
            }
            Some(style.intern(engine))
        });
        if let Some(derived) = derived {
            self.set_node_flag(node, NodeFlag::FollowsPrincipalStyle, false);
            self.apply_reinherited_style_record(host_calls, node, derived);
        }
    }

    pub(crate) fn reset_table_box_style_used_by_wrapper(&self, host_calls: HostCalls<'_>, node: NodeSlotId) {
        self.update_layout_style(host_calls, node, LayoutStyle::reset_table_properties);
    }

    pub(crate) fn reinherit_anonymous_descendants(&self, host_calls: HostCalls<'_>, node: NodeSlotId) {
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
                AnonymousStyleKind::TableWrapper,
                AnonymousStyleOverrides::default(),
            );
            self.apply_reinherited_style_record(host_calls, parent, derived);
            self.reset_table_box_style_used_by_wrapper(host_calls, node);
        }
        self.reinherit_anonymous_children(host_calls, node, self.node_style_record(node));
    }

    fn reinherit_anonymous_children(&self, host_calls: HostCalls<'_>, parent: NodeSlotId, parent_style_record: u64) {
        let mut child = self.data(parent).first_child.get();
        while !child.is_invalid() {
            let next_sibling = self.data(child).next_sibling.get();
            let data = self.data(child);
            let flags = data.flags.get();
            let is_anonymous_styled_child = flags & NodeFlag::Anonymous as u32 != 0
                && flags & NodeFlag::HasStyle as u32 != 0
                && data.kind.get() != NodeKind::TableWrapper;
            if is_anonymous_styled_child && flags & NodeFlag::IsPseudoElementPrincipalBox as u32 == 0 {
                // Generated content with no layout-derived overrides follows its principal
                // pseudo's complete record. Anonymous wrappers inherit only inherited groups. The marker of a
                // list-item pseudo is generated for that pseudo but carries its own ::marker record.
                let follows_principal = data.generated_for.get() != 0
                    && data.kind.get() != NodeKind::ListItemMarkerBox
                    && (!self.node_style_record_is_derived(child)
                        || flags & NodeFlag::FollowsPrincipalStyle as u32 != 0)
                    && self.data(parent).flags.get() & NodeFlag::IsPseudoElementPrincipalBox as u32 != 0
                    && self.data(parent).generated_for.get() == data.generated_for.get();
                if follows_principal {
                    let derived = self.with_style_engine(|engine| DerivedStyleRecord::pin(engine, parent_style_record));
                    self.apply_reinherited_style_record(host_calls, child, derived);
                    self.set_node_flag(child, NodeFlag::FollowsPrincipalStyle, true);
                    self.reinherit_anonymous_descendants(host_calls, child);
                    self.notify_shell_of_style_change(host_calls, child, true);
                } else {
                    let derived =
                        self.reinherit_anonymous_style_record(self.node_style_record(child), parent_style_record);
                    self.apply_reinherited_style_record(host_calls, child, derived);
                    self.reinherit_anonymous_children(host_calls, child, derived.record);
                }
            }
            child = next_sibling;
        }
    }

    pub(crate) fn apply_reinherited_style_record(
        &self,
        host_calls: HostCalls<'_>,
        slot: NodeSlotId,
        derived: DerivedStyleRecord,
    ) {
        let previous_payloads = self.data(slot).style.get();
        let changes_layout_affecting_style =
            !style_payloads_equal_in_layout_affecting_groups(previous_payloads, derived.payloads);
        self.replace_arena_pinned_style_record(slot, derived);
        if changes_layout_affecting_style {
            self.bump_fragment_cache_epoch_of_self_and_ancestors(slot);
            self.reset_cached_intrinsic_sizes_of_self_and_ancestors(slot);
        }
        self.notify_shell_of_style_change(host_calls, slot, false);
    }

    fn notify_shell_of_style_change(&self, host_calls: HostCalls<'_>, slot: NodeSlotId, attach_resources: bool) {
        host_calls.shell_style_changed(slot, attach_resources);
    }

    pub(crate) fn continue_containing_block_search(
        &self,
        search: &mut super::abspos_inputs::ContainingBlockSearch,
        limit: NodeSlotId,
    ) {
        if !search.containing_block.is_invalid() {
            return;
        }
        let establishes_containing_block =
            super::node_facts::containing_block_establishment_flag(search.is_fixed_position);
        let looks_for_inline_containing_block = !search.is_fixed_position;
        let has_lifted_boxes = !self.inline_boxes_lifted_out_of.borrow().is_empty();
        let mut node = search.frontier;
        while node != limit {
            let ancestor = self.data(node).parent.get();
            if ancestor.is_invalid() {
                break;
            }
            if looks_for_inline_containing_block
                && has_lifted_boxes
                && search.inline_containing_block.is_invalid()
                && let Some(inline_box) = self.inline_box_lifted_out_of(node)
            {
                search.inline_containing_block = self.nearest_inline_containing_block_from(inline_box);
            }
            node = ancestor;
            let data = self.data(node);
            let kind = data.kind.get();
            if super::node_facts::kind_is_box(kind) {
                if super::node_facts::has_flag(data, establishes_containing_block) {
                    search.containing_block = node;
                    break;
                }
            } else if looks_for_inline_containing_block
                && search.inline_containing_block.is_invalid()
                && kind == NodeKind::InlineNode
                && super::node_facts::has_flag(data, NodeFlag::EstablishesAbsolutePositionContainingBlock)
            {
                search.inline_containing_block = node;
            }
        }
        search.frontier = node;
        if search.containing_block.is_invalid() && search.is_fixed_position && self.data(node).parent.get().is_invalid()
        {
            search.containing_block = node;
        }
    }

    fn nearest_inline_containing_block_from(&self, inline_box: NodeSlotId) -> NodeSlotId {
        let mut inline_ancestor = inline_box;
        while !inline_ancestor.is_invalid() {
            let data = self.data(inline_ancestor);
            if data.kind.get() != NodeKind::InlineNode {
                break;
            }
            if super::node_facts::has_flag(data, NodeFlag::EstablishesAbsolutePositionContainingBlock) {
                return inline_ancestor;
            }
            inline_ancestor = data.parent.get();
        }
        NodeSlotId::INVALID
    }

    fn derive_containing_block_establishment_flags(&self, node: NodeSlotId) {
        let data = self.write_shape(node);
        let previous_flags = data.flags.get();
        let (absolute, fixed) = if data.kind.get() == NodeKind::InlineNode {
            let absolute = !super::node_facts::has_flag(&*data, NodeFlag::Anonymous)
                && self
                    .node_style_if_live(node)
                    .is_some_and(crate::painting::style_queries::inline_establishes_absolute_position_containing_block);
            (absolute, false)
        } else {
            crate::painting::style_queries::establishes_positioning_containing_blocks(self, node)
        };
        let establishment_flags = NodeFlag::EstablishesAbsolutePositionContainingBlock as u32
            | NodeFlag::EstablishesFixedPositionContainingBlock as u32;
        let mut flags = previous_flags & !establishment_flags;
        if absolute {
            flags |= NodeFlag::EstablishesAbsolutePositionContainingBlock as u32;
        }
        if fixed {
            flags |= NodeFlag::EstablishesFixedPositionContainingBlock as u32;
        }
        data.set_flags(flags);

        let previous = previous_flags & establishment_flags;
        let current = flags & establishment_flags;
        let gained = current & !previous;
        let lost = previous & !current;
        let catches_escaping_boxes = gained != 0 && previous_flags & NodeFlag::AbsposDescendantEscapes as u32 != 0;
        let releases_contained_boxes = lost != 0
            && self
                .out_of_flow_positioning_contained
                .borrow()
                .get(&node)
                .is_some_and(|contained| contained & lost != 0);
        if !catches_escaping_boxes && !releases_contained_boxes {
            return;
        }
        self.set_needs_layout_update(node, true);
        self.scrollable_overflow.contained_boxes_dirty.set(true);
        if releases_contained_boxes {
            self.record_partial_relayout_escape();
        }
    }

    pub(crate) fn containing_block_by_walking_ancestors(&self, node: NodeSlotId) -> NodeSlotId {
        PaintRead::node_containing_block_if_live(self, node).unwrap_or(NodeSlotId::INVALID)
    }

    fn mark_nodes_escaped_by_out_of_flow_box(&self, node: NodeSlotId, containing_block: NodeSlotId) {
        if let Some(inline_box) = self.inline_box_lifted_out_of(node) {
            let mut inline_ancestor = inline_box;
            while !inline_ancestor.is_invalid() && self.data(inline_ancestor).kind.get() == NodeKind::InlineNode {
                self.set_node_flag(inline_ancestor, NodeFlag::AbsposDescendantEscapes, true);
                inline_ancestor = self.data(inline_ancestor).parent.get();
            }
        }
        let mut ancestor = self.data(node).parent.get();
        while !ancestor.is_invalid() && ancestor != containing_block {
            self.set_node_flag(ancestor, NodeFlag::AbsposDescendantEscapes, true);
            ancestor = self.data(ancestor).parent.get();
        }
    }

    fn mark_nodes_escaped_by_attached_out_of_flow_box(&self, node: NodeSlotId) {
        if !super::node_facts::kind_is_box(self.data(node).kind.get())
            || !self
                .node_style_if_live(node)
                .is_some_and(|style| style.is_absolutely_positioned())
        {
            return;
        }
        let containing_block = self.containing_block_by_walking_ancestors(node);
        self.mark_nodes_escaped_by_out_of_flow_box(node, containing_block);
    }

    pub(crate) fn forget_committed_out_of_flow_facts(&self, node: NodeSlotId) {
        let data = self.write_shape(node);
        data.set_flags(data.flags.get() & !(NodeFlag::AbsposDescendantEscapes as u32));
        let mut contained = self.out_of_flow_positioning_contained.borrow_mut();
        if !contained.is_empty() {
            contained.remove(&node);
        }
    }

    pub(crate) fn note_committed_out_of_flow_box(&self, node: NodeSlotId, inputs: &AbsposLayoutInputs) {
        let positioning = super::node_facts::containing_block_establishment_flag(
            crate::painting::style_queries::is_fixed_position(self, node),
        ) as u32;
        {
            let mut contained = self.out_of_flow_positioning_contained.borrow_mut();
            *contained.entry(inputs.containing_block).or_default() |= positioning;
            if !inputs.inline_containing_block.is_invalid() {
                *contained.entry(inputs.inline_containing_block).or_default() |=
                    NodeFlag::EstablishesAbsolutePositionContainingBlock as u32;
            }
        }
        self.mark_nodes_escaped_by_out_of_flow_box(node, inputs.containing_block);
    }

    fn derive_containing_block_establishment_flags_of_children(&self, parent: NodeSlotId) {
        self.for_each_node_in_layout_subtree_in_pre_order_with_pruning(parent, |node| {
            if node == parent {
                return true;
            }
            self.derive_containing_block_establishment_flags(node);
            super::node_facts::has_flag(self.data(node), NodeFlag::Anonymous)
        });
    }

    pub(crate) fn refresh_style_flags(&self, slot: NodeSlotId) {
        let style = self.node_style_if_live(slot).expect("styled layout node");
        let had_preserve_3d_transform_style =
            super::node_facts::has_flag(self.data(slot), NodeFlag::HasPreserve3dTransformStyle);
        self.set_node_flag(
            slot,
            NodeFlag::HasAnchorNames,
            !style.anchor().anchor_names.as_slice().is_empty(),
        );
        self.refresh_insets_use_anchor_functions_flag(slot);
        self.set_node_flag(slot, NodeFlag::HasAnimatedOpacityOrTransform, false);
        self.set_node_flag(
            slot,
            NodeFlag::HasPreserve3dTransformStyle,
            style.transform().transform_style == crate::css::css_enums::transform_style::PRESERVE_3D,
        );
        if !self.data(slot).parent.get().is_invalid() || self.data(slot).kind.get() == NodeKind::Viewport {
            self.derive_containing_block_establishment_flags(slot);
        }
        if had_preserve_3d_transform_style
            || super::node_facts::has_flag(self.data(slot), NodeFlag::HasPreserve3dTransformStyle)
        {
            self.derive_containing_block_establishment_flags_of_children(slot);
        }
        self.refresh_ancestor_facts_of_anonymous_children(slot);
    }

    pub(crate) fn enroll_text_node_for_content_sync(&self, node: NodeSlotId) {
        self.text_nodes_enrolled_for_content_sync.borrow_mut().insert(node);
    }

    /// Stamps a row the build allocated for a DOM node, with the facts the style mirror publishes
    /// under the node's identity. The document names no identity of its own, and its row is
    /// recognized by its kind. The layout node made for the row answers for the paint facts the row
    /// is built with, which are not published.
    pub(crate) fn stamp_dom_row(&self, slot: NodeSlotId, kind: NodeKind, style_node: Option<StyleNodeID>) {
        let data = self.write_shape(slot);
        assert_eq!(
            data.kind.get(),
            NodeKind::Unset,
            "stamped a row the build allocated onto a bound slot"
        );
        data.set_kind(kind);
        let construction_facts = style_node.map_or(0, |style_node| {
            self.with_style_store(|engine| engine.element_construction_facts(style_node))
        });
        data.set_flags(super::node_facts::construction_flags(kind, false, construction_facts));
        if let Some(style_node) = style_node {
            self.stamp_dom_paint_facts(slot, style_node);
            // The name the document knows the row's node by. The mirror publishes one for an
            // element; a text node's row answers for nothing, as its identity reads zero.
            let unique_node_id = self.with_style_store(|engine| engine.element_unique_node_id(style_node));
            self.unique_node_ids().publish(slot, unique_node_id);
            // The spans a table cell or column takes from its attributes, which table fixup reads
            // before the build is over.
            let spans = self.with_style_store(|engine| engine.element_table_spans(style_node));
            self.set_table_spans(slot, spans);
        }
        self.set_node_style_node(slot, style_node);
        self.refresh_has_scroll_offset_flag(slot);
        self.enroll_node_for_replaced_content_facts_sync_if_eligible(slot);
        // A text row renders what the mirror publishes for its node, synced before layout.
        if kind == NodeKind::TextNode {
            self.enroll_text_node_for_content_sync(slot);
        }
    }

    /// Gives a row stamped for an element the style record the element published, the one
    /// [`Self::element_box_style_record`] names, as a layout node built from the element's record
    /// gave it.
    pub(crate) fn stamp_published_style(&self, slot: NodeSlotId, record: u64) {
        let published = self.with_style_engine(|engine| DerivedStyleRecord::pin(engine, record));
        self.stamp_published_style_record(slot, published);
    }

    /// Gives a row being built the record its node published, pinned by the arena until the row
    /// takes another. The node hands the row its next record through the row's layout node, which
    /// is made when first asked for and reads the row's style as it is made, so the record must
    /// outlive a publication that replaces it before then.
    fn stamp_published_style_record(&self, slot: NodeSlotId, published: DerivedStyleRecord) {
        if self.set_node_style(slot, published.record, published.payloads) {
            self.refresh_style_flags(slot);
        }
        self.style_record_pins[slot.slot_index() as usize].set(ArenaStylePin::Published);
        self.enroll_node_for_svg_paint_resources_sync(slot);
    }

    /// Gives a row being built the paint facts the DOM published under `style_node`, the node the
    /// row is built for. The row is not committed yet, so this is a plain write rather than the
    /// change a live row's facts go through.
    pub(crate) fn stamp_dom_paint_facts(&self, slot: NodeSlotId, style_node: StyleNodeID) {
        let facts = self.with_style_store(|engine| engine.node_dom_paint_facts(style_node));
        self.write_shape(slot).set_dom_paint_facts(facts);
    }

    /// Whether the build about to run may build the viewport, which is what needs the document's
    /// style: there is no viewport row yet, the whole tree is to be rebuilt, or the document is.
    pub(crate) fn tree_build_may_create_viewport(&self, document_style_node: StyleNodeID) -> bool {
        self.bound_viewport_row().is_invalid()
            || self.needs_full_layout_tree_update()
            || self.needs_layout_tree_update(document_style_node)
    }

    /// Stamps a row the build allocated for a pseudo-element of `generator`, with the style record
    /// the element published for the pseudo-element. The row stands for no DOM node, so it is
    /// anonymous; it carries its generator and the pseudo-element it is generated for.
    pub(crate) fn stamp_pseudo_element_row(
        &self,
        slot: NodeSlotId,
        kind: NodeKind,
        generator: StyleNodeID,
        generated_for: u8,
    ) {
        let data = self.write_shape(slot);
        assert_eq!(
            data.kind.get(),
            NodeKind::Unset,
            "stamped a pseudo-element row onto a bound slot"
        );
        data.set_kind(kind);
        data.set_flags(super::node_facts::construction_flags(kind, true, 0));
        self.set_node_generated_for(slot, generated_for, Some(generator));
        let published = self.with_style_engine(|engine| {
            let record = engine
                .pseudo_published_style_record(generator, pseudo_kind_of(generated_for))
                .expect("a pseudo-element the build stamps a box for has published its style");
            DerivedStyleRecord::pin(engine, record)
        });
        self.stamp_published_style_record(slot, published);
    }

    /// Stamps a row the build allocated for a piece of generated text, which names no DOM node and
    /// carries no style of its own.
    pub(crate) fn stamp_generated_text_row(&mut self, slot: NodeSlotId, text: ak::Utf16String) {
        let data = self.write_shape(slot);
        assert_eq!(
            data.kind.get(),
            NodeKind::Unset,
            "stamped a generated text row onto a bound slot"
        );
        data.set_kind(NodeKind::GeneratedTextNode);
        data.set_flags(super::node_facts::construction_flags(
            NodeKind::GeneratedTextNode,
            true,
            0,
        ));
        self.set_generated_text(slot, text);
        self.enroll_text_node_for_content_sync(slot);
    }

    /// The pseudo-element of kind `generated_for` on `generator` gives up the box it holds, which
    /// is what a build does before it decides whether the pseudo-element gets one. The outgoing box
    /// keeps its style readable for as long as the host holds it.
    pub(crate) fn clear_pseudo_element_box(&self, generator: StyleNodeID, generated_for: u8) {
        let bound = self.bound_pseudo_element_row(generator, generated_for);
        if bound.is_invalid() {
            return;
        }
        self.pin_style_record_for_detachment(bound);
        self.set_node_flag(bound, NodeFlag::IsPseudoElementPrincipalBox, false);
        self.unbind_row(bound);
        // The outgoing box no longer holds the offset the pseudo-element has scrolled to.
        self.refresh_has_scroll_offset_flag(bound);
    }

    /// Makes `slot` the box of the pseudo-element of kind `generated_for` on `generator`, which
    /// gave up the box it held before.
    pub(crate) fn stamp_pseudo_element_box(&self, slot: NodeSlotId, generator: StyleNodeID, generated_for: u8) {
        self.set_node_generated_for(slot, generated_for, Some(generator));
        debug_assert!(self.bound_pseudo_element_row(generator, generated_for).is_invalid());
        self.set_node_flag(slot, NodeFlag::IsPseudoElementPrincipalBox, true);
        self.bind_row(slot);
        // The box starts holding the offset the pseudo-element has scrolled to as it becomes its box.
        self.refresh_has_scroll_offset_flag(slot);
    }

    pub(crate) fn stamp_anonymous_box(&self, slot: NodeSlotId, kind: NodeKind, derived: DerivedStyleRecord) {
        let data = self.write_shape(slot);
        assert_eq!(
            data.kind.get(),
            NodeKind::Unset,
            "stamped an anonymous box onto a bound slot"
        );
        assert!(derived.record != 0 && !derived.payloads.is_null());
        data.set_kind(kind);
        data.set_flags(super::node_facts::construction_flags(kind, true, 0));
        self.style_records[slot.slot_index() as usize].set(derived.record);
        self.style_record_pins[slot.slot_index() as usize].set(ArenaStylePin::Derived);
        data.set_style(derived.payloads);
        data.mark();
        self.note_row_style(slot);
        self.enroll_node_for_replaced_content_facts_sync_if_eligible(slot);
    }

    pub(crate) fn refresh_insets_use_anchor_functions_flag(&self, slot: NodeSlotId) {
        let insets_use_anchor_functions = self.style_payloads(slot).is_some_and(|payloads| {
            super::node_facts::style_insets_use_anchor_functions(ComputedValuesView::new(&payloads.groups))
        });
        self.set_node_flag(slot, NodeFlag::InsetsUseAnchorFunctions, insets_use_anchor_functions);
    }

    pub(crate) fn set_box_presence_host(&self, host: Option<BoxPresenceHost>) {
        self.box_presence_host.set(host);
    }

    /// What boxes the node bound to `row` has. An invalid row means the node has none.
    fn box_presence_bits(&self, row: NodeSlotId) -> u8 {
        if row.is_invalid() {
            return 0;
        }
        let mut bits = BOX_PRESENCE_HAS_LAYOUT_BOX;
        if self.paintable_rows().paintable_row_is_populated(row) {
            bits |= BOX_PRESENCE_HAS_COMMITTED_BOX;
        }
        bits
    }

    /// Tells the host what boxes `node` has now. No row list may be borrowed here. A
    /// pseudo-element's boxes stay unmirrored, since nothing on the DOM side reads them as a bit.
    fn notify_box_presence(&self, node: BoundNode) {
        if let BoundNode::Identity(style_node) = node {
            self.gather_layout_style_snapshot_box_loss(style_node);
        }
        if self.box_presence_host.get().is_none() {
            return;
        }
        if let Some(queued) = self.queued_box_presence.borrow_mut().as_mut() {
            queued.push(node);
            return;
        }
        self.tell_host_of_box_presence(node);
    }

    fn tell_host_of_box_presence(&self, node: BoundNode) {
        let Some(BoxPresenceHost(context, callback)) = self.box_presence_host.get() else {
            return;
        };
        let Some((style_node, bits)) = self.box_presence_of(node) else {
            return;
        };
        // SAFETY: Registration and unregistration keep the host context live, and the host does
        // not reenter the arena.
        unsafe { callback(context, style_node, bits) };
    }

    /// The identity of `node`, 0 for the document, and what boxes it has, for a node whose boxes the host mirrors.
    fn box_presence_of(&self, node: BoundNode) -> Option<(u32, u8)> {
        let (style_node, row) = match node {
            BoundNode::Identity(style_node) => (style_node.raw(), self.bound_row(style_node)),
            BoundNode::Document => (0, self.bound_viewport_row()),
            BoundNode::PseudoElement(..) => return None,
        };
        Some((style_node, self.box_presence_bits(row)))
    }

    /// Holds back what the host hears of the boxes nodes gain and lose until the tree build or
    /// layout write that starts now is over, as it cannot call the host.
    pub(crate) fn queue_box_presence(&self) {
        let previous = self.queued_box_presence.replace(Some(Vec::new()));
        assert!(previous.is_none(), "box presence is queued for one change at a time");
    }

    /// What the host is to hear of the boxes the nodes the finished change changed have now. A node it unbound and
    /// bound again is told once per change, each time with its final state.
    pub(crate) fn take_queued_box_presence(&self) -> DueBoxPresence {
        let queued = self.queued_box_presence.take().expect("a change queued box presence");
        let Some(host) = self.box_presence_host.get() else {
            return DueBoxPresence::default();
        };
        let nodes = queued
            .into_iter()
            .filter_map(|node| self.box_presence_of(node))
            .collect();
        DueBoxPresence {
            host: Some(host),
            nodes,
        }
    }

    /// Tells the host what boxes the node `row` can be bound to has now, after `row` gained or lost
    /// its committed box.
    pub(crate) fn notify_committed_box_changed(&self, row: NodeSlotId) {
        if let Some(node) = self.bound_node_of(row) {
            self.notify_box_presence(node);
        }
    }

    pub(crate) fn replace_arena_pinned_style_record(&self, slot: NodeSlotId, derived: DerivedStyleRecord) {
        let previously_pinned =
            self.style_record_pins[slot.slot_index() as usize].replace(ArenaStylePin::Derived) != ArenaStylePin::None;
        assert!(derived.record != 0 && !derived.payloads.is_null());
        let previous_style_record = self.style_records[slot.slot_index() as usize].replace(derived.record);
        self.shape_writes.note_style();
        let shape = self.write_shape(slot);
        shape.set_style(derived.payloads);
        shape.mark();
        self.note_row_style(slot);
        self.refresh_style_flags(slot);
        self.invalidate_overflow_after_style_change(slot);
        self.enroll_text_children_for_content_sync(slot);
        self.enroll_node_for_replaced_content_facts_sync_if_eligible(slot);
        self.enroll_node_for_svg_paint_resources_sync(slot);
        if previously_pinned {
            self.with_style_engine(|engine| engine.unpin_layout_style_record(previous_style_record));
        }
    }

    pub(crate) fn replaced_paint_facts(
        &self,
        id: NodeSlotId,
    ) -> Option<crate::painting::replaced_paint_facts::ReplacedPaintFacts> {
        self.replaced_paint_facts.borrow().get(&id).cloned()
    }

    pub(crate) fn layer_image_paint_facts(
        &self,
        id: NodeSlotId,
        list: crate::painting::host::FfiLayerImageList,
        computed_index: u32,
    ) -> Option<crate::painting::layer_image_paint_facts::LayerImagePaintFacts> {
        crate::painting::layer_image_paint_facts::layer_image_paint_facts_in(
            &self.layer_image_paint_facts.borrow(),
            id,
            list,
            computed_index,
        )
    }

    pub(crate) fn set_layer_image_paint_facts(
        &self,
        id: NodeSlotId,
        entries: Vec<crate::painting::layer_image_paint_facts::LayerImagePaintFactsEntry>,
    ) -> bool {
        if !self.slot_is_live(id) {
            return false;
        }
        let mut table = self.layer_image_paint_facts.borrow_mut();
        let changed = if entries.is_empty() {
            table.contains_key(&id)
                && Arc::make_mut(&mut table)
                    .remove(&id)
                    .is_some_and(|previous| !previous.is_empty())
        } else if table.get(&id) == Some(&entries) {
            false
        } else {
            Arc::make_mut(&mut table).insert(id, entries);
            true
        };
        drop(table);
        if changed {
            use crate::painting::record::damage::PaintDamage;
            self.push_paint_damage_for_repaint(
                id,
                PaintDamage::DRAW_BACKGROUND | PaintDamage::DRAW_BORDER | PaintDamage::SCOPE_PREAMBLE,
            );
        }
        changed
    }

    /// Writes `facts` onto the box `target` names, and every row sharing its DOM node, where the box paints facts of
    /// their kind. A box that owns the provider of its image shows that provider's image, which only facts naming the
    /// box by its row carry, and none before the host hands it the provider.
    pub(crate) fn set_replaced_paint_facts(
        &self,
        target: super::tree_update_marks::MarkedBox,
        facts: crate::painting::replaced_paint_facts::ReplacedPaintFacts,
    ) {
        use super::tree_update_marks::MarkedBox;
        use crate::painting::replaced_paint_facts::ReplacedPaintFacts;
        let id = target.row(self);
        let Some(kind) = self.node_kind_if_live(id) else {
            return;
        };
        let shows = match facts {
            ReplacedPaintFacts::FormControl(_) => matches!(kind, NodeKind::CheckBox | NodeKind::RadioButton),
            ReplacedPaintFacts::Canvas(_) => kind == NodeKind::CanvasBox,
            ReplacedPaintFacts::NavigableContainer(_) => kind == NodeKind::NavigableContainerViewport,
            ReplacedPaintFacts::Image(_) => {
                matches!(kind, NodeKind::ImageBox | NodeKind::SVGImageBox)
                    && match self.owned_image_provider(id) {
                        None => true,
                        Some(OwnedImageProvider::HandedOver) => matches!(target, MarkedBox::Row(_)),
                        Some(OwnedImageProvider::Awaited) => false,
                    }
            }
            ReplacedPaintFacts::Video(_) => kind == NodeKind::VideoBox,
        };
        if !shows {
            return;
        }
        let damage = facts.damage_when_changed();
        for row in self.rows_sharing_dom_node_with(id) {
            let mut table = self.replaced_paint_facts.borrow_mut();
            if table.get(&row) == Some(&facts) {
                continue;
            }
            Arc::make_mut(&mut table).insert(row, facts.clone());
            drop(table);
            self.push_paint_damage_for_repaint(row, damage);
        }
    }

    /// How far the rows the host reads have been written since the arena was made: rows published
    /// at one version read as the arena does for as long as it stays that version. Every count it
    /// sums only grows, and a table a publication shares is copied by the write that changes it.
    pub(crate) fn rows_version(&self) -> RowsVersion {
        RowsVersion {
            writes: self.shape_writes.all()
                + self.text_slots.published.version()
                + self.bound_rows.borrow().version()
                + self.paintable_rows_version(),
            identity: self.rows_identity_version(),
            style: self.shape_writes.style(),
            population: self.paintable_population_version(),
            tables: [
                Arc::as_ptr(&self.replaced_paint_facts.borrow()).addr(),
                Arc::as_ptr(&self.layer_image_paint_facts.borrow()).addr(),
                self.svg_paint_resources.address(),
                self.image_map_areas().address(),
                self.paint_state
                    .borrow()
                    .visual_context
                    .tree
                    .as_ref()
                    .map_or(0, |tree| Arc::as_ptr(tree).addr()),
            ],
        }
    }

    /// How far what each row is (see [`ShapeWrites::identity`]) and the row each node is bound to have been written
    /// since the arena was made. Installing a style leaves it as it is, so rows published before one still answer
    /// these as the arena does.
    pub(crate) fn rows_identity_version(&self) -> u64 {
        self.shape_writes.identity() + self.bound_rows.borrow().version()
    }

    /// The text rows and the replaced, layer image and SVG paint resource tables as they are now,
    /// for a recording to read. Each is shared until the arena next writes it.
    pub(crate) fn publish_paint_facts(&mut self) -> crate::painting::published_frame::PublishedPaintFacts {
        crate::painting::published_frame::PublishedPaintFacts {
            text: self.text_slots.published.publish(),
            replaced: self.replaced_paint_facts.borrow().clone(),
            layer_images: self.layer_image_paint_facts.borrow().clone(),
            svg_paint_resources: self.svg_paint_resources.publish(),
        }
    }

    pub(crate) fn node_has_dom_paint_fact(&self, id: NodeSlotId, fact: DomPaintFact) -> bool {
        self.data(id).dom_paint_facts.get() & fact as u8 != 0
    }

    pub(crate) fn set_node_dom_paint_facts(&self, id: NodeSlotId, facts: u8) -> bool {
        let mut any_changed = false;
        for row in self.rows_sharing_dom_node_with(id) {
            let data = self.write_shape(row);
            if data.dom_paint_facts.get() == facts {
                continue;
            }
            data.set_dom_paint_facts(facts);
            any_changed = true;
            use crate::painting::record::damage::PaintDamage;
            self.push_paint_damage_for_repaint(row, PaintDamage::ALL_HIT | PaintDamage::SCROLL_METADATA);
        }
        any_changed
    }

    pub(crate) fn note_rows_share_dom_node(&self, bound_row: NodeSlotId, added_row: NodeSlotId) {
        assert!(
            self.node_is_dom_backed(added_row),
            "layout node arena shared rows of an anonymous node"
        );
        assert!(
            self.bound_node_of(added_row) == self.bound_node_of(bound_row),
            "layout node arena shared rows of different DOM nodes"
        );
        let added_link = &self.next_rows_built_for_same_node[added_row.slot_index() as usize];
        assert!(
            added_link.get().is_invalid(),
            "layout node arena shared a row that already shares a DOM node"
        );
        // The added row goes in behind the bound row's predecessor, at the end of the ring as seen from the bound row, so
        // adding a row does not change which row takes the binding over when the bound row is freed.
        let mut predecessor = bound_row;
        loop {
            let next = self.next_rows_built_for_same_node[predecessor.slot_index() as usize].get();
            if next.is_invalid() || next == bound_row {
                break;
            }
            predecessor = next;
        }
        added_link.set(bound_row);
        self.next_rows_built_for_same_node[predecessor.slot_index() as usize].set(added_row);
    }

    /// The rows built for the same DOM node as `id`, starting with `id`.
    pub(crate) fn rows_sharing_dom_node_with(&self, id: NodeSlotId) -> impl Iterator<Item = NodeSlotId> + '_ {
        let mut row = Some(id);
        std::iter::from_fn(move || {
            let current = row?;
            let next = self.next_rows_built_for_same_node[current.slot_index() as usize].get();
            row = (!next.is_invalid() && next != id).then_some(next);
            Some(current)
        })
    }

    fn forget_row_sharing_dom_node(&mut self, id: NodeSlotId) {
        let successor = self.next_rows_built_for_same_node[id.slot_index() as usize].replace(NodeSlotId::INVALID);
        if successor.is_invalid() {
            return;
        }
        // Close the ring behind the row that is leaving.
        let mut predecessor = successor;
        loop {
            let link = &self.next_rows_built_for_same_node[predecessor.slot_index() as usize];
            if link.get() == id {
                link.set(if predecessor == successor {
                    NodeSlotId::INVALID
                } else {
                    successor
                });
                break;
            }
            predecessor = link.get();
        }
        // The node stays bound to one of the rows that still share it.
        let Some(node) = self.bound_node_of(id) else {
            return;
        };
        if self.bound_node_of(successor) != Some(node) {
            return;
        }
        self.replace_bound_row(node, id, successor);
    }

    pub(crate) fn set_node_flag(&self, id: NodeSlotId, flag: NodeFlag, value: bool) {
        let data = self.write_shape(id);
        let previous = data.flags.get();
        let mut updated = previous;
        if value {
            updated |= flag as u32;
        } else {
            updated &= !(flag as u32);
        }
        if value
            && matches!(flag, NodeFlag::NeedsLayoutUpdate | NodeFlag::NeedsOwnGeometryUpdate)
            && data.flags.get() & (NodeFlag::NeedsLayoutUpdate as u32 | NodeFlag::NeedsOwnGeometryUpdate as u32) == 0
        {
            let mut nodes = self.nodes_with_layout_update_flags.borrow_mut();
            let previous = self
                .layout_update_flag_node_indices
                .borrow_mut()
                .insert(id, nodes.len());
            debug_assert!(previous.is_none());
            nodes.push(id);
        } else if !value
            && matches!(flag, NodeFlag::NeedsLayoutUpdate | NodeFlag::NeedsOwnGeometryUpdate)
            && updated & (NodeFlag::NeedsLayoutUpdate as u32 | NodeFlag::NeedsOwnGeometryUpdate as u32) == 0
        {
            self.remove_layout_update_flag_node(id);
        }
        data.set_flags(updated);
        // Retaining compositor-animated content decides whether a non-invertible transform
        // still records its stacking context.
        if flag == NodeFlag::HasAnimatedOpacityOrTransform && updated != previous {
            self.push_paint_damage(id, crate::painting::record::damage::PaintDamage::ELIGIBILITY);
        }
        // A commit leaves an ordinary inline's overflow unmeasured, since nothing reads it. Once the
        // inline stores a scroll offset, its overflow clamps that offset, so the pass measures it.
        if flag == NodeFlag::HasScrollOffset
            && value
            && updated != previous
            && self.paintable_row_is_populated(id)
            && !self.paintable_side_data(id).overflow_measured_this_commit.get()
        {
            self.note_row_overflow_unmeasured(id);
        }
    }

    pub(crate) fn set_node_needs_compositor_animation_frame(
        &self,
        id: NodeSlotId,
        kind: super::node_data::CompositorAnimationFrameKind,
        value: bool,
    ) {
        let data = self.write_shape(id);
        let mut updated = data.compositor_animation_frame_kinds.get();
        if value {
            updated |= kind as u8;
        } else {
            updated &= !(kind as u8);
        }
        data.set_compositor_animation_frame_kinds(updated);
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

    fn remove_layout_update_flag_node(&self, node: NodeSlotId) {
        let mut indices = self.layout_update_flag_node_indices.borrow_mut();
        let Some(index) = indices.remove(&node) else {
            return;
        };
        let mut nodes = self.nodes_with_layout_update_flags.borrow_mut();
        nodes.swap_remove(index);
        if let Some(&moved_node) = nodes.get(index) {
            *indices.get_mut(&moved_node).unwrap() = index;
        }
    }

    pub(crate) fn reset_layout_update_flags_in_subtree(&self, root: NodeSlotId) {
        let flags_to_clear = NodeFlag::NeedsLayoutUpdate as u32 | NodeFlag::NeedsOwnGeometryUpdate as u32;
        if self.data(root).kind.get() != NodeKind::Viewport {
            // NB: A partial-relayout batch commits each independent boundary separately.
            // Scanning the document's dirty list per boundary would make cleanup quadratic.
            self.for_each_node_in_layout_subtree_in_pre_order(root, |node| {
                let data = self.write_shape(node);
                data.set_flags(data.flags.get() & !flags_to_clear);
                self.remove_layout_update_flag_node(node);
            });
            return;
        }

        // NB: Dirty nodes share ancestor chains. Cache membership so a deeply nested
        // dirty chain is checked once, while detached dirty subtrees remain pending.
        let mut membership = HashMap::default();
        membership.insert(root, true);
        membership.insert(NodeSlotId::INVALID, false);
        let mut ancestors = Vec::new();
        let mut index = 0;
        loop {
            let Some(node) = self.nodes_with_layout_update_flags.borrow().get(index).copied() else {
                break;
            };
            let mut ancestor = node;
            while !membership.contains_key(&ancestor) {
                ancestors.push(ancestor);
                ancestor = self.data(ancestor).parent.get();
            }
            let is_in_subtree = membership[&ancestor];
            for ancestor in ancestors.drain(..) {
                membership.insert(ancestor, is_in_subtree);
            }
            if !is_in_subtree {
                index += 1;
                continue;
            }
            let data = self.write_shape(node);
            data.set_flags(data.flags.get() & !flags_to_clear);
            self.remove_layout_update_flag_node(node);
        }
    }

    /// Returns whether the node's ancestor facts changed.
    fn derive_ancestor_facts_for_node(&self, node: NodeSlotId) -> bool {
        let data = self.data(node);
        let parent = data.parent.get();
        let mut facts = 0;
        if !parent.is_invalid() {
            let parent_data = self.data(parent);
            let parent_style = super::node_facts::node_style_view(parent_data);
            if super::node_facts::node_is_flex_or_grid_container(parent_style) {
                facts |= AncestorFact::ParentIsFlexOrGridContainer as u8;
            }
            if parent_style.is_none_or(|style| {
                !style.is_floating() && (style.display().is_flow_inside() || style.display().is_flow_root_inside())
            }) {
                facts |= AncestorFact::ParentIsUnfloatedFlowContainer as u8;
            }
            if super::node_facts::has_flag(data, NodeFlag::Anonymous) {
                if super::node_facts::has_flag(parent_data, NodeFlag::UsesButtonLayout) {
                    facts |= AncestorFact::IsAnonymousButtonContentWrapper as u8;
                }
                if super::node_facts::has_ancestor_fact(parent_data, AncestorFact::IsAnonymousButtonContentWrapper) {
                    facts |= AncestorFact::IsAnonymousButtonContentBox as u8;
                }
                let inherits_text_overflow_ellipsis = if super::node_facts::has_flag(parent_data, NodeFlag::Anonymous) {
                    super::node_facts::has_ancestor_fact(parent_data, AncestorFact::InheritsTextOverflowEllipsis)
                } else {
                    super::node_facts::node_applies_text_overflow_ellipsis(parent_style)
                };
                if inherits_text_overflow_ellipsis {
                    facts |= AncestorFact::InheritsTextOverflowEllipsis as u8;
                }
            }
            if super::node_facts::has_ancestor_fact(parent_data, AncestorFact::HasInlineLevelInclusiveAncestor) {
                facts |= AncestorFact::HasInlineLevelInclusiveAncestor as u8;
            }
        }
        if super::node_facts::node_is_inline_outside(super::node_facts::node_style_view(data)) {
            facts |= AncestorFact::HasInlineLevelInclusiveAncestor as u8;
        }
        data.ancestor_facts.replace(facts) != facts
    }

    /// A style change reaches the anonymous boxes below the node without rebuilding them, and
    /// they take some of their ancestor facts from it.
    fn refresh_ancestor_facts_of_anonymous_children(&self, parent: NodeSlotId) {
        let mut child = self.data(parent).first_child.get();
        while !child.is_invalid() {
            let data = self.data(child);
            if super::node_facts::has_flag(data, NodeFlag::Anonymous) && self.derive_ancestor_facts_for_node(child) {
                self.bump_fragment_cache_epoch_of_self_and_ancestors(child);
                self.refresh_ancestor_facts_of_anonymous_children(child);
            }
            child = data.next_sibling.get();
        }
    }

    /// Returns every attached subtree root the derivation visited.
    pub(crate) fn derive_facts_after_tree_update(&self, rebuilt_roots: &[NodeSlotId]) -> HashSet<NodeSlotId> {
        // NB: Anonymous wrappers, generated content, and table fixup can attach nodes
        // outside the builder's reported rebuild roots. Include every attached subtree.
        let mut pending = self.pending_attached_subtree_roots.borrow_mut();
        let roots: HashSet<_> = pending
            .drain(..)
            .chain(rebuilt_roots.iter().copied())
            .filter(|&root| self.slot_is_live(root))
            .collect();
        drop(pending);
        for &root in &roots {
            let mut ancestor = self.data(root).parent.get();
            if ancestor.is_invalid() && self.data(root).kind.get() != NodeKind::Viewport {
                continue;
            }
            while !ancestor.is_invalid() && !roots.contains(&ancestor) {
                ancestor = self.data(ancestor).parent.get();
            }
            if ancestor.is_invalid() {
                self.derive_facts_in_subtree(root);
            }
        }
        roots
    }

    /// Derives what the nodes in the inclusive subtree of `root` take from their ancestors: whether they
    /// establish containing blocks, and their ancestor facts, which the pre-order walk finds already derived
    /// for the parent. Out-of-flow boxes in the subtree also mark the ancestors they escape.
    pub(crate) fn derive_facts_in_subtree(&self, root: NodeSlotId) {
        self.for_each_node_in_layout_subtree_in_pre_order(root, |node| {
            self.derive_containing_block_establishment_flags(node);
            self.mark_nodes_escaped_by_attached_out_of_flow_box(node);
            self.derive_ancestor_facts_for_node(node);
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

    pub(super) fn pre_order_label_of_subtree_successor(&self, node: NodeSlotId) -> u64 {
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
        // NB: Merely restoring strict order can leave the subtree dense enough to need another
        //     relabel on the next insertion. Restore the normal insertion spacing, or try a larger
        //     ancestor. The root already uses all available label space, so it cannot expand further.
        if step < MAXIMUM_PRE_ORDER_LABEL_STRIDE && !self.data(subtree_root).parent.get().is_invalid() {
            return false;
        }
        assert!(step >= 2, "pre-order label space exhausted");
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

    pub(crate) fn note_table_cell_measurement_cache_miss(&self) {
        self.table_cell_measurement_cache_misses
            .set(self.table_cell_measurement_cache_misses.get() + 1);
    }

    pub(crate) fn table_cell_measurement_cache_miss_count(&self) -> u64 {
        self.table_cell_measurement_cache_misses.get()
    }

    pub(crate) fn note_intrinsic_inline_measurement(&self) {
        self.intrinsic_inline_measurements
            .set(self.intrinsic_inline_measurements.get() + 1);
    }

    pub(crate) fn intrinsic_inline_measurement_count(&self) -> u64 {
        self.intrinsic_inline_measurements.get()
    }

    pub(crate) fn note_intrinsic_measurement(&self) {
        self.intrinsic_measurements.set(self.intrinsic_measurements.get() + 1);
    }

    pub(crate) fn intrinsic_measurement_count(&self) -> u64 {
        self.intrinsic_measurements.get()
    }

    pub(crate) fn saved_abspos_layout_inputs(&self, data: &NodeData) -> Option<AbsposLayoutInputs> {
        let (index, metadata) = self.slot_for_data(data);
        self.paintable_rows
            .with_committed_fragment_link(index, metadata.generation, |link| {
                link.and_then(|link| link.abspos_layout_inputs)
            })
            .or_else(|| {
                self.confined_abspos_layout_inputs
                    .borrow()
                    .get(&NodeSlotId::new(index, metadata.generation))
                    .copied()
            })
    }

    pub(crate) fn set_default_scroll_shift(
        &self,
        id: NodeSlotId,
        anchor: NodeSlotId,
        compensates_for_horizontal_scroll: bool,
        compensates_for_vertical_scroll: bool,
    ) {
        let anchor_is_live = self.slot_is_live(anchor);
        let anchor = if anchor_is_live { anchor } else { NodeSlotId::INVALID };
        let compensates_for_horizontal_scroll = anchor_is_live && compensates_for_horizontal_scroll;
        let compensates_for_vertical_scroll = anchor_is_live && compensates_for_vertical_scroll;
        let previous_flags = self.node_flags_if_live(id);
        let scroll_shift_inputs_changed = self.default_scroll_shift_anchor(id) != anchor
            || (previous_flags & NodeFlag::CompensatesForHorizontalScroll as u32 != 0)
                != compensates_for_horizontal_scroll
            || (previous_flags & NodeFlag::CompensatesForVerticalScroll as u32 != 0) != compensates_for_vertical_scroll;
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
            compensates_for_horizontal_scroll,
        );
        self.set_node_flag(
            id,
            NodeFlag::CompensatesForVerticalScroll,
            compensates_for_vertical_scroll,
        );
        if anchor_is_live {
            self.any_default_scroll_shift_anchor_ever_stored.set(true);
        }
        if scroll_shift_inputs_changed {
            self.note_visual_context_box_dirty(
                id,
                crate::painting::visual_context::dirty::VisualContextBoxDirtyKind::DefaultScrollShiftInputsChanged,
            );
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

    pub(crate) fn for_each_default_scroll_shift_anchor(&self, mut visit: impl FnMut(NodeSlotId, NodeSlotId)) {
        if !self.any_default_scroll_shift_anchor_ever_stored.get() {
            return;
        }
        let slots = self.default_scroll_shift_anchors.borrow();
        for (index, slot) in slots.iter().enumerate() {
            if slot.anchor.is_invalid() || slot.generation == 0 {
                continue;
            }
            let positioned = NodeSlotId::new(index as u32, slot.generation);
            if self.slot_is_live(positioned) && self.slot_is_live(slot.anchor) {
                visit(positioned, slot.anchor);
            }
        }
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

    pub(crate) fn set_committed_fragment_link(
        &self,
        data: &NodeData,
        link: super::fragment_tree::FragmentLink,
        geometry_epoch: Option<u32>,
    ) {
        let (index, metadata) = self.slot_for_data(data);
        self.paintable_rows
            .set_committed_fragment_link(index, metadata.generation, geometry_epoch, link);

        let data = self.write_shape_of(data);
        data.set_flags(data.flags.get() | NodeFlag::HasCommittedFragmentLink as u32);
    }

    pub(crate) fn epoch_of_geometry_laid_out_in_this_pass(&self, data: &NodeData) -> Option<u32> {
        (!self.fragment_cache_epoch_changed_during_layout_pass.get()).then(|| data.fragment_cache_epoch.get())
    }

    pub(crate) fn with_current_committed_fragment<R>(
        &self,
        node: NodeSlotId,
        read: impl FnOnce(&super::fragment_tree::Fragment) -> R,
    ) -> Option<R> {
        self.paintable_rows.with_current_committed_fragment(
            node.slot_index(),
            node.generation(),
            self.data(node).fragment_cache_epoch.get(),
            read,
        )
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
        let data = self.write_shape_of(data);
        data.set_flags(data.flags.get() & !(NodeFlag::HasCommittedFragmentLink as u32));
        link
    }

    pub(crate) fn clear_committed_fragment_link(&self, id: NodeSlotId) {
        // Cached runs may reuse the committed paintable subtree without replaying its fragments.
        // Once that subtree is cleared, a later layout must rebuild it instead.
        self.fc_run_cache_store.remove_entry(id.slot_index());
        drop(self.take_committed_fragment_link(self.data(id)));
    }

    fn text_node_state_mut(&mut self, id: NodeSlotId) -> TextStateMut<'_> {
        self.data(id);
        self.text_slots.state_mut(id)
    }

    fn text_node_state(&self, id: NodeSlotId) -> Option<&TextNodeState> {
        if !self.slot_is_live(id) {
            return None;
        }
        self.text_slots.state(id)
    }

    /// Record what a generated text row spells.
    pub(crate) fn set_generated_text(&mut self, id: NodeSlotId, text: ak::Utf16String) {
        self.text_node_state_mut(id).generated_text = Some(text);
    }

    /// Everything a text row renders from. A generated text row carries its own characters; a row
    /// bound to a DOM text node reads what the style mirror publishes for it.
    pub(crate) fn published_text_source(&self, id: NodeSlotId, uses_locale: bool) -> PublishedTextSource {
        if self.data(id).kind.get() == NodeKind::GeneratedTextNode {
            return PublishedTextSource {
                data: self
                    .text_node_state(id)
                    .and_then(|state| state.generated_text.clone())
                    .unwrap_or_default(),
                locale: uses_locale.then(|| self.generated_text_language_tag(id)).flatten(),
                is_password_input: false,
            };
        }
        let Some(style_node) = self
            .node_style_node(id)
            .filter(|style_node| style_node.text_index().is_some())
        else {
            return PublishedTextSource::default();
        };
        self.with_style_store(|engine| engine.published_text_source(style_node, uses_locale))
    }

    /// The language tag a generated text row's transform reads: the one the element the content
    /// was generated for resolves to. The row is either the pseudo-element's own box or a child of
    /// it; a generated row under any other box reads no tag.
    fn generated_text_language_tag(&self, id: NodeSlotId) -> Option<Vec<u16>> {
        let parent = self.data(id).parent.get();
        let generator = if self.node_is_generated_for_pseudo_element(id) {
            self.node_style_node(id)
        } else if self.node_is_generated_for_pseudo_element(parent) {
            self.node_style_node(parent)
        } else {
            None
        }?;
        self.with_style_store(|engine| {
            let tag = engine.element_language_tag(generator);
            (!tag.is_empty()).then(|| tag.to_vec())
        })
    }

    pub(crate) fn set_text_content(&mut self, id: NodeSlotId, content: TextContent) {
        let mut state = self.text_node_state_mut(id);
        if let Some(previous) = state.content.as_mut()
            && previous.has_same_content_as(&content)
        {
            previous.rendering_key = content.rendering_key;
            return;
        }
        state.content = Some(content);
        drop(state);
        self.searchable_text = None;
        // Publication can happen through a C++ text read before the enrolled
        // sync runs. Invalidate here so every publication invalidates layout,
        // including mapping-only changes with identical rendered code units.
        self.bump_fragment_cache_epoch_of_self_and_ancestors(id);
    }

    pub(crate) fn invalidate_text_content(&mut self, id: NodeSlotId) {
        self.data(id);
        if self.text_slots.state(id).is_some_and(|state| state.content.is_some())
            && let Some(content) = self.text_node_state_mut(id).content.as_mut()
        {
            content.rendering_key = None;
        }
        self.enroll_text_node_for_content_sync(id);
    }

    pub(super) fn finish_text_content_sync(&self, id: NodeSlotId) {
        self.text_nodes_enrolled_for_content_sync.borrow_mut().remove(&id);
    }

    pub(super) fn pending_text_nodes_for_content_sync(&self) -> Vec<NodeSlotId> {
        self.text_nodes_enrolled_for_content_sync
            .borrow()
            .iter()
            .copied()
            .collect()
    }

    /// The text rows that wait for their text to be rendered again, by slot index.
    pub(super) fn text_rows_awaiting_sync(&self) -> Box<[NodeSlotId]> {
        let mut rows: Box<[NodeSlotId]> = self
            .text_nodes_enrolled_for_content_sync
            .borrow()
            .iter()
            .copied()
            .collect();
        rows.sort_unstable_by_key(|row| row.index);
        rows
    }

    pub(super) fn text_content_needs_sync(&self, id: NodeSlotId) -> bool {
        self.text_nodes_enrolled_for_content_sync.borrow().contains(&id)
            || !self
                .text_content(id)
                .is_some_and(|content| content.rendering_key.is_some())
    }

    pub(crate) fn set_replaced_content_facts(&mut self, id: NodeSlotId, facts: FfiReplacedContentFacts) -> bool {
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

    pub(crate) fn invalidate_searchable_text(&mut self) {
        self.searchable_text = None;
    }

    /// Gives the row the spans its element published again, after one of the element's span
    /// attributes changed, and answers whether they moved.
    pub(crate) fn restamp_table_spans(&self, id: NodeSlotId) -> bool {
        let element = self
            .node_style_node(id)
            .expect("a row whose spans are restamped is built for an element");
        let spans = self.with_style_store(|engine| engine.element_table_spans(element));
        self.set_table_spans(id, spans)
    }

    /// Gives the row the spans its table cell or table column element has, and answers whether
    /// they changed.
    pub(crate) fn set_table_spans(&self, id: NodeSlotId, spans: TableSpans) -> bool {
        let data = self.data(id);
        let effective_spans_changed =
            data.table_column_span.get() != spans.column_span || data.table_row_span.get() != spans.row_span;
        data.table_column_span.set(spans.column_span);
        data.table_row_span.set(spans.row_span);
        let mut raw_spans = self.raw_table_column_spans.borrow_mut();
        let previous_raw_column_span = if spans.raw_column_span == 1 {
            raw_spans.remove(&id)
        } else {
            raw_spans.insert(id, spans.raw_column_span)
        };
        effective_spans_changed || previous_raw_column_span.unwrap_or(1) != spans.raw_column_span
    }

    pub(crate) fn raw_table_column_span(&self, id: NodeSlotId) -> u32 {
        // data() validates that id names a live slot with a matching generation.
        self.data(id);
        self.raw_table_column_spans.borrow().get(&id).copied().unwrap_or(1)
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
        assert_eq!(self.data(first_letter).kind.get(), NodeKind::TextNode);
        assert_eq!(self.data(remainder).kind.get(), NodeKind::TextNode);
        assert!(letter_end <= source_length);
        self.text_node_state_mut(first_letter).source_range = Some(FfiTextSourceRange {
            start: 0,
            length: letter_end,
        });
        let mut remainder_state = self.text_node_state_mut(remainder);
        remainder_state.source_range = Some(FfiTextSourceRange {
            start: letter_end,
            length: source_length - letter_end,
        });
        remainder_state.first_letter = first_letter;
        drop(remainder_state);
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

    pub(crate) fn text_has_source_range(&self, id: NodeSlotId) -> bool {
        self.text_node_state(id)
            .is_some_and(|state| state.source_range.is_some())
    }

    pub(crate) fn first_letter_owner_of_split_text(&self, id: NodeSlotId) -> Option<StyleNodeID> {
        let first_letter = self.text_node_state(id)?.first_letter;
        if !self.slot_is_live(first_letter) {
            return None;
        }
        self.node_style_node(self.data(first_letter).parent.get())
    }

    pub(crate) fn text_fragments(&self, primary: NodeSlotId) -> TextFragments {
        let mut fragments = TextFragments {
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

    /// The node's group payload pointer array, read in place from the
    /// Rust-owned style container that NodeData.style addresses. The node's
    /// retained immutable ComputedValues owns the container, and the pointer
    /// is only replaced between passes, so the array stays valid for as long
    /// as the node occupies its arena slot.
    /// Notes that `slot` was given a style with `content-visibility: auto`, if it was, so that
    /// every layout commit from then on collects the boxes with it, and with a scroll snap type,
    /// so that the scroll containers builds make from then on go to the document.
    fn note_row_style(&self, slot: NodeSlotId) {
        if self.may_have_auto_content_visibility.get() && self.may_have_scroll_snap_areas.get() {
            return;
        }
        let Some(payloads) = self.style_payloads(slot) else {
            return;
        };
        let style = ComputedValuesView::new(&payloads.groups);
        if style.content_visibility() == crate::css::css_enums::content_visibility::AUTO {
            self.may_have_auto_content_visibility.set(true);
        }
        if style.misc_reset().scroll_snap_strictness != crate::css::css_enums::scroll_snap_strictness::NONE {
            self.may_have_scroll_snap_areas.set(true);
        }
    }

    /// Whether a row has ever been given a style with `content-visibility: auto`.
    pub(crate) fn may_have_auto_content_visibility(&self) -> bool {
        self.may_have_auto_content_visibility.get()
    }

    /// Whether a row has ever been given a style with a scroll snap type, short of which no scroll container snaps.
    pub(crate) fn may_have_scroll_snap_areas(&self) -> bool {
        self.may_have_scroll_snap_areas.get()
    }

    pub(crate) fn style_payloads(&self, id: NodeSlotId) -> Option<&FfiStylePayloads> {
        Self::row_style_payloads(self.data(id))
    }

    fn row_style_payloads(data: &NodeData) -> Option<&FfiStylePayloads> {
        // SAFETY: The arena pins the style record of each of its rows while the row holds it.
        unsafe { super::node_data::style_payloads(data.style.get()) }
    }

    /// The row of the node `id` names, or `None` once the node is gone.
    pub(crate) fn live_row(&self, id: NodeSlotId) -> Option<LiveRow<'_>> {
        self.slot_is_live(id).then(|| LiveRow {
            data: self.data(id),
            style_node: self.style_nodes[id.slot_index() as usize].get(),
        })
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
        let paintable_rows = self.paintable_rows();
        while !node.is_invalid() {
            let data = self.data(node);
            if invalidation == AncestorInvalidation::StructuralChange {
                self.fc_run_cache_store.note_inline_layout_damage(node);
            }
            self.bump_fragment_cache_epoch(node);
            let (kind, parent) = (data.kind.get(), data.parent.get());
            if super::node_facts::kind_is_box(kind) {
                paintable_rows.clear_cached_overflow_data(node);
            }
            node = parent;
        }
    }

    pub(super) fn bump_fragment_cache_epoch(&self, node: NodeSlotId) {
        let data = self.data(node);
        let epoch = data.fragment_cache_epoch.get().wrapping_add(1);
        data.fragment_cache_epoch.set(epoch);
        if epoch == 0 {
            self.paintable_rows.invalidate_committed_geometry(node.slot_index());
        }
        if self.layout_pass_is_running() {
            self.fragment_cache_epoch_changed_during_layout_pass.set(true);
        }
        self.fc_run_cache_store.note_invalidated_entry(node);
    }

    pub(crate) fn note_structural_change_at_and_above(&self, node: NodeSlotId) {
        if !self.scrollable_overflow.non_child_boxes.borrow().is_empty() {
            self.scrollable_overflow.contained_boxes_dirty.set(true);
        }
        self.invalidate_at_and_above(node, AncestorInvalidation::StructuralChange);
    }

    pub(crate) fn bump_fragment_cache_epoch_of_self_and_ancestors(&self, node: NodeSlotId) {
        self.invalidate_at_and_above(node, AncestorInvalidation::ContentChange);
    }

    pub(crate) fn insert_child(&self, parent: NodeSlotId, child: NodeSlotId, before: NodeSlotId) {
        assert_ne!(parent, child, "a layout node cannot become its own child");
        let parent_data = self.write_shape(parent);
        let child_data = self.write_shape(child);

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

        child_data.set_parent(parent);
        child_data.set_previous_sibling(previous);
        child_data.set_next_sibling(before);
        if previous.is_invalid() {
            parent_data.set_first_child(child);
        } else {
            self.write_shape(previous).set_next_sibling(child);
        }
        if before.is_invalid() {
            parent_data.set_last_child(child);
        } else {
            self.write_shape(before).set_previous_sibling(child);
        }

        self.assign_pre_order_labels_to_inserted_subtree(parent, child);
        self.note_layout_subtree_attached(child);
        self.pending_attached_subtree_roots.borrow_mut().push(child);
        self.note_structural_change_at_and_above(parent);
    }

    pub(crate) fn remove_child(&self, parent: NodeSlotId, child: NodeSlotId) {
        if self.paintable_row_count() > 0 {
            self.push_enclosing_paint_order_damage(child);
        }
        self.unlink_child(parent, child);
        self.note_structural_change_at_and_above(parent);
    }

    fn unlink_child(&self, parent: NodeSlotId, child: NodeSlotId) {
        let parent_data = self.write_shape(parent);
        let child_data = self.write_shape(child);

        let child_parent = child_data.parent.get();
        assert_eq!(child_parent, parent, "removed layout node is not a child of the parent");
        let previous = child_data.previous_sibling.get();
        let next = child_data.next_sibling.get();

        if previous.is_invalid() {
            let first_child = parent_data.first_child.get();
            assert_eq!(first_child, child, "layout node child list lost its first child");
            parent_data.set_first_child(next);
        } else {
            let previous_data = self.write_shape(previous);
            let previous_next_sibling = previous_data.next_sibling.get();
            assert_eq!(
                previous_next_sibling, child,
                "layout node sibling chain is inconsistent"
            );
            previous_data.set_next_sibling(next);
        }

        if next.is_invalid() {
            let last_child = parent_data.last_child.get();
            assert_eq!(last_child, child, "layout node child list lost its last child");
            parent_data.set_last_child(previous);
        } else {
            let next_data = self.write_shape(next);
            let next_previous_sibling = next_data.previous_sibling.get();
            assert_eq!(
                next_previous_sibling, child,
                "layout node sibling chain is inconsistent"
            );
            next_data.set_previous_sibling(previous);
        }

        child_data.set_parent(NodeSlotId::INVALID);
        child_data.set_previous_sibling(NodeSlotId::INVALID);
        child_data.set_next_sibling(NodeSlotId::INVALID);
    }

    pub(crate) fn paint_state(&self) -> &RefCell<crate::painting::paint_state::PaintState> {
        &self.paint_state
    }

    pub(crate) fn node_data_if_live(&self, id: NodeSlotId) -> Option<&NodeData> {
        if !self.slot_is_live(id) {
            return None;
        }
        Some(self.data(id))
    }

    pub(crate) fn note_inline_box_lifted_out_of(&self, node: NodeSlotId, inline_box: Option<NodeSlotId>) {
        let mut lifted = self.inline_boxes_lifted_out_of.borrow_mut();
        match inline_box {
            Some(inline_box) => {
                lifted.insert(node, inline_box);
            }
            None => {
                lifted.remove(&node);
            }
        }
    }

    pub(crate) fn inline_box_lifted_out_of(&self, node: NodeSlotId) -> Option<NodeSlotId> {
        self.inline_boxes_lifted_out_of
            .borrow()
            .get(&node)
            .copied()
            .filter(|&inline_box| self.slot_is_live(inline_box))
    }

    pub(crate) fn slot_is_live(&self, id: NodeSlotId) -> bool {
        if id.is_invalid() {
            return false;
        }
        self.slot_metadata
            .get(id.slot_index() as usize)
            .is_some_and(|metadata| metadata.occupied && metadata.generation == id.generation())
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
            if self.node_is_dom_backed(current) || self.data(current).generated_for.get() != 0 {
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

    pub(crate) fn node_flags(&self, id: NodeSlotId) -> u32 {
        self.data(id).flags.get()
    }

    /// Owes the host `id`'s image resources once the layout update the running build is part of
    /// is over. An image box that owns its image's provider has no image until then.
    pub(crate) fn owe_image_resources(&self, id: NodeSlotId, owed: OwedImageResources) {
        if owed.owns_provider() {
            self.owned_image_providers
                .borrow_mut()
                .insert(id, OwnedImageProvider::Awaited);
        }
        self.image_resources_owed_to_host.borrow_mut().push((id, owed));
    }

    /// The image resources the finished builds owe the host, in the order they came to owe them.
    /// A later build can free a row, so whoever pays them asks whether it is live first.
    pub(crate) fn take_image_resources_owed_to_host(&self) -> Vec<(NodeSlotId, OwedImageResources)> {
        self.image_resources_owed_to_host.take()
    }

    /// Where `id` stands with the provider of its image, if it is an image box that owns one.
    pub(crate) fn owned_image_provider(&self, id: NodeSlotId) -> Option<OwnedImageProvider> {
        self.owned_image_providers.borrow().get(&id).copied()
    }

    /// Notes that the host is about to hand `id` the provider it owns, if it was waiting for one.
    pub(crate) fn note_owned_provider_handed_over(&self, id: NodeSlotId) {
        if let Some(provider) = self.owned_image_providers.borrow_mut().get_mut(&id) {
            *provider = OwnedImageProvider::HandedOver;
        }
    }

    /// Notes that the running build gave the scroll container `id` a style, which decides whether
    /// scroll snapping happens in it once the build is over.
    pub(crate) fn note_built_scroll_container(&self, id: NodeSlotId) {
        self.built_scroll_containers.borrow_mut().push(id);
    }

    /// The scroll containers the build that is over gave a style, for the document's scroll snap
    /// bookkeeping, each with whether it is a scroll snap container, which the viewport's answer
    /// needs the root element's box for. A row the build freed again is left out. Where no box was
    /// ever given a scroll snap type, none of them snaps, and the document has no snapped areas for
    /// them to forget, so it takes none.
    pub(crate) fn take_built_scroll_containers(&self) -> Vec<super::formatting_context::FfiBuiltScrollContainer> {
        let mut built = self.built_scroll_containers.borrow_mut();
        if !self.may_have_scroll_snap_areas.get() {
            built.clear();
            return Vec::new();
        }
        built
            .drain(..)
            .filter(|&slot| self.slot_is_live(slot))
            .map(|slot| {
                let axes = crate::painting::scroll_snap::snap_axes_of_scroll_container(self, slot);
                super::formatting_context::FfiBuiltScrollContainer {
                    slot,
                    is_scroll_snap_container: axes.x || axes.y,
                }
            })
            .collect()
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

    fn data_mut(&mut self, index: u32) -> &mut NodeData {
        self.shape_writes.note_identity();
        let index = index as usize;
        let chunk = self
            .chunks
            .get_mut(index / SLOTS_PER_CHUNK)
            .expect("invalid layout node arena slot ID");
        chunk.slot_mut(index / SLOTS_PER_CHUNK, index % SLOTS_PER_CHUNK, &self.shape_writes)
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

/// A live row of the arena, as painting reads it.
#[derive(Clone, Copy)]
pub(crate) struct LiveRow<'a> {
    data: &'a NodeData,
    style_node: Option<StyleNodeID>,
}

impl super::node_facts::NodeShape for LiveRow<'_> {
    fn kind(&self) -> NodeKind {
        self.data.kind.get()
    }

    fn flags(&self) -> u32 {
        self.data.flags.get()
    }
}

impl<'a> crate::painting::paint_read::PaintRow<'a> for LiveRow<'a> {
    fn generated_for(self) -> u8 {
        self.data.generated_for.get()
    }

    fn dom_paint_facts(self) -> u8 {
        self.data.dom_paint_facts.get()
    }

    fn compositor_animation_frame_kinds(self) -> u8 {
        self.data.compositor_animation_frame_kinds.get()
    }

    fn parent(self) -> NodeSlotId {
        self.data.parent.get()
    }

    fn first_child(self) -> NodeSlotId {
        self.data.first_child.get()
    }

    fn next_sibling(self) -> NodeSlotId {
        self.data.next_sibling.get()
    }

    fn style(self) -> Option<ComputedValuesView<'a>> {
        LayoutNodeArena::row_style_payloads(self.data).map(|payloads| ComputedValuesView::new(&payloads.groups))
    }

    fn style_node(self) -> Option<StyleNodeID> {
        self.style_node
    }
}

#[cfg(test)]
#[derive(Clone, Copy)]
pub(crate) struct NodeAllocation {
    pub(crate) slot: NodeSlotId,
}

/// Whether the counter styles the generated content of the element `style_node` names, or of its
/// pseudo-element `generated_for`, names now differ from the ones its box was built with.
///
/// # Safety
///
/// The arena must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_content_counter_styles_changed(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    style_node: u32,
    generated_for: u8,
) -> bool {
    let Some(owner) = counter_owner(style_node, generated_for) else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, read, owner, |arena, owner| {
            super::generated_content::content_counter_styles_changed(arena, owner)
        })
    }
}

fn counter_owner(style_node: u32, generated_for: u8) -> Option<super::counters::CounterOwner> {
    StyleNodeID::from_raw(style_node).map(|element| super::counters::CounterOwner { element, generated_for })
}

/// Whether an element whose installed style has the `count` group payloads `payloads` has an innermost `list-item`
/// counter of its own that counts forward, as the counters a layout tree build resolves for it from that style have:
/// appending a last list item to such an element leaves the items before it as they are numbered.
///
/// # Safety
///
/// `payloads` must point at `count` live group payloads.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_resets_forward_list_item_counter(payloads: *const *const c_void, count: usize) -> bool {
    assert!(!payloads.is_null(), "style group payload array is null");
    // SAFETY: Guaranteed by the caller.
    let groups = unsafe { std::slice::from_raw_parts(payloads, count) };
    super::counters::style_resets_forward_list_item_counter(ComputedValuesView::new(groups))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_node_needs_compositor_animation_frame(
    host: &DocumentHost,
    id: NodeSlotId,
    kind: super::node_data::CompositorAnimationFrameKind,
    value: bool,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        queue(
            host,
            LayoutChange::SetNodeNeedsCompositorAnimationFrame { node: id, kind, value },
        );
    }
}

/// What the row is scrolled to, as the document published it.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_row_scroll_offset(host: &DocumentHost, slot: NodeSlotId) -> FfiCssPixelPoint {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        read_arena(host, node_read(), slot, |arena, slot| {
            arena.row_scroll_offset(slot).into()
        })
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_node_style(
    host: &DocumentHost,
    id: NodeSlotId,
    style_record: u64,
    payloads: *const c_void,
) {
    let change = LayoutChange::SetNodeStyle {
        node: id,
        record: style_record,
        payloads: StylePayloadsRef::new(payloads.cast()),
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, change) };
}

/// Prepares `row` for leaving the layout tree. A detached box is read until its row is freed, so
/// the host pins its style record, and its paint cache is cleaned now rather than through the
/// journal, which would resolve the node's identity after a replacement row was bound. The image
/// observers the row holds are dropped, and the image provider it owns is told.
pub(crate) fn prepare_row_for_detach(host_calls: HostCalls<'_>, arena: &LayoutNodeArena, row: NodeSlotId) {
    arena.pin_style_record_for_detachment(row);
    arena.push_paint_damage(
        row,
        crate::painting::record::damage::PaintDamage::ALL_DRAW | crate::painting::record::damage::PaintDamage::ALL_HIT,
    );
    host_calls.row_detached(row, arena.data(row).kind.get());
}

/// The host half of a row leaving the layout tree: the image observers it holds are dropped, and
/// the image provider it owns is told.
pub(crate) fn tell_host_of_row_detach(main_thread: &MainThread, row: NodeSlotId, kind: NodeKind) {
    let Some(host_tables) = main_thread.host_tables() else {
        return;
    };
    if super::tree_builder::node_kind_is_node_with_style(kind) {
        let observers = host_tables.replace_image_observers(row, std::ptr::null_mut());
        crate::layout::tree_mutation::destroy_image_observers(main_thread, observers);
    }
    if kind == NodeKind::ImageBox {
        crate::layout::tree_mutation::notify_owned_image_provider_of_detach(
            main_thread,
            host_tables.owned_image_provider(row),
        );
    }
}

/// Prepares every row of the subtree `root` heads for leaving the layout tree, in pre-order.
pub(crate) fn prepare_subtree_for_detach(host_calls: HostCalls<'_>, arena: &LayoutNodeArena, root: NodeSlotId) {
    let mut rows = Vec::new();
    arena.for_each_node_in_layout_subtree_in_pre_order(root, |row| rows.push(row));
    for row in rows {
        prepare_row_for_detach(host_calls, arena, row);
    }
}

/// Pins, for the host, the style record of the box the element or text node `style_node` names is
/// bound to, or of the box of its pseudo-element `generated_for`, so that the box keeps its style
/// readable once the node has left the document. The row is found by identity, so this makes no
/// shell.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_pin_bound_box_style_record_for_detachment(
    host: &DocumentHost,
    style_node: u32,
    generated_for: u8,
) {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe {
        queue(
            host,
            LayoutChange::PinBoundBoxStyleRecordForDetachment {
                style_node,
                generated_for,
            },
        );
    }
}

/// Pins the style record `record` for the host's readers of the row `slot`, until the host
/// releases it or the row is freed.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `slot` a live row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_pin_node_style_record_for_host(
    host: &DocumentHost,
    slot: NodeSlotId,
    record: u64,
) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::PinNodeStyleRecordForHost { node: slot, record }) };
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `slot` a live row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_release_node_style_record_pin_for_host(host: &DocumentHost, slot: NodeSlotId) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::ReleaseNodeStyleRecordPinForHost { node: slot }) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_set_shell_factory(
    host: &DocumentHost,
    context: *mut c_void,
    factory: unsafe extern "C" fn(*mut c_void, NodeSlotId, NodeKind),
) {
    host.host_tables().shell_factory.set(Some((context, factory)));
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread. The host must stay registered only while
/// its context is live, and must not reenter the arena from the callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_box_presence_host(
    host: &DocumentHost,
    context: *mut c_void,
    callback: unsafe extern "C" fn(*mut c_void, u32, u8),
) {
    // SAFETY: Guaranteed by the caller.
    unsafe {
        queue(
            host,
            LayoutChange::SetBoxPresenceHost(Some(BoxPresenceHost(context, callback))),
        );
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_clear_box_presence_host(host: &DocumentHost) {
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::SetBoxPresenceHost(None)) };
}

/// Forgets every callback the document registered with its host, as the document is finalized.
#[unsafe(no_mangle)]
pub extern "C" fn document_host_clear_callbacks(host: &DocumentHost) {
    host.host_tables().clear_callbacks();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_attach_shell(host: &DocumentHost, id: NodeSlotId, shell: *mut c_void) {
    host.host_tables().attach_shell(id, shell);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_set_style_record_host_callbacks(
    host: &DocumentHost,
    callbacks: FfiStyleRecordHostCallbacks,
) {
    host.host_tables()
        .shell_style_changed_host
        .set(Some((callbacks.context, callbacks.shell_style_changed)));
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_layout_pass_is_running(host: &DocumentHost) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { read_arena(host, node_read(), (), |arena, ()| arena.layout_pass_is_running()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_needs_full_layout_tree_update(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { read_arena(host, read, (), |arena, ()| arena.needs_full_layout_tree_update()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_layout_root(
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
) -> NodeSlotId {
    // SAFETY: Guaranteed by the caller.
    unsafe { read_arena(host, read, (), |arena, ()| arena.layout_root()) }
}

/// Whether the layout of `host`'s document is up to date, where the host knows it without asking and no frame brings
/// layout the host waits for.
fn known_layout_is_up_to_date(
    host: &DocumentHost,
    _: &crate::render_state::NoFrameInFlight,
    document: Option<StyleNodeID>,
) -> Option<bool> {
    let up_to_date = host.known_layout_up_to_date_unless_built()?;
    Some(
        up_to_date
            && !document
                .is_some_and(|document| host.read_marks(|marks| marks.needs(document) || marks.child_needs(document))),
    )
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_layout_is_up_to_date(host: &DocumentHost, document_style_node: u32) -> bool {
    // A frame in flight, or a round that flew and is not paid yet, brings layout the host waits for. The document's
    // layout tree update marks are the frame's, so they are read only once no frame holds them.
    let Some(here) = host.layout_waits_for_no_frame() else {
        return false;
    };
    let document = StyleNodeID::from_raw(document_style_node);
    if let Some(up_to_date) = known_layout_is_up_to_date(host, &here, document) {
        return up_to_date;
    }
    host.ask(here, |state| {
        let arena = state.arena_mut();
        let document_needs_layout_tree_build = document.is_some_and(|document| {
            let marks = arena.layout_tree_update_marks().borrow();
            marks.needs(document) || marks.child_needs(document)
        });
        arena.layout_is_up_to_date(document_needs_layout_tree_build)
    })
}

/// Whether the host knows, without asking, that the layout of `host`'s document is not up to date: a frame brings
/// layout it waits for, or what it knows of the render state says so.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_layout_is_known_stale(host: &DocumentHost, document_style_node: u32) -> bool {
    let Some(here) = host.layout_waits_for_no_frame() else {
        return true;
    };
    known_layout_is_up_to_date(host, &here, StyleNodeID::from_raw(document_style_node)) == Some(false)
}

/// Refreshes the text content and replaced-content facts of every node enrolled since the last
/// sync, ahead of a pass that caches them. A pass already on the stack owns those caches, so a
/// request nested inside one is a no-op.
pub(crate) fn sync_enrolled_content_for_layout(arena: &mut LayoutNodeArena) {
    if arena.layout_pass_is_running() {
        return;
    }
    for node in arena.pending_text_nodes_for_content_sync() {
        // Detached nodes retain enrollment until a parent supplies their style.
        if !arena.slot_is_live(node) || arena.data(node).parent.get().is_invalid() {
            continue;
        }
        super::rendered_text::ensure_text_content(arena, node);
    }
    arena.sync_enrolled_replaced_content_facts();
}

#[cfg(test)]
mod tests {
    use std::cell::Cell;

    use crate::layout::abspos_inputs::{
        AbsposAxisMode, AbsposContainingBlockInfo, AbsposLayoutInputs, StaticPositionAlignment, StaticPositionRect,
    };
    use crate::layout::layout_node_arena::{
        Chunk, DerivedStyleRecord, IntrinsicBlockSizeMeasurement, IntrinsicInlineSizeMeasurement,
        IntrinsicSizeCacheKey, IntrinsicSizeCacheKind, LayoutNodeArena, SLOTS_PER_CHUNK, TableCellMeasurement,
        TableCellMeasurementKey,
    };
    use crate::layout::node_data::{NodeConstructionFacts, NodeFlag, NodeKind, NodeSlotId};
    use crate::layout::{CssPixels, fragment_tree, used_values};
    use crate::painting::paint_read::{GeometryRead, PaintRead};
    use std::ffi::c_void;

    fn test_construction_facts() -> NodeConstructionFacts {
        test_construction_facts_with_kind(NodeKind::Box)
    }

    fn test_anonymous_construction_facts() -> NodeConstructionFacts {
        NodeConstructionFacts {
            is_anonymous: true,
            ..test_construction_facts()
        }
    }

    fn test_construction_facts_with_kind(kind: NodeKind) -> NodeConstructionFacts {
        NodeConstructionFacts {
            kind,
            is_anonymous: false,
            is_html_input_element: false,
            is_html_html_element: false,
            is_document_element: false,
            is_in_user_agent_shadow_tree: false,
            uses_button_layout: false,
            is_editing_host: false,
            is_body: false,
            dom_paint_facts: 0,
            style_node: 0,
        }
    }

    #[test]
    fn published_bound_rows_stay_as_they_were_while_the_rows_rebind() {
        let element = super::StyleNodeID::from_raw(7).unwrap();
        let mut rows = super::BoundRows::default();
        let first = NodeSlotId::new(3, 1);
        assert_eq!(rows.set_row(element, first), NodeSlotId::INVALID);
        rows.set_pseudo_element_row(element, 1, NodeSlotId::new(4, 1));
        rows.set_viewport_row(NodeSlotId::new(5, 1));
        let version = rows.version();
        let published = rows.publish();
        assert_eq!(rows.version(), version, "publishing writes nothing");
        assert_eq!(rows.set_row(element, NodeSlotId::new(6, 1)), first);
        rows.set_pseudo_element_row(element, 1, NodeSlotId::INVALID);
        assert_ne!(rows.version(), version);
        assert_eq!(published.row(element), first);
        assert_eq!(published.pseudo_element_row(element, 1), NodeSlotId::new(4, 1));
        assert_eq!(published.viewport_row(), NodeSlotId::new(5, 1));
        assert_eq!(rows.publish().pseudo_element_row(element, 1), NodeSlotId::INVALID);
    }

    #[test]
    fn a_retired_style_node_leaves_every_row_carrying_it() {
        use crate::css::style::tree::StyleNodeID;
        // The rows name their generators, whose unique node ids the style mirror answers for.
        let mut engine = crate::css::style::StyleEngine::new();
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(crate::css::style::StyleEngineHandle::from_raw(&raw mut engine));
        let style_node = StyleNodeID::element(3);
        let rows = [(); 3].map(|()| {
            arena.allocate(NodeConstructionFacts {
                style_node: style_node.raw(),
                ..test_construction_facts()
            })
        });
        let generated = arena.allocate(test_anonymous_construction_facts());
        arena.set_node_generated_for(generated, 1, Some(style_node));
        assert!(rows.iter().all(|row| arena.node_style_node(*row) == Some(style_node)));

        arena
            .free_subtree(rows[1])
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        arena.forget_style_node(style_node);
        assert_eq!(arena.node_style_node(rows[0]), None);
        assert_eq!(arena.node_style_node(rows[2]), None);
        assert_eq!(arena.node_style_node(generated), None);

        let reconnected = StyleNodeID::element(1);
        arena.set_style_node_of_generated_subtree(generated, Some(reconnected));
        assert_eq!(arena.node_style_node(generated), Some(reconnected));
        for row in [rows[0], rows[2], generated] {
            arena
                .free_subtree(row)
                .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        }
        arena.forget_style_node(reconnected);
    }

    #[test]
    fn a_retired_shadow_host_takes_its_tree_scopes_anchor_names_with_it() {
        use crate::css::style::tree::StyleNodeID;
        let arena = LayoutNodeArena::new();
        let host = StyleNodeID::element(4);
        let anchor_in_shadow_tree = StyleNodeID::element(5);
        let anchor_in_document = StyleNodeID::element(6);
        let name = 0x1000;
        arena.set_anchor_name_elements(Some(host), name, [anchor_in_shadow_tree].into_iter());
        arena.set_anchor_name_elements(None, name, [anchor_in_document].into_iter());

        arena.forget_style_node(host);
        // Whatever element is issued the identity next hosts no names it did not register.
        let names = arena.style_node_tables.anchor_name_elements.borrow();
        assert!(!names.contains_key(&super::ScopedAnchorName {
            scope_host: Some(host),
            name
        }));
        assert_eq!(
            names.get(&super::ScopedAnchorName { scope_host: None, name }),
            Some(&vec![anchor_in_document])
        );
        drop(names);

        arena.set_anchor_name_elements(None, name, std::iter::empty());
        assert!(arena.style_node_tables.anchor_name_elements.borrow().is_empty());
    }

    #[test]
    fn an_anchor_name_answers_the_last_acceptable_element_of_its_tree_scope() {
        use crate::css::style::tree::StyleNodeID;
        let arena = LayoutNodeArena::new();
        let [first, second, third] = [7, 8, 9].map(StyleNodeID::element);
        let name = 0x2000;
        arena.set_anchor_name_elements(None, name, [first, second, third].into_iter());
        assert_eq!(arena.last_element_with_anchor_name(None, name, |_| true), Some(third));
        assert_eq!(
            arena.last_element_with_anchor_name(None, name, |&element| element != third),
            Some(second)
        );
        assert_eq!(arena.last_element_with_anchor_name(Some(first), name, |_| true), None);
        assert_eq!(arena.last_element_with_anchor_name(None, name + 1, |_| true), None);
    }

    #[test]
    fn a_commit_message_names_the_dom_node_a_row_stands_for() {
        use crate::css::style::tree::StyleNodeID;
        // The rows name their generators, whose unique node ids the style mirror answers for.
        let mut engine = crate::css::style::StyleEngine::new();
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(crate::css::style::StyleEngineHandle::from_raw(&raw mut engine));
        let element = StyleNodeID::element(3);
        let principal = arena.allocate(NodeConstructionFacts {
            style_node: element.raw(),
            ..test_construction_facts()
        });
        let viewport = arena.allocate(test_construction_facts_with_kind(NodeKind::Viewport));
        let anonymous = arena.allocate(test_anonymous_construction_facts());
        let pseudo_element = arena.allocate(test_anonymous_construction_facts());
        arena.set_node_generated_for(pseudo_element, 1, Some(element));

        assert_eq!(arena.commit_message_style_node(principal), Some(element.raw()));
        assert_eq!(arena.commit_message_style_node(viewport), Some(0));
        assert_eq!(arena.commit_message_style_node(anonymous), None);
        assert_eq!(arena.commit_message_style_node(pseudo_element), None);
        assert_eq!(arena.dom_node_style_node(pseudo_element), None);
        assert_eq!(arena.dom_node_style_node(principal), Some(element));

        arena
            .free_subtree(principal)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert_eq!(arena.commit_message_style_node(principal), None);
        for row in [viewport, anonymous, pseudo_element] {
            arena
                .free_subtree(row)
                .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        }
        arena.forget_style_node(element);
    }

    #[test]
    fn the_box_presence_host_hears_each_change_to_a_nodes_boxes() {
        use super::{BOX_PRESENCE_HAS_COMMITTED_BOX, BOX_PRESENCE_HAS_LAYOUT_BOX};
        use crate::css::style::tree::StyleNodeID;
        unsafe extern "C" fn record(context: *mut c_void, style_node: u32, bits: u8) {
            // SAFETY: The test registers a live Vec as the context, and reads it only after unregistering.
            unsafe { &mut *context.cast::<Vec<(u32, u8)>>() }.push((style_node, bits));
        }
        let mut reports: Vec<(u32, u8)> = Vec::new();
        let mut arena = LayoutNodeArena::new();
        arena.set_box_presence_host(Some(super::BoxPresenceHost(
            std::ptr::from_mut(&mut reports).cast::<c_void>(),
            record,
        )));
        let element = StyleNodeID::element(3);
        let row = arena.allocate(NodeConstructionFacts {
            style_node: element.raw(),
            ..test_construction_facts()
        });
        arena.bind_row(row);
        arena.populate_paintable_row(row);
        let reset = arena
            .prepare_paintable_row_cleared_reset(row)
            .expect("a populated row has a reset");
        arena.paintable_row_cleared(reset);
        arena
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        arena.set_box_presence_host(None);

        let both = BOX_PRESENCE_HAS_LAYOUT_BOX | BOX_PRESENCE_HAS_COMMITTED_BOX;
        let element = element.raw();
        assert_eq!(
            reports,
            [
                (element, BOX_PRESENCE_HAS_LAYOUT_BOX),
                (element, both),
                (element, BOX_PRESENCE_HAS_LAYOUT_BOX),
                (element, 0),
            ]
        );
    }

    #[test]
    fn a_change_that_forgets_a_style_node_owes_the_host_its_box_presence() {
        use crate::css::style::tree::StyleNodeID;
        use crate::layout::layout_changes::LayoutChange;
        unsafe extern "C" fn record(context: *mut c_void, style_node: u32, bits: u8) {
            // SAFETY: The test registers a live Vec as the context, and reads it only after unregistering.
            unsafe { &mut *context.cast::<Vec<(u32, u8)>>() }.push((style_node, bits));
        }
        let mut reports: Vec<(u32, u8)> = Vec::new();
        let mut arena = LayoutNodeArena::new();
        let element = StyleNodeID::element(3);
        let row = arena.allocate(NodeConstructionFacts {
            style_node: element.raw(),
            ..test_construction_facts()
        });
        arena.bind_row(row);
        arena.set_box_presence_host(Some(super::BoxPresenceHost(
            std::ptr::from_mut(&mut reports).cast::<c_void>(),
            record,
        )));

        let owed = LayoutChange::StyleNodeChanged {
            old: Some(element),
            new: None,
            generated_for: Default::default(),
        }
        .apply_owing(&mut arena);
        assert!(reports.is_empty(), "the change tells the host nothing while it applies");
        assert!(!owed.is_empty());
        owed.pay(&crate::stage::MainThread::for_test());
        arena.set_box_presence_host(None);
        assert_eq!(reports, [(element.raw(), 0)]);

        arena
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn a_node_is_bound_only_to_the_row_that_took_the_binding() {
        use crate::css::style::tree::StyleNodeID;
        let mut arena = LayoutNodeArena::new();
        let element = StyleNodeID::element(3);
        let facts = NodeConstructionFacts {
            style_node: element.raw(),
            ..test_construction_facts()
        };
        let old_row = arena.allocate(facts);
        assert!(arena.bound_row(element).is_invalid());
        arena.bind_row(old_row);
        assert_eq!(arena.bound_row(element), old_row);

        // A rebuilt row takes the binding; a row built without it, like a first-letter slice, only shares
        // the node.
        let new_row = arena.allocate(facts);
        arena.note_rows_share_dom_node(old_row, new_row);
        arena.bind_row(new_row);
        let slice_row = arena.allocate(facts);
        arena.note_rows_share_dom_node(new_row, slice_row);
        assert_eq!(arena.bound_row(element), new_row);

        // A changed identity carries the binding along.
        let changed = StyleNodeID::element(4);
        arena.set_style_node_of_rows_sharing_dom_node_with(new_row, Some(changed));
        assert!(arena.bound_row(element).is_invalid());
        assert_eq!(arena.bound_row(changed), new_row);

        // Freeing the bound row binds the node to a row that still shares it.
        arena
            .free_subtree(new_row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert_eq!(arena.bound_row(changed), old_row);
        arena.unbind_row(old_row);
        assert!(arena.bound_row(changed).is_invalid());
        arena.bind_row(slice_row);
        arena.forget_style_node(changed);
        assert!(arena.bound_row(changed).is_invalid());

        let viewport = arena.allocate(test_construction_facts_with_kind(NodeKind::Viewport));
        arena.bind_row(viewport);
        assert_eq!(arena.bound_viewport_row(), viewport);
        arena
            .free_subtree(viewport)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(arena.bound_viewport_row().is_invalid());

        for row in [old_row, slice_row] {
            arena
                .free_subtree(row)
                .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        }
    }

    #[test]
    fn a_pseudo_element_is_bound_only_to_its_principal_box() {
        use crate::css::style::tree::StyleNodeID;
        // The rows name their generators, whose unique node ids the style mirror answers for.
        let mut engine = crate::css::style::StyleEngine::new();
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(crate::css::style::StyleEngineHandle::from_raw(&raw mut engine));
        let generator = StyleNodeID::element(2);
        let principal_box = arena.allocate(test_anonymous_construction_facts());
        let content = arena.allocate(test_anonymous_construction_facts());
        for row in [principal_box, content] {
            arena.set_node_generated_for(row, 1, Some(generator));
        }
        arena.bind_row(principal_box);
        assert_eq!(arena.bound_pseudo_element_row(generator, 1), principal_box);
        assert!(arena.bound_pseudo_element_row(generator, 2).is_invalid());
        assert!(arena.bound_row(generator).is_invalid());

        // Content carrying the same pair leaves the binding alone.
        arena.unbind_row(content);
        assert_eq!(arena.bound_pseudo_element_row(generator, 1), principal_box);

        // A changed generator carries the binding along; a retired one clears it.
        let changed = StyleNodeID::element(3);
        arena.set_style_node_of_generated_subtree(principal_box, Some(changed));
        assert!(arena.bound_pseudo_element_row(generator, 1).is_invalid());
        assert_eq!(arena.bound_pseudo_element_row(changed, 1), principal_box);
        arena.forget_style_node(changed);
        assert!(arena.bound_pseudo_element_row(changed, 1).is_invalid());

        arena.set_node_generated_for(principal_box, 1, Some(generator));
        arena.bind_row(principal_box);
        arena
            .free_subtree(principal_box)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(arena.bound_pseudo_element_row(generator, 1).is_invalid());
        arena
            .free_subtree(content)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn element_and_text_style_nodes_with_the_same_index_chain_separately() {
        use crate::css::style::tree::StyleNodeID;
        let mut arena = LayoutNodeArena::new();
        let element = StyleNodeID::element(2);
        let text = StyleNodeID::text(2);
        let element_row = arena.allocate(NodeConstructionFacts {
            style_node: element.raw(),
            ..test_construction_facts()
        });
        let text_row = arena.allocate(NodeConstructionFacts {
            style_node: text.raw(),
            ..test_construction_facts()
        });
        assert_eq!(arena.node_style_node(element_row), Some(element));
        assert_eq!(arena.node_style_node(text_row), Some(text));

        arena.forget_style_node(text);
        assert_eq!(arena.node_style_node(text_row), None);
        assert_eq!(arena.node_style_node(element_row), Some(element));

        arena.set_style_node_of_rows_sharing_dom_node_with(text_row, Some(StyleNodeID::text(5)));
        assert_eq!(arena.node_style_node(text_row), Some(StyleNodeID::text(5)));
        for row in [element_row, text_row] {
            arena
                .free_subtree(row)
                .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        }
    }

    #[test]
    fn an_unbound_slot_has_no_shell_until_a_shell_is_bound() {
        let mut arena = LayoutNodeArena::new();
        let slot = arena.allocate_unbound();
        assert!(arena.slot_is_live(slot));
        assert_eq!(arena.data(slot).kind.get(), NodeKind::Unset);

        let unbound_freed = arena.free_subtree(slot);
        unbound_freed.destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(!arena.slot_is_live(slot));

        let slot = arena.allocate_unbound();
        arena.bind_shell(slot, test_construction_facts());
        assert_eq!(arena.data(slot).kind.get(), NodeKind::Box);
        assert!(arena.data(slot).flags.get() & NodeFlag::HasStyle as u32 != 0);
        arena
            .free_subtree(slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn freeing_the_layout_root_forgets_it_and_the_pending_rebuilt_roots() {
        let mut arena = LayoutNodeArena::new();
        let viewport = arena.allocate_unbound();
        let rebuilt = arena.allocate_unbound();
        arena.set_layout_root(viewport);
        arena.set_pending_rebuilt_subtree_roots(vec![rebuilt], true);
        assert_eq!(arena.layout_root(), viewport);

        arena
            .free_subtree(rebuilt)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert_eq!(arena.layout_root(), viewport);
        assert_eq!(arena.take_pending_rebuilt_subtree_roots(), (vec![rebuilt], true));

        arena.set_pending_rebuilt_subtree_roots(vec![viewport], false);
        arena
            .free_subtree(viewport)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(arena.layout_root().is_invalid());
        assert_eq!(arena.take_pending_rebuilt_subtree_roots(), (Vec::new(), false));
    }

    #[test]
    fn an_anonymous_box_stamped_by_the_arena_keeps_its_style_record_until_freed() {
        let mut arena = LayoutNodeArena::new();
        // The rows' styles hold no group, so the arena is told up front what it would read from them.
        arena.may_have_auto_content_visibility.set(true);
        arena.may_have_scroll_snap_areas.set(true);
        let payloads = [std::ptr::null::<c_void>(); 1];
        let slot = arena.allocate_unbound();
        arena.stamp_anonymous_box(
            slot,
            NodeKind::InlineNode,
            DerivedStyleRecord {
                record: 7,
                payloads: crate::layout::node_data::StylePayloadsRef::new(payloads.as_ptr().cast()),
            },
        );
        assert_eq!(arena.data(slot).kind.get(), NodeKind::InlineNode);
        assert!(arena.data(slot).flags.get() & NodeFlag::Anonymous as u32 != 0);
        assert!(arena.data(slot).flags.get() & NodeFlag::HasStyle as u32 != 0);
        assert_eq!(arena.node_style_record(slot), 7);
        assert!(arena.node_style_record_is_derived(slot));

        let element = arena.allocate(test_construction_facts_with_kind(NodeKind::InlineNode));
        arena.set_node_style(
            element,
            9,
            crate::layout::node_data::StylePayloadsRef::new(payloads.as_ptr().cast()),
        );
        assert_eq!(arena.node_style_record(element), 9);
        assert!(!arena.node_style_record_is_derived(element));

        let freed = arena.free_subtree(slot);
        assert_eq!(freed.arena_pinned_style_record_count(), 1);
        freed.destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        let freed = arena.free_subtree(element);
        assert_eq!(freed.arena_pinned_style_record_count(), 0);
        freed.destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn a_restamped_row_takes_the_paint_facts_its_node_published_since() {
        use crate::css::style::StyleEngine;
        use crate::css::style::tree::StyleNodeID;
        use crate::layout::node_data::DomPaintFact;

        let mut engine = StyleEngine::new();
        let mut text = [0_u32];
        engine.allocate_text_style_nodes(&mut text);
        let text = StyleNodeID::from_raw(text[0]).unwrap();
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(crate::css::style::StyleEngineHandle::from_raw(&raw mut engine));
        let main_thread = crate::stage::MainThread::for_test();

        engine.set_node_dom_paint_facts(text, DomPaintFact::Inert as u8);
        let row = arena.allocate_unbound();
        arena.stamp_dom_row(row, NodeKind::TextNode, Some(text));
        assert_eq!(arena.data(row).dom_paint_facts.get(), DomPaintFact::Inert as u8);
        arena
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&main_thread);

        engine.set_node_dom_paint_facts(text, 0);
        let row = arena.allocate_unbound();
        arena.stamp_dom_row(row, NodeKind::TextNode, Some(text));
        assert_eq!(arena.data(row).dom_paint_facts.get(), 0);
        arena
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&main_thread);
        arena.set_style_engine(crate::css::style::StyleEngineHandle::null());
    }

    #[test]
    fn a_stamped_row_takes_the_table_spans_its_element_published() {
        use crate::css::style::StyleEngine;
        use crate::css::style::tree::{StyleNodeID, TableSpans};

        let mut engine = StyleEngine::new();
        let mut element = [0_u32];
        engine.allocate_style_nodes(&mut element);
        let element = StyleNodeID::from_raw(element[0]).unwrap();
        let spans = TableSpans {
            column_span: 2,
            row_span: 3,
            raw_column_span: 1,
        };
        engine.set_element_table_spans(element, spans);
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(crate::css::style::StyleEngineHandle::from_raw(&raw mut engine));
        let main_thread = crate::stage::MainThread::for_test();

        let cell = arena.allocate_unbound();
        arena.stamp_dom_row(cell, NodeKind::BlockContainer, Some(element));
        assert_eq!(arena.data(cell).table_column_span.get(), 2);
        assert_eq!(arena.data(cell).table_row_span.get(), 3);

        engine.set_element_table_spans(element, TableSpans::default());
        arena.reset_layout_update_flags_in_subtree(cell);
        crate::layout::layout_changes::LayoutChange::RestampTableSpans { node: cell }.apply(&mut arena);
        assert_eq!(arena.data(cell).table_column_span.get(), 1);
        assert_eq!(arena.data(cell).table_row_span.get(), 1);
        assert!(arena.node_needs_layout_update(cell));
        arena
            .free_subtree(cell)
            .destroy_shells_and_invoke_callbacks(&main_thread);
        arena.set_style_engine(crate::css::style::StyleEngineHandle::null());
    }

    #[test]
    fn rows_answer_by_the_unique_node_id_their_element_published() {
        use crate::css::style::StyleEngine;
        use crate::css::style::tree::StyleNodeID;

        let mut engine = StyleEngine::new();
        let mut element = [0_u32];
        engine.allocate_style_nodes(&mut element);
        let element = StyleNodeID::from_raw(element[0]).unwrap();
        engine.set_element_unique_node_id(element, 42);
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(crate::css::style::StyleEngineHandle::from_raw(&raw mut engine));
        let main_thread = crate::stage::MainThread::for_test();

        let principal = arena.allocate_unbound();
        arena.stamp_dom_row(principal, NodeKind::BlockContainer, Some(element));
        let pseudo_element = arena.allocate_unbound();
        arena.set_node_generated_for(pseudo_element, 1, Some(element));
        assert_eq!(arena.unique_node_ids().id(principal), 42);
        assert_eq!(arena.unique_node_ids().id(pseudo_element), 42);

        arena
            .free_subtree(pseudo_element)
            .destroy_shells_and_invoke_callbacks(&main_thread);
        let recycled = arena.allocate_unbound();
        assert_eq!(recycled.slot_index(), pseudo_element.slot_index());
        assert_eq!(arena.unique_node_ids().id(recycled), 0);
        for row in [principal, recycled] {
            arena
                .free_subtree(row)
                .destroy_shells_and_invoke_callbacks(&main_thread);
        }
        arena.set_style_engine(crate::css::style::StyleEngineHandle::null());
    }

    #[test]
    fn rows_hold_the_scroll_offsets_published_by_identity() {
        use crate::css::css_pixels::CssPixelPoint;
        use crate::css::style::StyleEngine;
        use crate::css::style::tree::StyleNodeID;

        let mut engine = StyleEngine::new();
        let mut element = [0_u32];
        engine.allocate_style_nodes(&mut element);
        let element = StyleNodeID::from_raw(element[0]).unwrap();
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(crate::css::style::StyleEngineHandle::from_raw(&raw mut engine));
        let main_thread = crate::stage::MainThread::for_test();
        let has_scroll_offset =
            |arena: &LayoutNodeArena, row| arena.node_flags_if_live(row) & NodeFlag::HasScrollOffset as u32 != 0;
        let offset = CssPixelPoint::new(CssPixels::from_integer(0), CssPixels::from_integer(10));

        // A row stamped for an element that has scrolled holds the offset from the start.
        arena.set_element_scroll_offset(element, offset);
        let principal = arena.allocate_unbound();
        arena.stamp_dom_row(principal, NodeKind::BlockContainer, Some(element));
        arena.bind_row(principal);
        assert_eq!(arena.row_scroll_offset(principal), offset);
        assert!(has_scroll_offset(&arena, principal));
        arena.set_element_scroll_offset(element, CssPixelPoint::default());
        assert!(!has_scroll_offset(&arena, principal));

        // Only the box a pseudo-element is bound to holds what the pseudo-element has scrolled to.
        arena.set_pseudo_element_scroll_offset(element, 1, offset);
        let first = arena.allocate_unbound();
        arena.stamp_pseudo_element_box(first, element, 1);
        assert_eq!(arena.row_scroll_offset(first), offset);
        assert!(has_scroll_offset(&arena, first));
        arena.clear_pseudo_element_box(element, 1);
        let second = arena.allocate_unbound();
        arena.stamp_pseudo_element_box(second, element, 1);
        assert_eq!(arena.row_scroll_offset(first), CssPixelPoint::default());
        assert!(!has_scroll_offset(&arena, first));
        assert!(has_scroll_offset(&arena, second));

        // A retired identity takes what it and its pseudo-elements scrolled to with it.
        arena.forget_style_node(element);
        assert!(
            !arena
                .style_node_tables
                .pseudo_element_scroll_offsets
                .borrow()
                .contains_key(&element)
        );
        for row in [principal, first, second] {
            arena
                .free_subtree(row)
                .destroy_shells_and_invoke_callbacks(&main_thread);
        }
        arena.set_style_engine(crate::css::style::StyleEngineHandle::null());
    }

    #[test]
    fn freeing_a_row_releases_the_style_record_its_host_pinned_once() {
        let mut arena = LayoutNodeArena::new();
        let element = arena.allocate(test_construction_facts_with_kind(NodeKind::InlineNode));
        // The host's pin, as pin_node_style_record_for_host takes it, without an engine to count it.
        arena.style_records_pinned_by_host[element.slot_index() as usize].set(11);
        assert_eq!(arena.node_style_record_pinned_by_host(element), 11);

        let freed = arena.free_subtree(element);
        assert_eq!(freed.arena_pinned_style_record_count(), 1);
        freed.destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());

        let reused = arena.allocate(test_construction_facts_with_kind(NodeKind::InlineNode));
        assert_eq!(reused.slot_index(), element.slot_index());
        assert_eq!(arena.node_style_record_pinned_by_host(reused), 0);
        arena
            .free_subtree(reused)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn previous_dom_backed_or_generated_node_skips_anonymous_slots() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate(test_construction_facts());
        let anonymous_wrapper = arena.allocate(test_anonymous_construction_facts());
        let nested_anonymous = arena.allocate(test_anonymous_construction_facts());
        let element = arena.allocate(test_construction_facts());
        arena.insert_child(root, anonymous_wrapper, NodeSlotId::INVALID);
        arena.insert_child(anonymous_wrapper, nested_anonymous, NodeSlotId::INVALID);
        arena.insert_child(root, element, NodeSlotId::INVALID);

        assert_eq!(arena.previous_dom_backed_or_generated_node(element, false), root);
        assert!(arena.previous_dom_backed_or_generated_node(element, true).is_invalid());
        assert!(arena.previous_dom_backed_or_generated_node(root, false).is_invalid());

        arena.write_shape(nested_anonymous).set_generated_for(1);
        assert_eq!(
            arena.previous_dom_backed_or_generated_node(element, false),
            nested_anonymous
        );

        arena
            .free_subtree(root)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn rows_are_dom_backed_only_while_their_slot_is_live() {
        let mut arena = LayoutNodeArena::new();
        let anonymous = arena.allocate(test_anonymous_construction_facts());
        let element = arena.allocate(test_construction_facts());
        assert!(!arena.node_is_dom_backed(anonymous));
        assert!(arena.node_is_dom_backed(element));
        assert_eq!(arena.live_slot_count(), 2);

        arena
            .free_subtree(element)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(!arena.node_is_dom_backed(element));
        let reoccupant = arena.allocate_for_test();
        assert_eq!(reoccupant.slot.slot_index(), element.slot_index());
        assert!(!arena.node_is_dom_backed(element));
        assert!(!arena.node_is_dom_backed(reoccupant.slot));

        arena
            .free_subtree(anonymous)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        arena
            .free_subtree(reoccupant.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert_eq!(arena.live_slot_count(), 0);
    }

    #[test]
    fn committed_geometry_requires_a_current_layout_commit() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        let current = |arena: &LayoutNodeArena| arena.with_current_committed_fragment(node, |fragment| fragment.node);
        let commit_from_layout = |arena: &LayoutNodeArena| {
            let data = arena.data(node);
            arena.set_committed_fragment_link(
                data,
                fragment_tree::FragmentLink::for_test(node),
                arena.epoch_of_geometry_laid_out_in_this_pass(data),
            );
        };
        commit_from_layout(&arena);
        assert_eq!(current(&arena), Some(node));

        arena.bump_fragment_cache_epoch_of_self_and_ancestors(node);
        assert_eq!(current(&arena), None);
        commit_from_layout(&arena);
        assert_eq!(current(&arena), Some(node));

        let moved = arena.take_committed_fragment_link(arena.data(node)).unwrap();
        arena.set_committed_fragment_link(arena.data(node), moved, None);
        assert_eq!(current(&arena), None);

        arena.begin_active_layout_pass();
        arena.bump_fragment_cache_epoch_of_self_and_ancestors(node);
        commit_from_layout(&arena);
        assert!(arena.leave_active_layout_pass());
        assert_eq!(current(&arena), None);
        arena.begin_active_layout_pass();
        commit_from_layout(&arena);
        assert!(arena.leave_active_layout_pass());
        assert_eq!(current(&arena), Some(node));

        arena.data(node).fragment_cache_epoch.set(0);
        commit_from_layout(&arena);
        arena.data(node).fragment_cache_epoch.set(u32::MAX);
        arena.bump_fragment_cache_epoch_of_self_and_ancestors(node);
        assert_eq!(arena.data(node).fragment_cache_epoch.get(), 0);
        assert_eq!(current(&arena), None);
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
        arena
            .free_subtree(first.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        for allocation in allocations {
            arena
                .free_subtree(allocation.slot)
                .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
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
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn freed_slots_are_reused_with_a_new_generation() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate_for_test();
        arena
            .free_subtree(first.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());

        let second = arena.allocate_for_test();
        assert_eq!(second.slot.slot_index(), first.slot.slot_index());
        assert_ne!(second.slot, first.slot);
        assert_ne!(second.slot.generation(), first.slot.generation());
        arena
            .free_subtree(second.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
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

        arena
            .free_subtree(anchor.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
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
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        let positioned_slot_reoccupant = arena.allocate_for_test();
        assert_eq!(
            positioned_slot_reoccupant.slot.slot_index(),
            positioned.slot.slot_index()
        );
        arena.set_default_scroll_shift(positioned_slot_reoccupant.slot, anchor_slot_reoccupant.slot, true, true);
        arena
            .free_subtree(positioned_slot_reoccupant.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        let next_reoccupant = arena.allocate_for_test();
        assert!(arena.default_scroll_shift_anchor(next_reoccupant.slot).is_invalid());

        arena
            .free_subtree(next_reoccupant.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        arena
            .free_subtree(anchor_slot_reoccupant.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn default_scroll_shift_input_changes_note_the_positioned_box_dirty() {
        use crate::painting::visual_context::dirty::{BoxDirtyBits, VisualContextBoxDirtyKind};
        let mut arena = LayoutNodeArena::new();
        let positioned = arena.allocate_for_test();
        let anchor = arena.allocate_for_test();
        let other_anchor = arena.allocate_for_test();
        let take_dirty_bits = |arena: &LayoutNodeArena| -> Option<BoxDirtyBits> {
            let mut paint_state = arena.paint_state().borrow_mut();
            let bits = paint_state
                .visual_context
                .dirty_boxes
                .boxes
                .get(&positioned.slot)
                .copied();
            paint_state.visual_context.dirty_boxes.clear();
            bits
        };
        let notes_scroll_shift_change = |arena: &LayoutNodeArena| {
            take_dirty_bits(arena)
                .is_some_and(|bits| bits.contains(VisualContextBoxDirtyKind::DefaultScrollShiftInputsChanged))
        };

        arena.set_default_scroll_shift(positioned.slot, NodeSlotId::INVALID, false, false);
        assert!(!notes_scroll_shift_change(&arena));

        arena.set_default_scroll_shift(positioned.slot, anchor.slot, true, false);
        assert!(notes_scroll_shift_change(&arena));
        arena.set_default_scroll_shift(positioned.slot, anchor.slot, true, false);
        assert!(!notes_scroll_shift_change(&arena));

        arena.set_default_scroll_shift(positioned.slot, anchor.slot, true, true);
        assert!(notes_scroll_shift_change(&arena));
        arena.set_default_scroll_shift(positioned.slot, other_anchor.slot, true, true);
        assert!(notes_scroll_shift_change(&arena));
        arena.set_default_scroll_shift(positioned.slot, NodeSlotId::INVALID, false, false);
        assert!(notes_scroll_shift_change(&arena));

        let mut anchored_pairs = Vec::new();
        arena.set_default_scroll_shift(positioned.slot, anchor.slot, false, true);
        arena.for_each_default_scroll_shift_anchor(|positioned, anchor| anchored_pairs.push((positioned, anchor)));
        assert_eq!(anchored_pairs, vec![(positioned.slot, anchor.slot)]);
        arena
            .free_subtree(anchor.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        anchored_pairs.clear();
        arena.for_each_default_scroll_shift_anchor(|positioned, anchor| anchored_pairs.push((positioned, anchor)));
        assert!(anchored_pairs.is_empty());

        arena
            .free_subtree(positioned.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        arena
            .free_subtree(other_anchor.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn layout_update_flags_are_reset_only_in_the_requested_subtree() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test();
        let child = arena.allocate_for_test();
        let detached = arena.allocate_for_test();
        arena.write_shape(root.slot).set_kind(NodeKind::Viewport);
        arena.insert_child(root.slot, child.slot, NodeSlotId::INVALID);
        let update_flags = NodeFlag::NeedsLayoutUpdate as u32 | NodeFlag::NeedsOwnGeometryUpdate as u32;
        for node in [root.slot, child.slot, detached.slot] {
            arena.set_node_flag(node, NodeFlag::NeedsLayoutUpdate, true);
            arena.set_node_flag(node, NodeFlag::NeedsOwnGeometryUpdate, true);
        }

        arena.reset_layout_update_flags_in_subtree(child.slot);
        assert_eq!(arena.data(root.slot).flags.get() & update_flags, update_flags);
        assert_eq!(arena.data(child.slot).flags.get() & update_flags, 0);
        // An own-geometry update must be found even when its ancestors are clean.
        arena.reset_layout_update_flags_in_subtree(root.slot);
        arena.set_node_flag(child.slot, NodeFlag::NeedsOwnGeometryUpdate, true);
        arena.set_node_flag(child.slot, NodeFlag::NeedsOwnGeometryUpdate, true);
        arena.reset_layout_update_flags_in_subtree(root.slot);

        assert_eq!(arena.data(root.slot).flags.get() & update_flags, 0);
        assert_eq!(arena.data(child.slot).flags.get() & update_flags, 0);
        assert_eq!(arena.data(detached.slot).flags.get() & update_flags, update_flags);
        arena.remove_child(root.slot, child.slot);
        arena
            .free_subtree(root.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        arena
            .free_subtree(child.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        arena
            .free_subtree(detached.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn independent_partial_commits_remove_only_their_own_dirty_nodes() {
        let mut arena = LayoutNodeArena::new();
        let viewport = arena.allocate_for_test();
        arena.write_shape(viewport.slot).set_kind(NodeKind::Viewport);
        let mut boundaries = Vec::new();
        for _ in 0..32 {
            let boundary = arena.allocate_for_test();
            let child = arena.allocate_for_test();
            arena.insert_child(viewport.slot, boundary.slot, NodeSlotId::INVALID);
            arena.insert_child(boundary.slot, child.slot, NodeSlotId::INVALID);
            arena.set_node_flag(boundary.slot, NodeFlag::NeedsLayoutUpdate, true);
            arena.set_node_flag(child.slot, NodeFlag::NeedsOwnGeometryUpdate, true);
            boundaries.push((boundary, child));
        }
        for (index, (boundary, child)) in boundaries.iter().enumerate() {
            arena.reset_layout_update_flags_in_subtree(boundary.slot);
            assert_eq!(arena.data(boundary.slot).flags.get(), 0);
            assert_eq!(arena.data(child.slot).flags.get(), 0);
            assert_eq!(arena.nodes_with_layout_update_flags.borrow().len(), (31 - index) * 2);
            assert_eq!(arena.layout_update_flag_node_indices.borrow().len(), (31 - index) * 2);
            // Re-enrollment after removal must not leave duplicates or stale indices.
            arena.set_node_flag(child.slot, NodeFlag::NeedsLayoutUpdate, true);
            arena.set_node_flag(child.slot, NodeFlag::NeedsLayoutUpdate, false);
        }
        assert!(arena.nodes_with_layout_update_flags.borrow().is_empty());
        arena.set_node_flag(boundaries[0].0.slot, NodeFlag::NeedsLayoutUpdate, true);
        arena
            .free_subtree(viewport.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(arena.nodes_with_layout_update_flags.borrow().is_empty());
        assert!(arena.layout_update_flag_node_indices.borrow().is_empty());
    }

    #[test]
    fn full_commit_clears_only_attached_dirty_nodes() {
        let mut arena = LayoutNodeArena::new();
        let viewport = arena.allocate_for_test();
        arena.write_shape(viewport.slot).set_kind(NodeKind::Viewport);
        let detached = arena.allocate_for_test();
        let mut dirty_nodes = Vec::new();
        for root in [viewport.slot, detached.slot] {
            let mut parent = root;
            for _ in 0..64 {
                let child = arena.allocate_for_test();
                arena.insert_child(parent, child.slot, NodeSlotId::INVALID);
                dirty_nodes.push(child.slot);
                parent = child.slot;
            }
        }
        for &node in dirty_nodes.iter().rev() {
            arena.set_node_flag(node, NodeFlag::NeedsLayoutUpdate, true);
        }
        arena.reset_layout_update_flags_in_subtree(viewport.slot);
        for (index, &node) in dirty_nodes.iter().enumerate() {
            assert_eq!(
                arena.data(node).flags.get() & NodeFlag::NeedsLayoutUpdate as u32 != 0,
                index >= 64
            );
        }
        assert_eq!(arena.nodes_with_layout_update_flags.borrow().len(), 64);
        arena
            .free_subtree(viewport.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        arena
            .free_subtree(detached.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn stale_slot_ids_do_not_resolve_to_a_new_occupant() {
        let mut arena = LayoutNodeArena::new();
        let first = arena.allocate_for_test();
        arena
            .free_subtree(first.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        let second = arena.allocate_for_test();

        let stale_read = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| arena.data(first.slot)));
        assert!(stale_read.is_err());
        arena
            .free_subtree(second.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    fn test_abspos_layout_inputs() -> AbsposLayoutInputs {
        AbsposLayoutInputs {
            containing_block: NodeSlotId::INVALID,
            inline_containing_block: NodeSlotId::INVALID,
            static_position_rect: StaticPositionRect {
                rect: Default::default(),
                inline_alignment: StaticPositionAlignment::Center,
                block_alignment: StaticPositionAlignment::End,
                alignment_derives_from_own_computed_values: true,
                is_known: true,
            },
            containing_block_info: AbsposContainingBlockInfo {
                rect: Default::default(),
                inline_axis_mode: AbsposAxisMode::StaticPosition,
                block_axis_mode: AbsposAxisMode::InsetFromRect,
                inline_alignment: None,
                block_alignment: None,
                derives_from_own_computed_values: true,
            },
            resolved_anchor_insets: None,
        }
    }

    #[test]
    fn clearing_a_committed_box_evicts_its_fragment_link_and_abspos_inputs() {
        let mut arena = LayoutNodeArena::new();
        let allocation = arena.allocate_for_test();
        let inputs = test_abspos_layout_inputs();
        let mut link = fragment_tree::FragmentLink::for_test(allocation.slot);
        link.abspos_layout_inputs = Some(inputs);
        arena.set_committed_fragment_link(arena.data(allocation.slot), link, None);
        assert!(arena.committed_fragment_link(arena.data(allocation.slot)).is_some());
        assert_eq!(
            arena.saved_abspos_layout_inputs(arena.data(allocation.slot)),
            Some(inputs)
        );

        let work = crate::layout::tree_mutation::OwedHostWork::default();
        crate::painting::ffi::paintable_cleared_from_node(
            crate::layout::tree_mutation::HostCalls(&work),
            &mut arena,
            allocation.slot,
        );

        assert!(arena.committed_fragment_link(arena.data(allocation.slot)).is_none());
        assert_eq!(arena.saved_abspos_layout_inputs(arena.data(allocation.slot)), None);
        arena
            .free_subtree(allocation.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn committed_fragment_links_move_abspos_inputs_between_slots() {
        let mut arena = LayoutNodeArena::new();
        let old = arena.allocate_for_test();
        let new = arena.allocate_for_test();
        let inputs = test_abspos_layout_inputs();
        let mut link = fragment_tree::FragmentLink::for_test(old.slot);
        link.abspos_layout_inputs = Some(inputs);
        let retained_fragment = link.fragment.clone();
        arena.set_committed_fragment_link(arena.data(old.slot), link, None);

        let moved = arena
            .take_committed_fragment_link(arena.data(old.slot))
            .expect("old slot must retain its committed fragment");
        assert!(std::sync::Arc::ptr_eq(&moved.fragment, &retained_fragment));
        arena.set_committed_fragment_link(arena.data(new.slot), moved, None);

        assert!(arena.committed_fragment_link(arena.data(old.slot)).is_none());
        assert_eq!(arena.saved_abspos_layout_inputs(arena.data(old.slot)), None);
        assert_eq!(arena.saved_abspos_layout_inputs(arena.data(new.slot)), Some(inputs));
        let moved = arena
            .committed_fragment_link(arena.data(new.slot))
            .expect("new slot must receive the committed fragment");
        assert!(std::sync::Arc::ptr_eq(&moved.fragment, &retained_fragment));
        arena.set_committed_fragment_link(
            arena.data(new.slot),
            fragment_tree::FragmentLink::for_test(new.slot),
            None,
        );
        assert_eq!(arena.saved_abspos_layout_inputs(arena.data(new.slot)), None);
        arena
            .free_subtree(old.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        arena
            .free_subtree(new.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn intrinsic_size_cache_validates_epoch_and_generation() {
        let mut arena = LayoutNodeArena::new();
        let caches = super::IntrinsicSizeCaches::default();
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
                is_collapsed_borders_table_box: true,
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
        caches.intrinsic_block_size_cache_put(
            arena.intrinsic_size_cache_stamp(first.slot),
            IntrinsicSizeCacheKind::MinContentBlock,
            key,
            value,
        );
        caches.intrinsic_inline_size_measurement_cache_put(
            arena.intrinsic_size_cache_stamp(first.slot),
            IntrinsicSizeCacheKind::MaxContentInline,
            key,
            inline_measurement,
        );
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(first.slot),
                IntrinsicSizeCacheKind::MinContentBlock,
                key
            ),
            Some(value)
        );
        assert_eq!(
            caches.intrinsic_inline_size_measurement_cache_get(
                arena.intrinsic_size_cache_stamp(first.slot),
                IntrinsicSizeCacheKind::MaxContentInline,
                key
            ),
            Some(inline_measurement)
        );
        assert!(caches.intrinsic_inline_size_depends_on_block_size(
            arena.intrinsic_size_cache_stamp(first.slot),
            || {
                dependency_computations.set(dependency_computations.get() + 1);
                true
            }
        ));
        assert!(
            caches.intrinsic_inline_size_depends_on_block_size(arena.intrinsic_size_cache_stamp(first.slot), || false)
        );
        assert_eq!(dependency_computations.get(), 1);

        first_data
            .intrinsic_cache_epoch
            .set(first_data.intrinsic_cache_epoch.get() + 1);
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(first.slot),
                IntrinsicSizeCacheKind::MinContentBlock,
                key
            ),
            None
        );
        assert_eq!(
            caches.intrinsic_inline_size_measurement_cache_get(
                arena.intrinsic_size_cache_stamp(first.slot),
                IntrinsicSizeCacheKind::MaxContentInline,
                key
            ),
            None
        );
        assert!(!caches.intrinsic_inline_size_depends_on_block_size(
            arena.intrinsic_size_cache_stamp(first.slot),
            || {
                dependency_computations.set(dependency_computations.get() + 1);
                false
            }
        ));
        assert_eq!(dependency_computations.get(), 2);
        arena
            .free_subtree(first.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());

        let second = arena.allocate_for_test();
        assert_eq!(second.slot.slot_index(), first.slot.slot_index());
        assert_ne!(second.slot, first.slot);
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(second.slot),
                IntrinsicSizeCacheKind::MinContentBlock,
                key
            ),
            None
        );
        assert_eq!(
            caches.intrinsic_inline_size_measurement_cache_get(
                arena.intrinsic_size_cache_stamp(second.slot),
                IntrinsicSizeCacheKind::MaxContentInline,
                key
            ),
            None
        );
        arena
            .free_subtree(second.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn intrinsic_size_cache_answers_masked_probes_only_from_independent_measurements() {
        let mut arena = LayoutNodeArena::new();
        let caches = super::IntrinsicSizeCaches::default();
        let allocation = arena.allocate_for_test();
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
        caches.intrinsic_block_size_cache_put(
            arena.intrinsic_size_cache_stamp(allocation.slot),
            max_content,
            key_with_basis(100),
            independent,
        );
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(allocation.slot),
                max_content,
                key_with_basis(100)
            ),
            Some(independent)
        );
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(allocation.slot),
                max_content,
                key_with_basis(200)
            ),
            Some(independent)
        );
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(allocation.slot),
                max_content,
                key_without_basis
            ),
            Some(independent)
        );

        let dependent = IntrinsicBlockSizeMeasurement {
            size: CssPixels::from_raw(256),
            depends_on_percentage_block_size: true,
            depends_on_percentage_inline_basis: false,
        };
        caches.intrinsic_block_size_cache_put(
            arena.intrinsic_size_cache_stamp(allocation.slot),
            min_content,
            key_without_basis,
            dependent,
        );
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(allocation.slot),
                min_content,
                key_without_basis
            ),
            Some(dependent)
        );
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(allocation.slot),
                min_content,
                key_with_basis(100)
            ),
            None
        );
        caches.intrinsic_block_size_cache_put(
            arena.intrinsic_size_cache_stamp(allocation.slot),
            min_content,
            key_with_basis(100),
            dependent,
        );
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(allocation.slot),
                min_content,
                key_with_basis(100)
            ),
            Some(dependent)
        );
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(allocation.slot),
                min_content,
                key_with_basis(200)
            ),
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
        caches.intrinsic_block_size_cache_put(
            arena.intrinsic_size_cache_stamp(allocation.slot),
            max_content,
            key_at_another_inline_size_with_inline_basis(300),
            observes_inline_basis,
        );
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(allocation.slot),
                max_content,
                key_at_another_inline_size_with_inline_basis(300)
            ),
            Some(observes_inline_basis)
        );
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(allocation.slot),
                max_content,
                IntrinsicSizeCacheKey {
                    percentage_basis_block_size: Some(CssPixels::from_raw(200)),
                    ..key_at_another_inline_size_with_inline_basis(300)
                }
            ),
            Some(observes_inline_basis)
        );
        assert_eq!(
            caches.intrinsic_block_size_cache_get(
                arena.intrinsic_size_cache_stamp(allocation.slot),
                max_content,
                key_at_another_inline_size_with_inline_basis(400)
            ),
            None
        );
        arena
            .free_subtree(allocation.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn table_cell_measurements_follow_the_intrinsic_cache_epoch() {
        let mut arena = LayoutNodeArena::new();
        let caches = super::IntrinsicSizeCaches::default();
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
            depends_on_percentage_block_size: true,
        };

        let first_data = arena.data(first.slot);
        assert_eq!(
            caches.table_cell_measurement_cache_get(arena.intrinsic_size_cache_stamp(first.slot), key),
            None
        );
        caches.table_cell_measurement_cache_put(arena.intrinsic_size_cache_stamp(first.slot), key, value);
        assert_eq!(
            caches.table_cell_measurement_cache_get(arena.intrinsic_size_cache_stamp(first.slot), key),
            Some(value)
        );
        let percentage_resolved_key = TableCellMeasurementKey {
            content_block_size: CssPixels::from_raw(512),
            has_definite_block_size: true,
            adopt_automatic_content_block_size: false,
            ..key
        };
        assert_eq!(
            caches.table_cell_measurement_cache_get(
                arena.intrinsic_size_cache_stamp(first.slot),
                percentage_resolved_key
            ),
            None
        );

        first_data
            .intrinsic_cache_epoch
            .set(first_data.intrinsic_cache_epoch.get() + 1);
        assert_eq!(
            caches.table_cell_measurement_cache_get(arena.intrinsic_size_cache_stamp(first.slot), key),
            None
        );
        arena
            .free_subtree(first.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());

        let second = arena.allocate_for_test();
        assert_eq!(second.slot.slot_index(), first.slot.slot_index());
        assert_eq!(
            caches.table_cell_measurement_cache_get(arena.intrinsic_size_cache_stamp(second.slot), key),
            None
        );
        arena
            .free_subtree(second.slot)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }
}
