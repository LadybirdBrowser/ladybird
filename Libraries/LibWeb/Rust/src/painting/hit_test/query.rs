/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::painting::host::FfiHitTestQueryCallbacks;
use crate::painting::paintable_arena::PaintableArena;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TopmostItem {
    pub index: usize,
    pub local_point: CssPixelPoint,
}

impl HitTestList {
    fn item_contains(
        &self,
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
                callbacks.chrome_widget_contains(
                    paintables.data_ref(item.paintable).shell,
                    item.chrome_widget_kind,
                    local_point,
                )
            }
        }
    }

    fn find_topmost_item_in_list(
        &self,
        paintables: &PaintableArena,
        callbacks: &FfiHitTestQueryCallbacks,
        item_indices: &[usize],
        local: (f32, f32),
        topmost_item_index: &mut Option<usize>,
        caret_capable_only: bool,
    ) {
        for item_index in item_indices.iter().rev() {
            if topmost_item_index.is_some_and(|topmost| *item_index <= topmost) {
                return;
            }
            let item = &self.items[*item_index];
            if caret_capable_only && !item.can_produce_caret_position {
                continue;
            }
            if !self.item_contains(paintables, callbacks, item, local) {
                continue;
            }
            *topmost_item_index = Some(*item_index);
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
        &mut self,
        paintables: &PaintableArena,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
        with_caret_item: bool,
    ) -> (Option<TopmostItem>, Option<TopmostItem>) {
        self.build_derived_structures_if_needed();
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
                    paintables,
                    callbacks,
                    candidate_list,
                    local,
                    &mut topmost_hit_index,
                    false,
                );
                if with_caret_item {
                    self.find_topmost_item_in_list(
                        paintables,
                        callbacks,
                        candidate_list,
                        local,
                        &mut topmost_caret_index,
                        true,
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

    pub fn find_topmost_item(
        &mut self,
        paintables: &PaintableArena,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
    ) -> Option<TopmostItem> {
        self.find_topmost(paintables, callbacks, point, false).0
    }

    pub fn find_topmost_items_for_caret(
        &mut self,
        paintables: &PaintableArena,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
    ) -> (Option<TopmostItem>, Option<TopmostItem>) {
        let (hit, caret) = self.find_topmost(paintables, callbacks, point, true);
        (caret, hit)
    }

    pub fn hit_test_all(
        &mut self,
        paintables: &PaintableArena,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
    ) -> Vec<usize> {
        self.build_derived_structures_if_needed();
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
                    if self.item_contains(paintables, callbacks, &self.items[*item_index], local) {
                        hit_item_indices.push(*item_index);
                    }
                }
            }
        }
        hit_item_indices.sort_by(|a, b| b.cmp(a));
        hit_item_indices.dedup();
        hit_item_indices
    }
}
