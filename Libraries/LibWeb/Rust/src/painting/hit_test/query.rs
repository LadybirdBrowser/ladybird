/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::layout::LayoutNodeArena;
use crate::painting::host::FfiHitTestQueryCallbacks;
use crate::painting::paintable_arena::PaintableArena;

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
        paintables: &PaintableArena,
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
                if !paintables.is_live(item.paintable) {
                    return false;
                }
                let node = paintables.data_ref(item.paintable).layout_node;
                callbacks.chrome_widget_contains(layout_arena.shell_if_live(node), item.chrome_widget_kind, local_point)
            }
        }
    }

    fn find_topmost_item_in_list(
        &self,
        layout_arena: &LayoutNodeArena,
        paintables: &PaintableArena,
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
            if !self.item_contains(layout_arena, paintables, callbacks, item, local) {
                continue;
            }
            *search.index = Some(*item_index);
            return;
        }
    }

    fn candidate_item_lists(&self, visual_context_index: usize, local_point: CssPixelPoint) -> [Option<&[usize]>; 2] {
        let spatial_index = self.spatial_indexes[visual_context_index]
            .as_ref()
            .expect("used visual context without spatial index");
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
        paintables: &PaintableArena,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
        with_caret_item: bool,
    ) -> (Option<TopmostItem>, Option<TopmostItem>) {
        debug_assert!(self.derived_structures_built);
        let mut topmost_hit: Option<TopmostItem> = None;
        let mut topmost_caret: Option<TopmostItem> = None;
        let mut topmost_hit_index: Option<usize> = None;
        let mut topmost_caret_index: Option<usize> = None;
        for context_position in 0..self.used_visual_context_indices.len() {
            let visual_context_index = self.used_visual_context_indices[context_position];
            let Some(local) = local_float_point(callbacks, visual_context_index, point, true) else {
                continue;
            };
            let local_point = to_css_point(local);

            let previous_hit = topmost_hit_index;
            let previous_caret = topmost_caret_index;
            for candidate_list in self
                .candidate_item_lists(visual_context_index, local_point)
                .into_iter()
                .flatten()
            {
                self.find_topmost_item_in_list(
                    layout_arena,
                    paintables,
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
                        paintables,
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
    fn depth_sort_key(
        &self,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
        item_index: usize,
    ) -> (Option<i64>, usize) {
        (
            callbacks.plane_depth_key(self.items[item_index].visual_context_index, point),
            item_index,
        )
    }

    fn topmost_item_by_plane_depth(
        &self,
        layout_arena: &LayoutNodeArena,
        paintables: &PaintableArena,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
        topmost: TopmostItem,
    ) -> TopmostItem {
        let Some(group) = callbacks.sorting_context_group(self.items[topmost.index].visual_context_index) else {
            return topmost;
        };
        let mut winner = topmost;
        for &visual_context_index in &self.used_visual_context_indices {
            if callbacks.sorting_context_group(visual_context_index) != Some(group) {
                continue;
            }
            let Some(local) = local_float_point(callbacks, visual_context_index, point, true) else {
                continue;
            };
            let local_point = to_css_point(local);
            for candidate_list in self
                .candidate_item_lists(visual_context_index, local_point)
                .into_iter()
                .flatten()
            {
                for item_index in candidate_list {
                    if *item_index == winner.index
                        || !self.item_contains(layout_arena, paintables, callbacks, &self.items[*item_index], local)
                    {
                        continue;
                    }
                    if self.depth_sort_key(callbacks, point, *item_index)
                        > self.depth_sort_key(callbacks, point, winner.index)
                    {
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
        paintables: &PaintableArena,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
    ) -> Option<TopmostItem> {
        let topmost = self.find_topmost(layout_arena, paintables, callbacks, point, false).0?;
        // Record order misranks content inside a 3D rendering context, whose planes paint depth sorted. A winner
        // on such a plane is re-resolved against every hit plane of its outermost context. Content outside the
        // context keeps record order, which stays correct because a context's items are recorded contiguously.
        Some(self.topmost_item_by_plane_depth(layout_arena, paintables, callbacks, point, topmost))
    }

    pub(crate) fn find_topmost_items_for_caret(
        &self,
        layout_arena: &LayoutNodeArena,
        paintables: &PaintableArena,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
    ) -> (Option<TopmostItem>, Option<TopmostItem>) {
        let (hit, caret) = self.find_topmost(layout_arena, paintables, callbacks, point, true);
        (caret, hit)
    }

    pub(crate) fn hit_test_all(
        &self,
        layout_arena: &LayoutNodeArena,
        paintables: &PaintableArena,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
    ) -> Vec<usize> {
        debug_assert!(self.derived_structures_built);
        let mut hit_item_indices: Vec<usize> = Vec::new();
        for context_position in 0..self.used_visual_context_indices.len() {
            let visual_context_index = self.used_visual_context_indices[context_position];
            let Some(local) = local_float_point(callbacks, visual_context_index, point, true) else {
                continue;
            };
            let local_point = to_css_point(local);
            for candidate_list in self
                .candidate_item_lists(visual_context_index, local_point)
                .into_iter()
                .flatten()
            {
                for item_index in candidate_list {
                    if self.item_contains(layout_arena, paintables, callbacks, &self.items[*item_index], local) {
                        hit_item_indices.push(*item_index);
                    }
                }
            }
        }
        hit_item_indices.sort_by(|a, b| b.cmp(a));
        hit_item_indices.dedup();
        self.sort_hits_within_sorting_contexts(callbacks, point, &mut hit_item_indices);
        hit_item_indices
    }

    // Runs of hits on the planes of one 3D rendering context are reordered front to back. A context's items
    // are recorded contiguously, so after the record-order sort its hits sit adjacent.
    fn sort_hits_within_sorting_contexts(
        &self,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
        hit_item_indices: &mut [usize],
    ) {
        let group_of = |item_index: usize| callbacks.sorting_context_group(self.items[item_index].visual_context_index);
        let mut run_begin = 0;
        while run_begin < hit_item_indices.len() {
            let group = group_of(hit_item_indices[run_begin]);
            let mut run_end = run_begin + 1;
            if group.is_some() {
                while run_end < hit_item_indices.len() && group_of(hit_item_indices[run_end]) == group {
                    run_end += 1;
                }
                hit_item_indices[run_begin..run_end]
                    .sort_by_key(|item_index| std::cmp::Reverse(self.depth_sort_key(callbacks, point, *item_index)));
            }
            run_begin = run_end;
        }
    }
}
