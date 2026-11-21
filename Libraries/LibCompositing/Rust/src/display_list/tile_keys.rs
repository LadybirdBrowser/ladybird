/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The content hashes of the tiles of the layers of one band, which alone decide whether a tile is rastered again.
//!
//! A tile's content hash covers what it holds: the records of the band that can show in it, as the geometry of the
//! band's raster space places their visual contexts, and the frame state those records read. A chunk is placed once for
//! each placement of the contexts of its runs, and the placement is cached on the chunk for every list that names it. A
//! band bound to another list or geometry bins its tiles again, and a tile that holds what it held before gets the
//! content hash it had before.

use super::builder::{inline_clips_of, read_command};
use super::commands::{
    ClipNodeIndex, ContextRef, DisplayListCommandRun, DrawCompositedContext, EffectNodeIndex, PaintCaret,
    PaintScrollBar, SpatialNodeIndex,
};
use super::effect_clip_plan::EffectClipPlan;
use super::effect_geometry::{
    FiltersOfEffects, LARGEST_FILTER_READ_REACH_IN_DEVICE_PIXELS, backdrop_filter_read_reach,
    filter_read_reach_in_device_pixels, nearest_effects_at_or_above_where, spreading_filter_of,
};
use super::layers::{BandIndex, BarrierCutsOfCell, CutsOfCell, LayerIndex, LayerStack, PlacementNodeBand};
use super::nested_records::for_each_command_including_nested;
use super::placement::{
    ANTIALIASING_MARGIN_IN_DEVICE_PIXELS, FiltersAndClipsOfList, RasterSpaceGeometry, RunGroupIndex, RunGroups,
    enclosing_int_rect_within_extent_limit, is_finite_rect,
};
use super::reach::Reach;
use super::replay::{ScrollbarShownByFrame, caret_is_visible_at_time, scrollbar_shown_by_frame};
use super::storage::{ChunkIndexInList, DisplayListStorage, ListChunk, ListPosition, ListSpan, RunIndexInChunk};
use super::summary::{LiveContent, live_content_of};
use super::tiles::{CellRange, TileCoord, entries_within, for_each_entry_within};
use super::window_server_placement::RoundedRect;
use crate::display_list::commands::{CanvasId, CompositorContextId, ImageFrameResourceId, VideoSinkHandle};
use crate::visual_context::{
    ClipData, ClipNodeData, EffectNodeData, VisualContextTree, WorldTransforms, device_offset_for_index,
};
use foldhash::fast::{FixedState, FoldHasher};
use libgfx_rust::{Color, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, IntRect};
use std::cell::{Cell, OnceCell, RefCell};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::hash::{BuildHasher, Hasher};
use std::sync::Arc;

mod chunk_placement;
mod coverage;

pub(crate) use chunk_placement::ChunkRecord;
pub use chunk_placement::{ChunkPlacedInRasterSpace, PlacedRecordRangeIndex, RecordIndexInChunk, RecordTable};
use chunk_placement::{
    FRAME_STATE_CARET_BLINK_PHASE, FRAME_STATE_LIVE_CONTENT, FRAME_STATE_SAMPLED_BACKGROUND_COLOR,
    FRAME_STATE_SCROLL_BAR, FRAME_STATE_SPACES_OF_INLINE_CLIPS, PlacedRecordRange,
    background_color_animation_effect_of,
};
pub use coverage::TileCoverage;

pub type ContentHash = u128;

const LOW_HALF_SEED: u64 = 0x243f_6a88_85a3_08d3;
const HIGH_HALF_SEED: u64 = 0x1319_8a2e_0370_7344;
// The farthest past its cell that a tile is rastered, in device pixels. A tile whose backdrop filters read farther takes
// how far as not known, which keeps the rects that rastering inflates by it within the limits of an `IntRect`.
const LARGEST_RASTER_MARGIN_IN_DEVICE_PIXELS: i32 = 4 * LARGEST_FILTER_READ_REACH_IN_DEVICE_PIXELS as i32;

struct ContentHasher {
    low: FoldHasher<'static>,
    high: FoldHasher<'static>,
}

impl ContentHasher {
    fn new() -> Self {
        Self {
            low: FixedState::with_seed(LOW_HALF_SEED).build_hasher(),
            high: FixedState::with_seed(HIGH_HALF_SEED).build_hasher(),
        }
    }

    fn write_bytes(&mut self, bytes: &[u8]) {
        self.write_u64(bytes.len() as u64);
        self.low.write(bytes);
        self.high.write(bytes);
    }

    fn write_u64(&mut self, value: u64) {
        self.low.write_u64(value);
        self.high.write_u64(value);
    }

    fn write_u32(&mut self, value: u32) {
        self.low.write_u32(value);
        self.high.write_u32(value);
    }

    fn write_i32(&mut self, value: i32) {
        self.write_u32(value as u32);
    }

    fn write_bool(&mut self, value: bool) {
        self.write_u32(u32::from(value));
    }

    fn write_f32(&mut self, value: f32) {
        self.write_u32(value.to_bits());
    }

    fn write_content_hash(&mut self, content_hash: ContentHash) {
        self.write_u64(content_hash as u64);
        self.write_u64((content_hash >> 64) as u64);
    }

    fn write_int_rect(&mut self, rect: IntRect) {
        self.write_i32(rect.x);
        self.write_i32(rect.y);
        self.write_i32(rect.width);
        self.write_i32(rect.height);
    }

    fn write_float_rect(&mut self, rect: FloatRect) {
        self.write_f32(rect.x);
        self.write_f32(rect.y);
        self.write_f32(rect.width);
        self.write_f32(rect.height);
    }

    fn write_float_point(&mut self, point: FloatPoint) {
        self.write_f32(point.x);
        self.write_f32(point.y);
    }

    fn write_matrix(&mut self, matrix: &FloatMatrix4x4) {
        for row in &matrix.elements {
            for value in row {
                self.write_f32(*value);
            }
        }
    }

    fn write_corner_radii(&mut self, corner_radii: &libgfx_rust::CornerRadii) {
        for corner in [
            corner_radii.top_left,
            corner_radii.top_right,
            corner_radii.bottom_right,
            corner_radii.bottom_left,
        ] {
            self.write_i32(corner.horizontal_radius);
            self.write_i32(corner.vertical_radius);
        }
    }

    fn finish(&self) -> ContentHash {
        (u128::from(self.high.finish()) << 64) | u128::from(self.low.finish())
    }
}

pub trait LiveContentVersions {
    fn canvas_content_generation(&self, canvas: CanvasId) -> u64;
    fn video_frame_id(&self, video_sink: VideoSinkHandle) -> Option<u64>;
    fn image_frame_pixel_generation(&self, image_frame: ImageFrameResourceId) -> Option<u64>;
    fn nested_context_frame_generation(&self, nested_context: CompositorContextId) -> Option<u64>;
}

#[derive(Clone, Copy)]
pub struct FrameStateReadByReplay<'a> {
    pub scrollbars: &'a [ScrollbarShownByFrame],
    pub time_ns: i64,
    pub live_content: &'a dyn LiveContentVersions,
}

#[derive(Clone, Copy)]
pub struct LayerRasterSettings {
    pub raster_space_identity: u64,
    pub clear_color: Color,
    pub root_group_color: Color,
    pub viewport_to_raster_transform: FloatMatrix4x4,
    pub allows_lcd_text: bool,
}

impl LayerRasterSettings {
    pub fn content_hash(&self) -> ContentHash {
        let mut hasher = ContentHasher::new();
        hasher.write_u64(self.raster_space_identity);
        hasher.write_u32(self.clear_color.0);
        hasher.write_u32(self.root_group_color.0);
        hasher.write_matrix(&self.viewport_to_raster_transform);
        hasher.write_bool(self.allows_lcd_text);
        hasher.finish()
    }
}

pub struct TileKeyingOfList<'a> {
    display_list: &'a DisplayListStorage,
    effect_clips: &'a EffectClipPlan,
    tree: &'a VisualContextTree,
    frame_state: FrameStateReadByReplay<'a>,
    // Worked out on first use, as most frames bind no band and key no tile.
    run_groups: OnceCell<Arc<RunGroups>>,
    filters_of_effects: OnceCell<FiltersOfEffects>,
    filters_inside_composited_effects: OnceCell<FiltersOfEffects>,
    replays_every_record_into_every_tile: bool,
    tree_sorts_planes_by_depth: bool,
    frame_state_hash: ContentHash,
    innermost_composited_effect_by_effect_index: Vec<EffectNodeIndex>,
    /// Per effect, the nearest effect at or around it that the compositor composites or that is not a plain group,
    /// once the coverage of a tile asks.
    nearest_composited_or_not_plain_effect_by_effect_index: OnceCell<Vec<EffectNodeIndex>>,
    /// Per effect, the nearest effect at or around it that the compositor composites or whose blend mode can lower the
    /// alpha of what is below it, once the coverage of a tile asks.
    nearest_composited_or_alpha_lowering_effect_by_effect_index: OnceCell<Vec<EffectNodeIndex>>,
}

impl<'a> TileKeyingOfList<'a> {
    /// Prepares keying the tiles of a frame's list, of which `stack` was worked out.
    pub fn new(
        display_list: &'a DisplayListStorage,
        tree: &'a VisualContextTree,
        stack: &LayerStack,
        frame_state: FrameStateReadByReplay<'a>,
    ) -> Self {
        let layering_obstacles = stack.layering_obstacles;
        let mut is_composited_by_effect_index = vec![false; tree.effect_nodes.len()];
        for composited in &stack.composited_effects {
            is_composited_by_effect_index[composited.effect.0 as usize] = true;
        }
        let innermost_composited_effect_by_effect_index =
            nearest_effects_at_or_above_where(tree, |effect, _| is_composited_by_effect_index[effect.0 as usize]);
        Self {
            display_list,
            effect_clips: display_list.effect_clip_plan(tree),
            tree,
            frame_state,
            run_groups: OnceCell::new(),
            filters_of_effects: OnceCell::new(),
            filters_inside_composited_effects: OnceCell::new(),
            replays_every_record_into_every_tile: layering_obstacles.every_tile_replays_every_record()
                || stack.one_layer_list_cannot_tell_which_records_show_in_tile(),
            tree_sorts_planes_by_depth: layering_obstacles.sorts_planes_by_depth,
            frame_state_hash: frame_state_hash_of(display_list, &frame_state),
            innermost_composited_effect_by_effect_index,
            nearest_composited_or_not_plain_effect_by_effect_index: OnceCell::new(),
            nearest_composited_or_alpha_lowering_effect_by_effect_index: OnceCell::new(),
        }
    }

    fn run_groups(&self) -> &RunGroups {
        self.run_groups.get_or_init(|| self.display_list.run_groups(self.tree))
    }

    fn filters_of_effects(&self) -> &FiltersOfEffects {
        self.filters_of_effects
            .get_or_init(|| FiltersOfEffects::of(self.tree, None, false))
    }

    fn filters_inside_composited_effects(&self) -> &FiltersOfEffects {
        self.filters_inside_composited_effects.get_or_init(|| {
            FiltersOfEffects::of(
                self.tree,
                Some(&self.innermost_composited_effect_by_effect_index),
                false,
            )
        })
    }

    fn compositor_composites(&self, effect: EffectNodeIndex) -> bool {
        self.innermost_composited_effect_by_effect_index[effect.0 as usize] == effect
    }
}

fn version_of_live_content(live_content: &dyn LiveContentVersions, content: LiveContent) -> u64 {
    match content {
        LiveContent::Canvas(canvas) => live_content.canvas_content_generation(canvas),
        LiveContent::VideoFrame(video_sink) => live_content.video_frame_id(video_sink).unwrap_or(u64::MAX),
        LiveContent::ImageFrame(image_frame) => live_content
            .image_frame_pixel_generation(image_frame)
            .unwrap_or(u64::MAX),
        LiveContent::CompositedContext(nested_context) => live_content
            .nested_context_frame_generation(nested_context)
            .unwrap_or(u64::MAX),
    }
}

fn write_scrollbar_shown_by_frame(hasher: &mut ContentHasher, scrollbar: &ScrollbarShownByFrame) {
    hasher.write_float_rect(scrollbar.gutter_rect);
    hasher.write_float_rect(scrollbar.thumb_rect);
    hasher.write_f32(scrollbar.expansion);
    hasher.write_f32(scrollbar.opacity);
}

fn frame_state_hash_of(display_list: &DisplayListStorage, frame_state: &FrameStateReadByReplay<'_>) -> ContentHash {
    let summary = display_list.summary();
    let mut hasher = ContentHasher::new();
    for caret in &summary.carets {
        let (_, payload) = display_list.record_at(caret.position);
        hasher.write_bool(caret_is_visible_at_time(
            &read_command::<PaintCaret>(payload),
            frame_state.time_ns,
        ));
    }
    for scrollbar in frame_state.scrollbars {
        hasher.write_u32(scrollbar.scroll_node_index.0);
        hasher.write_bool(scrollbar.vertical);
        write_scrollbar_shown_by_frame(&mut hasher, scrollbar);
    }
    for live_content_record in &summary.live_content_records {
        hasher.write_u64(version_of_live_content(
            frame_state.live_content,
            live_content_record.content,
        ));
    }
    hasher.finish()
}

struct BackdropReadInLayer {
    first_record_position: ListPosition,
    region_reach: Reach<IntRect>,
    backdrop_read_reach: Reach<IntRect>,
    read_reach_in_device_pixels: Option<i32>,
    spread_past_region_in_device_pixels: i32,
    // A mirroring blur stops at the frame's edge where its region reaches past the frame, as a raster of the frame
    // does.
    mirroring_blur_region: Option<Reach<IntRect>>,
}

impl BackdropReadInLayer {
    fn blur_mirrors_at_edge_of_frame(&self, frame: IntRect) -> bool {
        match self.mirroring_blur_region {
            None | Some(Reach::Nothing) => false,
            Some(Reach::Within(region)) => !frame.contains_rect(region),
            Some(Reach::Anywhere) => true,
        }
    }
}

// The most backdrop reads that a band follows one by one, and the most passes over them that follow the reach of
// content through reads that read one another. Past either, content that a backdrop read can reach reaches the regions
// of every backdrop read of the band, and the cells those regions touch take what their tiles read as not known.
const MAXIMUM_BACKDROP_READS_A_BAND_FOLLOWS_ONE_BY_ONE: usize = 64;
const MAXIMUM_PASSES_OVER_BACKDROP_READS: usize = 8;

/// The backdrop reads of the layers of a band, in the paint order of their first records.
struct BackdropReadsOfBand {
    reads_in_paint_order: Vec<BackdropReadInLayer>,
    region_reach_of_every_read: Reach<IntRect>,
}

impl BackdropReadsOfBand {
    fn new(reads_in_paint_order: Vec<BackdropReadInLayer>) -> Self {
        debug_assert!(
            reads_in_paint_order
                .windows(2)
                .all(|pair| pair[0].first_record_position <= pair[1].first_record_position)
        );
        let region_reach_of_every_read = reads_in_paint_order
            .iter()
            .fold(Reach::Nothing, |united, read| united.united(read.region_reach));
        Self {
            reads_in_paint_order,
            region_reach_of_every_read,
        }
    }

    fn is_empty(&self) -> bool {
        self.reads_in_paint_order.is_empty()
    }

    fn are_followed_one_by_one(&self) -> bool {
        self.reads_in_paint_order.len() <= MAXIMUM_BACKDROP_READS_A_BAND_FOLLOWS_ONE_BY_ONE
    }

    // A tile that shows part of a backdrop filter's region must also hold what the filter reads past that part.
    fn reach_extended_through_reads(&self, mut reach: Reach<IntRect>, paint_position: ListPosition) -> Reach<IntRect> {
        let first_read_painted_after = self
            .reads_in_paint_order
            .partition_point(|read| read.first_record_position <= paint_position);
        let reads_painted_after = &self.reads_in_paint_order[first_read_painted_after..];
        if reads_painted_after.is_empty() {
            return reach;
        }
        if !self.are_followed_one_by_one() {
            return reach.united(self.region_reach_of_every_read);
        }
        let pass_count = self.reads_in_paint_order.len() + 1;
        for _ in 0..pass_count.min(MAXIMUM_PASSES_OVER_BACKDROP_READS) {
            let mut reach_grew = false;
            for backdrop_read in reads_painted_after {
                let Reach::Within(rect) = reach else {
                    continue;
                };
                if !backdrop_read.backdrop_read_reach.touches(rect) {
                    continue;
                }
                let reach_through_backdrop_read =
                    match (backdrop_read.region_reach, backdrop_read.read_reach_in_device_pixels) {
                        (Reach::Within(region), Some(read_reach_in_device_pixels)) => {
                            let distance =
                                read_reach_in_device_pixels + backdrop_read.spread_past_region_in_device_pixels;
                            Reach::Within(
                                rect.inflated_edges(distance, distance, distance, distance)
                                    .intersected(region),
                            )
                        }
                        (region_reach, _) => region_reach,
                    };
                let extended = reach.united(reach_through_backdrop_read);
                if extended != reach {
                    reach = extended;
                    reach_grew = true;
                }
            }
            if !reach_grew {
                return reach;
            }
        }
        if pass_count > MAXIMUM_PASSES_OVER_BACKDROP_READS {
            return reach.united(self.region_reach_of_every_read);
        }
        reach
    }

    /// How far past `cell` its tiles are rastered, in device pixels: as far as the backdrop filters whose regions touch
    /// the cell read and spread what they read, or none where that is not known or is farther than
    /// `LARGEST_RASTER_MARGIN_IN_DEVICE_PIXELS`.
    fn raster_margin_of_cell(&self, cell: IntRect) -> Option<i32> {
        if !self.are_followed_one_by_one() {
            return (!self.region_reach_of_every_read.touches(cell)).then_some(0);
        }
        self.reads_in_paint_order
            .iter()
            .filter(|backdrop_read| backdrop_read.region_reach.touches(cell))
            .try_fold(0i32, |margin, backdrop_read| {
                Some(
                    margin
                        .saturating_add(backdrop_read.read_reach_in_device_pixels?)
                        .saturating_add(backdrop_read.spread_past_region_in_device_pixels),
                )
            })
            .filter(|&margin| margin <= LARGEST_RASTER_MARGIN_IN_DEVICE_PIXELS)
    }

    /// The frame where the blurs of some backdrop read of the band mirror at its edge, in any cell.
    fn frame_edge_blurs_mirror_at(&self, frame: Option<IntRect>) -> Option<IntRect> {
        frame.filter(|frame| {
            if !self.are_followed_one_by_one() {
                return self.region_reach_of_every_read != Reach::Nothing;
            }
            self.reads_in_paint_order
                .iter()
                .any(|backdrop_read| backdrop_read.blur_mirrors_at_edge_of_frame(*frame))
        })
    }

    fn frame_edge_blurs_of_cell_mirror_at(&self, cell: IntRect, frame: Option<IntRect>) -> Option<IntRect> {
        frame.filter(|frame| {
            if !self.are_followed_one_by_one() {
                return self.region_reach_of_every_read.touches(cell);
            }
            self.reads_in_paint_order.iter().any(|backdrop_read| {
                backdrop_read.region_reach.touches(cell) && backdrop_read.blur_mirrors_at_edge_of_frame(*frame)
            })
        })
    }
}

impl RoundedRect {
    /// The rect in device pixels, where its space maps onto them by a scale and a translation.
    fn device_aligned_of(
        world: &WorldTransforms,
        spatial: SpatialNodeIndex,
        rect: FloatRect,
        corner_radii: &CornerRadii,
    ) -> Option<Self> {
        let matrix = world.draw_matrix(spatial);
        if !matrix.is_2d_affine() {
            return None;
        }
        let affine = matrix.extract_2d_affine();
        if affine.b() != 0.0 || affine.c() != 0.0 || affine.a() <= 0.0 || affine.d() <= 0.0 {
            return None;
        }
        let rect = world.map_rect_to_root(spatial, rect);
        if !is_finite_rect(rect) {
            return None;
        }
        let radius_in_device_pixels = |radius: &libgfx_rust::CornerRadius| {
            (
                radius.horizontal_radius as f32 * affine.a(),
                radius.vertical_radius as f32 * affine.d(),
            )
        };
        Some(Self {
            rect,
            horizontal_and_vertical_radii_clockwise_from_top_left: [
                radius_in_device_pixels(&corner_radii.top_left),
                radius_in_device_pixels(&corner_radii.top_right),
                radius_in_device_pixels(&corner_radii.bottom_right),
                radius_in_device_pixels(&corner_radii.bottom_left),
            ],
        })
    }

    fn write_part_within_or_whole_if_corner_reaches_in(&self, hasher: &mut ContentHasher, area: FloatRect) {
        for (horizontal, vertical) in self.horizontal_and_vertical_radii_clockwise_from_top_left {
            hasher.write_f32(horizontal);
            hasher.write_f32(vertical);
        }
        let rect = self.rect;
        if self.rounded_corner_reaches_into(area) {
            hasher.write_u32(1);
            hasher.write_float_rect(rect);
            return;
        }
        let part = rect.intersected(area);
        if part.is_empty() {
            hasher.write_u32(2);
            return;
        }
        hasher.write_u32(3);
        hasher.write_float_rect(part);
    }
}

struct ContentHashesNearArea {
    area: FloatRect,
    content_hash_by_clip: HashMap<u32, ContentHash>,
    content_hash_by_effect: HashMap<u32, ContentHash>,
}

impl ContentHashesNearArea {
    /// The area that the tiles of the cell at `coord` raster within, where their raster margin is known.
    fn of_cell(coord: TileCoord, raster_margin_in_device_pixels: Option<i32>) -> Option<Self> {
        raster_margin_in_device_pixels.map(|backdrop_read_margin| {
            let margin = 2.0 * (ANTIALIASING_MARGIN_IN_DEVICE_PIXELS + backdrop_read_margin) as f32;
            Self {
                area: coord.cell_rect().to_float().inflated(margin, margin),
                content_hash_by_clip: HashMap::new(),
                content_hash_by_effect: HashMap::new(),
            }
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
enum TileReplayItem {
    PlacedRecordRange {
        chunk_index: ChunkIndexInList,
        range_index: PlacedRecordRangeIndex,
    },
    // The records of a span of a chunk: one that every tile replays, or a record that no placed record range of the band
    // holds, replayed for the mask content it declares.
    RecordSpan {
        chunk_index: ChunkIndexInList,
        begin_offset: u32,
        end_offset: u32,
    },
}

impl TileReplayItem {
    fn chunk_index(&self) -> ChunkIndexInList {
        match *self {
            TileReplayItem::PlacedRecordRange { chunk_index, .. } | TileReplayItem::RecordSpan { chunk_index, .. } => {
                chunk_index
            }
        }
    }
}

/// A tile of a layer of a band, with the hash of what it holds, the replay items of the band replayed into it, and how
/// far past its cell content is read to raster it.
pub struct KeyedTile {
    pub layer_index: LayerIndex,
    pub coord: TileCoord,
    pub content_hash: ContentHash,
    pub raster_margin_in_device_pixels: Option<i32>,
    /// Which barriers cut the tile: tiles of a layer that are cut alike hold every replay item that reaches them alike,
    /// so one replay can raster them together.
    pub cuts: BarrierCutsOfCell,
    /// The part of the tile's cell that what it holds can reach.
    pub content_bounds: IntRect,
    replay_items_in_paint_order: Vec<TileReplayItem>,
}

/// A chunk that the spans of a band hold runs of. The spans hold whole runs.
struct ChunkHeldByBand {
    chunk_index: ChunkIndexInList,
    placement: Arc<ChunkPlacedInRasterSpace>,
    held_run_indices: Vec<RunIndexInChunk>,
    reach_of_held_records: Reach<IntRect>,
}

struct RunReplayedWholeWhereverRunGroupShows {
    reach: Reach<IntRect>,
    /// The innermost mask around the run with declared content, whose declarations, and those of the declared masks
    /// around it, replay with the run, or `EffectNodeIndex::NONE`.
    innermost_declared_mask_around: EffectNodeIndex,
}

/// A mask with declared content around a run that a band replays whole.
struct DeclaredMaskAroundRunReplayedWhole {
    /// The items that replay its declarations, each with where it is painted and how far it reaches.
    declarations: Vec<(ListPosition, TileReplayItem, Reach<IntRect>)>,
    next_declared_mask_around: EffectNodeIndex,
}

type RunsReplayedWhole = HashMap<(ChunkIndexInList, RunIndexInChunk), RunReplayedWholeWhereverRunGroupShows>;

/// The masks with declared content, for a band that replays runs whole: where the list declares the content of each, and
/// per effect the nearest such mask at or around it.
struct MasksWithDeclaredContent {
    declaration_positions_by_mask: HashMap<EffectNodeIndex, Vec<ListPosition>>,
    nearest_declared_mask_by_effect_index: Vec<EffectNodeIndex>,
}

impl MasksWithDeclaredContent {
    fn nearest_declared_mask_at_or_above(&self, effect: EffectNodeIndex) -> EffectNodeIndex {
        self.nearest_declared_mask_by_effect_index
            .get(effect.0 as usize)
            .copied()
            .unwrap_or(EffectNodeIndex::NONE)
    }
}

struct BandBoundToListAndGeometry {
    /// In the order of their chunks, as the spans of a band are in paint order.
    held_chunks: Vec<ChunkHeldByBand>,
    runs_replayed_whole: RunsReplayedWhole,
    declared_masks_around_runs_replayed_whole: HashMap<EffectNodeIndex, DeclaredMaskAroundRunReplayedWhole>,
    backdrop_reads: BackdropReadsOfBand,
    items_replayed_in_every_tile: Option<Vec<(ListPosition, TileReplayItem)>>,
    replays_records_reading_frame_state_hashed_for_the_list: bool,
}

impl BandBoundToListAndGeometry {
    fn held_chunk_if_held(&self, chunk_index: ChunkIndexInList) -> Option<&ChunkHeldByBand> {
        self.held_chunks
            .binary_search_by_key(&chunk_index, |held_chunk| held_chunk.chunk_index)
            .ok()
            .map(|held_chunk_index| &self.held_chunks[held_chunk_index])
    }

    fn held_chunk(&self, chunk_index: ChunkIndexInList) -> &ChunkHeldByBand {
        self.held_chunk_if_held(chunk_index)
            .expect("an item of a band names a chunk the band holds")
    }

    fn placed_record_range(
        &self,
        chunk_index: ChunkIndexInList,
        range_index: PlacedRecordRangeIndex,
    ) -> &PlacedRecordRange {
        &self.held_chunk(chunk_index).placement.record_ranges[range_index.0 as usize]
    }

    fn reach_of_item(&self, item: TileReplayItem) -> Reach<IntRect> {
        let TileReplayItem::PlacedRecordRange {
            chunk_index,
            range_index,
        } = item
        else {
            return Reach::Nothing;
        };
        let held_chunk = self.held_chunk(chunk_index);
        let range = &held_chunk.placement.record_ranges[range_index.0 as usize];
        if let Some(run_replayed_whole) = self.runs_replayed_whole.get(&(chunk_index, range.run_index))
            && held_chunk.placement.ranges_of_each_run[range.run_index.0 as usize].whole_run_range_index == range_index
        {
            return run_replayed_whole.reach;
        }
        self.backdrop_reads
            .reach_extended_through_reads(range.reach, ListPosition::new(chunk_index, range.begin_offset))
    }

    fn span_of(&self, item: TileReplayItem) -> ListSpan {
        let (chunk_index, begin_offset, end_offset) = match item {
            TileReplayItem::PlacedRecordRange {
                chunk_index,
                range_index,
            } => {
                let range = self.placed_record_range(chunk_index, range_index);
                (chunk_index, range.begin_offset, range.end_offset)
            }
            TileReplayItem::RecordSpan {
                chunk_index,
                begin_offset,
                end_offset,
            } => (chunk_index, begin_offset, end_offset),
        };
        ListSpan {
            begin: ListPosition::new(chunk_index, begin_offset),
            end: ListPosition::new(chunk_index, end_offset),
        }
    }

    fn item_replaying_record(
        &self,
        display_list: &DisplayListStorage,
        chunk_index: ChunkIndexInList,
        record_index: RecordIndexInChunk,
    ) -> TileReplayItem {
        let table = display_list.chunk(chunk_index).record_table();
        let record = table.record(record_index);
        let record_alone = TileReplayItem::RecordSpan {
            chunk_index,
            begin_offset: record.offset,
            end_offset: record.end_offset,
        };
        let Some(held_chunk) = self.held_chunk_if_held(chunk_index) else {
            return record_alone;
        };
        let run_index = record.run_index;
        if held_chunk.held_run_indices.binary_search(&run_index).is_err() {
            return record_alone;
        }
        let ranges_of_run = &held_chunk.placement.ranges_of_each_run[run_index.0 as usize];
        if self.runs_replayed_whole.contains_key(&(chunk_index, run_index)) {
            return TileReplayItem::PlacedRecordRange {
                chunk_index,
                range_index: ranges_of_run.whole_run_range_index,
            };
        }
        // The binned ranges of a run are in the order of their records and apart.
        let first_binned_range_index = ranges_of_run.first_binned_range_index;
        let binned_ranges = &held_chunk.placement.record_ranges
            [first_binned_range_index.0 as usize..ranges_of_run.whole_run_range_index.0 as usize];
        let position_among_binned_ranges =
            binned_ranges.partition_point(|range| range.end_record_index <= record_index);
        match binned_ranges.get(position_among_binned_ranges) {
            Some(range) if range.first_record_index <= record_index => TileReplayItem::PlacedRecordRange {
                chunk_index,
                range_index: PlacedRecordRangeIndex(first_binned_range_index.0 + position_among_binned_ranges as u32),
            },
            _ => record_alone,
        }
    }

    fn for_each_held_item(
        &self,
        held_chunk: &ChunkHeldByBand,
        mut visit: impl FnMut(ListPosition, TileReplayItem, Reach<IntRect>, EffectNodeIndex),
    ) {
        for &run_index in &held_chunk.held_run_indices {
            let ranges_of_run = &held_chunk.placement.ranges_of_each_run[run_index.0 as usize];
            if let Some(run_replayed_whole) = self.runs_replayed_whole.get(&(held_chunk.chunk_index, run_index)) {
                let whole_run_range =
                    &held_chunk.placement.record_ranges[ranges_of_run.whole_run_range_index.0 as usize];
                visit(
                    ListPosition::new(held_chunk.chunk_index, whole_run_range.begin_offset),
                    TileReplayItem::PlacedRecordRange {
                        chunk_index: held_chunk.chunk_index,
                        range_index: ranges_of_run.whole_run_range_index,
                    },
                    run_replayed_whole.reach,
                    run_replayed_whole.innermost_declared_mask_around,
                );
                continue;
            }
            for range_index in (ranges_of_run.first_binned_range_index.0..ranges_of_run.whole_run_range_index.0)
                .map(PlacedRecordRangeIndex)
            {
                let range = &held_chunk.placement.record_ranges[range_index.0 as usize];
                let paint_position = ListPosition::new(held_chunk.chunk_index, range.begin_offset);
                visit(
                    paint_position,
                    TileReplayItem::PlacedRecordRange {
                        chunk_index: held_chunk.chunk_index,
                        range_index,
                    },
                    self.backdrop_reads
                        .reach_extended_through_reads(range.reach, paint_position),
                    EffectNodeIndex::NONE,
                );
            }
        }
    }

    fn work_out_reach_of_held_records(&mut self, display_list: &DisplayListStorage) {
        for held_chunk_index in 0..self.held_chunks.len() {
            let held_chunk = &self.held_chunks[held_chunk_index];
            let mut reach_of_runs_replayed_whole = Reach::Nothing;
            let mut reach_of_runs_binned_range_by_range = Reach::Nothing;
            for &run_index in &held_chunk.held_run_indices {
                match self.runs_replayed_whole.get(&(held_chunk.chunk_index, run_index)) {
                    Some(run_replayed_whole) => {
                        reach_of_runs_replayed_whole = reach_of_runs_replayed_whole.united(run_replayed_whole.reach);
                    }
                    None => {
                        reach_of_runs_binned_range_by_range = reach_of_runs_binned_range_by_range.united(
                            held_chunk.placement.ranges_of_each_run[run_index.0 as usize].reach_of_binned_ranges,
                        );
                    }
                }
            }
            // A backdrop filter can extend content painted before it past the cells that content reaches.
            if !self.backdrop_reads.is_empty() {
                let first_held_offset = held_chunk.held_run_indices.first().map_or(0, |run_index| {
                    display_list.chunk(held_chunk.chunk_index).runs()[run_index.0 as usize].offset
                });
                reach_of_runs_binned_range_by_range = self.backdrop_reads.reach_extended_through_reads(
                    reach_of_runs_binned_range_by_range,
                    ListPosition::new(held_chunk.chunk_index, first_held_offset),
                );
            }
            self.held_chunks[held_chunk_index].reach_of_held_records =
                reach_of_runs_replayed_whole.united(reach_of_runs_binned_range_by_range);
        }
    }
}

/// What the tiles of the layers of one band hold, kept from frame to frame while the band is bound to one list and one
/// geometry: the tiles keyed so far, binned against the list, the geometry and the band of the stack they are bound to.
/// Every tile is keyed again where a value of the frame that some tile of the band can read changed.
pub struct BandTileKeys {
    cells: BTreeMap<TileCoord, Vec<KeyedTile>>,
    /// The cells where the band holds tiles at every cell it can hold something in, as the latest frame keyed them.
    /// Only the cells where the band can hold something have an entry in `cells`, so a frame that shows no cell outside
    /// these looks for no cell to key.
    cells_looked_at: CellRange,
    bound_band: BandBoundToListAndGeometry,
    values_of_frame_tiles_were_keyed_with: Option<ValuesOfFrameThatTilesOfBandRead>,
}

/// What the tiles of a band read of a frame besides the records they replay. A frame keys every tile of the band again
/// where any of these differs from the frame before.
#[derive(Clone, Copy, PartialEq, Eq)]
struct ValuesOfFrameThatTilesOfBandRead {
    frame_state_hash: Option<ContentHash>,
    tree_copy_number: u64,
    raster_settings_hash_of_layers_of_band: ContentHash,
    frame_edge_backdrop_blurs_mirror_at: Option<IntRect>,
}

/// What keying the shown tiles of a band did.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TileKeyingCounts {
    pub tiles_keyed: usize,
    /// The cells binned anew where the band holds something.
    pub cells_binned: usize,
}

impl BandTileKeys {
    pub fn tile(&self, coord: TileCoord, layer: LayerIndex) -> Option<&KeyedTile> {
        self.cells.get(&coord)?.iter().find(|tile| tile.layer_index == layer)
    }

    /// The keyed tiles within `rect`, by row and then by layer. A rect of any size costs no more than the cells the
    /// band holds.
    pub fn tiles_within(&self, rect: IntRect) -> impl Iterator<Item = &KeyedTile> {
        entries_within(&self.cells, CellRange::touched_by(rect)).flat_map(|(_, tiles)| tiles)
    }

    pub fn record_spans_of_tiles<'a>(&self, tiles: impl IntoIterator<Item = &'a KeyedTile>) -> Vec<ListSpan> {
        let bound_band = &self.bound_band;
        let mut spans: Vec<ListSpan> = tiles
            .into_iter()
            .flat_map(|tile| {
                tile.replay_items_in_paint_order
                    .iter()
                    .map(|item| bound_band.span_of(*item))
            })
            .collect();
        spans.sort_unstable_by_key(|span| (span.begin, span.end));
        spans.dedup();
        let mut merged: Vec<ListSpan> = Vec::with_capacity(spans.len());
        for span in spans {
            match merged.last_mut() {
                Some(last) if last.end >= span.begin => last.end = last.end.max(span.end),
                _ => merged.push(span),
            }
        }
        merged
    }
}

/// The most cells that one band keys in a frame. A frame keys only the cells where the band can hold something and
/// that the frame shows. A viewport of a real size shows content at a sane scale in far fewer cells. A transform that
/// the compositor animates can show large content at a small part of the scale it is rastered at, and then the frame
/// shows many more cells of it. This bounds the work and the tiles of the band in such a frame: it keys only the cells
/// nearest to the middle of the shown rect, and shows nothing in the others.
pub const MAXIMUM_CELLS_ONE_BAND_KEYS_IN_A_FRAME: usize = 4096;

/// Per chunk that `record_spans` hold records of, in the order of the chunks, the offset ranges they hold there, joined
/// where they touch.
fn held_offset_ranges_by_chunk(
    display_list: &DisplayListStorage,
    record_spans: &[ListSpan],
) -> Vec<(ChunkIndexInList, Vec<(u32, u32)>)> {
    let mut held_offset_ranges_by_chunk: Vec<(ChunkIndexInList, Vec<(u32, u32)>)> = Vec::new();
    for span in record_spans {
        for (chunk_index, begin, end) in display_list.parts_of_span(*span) {
            match held_offset_ranges_by_chunk.last_mut() {
                Some((last_chunk_index, held_offset_ranges)) if *last_chunk_index == chunk_index => {
                    match held_offset_ranges.last_mut() {
                        Some(last) if last.1 == begin => last.1 = end,
                        _ => held_offset_ranges.push((begin, end)),
                    }
                }
                _ => held_offset_ranges_by_chunk.push((chunk_index, vec![(begin, end)])),
            }
        }
    }
    held_offset_ranges_by_chunk
}

/// The runs of a chunk that `held_offset_ranges`, which are in order and of whole runs, hold.
fn runs_within_held_offset_ranges(
    runs: &[DisplayListCommandRun],
    held_offset_ranges: &[(u32, u32)],
) -> Vec<RunIndexInChunk> {
    let mut held_runs = Vec::new();
    for &(begin, end) in held_offset_ranges {
        let first_run_index = runs.partition_point(|run| run.offset < begin);
        let end_run_index = runs.partition_point(|run| run.offset < end);
        debug_assert!(
            first_run_index < end_run_index
                && runs[first_run_index].offset == begin
                && runs[end_run_index - 1].offset + runs[end_run_index - 1].size == end,
            "the checks of a plan take only bands whose spans are of whole runs"
        );
        held_runs.extend((first_run_index..end_run_index).map(|run_index| RunIndexInChunk(run_index as u32)));
    }
    held_runs
}

/// Whether `held_offset_ranges`, which are in order and apart, hold the record at `offset`.
fn offset_is_held(held_offset_ranges: &[(u32, u32)], offset: u32) -> bool {
    let index = held_offset_ranges.partition_point(|&(_, end)| end <= offset);
    held_offset_ranges.get(index).is_some_and(|&(begin, _)| begin <= offset)
}

/// The cells of `shown_rect` that a band keys in a frame: those where it can hold something and holds no tiles yet.
/// The band can hold the base layer and items replayed into every tile at every cell, and the records of a held chunk
/// where the chunk reaches. Where those are more than `MAXIMUM_CELLS_ONE_BAND_KEYS_IN_A_FRAME` cells, the band keys
/// only those nearest to the middle of `shown_rect`, and forgets its tiles at all other cells.
fn coords_to_key(
    cells: &mut BTreeMap<TileCoord, Vec<KeyedTile>>,
    cells_looked_at: &mut CellRange,
    bound_band: &BandBoundToListAndGeometry,
    holds_base_layer: bool,
    shown_rect: IntRect,
) -> Vec<TileCoord> {
    let shown_cells = CellRange::touched_by(shown_rect);
    if cells_looked_at.contains_range(shown_cells) {
        return Vec::new();
    }
    // The band of the base layer is in a space that an offset maps to the frame, so it shows about the cells of the
    // viewport. The budget holds a received stack that says otherwise.
    let mut ranges_of_content: Vec<CellRange> = if holds_base_layer || bound_band.items_replayed_in_every_tile.is_some()
    {
        vec![shown_cells]
    } else {
        let area_of_shown_cells = shown_cells.rect();
        bound_band
            .held_chunks
            .iter()
            .map(|held_chunk| CellRange::touched_by(held_chunk.reach_of_held_records.part_of(area_of_shown_cells)))
            .filter(|range| !range.is_empty())
            .collect()
    };
    ranges_of_content.sort_unstable();
    ranges_of_content.dedup();
    let coords_of_content = match coords_within_ranges_up_to(&ranges_of_content, MAXIMUM_CELLS_ONE_BAND_KEYS_IN_A_FRAME)
    {
        Some(coords_of_content) => {
            *cells_looked_at = shown_cells;
            coords_of_content
        }
        None => {
            let middle = TileCoord::of_pixel(
                shown_rect.x + shown_rect.width / 2,
                shown_rect.y + shown_rect.height / 2,
            );
            let window = window_within_budget(
                &ranges_of_content,
                shown_cells,
                middle,
                MAXIMUM_CELLS_ONE_BAND_KEYS_IN_A_FRAME,
            );
            cells.retain(|coord, _| window.contains(*coord));
            *cells_looked_at = window.intersected(shown_cells);
            ranges_of_content
                .iter()
                .flat_map(|range| range.intersected(window).coords())
                .collect()
        }
    };
    coords_of_content
        .into_iter()
        .filter(|coord| !cells.contains_key(coord))
        .collect()
}

/// The cells of `ranges`, row by row, or none where they are more than `limit`. It visits at most `limit` cells of a
/// range, and stops when it has found more than `limit`.
fn coords_within_ranges_up_to(ranges: &[CellRange], limit: usize) -> Option<BTreeSet<TileCoord>> {
    let mut coords = BTreeSet::new();
    for range in ranges {
        if range.cell_count() > limit as u64 {
            return None;
        }
        coords.extend(range.coords());
        if coords.len() > limit {
            return None;
        }
    }
    Some(coords)
}

/// The largest window of cells around `middle` where `ranges` hold at most `budget` cells, or no cell where the ranges
/// hold more at `middle` alone. The ranges lie within `shown_cells` and hold more than `budget` cells there. A cell of
/// two ranges counts twice, so ranges that overlap can make the window hold fewer cells than `budget`.
fn window_within_budget(ranges: &[CellRange], shown_cells: CellRange, middle: TileCoord, budget: usize) -> CellRange {
    let cells_of_ranges_within = |distance: i32| -> u64 {
        let window = CellRange::around(middle, distance);
        ranges.iter().map(|range| range.intersected(window).cell_count()).sum()
    };
    if cells_of_ranges_within(0) > budget as u64 {
        return CellRange::NONE;
    }
    // The window at the distance to the farthest shown cell holds every range whole.
    let mut distance_within_budget = 0;
    let mut distance_past_budget = [
        shown_cells.last_row - middle.row,
        middle.row - shown_cells.first_row,
        shown_cells.last_column - middle.column,
        middle.column - shown_cells.first_column,
    ]
    .into_iter()
    .max()
    .unwrap_or(0)
    .max(1);
    while distance_past_budget - distance_within_budget > 1 {
        let distance = distance_within_budget + (distance_past_budget - distance_within_budget) / 2;
        if cells_of_ranges_within(distance) <= budget as u64 {
            distance_within_budget = distance;
        } else {
            distance_past_budget = distance;
        }
    }
    CellRange::around(middle, distance_within_budget)
}

/// The indices of the nodes of a chain from `index` up to, but not with, the first node whose value `known_value` gives,
/// and that value, or `value_above_root` where no node up to the root has a known value. `parent_index` gives the index
/// of a node's parent, which is `u32::MAX` at the root. A caller then works out the values of the nodes down from there,
/// each from its parent's, in a loop: a tree from another process can chain clips and effects deeper than a call stack
/// holds.
fn nodes_up_to_known_value<Value>(
    index: u32,
    parent_index: impl Fn(u32) -> u32,
    value_above_root: Value,
    mut known_value: impl FnMut(u32) -> Option<Value>,
) -> (Vec<u32>, Value) {
    let mut indices = Vec::new();
    let mut index = index;
    while index != u32::MAX {
        if let Some(value) = known_value(index) {
            return (indices, value);
        }
        indices.push(index);
        index = parent_index(index);
    }
    (indices, value_above_root)
}

struct LazyContentHashesPerNode {
    content_hash_by_node_index: RefCell<Vec<Option<ContentHash>>>,
    node_count: usize,
}

impl LazyContentHashesPerNode {
    fn new(node_count: usize) -> Self {
        Self {
            content_hash_by_node_index: RefCell::new(Vec::new()),
            node_count,
        }
    }

    fn get(&self, index: u32) -> Option<ContentHash> {
        self.content_hash_by_node_index
            .borrow()
            .get(index as usize)
            .copied()
            .flatten()
    }

    fn cache(&self, index: u32, content_hash: ContentHash) -> ContentHash {
        let mut content_hash_by_node_index = self.content_hash_by_node_index.borrow_mut();
        if content_hash_by_node_index.is_empty() {
            content_hash_by_node_index.resize(self.node_count, None);
        }
        content_hash_by_node_index[index as usize] = Some(content_hash);
        content_hash
    }
}

#[derive(Clone, Copy, PartialEq)]
enum EffectHashScope {
    AllEffects,
    EffectsDrawnInsideTiles,
    /// Every effect, but of those the compositor composites only what places their content: not their opacity or
    /// blend mode, which compositing applies, so that a chunk stays placed while the compositor animates them.
    WhatPlacingReads,
}

pub struct TileKeyingOfRasterSpace<'a> {
    list: &'a TileKeyingOfList<'a>,
    geometry: &'a RasterSpaceGeometry,
    spatial_content_hashes: LazyContentHashesPerNode,
    clip_content_hashes: LazyContentHashesPerNode,
    effect_content_hashes_by_scope: [LazyContentHashesPerNode; 3],
    /// Per effect, once worked out, how far past each pixel its filter and the filters around it read, in device
    /// pixels, or none where that is not known.
    read_reach_of_filters_at_and_around_by_effect_index: RefCell<Vec<Option<Option<i32>>>>,
    rastered_bounds_of_clips_by_first_and_end_clip: RefCell<HashMap<(ClipNodeIndex, ClipNodeIndex), Option<FloatRect>>>,
    clips_without_rastered_bounds: RefCell<Vec<ClipNodeIndex>>,
    chunks_placed: Cell<u64>,
}

const CONTENT_HASH_OF_EFFECT_LEFT_OUT_OF_TILES: ContentHash = 1;

impl<'a> TileKeyingOfRasterSpace<'a> {
    pub fn new(list: &'a TileKeyingOfList<'a>, geometry: &'a RasterSpaceGeometry) -> Self {
        let tree = list.tree;
        Self {
            list,
            geometry,
            spatial_content_hashes: LazyContentHashesPerNode::new(tree.spatial_nodes.len()),
            clip_content_hashes: LazyContentHashesPerNode::new(tree.clip_nodes.len()),
            effect_content_hashes_by_scope: [
                LazyContentHashesPerNode::new(tree.effect_nodes.len()),
                LazyContentHashesPerNode::new(tree.effect_nodes.len()),
                LazyContentHashesPerNode::new(tree.effect_nodes.len()),
            ],
            read_reach_of_filters_at_and_around_by_effect_index: RefCell::new(Vec::new()),
            rastered_bounds_of_clips_by_first_and_end_clip: RefCell::new(HashMap::new()),
            clips_without_rastered_bounds: RefCell::new(Vec::new()),
            chunks_placed: Cell::new(0),
        }
    }

    /// How many chunks were placed in the space since it was made, rather than found placed alike before.
    pub fn chunks_placed(&self) -> u64 {
        self.chunks_placed.get()
    }

    fn reach_of_content_within(&self, rect: FloatRect, context: ContextRef) -> Reach<IntRect> {
        let list = self.list;
        let filters_and_clips = FiltersAndClipsOfList {
            tree: list.tree,
            filters: list.filters_of_effects(),
            effect_clips: list.effect_clips,
        };
        let composited_effect = if context.effect.is_none() {
            EffectNodeIndex::NONE
        } else {
            list.innermost_composited_effect_by_effect_index[context.effect.0 as usize]
        };
        if composited_effect.is_none()
            || list
                .filters_of_effects()
                .nearest_spreading_effect_at_or_above(composited_effect)
                .is_none()
        {
            return self.geometry.reach_of_content(&filters_and_clips, rect, context);
        }
        self.geometry.reach_of_content_inside_composited_effect(
            &filters_and_clips,
            list.filters_inside_composited_effects(),
            rect,
            context,
            composited_effect,
            self.read_reach_in_device_pixels_of_filters_at_and_around(composited_effect),
            |first_clip, end_clip| {
                self.geometry.rastered_bounds_of_clips_from_up_to(
                    list.tree,
                    first_clip,
                    end_clip,
                    &mut self.rastered_bounds_of_clips_by_first_and_end_clip.borrow_mut(),
                    &mut self.clips_without_rastered_bounds.borrow_mut(),
                )
            },
        )
    }

    fn read_reach_in_device_pixels_of_filters_at_and_around(&self, composited_effect: EffectNodeIndex) -> Option<i32> {
        let tree = self.list.tree;
        let mut known_read_reach_by_effect_index =
            self.read_reach_of_filters_at_and_around_by_effect_index.borrow_mut();
        if known_read_reach_by_effect_index.is_empty() {
            known_read_reach_by_effect_index.resize(tree.effect_nodes.len(), None);
        }
        let (effects_to_work_out, mut read_reach) = nodes_up_to_known_value(
            composited_effect.0,
            |effect| tree.effect_nodes[effect as usize].parent.0,
            Some(0i32),
            |effect| known_read_reach_by_effect_index[effect as usize],
        );
        for &effect in effects_to_work_out.iter().rev() {
            let node = &tree.effect_nodes[effect as usize];
            if let Some(filter) = spreading_filter_of(node) {
                read_reach = read_reach.and_then(|sum| {
                    let filter_read_reach =
                        filter_read_reach_in_device_pixels(filter, &self.geometry.world.draw_matrix(node.spatial))?;
                    Some(sum.saturating_add(filter_read_reach))
                });
            }
            known_read_reach_by_effect_index[effect as usize] = Some(read_reach);
        }
        read_reach
    }

    fn backdrop_read_in_layer(
        &self,
        effect: EffectNodeIndex,
        first_record_position: ListPosition,
    ) -> BackdropReadInLayer {
        let tree = self.list.tree;
        let node = &tree.effect_nodes[effect.0 as usize];
        let EffectNodeData::Effects(effects) = &node.data else {
            unreachable!("only effects with a backdrop filter are read as backdrops");
        };
        let backdrop_filter = effects
            .backdrop_filter
            .as_ref()
            .expect("only effects with a backdrop filter are read as backdrops");
        let region_in_effect_space = backdrop_filter.region.to_float();
        let backdrop_context = ContextRef {
            spatial: node.spatial,
            clip: self.list.effect_clips.output_clip(effect),
            effect,
        };
        let region_reach = self.reach_of_content_within(region_in_effect_space, backdrop_context);
        let region_in_device_pixels = Some(
            self.geometry
                .world
                .map_rect_to_root(node.spatial, region_in_effect_space),
        )
        .filter(|rect| is_finite_rect(*rect))
        .map(enclosing_int_rect_within_extent_limit);
        let read_reach_in_device_pixels = region_in_device_pixels.and_then(|_| {
            filter_read_reach_in_device_pixels(&backdrop_filter.filter, &self.geometry.world.draw_matrix(node.spatial))
        });
        let spread_past_region_in_device_pixels = match (region_reach, region_in_device_pixels) {
            (Reach::Within(spread), Some(region)) if read_reach_in_device_pixels.is_some() => [
                region.x - spread.x,
                spread.right() - region.right(),
                region.y - spread.y,
                spread.bottom() - region.bottom(),
            ]
            .into_iter()
            .fold(0, i32::max),
            _ => 0,
        };
        BackdropReadInLayer {
            first_record_position,
            region_reach,
            backdrop_read_reach: backdrop_filter_read_reach(tree, self.geometry, effect),
            read_reach_in_device_pixels,
            spread_past_region_in_device_pixels,
            mirroring_blur_region: libgfx_rust::filter::Filter::serialized_has_mirroring_blur(&backdrop_filter.filter)
                .then(|| region_in_device_pixels.map_or(Reach::Anywhere, Reach::Within)),
        }
    }

    fn spatial_content_hash(&self, spatial: SpatialNodeIndex) -> ContentHash {
        if let Some(cached) = self.spatial_content_hashes.get(spatial.0) {
            return cached;
        }
        let mut hasher = ContentHasher::new();
        hasher.write_matrix(&self.geometry.world.draw_matrix(spatial));
        hasher.write_bool(self.geometry.world.backface_culled[spatial.0 as usize]);
        self.spatial_content_hashes.cache(spatial.0, hasher.finish())
    }

    fn clip_content_hash(&self, clip: ClipNodeIndex) -> ContentHash {
        let tree = self.list.tree;
        let (clips_to_hash, mut content_hash) = nodes_up_to_known_value(
            clip.0,
            |clip| tree.clip_nodes[clip as usize].parent.0,
            0,
            |clip| self.clip_content_hashes.get(clip),
        );
        for clip in clips_to_hash.into_iter().rev().map(ClipNodeIndex) {
            if !self.geometry.clip_is_left_to_composite(clip) {
                let mut hasher = ContentHasher::new();
                hasher.write_content_hash(content_hash);
                self.write_clip_node(&mut hasher, clip);
                content_hash = hasher.finish();
            }
            self.clip_content_hashes.cache(clip.0, content_hash);
        }
        content_hash
    }

    fn write_clip_node(&self, hasher: &mut ContentHasher, clip: ClipNodeIndex) {
        let node = &self.list.tree.clip_nodes[clip.0 as usize];
        hasher.write_content_hash(self.spatial_content_hash(node.spatial));
        hasher.write_bool(node.clips_everything);
        match &node.data {
            ClipNodeData::Rect(data) => {
                hasher.write_u32(1);
                hasher.write_float_rect(data.rect);
                hasher.write_corner_radii(&data.corner_radii);
                hasher.write_u32(data.mode as u32);
            }
            ClipNodeData::Path(data) => {
                hasher.write_u32(2);
                hasher.write_int_rect(data.bounding_rect);
                hasher.write_u32(data.fill_rule as u32);
                hasher.write_bytes(&data.path.serialize_to_bytes());
            }
            ClipNodeData::Dead => hasher.write_u32(3),
        }
    }

    fn clip_content_hash_near_area(&self, clip: ClipNodeIndex, near_area: &mut ContentHashesNearArea) -> ContentHash {
        let tree = self.list.tree;
        let (clips_to_hash, mut content_hash) = nodes_up_to_known_value(
            clip.0,
            |clip| tree.clip_nodes[clip as usize].parent.0,
            0,
            |clip| near_area.content_hash_by_clip.get(&clip).copied(),
        );
        for clip in clips_to_hash.into_iter().rev().map(ClipNodeIndex) {
            content_hash = self.content_hash_of_clip_near_area(clip, content_hash, near_area);
            near_area.content_hash_by_clip.insert(clip.0, content_hash);
        }
        content_hash
    }

    fn content_hash_of_clip_near_area(
        &self,
        clip: ClipNodeIndex,
        parent_hash: ContentHash,
        near_area: &ContentHashesNearArea,
    ) -> ContentHash {
        let node = &self.list.tree.clip_nodes[clip.0 as usize];
        if self.geometry.clip_is_left_to_composite(clip) {
            parent_hash
        } else {
            let mut hasher = ContentHasher::new();
            hasher.write_content_hash(parent_hash);
            let device_aligned_rect_and_mode = match &node.data {
                ClipNodeData::Rect(ClipData {
                    rect,
                    corner_radii,
                    mode,
                }) => RoundedRect::device_aligned_of(&self.geometry.world, node.spatial, *rect, corner_radii)
                    .map(|rect| (rect, *mode)),
                _ => None,
            };
            match device_aligned_rect_and_mode {
                Some((rect, mode)) => {
                    hasher.write_u32(4);
                    hasher.write_bool(node.clips_everything);
                    hasher.write_u32(mode as u32);
                    rect.write_part_within_or_whole_if_corner_reaches_in(&mut hasher, near_area.area);
                }
                None => self.write_clip_node(&mut hasher, clip),
            }
            hasher.finish()
        }
    }

    fn effect_content_hash(&self, effect: EffectNodeIndex, scope: EffectHashScope) -> ContentHash {
        let tree = self.list.tree;
        let cached_hashes = &self.effect_content_hashes_by_scope[scope as usize];
        let (effects_to_hash, mut content_hash) = nodes_up_to_known_value(
            effect.0,
            |effect| tree.effect_nodes[effect as usize].parent.0,
            0,
            |effect| {
                if scope == EffectHashScope::EffectsDrawnInsideTiles
                    && self.list.compositor_composites(EffectNodeIndex(effect))
                {
                    return Some(CONTENT_HASH_OF_EFFECT_LEFT_OUT_OF_TILES);
                }
                cached_hashes.get(effect)
            },
        );
        for effect in effects_to_hash.into_iter().rev().map(EffectNodeIndex) {
            content_hash = self.content_hash_of_effect_node(
                effect,
                content_hash,
                self.clip_content_hash(self.list.effect_clips.output_clip(effect)),
                scope,
            );
            cached_hashes.cache(effect.0, content_hash);
        }
        content_hash
    }

    fn effect_content_hash_inside_tiles_near_area(
        &self,
        effect: EffectNodeIndex,
        near_area: &mut ContentHashesNearArea,
    ) -> ContentHash {
        let tree = self.list.tree;
        let (effects_to_hash, mut content_hash) = nodes_up_to_known_value(
            effect.0,
            |effect| tree.effect_nodes[effect as usize].parent.0,
            0,
            |effect| {
                if self.list.compositor_composites(EffectNodeIndex(effect)) {
                    return Some(CONTENT_HASH_OF_EFFECT_LEFT_OUT_OF_TILES);
                }
                near_area.content_hash_by_effect.get(&effect).copied()
            },
        );
        for effect in effects_to_hash.into_iter().rev().map(EffectNodeIndex) {
            let output_clip_hash =
                self.clip_content_hash_near_area(self.list.effect_clips.output_clip(effect), near_area);
            content_hash = self.content_hash_of_effect_node(
                effect,
                content_hash,
                output_clip_hash,
                EffectHashScope::EffectsDrawnInsideTiles,
            );
            near_area.content_hash_by_effect.insert(effect.0, content_hash);
        }
        content_hash
    }

    fn content_hash_of_effect_node(
        &self,
        effect: EffectNodeIndex,
        parent_hash: ContentHash,
        output_clip_hash: ContentHash,
        scope: EffectHashScope,
    ) -> ContentHash {
        let tree = self.list.tree;
        let node = &tree.effect_nodes[effect.0 as usize];
        let mut hasher = ContentHasher::new();
        // Sibling nodes of equal values are composited as a group each, so the index tells them apart.
        hasher.write_u32(effect.0);
        hasher.write_content_hash(parent_hash);
        hasher.write_content_hash(self.spatial_content_hash(node.spatial));
        hasher.write_content_hash(output_clip_hash);
        hasher.write_bool(tree.root_isolation_effect == Some(effect));
        let tiles_leave_effect_out = self.list.compositor_composites(effect);
        hasher.write_bool(tiles_leave_effect_out);
        match &node.data {
            EffectNodeData::Effects(effects) => {
                hasher.write_u32(1);
                hasher.write_bool(effects.isolation);
                let hashes_what_compositing_applies =
                    !(scope == EffectHashScope::WhatPlacingReads && tiles_leave_effect_out);
                if hashes_what_compositing_applies {
                    hasher.write_f32(effects.opacity);
                    hasher.write_u32(effects.blend_mode as u32);
                }
                match &effects.filter {
                    Some(filter) => hasher.write_bytes(filter),
                    None => hasher.write_u64(u64::MAX),
                }
                match &effects.backdrop_filter {
                    Some(backdrop_filter) => {
                        hasher.write_bytes(&backdrop_filter.filter);
                        hasher.write_int_rect(backdrop_filter.region);
                        hasher.write_corner_radii(&backdrop_filter.corner_radii);
                    }
                    None => hasher.write_u64(u64::MAX),
                }
            }
            EffectNodeData::Mask(mask) => {
                hasher.write_u32(2);
                hasher.write_int_rect(mask.rect);
                hasher.write_u32(mask.kind as u32);
                hasher.write_u32(mask.origin as u32);
                hasher.write_bool(mask.masks_backdrop);
            }
            EffectNodeData::BackgroundColorAnimation => hasher.write_u32(3),
            EffectNodeData::Dead => hasher.write_u32(4),
        }
        hasher.finish()
    }

    fn context_content_hash(&self, context: ContextRef, scope: EffectHashScope) -> ContentHash {
        let mut hasher = ContentHasher::new();
        hasher.write_content_hash(self.spatial_content_hash(context.spatial));
        hasher.write_content_hash(self.clip_content_hash(context.clip));
        hasher.write_content_hash(self.effect_content_hash(context.effect, scope));
        hasher.finish()
    }

    fn content_hash_of_placed_range_near_area(
        &self,
        chunk: &ListChunk,
        range: &PlacedRecordRange,
        near_area: &mut ContentHashesNearArea,
    ) -> ContentHash {
        let mut hasher = ContentHasher::new();
        hasher.write_content_hash(self.spatial_content_hash(range.context.spatial));
        hasher.write_content_hash(self.clip_content_hash_near_area(range.context.clip, near_area));
        hasher.write_content_hash(self.effect_content_hash_inside_tiles_near_area(range.context.effect, near_area));
        match &range.device_aligned_fill {
            Some(fill) => {
                hasher.write_content_hash(fill.content_hash_except_rects);
                fill.rect
                    .write_part_within_or_whole_if_corner_reaches_in(&mut hasher, near_area.area);
                // The inner rect's corners curve only within the outer rect's, whose part tells of them.
                if let Some(unfilled_inner_rect) = &fill.unfilled_inner_rect_of_ring {
                    unfilled_inner_rect.write_part_within_or_whole_if_corner_reaches_in(&mut hasher, near_area.area);
                }
            }
            None => {
                for record in chunk
                    .record_table()
                    .records_in(range.first_record_index..range.end_record_index)
                {
                    hasher.write_content_hash(record.content_hash_of_bytes);
                }
            }
        }
        hasher.finish()
    }

    fn write_frame_state_read_by_record(&self, hasher: &mut ContentHasher, chunk: &ListChunk, record: &ChunkRecord) {
        let list = self.list;
        let frame_state = &list.frame_state;
        let record_bytes = &chunk.records()[record.offset as usize..record.end_offset as usize];
        let (header, payload) = chunk.record_at(record.offset);
        if record.frame_state_read_flags & FRAME_STATE_CARET_BLINK_PHASE != 0 {
            hasher.write_bool(caret_is_visible_at_time(
                &read_command::<PaintCaret>(payload),
                frame_state.time_ns,
            ));
        }
        if record.frame_state_read_flags & FRAME_STATE_SCROLL_BAR != 0 {
            let command = read_command::<PaintScrollBar>(payload);
            match scrollbar_shown_by_frame(frame_state.scrollbars, &command) {
                Some(scrollbar) => {
                    hasher.write_u32(1);
                    write_scrollbar_shown_by_frame(hasher, scrollbar);
                }
                None => {
                    hasher.write_u32(2);
                    hasher.write_float_point(device_offset_for_index(
                        &self.geometry.raster_scroll_offsets,
                        command.scroll_node_index,
                    ));
                }
            }
        }
        if record.frame_state_read_flags & FRAME_STATE_SPACES_OF_INLINE_CLIPS != 0 {
            for clip in inline_clips_of(&header, payload).filter(|clip| clip.has_own_space) {
                match self.geometry.world.draw_matrix_if_known(clip.space) {
                    Some(matrix) => hasher.write_matrix(&matrix),
                    None => hasher.write_u64(u64::MAX),
                }
            }
        }
        if record.frame_state_read_flags & (FRAME_STATE_LIVE_CONTENT | FRAME_STATE_SAMPLED_BACKGROUND_COLOR) == 0 {
            return;
        }
        let tree = list.tree;
        for_each_command_including_nested(record_bytes, &mut |command_type, _, payload| {
            if let Some(content) = live_content_of(command_type, payload) {
                hasher.write_u32(match content {
                    LiveContent::Canvas(_) => 1,
                    LiveContent::VideoFrame(_) => 2,
                    LiveContent::ImageFrame(_) => 3,
                    LiveContent::CompositedContext(_) => 4,
                });
                hasher.write_u64(version_of_live_content(frame_state.live_content, content));
                if matches!(content, LiveContent::CompositedContext(_)) {
                    let command = read_command::<DrawCompositedContext>(payload);
                    hasher.write_bool(tree.translations_snap_to_device_pixels(command.spatial_node_index));
                }
            }
            let background_color_animation_effect = background_color_animation_effect_of(command_type, payload);
            if !background_color_animation_effect.is_none() {
                let sampled = tree.sampled_background_color(background_color_animation_effect);
                hasher.write_u32(sampled.map_or(u32::MAX, |color| color.0));
                hasher.write_bool(sampled.is_some());
            }
        });
    }

    fn content_hash_of_records_without_frame_state(
        &self,
        chunk: &ListChunk,
        record_indices: std::ops::Range<RecordIndexInChunk>,
    ) -> ContentHash {
        let table = chunk.record_table();
        let mut hasher = ContentHasher::new();
        let mut run_index_of_previous_record = RunIndexInChunk(u32::MAX);
        for record in table.records_in(record_indices) {
            if record.run_index != run_index_of_previous_record {
                run_index_of_previous_record = record.run_index;
                hasher.write_content_hash(self.context_content_hash(
                    chunk.runs()[record.run_index.0 as usize].context,
                    EffectHashScope::EffectsDrawnInsideTiles,
                ));
            }
            hasher.write_content_hash(record.content_hash_of_bytes);
        }
        hasher.finish()
    }

    fn frame_state_hash_of_records(
        &self,
        chunk: &ListChunk,
        record_indices: std::ops::Range<RecordIndexInChunk>,
    ) -> Option<ContentHash> {
        let table = chunk.record_table();
        let records = table.records_in(record_indices);
        if records.iter().all(|record| record.frame_state_read_flags == 0) {
            return None;
        }
        let mut hasher = ContentHasher::new();
        for record in records.iter().filter(|record| record.frame_state_read_flags != 0) {
            self.write_frame_state_read_by_record(&mut hasher, chunk, record);
        }
        Some(hasher.finish())
    }

    fn content_hash_of_records(
        &self,
        chunk: &ListChunk,
        record_indices: std::ops::Range<RecordIndexInChunk>,
    ) -> ContentHash {
        let mut hasher = ContentHasher::new();
        hasher.write_content_hash(self.content_hash_of_records_without_frame_state(chunk, record_indices.clone()));
        if let Some(frame_state_hash) = self.frame_state_hash_of_records(chunk, record_indices) {
            hasher.write_content_hash(frame_state_hash);
        }
        hasher.finish()
    }

    /// The hash of what compositing the layers of a node inside `effect` reads of the effect: its values, where it is,
    /// and the clips it is entered within, which changes wherever the composited effect draws differently.
    pub fn content_hash_of_composited_effect(&self, effect: EffectNodeIndex) -> ContentHash {
        self.effect_content_hash(effect, EffectHashScope::AllEffects)
    }

    /// The hash of what the records within `span` draw in the frame.
    pub fn content_hash_of_records_within(&self, span: ListSpan) -> ContentHash {
        let display_list = self.list.display_list;
        let mut hasher = ContentHasher::new();
        for (chunk_index, begin_offset, end_offset) in display_list.parts_of_span(span) {
            let chunk = display_list.chunk(chunk_index);
            let table = chunk.record_table();
            let record_indices = table.record_indices_between_offsets(begin_offset, end_offset);
            let mut first_record_index_of_run_part = record_indices.start;
            while first_record_index_of_run_part < record_indices.end {
                let run_index = table.record(first_record_index_of_run_part).run_index;
                let end_record_index_of_run_part = table.record_indices_of_run(run_index).end.min(record_indices.end);
                hasher.write_content_hash(
                    self.content_hash_of_records(chunk, first_record_index_of_run_part..end_record_index_of_run_part),
                );
                first_record_index_of_run_part = end_record_index_of_run_part;
            }
        }
        hasher.finish()
    }

    fn content_hash_of_replay_item(
        &self,
        bound_band: &BandBoundToListAndGeometry,
        item: TileReplayItem,
        near_area: Option<&mut ContentHashesNearArea>,
    ) -> ContentHash {
        let display_list = self.list.display_list;
        let chunk = display_list.chunk(item.chunk_index());
        let (content_hash_without_frame_state, record_indices) = match item {
            TileReplayItem::PlacedRecordRange {
                chunk_index,
                range_index,
            } => {
                let range = bound_band.placed_record_range(chunk_index, range_index);
                let record_indices = range.first_record_index..range.end_record_index;
                let content_hash = match near_area {
                    Some(near_area) if range.is_spread_by_no_filter => {
                        self.content_hash_of_placed_range_near_area(chunk, range, near_area)
                    }
                    _ => self.content_hash_of_records_without_frame_state(chunk, record_indices.clone()),
                };
                (content_hash, record_indices)
            }
            TileReplayItem::RecordSpan {
                begin_offset,
                end_offset,
                ..
            } => {
                let record_indices = chunk
                    .record_table()
                    .record_indices_between_offsets(begin_offset, end_offset);
                (
                    self.content_hash_of_records_without_frame_state(chunk, record_indices.clone()),
                    record_indices,
                )
            }
        };
        match self.frame_state_hash_of_records(chunk, record_indices) {
            None => content_hash_without_frame_state,
            Some(frame_state_hash) => {
                let mut hasher = ContentHasher::new();
                hasher.write_content_hash(content_hash_without_frame_state);
                hasher.write_content_hash(frame_state_hash);
                hasher.finish()
            }
        }
    }

    fn records_of_item(&self, bound_band: &BandBoundToListAndGeometry, item: TileReplayItem) -> &[ChunkRecord] {
        let table = self.list.display_list.chunk(item.chunk_index()).record_table();
        let record_indices = match item {
            TileReplayItem::PlacedRecordRange {
                chunk_index,
                range_index,
            } => {
                let range = bound_band.placed_record_range(chunk_index, range_index);
                range.first_record_index..range.end_record_index
            }
            TileReplayItem::RecordSpan {
                begin_offset,
                end_offset,
                ..
            } => table.record_indices_between_offsets(begin_offset, end_offset),
        };
        table.records_in(record_indices)
    }

    fn root_of_context_sorting_item_by_depth(
        &self,
        bound_band: &BandBoundToListAndGeometry,
        item: TileReplayItem,
    ) -> Option<Option<SpatialNodeIndex>> {
        let chunk = self.list.display_list.chunk(item.chunk_index());
        let first_run_of_chunk = self.list.display_list.first_run_of_chunk(item.chunk_index());
        self.records_of_item(bound_band, item).iter().find_map(|record| {
            self.list
                .run_groups()
                .run_is_sorted_by_depth(first_run_of_chunk + record.run_index.0 as usize)
                .then(|| {
                    self.list.run_groups().root_of_context_sorting_planes_by_depth(
                        chunk.runs()[record.run_index.0 as usize].context.spatial,
                    )
                })
        })
    }

    fn bind_band_to_list_and_geometry(&self, stack: &LayerStack, band_index: BandIndex) -> BandBoundToListAndGeometry {
        let display_list = self.list.display_list;
        let band = &stack.bands[band_index.0];
        let held_offset_ranges_by_chunk = held_offset_ranges_by_chunk(display_list, &band.record_spans);
        if self.list.replays_every_record_into_every_tile {
            return self.band_replaying_every_held_record_in_every_tile(&held_offset_ranges_by_chunk);
        }
        let held_chunks = self.held_chunks_with_their_placements(held_offset_ranges_by_chunk);
        let backdrop_reads = self.backdrop_reads_of_held_runs(stack, band, &held_chunks);
        let (runs_replayed_whole, masks_with_declared_content) =
            self.runs_replayed_whole(&held_chunks, &backdrop_reads);
        let held_runs_read_frame_state_hashed_for_the_list = held_chunks.iter().any(|held_chunk| {
            let table = display_list.chunk(held_chunk.chunk_index).record_table();
            held_chunk
                .held_run_indices
                .iter()
                .any(|&run_index| table.run_reads_frame_state_hashed_for_the_list(run_index))
        });
        let mut bound_band = BandBoundToListAndGeometry {
            held_chunks,
            runs_replayed_whole,
            declared_masks_around_runs_replayed_whole: HashMap::new(),
            backdrop_reads,
            items_replayed_in_every_tile: None,
            replays_records_reading_frame_state_hashed_for_the_list: held_runs_read_frame_state_hashed_for_the_list,
        };

        if !masks_with_declared_content.declaration_positions_by_mask.is_empty() {
            let (declared_masks, declarations_read_frame_state_hashed_for_the_list) =
                self.declared_masks_around_runs_replayed_whole(&bound_band, &masks_with_declared_content);
            bound_band.declared_masks_around_runs_replayed_whole = declared_masks;
            bound_band.replays_records_reading_frame_state_hashed_for_the_list |=
                declarations_read_frame_state_hashed_for_the_list;
        }

        bound_band.work_out_reach_of_held_records(display_list);
        bound_band
    }

    fn band_replaying_every_held_record_in_every_tile(
        &self,
        held_offset_ranges_by_chunk: &[(ChunkIndexInList, Vec<(u32, u32)>)],
    ) -> BandBoundToListAndGeometry {
        let list = self.list;
        let display_list = list.display_list;
        let mut items_replayed_in_every_tile: Vec<(ListPosition, TileReplayItem)> = held_offset_ranges_by_chunk
            .iter()
            .flat_map(|(chunk_index, held_offset_ranges)| {
                held_offset_ranges.iter().map(|&(begin_offset, end_offset)| {
                    (
                        ListPosition::new(*chunk_index, begin_offset),
                        TileReplayItem::RecordSpan {
                            chunk_index: *chunk_index,
                            begin_offset,
                            end_offset,
                        },
                    )
                })
            })
            .collect();
        let tree = list.tree;
        let mut effect_is_around_held_run = vec![false; tree.effect_nodes.len()];
        let mut replays_records_reading_frame_state_hashed_for_the_list = false;
        for (chunk_index, held_offset_ranges) in held_offset_ranges_by_chunk {
            let chunk = display_list.chunk(*chunk_index);
            let runs = chunk.runs();
            for run_index in runs_within_held_offset_ranges(runs, held_offset_ranges) {
                replays_records_reading_frame_state_hashed_for_the_list |= chunk
                    .record_table()
                    .run_reads_frame_state_hashed_for_the_list(run_index);
                let mut effect = runs[run_index.0 as usize].context.effect;
                while !effect.is_none() && !effect_is_around_held_run[effect.0 as usize] {
                    effect_is_around_held_run[effect.0 as usize] = true;
                    effect = tree.effect_nodes[effect.0 as usize].parent;
                }
            }
        }
        for declaration in &display_list.summary().mask_declarations {
            let (mask_effect, declaration_position) = (declaration.mask_effect, declaration.record.position);
            // A declaration can name any effect node.
            let mask_is_around_held_run = effect_is_around_held_run.get(mask_effect.0 as usize) == Some(&true)
                && matches!(tree.effect_nodes[mask_effect.0 as usize].data, EffectNodeData::Mask(_));
            let declaration_is_held = held_offset_ranges_by_chunk
                .binary_search_by_key(&declaration_position.chunk_index, |(chunk_index, _)| *chunk_index)
                .is_ok_and(|position| {
                    offset_is_held(&held_offset_ranges_by_chunk[position].1, declaration_position.offset)
                });
            if !mask_is_around_held_run || declaration_is_held {
                continue;
            }
            let table = display_list.chunk(declaration_position.chunk_index).record_table();
            let record_index = table
                .record_indices_between_offsets(declaration_position.offset, declaration_position.offset + 1)
                .start;
            replays_records_reading_frame_state_hashed_for_the_list |=
                table.record(record_index).reads_frame_state_hashed_for_the_list();
            items_replayed_in_every_tile.push((
                declaration_position,
                TileReplayItem::RecordSpan {
                    chunk_index: declaration_position.chunk_index,
                    begin_offset: declaration_position.offset,
                    end_offset: table.record(record_index).end_offset,
                },
            ));
        }
        BandBoundToListAndGeometry {
            held_chunks: Vec::new(),
            runs_replayed_whole: HashMap::new(),
            declared_masks_around_runs_replayed_whole: HashMap::new(),
            backdrop_reads: BackdropReadsOfBand::new(Vec::new()),
            items_replayed_in_every_tile: Some(items_replayed_in_every_tile),
            replays_records_reading_frame_state_hashed_for_the_list,
        }
    }

    fn held_chunks_with_their_placements(
        &self,
        held_offset_ranges_by_chunk: Vec<(ChunkIndexInList, Vec<(u32, u32)>)>,
    ) -> Vec<ChunkHeldByBand> {
        let display_list = self.list.display_list;
        let mut held_chunks: Vec<ChunkHeldByBand> = Vec::with_capacity(held_offset_ranges_by_chunk.len());
        for (chunk_index, held_offset_ranges) in held_offset_ranges_by_chunk {
            let held_run_indices =
                runs_within_held_offset_ranges(display_list.chunk(chunk_index).runs(), &held_offset_ranges);
            let placement = self.placement_of_runs_of_chunk(chunk_index, &held_run_indices);
            held_chunks.push(ChunkHeldByBand {
                chunk_index,
                placement,
                held_run_indices,
                reach_of_held_records: Reach::Nothing,
            });
        }
        debug_assert!(
            held_chunks
                .windows(2)
                .all(|pair| pair[0].chunk_index < pair[1].chunk_index),
            "the checks of a plan take only bands whose spans are in paint order"
        );
        held_chunks
    }

    fn backdrop_reads_of_held_runs(
        &self,
        stack: &LayerStack,
        band: &PlacementNodeBand,
        held_chunks: &[ChunkHeldByBand],
    ) -> BackdropReadsOfBand {
        let display_list = self.list.display_list;
        let tree = self.list.tree;
        let effect_composited_around_band = stack.nodes[band.placement_node_index.0]
            .composited_effect
            .unwrap_or(EffectNodeIndex::NONE);
        let mut effect_was_visited = vec![false; tree.effect_nodes.len()];
        let mut backdrop_reads_in_paint_order = Vec::new();
        for held_chunk in held_chunks {
            let runs = display_list.chunk(held_chunk.chunk_index).runs();
            for &run_index in &held_chunk.held_run_indices {
                let run = &runs[run_index.0 as usize];
                let mut effect = run.context.effect;
                while !effect.is_none()
                    && effect != effect_composited_around_band
                    && !effect_was_visited[effect.0 as usize]
                {
                    effect_was_visited[effect.0 as usize] = true;
                    let node = &tree.effect_nodes[effect.0 as usize];
                    if matches!(&node.data, EffectNodeData::Effects(effects) if effects.backdrop_filter.is_some()) {
                        backdrop_reads_in_paint_order.push(
                            self.backdrop_read_in_layer(effect, ListPosition::new(held_chunk.chunk_index, run.offset)),
                        );
                    }
                    effect = node.parent;
                }
            }
        }
        BackdropReadsOfBand::new(backdrop_reads_in_paint_order)
    }

    /// The held runs that replay whole wherever any run of their run group shows, and the masks with declared content
    /// around them.
    fn runs_replayed_whole(
        &self,
        held_chunks: &[ChunkHeldByBand],
        backdrop_reads: &BackdropReadsOfBand,
    ) -> (RunsReplayedWhole, MasksWithDeclaredContent) {
        let list = self.list;
        let display_list = list.display_list;
        let tree = list.tree;
        // A mask or a depth sort reads every run of its run group, so those runs replay whole wherever any of them
        // shows.
        let run_groups = list.run_groups();
        let mut reach_and_first_position_of_run_groups_replayed_whole: BTreeMap<
            RunGroupIndex,
            (Reach<IntRect>, ListPosition),
        > = BTreeMap::new();
        let mut held_runs_replayed_whole: Vec<(ChunkIndexInList, RunIndexInChunk, RunGroupIndex)> = Vec::new();
        for held_chunk in held_chunks {
            let runs = display_list.chunk(held_chunk.chunk_index).runs();
            for &run_index in &held_chunk.held_run_indices {
                let position = ListPosition::new(held_chunk.chunk_index, runs[run_index.0 as usize].offset);
                let run_group_index = run_groups
                    .group_of_run(display_list.first_run_of_chunk(held_chunk.chunk_index) + run_index.0 as usize);
                if !run_groups.group_is_under_mask(run_group_index)
                    && !run_groups.group_is_sorted_by_depth(run_group_index)
                {
                    continue;
                }
                let ranges_of_run = &held_chunk.placement.ranges_of_each_run[run_index.0 as usize];
                let whole_run_reach =
                    held_chunk.placement.record_ranges[ranges_of_run.whole_run_range_index.0 as usize].reach;
                reach_and_first_position_of_run_groups_replayed_whole
                    .entry(run_group_index)
                    .and_modify(|(united_reach, first_position)| {
                        *united_reach = united_reach.united(whole_run_reach);
                        *first_position = (*first_position).min(position);
                    })
                    .or_insert((whole_run_reach, position));
                held_runs_replayed_whole.push((held_chunk.chunk_index, run_index, run_group_index));
            }
        }
        let mut declaration_positions_by_mask: HashMap<EffectNodeIndex, Vec<ListPosition>> = HashMap::new();
        if !held_runs_replayed_whole.is_empty() {
            for declaration in &display_list.summary().mask_declarations {
                declaration_positions_by_mask
                    .entry(declaration.mask_effect)
                    .or_default()
                    .push(declaration.record.position);
            }
        }
        let nearest_declared_mask_by_effect_index = if declaration_positions_by_mask.is_empty() {
            Vec::new()
        } else {
            nearest_effects_at_or_above_where(tree, |effect, node| {
                matches!(node.data, EffectNodeData::Mask(_)) && declaration_positions_by_mask.contains_key(&effect)
            })
        };
        let masks_with_declared_content = MasksWithDeclaredContent {
            declaration_positions_by_mask,
            nearest_declared_mask_by_effect_index,
        };
        let runs_replayed_whole = held_runs_replayed_whole
            .into_iter()
            .map(|(chunk_index, run_index, run_group_index)| {
                let (run_group_reach, first_position) =
                    reach_and_first_position_of_run_groups_replayed_whole[&run_group_index];
                let run_replayed_whole = RunReplayedWholeWhereverRunGroupShows {
                    reach: backdrop_reads.reach_extended_through_reads(run_group_reach, first_position),
                    innermost_declared_mask_around: masks_with_declared_content.nearest_declared_mask_at_or_above(
                        display_list.chunk(chunk_index).runs()[run_index.0 as usize]
                            .context
                            .effect,
                    ),
                };
                ((chunk_index, run_index), run_replayed_whole)
            })
            .collect();
        (runs_replayed_whole, masks_with_declared_content)
    }

    fn declared_masks_around_runs_replayed_whole(
        &self,
        bound_band: &BandBoundToListAndGeometry,
        masks_with_declared_content: &MasksWithDeclaredContent,
    ) -> (HashMap<EffectNodeIndex, DeclaredMaskAroundRunReplayedWhole>, bool) {
        let display_list = self.list.display_list;
        let tree = self.list.tree;
        // Each declared mask is looked at once, and every run inside it shares the chain of declared masks from it
        // out.
        let mut declared_masks: HashMap<EffectNodeIndex, DeclaredMaskAroundRunReplayedWhole> = HashMap::new();
        let mut declared_masks_not_looked_at = Vec::new();
        let mut declared_items_of_mask = HashSet::new();
        let mut declarations_read_frame_state_hashed_for_the_list = false;
        for run_replayed_whole in bound_band.runs_replayed_whole.values() {
            declared_masks_not_looked_at.clear();
            let mut mask = run_replayed_whole.innermost_declared_mask_around;
            while !mask.is_none() && !declared_masks.contains_key(&mask) {
                declared_masks_not_looked_at.push(mask);
                mask = masks_with_declared_content
                    .nearest_declared_mask_at_or_above(tree.effect_nodes[mask.0 as usize].parent);
            }
            for &mask in &declared_masks_not_looked_at {
                let next_declared_mask_around = masks_with_declared_content
                    .nearest_declared_mask_at_or_above(tree.effect_nodes[mask.0 as usize].parent);
                let mut declarations: Vec<(ListPosition, TileReplayItem, Reach<IntRect>)> = Vec::new();
                declared_items_of_mask.clear();
                for &declaration_position in &masks_with_declared_content.declaration_positions_by_mask[&mask] {
                    let table = display_list.chunk(declaration_position.chunk_index).record_table();
                    let record_index = table
                        .record_indices_between_offsets(declaration_position.offset, declaration_position.offset + 1)
                        .start;
                    declarations_read_frame_state_hashed_for_the_list |=
                        table.record(record_index).reads_frame_state_hashed_for_the_list();
                    let item =
                        bound_band.item_replaying_record(display_list, declaration_position.chunk_index, record_index);
                    if !declared_items_of_mask.insert(item) {
                        continue;
                    }
                    declarations.push((bound_band.span_of(item).begin, item, bound_band.reach_of_item(item)));
                }
                declared_masks.insert(
                    mask,
                    DeclaredMaskAroundRunReplayedWhole {
                        declarations,
                        next_declared_mask_around,
                    },
                );
            }
        }
        (declared_masks, declarations_read_frame_state_hashed_for_the_list)
    }

    /// The tiles of `band_index` bound to the list and the geometry of the frame and to the band in `stack`, with no
    /// tile keyed yet.
    pub fn bind_band(&self, stack: &LayerStack, band_index: BandIndex) -> BandTileKeys {
        BandTileKeys {
            cells: BTreeMap::new(),
            cells_looked_at: CellRange::NONE,
            bound_band: self.bind_band_to_list_and_geometry(stack, band_index),
            values_of_frame_tiles_were_keyed_with: None,
        }
    }

    /// Keys the tiles of the layers of `band_index` within `shown_rect` that are not keyed yet: each replay item of the
    /// band that reaches a tile goes into the layer of the band its paint position falls in there, and the base layer,
    /// if it is one of the band's, holds every tile. Every tile keyed before is keyed again where the frame state that
    /// records of the band read, the tree, the raster settings of a layer of the band, or the edge of `frame` that its
    /// backdrop blurs mirror at changed, and those outside `kept_rect` are forgotten. `frame` is where the frame is in
    /// the band's space, where that is not the frame's own. The band is bound to the list and the geometry of the frame.
    ///
    /// Only the cells where the band can hold something are keyed, at most `MAXIMUM_CELLS_ONE_BAND_KEYS_IN_A_FRAME` of
    /// them, so the work and the tiles of a frame have a bound whatever rect of the space the frame shows.
    #[allow(clippy::too_many_arguments)]
    pub fn key_shown_tiles(
        &self,
        band_tile_keys: &mut BandTileKeys,
        stack: &LayerStack,
        band_index: BandIndex,
        raster_settings_hash_by_layer: &[ContentHash],
        shown_rect: IntRect,
        kept_rect: IntRect,
        frame: Option<IntRect>,
    ) -> TileKeyingCounts {
        let list = self.list;
        let band = &stack.bands[band_index.0];
        band_tile_keys
            .cells
            .retain(|coord, _| !coord.cell_rect().intersected(kept_rect).is_empty());
        band_tile_keys.cells_looked_at = band_tile_keys
            .cells_looked_at
            .intersected(CellRange::touched_by(kept_rect));
        let bound_band = &band_tile_keys.bound_band;
        let new_coords = coords_to_key(
            &mut band_tile_keys.cells,
            &mut band_tile_keys.cells_looked_at,
            bound_band,
            stack.base_layer == band.layer_below_all_cuts,
            shown_rect,
        );

        let mut counts = TileKeyingCounts::default();
        let mut raster_settings_hasher = ContentHasher::new();
        for layer_index in
            std::iter::once(band.layer_below_all_cuts).chain(band.cuts.iter().map(|cut| cut.layer_above_cut))
        {
            raster_settings_hasher.write_content_hash(raster_settings_hash_by_layer[layer_index.0]);
        }
        let values_of_frame = ValuesOfFrameThatTilesOfBandRead {
            frame_state_hash: bound_band
                .replays_records_reading_frame_state_hashed_for_the_list
                .then_some(list.frame_state_hash),
            tree_copy_number: list.tree.copy.number(),
            raster_settings_hash_of_layers_of_band: raster_settings_hasher.finish(),
            frame_edge_backdrop_blurs_mirror_at: bound_band.backdrop_reads.frame_edge_blurs_mirror_at(frame),
        };
        if band_tile_keys
            .values_of_frame_tiles_were_keyed_with
            .replace(values_of_frame)
            != Some(values_of_frame)
        {
            for (coord, tiles) in &mut band_tile_keys.cells {
                counts.tiles_keyed +=
                    self.key_tiles_of_cell(bound_band, *coord, tiles, raster_settings_hash_by_layer, frame);
            }
        }

        if new_coords.is_empty() {
            return counts;
        }
        for (coord, mut tiles) in self.bin_new_cells(bound_band, stack, band_index, &new_coords) {
            counts.tiles_keyed +=
                self.key_tiles_of_cell(bound_band, coord, &mut tiles, raster_settings_hash_by_layer, frame);
            let band_holds_something_in_cell = !tiles.is_empty();
            if band_holds_something_in_cell {
                counts.cells_binned += 1;
            }
            band_tile_keys.cells.insert(coord, tiles);
        }
        counts
    }

    /// The tiles of the band at each of `new_coords`, not hashed yet: each replay item of the band that reaches a cell
    /// goes into the layer of the band its paint position falls in there, and the base layer, if it is the band's layer
    /// below all cuts, holds every cell.
    fn bin_new_cells(
        &self,
        bound_band: &BandBoundToListAndGeometry,
        stack: &LayerStack,
        band_index: BandIndex,
        new_coords: &[TileCoord],
    ) -> Vec<(TileCoord, Vec<KeyedTile>)> {
        let band = &stack.bands[band_index.0];
        let base_layer = (stack.base_layer == band.layer_below_all_cuts).then_some(stack.base_layer);
        let new_cells_area = new_coords
            .iter()
            .fold(IntRect::default(), |cells, coord| cells.united(coord.cell_rect()));
        // Per layer that items go into in the cell, the items, and the declared masks whose declarations those items hold.
        type ItemsOfLayerInCell = (
            LayerIndex,
            Vec<(ListPosition, TileReplayItem, Reach<IntRect>)>,
            HashSet<EffectNodeIndex>,
        );
        struct BinOfCell<'a> {
            cuts: CutsOfCell<'a>,
            position_of_layer_by_count_of_cuts_below: Vec<Option<usize>>,
            items_by_layer: Vec<ItemsOfLayerInCell>,
        }
        let mut bins: BTreeMap<TileCoord, BinOfCell<'_>> = new_coords
            .iter()
            .map(|coord| {
                let cuts = CutsOfCell::of(band, coord.cell_rect());
                let layer_count = cuts.cuts().count() + 1;
                (
                    *coord,
                    BinOfCell {
                        cuts,
                        position_of_layer_by_count_of_cuts_below: vec![None; layer_count],
                        items_by_layer: Vec::new(),
                    },
                )
            })
            .collect();
        let mut add_to_bins = |paint_position: ListPosition,
                               item: TileReplayItem,
                               reach: Reach<IntRect>,
                               innermost_declared_mask_around_item: EffectNodeIndex| {
            let reach_in_new_cells = reach.part_of(new_cells_area);
            if reach_in_new_cells.is_empty() {
                return;
            }
            // The new cells can lie far apart, with many more cells between them than they are.
            for_each_entry_within(&mut bins, CellRange::touched_by(reach_in_new_cells), |_, bin| {
                let (layer_index, count_of_cuts_below) = bin.cuts.layer_of_record(paint_position);
                // A frame composites only the base layer and the layers the stack says can hold content. A stretch and
                // a placed record range both take their reach from `RasterSpaceGeometry::reach_of_content`, the
                // stretch for the ink of its runs, so a range stays within the reach of its stretch wherever the
                // space's extent lets it show. A stack from another process can say otherwise, which only leaves out
                // what the item draws there.
                if layer_index != stack.base_layer && !stack.layers[layer_index.0].may_hold_content {
                    return;
                }
                let position_of_layer = match bin.position_of_layer_by_count_of_cuts_below[count_of_cuts_below] {
                    Some(position) => position,
                    None => {
                        bin.items_by_layer.push((layer_index, Vec::new(), HashSet::new()));
                        let position = bin.items_by_layer.len() - 1;
                        bin.position_of_layer_by_count_of_cuts_below[count_of_cuts_below] = Some(position);
                        position
                    }
                };
                let (_, items_of_layer, declared_masks_of_layer) = &mut bin.items_by_layer[position_of_layer];
                items_of_layer.push((paint_position, item, reach));
                // Where the layer holds the declarations of a mask, it holds those of the masks around it too.
                let mut mask = innermost_declared_mask_around_item;
                while !mask.is_none() && declared_masks_of_layer.insert(mask) {
                    let declared_mask = &bound_band.declared_masks_around_runs_replayed_whole[&mask];
                    items_of_layer.extend_from_slice(&declared_mask.declarations);
                    mask = declared_mask.next_declared_mask_around;
                }
            });
        };
        match &bound_band.items_replayed_in_every_tile {
            Some(items_replayed_in_every_tile) => {
                for &(paint_position, item) in items_replayed_in_every_tile {
                    add_to_bins(paint_position, item, Reach::Anywhere, EffectNodeIndex::NONE);
                }
            }
            None => {
                for held_chunk in &bound_band.held_chunks {
                    if held_chunk.reach_of_held_records.part_of(new_cells_area).is_empty() {
                        continue;
                    }
                    bound_band.for_each_held_item(held_chunk, &mut add_to_bins);
                }
            }
        }

        bins.into_iter()
            .map(|(coord, bin)| {
                let BinOfCell {
                    cuts: cuts_of_cell,
                    items_by_layer: mut bin,
                    ..
                } = bin;
                // The base layer is the band's layer below all cuts.
                if let Some(base_layer) = base_layer
                    && !bin.iter().any(|(layer_index, _, _)| *layer_index == base_layer)
                {
                    bin.push((base_layer, Vec::new(), HashSet::new()));
                }
                let cell = coord.cell_rect();
                let raster_margin_in_device_pixels = bound_band.backdrop_reads.raster_margin_of_cell(cell);
                let cuts = cuts_of_cell.barrier_cuts();
                let tiles = bin
                    .into_iter()
                    .map(|(layer_index, mut items_of_layer, _)| {
                        items_of_layer.sort_unstable_by_key(|(paint_position, item, _)| (*paint_position, *item));
                        items_of_layer.dedup_by_key(|(_, item, _)| *item);
                        let content_bounds =
                            items_of_layer
                                .iter()
                                .fold(IntRect::default(), |bounds, (_, _, reach)| match reach {
                                    Reach::Within(rect) => bounds.united(rect.intersected(cell)),
                                    Reach::Anywhere => cell,
                                    Reach::Nothing => bounds,
                                });
                        KeyedTile {
                            layer_index,
                            coord,
                            content_hash: 0,
                            raster_margin_in_device_pixels,
                            cuts: cuts.clone(),
                            content_bounds,
                            replay_items_in_paint_order: items_of_layer.into_iter().map(|(_, item, _)| item).collect(),
                        }
                    })
                    .collect();
                (coord, tiles)
            })
            .collect()
    }

    /// Hashes the tiles of the cell at `coord`, which are keyed together with one raster margin, and returns how many it
    /// hashed.
    fn key_tiles_of_cell(
        &self,
        bound_band: &BandBoundToListAndGeometry,
        coord: TileCoord,
        tiles: &mut [KeyedTile],
        raster_settings_hash_by_layer: &[ContentHash],
        frame: Option<IntRect>,
    ) -> usize {
        let frame_edge_backdrop_blurs_mirror_at = bound_band
            .backdrop_reads
            .frame_edge_blurs_of_cell_mirror_at(coord.cell_rect(), frame);
        let mut near_area = tiles
            .first()
            .and_then(|tile| ContentHashesNearArea::of_cell(coord, tile.raster_margin_in_device_pixels));
        for tile in tiles.iter_mut() {
            let mut hasher = ContentHasher::new();
            hasher.write_content_hash(raster_settings_hash_by_layer[tile.layer_index.0]);
            hasher.write_i32(coord.column);
            hasher.write_i32(coord.row);
            hasher.write_bool(self.list.replays_every_record_into_every_tile);
            match frame_edge_backdrop_blurs_mirror_at {
                Some(frame) => hasher.write_int_rect(frame),
                None => hasher.write_u32(0),
            }
            for &item in &tile.replay_items_in_paint_order {
                hasher.write_content_hash(self.content_hash_of_replay_item(bound_band, item, near_area.as_mut()));
                // Depth-sorted planes replay in an order their sorting context decides.
                if self.list.tree_sorts_planes_by_depth
                    && let Some(root) = self.root_of_context_sorting_item_by_depth(bound_band, item)
                {
                    hasher.write_u32(1);
                    hasher.write_u32(root.map_or(u32::MAX, |root| root.0));
                }
            }
            tile.content_hash = hasher.finish();
        }
        tiles.len()
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::builder::DisplayListBuilder;
    use crate::display_list::commands::{
        ClipMode, DeclareMaskContent, FillRect, FillRoundedRectRing, VISUAL_VIEWPORT_NODE_INDEX,
    };
    use crate::display_list::layers::planning::{LayerStackInputs, plan_layer_stack};
    use crate::display_list::layers::{LayerIdentity, LayerStack, LayeringObstacles, SpaceToFrame, WhyListIsOneLayer};
    use crate::display_list::placement::{RasterSpaceIndex, RasterSpaces, WhatEverySpaceOfTreeShares};
    use crate::display_list::test_fixtures::{
        GREEN, ListOfOneChunk, NO_FRAME_STATE, VIEWPORT, animation_of, canvas, chunk_of, effects, fill, frosted_glass,
        opacity_animation_of, raster_settings_hash_by_layer, transform, transform_animation_of,
        tree_with_document_scroll, tree_with_root_isolation_effect,
    };
    use crate::display_list::tiles::TILE_SIZE_IN_DEVICE_PIXELS;
    use crate::host::FfiVisualAnimationTargetKind;
    use crate::visual_context::{EffectsData, SpatialData, TransformData, TransformDataRole};
    use libgfx_rust::{AffineTransform, CompositingAndBlendingOperator};

    const RED: Color = Color::from_rgb(255, 0, 0);
    const BLUE: Color = Color::from_rgb(0, 0, 255);

    // The recorder declares a mask's content under the effects of what the mask holds, so no page has a declaration made
    // around the mask.
    #[test]
    fn a_mask_declared_around_its_mask_is_replayed_with_what_it_masks() {
        use crate::visual_context::{MaskData, MaskLayerOrigin};
        let (mut tree, root) = tree_with_root_isolation_effect(1.0);
        let masked = tree.append_effect_node_under(
            root,
            EffectNodeData::Mask(MaskData {
                rect: IntRect::new(0, 0, 100, 100),
                kind: libgfx_rust::MaskKind::Alpha,
                origin: MaskLayerOrigin::CssMaskLayers,
                masks_backdrop: false,
            }),
        );
        let mut list = ListOfOneChunk::default().with(fill(VIEWPORT, GREEN), root);
        let declaration_position = ListPosition::new(ChunkIndexInList(0), list.builder.byte_size() as u32);
        let declaration = list.builder.begin_group::<DeclareMaskContent>(Vec::new());
        list.builder.append(&fill(IntRect::new(0, 0, 50, 50), RED), &[], root);
        let content = list.builder.group_content_span(&declaration);
        list.builder.finish_group(
            declaration,
            &DeclareMaskContent {
                rect: IntRect::new(0, 0, 100, 100),
                effect: masked.effect,
                content,
            },
            root,
        );
        let masked_position = ListPosition::new(ChunkIndexInList(0), list.builder.byte_size() as u32);
        list = list.with(fill(IntRect::new(10, 10, 80, 80), BLUE), masked);

        let display_list = DisplayListStorage::of_builder(&list.builder);
        let stack = plan_layer_stack(&LayerStackInputs {
            list: &display_list,
            tree: &tree,
            scroll_offsets: &[],
            viewport_rect: VIEWPORT,
        })
        .stack;
        assert_eq!(stack.why_list_is_one_layer, None);
        let band = BandIndex(
            stack
                .bands
                .iter()
                .position(|band| band.record_spans.iter().any(|span| span.contains(masked_position)))
                .unwrap(),
        );
        let (band_tile_keys, _) = band_tile_keys_after_frames_showing(
            (&display_list, &tree, &stack),
            RasterSpaceIndex::VIEWPORT,
            band,
            &[VIEWPORT],
        );
        let tile_with_masked_content = band_tile_keys
            .tiles_within(TileCoord::new(0, 0).cell_rect())
            .find(|tile| {
                band_tile_keys
                    .record_spans_of_tiles([*tile])
                    .iter()
                    .any(|span| span.contains(masked_position))
            })
            .unwrap();
        let spans = band_tile_keys.record_spans_of_tiles([tile_with_masked_content]);
        assert!(spans.iter().any(|span| span.contains(declaration_position)));
        assert!(spans.windows(2).all(|pair| pair[0].end <= pair[1].begin));
    }

    fn stack_of(display_list: &DisplayListStorage, tree: &VisualContextTree) -> LayerStack {
        plan_layer_stack(&LayerStackInputs {
            list: display_list,
            tree,
            scroll_offsets: &[],
            viewport_rect: VIEWPORT,
        })
        .stack
    }

    fn geometry_of_space(
        tree: &VisualContextTree,
        stack: &LayerStack,
        raster_space_index: RasterSpaceIndex,
    ) -> RasterSpaceGeometry {
        let spaces = RasterSpaces::of_spaces(tree, stack.spaces.clone());
        RasterSpaceGeometry::new(
            tree,
            &spaces,
            raster_space_index,
            &[],
            FloatMatrix4x4::identity(),
            &WhatEverySpaceOfTreeShares::of(tree),
        )
    }

    // Keys the tiles of the band within `shown_rect`, and forgets those outside it.
    fn key_band(
        raster_space: &TileKeyingOfRasterSpace<'_>,
        band_tile_keys: &mut BandTileKeys,
        stack: &LayerStack,
        band_index: BandIndex,
        shown_rect: IntRect,
    ) -> TileKeyingCounts {
        raster_space.key_shown_tiles(
            band_tile_keys,
            stack,
            band_index,
            &raster_settings_hash_by_layer(stack),
            shown_rect,
            shown_rect,
            None,
        )
    }

    fn tiles_within(band_tile_keys: &BandTileKeys, rect: IntRect) -> Vec<(TileCoord, LayerIndex, ContentHash)> {
        band_tile_keys
            .tiles_within(rect)
            .map(|tile| (tile.coord, tile.layer_index, tile.content_hash))
            .collect()
    }

    // Each frame binds the band again under a geometry of its own.
    fn key_band_in_frames_in_turn(
        frames: &[(&DisplayListStorage, &VisualContextTree, &LayerStack)],
        raster_space_index: RasterSpaceIndex,
        band_index: BandIndex,
    ) -> Vec<(u64, TileKeyingCounts, Vec<(TileCoord, LayerIndex, ContentHash)>)> {
        frames
            .iter()
            .map(|&(display_list, tree, stack)| {
                let list_keying = TileKeyingOfList::new(display_list, tree, stack, NO_FRAME_STATE);
                let geometry = geometry_of_space(tree, stack, raster_space_index);
                let raster_space = TileKeyingOfRasterSpace::new(&list_keying, &geometry);
                let mut band_tile_keys = raster_space.bind_band(stack, band_index);
                let counts = key_band(&raster_space, &mut band_tile_keys, stack, band_index, VIEWPORT);
                (
                    raster_space.chunks_placed(),
                    counts,
                    tiles_within(&band_tile_keys, VIEWPORT),
                )
            })
            .collect()
    }

    fn sliding_box_tree_and_list(
        overshooting_easing: bool,
    ) -> (VisualContextTree, Arc<DisplayListStorage>, SpatialNodeIndex) {
        use crate::host::FfiVisualAnimationTransformOperationKind;
        use crate::visual_animation::VisualAnimation;
        let (mut tree, root) = tree_with_root_isolation_effect(1.0);
        let moving_node = tree.append_spatial(
            SpatialData::Transform(TransformData {
                matrix: FloatMatrix4x4::identity(),
                origin: FloatPoint::default(),
                sorting_context_root_index: None,
                flattens_inherited_transform: false,
                role: TransformDataRole::CssTransform,
                synthetic_plane: false,
                establishes_sorting_context: false,
                bounds: FloatRect::new(0.0, 0.0, 50.0, 50.0),
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let easing = if overshooting_easing {
            crate::easing::Easing::CubicBezier {
                x1: 0.3,
                y1: 1.6,
                x2: 0.6,
                y2: 1.0,
            }
        } else {
            crate::easing::Easing::default()
        };
        tree.set_visual_animations(vec![VisualAnimation {
            easing,
            ..transform_animation_of(
                moving_node,
                FfiVisualAnimationTransformOperationKind::TranslateX,
                0.0,
                400.0,
            )
        }]);
        let moving = ContextRef {
            spatial: moving_node,
            ..root
        };
        let display_list = DisplayListStorage::of_chunks(vec![
            chunk_of(fill(VIEWPORT, GREEN), root),
            chunk_of(fill(IntRect::new(100, 300, 50, 50), BLUE), moving),
            chunk_of(fill(IntRect::new(600, 400, 20, 20), RED), root),
        ])
        .unwrap();
        (tree, display_list, moving_node)
    }

    // An easing that overshoots leaves nothing to bound where the box goes, so it stays in the tiles of the viewport.
    #[test]
    fn an_animation_sample_places_again_only_the_chunks_under_the_node_it_moves() {
        let (tree, display_list, _) = sliding_box_tree_and_list(true);
        let at_start = tree.with_visual_animation_samples(0);
        let halfway = tree.with_visual_animation_samples(500_000_000);
        let stack = stack_of(&display_list, &at_start);
        assert_eq!(stack.spaces.len(), 2);
        assert_eq!(stack.nodes.len(), 1);
        assert_eq!(stack_of(&display_list, &halfway).layers, stack.layers);

        let turns = key_band_in_frames_in_turn(
            &[(&display_list, &at_start, &stack), (&display_list, &halfway, &stack)],
            RasterSpaceIndex::VIEWPORT,
            BandIndex(0),
        );
        assert_eq!(turns[0].0, 3);
        // Only the chunk under the moving node is placed again.
        assert_eq!(turns[1].0, 1);
    }

    #[test]
    fn an_animation_sample_leaves_the_tiles_of_the_space_of_the_node_it_moves_alike() {
        let (tree, display_list, moving_node) = sliding_box_tree_and_list(false);
        let at_start = tree.with_visual_animation_samples(0);
        let halfway = tree.with_visual_animation_samples(500_000_000);
        let stack = stack_of(&display_list, &at_start);
        let animated_space = RasterSpaceIndex(1);
        assert_eq!(stack.spaces[animated_space.0].anchor, moving_node);
        let turns = key_band_in_frames_in_turn(
            &[(&display_list, &at_start, &stack), (&display_list, &halfway, &stack)],
            animated_space,
            band_of_space(&stack, animated_space),
        );
        assert_eq!(turns[0].0, 1);
        let (chunks_placed, _, hashes) = &turns[1];
        assert_eq!(*chunks_placed, 0);
        assert_eq!(hashes, &turns[0].2);
    }

    // As the compositor keys a band across the samples of its animations where the plan says the space of the band
    // holds nothing that moves within it: under the geometry of the first frame, to which the band is bound once.
    fn counts_and_viewport_tile_hashes_of_samples_keyed_in_turn_under_one_geometry(
        display_list: &DisplayListStorage,
        stack: &LayerStack,
        band_index: BandIndex,
        samples: &[&VisualContextTree],
    ) -> Vec<(TileKeyingCounts, Vec<(TileCoord, LayerIndex, ContentHash)>)> {
        let raster_space_index = stack.nodes[stack.bands[band_index.0].placement_node_index.0].raster_space_index;
        assert!(!stack.space_holds_content_that_moves_within_it(raster_space_index));
        let geometry = geometry_of_space(samples[0], stack, raster_space_index);
        let mut band_tile_keys_bound_in_first_frame = None;
        samples
            .iter()
            .map(|tree| {
                let list_keying = TileKeyingOfList::new(display_list, tree, stack, NO_FRAME_STATE);
                let raster_space = TileKeyingOfRasterSpace::new(&list_keying, &geometry);
                let band_tile_keys = band_tile_keys_bound_in_first_frame
                    .get_or_insert_with(|| raster_space.bind_band(stack, band_index));
                let counts = key_band(&raster_space, band_tile_keys, stack, band_index, VIEWPORT);
                (counts, tiles_within(band_tile_keys, VIEWPORT))
            })
            .collect()
    }

    #[test]
    fn a_ring_whose_inner_edges_lie_past_the_coordinate_range_is_no_device_aligned_fill() {
        let (tree, root) = tree_with_root_isolation_effect(1.0);
        let near_the_limit = i32::MAX - 100;
        let list = ListOfOneChunk::default().with(
            FillRoundedRectRing {
                rect: IntRect::new(near_the_limit, near_the_limit, 200, 200),
                corner_radii: CornerRadii::default(),
                top_width: 150,
                right_width: 10,
                bottom_width: 10,
                left_width: 150,
                color: GREEN,
            },
            root,
        );
        let display_list = DisplayListStorage::of_builder(&list.builder);
        let stack = stack_of(&display_list, &tree);
        let list_keying = TileKeyingOfList::new(&display_list, &tree, &stack, NO_FRAME_STATE);
        let geometry = geometry_of_space(&tree, &stack, RasterSpaceIndex::VIEWPORT);
        let raster_space = TileKeyingOfRasterSpace::new(&list_keying, &geometry);
        let chunk = display_list.chunk(ChunkIndexInList(0));
        let ring_record = chunk.record_table().record(RecordIndexInChunk(0));
        assert!(
            raster_space
                .device_aligned_fill_of_record(chunk, ring_record, root)
                .is_none()
        );
    }

    struct CanvasesAtGeneration(u64);

    impl LiveContentVersions for CanvasesAtGeneration {
        fn canvas_content_generation(&self, _: CanvasId) -> u64 {
            self.0
        }
        fn video_frame_id(&self, _: VideoSinkHandle) -> Option<u64> {
            None
        }
        fn image_frame_pixel_generation(&self, _: ImageFrameResourceId) -> Option<u64> {
            None
        }
        fn nested_context_frame_generation(&self, _: CompositorContextId) -> Option<u64> {
            None
        }
    }

    // How many tiles each frame keys of the band that holds the record of `run_index`, under one geometry, as the
    // canvases of the list are at a new generation in each frame, and the tiles of the band in the viewport after it.
    fn tiles_keyed_and_kept_in_frames_with_a_new_canvas_generation(
        list: &ListOfOneChunk,
        tree: &VisualContextTree,
        run_index: usize,
    ) -> Vec<(usize, Vec<(TileCoord, LayerIndex, ContentHash)>)> {
        let display_list = DisplayListStorage::of_builder(&list.builder);
        let stack = stack_of(&display_list, tree);
        let position = display_list.span_of_run(run_index).begin;
        let band_index = BandIndex(
            stack
                .bands
                .iter()
                .position(|band| band.record_spans.iter().any(|span| span.contains(position)))
                .expect("a band holds the record"),
        );
        let raster_space_index = stack.nodes[stack.bands[band_index.0].placement_node_index.0].raster_space_index;
        let geometry = geometry_of_space(tree, &stack, raster_space_index);
        let mut band_tile_keys_bound_in_first_frame = None;
        (0..3)
            .map(|generation| {
                let live_content = CanvasesAtGeneration(generation);
                let frame_state = FrameStateReadByReplay {
                    live_content: &live_content,
                    ..NO_FRAME_STATE
                };
                let list_keying = TileKeyingOfList::new(&display_list, tree, &stack, frame_state);
                let raster_space = TileKeyingOfRasterSpace::new(&list_keying, &geometry);
                let band_tile_keys = band_tile_keys_bound_in_first_frame
                    .get_or_insert_with(|| raster_space.bind_band(&stack, band_index));
                let tiles_keyed = key_band(&raster_space, band_tile_keys, &stack, band_index, VIEWPORT).tiles_keyed;
                (tiles_keyed, tiles_within(band_tile_keys, VIEWPORT))
            })
            .collect()
    }

    #[test]
    fn a_new_canvas_frame_keys_again_only_the_bands_whose_records_draw_the_canvas() {
        let canvas_rect = IntRect::new(100, 100, 200, 100);
        let (tree, root) = tree_with_root_isolation_effect(1.0);
        let list_with_canvas_in_own_layer = ListOfOneChunk::default()
            .with(fill(VIEWPORT, GREEN), root)
            .with(canvas(canvas_rect, 1), root);
        let display_list = DisplayListStorage::of_builder(&list_with_canvas_in_own_layer.builder);
        assert!(
            stack_of(&display_list, &tree)
                .layers
                .iter()
                .any(|layer| matches!(layer.identity, LayerIdentity::Media { .. }))
        );
        let frames =
            tiles_keyed_and_kept_in_frames_with_a_new_canvas_generation(&list_with_canvas_in_own_layer, &tree, 0);
        assert!(frames[0].0 > 0);
        assert_eq!([frames[1].0, frames[2].0], [0, 0]);

        let (mut tree, root) = tree_with_root_isolation_effect(1.0);
        let moved_by_filter = tree.append_effect_node_under(
            root,
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: Some(Arc::new(
                    libgfx_rust::filter::Filter::Offset {
                        dx: 40.0,
                        dy: 0.0,
                        input: None,
                    }
                    .serialize(),
                )),
                backdrop_filter: None,
            }),
        );
        let list_with_canvas_in_filter = ListOfOneChunk::default()
            .with(fill(VIEWPORT, GREEN), root)
            .with(canvas(canvas_rect, 1), moved_by_filter);
        let display_list = DisplayListStorage::of_builder(&list_with_canvas_in_filter.builder);
        assert!(
            !stack_of(&display_list, &tree)
                .layers
                .iter()
                .any(|layer| matches!(layer.identity, LayerIdentity::Media { .. }))
        );
        let frames = tiles_keyed_and_kept_in_frames_with_a_new_canvas_generation(&list_with_canvas_in_filter, &tree, 1);
        assert!(frames[0].0 > 0);
        assert_eq!([frames[1].0, frames[2].0], [frames[0].0, frames[0].0]);
        let cell_drawing_the_canvas = cell_of(canvas_rect);
        assert_eq!(
            cells_whose_hashes_differ(&frames[0].1, &frames[1].1),
            [cell_drawing_the_canvas]
        );
        assert_eq!(
            cells_whose_hashes_differ(&frames[1].1, &frames[2].1),
            [cell_drawing_the_canvas]
        );
    }

    fn cells_whose_hashes_differ(
        before: &[(TileCoord, LayerIndex, ContentHash)],
        after: &[(TileCoord, LayerIndex, ContentHash)],
    ) -> Vec<TileCoord> {
        before
            .iter()
            .zip(after)
            .filter(|(before, after)| before != after)
            .map(|(before, _)| before.0)
            .collect()
    }

    fn cell_of(rect: IntRect) -> TileCoord {
        TileCoord::new(rect.x / TILE_SIZE_IN_DEVICE_PIXELS, rect.y / TILE_SIZE_IN_DEVICE_PIXELS)
    }

    // Each sample keys every tile of the band again, and only the tile that draws a value the sample changed gets a new
    // hash.
    fn assert_only_the_cell_drawing_the_sampled_value_gets_a_new_hash(
        display_list: &DisplayListStorage,
        stack: &LayerStack,
        band_index: BandIndex,
        tree: &VisualContextTree,
        cell_drawing_the_sampled_value: TileCoord,
    ) {
        let at_start = tree.with_visual_animation_samples(0);
        let halfway = tree.with_visual_animation_samples(500_000_000);
        let halfway_again = tree.with_visual_animation_samples(500_000_000);
        let turns = counts_and_viewport_tile_hashes_of_samples_keyed_in_turn_under_one_geometry(
            display_list,
            stack,
            band_index,
            &[&at_start, &halfway, &halfway_again],
        );
        let keyed_from_nothing = key_band_in_frames_in_turn(
            &[(display_list, &halfway, stack)],
            RasterSpaceIndex::VIEWPORT,
            band_index,
        );
        assert_eq!(turns[1].1, keyed_from_nothing[0].2);
        assert_eq!(
            cells_whose_hashes_differ(&turns[0].1, &turns[1].1),
            [cell_drawing_the_sampled_value]
        );
        assert_eq!(turns[1].0.tiles_keyed, turns[1].1.len());
        assert_eq!(turns[2].0.tiles_keyed, turns[2].1.len());
        assert_eq!(turns[2].1, turns[1].1);
    }

    #[test]
    fn a_tile_kept_through_an_animation_sample_reads_the_sampled_background_color_again() {
        use crate::visual_animation::{VisualAnimationKeyframe, VisualAnimationValue};
        let (mut tree, root) = tree_with_root_isolation_effect(1.0);
        let animated_color = tree.append_effect(
            EffectNodeData::BackgroundColorAnimation,
            root.effect,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let color_at = |offset: f64, color: Color| VisualAnimationKeyframe {
            offset,
            easing: crate::easing::Easing::default(),
            value: VisualAnimationValue::BackgroundColor(color),
        };
        tree.set_visual_animations(vec![animation_of(
            FfiVisualAnimationTargetKind::BackgroundColor,
            animated_color.0,
            vec![color_at(0.0, RED), color_at(1.0, BLUE)],
        )]);
        let animated_box = IntRect::new(600, 400, 20, 20);
        let display_list = DisplayListStorage::of_chunks(vec![
            chunk_of(fill(VIEWPORT, GREEN), root),
            chunk_of(
                FillRect {
                    background_color_animation_effect: animated_color,
                    ..fill(animated_box, RED)
                },
                ContextRef {
                    effect: animated_color,
                    ..root
                },
            ),
        ])
        .unwrap();
        let stack = stack_of(&display_list, &tree.with_visual_animation_samples(0));
        assert_only_the_cell_drawing_the_sampled_value_gets_a_new_hash(
            &display_list,
            &stack,
            BandIndex(0),
            &tree,
            cell_of(animated_box),
        );
    }

    struct ListWithBoxWhoseAnimatedOpacityHasNoLayer {
        display_list: Arc<DisplayListStorage>,
        stack: LayerStack,
        effect_without_layer: EffectNodeIndex,
        band_of_box_without_layer: BandIndex,
    }

    const BOX_WHOSE_ANIMATED_OPACITY_HAS_NO_LAYER: IntRect = IntRect::new(600, 550, 20, 20);

    // The compositor gives layers of their own to the first effects in paint order whose opacity it animates, up to a
    // limit, and tiles draw the opacity of the others. The box of the last one is inside `around_box_without_layer`.
    fn list_with_a_box_whose_animated_opacity_has_no_layer(
        tree: &mut VisualContextTree,
        root: ContextRef,
        around_box_without_layer: ContextRef,
    ) -> ListWithBoxWhoseAnimatedOpacityHasNoLayer {
        use crate::display_list::layers::planning::MAXIMUM_ANIMATED_NODES_WITH_LAYERS;
        use crate::visual_animation::VisualAnimation;
        let faded: Vec<ContextRef> = (0..=MAXIMUM_ANIMATED_NODES_WITH_LAYERS)
            .map(|index| {
                let parent = if index == MAXIMUM_ANIMATED_NODES_WITH_LAYERS {
                    around_box_without_layer
                } else {
                    root
                };
                tree.append_effect_node_under(parent, effects(1.0, CompositingAndBlendingOperator::Normal))
            })
            .collect();
        tree.set_visual_animations(vec![VisualAnimation {
            node_indices: faded.iter().map(|context| context.effect.0).collect(),
            ..opacity_animation_of(faded[0].effect, 1.0, 0.2)
        }]);
        let mut chunks = vec![chunk_of(fill(VIEWPORT, GREEN), root)];
        chunks.extend(faded.iter().enumerate().map(|(index, &context)| {
            let rect = if index == MAXIMUM_ANIMATED_NODES_WITH_LAYERS {
                BOX_WHOSE_ANIMATED_OPACITY_HAS_NO_LAYER
            } else {
                IntRect::new(10 + 12 * index as i32, 10, 10, 10)
            };
            chunk_of(fill(rect, BLUE), context)
        }));
        let display_list = DisplayListStorage::of_chunks(chunks).unwrap();
        let stack = stack_of(&display_list, &tree.with_visual_animation_samples(0));
        let effect_without_layer = faded[MAXIMUM_ANIMATED_NODES_WITH_LAYERS].effect;
        assert_eq!(stack.composited_effects.len(), MAXIMUM_ANIMATED_NODES_WITH_LAYERS);
        assert!(
            stack
                .composited_effects
                .iter()
                .all(|composited| composited.effect != effect_without_layer)
        );
        let position_of_box_without_layer = ListPosition::new(ChunkIndexInList(faded.len() as u32), 0);
        let band_of_box_without_layer = BandIndex(
            stack
                .bands
                .iter()
                .position(|band| {
                    band.record_spans
                        .iter()
                        .any(|span| span.contains(position_of_box_without_layer))
                })
                .expect("a band holds the box"),
        );
        ListWithBoxWhoseAnimatedOpacityHasNoLayer {
            display_list,
            stack,
            effect_without_layer,
            band_of_box_without_layer,
        }
    }

    #[test]
    fn a_tile_kept_through_an_animation_sample_reads_the_sampled_opacity_of_an_effect_without_a_layer_again() {
        let (mut tree, root) = tree_with_root_isolation_effect(1.0);
        let list = list_with_a_box_whose_animated_opacity_has_no_layer(&mut tree, root, root);
        assert_only_the_cell_drawing_the_sampled_value_gets_a_new_hash(
            &list.display_list,
            &list.stack,
            list.band_of_box_without_layer,
            &tree,
            cell_of(BOX_WHOSE_ANIMATED_OPACITY_HAS_NO_LAYER),
        );
    }

    #[test]
    fn a_kept_tile_reads_the_sampled_opacity_of_an_effect_without_a_layer_inside_a_blur_again() {
        let (mut tree, root) = tree_with_root_isolation_effect(1.0);
        let blurred = tree.append_effect_node_under(
            root,
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: Some(Arc::new(libgfx_rust::filter::Filter::blur(4.0, 4.0, None).serialize())),
                backdrop_filter: None,
            }),
        );
        let list = list_with_a_box_whose_animated_opacity_has_no_layer(&mut tree, root, blurred);
        let list_keying = TileKeyingOfList::new(&list.display_list, &tree, &list.stack, NO_FRAME_STATE);
        assert_eq!(
            list_keying
                .filters_of_effects()
                .nearest_spreading_effect_at_or_above(list.effect_without_layer),
            blurred.effect
        );
        assert_only_the_cell_drawing_the_sampled_value_gets_a_new_hash(
            &list.display_list,
            &list.stack,
            list.band_of_box_without_layer,
            &tree,
            cell_of(BOX_WHOSE_ANIMATED_OPACITY_HAS_NO_LAYER),
        );
    }

    // An effect that holds a backdrop filter gets no layer for the animation of its opacity.
    #[test]
    fn a_kept_tile_with_no_raster_margin_known_reads_the_sampled_opacity_of_an_effect_without_a_layer_again() {
        let (mut tree, root) = tree_with_root_isolation_effect(1.0);
        let faded = tree.append_effect_node_under(root, effects(1.0, CompositingAndBlendingOperator::Normal));
        tree.set_visual_animations(vec![opacity_animation_of(faded.effect, 1.0, 0.2)]);
        let region_of_glass = IntRect::new(590, 540, 40, 40);
        let blur_radius_past_largest_raster_margin = LARGEST_RASTER_MARGIN_IN_DEVICE_PIXELS as f32;
        let glass = tree.append_effect_node_under(
            faded,
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: Some(crate::visual_context::BackdropFilterData {
                    filter: Arc::new(
                        libgfx_rust::filter::Filter::blur(
                            blur_radius_past_largest_raster_margin,
                            blur_radius_past_largest_raster_margin,
                            None,
                        )
                        .serialize(),
                    ),
                    region: region_of_glass,
                    corner_radii: CornerRadii::default(),
                }),
            }),
        );
        let display_list = DisplayListStorage::of_chunks(vec![
            chunk_of(fill(VIEWPORT, GREEN), root),
            chunk_of(fill(BOX_WHOSE_ANIMATED_OPACITY_HAS_NO_LAYER, BLUE), faded),
            chunk_of(fill(region_of_glass, BLUE), glass),
        ])
        .unwrap();
        let stack = stack_of(&display_list, &tree.with_visual_animation_samples(0));
        assert!(stack.composited_effects.is_empty());
        let (band_tile_keys, _) = band_tile_keys_after_frames_showing(
            (&display_list, &tree, &stack),
            RasterSpaceIndex::VIEWPORT,
            BandIndex(0),
            &[VIEWPORT],
        );
        let cell_of_box = cell_of(BOX_WHOSE_ANIMATED_OPACITY_HAS_NO_LAYER);
        assert_eq!(
            band_tile_keys
                .tile(cell_of_box, stack.base_layer)
                .map(|tile| tile.raster_margin_in_device_pixels),
            Some(None)
        );
        assert_only_the_cell_drawing_the_sampled_value_gets_a_new_hash(
            &display_list,
            &stack,
            BandIndex(0),
            &tree,
            cell_of_box,
        );
    }

    // A tree from another process can nest plain effects as deep as it likes, each clipped by a chain of clips as deep.
    // A red plane in front of a blue plane that covers the viewport, though the red plane comes first in the list, as
    // `transform-style: preserve-3d` and `translateZ()` make them, drawn as one layer.
    #[test]
    fn the_tiles_of_one_layer_over_planes_sorted_by_depth_replay_the_whole_list_and_have_no_coverage_known() {
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let context_root = tree.append_spatial(
            SpatialData::Transform(TransformData {
                establishes_sorting_context: true,
                ..transform(VIEWPORT.to_float())
            }),
            viewport.spatial,
        );
        let mut plane_at = |matrix: FloatMatrix4x4| ContextRef {
            spatial: tree.append_spatial(
                SpatialData::Transform(TransformData {
                    matrix,
                    sorting_context_root_index: Some(context_root),
                    ..transform(VIEWPORT.to_float())
                }),
                context_root,
            ),
            ..viewport
        };
        let mut moved_forward = FloatMatrix4x4::identity();
        moved_forward.elements[2][3] = 10.0;
        let red_plane = plane_at(moved_forward);
        let blue_plane = plane_at(FloatMatrix4x4::identity());
        let display_list = ListOfOneChunk::default()
            .with(fill(VIEWPORT, RED), red_plane)
            .with(fill(VIEWPORT, BLUE), blue_plane)
            .storage();
        let stack = LayerStack::one_layer_replaying_whole_list(
            VIEWPORT,
            display_list.whole_span(),
            Some(WhyListIsOneLayer::FrameIsNotDrawnAsPlanned),
            LayeringObstacles::of(&display_list, &tree),
        );
        assert!(stack.one_layer_list_cannot_tell_which_records_show_in_tile());
        let list_keying = TileKeyingOfList::new(&display_list, &tree, &stack, NO_FRAME_STATE);
        let geometry = geometry_of_space(&tree, &stack, RasterSpaceIndex::VIEWPORT);
        let raster_space = TileKeyingOfRasterSpace::new(&list_keying, &geometry);
        let mut band_tile_keys = raster_space.bind_band(&stack, BandIndex(0));
        key_band(&raster_space, &mut band_tile_keys, &stack, BandIndex(0), VIEWPORT);
        // Walked in list order from the opaque clear color, the blue fill would cover what the red plane draws.
        let tile = band_tile_keys
            .tile(TileCoord::new(0, 0), stack.base_layer)
            .expect("the base layer holds a tile at every shown cell");
        let white = Color::from_rgb(255, 255, 255);
        assert_eq!(
            raster_space.coverage_of_tile(&band_tile_keys, tile, tile.coord.cell_rect(), white),
            TileCoverage::Unknown
        );
    }

    #[test]
    fn the_coverage_of_a_tile_under_deeply_nested_effects_and_clips_holds_what_its_fills_draw() {
        use crate::visual_context::VisualContextNodeSink;
        let (mut tree, root) = tree_with_root_isolation_effect(1.0);
        let mut innermost = root;
        for _ in 0..20_000 {
            innermost = tree.append_clip_node_under(
                innermost,
                ClipNodeData::Rect(ClipData {
                    rect: VIEWPORT.to_float(),
                    corner_radii: CornerRadii::default(),
                    mode: ClipMode::Intersect,
                }),
            );
            innermost = tree.append_effect_node_under(innermost, effects(1.0, CompositingAndBlendingOperator::Normal));
        }
        let multiplied_fill = FillRect {
            compositing_and_blending_operator: CompositingAndBlendingOperator::Multiply,
            ..fill(VIEWPORT, BLUE)
        };
        let display_list = DisplayListStorage::of_chunks(vec![
            chunk_of(fill(VIEWPORT, GREEN), innermost),
            chunk_of(multiplied_fill, innermost),
        ])
        .unwrap();
        let stack = stack_of(&display_list, &tree);
        let list_keying = TileKeyingOfList::new(&display_list, &tree, &stack, NO_FRAME_STATE);
        let geometry = geometry_of_space(&tree, &stack, RasterSpaceIndex::VIEWPORT);
        let raster_space = TileKeyingOfRasterSpace::new(&list_keying, &geometry);
        let mut band_tile_keys = raster_space.bind_band(&stack, BandIndex(0));
        key_band(&raster_space, &mut band_tile_keys, &stack, BandIndex(0), VIEWPORT);
        let tile = band_tile_keys
            .tile(TileCoord::new(0, 0), stack.base_layer)
            .expect("the base layer holds a tile at every shown cell");
        let area = tile.coord.cell_rect();
        assert_eq!(
            raster_space.coverage_of_tile(&band_tile_keys, tile, area, Color::TRANSPARENT),
            TileCoverage::Opaque
        );
    }

    fn band_of_space(stack: &LayerStack, raster_space_index: RasterSpaceIndex) -> BandIndex {
        BandIndex(
            stack
                .bands
                .iter()
                .position(|band| stack.nodes[band.placement_node_index.0].raster_space_index == raster_space_index)
                .expect("the space has a band"),
        )
    }

    // The rect of the space that the viewport shows through a transform that scales the space by `scale`.
    fn viewport_in_space_scaled_by(scale: f32) -> IntRect {
        SpaceToFrame::Transform(AffineTransform::new(scale, 0.0, 0.0, scale, 0.0, 0.0)).rect_in_space(VIEWPORT)
    }

    fn band_tile_keys_after_frames_showing(
        frame: (&DisplayListStorage, &VisualContextTree, &LayerStack),
        raster_space_index: RasterSpaceIndex,
        band_index: BandIndex,
        shown_rects: &[IntRect],
    ) -> (BandTileKeys, Vec<TileKeyingCounts>) {
        let (display_list, tree, stack) = frame;
        let list_keying = TileKeyingOfList::new(display_list, tree, stack, NO_FRAME_STATE);
        let geometry = geometry_of_space(tree, stack, raster_space_index);
        let raster_space = TileKeyingOfRasterSpace::new(&list_keying, &geometry);
        let mut band_tile_keys = raster_space.bind_band(stack, band_index);
        let counts = shown_rects
            .iter()
            .map(|&shown_rect| key_band(&raster_space, &mut band_tile_keys, stack, band_index, shown_rect))
            .collect();
        (band_tile_keys, counts)
    }

    fn viewport_filled_with_green() -> (VisualContextTree, Arc<DisplayListStorage>) {
        let (tree, root) = tree_with_root_isolation_effect(1.0);
        let display_list = DisplayListStorage::of_chunks(vec![chunk_of(fill(VIEWPORT, GREEN), root)]).unwrap();
        (tree, display_list)
    }

    // A run that the held ranges skip, and a held range over several runs.
    #[test]
    fn held_runs_are_found_by_binary_search_of_the_held_ranges() {
        let run = |offset: u32, size: u32| DisplayListCommandRun {
            offset,
            size,
            ..DisplayListCommandRun::default()
        };
        let runs = [
            run(0, 16),
            run(16, 32),
            run(48, 16),
            run(64, 16),
            run(80, 16),
            run(96, 16),
        ];
        let held_offset_ranges = [(16, 48), (64, 112)];
        let overlap =
            |run: &DisplayListCommandRun, &(begin, end): &(u32, u32)| begin < run.offset + run.size && run.offset < end;
        let held_runs_one_by_one: Vec<RunIndexInChunk> = runs
            .iter()
            .enumerate()
            .filter(|(_, run)| held_offset_ranges.iter().any(|range| overlap(run, range)))
            .map(|(run_index, _)| RunIndexInChunk(run_index as u32))
            .collect();
        assert_eq!(
            runs_within_held_offset_ranges(&runs, &held_offset_ranges),
            held_runs_one_by_one
        );
        let held_offset_ranges = [(8, 24), (40, 56), (80, 112)];
        for offset in 0..120 {
            assert_eq!(
                offset_is_held(&held_offset_ranges, offset),
                held_offset_ranges
                    .iter()
                    .any(|&(begin, end)| begin <= offset && offset < end)
            );
        }
    }

    // A tree from another process can chain clips and effects as deep as it holds nodes.
    #[test]
    fn content_inside_a_chain_of_100000_clips_and_effects_is_keyed() {
        const CHAIN_LENGTH: usize = 100_000;
        let (mut tree, mut context) = tree_with_root_isolation_effect(1.0);
        for _ in 0..CHAIN_LENGTH {
            let clip = tree.append_clip(
                ClipNodeData::Rect(ClipData {
                    rect: VIEWPORT.to_float(),
                    corner_radii: CornerRadii::default(),
                    mode: ClipMode::Intersect,
                }),
                context.clip,
                context.spatial,
            );
            context = tree.append_effect_node_under(
                ContextRef { clip, ..context },
                effects(1.0, CompositingAndBlendingOperator::Normal),
            );
        }
        let display_list =
            DisplayListStorage::of_builder(&ListOfOneChunk::default().with(fill(VIEWPORT, GREEN), context).builder);
        let stack = LayerStack::one_layer_replaying_whole_list(
            VIEWPORT,
            display_list.whole_span(),
            None,
            LayeringObstacles::default(),
        );
        let list_keying = TileKeyingOfList::new(&display_list, &tree, &stack, NO_FRAME_STATE);
        let geometry = geometry_of_space(&tree, &stack, RasterSpaceIndex::VIEWPORT);
        let raster_space = TileKeyingOfRasterSpace::new(&list_keying, &geometry);
        let mut band_tile_keys = raster_space.bind_band(&stack, BandIndex(0));
        key_band(&raster_space, &mut band_tile_keys, &stack, BandIndex(0), VIEWPORT);
        assert_eq!(tiles_within(&band_tile_keys, VIEWPORT).len(), 4);
        assert_ne!(raster_space.content_hash_of_composited_effect(context.effect), 0);
    }

    fn backdrop_read_of_viewport(read_reach: i32, spread_past_region: i32) -> BackdropReadInLayer {
        BackdropReadInLayer {
            first_record_position: ListPosition::default(),
            region_reach: Reach::Within(VIEWPORT),
            backdrop_read_reach: Reach::Within(VIEWPORT),
            read_reach_in_device_pixels: Some(read_reach),
            spread_past_region_in_device_pixels: spread_past_region,
            mirroring_blur_region: None,
        }
    }

    // A backdrop filter in a tree from another process can spread what it reads about as far as the extent of a space.
    #[test]
    fn a_tile_whose_backdrop_filters_read_farther_than_the_largest_raster_margin_has_no_margin_known() {
        let cell = TileCoord::new(0, 0).cell_rect();
        let largest = LARGEST_RASTER_MARGIN_IN_DEVICE_PIXELS;
        let at_largest = [
            backdrop_read_of_viewport(2000, largest - 2100),
            backdrop_read_of_viewport(100, 0),
        ];
        let raster_margin_of_cell =
            |reads: Vec<BackdropReadInLayer>| BackdropReadsOfBand::new(reads).raster_margin_of_cell(cell);
        assert_eq!(raster_margin_of_cell(at_largest.into()), Some(largest));
        let past_largest = [
            backdrop_read_of_viewport(2000, largest - 2100),
            backdrop_read_of_viewport(101, 0),
        ];
        assert_eq!(raster_margin_of_cell(past_largest.into()), None);
        let spread_as_far_as_the_extent: Vec<BackdropReadInLayer> =
            (0..5).map(|_| backdrop_read_of_viewport(2051, 1 << 29)).collect();
        assert_eq!(raster_margin_of_cell(spread_as_far_as_the_extent), None);
        let elsewhere = BackdropReadInLayer {
            region_reach: Reach::Within(TileCoord::new(4, 4).cell_rect()),
            ..backdrop_read_of_viewport(2051, 1 << 29)
        };
        assert_eq!(raster_margin_of_cell(vec![elsewhere]), Some(0));
    }

    // A tree from another process can put any number of backdrop filters in one band.
    #[test]
    fn a_band_follows_backdrop_reads_one_by_one_up_to_a_bound_and_takes_their_regions_as_reached_past_it() {
        let backdrop_reads = |count: usize| {
            BackdropReadsOfBand::new(
                (0..count as i32)
                    .map(|index| {
                        let region = IntRect::new(600 + index, 10, 1, 1);
                        BackdropReadInLayer {
                            first_record_position: ListPosition::new(ChunkIndexInList(0), 100 + index as u32),
                            region_reach: Reach::Within(region),
                            backdrop_read_reach: Reach::Within(region.inflated_edges(1, 1, 1, 1)),
                            read_reach_in_device_pixels: Some(1),
                            spread_past_region_in_device_pixels: 0,
                            mirroring_blur_region: None,
                        }
                    })
                    .collect(),
            )
        };
        let content_reach = Reach::Within(IntRect::new(0, 0, 10, 10));
        let painted_before_every_read = ListPosition::new(ChunkIndexInList(0), 0);
        let cell_of_regions = TileCoord::new(1, 0).cell_rect();
        let cell_of_content = TileCoord::new(0, 0).cell_rect();

        let followed = backdrop_reads(MAXIMUM_BACKDROP_READS_A_BAND_FOLLOWS_ONE_BY_ONE);
        assert_eq!(
            followed.reach_extended_through_reads(content_reach, painted_before_every_read),
            content_reach
        );
        assert_eq!(
            followed.raster_margin_of_cell(cell_of_regions),
            Some(MAXIMUM_BACKDROP_READS_A_BAND_FOLLOWS_ONE_BY_ONE as i32)
        );
        assert_eq!(followed.raster_margin_of_cell(cell_of_content), Some(0));

        let past_bound = backdrop_reads(MAXIMUM_BACKDROP_READS_A_BAND_FOLLOWS_ONE_BY_ONE + 1);
        assert_eq!(
            past_bound.reach_extended_through_reads(content_reach, painted_before_every_read),
            content_reach.united(past_bound.region_reach_of_every_read)
        );
        assert_eq!(past_bound.raster_margin_of_cell(cell_of_regions), None);
        assert_eq!(past_bound.raster_margin_of_cell(cell_of_content), Some(0));
    }

    // The region of a backdrop filter in a tree from another process can lie past the extent of any space.
    #[test]
    fn a_backdrop_filter_whose_region_lies_past_the_extent_limit_reads_within_rects_whose_edges_fit() {
        let (mut tree, root) = tree_with_root_isolation_effect(1.0);
        let region_past_extent_limit = IntRect::new(i32::MAX - 1000, 0, 1000, 100);
        let frosted = frosted_glass(&mut tree, root, region_past_extent_limit);
        let list = ListOfOneChunk::default()
            .with(fill(VIEWPORT, GREEN), root)
            .with(fill(region_past_extent_limit, BLUE), frosted);
        let display_list = DisplayListStorage::of_builder(&list.builder);
        let stack = LayerStack::one_layer_replaying_whole_list(
            VIEWPORT,
            display_list.whole_span(),
            None,
            LayeringObstacles::default(),
        );
        let list_keying = TileKeyingOfList::new(&display_list, &tree, &stack, NO_FRAME_STATE);
        let geometry = geometry_of_space(&tree, &stack, RasterSpaceIndex::VIEWPORT);
        let raster_space = TileKeyingOfRasterSpace::new(&list_keying, &geometry);
        let backdrop_read = raster_space.backdrop_read_in_layer(frosted.effect, display_list.whole_span().begin);
        let edges_fit_in_int_rect = |reach: Reach<IntRect>| match reach {
            Reach::Within(rect) => {
                rect.x.checked_add(rect.width).is_some() && rect.y.checked_add(rect.height).is_some()
            }
            Reach::Nothing | Reach::Anywhere => true,
        };
        assert!(edges_fit_in_int_rect(backdrop_read.backdrop_read_reach));
        assert!(edges_fit_in_int_rect(backdrop_read.region_reach));
        let (band_tile_keys, _) = band_tile_keys_after_frames_showing(
            (&display_list, &tree, &stack),
            RasterSpaceIndex::VIEWPORT,
            BandIndex(0),
            &[VIEWPORT],
        );
        assert_eq!(tiles_within(&band_tile_keys, VIEWPORT).len(), 4);
    }

    // A box that the compositor scales up from nothing shows at a tiny part of its raster scale at first.
    #[test]
    fn a_band_shown_at_a_tiny_part_of_its_raster_scale_keys_only_the_cells_its_content_reaches() {
        let (tree, display_list, _) = sliding_box_tree_and_list(false);
        let at_start = tree.with_visual_animation_samples(0);
        let stack = stack_of(&display_list, &at_start);
        let animated_space = RasterSpaceIndex(1);
        let band = band_of_space(&stack, animated_space);
        let shown_rect = viewport_in_space_scaled_by(1.0e-4);
        assert!(CellRange::touched_by(shown_rect).cell_count() > 100_000_000);

        let frame = (&*display_list, &at_start, &stack);
        let (keyed_at_tiny_scale, counts) =
            band_tile_keys_after_frames_showing(frame, animated_space, band, &[shown_rect, shown_rect]);
        let tiles = tiles_within(&keyed_at_tiny_scale, shown_rect);
        let (keyed_at_raster_scale, _) = band_tile_keys_after_frames_showing(frame, animated_space, band, &[VIEWPORT]);
        assert!(!tiles.is_empty());
        assert_eq!(tiles, tiles_within(&keyed_at_raster_scale, VIEWPORT));
        let mut cells_of_tiles: Vec<TileCoord> = tiles.iter().map(|(coord, _, _)| *coord).collect();
        cells_of_tiles.dedup();
        assert!(keyed_at_tiny_scale.cells.keys().copied().eq(cells_of_tiles));
        // A frame that shows the same rect again looks for no cell to key.
        assert_eq!(counts[1], TileKeyingCounts::default());
    }

    // A scroll within the rows and columns of cells that a frame showed shows no new cell, so the band must have keyed
    // every cell of those that it holds something in, also where its content lies outside the pixels that frame showed.
    #[test]
    fn a_band_keys_content_in_a_shown_cell_outside_the_shown_pixels_before_a_scroll_shows_it() {
        let (mut tree, _, document) = tree_with_document_scroll();
        let faded = tree.append_effect_node_under(document, effects(1.0, CompositingAndBlendingOperator::Normal));
        tree.set_visual_animations(vec![opacity_animation_of(faded.effect, 1.0, 0.2)]);
        let box_below_viewport = IntRect::new(100, 700, 100, 200);
        let display_list = DisplayListStorage::of_chunks(vec![
            chunk_of(fill(IntRect::new(0, 0, 800, 2600), GREEN), document),
            chunk_of(fill(box_below_viewport, BLUE), faded),
        ])
        .unwrap();
        let tree = tree.with_visual_animation_samples(0);
        let stack = stack_of(&display_list, &tree);
        let position_of_box = ListPosition::new(ChunkIndexInList(1), 0);
        let band_of_box = BandIndex(
            stack
                .bands
                .iter()
                .position(|band| band.record_spans.iter().any(|span| span.contains(position_of_box)))
                .expect("a band holds the box"),
        );
        assert_ne!(stack.bands[band_of_box.0].layer_below_all_cuts, stack.base_layer);
        let scrolled_by_200 = VIEWPORT.translated(0, 200);
        assert_eq!(CellRange::touched_by(scrolled_by_200), CellRange::touched_by(VIEWPORT));
        assert!(VIEWPORT.intersected(box_below_viewport).is_empty());

        let document_space = stack.nodes[stack.bands[band_of_box.0].placement_node_index.0].raster_space_index;
        assert_ne!(document_space, RasterSpaceIndex::VIEWPORT);
        let (band_tile_keys, _) = band_tile_keys_after_frames_showing(
            (&display_list, &tree, &stack),
            document_space,
            band_of_box,
            &[VIEWPORT, scrolled_by_200],
        );
        assert_eq!(
            tiles_within(&band_tile_keys, scrolled_by_200.intersected(box_below_viewport))
                .iter()
                .map(|(coord, _, _)| *coord)
                .collect::<Vec<_>>(),
            [cell_of(box_below_viewport)]
        );
    }

    #[test]
    fn a_band_keys_at_most_the_budget_of_cells_nearest_to_the_middle_of_what_it_shows() {
        let (tree, display_list) = viewport_filled_with_green();
        let stack = stack_of(&display_list, &tree);
        assert_eq!(stack.base_layer, stack.bands[0].layer_below_all_cuts);
        // The band of the base layer holds a tile at every cell it shows.
        let shown_rect = viewport_in_space_scaled_by(1.0e-4);
        let moved_shown_rect = shown_rect.translated(10 * TILE_SIZE_IN_DEVICE_PIXELS, 0);
        let (band_tile_keys, counts) = band_tile_keys_after_frames_showing(
            (&display_list, &tree, &stack),
            RasterSpaceIndex::VIEWPORT,
            BandIndex(0),
            &[shown_rect, shown_rect, moved_shown_rect],
        );
        let middle_of = |rect: IntRect| TileCoord::of_pixel(rect.x + rect.width / 2, rect.y + rect.height / 2);
        assert!(
            counts
                .iter()
                .all(|counts| counts.tiles_keyed <= MAXIMUM_CELLS_ONE_BAND_KEYS_IN_A_FRAME)
        );
        assert!(counts[0].tiles_keyed > MAXIMUM_CELLS_ONE_BAND_KEYS_IN_A_FRAME / 2);
        assert_eq!(counts[1], TileKeyingCounts::default());
        // Moving the window by ten columns keys the cells it now holds and forgets those it left.
        let window = CellRange::around(middle_of(moved_shown_rect), 31);
        assert_eq!(window.cell_count(), 63 * 63);
        assert_eq!(counts[2].tiles_keyed, 10 * 63);
        assert!(
            band_tile_keys
                .cells
                .keys()
                .copied()
                .eq(window.coords().collect::<BTreeSet<_>>())
        );
        assert_eq!(
            tiles_within(&band_tile_keys, moved_shown_rect).len(),
            window.cell_count() as usize
        );
    }

    #[test]
    fn a_space_whose_transform_is_not_finite_or_has_no_inverse_keys_nothing() {
        let (tree, display_list) = viewport_filled_with_green();
        let stack = stack_of(&display_list, &tree);
        for transform in [
            AffineTransform::new(f32::NAN, 0.0, 0.0, 1.0, 0.0, 0.0),
            AffineTransform::new(1.0, 0.0, 0.0, 1.0, 0.0, f32::INFINITY),
            AffineTransform::new(0.0, 0.0, 0.0, 0.0, 0.0, 0.0),
            AffineTransform::new(1.0e-30, 0.0, 0.0, 1.0e-30, 0.0, 0.0),
        ] {
            let shown_rect = SpaceToFrame::Transform(transform).rect_in_space(VIEWPORT);
            assert_eq!(shown_rect, IntRect::default());
            let (band_tile_keys, counts) = band_tile_keys_after_frames_showing(
                (&display_list, &tree, &stack),
                RasterSpaceIndex::VIEWPORT,
                BandIndex(0),
                &[shown_rect],
            );
            assert!(band_tile_keys.cells.is_empty());
            assert_eq!(counts[0], TileKeyingCounts::default());
        }
    }

    fn span_from_run_to_end_of_list(display_list: &DisplayListStorage, chunk_index: u32, run_index: usize) -> ListSpan {
        let chunk_index = ChunkIndexInList(chunk_index);
        ListSpan {
            begin: ListPosition::new(chunk_index, display_list.chunk(chunk_index).runs()[run_index].offset),
            end: display_list.whole_span().end,
        }
    }

    // A band of what a mask holds can hold the declaration of the mask's content, leave it out, or also hold other runs
    // of the chunk of the declaration, which draw away from the cells the masked content reaches.
    #[test]
    fn a_tile_of_masked_content_replays_the_declaration_of_its_mask_and_no_other_run_of_its_chunk() {
        use crate::visual_context::{MaskData, MaskLayerOrigin};
        let (mut tree, root) = tree_with_root_isolation_effect(1.0);
        let masked = tree.append_effect_node_under(
            root,
            EffectNodeData::Mask(MaskData {
                rect: IntRect::new(0, 0, 100, 100),
                kind: libgfx_rust::MaskKind::Alpha,
                origin: MaskLayerOrigin::CssMaskLayers,
                masks_backdrop: false,
            }),
        );
        let isolated = tree.append_effect_node_under(
            root,
            EffectNodeData::Effects(EffectsData {
                isolation: true,
                opacity: 1.0,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
        );
        // The first run of the first chunk is a box, and the second the declaration, which both draw in the cell to the
        // right of the cell of the masked box.
        let mut builder = DisplayListBuilder::new();
        builder.append(&fill(IntRect::new(700, 300, 50, 50), GREEN), &[], isolated);
        let declaration = builder.begin_group::<DeclareMaskContent>(Vec::new());
        builder.append(&fill(IntRect::new(600, 0, 50, 50), RED), &[], root);
        let content = builder.group_content_span(&declaration);
        builder.finish_group(
            declaration,
            &DeclareMaskContent {
                rect: IntRect::new(600, 0, 100, 100),
                effect: masked.effect,
                content,
            },
            root,
        );
        let display_list = DisplayListStorage::of_chunks(vec![
            Arc::new(ListChunk::copied(builder.bytes(), builder.command_runs(), 0)),
            chunk_of(fill(IntRect::new(10, 10, 80, 80), BLUE), masked),
        ])
        .unwrap();
        let declaration_and_masked_box = {
            let mut stack = LayerStack::one_layer_replaying_whole_list(
                VIEWPORT,
                display_list.whole_span(),
                None,
                LayeringObstacles::default(),
            );
            stack.bands[0].record_spans = vec![span_from_run_to_end_of_list(&display_list, 0, 1)];
            stack
        };
        let mut masked_box_alone = declaration_and_masked_box.clone();
        masked_box_alone.bands[0].record_spans = vec![span_from_run_to_end_of_list(&display_list, 1, 0)];
        let mut both_runs_and_masked_box = declaration_and_masked_box.clone();
        both_runs_and_masked_box.bands[0].record_spans = vec![display_list.whole_span()];

        let cell_of_masked_box = TileCoord::new(0, 0);
        for stack in [
            &declaration_and_masked_box,
            &masked_box_alone,
            &both_runs_and_masked_box,
        ] {
            let (band_tile_keys, _) = band_tile_keys_after_frames_showing(
                (&display_list, &tree, stack),
                RasterSpaceIndex::VIEWPORT,
                BandIndex(0),
                &[VIEWPORT],
            );
            let spans_of_masked_box =
                band_tile_keys.record_spans_of_tiles(band_tile_keys.tile(cell_of_masked_box, LayerIndex(0)));
            let declaration_position = ListPosition::new(
                ChunkIndexInList(0),
                display_list.chunk(ChunkIndexInList(0)).runs()[1].offset,
            );
            let first_box_position = ListPosition::new(ChunkIndexInList(0), 0);
            assert!(
                spans_of_masked_box
                    .iter()
                    .any(|span| span.contains(declaration_position))
            );
            assert!(!spans_of_masked_box.iter().any(|span| span.contains(first_box_position)));
        }
    }
}
