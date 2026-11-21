/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The raster spaces that a plan places a list among, and the geometry of each, with what placing reads past what
//! rastering reads: the spatial nodes that move on their own relative to the spaces, the effects whose filters can
//! spread their content farther on their own, the bounds of the clip chains fixed in a space, and the extent of each
//! space.

use crate::display_list::commands::{ContextRef, EffectNodeIndex, SpatialNodeIndex};
use crate::display_list::effect_geometry::filter_may_spread_at_some_sample;
use crate::display_list::layers::anchors_of_layer_plans;
use crate::display_list::placement::{
    FiltersAndClipsOfList, RasterSpace, RasterSpaceAnchorKind, RasterSpaceGeometry, RasterSpaceIndex, RasterSpaces,
    WhatEverySpaceOfTreeShares, antialiased_reach_in_device_pixels, chain_bounds_of_clip, compositor_scroll_record_of,
    enclosing_int_rect_within_extent_limit, extent_limit_rect, intersect_bounds, is_finite_rect, raster_scroll_offsets,
    space_is_ancestor_or_self, spatial_nodes_that_move_on_their_own_relative_to_spaces_at,
    spatial_nodes_whose_scroll_offset_placement_leaves_out_of_nodes_moving, translation_if_only_translates,
};
use crate::display_list::reach::Reach;
use crate::host::FfiVisualAnimationTargetKind;
use crate::visual_context::{VisualContextTree, WorldTransforms};
use libgfx_rust::{FloatMatrix4x4, FloatPoint, FloatRect, IntRect};
use std::rc::Rc;
use std::sync::Arc;

const ANCHOR_TRANSLATION_ROUNDING_OVERSHOOT_IN_DEVICE_PIXELS: i32 = 1;

/// The rect that content within `rect` covers wherever a translation between `lowest` and `highest` takes it, with the
/// rounding of that translation to whole device pixels.
pub(super) fn rect_swept_over_translations(rect: IntRect, lowest: FloatPoint, highest: FloatPoint) -> IntRect {
    if rect.is_empty() {
        return rect;
    }
    let swept = FloatRect::new(
        rect.x as f32 + lowest.x,
        rect.y as f32 + lowest.y,
        rect.width as f32 + highest.x - lowest.x,
        rect.height as f32 + highest.y - lowest.y,
    );
    if !is_finite_rect(swept) {
        return enclosing_int_rect_within_extent_limit(extent_limit_rect());
    }
    let margin = ANCHOR_TRANSLATION_ROUNDING_OVERSHOOT_IN_DEVICE_PIXELS;
    enclosing_int_rect_within_extent_limit(swept).inflated_edges(margin, margin, margin, margin)
}

/// Per effect node, whether how far its filters spread its content can change while the list and the tree's structure
/// stay, as the compositor animates the filter of it or of an effect above it, and the filter spreads at some sample.
fn effects_whose_spread_moves_on_its_own(tree: &VisualContextTree) -> Vec<bool> {
    let mut spread_moves = vec![false; tree.effect_nodes.len()];
    if !tree
        .visual_animations()
        .iter()
        .any(|animation| animation.target_kind == FfiVisualAnimationTargetKind::Filter)
    {
        return spread_moves;
    }
    tree.visit_effect_nodes_parents_first(|index| {
        let node = &tree.effect_nodes[index];
        let effect = EffectNodeIndex(index as u32);
        let spread_of_parent_moves = !node.parent.is_none() && spread_moves[node.parent.0 as usize];
        spread_moves[index] = spread_of_parent_moves
            || (tree.effect_filter_is_animated(effect) && filter_may_spread_at_some_sample(tree, effect));
    });
    spread_moves
}

fn extent_of_space(
    tree: &VisualContextTree,
    spaces: &[RasterSpace],
    space: RasterSpaceIndex,
    world_with_anchors_of_space_at_origin: &WorldTransforms,
    viewport_rect: IntRect,
) -> IntRect {
    let world = world_with_anchors_of_space_at_origin;
    let unbounded = enclosing_int_rect_within_extent_limit(extent_limit_rect());
    let space = &spaces[space.0];
    if let RasterSpaceAnchorKind::AnimatedTransform { .. } = space.anchor_kind {
        return unbounded;
    }
    if let RasterSpaceAnchorKind::StickyBox { .. } = space.anchor_kind {
        let extent_of_parent = if space.parent_index == RasterSpaceIndex::VIEWPORT {
            viewport_rect
        } else {
            extent_of_space(tree, spaces, space.parent_index, world, viewport_rect)
        };
        let negated = |point: FloatPoint| FloatPoint {
            x: -point.x,
            y: -point.y,
        };
        let extent_of_parent_translated_back_by_every_box_offset = rect_swept_over_translations(
            extent_of_parent,
            negated(space.highest_anchor_translation),
            negated(space.lowest_anchor_translation),
        );
        return extent_of_parent_translated_back_by_every_box_offset;
    }
    let Some(record) = compositor_scroll_record_of(tree, space.anchor) else {
        return unbounded;
    };
    let Some(scrollport_offset) =
        translation_if_only_translates(&world.draw_matrix(tree.spatial_nodes[space.anchor.0 as usize].parent))
    else {
        return unbounded;
    };
    let scrollport = record
        .scrollport_rect
        .to_float()
        .translated(scrollport_offset.x, scrollport_offset.y);
    let (minimum, maximum) = (record.min_scroll_offset, record.max_scroll_offset);
    let extent = FloatRect::new(
        scrollport.x + minimum.x.min(0.0),
        scrollport.y + minimum.y.min(0.0),
        scrollport.width + (maximum.x.max(0.0) - minimum.x.min(0.0)),
        scrollport.height + (maximum.y.max(0.0) - minimum.y.min(0.0)),
    );
    if !is_finite_rect(extent) {
        return unbounded;
    }
    enclosing_int_rect_within_extent_limit(extent)
}

/// What plans of layers work out of a tree alone, once for every plan under the tree: the anchors of the raster spaces
/// it offers, and per spatial node, whether the node moves on its own relative to those spaces, and whether a plan
/// leaves out the scroll offset of the node.
#[derive(Debug)]
pub(super) struct RasterSpaceAnchorsOfTree {
    pub(super) anchors: Vec<SpatialNodeIndex>,
    spatial_node_moves_on_its_own: Arc<[bool]>,
    pub(super) leaves_out_scroll_offset_of_node: Vec<bool>,
}

impl RasterSpaceAnchorsOfTree {
    pub(super) fn of(tree: &VisualContextTree) -> Self {
        let anchors = anchors_of_layer_plans(tree);
        let spatial_node_moves_on_its_own =
            spatial_nodes_that_move_on_their_own_relative_to_spaces_at(tree, Some(&anchors));
        Self {
            leaves_out_scroll_offset_of_node: spatial_nodes_whose_scroll_offset_placement_leaves_out_of_nodes_moving(
                tree,
                &anchors,
                spatial_node_moves_on_its_own.clone(),
            ),
            anchors,
            spatial_node_moves_on_its_own: spatial_node_moves_on_its_own.into(),
        }
    }
}

/// The raster spaces of a tree at a set of anchors, with what placing a list among them reads past what rastering
/// reads: per spatial node, whether it moves on its own relative to the spaces, and per effect node, whether how far
/// its filters spread its content moves on its own.
#[derive(Debug)]
pub(super) struct RasterSpacesForPlacing {
    raster_spaces: RasterSpaces,
    spatial_node_moves_on_its_own: Arc<[bool]>,
    effect_node_spread_moves_on_its_own: Vec<bool>,
}

impl RasterSpacesForPlacing {
    /// The spaces at the anchors of `tree`, without the extent of each but the viewport's, which placing reads of the
    /// geometry of each space.
    fn at_anchors(
        tree: &VisualContextTree,
        anchors_of_tree: &RasterSpaceAnchorsOfTree,
        viewport_rect: IntRect,
    ) -> Self {
        let raster_spaces = RasterSpaces::of_spaces(
            tree,
            RasterSpaces::spaces_at_anchors(tree, &anchors_of_tree.anchors, viewport_rect),
        );
        debug_assert!(
            raster_spaces
                .spaces()
                .iter()
                .skip(1)
                .map(|space| space.anchor)
                .eq(anchors_of_tree.anchors.iter().copied()),
            "every anchor of a tree has a space"
        );
        Self {
            spatial_node_moves_on_its_own: anchors_of_tree.spatial_node_moves_on_its_own.clone(),
            effect_node_spread_moves_on_its_own: effects_whose_spread_moves_on_its_own(tree),
            raster_spaces,
        }
    }

    pub(super) fn spaces(&self) -> &[RasterSpace] {
        self.raster_spaces.spaces()
    }

    pub(super) fn deepest_space_of(&self, spatial: SpatialNodeIndex) -> RasterSpaceIndex {
        self.raster_spaces.deepest_space_of(spatial)
    }

    pub(super) fn moves_with(&self, spatial: SpatialNodeIndex, space: RasterSpaceIndex) -> bool {
        space_is_ancestor_or_self(self.spaces(), space, self.deepest_space_of(spatial))
    }

    pub(super) fn spatial_moves_on_its_own(&self, spatial: SpatialNodeIndex) -> bool {
        self.spatial_node_moves_on_its_own
            .get(spatial.0 as usize)
            .copied()
            .unwrap_or(false)
    }

    pub(super) fn effect_spread_moves_on_its_own(&self, effect: EffectNodeIndex) -> bool {
        !effect.is_none()
            && self
                .effect_node_spread_moves_on_its_own
                .get(effect.0 as usize)
                .copied()
                .unwrap_or(false)
    }
}

/// The geometry of one raster space, with what placing a list in it reads past what rastering reads: the bounds of
/// the clip chains fixed in the space, and the extent of the space.
pub(super) struct RasterSpaceGeometryForPlacing {
    pub(super) raster: RasterSpaceGeometry,
    clip_chain_bounds_fixed_in_space_by_clip: Vec<Option<FloatRect>>,
    pub(super) extent: IntRect,
}

impl RasterSpaceGeometryForPlacing {
    fn new(
        tree: &VisualContextTree,
        spaces: &RasterSpacesForPlacing,
        space: RasterSpaceIndex,
        scroll_offsets: &[FloatPoint],
        viewport_rect: IntRect,
        what_every_space_of_tree_shares: &Rc<WhatEverySpaceOfTreeShares>,
        world_of_viewport_space: Option<&Rc<WorldTransforms>>,
    ) -> Self {
        let raster = match world_of_viewport_space {
            Some(world) if spaces.spaces()[space.0].raster_root().is_none() => RasterSpaceGeometry::with_world(
                tree,
                &spaces.raster_spaces,
                space,
                raster_scroll_offsets(spaces.spaces(), space, scroll_offsets),
                world.clone(),
                what_every_space_of_tree_shares,
            ),
            _ => RasterSpaceGeometry::new(
                tree,
                &spaces.raster_spaces,
                space,
                scroll_offsets,
                FloatMatrix4x4::identity(),
                what_every_space_of_tree_shares,
            ),
        };
        let mut clip_chain_bounds_fixed_in_space_by_clip: Vec<Option<FloatRect>> = vec![None; tree.clip_nodes.len()];
        tree.visit_clip_nodes_parents_first(|index| {
            let node = &tree.clip_nodes[index];
            let own_bounds_if_fixed_in_space = raster.rastered_affine_own_clip_bounds_by_clip[index].filter(|_| {
                spaces.deepest_space_of(node.spatial) == space && !spaces.spatial_moves_on_its_own(node.spatial)
            });
            clip_chain_bounds_fixed_in_space_by_clip[index] = intersect_bounds(
                own_bounds_if_fixed_in_space,
                chain_bounds_of_clip(node.parent, &clip_chain_bounds_fixed_in_space_by_clip),
            );
        });
        let extent = if space == RasterSpaceIndex::VIEWPORT {
            viewport_rect
        } else {
            extent_of_space(tree, spaces.spaces(), space, &raster.world, viewport_rect)
        };
        Self {
            raster,
            clip_chain_bounds_fixed_in_space_by_clip,
            extent,
        }
    }

    /// As `RasterSpaceGeometry::reach_of_content`, or without `drawn_within`, where content of `context` can show
    /// anywhere it can move: within the clips fixed in the space, which are some of those that rasters of the space
    /// apply.
    pub(super) fn reach_of_content(
        &self,
        list: &FiltersAndClipsOfList<'_>,
        drawn_within: Option<FloatRect>,
        context: ContextRef,
    ) -> Reach<IntRect> {
        match drawn_within {
            Some(drawn_within) => self.raster.reach_of_content(list, drawn_within, context),
            None => antialiased_reach_in_device_pixels(chain_bounds_of_clip(
                list.clip_bounding_filter_spread(context),
                &self.clip_chain_bounds_fixed_in_space_by_clip,
            )),
        }
    }

    pub(super) fn part_of_extent_reached_by(&self, reach: Reach<IntRect>) -> IntRect {
        reach.part_of(self.extent)
    }
}

/// The spaces at the anchors of `tree` and the geometry of each, in the device pixels of the viewport.
pub(super) fn spaces_with_geometries(
    tree: &VisualContextTree,
    anchors_of_tree: &RasterSpaceAnchorsOfTree,
    scroll_offsets: &[FloatPoint],
    viewport_rect: IntRect,
) -> (RasterSpacesForPlacing, Vec<RasterSpaceGeometryForPlacing>) {
    let spaces = RasterSpacesForPlacing::at_anchors(tree, anchors_of_tree, viewport_rect);
    let what_every_space_of_tree_shares = WhatEverySpaceOfTreeShares::of(tree);
    let geometry_of_viewport_space = RasterSpaceGeometryForPlacing::new(
        tree,
        &spaces,
        RasterSpaceIndex::VIEWPORT,
        scroll_offsets,
        viewport_rect,
        &what_every_space_of_tree_shares,
        None,
    );
    // The scroll offsets a plan reads leave out that of every anchor, so every space whose anchor the compositor does
    // not animate has the world transforms of the viewport's space.
    let world_of_viewport_space = geometry_of_viewport_space.raster.world.clone();
    let geometries: Vec<RasterSpaceGeometryForPlacing> = std::iter::once(geometry_of_viewport_space)
        .chain((1..spaces.spaces().len()).map(|space| {
            RasterSpaceGeometryForPlacing::new(
                tree,
                &spaces,
                RasterSpaceIndex(space),
                scroll_offsets,
                viewport_rect,
                &what_every_space_of_tree_shares,
                Some(&world_of_viewport_space),
            )
        }))
        .collect();
    (spaces, geometries)
}
