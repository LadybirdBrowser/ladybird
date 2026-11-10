/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod caret;
pub mod query;
pub mod resolve;

use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::css::style::fast_hash::FastMap;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::visual_context::VisualContextTree;
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum HitTestItemKind {
    Box = 0,
    SvgPath = 1,
    TextFragment = 2,
    EmptyEditable = 3,
    ChromeWidget = 4,
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
    pub rect: CssPixelRect,
    pub context: ContextRef,
    pub border_radii: BorderRadii,
    pub path: Option<Arc<libgfx_rust::path::Path>>,
    pub winding_rule: libgfx_rust::WindingRule,
    /// For an SVG path, the `svg_hit_area` flags naming which of its areas pointer events can target.
    pub svg_hit_areas: u8,
    /// For an SVG path whose stroke pointer events can target, the shape of that stroke.
    pub svg_stroke: Option<Arc<SvgStrokeHitRegion>>,
}

/// The areas of an SVG graphics element that pointer events can target.
pub mod svg_hit_area {
    pub const FILL: u8 = 1 << 0;
    pub const STROKE: u8 = 1 << 1;
    pub const BOUNDING_BOX: u8 = 1 << 2;
}

/// The stroke of an SVG path as hit testing reads it. The outline of the stroke is only built when a point first
/// reaches it, and the copies of an item that later recordings reuse share it.
#[derive(Debug)]
pub struct SvgStrokeHitRegion {
    style: libgfx_rust::path::StrokeStyle,
    outline: std::sync::OnceLock<libgfx_rust::path::Path>,
}

impl SvgStrokeHitRegion {
    pub fn new(style: libgfx_rust::path::StrokeStyle) -> Self {
        Self {
            style,
            outline: std::sync::OnceLock::new(),
        }
    }

    /// How far past the path its stroke can reach.
    pub fn outset(&self) -> f32 {
        let mut factor = 1.0f32;
        if self.style.join_style == libgfx_rust::JoinStyle::Miter {
            factor = factor.max(self.style.miter_limit);
        }
        if self.style.cap_style == libgfx_rust::CapStyle::Square {
            factor = factor.max(std::f32::consts::SQRT_2);
        }
        self.style.thickness / 2.0 * factor
    }

    pub fn contains(&self, path: &libgfx_rust::path::Path, x: f32, y: f32) -> bool {
        self.outline
            .get_or_init(|| path.stroke_to_fill(&self.style))
            .contains(x, y, libgfx_rust::WindingRule::Nonzero)
    }
}

impl PartialEq for SvgStrokeHitRegion {
    fn eq(&self, other: &Self) -> bool {
        self.style == other.style
    }
}

#[derive(Default)]
pub struct SpatialIndex {
    pub cells: FastMap<u64, Vec<usize>>,
    pub unbucketed_items: Vec<usize>,
}

const SPATIAL_INDEX_CELL_SIZE: f64 = 128.0;
const MAX_BUCKETED_CELLS_PER_ITEM: u64 = 64;

pub fn spatial_index_cell_for(offset: CssPixels) -> i32 {
    (offset.to_double() / SPATIAL_INDEX_CELL_SIZE).floor() as i32
}

pub fn spatial_index_cell_key(x: i32, y: i32) -> u64 {
    ((x as u32 as u64) << 32) | (y as u32 as u64)
}

#[derive(Default)]
pub struct HitTestList {
    pub generation: u64,
    /// The structural epoch of the visual context tree the items were recorded against, whose nodes they name.
    pub recorded_structural_epoch: u64,
    pub items: Arc<Vec<HitTestItem>>,
    pub item_capacity_hint_from_previous_list: usize,
    /// Point queries read only the spatial indexes, which are built on first use.
    pub spatial_indexes_built: bool,
    pub spatial_indexes_by_context: Vec<(ContextRef, SpatialIndex)>,
    pub spatial_index_position_by_context: FastMap<ContextRef, usize>,
}

impl HitTestList {
    pub fn append(&mut self, item: HitTestItem) {
        assert!(
            !self.spatial_indexes_built,
            "hit-test item appended after the spatial indexes were built"
        );
        let items = Arc::make_mut(&mut self.items);
        if items.capacity() == 0 {
            items.reserve(self.item_capacity_hint_from_previous_list);
        }
        items.push(item);
    }

    pub(crate) fn append_copies_of(&mut self, source: &[HitTestItem]) {
        assert!(
            !self.spatial_indexes_built,
            "hit-test item appended after the spatial indexes were built"
        );
        if source.is_empty() {
            return;
        }
        let items = Arc::make_mut(&mut self.items);
        if items.capacity() == 0 {
            items.reserve(self.item_capacity_hint_from_previous_list.max(source.len()));
        }
        items.extend_from_slice(source);
    }

    pub(crate) fn build_spatial_indexes_if_needed(&mut self) {
        if self.spatial_indexes_built {
            return;
        }
        self.spatial_indexes_built = true;
        for item_index in 0..self.items.len() {
            self.add_item_to_spatial_index(item_index);
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
}

pub(crate) fn to_css_point(local: libgfx_rust::FloatPoint) -> CssPixelPoint {
    CssPixelPoint::new(
        CssPixels::nearest_value_for_f32(local.x),
        CssPixels::nearest_value_for_f32(local.y),
    )
}
