/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The compositor layers overlay: over each frame, the outline of each layer the frame is composited from and the tiles
//! rastered lately, highlighted by why they were and fading out. Each layer selected in the compositor layers window
//! shows the tiles it holds and how often each was rastered, and the layer the pointer is over in the window is marked
//! out. What it shows of a frame is worked out into a plain model first, which is both drawn and described to the
//! compositor layers window.

use crate::overlays::{
    CompositorLayersOverlayState, OverlayLabel, OverlayLabelShaper, OverlayRecorder, OverlayRecords,
};
use libcompositing_rust::display_list::layers::{LayerIdentity, SpaceToFrame};
use libcompositing_rust::display_list::tiles::TileCoord;
use libcompositing_rust::ipc_types::{
    Compositing_CompositorLayerDescription as CompositorLayerDescription,
    Compositing_CompositorLayerNodeDescription as CompositorLayerNodeDescription,
    Compositing_CompositorLayersDescription as CompositorLayersDescription,
    Compositing_CompositorTileHighlightDescription as CompositorTileHighlightDescription,
    Compositing_VisualContextNodeKind as VisualContextNodeKind,
};
use libcompositing_rust::time::{Duration, MonotonicTime};
use libgfx_rust::{Color, FloatPoint, IntPoint, IntRect, IntSize};
use std::collections::{BTreeMap, HashMap, HashSet, btree_map};
use std::hash::{DefaultHasher, Hash, Hasher};
use std::rc::Rc;

pub const RASTER_HIGHLIGHT_FADE_DURATION: Duration = Duration::from_milliseconds(500);
const MAXIMUM_CACHED_LABEL_COUNT: usize = 4096;
const LABEL_FONT_SIZE_IN_POINTS: f32 = 10.0;
const SHORTEST_TIME_BETWEEN_PUBLISHED_DESCRIPTIONS: Duration = Duration::from_milliseconds(100);
// The compositor layers window lists no more layers, and no longer names of them, than a person can read.
const MAXIMUM_LISTED_LAYER_COUNT: usize = 256;
const MAXIMUM_LISTED_LAYER_NAME_LENGTH: usize = 1024;
// A frame labels at most this many tiles of the selected layers, so that layers of very many tiles shape and place few
// labels, which the cache of labels holds.
const MAXIMUM_LABELED_TILE_COUNT: usize = 1024;
const BYTES_PER_PIXEL: u64 = 4;
const RASTER_HIGHLIGHT_ALPHA_BEFORE_FADING: u8 = 115;
const ONE_COLOR_TILE_TINT_ALPHA: u8 = 40;
const TILE_GRID_ALPHA: u8 = 150;
const OUTLINE_ALPHA_OF_UNMARKED_LAYER: u8 = 170;
const FILL_ALPHA_OF_HOVERED_LAYER: u8 = 50;
const LABEL_BACKGROUND_COLOR: Color = Color::from_rgba(16, 18, 24, 200);
const LABEL_TEXT_COLOR: Color = Color::from_rgba(255, 255, 255, 255);
const OUTLINE_HALO_COLOR: Color = Color::from_rgba(0, 0, 0, 150);

/// Why a tile was rastered, as the compositor tells the cases apart.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RasterCause {
    /// The layer held no tile at the cell.
    NewlyShown,
    /// The tile held pixels of other content.
    ContentHashChanged,
    /// The frame made every tile stale, or rastering the tile failed before.
    HeldPixelsInvalidated,
    /// The tile held less of its cell than the frame shows.
    HeldLessThanNeeded,
    /// The tile's surface was on another backend than the frame's.
    HeldOnAnotherBackend,
    /// No surface could be had for the tile, so the frame was painted straight from the list where the tile shows.
    PaintedStraightIntoTheFrame,
}

impl RasterCause {
    pub const ALL: [RasterCause; 6] = [
        RasterCause::NewlyShown,
        RasterCause::ContentHashChanged,
        RasterCause::HeldPixelsInvalidated,
        RasterCause::HeldLessThanNeeded,
        RasterCause::HeldOnAnotherBackend,
        RasterCause::PaintedStraightIntoTheFrame,
    ];

    pub fn name(self) -> &'static str {
        match self {
            RasterCause::NewlyShown => "newly shown",
            RasterCause::ContentHashChanged => "key changed",
            RasterCause::HeldPixelsInvalidated => "held pixels invalidated",
            RasterCause::HeldLessThanNeeded => "held less than needed",
            RasterCause::HeldOnAnotherBackend => "held on another backend",
            RasterCause::PaintedStraightIntoTheFrame => "painted straight into the frame",
        }
    }

    pub fn highlight_color(self) -> Color {
        match self {
            RasterCause::NewlyShown => Color::from_rgb(34, 197, 94),
            RasterCause::ContentHashChanged => Color::from_rgb(239, 68, 68),
            RasterCause::HeldPixelsInvalidated => Color::from_rgb(245, 158, 11),
            RasterCause::HeldLessThanNeeded => Color::from_rgb(59, 130, 246),
            RasterCause::HeldOnAnotherBackend => Color::from_rgb(168, 85, 247),
            RasterCause::PaintedStraightIntoTheFrame => Color::from_rgb(236, 72, 153),
        }
    }
}

/// How many tiles were rastered, by why.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RasterCountsByCause([u64; RasterCause::ALL.len()]);

impl RasterCountsByCause {
    fn count(&mut self, cause: RasterCause) {
        self.0[cause as usize] += 1;
    }

    pub fn total(&self) -> u64 {
        self.0.iter().sum()
    }

    /// The causes counted, the most counted first, as "12 key changed, 3 newly shown".
    fn text(&self) -> String {
        let mut counted_causes: Vec<(u64, RasterCause)> = RasterCause::ALL
            .iter()
            .map(|&cause| (self.0[cause as usize], cause))
            .filter(|&(count, _)| count != 0)
            .collect();
        counted_causes.sort_by(|(count, _), (other_count, _)| other_count.cmp(count));
        counted_causes
            .iter()
            .map(|(count, cause)| format!("{count} {}", cause.name()))
            .collect::<Vec<_>>()
            .join(", ")
    }
}

/// How a canvas, video or nested context layer reaches the screen.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MediaPresentation {
    ShownByWindowServer,
    Replayed { why: String },
}

impl std::fmt::Display for MediaPresentation {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            MediaPresentation::ShownByWindowServer => formatter.write_str("shown by the window server"),
            MediaPresentation::Replayed { why } => write!(formatter, "replayed: {why}"),
        }
    }
}

/// Who composites a painted layer of a frame.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PaintedLayerPresentation {
    ShownByWindowServer,
    HeldByFrame { why: String },
}

impl std::fmt::Display for PaintedLayerPresentation {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            PaintedLayerPresentation::ShownByWindowServer => formatter.write_str("shown by the window server"),
            PaintedLayerPresentation::HeldByFrame { why } => write!(formatter, "held by the frame: {why}"),
        }
    }
}

/// What a frame did to bring the tiles of its layers up to date.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FrameWorkCounts {
    pub tiles_rastered: u64,
    pub shown_tiles_of_one_color: u64,
    pub tiles_covered_by_opaque_tiles_above: u64,
    pub cells_binned: u64,
    pub chunks_placed: u64,
    /// Whether the frame took a plan of layers into use that the frame before was not drawn with.
    pub took_plan_into_use: bool,
}

/// The last time a tile was rastered, as the overlay shows it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LastRaster {
    pub cause: RasterCause,
    pub rect_in_layer_space: IntRect,
    pub age: Duration,
}

impl LastRaster {
    pub fn is_highlighted(&self) -> bool {
        self.age < RASTER_HIGHLIGHT_FADE_DURATION
    }

    pub fn highlight_opacity(&self) -> f32 {
        if !self.is_highlighted() {
            return 0.0;
        }
        1.0 - self.age.nanoseconds() as f32 / RASTER_HIGHLIGHT_FADE_DURATION.nanoseconds() as f32
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModelTile {
    pub coord: TileCoord,
    /// None where the layer holds no tile at the cell, as where a tile was painted straight into the frame.
    pub held_part_of_cell: Option<IntRect>,
    /// Whether the tile is of one color, which has no surface.
    pub is_one_color: bool,
    /// How often the tile was rastered while the layer kept it, since the counts were last zeroed.
    pub raster_count: u32,
    pub last_raster: Option<LastRaster>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ModelLayerContent {
    /// By cell, the tiles the layer holds, and those highlighted where it holds none.
    Painted {
        tiles: Vec<ModelTile>,
        presentation: PaintedLayerPresentation,
    },
    Media {
        reach: IntRect,
        presentation: MediaPresentation,
    },
}

/// A node of the visual context tree that the name of a layer mentions, by its index in the tree.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NamedVisualContextNode {
    Spatial(u32),
    Effect(u32),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModelLayer {
    pub identity: LayerIdentity,
    /// What the display list dump calls the layer.
    pub name: Rc<str>,
    /// The nodes of the visual context tree that the name mentions, in the order it does, each once.
    pub nodes_named: Rc<[NamedVisualContextNode]>,
    /// Whether the frames leave the layer out, as the compositor layers window hides it.
    pub hidden: bool,
    /// From the raster coordinates of the layer's space into those of the frame.
    pub space_to_frame: SpaceToFrame,
    /// In the raster coordinates of the frame.
    pub composite_clip: Option<IntRect>,
    pub content: ModelLayerContent,
    /// The tiles of the layer rastered since the counts were last zeroed.
    pub raster_counts_since_reset: RasterCountsByCause,
}

impl ModelLayer {
    fn color(&self) -> Color {
        color_of_layer(&self.identity)
    }

    fn visible_rect_in_frame(&self, viewport_rect: IntRect) -> IntRect {
        self.composite_clip
            .map_or(viewport_rect, |clip| viewport_rect.intersected(clip))
    }

    fn rect_in_frame(&self, rect_in_layer_space: IntRect) -> IntRect {
        self.space_to_frame.rect_in_frame(rect_in_layer_space)
    }

    fn bounds_in_layer_space(&self) -> IntRect {
        match &self.content {
            ModelLayerContent::Painted { tiles, .. } => tiles
                .iter()
                .filter_map(|tile| tile.held_part_of_cell)
                .fold(IntRect::default(), IntRect::united),
            ModelLayerContent::Media { reach, .. } => *reach,
        }
    }

    /// What of the layer's outline the frame shows.
    fn shown_bounds_in_frame(&self, viewport_rect: IntRect) -> IntRect {
        self.rect_in_frame(self.bounds_in_layer_space())
            .intersected(self.visible_rect_in_frame(viewport_rect))
    }
}

/// What the overlay shows over a frame.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LayerOverlayModel {
    pub viewport_rect: IntRect,
    /// From the base layer up.
    pub layers: Vec<ModelLayer>,
    pub why_list_is_one_layer: Option<String>,
    /// The layer stack's lines of composited effects, media without layers and effects reading only their own layer.
    pub notes_of_layer_stack: Rc<[String]>,
    pub work_counts_of_frame: FrameWorkCounts,
    /// The structural epoch of the visual context tree whose nodes the layers name.
    pub visual_context_tree_structural_epoch: u64,
    /// The tiles rastered since the counts were last zeroed, of layers in the stack or not.
    pub raster_counts_since_reset: RasterCountsByCause,
    /// How many frames rastered tiles since the counts were last zeroed.
    pub frames_rastering_since_reset: u64,
}

/// Per layer of a model, from the base layer up, given whether each is hidden, whether the layers window lists it: only
/// those show the layer's name.
pub fn layers_whose_names_are_shown(hidden_by_layer: &[bool]) -> Vec<bool> {
    hidden_by_layer
        .iter()
        .enumerate()
        .map(|(position, &hidden)| position < MAXIMUM_LISTED_LAYER_COUNT || hidden)
        .collect()
}

/// The layers the compositor layers window selects and the pointer is over in it, by the ids the descriptions give them.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct MarkedLayers {
    selected_layer_ids: Vec<u64>,
    hovered_layer_id: Option<u64>,
}

impl MarkedLayers {
    fn of_state(state_from_ui: &CompositorLayersOverlayState) -> Self {
        Self {
            selected_layer_ids: state_from_ui.selected_layer_ids.clone(),
            hovered_layer_id: state_from_ui.hovered_layer_id,
        }
    }

    fn selects(&self, layer: &ModelLayer) -> bool {
        self.selected_layer_ids.contains(&id_of_layer(&layer.identity))
    }

    /// The selected layers the frames show, from the base layer up.
    fn selected_layers<'a>(&self, model: &'a LayerOverlayModel) -> Vec<&'a ModelLayer> {
        model
            .layers
            .iter()
            .filter(|layer| !layer.hidden && self.selects(layer))
            .collect()
    }

    /// The layer the pointer is over, unless it is selected, when it shows more of itself already.
    fn hovered_layer<'a>(&self, model: &'a LayerOverlayModel) -> Option<&'a ModelLayer> {
        let hovered_layer_id = self.hovered_layer_id?;
        model
            .layers
            .iter()
            .find(|layer| !layer.hidden && !self.selects(layer) && id_of_layer(&layer.identity) == hovered_layer_id)
    }

    fn marks(&self, layer: &ModelLayer) -> bool {
        self.selects(layer) || self.hovered_layer_id == Some(id_of_layer(&layer.identity))
    }
}

/// What a frame of the overlay labels: the names of the selected layers and of the layer the pointer is over, where the
/// frame shows them, and the tiles of the selected layers that the frame shows, up to as many as a frame labels.
struct LabelsOfFrame<'a> {
    named_layers: Vec<&'a ModelLayer>,
    labeled_tiles_of_selected_layers: Vec<(&'a ModelLayer, Vec<&'a ModelTile>)>,
}

fn labels_of_model<'a>(model: &'a LayerOverlayModel, marked: &MarkedLayers) -> LabelsOfFrame<'a> {
    let selected_layers = marked.selected_layers(model);
    let named_layers = selected_layers
        .iter()
        .copied()
        .chain(marked.hovered_layer(model))
        .collect();
    let mut labeled_tile_count = 0;
    let labeled_tiles_of_selected_layers = selected_layers
        .into_iter()
        .rev()
        .map(|layer| {
            let mut labeled_tiles = Vec::new();
            if let ModelLayerContent::Painted { tiles, .. } = &layer.content {
                let visible_rect = layer.visible_rect_in_frame(model.viewport_rect);
                for tile in tiles {
                    if labeled_tile_count == MAXIMUM_LABELED_TILE_COUNT {
                        break;
                    }
                    let shown = tile.held_part_of_cell.is_some_and(|held_part_of_cell| {
                        !layer
                            .rect_in_frame(held_part_of_cell)
                            .intersected(visible_rect)
                            .is_empty()
                    });
                    if shown {
                        labeled_tile_count += 1;
                        labeled_tiles.push(tile);
                    }
                }
            }
            (layer, labeled_tiles)
        })
        .collect();
    LabelsOfFrame {
        named_layers,
        labeled_tiles_of_selected_layers,
    }
}

fn count_with_noun(count: u64, singular_noun: &str, plural_noun: &str) -> String {
    if count == 1 {
        format!("1 {singular_noun}")
    } else {
        format!("{count} {plural_noun}")
    }
}

fn memory_size_text(bytes: u64) -> String {
    const MEBIBYTE: u64 = 1024 * 1024;
    if bytes < MEBIBYTE {
        return format!("{} KiB", bytes.div_ceil(1024));
    }
    format!("{:.1} MiB", bytes as f64 / MEBIBYTE as f64)
}

impl LayerOverlayModel {
    fn details_of_layer(layer: &ModelLayer) -> String {
        match &layer.content {
            ModelLayerContent::Painted { tiles, presentation } => {
                let held_tile_count = tiles.iter().filter(|tile| tile.held_part_of_cell.is_some()).count() as u64;
                let one_color_tile_count = tiles.iter().filter(|tile| tile.is_one_color).count();
                let bytes_held_by_tile_surfaces: u64 = tiles
                    .iter()
                    .filter(|tile| !tile.is_one_color)
                    .filter_map(|tile| tile.held_part_of_cell)
                    .map(|held_part| held_part.width.max(0) as u64 * held_part.height.max(0) as u64 * BYTES_PER_PIXEL)
                    .sum();
                format!(
                    "{} ({one_color_tile_count} of one color), {}, {presentation}",
                    count_with_noun(held_tile_count, "tile", "tiles"),
                    memory_size_text(bytes_held_by_tile_surfaces)
                )
            }
            ModelLayerContent::Media { presentation, .. } => presentation.to_string(),
        }
    }

    fn summary_of_last_frame(&self) -> String {
        let counts = self.work_counts_of_frame;
        format!(
            "last frame: {} rastered, {} of one color, {} covered, {} binned, {} placed, {}",
            count_with_noun(counts.tiles_rastered, "tile", "tiles"),
            counts.shown_tiles_of_one_color,
            counts.tiles_covered_by_opaque_tiles_above,
            count_with_noun(counts.cells_binned, "tile", "tiles"),
            count_with_noun(counts.chunks_placed, "chunk", "chunks"),
            if counts.took_plan_into_use {
                "a new plan was taken into use"
            } else {
                "the stack was kept"
            }
        )
    }

    fn summary_since_reset(&self) -> String {
        let tiles_rastered = self.raster_counts_since_reset.total();
        if tiles_rastered == 0 {
            return "since reset: nothing rastered".into();
        }
        format!(
            "since reset: {} rastered by {} ({})",
            count_with_noun(tiles_rastered, "tile", "tiles"),
            count_with_noun(self.frames_rastering_since_reset, "frame", "frames"),
            self.raster_counts_since_reset.text()
        )
    }

    pub fn description_for_layers_window(&self) -> CompositorLayersDescription {
        let rgb_of_color =
            |color: Color| u32::from(color.red()) << 16 | u32::from(color.green()) << 8 | u32::from(color.blue());
        let mut notes: Vec<String> = Vec::new();
        if let Some(why) = &self.why_list_is_one_layer {
            notes.push(format!("single layer: {why}"));
        }
        // The window forgets a hidden layer it is not told of, so hidden layers past those it lists are listed too.
        let listed_layers: Vec<&ModelLayer> = self
            .layers
            .iter()
            .enumerate()
            .filter(|(position, layer)| *position < MAXIMUM_LISTED_LAYER_COUNT || layer.hidden)
            .map(|(_, layer)| layer)
            .collect();
        let unlisted_layer_count = self.layers.len() - listed_layers.len();
        if unlisted_layer_count != 0 {
            notes.push(format!(
                "not listed: {} above these",
                count_with_noun(unlisted_layer_count as u64, "layer", "layers")
            ));
        }
        notes.extend(self.notes_of_layer_stack.iter().cloned());
        CompositorLayersDescription {
            layers: listed_layers
                .into_iter()
                .map(|layer| CompositorLayerDescription {
                    id: id_of_layer(&layer.identity),
                    name: layer.name[..layer.name.floor_char_boundary(MAXIMUM_LISTED_LAYER_NAME_LENGTH)].into(),
                    details: Self::details_of_layer(layer).into(),
                    rgb_of_outline: rgb_of_color(layer.color()),
                    hidden: layer.hidden,
                    visible_rect_in_viewport: layer.shown_bounds_in_frame(self.viewport_rect),
                    nodes: layer
                        .nodes_named
                        .iter()
                        .map(|node| {
                            let (kind, index) = match *node {
                                NamedVisualContextNode::Spatial(index) => (VisualContextNodeKind::Spatial, index),
                                NamedVisualContextNode::Effect(index) => (VisualContextNodeKind::Effect, index),
                            };
                            CompositorLayerNodeDescription {
                                kind,
                                index,
                                element: Default::default(),
                            }
                        })
                        .collect(),
                    tiles_rastered_since_reset: layer.raster_counts_since_reset.total(),
                    raster_causes_since_reset: layer.raster_counts_since_reset.text().into(),
                })
                .collect(),
            notes: notes.into_iter().map(Into::into).collect(),
            summary_of_last_frame: self.summary_of_last_frame().into(),
            tile_highlights: RasterCause::ALL
                .iter()
                .map(|cause| CompositorTileHighlightDescription {
                    raster_cause: cause.name().to_owned().into(),
                    rgb: rgb_of_color(cause.highlight_color()),
                })
                .collect(),
            summary_since_reset: self.summary_since_reset().into(),
            visual_context_tree_structural_epoch: self.visual_context_tree_structural_epoch,
        }
    }
}

fn tile_label_text(tile: &ModelTile) -> String {
    format!("{},{} x{}", tile.coord.column, tile.coord.row, tile.raster_count)
}

fn layer_label_text(layer: &ModelLayer) -> String {
    match &layer.content {
        ModelLayerContent::Painted { .. } => layer.name.to_string(),
        ModelLayerContent::Media { presentation, .. } => format!("{}: {presentation}", layer.name),
    }
}

/// Names the layer in the descriptions the compositor publishes, for as long as its identity stays.
fn id_of_layer(identity: &LayerIdentity) -> u64 {
    let mut hasher = DefaultHasher::new();
    identity.hash(&mut hasher);
    hasher.finish()
}

/// A color of the layer's own, which stays as long as its identity does, wherever the layer is in the stack.
fn color_of_layer(identity: &LayerIdentity) -> Color {
    let hash = id_of_layer(identity);
    let hue = (hash % 360) as f32;
    let lighter_shade_tells_apart_similar_hues = hash & (1 << 40) != 0;
    let lightness = if lighter_shade_tells_apart_similar_hues {
        0.62
    } else {
        0.5
    };
    color_of_hsl(hue, 0.9, lightness)
}

fn color_of_hsl(hue_in_degrees: f32, saturation: f32, lightness: f32) -> Color {
    let chroma = (1.0 - (2.0 * lightness - 1.0).abs()) * saturation;
    let sector = hue_in_degrees / 60.0;
    let second_largest_component = chroma * (1.0 - (sector % 2.0 - 1.0).abs());
    let (red, green, blue) = match sector as u32 {
        0 => (chroma, second_largest_component, 0.0),
        1 => (second_largest_component, chroma, 0.0),
        2 => (0.0, chroma, second_largest_component),
        3 => (0.0, second_largest_component, chroma),
        4 => (second_largest_component, 0.0, chroma),
        _ => (chroma, 0.0, second_largest_component),
    };
    let lightness_added_to_each_component = lightness - chroma / 2.0;
    let channel = |component: f32| {
        ((component + lightness_added_to_each_component) * 255.0)
            .round()
            .clamp(0.0, 255.0) as u8
    };
    Color::from_rgb(channel(red), channel(green), channel(blue))
}

#[derive(Clone, Copy, Debug)]
struct RasterHistoryOfTile {
    raster_count: u32,
    last_cause: RasterCause,
    last_rect_in_layer_space: IntRect,
    last_rastered_at: MonotonicTime,
}

#[derive(Default)]
struct ShapedLabelCache {
    font_family: Option<String>,
    device_pixel_ratio: f64,
    label_by_text: HashMap<String, Option<OverlayLabel>>,
}

impl ShapedLabelCache {
    fn shape_labels_not_cached_yet(
        &mut self,
        texts: &[String],
        font_family: Option<&str>,
        device_pixel_ratio: f64,
        shaper: &dyn OverlayLabelShaper,
    ) {
        if self.font_family.as_deref() != font_family
            || self.device_pixel_ratio != device_pixel_ratio
            || self.label_by_text.len() > MAXIMUM_CACHED_LABEL_COUNT
        {
            self.label_by_text.clear();
            self.font_family = font_family.map(str::to_owned);
            self.device_pixel_ratio = device_pixel_ratio;
        }
        for text in texts {
            if self.label_by_text.contains_key(text) {
                continue;
            }
            let label = shaper.shape_overlay_label(font_family, LABEL_FONT_SIZE_IN_POINTS, device_pixel_ratio, text);
            self.label_by_text.insert(text.clone(), label);
        }
    }

    fn label_of_text(&self, text: &str) -> Option<&OverlayLabel> {
        self.label_by_text.get(text)?.as_ref()
    }
}

/// The overlay as of the frame painted last, and how it is drawn over that frame.
pub struct LayerOverlayFrame {
    pub model: LayerOverlayModel,
    pub records: OverlayRecords,
    /// Until when some tile stays highlighted.
    pub highlights_fade_until: Option<MonotonicTime>,
}

/// What the overlay keeps while it is switched on: how often each tile was rastered, and why last, how many tiles of
/// each layer were rastered since the window last zeroed the counts, and what the compositor layers window was told
/// last. A layer keeps the raster history of a tile while the layer holds the tile or the tile is highlighted. A frame
/// that does neither drops that history, and the count starts again from zero.
#[derive(Default)]
pub struct LayerOverlay {
    state_from_ui: CompositorLayersOverlayState,
    raster_history_of_tiles_by_layer: HashMap<LayerIdentity, BTreeMap<TileCoord, RasterHistoryOfTile>>,
    raster_counts_since_reset_by_layer: HashMap<LayerIdentity, RasterCountsByCause>,
    raster_counts_since_reset: RasterCountsByCause,
    frames_rastering_since_reset: u64,
    painted_at_of_latest_frame_counted: Option<MonotonicTime>,
    shaped_labels: ShapedLabelCache,
    latest_frame: Option<LayerOverlayFrame>,
    publish_time_and_description_published_last: Option<(MonotonicTime, CompositorLayersDescription)>,
    unpublished_description_waits_until: Option<MonotonicTime>,
}

impl LayerOverlay {
    /// Takes in that a frame painted at `painted_at` rastered the tile at `coord` of the layer, or painted it straight
    /// into the frame.
    pub fn record_raster(
        &mut self,
        layer_identity: LayerIdentity,
        coord: TileCoord,
        cause: RasterCause,
        rect_in_layer_space: IntRect,
        painted_at: MonotonicTime,
    ) {
        let history = self
            .raster_history_of_tiles_by_layer
            .entry(layer_identity)
            .or_default()
            .entry(coord)
            .or_insert(RasterHistoryOfTile {
                raster_count: 0,
                last_cause: cause,
                last_rect_in_layer_space: rect_in_layer_space,
                last_rastered_at: painted_at,
            });
        if cause != RasterCause::PaintedStraightIntoTheFrame {
            history.raster_count += 1;
        }
        history.last_cause = cause;
        history.last_rect_in_layer_space = rect_in_layer_space;
        history.last_rastered_at = painted_at;
        self.raster_counts_since_reset_by_layer
            .entry(layer_identity)
            .or_default()
            .count(cause);
        self.raster_counts_since_reset.count(cause);
    }

    fn take_state_from_ui(&mut self, state_from_ui: &CompositorLayersOverlayState) {
        if self.state_from_ui == *state_from_ui {
            return;
        }
        if self.state_from_ui.raster_count_reset_generation != state_from_ui.raster_count_reset_generation {
            self.reset_raster_counts();
        }
        self.state_from_ui = state_from_ui.clone();
    }

    fn reset_raster_counts(&mut self) {
        self.raster_counts_since_reset_by_layer.clear();
        self.raster_counts_since_reset = RasterCountsByCause::default();
        self.frames_rastering_since_reset = 0;
        for history in self
            .raster_history_of_tiles_by_layer
            .values_mut()
            .flat_map(|raster_history_of_tiles| raster_history_of_tiles.values_mut())
        {
            history.raster_count = 0;
        }
    }

    pub fn latest_frame(&self) -> Option<&LayerOverlayFrame> {
        self.latest_frame.as_ref()
    }

    /// Works out the overlay of a frame painted at `painted_at` from the layers of the frame and the tiles rastered so
    /// far, and records how it is drawn.
    pub fn update_with_frame(
        &mut self,
        mut model: LayerOverlayModel,
        painted_at: MonotonicTime,
        device_pixel_ratio: f64,
        shaper: &dyn OverlayLabelShaper,
    ) -> &LayerOverlayFrame {
        let highlights_fade_until = self.merge_raster_history_into(&mut model.layers, painted_at);
        self.merge_raster_counts_since_reset_into(&mut model, painted_at);
        let records = self.record_drawing_of_model(&model, device_pixel_ratio, shaper);
        self.latest_frame.insert(LayerOverlayFrame {
            model,
            records,
            highlights_fade_until,
        })
    }

    pub fn hides_layer(&self, identity: &LayerIdentity) -> bool {
        self.state_from_ui.hidden_layer_ids.contains(&id_of_layer(identity))
    }

    /// What the compositor layers window is to show of the frame painted last, unless it shows that already or was
    /// told what changed too short a time ago, when a later frame tells it.
    pub fn take_description_to_publish(&mut self, now: MonotonicTime) -> Option<CompositorLayersDescription> {
        let description = self.latest_frame.as_ref()?.model.description_for_layers_window();
        self.unpublished_description_waits_until = None;
        if let Some((published_at, published)) = &self.publish_time_and_description_published_last {
            if *published == description {
                return None;
            }
            let earliest_next_publish_time = *published_at + SHORTEST_TIME_BETWEEN_PUBLISHED_DESCRIPTIONS;
            if now < earliest_next_publish_time {
                self.unpublished_description_waits_until = Some(earliest_next_publish_time);
                return None;
            }
        }
        self.publish_time_and_description_published_last = Some((now, description.clone()));
        Some(description)
    }

    /// Until when frames are needed without anything else changing: while a highlight fades, and until the compositor
    /// layers window can be told what changed.
    pub fn needs_frames_until(&self) -> Option<MonotonicTime> {
        let highlights_fade_until = self.latest_frame.as_ref().and_then(|frame| frame.highlights_fade_until);
        highlights_fade_until.max(self.unpublished_description_waits_until)
    }

    /// Gives the tiles of the layers of a frame painted at `painted_at` their raster history, adds the tiles that are
    /// highlighted where a layer holds none, and keeps the raster history of only those tiles. Answers until when some
    /// tile stays highlighted.
    fn merge_raster_history_into(
        &mut self,
        layers: &mut [ModelLayer],
        painted_at: MonotonicTime,
    ) -> Option<MonotonicTime> {
        let mut highlights_fade_until: Option<MonotonicTime> = None;
        // The history that the layers of the frame do not take is of layers that left the stack, and is dropped.
        let mut raster_history_of_tiles_by_layer = std::mem::take(&mut self.raster_history_of_tiles_by_layer);
        let mut kept_raster_history_of_tiles_by_layer = HashMap::with_capacity(layers.len());
        for layer in layers {
            let ModelLayerContent::Painted { tiles, .. } = &mut layer.content else {
                continue;
            };
            let Some(mut raster_history_of_tiles) = raster_history_of_tiles_by_layer.remove(&layer.identity) else {
                continue;
            };
            let mut tile_by_coord: BTreeMap<TileCoord, ModelTile> = std::mem::take(tiles)
                .into_iter()
                .map(|tile| (tile.coord, tile))
                .collect();
            raster_history_of_tiles.retain(|&coord, history| {
                let last_raster = LastRaster {
                    cause: history.last_cause,
                    rect_in_layer_space: history.last_rect_in_layer_space,
                    age: painted_at - history.last_rastered_at,
                };
                if last_raster.is_highlighted() {
                    let fades_at = history.last_rastered_at + RASTER_HIGHLIGHT_FADE_DURATION;
                    highlights_fade_until = Some(highlights_fade_until.map_or(fades_at, |until| until.max(fades_at)));
                }
                let shows_cell_holding_no_tile = last_raster.is_highlighted();
                let tile = match tile_by_coord.entry(coord) {
                    btree_map::Entry::Occupied(entry) => entry.into_mut(),
                    btree_map::Entry::Vacant(entry) if shows_cell_holding_no_tile => entry.insert(ModelTile {
                        coord,
                        held_part_of_cell: None,
                        is_one_color: false,
                        raster_count: 0,
                        last_raster: None,
                    }),
                    btree_map::Entry::Vacant(_) => return false,
                };
                tile.raster_count = history.raster_count;
                tile.last_raster = Some(last_raster);
                true
            });
            *tiles = tile_by_coord.into_values().collect();
            if !raster_history_of_tiles.is_empty() {
                kept_raster_history_of_tiles_by_layer.insert(layer.identity, raster_history_of_tiles);
            }
        }
        self.raster_history_of_tiles_by_layer = kept_raster_history_of_tiles_by_layer;
        highlights_fade_until
    }

    /// Gives the model of a frame painted at `painted_at` the counts of tiles rastered since the window last zeroed
    /// them. The counts of a layer that left the stack stay only in those of all layers.
    fn merge_raster_counts_since_reset_into(&mut self, model: &mut LayerOverlayModel, painted_at: MonotonicTime) {
        // A frame is worked out again where presenting to the window server fails, and counts once.
        if model.work_counts_of_frame.tiles_rastered != 0 && self.painted_at_of_latest_frame_counted != Some(painted_at)
        {
            self.frames_rastering_since_reset += 1;
            self.painted_at_of_latest_frame_counted = Some(painted_at);
        }
        let identities_of_layers: HashSet<LayerIdentity> = model.layers.iter().map(|layer| layer.identity).collect();
        self.raster_counts_since_reset_by_layer
            .retain(|identity, _| identities_of_layers.contains(identity));
        for layer in &mut model.layers {
            layer.raster_counts_since_reset = self
                .raster_counts_since_reset_by_layer
                .get(&layer.identity)
                .copied()
                .unwrap_or_default();
        }
        model.raster_counts_since_reset = self.raster_counts_since_reset;
        model.frames_rastering_since_reset = self.frames_rastering_since_reset;
    }

    fn record_drawing_of_model(
        &mut self,
        model: &LayerOverlayModel,
        device_pixel_ratio: f64,
        shaper: &dyn OverlayLabelShaper,
    ) -> OverlayRecords {
        let marked = MarkedLayers::of_state(&self.state_from_ui);
        let labels = labels_of_model(model, &marked);
        let mut label_texts: Vec<String> = labels
            .named_layers
            .iter()
            .map(|layer| layer_label_text(layer))
            .collect();
        label_texts.extend(
            labels
                .labeled_tiles_of_selected_layers
                .iter()
                .flat_map(|(_, tiles)| tiles.iter().map(|tile| tile_label_text(tile))),
        );
        self.shaped_labels.shape_labels_not_cached_yet(
            &label_texts,
            self.state_from_ui.font_family.as_deref(),
            device_pixel_ratio,
            shaper,
        );
        let drawing = OverlayDrawing {
            shaped_labels: &self.shaped_labels,
            scale: device_pixel_ratio.max(1.0) as f32,
            marked,
        };
        OverlayRecords::record(|recorder| drawing.draw(recorder, model))
    }
}

struct OverlayDrawing<'a> {
    shaped_labels: &'a ShapedLabelCache,
    scale: f32,
    marked: MarkedLayers,
}

impl OverlayDrawing<'_> {
    fn device_pixels(&self, logical_pixels: f32) -> i32 {
        (logical_pixels * self.scale).round().max(1.0) as i32
    }

    /// Draws the tiles rastered lately and a thin outline of each layer the frames show, then the layer the pointer is
    /// over, filled, and the selected layers with the tiles they hold, all outlined boldly over the others and named.
    fn draw(&self, recorder: &mut OverlayRecorder<'_>, model: &LayerOverlayModel) {
        let viewport_rect = model.viewport_rect;
        let shown_layers = || model.layers.iter().filter(|layer| !layer.hidden);
        for layer in shown_layers() {
            self.draw_raster_highlights_of_layer(recorder, layer, viewport_rect);
        }
        for layer in shown_layers().filter(|layer| !self.marked.marks(layer)) {
            self.draw_thin_outline_of_layer(recorder, layer, viewport_rect);
        }
        if let Some(layer) = self.marked.hovered_layer(model) {
            Self::fill_within(
                recorder,
                layer.shown_bounds_in_frame(viewport_rect),
                layer.color().with_alpha(FILL_ALPHA_OF_HOVERED_LAYER),
                viewport_rect,
            );
            self.draw_bold_outline_of_layer(recorder, layer, viewport_rect);
        }
        for layer in self.marked.selected_layers(model) {
            self.draw_tiles_held_by_layer(recorder, layer, viewport_rect);
            self.draw_bold_outline_of_layer(recorder, layer, viewport_rect);
        }
        let labels = labels_of_model(model, &self.marked);
        let mut rects_of_drawn_labels = Vec::new();
        for layer in &labels.named_layers {
            self.draw_name_of_layer(recorder, layer, viewport_rect, &mut rects_of_drawn_labels);
        }
        for (layer, labeled_tiles) in &labels.labeled_tiles_of_selected_layers {
            self.draw_tile_labels_of_layer(
                recorder,
                layer,
                labeled_tiles,
                viewport_rect,
                &mut rects_of_drawn_labels,
            );
        }
    }

    fn fill_within(recorder: &mut OverlayRecorder<'_>, rect: IntRect, color: Color, within: IntRect) {
        let rect = rect.intersected(within);
        if !rect.is_empty() {
            recorder.fill_rect(rect, color);
        }
    }

    fn stroke_within(recorder: &mut OverlayRecorder<'_>, rect: IntRect, thickness: i32, color: Color, within: IntRect) {
        if rect.is_empty() {
            return;
        }
        let thickness = thickness.min(rect.width / 2).min(rect.height / 2).max(1);
        let inner_height = rect.height - 2 * thickness;
        for edge in [
            IntRect::new(rect.x, rect.y, rect.width, thickness),
            IntRect::new(rect.x, rect.bottom() - thickness, rect.width, thickness),
            IntRect::new(rect.x, rect.y + thickness, thickness, inner_height),
            IntRect::new(rect.right() - thickness, rect.y + thickness, thickness, inner_height),
        ] {
            Self::fill_within(recorder, edge, color, within);
        }
    }

    fn draw_raster_highlights_of_layer(
        &self,
        recorder: &mut OverlayRecorder<'_>,
        layer: &ModelLayer,
        viewport_rect: IntRect,
    ) {
        let ModelLayerContent::Painted { tiles, .. } = &layer.content else {
            return;
        };
        let visible_rect = layer.visible_rect_in_frame(viewport_rect);
        for tile in tiles {
            let Some(last_raster) = tile.last_raster.filter(LastRaster::is_highlighted) else {
                continue;
            };
            let alpha =
                (f32::from(RASTER_HIGHLIGHT_ALPHA_BEFORE_FADING) * last_raster.highlight_opacity()).round() as u8;
            Self::fill_within(
                recorder,
                layer.rect_in_frame(last_raster.rect_in_layer_space),
                last_raster.cause.highlight_color().with_alpha(alpha),
                visible_rect,
            );
        }
    }

    /// The grid of the tiles the layer holds, tinting those of one color.
    fn draw_tiles_held_by_layer(&self, recorder: &mut OverlayRecorder<'_>, layer: &ModelLayer, viewport_rect: IntRect) {
        let ModelLayerContent::Painted { tiles, .. } = &layer.content else {
            return;
        };
        let visible_rect = layer.visible_rect_in_frame(viewport_rect);
        let color = layer.color();
        for tile in tiles.iter().filter(|tile| tile.is_one_color) {
            if let Some(held_part_of_cell) = tile.held_part_of_cell {
                Self::fill_within(
                    recorder,
                    layer.rect_in_frame(held_part_of_cell),
                    color.with_alpha(ONE_COLOR_TILE_TINT_ALPHA),
                    visible_rect,
                );
            }
        }
        for held_part_of_cell in tiles.iter().filter_map(|tile| tile.held_part_of_cell) {
            Self::stroke_within(
                recorder,
                layer.rect_in_frame(held_part_of_cell),
                self.device_pixels(1.0),
                color.with_alpha(TILE_GRID_ALPHA),
                visible_rect,
            );
        }
    }

    fn draw_thin_outline_of_layer(
        &self,
        recorder: &mut OverlayRecorder<'_>,
        layer: &ModelLayer,
        viewport_rect: IntRect,
    ) {
        Self::stroke_within(
            recorder,
            layer.rect_in_frame(layer.bounds_in_layer_space()),
            self.device_pixels(1.0),
            layer.color().with_alpha(OUTLINE_ALPHA_OF_UNMARKED_LAYER),
            layer.visible_rect_in_frame(viewport_rect),
        );
    }

    fn draw_bold_outline_of_layer(
        &self,
        recorder: &mut OverlayRecorder<'_>,
        layer: &ModelLayer,
        viewport_rect: IntRect,
    ) {
        let visible_rect = layer.visible_rect_in_frame(viewport_rect);
        let color = layer.color();
        let bounds_in_frame = layer.rect_in_frame(layer.bounds_in_layer_space());
        let halo_thickness = self.device_pixels(1.0);
        Self::stroke_within(
            recorder,
            bounds_in_frame,
            self.device_pixels(2.0) + 2 * halo_thickness,
            OUTLINE_HALO_COLOR,
            visible_rect,
        );
        Self::stroke_within(
            recorder,
            bounds_in_frame.inflated(-2 * halo_thickness, -2 * halo_thickness),
            self.device_pixels(2.0),
            color,
            visible_rect,
        );
    }

    fn label_size(&self, label: &OverlayLabel, has_swatch: bool) -> IntSize {
        let text_height = (label.descent - label.ascent).ceil() as i32;
        let swatch_width = if has_swatch {
            text_height + self.device_pixels(3.0)
        } else {
            0
        };
        IntSize {
            width: label.width.ceil() as i32 + swatch_width + 2 * self.device_pixels(3.0),
            height: text_height + 2 * self.device_pixels(1.0),
        }
    }

    fn draw_label(
        &self,
        recorder: &mut OverlayRecorder<'_>,
        label: &OverlayLabel,
        top_left: IntPoint,
        swatch: Option<Color>,
        background: Option<Color>,
    ) {
        let size = self.label_size(label, swatch.is_some());
        let text_height = (label.descent - label.ascent).ceil() as i32;
        if let Some(background) = background {
            recorder.fill_rect(
                IntRect::new(top_left.x, top_left.y, size.width, size.height),
                background,
            );
        }
        let mut text_x = top_left.x + self.device_pixels(3.0);
        if let Some(swatch) = swatch {
            let swatch_rect = IntRect::new(text_x, top_left.y + self.device_pixels(1.0), text_height, text_height);
            recorder.fill_rect(swatch_rect, swatch);
            text_x += text_height + self.device_pixels(3.0);
        }
        let baseline = FloatPoint {
            x: text_x as f32,
            y: (top_left.y + self.device_pixels(1.0)) as f32 - label.ascent,
        };
        recorder.draw_label(label, baseline, LABEL_TEXT_COLOR, self.scale);
    }

    fn draw_name_of_layer(
        &self,
        recorder: &mut OverlayRecorder<'_>,
        layer: &ModelLayer,
        viewport_rect: IntRect,
        rects_of_drawn_labels: &mut Vec<IntRect>,
    ) {
        let visible_rect = layer.visible_rect_in_frame(viewport_rect);
        let visible_bounds = layer
            .rect_in_frame(layer.bounds_in_layer_space())
            .intersected(visible_rect);
        let Some(label) = self.shaped_labels.label_of_text(&layer_label_text(layer)) else {
            return;
        };
        if visible_bounds.is_empty() {
            return;
        }
        let size = self.label_size(label, true);
        let inset = self.device_pixels(3.0);
        let mut rect = IntRect::new(
            visible_bounds.x + inset,
            visible_bounds.y + inset,
            size.width,
            size.height,
        );
        while rects_of_drawn_labels
            .iter()
            .any(|other| !other.intersected(rect).is_empty())
        {
            rect.y += size.height + self.device_pixels(1.0);
        }
        if !visible_bounds.contains_point(IntPoint { x: rect.x, y: rect.y }) {
            return;
        }
        rects_of_drawn_labels.push(rect);
        recorder.clipped_to(visible_rect, |recorder| {
            self.draw_label(
                recorder,
                label,
                IntPoint { x: rect.x, y: rect.y },
                Some(layer.color()),
                Some(LABEL_BACKGROUND_COLOR),
            );
        });
    }

    fn draw_tile_labels_of_layer(
        &self,
        recorder: &mut OverlayRecorder<'_>,
        layer: &ModelLayer,
        labeled_tiles: &[&ModelTile],
        viewport_rect: IntRect,
        rects_of_drawn_labels: &mut Vec<IntRect>,
    ) {
        let visible_rect = layer.visible_rect_in_frame(viewport_rect);
        let inset = self.device_pixels(3.0);
        recorder.clipped_to(visible_rect, |recorder| {
            for tile in labeled_tiles {
                let Some(held_part_of_cell) = tile.held_part_of_cell else {
                    continue;
                };
                let held_rect_in_frame = layer.rect_in_frame(held_part_of_cell);
                let Some(label) = self.shaped_labels.label_of_text(&tile_label_text(tile)) else {
                    continue;
                };
                let size = self.label_size(label, false);
                let rect = IntRect::new(
                    held_rect_in_frame.right() - size.width - inset,
                    held_rect_in_frame.bottom() - size.height - inset,
                    size.width,
                    size.height,
                );
                if !visible_rect.contains_rect(rect)
                    || rects_of_drawn_labels
                        .iter()
                        .any(|other| !other.intersected(rect).is_empty())
                {
                    continue;
                }
                rects_of_drawn_labels.push(rect);
                self.draw_label(
                    recorder,
                    label,
                    IntPoint { x: rect.x, y: rect.y },
                    None,
                    Some(LABEL_BACKGROUND_COLOR),
                );
            }
        });
    }
}

/// The overlay while it is switched on, and whether the frame painted last showed it, which the frame after then
/// erases.
#[derive(Default)]
pub struct LayerOverlaySwitch {
    overlay: Option<LayerOverlay>,
    shown_by_latest_frame: bool,
}

impl LayerOverlaySwitch {
    /// Switches the overlay on or off from the frame on, dropping all it kept as it is switched off.
    pub fn switch_for_frame(&mut self, state_from_ui: Option<&CompositorLayersOverlayState>) {
        let Some(state_from_ui) = state_from_ui else {
            self.overlay = None;
            return;
        };
        self.overlay.get_or_insert_default().take_state_from_ui(state_from_ui);
    }

    /// Whether the frame repaints all of the viewport, to draw the overlay over it or to erase it.
    pub fn repaints_everything(&self) -> bool {
        self.overlay.is_some() || self.shown_by_latest_frame
    }

    pub fn did_paint_frame(&mut self) {
        self.shown_by_latest_frame = self.overlay.is_some();
    }

    pub fn overlay(&self) -> Option<&LayerOverlay> {
        self.overlay.as_ref()
    }

    pub fn overlay_mut(&mut self) -> Option<&mut LayerOverlay> {
        self.overlay.as_mut()
    }

    pub fn latest_frame(&self) -> Option<&LayerOverlayFrame> {
        self.overlay.as_ref()?.latest_frame()
    }

    pub fn needs_frames_until(&self) -> Option<MonotonicTime> {
        self.overlay.as_ref()?.needs_frames_until()
    }

    pub fn hides_layer(&self, identity: &LayerIdentity) -> bool {
        self.overlay
            .as_ref()
            .is_some_and(|overlay| overlay.hides_layer(identity))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libcompositing_rust::display_list::layers::PlacementNodeIdentity;

    fn painted_layer_identity(node: u64) -> LayerIdentity {
        LayerIdentity::Painted {
            node: PlacementNodeIdentity(node),
            band_ordinal_in_node: 0,
            above_barrier: None,
        }
    }

    fn painted_layer_holding(identity: LayerIdentity, held_coords: &[TileCoord]) -> ModelLayer {
        ModelLayer {
            identity,
            name: "painted layer".into(),
            nodes_named: Rc::default(),
            hidden: false,
            space_to_frame: SpaceToFrame::Offset(IntPoint::default()),
            composite_clip: None,
            raster_counts_since_reset: RasterCountsByCause::default(),
            content: ModelLayerContent::Painted {
                presentation: PaintedLayerPresentation::ShownByWindowServer,
                tiles: held_coords
                    .iter()
                    .map(|&coord| ModelTile {
                        coord,
                        held_part_of_cell: Some(coord.cell_rect()),
                        is_one_color: false,
                        raster_count: 0,
                        last_raster: None,
                    })
                    .collect(),
            },
        }
    }

    // A plan from another process can have many more layers than the window lists.
    #[test]
    fn only_the_layers_the_window_lists_show_their_names() {
        let layer_count = MAXIMUM_LISTED_LAYER_COUNT + 100;
        let topmost_hidden = layer_count - 10;
        let hidden_by_layer: Vec<bool> = (0..layer_count).map(|position| position >= topmost_hidden).collect();
        let shown = layers_whose_names_are_shown(&hidden_by_layer);
        assert!(
            shown
                .iter()
                .enumerate()
                .all(|(position, &shown)| shown == (position < MAXIMUM_LISTED_LAYER_COUNT || hidden_by_layer[position]))
        );
    }

    fn model_of(layers: Vec<ModelLayer>) -> LayerOverlayModel {
        LayerOverlayModel {
            viewport_rect: IntRect::new(0, 0, 800, 600),
            layers,
            why_list_is_one_layer: None,
            notes_of_layer_stack: Rc::default(),
            work_counts_of_frame: FrameWorkCounts::default(),
            visual_context_tree_structural_epoch: 0,
            raster_counts_since_reset: RasterCountsByCause::default(),
            frames_rastering_since_reset: 0,
        }
    }

    fn coords_with_raster_history(overlay: &LayerOverlay, identity: &LayerIdentity) -> Vec<TileCoord> {
        overlay
            .raster_history_of_tiles_by_layer
            .get(identity)
            .map_or_else(Vec::new, |history| history.keys().copied().collect())
    }

    // Per tile of the layer of a frame painted at `painted_at`, once it has its raster history: where it is, whether the
    // layer holds it, and how often it was rastered. Answers too until when some tile stays highlighted.
    fn tiles_with_raster_history(
        overlay: &mut LayerOverlay,
        painted_at: MonotonicTime,
        layer: ModelLayer,
    ) -> (Vec<(TileCoord, bool, u32)>, Option<MonotonicTime>) {
        let mut layers = [layer];
        let highlights_fade_until = overlay.merge_raster_history_into(&mut layers, painted_at);
        let ModelLayerContent::Painted { tiles, .. } = &layers[0].content else {
            panic!("the layer is painted");
        };
        let tiles = tiles
            .iter()
            .map(|tile| (tile.coord, tile.held_part_of_cell.is_some(), tile.raster_count))
            .collect();
        (tiles, highlights_fade_until)
    }

    #[test]
    fn raster_history_is_kept_only_for_tiles_held_or_highlighted() {
        let layer = painted_layer_identity(1);
        let held = TileCoord::new(0, 0);
        let let_go = TileCoord::new(1, 0);
        let rastered_at = MonotonicTime::from_nanoseconds(1_000_000_000);
        let mut overlay = LayerOverlay::default();
        for coord in [held, let_go] {
            overlay.record_raster(layer, coord, RasterCause::NewlyShown, coord.cell_rect(), rastered_at);
        }

        // A tile the layer no longer holds shows while its highlight fades.
        let while_fading = tiles_with_raster_history(
            &mut overlay,
            rastered_at + Duration::from_milliseconds(100),
            painted_layer_holding(layer, &[held]),
        );
        assert_eq!(
            while_fading,
            (
                vec![(held, true, 1), (let_go, false, 1)],
                Some(rastered_at + RASTER_HIGHLIGHT_FADE_DURATION)
            )
        );
        assert_eq!(coords_with_raster_history(&overlay, &layer), [held, let_go]);

        // Once the highlight faded, only the tile the layer holds keeps its history.
        let after_fading = tiles_with_raster_history(
            &mut overlay,
            rastered_at + RASTER_HIGHLIGHT_FADE_DURATION,
            painted_layer_holding(layer, &[held]),
        );
        assert_eq!(after_fading, (vec![(held, true, 1)], None));
        assert_eq!(coords_with_raster_history(&overlay, &layer), [held]);

        // A tile the layer holds again after it let the tile go counts its rasters from zero.
        let held_again = tiles_with_raster_history(
            &mut overlay,
            rastered_at + RASTER_HIGHLIGHT_FADE_DURATION,
            painted_layer_holding(layer, &[held, let_go]),
        );
        assert_eq!(held_again.0, [(held, true, 1), (let_go, true, 0)]);

        // A layer that left the stack keeps no history.
        tiles_with_raster_history(
            &mut overlay,
            rastered_at + RASTER_HIGHLIGHT_FADE_DURATION,
            painted_layer_holding(painted_layer_identity(2), &[held]),
        );
        assert!(overlay.raster_history_of_tiles_by_layer.is_empty());
    }

    struct LabelShaperCountingTexts(std::cell::RefCell<Vec<String>>);

    impl OverlayLabelShaper for LabelShaperCountingTexts {
        fn shape_overlay_label(&self, _: Option<&str>, _: f32, _: f64, text: &str) -> Option<OverlayLabel> {
            self.0.borrow_mut().push(text.to_owned());
            None
        }
    }

    // A stack of many layers that each hold a tile the frame shows.
    #[test]
    fn a_frame_labels_only_the_selected_layers_their_tiles_and_the_hovered_layer() {
        let layers = (0..100)
            .map(|index| ModelLayer {
                name: format!("layer {index}").into(),
                ..painted_layer_holding(painted_layer_identity(index), &[TileCoord::new(0, 0)])
            })
            .collect();
        let shaper = LabelShaperCountingTexts(std::cell::RefCell::new(Vec::new()));
        let mut overlay = LayerOverlay::default();
        overlay.take_state_from_ui(&CompositorLayersOverlayState {
            selected_layer_ids: vec![
                id_of_layer(&painted_layer_identity(40)),
                id_of_layer(&painted_layer_identity(60)),
            ],
            hovered_layer_id: Some(id_of_layer(&painted_layer_identity(7))),
            ..CompositorLayersOverlayState::default()
        });
        overlay.update_with_frame(model_of(layers), MonotonicTime::default(), 1.0, &shaper);
        assert_eq!(*shaper.0.borrow(), ["layer 40", "layer 60", "layer 7", "0,0 x0"]);
    }

    #[test]
    fn the_layers_window_is_told_of_no_more_layers_than_a_person_can_read() {
        let long_name = format!("a{}", "é".repeat(MAXIMUM_LISTED_LAYER_NAME_LENGTH));
        let model_with_layer_count = |layer_count: usize| LayerOverlayModel {
            viewport_rect: IntRect::new(0, 0, 800, 600),
            layers: (0..layer_count)
                .map(|index| ModelLayer {
                    name: long_name.as_str().into(),
                    content: ModelLayerContent::Media {
                        reach: IntRect::new(0, 0, 10, 10),
                        presentation: MediaPresentation::ShownByWindowServer,
                    },
                    ..painted_layer_holding(painted_layer_identity(index as u64), &[])
                })
                .collect(),
            notes_of_layer_stack: Rc::from(["a note of the plan".to_owned()]),
            ..model_of(Vec::new())
        };
        let notes_of = |description: &CompositorLayersDescription| -> Vec<String> {
            description
                .notes
                .iter()
                .map(|note| note.as_str().unwrap().to_owned())
                .collect()
        };

        let all_listed = model_with_layer_count(MAXIMUM_LISTED_LAYER_COUNT).description_for_layers_window();
        assert_eq!(all_listed.layers.len(), MAXIMUM_LISTED_LAYER_COUNT);
        assert_eq!(notes_of(&all_listed), ["a note of the plan"]);

        let some_unlisted = model_with_layer_count(MAXIMUM_LISTED_LAYER_COUNT + 3).description_for_layers_window();
        assert_eq!(some_unlisted.layers.len(), MAXIMUM_LISTED_LAYER_COUNT);
        assert_eq!(
            some_unlisted.layers.last().map(|layer| layer.id),
            Some(id_of_layer(&painted_layer_identity(
                MAXIMUM_LISTED_LAYER_COUNT as u64 - 1
            )))
        );
        assert_eq!(
            notes_of(&some_unlisted),
            ["not listed: 3 layers above these", "a note of the plan"]
        );
        assert!(some_unlisted.layers.iter().all(|layer| {
            let name = layer.name.as_str().unwrap();
            name.len() == MAXIMUM_LISTED_LAYER_NAME_LENGTH - 1 && long_name.starts_with(name)
        }));

        // A layer hidden in the window stays listed where layers below it push it past those listed.
        let mut with_hidden_layer_past_those_listed = model_with_layer_count(MAXIMUM_LISTED_LAYER_COUNT + 3);
        with_hidden_layer_past_those_listed.layers[MAXIMUM_LISTED_LAYER_COUNT + 1].hidden = true;
        let description = with_hidden_layer_past_those_listed.description_for_layers_window();
        assert_eq!(description.layers.len(), MAXIMUM_LISTED_LAYER_COUNT + 1);
        assert_eq!(
            description.layers.last().map(|layer| (layer.id, layer.hidden)),
            Some((
                id_of_layer(&painted_layer_identity(MAXIMUM_LISTED_LAYER_COUNT as u64 + 1)),
                true
            ))
        );
        assert_eq!(notes_of(&description)[0], "not listed: 2 layers above these");
    }
}
