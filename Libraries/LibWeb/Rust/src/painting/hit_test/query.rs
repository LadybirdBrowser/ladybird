/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::painting::chrome_geometry::{ChromeGeometry, scrollbar_is_hidden};
use crate::painting::ffi::ScrollDirection;
use crate::painting::host::FfiHitTestQueryCallbacks;
use crate::painting::paint_read::PaintRead;
use crate::painting::visual_context::{
    NO_SORTING_CONTEXT, SortingContexts, SpatialNodeIndex, VisualContextTree, WorldTransforms,
};
use libgfx_rust::FloatPoint;
use std::collections::HashMap;

pub(super) struct DepthSortingState<'a> {
    tree: &'a VisualContextTree,
    point: CssPixelPoint,
    device_pixels_per_css_pixel: f32,
    world_transforms: &'a WorldTransforms,
    sorting_contexts: Option<SortingContexts>,
    depth_key_by_plane: HashMap<u32, Option<i64>>,
}

impl<'a> DepthSortingState<'a> {
    pub(super) fn new(
        callbacks: &'a FfiHitTestQueryCallbacks,
        tree: &'a VisualContextTree,
        point: CssPixelPoint,
    ) -> Self {
        Self {
            tree,
            point,
            device_pixels_per_css_pixel: callbacks.device_pixels_per_css_pixel as f32,
            world_transforms: callbacks.world_transforms(),
            sorting_contexts: None,
            depth_key_by_plane: HashMap::new(),
        }
    }

    fn sorting_contexts(&mut self) -> &SortingContexts {
        self.sorting_contexts
            .get_or_insert_with(|| self.tree.resolve_sorting_contexts())
    }

    pub(super) fn group_for(&mut self, spatial: SpatialNodeIndex) -> Option<u32> {
        let contexts = self.sorting_contexts();
        if contexts.is_empty() || contexts.leaf_by_node[spatial.0 as usize] == NO_SORTING_CONTEXT {
            None
        } else {
            Some(
                contexts
                    .outermost_context_of(contexts.context_by_node[spatial.0 as usize])
                    .0,
            )
        }
    }

    pub(super) fn depth_key_for(&mut self, spatial: SpatialNodeIndex) -> Option<i64> {
        let contexts = self.sorting_contexts();
        if contexts.is_empty() {
            return None;
        }
        let leaf = contexts.leaf_by_node[spatial.0 as usize];
        if leaf == NO_SORTING_CONTEXT {
            return None;
        }
        if let Some(depth_key) = self.depth_key_by_plane.get(&leaf.0) {
            return *depth_key;
        }

        let screen_point = libgfx_rust::FloatPoint {
            x: self.point.x.to_float() * self.device_pixels_per_css_pixel,
            y: self.point.y.to_float() * self.device_pixels_per_css_pixel,
        };
        let depth = self
            .world_transforms
            .plane_depth_at_point_for_hit_test(leaf, screen_point);
        const DEPTH_LIMIT: f32 = 16777216.0;
        let depth_key = depth.map(|depth| (depth.clamp(-DEPTH_LIMIT, DEPTH_LIMIT) * 8.0).round() as i64);
        self.depth_key_by_plane.insert(leaf.0, depth_key);
        depth_key
    }
}

pub(super) fn item_contains_point(
    layout_arena: &impl PaintRead,
    callbacks: &FfiHitTestQueryCallbacks,
    item: &HitTestItem,
    local: FloatPoint,
) -> bool {
    let local_point = to_css_point(local);
    match item.kind {
        HitTestItemKind::Box => {
            item.rect.contains_point(local_point) && item.border_radii.contains(local_point, item.rect)
        }
        HitTestItemKind::SvgPath => {
            // SVG paths test in float: their local units can map to many device pixels each.
            let rect = item.rect;
            let FloatPoint { x, y } = local;
            let in_rect = x >= rect.left().to_float()
                && x < rect.right().to_float()
                && y >= rect.top().to_float()
                && y < rect.bottom().to_float();
            if !in_rect {
                return false;
            }
            let Some(path) = item.path.as_ref() else {
                return false;
            };
            (item.svg_hit_areas & svg_hit_area::FILL != 0 && path.contains(x, y, item.winding_rule))
                || (item.svg_hit_areas & svg_hit_area::STROKE != 0
                    && item
                        .svg_stroke
                        .as_ref()
                        .is_some_and(|stroke| stroke.contains(path, x, y)))
        }
        HitTestItemKind::TextFragment => item.rect.contains_point(local_point),
        HitTestItemKind::EmptyEditable => item.rect.contains_point(local_point),
        HitTestItemKind::ChromeWidget => {
            if !layout_arena.paintable_row_is_populated(item.paintable) {
                return false;
            }
            assert!(
                callbacks.has_chrome_metrics,
                "chrome widget hit testing needs the chrome metrics"
            );
            let node = item.paintable;
            let rows = layout_arena;
            let chrome_geometry = ChromeGeometry::for_hit_test_query(rows, callbacks);
            match item.chrome_widget_kind {
                1 => chrome_geometry.resizer_contains(node, local_point),
                2 | 3 => {
                    let direction = if item.chrome_widget_kind == 2 {
                        ScrollDirection::Horizontal
                    } else {
                        ScrollDirection::Vertical
                    };
                    // An overlay scrollbar that has faded out takes no pointer input, as in Chromium. The
                    // compositor grows a hovered scrollbar, which is hit tested here as it is before it grows.
                    !scrollbar_is_hidden(rows, node, direction)
                        && chrome_geometry
                            .absolute_scrollbar_rect(node, direction, false)
                            .is_some_and(|rect| rect.contains_point(local_point))
                }
                _ => false,
            }
        }
    }
}
