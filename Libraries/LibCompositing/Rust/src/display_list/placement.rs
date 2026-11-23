/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where the content of a display list can be rastered: the raster spaces a visual context tree offers, anchored at
//! the scroll nodes the compositor scrolls and at the sticky boxes within them, the space each visual
//! context is placed in and the clips left to apply when its rasters are composited, and the runs that are placed
//! together because they hold whole group effects.

use crate::display_list::animated_transform_spaces::{
    MAXIMUM_ANIMATED_TRANSFORM_SPACE_COUNT, animated_transform_anchors, raster_scale_of, roots_of_transform_animations,
};
use crate::display_list::commands::{
    ClipMode, ClipNodeIndex, ContextRef, DisplayListCommandRun, EffectNodeIndex, SpatialNodeIndex,
    VISUAL_VIEWPORT_NODE_INDEX,
};
use crate::display_list::effect_clip_plan::EffectClipPlan;
use crate::display_list::effect_geometry::{Extent, FiltersOfEffects, changed_extent_through_filters};
use crate::display_list::reach::Reach;
use crate::visual_context::queries::TreeCullingScratch;
use crate::visual_context::world_transforms::translation_on_whole_device_pixels;
use crate::visual_context::{
    ClipNodeData, CompositorScrollNodeRecord, EffectNodeData, RasterRoot, SpatialData, StickyData, VisualContextTree,
    WorldTransforms,
};
use libgfx_rust::{CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, IntPoint, IntRect, enclosing_int_rect};
use std::collections::HashMap;
use std::hash::{BuildHasher, Hasher};
use std::rc::Rc;

/// The index of a raster space among the spaces of a placement, each after the space it is in.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct RasterSpaceIndex(pub usize);

impl RasterSpaceIndex {
    pub const VIEWPORT: Self = Self(0);
}

const MAXIMUM_SCROLLER_SPACE_COUNT: usize = 7;
// Enough for twenty section headers and a row of sticky table headers.
const MAXIMUM_STICKY_BOX_SPACE_COUNT: usize = 32;
/// The most spaces a placement makes: the viewport's, and one at each anchor.
pub(super) const MAXIMUM_RASTER_SPACE_COUNT: usize =
    1 + MAXIMUM_SCROLLER_SPACE_COUNT + MAXIMUM_STICKY_BOX_SPACE_COUNT + MAXIMUM_ANIMATED_TRANSFORM_SPACE_COUNT;
/// How far past its bounds antialiasing can show content.
pub(crate) const ANTIALIASING_MARGIN_IN_DEVICE_PIXELS: i32 = 3;
const LARGEST_DEVICE_PIXEL_RATIO_EXTENTS_COVER_PAGES_AT: f32 = 8.0;
// Small enough that a rect twice as wide still fits in an `IntRect`.
pub(crate) const EXTENT_LIMIT_IN_DEVICE_PIXELS: f32 =
    (crate::css_pixels::MAX_INTEGER_VALUE as f32 + 1.0) * LARGEST_DEVICE_PIXEL_RATIO_EXTENTS_COVER_PAGES_AT;

pub(crate) fn extent_limit_rect() -> FloatRect {
    FloatRect::new(
        -EXTENT_LIMIT_IN_DEVICE_PIXELS,
        -EXTENT_LIMIT_IN_DEVICE_PIXELS,
        2.0 * EXTENT_LIMIT_IN_DEVICE_PIXELS,
        2.0 * EXTENT_LIMIT_IN_DEVICE_PIXELS,
    )
}

/// What the anchor of a space is, which decides how its translation is rounded and what the dump calls the space.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum RasterSpaceAnchorKind {
    Viewport,
    /// The scroll node that scrolls the viewport, whose content is the document.
    ViewportScroller,
    Scroller,
    /// A sticky box, laid out at `border_box_origin`, which its translation is rounded relative to as world transforms
    /// round it.
    StickyBox {
        border_box_origin: FloatPoint,
    },
    /// A spatial node whose transform the compositor animates, whose content is rastered in its own coordinates times
    /// `raster_scale` and composited through the transform each frame.
    AnimatedTransform {
        raster_scale: f32,
    },
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RasterSpace {
    pub anchor: SpatialNodeIndex,
    /// Names the space by its anchor's scroll node or sticky box as the page knows it, which outlives the tree and its
    /// indices.
    pub identity: u64,
    pub parent_index: RasterSpaceIndex,
    pub anchor_kind: RasterSpaceAnchorKind,
    /// How far the anchor can translate its content: the negated scroll range of a scroller, the offset range of a
    /// sticky box.
    pub lowest_anchor_translation: FloatPoint,
    pub highest_anchor_translation: FloatPoint,
    /// Where content that moves with the anchor can show, in the space's raster coordinates: a scroller's scrollport
    /// over the whole scroll range, and for a sticky box the extent of the space around it wherever the box is held.
    /// For the viewport's space, the viewport.
    pub extent: IntRect,
}

impl RasterSpace {
    pub(crate) fn of_viewport(viewport_rect: IntRect) -> Self {
        Self {
            anchor: VISUAL_VIEWPORT_NODE_INDEX,
            identity: 0,
            parent_index: RasterSpaceIndex::VIEWPORT,
            anchor_kind: RasterSpaceAnchorKind::Viewport,
            lowest_anchor_translation: FloatPoint::default(),
            highest_anchor_translation: FloatPoint::default(),
            extent: viewport_rect,
        }
    }

    /// For a space whose anchor the compositor animates, the root its rasters map its anchor's content from.
    pub fn raster_root(&self) -> Option<RasterRoot> {
        match self.anchor_kind {
            RasterSpaceAnchorKind::AnimatedTransform { raster_scale } => Some(RasterRoot {
                anchor: self.anchor,
                raster_scale,
            }),
            _ => None,
        }
    }

    pub fn anchor_translation_on_whole_device_pixels(&self, offset: FloatPoint) -> IntPoint {
        let sticky_box_origin = match self.anchor_kind {
            RasterSpaceAnchorKind::StickyBox { border_box_origin } => Some(border_box_origin),
            _ => None,
        };
        let translation = translation_on_whole_device_pixels(offset, sticky_box_origin);
        IntPoint {
            x: translation.x as i32,
            y: translation.y as i32,
        }
    }
}

/// A rect, with rounded corners or not, that rasters of content are clipped to as they are composited, in the raster
/// coordinates of a space whose anchor the content does not move with.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct CompositeClip {
    pub raster_space_index: RasterSpaceIndex,
    pub rect: IntRect,
    pub corner_radii: CornerRadii,
}

impl CompositeClip {
    pub fn is_rounded(&self) -> bool {
        self.corner_radii.has_any_radius()
    }
}

pub(crate) fn space_and_ancestors_below_viewport(
    spaces: &[RasterSpace],
    mut space: RasterSpaceIndex,
) -> impl Iterator<Item = RasterSpaceIndex> + '_ {
    std::iter::from_fn(move || {
        if space == RasterSpaceIndex::VIEWPORT {
            return None;
        }
        let current = space;
        space = spaces[space.0].parent_index;
        Some(current)
    })
}

pub(crate) fn space_is_ancestor_or_self(
    spaces: &[RasterSpace],
    ancestor: RasterSpaceIndex,
    mut space: RasterSpaceIndex,
) -> bool {
    loop {
        if space == ancestor {
            return true;
        }
        if space == RasterSpaceIndex::VIEWPORT {
            return false;
        }
        space = spaces[space.0].parent_index;
    }
}

/// The scroll offsets content of `space` is rastered under: those of the frame, with the anchors of the space and of
/// the spaces it is in at their origin.
pub fn raster_scroll_offsets(
    spaces: &[RasterSpace],
    space: RasterSpaceIndex,
    scroll_offsets: &[FloatPoint],
) -> Vec<FloatPoint> {
    let mut offsets = scroll_offsets.to_vec();
    for space in space_and_ancestors_below_viewport(spaces, space) {
        if let Some(offset) = offsets.get_mut(spaces[space.0].anchor.0 as usize) {
            *offset = FloatPoint::default();
        }
    }
    offsets
}

pub(crate) fn translation_if_only_translates(matrix: &FloatMatrix4x4) -> Option<FloatPoint> {
    if !matrix.is_2d_affine() {
        return None;
    }
    let affine = matrix.extract_2d_affine();
    affine.is_identity_or_translation().then(|| FloatPoint {
        x: affine.e(),
        y: affine.f(),
    })
}

fn node_keeps_whole_pixel_translations_below_it(data: &SpatialData) -> bool {
    match data {
        SpatialData::Scroll(_) | SpatialData::BackfaceVisibility(_) => true,
        SpatialData::Transform(transform) => transform.matrix_including_origin() == FloatMatrix4x4::identity(),
        _ => false,
    }
}

/// A value per spatial node that is worked out of the node and the value of its parent, for each node asked about and
/// the nodes above it, each once, so that questions about many nodes of a deep tree take one walk of each chain. The
/// visual viewport's node has the value given for it. A node whose chain leaves the tree or loops has none.
pub(crate) struct ValuesFoldedDownSpatialChains<T> {
    value_by_spatial_index: Vec<Option<T>>,
    chain_without_values: Vec<SpatialNodeIndex>,
}

impl<T: Copy> ValuesFoldedDownSpatialChains<T> {
    // The table takes its memory at the first question.
    pub(crate) fn new() -> Self {
        Self {
            value_by_spatial_index: Vec::new(),
            chain_without_values: Vec::new(),
        }
    }

    /// The value of `node`, where the value of the viewport's node is `value_of_viewport` and that of any other node
    /// is what `fold` gives for it and the value of its parent. Every question to one table gives the same two.
    pub(crate) fn value_of(
        &mut self,
        tree: &VisualContextTree,
        node: SpatialNodeIndex,
        value_of_viewport: T,
        fold: impl Fn(SpatialNodeIndex, T) -> T,
    ) -> Option<T> {
        if node == VISUAL_VIEWPORT_NODE_INDEX {
            return Some(value_of_viewport);
        }
        if self.value_by_spatial_index.is_empty() {
            self.value_by_spatial_index = vec![None; tree.spatial_nodes.len()];
        }
        let mut node_on_chain = node;
        let mut value = loop {
            let Some(&known_value) = self.value_by_spatial_index.get(node_on_chain.0 as usize) else {
                self.chain_without_values.clear();
                return None;
            };
            if let Some(known_value) = known_value {
                break known_value;
            }
            if node_on_chain == VISUAL_VIEWPORT_NODE_INDEX {
                break value_of_viewport;
            }
            if self.chain_without_values.len() == self.value_by_spatial_index.len() {
                self.chain_without_values.clear();
                return None;
            }
            self.chain_without_values.push(node_on_chain);
            node_on_chain = tree.spatial_nodes[node_on_chain.0 as usize].parent;
        };
        self.value_by_spatial_index[node_on_chain.0 as usize] = Some(value);
        while let Some(node_below) = self.chain_without_values.pop() {
            value = fold(node_below, value);
            self.value_by_spatial_index[node_below.0 as usize] = Some(value);
        }
        Some(value)
    }
}

/// Per spatial node, whether where it lands can change while the list and the tree's structure stay: it is under a
/// sticky box, an anchor scroll shift or a transform the compositor animates.
pub fn spatial_nodes_that_move_on_their_own(tree: &VisualContextTree) -> Vec<bool> {
    spatial_nodes_that_move_on_their_own_relative_to_spaces_at(tree, None)
}

pub(crate) fn spatial_nodes_that_move_on_their_own_relative_to_spaces_at(
    tree: &VisualContextTree,
    anchors: Option<&[SpatialNodeIndex]>,
) -> Vec<bool> {
    let is_anchor = |node: SpatialNodeIndex| anchors.is_some_and(|anchors| anchors.contains(&node));
    let mut roots_of_subtrees_moving_on_their_own: Vec<SpatialNodeIndex> =
        roots_of_transform_animations(tree, |node| !is_anchor(node)).collect();
    if let Some(anchors) = anchors {
        roots_of_subtrees_moving_on_their_own.extend(compositor_scrolled_nodes_without_own_space(tree, anchors));
    }
    roots_of_subtrees_moving_on_their_own.extend(tree.spatial_nodes.iter().enumerate().filter_map(|(index, node)| {
        let node_index = SpatialNodeIndex(index as u32);
        let moves = match node.data {
            SpatialData::Sticky(_) => !is_anchor(node_index),
            SpatialData::AnchorScrollShift(_) => true,
            _ => false,
        };
        moves.then_some(node_index)
    }));
    tree.spatial_nodes_in_subtrees_of(&roots_of_subtrees_moving_on_their_own)
}

/// Per spatial node, whether placing a list among the spaces at `anchors` leaves out the node's scroll offset: the
/// node is one of the anchors, the compositor scrolls the node, or the node moves on its own in those spaces. What
/// moves with such a node is placed in a space that follows it, or wherever the node can take it.
pub fn spatial_nodes_whose_scroll_offset_placement_leaves_out(
    tree: &VisualContextTree,
    anchors: &[SpatialNodeIndex],
) -> Vec<bool> {
    spatial_nodes_whose_scroll_offset_placement_leaves_out_of_nodes_moving(
        tree,
        anchors,
        spatial_nodes_that_move_on_their_own_relative_to_spaces_at(tree, Some(anchors)),
    )
}

/// As `spatial_nodes_whose_scroll_offset_placement_leaves_out`, from whether each spatial node moves on its own
/// relative to the spaces at `anchors`.
pub(crate) fn spatial_nodes_whose_scroll_offset_placement_leaves_out_of_nodes_moving(
    tree: &VisualContextTree,
    anchors: &[SpatialNodeIndex],
    spatial_node_moves_on_its_own: Vec<bool>,
) -> Vec<bool> {
    let mut leaves_out_by_node = spatial_node_moves_on_its_own;
    let nodes_the_compositor_scrolls = tree
        .compositor_scroll_table()
        .nodes
        .iter()
        .map(|record| record.scroll_node_index);
    // The compositor also moves a sticky box at an anchor as it scrolls the scroller of the box.
    for node in nodes_the_compositor_scrolls.chain(anchors.iter().copied()) {
        if let Some(leaves_out) = leaves_out_by_node.get_mut(node.0 as usize) {
            *leaves_out = true;
        }
    }
    leaves_out_by_node
}

fn compositor_scrolled_nodes_without_own_space<'a>(
    tree: &'a VisualContextTree,
    scrollers_with_spaces: &'a [SpatialNodeIndex],
) -> impl Iterator<Item = SpatialNodeIndex> + 'a {
    tree.compositor_scroll_table()
        .nodes
        .iter()
        .map(|record| record.scroll_node_index)
        .filter(move |node| *node != VISUAL_VIEWPORT_NODE_INDEX && !scrollers_with_spaces.contains(node))
}

fn visual_viewport_only_translates(tree: &VisualContextTree) -> bool {
    let Some(SpatialData::Transform(transform)) = tree.spatial_nodes.first().map(|node| &node.data) else {
        return false;
    };
    translation_if_only_translates(&transform.matrix_including_origin()).is_some()
}

// How deep below the viewport each node is, where nothing between it and the viewport transforms or moves on its own,
// so that content of the node lands on whole device pixels wherever it is scrolled to.
struct DepthsBelowViewportOnWholeDevicePixels<'a> {
    spatial_node_moves_on_its_own: &'a [bool],
    // Per node, how many nodes from it up to the viewport there are, where none of them breaks whole pixels.
    node_count_of_chain_on_whole_device_pixels: ValuesFoldedDownSpatialChains<Option<usize>>,
}

impl<'a> DepthsBelowViewportOnWholeDevicePixels<'a> {
    fn new(spatial_node_moves_on_its_own: &'a [bool]) -> Self {
        Self {
            spatial_node_moves_on_its_own,
            node_count_of_chain_on_whole_device_pixels: ValuesFoldedDownSpatialChains::new(),
        }
    }

    fn depth_of(&mut self, tree: &VisualContextTree, node: SpatialNodeIndex) -> Option<usize> {
        let parent = tree.spatial_nodes.get(node.0 as usize)?.parent;
        let spatial_node_moves_on_its_own = self.spatial_node_moves_on_its_own;
        let node_count_of_chain_from_parent = self.node_count_of_chain_on_whole_device_pixels.value_of(
            tree,
            parent,
            Some(0),
            |node, node_count_of_chain_from_parent| {
                let node_breaks_whole_pixels =
                    !node_keeps_whole_pixel_translations_below_it(&tree.spatial_nodes[node.0 as usize].data)
                        || spatial_node_moves_on_its_own[node.0 as usize];
                node_count_of_chain_from_parent
                    .filter(|_| !node_breaks_whole_pixels)
                    .map(|count| count + 1)
            },
        )??;
        Some(node_count_of_chain_from_parent + 1)
    }
}

fn shallowest_first_up_to(
    mut depths_and_nodes: Vec<(usize, SpatialNodeIndex)>,
    maximum_count: usize,
) -> Vec<SpatialNodeIndex> {
    depths_and_nodes.sort_by_key(|&(depth, node)| (depth, node.0));
    depths_and_nodes.truncate(maximum_count);
    depths_and_nodes.into_iter().map(|(_, node)| node).collect()
}

fn spatial_nodes_moving_on_their_own_or_under_scrollers_without_space(
    tree: &VisualContextTree,
    spatial_nodes_that_move_on_their_own: &[bool],
    scrollers_with_spaces: &[SpatialNodeIndex],
) -> Vec<bool> {
    let scrolled_nodes_without_own_space: Vec<SpatialNodeIndex> =
        compositor_scrolled_nodes_without_own_space(tree, scrollers_with_spaces).collect();
    if scrolled_nodes_without_own_space.is_empty() {
        return spatial_nodes_that_move_on_their_own.to_vec();
    }
    spatial_nodes_that_move_on_their_own
        .iter()
        .zip(tree.spatial_nodes_in_subtrees_of(&scrolled_nodes_without_own_space))
        .map(|(&moves_on_its_own, under_scroller_without_own_space)| {
            moves_on_its_own || under_scroller_without_own_space
        })
        .collect()
}

fn sticky_boxes_that_can_be_held_with_depths(
    tree: &VisualContextTree,
    spatial_nodes_that_move_on_their_own: &[bool],
    scrollers_with_spaces: &[SpatialNodeIndex],
) -> Vec<(usize, SpatialNodeIndex)> {
    if !tree
        .spatial_nodes
        .iter()
        .any(|node| matches!(node.data, SpatialData::Sticky(_)))
    {
        return Vec::new();
    }
    let spatial_node_moves_on_its_own = spatial_nodes_moving_on_their_own_or_under_scrollers_without_space(
        tree,
        spatial_nodes_that_move_on_their_own,
        scrollers_with_spaces,
    );
    let mut depths_below_viewport = DepthsBelowViewportOnWholeDevicePixels::new(&spatial_node_moves_on_its_own);
    const _: () = assert!(MAXIMUM_SCROLLER_SPACE_COUNT <= u64::BITS as usize);
    let mut scrollers_with_spaces_at_or_above = ValuesFoldedDownSpatialChains::<u64>::new();
    let bit_of_scroller_with_space = |node: SpatialNodeIndex| {
        scrollers_with_spaces
            .iter()
            .position(|&scroller| scroller == node)
            .map_or(0, |position| 1u64 << position)
    };
    tree.spatial_nodes
        .iter()
        .enumerate()
        .filter_map(|(index, node)| {
            let SpatialData::Sticky(sticky) = &node.data else {
                return None;
            };
            let node_index = SpatialNodeIndex(index as u32);
            if !sticky.non_scrolling_scroll_container.is_invalid() || !scrollers_with_spaces.contains(&sticky.scroller)
            {
                return None;
            }
            let depth = depths_below_viewport.depth_of(tree, node_index)?;
            let moves_with_its_scroller = scrollers_with_spaces_at_or_above
                .value_of(tree, node.parent, 0, |node, scrollers_above| {
                    scrollers_above | bit_of_scroller_with_space(node)
                })
                .is_some_and(|scrollers| scrollers & bit_of_scroller_with_space(sticky.scroller) != 0);
            let (lowest, highest) = sticky.offset_range_without_sticky_ancestors();
            let can_be_held = lowest.x < 0.0 || lowest.y < 0.0 || highest.x > 0.0 || highest.y > 0.0;
            (moves_with_its_scroller && can_be_held).then_some((depth, node_index))
        })
        .collect()
}

struct ScrollerThatCanHaveSpace {
    depth: usize,
    node: SpatialNodeIndex,
    can_be_wheel_scrolled: bool,
}

// A candidate that gets a space takes spaces for the candidates around it too, so it gets one only where those fit.
fn scrollers_to_give_spaces_parents_first(
    tree: &VisualContextTree,
    mut candidates: Vec<ScrollerThatCanHaveSpace>,
    maximum_count: usize,
) -> Vec<SpatialNodeIndex> {
    // Zero for a node that is not a candidate, as every candidate is at least one deep.
    let mut candidate_depth_by_spatial_index = vec![0usize; tree.spatial_nodes.len()];
    for candidate in &candidates {
        candidate_depth_by_spatial_index[candidate.node.0 as usize] = candidate.depth;
    }
    candidates.sort_by_key(|candidate| (!candidate.can_be_wheel_scrolled, candidate.depth, candidate.node.0));
    let mut nearest_candidate_at_or_above = ValuesFoldedDownSpatialChains::<Option<SpatialNodeIndex>>::new();
    let mut is_chosen = vec![false; tree.spatial_nodes.len()];
    let mut chosen_depths_and_nodes: Vec<(usize, SpatialNodeIndex)> = Vec::new();
    let mut candidate_and_unchosen_candidate_ancestors = Vec::new();
    for candidate in &candidates {
        if chosen_depths_and_nodes.len() == maximum_count {
            break;
        }
        if is_chosen[candidate.node.0 as usize] {
            continue;
        }
        candidate_and_unchosen_candidate_ancestors.clear();
        let mut candidate_on_chain = Some(candidate.node);
        let mut fits = true;
        while let Some(node) = candidate_on_chain {
            let index = node.0 as usize;
            if !is_chosen[index] {
                candidate_and_unchosen_candidate_ancestors.push((candidate_depth_by_spatial_index[index], node));
                if chosen_depths_and_nodes.len() + candidate_and_unchosen_candidate_ancestors.len() > maximum_count {
                    fits = false;
                    break;
                }
            }
            candidate_on_chain = nearest_candidate_at_or_above
                .value_of(
                    tree,
                    tree.spatial_nodes[index].parent,
                    None,
                    |node, nearest_candidate_above| {
                        if candidate_depth_by_spatial_index[node.0 as usize] != 0 {
                            Some(node)
                        } else {
                            nearest_candidate_above
                        }
                    },
                )
                .flatten();
        }
        if !fits {
            continue;
        }
        for &(_, node) in &candidate_and_unchosen_candidate_ancestors {
            is_chosen[node.0 as usize] = true;
        }
        chosen_depths_and_nodes.extend_from_slice(&candidate_and_unchosen_candidate_ancestors);
    }
    chosen_depths_and_nodes.sort_by_key(|&(depth, node)| (depth, node.0));
    chosen_depths_and_nodes.into_iter().map(|(_, node)| node).collect()
}

/// The nodes that layers can be rastered at, parents first, which the tree alone decides. First the scroll nodes the
/// compositor scrolls, over a scroll range, whose content lands on whole device pixels of the viewport wherever they
/// are scrolled to, since nothing between them and the viewport transforms, or can be animated to. Those the user can
/// scroll with a wheel go ahead of the rest, with the scrollers they are nested in. Then the sticky boxes within those
/// scrollers that can be held at a sticky edge, held or not, so that a box that starts or stops being held moves its
/// space rather than changing the spaces. Each kind goes shallowest first and then in tree order up to as many as it
/// may have.
pub fn raster_space_anchors(
    tree: &VisualContextTree,
    spatial_nodes_that_move_on_their_own: &[bool],
) -> Vec<SpatialNodeIndex> {
    if !visual_viewport_only_translates(tree) {
        return Vec::new();
    }
    let mut depths_below_viewport = DepthsBelowViewportOnWholeDevicePixels::new(spatial_nodes_that_move_on_their_own);
    let candidate_scrollers: Vec<ScrollerThatCanHaveSpace> = tree
        .compositor_scroll_table()
        .nodes
        .iter()
        .filter_map(|record| {
            let node = record.scroll_node_index;
            if node == VISUAL_VIEWPORT_NODE_INDEX || record.min_scroll_offset == record.max_scroll_offset {
                return None;
            }
            if !matches!(tree.spatial_nodes.get(node.0 as usize)?.data, SpatialData::Scroll(_)) {
                return None;
            }
            Some(ScrollerThatCanHaveSpace {
                depth: depths_below_viewport.depth_of(tree, node)?,
                node,
                can_be_wheel_scrolled: record.can_be_wheel_scrolled_horizontally
                    || record.can_be_wheel_scrolled_vertically,
            })
        })
        .collect();
    let mut anchors = scrollers_to_give_spaces_parents_first(tree, candidate_scrollers, MAXIMUM_SCROLLER_SPACE_COUNT);
    let holdable_sticky_boxes =
        sticky_boxes_that_can_be_held_with_depths(tree, spatial_nodes_that_move_on_their_own, &anchors);
    anchors.extend(shallowest_first_up_to(
        holdable_sticky_boxes,
        MAXIMUM_STICKY_BOX_SPACE_COUNT,
    ));
    anchors.extend(animated_transform_anchors(tree));
    anchors
}

pub(crate) fn compositor_scroll_record_of(
    tree: &VisualContextTree,
    node: SpatialNodeIndex,
) -> Option<&CompositorScrollNodeRecord> {
    tree.compositor_scroll_table()
        .nodes
        .iter()
        .find(|record| record.scroll_node_index == node)
}

const SCROLLER_ANCHOR_IDENTITY_SEED: u64 = 0x7261_7374_6572_5f73;
const ANIMATED_TRANSFORM_ANCHOR_IDENTITY_SEED: u64 = 0x616e_696d_6174_6564;

// A node index names the node for as long as the tree keeps it, which outlives lists and animation samples.
fn identity_of_animated_transform_anchor(anchor: SpatialNodeIndex, raster_scale: f32) -> u64 {
    let mut hasher = foldhash::fast::FixedState::with_seed(ANIMATED_TRANSFORM_ANCHOR_IDENTITY_SEED).build_hasher();
    hasher.write_u32(anchor.0);
    hasher.write_u32(raster_scale.to_bits());
    hasher.finish()
}
const STICKY_BOX_ANCHOR_IDENTITY_SEED: u64 = 0x7374_6963_6b79_5f62;

fn identity_of_scroller_anchor(record: &CompositorScrollNodeRecord) -> u64 {
    let mut hasher = foldhash::fast::FixedState::with_seed(SCROLLER_ANCHOR_IDENTITY_SEED).build_hasher();
    hasher.write_i64(record.scrollable_node_id.0);
    hasher.write_u8(record.scroll_node_kind as u8);
    hasher.write_u8(record.pseudo_element_type);
    let page_leaves_node_unnamed = record.scrollable_node_id.0 == 0;
    if page_leaves_node_unnamed {
        hasher.write_u32(record.scroll_node_index.0);
    }
    hasher.finish() | 1
}

fn identity_of_sticky_box_anchor(sticky: &StickyData, node: SpatialNodeIndex) -> u64 {
    let mut hasher = foldhash::fast::FixedState::with_seed(STICKY_BOX_ANCHOR_IDENTITY_SEED).build_hasher();
    hasher.write_i64(sticky.box_node_id.0);
    hasher.write_u8(sticky.box_pseudo_element_type);
    let page_leaves_box_unnamed = sticky.box_node_id.0 == 0;
    if page_leaves_box_unnamed {
        hasher.write_u32(node.0);
    }
    hasher.finish() | 1
}

pub(crate) fn enclosing_int_rect_within_extent_limit(rect: FloatRect) -> IntRect {
    enclosing_int_rect(rect.intersected(extent_limit_rect()))
}

// The space anchored at `anchor` inside the space at `parent`, unless the anchor is a scroller the compositor does not
// scroll.
fn space_at_anchor(
    tree: &VisualContextTree,
    anchor: SpatialNodeIndex,
    parent: RasterSpaceIndex,
) -> Option<RasterSpace> {
    if let SpatialData::Transform(_) = &tree.spatial_nodes[anchor.0 as usize].data {
        let raster_scale = raster_scale_of(tree, anchor);
        return Some(RasterSpace {
            anchor,
            identity: identity_of_animated_transform_anchor(anchor, raster_scale),
            parent_index: parent,
            anchor_kind: RasterSpaceAnchorKind::AnimatedTransform { raster_scale },
            lowest_anchor_translation: FloatPoint::default(),
            highest_anchor_translation: FloatPoint::default(),
            extent: IntRect::default(),
        });
    }
    if let SpatialData::Sticky(sticky) = &tree.spatial_nodes[anchor.0 as usize].data {
        let (lowest_anchor_translation, highest_anchor_translation) = sticky.offset_range_without_sticky_ancestors();
        return Some(RasterSpace {
            anchor,
            identity: identity_of_sticky_box_anchor(sticky, anchor),
            parent_index: parent,
            anchor_kind: RasterSpaceAnchorKind::StickyBox {
                border_box_origin: sticky.border_box_origin,
            },
            lowest_anchor_translation,
            highest_anchor_translation,
            extent: IntRect::default(),
        });
    }
    let record = compositor_scroll_record_of(tree, anchor)?;
    let mut lowest_anchor_translation = FloatPoint {
        x: -record.max_scroll_offset.x,
        y: -record.max_scroll_offset.y,
    };
    let mut highest_anchor_translation = FloatPoint {
        x: -record.min_scroll_offset.x,
        y: -record.min_scroll_offset.y,
    };
    if record.shows_overscroll_effect_horizontally {
        lowest_anchor_translation.x -= record.scrollport_rect.width as f32;
        highest_anchor_translation.x += record.scrollport_rect.width as f32;
    }
    if record.shows_overscroll_effect_vertically {
        lowest_anchor_translation.y -= record.scrollport_rect.height as f32;
        highest_anchor_translation.y += record.scrollport_rect.height as f32;
    }
    Some(RasterSpace {
        anchor,
        identity: identity_of_scroller_anchor(record),
        parent_index: parent,
        anchor_kind: if record.is_viewport {
            RasterSpaceAnchorKind::ViewportScroller
        } else {
            RasterSpaceAnchorKind::Scroller
        },
        lowest_anchor_translation,
        highest_anchor_translation,
        extent: IntRect::default(),
    })
}

/// The raster spaces of a tree at a set of anchors, and the deepest of them that each spatial node moves with.
#[derive(Clone, Debug)]
pub struct RasterSpaces {
    spaces: Vec<RasterSpace>,
    deepest_space_of_spatial_node: Vec<RasterSpaceIndex>,
    clips_left_to_composite_by_space: Vec<std::cell::OnceCell<Rc<[bool]>>>,
}

impl RasterSpaces {
    /// The spaces at `anchors`, without what `of_spaces` works out of every node of the tree for them.
    pub fn spaces_at_anchors(
        tree: &VisualContextTree,
        anchors: &[SpatialNodeIndex],
        viewport_rect: IntRect,
    ) -> Vec<RasterSpace> {
        let mut spaces = vec![RasterSpace::of_viewport(viewport_rect)];
        let mut nearest_anchor_at_or_above = ValuesFoldedDownSpatialChains::<Option<SpatialNodeIndex>>::new();
        for &anchor in anchors {
            // The space is in the space of the nearest anchor above it that has a space already.
            let mut parent = RasterSpaceIndex::VIEWPORT;
            let mut ancestor = tree.spatial_nodes[anchor.0 as usize].parent;
            while ancestor != VISUAL_VIEWPORT_NODE_INDEX {
                let Some(nearest_anchor) = nearest_anchor_at_or_above
                    .value_of(tree, ancestor, None, |node, nearest_anchor_above| {
                        if anchors.contains(&node) {
                            Some(node)
                        } else {
                            nearest_anchor_above
                        }
                    })
                    .flatten()
                else {
                    break;
                };
                if let Some(space) = spaces.iter().position(|space| space.anchor == nearest_anchor) {
                    parent = RasterSpaceIndex(space);
                    break;
                }
                ancestor = tree.spatial_nodes[nearest_anchor.0 as usize].parent;
            }
            let Some(space) = space_at_anchor(tree, anchor, parent) else {
                continue;
            };
            spaces.push(space);
        }
        spaces
    }

    /// The given spaces, which a tree offers, and the deepest of them each spatial node of the tree moves with.
    pub fn of_spaces(tree: &VisualContextTree, spaces: Vec<RasterSpace>) -> Self {
        let mut first_space_anchored_at_spatial_node: Vec<Option<RasterSpaceIndex>> =
            vec![None; tree.spatial_nodes.len()];
        for (space_index, space) in spaces.iter().enumerate().skip(1) {
            if let Some(first_space) = first_space_anchored_at_spatial_node.get_mut(space.anchor.0 as usize) {
                first_space.get_or_insert(RasterSpaceIndex(space_index));
            }
        }
        let mut deepest_space_of_spatial_node = vec![RasterSpaceIndex::VIEWPORT; tree.spatial_nodes.len()];
        tree.visit_spatial_nodes_parents_first(|index| {
            if index == VISUAL_VIEWPORT_NODE_INDEX.0 as usize {
                return;
            }
            let node = &tree.spatial_nodes[index];
            deepest_space_of_spatial_node[index] =
                if let Some(space_anchored_at_node) = first_space_anchored_at_spatial_node[index] {
                    space_anchored_at_node
                } else if matches!(node.data, SpatialData::AnchorScrollShift(_)) {
                    RasterSpaceIndex::VIEWPORT
                } else {
                    deepest_space_of_spatial_node[node.parent.0 as usize]
                };
        });
        Self {
            clips_left_to_composite_by_space: vec![std::cell::OnceCell::new(); spaces.len()],
            spaces,
            deepest_space_of_spatial_node,
        }
    }

    pub fn spaces(&self) -> &[RasterSpace] {
        &self.spaces
    }

    pub fn len(&self) -> usize {
        self.spaces.len()
    }

    pub fn is_empty(&self) -> bool {
        self.spaces.is_empty()
    }

    pub fn deepest_space_of(&self, spatial: SpatialNodeIndex) -> RasterSpaceIndex {
        self.deepest_space_of_spatial_node
            .get(spatial.0 as usize)
            .copied()
            .unwrap_or(RasterSpaceIndex::VIEWPORT)
    }

    /// Per clip node, whether rasters of `space` leave it to be applied as they are composited, since the clip does
    /// not move with the space. Worked out once per space, as every tree the spaces serve has the same nodes.
    pub fn clips_left_to_composite(&self, space: RasterSpaceIndex, tree: &VisualContextTree) -> Rc<[bool]> {
        self.clips_left_to_composite_by_space[space.0]
            .get_or_init(|| {
                tree.clip_nodes
                    .iter()
                    .map(|node| {
                        self.deepest_space_of_spatial_node
                            .get(node.spatial.0 as usize)
                            .is_none_or(|&deepest| !space_is_ancestor_or_self(&self.spaces, space, deepest))
                    })
                    .collect()
            })
            .clone()
    }
}

fn bounds_of_projected_rect_if_corners_in_front_of_eye(matrix: FloatMatrix4x4, rect: FloatRect) -> Option<FloatRect> {
    const SMALLEST_W_IN_FRONT_OF_EYE: f32 = 0.00001;
    let corners = [
        (rect.x, rect.y),
        (rect.right(), rect.y),
        (rect.x, rect.bottom()),
        (rect.right(), rect.bottom()),
    ]
    .map(|(x, y)| matrix.map_vector4([x, y, 0.0, 1.0]));
    if corners.iter().any(|corner| corner[3] <= SMALLEST_W_IN_FRONT_OF_EYE) {
        return None;
    }
    let xs = corners.map(|corner| corner[0] / corner[3]);
    let ys = corners.map(|corner| corner[1] / corner[3]);
    let (left, right) = (
        xs.into_iter().fold(f32::INFINITY, f32::min),
        xs.into_iter().fold(f32::NEG_INFINITY, f32::max),
    );
    let (top, bottom) = (
        ys.into_iter().fold(f32::INFINITY, f32::min),
        ys.into_iter().fold(f32::NEG_INFINITY, f32::max),
    );
    Some(FloatRect::new(left, top, right - left, bottom - top))
}

pub(crate) fn is_finite_rect(rect: FloatRect) -> bool {
    rect.x.is_finite() && rect.y.is_finite() && rect.width.is_finite() && rect.height.is_finite()
}

/// What the filters and clips of a list drawn under a tree do to where its content can show.
#[derive(Clone, Copy)]
pub struct FiltersAndClipsOfList<'a> {
    pub tree: &'a VisualContextTree,
    /// The filters of the whole effect chain of each effect: of every sample of the animations of the tree, as placing
    /// follows them, or those the tree holds, as keying the tiles of a frame follows them.
    pub filters: &'a FiltersOfEffects,
    pub effect_clips: &'a EffectClipPlan,
}

impl FiltersAndClipsOfList<'_> {
    /// The clip that bounds how far the filters that content of `context` is drawn through spread it: the clip outside
    /// every such filter, or the clip of the context where no filter spreads it.
    pub(crate) fn clip_bounding_filter_spread(&self, context: ContextRef) -> ClipNodeIndex {
        clip_bounding_spread_of_filters(self.filters, self.effect_clips, context)
    }
}

fn clip_bounding_spread_of_filters(
    filters: &FiltersOfEffects,
    effect_clips: &EffectClipPlan,
    context: ContextRef,
) -> ClipNodeIndex {
    filters
        .effect_bounding_spread_of(context.effect)
        .map_or(context.clip, |effect| effect_clips.output_clip(effect))
}

/// What `bounds_by_clip` holds for `clip`, or none for no clip.
pub(crate) fn chain_bounds_of_clip(clip: ClipNodeIndex, bounds_by_clip: &[Option<FloatRect>]) -> Option<FloatRect> {
    if clip.is_none() {
        None
    } else {
        bounds_by_clip[clip.0 as usize]
    }
}

pub(crate) fn antialiased_reach_in_device_pixels(bounds: Option<FloatRect>) -> Reach<IntRect> {
    let Some(bounds) = bounds.filter(|bounds| is_finite_rect(*bounds)) else {
        return Reach::Anywhere;
    };
    let bounds = bounds.intersected(extent_limit_rect());
    if bounds.is_empty() {
        return Reach::Nothing;
    }
    let margin = ANTIALIASING_MARGIN_IN_DEVICE_PIXELS;
    Reach::Within(enclosing_int_rect(bounds).inflated_edges(margin, margin, margin, margin))
}

/// What the geometries of every space of a tree share: the order its spatial nodes are filled in, and what it culls.
pub struct WhatEverySpaceOfTreeShares {
    spatial_dependency_order: Vec<u32>,
    culling: TreeCullingScratch,
}

impl WhatEverySpaceOfTreeShares {
    pub fn of(tree: &VisualContextTree) -> Rc<Self> {
        let mut culling = TreeCullingScratch::default();
        tree.fill_culling_scratch(&mut culling);
        Rc::new(Self {
            spatial_dependency_order: tree.spatial_dependency_order(),
            culling,
        })
    }
}

/// Where the content of one raster space lands in the space's raster coordinates under the scroll offsets of a frame:
/// the transforms of the tree's spatial nodes with the anchors of the space's chain at their origin, what the tree
/// culls, and the bounds of its clip chains.
pub struct RasterSpaceGeometry {
    pub raster_scroll_offsets: Vec<FloatPoint>,
    pub world: Rc<WorldTransforms>,
    what_every_space_of_tree_shares: Rc<WhatEverySpaceOfTreeShares>,
    /// Empty for the viewport's space, which applies every clip.
    pub clips_left_to_composite: Rc<[bool]>,
    rastered_affine_clip_chain_bounds_by_clip: Vec<Option<FloatRect>>,
    pub(crate) rastered_affine_own_clip_bounds_by_clip: Vec<Option<FloatRect>>,
}

impl RasterSpaceGeometry {
    /// `viewport_to_raster_transform` maps the device pixels of the viewport into those of the raster. It is the
    /// identity wherever there is more than one space.
    pub fn new(
        tree: &VisualContextTree,
        spaces: &RasterSpaces,
        space: RasterSpaceIndex,
        scroll_offsets: &[FloatPoint],
        viewport_to_raster_transform: FloatMatrix4x4,
        what_every_space_of_tree_shares: &Rc<WhatEverySpaceOfTreeShares>,
    ) -> Self {
        debug_assert!(viewport_to_raster_transform == FloatMatrix4x4::identity() || spaces.len() == 1);
        let raster_scroll_offsets = raster_scroll_offsets(spaces.spaces(), space, scroll_offsets);
        let mut world = WorldTransforms::default();
        world.fill(
            tree,
            &what_every_space_of_tree_shares.spatial_dependency_order,
            &raster_scroll_offsets,
            Some(viewport_to_raster_transform),
            spaces.spaces()[space.0].raster_root(),
        );
        Self::with_world(
            tree,
            spaces,
            space,
            raster_scroll_offsets,
            Rc::new(world),
            what_every_space_of_tree_shares,
        )
    }

    /// As `new`, with the world transforms that `new` fills for the space under `raster_scroll_offsets`.
    pub(crate) fn with_world(
        tree: &VisualContextTree,
        spaces: &RasterSpaces,
        space: RasterSpaceIndex,
        raster_scroll_offsets: Vec<FloatPoint>,
        world: Rc<WorldTransforms>,
        what_every_space_of_tree_shares: &Rc<WhatEverySpaceOfTreeShares>,
    ) -> Self {
        let clips_left_to_composite = if space == RasterSpaceIndex::VIEWPORT {
            Rc::default()
        } else {
            spaces.clips_left_to_composite(space, tree)
        };
        let mut rastered_affine_clip_chain_bounds_by_clip: Vec<Option<FloatRect>> = vec![None; tree.clip_nodes.len()];
        let mut rastered_affine_own_clip_bounds_by_clip: Vec<Option<FloatRect>> = vec![None; tree.clip_nodes.len()];
        tree.visit_clip_nodes_parents_first(|index| {
            let node = &tree.clip_nodes[index];
            let own_clip_rect = match &node.data {
                ClipNodeData::Rect(data) if data.mode == ClipMode::Intersect => Some(data.rect),
                ClipNodeData::Path(path) => Some(path.bounding_rect.to_float()),
                _ => None,
            };
            let left_to_composite = clips_left_to_composite.get(index).copied().unwrap_or(false);
            let own_bounds_if_rastered_affinely = own_clip_rect
                .filter(|_| !left_to_composite && world.draw_matrix(node.spatial).is_2d_affine())
                .map(|rect| world.map_rect_to_root(node.spatial, rect))
                .filter(|rect| is_finite_rect(*rect));
            rastered_affine_own_clip_bounds_by_clip[index] = own_bounds_if_rastered_affinely;
            rastered_affine_clip_chain_bounds_by_clip[index] = intersect_bounds(
                own_bounds_if_rastered_affinely,
                chain_bounds_of_clip(node.parent, &rastered_affine_clip_chain_bounds_by_clip),
            );
        });
        Self {
            raster_scroll_offsets,
            world,
            what_every_space_of_tree_shares: what_every_space_of_tree_shares.clone(),
            clips_left_to_composite,
            rastered_affine_clip_chain_bounds_by_clip,
            rastered_affine_own_clip_bounds_by_clip,
        }
    }

    pub fn clip_is_left_to_composite(&self, clip: ClipNodeIndex) -> bool {
        self.clips_left_to_composite
            .get(clip.0 as usize)
            .copied()
            .unwrap_or(false)
    }

    fn shows_nothing_of(&self, drawn_within: FloatRect, context: ContextRef) -> bool {
        drawn_within.is_empty()
            || self
                .what_every_space_of_tree_shares
                .culling
                .context_culls_everything(context)
            || self.world.backface_culled[context.spatial.0 as usize]
    }

    /// Where content of `context` can show in the space's raster coordinates, antialiased: drawn within `drawn_within`
    /// as the frame places it, spread by the filters it is drawn through, within the clips that rasters of the space
    /// apply. Placing a list asks this of the ink of its runs, and keying tiles of the records in them, so what a
    /// record reaches in a frame is within what its run reaches, as far as the space's extent shows either.
    pub fn reach_of_content(
        &self,
        list: &FiltersAndClipsOfList<'_>,
        drawn_within: FloatRect,
        context: ContextRef,
    ) -> Reach<IntRect> {
        if self.shows_nothing_of(drawn_within, context) {
            return Reach::Nothing;
        }
        let (space_of_extent, extent) = changed_extent_through_filters(
            list.tree,
            list.filters,
            context.spatial,
            drawn_within,
            context.effect,
            &self.world,
        );
        let Some(bounds) = self.bounds_where_extent_lands(space_of_extent, extent, context.spatial) else {
            return Reach::Nothing;
        };
        antialiased_reach_in_device_pixels(intersect_bounds(
            bounds,
            chain_bounds_of_clip(
                list.clip_bounding_filter_spread(context),
                &self.rastered_affine_clip_chain_bounds_by_clip,
            ),
        ))
    }

    /// As `reach_of_content`, for content a raster draws inside `composited_effect`, the innermost effect the compositor
    /// composites at or above the effect of `context`, which it leaves out with the effects around it and the clips the
    /// effect is entered within: through the filters inside the effect, within the clips inside it, and within as far
    /// past the clips it is entered within as the filters left out read, by `read_reach_of_filters_left_out` where that
    /// is known. `bounds_of_rastered_clips` gives what `rastered_bounds_of_clips_from_up_to` gives, which a caller can
    /// keep for every content of one context.
    #[allow(clippy::too_many_arguments)]
    pub fn reach_of_content_inside_composited_effect(
        &self,
        list: &FiltersAndClipsOfList<'_>,
        filters_inside_composited_effects: &FiltersOfEffects,
        drawn_within: FloatRect,
        context: ContextRef,
        composited_effect: EffectNodeIndex,
        read_reach_of_filters_left_out: Option<i32>,
        bounds_of_rastered_clips: impl FnOnce(ClipNodeIndex, ClipNodeIndex) -> Option<FloatRect>,
    ) -> Reach<IntRect> {
        if self.shows_nothing_of(drawn_within, context) {
            return Reach::Nothing;
        }
        let (space_of_extent, extent) = changed_extent_through_filters(
            list.tree,
            filters_inside_composited_effects,
            context.spatial,
            drawn_within,
            context.effect,
            &self.world,
        );
        let Some(bounds) = self.bounds_where_extent_lands(space_of_extent, extent, context.spatial) else {
            return Reach::Nothing;
        };
        let clip_bounding_filter_spread =
            clip_bounding_spread_of_filters(filters_inside_composited_effects, list.effect_clips, context);
        let clip_composited_effect_is_entered_within = list
            .effect_clips
            .output_clip_if_participating(composited_effect)
            .unwrap_or(ClipNodeIndex::NONE);
        let bounds_of_clips_inside_composited_effect =
            bounds_of_rastered_clips(clip_bounding_filter_spread, clip_composited_effect_is_entered_within);
        let clips_entered_within_widened_by_what_filters_left_out_read =
            read_reach_of_filters_left_out.and_then(|read_reach| {
                let bounds = if clip_composited_effect_is_entered_within.is_none() {
                    None
                } else {
                    self.rastered_affine_clip_chain_bounds_by_clip[clip_composited_effect_is_entered_within.0 as usize]
                }?;
                let twice_read_reach = 2.0 * read_reach as f32;
                Some(bounds.inflated(twice_read_reach, twice_read_reach))
            });
        antialiased_reach_in_device_pixels(intersect_bounds(
            intersect_bounds(bounds, bounds_of_clips_inside_composited_effect),
            clips_entered_within_widened_by_what_filters_left_out_read,
        ))
    }

    /// The bounds, rastered in the space, of the clips from `first_clip` up to the clip before `end_clip`, using what
    /// `bounds_by_first_and_end_clip` holds, and keeping there what it works out, so that a walk of the clips of many
    /// contents passes each clip once for each end clip.
    pub fn rastered_bounds_of_clips_from_up_to(
        &self,
        tree: &VisualContextTree,
        first_clip: ClipNodeIndex,
        end_clip: ClipNodeIndex,
        bounds_by_first_and_end_clip: &mut HashMap<(ClipNodeIndex, ClipNodeIndex), Option<FloatRect>>,
        clips_without_bounds: &mut Vec<ClipNodeIndex>,
    ) -> Option<FloatRect> {
        clips_without_bounds.clear();
        let mut clip = first_clip;
        let mut bounds = loop {
            if clip.is_none() || clip == end_clip {
                break None;
            }
            if let Some(&bounds) = bounds_by_first_and_end_clip.get(&(clip, end_clip)) {
                break bounds;
            }
            clips_without_bounds.push(clip);
            clip = tree.clip_nodes[clip.0 as usize].parent;
        };
        while let Some(clip) = clips_without_bounds.pop() {
            bounds = intersect_bounds(self.rastered_affine_own_clip_bounds_by_clip[clip.0 as usize], bounds);
            bounds_by_first_and_end_clip.insert((clip, end_clip), bounds);
        }
        bounds
    }

    // None where the extent lands nowhere, Some(None) where nothing bounds where it lands.
    fn bounds_where_extent_lands(
        &self,
        space_of_extent: SpatialNodeIndex,
        extent: Extent,
        context_spatial: SpatialNodeIndex,
    ) -> Option<Option<FloatRect>> {
        let maps_affinely = self.world.draw_matrix(space_of_extent).is_2d_affine()
            && self.world.draw_matrix(context_spatial).is_2d_affine();
        Some(match extent {
            Extent::Nothing => return None,
            Extent::Within(rect) if maps_affinely => {
                Some(self.world.map_rect_to_root(space_of_extent, rect)).filter(|rect| is_finite_rect(*rect))
            }
            Extent::Within(rect) if space_of_extent == context_spatial => {
                bounds_of_projected_rect_if_corners_in_front_of_eye(self.world.draw_matrix(space_of_extent), rect)
                    .filter(|rect| is_finite_rect(*rect))
            }
            _ => None,
        })
    }
}

pub(crate) fn intersect_bounds(bounds: Option<FloatRect>, limit: Option<FloatRect>) -> Option<FloatRect> {
    match (bounds, limit) {
        (Some(bounds), Some(limit)) => Some(bounds.intersected(limit)),
        (bounds, limit) => bounds.or(limit),
    }
}

/// The index of a run group among the `RunGroups` of a list, and in `ListPlacement::run_groups`, which places them in
/// the same order.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct RunGroupIndex(pub usize);

/// Runs that are placed together, as they hold every run of the group effects and of the 3D sorting contexts that sort
/// their planes by depth among them, in paint order.
pub struct RunGroups {
    first_and_last_run_of_groups: Vec<(u32, u32)>,
    group_of_run: Vec<u32>,
    group_is_under_mask: Vec<bool>,
    roots_of_contexts_sorting_planes_by_depth: Vec<Option<SpatialNodeIndex>>,
    run_is_sorted_by_depth: Vec<bool>,
    group_is_sorted_by_depth: Vec<bool>,
}

// By the tree's structure rather than effect values, so that groups stay put while an opacity animates. An effect that
// `effect_leaves_its_runs_ungrouped_by_effect_index` marks groups nothing, which holds only where no effect around it
// groups.
fn outermost_grouping_effects(
    tree: &VisualContextTree,
    effect_leaves_its_runs_ungrouped_by_effect_index: &[bool],
) -> Vec<EffectNodeIndex> {
    let mut outermost_grouping_effect_of_effect = vec![EffectNodeIndex::NONE; tree.effect_nodes.len()];
    tree.visit_effect_nodes_parents_first(|index| {
        let node = &tree.effect_nodes[index];
        let outermost_of_parent = if node.parent.is_none() {
            EffectNodeIndex::NONE
        } else {
            outermost_grouping_effect_of_effect[node.parent.0 as usize]
        };
        let leaves_its_runs_ungrouped = effect_leaves_its_runs_ungrouped_by_effect_index
            .get(index)
            .copied()
            .unwrap_or(false);
        let is_grouping = node.data.pushes_layer()
            && tree.root_isolation_effect != Some(EffectNodeIndex(index as u32))
            && !leaves_its_runs_ungrouped;
        outermost_grouping_effect_of_effect[index] = if !outermost_of_parent.is_none() {
            outermost_of_parent
        } else if is_grouping {
            EffectNodeIndex(index as u32)
        } else {
            EffectNodeIndex::NONE
        };
    });
    outermost_grouping_effect_of_effect
}

fn first_and_last_run_of_groups_holding_whole_grouping_effects_and_sorting_contexts(
    grouping_effect_of_run: &[EffectNodeIndex],
    sorting_context_of_run: &[Option<SpatialNodeIndex>],
) -> Vec<(u32, u32)> {
    let mut last_run_of_grouping_effect = HashMap::new();
    let mut last_run_of_sorting_context = HashMap::new();
    for (run_index, (grouping_effect, sorting_context)) in
        grouping_effect_of_run.iter().zip(sorting_context_of_run).enumerate()
    {
        if !grouping_effect.is_none() {
            last_run_of_grouping_effect.insert(grouping_effect.0, run_index);
        }
        if let Some(sorting_context) = sorting_context {
            last_run_of_sorting_context.insert(sorting_context.0, run_index);
        }
    }
    let mut first_and_last_run_of_groups = Vec::new();
    let mut run_index = 0;
    while run_index < grouping_effect_of_run.len() {
        let first = run_index;
        let mut last = run_index;
        let mut run_that_may_extend_group = run_index;
        while run_that_may_extend_group <= last {
            let grouping_effect = grouping_effect_of_run[run_that_may_extend_group];
            if !grouping_effect.is_none() {
                last = last.max(last_run_of_grouping_effect[&grouping_effect.0]);
            }
            if let Some(sorting_context) = sorting_context_of_run[run_that_may_extend_group] {
                last = last.max(last_run_of_sorting_context[&sorting_context.0]);
            }
            run_that_may_extend_group += 1;
        }
        first_and_last_run_of_groups.push((first as u32, last as u32));
        run_index = last + 1;
    }
    first_and_last_run_of_groups
}

impl RunGroups {
    /// The groups of the runs of a list, which the structure of its tree decides, with the 3D sorting contexts of the
    /// tree that sort their planes by depth, which the transforms of their planes decide.
    pub fn new<'a>(
        tree: &VisualContextTree,
        command_runs: impl IntoIterator<Item = &'a DisplayListCommandRun>,
    ) -> Self {
        Self::new_leaving_runs_of_effects_ungrouped(tree, command_runs, &[])
    }

    /// As `new`, but the runs of each effect that `effect_leaves_its_runs_ungrouped_by_effect_index` marks are grouped
    /// only as the effects inside it group them. No effect around a marked effect may group its runs.
    pub fn new_leaving_runs_of_effects_ungrouped<'a>(
        tree: &VisualContextTree,
        command_runs: impl IntoIterator<Item = &'a DisplayListCommandRun>,
        effect_leaves_its_runs_ungrouped_by_effect_index: &[bool],
    ) -> Self {
        let roots_of_contexts_sorting_planes_by_depth = tree.roots_of_contexts_sorting_planes_by_depth();
        let mut sorting_context_of_run = Vec::new();
        let outermost_grouping_effects =
            outermost_grouping_effects(tree, effect_leaves_its_runs_ungrouped_by_effect_index);
        let mut effect_chain_holds_mask = vec![false; tree.effect_nodes.len()];
        tree.visit_effect_nodes_parents_first(|index| {
            let node = &tree.effect_nodes[index];
            let parent_holds_mask = !node.parent.is_none() && effect_chain_holds_mask[node.parent.0 as usize];
            effect_chain_holds_mask[index] = parent_holds_mask || matches!(node.data, EffectNodeData::Mask(_));
        });
        let (grouping_effect_of_run, run_is_under_mask): (Vec<EffectNodeIndex>, Vec<bool>) = command_runs
            .into_iter()
            .map(|run| {
                sorting_context_of_run.push(
                    roots_of_contexts_sorting_planes_by_depth
                        .get(run.context.spatial.0 as usize)
                        .copied()
                        .flatten(),
                );
                if run.context.effect.is_none() {
                    (EffectNodeIndex::NONE, false)
                } else {
                    (
                        outermost_grouping_effects[run.context.effect.0 as usize],
                        effect_chain_holds_mask[run.context.effect.0 as usize],
                    )
                }
            })
            .unzip();
        let first_and_last_run_of_groups =
            first_and_last_run_of_groups_holding_whole_grouping_effects_and_sorting_contexts(
                &grouping_effect_of_run,
                &sorting_context_of_run,
            );
        let mut group_of_run = vec![0u32; grouping_effect_of_run.len()];
        for (group_index, &(first, last)) in first_and_last_run_of_groups.iter().enumerate() {
            group_of_run[first as usize..=last as usize].fill(group_index as u32);
        }

        let group_is_under_mask = first_and_last_run_of_groups
            .iter()
            .map(|&(first, last)| {
                run_is_under_mask[first as usize..=last as usize]
                    .iter()
                    .any(|&under_mask| under_mask)
            })
            .collect();
        let run_is_sorted_by_depth: Vec<bool> = sorting_context_of_run.iter().map(Option::is_some).collect();
        let group_is_sorted_by_depth = first_and_last_run_of_groups
            .iter()
            .map(|&(first, last)| {
                run_is_sorted_by_depth[first as usize..=last as usize]
                    .iter()
                    .any(|&sorted| sorted)
            })
            .collect();
        Self {
            first_and_last_run_of_groups,
            group_of_run,
            group_is_under_mask,
            roots_of_contexts_sorting_planes_by_depth,
            run_is_sorted_by_depth,
            group_is_sorted_by_depth,
        }
    }

    pub fn root_of_context_sorting_planes_by_depth(&self, spatial: SpatialNodeIndex) -> Option<SpatialNodeIndex> {
        self.roots_of_contexts_sorting_planes_by_depth
            .get(spatial.0 as usize)
            .copied()
            .flatten()
    }

    /// Whether the groups are those of a tree whose 3D sorting contexts sort the same content by depth.
    pub fn match_depth_sorting_roots(
        &self,
        roots_of_contexts_sorting_planes_by_depth: &[Option<SpatialNodeIndex>],
    ) -> bool {
        self.roots_of_contexts_sorting_planes_by_depth == roots_of_contexts_sorting_planes_by_depth
    }

    /// Whether the group holds a 3D sorting context that sorts its planes by depth, whose content is replayed whole
    /// wherever any of the group shows, as sorting the planes reads all of them.
    pub fn group_is_sorted_by_depth(&self, group: RunGroupIndex) -> bool {
        self.group_is_sorted_by_depth[group.0]
    }

    pub fn run_is_sorted_by_depth(&self, run_index: usize) -> bool {
        self.run_is_sorted_by_depth[run_index]
    }

    pub fn first_and_last_run_of_groups(&self) -> &[(u32, u32)] {
        &self.first_and_last_run_of_groups
    }

    pub fn group_of_run(&self, run_index: usize) -> RunGroupIndex {
        RunGroupIndex(self.group_of_run[run_index] as usize)
    }

    /// Whether the group is under a mask, whose content is replayed whole wherever any of it shows.
    pub fn group_is_under_mask(&self, group: RunGroupIndex) -> bool {
        self.group_is_under_mask[group.0]
    }
}
