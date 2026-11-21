/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The layers a frame is painted in, each cached as tiles from one frame and one list to the next, and what of them the
//! window server shows above the frame by itself.

use super::layer_overlay::{
    FrameWorkCounts, LayerOverlayModel, MediaPresentation, ModelLayer, ModelLayerContent, NamedVisualContextNode,
    PaintedLayerPresentation, RasterCountsByCause, layers_whose_names_are_shown,
};
use super::tiles::{LayerTiles, LayerToRaster, TileDraw, TileShownInFrame, joined_rects_of_grid_given_row_by_row};
use super::window_server::{WhyMediaIsReplayed, WindowServerLayers};
use super::{
    ContextPresenting, FrameToPaint, NestedContexts, PresentingEnvironment, apply_raster_transform,
    composite_inside_effects_of_frame, inflated, rect_of_size, replay_into, replay_with_scroll_offsets, size_of,
};
use crate::context_scene::ReceivedLayerPlan;
use crate::raster_host::{FfiSublayer, LiveContent};
use crate::surfaces::painting_surface::PaintingSurface;
use libcompositing_rust::display_list::commands::{
    CanvasId, CompositorContextId, EffectNodeIndex, ImageFrameResourceId, SpatialNodeIndex, VideoSinkHandle,
};
use libcompositing_rust::display_list::effect_geometry::{
    backdrop_filter_read_reach, composited_filter_read_reach_in_device_pixels, spreading_filter_of,
};
use libcompositing_rust::display_list::layers::{
    BandIndex, LayerContent, LayerIdentity, LayerIndex, LayerStack, LayeringObstacles, NodeNamesFromIndices,
    PlacementNodeIdentity, RasterSpaceIndex, SpaceToFrame, StackLayer, VisualContextNodeNamer,
    WhatWindowServerShowsOfFrame, WhyFrameHoldsLayer, WhyListIsOneLayer, WindowServerCompositing,
    WindowServerPlacementsOfEffects, WindowServerPlacementsOfMedia,
};
use libcompositing_rust::display_list::placement::{
    RasterSpaceGeometry, RasterSpaces, WhatEverySpaceOfTreeShares, raster_scroll_offsets,
};
use libcompositing_rust::display_list::reach::Reach;
use libcompositing_rust::display_list::replay::{CompositeClipsOfRaster, ReplaySelection};
use libcompositing_rust::display_list::tile_keys::{
    BandTileKeys, ContentHash, FrameStateReadByReplay, LayerRasterSettings, LiveContentVersions, TileCoverage,
    TileKeyingOfList, TileKeyingOfRasterSpace,
};
use libcompositing_rust::display_list::tiles::TILE_SIZE_IN_DEVICE_PIXELS;
use libcompositing_rust::scrolling::DisplayList;
use libcompositing_rust::visual_context::{VisualContextTree, WorldTransforms};
use libgfx_rust::{Color, CornerRadii, FloatMatrix4x4, FloatPoint, IntPoint, IntRect};
use std::borrow::Cow;
use std::cell::{OnceCell, RefCell};
use std::collections::{HashMap, HashSet};
use std::rc::{Rc, Weak};

// A frame drawn with a plan may show tiles, of one color or not, of as many pixels as this many layers that each show
// every cell a rect of the viewport's size can touch, which a viewport smaller than a cell needs whole. The layers of a
// page show two or three viewports of tiles. A frame whose plan shows more is drawn as one layer, which shows no more
// than one such layer.
const MAXIMUM_LAYERS_OF_TILES_A_PLAN_SHOWS: u64 = 16;
// Each shown tile costs work whatever part of its cell it shows, so a frame drawn with a plan may show at most this many
// tiles per cell that one such layer can touch. The pages of the tests show at most about twenty.
const MAXIMUM_TILES_A_PLAN_SHOWS_PER_CELL_ONE_LAYER_SHOWS: u64 = 64;

// Growing damage over what composited effects read takes a pass over them for each one it newly reaches. Past this many
// passes, the damage grows over all they read, which holds what more passes would add.
const MAXIMUM_PASSES_GROWING_DAMAGE_OVER_WHAT_COMPOSITED_EFFECTS_READ: usize = 8;
// A tile below is tested against the opaque rects of the layers above, and against at most this many parts of it left
// uncovered. Past either bound, tiles are not hidden but drawn, which is right, only slower.
const MAXIMUM_OPAQUE_RECTS_HIDING_TILES_BELOW: usize = 128;
const MAXIMUM_UNCOVERED_PARTS_OF_TILE: usize = 64;
const MAXIMUM_COMPOSITED_EFFECT_READS_TESTED_ONE_BY_ONE: usize = 64;

#[derive(Clone, Copy, PartialEq, Eq)]
struct MediaRecordsReplayedInFrame {
    content_hash: ContentHash,
    reach_in_frame: IntRect,
}

struct LiveContentVersionsOfFrame<'a> {
    live_content: &'a LiveContent,
    nested_contexts: &'a dyn NestedContexts,
}

impl LiveContentVersions for LiveContentVersionsOfFrame<'_> {
    fn canvas_content_generation(&self, canvas: CanvasId) -> u64 {
        self.live_content
            .canvases
            .get(&canvas.0)
            .map_or(0, |canvas| canvas.content_generation)
    }

    fn video_frame_id(&self, video_sink: VideoSinkHandle) -> Option<u64> {
        self.live_content
            .video_sinks
            .get(&video_sink.0)
            .and_then(|sink| sink.has_frame.then_some(sink.frame_id))
    }

    fn image_frame_pixel_generation(&self, image_frame: ImageFrameResourceId) -> Option<u64> {
        self.live_content.image_frames.get(&image_frame.0).copied()
    }

    fn nested_context_frame_generation(&self, nested_context: CompositorContextId) -> Option<u64> {
        self.nested_contexts.frame_generation_of(nested_context)
    }
}

/// Where a frame composites a layer: offset by the scroll offsets of its space's anchors, within its composite clip
/// and the rounded composite clips inside it, and whether the frame holds it or the window server shows it by
/// itself.
#[derive(Clone, Debug)]
pub struct LayerCompositingInFrame {
    /// Where the layer is in the stack the frame draws with.
    pub(super) layer_index: LayerIndex,
    pub(super) identity: LayerIdentity,
    pub(super) space_to_frame: SpaceToFrame,
    pub(super) composite_clip: Option<IntRect>,
    pub(super) rounded_composite_clips: Vec<(IntRect, CornerRadii)>,
    pub(super) shown_by_window_server: bool,
}

// Two frames composite a layer alike wherever their stacks hold it.
impl PartialEq for LayerCompositingInFrame {
    fn eq(&self, other: &Self) -> bool {
        let Self {
            layer_index: _,
            identity,
            space_to_frame,
            composite_clip,
            rounded_composite_clips,
            shown_by_window_server,
        } = self;
        *identity == other.identity
            && *space_to_frame == other.space_to_frame
            && *composite_clip == other.composite_clip
            && *rounded_composite_clips == other.rounded_composite_clips
            && *shown_by_window_server == other.shown_by_window_server
    }
}

impl LayerCompositingInFrame {
    pub(super) fn rect_in_frame(&self, rect_in_layer_space: IntRect, within: IntRect) -> IntRect {
        rect_in_frame_of_space(rect_in_layer_space, self.space_to_frame, self.composite_clip, within)
    }

    pub(super) fn rect_shown_in_layer_space(
        &self,
        viewport_rect: IntRect,
        composited_filter_read_reach: i32,
    ) -> IntRect {
        let shown_in_viewport = match self.composite_clip {
            Some(composite_clip) => viewport_rect.intersected(composite_clip),
            None => viewport_rect,
        };
        inflated(
            self.space_to_frame.rect_in_space(shown_in_viewport),
            composited_filter_read_reach,
        )
    }

    /// The layer composited, in the stack the frame worked out its compositing with.
    pub(super) fn layer_in<'a>(&self, stack: &'a LayerStack) -> &'a StackLayer {
        let layer = &stack.layers[self.layer_index.0];
        debug_assert!(layer.identity == self.identity);
        layer
    }
}

/// The tile keys of a band of the stack of frames, and the geometry they were bound with besides the list of the stack.
struct BandTileKeysOfFrame {
    geometry: Rc<RasterSpaceGeometry>,
    tile_keys: BandTileKeys,
}

/// What a frame paints of its layers: where it composites them, the tiles it shows of each, and the media and composited
/// effects it draws.
pub struct LayerUpdate {
    pub(super) layers: PaintedLayers,
    /// What each composited effect that reads past each of its pixels reads and draws as the frame places it.
    read_reaches_of_composited_effects_in_frame: Vec<IntRect>,
    // Per layer of the stack.
    shown_tiles_by_layer: Vec<Vec<TileShownInFrame>>,
    composited_filter_read_reach_by_layer: Vec<i32>,
    pub(super) frame_allows_lcd_text: bool,
    work_counts: FrameWorkCounts,
}

impl LayerUpdate {
    fn shown_tiles_of_layer(&self, layer_index: LayerIndex) -> &[TileShownInFrame] {
        &self.shown_tiles_by_layer[layer_index.0]
    }

    pub fn shown_by_window_server(&self) -> bool {
        self.layers.shown_by_window_server()
    }

    pub fn window_server_shows_every_layer(&self) -> bool {
        self.layers.window_server_shows_every_layer()
    }

    /// Whether the window server shows layers of the frame tile by tile, which the frame rasters the tiles of even
    /// where it paints nothing into a store.
    pub fn window_server_shows_tiles(&self) -> bool {
        self.layers.shown_by_window_server() && self.layers.window_server_compositing.shows_tiles_of_painted_layers
    }

    /// Has the frame hold every layer, as where the window server could not be given what it was to show.
    pub(super) fn hold_every_layer_in_frame(&mut self) {
        for compositing in &mut self.layers.compositing_of_layers {
            compositing.shown_by_window_server = false;
        }
    }

    /// The damage grown over all that a composited effect reads wherever it reaches that, as compositing the effect
    /// reads what is composited below it, which the frame painted before holds only under what it composited above.
    pub fn damage_grown_over_what_composited_effects_read(&self, mut damage: IntRect) -> IntRect {
        for _ in 0..MAXIMUM_PASSES_GROWING_DAMAGE_OVER_WHAT_COMPOSITED_EFFECTS_READ {
            let mut grew = false;
            for &read_reach in &self.read_reaches_of_composited_effects_in_frame {
                if !read_reach.intersected(damage).is_empty() && !damage.contains_rect(read_reach) {
                    damage = damage.united(read_reach);
                    grew = true;
                }
            }
            if !grew {
                return damage;
            }
        }
        self.read_reaches_of_composited_effects_in_frame
            .iter()
            .fold(damage, |damage, &read_reach| damage.united(read_reach))
    }
}

struct CachedRasterSpaceGeometry {
    tree_copy_number: u64,
    tree_copy_without_animation_samples: u64,
    geometry: Rc<RasterSpaceGeometry>,
}

#[derive(Clone)]
struct NameOfLayerForOverlay {
    name: Rc<str>,
    nodes_named: Rc<[NamedVisualContextNode]>,
}

/// Names the nodes of the tree by their indices, as the overlay of layers does, noting each node it names once.
#[derive(Default)]
struct NodeNamesFromIndicesNotingNodes {
    nodes_named: RefCell<Vec<NamedVisualContextNode>>,
}

impl NodeNamesFromIndicesNotingNodes {
    fn note(&self, node: NamedVisualContextNode) {
        let mut nodes_named = self.nodes_named.borrow_mut();
        if !nodes_named.contains(&node) {
            nodes_named.push(node);
        }
    }
}

impl VisualContextNodeNamer for NodeNamesFromIndicesNotingNodes {
    fn name_of_spatial_node(&self, index: SpatialNodeIndex) -> String {
        self.note(NamedVisualContextNode::Spatial(index.0));
        NodeNamesFromIndices.name_of_spatial_node(index)
    }

    fn name_of_effect_node(&self, index: EffectNodeIndex) -> String {
        self.note(NamedVisualContextNode::Effect(index.0));
        NodeNamesFromIndices.name_of_effect_node(index)
    }
}

/// The stack that frames draw with, and what they keep of it from one frame to the next while they draw with the same
/// plan, whose trees hold the same nodes and differ only in their values.
struct StackOfFrames {
    /// The plan WebContent sent, or none for one layer that replays the whole list, for a frame that is not drawn as a
    /// plan was made for. One plan stands for one list, one tree without the samples of the compositor's animations,
    /// one viewport, and the scroll offsets the plan reads.
    layer_plan: Option<ReceivedLayerPlan>,
    stack: Rc<LayerStack>,
    // Keeps the address of the list the frames draw from being taken by another list.
    display_list: Weak<DisplayList>,
    viewport_to_raster_transform: FloatMatrix4x4,
    raster_spaces: Option<RasterSpaces>,
    geometry_by_space: Vec<Option<CachedRasterSpaceGeometry>>,
    /// With the copy of the tree without the samples of the compositor's animations it was worked out of, as it reads
    /// no sample.
    what_every_space_of_tree_shares: Option<(u64, Rc<WhatEverySpaceOfTreeShares>)>,
    /// By band index, the tile keys of the bands as the latest frame drawn with the stack keyed its tiles, painted or not,
    /// or none for a band no frame has bound yet.
    band_tile_keys: Vec<Option<BandTileKeysOfFrame>>,
    /// What the layers overlay calls the layers whose names it shows, and the nodes of the tree each name mentions,
    /// worked out by the first frame that shows each.
    names_of_layers_for_overlay: RefCell<HashMap<LayerIndex, NameOfLayerForOverlay>>,
}

impl StackOfFrames {
    /// Keeps the geometries of the spaces of the stack the latest frame drew with where the spaces are at the same
    /// anchors under the same raster transform, as the geometry of a space does not depend on the viewport. Every band
    /// starts with no tile keys.
    fn new(
        layer_plan: Option<ReceivedLayerPlan>,
        stack: Rc<LayerStack>,
        display_list: &Rc<DisplayList>,
        viewport_to_raster_transform: FloatMatrix4x4,
        latest: Option<StackOfFrames>,
    ) -> Self {
        let geometry_by_space = match latest {
            Some(latest)
                if latest.viewport_to_raster_transform == viewport_to_raster_transform
                    && latest
                        .stack
                        .spaces
                        .iter()
                        .map(|space| space.anchor)
                        .eq(stack.spaces.iter().map(|space| space.anchor)) =>
            {
                latest.geometry_by_space
            }
            _ => (0..stack.spaces.len()).map(|_| None).collect(),
        };
        Self {
            layer_plan,
            band_tile_keys: (0..stack.bands.len()).map(|_| None).collect(),
            stack,
            display_list: Rc::downgrade(display_list),
            viewport_to_raster_transform,
            raster_spaces: None,
            geometry_by_space,
            what_every_space_of_tree_shares: None,
            names_of_layers_for_overlay: RefCell::default(),
        }
    }

    /// Per raster space, its geometry under the tree and the scroll offsets of the frame.
    fn geometries_of_spaces(
        &mut self,
        tree: &VisualContextTree,
        scroll_offsets: &[FloatPoint],
    ) -> Vec<Rc<RasterSpaceGeometry>> {
        let stack = &self.stack;
        let mut geometries = Vec::with_capacity(stack.spaces.len());
        for (space_index, cached_geometry) in self.geometry_by_space.iter_mut().enumerate() {
            let raster_space_index = RasterSpaceIndex(space_index);
            // What reads the geometry of a space of a plan that holds nothing that moves within it reads no value the
            // compositor animates, and of the scroll offsets only those the plan reads. The plan leaves out the offsets
            // of the anchors of the spaces, which are the only ones a geometry's raster scroll offsets change.
            let nodes_whose_scroll_offset_geometry_readers_read = self
                .layer_plan
                .as_ref()
                .filter(|_| !stack.space_holds_content_that_moves_within_it(raster_space_index))
                .map(|layer_plan| &*layer_plan.nodes_whose_scroll_offset_plan_reads);
            let offset_of_node = |scroll_offsets: &[FloatPoint], node_index: u32| {
                scroll_offsets.get(node_index as usize).copied().unwrap_or_default()
            };
            if let Some(cached) = cached_geometry
                && (cached.tree_copy_number == tree.copy.number()
                    || (nodes_whose_scroll_offset_geometry_readers_read.is_some()
                        && cached.tree_copy_without_animation_samples == tree.copy_without_animation_samples()))
                && match nodes_whose_scroll_offset_geometry_readers_read {
                    Some(nodes) => nodes.iter().all(|&node_index| {
                        offset_of_node(&cached.geometry.raster_scroll_offsets, node_index)
                            == offset_of_node(scroll_offsets, node_index)
                    }),
                    None => {
                        cached.geometry.raster_scroll_offsets
                            == raster_scroll_offsets(&stack.spaces, raster_space_index, scroll_offsets)
                    }
                }
            {
                geometries.push(cached.geometry.clone());
                continue;
            }
            let raster_spaces = self
                .raster_spaces
                .get_or_insert_with(|| RasterSpaces::of_spaces(tree, stack.spaces.clone()));
            let tree_copy_without_animation_samples = tree.copy_without_animation_samples();
            let what_every_space_of_tree_shares = match &self.what_every_space_of_tree_shares {
                Some((tree_copy, shares)) if *tree_copy == tree_copy_without_animation_samples => shares,
                _ => {
                    &self
                        .what_every_space_of_tree_shares
                        .insert((
                            tree_copy_without_animation_samples,
                            WhatEverySpaceOfTreeShares::of(tree),
                        ))
                        .1
                }
            };
            let geometry = Rc::new(RasterSpaceGeometry::new(
                tree,
                raster_spaces,
                raster_space_index,
                scroll_offsets,
                self.viewport_to_raster_transform,
                what_every_space_of_tree_shares,
            ));
            *cached_geometry = Some(CachedRasterSpaceGeometry {
                tree_copy_number: tree.copy.number(),
                tree_copy_without_animation_samples: tree.copy_without_animation_samples(),
                geometry: geometry.clone(),
            });
            geometries.push(geometry);
        }
        geometries
    }
}

/// What of a list and a tree keeps the tiles of a stack of one layer from replaying only the records that reach them,
/// worked out once for both.
struct LayeringObstaclesOfListAndTree {
    // Keeps the address of the list from being taken by another list.
    display_list: Weak<DisplayList>,
    tree_copy_number: u64,
    obstacles: LayeringObstacles,
}

/// What a frame does with its layers. Those of the latest frame that was painted, or that showed nothing new, are what a
/// snapshot of the frame on screen and what the window server shows above it are composited with until another frame is
/// painted.
pub struct PaintedLayers {
    /// The stack the frame draws with.
    pub(super) stack: Rc<LayerStack>,
    /// From the base layer up, the layers that can hold something, and where the frame composites each.
    pub(super) compositing_of_layers: Vec<LayerCompositingInFrame>,
    pub(super) window_server_placements_of_media: WindowServerPlacementsOfMedia,
    pub(super) window_server_placements_of_effects: WindowServerPlacementsOfEffects,
    /// What the window server that shows the frame composites by itself, besides canvases and videos.
    pub(super) window_server_compositing: WindowServerCompositing,
    content_hash_by_composited_node: HashMap<PlacementNodeIdentity, ContentHash>,
    media_replayed_by_identity: HashMap<LayerIdentity, MediaRecordsReplayedInFrame>,
}

impl PaintedLayers {
    /// Whether the window server shows layers of the frame by itself.
    pub fn shown_by_window_server(&self) -> bool {
        self.compositing_of_layers
            .iter()
            .any(|compositing| compositing.shown_by_window_server)
    }

    /// Whether the window server shows every layer of the frame by itself, so that the frame holds none and needs no
    /// store. It shows the base layer only where it shows every layer above it.
    pub fn window_server_shows_every_layer(&self) -> bool {
        self.compositing_of_layers
            .first()
            .is_some_and(|base_layer_compositing| base_layer_compositing.shown_by_window_server)
    }

    /// Whether the window server shows the tiles of the layer by itself, so that what changes of them changes
    /// nothing of what the frame holds.
    fn window_server_shows_tiles_of(&self, compositing: &LayerCompositingInFrame) -> bool {
        compositing.shown_by_window_server && self.window_server_compositing.shows_tiles_of_painted_layers
    }
}

/// The layers of the frames of a context, their tiles, and what of them the window server shows.
#[derive(Default)]
pub struct ContextLayers {
    /// The stack the latest frame was drawn with, painted or not, and what frames keep of it.
    stack_of_frames: Option<StackOfFrames>,
    layering_obstacles_of_frames_not_drawn_as_planned: Option<LayeringObstaclesOfListAndTree>,
    pub(super) tiles: LayerTiles,
    pub(super) window_server: WindowServerLayers,
    /// The plan whose frames show more tiles than a frame may, which frames draw as one layer until another plan comes.
    /// The address of its stack is kept from being taken by the stack of another plan.
    plan_showing_too_many_tiles: Option<Weak<LayerStack>>,
}

impl ContextLayers {
    pub fn spare_tile_eviction_timer_fired(&mut self, env: &PresentingEnvironment<'_>) {
        self.tiles.evict_spare_tiles(env);
    }

    /// Whether frames draw `layer_plan` as one layer, as a frame drawn with it showed more tiles than a frame may.
    fn plan_shows_too_many_tiles(&self, layer_plan: &ReceivedLayerPlan) -> bool {
        self.plan_showing_too_many_tiles
            .as_ref()
            .is_some_and(|stack| std::ptr::eq(stack.as_ptr(), Rc::as_ptr(&layer_plan.stack)))
    }

    /// Has later frames draw the plan of the stack the frame took as one layer, as the frame showed more tiles than a
    /// frame may.
    fn draw_plan_of_frames_as_one_layer(&mut self) {
        let layer_plan = self
            .stack_of_frames
            .as_ref()
            .and_then(|stack_of_frames| stack_of_frames.layer_plan.as_ref())
            .expect("only a frame drawn with a plan has a budget of tiles");
        self.plan_showing_too_many_tiles = Some(Rc::downgrade(&layer_plan.stack));
    }

    /// The stack of one layer that replays the whole list, for a frame that is not drawn as a plan was made for, with
    /// what of the list and the tree keeps its tiles from replaying only the records that reach them, as a plan of one
    /// layer has.
    fn one_layer_stack_of_frame(
        &mut self,
        display_list: &Rc<DisplayList>,
        tree: &VisualContextTree,
        viewport_rect: IntRect,
        why_list_is_one_layer: WhyListIsOneLayer,
    ) -> LayerStack {
        let obstacles = match &self.layering_obstacles_of_frames_not_drawn_as_planned {
            Some(of_list_and_tree)
                if of_list_and_tree.tree_copy_number == tree.copy.number()
                    && std::ptr::eq(of_list_and_tree.display_list.as_ptr(), Rc::as_ptr(display_list)) =>
            {
                of_list_and_tree.obstacles
            }
            _ => {
                let obstacles = LayeringObstacles::of(display_list.storage(), tree);
                self.layering_obstacles_of_frames_not_drawn_as_planned = Some(LayeringObstaclesOfListAndTree {
                    display_list: Rc::downgrade(display_list),
                    tree_copy_number: tree.copy.number(),
                    obstacles,
                });
                obstacles
            }
        };
        LayerStack::one_layer_replaying_whole_list(
            viewport_rect,
            display_list.storage().whole_span(),
            Some(why_list_is_one_layer),
            obstacles,
        )
    }

    /// Takes the stack a frame draws with: the plan WebContent sent, or for a frame that is not drawn as a plan was
    /// made for, one layer that replays the whole list. Keeps what frames kept of the stack where the frame before drew
    /// with the same plan, or with the same one layer of the same list under the same raster transform, and answers
    /// whether the frame took another stack.
    fn take_stack_of_frame(
        &mut self,
        layer_plan: Option<&ReceivedLayerPlan>,
        display_list: &Rc<DisplayList>,
        one_layer: impl FnOnce(&mut Self) -> LayerStack,
        viewport_to_raster_transform: FloatMatrix4x4,
    ) -> bool {
        let latest_plan = self
            .stack_of_frames
            .as_ref()
            .and_then(|stack_of_frames| stack_of_frames.layer_plan.as_ref());
        if let Some(layer_plan) = layer_plan
            && latest_plan.is_some_and(|latest_plan| Rc::ptr_eq(&latest_plan.stack, &layer_plan.stack))
        {
            return false;
        }
        let stack = match layer_plan {
            Some(layer_plan) => layer_plan.stack.clone(),
            None => {
                let one_layer = one_layer(self);
                let latest_drew_same_one_layer = self.stack_of_frames.as_ref().is_some_and(|latest| {
                    latest.layer_plan.is_none()
                        && latest.viewport_to_raster_transform == viewport_to_raster_transform
                        && std::ptr::eq(latest.display_list.as_ptr(), Rc::as_ptr(display_list))
                        && *latest.stack == one_layer
                });
                if latest_drew_same_one_layer {
                    return false;
                }
                Rc::new(one_layer)
            }
        };
        self.stack_of_frames = Some(StackOfFrames::new(
            layer_plan.cloned(),
            stack,
            display_list,
            viewport_to_raster_transform,
            self.stack_of_frames.take(),
        ));
        true
    }

    pub fn tile_raster_scratch_surface_release_timer_fired(&mut self) {
        self.tiles.release_raster_scratch_surface();
    }
}

fn rect_in_frame_of_space(
    rect_in_space: IntRect,
    space_to_frame: SpaceToFrame,
    composite_clip: Option<IntRect>,
    within: IntRect,
) -> IntRect {
    let rect_in_frame = space_to_frame.rect_in_frame(rect_in_space).intersected(within);
    composite_clip.map_or(rect_in_frame, |composite_clip| {
        rect_in_frame.intersected(composite_clip)
    })
}

fn frame_rect_in_space_unless_viewport_or_transformed(
    raster_space_index: RasterSpaceIndex,
    space_to_frame: SpaceToFrame,
    viewport_rect: IntRect,
) -> Option<IntRect> {
    match space_to_frame {
        SpaceToFrame::Offset(offset) if raster_space_index != RasterSpaceIndex::VIEWPORT => {
            Some(viewport_rect.translated(-offset.x, -offset.y))
        }
        _ => None,
    }
}

fn rect_of_kept_tiles(
    raster_space_index: RasterSpaceIndex,
    rect_shown: IntRect,
    viewport_rect: IntRect,
    composited_filter_read_reach: i32,
) -> IntRect {
    if raster_space_index == RasterSpaceIndex::VIEWPORT {
        return inflated(viewport_rect, composited_filter_read_reach);
    }
    if rect_shown.is_empty() {
        return IntRect::default();
    }
    let (margin_x_kept_for_scrolling_back, margin_y_kept_for_scrolling_back) =
        (viewport_rect.width / 2, viewport_rect.height / 2);
    rect_shown.inflated_edges(
        margin_y_kept_for_scrolling_back,
        margin_x_kept_for_scrolling_back,
        margin_y_kept_for_scrolling_back,
        margin_x_kept_for_scrolling_back,
    )
}

fn composited_filter_read_reach_by_layer(
    stack: &LayerStack,
    tree: &VisualContextTree,
    geometries: &[Rc<RasterSpaceGeometry>],
) -> Vec<i32> {
    stack.summed_read_reach_of_composited_filters_by_layer(|node_index, effect| {
        let effect_node = &tree.effect_nodes[effect.0 as usize];
        spreading_filter_of(effect_node).map_or(0, |filter| {
            composited_filter_read_reach_in_device_pixels(
                filter,
                &geometries[stack.nodes[node_index.0].raster_space_index.0]
                    .world
                    .draw_matrix(effect_node.spatial),
            )
        })
    })
}

/// How many cells a rect of the size of `viewport_rect` can touch.
fn cells_one_layer_shows_at_most(viewport_rect: IntRect) -> u64 {
    let tile_size = TILE_SIZE_IN_DEVICE_PIXELS as u64;
    let most_cells_along = |length: i32| (length.max(1) as u64 - 1).div_ceil(tile_size) + 1;
    most_cells_along(viewport_rect.width) * most_cells_along(viewport_rect.height)
}

/// The tiles a frame drawn with a plan shows so far, which it counts as it keys them, against how many it may show.
struct TileBudgetOfFrame {
    most_tiles: u64,
    most_tile_pixels: u64,
    tiles: u64,
    tile_pixels: u64,
}

impl TileBudgetOfFrame {
    fn within(viewport_rect: IntRect) -> Self {
        let cells = cells_one_layer_shows_at_most(viewport_rect);
        let cell_pixels = (TILE_SIZE_IN_DEVICE_PIXELS as u64).pow(2);
        Self {
            most_tiles: MAXIMUM_TILES_A_PLAN_SHOWS_PER_CELL_ONE_LAYER_SHOWS * cells,
            most_tile_pixels: MAXIMUM_LAYERS_OF_TILES_A_PLAN_SHOWS * cells * cell_pixels,
            tiles: 0,
            tile_pixels: 0,
        }
    }

    /// Counts a tile that shows `part_of_cell_needed`, and answers whether the frame still shows no more than it may.
    fn takes_tile(&mut self, part_of_cell_needed: IntRect) -> bool {
        self.tiles += 1;
        self.tile_pixels += part_of_cell_needed.width.max(0) as u64 * part_of_cell_needed.height.max(0) as u64;
        self.tiles <= self.most_tiles && self.tile_pixels <= self.most_tile_pixels
    }
}

fn rect_is_covered_by_union_of(rect: IntRect, covering_rects: &[IntRect]) -> bool {
    if covering_rects.iter().any(|cover| cover.contains_rect(rect)) {
        return true;
    }
    if covering_rects.iter().all(|cover| cover.intersected(rect).is_empty()) {
        return false;
    }
    let mut uncovered_parts = vec![rect];
    for &cover in covering_rects {
        if cover.intersected(rect).is_empty() {
            continue;
        }
        uncovered_parts = uncovered_parts
            .into_iter()
            .flat_map(|part| {
                let overlap = part.intersected(cover);
                if overlap.is_empty() {
                    return vec![part];
                }
                let above_overlap = IntRect::new(part.x, part.y, part.width, overlap.y - part.y);
                let below_overlap =
                    IntRect::new(part.x, overlap.bottom(), part.width, part.bottom() - overlap.bottom());
                let left_of_overlap = IntRect::new(part.x, overlap.y, overlap.x - part.x, overlap.height);
                let right_of_overlap = IntRect::new(
                    overlap.right(),
                    overlap.y,
                    part.right() - overlap.right(),
                    overlap.height,
                );
                [above_overlap, below_overlap, left_of_overlap, right_of_overlap]
                    .into_iter()
                    .filter(|piece| !piece.is_empty())
                    .collect()
            })
            .collect();
        if uncovered_parts.is_empty() {
            return true;
        }
        if uncovered_parts.len() > MAXIMUM_UNCOVERED_PARTS_OF_TILE {
            return false;
        }
    }
    uncovered_parts.is_empty()
}

// A tile below is tested against what at most `MAXIMUM_COMPOSITED_EFFECT_READS_TESTED_ONE_BY_ONE` composited effects
// read, one by one. Past that, it is tested against the bounds of all they read, which hides fewer tiles.
fn composited_effect_reads_tiles_are_tested_against(read_reaches_in_frame: &[IntRect]) -> Cow<'_, [IntRect]> {
    if read_reaches_in_frame.len() <= MAXIMUM_COMPOSITED_EFFECT_READS_TESTED_ONE_BY_ONE {
        return Cow::Borrowed(read_reaches_in_frame);
    }
    Cow::Owned(vec![
        read_reaches_in_frame
            .iter()
            .fold(IntRect::default(), |bounds, &read_reach| bounds.united(read_reach)),
    ])
}

// A backdrop filter that reads past its tile by an unknown reach reads its layer's tiles wherever they are, so none of
// them may be hidden.
fn hide_tiles_covered_by_opaque_tiles_above(
    stack: &LayerStack,
    shown_tiles_by_layer: &mut [Vec<TileShownInFrame>],
    compositing_of_layers: &[LayerCompositingInFrame],
    read_reaches_of_composited_effects_in_frame: &[IntRect],
    viewport_rect: IntRect,
    is_hidden_from_view: impl Fn(&LayerIdentity) -> bool,
) -> usize {
    let composited_effect_reads_tested_against_tiles =
        composited_effect_reads_tiles_are_tested_against(read_reaches_of_composited_effects_in_frame);
    let mut opaque_rects_in_frame_above: Vec<IntRect> = Vec::new();
    let mut hidden_tile_count = 0;
    for compositing in compositing_of_layers.iter().rev() {
        if opaque_rects_in_frame_above.len() > MAXIMUM_OPAQUE_RECTS_HIDING_TILES_BELOW {
            break;
        }
        let layer_index = compositing.layer_index;
        let layer = &stack.layers[layer_index.0];
        if !matches!(layer.content, LayerContent::Painted) {
            continue;
        }
        let shown = &mut shown_tiles_by_layer[layer_index.0];
        let every_backdrop_read_reach_is_known = shown.iter().all(|tile| tile.raster_margin_in_device_pixels.is_some());
        if !opaque_rects_in_frame_above.is_empty() && every_backdrop_read_reach_is_known {
            let tile_count_before = shown.len();
            shown.retain(|tile| {
                let needed_rect_in_frame = compositing.rect_in_frame(tile.part_of_cell_needed, viewport_rect);
                let is_read_by_composited_effect = composited_effect_reads_tested_against_tiles
                    .iter()
                    .any(|read_reach| !read_reach.intersected(needed_rect_in_frame).is_empty());
                needed_rect_in_frame.is_empty()
                    || is_read_by_composited_effect
                    || !rect_is_covered_by_union_of(needed_rect_in_frame, &opaque_rects_in_frame_above)
            });
            hidden_tile_count += tile_count_before - shown.len();
        }
        // A layer the window server shows is above every layer the frame holds, so it covers the layers below it
        // whichever of the two composites it.
        let is_inside_composited_effects = stack.layer_is_inside_composited_effects(layer_index);
        let covers_layers_below = !is_inside_composited_effects
            && compositing.rounded_composite_clips.is_empty()
            && matches!(compositing.space_to_frame, SpaceToFrame::Offset(_))
            && !is_hidden_from_view(&compositing.identity);
        if covers_layers_below {
            let opaque_rects_in_frame = shown
                .iter()
                .filter(|tile| tile.coverage.is_opaque())
                .map(|tile| (compositing.rect_in_frame(tile.part_of_cell_needed, viewport_rect), ()));
            opaque_rects_in_frame_above.extend(
                joined_rects_of_grid_given_row_by_row(opaque_rects_in_frame)
                    .into_iter()
                    .map(|(rect, _)| rect),
            );
        }
    }
    hidden_tile_count
}

fn layer_allows_lcd_text(stack: &LayerStack, layer_index: LayerIndex, frame_allows_lcd_text: bool) -> bool {
    frame_allows_lcd_text && layer_index == stack.base_layer && !stack.layer_is_inside_composited_effects(layer_index)
}

fn clear_color_of_layer(stack: &LayerStack, layer_index: LayerIndex, surface_clear_color: Color) -> Color {
    if layer_index == stack.base_layer {
        surface_clear_color
    } else {
        Color::TRANSPARENT
    }
}

/// Where a frame that composites the layers it holds as the frame before did, except through other transforms of
/// spaces whose anchors the compositor animates, needs painting: where those layers were in the frame before, as the
/// tiles that frame held of them and their reach tell, and where they are now. None for a frame that composites
/// anything else it holds differently.
fn damage_of_layers_moved_by_animated_transforms(
    compositing_of_latest_frame: &[LayerCompositingInFrame],
    bounds_of_tiles_held_by: impl Fn(&LayerIdentity) -> IntRect,
    layer_update: &LayerUpdate,
    viewport_rect: IntRect,
) -> Option<IntRect> {
    if compositing_of_latest_frame.len() != layer_update.layers.compositing_of_layers.len() {
        return None;
    }
    let mut damage = IntRect::default();
    for (latest, current) in compositing_of_latest_frame
        .iter()
        .zip(&layer_update.layers.compositing_of_layers)
    {
        if latest == current {
            continue;
        }
        // The window server moves and clips a layer it shows tile by tile, which changes nothing of what the frame
        // holds.
        let window_server_shows_layer_in_both_frames = latest.identity == current.identity
            && latest.shown_by_window_server
            && layer_update.layers.window_server_shows_tiles_of(current);
        if window_server_shows_layer_in_both_frames {
            continue;
        }
        let moved_by_animated_transform = matches!(
            (latest.space_to_frame, current.space_to_frame),
            (SpaceToFrame::Transform(_), SpaceToFrame::Transform(_))
        ) && LayerCompositingInFrame {
            space_to_frame: current.space_to_frame,
            ..latest.clone()
        } == *current;
        if !moved_by_animated_transform {
            return None;
        }
        let composited_filter_read_reach = layer_update.composited_filter_read_reach_by_layer[current.layer_index.0];
        let reach_in_layer_space = layer_update.layers.stack.layers[current.layer_index.0].reach;
        // A new list can shrink the reach of the layer, and the frame before showed every tile it held of the layer.
        let held_in_latest_frame = bounds_of_tiles_held_by(&latest.identity);
        damage = damage
            .united(latest.rect_in_frame(
                inflated(
                    reach_in_layer_space.united(held_in_latest_frame),
                    composited_filter_read_reach,
                ),
                viewport_rect,
            ))
            .united(current.rect_in_frame(
                inflated(reach_in_layer_space, composited_filter_read_reach),
                viewport_rect,
            ));
    }
    Some(damage)
}

struct CompositedEffectsOfFrame {
    stale_rect: IntRect,
    read_reaches_in_frame: Vec<IntRect>,
    content_hash_by_composited_node: HashMap<PlacementNodeIdentity, ContentHash>,
}

#[allow(clippy::too_many_arguments)]
fn composited_effects_of_frame(
    stack: &LayerStack,
    tree: &VisualContextTree,
    geometries: &[Rc<RasterSpaceGeometry>],
    tile_keying_by_raster_space: &[TileKeyingOfRasterSpace<'_>],
    composite_offsets: &[IntPoint],
    spaces_to_frame: &[SpaceToFrame],
    viewport_rect: IntRect,
    content_hash_by_composited_node_of_painted_frame: Option<&HashMap<PlacementNodeIdentity, ContentHash>>,
) -> CompositedEffectsOfFrame {
    let mut effects = CompositedEffectsOfFrame {
        stale_rect: IntRect::default(),
        read_reaches_in_frame: Vec::new(),
        content_hash_by_composited_node: HashMap::new(),
    };
    for composited in &stack.composited_effects {
        let node = &stack.nodes[composited.placement_node_index.0];
        let space_to_frame = spaces_to_frame[node.raster_space_index.0];
        let composite_clip =
            stack.composite_clip_of_node_in_viewport(composited.placement_node_index, composite_offsets);
        let rect_in_frame_of_reach = |reach: Reach<IntRect>, rect_if_anywhere: IntRect| match reach {
            Reach::Within(rect) => rect_in_frame_of_space(rect, space_to_frame, composite_clip, viewport_rect),
            Reach::Nothing => IntRect::default(),
            Reach::Anywhere => rect_if_anywhere,
        };
        let viewport_rect_within_composite_clip = composite_clip.map_or(viewport_rect, |composite_clip| {
            viewport_rect.intersected(composite_clip)
        });
        let node_reach_in_frame =
            rect_in_frame_of_reach(node.reach_in_parent_node, viewport_rect_within_composite_clip);
        let content_hash =
            tile_keying_by_raster_space[node.raster_space_index.0].content_hash_of_composited_effect(composited.effect);
        if content_hash_by_composited_node_of_painted_frame
            .and_then(|content_hashes| content_hashes.get(&node.identity))
            != Some(&content_hash)
        {
            effects.stale_rect = effects.stale_rect.united(node_reach_in_frame);
        }
        effects
            .content_hash_by_composited_node
            .insert(node.identity, content_hash);
        if composited.filter_spreads_what_it_composites {
            effects.read_reaches_in_frame.push(node_reach_in_frame);
        } else if composited.reads_what_is_composited_below_it {
            let backdrop_read_reach =
                backdrop_filter_read_reach(tree, &geometries[node.raster_space_index.0], composited.effect);
            effects
                .read_reaches_in_frame
                .push(rect_in_frame_of_reach(backdrop_read_reach, node_reach_in_frame));
        }
    }
    effects
}

/// Which layers above the base layer a pass over the layers of a frame composites.
#[derive(Clone, Copy, PartialEq, Eq)]
enum LayersToComposite {
    HeldByFrame,
    ShownByWindowServer,
}

fn compositing_of_layer(
    stack: &LayerStack,
    layer_index: LayerIndex,
    composite_offsets: &[IntPoint],
    spaces_to_frame: &[SpaceToFrame],
) -> LayerCompositingInFrame {
    LayerCompositingInFrame {
        layer_index,
        identity: stack.layers[layer_index.0].identity,
        space_to_frame: spaces_to_frame[stack.space_of_layer(layer_index).0],
        composite_clip: stack.composite_clip_of_layer_in_viewport(layer_index, composite_offsets),
        rounded_composite_clips: stack.rounded_composite_clips_of_node_in_viewport(
            stack.layers[layer_index.0].placement_node_index,
            composite_offsets,
        ),
        shown_by_window_server: false,
    }
}

impl ContextPresenting {
    /// Works out the layers of the frame within `viewport_rect`, in the device pixels of the raster: those of the plan
    /// WebContent sent where the frame is drawn as the plan was made for, and one layer that replays the whole list
    /// otherwise. A frame drawn with a plan that shows more tiles than a frame may is drawn as one layer instead.
    ///
    /// Keys the tiles the frame shows of each layer, and answers too the rect of the viewport that the tiles to raster
    /// again, the tiles that go, and the media replayed anew cover. `invalidates_every_tile` makes every tile stale
    /// whatever its content hash, as when the frame draws content that the list does not name. What a frame is made to
    /// repaint otherwise leaves the tiles as they are: it is either drawn over the layers by the compositor, as its
    /// scroll bars are, or told by the content hashes of the tiles that hold it, as a caret's blink is.
    pub(super) fn layer_update_for(
        &mut self,
        env: &PresentingEnvironment<'_>,
        frame: &FrameToPaint<'_>,
        viewport_rect: IntRect,
        live_content: &LiveContent,
        invalidates_every_tile: bool,
        frame_allows_lcd_text: bool,
    ) -> (LayerUpdate, IntRect) {
        if invalidates_every_tile {
            self.layers.tiles.invalidate_every_tile();
        }
        let raster_transform = self.raster_transform();
        let tree = frame.frame_inputs.visual_context_tree.as_arc();
        // A plan places spaces that scroll at whole device pixels of the viewport it was made within, which a raster
        // that scales them or shifts them by a fraction lacks, as does a viewport of another size. So only a stack of
        // one layer, in the viewport's space, is drawn under a raster transform other than identity, and what
        // composites the spaces and effects of a plan does not apply the raster transform.
        let plan_drawn_as_made_for = frame.frame_inputs.layer_plan.as_ref().filter(|layer_plan| {
            raster_transform == FloatMatrix4x4::identity() && layer_plan.viewport_rect == viewport_rect
        });
        let layer_plan = plan_drawn_as_made_for.filter(|layer_plan| !self.layers.plan_shows_too_many_tiles(layer_plan));
        let why_list_is_one_layer = if plan_drawn_as_made_for.is_some() {
            WhyListIsOneLayer::PlanShowsTooManyTiles
        } else {
            WhyListIsOneLayer::FrameIsNotDrawnAsPlanned
        };
        let one_layer = |layers: &mut ContextLayers| {
            layers.one_layer_stack_of_frame(frame.display_list, tree, viewport_rect, why_list_is_one_layer)
        };
        let took_another_stack =
            self.layers
                .take_stack_of_frame(layer_plan, frame.display_list, one_layer, raster_transform);
        let took_plan_into_use = layer_plan.is_some() && took_another_stack;
        let layer_update = self.layer_update_of_stack(env, frame, viewport_rect, live_content, frame_allows_lcd_text);
        let Some((mut layer_update, stale_rect)) = layer_update else {
            // Frames draw the plan as one layer from now on. The budget does not hold a frame of one layer, so the frame
            // is worked out once more at most.
            self.layers.draw_plan_of_frames_as_one_layer();
            return self.layer_update_for(
                env,
                frame,
                viewport_rect,
                live_content,
                invalidates_every_tile,
                frame_allows_lcd_text,
            );
        };
        layer_update.work_counts.took_plan_into_use = took_plan_into_use;
        (layer_update, stale_rect)
    }

    /// Where a frame that composites its layers as the last painted one did, except through other transforms of spaces
    /// whose anchors the compositor animates, needs painting: where those layers were and are. None for a frame that
    /// composites anything else differently.
    pub(super) fn damage_of_layers_moved_by_animated_transforms(
        &self,
        layer_update: &LayerUpdate,
        viewport_rect: IntRect,
    ) -> Option<IntRect> {
        damage_of_layers_moved_by_animated_transforms(
            &self.painted_frame.as_ref()?.layers.compositing_of_layers,
            |identity| self.layers.tiles.bounds_held_by(identity),
            layer_update,
            viewport_rect,
        )
    }

    fn layer_update_of_stack(
        &mut self,
        env: &PresentingEnvironment<'_>,
        frame: &FrameToPaint<'_>,
        viewport_rect: IntRect,
        live_content: &LiveContent,
        frame_allows_lcd_text: bool,
    ) -> Option<(LayerUpdate, IntRect)> {
        let storage = frame.display_list.storage();
        let tree = frame.frame_inputs.visual_context_tree.as_arc();
        let scroll_offsets = frame.frame_inputs.presented_scroll_state_snapshot.device_offsets();
        let raster_transform = self.raster_transform();
        let stack_of_frames = self
            .layers
            .stack_of_frames
            .as_mut()
            .expect("a frame with layers has a stack");
        let geometries = stack_of_frames.geometries_of_spaces(tree, scroll_offsets);
        let stack_the_frame_draws_with = stack_of_frames.stack.clone();
        let mut tile_budget = stack_of_frames
            .layer_plan
            .is_some()
            .then(|| TileBudgetOfFrame::within(viewport_rect));
        let stack = stack_the_frame_draws_with.as_ref();
        // Built at the first question, as only media the window server may show and spaces whose anchors the
        // compositor animates ask for it.
        let world_transforms_of_frame = OnceCell::new();
        let world_of_frame = || {
            world_transforms_of_frame
                .get_or_init(|| WorldTransforms::new(tree, scroll_offsets, Some(FloatMatrix4x4::identity())))
        };
        let window_server_may_show_media = self.why_window_server_shows_no_media_of_this_context(env).is_none();
        let window_server_compositing = self.window_server_compositing(env);
        let window_server_placements_of_media = if window_server_may_show_media {
            stack.window_server_placements_of_media(storage, tree, world_of_frame, viewport_rect)
        } else {
            WindowServerPlacementsOfMedia::default()
        };
        let composite_offsets = stack.composite_offsets(scroll_offsets);
        let spaces_to_frame = stack.spaces_to_frame(&composite_offsets, |anchor| world_of_frame().draw_matrix(anchor));
        let window_server_placements_of_effects = if window_server_compositing.shows_tiles_of_painted_layers {
            stack.window_server_placements_of_composited_effects(
                storage,
                tree,
                world_of_frame,
                window_server_compositing,
            )
        } else {
            WindowServerPlacementsOfEffects::default()
        };
        let shown_by_window_server_by_layer = if window_server_may_show_media {
            stack.layers_the_window_server_shows(
                &composite_offsets,
                &WhatWindowServerShowsOfFrame {
                    compositing: window_server_compositing,
                    can_show_media_of_layer: &|layer_index| {
                        self.why_window_server_does_not_show_media_layer(
                            env,
                            &window_server_placements_of_media,
                            layer_index,
                            &stack.layers[layer_index.0],
                            live_content,
                        )
                        .is_none()
                    },
                    can_composite_effect_of_node: &|node_index| {
                        matches!(window_server_placements_of_effects.of_node(node_index), Some(Ok(_)))
                    },
                },
            )
        } else {
            vec![false; stack.layers.len()]
        };
        let compositing_of_layers: Vec<LayerCompositingInFrame> = (stack.base_layer.0..stack.layers.len())
            .map(LayerIndex)
            .filter(|&layer_index| layer_index == stack.base_layer || stack.layers[layer_index.0].may_hold_content)
            .map(|layer_index| LayerCompositingInFrame {
                shown_by_window_server: shown_by_window_server_by_layer[layer_index.0],
                ..compositing_of_layer(stack, layer_index, &composite_offsets, &spaces_to_frame)
            })
            .collect();
        let scrollbars = frame.frame_inputs.scrollbars_shown_for_the_page();
        let live_content_versions = LiveContentVersionsOfFrame {
            live_content,
            nested_contexts: env.nested_contexts,
        };
        let tile_keying_of_list = TileKeyingOfList::new(
            storage,
            tree,
            stack,
            FrameStateReadByReplay {
                scrollbars: &scrollbars,
                time_ns: frame.frame_inputs.painted_at.nanoseconds(),
                live_content: &live_content_versions,
            },
        );
        let tile_keying_by_raster_space: Vec<TileKeyingOfRasterSpace<'_>> = geometries
            .iter()
            .map(|geometry| TileKeyingOfRasterSpace::new(&tile_keying_of_list, geometry))
            .collect();
        let surface_clear_color = frame.surface_clear_color();
        let root_group_color = frame.display_list.root_group_color();
        let raster_settings_hash_by_layer: Vec<ContentHash> = (0..stack.layers.len())
            .map(LayerIndex)
            .map(|layer_index| {
                LayerRasterSettings {
                    raster_space_identity: stack.spaces[stack.space_of_layer(layer_index).0].identity,
                    clear_color: clear_color_of_layer(stack, layer_index, surface_clear_color),
                    root_group_color,
                    viewport_to_raster_transform: raster_transform,
                    allows_lcd_text: layer_allows_lcd_text(stack, layer_index, frame_allows_lcd_text),
                }
                .content_hash()
            })
            .collect();

        let composited_filter_read_reach_by_layer = composited_filter_read_reach_by_layer(stack, tree, &geometries);
        let mut work_counts = FrameWorkCounts::default();
        let mut shown_tiles_by_layer: Vec<Vec<TileShownInFrame>> =
            (0..stack.layers.len()).map(|_| Vec::new()).collect();
        let mut frame_shows_too_many_tiles = false;
        let band_tile_keys_by_band = &mut self
            .layers
            .stack_of_frames
            .as_mut()
            .expect("a frame with layers has a stack")
            .band_tile_keys;
        for (band_position, band) in stack.bands.iter().enumerate() {
            let band_index = BandIndex(band_position);
            if !stack.band_has_layer_at_or_above_base_layer(band_index) {
                continue;
            }
            let raster_space_index = stack.nodes[band.placement_node_index.0].raster_space_index;
            let tile_keying = &tile_keying_by_raster_space[raster_space_index.0];
            let compositing_of_layer_below_all_cuts =
                compositing_of_layer(stack, band.layer_below_all_cuts, &composite_offsets, &spaces_to_frame);
            let composited_filter_read_reach = composited_filter_read_reach_by_layer[band.layer_below_all_cuts.0];
            let shown_rect = compositing_of_layer_below_all_cuts
                .rect_shown_in_layer_space(viewport_rect, composited_filter_read_reach);
            // Frames drawn with one stack draw one list, so the tile keys of the band are bound to the list and the band
            // of the frame.
            let geometry = &geometries[raster_space_index.0];
            let band_tile_keys_slot = &mut band_tile_keys_by_band[band_position];
            let band_tile_keys = match band_tile_keys_slot {
                Some(band_tile_keys) if Rc::ptr_eq(&band_tile_keys.geometry, geometry) => band_tile_keys,
                _ => band_tile_keys_slot.insert(BandTileKeysOfFrame {
                    geometry: geometry.clone(),
                    tile_keys: tile_keying.bind_band(stack, band_index),
                }),
            };
            let keying_counts = tile_keying.key_shown_tiles(
                &mut band_tile_keys.tile_keys,
                stack,
                band_index,
                &raster_settings_hash_by_layer,
                shown_rect,
                rect_of_kept_tiles(
                    raster_space_index,
                    shown_rect,
                    viewport_rect,
                    composited_filter_read_reach,
                ),
                frame_rect_in_space_unless_viewport_or_transformed(
                    raster_space_index,
                    compositing_of_layer_below_all_cuts.space_to_frame,
                    viewport_rect,
                ),
            );
            work_counts.cells_binned += keying_counts.cells_binned as u64;
            for tile in band_tile_keys
                .tile_keys
                .tiles_within(shown_rect)
                .filter(|tile| tile.layer_index >= stack.base_layer)
            {
                let mut part_of_cell_needed = if tile.layer_index == stack.base_layer {
                    tile.coord.cell_rect()
                } else {
                    tile.content_bounds
                };
                if raster_space_index == RasterSpaceIndex::VIEWPORT {
                    part_of_cell_needed =
                        part_of_cell_needed.intersected(inflated(viewport_rect, composited_filter_read_reach));
                }
                if part_of_cell_needed.is_empty() {
                    continue;
                }
                if let Some(tile_budget) = &mut tile_budget
                    && !tile_budget.takes_tile(part_of_cell_needed)
                {
                    frame_shows_too_many_tiles = true;
                    break;
                }
                let coverage = self
                    .layers
                    .tiles
                    .current_coverage(tile.content_hash, part_of_cell_needed)
                    .unwrap_or_else(|| {
                        tile_keying.coverage_of_tile(
                            &band_tile_keys.tile_keys,
                            tile,
                            part_of_cell_needed,
                            clear_color_of_layer(stack, tile.layer_index, surface_clear_color),
                        )
                    });
                shown_tiles_by_layer[tile.layer_index.0].push(TileShownInFrame {
                    coord: tile.coord,
                    content_hash: tile.content_hash,
                    part_of_cell_needed,
                    cuts: tile.cuts.clone(),
                    raster_margin_in_device_pixels: tile.raster_margin_in_device_pixels,
                    coverage,
                });
            }
            if frame_shows_too_many_tiles {
                break;
            }
        }
        if frame_shows_too_many_tiles {
            return None;
        }
        work_counts.chunks_placed = tile_keying_by_raster_space
            .iter()
            .map(|tile_keying| tile_keying.chunks_placed())
            .sum();

        let painted = self.painted_frame.as_ref().map(|painted_frame| &painted_frame.layers);
        let composited_effects = composited_effects_of_frame(
            stack,
            tree,
            &geometries,
            &tile_keying_by_raster_space,
            &composite_offsets,
            &spaces_to_frame,
            viewport_rect,
            painted.map(|painted| &painted.content_hash_by_composited_node),
        );
        let mut stale_rect = composited_effects.stale_rect;
        work_counts.tiles_covered_by_opaque_tiles_above = hide_tiles_covered_by_opaque_tiles_above(
            stack,
            &mut shown_tiles_by_layer,
            &compositing_of_layers,
            &composited_effects.read_reaches_in_frame,
            viewport_rect,
            |identity| self.layer_overlay.hides_layer(identity),
        ) as u64;
        let mut media_replayed_by_identity = HashMap::new();
        for compositing in &compositing_of_layers {
            let layer_index = compositing.layer_index;
            let layer = &stack.layers[layer_index.0];
            if let LayerContent::Media { record_span } = &layer.content {
                let raster_space_index = stack.space_of_layer(layer_index);
                let media_replayed = MediaRecordsReplayedInFrame {
                    content_hash: tile_keying_by_raster_space[raster_space_index.0]
                        .content_hash_of_records_within(*record_span),
                    reach_in_frame: compositing.rect_in_frame(layer.reach, viewport_rect),
                };
                let media_replayed_in_latest_frame =
                    painted.and_then(|painted| painted.media_replayed_by_identity.get(&layer.identity).copied());
                if !compositing.shown_by_window_server && media_replayed_in_latest_frame != Some(media_replayed) {
                    stale_rect = stale_rect.united(media_replayed.reach_in_frame).united(
                        media_replayed_in_latest_frame.map_or(IntRect::default(), |latest| latest.reach_in_frame),
                    );
                }
                media_replayed_by_identity.insert(layer.identity, media_replayed);
                continue;
            }
            work_counts.shown_tiles_of_one_color += shown_tiles_by_layer[layer_index.0]
                .iter()
                .filter(|tile| matches!(tile.coverage, TileCoverage::OneColor(_)))
                .count() as u64;
            // The window server shows the tiles of the layer as they are, so the frame holds nothing that goes stale
            // with them.
            if compositing.shown_by_window_server && window_server_compositing.shows_tiles_of_painted_layers {
                continue;
            }
            stale_rect = stale_rect.united(self.layers.tiles.stale_rect_of_layer(
                &shown_tiles_by_layer[layer_index.0],
                compositing,
                viewport_rect,
                composited_filter_read_reach_by_layer[layer_index.0],
            ));
        }
        let layer_update = LayerUpdate {
            layers: PaintedLayers {
                stack: stack_the_frame_draws_with,
                compositing_of_layers,
                window_server_placements_of_media,
                window_server_placements_of_effects,
                window_server_compositing,
                content_hash_by_composited_node: composited_effects.content_hash_by_composited_node,
                media_replayed_by_identity,
            },
            read_reaches_of_composited_effects_in_frame: composited_effects.read_reaches_in_frame,
            shown_tiles_by_layer,
            composited_filter_read_reach_by_layer,
            frame_allows_lcd_text,
            work_counts,
        };
        Some((layer_update, stale_rect))
    }

    fn update_layer_overlay_with_frame(
        &mut self,
        env: &PresentingEnvironment<'_>,
        frame: &FrameToPaint<'_>,
        layer_update: &LayerUpdate,
        window_server_shows_layers: bool,
        some_tiles_were_painted_straight: bool,
        live_content: &LiveContent,
    ) {
        if self.layer_overlay.overlay().is_none() {
            return;
        }
        let stack_of_frames = self
            .layers
            .stack_of_frames
            .as_ref()
            .expect("a frame with layers has a stack");
        let stack = &layer_update.layers.stack;
        let hidden_by_layer: Vec<bool> = layer_update
            .layers
            .compositing_of_layers
            .iter()
            .map(|compositing| self.layer_overlay.hides_layer(&compositing.identity))
            .collect();
        let name_is_shown_by_layer = layers_whose_names_are_shown(&hidden_by_layer);
        let mut names_of_layers = stack_of_frames.names_of_layers_for_overlay.borrow_mut();
        let name_not_shown = NameOfLayerForOverlay {
            name: Rc::from(""),
            nodes_named: Rc::default(),
        };
        let why_context_shows_no_media = self.why_window_server_shows_no_media_of_this_context(env);
        let layers = layer_update
            .layers
            .compositing_of_layers
            .iter()
            .enumerate()
            .map(|(position, compositing)| {
                let layer_index = compositing.layer_index;
                let layer = &stack.layers[layer_index.0];
                let content = match &layer.content {
                    LayerContent::Painted => ModelLayerContent::Painted {
                        tiles: self.layers.tiles.model_tiles_of(&compositing.identity),
                        presentation: if window_server_shows_layers && compositing.shown_by_window_server {
                            PaintedLayerPresentation::ShownByWindowServer
                        } else {
                            let why = match why_context_shows_no_media {
                                Some(why_context_shows_no_media) => why_context_shows_no_media.to_string(),
                                None if some_tiles_were_painted_straight => {
                                    WhyMediaIsReplayed::TilesWerePaintedStraightIntoTheFrame.to_string()
                                }
                                None => self
                                    .why_frame_holds_layer(env, &layer_update.layers, layer_index, live_content)
                                    .to_string(),
                            };
                            PaintedLayerPresentation::HeldByFrame { why }
                        },
                    },
                    LayerContent::Media { .. } => ModelLayerContent::Media {
                        reach: layer.reach,
                        presentation: if window_server_shows_layers && compositing.shown_by_window_server {
                            MediaPresentation::ShownByWindowServer
                        } else {
                            let why = why_context_shows_no_media
                                .or_else(|| {
                                    self.why_window_server_does_not_show_media_layer(
                                        env,
                                        &layer_update.layers.window_server_placements_of_media,
                                        layer_index,
                                        layer,
                                        live_content,
                                    )
                                })
                                .unwrap_or(if some_tiles_were_painted_straight {
                                    WhyMediaIsReplayed::TilesWerePaintedStraightIntoTheFrame
                                } else if stack.layer_is_inside_composited_effects(layer_index) {
                                    WhyMediaIsReplayed::InsideAnEffectTheCompositorComposites
                                } else {
                                    WhyMediaIsReplayed::UnderALayerTheFrameHolds
                                });
                            MediaPresentation::Replayed { why: why.to_string() }
                        },
                    },
                };
                let NameOfLayerForOverlay { name, nodes_named } = if name_is_shown_by_layer[position] {
                    names_of_layers
                        .entry(layer_index)
                        .or_insert_with(|| {
                            let node_namer = NodeNamesFromIndicesNotingNodes::default();
                            let name = Rc::from(stack.name_of_layer(layer_index, &node_namer));
                            NameOfLayerForOverlay {
                                name,
                                nodes_named: Rc::from(node_namer.nodes_named.into_inner()),
                            }
                        })
                        .clone()
                } else {
                    name_not_shown.clone()
                };
                ModelLayer {
                    identity: compositing.identity,
                    name,
                    nodes_named,
                    hidden: hidden_by_layer[position],
                    space_to_frame: compositing.space_to_frame,
                    composite_clip: compositing.composite_clip,
                    content,
                    raster_counts_since_reset: RasterCountsByCause::default(),
                }
            })
            .collect();
        let model = LayerOverlayModel {
            viewport_rect: self.raster_rect(),
            layers,
            why_list_is_one_layer: stack.why_list_is_one_layer.map(|why| why.to_string()),
            notes_of_layer_stack: stack_of_frames
                .layer_plan
                .as_ref()
                .map_or_else(Rc::default, |layer_plan| layer_plan.note_lines.clone()),
            work_counts_of_frame: layer_update.work_counts,
            visual_context_tree_structural_epoch: frame.frame_inputs.visual_context_tree.structural_epoch(),
            raster_counts_since_reset: RasterCountsByCause::default(),
            frames_rastering_since_reset: 0,
        };
        let painted_at = frame.frame_inputs.painted_at;
        let device_pixel_ratio = self.device_pixel_ratio;
        let shaper = env.host.overlay_label_shaper(env.context_id);
        if let Some(overlay) = self.layer_overlay.overlay_mut() {
            overlay.update_with_frame(model, painted_at, device_pixel_ratio, &shaper);
        }
    }

    fn why_frame_holds_layer(
        &self,
        env: &PresentingEnvironment<'_>,
        painted_layers: &PaintedLayers,
        layer_index: LayerIndex,
        live_content: &LiveContent,
    ) -> WhyFrameHoldsLayer {
        let stack = &painted_layers.stack;
        stack.why_frame_holds_layer(
            layer_index,
            &WhatWindowServerShowsOfFrame {
                compositing: painted_layers.window_server_compositing,
                can_show_media_of_layer: &|media_layer_index| {
                    self.why_window_server_does_not_show_media_layer(
                        env,
                        &painted_layers.window_server_placements_of_media,
                        media_layer_index,
                        &stack.layers[media_layer_index.0],
                        live_content,
                    )
                    .is_none()
                },
                can_composite_effect_of_node: &|node_index| {
                    matches!(
                        painted_layers.window_server_placements_of_effects.of_node(node_index),
                        Some(Ok(_))
                    )
                },
            },
        )
    }

    /// Takes in which layers left the stack with the frame painted, or that shows nothing new, whose tiles stay in the
    /// pool for a while, and answers what the frame did with its layers.
    pub(super) fn commit_layer_update(
        &mut self,
        env: &PresentingEnvironment<'_>,
        layer_update: LayerUpdate,
    ) -> PaintedLayers {
        let painted_layers = layer_update.layers;
        let frame_draws_with_stack_of_painted_frame = self
            .painted_frame
            .as_ref()
            .is_some_and(|painted_frame| Rc::ptr_eq(&painted_frame.layers.stack, &painted_layers.stack));
        let layers = &mut self.layers;
        if !frame_draws_with_stack_of_painted_frame {
            let identities_in_stack: HashSet<LayerIdentity> = painted_layers
                .compositing_of_layers
                .iter()
                .map(|compositing| compositing.identity)
                .collect();
            layers.window_server.keep_only_layers(&identities_in_stack);
            layers.tiles.keep_only_layers(&identities_in_stack);
        }
        layers.tiles.evict_spare_tiles_if_tiles_changed(env);
        painted_layers
    }

    /// Paints what the frame holds of its layers over `damage_rect`, and presents the layers the window server shows to
    /// it. Answers the rect it painted, which grows to the whole raster where the frame has to hold what the window
    /// server was to show by itself, and the sublayers the window server shows of the frame, bottom to top. Unless
    /// `paints_ui_overlays` is set, the frame leaves out the overlays of the browser and of the layers.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn paint_layered_frame(
        &mut self,
        env: &PresentingEnvironment<'_>,
        surface: &Rc<PaintingSurface>,
        frame: &FrameToPaint<'_>,
        mut damage_rect: IntRect,
        layer_update: &mut LayerUpdate,
        live_content: &LiveContent,
        paints_ui_overlays: bool,
    ) -> (IntRect, Vec<FfiSublayer>) {
        env.raster.start_raster_cache_frame(env.context_id);
        // A store can miss the damage of several frames, which joined can reach part of what a composited effect reads.
        damage_rect = layer_update.damage_grown_over_what_composited_effects_read(damage_rect);
        let rects_painted_straight = self.raster_tiles_of_layers(env, surface, frame, layer_update);

        // The overlay goes into the surface the window server shows the overlays from, so it is worked out first,
        // expecting presenting to the window server to succeed.
        let expected_shown_by_window_server =
            layer_update.shown_by_window_server() && rects_painted_straight.is_empty();
        self.update_layer_overlay_with_frame(
            env,
            frame,
            layer_update,
            expected_shown_by_window_server,
            !rects_painted_straight.is_empty(),
            live_content,
        );

        let mut sublayers = Vec::new();
        if layer_update.shown_by_window_server() {
            let presented_sublayers = rects_painted_straight
                .is_empty()
                .then(|| {
                    self.present_layers_to_window_server(env, frame, &layer_update.layers, surface, live_content, false)
                })
                .flatten();
            match presented_sublayers {
                Some(presented_sublayers) => sublayers = presented_sublayers,
                None => {
                    layer_update.hold_every_layer_in_frame();
                    damage_rect = self.raster_rect();
                }
            }
        }
        if layer_update.shown_by_window_server() != expected_shown_by_window_server {
            self.update_layer_overlay_with_frame(
                env,
                frame,
                layer_update,
                layer_update.shown_by_window_server(),
                !rects_painted_straight.is_empty(),
                live_content,
            );
        }

        self.composite_frame_from_layers(
            env,
            surface,
            frame,
            damage_rect,
            layer_update,
            &rects_painted_straight,
            paints_ui_overlays,
        );
        if !layer_update.shown_by_window_server() {
            self.layers.window_server.did_paint_compositor_overlays_into_store();
        }
        self.layer_overlay.did_paint_frame();
        (damage_rect, sublayers)
    }

    /// Presents to the window server the layers it shows of a frame that changes nothing of what the frame holds, or
    /// that holds nothing, once it has rastered the tiles those layers show, and answers the sublayers the window
    /// server then shows of the frame, bottom to top. None where the window server cannot be given the layers, which
    /// the frame then has to hold.
    pub(super) fn present_layers_of_frame_painted_into_no_store(
        &mut self,
        env: &PresentingEnvironment<'_>,
        frame: &FrameToPaint<'_>,
        layer_update: &mut LayerUpdate,
        live_content: &LiveContent,
        overlays_draw_same_as_in_latest_frame: bool,
    ) -> Option<Vec<FfiSublayer>> {
        let surface_tiles_are_made_like = self
            .layers
            .window_server
            .surface_tiles_are_made_like_without_store(env)?;
        env.raster.start_raster_cache_frame(env.context_id);
        let rects_painted_straight =
            self.raster_tiles_of_layers(env, &surface_tiles_are_made_like, frame, layer_update);
        if !rects_painted_straight.is_empty() {
            return None;
        }
        self.update_layer_overlay_with_frame(env, frame, layer_update, true, false, live_content);
        let sublayers = self.present_layers_to_window_server(
            env,
            frame,
            &layer_update.layers,
            &surface_tiles_are_made_like,
            live_content,
            overlays_draw_same_as_in_latest_frame,
        )?;
        self.layer_overlay.did_paint_frame();
        Some(sublayers)
    }

    /// Rasters the tiles that each painted layer of the frame shows and that lack current pixels, and answers the
    /// rects of the frame where tiles are painted straight from the list instead.
    fn raster_tiles_of_layers(
        &mut self,
        env: &PresentingEnvironment<'_>,
        like: &Rc<PaintingSurface>,
        frame: &FrameToPaint<'_>,
        layer_update: &mut LayerUpdate,
    ) -> Vec<IntRect> {
        let raster = env.raster;
        let viewport_rect = self.raster_rect();
        let (raster_scale, raster_translation) = (self.raster_scale, self.raster_translation);
        let surface_clear_color = frame.surface_clear_color();
        let painted_at = frame.frame_inputs.painted_at;
        let holder_count_by_content_hash = self.layers.tiles.holder_counts(&layer_update.shown_tiles_by_layer);
        let stack_of_frames = self
            .layers
            .stack_of_frames
            .as_ref()
            .expect("a frame with layers has a stack");
        debug_assert!(Rc::ptr_eq(&layer_update.layers.stack, &stack_of_frames.stack));
        let stack = layer_update.layers.stack.as_ref();
        let tiles_replay_the_whole_list = stack.one_layer_list_cannot_tell_which_records_show_in_tile();
        let mut rects_painted_straight = Vec::new();
        for compositing in &layer_update.layers.compositing_of_layers {
            let layer_index = compositing.layer_index;
            let layer = &stack.layers[layer_index.0];
            if matches!(layer.content, LayerContent::Media { .. }) {
                continue;
            }
            let raster_space_index = stack.space_of_layer(layer_index);
            let composited_filter_read_reach = layer_update.composited_filter_read_reach_by_layer[layer_index.0];
            let rect_shown_in_layer_space =
                compositing.rect_shown_in_layer_space(viewport_rect, composited_filter_read_reach);
            let allows_lcd_text = layer_allows_lcd_text(stack, layer_index, layer_update.frame_allows_lcd_text);
            let frame_rect_in_layer_space = frame_rect_in_space_unless_viewport_or_transformed(
                raster_space_index,
                compositing.space_to_frame,
                viewport_rect,
            );
            let replay_tiles = |target: &Rc<PaintingSurface>, tiles_replayed: &[&TileShownInFrame]| {
                let band_tile_keys = stack_of_frames.band_tile_keys[layer.band_index.0]
                    .as_ref()
                    .expect("a band with shown tiles has tile keys");
                let geometry = &band_tile_keys.geometry;
                let record_spans = (!tiles_replay_the_whole_list).then(|| {
                    band_tile_keys
                        .tile_keys
                        .record_spans_of_tiles(tiles_replayed.iter().map(|tile| {
                            band_tile_keys
                                .tile_keys
                                .tile(tile.coord, layer_index)
                                .expect("a shown tile is keyed")
                        }))
                });
                let selection = ReplaySelection {
                    record_spans: record_spans.as_deref(),
                    clips_left_to_composite: (!geometry.clips_left_to_composite.is_empty())
                        .then_some(&geometry.clips_left_to_composite[..]),
                    composited_effect: stack.nodes[layer.placement_node_index.0].composited_effect,
                    allows_lcd_text,
                    frame_edge_backdrop_blurs_mirror_at: frame_rect_in_layer_space.map(IntRect::to_float),
                    raster_root: stack.spaces[raster_space_index.0].raster_root(),
                    ..ReplaySelection::default()
                };
                apply_raster_transform(raster, target, raster_scale, raster_translation);
                replay_with_scroll_offsets(env, target, frame, selection, &geometry.raster_scroll_offsets);
            };
            let rastered = self.layers.tiles.raster_layer(
                env,
                like,
                LayerToRaster {
                    identity: compositing.identity,
                    shown: layer_update.shown_tiles_of_layer(layer_index),
                    rect_shown_in_layer_space,
                    kept_tile_rect: rect_of_kept_tiles(
                        raster_space_index,
                        rect_shown_in_layer_space,
                        viewport_rect,
                        composited_filter_read_reach,
                    ),
                    clear_color: clear_color_of_layer(stack, layer_index, surface_clear_color),
                    rastered_within: (raster_space_index == RasterSpaceIndex::VIEWPORT).then_some(viewport_rect),
                    shown_by_window_server: layer_update.layers.window_server_shows_tiles_of(compositing),
                    window_server_shows_tiles: layer_update
                        .layers
                        .window_server_compositing
                        .shows_tiles_of_painted_layers,
                },
                &holder_count_by_content_hash,
                replay_tiles,
                self.layer_overlay.overlay_mut().map(|overlay| {
                    move |coord, cause, rect| {
                        overlay.record_raster(compositing.identity, coord, cause, rect, painted_at);
                    }
                }),
            );
            rects_painted_straight.extend(
                rastered
                    .rects_painted_straight
                    .into_iter()
                    .map(|rect_in_layer_space| compositing.rect_in_frame(rect_in_layer_space, viewport_rect)),
            );
            layer_update.work_counts.tiles_rastered += rastered.tile_count;
        }
        rects_painted_straight
    }

    #[allow(clippy::too_many_arguments)]
    fn composite_frame_from_layers(
        &self,
        env: &PresentingEnvironment<'_>,
        surface: &Rc<PaintingSurface>,
        frame: &FrameToPaint<'_>,
        damage_rect: IntRect,
        layer_update: &LayerUpdate,
        rects_painted_straight: &[IntRect],
        paints_ui_overlays: bool,
    ) {
        let raster = env.raster;
        let save_count = raster.save(surface);
        raster.clip_rect(surface, damage_rect);
        if let Some(base_layer_compositing) = layer_update
            .layers
            .compositing_of_layers
            .first()
            .filter(|base_layer_compositing| !base_layer_compositing.shown_by_window_server)
        {
            if self.layer_overlay.hides_layer(&base_layer_compositing.identity) {
                raster.clear(surface, frame.surface_clear_color());
            } else if self.layers.tiles.has_layer(&base_layer_compositing.identity) {
                // The base layer replaces what is below it, and nothing is below it outside its composite clip. Plans
                // clip no base layer, but the compositor does not trust a plan from WebContent.
                if base_layer_compositing.composite_clip.is_some() {
                    raster.clear(surface, frame.surface_clear_color());
                }
                self.layers
                    .tiles
                    .copy_into(raster, surface, damage_rect, base_layer_compositing);
            }
        }
        self.composite_layers_above_base_of_stack(
            env,
            surface,
            frame,
            damage_rect,
            &layer_update.layers,
            &layer_update.composited_filter_read_reach_by_layer,
            LayersToComposite::HeldByFrame,
        );
        if !layer_update.shown_by_window_server() {
            let selection_painting_straight = ReplaySelection {
                allows_lcd_text: layer_update.frame_allows_lcd_text,
                ..Default::default()
            };
            for &rect_painted_straight in rects_painted_straight {
                let damaged_rect_painted_straight = rect_painted_straight.intersected(damage_rect);
                if damaged_rect_painted_straight.is_empty() {
                    continue;
                }
                let save_count = raster.save(surface);
                raster.clip_rect(surface, damaged_rect_painted_straight);
                raster.clear(surface, frame.surface_clear_color());
                self.apply_raster_transform(raster, surface);
                replay_into(env, surface, frame, selection_painting_straight);
                raster.restore_to_count(surface, save_count);
            }
            self.apply_raster_transform(raster, surface);
            self.paint_overlays(env, surface, frame.frame_inputs, paints_ui_overlays);
        }
        raster.restore_to_count(surface, save_count);
    }

    /// Composites the layers that the window server showed by itself with the latest frame into the surface, which
    /// holds the rest of the frame, as for a snapshot of the frame on screen.
    pub(super) fn composite_layers_shown_by_window_server(
        &self,
        env: &PresentingEnvironment<'_>,
        surface: &Rc<PaintingSurface>,
        frame: &FrameToPaint<'_>,
        damage_rect: IntRect,
        painted: &PaintedLayers,
    ) {
        if let Some(base_layer_compositing) = painted
            .compositing_of_layers
            .first()
            .filter(|base_layer_compositing| base_layer_compositing.shown_by_window_server)
        {
            env.raster.clear(surface, frame.surface_clear_color());
            if !self.layer_overlay.hides_layer(&base_layer_compositing.identity) {
                self.layers
                    .tiles
                    .copy_into(env.raster, surface, damage_rect, base_layer_compositing);
            }
        }
        self.composite_layers_above_base_of_stack(
            env,
            surface,
            frame,
            damage_rect,
            painted,
            &[],
            LayersToComposite::ShownByWindowServer,
        );
    }

    #[allow(clippy::too_many_arguments)]
    fn composite_layers_above_base_of_stack(
        &self,
        env: &PresentingEnvironment<'_>,
        surface: &Rc<PaintingSurface>,
        frame: &FrameToPaint<'_>,
        damage_rect: IntRect,
        painted_layers: &PaintedLayers,
        composited_filter_read_reach_by_layer: &[i32],
        layers_to_composite: LayersToComposite,
    ) {
        let composites_layers_the_window_server_shows = layers_to_composite == LayersToComposite::ShownByWindowServer;
        let (stack, compositing_of_layers) = (&painted_layers.stack, &painted_layers.compositing_of_layers);
        if compositing_of_layers
            .iter()
            .skip(1)
            .all(|compositing| compositing.shown_by_window_server != composites_layers_the_window_server_shows)
        {
            return;
        }
        debug_assert!(self.raster_transform() == FloatMatrix4x4::identity());
        composite_inside_effects_of_frame(env, surface, frame, |compositing_session| {
            let mut entered_composited_bands: Vec<BandIndex> = Vec::new();
            let mut composited_bands_around_layer: Vec<BandIndex> = Vec::new();
            for compositing in compositing_of_layers
                .iter()
                .skip(1)
                .filter(|compositing| compositing.shown_by_window_server == composites_layers_the_window_server_shows)
                .filter(|compositing| !self.layer_overlay.hides_layer(&compositing.identity))
            {
                let layer_index = compositing.layer_index;
                let layer = compositing.layer_in(stack);
                let damage_grown_by_filter_reads = inflated(
                    damage_rect,
                    composited_filter_read_reach_by_layer
                        .get(layer_index.0)
                        .copied()
                        .unwrap_or(0),
                );
                composited_bands_around_layer.clear();
                composited_bands_around_layer.extend(stack.composited_bands_around_layer_innermost_first(layer_index));
                composited_bands_around_layer.reverse();
                let count_of_bands_still_entered = entered_composited_bands
                    .iter()
                    .zip(&composited_bands_around_layer)
                    .take_while(|(entered, around_layer)| entered == around_layer)
                    .count();
                while entered_composited_bands.len() > count_of_bands_still_entered {
                    compositing_session.leave_effect();
                    entered_composited_bands.pop();
                }
                for &band_index in &composited_bands_around_layer[count_of_bands_still_entered..] {
                    let effect = stack.nodes[stack.bands[band_index.0].placement_node_index.0]
                        .composited_effect
                        .expect("a composited band is of a composited node");
                    compositing_session.enter_effect(effect);
                    entered_composited_bands.push(band_index);
                }
                if let LayerContent::Media { record_span } = &layer.content {
                    if !compositing
                        .rect_in_frame(layer.reach, damage_grown_by_filter_reads)
                        .is_empty()
                    {
                        compositing_session.replay_records(*record_span);
                    }
                    continue;
                }
                let clips_of_layer = CompositeClipsOfRaster {
                    rect: compositing.composite_clip,
                    rounded_rects: &compositing.rounded_composite_clips,
                };
                let (layer_to_frame_offset, layer_to_frame_transform) = match compositing.space_to_frame {
                    SpaceToFrame::Offset(offset) => (offset, None),
                    SpaceToFrame::Transform(transform) => (IntPoint::default(), Some(transform)),
                };
                for draw in self
                    .layers
                    .tiles
                    .draws_reaching_into(compositing, damage_grown_by_filter_reads)
                {
                    match draw {
                        TileDraw::TileSurface {
                            surface,
                            rect_in_layer_space,
                            ..
                        } => compositing_session.draw_raster(
                            surface.painting_surface().as_ffi().cast_mut().cast(),
                            rect_of_size(size_of(rect_in_layer_space)),
                            rect_in_layer_space.translated(layer_to_frame_offset.x, layer_to_frame_offset.y),
                            clips_of_layer,
                            layer_to_frame_transform,
                        ),
                        TileDraw::FillOfJoinedOneColorTiles {
                            rect_in_layer_space,
                            color,
                        } if color.alpha() != 0 => {
                            compositing_session.fill_rect(
                                rect_in_layer_space.translated(layer_to_frame_offset.x, layer_to_frame_offset.y),
                                color,
                                clips_of_layer,
                                layer_to_frame_transform,
                            );
                        }
                        TileDraw::FillOfJoinedOneColorTiles { .. } => {}
                    }
                }
            }
            for _ in entered_composited_bands {
                compositing_session.leave_effect();
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libcompositing_rust::display_list::builder::DisplayListBuilder;
    use libcompositing_rust::display_list::commands::{
        ContextRef, EffectNodeIndex, FillRect, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
    };
    use libcompositing_rust::display_list::storage::DisplayListStorage;
    use libcompositing_rust::display_list::tiles::TileCoord;
    use libcompositing_rust::visual_context::{SpatialData, TransformData, TransformDataRole};
    use libgfx_rust::{AffineTransform, CompositingAndBlendingOperator, FloatRect};
    use std::sync::Arc;

    const VIEWPORT: IntRect = IntRect::new(0, 0, 800, 600);

    fn tree_of_viewport() -> Arc<VisualContextTree> {
        Arc::new(VisualContextTree::create(TransformData {
            matrix: FloatMatrix4x4::identity(),
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        }))
    }

    fn display_list_recorded_by(builder: DisplayListBuilder, tree: &Arc<VisualContextTree>) -> Rc<DisplayList> {
        Rc::new(DisplayList::new(
            DisplayListStorage::recorded(&builder.finish(), tree.clone()),
            1,
            tree.structural_epoch,
            None,
            None,
            Default::default(),
        ))
    }

    fn empty_display_list(tree: &Arc<VisualContextTree>) -> Rc<DisplayList> {
        display_list_recorded_by(DisplayListBuilder::new(), tree)
    }

    fn one_layer_of(display_list: &Rc<DisplayList>, viewport_rect: IntRect) -> LayerStack {
        LayerStack::one_layer_replaying_whole_list(
            viewport_rect,
            display_list.storage().whole_span(),
            None,
            LayeringObstacles::default(),
        )
    }

    fn scaled_twice() -> FloatMatrix4x4 {
        let mut matrix = FloatMatrix4x4::identity();
        matrix.elements[0][0] = 2.0;
        matrix.elements[1][1] = 2.0;
        matrix
    }

    fn frame_keeps_what_frames_kept_of_the_stack(
        layers: &mut ContextLayers,
        layer_plan: Option<&ReceivedLayerPlan>,
        display_list: &Rc<DisplayList>,
        one_layer: LayerStack,
    ) -> bool {
        !layers.take_stack_of_frame(layer_plan, display_list, |_| one_layer, FloatMatrix4x4::identity())
    }

    // A plan received again is another plan, even where it holds what the plan before held.
    fn plan_and_plan_received_again(display_list: &Rc<DisplayList>) -> (ReceivedLayerPlan, ReceivedLayerPlan) {
        let plan = ReceivedLayerPlan {
            viewport_rect: VIEWPORT,
            stack: Rc::new(one_layer_of(display_list, VIEWPORT)),
            nodes_whose_scroll_offset_plan_reads: Rc::default(),
            note_lines: Rc::default(),
        };
        let plan_received_again = ReceivedLayerPlan {
            stack: Rc::new((*plan.stack).clone()),
            ..plan.clone()
        };
        (plan, plan_received_again)
    }

    #[test]
    fn frames_keep_what_they_kept_of_a_stack_only_while_they_draw_with_the_same_plan() {
        let tree = tree_of_viewport();
        let display_list = empty_display_list(&tree);
        let (plan, plan_received_again) = plan_and_plan_received_again(&display_list);
        let mut layers = ContextLayers::default();
        let one_layer = || one_layer_of(&display_list, VIEWPORT);
        let mut keeps =
            |layer_plan| frame_keeps_what_frames_kept_of_the_stack(&mut layers, layer_plan, &display_list, one_layer());
        assert!(!keeps(Some(&plan)));
        assert!(keeps(Some(&plan)));
        assert!(!keeps(Some(&plan_received_again)));
        assert!(!keeps(None));
        assert!(keeps(None));
        assert!(!keeps(Some(&plan_received_again)));
    }

    #[test]
    fn frames_not_drawn_as_planned_keep_their_stack_of_one_layer_only_while_it_is_the_same() {
        let tree = tree_of_viewport();
        let display_list = empty_display_list(&tree);
        let another_display_list = empty_display_list(&tree);
        let resized_viewport = IntRect::new(0, 0, 400, 300);
        let mut layers = ContextLayers::default();
        let mut keeps = |display_list: &Rc<DisplayList>, viewport_rect: IntRect| {
            let one_layer = one_layer_of(display_list, viewport_rect);
            frame_keeps_what_frames_kept_of_the_stack(&mut layers, None, display_list, one_layer)
        };
        assert!(!keeps(&display_list, VIEWPORT));
        assert!(keeps(&display_list, VIEWPORT));
        assert!(!keeps(&another_display_list, VIEWPORT));
        assert!(!keeps(&another_display_list, resized_viewport));
        assert!(keeps(&another_display_list, resized_viewport));
    }

    #[test]
    fn a_new_stack_keeps_the_geometries_of_spaces_at_the_same_anchors_under_the_same_raster_transform() {
        let tree = tree_of_viewport();
        let display_list = empty_display_list(&tree);
        let identity = FloatMatrix4x4::identity();
        let resized_viewport = IntRect::new(0, 0, 400, 300);
        let mut layers = ContextLayers::default();
        layers.take_stack_of_frame(None, &display_list, |_| one_layer_of(&display_list, VIEWPORT), identity);
        let stack_of_frames = layers.stack_of_frames.as_mut().expect("the frame took a stack");
        let geometry = stack_of_frames.geometries_of_spaces(&tree, &[]).remove(0);

        assert!(layers.take_stack_of_frame(
            None,
            &display_list,
            |_| one_layer_of(&display_list, resized_viewport),
            identity
        ));
        let resized = layers.stack_of_frames.as_mut().expect("the frame took a stack");
        assert!(Rc::ptr_eq(&resized.geometries_of_spaces(&tree, &[])[0], &geometry));

        layers.take_stack_of_frame(
            None,
            &display_list,
            |_| one_layer_of(&display_list, resized_viewport),
            scaled_twice(),
        );
        let scaled = layers.stack_of_frames.as_ref().expect("the frame took a stack");
        assert!(scaled.geometry_by_space.iter().all(Option::is_none));
    }

    // A red plane in front of a blue plane that covers the viewport, though the red plane comes first in the list, as
    // `transform-style: preserve-3d` and `translateZ()` make them.
    fn planes_sorted_by_depth() -> (Arc<VisualContextTree>, Rc<DisplayList>) {
        let mut tree = (*tree_of_viewport()).clone();
        let plane = |tree: &mut VisualContextTree,
                     parent: SpatialNodeIndex,
                     sorting_context_root: Option<SpatialNodeIndex>,
                     matrix: FloatMatrix4x4| {
            tree.append_spatial(
                SpatialData::Transform(TransformData {
                    matrix,
                    origin: FloatPoint::default(),
                    sorting_context_root_index: sorting_context_root,
                    flattens_inherited_transform: false,
                    role: TransformDataRole::CssTransform,
                    synthetic_plane: false,
                    establishes_sorting_context: sorting_context_root.is_none(),
                    bounds: VIEWPORT.to_float(),
                }),
                parent,
            )
        };
        let context_root = plane(&mut tree, VISUAL_VIEWPORT_NODE_INDEX, None, FloatMatrix4x4::identity());
        let mut moved_forward = FloatMatrix4x4::identity();
        moved_forward.elements[2][3] = 10.0;
        let red_plane = plane(&mut tree, context_root, Some(context_root), moved_forward);
        let blue_plane = plane(&mut tree, context_root, Some(context_root), FloatMatrix4x4::identity());
        let tree = Arc::new(tree);
        let mut builder = DisplayListBuilder::new();
        for (spatial, color) in [
            (red_plane, Color::from_rgb(255, 0, 0)),
            (blue_plane, Color::from_rgb(0, 0, 255)),
        ] {
            builder.append(
                &FillRect {
                    rect: VIEWPORT,
                    color,
                    compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                    background_color_animation_effect: EffectNodeIndex::NONE,
                },
                &[],
                ContextRef {
                    spatial,
                    ..ContextRef::default()
                },
            );
        }
        let display_list = display_list_recorded_by(builder, &tree);
        (tree, display_list)
    }

    // A frame is not drawn as planned at a pinch, at a resize, or under the raster transform of a nested context.
    #[test]
    fn frames_not_drawn_as_planned_look_at_a_list_and_a_tree_once_for_what_keeps_tiles_from_replaying_part_of_it() {
        let (tree, display_list) = planes_sorted_by_depth();
        let mut layers = ContextLayers::default();
        let not_as_planned = WhyListIsOneLayer::FrameIsNotDrawnAsPlanned;
        let stack = layers.one_layer_stack_of_frame(&display_list, &tree, VIEWPORT, not_as_planned);
        assert!(stack.one_layer_list_cannot_tell_which_records_show_in_tile());

        // The list and the tree are looked at once for the frames that draw them.
        let obstacles_of_list_and_tree = stack.layering_obstacles;
        layers
            .layering_obstacles_of_frames_not_drawn_as_planned
            .as_mut()
            .expect("the obstacles of the list and the tree are kept")
            .obstacles
            .mask_declared_under_effects_its_mask_is_not_under = true;
        let stack_of_next_frame = layers.one_layer_stack_of_frame(&display_list, &tree, VIEWPORT, not_as_planned);
        assert!(
            stack_of_next_frame
                .layering_obstacles
                .mask_declared_under_effects_its_mask_is_not_under
        );
        let another_tree = Arc::new((*tree).clone());
        let stack_of_another_tree =
            layers.one_layer_stack_of_frame(&display_list, &another_tree, VIEWPORT, not_as_planned);
        assert_eq!(stack_of_another_tree.layering_obstacles, obstacles_of_list_and_tree);
    }

    // A spinner that the compositor rotates, whose label a new list makes shorter.
    #[test]
    fn a_layer_moved_by_an_animated_transform_damages_what_it_showed_in_the_frame_before() {
        let tree = tree_of_viewport();
        let display_list = empty_display_list(&tree);
        let short_label = IntRect::new(0, 0, 100, 20);
        let long_label = IntRect::new(0, 0, 300, 20);
        // The one layer of the stack reaches as far as the short label.
        let stack = one_layer_of(&display_list, short_label);
        let composited_at = |y: f32| LayerCompositingInFrame {
            layer_index: LayerIndex(0),
            identity: stack.layers[0].identity,
            space_to_frame: SpaceToFrame::Transform(AffineTransform::new(1.0, 0.0, 0.0, 1.0, 100.0, y)),
            composite_clip: None,
            rounded_composite_clips: Vec::new(),
            shown_by_window_server: false,
        };
        let (latest, current) = (composited_at(100.0), composited_at(200.0));
        let layer_update = layer_update_of(Rc::new(stack), vec![current.clone()], Vec::new());
        // The frame before held the tiles of the long label.
        let damage = damage_of_layers_moved_by_animated_transforms(
            std::slice::from_ref(&latest),
            |_| long_label,
            &layer_update,
            VIEWPORT,
        )
        .expect("only an animated transform moved the layer");
        assert!(damage.contains_rect(latest.rect_in_frame(long_label, VIEWPORT)));
        assert!(damage.contains_rect(current.rect_in_frame(short_label, VIEWPORT)));
    }

    // The layer update of a frame that shows no tiles, of a stack whose layers it each composites.
    fn layer_update_of(
        stack: Rc<LayerStack>,
        compositing_of_layers: Vec<LayerCompositingInFrame>,
        read_reaches_of_composited_effects_in_frame: Vec<IntRect>,
    ) -> LayerUpdate {
        LayerUpdate {
            shown_tiles_by_layer: compositing_of_layers.iter().map(|_| Vec::new()).collect(),
            composited_filter_read_reach_by_layer: vec![0; compositing_of_layers.len()],
            layers: PaintedLayers {
                stack,
                compositing_of_layers,
                window_server_placements_of_media: WindowServerPlacementsOfMedia::default(),
                window_server_placements_of_effects: WindowServerPlacementsOfEffects::default(),
                window_server_compositing: WindowServerCompositing::default(),
                content_hash_by_composited_node: HashMap::new(),
                media_replayed_by_identity: HashMap::new(),
            },
            read_reaches_of_composited_effects_in_frame,
            frame_allows_lcd_text: false,
            work_counts: FrameWorkCounts::default(),
        }
    }

    fn layer_update_with_read_reaches(read_reaches_of_composited_effects_in_frame: Vec<IntRect>) -> LayerUpdate {
        let stack = one_layer_of(&empty_display_list(&tree_of_viewport()), VIEWPORT);
        layer_update_of(Rc::new(stack), Vec::new(), read_reaches_of_composited_effects_in_frame)
    }

    // Composited effects whose reads overlap in a chain, listed from the far end of the chain, so that each pass over
    // them grows the damage by one more.
    #[test]
    fn damage_grows_over_a_long_chain_of_what_composited_effects_read_in_a_bounded_number_of_passes() {
        let chain_of_reads = |length: i32| -> Vec<IntRect> {
            (0..length)
                .rev()
                .map(|index| IntRect::new(10 + index * 10, 0, 15, 10))
                .collect()
        };
        let damage = IntRect::new(0, 0, 12, 10);
        let read_apart = IntRect::new(500, 500, 10, 10);
        let mut short_chain = chain_of_reads(3);
        short_chain.push(read_apart);
        let grown = layer_update_with_read_reaches(short_chain).damage_grown_over_what_composited_effects_read(damage);
        assert_eq!(grown, IntRect::new(0, 0, 45, 10));

        let length = 4 * MAXIMUM_PASSES_GROWING_DAMAGE_OVER_WHAT_COMPOSITED_EFFECTS_READ as i32;
        let long_chain = chain_of_reads(length);
        let grown =
            layer_update_with_read_reaches(long_chain.clone()).damage_grown_over_what_composited_effects_read(damage);
        assert!(long_chain.iter().all(|read| grown.contains_rect(*read)));
        assert!(
            layer_update_with_read_reaches(long_chain).damage_grown_over_what_composited_effects_read(read_apart)
                == read_apart
        );
    }

    // The squares of a checkerboard, light ones first, as layers above would leave them, split what they leave of a
    // tile into many parts before the dark ones cover the rest.
    #[test]
    fn a_tile_that_rects_cover_only_after_splitting_it_into_many_parts_is_not_hidden() {
        let tile = IntRect::new(0, 0, 512, 512);
        let squares_of_color = |color: i32| {
            (0..32)
                .flat_map(|row| (0..32).map(move |column| (row, column)))
                .filter(move |(row, column)| (row + column) % 2 == color)
                .map(|(row, column)| IntRect::new(column * 16, row * 16, 16, 16))
        };
        let checkerboard: Vec<IntRect> = squares_of_color(0).chain(squares_of_color(1)).collect();
        assert!(!rect_is_covered_by_union_of(tile, &checkerboard));
        assert!(rect_is_covered_by_union_of(
            tile,
            &[IntRect::new(0, 0, 300, 512), IntRect::new(200, 0, 312, 512)]
        ));
    }

    // A frame from another process can composite many effects that each read past each of their pixels.
    #[test]
    fn tiles_are_tested_against_the_bounds_of_what_composited_effects_read_past_a_bound() {
        let reads: Vec<IntRect> = (0..MAXIMUM_COMPOSITED_EFFECT_READS_TESTED_ONE_BY_ONE as i32)
            .map(|index| IntRect::new(index * 10, 0, 5, 5))
            .collect();
        assert_eq!(*composited_effect_reads_tiles_are_tested_against(&reads), reads[..]);
        let mut reads_past_bound = reads;
        reads_past_bound.push(IntRect::new(0, 1000, 5, 5));
        let tested = composited_effect_reads_tiles_are_tested_against(&reads_past_bound);
        assert_eq!(tested.len(), 1);
        assert!(reads_past_bound.iter().all(|read| tested[0].contains_rect(*read)));
    }

    // Many layers that each hold a fill of the whole viewport, of one color or not, or many small fills.
    #[test]
    fn a_frame_drawn_with_a_plan_shows_tiles_of_any_color_up_to_a_budget_of_pixels_and_of_tiles() {
        // A viewport of 800 by 600 pixels touches at most 3 by 3 cells.
        let cells_shown_most = MAXIMUM_LAYERS_OF_TILES_A_PLAN_SHOWS * 9;
        let whole_cell = TileCoord::new(0, 0).cell_rect();
        let one_pixel = IntRect::new(0, 0, 1, 1);
        let mut budget_of_whole_cells = TileBudgetOfFrame::within(VIEWPORT);
        assert!((0..cells_shown_most).all(|_| budget_of_whole_cells.takes_tile(whole_cell)));
        assert!(!budget_of_whole_cells.takes_tile(one_pixel));
        let tiles_shown_most = MAXIMUM_TILES_A_PLAN_SHOWS_PER_CELL_ONE_LAYER_SHOWS * 9;
        let mut budget_of_small_tiles = TileBudgetOfFrame::within(VIEWPORT);
        assert!((0..tiles_shown_most).all(|_| budget_of_small_tiles.takes_tile(one_pixel)));
        assert!(!budget_of_small_tiles.takes_tile(one_pixel));
    }

    #[test]
    fn a_plan_whose_frame_shows_more_tiles_than_a_frame_may_is_drawn_as_one_layer_until_another_plan_comes() {
        let tree = tree_of_viewport();
        let display_list = empty_display_list(&tree);
        let (plan, plan_received_again) = plan_and_plan_received_again(&display_list);
        let mut layers = ContextLayers::default();
        layers.take_stack_of_frame(
            Some(&plan),
            &display_list,
            |_| unreachable!("a frame drawn with a plan takes its stack"),
            FloatMatrix4x4::identity(),
        );
        assert!(!layers.plan_shows_too_many_tiles(&plan));
        layers.draw_plan_of_frames_as_one_layer();
        assert!(layers.plan_shows_too_many_tiles(&plan));
        assert!(!layers.plan_shows_too_many_tiles(&plan_received_again));
    }

    // An iframe smaller than a cell shows a whole cell of each layer whose space it scrolls.
    #[test]
    fn the_tiles_of_a_viewport_smaller_than_a_cell_are_counted_in_whole_cells() {
        assert_eq!(cells_one_layer_shows_at_most(IntRect::new(0, 0, 1, 1)), 1);
        assert_eq!(cells_one_layer_shows_at_most(IntRect::new(0, 0, 100, 40)), 4);
        assert_eq!(cells_one_layer_shows_at_most(IntRect::new(0, 0, 800, 1)), 3);
        assert_eq!(cells_one_layer_shows_at_most(VIEWPORT), 9);
        assert_eq!(cells_one_layer_shows_at_most(IntRect::new(0, 0, 1025, 513)), 6);
    }
}
