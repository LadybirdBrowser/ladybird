/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod caret;
pub mod geometry;
pub mod query;
pub mod resolve;

use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::css::style::fast_hash::FastMap;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::host::FfiHitTestQueryCallbacks;
use crate::painting::paintable_rows::PaintableRowsRef;
use crate::painting::visual_context::{ClipBehavior, VisualContextTree};
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

#[derive(Clone, Debug, PartialEq)]
pub struct HitTestItem {
    pub kind: HitTestItemKind,
    pub paintable: NodeSlotId,
    /// The node whose style admitted this hit: a text fragment's parent, otherwise the paintable itself.
    pub hit_node: NodeSlotId,
    pub chrome_widget_kind: u8,
    pub text_fragment_index: Option<u32>,
    pub caret_node: NodeSlotId,
    pub caret_offset: usize,
    pub rect: CssPixelRect,
    pub caret_rect: CssPixelRect,
    pub caret_line_index: Option<usize>,
    pub block_container: NodeSlotId,
    pub context: ContextRef,
    pub border_radii: BorderRadii,
    pub path: Option<Rc<libgfx_rust::path::OwnedPath>>,
    pub winding_rule: i32,
    pub writing_mode: u8,
    pub inline_axis_is_reverse: bool,
    pub block_axis_is_reverse: bool,
    pub containing_block: NodeSlotId,
    pub can_produce_caret_position: bool,
}

impl HitTestItem {
    fn recorded_caret_line(&self) -> Option<(ContextRef, NodeSlotId, usize)> {
        self.caret_line_index
            .map(|index| (self.context, self.block_container, index))
    }
}

#[derive(Default)]
pub struct SpatialIndex {
    pub cells: FastMap<u64, Vec<usize>>,
    pub unbucketed_items: Vec<usize>,
}

// A visual line assembled from caret-capable display-list items. Caret lines preserve painted
// topology independently of the spatial hit-test index so keyboard navigation can reason about lines that contain
// empty or zero-area caret targets. The block container is the first populated one among the line's items.
#[derive(Clone, Debug)]
pub struct CaretLine {
    pub rect: CssPixelRect,
    pub block_container: NodeSlotId,
    pub context: ContextRef,
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
    pub item_capacity_hint_from_previous_list: usize,
    /// Point queries read only the spatial indexes, and caret navigation reads only the caret lines,
    /// which resolve line box geometry for every caret-capable item. Each is built on first use.
    pub spatial_indexes_built: bool,
    pub caret_lines_built: bool,
    pub caret_item_indices: Vec<usize>,
    pub caret_lines: Vec<CaretLine>,
    pub spatial_indexes_by_context: Vec<(ContextRef, SpatialIndex)>,
    pub spatial_index_position_by_context: FastMap<ContextRef, usize>,
}

impl HitTestList {
    pub fn append(&mut self, item: HitTestItem) {
        assert!(
            !self.spatial_indexes_built && !self.caret_lines_built,
            "hit-test item appended after the derived structures were built"
        );
        let items = std::rc::Rc::make_mut(&mut self.items);
        if items.capacity() == 0 {
            items.reserve(self.item_capacity_hint_from_previous_list);
        }
        items.push(item);
    }

    pub(crate) fn append_copies_of(&mut self, source: &[HitTestItem]) {
        assert!(
            !self.spatial_indexes_built && !self.caret_lines_built,
            "hit-test item appended after the derived structures were built"
        );
        if source.is_empty() {
            return;
        }
        let items = Rc::make_mut(&mut self.items);
        if items.capacity() == 0 {
            items.reserve(self.item_capacity_hint_from_previous_list.max(source.len()));
        }
        items.extend_from_slice(source);
    }

    pub(crate) fn caret_line_rect_for_item(rows: &PaintableRowsRef<'_>, item: &HitTestItem) -> CssPixelRect {
        let Some(line_rect) = geometry::containing_line_box_rect(rows, item) else {
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

    pub(crate) fn build_spatial_indexes_if_needed(&mut self) {
        if self.spatial_indexes_built {
            return;
        }
        self.spatial_indexes_built = true;
        for item_index in 0..self.items.len() {
            let item_is_caret_target_only = self.items[item_index].kind == HitTestItemKind::EmptyLine;
            if !item_is_caret_target_only {
                self.add_item_to_spatial_index(item_index);
            }
        }
    }

    pub(crate) fn build_caret_lines_if_needed(&mut self, arena: &LayoutNodeArena) {
        if self.caret_lines_built {
            return;
        }
        self.caret_lines_built = true;
        let rows = arena.paintable_rows();
        // Inline boxes and text from one layout line can be separated in paint order.
        // Gather that line's caret targets together while preserving the order in
        // which lines first appeared, and the paint order of targets within each line.
        let mut group_by_line = FastMap::default();
        let mut groups: Vec<smallvec::SmallVec<[usize; 4]>> = Vec::new();
        for (item_index, item) in self.items.iter().enumerate() {
            if item.caret_rect.is_empty() || !item.can_produce_caret_position {
                continue;
            }
            let mut new_group = || {
                groups.push(smallvec::SmallVec::new());
                groups.len() - 1
            };
            let group = match item.recorded_caret_line() {
                Some(line) => *group_by_line.entry(line).or_insert_with(new_group),
                None => new_group(),
            };
            groups[group].push(item_index);
        }
        for group in groups {
            for item_index in group {
                self.add_item_to_caret_items(&rows, item_index);
            }
        }
    }

    fn spatial_index_for(&mut self, context: ContextRef) -> &mut SpatialIndex {
        let items_arrive_in_runs_sharing_a_context = self
            .spatial_indexes_by_context
            .last()
            .is_some_and(|(last_context, _)| *last_context == context);
        let position = if items_arrive_in_runs_sharing_a_context {
            self.spatial_indexes_by_context.len() - 1
        } else {
            *self
                .spatial_index_position_by_context
                .entry(context)
                .or_insert_with(|| {
                    self.spatial_indexes_by_context.push((context, SpatialIndex::default()));
                    self.spatial_indexes_by_context.len() - 1
                })
        };
        &mut self.spatial_indexes_by_context[position].1
    }

    fn add_item_to_spatial_index(&mut self, item_index: usize) {
        let (kind, rect, context) = {
            let item = &self.items[item_index];
            (item.kind, item.rect, item.context)
        };
        let spatial_index = self.spatial_index_for(context);
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

    fn add_item_to_caret_items(&mut self, rows: &PaintableRowsRef<'_>, item_index: usize) {
        let item = &self.items[item_index];
        let caret_item_index = self.caret_item_indices.len();
        self.caret_item_indices.push(item_index);

        let writing_mode = item.writing_mode;
        let item_line_rect = Self::caret_line_rect_for_item(rows, item);
        let item_block_container = geometry::populated_block_container(rows, item.block_container);
        if let Some(line) = self.caret_lines.last_mut() {
            let first_line_item = &self.items[self.caret_item_indices[line.first_caret_item_index]];
            // Recorded lines belong to a line-producing block. Its layout containing
            // block can be shared by several paragraphs, including their empty lines.
            let same_recorded_line = item.recorded_caret_line().is_some()
                && first_line_item.recorded_caret_line() == item.recorded_caret_line();
            let same_inferred_line = first_line_item.caret_line_index.is_none()
                && item.caret_line_index.is_none()
                && first_line_item.containing_block == item.containing_block
                && rects_overlap_in_block_axis(line.rect, item.caret_rect, writing_mode);
            if line.context == item.context && (same_recorded_line || same_inferred_line) {
                line.rect.unite(item_line_rect);
                if line.block_container.is_invalid() {
                    line.block_container = item_block_container;
                }
                line.last_caret_item_index = caret_item_index;
                return;
            }
        }

        self.caret_lines.push(CaretLine {
            rect: item_line_rect,
            block_container: item_block_container,
            context: item.context,
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
    visual_context_tree: &VisualContextTree,
    callbacks: &FfiHitTestQueryCallbacks,
    context: ContextRef,
    point: CssPixelPoint,
    respect_clip: bool,
) -> Option<(f32, f32)> {
    // Returned in float local units: fixed-point CSSPixels quantization here would be magnified by the
    // accumulated transform for content in scaled-down local spaces, such as SVG user units under a
    // small viewBox.
    let pixel_ratio = callbacks.device_pixels_per_css_pixel as f32;
    let screen_point = libgfx_rust::FloatPoint {
        x: point.x.to_float() * pixel_ratio,
        y: point.y.to_float() * pixel_ratio,
    };
    let clip_behavior = ClipBehavior::from_respect_clip(respect_clip);
    let local = visual_context_tree.transform_point_for_hit_test(
        context,
        screen_point,
        callbacks.scroll_offsets(),
        clip_behavior,
    )?;
    Some((local.x / pixel_ratio, local.y / pixel_ratio))
}

pub(crate) fn to_css_point(local: (f32, f32)) -> CssPixelPoint {
    CssPixelPoint::new(
        CssPixels::nearest_value_for_f32(local.0),
        CssPixels::nearest_value_for_f32(local.1),
    )
}
