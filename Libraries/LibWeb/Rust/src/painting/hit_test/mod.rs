/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod caret;
pub mod query;

use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::layout::node_data::NodeSlotId;
use crate::painting::host::FfiHitTestQueryCallbacks;
use std::collections::HashMap;
use std::rc::Rc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum HitTestItemKind {
    Box = 0,
    SvgPath = 1,
    TextFragment = 2,
    EmptyLine = 3,
    EmptyEditable = 4,
    ChromeWidget = 5,
}

pub const CHROME_WIDGET_NONE: u8 = 0;
pub const CHROME_WIDGET_RESIZE_HANDLE: u8 = 1;
pub const CHROME_WIDGET_HORIZONTAL_SCROLLBAR: u8 = 2;
pub const CHROME_WIDGET_VERTICAL_SCROLLBAR: u8 = 3;

pub use crate::painting::border_radii::BorderRadii;

#[derive(Clone)]
pub struct HitTestItem {
    pub kind: HitTestItemKind,
    pub paintable: NodeSlotId,
    pub chrome_widget_kind: u8,
    pub text_fragment_index: Option<u32>,
    pub caret_node: NodeSlotId,
    pub caret_offset: usize,
    pub rect: CssPixelRect,
    pub caret_rect: CssPixelRect,
    pub caret_line_index: Option<usize>,
    pub caret_line_rect: Option<CssPixelRect>,
    pub block_container_margin_rect: Option<CssPixelRect>,
    pub visual_context_index: usize,
    pub border_radii: BorderRadii,
    pub path: Option<Rc<libgfx_rust::path::OwnedPath>>,
    pub winding_rule: i32,
    pub writing_mode: u8,
    pub inline_axis_is_reverse: bool,
    pub block_axis_is_reverse: bool,
    pub containing_block: NodeSlotId,
    pub can_produce_caret_position: bool,
}

#[derive(Default)]
pub struct SpatialIndex {
    pub cells: HashMap<u64, Vec<usize>>,
    pub unbucketed_items: Vec<usize>,
}

// A visual line assembled from consecutive caret-capable display-list items. Caret lines preserve painted
// topology independently of the spatial hit-test index so keyboard navigation can reason about lines that contain
// empty or zero-area caret targets.
#[derive(Clone, Debug)]
pub struct CaretLine {
    pub rect: CssPixelRect,
    pub block_container_margin_rect: Option<CssPixelRect>,
    pub visual_context_index: usize,
    pub first_caret_item_index: usize,
    pub last_caret_item_index: usize,
}

const SPATIAL_INDEX_CELL_SIZE: f64 = 128.0;
const MAX_BUCKETED_CELLS_PER_ITEM: u64 = 64;

pub fn spatial_index_cell_for(offset: CssPixels) -> i32 {
    (offset.to_double() / SPATIAL_INDEX_CELL_SIZE).floor() as i32
}

pub fn spatial_index_cell_key(x: i32, y: i32) -> u64 {
    ((x as u32 as u64) << 32) | (y as u32 as u64)
}

pub fn writing_mode_is_horizontal(writing_mode: u8) -> bool {
    writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB
}

pub fn block_axis_start(rect: CssPixelRect, writing_mode: u8) -> CssPixels {
    if writing_mode_is_horizontal(writing_mode) {
        rect.top()
    } else {
        rect.left()
    }
}

pub fn block_axis_end(rect: CssPixelRect, writing_mode: u8) -> CssPixels {
    if writing_mode_is_horizontal(writing_mode) {
        rect.bottom()
    } else {
        rect.right()
    }
}

pub fn rects_overlap_in_block_axis(a: CssPixelRect, b: CssPixelRect, writing_mode: u8) -> bool {
    block_axis_start(a, writing_mode) < block_axis_end(b, writing_mode)
        && block_axis_start(b, writing_mode) < block_axis_end(a, writing_mode)
}

#[derive(Default)]
pub struct HitTestList {
    pub generation: u64,
    pub items: std::rc::Rc<Vec<HitTestItem>>,
    pub derived_structures_built: bool,
    pub caret_item_indices: Vec<usize>,
    pub caret_lines: Vec<CaretLine>,
    pub spatial_indexes: Vec<Option<SpatialIndex>>,
    pub used_visual_context_indices: Vec<usize>,
}

impl HitTestList {
    pub fn append(&mut self, item: HitTestItem) {
        assert!(
            !self.derived_structures_built,
            "hit-test item appended after the derived structures were built"
        );
        std::rc::Rc::get_mut(&mut self.items)
            .expect("hit-test items are exclusively owned while recording")
            .push(item);
    }

    pub fn caret_line_rect_for_item(item: &HitTestItem) -> CssPixelRect {
        let Some(line_rect) = item.caret_line_rect else {
            return item.caret_rect;
        };
        let mut rect = item.caret_rect;
        if writing_mode_is_horizontal(item.writing_mode) {
            rect.unite_vertically(line_rect);
        } else {
            rect.unite_horizontally(line_rect);
        }
        rect
    }

    pub fn build_derived_structures_if_needed(&mut self) {
        if self.derived_structures_built {
            return;
        }
        self.derived_structures_built = true;
        for item_index in 0..self.items.len() {
            let item_is_caret_target_only = self.items[item_index].kind == HitTestItemKind::EmptyLine;
            if !item_is_caret_target_only {
                self.add_item_to_spatial_index(item_index);
            }
            self.add_item_to_caret_items(item_index);
        }
    }

    fn spatial_index_for(&mut self, visual_context_index: usize) -> &mut SpatialIndex {
        if self.spatial_indexes.len() <= visual_context_index {
            self.spatial_indexes.resize_with(visual_context_index + 1, || None);
        }
        if self.spatial_indexes[visual_context_index].is_none() {
            self.spatial_indexes[visual_context_index] = Some(SpatialIndex::default());
            self.used_visual_context_indices.push(visual_context_index);
        }
        self.spatial_indexes[visual_context_index].as_mut().unwrap()
    }

    fn add_item_to_spatial_index(&mut self, item_index: usize) {
        let (kind, rect, visual_context_index) = {
            let item = &self.items[item_index];
            (item.kind, item.rect, item.visual_context_index)
        };
        let spatial_index = self.spatial_index_for(visual_context_index);
        if kind == HitTestItemKind::ChromeWidget || rect.is_empty() {
            spatial_index.unbucketed_items.push(item_index);
            return;
        }
        let min_x = spatial_index_cell_for(rect.left());
        let max_x = spatial_index_cell_for(rect.right());
        let min_y = spatial_index_cell_for(rect.top());
        let max_y = spatial_index_cell_for(rect.bottom());
        let column_count = i64::from(max_x) - i64::from(min_x) + 1;
        let row_count = i64::from(max_y) - i64::from(min_y) + 1;
        if column_count <= 0 || row_count <= 0 {
            spatial_index.unbucketed_items.push(item_index);
            return;
        }
        let cell_count = column_count as u64 * row_count as u64;
        if cell_count > MAX_BUCKETED_CELLS_PER_ITEM {
            spatial_index.unbucketed_items.push(item_index);
            return;
        }
        for y in min_y..=max_y {
            for x in min_x..=max_x {
                spatial_index
                    .cells
                    .entry(spatial_index_cell_key(x, y))
                    .or_default()
                    .push(item_index);
            }
        }
    }

    fn add_item_to_caret_items(&mut self, item_index: usize) {
        let item = &self.items[item_index];
        if item.caret_rect.is_empty() || !item.can_produce_caret_position {
            return;
        }
        let caret_item_index = self.caret_item_indices.len();
        self.caret_item_indices.push(item_index);

        let writing_mode = item.writing_mode;
        let item_line_rect = Self::caret_line_rect_for_item(item);
        if let Some(line) = self.caret_lines.last_mut() {
            let first_line_item = &self.items[self.caret_item_indices[line.first_caret_item_index]];
            // Text fragments record their originating line box. Other caret-capable items, such as
            // atomic inline boxes, only join the previous caret line if their caret rects overlap
            // in the block axis.
            let same_recorded_line = first_line_item.caret_line_index.is_some()
                && item.caret_line_index.is_some()
                && first_line_item.caret_line_index == item.caret_line_index;
            let same_inferred_line = first_line_item.caret_line_index.is_none()
                && item.caret_line_index.is_none()
                && rects_overlap_in_block_axis(line.rect, item.caret_rect, writing_mode);
            if line.visual_context_index == item.visual_context_index
                && first_line_item.containing_block == item.containing_block
                && (same_recorded_line || same_inferred_line)
            {
                line.rect.unite(item_line_rect);
                if line.block_container_margin_rect.is_none() {
                    line.block_container_margin_rect = item.block_container_margin_rect;
                }
                line.last_caret_item_index = caret_item_index;
                return;
            }
        }

        self.caret_lines.push(CaretLine {
            rect: item_line_rect,
            block_container_margin_rect: item.block_container_margin_rect,
            visual_context_index: item.visual_context_index,
            first_caret_item_index: caret_item_index,
            last_caret_item_index: caret_item_index,
        });
    }
}

pub fn block_axis_coordinate(point: CssPixelPoint, writing_mode: u8) -> CssPixels {
    if writing_mode_is_horizontal(writing_mode) {
        point.y
    } else {
        point.x
    }
}

pub fn inline_axis_start(rect: CssPixelRect, writing_mode: u8) -> CssPixels {
    if writing_mode_is_horizontal(writing_mode) {
        rect.left()
    } else {
        rect.top()
    }
}

pub fn inline_axis_end(rect: CssPixelRect, writing_mode: u8) -> CssPixels {
    if writing_mode_is_horizontal(writing_mode) {
        rect.right()
    } else {
        rect.bottom()
    }
}

pub fn inline_axis_coordinate(point: CssPixelPoint, writing_mode: u8) -> CssPixels {
    if writing_mode_is_horizontal(writing_mode) {
        point.x
    } else {
        point.y
    }
}

pub(crate) fn local_float_point(
    callbacks: &FfiHitTestQueryCallbacks,
    visual_context_index: usize,
    point: CssPixelPoint,
    respect_clip: bool,
) -> Option<(f32, f32)> {
    // Returned in float local units: fixed-point CSSPixels quantization here would be magnified by the
    // accumulated transform for content in scaled-down local spaces, such as SVG user units under a
    // small viewBox.
    callbacks.local_point_for_visual_context(
        visual_context_index,
        point.x.raw_value(),
        point.y.raw_value(),
        respect_clip,
    )
}

pub(crate) fn to_css_point(local: (f32, f32)) -> CssPixelPoint {
    CssPixelPoint::new(
        CssPixels::nearest_value_for_f32(local.0),
        CssPixels::nearest_value_for_f32(local.1),
    )
}
