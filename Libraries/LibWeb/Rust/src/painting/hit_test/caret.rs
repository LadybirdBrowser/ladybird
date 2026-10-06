/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::painting::host::{
    FfiCaretAt, FfiCaretBoundaryKind, FfiCaretPositionQuery, FfiHitTestQueryCallbacks, FfiNodeIdentity,
    FfiResolvedCaret,
};
use crate::painting::paint_read::PaintRead;
use crate::painting::rect_to_viewport_transform::RectToViewportTransform;
use crate::painting::text_fragment::CaretMatch;
use crate::painting::visual_context::{IncludeVisualViewportTransform, SpatialNodeIndex};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum CaretPositionType {
    Closest = 0,
    Before = 1,
    After = 2,
}

impl CaretPositionType {
    pub fn from_u8(value: u8) -> Self {
        match value {
            value if value == Self::Before as u8 => Self::Before,
            value if value == Self::After as u8 => Self::After,
            _ => Self::Closest,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum CaretPositionMode {
    Normal = 0,
    SelectionStart = 1,
    Selection = 2,
}

impl CaretPositionMode {
    pub fn from_u8(value: u8) -> Self {
        match value {
            value if value == Self::SelectionStart as u8 => Self::SelectionStart,
            value if value == Self::Selection as u8 => Self::Selection,
            _ => Self::Normal,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum CaretLineDirection {
    Previous = 0,
    Next = 1,
}
// Treat small block-axis gaps between caret line fragments as the same visual row.
const CARET_LINE_BLOCK_AXIS_RANGE_SLOP: i64 = 4;
// Prefer a nearby line in the same visual column over a slightly closer line in another column.
const CARET_LINE_BLOCK_AXIS_COMPARE_SLOP: i64 = 12;
// Within the chosen line, tolerate larger block-axis differences before snapping across inline gaps.
const CARET_ITEM_BLOCK_AXIS_COMPARE_SLOP: i64 = 32;

fn px(value: i64) -> CssPixels {
    CssPixels::from_integer(value)
}
fn distance_to_range(coordinate: CssPixels, start: CssPixels, end: CssPixels) -> CssPixels {
    if coordinate < start {
        return start - coordinate;
    }
    if coordinate > end {
        return coordinate - end;
    }
    CssPixels::from_raw(0)
}

fn block_axis_distance_to_line_rect(rect: CssPixelRect, point: CssPixelPoint, writing_mode: u8) -> CssPixels {
    distance_to_range(
        block_axis_coordinate(point, writing_mode),
        block_axis_start(rect, writing_mode) - px(CARET_LINE_BLOCK_AXIS_RANGE_SLOP),
        block_axis_end(rect, writing_mode) + px(CARET_LINE_BLOCK_AXIS_RANGE_SLOP),
    )
}

fn inline_axis_distance_to_rect(rect: CssPixelRect, point: CssPixelPoint, writing_mode: u8) -> CssPixels {
    distance_to_range(
        inline_axis_coordinate(point, writing_mode),
        inline_axis_start(rect, writing_mode),
        inline_axis_end(rect, writing_mode),
    )
}

fn absolute_difference(a: CssPixels, b: CssPixels) -> CssPixels {
    if a > b { a - b } else { b - a }
}

fn caret_line_is_better_candidate(
    block_distance: CssPixels,
    inline_distance: CssPixels,
    closest_block_distance: CssPixels,
    closest_inline_distance: CssPixels,
    block_axis_compare_slop: CssPixels,
) -> bool {
    if absolute_difference(block_distance, closest_block_distance) <= block_axis_compare_slop {
        if inline_distance != closest_inline_distance {
            return inline_distance < closest_inline_distance;
        }
        return block_distance < closest_block_distance;
    }
    block_distance < closest_block_distance
}

fn line_block_middle(rect: CssPixelRect, writing_mode: u8) -> CssPixels {
    block_axis_start(rect, writing_mode)
        + (block_axis_end(rect, writing_mode) - block_axis_start(rect, writing_mode)).scaled(0.5)
}
#[derive(Clone, Copy, Debug)]
pub struct ClosestLine {
    pub index: Option<usize>,
    pub local_point: CssPixelPoint,
    pub block_distance: CssPixels,
    pub block_start_distance: CssPixels,
    pub inline_distance: CssPixels,
    pub block_container: NodeSlotId,
    pub is_before_point: bool,
    pub contains_point_in_block_axis: bool,
}

impl Default for ClosestLine {
    fn default() -> Self {
        Self {
            index: None,
            local_point: CssPixelPoint::default(),
            block_distance: CssPixels::from_raw(i32::MAX),
            block_start_distance: CssPixels::from_raw(i32::MAX),
            inline_distance: CssPixels::from_raw(i32::MAX),
            block_container: NodeSlotId::INVALID,
            is_before_point: false,
            contains_point_in_block_axis: false,
        }
    }
}

impl HitTestList {
    pub(crate) fn caret_line_for_position(
        &self,
        arena: &impl PaintRead,
        query: &FfiCaretPositionQuery,
        offset: usize,
        affinity_is_downstream: bool,
    ) -> Option<usize> {
        // At a soft wrap, prefer the fragment whose line directly owns the position. Only use the preceding
        // fragment's fallback match when no direct match exists.
        for allow_soft_wrap_fallback in [false, true] {
            for (line_index, line) in self.caret_lines.iter().enumerate() {
                for caret_item_index in line.first_caret_item_index..=line.last_caret_item_index {
                    let item_index = self.caret_item_indices[caret_item_index];
                    let position_match =
                        self.item_position_match(arena, query, item_index, offset, affinity_is_downstream);
                    match position_match {
                        CaretMatch::None => continue,
                        CaretMatch::SoftWrapFallback if !allow_soft_wrap_fallback => continue,
                        CaretMatch::Direct | CaretMatch::SoftWrapFallback => return Some(line_index),
                    }
                }
            }
        }
        None
    }

    fn item_position_match(
        &self,
        arena: &impl PaintRead,
        query: &FfiCaretPositionQuery,
        item_index: usize,
        offset: usize,
        affinity_is_downstream: bool,
    ) -> CaretMatch {
        let item = &self.items[item_index];
        match item.kind {
            HitTestItemKind::TextFragment => resolve::with_item_fragment(arena, item, |fragment| {
                let node = resolve::row_dom_style_node(arena, fragment.layout_node);
                if query.is_query_node(node) {
                    return crate::painting::text_fragment::caret_match(fragment, offset, affinity_is_downstream);
                }
                if fragment.dom_start_offset_in_node == 0 && query.boundary_descends_to(node) {
                    return CaretMatch::Direct;
                }
                if query.boundary_follows_end(node, fragment.dom_end_offset_with_trailing_whitespace) {
                    return CaretMatch::Direct;
                }
                CaretMatch::None
            })
            .unwrap_or(CaretMatch::None),
            HitTestItemKind::EmptyLine => {
                let node = resolve::row_dom_style_node(arena, item.caret_node);
                let matches = if super::resolve::empty_line_is_anchored_to_its_forced_break(arena, item) {
                    query.boundary_precedes(node)
                } else {
                    item.caret_offset == offset && query.is_query_node(node)
                };
                if matches { CaretMatch::Direct } else { CaretMatch::None }
            }
            HitTestItemKind::EmptyEditable => {
                let node = resolve::row_dom_style_node(arena, item.paintable);
                if offset == 0 && query.is_query_node(node) {
                    CaretMatch::Direct
                } else {
                    CaretMatch::None
                }
            }
            HitTestItemKind::Box => {
                let node = resolve::row_dom_style_node(arena, item.paintable);
                if query.is_adjacent_to(node) {
                    CaretMatch::Direct
                } else {
                    CaretMatch::None
                }
            }
            HitTestItemKind::SvgPath | HitTestItemKind::ChromeWidget => CaretMatch::None,
        }
    }

    pub fn box_point_is_before(&self, item_index: usize, local_point: CssPixelPoint) -> bool {
        let item = &self.items[item_index];
        debug_assert_eq!(item.kind, HitTestItemKind::Box);

        let block_coordinate = block_axis_coordinate(local_point, item.writing_mode);
        if block_coordinate < block_axis_start(item.rect, item.writing_mode) {
            return !item.block_axis_is_reverse;
        }
        if block_coordinate >= block_axis_end(item.rect, item.writing_mode) {
            return item.block_axis_is_reverse;
        }

        let inline_start = inline_axis_start(item.rect, item.writing_mode);
        let inline_end = inline_axis_end(item.rect, item.writing_mode);
        let inline_middle = inline_start + (inline_end - inline_start).scaled(0.5);
        let inline_coordinate = inline_axis_coordinate(local_point, item.writing_mode);
        if item.inline_axis_is_reverse {
            inline_coordinate > inline_middle
        } else {
            inline_coordinate <= inline_middle
        }
    }

    fn first_item_of_line(&self, line: &CaretLine) -> &HitTestItem {
        &self.items[self.caret_item_indices[line.first_caret_item_index]]
    }

    pub fn item_at_line_edge(&self, line_index: usize, position_type: CaretPositionType) -> usize {
        // INTEROP: Home and End operate on visual lines in other engines. Choose the furthest caret-capable painted
        // item along the logical inline axis instead of assuming that display-list order or DOM order describes that
        // edge.
        debug_assert!(self.caret_lines_built);
        let line = self.caret_lines[line_index].clone();
        let first_item = self.first_item_of_line(&line);
        let writing_mode = first_item.writing_mode;
        let inline_axis_is_reverse = first_item.inline_axis_is_reverse;
        let coordinate_for_item = |item: &HitTestItem| -> CssPixels {
            if position_type == CaretPositionType::Before {
                return if inline_axis_is_reverse {
                    inline_axis_end(item.caret_rect, writing_mode)
                } else {
                    inline_axis_start(item.caret_rect, writing_mode)
                };
            }
            if inline_axis_is_reverse {
                inline_axis_start(item.caret_rect, writing_mode)
            } else {
                inline_axis_end(item.caret_rect, writing_mode)
            }
        };
        let coordinate_is_closer_to_line_edge = |coordinate: CssPixels, best_coordinate: CssPixels| -> bool {
            if position_type == CaretPositionType::Before {
                return if inline_axis_is_reverse {
                    coordinate > best_coordinate
                } else {
                    coordinate < best_coordinate
                };
            }
            if inline_axis_is_reverse {
                coordinate < best_coordinate
            } else {
                coordinate > best_coordinate
            }
        };
        let mut best_item_index = self.caret_item_indices[line.first_caret_item_index];
        let mut best_coordinate = coordinate_for_item(&self.items[best_item_index]);
        let item_is_on_line = |item: &HitTestItem| -> bool {
            if let Some(recorded_line) = first_item.recorded_caret_line() {
                return item.recorded_caret_line() == Some(recorded_line);
            }
            if line.context != item.context || first_item.containing_block != item.containing_block {
                return false;
            }
            if item.caret_line_index.is_none() {
                return rects_overlap_in_block_axis(line.rect, item.caret_rect, writing_mode);
            }
            false
        };
        // Items without a recorded line identity can still belong to the same
        // inferred visual line even when other caret runs separate them.
        for item_index in &self.caret_item_indices {
            let item = &self.items[*item_index];
            if !item_is_on_line(item) {
                continue;
            }
            let coordinate = coordinate_for_item(item);
            if coordinate_is_closer_to_line_edge(coordinate, best_coordinate) {
                best_item_index = *item_index;
                best_coordinate = coordinate;
            }
        }
        best_item_index
    }

    pub(crate) fn caret_item_for_line(
        &self,
        arena: &impl PaintRead,
        line_index: usize,
        local_point: CssPixelPoint,
        mode: CaretPositionMode,
    ) -> Option<(usize, CaretPositionType)> {
        debug_assert!(self.caret_lines_built);
        let rows = arena;
        let line = self.caret_lines[line_index].clone();
        let first_item = self.first_item_of_line(&line);
        let writing_mode = first_item.writing_mode;
        let inline_axis_is_reverse = first_item.inline_axis_is_reverse;

        let block_coordinate = block_axis_coordinate(local_point, writing_mode);
        // Once a line has been selected, points before or after its block-axis range resolve to
        // the logical line edges. Points inside the line range resolve to the closest caret-capable
        // item on that line.
        if block_coordinate < block_axis_start(line.rect, writing_mode) {
            return Some((
                self.item_at_line_edge(line_index, CaretPositionType::Before),
                CaretPositionType::Before,
            ));
        }
        let inline_coordinate = inline_axis_coordinate(local_point, writing_mode);
        if mode == CaretPositionMode::Selection && block_coordinate >= block_axis_end(line.rect, writing_mode) {
            return Some((
                self.item_at_line_edge(line_index, CaretPositionType::After),
                CaretPositionType::After,
            ));
        }
        if block_coordinate >= block_axis_end(line.rect, writing_mode) + px(CARET_LINE_BLOCK_AXIS_COMPARE_SLOP) {
            return Some((
                self.item_at_line_edge(line_index, CaretPositionType::After),
                CaretPositionType::After,
            ));
        }
        if block_coordinate >= block_axis_end(line.rect, writing_mode)
            && (inline_coordinate < inline_axis_start(line.rect, writing_mode)
                || inline_coordinate >= inline_axis_end(line.rect, writing_mode))
        {
            return Some((
                self.item_at_line_edge(line_index, CaretPositionType::After),
                CaretPositionType::After,
            ));
        }
        // Points past either inline edge resolve to the corresponding logical line edge.
        if inline_coordinate < inline_axis_start(line.rect, writing_mode) {
            let position_type = if inline_axis_is_reverse {
                CaretPositionType::After
            } else {
                CaretPositionType::Before
            };
            return Some((self.item_at_line_edge(line_index, position_type), position_type));
        }
        if inline_coordinate >= inline_axis_end(line.rect, writing_mode) {
            let position_type = if inline_axis_is_reverse {
                CaretPositionType::Before
            } else {
                CaretPositionType::After
            };
            return Some((self.item_at_line_edge(line_index, position_type), position_type));
        }

        let mut closest_item_index: Option<usize> = None;
        let mut closest_block_distance = CssPixels::from_raw(i32::MAX);
        let mut closest_inline_distance = CssPixels::from_raw(i32::MAX);
        for caret_item_index in line.first_caret_item_index..=line.last_caret_item_index {
            let item_index = self.caret_item_indices[caret_item_index];
            let item = &self.items[item_index];
            let item_writing_mode = item.writing_mode;
            let block_distance = block_axis_distance_to_line_rect(
                Self::caret_line_rect_for_item(rows, item),
                local_point,
                item_writing_mode,
            );
            let inline_distance = inline_axis_distance_to_rect(item.caret_rect, local_point, item_writing_mode);
            if closest_item_index.is_none()
                || caret_line_is_better_candidate(
                    block_distance,
                    inline_distance,
                    closest_block_distance,
                    closest_inline_distance,
                    px(CARET_ITEM_BLOCK_AXIS_COMPARE_SLOP),
                )
            {
                closest_item_index = Some(item_index);
                closest_block_distance = block_distance;
                closest_inline_distance = inline_distance;
            }
        }
        closest_item_index.map(|index| (index, CaretPositionType::Closest))
    }

    pub fn line_block_coordinate(&self, line_index: usize) -> CssPixels {
        debug_assert!(self.caret_lines_built);
        let line = &self.caret_lines[line_index];
        let writing_mode = self.first_item_of_line(line).writing_mode;
        line_block_middle(line.rect, writing_mode)
    }

    pub fn item_is_inline_adjacent_to_line(&self, item_index: usize, line_index: usize) -> bool {
        debug_assert!(self.caret_lines_built);
        let item = &self.items[item_index];
        let line = &self.caret_lines[line_index];
        if item.context != line.context || item.rect.is_empty() {
            return false;
        }
        let writing_mode = self.first_item_of_line(line).writing_mode;
        if !rects_overlap_in_block_axis(item.rect, line.rect, writing_mode) {
            return false;
        }
        inline_axis_end(item.rect, writing_mode) <= inline_axis_start(line.rect, writing_mode)
            || inline_axis_end(line.rect, writing_mode) <= inline_axis_start(item.rect, writing_mode)
    }

    fn line_in_scope(
        &self,
        main_thread: &crate::stage::MainThread,
        arena: &impl PaintRead,
        callbacks: &FfiHitTestQueryCallbacks,
        line_index: usize,
        scope: FfiNodeIdentity,
    ) -> bool {
        let line = &self.caret_lines[line_index];
        for caret_item_index in line.first_caret_item_index..=line.last_caret_item_index {
            let node = self.item_target(arena, self.caret_item_indices[caret_item_index]);
            if !node.is_none() && callbacks.contains(main_thread, scope, node) {
                return true;
            }
        }
        false
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn find_closest_line(
        &self,
        main_thread: &crate::stage::MainThread,
        arena: &impl PaintRead,
        visual_context_tree: &VisualContextTree,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
        mode: CaretPositionMode,
        scope: FfiNodeIdentity,
        respect_clip: bool,
    ) -> ClosestLine {
        debug_assert!(self.caret_lines_built);
        let rows = arena;
        let mut closest_line = ClosestLine::default();
        let mut closest_line_after_point = ClosestLine::default();
        let mut closest_line_before_point = ClosestLine::default();

        let line_after_point_is_better_candidate = |block_start_distance: CssPixels,
                                                    inline_distance: CssPixels,
                                                    closest_block_start_distance: CssPixels,
                                                    closest_inline_distance: CssPixels|
         -> bool {
            if absolute_difference(block_start_distance, closest_block_start_distance)
                <= px(CARET_LINE_BLOCK_AXIS_COMPARE_SLOP)
            {
                if inline_distance != closest_inline_distance {
                    return inline_distance < closest_inline_distance;
                }
                return block_start_distance < closest_block_start_distance;
            }
            block_start_distance < closest_block_start_distance
        };

        for line_index in 0..self.caret_lines.len() {
            if !scope.is_none() && !self.line_in_scope(main_thread, arena, callbacks, line_index, scope) {
                continue;
            }
            let line = self.caret_lines[line_index].clone();
            let Some(local) = local_float_point(visual_context_tree, callbacks, line.context, point, respect_clip)
            else {
                continue;
            };
            let local_point = to_css_point(local);
            let writing_mode = self.first_item_of_line(&line).writing_mode;
            let block_distance = block_axis_distance_to_line_rect(line.rect, local_point, writing_mode);
            let block_coordinate = block_axis_coordinate(local_point, writing_mode);
            let inline_distance = inline_axis_distance_to_rect(line.rect, local_point, writing_mode);
            let contains_point_in_block_axis = block_coordinate >= block_axis_start(line.rect, writing_mode)
                && block_coordinate < block_axis_end(line.rect, writing_mode);
            let is_better_candidate = {
                if closest_line.index.is_none() {
                    true
                } else {
                    // Between lines of the same block container, a line whose block-axis range
                    // contains the point always beats lines that do not.
                    let same_block_container =
                        !line.block_container.is_invalid() && line.block_container == closest_line.block_container;
                    if same_block_container && contains_point_in_block_axis != closest_line.contains_point_in_block_axis
                    {
                        contains_point_in_block_axis
                    } else {
                        caret_line_is_better_candidate(
                            block_distance,
                            inline_distance,
                            closest_line.block_distance,
                            closest_line.inline_distance,
                            px(CARET_LINE_BLOCK_AXIS_COMPARE_SLOP),
                        )
                    }
                }
            };
            if is_better_candidate {
                closest_line.index = Some(line_index);
                closest_line.local_point = local_point;
                closest_line.block_distance = block_distance;
                closest_line.inline_distance = inline_distance;
                closest_line.block_container = line.block_container;
                closest_line.is_before_point = block_axis_end(line.rect, writing_mode) < block_coordinate;
                closest_line.contains_point_in_block_axis = contains_point_in_block_axis;
            }

            if block_axis_end(line.rect, writing_mode) < block_coordinate
                && (closest_line_before_point.index.is_none()
                    || caret_line_is_better_candidate(
                        block_distance,
                        inline_distance,
                        closest_line_before_point.block_distance,
                        closest_line_before_point.inline_distance,
                        px(CARET_LINE_BLOCK_AXIS_COMPARE_SLOP),
                    ))
            {
                closest_line_before_point.index = Some(line_index);
                closest_line_before_point.local_point = local_point;
                closest_line_before_point.block_distance = block_distance;
                closest_line_before_point.inline_distance = inline_distance;
                closest_line_before_point.block_container = line.block_container;
                closest_line_before_point.is_before_point = true;
            }

            let block_start = block_axis_start(line.rect, writing_mode);
            if block_start <= block_coordinate {
                continue;
            }
            // Keep track of the nearest following line separately.
            let block_start_distance = block_start - block_coordinate;
            if closest_line_after_point.index.is_none()
                || line_after_point_is_better_candidate(
                    block_start_distance,
                    inline_distance,
                    closest_line_after_point.block_start_distance,
                    closest_line_after_point.inline_distance,
                )
            {
                closest_line_after_point.index = Some(line_index);
                closest_line_after_point.local_point = local_point;
                closest_line_after_point.block_distance = block_distance;
                closest_line_after_point.block_start_distance = block_start_distance;
                closest_line_after_point.inline_distance = inline_distance;
                closest_line_after_point.block_container = line.block_container;
            }
        }

        if mode == CaretPositionMode::SelectionStart
            && !closest_line.contains_point_in_block_axis
            && closest_line_before_point.index.is_some()
            && closest_line.index != closest_line_before_point.index
            && closest_line_before_point.block_distance <= px(CARET_ITEM_BLOCK_AXIS_COMPARE_SLOP)
        {
            return closest_line_before_point;
        }

        if let Some(closest_index) = closest_line.index
            && closest_line.is_before_point
            && closest_line_after_point.index.is_some()
            && closest_line_after_point.block_distance <= px(CARET_LINE_BLOCK_AXIS_COMPARE_SLOP)
            && closest_line_after_point.inline_distance <= closest_line.inline_distance
        {
            let line = &self.caret_lines[closest_index];
            let writing_mode = self.first_item_of_line(line).writing_mode;
            let block_coordinate = block_axis_coordinate(closest_line.local_point, writing_mode);
            let point_is_in_closest_line_block_container_margin =
                super::geometry::block_container_margin_rect(rows, closest_line.block_container)
                    .is_some_and(|rect| block_coordinate < block_axis_end(rect, writing_mode));
            let lines_share_block_container = !closest_line.block_container.is_invalid()
                && closest_line.block_container == closest_line_after_point.block_container;
            // A point still inside the previous block container's margin box should not jump to
            // text in a different block container, even if that following line is close.
            if point_is_in_closest_line_block_container_margin && !lines_share_block_container {
                return closest_line;
            }
            return closest_line_after_point;
        }
        closest_line
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn adjacent_line(
        &self,
        main_thread: &crate::stage::MainThread,
        arena: &impl PaintRead,
        callbacks: &FfiHitTestQueryCallbacks,
        current_line_index: usize,
        direction: CaretLineDirection,
        inline_coordinate: CssPixels,
        scope: FfiNodeIdentity,
    ) -> Option<(usize, CssPixelPoint)> {
        // INTEROP: Vertical caret movement in Chromium, WebKit, and Gecko follows rendered line geometry rather than
        // DOM tree order. Rank candidates in the requested logical block direction, prefer the current line-producing
        // context, then minimize block distance and finally distance from the remembered inline coordinate.

        // Keep these coordinates fractional. Rounding line geometry before comparison can reorder candidates when
        // layout positions or device scaling produce subpixel line centers.
        debug_assert!(self.caret_lines_built);
        let current_line = self.caret_lines[current_line_index].clone();
        let current_first_item = self.first_item_of_line(&current_line);
        let writing_mode = current_first_item.writing_mode;
        let block_axis_is_reverse = current_first_item.block_axis_is_reverse;
        let physically_after = (direction == CaretLineDirection::Next) != block_axis_is_reverse;
        let current_line_context = current_first_item.paintable;
        let current_block_coordinate = line_block_middle(current_line.rect, writing_mode);

        let mut closest_line_index: Option<usize> = None;
        let mut closest_line_shares_context = false;
        let mut closest_block_distance = CssPixels::from_raw(i32::MAX);
        let mut closest_inline_distance = CssPixels::from_raw(i32::MAX);
        for line_index in 0..self.caret_lines.len() {
            let line = &self.caret_lines[line_index];
            // INTEROP: Keyboard navigation follows layout geometry across clips, effects, and transforms.
            //          Separate paint contexts inside one editing host must not isolate its editable lines.
            if line_index == current_line_index || !self.line_in_scope(main_thread, arena, callbacks, line_index, scope)
            {
                continue;
            }
            let candidate_block_coordinate = line_block_middle(line.rect, writing_mode);
            if (physically_after && candidate_block_coordinate <= current_block_coordinate)
                || (!physically_after && candidate_block_coordinate >= current_block_coordinate)
            {
                continue;
            }
            let block_distance = absolute_difference(candidate_block_coordinate, current_block_coordinate);
            let inline_distance = distance_to_range(
                inline_coordinate,
                inline_axis_start(line.rect, writing_mode),
                inline_axis_end(line.rect, writing_mode),
            );
            let candidate_first_item = self.first_item_of_line(line);
            // Prefer lines produced by the same line paintable. Independently painted content, such as a floated first
            // letter, may occupy the same block-axis neighborhood without being the next line of the current content.
            let shares_line_context = candidate_first_item.paintable == current_line_context;
            if closest_line_index.is_none()
                || (shares_line_context && !closest_line_shares_context)
                || (shares_line_context == closest_line_shares_context
                    && (block_distance < closest_block_distance
                        || (block_distance == closest_block_distance && inline_distance < closest_inline_distance)))
            {
                closest_line_index = Some(line_index);
                closest_line_shares_context = shares_line_context;
                closest_block_distance = block_distance;
                closest_inline_distance = inline_distance;
            }
        }
        let closest_line_index = closest_line_index?;
        let closest_line = &self.caret_lines[closest_line_index];
        let block_coordinate = line_block_middle(closest_line.rect, writing_mode);
        let point = if writing_mode_is_horizontal(writing_mode) {
            CssPixelPoint::new(inline_coordinate, block_coordinate)
        } else {
            CssPixelPoint::new(block_coordinate, inline_coordinate)
        };
        Some((closest_line_index, point))
    }
}

impl FfiCaretAt {
    // Moves the debug rect from the local space of `spatial` into the viewport.
    fn with_debug_rect_in_viewport(
        mut self,
        tree: &VisualContextTree,
        callbacks: &FfiHitTestQueryCallbacks,
        spatial: SpatialNodeIndex,
    ) -> Self {
        if self.caret.has_debug_rect {
            let transform = RectToViewportTransform {
                visual_context_tree: tree,
                scroll_offsets: callbacks.scroll_offsets(),
                device_pixels_per_css_pixel: callbacks.device_pixels_per_css_pixel as f32,
            };
            self.caret.debug_rect = transform
                .transform_rect_in_space(
                    spatial,
                    self.caret.debug_rect.into(),
                    IncludeVisualViewportTransform::Yes,
                )
                .into();
        }
        self
    }
}

impl HitTestList {
    fn item_is_direct_caret_target(&self, arena: &impl PaintRead, item_index: usize) -> bool {
        let target = self.item_target(arena, item_index);
        !target.is_none() && target == self.item_dispatch_target(arena, item_index)
    }

    fn caret_at_item(
        &self,
        arena: &impl PaintRead,
        item_index: usize,
        local_point: CssPixelPoint,
        position_type: CaretPositionType,
    ) -> Option<FfiCaretAt> {
        let caret = self.resolve_caret(arena, item_index, local_point, position_type);
        caret.has_position.then_some(FfiCaretAt {
            paintable: self.items[item_index].paintable,
            caret,
        })
    }

    fn caret_at_line(
        &self,
        arena: &impl PaintRead,
        line_index: usize,
        local_point: CssPixelPoint,
        mode: CaretPositionMode,
    ) -> Option<FfiCaretAt> {
        let (item_index, position_type) = self.caret_item_for_line(arena, line_index, local_point, mode)?;
        self.caret_at_item(arena, item_index, local_point, position_type)
    }

    // The start of the node the item stands for, for a hit on an item that produces no caret position itself.
    fn caret_at_hit_container(&self, arena: &impl PaintRead, item_index: usize) -> Option<FfiCaretAt> {
        let node = self.item_target(arena, item_index);
        if node.is_none() {
            return None;
        }
        let item = &self.items[item_index];
        Some(FfiCaretAt {
            paintable: item.paintable,
            caret: FfiResolvedCaret {
                has_position: true,
                node,
                has_debug_rect: true,
                debug_rect: item.caret_rect.into(),
                ..Default::default()
            },
        })
    }

    // Whether the node of the caret's boundary is `ancestor` or one of its descendants. A boundary beside a node is
    // inside that node's parent.
    fn caret_boundary_is_inside(
        main_thread: &crate::stage::MainThread,
        callbacks: &FfiHitTestQueryCallbacks,
        ancestor: FfiNodeIdentity,
        caret: &FfiResolvedCaret,
    ) -> bool {
        let boundary_is_beside_node = caret.boundary != FfiCaretBoundaryKind::Offset;
        (!boundary_is_beside_node || ancestor != caret.node) && callbacks.contains(main_thread, ancestor, caret.node)
    }

    /// The caret position at `point`. When `constraint_scope` names a node, the position is constrained to lines inside
    /// it, and points outside it resolve to the closest position within it.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn caret_position_from_point(
        &self,
        main_thread: &crate::stage::MainThread,
        arena: &impl PaintRead,
        tree: &VisualContextTree,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
        mode: CaretPositionMode,
        constraint_scope: FfiNodeIdentity,
    ) -> Option<FfiCaretAt> {
        let constrained = !constraint_scope.is_none();
        // First find both the topmost hit-test item and the topmost item that can directly produce a caret.
        // Non-caret items are still needed to keep later line fallback scoped to the hit content.
        // FIXME: Caret placement compares items by record order alone, ignoring the depth-sorted paint order of
        //        planes inside 3D rendering contexts.
        let (mut topmost_item, topmost_hit_item) = self.find_topmost_items_for_caret(arena, tree, callbacks, point);

        // A constrained search only accepts direct hits inside the constraint scope.
        if constrained && let Some(item) = topmost_item {
            let node = self.item_target(arena, item.index);
            if node.is_none() || !callbacks.contains(main_thread, constraint_scope, node) {
                topmost_item = None;
            }
        }

        // Direct caret hits win unless another non-caret item is visibly on top of them.
        if let Some(item) = topmost_item {
            let matches_hit_item = topmost_hit_item
                .is_some_and(|hit| hit.index == item.index && self.item_is_direct_caret_target(arena, item.index));
            if (constrained || topmost_hit_item.is_none() || matches_hit_item)
                && let Some(caret) = self.caret_at_item(arena, item.index, item.local_point, CaretPositionType::Closest)
            {
                return Some(caret.with_debug_rect_in_viewport(
                    tree,
                    callbacks,
                    self.items[item.index].context.spatial,
                ));
            }
        }

        // If the point is over a non-caret item, only consider caret lines inside that item's event-dispatch node first.
        // This prevents overlays or side content from snapping the caret to unrelated nearby text.
        let line_scope = match topmost_hit_item {
            _ if constrained => constraint_scope,
            Some(hit)
                if !self.items[hit.index].can_produce_caret_position
                    || !self.item_is_direct_caret_target(arena, hit.index) =>
            {
                self.item_dispatch_target(arena, hit.index)
            }
            _ => FfiNodeIdentity::default(),
        };

        // A constrained search must find a line even when the point is outside the scope's clipped area (e.g. dragging
        // a selection outside a textarea), so it transforms points without rejecting them against clips.
        let respect_clip = !constrained;
        let find_closest_line =
            |scope| self.find_closest_line(main_thread, arena, tree, callbacks, point, mode, scope, respect_clip);
        let mut closest_line = find_closest_line(line_scope);
        if !line_scope.is_none() && !constrained {
            // The scoped search is only a guard against unrelated nearby content. If there is a plainly closer line
            // outside the scope, use it instead.
            let unscoped_closest_line = find_closest_line(FfiNodeIdentity::default());
            if closest_line.index.is_none()
                || (unscoped_closest_line.index.is_some()
                    && unscoped_closest_line.block_distance < closest_line.block_distance)
            {
                closest_line = unscoped_closest_line;
            }
        }

        let Some(line_index) = closest_line.index else {
            let hit = topmost_hit_item.filter(|_| !constrained)?;
            return self.caret_at_hit_container(arena, hit.index).map(|caret| {
                caret.with_debug_rect_in_viewport(tree, callbacks, self.items[hit.index].context.spatial)
            });
        };
        let caret = self
            .caret_at_line(arena, line_index, closest_line.local_point, mode)?
            .with_debug_rect_in_viewport(tree, callbacks, self.caret_lines[line_index].context.spatial);

        if !constrained && let Some(hit) = topmost_hit_item {
            let hit_target = self.item_dispatch_target(arena, hit.index);
            if !hit_target.is_none()
                && !caret.caret.node.is_none()
                && !Self::caret_boundary_is_inside(main_thread, callbacks, hit_target, &caret.caret)
            {
                if self.items[hit.index].can_produce_caret_position
                    && self.item_is_direct_caret_target(arena, hit.index)
                {
                    return self
                        .caret_at_item(arena, hit.index, hit.local_point, CaretPositionType::Closest)
                        .map(|caret| {
                            caret.with_debug_rect_in_viewport(tree, callbacks, self.items[hit.index].context.spatial)
                        });
                }
                return self
                    .item_is_inline_adjacent_to_line(hit.index, line_index)
                    .then_some(caret);
            }
        }
        Some(caret)
    }

    /// The caret position at the start or end of the painted line holding the position. A visual line can span several
    /// DOM nodes and atomic inline boxes, so a text-node or block-element boundary is not necessarily a line boundary.
    pub(crate) fn caret_at_line_edge(
        &self,
        arena: &impl PaintRead,
        query: &FfiCaretPositionQuery,
        offset: usize,
        affinity_is_downstream: bool,
        edge: CaretPositionType,
    ) -> Option<FfiCaretAt> {
        let line_index = self.caret_line_for_position(arena, query, offset, affinity_is_downstream)?;
        self.caret_at_item(
            arena,
            self.item_at_line_edge(line_index, edge),
            CssPixelPoint::default(),
            edge,
        )
    }

    /// The caret position on the visually adjacent line within `scope`, preserving the requested inline coordinate.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn caret_on_adjacent_line(
        &self,
        main_thread: &crate::stage::MainThread,
        arena: &impl PaintRead,
        callbacks: &FfiHitTestQueryCallbacks,
        query: &FfiCaretPositionQuery,
        offset: usize,
        affinity_is_downstream: bool,
        direction: CaretLineDirection,
        inline_coordinate: CssPixels,
        scope: FfiNodeIdentity,
    ) -> Option<FfiCaretAt> {
        let line_index = self.caret_line_for_position(arena, query, offset, affinity_is_downstream)?;
        let (adjacent_line_index, point) = self.adjacent_line(
            main_thread,
            arena,
            callbacks,
            line_index,
            direction,
            inline_coordinate,
            scope,
        )?;
        // Reuse point-to-caret resolution after choosing the line so text, atomic boxes, and empty lines share one rule
        // for selecting the position closest to the preferred inline coordinate.
        self.caret_at_line(arena, adjacent_line_index, point, CaretPositionMode::Normal)
    }
}
