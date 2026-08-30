/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::layout::LayoutNodeArena;
use crate::painting::chrome_geometry::{ChromeGeometry, scrollbar_is_enlarged};
use crate::painting::ffi::ScrollDirection;
use crate::painting::host::FfiHitTestQueryCallbacks;
use crate::painting::visual_context::{NO_SORTING_CONTEXT, SortingContexts, SpatialNodeIndex, VisualContextTree};

struct DepthSortingState<'a> {
    tree: &'a VisualContextTree,
    point: CssPixelPoint,
    device_pixels_per_css_pixel: f32,
    scroll_offsets: &'a [libgfx_rust::FloatPoint],
    sorting_contexts: Option<SortingContexts>,
    depth_key_by_plane: HashMap<u32, Option<i64>>,
}

impl<'a> DepthSortingState<'a> {
    fn new(callbacks: &'a FfiHitTestQueryCallbacks, tree: &'a VisualContextTree, point: CssPixelPoint) -> Self {
        Self {
            tree,
            point,
            device_pixels_per_css_pixel: callbacks.device_pixels_per_css_pixel as f32,
            scroll_offsets: callbacks.scroll_offsets(),
            sorting_contexts: None,
            depth_key_by_plane: HashMap::new(),
        }
    }

    fn sorting_contexts(&mut self) -> &SortingContexts {
        self.sorting_contexts
            .get_or_insert_with(|| self.tree.resolve_sorting_contexts())
    }

    fn group_for(&mut self, spatial: SpatialNodeIndex) -> Option<u32> {
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

    fn depth_key_for(&mut self, spatial: SpatialNodeIndex) -> Option<i64> {
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
            .tree
            .plane_depth_at_point_for_hit_test(leaf, screen_point, self.scroll_offsets);
        const DEPTH_LIMIT: f32 = 16777216.0;
        let depth_key = depth.map(|depth| (depth.clamp(-DEPTH_LIMIT, DEPTH_LIMIT) * 8.0).round() as i64);
        self.depth_key_by_plane.insert(leaf.0, depth_key);
        depth_key
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TopmostItem {
    pub index: usize,
    pub local_point: CssPixelPoint,
}

struct TopmostSearch<'a> {
    index: &'a mut Option<usize>,
    caret_capable_only: bool,
}

impl HitTestList {
    fn item_contains(
        &self,
        layout_arena: &LayoutNodeArena,
        callbacks: &FfiHitTestQueryCallbacks,
        item: &HitTestItem,
        local: (f32, f32),
    ) -> bool {
        let local_point = to_css_point(local);
        match item.kind {
            HitTestItemKind::Box => {
                item.rect.contains_point(local_point) && item.border_radii.contains(local_point, item.rect)
            }
            HitTestItemKind::SvgPath => {
                // SVG paths test in float: their local units can map to many device pixels each.
                let rect = item.rect;
                let (x, y) = local;
                let in_rect = x >= rect.left().to_float()
                    && x < rect.right().to_float()
                    && y >= rect.top().to_float()
                    && y < rect.bottom().to_float();
                in_rect
                    && item
                        .path
                        .as_ref()
                        .is_some_and(|path| path.contains(x, y, item.winding_rule))
            }
            HitTestItemKind::TextFragment => item.rect.contains_point(local_point),
            // Empty lines are caret targets only; they never contain a point for regular hit testing.
            HitTestItemKind::EmptyLine => false,
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
                let rows = layout_arena.paintable_rows();
                let chrome_geometry = ChromeGeometry::for_hit_test_query(&rows, callbacks);
                match item.chrome_widget_kind {
                    1 => chrome_geometry.resizer_contains(node, local_point),
                    2 | 3 => {
                        let direction = if item.chrome_widget_kind == 2 {
                            ScrollDirection::Horizontal
                        } else {
                            ScrollDirection::Vertical
                        };
                        chrome_geometry
                            .absolute_scrollbar_rect(node, direction, scrollbar_is_enlarged(&rows, node, direction))
                            .is_some_and(|rect| rect.contains_point(local_point))
                    }
                    _ => false,
                }
            }
        }
    }

    fn find_topmost_item_in_list(
        &self,
        layout_arena: &LayoutNodeArena,
        callbacks: &FfiHitTestQueryCallbacks,
        item_indices: &[usize],
        local: (f32, f32),
        search: TopmostSearch<'_>,
    ) {
        for item_index in item_indices.iter().rev() {
            if search.index.is_some_and(|topmost| *item_index <= topmost) {
                return;
            }
            let item = &self.items[*item_index];
            if search.caret_capable_only && !item.can_produce_caret_position {
                continue;
            }
            if !self.item_contains(layout_arena, callbacks, item, local) {
                continue;
            }
            *search.index = Some(*item_index);
            return;
        }
    }

    fn candidate_item_lists(spatial_index: &SpatialIndex, local_point: CssPixelPoint) -> [Option<&[usize]>; 2] {
        let x = spatial_index_cell_for(local_point.x);
        let y = spatial_index_cell_for(local_point.y);
        let bucket = spatial_index.cells.get(&spatial_index_cell_key(x, y));
        [
            Some(spatial_index.unbucketed_items.as_slice()),
            bucket.map(|bucket| bucket.as_slice()),
        ]
    }

    fn find_topmost(
        &self,
        layout_arena: &LayoutNodeArena,
        visual_context_tree: &VisualContextTree,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
        with_caret_item: bool,
    ) -> (Option<TopmostItem>, Option<TopmostItem>) {
        debug_assert!(self.derived_structures_built);
        let mut topmost_hit: Option<TopmostItem> = None;
        let mut topmost_caret: Option<TopmostItem> = None;
        let mut topmost_hit_index: Option<usize> = None;
        let mut topmost_caret_index: Option<usize> = None;
        for (context, spatial_index) in &self.spatial_indexes_by_context {
            let Some(local) = local_float_point(visual_context_tree, callbacks, *context, point, true) else {
                continue;
            };
            let local_point = to_css_point(local);

            let previous_hit = topmost_hit_index;
            let previous_caret = topmost_caret_index;
            for candidate_list in Self::candidate_item_lists(spatial_index, local_point)
                .into_iter()
                .flatten()
            {
                self.find_topmost_item_in_list(
                    layout_arena,
                    callbacks,
                    candidate_list,
                    local,
                    TopmostSearch {
                        index: &mut topmost_hit_index,
                        caret_capable_only: false,
                    },
                );
                if with_caret_item {
                    self.find_topmost_item_in_list(
                        layout_arena,
                        callbacks,
                        candidate_list,
                        local,
                        TopmostSearch {
                            index: &mut topmost_caret_index,
                            caret_capable_only: true,
                        },
                    );
                }
            }
            if topmost_hit_index != previous_hit {
                topmost_hit = topmost_hit_index.map(|index| TopmostItem { index, local_point });
            }
            if topmost_caret_index != previous_caret {
                topmost_caret = topmost_caret_index.map(|index| TopmostItem { index, local_point });
            }
        }
        (topmost_hit, topmost_caret)
    }

    // Planes of a 3D rendering context paint depth sorted, so the front-most plane at the point wins over a
    // plane recorded later. Coplanar planes compare equal and fall back to record order.
    fn depth_sort_key(&self, depth_sorting: &mut DepthSortingState<'_>, item_index: usize) -> (Option<i64>, usize) {
        (
            depth_sorting.depth_key_for(self.items[item_index].context.spatial),
            item_index,
        )
    }

    fn topmost_item_by_plane_depth(
        &self,
        layout_arena: &LayoutNodeArena,
        visual_context_tree: &VisualContextTree,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
        topmost: TopmostItem,
    ) -> TopmostItem {
        let mut depth_sorting = DepthSortingState::new(callbacks, visual_context_tree, point);
        let Some(group) = depth_sorting.group_for(self.items[topmost.index].context.spatial) else {
            return topmost;
        };
        let mut winner = topmost;
        for (context, spatial_index) in &self.spatial_indexes_by_context {
            if depth_sorting.group_for(context.spatial) != Some(group) {
                continue;
            }
            let Some(local) = local_float_point(visual_context_tree, callbacks, *context, point, true) else {
                continue;
            };
            let local_point = to_css_point(local);
            for candidate_list in Self::candidate_item_lists(spatial_index, local_point)
                .into_iter()
                .flatten()
            {
                for item_index in candidate_list {
                    if *item_index == winner.index
                        || !self.item_contains(layout_arena, callbacks, &self.items[*item_index], local)
                    {
                        continue;
                    }
                    let candidate_key = self.depth_sort_key(&mut depth_sorting, *item_index);
                    let winner_key = self.depth_sort_key(&mut depth_sorting, winner.index);
                    if candidate_key > winner_key {
                        winner = TopmostItem {
                            index: *item_index,
                            local_point,
                        };
                    }
                }
            }
        }
        winner
    }

    pub(crate) fn find_topmost_item(
        &self,
        layout_arena: &LayoutNodeArena,
        visual_context_tree: &VisualContextTree,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
    ) -> Option<TopmostItem> {
        let topmost = self
            .find_topmost(layout_arena, visual_context_tree, callbacks, point, false)
            .0?;
        // Record order misranks content inside a 3D rendering context, whose planes paint depth sorted. A winner
        // on such a plane is re-resolved against every hit plane of its outermost context. Content outside the
        // context keeps record order, which stays correct because a context's items are recorded contiguously.
        Some(self.topmost_item_by_plane_depth(layout_arena, visual_context_tree, callbacks, point, topmost))
    }

    pub(crate) fn find_topmost_items_for_caret(
        &self,
        layout_arena: &LayoutNodeArena,
        visual_context_tree: &VisualContextTree,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
    ) -> (Option<TopmostItem>, Option<TopmostItem>) {
        let (hit, caret) = self.find_topmost(layout_arena, visual_context_tree, callbacks, point, true);
        (caret, hit)
    }

    pub(crate) fn hit_test_all(
        &self,
        layout_arena: &LayoutNodeArena,
        visual_context_tree: &VisualContextTree,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
    ) -> Vec<usize> {
        debug_assert!(self.derived_structures_built);
        let mut hit_item_indices: Vec<usize> = Vec::new();
        for (context, spatial_index) in &self.spatial_indexes_by_context {
            let Some(local) = local_float_point(visual_context_tree, callbacks, *context, point, true) else {
                continue;
            };
            let local_point = to_css_point(local);
            for candidate_list in Self::candidate_item_lists(spatial_index, local_point)
                .into_iter()
                .flatten()
            {
                for item_index in candidate_list {
                    if self.item_contains(layout_arena, callbacks, &self.items[*item_index], local) {
                        hit_item_indices.push(*item_index);
                    }
                }
            }
        }
        hit_item_indices.sort_by(|a, b| b.cmp(a));
        hit_item_indices.dedup();
        self.sort_hits_within_sorting_contexts(visual_context_tree, callbacks, point, &mut hit_item_indices);
        hit_item_indices
    }

    // Runs of hits on the planes of one 3D rendering context are reordered front to back. A context's items
    // are recorded contiguously, so after the record-order sort its hits sit adjacent.
    fn sort_hits_within_sorting_contexts(
        &self,
        visual_context_tree: &VisualContextTree,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
        hit_item_indices: &mut [usize],
    ) {
        let mut depth_sorting = DepthSortingState::new(callbacks, visual_context_tree, point);
        let mut run_begin = 0;
        while run_begin < hit_item_indices.len() {
            let group = depth_sorting.group_for(self.items[hit_item_indices[run_begin]].context.spatial);
            let mut run_end = run_begin + 1;
            if group.is_some() {
                while run_end < hit_item_indices.len()
                    && depth_sorting.group_for(self.items[hit_item_indices[run_end]].context.spatial) == group
                {
                    run_end += 1;
                }
                hit_item_indices[run_begin..run_end]
                    .sort_by_key(|item_index| std::cmp::Reverse(self.depth_sort_key(&mut depth_sorting, *item_index)));
            }
            run_begin = run_end;
        }
    }
}
