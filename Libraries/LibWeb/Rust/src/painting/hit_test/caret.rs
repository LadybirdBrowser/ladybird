/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::layout::LayoutNodeArena;
use crate::painting::host::{FfiCaretPositionQueryCallbacks, FfiHitTestQueryCallbacks};
use crate::painting::text_fragment::CaretMatch;

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
    pub block_container_margin_rect: Option<CssPixelRect>,
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
            block_container_margin_rect: None,
            is_before_point: false,
            contains_point_in_block_axis: false,
        }
    }
}

impl HitTestList {
    pub(crate) fn caret_line_for_position(
        &self,
        arena: &LayoutNodeArena,
        callbacks: &FfiCaretPositionQueryCallbacks,
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
                        self.item_position_match(arena, callbacks, item_index, offset, affinity_is_downstream);
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
        arena: &LayoutNodeArena,
        callbacks: &FfiCaretPositionQueryCallbacks,
        item_index: usize,
        offset: usize,
        affinity_is_downstream: bool,
    ) -> CaretMatch {
        let item = &self.items[item_index];
        match item.kind {
            HitTestItemKind::TextFragment => resolve::with_item_fragment(arena, item, |fragment| {
                let shell = arena.shell_if_live(fragment.layout_node);
                if shell.is_null() {
                    return CaretMatch::None;
                }
                if callbacks.shell_is_query_node(shell) {
                    return crate::painting::text_fragment::caret_match(fragment, offset, affinity_is_downstream);
                }
                if fragment.dom_start_offset_in_node == 0 && callbacks.query_boundary_descends_to_shell(shell) {
                    return CaretMatch::Direct;
                }
                if callbacks.query_boundary_follows_shell_end(shell, fragment.dom_end_offset_with_trailing_whitespace) {
                    return CaretMatch::Direct;
                }
                CaretMatch::None
            })
            .unwrap_or(CaretMatch::None),
            HitTestItemKind::EmptyLine => {
                let shell = arena.shell_if_live(item.caret_node);
                if !shell.is_null() && item.caret_offset == offset && callbacks.shell_is_query_node(shell) {
                    CaretMatch::Direct
                } else {
                    CaretMatch::None
                }
            }
            HitTestItemKind::EmptyEditable => {
                let shell = arena.shell_if_live(item.paintable);
                if !shell.is_null() && offset == 0 && callbacks.shell_is_query_node(shell) {
                    CaretMatch::Direct
                } else {
                    CaretMatch::None
                }
            }
            HitTestItemKind::Box => {
                let shell = arena.shell_if_live(item.paintable);
                if !shell.is_null() && callbacks.query_is_adjacent_to_shell(shell) {
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
        debug_assert!(self.derived_structures_built);
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
            if line.context != item.context || first_item.containing_block != item.containing_block {
                return false;
            }
            if first_item.caret_line_index.is_some() && item.caret_line_index.is_some() {
                return first_item.caret_line_index == item.caret_line_index;
            }
            if first_item.caret_line_index.is_none() && item.caret_line_index.is_none() {
                return rects_overlap_in_block_axis(line.rect, item.caret_rect, writing_mode);
            }
            false
        };
        // Atomic inline boxes are recorded during the background paint phase, while text fragments
        // are recorded during the foreground phase. Other boxes can therefore separate two items
        // from the same line in the caret item list.
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

    pub fn caret_item_for_line(
        &self,
        line_index: usize,
        local_point: CssPixelPoint,
        mode: CaretPositionMode,
    ) -> Option<(usize, CaretPositionType)> {
        debug_assert!(self.derived_structures_built);
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
            let block_distance =
                block_axis_distance_to_line_rect(Self::caret_line_rect_for_item(item), local_point, item_writing_mode);
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
        debug_assert!(self.derived_structures_built);
        let line = &self.caret_lines[line_index];
        let writing_mode = self.first_item_of_line(line).writing_mode;
        line_block_middle(line.rect, writing_mode)
    }

    pub fn item_is_inline_adjacent_to_line(&self, item_index: usize, line_index: usize) -> bool {
        debug_assert!(self.derived_structures_built);
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

    fn line_in_scope(&self, arena: &LayoutNodeArena, callbacks: &FfiHitTestQueryCallbacks, line_index: usize) -> bool {
        let line = &self.caret_lines[line_index];
        for caret_item_index in line.first_caret_item_index..=line.last_caret_item_index {
            let shell = self.item_target_shell(arena, self.caret_item_indices[caret_item_index]);
            if !shell.is_null() && callbacks.shell_in_scope(shell) {
                return true;
            }
        }
        false
    }

    pub(crate) fn find_closest_line(
        &self,
        arena: &LayoutNodeArena,
        callbacks: &FfiHitTestQueryCallbacks,
        point: CssPixelPoint,
        mode: CaretPositionMode,
        scoped: bool,
        respect_clip: bool,
    ) -> ClosestLine {
        debug_assert!(self.derived_structures_built);
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
            if scoped && !self.line_in_scope(arena, callbacks, line_index) {
                continue;
            }
            let line = self.caret_lines[line_index].clone();
            let Some(local) = local_float_point(callbacks, line.context, point, respect_clip) else {
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
                    let same_block_container = line.block_container_margin_rect.is_some()
                        && closest_line.block_container_margin_rect.is_some()
                        && line.block_container_margin_rect == closest_line.block_container_margin_rect;
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
                closest_line.block_container_margin_rect = line.block_container_margin_rect;
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
                closest_line_before_point.block_container_margin_rect = line.block_container_margin_rect;
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
                closest_line_after_point.block_container_margin_rect = line.block_container_margin_rect;
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
            let point_is_in_closest_line_block_container_margin = closest_line
                .block_container_margin_rect
                .is_some_and(|rect| block_coordinate < block_axis_end(rect, writing_mode));
            let lines_share_block_container_margin = closest_line.block_container_margin_rect.is_some()
                && closest_line_after_point.block_container_margin_rect.is_some()
                && closest_line.block_container_margin_rect == closest_line_after_point.block_container_margin_rect;
            // A point still inside the previous block container's margin box should not jump to
            // text in a different block container, even if that following line is close.
            if point_is_in_closest_line_block_container_margin && !lines_share_block_container_margin {
                return closest_line;
            }
            return closest_line_after_point;
        }
        closest_line
    }

    pub(crate) fn adjacent_line(
        &self,
        arena: &LayoutNodeArena,
        callbacks: &FfiHitTestQueryCallbacks,
        current_line_index: usize,
        direction: CaretLineDirection,
        inline_coordinate: CssPixels,
    ) -> Option<(usize, CssPixelPoint)> {
        // INTEROP: Vertical caret movement in Chromium, WebKit, and Gecko follows rendered line geometry rather than
        // DOM tree order. Rank candidates in the requested logical block direction, prefer the current line-producing
        // context, then minimize block distance and finally distance from the remembered inline coordinate.

        // Keep these coordinates fractional. Rounding line geometry before comparison can reorder candidates when
        // layout positions or device scaling produce subpixel line centers.
        debug_assert!(self.derived_structures_built);
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
            if line_index == current_line_index
                || line.context != current_line.context
                || !self.line_in_scope(arena, callbacks, line_index)
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
