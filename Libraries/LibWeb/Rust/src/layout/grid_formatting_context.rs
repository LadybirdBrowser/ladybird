/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Alignment {
    Normal,
    Start,
    End,
    Center,
    Stretch,
    Baseline,
    SelfStart,
    SelfEnd,
    Safe,
    Unsafe,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
}

pub(crate) fn inline_item_alignment(justify_self_value: u8, container_justify_items: u8) -> Alignment {
    match justify_self_value {
        justify_self::AUTO => match container_justify_items {
            justify_items::BASELINE => Alignment::Baseline,
            justify_items::CENTER => Alignment::Center,
            justify_items::END | justify_items::FLEX_END | justify_items::RIGHT => Alignment::End,
            justify_items::FLEX_START | justify_items::START | justify_items::LEFT => Alignment::Start,
            justify_items::NORMAL | justify_items::LEGACY => Alignment::Normal,
            justify_items::SAFE => Alignment::Safe,
            justify_items::SELF_END => Alignment::SelfEnd,
            justify_items::SELF_START => Alignment::SelfStart,
            justify_items::STRETCH => Alignment::Stretch,
            justify_items::UNSAFE => Alignment::Unsafe,
            _ => unreachable!("invalid justify-items value"),
        },
        justify_self::BASELINE => Alignment::Baseline,
        justify_self::CENTER => Alignment::Center,
        justify_self::END | justify_self::FLEX_END | justify_self::RIGHT => Alignment::End,
        justify_self::FLEX_START | justify_self::START | justify_self::LEFT => Alignment::Start,
        justify_self::NORMAL => Alignment::Normal,
        justify_self::SAFE => Alignment::Safe,
        justify_self::SELF_END => Alignment::SelfEnd,
        justify_self::SELF_START => Alignment::SelfStart,
        justify_self::STRETCH => Alignment::Stretch,
        justify_self::UNSAFE => Alignment::Unsafe,
        _ => unreachable!("invalid justify-self value"),
    }
}

pub(crate) fn block_item_alignment(align_self_value: u8, container_align_items: u8) -> Alignment {
    match align_self_value {
        align_self::AUTO => match container_align_items {
            align_items::BASELINE => Alignment::Baseline,
            align_items::CENTER => Alignment::Center,
            align_items::END | align_items::FLEX_END => Alignment::End,
            align_items::FLEX_START | align_items::START => Alignment::Start,
            align_items::NORMAL => Alignment::Normal,
            align_items::SAFE => Alignment::Safe,
            align_items::SELF_END => Alignment::SelfEnd,
            align_items::SELF_START => Alignment::SelfStart,
            align_items::STRETCH => Alignment::Stretch,
            align_items::UNSAFE => Alignment::Unsafe,
            _ => unreachable!("invalid align-items value"),
        },
        align_self::BASELINE => Alignment::Baseline,
        align_self::CENTER => Alignment::Center,
        align_self::END | align_self::FLEX_END => Alignment::End,
        align_self::FLEX_START | align_self::START => Alignment::Start,
        align_self::NORMAL => Alignment::Normal,
        align_self::SAFE => Alignment::Safe,
        align_self::SELF_END => Alignment::SelfEnd,
        align_self::SELF_START => Alignment::SelfStart,
        align_self::STRETCH => Alignment::Stretch,
        align_self::UNSAFE => Alignment::Unsafe,
        _ => unreachable!("invalid align-self value"),
    }
}

pub(crate) fn inline_content_alignment(value: u8) -> Alignment {
    match value {
        justify_content::NORMAL => Alignment::Normal,
        justify_content::START | justify_content::FLEX_START | justify_content::LEFT => Alignment::Start,
        justify_content::END | justify_content::FLEX_END | justify_content::RIGHT => Alignment::End,
        justify_content::CENTER => Alignment::Center,
        justify_content::SPACE_BETWEEN => Alignment::SpaceBetween,
        justify_content::SPACE_AROUND => Alignment::SpaceAround,
        justify_content::SPACE_EVENLY => Alignment::SpaceEvenly,
        justify_content::STRETCH => Alignment::Stretch,
        _ => unreachable!("invalid justify-content value"),
    }
}

pub(crate) fn block_content_alignment(value: u8) -> Alignment {
    match value {
        align_content::NORMAL => Alignment::Normal,
        align_content::START | align_content::FLEX_START => Alignment::Start,
        align_content::END | align_content::FLEX_END => Alignment::End,
        align_content::CENTER => Alignment::Center,
        align_content::SPACE_BETWEEN => Alignment::SpaceBetween,
        align_content::SPACE_AROUND => Alignment::SpaceAround,
        align_content::SPACE_EVENLY => Alignment::SpaceEvenly,
        align_content::STRETCH => Alignment::Stretch,
        _ => unreachable!("invalid align-content value"),
    }
}

pub(crate) fn content_start_offset(
    alignment: Alignment,
    container_size: CssPixels,
    tracks_and_gaps_size: CssPixels,
) -> CssPixels {
    let free_space = container_size - tracks_and_gaps_size;
    // CSS Align's automatic overflow alignment is unsafe for grid content
    // alignment, so preserve negative free space here.
    match alignment {
        Alignment::Center => free_space / 2,
        Alignment::SpaceAround | Alignment::SpaceEvenly => CssPixels::default().max(free_space) / 2,
        Alignment::End => free_space,
        _ => CssPixels::default(),
    }
}

pub(crate) fn distributed_gap_size(
    alignment: Alignment,
    container_size: CssPixels,
    track_size_sum: CssPixels,
    gap_count: usize,
    minimum_gap: CssPixels,
) -> CssPixels {
    if gap_count == 0 {
        return CssPixels::default();
    }
    let available = CssPixels::default().max(container_size - track_size_sum);
    let distributed = match alignment {
        Alignment::SpaceBetween => available / gap_count,
        Alignment::SpaceAround => available / gap_count.saturating_add(1),
        Alignment::SpaceEvenly => available / gap_count.saturating_add(2),
        _ => CssPixels::default(),
    };
    distributed.max(minimum_gap)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ItemAlignment {
    pub(crate) margin_start: CssPixels,
    pub(crate) margin_end: CssPixels,
    pub(crate) size: CssPixels,
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn align_item(
    original_size: CssPixels,
    size_is_auto: bool,
    is_replaced: bool,
    containing_block_size: CssPixels,
    margin_box_start: CssPixels,
    margin_box_end: CssPixels,
    used_margin_start: CssPixels,
    used_margin_end: CssPixels,
    margin_start_is_auto: bool,
    margin_end_is_auto: bool,
    alignment: Alignment,
) -> ItemAlignment {
    let mut result = ItemAlignment {
        margin_start: used_margin_start,
        margin_end: used_margin_end,
        size: original_size,
    };
    // https://drafts.csswg.org/css-grid/#auto-margins
    // Auto margins in either axis absorb positive free space prior to alignment via the box alignment
    // properties, thereby disabling the effects of any self-alignment properties in that axis.
    // Overflowing grid items resolve their auto margins to zero and overflow as specified by their box
    // alignment properties.
    let margin_space = containing_block_size - result.size - margin_box_start - margin_box_end;
    let absorbed = CssPixels::default().max(margin_space);
    if margin_start_is_auto && margin_end_is_auto {
        result.margin_start = absorbed / 2;
        result.margin_end = absorbed / 2;
    } else if margin_start_is_auto {
        result.margin_start = absorbed;
    } else if margin_end_is_auto {
        result.margin_end = absorbed;
    } else if size_is_auto && !is_replaced {
        result.size += margin_space;
    }

    // If auto margins absorbed positive free space, alignment properties have no effect in this dimension.
    if (margin_start_is_auto || margin_end_is_auto) && margin_space > CssPixels::default() {
        return result;
    }

    let alignment_space = containing_block_size - original_size - margin_box_start - margin_box_end;
    match alignment {
        Alignment::Center => {
            result.margin_start += alignment_space / 2;
            result.margin_end += alignment_space / 2;
            result.size = original_size;
        }
        Alignment::Baseline | Alignment::Start => {
            result.margin_end += alignment_space;
            result.size = original_size;
        }
        Alignment::End => {
            result.margin_start += alignment_space;
            result.size = original_size;
        }
        _ => {}
    }
    result
}

/// The pass-facing view of one stored track sizing function; sized breadths
/// borrow the computed size from the style group payload, which outlives the
/// pass.
#[derive(Clone, Copy, Debug)]
pub(crate) enum GridTrackBreadth {
    Auto,
    LengthPercentage(&'static ComputedSize),
    Flex(f64),
    MinContent,
    MaxContent,
    FitContent(&'static ComputedSize),
}

fn grid_track_breadth_view(breadth: &'static ComputedGridTrackBreadth) -> GridTrackBreadth {
    if breadth.is_flex {
        return GridTrackBreadth::Flex(breadth.flex_factor);
    }
    match breadth.size.kind {
        ComputedSizeKind::Auto => GridTrackBreadth::Auto,
        ComputedSizeKind::Calculated | ComputedSizeKind::Length | ComputedSizeKind::Percentage => {
            GridTrackBreadth::LengthPercentage(&breadth.size)
        }
        ComputedSizeKind::MinContent => GridTrackBreadth::MinContent,
        ComputedSizeKind::MaxContent => GridTrackBreadth::MaxContent,
        ComputedSizeKind::FitContent => GridTrackBreadth::FitContent(&breadth.size),
        ComputedSizeKind::None => unreachable!("grid track sizes cannot be none"),
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum GridTrackType {
    Explicit,
    Implicit,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum GridTrackState {
    Static,
    Repeat,
    Removed,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiUsedGridLine {
    pub names: *const usize,
    pub name_count: usize,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiUsedGridTrackList {
    pub is_subgrid: bool,
    pub lines: *const FfiUsedGridLine,
    pub line_count: usize,
    pub track_sizes: *const crate::layout::CssPixels,
    pub track_count: usize,
}

#[derive(PartialEq, Eq)]
pub(crate) struct GridLayoutLine {
    pub(crate) names: Vec<String>,
    pub(crate) start: crate::layout::CssPixels,
    pub(crate) breadth: crate::layout::CssPixels,
    pub(crate) type_: GridTrackType,
    pub(crate) number: u32,
    pub(crate) negative_number: i32,
}

#[derive(PartialEq, Eq)]
pub(crate) struct GridLayoutTrack {
    pub(crate) start: crate::layout::CssPixels,
    pub(crate) breadth: crate::layout::CssPixels,
    pub(crate) type_: GridTrackType,
    pub(crate) state: GridTrackState,
}

#[derive(PartialEq, Eq)]
pub(crate) struct GridLayoutArea {
    pub(crate) name: String,
    pub(crate) type_: GridTrackType,
    pub(crate) row_start: u32,
    pub(crate) row_end: u32,
    pub(crate) column_start: u32,
    pub(crate) column_end: u32,
}

#[derive(PartialEq, Eq)]
pub(crate) struct GridLayoutDimension {
    pub(crate) lines: Vec<GridLayoutLine>,
    pub(crate) tracks: Vec<GridLayoutTrack>,
}

#[derive(PartialEq, Eq)]
pub(crate) struct GridLayoutFragment {
    pub(crate) areas: Vec<GridLayoutArea>,
    pub(crate) columns: GridLayoutDimension,
    pub(crate) rows: GridLayoutDimension,
}

#[derive(PartialEq, Eq)]
pub(crate) struct GridLayoutData {
    pub(crate) direction: u8,
    pub(crate) writing_mode: u8,
    pub(crate) is_subgrid: bool,
    pub(crate) fragments: Vec<GridLayoutFragment>,
}

#[derive(PartialEq, Eq)]
pub(crate) struct OwnedUsedGridTrackList {
    pub(crate) is_subgrid: bool,
    pub(crate) lines: Vec<Vec<usize>>,
    pub(crate) track_sizes: Vec<crate::layout::CssPixels>,
}

impl OwnedUsedGridTrackList {
    fn ffi_lines(&self) -> Vec<FfiUsedGridLine> {
        self.lines
            .iter()
            .map(|names| FfiUsedGridLine {
                names: names.as_ptr(),
                name_count: names.len(),
            })
            .collect()
    }
}

#[derive(PartialEq, Eq)]
pub(crate) struct OwnedUsedGridTracks {
    pub(crate) columns: OwnedUsedGridTrackList,
    pub(crate) rows: OwnedUsedGridTrackList,
}

impl OwnedUsedGridTracks {
    pub(crate) fn with_ffi_views(&self, callback: impl FnOnce(&FfiUsedGridTrackList, &FfiUsedGridTrackList)) {
        let column_lines = self.columns.ffi_lines();
        let row_lines = self.rows.ffi_lines();
        let columns = FfiUsedGridTrackList {
            is_subgrid: self.columns.is_subgrid,
            lines: column_lines.as_ptr(),
            line_count: column_lines.len(),
            track_sizes: self.columns.track_sizes.as_ptr(),
            track_count: self.columns.track_sizes.len(),
        };
        let rows = FfiUsedGridTrackList {
            is_subgrid: self.rows.is_subgrid,
            lines: row_lines.as_ptr(),
            line_count: row_lines.len(),
            track_sizes: self.rows.track_sizes.as_ptr(),
            track_count: self.rows.track_sizes.len(),
        };
        callback(&columns, &rows);
    }
}

// https://drafts.csswg.org/css-grid/#overlarge-grids
// Since memory is limited, UAs may clamp the possible size of the implicit grid to be within a UA-defined limit
// (which should accommodate lines in the range [-10000, 10000]), dropping all lines outside that limit. If a grid item
// is placed outside this limit, its grid area must be clamped to within this limited grid.
const MAX_GRID_LINE_NUMBER: i32 = 10_000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ResolvedPlacementPosition {
    pub(crate) start: i32,
    pub(crate) end: i32,
    pub(crate) span: usize,
}

fn placement_kind(placement: ComputedGridPlacement) -> ComputedGridPlacementKind {
    assert!(placement.kind <= ComputedGridPlacementKind::Span as u8);
    // SAFETY: The range check covers every repr(u8) variant.
    unsafe { std::mem::transmute(placement.kind) }
}

fn is_positioned(placement: ComputedGridPlacement) -> bool {
    placement_kind(placement) == ComputedGridPlacementKind::Line
}

fn is_span(placement: ComputedGridPlacement) -> bool {
    placement_kind(placement) == ComputedGridPlacementKind::Span
}

fn clamped_line_number(placement: ComputedGridPlacement) -> Option<i32> {
    placement
        .has_line_number
        .then(|| placement.line_number.clamp(-MAX_GRID_LINE_NUMBER, MAX_GRID_LINE_NUMBER))
}

fn clamped_span(placement: ComputedGridPlacement) -> usize {
    placement.line_number.clamp(1, MAX_GRID_LINE_NUMBER) as usize
}

/// Resolves one axis exactly as `GridFormattingContext::resolve_grid_position`.
/// Names are pre-interned in F1, including the area `-start`/`-end` aliases.
pub(crate) fn resolve_placement_position(
    placement_start: ComputedGridPlacement,
    placement_end: ComputedGridPlacement,
    placement_names: &[usize],
    lines: &[Vec<LineName>],
    explicit_line_count: usize,
    occupation_track_count: usize,
) -> ResolvedPlacementPosition {
    let start_line_number = clamped_line_number(placement_start);
    let end_line_number = clamped_line_number(placement_end);
    let mut result = ResolvedPlacementPosition {
        start: 0,
        end: 0,
        span: 1,
    };

    if let Some(number) = start_line_number {
        result.start = if number > 0 {
            number - 1
        } else {
            explicit_line_count as i32 + number
        };
    }
    if let Some(number) = end_line_number {
        result.end = number - 1;
    }
    if result.end < 0 {
        result.end = occupation_track_count as i32 + result.end + 2;
    }

    // FIXME: If a name is given as a <custom-ident>, only lines with that name are counted. If not enough lines with
    //        that name exist, all implicit grid lines on the side of the explicit grid corresponding to the search
    //        direction are assumed to have that name for the purpose of counting this span.
    if is_span(placement_end) {
        result.span = clamped_span(placement_end);
    }
    if is_span(placement_start) {
        result.span = clamped_span(placement_start);
        result.start = result.end - result.span as i32;
        // FIXME: Remove me once have implemented spans overflowing into negative indexes, e.g., grid-row: span 2 / 1
        result.start = result.start.max(0);
    }

    if placement_end.has_name {
        let number = end_line_number.unwrap_or(1);
        result.end = placement_names
            .get(placement_end.implicit_end_name_index as usize)
            .and_then(|name| nth_named_line(lines, *name, number))
            .or_else(|| {
                placement_names
                    .get(placement_end.name_index as usize)
                    .and_then(|name| nth_named_line(lines, *name, number))
            })
            .unwrap_or(explicit_line_count as i32);
        if !placement_start.has_line_number {
            result.start = result.end - 1;
        }
    }

    if placement_start.has_name {
        let number = start_line_number.unwrap_or(1);
        result.start = placement_names
            .get(placement_start.implicit_start_name_index as usize)
            .and_then(|name| nth_named_line(lines, *name, number))
            .or_else(|| {
                placement_names
                    .get(placement_start.name_index as usize)
                    .and_then(|name| nth_named_line(lines, *name, number))
            })
            .unwrap_or(explicit_line_count as i32);
    }

    if !is_positioned(placement_start) && is_positioned(placement_end) && !is_span(placement_end) {
        result.start = result.end - result.span as i32;
    }

    if is_positioned(placement_start) && is_positioned(placement_end) {
        if result.start > result.end {
            std::mem::swap(&mut result.start, &mut result.end);
        }
        if result.start != result.end {
            result.span = (result.end - result.start) as usize;
        } else {
            result.span = 1;
            result.end = result.start + 1;
        }
    }

    // If the placement contains two spans, remove the one contributed by the end grid-placement
    // property.
    if is_span(placement_start) && is_span(placement_end) {
        result.span = clamped_span(placement_start);
    }
    result
}

pub(crate) fn resolve_placement_span(
    placement_start: ComputedGridPlacement,
    placement_end: ComputedGridPlacement,
    automatic_subgrid_span: Option<usize>,
) -> usize {
    if is_span(placement_start) {
        return clamped_span(placement_start);
    }
    if is_span(placement_end) {
        return clamped_span(placement_end);
    }
    if !(is_positioned(placement_start) && is_positioned(placement_end))
        && let Some(span) = automatic_subgrid_span
    {
        // https://drafts.csswg.org/css-grid-2/#grid-placement
        // Otherwise, its grid span is automatic: if it is subgridded in
        // that axis, its grid span is determined from its line-name-list;
        // otherwise its grid span is 1.
        //
        // https://drafts.csswg.org/css-grid-2/#subgrid-span
        // If it has an automatic grid span, then its used grid span is
        // taken from the number of explicit tracks specified for that axis
        // by its grid-template-* properties, floored at one.
        return span.max(1);
    }
    1
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AutoFlowAxis {
    Row,
    Column,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ResolvedAxisPlacement {
    pub(crate) start: Option<i32>,
    pub(crate) span: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PlacementInput {
    pub(crate) id: usize,
    pub(crate) order: i32,
    pub(crate) row: ResolvedAxisPlacement,
    pub(crate) column: ResolvedAxisPlacement,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PlacedItem {
    pub(crate) id: usize,
    pub(crate) row: i32,
    pub(crate) row_span: usize,
    pub(crate) column: i32,
    pub(crate) column_span: usize,
}

#[derive(Debug)]
pub(crate) struct PlacementResult {
    pub(crate) items: Vec<PlacedItem>,
    pub(crate) column_count: usize,
    pub(crate) row_count: usize,
    pub(crate) explicit_column_start: usize,
    pub(crate) explicit_row_start: usize,
}

#[derive(Default)]
pub(crate) struct OccupationGrid {
    occupied: HashSet<(i32, i32)>,
    min_column_index: i32,
    max_column_index: i32,
    min_row_index: i32,
    max_row_index: i32,
}

impl OccupationGrid {
    pub(crate) fn new(column_count: usize, row_count: usize) -> Self {
        Self {
            occupied: HashSet::default(),
            min_column_index: 0,
            max_column_index: column_count.saturating_sub(1) as i32,
            min_row_index: 0,
            max_row_index: row_count.saturating_sub(1) as i32,
        }
    }

    pub(crate) fn set_occupied(&mut self, column_start: i32, column_end: i32, row_start: i32, row_end: i32) {
        for row in row_start..row_end {
            for column in column_start..column_end {
                self.min_column_index = self.min_column_index.min(column);
                self.max_column_index = self.max_column_index.max(column);
                self.min_row_index = self.min_row_index.min(row);
                self.max_row_index = self.max_row_index.max(row);
                self.occupied.insert((column, row));
            }
        }
    }

    pub(crate) fn is_occupied(&self, column: i32, row: i32) -> bool {
        self.occupied.contains(&(column, row))
    }

    pub(crate) fn is_area_occupied(
        &self,
        column_start: i32,
        row_start: i32,
        column_span: usize,
        row_span: usize,
    ) -> bool {
        for row in row_start..row_start + row_span as i32 {
            for column in column_start..column_start + column_span as i32 {
                if self.is_occupied(column, row) {
                    return true;
                }
            }
        }
        false
    }

    fn find_unoccupied_grid_area(
        &self,
        flow: AutoFlowAxis,
        column: &mut i32,
        row: &mut i32,
        column_span: usize,
        row_span: usize,
    ) {
        match flow {
            AutoFlowAxis::Row => {
                // Row-flow: columns are the inner (minor) axis, rows are the outer (major) axis.
                while *row <= self.max_row_index {
                    while *column <= self.max_column_index {
                        let minor_axis_fits = *column + column_span as i32 - 1 <= self.max_column_index;
                        if minor_axis_fits && !self.is_area_occupied(*column, *row, column_span, row_span) {
                            return;
                        }
                        *column += 1;
                    }
                    *row += 1;
                    *column = self.min_column_index;
                }
            }
            AutoFlowAxis::Column => {
                // Column-flow: rows are the inner (minor) axis, columns are the outer (major) axis.
                while *column <= self.max_column_index {
                    while *row <= self.max_row_index {
                        let minor_axis_fits = *row + row_span as i32 - 1 <= self.max_row_index;
                        if minor_axis_fits && !self.is_area_occupied(*column, *row, column_span, row_span) {
                            return;
                        }
                        *row += 1;
                    }
                    *column += 1;
                    *row = self.min_row_index;
                }
            }
        }
    }
}

fn record(grid: &mut OccupationGrid, output: &mut Vec<PlacedItem>, item: PlacementInput, row: i32, column: i32) {
    grid.set_occupied(
        column,
        column + item.column.span as i32,
        row,
        row + item.row.span as i32,
    );
    output.push(PlacedItem {
        id: item.id,
        row,
        row_span: item.row.span,
        column,
        column_span: item.column.span,
    });
}

pub(crate) fn place_items_with_grid(
    items: &[PlacementInput],
    explicit_column_count: usize,
    explicit_row_count: usize,
    flow: AutoFlowAxis,
    dense: bool,
) -> PlacementResult {
    // https://drafts.csswg.org/css-grid/#overview-placement
    // 2.2. Placing Items
    // The contents of the grid container are organized into individual grid items (analogous to
    // flex items), which are then assigned to predefined areas in the grid. They can be explicitly
    // placed using coordinates through the grid-placement properties or implicitly placed into
    // empty areas using auto-placement.
    //
    // https://drafts.csswg.org/css-grid/#auto-placement-algo
    // 8.5. Grid Item Placement Algorithm
    let mut ordered_indices = (0..items.len()).collect::<Vec<_>>();
    ordered_indices.sort_by_key(|index| (items[*index].order, *index));

    let mut remaining = vec![true; items.len()];
    let mut grid = OccupationGrid::new(explicit_column_count, explicit_row_count);
    let mut output = Vec::with_capacity(items.len());

    // FIXME: 0. Generate anonymous grid items

    // 1. Position items that are definite in both axes.
    // 1. Position anything that's not auto-positioned.
    for &index in &ordered_indices {
        let item = items[index];
        if let (Some(row), Some(column)) = (item.row.start, item.column.start) {
            record(&mut grid, &mut output, item, row, column);
            remaining[index] = false;
        }
    }

    // 2. Position items locked to a row.
    // 2. Process the items locked to a given row.
    for &index in &ordered_indices {
        if !remaining[index] {
            continue;
        }
        let item = items[index];
        let Some(row) = item.row.start else {
            continue;
        };
        if item.column.start.is_some() {
            continue;
        }

        let mut column = 0;
        let mut found = false;
        while column <= grid.max_column_index {
            if !grid.is_area_occupied(column, row, item.column.span, item.row.span) {
                found = true;
                break;
            }
            column += 1;
        }
        if !found {
            column = grid.max_column_index + 1;
        }
        record(&mut grid, &mut output, item, row, column);
        remaining[index] = false;
    }

    // 3. Make the implicit grid wide enough for every auto-column span.
    // 3. Determine the columns in the implicit grid.
    // NOTE: "implicit grid" here is the same as the m_occupation_grid

    // 3.1. Start with the columns from the explicit grid.
    // NOTE: Done in step 1.

    // 3.2. Among all the items with a definite column position (explicitly positioned items, items
    // positioned in the previous step, and items not yet positioned but with a definite column) add
    // columns to the beginning and end of the implicit grid as necessary to accommodate those items.
    // NOTE: "Explicitly positioned items" and "items positioned in the previous step" done in step 1
    // and 2, respectively. Adding columns for "items not yet positioned but with a definite column"
    // will be done in step 4.

    // 3.3. If the largest column span among all the items without a definite column position is larger
    // than the width of the implicit grid, add columns to the end of the implicit grid to accommodate
    // that column span.
    for &index in &ordered_indices {
        if !remaining[index] {
            continue;
        }
        let span = items[index].column.span;
        if span.saturating_sub(1) > grid.max_column_index as usize {
            grid.max_column_index = span.saturating_sub(1) as i32;
        }
    }

    // 4. Position the remaining items in order-modified document order.
    // 4. Position the remaining grid items.
    // For each grid item that hasn't been positioned by the previous steps, in order-modified document
    // order:
    let mut cursor_column = 0;
    let mut cursor_row = 0;
    for &index in &ordered_indices {
        if !remaining[index] {
            continue;
        }
        let item = items[index];
        if let Some(column) = item.column.start {
            // 4.1.1 / 4.2.1: Item with definite column position.
            if dense {
                // Dense: reset row cursor to start of implicit grid.
                cursor_row = grid.min_row_index;
            } else if column < cursor_column {
                // Sparse: if column moved backward, bump row.
                cursor_row += 1;
            }
            cursor_column = column;
            // https://drafts.csswg.org/css-grid-2/#auto-placement-algo
            // Increment the auto-placement cursor's row position until a value is found
            // where the grid item does not overlap any occupied grid cells (creating
            // new rows in the implicit grid as necessary).
            //
            // https://drafts.csswg.org/css-grid-2/#subgrid-implicit
            // The subgrid does not have any implicit grid tracks in the subgridded
            // dimension(s). Hypothetical implicit grid lines are used to resolve
            // placement as usual when the explicit grid does not have enough lines;
            // however each grid item's grid area is clamped to the subgrid's explicit
            // grid.
            while grid.is_area_occupied(cursor_column, cursor_row, item.column.span, item.row.span) {
                cursor_row += 1;
            }
            record(&mut grid, &mut output, item, cursor_row, cursor_column);
        } else {
            // 4.1.2 / 4.2.2: Item with auto position in both axes.
            if dense {
                // Dense: reset cursor to start of implicit grid.
                cursor_column = grid.min_column_index;
                cursor_row = grid.min_row_index;
            }
            // 4.1.2.1. Increment the column position of the auto-placement cursor until either this item's grid
            // area does not overlap any occupied grid cells, or the cursor's column position, plus the item's
            // column span, overflow the number of columns in the implicit grid, as determined earlier in this
            // algorithm.
            grid.find_unoccupied_grid_area(
                flow,
                &mut cursor_column,
                &mut cursor_row,
                item.column.span,
                item.row.span,
            );
            // 4.1.2.2. If a non-overlapping position was found in the previous step, set the item's row-start
            // and column-start lines to the cursor's position. Otherwise, increment the auto-placement cursor's
            // row position (creating new rows in the implicit grid as necessary), set its column position to the
            // start-most column line in the implicit grid, and return to the previous step.
            let column = cursor_column;
            let row = cursor_row;
            // NB: The cursor's major-axis position must stay at the item's start line, so that subsequent items
            //     keep packing into the same row (or column, for column flow) before moving on. Only advance the
            //     minor-axis position past the item.
            match flow {
                AutoFlowAxis::Row => cursor_column += item.column.span as i32,
                AutoFlowAxis::Column => cursor_row += item.row.span as i32,
            }
            record(&mut grid, &mut output, item, row, column);
        }
    }

    let explicit_column_start = grid.min_column_index.unsigned_abs() as usize;
    let explicit_row_start = grid.min_row_index.unsigned_abs() as usize;
    let column_count = explicit_column_start
        .saturating_add(grid.max_column_index.max(0) as usize)
        .saturating_add(1);
    let row_count = explicit_row_start
        .saturating_add(grid.max_row_index.max(0) as usize)
        .saturating_add(1);
    for item in &mut output {
        item.row -= grid.min_row_index;
        item.column -= grid.min_column_index;
    }
    // NOTE: When final implicit grid sizes are known, we can offset their positions so leftmost grid track has 0 index.
    PlacementResult {
        items: output,
        column_count,
        row_count,
        explicit_column_start,
        explicit_row_start,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Axis {
    Column,
    Row,
}

impl Axis {
    fn is_column(self) -> bool {
        self == Self::Column
    }

    // Picks whichever of the two per-axis values belongs to this axis.
    fn select<T>(self, column: T, row: T) -> T {
        if self.is_column() { column } else { row }
    }

    fn sizing_axis(self) -> SizingAxis {
        self.select(SizingAxis::Inline, SizingAxis::Block)
    }

    fn opposite(self) -> Self {
        self.select(Self::Row, Self::Column)
    }
}

#[derive(Clone, Copy)]
struct GridItem {
    box_: Node,
    row: i32,
    row_span: usize,
    column: i32,
    column_span: usize,
    // Accumulated while projecting a nested subgrid item into an ancestor's track
    // sizing without modifying the UsedValues owned by the nested grid context.
    extra_margin_top: CssPixels,
    extra_margin_right: CssPixels,
    extra_margin_bottom: CssPixels,
    extra_margin_left: CssPixels,
}

impl GridItem {
    fn placement(self) -> GridItemPlacement {
        GridItemPlacement {
            row: self.row,
            row_span: self.row_span,
            column: self.column,
            column_span: self.column_span,
            extra_margin_top: self.extra_margin_top,
            extra_margin_right: self.extra_margin_right,
            extra_margin_bottom: self.extra_margin_bottom,
            extra_margin_left: self.extra_margin_left,
        }
    }

    fn position(self, axis: Axis) -> i32 {
        self.placement().position(axis)
    }

    fn span(self, axis: Axis) -> usize {
        self.placement().span(axis)
    }
}

pub(crate) struct GridFormattingContext {
    purpose: formatting_context::LayoutPurpose,
    records: std::rc::Rc<RunRecords>,
    grid_container: Node,
    derived_baselines_of_root_box: DerivedBaselines,
    parent_grid: Option<ParentGridData>,
    layout_mode: LayoutMode,
    callbacks: FfiLayoutFcCallbacks,
    should_collect_devtools_layout_data: bool,
    treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
    fragments: Option<std::rc::Rc<fragment_tree::RunFragmentBuilder>>,
    available_space: Option<AvailableSpace>,
    layout_input: Option<LayoutInput>,
    column_lines: Vec<Vec<LineName>>,
    row_lines: Vec<Vec<LineName>>,
    columns: Vec<Track>,
    rows: Vec<Track>,
    column_gaps: Vec<Track>,
    row_gaps: Vec<Track>,
    items: Vec<GridItem>,
    explicit_column_line_count: usize,
    explicit_row_line_count: usize,
    explicit_column_start: usize,
    explicit_row_start: usize,
    subgridded_columns: bool,
    subgridded_rows: bool,
    automatic_content_block_size: CssPixels,
    row_alignment_container_size: CssPixels,
    use_row_alignment_container_size: bool,
}

#[derive(Clone, Copy)]
struct GridItemPlacement {
    row: i32,
    row_span: usize,
    column: i32,
    column_span: usize,
    extra_margin_top: CssPixels,
    extra_margin_right: CssPixels,
    extra_margin_bottom: CssPixels,
    extra_margin_left: CssPixels,
}

impl GridItemPlacement {
    fn position(self, axis: Axis) -> i32 {
        if axis.is_column() { self.column } else { self.row }
    }

    fn span(self, axis: Axis) -> usize {
        axis.select(self.column_span, self.row_span)
    }
}

#[derive(Clone)]
struct ParentGridData {
    placement_of_this_container: Option<GridItemPlacement>,
    column_lines: Vec<Vec<LineName>>,
    row_lines: Vec<Vec<LineName>>,
    columns: Vec<Track>,
    rows: Vec<Track>,
    column_gaps: Vec<Track>,
    row_gaps: Vec<Track>,
}

impl ParentGridData {
    fn for_child_container(parent: &GridFormattingContext, child_container: Node) -> Self {
        Self {
            placement_of_this_container: parent
                .items
                .iter()
                .find(|item| item.box_ == child_container)
                .map(|item| item.placement()),
            column_lines: parent.column_lines.clone(),
            row_lines: parent.row_lines.clone(),
            columns: parent.columns.clone(),
            rows: parent.rows.clone(),
            column_gaps: parent.column_gaps.clone(),
            row_gaps: parent.row_gaps.clone(),
        }
    }
}

/// Conservative superset of is_subgridded() for callers outside a live grid run
/// (the fc-run-cache probe): a declared subgrid axis counts regardless of the
/// parent-grid placement check only a run in progress can make.
pub(super) fn grid_template_declares_a_subgrid_axis(callbacks: &FfiLayoutFcCallbacks, box_: Node) -> bool {
    let grid_style = ComputedValuesView::new(&callbacks.style_payloads(box_).groups).grid_values();
    grid_style.template_columns.is_subgrid || grid_style.template_rows.is_subgrid
}

impl GridFormattingContext {
    pub(crate) fn new(run: &FormattingContextRun, parent_grid: Option<&GridFormattingContext>) -> Self {
        let grid_container = run.box_;
        Self {
            purpose: run.purpose,
            records: run.records.clone(),
            grid_container,
            derived_baselines_of_root_box: DerivedBaselines::default(),
            parent_grid: parent_grid.map(|parent| ParentGridData::for_child_container(parent, grid_container)),
            layout_mode: run.layout_mode,
            callbacks: run.callbacks,
            should_collect_devtools_layout_data: run.should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root: run
                .treat_block_axis_percentage_insets_as_auto_beyond_root,
            fragments: run.fragments.clone(),
            available_space: None,
            layout_input: None,
            column_lines: Vec::new(),
            row_lines: Vec::new(),
            columns: Vec::new(),
            rows: Vec::new(),
            column_gaps: Vec::new(),
            row_gaps: Vec::new(),
            items: Vec::new(),
            explicit_column_line_count: 0,
            explicit_row_line_count: 0,
            explicit_column_start: 0,
            explicit_row_start: 0,
            subgridded_columns: false,
            subgridded_rows: false,
            automatic_content_block_size: CssPixels::default(),
            row_alignment_container_size: CssPixels::default(),
            use_row_alignment_container_size: false,
        }
    }

    fn formatting_context_run(&self) -> FormattingContextRun {
        FormattingContextRun {
            purpose: self.purpose,
            records: self.records.clone(),
            box_: self.grid_container,
            layout_mode: self.layout_mode,
            callbacks: self.callbacks,
            should_collect_devtools_layout_data: self.should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root: self
                .treat_block_axis_percentage_insets_as_auto_beyond_root,
            fragments: self.fragments.clone(),
            previous_line_data: None,
        }
    }

    fn reset_for_run(&mut self, input: LayoutInput) {
        self.available_space = Some(input.available_space);
        self.layout_input = Some(input);
        self.column_lines.clear();
        self.row_lines.clear();
        self.columns.clear();
        self.rows.clear();
        self.column_gaps.clear();
        self.row_gaps.clear();
        self.items.clear();
        self.explicit_column_line_count = 0;
        self.explicit_row_line_count = 0;
        self.explicit_column_start = 0;
        self.explicit_row_start = 0;
        self.subgridded_columns = false;
        self.subgridded_rows = false;
        self.automatic_content_block_size = CssPixels::default();
        self.row_alignment_container_size = CssPixels::default();
        self.use_row_alignment_container_size = false;
    }

    fn container_used(&self) -> std::rc::Rc<UsedValues> {
        self.records.used_values(self.grid_container)
    }

    fn used(&self, item: GridItem) -> std::rc::Rc<UsedValues> {
        self.records.used_values(item.box_)
    }
    fn style(&self, node: Node) -> StyleValues<'static> {
        StyleValues::for_node(&self.callbacks, node)
    }

    fn facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.callbacks, node)
    }

    /// The node's computed grid style group, read in place. The payload
    /// outlives the pass because the node's ComputedValues keep it alive and
    /// style containers are only replaced between passes.
    fn grid_style(&self, node: Node) -> &'static GridValues {
        ComputedValuesView::new(&self.callbacks.style_payloads(node).groups).grid_values()
    }

    fn sizing(&self) -> sizing_context::SizingContext {
        sizing_context::SizingContext::new(self.purpose, self.records.clone(), self.callbacks)
    }

    fn parent_grid(&self) -> Option<&ParentGridData> {
        self.parent_grid.as_ref()
    }

    fn parent_grid_placement(&self) -> Option<GridItemPlacement> {
        self.parent_grid()?.placement_of_this_container
    }

    fn is_subgridded(&self, axis: Axis, grid_style: &GridValues) -> bool {
        // https://drafts.csswg.org/css-grid-2/#subgrid-listing
        // If there is no parent grid, or if the grid container is otherwise forced
        // to establish an independent formatting context, the used value is
        // the initial value, grid-template-rows/none, and the grid container is not
        // a subgrid.
        // FIXME: Also reject subgrid here when the grid container is forced to
        // establish an independent formatting context.
        let list = axis.select(grid_style.template_columns, grid_style.template_rows);
        list.is_subgrid && self.parent_grid_placement().is_some()
    }

    fn container_is_subgridded(&self, axis: Axis) -> bool {
        axis.select(self.subgridded_columns, self.subgridded_rows)
    }

    fn cache_subgrid_axes(&mut self, grid_style: &GridValues) {
        self.subgridded_columns = self.is_subgridded(Axis::Column, grid_style);
        self.subgridded_rows = self.is_subgridded(Axis::Row, grid_style);
    }

    fn axis_available(&self, axis: Axis) -> AvailableSize {
        let space = self.available_space.unwrap();
        axis.select(space.inline_size, space.block_size)
    }

    fn axis_gap_value(&self, axis: Axis) -> &'static ComputedGap {
        let style = self.style(self.grid_container);
        axis.select(style.column_gap(), style.row_gap())
    }

    fn parent_gap_size_for_subgrid(&self, axis: Axis) -> CssPixels {
        let Some(parent) = self.parent_grid() else {
            return CssPixels::default();
        };
        let Some(item) = self.parent_grid_placement() else {
            return CssPixels::default();
        };
        if item.span(axis) <= 1 {
            return CssPixels::default();
        }
        let gaps = axis.select(&parent.column_gaps, &parent.row_gaps);
        if gaps.is_empty() {
            return CssPixels::default();
        }
        let position = item.position(axis);
        if position >= 0
            && let Some(gap) = gaps.get(position as usize)
        {
            return gap.base_size;
        }
        gaps[0].base_size
    }

    fn resolved_gap(&self, axis: Axis, available: AvailableSize) -> CssPixels {
        if self.container_is_subgridded(axis) {
            // https://drafts.csswg.org/css-grid-2/#subgrid-gaps
            // The parent's grid tracks will be sized as specified, and the
            // subgrid's gutters will visually center-align with the parent grid's
            // gutters.
            return self.parent_gap_size_for_subgrid(axis);
        }
        self.axis_gap_value(axis).to_px(available.to_px_or_zero())
    }

    fn subgrid_gap_extra_margin(&self, axis: Axis, available: AvailableSize) -> CssPixels {
        if !self.container_is_subgridded(axis) {
            return CssPixels::default();
        }
        let gap = self.axis_gap_value(axis);
        if gap.is_normal() {
            // https://drafts.csswg.org/css-grid-2/#subgrid-gaps
            // A value of normal indicates that the subgrid has the same size gutters
            // as its parent grid, i.e. the applied difference is zero.
            return CssPixels::default();
        }
        // https://drafts.csswg.org/css-grid-2/#subgrid-gaps
        // Half the size of the difference between the subgrid's gutters
        // (row-gap/column-gap) and its parent grid's gutters is applied as an extra
        // layer of (potentially negative) margin to the items not at those edges.
        (gap.to_px(available.to_px_or_zero()) - self.parent_gap_size_for_subgrid(axis)) / 2
    }

    fn project_parent_grid_areas(
        &self,
        columns: &mut [Vec<LineName>],
        rows: &mut [Vec<LineName>],
        column_is_subgrid: bool,
        row_is_subgrid: bool,
    ) {
        // https://drafts.csswg.org/css-grid-2/#subgrid-area-inheritance
        // When a subgrid overlaps a named grid area in its parent that was created by a
        // grid-template-areas property declaration, implicitly-assigned line names are assigned to represent
        // the parent's named grid area within the subgrid.
        //
        // Note: If a named grid area only partially overlaps the subgrid, its implicitly-assigned line names
        // will be assigned to the first and/or last line of the subgrid such that a named grid area exists
        // representing that partially overlapped area of the subgrid; thus the line name assignments of the
        // subgrid might not always correspond exactly to the line name assignments of the parent grid.
        #[derive(Clone, Copy)]
        struct Area {
            name_raw: usize,
            start_name_raw: usize,
            end_name_raw: usize,
            row_start: Option<usize>,
            row_end: Option<usize>,
            column_start: Option<usize>,
            column_end: Option<usize>,
        }

        // Names the two lines a parent grid area contributes to a subgrid, in subgrid-local coordinates.
        fn name_subgrid_area_lines(lines: &mut [Vec<LineName>], area: Area, start: i32, end: i32) {
            for (index, raw, area_is_start) in [(start, area.start_name_raw, true), (end, area.end_name_raw, false)] {
                let Some(line) = lines.get_mut(index as usize) else {
                    continue;
                };
                line.push(LineName {
                    name_index: crate::layout::GRID_NO_INDEX,
                    raw,
                    implicit: true,
                    adopted_from_parent: false,
                    area_name_raw: area.name_raw,
                    area_is_start,
                });
            }
        }

        let Some(parent) = self.parent_grid() else {
            return;
        };
        let Some(parent_item) = self.parent_grid_placement() else {
            return;
        };
        let mut areas = Vec::<Area>::new();
        for (axis, lines) in [(Axis::Column, &parent.column_lines), (Axis::Row, &parent.row_lines)] {
            for (line_index, names) in lines.iter().enumerate() {
                for name in names.iter().filter(|name| name.implicit && name.area_name_raw != 0) {
                    let area_index = areas
                        .iter()
                        .position(|area| area.name_raw == name.area_name_raw)
                        .unwrap_or_else(|| {
                            areas.push(Area {
                                name_raw: name.area_name_raw,
                                start_name_raw: 0,
                                end_name_raw: 0,
                                row_start: None,
                                row_end: None,
                                column_start: None,
                                column_end: None,
                            });
                            areas.len() - 1
                        });
                    let area = &mut areas[area_index];
                    let coordinate = match (axis, name.area_is_start) {
                        (Axis::Column, true) => &mut area.column_start,
                        (Axis::Column, false) => &mut area.column_end,
                        (Axis::Row, true) => &mut area.row_start,
                        (Axis::Row, false) => &mut area.row_end,
                    };
                    *coordinate = Some(match (*coordinate, name.area_is_start) {
                        (Some(old), true) => old.min(line_index),
                        (Some(old), false) => old.max(line_index),
                        (None, _) => line_index,
                    });
                    if name.area_is_start {
                        area.start_name_raw = name.raw;
                    } else {
                        area.end_name_raw = name.raw;
                    }
                }
            }
        }

        let subgrid_column_start = parent_item.position(Axis::Column);
        let subgrid_column_end = subgrid_column_start + parent_item.span(Axis::Column) as i32;
        let subgrid_row_start = parent_item.position(Axis::Row);
        let subgrid_row_end = subgrid_row_start + parent_item.span(Axis::Row) as i32;
        let overlaps = |start_a: i32, end_a: i32, start_b: i32, end_b: i32| start_a.max(start_b) < end_a.min(end_b);

        for area in areas {
            let (Some(row_start), Some(row_end), Some(column_start), Some(column_end)) =
                (area.row_start, area.row_end, area.column_start, area.column_end)
            else {
                continue;
            };
            let row_start = row_start as i32;
            let row_end = row_end as i32;
            let column_start = column_start as i32;
            let column_end = column_end as i32;
            if !overlaps(row_start, row_end, subgrid_row_start, subgrid_row_end)
                || !overlaps(column_start, column_end, subgrid_column_start, subgrid_column_end)
            {
                continue;
            }

            if column_is_subgrid {
                let start = column_start.max(subgrid_column_start) - subgrid_column_start;
                let end = column_end.min(subgrid_column_end) - subgrid_column_start;
                name_subgrid_area_lines(columns, area, start, end);
            }
            if row_is_subgrid {
                let start = row_start.max(subgrid_row_start) - subgrid_row_start;
                let end = row_end.min(subgrid_row_end) - subgrid_row_start;
                name_subgrid_area_lines(rows, area, start, end);
            }
        }
    }

    fn automatic_repeat_count(
        &self,
        source: TrackListSource,
        entry: &crate::layout::ComputedGridTrackEntry,
        axis: Axis,
    ) -> usize {
        // https://www.w3.org/TR/css-grid-2/#auto-repeat
        // 7.2.3.2. Repeat-to-fill: auto-fill and auto-fit repetitions
        // On a subgridded axis, the auto-fill keyword is only valid once per <line-name-list>, and repeats
        // enough times for the name list to match the subgrid’s specified grid span (falling back to 0 if
        // the span is already fulfilled).
        //
        // Otherwise on a standalone axis, when auto-fill is given as the repetition number, if the grid
        // container has a definite preferred size or maximum size in the relevant axis, then the number of
        // repetitions is the largest possible positive integer that does not cause the grid to overflow the
        // content box of its grid container taking gap into account; if any number of repetitions would
        // overflow, then 1 repetition.
        let available = self.axis_available(axis);
        let resolution_available = self.available_space.unwrap().inline_size;
        // For this purpose, each track is treated as its max track sizing function if that is definite or
        // else its min track sizing function if that is definite. If both are definite, floor the max track
        // sizing function by the min track sizing function. If neither are definite, the number of
        // repetitions is one.
        let repeated = expand_standalone(source, entry.repeat_list, |_index, _entry| 1);
        let mut repeated_size = CssPixels::default();
        for definition in &repeated.tracks {
            let min = TrackSizingFunction::from_breadth(definition.min);
            let max = TrackSizingFunction::from_breadth(definition.max);
            let size = if matches!(max, TrackSizingFunction::Fixed(_)) {
                max.resolve(resolution_available)
                    .max(if matches!(min, TrackSizingFunction::Fixed(_)) {
                        min.resolve(resolution_available)
                    } else {
                        CssPixels::default()
                    })
            } else if matches!(min, TrackSizingFunction::Fixed(_)) {
                min.resolve(resolution_available)
            } else {
                return 1;
            };
            // For the purpose of finding the number of auto-repeated tracks in a standalone axis, the UA must
            // floor the track size to a UA-specified value to avoid division by zero. It is suggested that this
            // floor be 1px.
            repeated_size += size.max(CssPixels::from_integer(1));
        }
        let gap = self.resolved_gap(axis, available);
        let denominator = repeated_size + gap * repeated.tracks.len();
        if let AvailableSize::Definite(available_size) = available
            && denominator > CssPixels::default()
        {
            // NOTE: Gap size is added to free space to compensate for the fact that the last track does not have a gap
            // If any number of repetitions would overflow, then 1 repetition.
            return (((available_size + gap).raw_value() as i64 / denominator.raw_value() as i64).max(1)) as usize;
        }
        // FIXME: Otherwise, if the grid container has a definite minimum size in the relevant axis, the number of
        //        repetitions is the smallest possible positive integer that fulfills that minimum requirement.
        //
        // Otherwise, the specified track list repeats only once.
        1
    }

    fn expand_axis(&self, axis: Axis, grid_style: &'static GridValues) -> ExpandedTrackList {
        let list = axis.select(grid_style.template_columns, grid_style.template_rows);
        let source = TrackListSource::from_grid_style(grid_style);
        if self.is_subgridded(axis, grid_style) {
            let parent_item = self.parent_grid_placement().unwrap();
            let track_count = parent_item.span(axis);
            let inherited = self
                .parent_grid()
                .map(|parent| {
                    let lines = axis.select(&parent.column_lines, &parent.row_lines);
                    let start = parent_item.position(axis).max(0) as usize;
                    lines
                        .iter()
                        .skip(start)
                        .take(track_count.saturating_add(1))
                        .cloned()
                        .collect::<Vec<_>>()
                })
                .unwrap_or_default();
            return expand_subgrid(source, list, track_count, &inherited);
        }
        expand_standalone(source, list, |_index, entry| {
            self.automatic_repeat_count(source, entry, axis)
        })
    }

    fn initialize_lines(&mut self, grid_style: &'static GridValues) -> (ExpandedTrackList, ExpandedTrackList) {
        let mut columns = self.expand_axis(Axis::Column, grid_style);
        let mut rows = self.expand_axis(Axis::Row, grid_style);
        self.project_parent_grid_areas(
            &mut columns.lines,
            &mut rows.lines,
            self.is_subgridded(Axis::Column, grid_style),
            self.is_subgridded(Axis::Row, grid_style),
        );
        add_template_area_lines(
            &mut columns.lines,
            &mut rows.lines,
            grid_style.areas.as_slice(),
            grid_style.names.raws(),
        );
        self.explicit_column_line_count = columns.lines.len();
        self.explicit_row_line_count = rows.lines.len();
        self.column_lines.clone_from(&columns.lines);
        self.row_lines.clone_from(&rows.lines);
        (columns, rows)
    }

    fn axis_placements(
        &self,
        start: ComputedGridPlacement,
        end: ComputedGridPlacement,
        axis: Axis,
        automatic_subgrid_span_value: Option<usize>,
        placement_names: &[usize],
    ) -> ResolvedAxisPlacement {
        let start_is_auto = start.kind != crate::layout::ComputedGridPlacementKind::Line as u8;
        let end_is_auto = end.kind != crate::layout::ComputedGridPlacementKind::Line as u8;
        let lines = self.axis_lines(axis);
        let explicit_lines = self.explicit_line_count(axis);
        let explicit_tracks = lines.len().saturating_sub(1);
        if start_is_auto && end_is_auto {
            return ResolvedAxisPlacement {
                start: None,
                span: resolve_placement_span(start, end, automatic_subgrid_span_value),
            };
        }
        let resolved = resolve_placement_position(start, end, placement_names, lines, explicit_lines, explicit_tracks);
        ResolvedAxisPlacement {
            start: Some(resolved.start),
            span: resolved.span,
        }
    }

    fn clamp_area_to_subgrid(start: &mut i32, span: &mut usize, track_count: usize) {
        if track_count == 0 {
            return;
        }
        // https://drafts.csswg.org/css-grid-2/#subgrid-implicit
        // The subgrid does not have any implicit grid tracks in the subgridded dimension(s).
        // Hypothetical implicit grid lines are used to resolve placement as usual when the
        // explicit grid does not have enough lines; however each grid item's grid area is
        // clamped to the subgrid's explicit grid.
        //
        // https://drafts.csswg.org/css-grid-2/#overlarge-grids
        // To clamp a grid area:
        // * If the grid area would span outside the limited grid, its span is clamped to the
        //   last line of the limited grid.
        // * If the grid area would be placed completely outside the limited grid, its span must
        //   be truncated to 1 and the area repositioned into the last grid track on that side
        //   of the grid.
        let mut end = start.saturating_add(*span as i32);
        let limited_end = track_count as i32;
        if end <= 0 {
            *start = 0;
            end = 1;
        } else if *start >= limited_end {
            *start = limited_end - 1;
            end = limited_end;
        } else {
            *start = (*start).max(0);
            end = end.min(limited_end);
        }
        *span = (end - *start) as usize;
    }

    fn place_items(&mut self) {
        let mut nodes = Vec::new();
        let mut inputs = Vec::new();
        let subgridded_columns = self.container_is_subgridded(Axis::Column);
        let subgridded_rows = self.container_is_subgridded(Axis::Row);
        let mut child = self.callbacks.first_child(self.grid_container);
        while !child.is_invalid() {
            let next = self.callbacks.next_sibling(child);
            let box_facts = self.facts(child);
            if box_facts.is_box() && !box_facts.is_absolutely_positioned() {
                let skip = self.callbacks.can_skip_is_anonymous_text_run(child);
                if !skip {
                    self.callbacks.arena().set_node_flag(child, NodeFlag::IsGridItem, true);
                    let child_grid_style = self.grid_style(child);
                    let source = TrackListSource::from_grid_style(child_grid_style);
                    let column_subgrid_span = child_grid_style
                        .template_columns
                        .is_subgrid
                        .then(|| automatic_subgrid_span(source, child_grid_style.template_columns));
                    let row_subgrid_span = child_grid_style
                        .template_rows
                        .is_subgrid
                        .then(|| automatic_subgrid_span(source, child_grid_style.template_rows));
                    inputs.push(PlacementInput {
                        id: nodes.len(),
                        order: self.style(child).order(),
                        row: self.axis_placements(
                            child_grid_style.row_start,
                            child_grid_style.row_end,
                            Axis::Row,
                            row_subgrid_span,
                            child_grid_style.names.raws(),
                        ),
                        column: self.axis_placements(
                            child_grid_style.column_start,
                            child_grid_style.column_end,
                            Axis::Column,
                            column_subgrid_span,
                            child_grid_style.names.raws(),
                        ),
                    });
                    self.records
                        .create_used_values(&self.callbacks, child, ContainingBlockConstraints::default());
                    nodes.push(child);
                }
            }
            child = next;
        }

        let style = self.style(self.grid_container);
        let mut result = place_items_with_grid(
            &inputs,
            self.column_lines.len().saturating_sub(1),
            self.row_lines.len().saturating_sub(1),
            if style.grid_auto_flow_row() {
                AutoFlowAxis::Row
            } else {
                AutoFlowAxis::Column
            },
            style.grid_auto_flow_dense(),
        );
        let column_track_count = self.column_lines.len().saturating_sub(1);
        let row_track_count = self.row_lines.len().saturating_sub(1);
        for placed in &mut result.items {
            if subgridded_columns {
                Self::clamp_area_to_subgrid(&mut placed.column, &mut placed.column_span, column_track_count);
            }
            if subgridded_rows {
                Self::clamp_area_to_subgrid(&mut placed.row, &mut placed.row_span, row_track_count);
            }
        }
        self.explicit_column_start = if subgridded_columns {
            0
        } else {
            result.explicit_column_start
        };
        self.explicit_row_start = if subgridded_rows { 0 } else { result.explicit_row_start };
        if !subgridded_columns && self.explicit_column_start > 0 {
            let mut lines = Vec::with_capacity(self.column_lines.len() + self.explicit_column_start);
            lines.resize_with(self.explicit_column_start, Vec::new);
            lines.append(&mut self.column_lines);
            self.column_lines = lines;
        }
        if !subgridded_rows && self.explicit_row_start > 0 {
            let mut lines = Vec::with_capacity(self.row_lines.len() + self.explicit_row_start);
            lines.resize_with(self.explicit_row_start, Vec::new);
            lines.append(&mut self.row_lines);
            self.row_lines = lines;
        }
        for placed in result.items {
            let box_ = nodes[placed.id];
            self.items.push(GridItem {
                box_,
                row: placed.row,
                row_span: placed.row_span,
                column: placed.column,
                column_span: placed.column_span,
                extra_margin_top: CssPixels::default(),
                extra_margin_right: CssPixels::default(),
                extra_margin_bottom: CssPixels::default(),
                extra_margin_left: CssPixels::default(),
            });
        }
        if !subgridded_columns {
            self.column_lines
                .resize_with(result.column_count.saturating_add(1), Vec::new);
        }
        if !subgridded_rows {
            self.row_lines.resize_with(result.row_count.saturating_add(1), Vec::new);
        }
    }

    fn expanded_auto_tracks(&self, grid_style: &'static GridValues, axis: Axis) -> Vec<TrackDefinition> {
        let list = axis.select(grid_style.auto_columns, grid_style.auto_rows);
        expand_standalone(TrackListSource::from_grid_style(grid_style), list, |_index, _entry| 1).tracks
    }

    fn initialize_tracks_for_axis(
        &self,
        axis: Axis,
        grid_style: &'static GridValues,
        explicit: &ExpandedTrackList,
        total_count: usize,
        explicit_start: usize,
    ) -> Vec<Track> {
        if self.is_subgridded(axis, grid_style) {
            // https://drafts.csswg.org/css-grid-2/#subgrid-tracks
            // Placing the subgrid creates a correspondence between its subgridded tracks and those that it
            // spans in its parent grid. The grid lines thus shared between the subgrid and its parent form the
            // subgrid's explicit grid, and its track sizes are governed by the parent grid.
            let Some(parent) = self.parent_grid() else {
                return vec![Track::auto()];
            };
            let parent_item = self.parent_grid_placement().unwrap();
            let parent_tracks = axis.select(&parent.columns, &parent.rows);
            let mut tracks = Vec::new();
            for offset in 0..parent_item.span(axis) {
                let index = parent_item.position(axis) + offset as i32;
                if let Some(parent_track) = usize::try_from(index).ok().and_then(|index| parent_tracks.get(index)) {
                    if self.layout_mode == LayoutMode::IntrinsicSizing {
                        // https://drafts.csswg.org/css-grid-2/#subgrid-size-contribution
                        // The subgrid itself lays out as an ordinary grid item in its parent grid,
                        // but acts as if it was completely empty for track sizing purposes in the
                        // subgridded dimension.
                        //
                        // https://drafts.csswg.org/css-grid-2/#subgrid-item-contribution
                        // The subgrid's own grid items participate in the sizing of its parent grid
                        // in the subgridded dimension(s) and are aligned to it in those dimensions.
                        let mut track = *parent_track;
                        track.base_size = CssPixels::default();
                        track.growth_limit = Some(CssPixels::default());
                        tracks.push(track);
                    } else {
                        tracks.push(Track::fixed(parent_track.base_size));
                    }
                } else {
                    tracks.push(Track::auto());
                }
            }
            if tracks.is_empty() {
                tracks.push(Track::auto());
            }
            return tracks;
        }

        let automatic = self.expanded_auto_tracks(grid_style, axis);
        let mut automatic_index = 0usize;
        let mut tracks = Vec::with_capacity(total_count);
        // NOTE: If there are implicit tracks created by items with negative indexes they should prepend explicitly defined tracks
        for _ in 0..explicit_start {
            tracks.push(if automatic.is_empty() {
                Track::auto()
            } else {
                let definition = automatic[automatic_index % automatic.len()];
                automatic_index += 1;
                Track::from_definition(definition)
            });
        }
        tracks.extend(explicit.tracks.iter().copied().map(Track::from_definition));
        // NOTE: If there are implicit tracks created by items with negative indexes they should prepend explicitly defined tracks
        while tracks.len() < total_count {
            tracks.push(if automatic.is_empty() {
                Track::auto()
            } else {
                let definition = automatic[automatic_index % automatic.len()];
                automatic_index += 1;
                Track::from_definition(definition)
            });
        }
        tracks
    }

    fn collapse_auto_fit(&mut self, axis: Axis) {
        // https://www.w3.org/TR/css-grid-2/#auto-repeat
        // The auto-fit keyword behaves the same as auto-fill, except that after grid item placement any
        // empty repeated tracks are collapsed. An empty track is one with no in-flow grid items placed into
        // or spanning across it. (This can result in all tracks being collapsed, if they’re all empty.)
        let occupied = |track_index: usize, items: &[GridItem]| {
            items.iter().any(|item| {
                let start = item.position(axis).max(0) as usize;
                track_index >= start && track_index < start.saturating_add(item.span(axis))
            })
        };
        let items = &self.items;
        let tracks = axis.select(&mut self.columns, &mut self.rows);
        for (index, track) in tracks.iter_mut().enumerate() {
            if track.is_auto_fit && !occupied(index, items) {
                // A collapsed grid track is treated as having a fixed track sizing function of 0px, and the gutters on
                // either side of it--including any space allotted through distributed alignment--collapse.
                // NB: The gutter collapsing is handled by initialize_gap_tracks(), which runs after this.
                track.collapse();
            }
        }
    }

    fn initialize_gaps_for_axis(&mut self, axis: Axis, available: AvailableSize) {
        // https://www.w3.org/TR/css-grid-2/#gutters
        // 11.1. Gutters: the row-gap, column-gap, and gap properties
        // For the purpose of track sizing, each gutter is treated as an extra, empty, fixed-size track of the specified
        // size, which is spanned by any grid items that span across its corresponding grid line.
        let gap_size = self.resolved_gap(axis, available);
        let tracks = if axis.is_column() { &self.columns } else { &self.rows };
        let mut gaps = Vec::with_capacity(tracks.len().saturating_sub(1));
        let mut seen_non_collapsed = false;
        // When a collapsed track's gutters collapse, they coincide exactly--the two gutters overlap so that their start
        // and end edges coincide. If one side of a collapsed track does not have a gutter (e.g. if it is the first or
        // last track of the implicit grid), then collapsing its gutters results in no gutter on either "side" of the
        // collapsed track.
        // NB: We model this by keeping the gutter that directly precedes each non-collapsed track (except the first such
        //     track) and giving all other gutters a zero size.
        for index in 0..tracks.len().saturating_sub(1) {
            seen_non_collapsed |= !tracks[index].is_collapsed;
            let collapse = tracks[index + 1].is_collapsed || !seen_non_collapsed;
            gaps.push(Track::gap(if collapse { CssPixels::default() } else { gap_size }));
        }
        if axis.is_column() {
            self.column_gaps = gaps;
        } else {
            self.row_gaps = gaps;
        }
    }

    fn initialize_tracks(
        &mut self,
        grid_style: &'static GridValues,
        columns: &ExpandedTrackList,
        rows: &ExpandedTrackList,
    ) {
        self.columns = self.initialize_tracks_for_axis(
            Axis::Column,
            grid_style,
            columns,
            self.column_lines.len().saturating_sub(1),
            self.explicit_column_start,
        );
        self.rows = self.initialize_tracks_for_axis(
            Axis::Row,
            grid_style,
            rows,
            self.row_lines.len().saturating_sub(1),
            self.explicit_row_start,
        );
        self.collapse_auto_fit(Axis::Column);
        self.collapse_auto_fit(Axis::Row);
        let available = self.available_space.unwrap();
        self.initialize_gaps_for_axis(Axis::Column, available.inline_size);
        self.initialize_gaps_for_axis(Axis::Row, available.block_size);
    }

    fn axis_tracks(&self, axis: Axis) -> &[Track] {
        axis.select(&self.columns, &self.rows)
    }

    fn axis_gaps(&self, axis: Axis) -> &[Track] {
        axis.select(&self.column_gaps, &self.row_gaps)
    }

    fn axis_lines(&self, axis: Axis) -> &[Vec<LineName>] {
        axis.select(&self.column_lines, &self.row_lines)
    }

    fn explicit_line_count(&self, axis: Axis) -> usize {
        axis.select(self.explicit_column_line_count, self.explicit_row_line_count)
    }

    fn content_alignment(&self, axis: Axis) -> Alignment {
        let style = self.style(self.grid_container);
        axis.select(
            inline_content_alignment(style.justify_content()),
            block_content_alignment(style.align_content()),
        )
    }

    fn container_padding_start(&self, axis: Axis) -> CssPixels {
        let used = self.container_used();
        axis.select(used.padding_left.get(), used.padding_top.get())
    }

    fn container_padding_end(&self, axis: Axis) -> CssPixels {
        let used = self.container_used();
        axis.select(used.padding_right.get(), used.padding_bottom.get())
    }

    fn interleaved_tracks(&self, axis: Axis) -> Vec<Track> {
        let tracks = self.axis_tracks(axis);
        let gaps = self.axis_gaps(axis);
        let mut result = Vec::with_capacity(tracks.len().saturating_mul(2).saturating_sub(1));
        for (index, track) in tracks.iter().copied().enumerate() {
            result.push(track);
            if let Some(gap) = gaps.get(index) {
                result.push(*gap);
            }
        }
        result
    }

    fn interleaved_track_iter(&self, axis: Axis) -> impl Iterator<Item = &Track> {
        let tracks = self.axis_tracks(axis);
        let gaps = self.axis_gaps(axis);
        tracks
            .iter()
            .enumerate()
            .flat_map(move |(index, track)| std::iter::once(track).chain(gaps.get(index)))
    }

    fn interleaved_index_of_track(track_index: usize) -> usize {
        track_index * 2
    }

    fn interleaved_index_of_gap_after_track(track_index: usize) -> usize {
        track_index * 2 + 1
    }

    fn store_interleaved_tracks(&mut self, axis: Axis, interleaved: &[Track]) {
        let (tracks, gaps) = axis.select(
            (&mut self.columns, &mut self.column_gaps),
            (&mut self.rows, &mut self.row_gaps),
        );
        for (index, track) in tracks.iter_mut().enumerate() {
            *track = interleaved[Self::interleaved_index_of_track(index)];
        }
        for (index, gap) in gaps.iter_mut().enumerate() {
            *gap = interleaved[Self::interleaved_index_of_gap_after_track(index)];
        }
    }

    fn spanned_interleaved_indices(item: GridItem, axis: Axis, track_count: usize) -> Vec<usize> {
        let start = item.position(axis).max(0) as usize;
        let end = start.saturating_add(item.span(axis)).min(track_count);
        let mut indices = Vec::new();
        for track in start..end {
            indices.push(Self::interleaved_index_of_track(track));
            if track + 1 < end {
                indices.push(Self::interleaved_index_of_gap_after_track(track));
            }
        }
        indices
    }

    fn containing_block_size(&self, item: GridItem, axis: Axis) -> CssPixels {
        let tracks = self.axis_tracks(axis);
        let gaps = self.axis_gaps(axis);
        let start = item.position(axis).max(0) as usize;
        let end = start.saturating_add(item.span(axis)).min(tracks.len());
        let mut size = CssPixels::default();
        for (index, track) in tracks.iter().enumerate().take(end).skip(start) {
            size += track.base_size;
            if index + 1 < end {
                size += gaps.get(index).map_or(CssPixels::default(), |gap| gap.base_size);
            }
        }
        size
    }

    fn item_available_space(&self, item: GridItem) -> AvailableSpace {
        let used = self.used(item);
        AvailableSpace {
            inline_size: if used.has_definite_inline_size() {
                AvailableSize::definite(used.content_inline_size.get())
            } else {
                AvailableSize::Indefinite
            },
            block_size: if used.has_definite_block_size() {
                AvailableSize::definite(used.content_block_size.get())
            } else {
                AvailableSize::Indefinite
            },
        }
    }

    fn track_sizing_constraints(&self) -> ContainingBlockConstraints {
        // During track sizing the grid area is not known yet, so items have no percentage basis.
        //
        // Intrinsic contributions during track sizing measure items against the grid container's own content box, since the
        // grid area does not exist yet.
        let inherited = self.sizing().constraints_for_child_context(
            self.grid_container,
            self.layout_input.unwrap().containing_block_constraints,
        );
        ContainingBlockConstraints {
            percentage_basis_inline_size: None,
            percentage_basis_block_size: None,
            ..inherited
        }
    }

    fn container_constraints(&self) -> ContainingBlockConstraints {
        self.sizing().constraints_for_child_context(
            self.grid_container,
            self.layout_input.unwrap().containing_block_constraints,
        )
    }

    fn grid_area_constraints(&self, item: GridItem) -> ContainingBlockConstraints {
        let inherited = self.track_sizing_constraints();
        ContainingBlockConstraints {
            percentage_basis_inline_size: Some(self.containing_block_size(item, Axis::Column)),
            ..inherited
        }
    }

    fn outer_edges(&self, item: GridItem, axis: Axis) -> CssPixels {
        self.item_margin_box_start(item, axis) + self.item_margin_box_end(item, axis)
    }

    fn add_outer_size(&self, item: GridItem, axis: Axis, size: CssPixels) -> CssPixels {
        size + self.outer_edges(item, axis)
    }

    fn preferred_size(&self, item: GridItem, axis: Axis) -> &'static ComputedSize {
        let style = self.style(item.box_);
        axis.select(style.width(), style.height())
    }

    fn minimum_size(&self, item: GridItem, axis: Axis) -> &'static ComputedSize {
        let style = self.style(item.box_);
        axis.select(style.min_width(), style.min_height())
    }

    fn maximum_size(&self, item: GridItem, axis: Axis) -> &'static ComputedSize {
        let style = self.style(item.box_);
        axis.select(style.max_width(), style.max_height())
    }

    fn preferred_behaves_as_auto(&self, item: GridItem, axis: Axis) -> bool {
        let available = self.item_available_space(item);
        let behaves_as_auto = self.sizing().should_treat_size_as_auto(
            item.box_,
            axis.sizing_axis(),
            available,
            self.track_sizing_constraints(),
        );
        // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
        // When a non-replaced grid item's percentage preferred size contributes to
        // sizing tracks in the same axis, the percentage is cyclic and behaves as
        // the property's initial value for intrinsic contribution calculations.
        behaves_as_auto
            || (!self.facts(item.box_).is_replaced_box() && self.preferred_size(item, axis).contains_percentage())
    }

    fn min_content_size(&self, item: GridItem, axis: Axis) -> CssPixels {
        if axis.is_column() {
            self.sizing()
                .calculate_min_content_inline_size(item.box_, self.container_constraints())
        } else {
            self.sizing().calculate_min_content_block_size(
                item.box_,
                self.item_available_space(item).inline_size.to_px_or_zero(),
                self.container_constraints(),
            )
        }
    }

    fn min_content_contribution(&self, item: GridItem, axis: Axis) -> CssPixels {
        let max = self.maximum_size(item, axis);
        let maximum = if max.is_length_percentage() && !max.contains_percentage() {
            max.to_px(CssPixels::default())
        } else {
            CssPixels::from_raw(i32::MAX)
        };
        let content = if self.preferred_behaves_as_auto(item, axis) {
            if axis.is_column() && self.facts(item.box_).is_scroll_container() {
                // NOTE: Not defined in spec, but matches other browsers: a scroll container's min-content
                //       width contribution is 0 because its content can overflow and scroll horizontally.
                //       This does NOT apply to the row dimension — scroll containers must still contribute
                //       their content height, otherwise grids with height:min-content collapse rows to 0.
                CssPixels::default()
            } else {
                self.min_content_size(item, axis)
            }
        } else {
            let property = axis.select(SizingProperty::Width, SizingProperty::Height);
            self.sizing().calculate_inner_size_for_property(
                item.box_,
                axis.sizing_axis(),
                property,
                self.available_space.unwrap(),
                self.track_sizing_constraints(),
            )
        };
        self.add_outer_size(item, axis, content).min(maximum)
    }

    fn max_content_contribution(&self, item: GridItem, axis: Axis) -> CssPixels {
        let max = self.maximum_size(item, axis);
        let maximum = if max.is_length_percentage() && !max.contains_percentage() {
            max.to_px(CssPixels::default())
        } else {
            CssPixels::from_raw(i32::MAX)
        };
        let preferred = self.preferred_size(item, axis);
        let content = if self.preferred_behaves_as_auto(item, axis) || preferred.is_fit_content() {
            self.sizing().calculate_fit_content_size(
                item.box_,
                axis.sizing_axis(),
                self.item_available_space(item),
                self.container_constraints(),
            )
        } else {
            let area_space = AvailableSpace {
                inline_size: AvailableSize::definite(clamp_to_max_dimension_value(
                    self.containing_block_size(item, Axis::Column),
                )),
                block_size: AvailableSize::definite(clamp_to_max_dimension_value(
                    self.containing_block_size(item, Axis::Row),
                )),
            };
            self.sizing().calculate_inner_size_for_property(
                item.box_,
                axis.sizing_axis(),
                axis.select(SizingProperty::Width, SizingProperty::Height),
                area_space,
                self.track_sizing_constraints(),
            )
        };
        self.add_outer_size(item, axis, content).min(maximum)
    }

    fn content_size_suggestion(&self, item: GridItem, axis: Axis) -> CssPixels {
        // The content size suggestion is the min-content size in the relevant axis
        // FIXME: clamped, if it has a preferred aspect ratio, by any definite opposite-axis minimum and maximum sizes
        // converted through the aspect ratio.
        self.min_content_size(item, axis)
    }

    fn specified_size_suggestion(&self, item: GridItem, axis: Axis) -> Option<CssPixels> {
        // https://www.w3.org/TR/css-grid-1/#specified-size-suggestion
        // If the item’s preferred size in the relevant axis is definite, then the specified size suggestion is that size.
        // It is otherwise undefined.
        let preferred_size = self.preferred_size(item, axis);
        if !self.facts(item.box_).is_replaced_box() && preferred_size.contains_percentage() {
            return None;
        }

        let used = self.used(item);
        let has_definite_preferred_size = axis.select(used.has_definite_inline_size(), used.has_definite_block_size());
        if has_definite_preferred_size {
            // FIXME: consider margins, padding and borders because it is outer size.
            let containing_block_size = self.containing_block_size(item, axis);
            return Some(preferred_size.to_px(containing_block_size));
        }

        None
    }

    fn transferred_size_suggestion(&self, item: GridItem, axis: Axis) -> Option<CssPixels> {
        // https://www.w3.org/TR/css-grid-2/#transferred-size-suggestion
        // If the item has a preferred aspect ratio and its preferred size in the opposite axis is definite, then the transferred
        // size suggestion is that size (clamped by the opposite-axis minimum and maximum sizes if they are definite), converted
        // through the aspect ratio. It is otherwise undefined.
        let preferred_aspect_ratio = self.facts(item.box_).preferred_aspect_ratio()?;

        let preferred_size_in_opposite_axis = self.preferred_size(item, axis.opposite());
        if preferred_size_in_opposite_axis.is_length() {
            let opposite_axis_size = preferred_size_in_opposite_axis.to_px(CssPixels::default());
            // FIXME: Clamp by opposite-axis minimum and maximum sizes if they are definite
            return Some(preferred_aspect_ratio.multiply(opposite_axis_size));
        }

        None
    }

    fn content_based_minimum_size(&self, item: GridItem, axis: Axis) -> CssPixels {
        // https://www.w3.org/TR/css-grid-1/#content-based-minimum-size

        let mut result;
        // The content-based minimum size for a grid item in a given dimension is its specified size suggestion if it exists,
        if let Some(specified_size_suggestion) = self.specified_size_suggestion(item, axis) {
            result = specified_size_suggestion;
        }
        // otherwise its transferred size suggestion if that exists,
        else if let Some(transferred_size_suggestion) = self.transferred_size_suggestion(item, axis) {
            result = transferred_size_suggestion;
        }
        // else its content size suggestion.
        else {
            result = self.content_size_suggestion(item, axis);
        }

        // However, if in a given dimension the grid item spans only grid tracks that have a fixed max track sizing function, then
        // its specified size suggestion and content size suggestion in that dimension (and its input from this dimension to the
        // transferred size suggestion in the opposite dimension) are further clamped to less than or equal to the stretch fit into
        // the grid area’s maximum size in that dimension, as represented by the sum of those grid tracks’ max track sizing functions
        // plus any intervening fixed gutters.
        // FIXME: Account for intervening fixed gutters.
        let available = self.axis_available(axis);
        let tracks = self.axis_tracks(axis);
        let start = item.position(axis).max(0) as usize;
        let end = start.saturating_add(item.span(axis)).min(tracks.len());
        let mut fixed_track_limit = CssPixels::default();
        let mut all_fixed = true;
        for track in &tracks[start..end] {
            if !track.max_sizing.is_fixed(available) {
                all_fixed = false;
                break;
            }
            fixed_track_limit += track.max_sizing.resolve(available);
        }
        if all_fixed {
            result = result.min(fixed_track_limit);
        }

        // In all cases, the size suggestion is additionally clamped by the maximum size in the affected axis, if it’s definite.
        let maximum_size = self.maximum_size(item, axis);
        if maximum_size.is_length_percentage() && !maximum_size.contains_percentage() {
            result = result.min(maximum_size.to_px(CssPixels::default()));
        }

        // If the item is a compressible replaced element, and has a definite preferred size or maximum size in the relevant axis,
        // the size suggestion is capped by those sizes; for this purpose, any indefinite percentages in these sizes are resolved
        // against zero (and considered definite).
        // FIXME: "compressible replaced element" includes more elements than is_replaced_box().
        let preferred_size = self.preferred_size(item, axis);
        if self.facts(item.box_).is_replaced_box() && (preferred_size.is_percentage() || maximum_size.is_percentage()) {
            // NOTE: Implements "for this purpose, any indefinite percentages in these sizes are resolved
            //       against zero (and considered definite)." part.
            result = CssPixels::default();
        }

        result
    }

    fn automatic_minimum_size(&self, item: GridItem, axis: Axis) -> CssPixels {
        // To provide a more reasonable default minimum size for grid items, the used value of its automatic minimum size
        // in a given axis is the content-based minimum size if all of the following are true:
        // - it is not a scroll container
        // - it spans at least one track in that axis whose min track sizing function is auto
        // - if it spans more than one track in that axis, none of those tracks are flexible
        let available = self.axis_available(axis);
        let tracks = self.axis_tracks(axis);
        let start = item.position(axis).max(0) as usize;
        let end = start.saturating_add(item.span(axis)).min(tracks.len());
        if start >= end {
            return CssPixels::default();
        }
        let spans_auto = tracks[start..end]
            .iter()
            .any(|track| track.min_sizing.is_auto(available));
        let spans_flexible = tracks[start..end]
            .iter()
            .any(|track| track.max_sizing.flex_factor().is_some());
        if spans_auto && !self.facts(item.box_).is_scroll_container() && (item.span(axis) == 1 || !spans_flexible) {
            return self.content_based_minimum_size(item, axis);
        }
        // Otherwise, the automatic minimum size is zero, as usual.
        CssPixels::default()
    }

    fn minimum_contribution(&self, item: GridItem, axis: Axis) -> CssPixels {
        // The minimum contribution of an item is the smallest outer size it can have.
        // Specifically, if the item’s computed preferred size behaves as auto or depends on the size of its
        // containing block in the relevant axis, its minimum contribution is the outer size that would
        // result from assuming the item’s used minimum size as its preferred size; else the item’s minimum
        // contribution is its min-content contribution. Because the minimum contribution often depends on
        // the size of the item’s content, it is considered a type of intrinsic size contribution.
        if !self.preferred_behaves_as_auto(item, axis) {
            return self.min_content_contribution(item, axis);
        }
        let minimum = self.minimum_size(item, axis);
        let content = if minimum.is_auto() {
            self.automatic_minimum_size(item, axis)
        } else if minimum.is_min_content() {
            return self.min_content_contribution(item, axis);
        } else if minimum.is_max_content() {
            return self.max_content_contribution(item, axis);
        } else {
            let mut available = self.item_available_space(item);
            if axis.is_column() && self.facts(item.box_).is_table_wrapper() && minimum.contains_percentage() {
                // Percentage minimum sizes on a table wrapper resolve against the same non-cyclic
                // inline size that the wrapper's own inline-size resolution uses.
                let containing = self.containing_block_size(item, Axis::Column);
                available.inline_size = AvailableSize::definite(clamp_to_max_dimension_value(
                    self.non_cyclic_table_wrapper_inline_size(item, containing),
                ));
            }
            self.sizing().calculate_inner_size_for_property(
                item.box_,
                axis.sizing_axis(),
                axis.select(SizingProperty::MinWidth, SizingProperty::MinHeight),
                available,
                self.track_sizing_constraints(),
            )
        };
        self.add_outer_size(item, axis, content)
    }

    fn fixed_track_limit(&self, item: GridItem, axis: Axis) -> Option<CssPixels> {
        // https://drafts.csswg.org/css-grid-2/#algo-spanning-items
        // For an item spanning multiple tracks, the upper limit used to calculate its limited min-/max-content
        // contribution is the sum of the fixed max track sizing functions of any tracks it spans, and is applied
        // if it only spans such tracks.
        //
        // https://drafts.csswg.org/css-grid-2#algo-terms
        // In all cases, treat auto and fit-content() as max-content, except where specified otherwise for fit-content().
        let available = self.axis_available(axis);
        let tracks = self.axis_tracks(axis);
        let gaps = self.axis_gaps(axis);
        let start = item.position(axis).max(0) as usize;
        let end = start.saturating_add(item.span(axis)).min(tracks.len());
        if start == end {
            return None;
        }
        let mut result = CssPixels::default();
        for (index, track) in tracks.iter().enumerate().take(end).skip(start) {
            let max = track.max_sizing;
            if max.is_fixed(available)
                || matches!(max, TrackSizingFunction::FitContent(value) if !value.contains_percentage() || matches!(available, AvailableSize::Definite(_)))
            {
                result += max.resolve(available);
            } else {
                return None;
            }
            if index + 1 < end {
                result += gaps.get(index).map_or(CssPixels::default(), |gap| gap.base_size);
            }
        }
        Some(result)
    }

    fn calculate_grid_container_maximum_size(&self, axis: Axis) -> CssPixels {
        let available = self.available_space.unwrap();
        let constraints = self.layout_input.unwrap().containing_block_constraints;
        self.sizing().calculate_inner_size_for_property(
            self.grid_container,
            axis.sizing_axis(),
            axis.select(SizingProperty::MaxWidth, SizingProperty::MaxHeight),
            available,
            constraints,
        )
    }

    fn grid_container_maximum_size(&self, axis: Axis) -> Option<CssPixels> {
        let available = self.axis_available(axis);
        let constraints = self.layout_input.unwrap().containing_block_constraints;
        let is_none = if axis.is_column() {
            self.sizing()
                .should_treat_max_inline_size_as_none(self.grid_container, available, constraints)
        } else {
            self.sizing()
                .should_treat_max_block_size_as_none(self.grid_container, available, constraints)
        };
        if is_none {
            return None;
        }
        Some(self.calculate_grid_container_maximum_size(axis))
    }

    fn grid_container_maximum_size_for_maximize_tracks(&self, axis: Axis) -> Option<CssPixels> {
        let available_size = self.axis_available(axis);
        let computed_values = self.style(self.grid_container);
        let should_treat_grid_container_maximum_size_as_none = if axis.is_column() {
            self.sizing().should_treat_max_inline_size_as_none(
                self.grid_container,
                available_size,
                self.layout_input.unwrap().containing_block_constraints,
            )
        } else {
            !computed_values.max_height().is_auto()
        };

        if should_treat_grid_container_maximum_size_as_none {
            return None;
        }
        Some(self.calculate_grid_container_maximum_size(axis))
    }

    fn limited_content_contribution(
        &self,
        content: CssPixels,
        minimum: CssPixels,
        item: GridItem,
        axis: Axis,
    ) -> CssPixels {
        // The limited min-content contribution of an item is its min-content contribution,
        // limited by the max track sizing function (which could be the argument to a fit-content() track
        // sizing function) if that is fixed and ultimately floored by its minimum contribution.
        //
        // The limited max-content contribution of an item is its max-content contribution,
        // limited by the max track sizing function (which could be the argument to a fit-content() track
        // sizing function) if that is fixed and ultimately floored by its minimum contribution.
        if content < minimum {
            return minimum;
        }
        if let Some(limit) = self.fixed_track_limit(item, axis) {
            return content.min(limit).max(minimum);
        }
        if let Some(maximum) = self.grid_container_maximum_size(axis)
            && content > maximum
        {
            return maximum;
        }
        content
    }

    fn item_contribution(&self, item: GridItem, axis: Axis, combined_track_count: usize) -> ItemContribution {
        let minimum = self.minimum_contribution(item, axis);
        let min_content = self.min_content_contribution(item, axis);
        let max_content = self.max_content_contribution(item, axis);
        let limited_min = self.limited_content_contribution(min_content, minimum, item, axis);
        let limited_max = self.limited_content_contribution(max_content, minimum, item, axis);
        ItemContribution {
            spanned_tracks: Self::spanned_interleaved_indices(item, axis, combined_track_count),
            span: item.span(axis),
            minimum,
            min_content,
            limited_min_content: limited_min,
            max_content,
            limited_max_content: limited_max,
            is_scroll_container: self.facts(item.box_).is_scroll_container(),
        }
    }

    fn grid_item_is_subgridded(&self, item: GridItem, axis: Axis) -> bool {
        if !self.facts(item.box_).display().is_grid_inside() {
            return false;
        }
        let grid_style = self.grid_style(item.box_);
        axis.select(
            grid_style.template_columns.is_subgrid,
            grid_style.template_rows.is_subgrid,
        )
    }

    fn apply_subgrid_edge_extra_margins(&self, item: &mut GridItem, axis: Axis) {
        if !self.container_is_subgridded(axis) {
            return;
        }
        let Some(container_as_parent_grid_item) = self.parent_grid_placement() else {
            return;
        };
        // https://drafts.csswg.org/css-grid-2/#subgrid-margins
        // In this process, the sum of the subgrid's margin, padding, scrollbar
        // gutter, and border at each edge are applied as an extra layer of
        // (potentially negative) margin to the items at those edges.
        // This extra layer of "margin" accumulates through multiple levels of
        // subgrids.
        //
        // NB: Scrollbar gutters are not represented in UsedValues yet.
        let used = self.container_used();
        let (start, end) = if axis.is_column() {
            (
                used.margin_left.get() + used.border_left.get() + used.padding_left.get(),
                used.padding_right.get() + used.border_right.get() + used.margin_right.get(),
            )
        } else {
            (
                used.margin_top.get() + used.border_top.get() + used.padding_top.get(),
                used.padding_bottom.get() + used.border_bottom.get() + used.margin_bottom.get(),
            )
        };
        if item.position(axis) == 0 {
            if axis.is_column() {
                item.extra_margin_left += start + container_as_parent_grid_item.extra_margin_left;
            } else {
                item.extra_margin_top += start + container_as_parent_grid_item.extra_margin_top;
            }
        }
        if item.position(axis) + item.span(axis) as i32 == self.axis_tracks(axis).len() as i32 {
            if axis.is_column() {
                item.extra_margin_right += end + container_as_parent_grid_item.extra_margin_right;
            } else {
                item.extra_margin_bottom += end + container_as_parent_grid_item.extra_margin_bottom;
            }
        }
    }

    fn subgrid_item_contributions_to_track_sizing(&self, subgrid: GridItem, axis: Axis) -> Vec<ItemContribution> {
        let scratch = formatting_context::MeasurementState::create(self.callbacks);
        let live = self.used(subgrid);
        let scratch_root = scratch.create_used_values(subgrid.box_, ContainingBlockConstraints::default());
        live.mirror_box_metrics_and_size_constraints_into(&scratch_root);
        scratch_root
            .has_definite_inline_size
            .set(live.has_definite_inline_size.get());
        scratch_root
            .has_definite_block_size
            .set(live.has_definite_block_size.get());
        let scratch_run = FormattingContextRun {
            purpose: formatting_context::LayoutPurpose::Measurement,
            records: std::rc::Rc::new(RunRecords::new(self.callbacks.arena, subgrid.box_, scratch_root)),
            box_: subgrid.box_,
            layout_mode: LayoutMode::IntrinsicSizing,
            callbacks: self.callbacks,
            should_collect_devtools_layout_data: false,
            treat_block_axis_percentage_insets_as_auto_beyond_root: false,
            fragments: None,
            previous_line_data: None,
        };
        let mut context = GridFormattingContext::new(&scratch_run, Some(self));
        let mut available = self.available_space.unwrap();
        if !axis.is_column() && live.has_definite_inline_size() {
            available.inline_size = AvailableSize::definite(live.content_inline_size.get());
        }
        let input = LayoutInput::new(
            available,
            self.track_sizing_constraints(),
            ParticipationInParentFormattingContext::Item,
        );
        context.reset_for_run(input);
        let grid_style = context.grid_style(context.grid_container);
        context.cache_subgrid_axes(grid_style);
        let (columns, rows) = context.initialize_lines(grid_style);
        context.place_items();
        context.initialize_tracks(grid_style, &columns, &rows);
        if !axis.is_column() {
            context.resolve_item_metrics(Axis::Column);
            context.run_track_sizing(Axis::Column);
            context.resolve_item_metrics(Axis::Column);
            context.resolve_item_sizes(Axis::Column);
        }
        context.resolve_item_metrics(axis);

        let mut items = std::mem::take(&mut context.items);
        for item in &mut items {
            context.apply_subgrid_edge_extra_margins(item, axis);
        }
        context.items = items;

        let mut contributions = context.item_contributions_to_track_sizing(axis);
        let interleaved_index_offset_in_parent =
            Self::interleaved_index_of_track(subgrid.position(axis).max(0) as usize);
        for contribution in &mut contributions {
            for index in &mut contribution.spanned_tracks {
                *index += interleaved_index_offset_in_parent;
            }
        }
        contributions
    }

    fn item_contributions_to_track_sizing(&self, axis: Axis) -> Vec<ItemContribution> {
        // https://drafts.csswg.org/css-grid-2/#subgrid-size-contribution
        // The subgrid itself lays out as an ordinary grid item in its parent grid,
        // but acts as if it was completely empty for track sizing purposes
        // in the subgridded dimension.
        //
        // https://drafts.csswg.org/css-grid-2/#subgrid-item-contribution
        // The subgrid's own grid items participate in the sizing of its parent grid
        // in the subgridded dimension(s) and are aligned to it in those dimensions.
        //
        // https://drafts.csswg.org/css-grid-2/#algo-grid-sizing
        // In this process, any grid item which is subgridded in the grid container’s
        // inline axis is treated as empty and its grid items (the grandchildren) are
        // treated as direct children of the grid container (their grandparent).
        // This introspection is recursive.
        //
        // In this process, any grid item which is subgridded in the grid container’s
        // block axis is treated as empty and its grid items (the grandchildren) are
        // treated as direct children of the grid container (their grandparent).
        // This introspection is recursive.
        let track_count = self.axis_tracks(axis).len();
        let mut result = Vec::new();
        for item in self.items.iter().copied() {
            if self.grid_item_is_subgridded(item, axis) {
                result.extend(self.subgrid_item_contributions_to_track_sizing(item, axis));
            } else {
                result.push(self.item_contribution(item, axis, track_count));
            }
        }
        result
    }

    fn run_track_sizing(&mut self, axis: Axis) {
        let mut tracks = self.interleaved_tracks(axis);
        let contributions = self.item_contributions_to_track_sizing(axis);
        let style = self.style(self.grid_container);
        let distribution_stretches = axis.select(
            matches!(
                style.justify_content(),
                justify_content::NORMAL | justify_content::STRETCH
            ),
            matches!(style.align_content(), align_content::NORMAL | align_content::STRETCH),
        );
        run_track_sizing(
            &mut tracks,
            CssPixels::default(),
            &contributions,
            self.axis_available(axis),
            || self.grid_container_maximum_size_for_maximize_tracks(axis),
            !axis.is_column(),
            distribution_stretches,
        );
        self.store_interleaved_tracks(axis, &tracks);
    }

    fn resolve_item_metrics(&mut self, axis: Axis) {
        for item_index in 0..self.items.len() {
            let item = self.items[item_index];
            let style = self.style(item.box_);
            let inline_basis = self.containing_block_size(item, Axis::Column);
            let extra_margin = self.subgrid_gap_extra_margin(axis, self.axis_available(axis));
            let item_start = item.position(axis);
            let item_end = item_start + item.span(axis) as i32;
            let track_count = self.axis_tracks(axis).len() as i32;
            let used = self.used(item);
            if axis.is_column() {
                used.padding_left.set(style.padding_left().to_px(inline_basis));
                used.padding_right.set(style.padding_right().to_px(inline_basis));
                used.margin_left.set(style.margin_left().to_px(inline_basis));
                used.margin_right.set(style.margin_right().to_px(inline_basis));
                used.border_left.set(style.border_left_width());
                used.border_right.set(style.border_right_width());
                if item_start > 0 {
                    used.margin_left.set(used.margin_left.get() + extra_margin);
                }
                if item_end < track_count {
                    used.margin_right.set(used.margin_right.get() + extra_margin);
                }
            } else {
                used.padding_top.set(style.padding_top().to_px(inline_basis));
                used.padding_bottom.set(style.padding_bottom().to_px(inline_basis));
                used.margin_top.set(style.margin_top().to_px(inline_basis));
                used.margin_bottom.set(style.margin_bottom().to_px(inline_basis));
                used.border_top.set(style.border_top_width());
                used.border_bottom.set(style.border_bottom_width());
                if item_start > 0 {
                    used.margin_top.set(used.margin_top.get() + extra_margin);
                }
                if item_end < track_count {
                    used.margin_bottom.set(used.margin_bottom.get() + extra_margin);
                }
            }
        }
    }

    fn item_alignment_for_node(&self, node: Node, axis: Axis) -> Alignment {
        let item_style = self.style(node);
        let container_style = self.style(self.grid_container);
        axis.select(
            inline_item_alignment(item_style.justify_self(), container_style.justify_items()),
            block_item_alignment(item_style.align_self(), container_style.align_items()),
        )
    }

    fn item_alignment(&self, item: GridItem, axis: Axis) -> Alignment {
        self.item_alignment_for_node(item.box_, axis)
    }

    fn item_margin_box_start(&self, item: GridItem, axis: Axis) -> CssPixels {
        let used = self.used(item);
        axis.select(
            used.margin_left.get() + used.border_left.get() + used.padding_left.get() + item.extra_margin_left,
            used.margin_top.get() + used.border_top.get() + used.padding_top.get() + item.extra_margin_top,
        )
    }

    fn item_margin_box_end(&self, item: GridItem, axis: Axis) -> CssPixels {
        let used = self.used(item);
        axis.select(
            used.padding_right.get() + used.border_right.get() + used.margin_right.get() + item.extra_margin_right,
            used.padding_bottom.get() + used.border_bottom.get() + used.margin_bottom.get() + item.extra_margin_bottom,
        )
    }

    fn non_cyclic_table_wrapper_inline_size(&self, item: GridItem, containing: CssPixels) -> CssPixels {
        let table_box = self.sizing().table_box_inside_wrapper(item.box_);
        let table_style = self.style(table_box);
        let wrapper_style = self.style(item.box_);
        if !wrapper_style.width().contains_percentage()
            && !wrapper_style.min_width().contains_percentage()
            && !wrapper_style.max_width().contains_percentage()
            && !table_style.width().contains_percentage()
            && !table_style.min_width().contains_percentage()
            && !table_style.max_width().contains_percentage()
        {
            return containing;
        }

        let container = self.container_used();
        if !container.has_definite_inline_size() {
            return containing;
        }

        let available = AvailableSize::definite(clamp_to_max_dimension_value(container.content_inline_size.get()));
        let tracks = self.axis_tracks(Axis::Column);
        let start = item.position(Axis::Column).max(0) as usize;
        let end = start.saturating_add(item.span(Axis::Column)).min(tracks.len());
        if !tracks[start..end]
            .iter()
            .any(|track| track.min_sizing.is_intrinsic(available) || track.max_sizing.is_intrinsic(available))
        {
            return containing;
        }

        let total = self.track_sum(Axis::Column);
        // CSS Grid breaks cyclic percentage dependencies during intrinsic track sizing. Percentage table width/min/max
        // constraints can contribute to intrinsic column tracks, so do not feed that contribution back into the table
        // wrapper containing block when resolving the final table width or margins.
        if total <= container.content_inline_size.get() {
            return containing;
        }

        let non_spanned = CssPixels::default().max(total - containing);
        let non_cyclic = CssPixels::default().max(container.content_inline_size.get() - non_spanned);
        containing.min(non_cyclic)
    }

    fn resolve_table_wrapper_inline_size(&self, item: GridItem, containing: CssPixels) -> ItemAlignment {
        let containing_for_wrapper = self.non_cyclic_table_wrapper_inline_size(item, containing);
        let containing_block = self.containing_block_size(item, Axis::Row);
        let available = AvailableSpace {
            inline_size: AvailableSize::definite(clamp_to_max_dimension_value(containing_for_wrapper)),
            block_size: AvailableSize::definite(clamp_to_max_dimension_value(containing_block)),
        };
        let mut constraints = self.container_constraints();
        constraints.percentage_basis_inline_size = Some(containing_for_wrapper);

        let mut wrapper_size = self.sizing().compute_table_box_inline_size_inside_wrapper(
            item.box_,
            available,
            constraints,
            Some(containing_for_wrapper),
            formatting_context::TableWrapperInlineSizeMode::UseTableUsedInlineSizeIfNotAuto,
        );
        let wrapper_style = self.style(item.box_);
        let table_box = self.sizing().table_box_inside_wrapper(item.box_);
        let table_style = self.style(table_box);
        let sizing = self.sizing();
        let current_used = self.used(item);
        let base_margin_start = wrapper_style.margin_left().to_px(containing_for_wrapper);
        let base_margin_end = wrapper_style.margin_right().to_px(containing_for_wrapper);
        let margin_box_start =
            self.item_margin_box_start(item, Axis::Column) - current_used.margin_left.get() + base_margin_start;
        let margin_box_end =
            self.item_margin_box_end(item, Axis::Column) - current_used.margin_right.get() + base_margin_end;

        if !wrapper_style.width().is_auto() {
            wrapper_size = wrapper_size.max(sizing.calculate_inner_size_for_property(
                item.box_,
                SizingAxis::Inline,
                SizingProperty::Width,
                available,
                self.grid_area_constraints(item),
            ));
        }
        if table_style.width().is_auto() {
            wrapper_size = wrapper_size.max(sizing.calculate_min_content_inline_size(item.box_, constraints));
        }

        let alignment = self.item_alignment(item, Axis::Column);
        if table_style.width().is_auto()
            && matches!(alignment, Alignment::Normal | Alignment::Stretch)
            && !wrapper_style.margin_left().is_auto()
            && !wrapper_style.margin_right().is_auto()
        {
            // Stretching the wrapper also stretches the auto-width table inside it, so the stretched inline size has to
            // respect the max-width of both the wrapper and the table box.
            let mut stretched = containing_for_wrapper - margin_box_start - margin_box_end;
            let area_constraints = self.grid_area_constraints(item);
            if !sizing.should_treat_max_inline_size_as_none(item.box_, available.inline_size, area_constraints) {
                stretched = stretched.min(sizing.calculate_inner_size_for_property(
                    item.box_,
                    SizingAxis::Inline,
                    SizingProperty::MaxWidth,
                    available,
                    area_constraints,
                ));
            }
            if !sizing.should_treat_max_inline_size_as_none(table_box, available.inline_size, area_constraints) {
                if table_style.max_width().is_length_percentage() {
                    let mut table_max = table_style.max_width().to_px(containing_for_wrapper);
                    if table_style.box_sizing() != box_sizing::BORDER_BOX {
                        table_max += table_style.border_left_width()
                            + table_style.padding_left().to_px(containing_for_wrapper)
                            + table_style.padding_right().to_px(containing_for_wrapper)
                            + table_style.border_right_width();
                    }
                    stretched = stretched.min(table_max);
                } else {
                    stretched = stretched.min(wrapper_size);
                }
            }
            wrapper_size = wrapper_size.max(stretched);
        }

        let area_constraints = self.grid_area_constraints(item);
        if !sizing.should_treat_max_inline_size_as_none(item.box_, available.inline_size, area_constraints) {
            wrapper_size = wrapper_size.min(sizing.calculate_inner_size_for_property(
                item.box_,
                SizingAxis::Inline,
                SizingProperty::MaxWidth,
                available,
                area_constraints,
            ));
        }
        if !wrapper_style.min_width().is_auto() {
            wrapper_size = wrapper_size.max(sizing.calculate_inner_size_for_property(
                item.box_,
                SizingAxis::Inline,
                SizingProperty::MinWidth,
                available,
                area_constraints,
            ));
        }

        align_item(
            wrapper_size,
            false,
            false,
            containing_for_wrapper,
            margin_box_start,
            margin_box_end,
            base_margin_start + item.extra_margin_left,
            base_margin_end + item.extra_margin_right,
            wrapper_style.margin_left().is_auto(),
            wrapper_style.margin_right().is_auto(),
            alignment,
        )
    }

    fn resolve_item_sizes(&mut self, axis: Axis) {
        for item_index in 0..self.items.len() {
            let item = self.items[item_index];
            // https://drafts.csswg.org/css-grid-1/#grid-item-sizing
            // A grid item is sized within the containing block defined by its grid area.
            let containing = self.containing_block_size(item, axis);
            let containing_inline = self.containing_block_size(item, Axis::Column);
            let containing_block = self.containing_block_size(item, Axis::Row);
            let style = self.style(item.box_);
            let facts = self.facts(item.box_);
            // https://drafts.csswg.org/css-sizing-3/#max-content-block-size
            // Usually the block size of the content after layout.
            // NB: The column pass has already resolved this item's content width and layout_items() lays the item out
            //     at that width, so block contents must be measured against the same basis. Orthogonal items with
            //     inline contents instead contribute their inline-axis size to the physical row axis.
            let use_resolved_content_width = !axis.is_column()
                && (style.writing_mode() == writing_mode::HORIZONTAL_TB || !facts.children_are_inline());
            let available_inline = if use_resolved_content_width {
                self.used(item).content_inline_size.get()
            } else {
                containing_inline
            };
            let available = AvailableSpace {
                inline_size: AvailableSize::definite(clamp_to_max_dimension_value(available_inline)),
                block_size: AvailableSize::definite(clamp_to_max_dimension_value(containing_block)),
            };
            let mut constraints = self.grid_area_constraints(item);
            if !axis.is_column() {
                constraints.percentage_basis_block_size = Some(containing);
            }
            let preferred = axis.select(style.width(), style.height());
            let alignment = self.item_alignment(item, axis);
            let has_natural = axis.select(
                facts.has_auto_content_width() || facts.has_auto_content_height() && facts.has_preferred_aspect_ratio(),
                facts.has_auto_content_height() || facts.has_auto_content_width() && facts.has_preferred_aspect_ratio(),
            );
            // https://drafts.csswg.org/css-grid-1/#grid-item-sizing
            // If the grid item has no preferred aspect ratio, and no natural size in the relevant axis (if it is a replaced
            // element), the grid item is sized as for 'align-self: stretch'.
            // INTEROP: Blink, WebKit, and Gecko instead use replaced sizing for normal-aligned replaced items. Match that
            //          behavior for leaf replaced boxes, while retaining our existing behavior for controls with children.
            let use_replaced = facts.is_replaced_box()
                && (has_natural || (alignment == Alignment::Normal && !facts.is_replaced_box_with_children()));

            if facts.is_table_wrapper() && axis.is_column() {
                // CSS Grid lays out each grid item into its grid-area containing block before alignment. For
                // display:table, the anonymous table wrapper is the grid item, while table layout computes the inner
                // table's border-box inline size, so resolve the wrapper with the same grid-area basis used later.
                let resolved = self.resolve_table_wrapper_inline_size(item, containing);
                let used = self.used(item);
                used.margin_left.set(resolved.margin_start);
                used.margin_right.set(resolved.margin_end);
                used.set_content_inline_size(resolved.size);
                continue;
            }

            let size = if use_replaced {
                if axis.is_column() {
                    self.sizing()
                        .compute_inline_size_for_replaced_element(item.box_, available, constraints)
                } else {
                    self.sizing()
                        .compute_block_size_for_replaced_element(item.box_, available, constraints)
                }
            } else if !axis.is_column()
                && preferred.is_auto()
                && facts.has_preferred_aspect_ratio()
                && self.used(item).has_definite_inline_size()
            {
                // NB: When the item has a preferred aspect ratio and a definite width, resolve the
                //     height through the aspect ratio instead of using fit-content sizing, which would
                //     incorrectly use the available width (grid area width) instead of the item's width.
                self.sizing().calculate_inner_size_for_property(
                    item.box_,
                    SizingAxis::Block,
                    SizingProperty::Height,
                    available,
                    constraints,
                )
            } else if preferred.is_auto()
                && matches!(alignment, Alignment::Stretch | Alignment::Normal)
                && !(if axis.is_column() {
                    style.margin_left().is_auto() || style.margin_right().is_auto()
                } else {
                    style.margin_top().is_auto() || style.margin_bottom().is_auto()
                })
            {
                // OPTIMIZATION: For auto-sized items with stretch/normal alignment and no auto margins, the item stretches
                //               to fill the containing block. We can compute this directly without the expensive
                //               calculate_fit_content_inline_size/height calls that trigger intrinsic sizing.
                // NB: Final grid item alignment works with the resolved grid area size. Percentage preferred sizes
                //     must resolve against that definite area instead of being reclassified as auto from the outer
                //     grid container's own definiteness.
                containing - self.item_margin_box_start(item, axis) - self.item_margin_box_end(item, axis)
            } else if preferred.is_auto() || preferred.is_fit_content() {
                self.sizing()
                    .calculate_fit_content_size(item.box_, axis.sizing_axis(), available, constraints)
            } else {
                self.sizing().calculate_inner_size_for_property(
                    item.box_,
                    axis.sizing_axis(),
                    axis.select(SizingProperty::Width, SizingProperty::Height),
                    available,
                    constraints,
                )
            };

            let used = self.used(item);
            let (used_margin_start, used_margin_end, start_auto, end_auto) = if axis.is_column() {
                (
                    used.margin_left.get() + item.extra_margin_left,
                    used.margin_right.get() + item.extra_margin_right,
                    style.margin_left().is_auto(),
                    style.margin_right().is_auto(),
                )
            } else {
                (
                    used.margin_top.get() + item.extra_margin_top,
                    used.margin_bottom.get() + item.extra_margin_bottom,
                    style.margin_top().is_auto(),
                    style.margin_bottom().is_auto(),
                )
            };
            let resolve_alignment = |size, size_is_auto| {
                align_item(
                    size,
                    size_is_auto,
                    facts.is_replaced_box(),
                    containing,
                    self.item_margin_box_start(item, axis),
                    self.item_margin_box_end(item, axis),
                    used_margin_start,
                    used_margin_end,
                    start_auto,
                    end_auto,
                    alignment,
                )
            };
            let mut resolved = resolve_alignment(size, preferred.is_auto());

            let maximum_is_none = if axis.is_column() {
                self.sizing()
                    .should_treat_max_inline_size_as_none(item.box_, available.inline_size, constraints)
            } else {
                self.sizing()
                    .should_treat_max_block_size_as_none(item.box_, available.block_size, constraints)
            };
            if !maximum_is_none {
                let maximum = self.sizing().calculate_inner_size_for_property(
                    item.box_,
                    axis.sizing_axis(),
                    axis.select(SizingProperty::MaxWidth, SizingProperty::MaxHeight),
                    available,
                    constraints,
                );
                if resolved.size > maximum {
                    resolved = resolve_alignment(maximum, false);
                }
            }
            let minimum = axis.select(style.min_width(), style.min_height());
            if !minimum.is_auto() {
                let minimum = self.sizing().calculate_inner_size_for_property(
                    item.box_,
                    axis.sizing_axis(),
                    axis.select(SizingProperty::MinWidth, SizingProperty::MinHeight),
                    available,
                    constraints,
                );
                if resolved.size < minimum {
                    resolved = resolve_alignment(minimum, false);
                }
            }

            let used = self.used(item);
            if axis.is_column() {
                used.margin_left.set(resolved.margin_start);
                used.margin_right.set(resolved.margin_end);
                used.set_content_inline_size(resolved.size);
            } else {
                used.margin_top.set(resolved.margin_start);
                used.margin_bottom.set(resolved.margin_end);
                used.set_content_block_size(resolved.size);
            }
        }
    }

    fn track_sum(&self, axis: Axis) -> CssPixels {
        self.interleaved_track_iter(axis)
            .fold(CssPixels::default(), |sum, track| sum + track.base_size)
    }

    fn grid_container_alignment_size(&self, axis: Axis) -> CssPixels {
        if axis.is_column() {
            return self.container_used().content_inline_size.get();
        }
        if self.use_row_alignment_container_size {
            let style = self.style(self.grid_container);
            if !style.min_height().is_auto() {
                return self
                    .row_alignment_container_size
                    .max(self.container_used().content_block_size.get());
            }
            return self.row_alignment_container_size;
        }
        if self.container_used().has_definite_block_size() {
            self.container_used().content_block_size.get()
        } else {
            self.row_alignment_container_size
        }
    }

    fn resolve_track_spacing(&mut self, axis: Axis) {
        let container = self.grid_container_alignment_size(axis);
        let track_sum = self
            .axis_tracks(axis)
            .iter()
            .fold(CssPixels::default(), |sum, track| sum + track.base_size);
        let alignment = self.content_alignment(axis);
        let minimum = self.resolved_gap(axis, AvailableSize::definite(container));
        let size = distributed_gap_size(alignment, container, track_sum, self.axis_gaps(axis).len(), minimum);
        let gaps = axis.select(&mut self.column_gaps, &mut self.row_gaps);
        for gap in gaps {
            gap.base_size = size;
        }
    }

    fn used_container_block_size_for_second_row_layout(&self) -> CssPixels {
        let mut block_size = self.automatic_content_block_size;
        let available = self.available_space.unwrap();
        let constraints = self.layout_input.unwrap().containing_block_constraints;
        let style = self.style(self.grid_container);
        let sizing = self.sizing();
        if !style.max_height().is_auto()
            && !sizing.should_treat_max_block_size_as_none(self.grid_container, available.block_size, constraints)
        {
            block_size = block_size.min(sizing.calculate_inner_size_for_property(
                self.grid_container,
                SizingAxis::Block,
                SizingProperty::MaxHeight,
                available,
                constraints,
            ));
        }
        if !style.min_height().is_auto() {
            block_size = block_size.max(sizing.calculate_inner_size_for_property(
                self.grid_container,
                SizingAxis::Block,
                SizingProperty::MinHeight,
                available,
                constraints,
            ));
        }
        block_size
    }

    fn rerun_rows_with_container_block_size(&mut self, block_size: CssPixels) {
        self.available_space.as_mut().unwrap().block_size = AvailableSize::definite(block_size);
        self.initialize_gaps_for_axis(Axis::Row, AvailableSize::definite(block_size));
        self.resolve_item_metrics(Axis::Row);
        self.run_track_sizing(Axis::Row);
        self.resolve_item_metrics(Axis::Row);
        self.resolve_item_sizes(Axis::Row);
    }

    fn grid_area(&self, item: GridItem) -> geometry::LogicalRect {
        let (inline_offset, inline_size) = self.axis_grid_area(Axis::Column, Some((item.column, item.column_span)));
        let (block_offset, block_size) = self.axis_grid_area(Axis::Row, Some((item.row, item.row_span)));
        geometry::LogicalRect {
            offset: geometry::LogicalOffset {
                inline_offset,
                block_offset,
            },
            size: geometry::LogicalSize {
                inline_size,
                block_size,
            },
        }
    }

    fn axis_grid_area(&self, axis: Axis, placement: Option<(i32, usize)>) -> (CssPixels, CssPixels) {
        let padding_start = self.container_padding_start(axis);
        let padding_end = self.container_padding_end(axis);
        let Some((position, span)) = placement else {
            let content_size = match self.axis_available(axis) {
                AvailableSize::Definite(size) => size,
                _ => self.track_sum(axis),
            };
            return (-padding_start, content_size + padding_start + padding_end);
        };
        if position == self.axis_tracks(axis).len() as i32 {
            return (self.track_sum(axis), padding_end);
        }

        let start = position.saturating_mul(2);
        let end = start.saturating_add(span.saturating_mul(2) as i32);
        let container = self.grid_container_alignment_size(axis);
        let alignment = self.content_alignment(axis);
        let initial_offset = content_start_offset(alignment, container, self.track_sum(axis));
        let mut start_offset = initial_offset;
        let mut end_offset = initial_offset;
        for track in self.interleaved_track_iter(axis).take(start.max(0) as usize) {
            start_offset += track.base_size;
        }
        for track in self.interleaved_track_iter(axis).take(end.max(0) as usize) {
            end_offset += track.base_size;
        }
        (start_offset, end_offset - start_offset)
    }

    fn absolute_axis_grid_area(
        &self,
        axis: Axis,
        start: ComputedGridPlacement,
        end: ComputedGridPlacement,
        placement_names: &[usize],
    ) -> (CssPixels, CssPixels) {
        let lines = self.axis_lines(axis);
        let explicit_lines = self.explicit_line_count(axis);
        let resolved = resolve_placement_position(
            start,
            end,
            placement_names,
            lines,
            explicit_lines,
            lines.len().saturating_sub(1),
        );
        let is_auto_positioned =
            |placement: ComputedGridPlacement| placement.kind != crate::layout::ComputedGridPlacementKind::Line as u8;
        let mut rect = self.axis_grid_area(
            axis,
            (!(is_auto_positioned(start) && is_auto_positioned(end))).then_some((resolved.start, resolved.span)),
        );

        let start_is_augmented =
            is_auto_positioned(start) && end.kind == crate::layout::ComputedGridPlacementKind::Line as u8;
        let end_is_augmented =
            is_auto_positioned(end) && start.kind == crate::layout::ComputedGridPlacementKind::Line as u8;
        if !start_is_augmented && !end_is_augmented {
            return rect;
        }

        // Instead of auto-placement, an auto value for a grid-placement property contributes a special line to the placement whose position
        // is that of the corresponding padding edge of the grid container (the padding edge of the scrollable area, if the grid container
        // overflows). These lines become the first and last lines (0th and -0th) of the augmented grid used for positioning absolutely-positioned items.
        let explicit_line_position = |line: i32| {
            let tracks = self.interleaved_tracks(axis);
            let alignment = self.content_alignment(axis);
            let mut offset = content_start_offset(
                alignment,
                self.axis_available(axis).to_px_or_zero(),
                self.track_sum(axis),
            );
            for track in tracks.iter().take(line.saturating_mul(2).max(0) as usize) {
                offset += track.base_size;
            }
            offset
        };
        let augmented_edge = |is_start: bool| {
            if is_start {
                -self.container_padding_start(axis)
            } else {
                let mut offset = match self.axis_available(axis) {
                    AvailableSize::Definite(size) => size,
                    _ => self.track_sum(axis),
                };
                offset += self.container_padding_end(axis);
                offset
            }
        };
        let start_offset = if start_is_augmented {
            augmented_edge(true)
        } else {
            explicit_line_position(resolved.start)
        };
        let end_offset = if end_is_augmented {
            augmented_edge(false)
        } else {
            explicit_line_position(resolved.end)
        };
        rect = (start_offset, end_offset - start_offset);
        rect
    }

    fn layout_items(&mut self, run: &FormattingContextRun) {
        for item_index in 0..self.items.len() {
            let item = self.items[item_index];
            let area = self.grid_area(item);
            let mut table_wrapper_inline_basis = None;
            if self.facts(item.box_).is_table_wrapper() {
                // Track spacing can expand the final grid area after the earlier inline-size pass. Recompute the wrapper
                // against that final area so the real table layout resolves percentages against the grid area.
                let resolved = self.resolve_table_wrapper_inline_size(item, area.size.inline_size);
                table_wrapper_inline_basis =
                    Some(self.non_cyclic_table_wrapper_inline_size(item, area.size.inline_size));
                let used = self.used(item);
                used.margin_left.set(resolved.margin_start);
                used.margin_right.set(resolved.margin_end);
                used.set_content_inline_size(resolved.size);
            }
            {
                let used = self.used(item);
                used.has_definite_inline_size.set(true);
                used.has_definite_block_size.set(true);
            }
            let input = LayoutInput {
                available_space: AvailableSpace {
                    inline_size: AvailableSize::definite(self.used(item).content_inline_size.get()),
                    block_size: AvailableSize::definite(self.used(item).content_block_size.get()),
                },
                containing_block_constraints: {
                    let mut constraints = self.grid_area_constraints(item);
                    constraints.percentage_basis_block_size = Some(area.size.block_size);
                    if let Some(inline_basis) = table_wrapper_inline_basis {
                        // Table wrappers pass their constraints through to the table box, so hand them the
                        // grid area in both axes for the table's percentage resolution.
                        constraints.percentage_basis_inline_size = Some(inline_basis);
                    }
                    constraints
                },
                content_box_position_in_bfc_root: None,
                sizing: RootSizingDirectives::default(),
                participation: ParticipationInParentFormattingContext::Item,
            };
            match formatting_context::layout_inside_child(
                run,
                None,
                Some(self),
                item.box_,
                LayoutMode::Normal,
                input,
                false,
            ) {
                ChildLayoutOutcome::Created(_) | ChildLayoutOutcome::Skipped => {}
                ChildLayoutOutcome::ReenterCurrent => {
                    self.run(run, input);
                }
            };
            let offset = FfiCssPixelPoint {
                x: area.offset.inline_offset + self.item_margin_box_start(item, Axis::Column),
                y: area.offset.block_offset + self.item_margin_box_start(item, Axis::Row),
            };
            // Resolve relative-position insets before placement seals the
            // item's committed metrics.
            abspos_engine::compute_inset_native(run, item.box_, area.size.inline_size, area.size.block_size);
            formatting_context::place_child(&self.formatting_context_run(), item.box_, offset, None);
        }
        self.derived_baselines_of_root_box =
            formatting_context::derive_baselines(&self.records, &self.callbacks, self.grid_container, false);
    }

    fn used_track_list_data(&self, axis: Axis, subgrid: bool) -> OwnedUsedGridTrackList {
        // https://drafts.csswg.org/css-grid-2/#resolved-track-list-subgrid
        // When an element generates a grid container box that is a subgrid, the resolved value of the
        // grid-template-rows and grid-template-columns properties represents the used number of columns,
        // serialized as the subgrid keyword followed by a list representing each of its lines as a line
        // name set of all the line's names explicitly defined on the subgrid, without using repeat().
        let lines = self.axis_lines(axis);
        let mut names = Vec::with_capacity(lines.len());
        for line in lines {
            names.push(
                line.iter()
                    .filter(|name| !name.implicit && (!subgrid || !name.adopted_from_parent))
                    .map(|name| name.raw)
                    .collect::<Vec<_>>(),
            );
        }
        let track_sizes = if subgrid {
            Vec::new()
        } else {
            self.axis_tracks(axis).iter().map(|track| track.base_size).collect()
        };
        OwnedUsedGridTrackList {
            is_subgrid: subgrid,
            lines: names,
            track_sizes,
        }
    }

    fn save_used_tracks(&self, grid_style: &GridValues) {
        // getComputedStyle() needs to return the resolved values of grid-template-columns and grid-template-rows
        // so they need to be saved in the state, and then assigned to paintables by the Rust commit walk.
        let tracks = OwnedUsedGridTracks {
            columns: self.used_track_list_data(Axis::Column, self.is_subgridded(Axis::Column, grid_style)),
            rows: self.used_track_list_data(Axis::Row, self.is_subgridded(Axis::Row, grid_style)),
        };
        self.container_used().rare_data_mut().used_grid_tracks = Some(std::rc::Rc::new(tracks));
    }

    fn save_devtools_data(&self, grid_style: &GridValues) {
        if !self.should_collect_devtools_layout_data {
            return;
        }
        let serialize = |axis: Axis| {
            let tracks = self.axis_tracks(axis);
            let gaps = self.axis_gaps(axis);
            let lines = self.axis_lines(axis);
            let explicit_start = axis.select(self.explicit_column_start, self.explicit_row_start);
            let explicit_count = self.explicit_line_count(axis);
            let alignment = self.content_alignment(axis);
            let mut start = content_start_offset(
                alignment,
                self.grid_container_alignment_size(axis),
                self.track_sum(axis),
            );
            let mut name_storage = Vec::with_capacity(lines.len());
            for line in lines {
                name_storage.push(
                    line.iter()
                        .map(|name| crate::css::serialize::fly_string_raw_to_string(name.raw))
                        .collect::<Vec<_>>(),
                );
            }
            let mut serialized_lines = Vec::with_capacity(lines.len());
            let mut serialized_tracks = Vec::with_capacity(tracks.len());
            for (index, names) in name_storage.into_iter().enumerate().take(lines.len()) {
                let breadth = index
                    .checked_sub(1)
                    .and_then(|gap| gaps.get(gap))
                    .map_or(CssPixels::default(), |gap| gap.base_size);
                serialized_lines.push(GridLayoutLine {
                    names,
                    start,
                    breadth,
                    type_: if index < explicit_start || index >= explicit_start + explicit_count {
                        GridTrackType::Implicit
                    } else {
                        GridTrackType::Explicit
                    },
                    number: if index < explicit_start {
                        0
                    } else {
                        (index - explicit_start + 1) as u32
                    },
                    negative_number: if index >= explicit_start + explicit_count {
                        0
                    } else {
                        -((explicit_start + explicit_count - index) as i32)
                    },
                });
                if let Some(track) = tracks.get(index) {
                    serialized_tracks.push(GridLayoutTrack {
                        start: start + breadth,
                        breadth: track.base_size,
                        type_: if index < explicit_start || index >= explicit_start + explicit_count.saturating_sub(1) {
                            GridTrackType::Implicit
                        } else {
                            GridTrackType::Explicit
                        },
                        state: if track.is_auto_repeat {
                            if track.is_auto_fit && track.is_collapsed {
                                GridTrackState::Removed
                            } else {
                                GridTrackState::Repeat
                            }
                        } else {
                            GridTrackState::Static
                        },
                    });
                    start += breadth + track.base_size;
                }
            }
            GridLayoutDimension {
                lines: serialized_lines,
                tracks: serialized_tracks,
            }
        };
        let columns = serialize(Axis::Column);
        let rows = serialize(Axis::Row);
        let name_raws = grid_style.names.raws();
        let areas = grid_style
            .areas
            .as_slice()
            .iter()
            .map(|area| GridLayoutArea {
                name: crate::css::serialize::fly_string_raw_to_string(name_raws[area.name_index as usize]),
                type_: GridTrackType::Explicit,
                row_start: area.row_start as u32 + 1,
                row_end: area.row_end as u32 + 1,
                column_start: area.column_start as u32 + 1,
                column_end: area.column_end as u32 + 1,
            })
            .collect::<Vec<_>>();
        let fragment = GridLayoutFragment { areas, columns, rows };
        let style = self.style(self.grid_container);
        let data = GridLayoutData {
            direction: style.direction(),
            writing_mode: style.writing_mode(),
            is_subgrid: self.is_subgridded(Axis::Column, grid_style) || self.is_subgridded(Axis::Row, grid_style),
            fragments: vec![fragment],
        };
        self.container_used().rare_data_mut().grid_layout_data = Some(std::rc::Rc::new(data));
    }

    pub(crate) fn run(&mut self, run: &FormattingContextRun, input: LayoutInput) {
        let available = input.available_space;
        // OPTIMIZATION: If we're in intrinsic sizing layout, but the grid container is not the
        //               box being measured, we can skip everything here.
        //               The parent formatting context has already figured out our size anyway.
        //               However, an inline-level container must still lay out its items, since the
        //               parent inline formatting context derives the fragment's baseline from them.
        if self.layout_mode == LayoutMode::IntrinsicSizing
            && !available.inline_size.is_intrinsic_sizing_constraint()
            && !available.block_size.is_intrinsic_sizing_constraint()
            && !self.facts(self.grid_container).display().is_inline_outside()
        {
            return;
        }
        self.reset_for_run(input);
        let grid_style = self.grid_style(self.grid_container);
        self.cache_subgrid_axes(grid_style);
        // NOTE: We store explicit grid sizes to later use in determining the position of items with negative index.
        let (columns, rows) = self.initialize_lines(grid_style);
        self.place_items();
        // NB: Gap tracks must be initialized after collapsing auto-fit tracks, since gutters next to collapsed tracks
        //     collapse as well.
        self.initialize_tracks(grid_style, &columns, &rows);

        // Do the first pass of resolving grid items box metrics to compute values that are independent of a track width
        self.resolve_item_metrics(Axis::Column);
        self.run_track_sizing(Axis::Column);
        // Do the second pass of resolving box metrics to compute values that depend on a track width
        self.resolve_item_metrics(Axis::Column);
        // Once the sizes of column tracks, which determine the widths of the grid areas forming the containing blocks
        // for grid items, ara calculated, it becomes possible to determine the final widths of the grid items.
        self.resolve_item_sizes(Axis::Column);

        // Do the first pass of resolving grid items box metrics to compute values that are independent of a track height
        self.resolve_item_metrics(Axis::Row);
        self.run_track_sizing(Axis::Row);
        // Do the second pass of resolving box metrics to compute values that depend on a track height
        self.resolve_item_metrics(Axis::Row);
        self.resolve_item_sizes(Axis::Row);

        self.automatic_content_block_size = self.track_sum(Axis::Row);
        self.row_alignment_container_size = self.automatic_content_block_size;
        self.use_row_alignment_container_size = false;
        let intrinsic_block_size = self.automatic_content_block_size;
        if self.layout_mode == LayoutMode::Normal && available.block_size == AvailableSize::Indefinite {
            let resolved_block_size = self.used_container_block_size_for_second_row_layout();
            self.rerun_rows_with_container_block_size(resolved_block_size);
            self.row_alignment_container_size = resolved_block_size;
            self.use_row_alignment_container_size = true;
            self.automatic_content_block_size = intrinsic_block_size;
        } else if self.layout_mode == LayoutMode::Normal
            && let AvailableSize::Definite(block_size) = available.block_size
            && self.sizing().should_treat_block_size_as_auto(
                self.grid_container,
                available,
                self.layout_input.unwrap().containing_block_constraints,
            )
        {
            self.row_alignment_container_size = block_size;
            self.use_row_alignment_container_size = true;
        }
        self.resolve_track_spacing(Axis::Column);
        self.resolve_track_spacing(Axis::Row);

        if available.inline_size.is_intrinsic_sizing_constraint()
            || available.block_size.is_intrinsic_sizing_constraint()
        {
            // https://www.w3.org/TR/css-grid-1/#intrinsic-sizes
            // The max-content size (min-content size) of a grid container is the sum of the grid container’s track sizes
            // (including gutters) in the appropriate axis, when the grid is sized under a max-content constraint (min-content constraint).
            if available.inline_size.is_intrinsic_sizing_constraint() {
                let size = self.track_sum(Axis::Column);
                self.container_used().set_content_inline_size(size);
            }
            if available.block_size.is_intrinsic_sizing_constraint() {
                let size = self.track_sum(Axis::Row);
                self.container_used().set_content_block_size(size);
            }
            return;
        }

        self.layout_items(run);
        self.save_used_tracks(grid_style);
        self.save_devtools_data(grid_style);
    }

    pub(crate) fn derived_baselines_of_root_box(&self) -> DerivedBaselines {
        self.derived_baselines_of_root_box
    }

    pub(crate) fn automatic_content_inline_size(&self) -> CssPixels {
        self.container_used().content_inline_size.get()
    }

    pub(crate) fn automatic_content_block_size(&self) -> CssPixels {
        self.automatic_content_block_size
    }

    pub(crate) fn parent_did_dimension(&self) {
        if self.layout_mode != LayoutMode::Normal {
            return;
        }
        let mut child = self.callbacks.first_child(self.grid_container);
        while !child.is_invalid() {
            let next = self.callbacks.next_sibling(child);
            if self.facts(child).is_absolutely_positioned() {
                let rect = abspos_inputs::StaticPositionRect {
                    rect: geometry::LogicalRect::default(),
                    inline_alignment: StaticPositionAlignment::Start,
                    block_alignment: StaticPositionAlignment::Start,
                    alignment_derives_from_own_computed_values: false,
                };
                // The grid area supplies both the containing block and the
                // static position for the grid's own abspos children.
                let containing_block_info = (self.callbacks.containing_block(child) == self.grid_container)
                    .then(|| self.abspos_containing_block_info(child));
                formatting_context::register_contained_abspos_child(
                    &self.callbacks,
                    self.fragments.as_deref(),
                    self.grid_container,
                    child,
                    rect,
                    containing_block_info,
                );
            }
            child = next;
        }
        if let Some(fragments) = self.fragments.as_deref() {
            for child in
                fragments.pending_abspos_children_awaiting_containing_block_info(self.grid_container, &self.callbacks)
            {
                // Deeper descendants inside grid items still get the grid area
                // as their containing block, but their static position comes
                // from their in-flow ancestor, so axis modes fall back to
                // their own insets.
                let mut info = self.abspos_containing_block_info(child);
                // Registration-time axis modes read raw style: anchor()
                // insets resolve later in layout_pending_child, and an
                // anchor-bearing inset is never auto either way.
                let (inline_axis_mode, block_axis_mode) = abspos_engine::axis_modes(self.style(child));
                info.inline_axis_mode = inline_axis_mode;
                info.block_axis_mode = block_axis_mode;
                fragments.register_abspos_containing_block_info(child, info);
            }
        }
    }

    // https://www.w3.org/TR/css-grid-2/#abspos-items
    pub(crate) fn abspos_containing_block_info(&self, node: Node) -> abspos_inputs::AbsposContainingBlockInfo {
        let grid_style = self.grid_style(node);
        let name_raws = grid_style.names.raws();
        let (block_offset, block_size) =
            self.absolute_axis_grid_area(Axis::Row, grid_style.row_start, grid_style.row_end, name_raws);
        let (inline_offset, inline_size) =
            self.absolute_axis_grid_area(Axis::Column, grid_style.column_start, grid_style.column_end, name_raws);
        abspos_inputs::AbsposContainingBlockInfo {
            rect: geometry::LogicalRect {
                offset: geometry::LogicalOffset {
                    inline_offset,
                    block_offset,
                },
                size: geometry::LogicalSize {
                    inline_size,
                    block_size,
                },
            },
            inline_axis_mode: abspos_inputs::AbsposAxisMode::InsetFromRect,
            block_axis_mode: abspos_inputs::AbsposAxisMode::InsetFromRect,
            inline_alignment: Some(abspos_alignment(self.item_alignment_for_node(node, Axis::Column))),
            block_alignment: Some(abspos_alignment(self.item_alignment_for_node(node, Axis::Row))),
            derives_from_own_computed_values: true,
        }
    }
}

fn abspos_alignment(alignment: Alignment) -> AbsposAlignment {
    match alignment {
        Alignment::Baseline => AbsposAlignment::Baseline,
        Alignment::Center => AbsposAlignment::Center,
        Alignment::End => AbsposAlignment::End,
        Alignment::Normal => AbsposAlignment::Normal,
        Alignment::Safe => AbsposAlignment::Safe,
        Alignment::SelfEnd => AbsposAlignment::SelfEnd,
        Alignment::SelfStart => AbsposAlignment::SelfStart,
        Alignment::SpaceAround => AbsposAlignment::SpaceAround,
        Alignment::SpaceBetween => AbsposAlignment::SpaceBetween,
        Alignment::SpaceEvenly => AbsposAlignment::SpaceEvenly,
        Alignment::Start => AbsposAlignment::Start,
        Alignment::Stretch => AbsposAlignment::Stretch,
        Alignment::Unsafe => AbsposAlignment::Unsafe,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SpaceDistributionPhase {
    Minimum,
    MinContent,
    MaxContent,
}

fn distribute_spanning_base_size_for_indices(
    tracks: &mut [Track],
    spanned: &[usize],
    item_size_contribution: CssPixels,
    phase: SpaceDistributionPhase,
    matcher: impl Fn(usize, &Track) -> bool,
) -> Vec<CssPixels> {
    let affected_positions = spanned
        .iter()
        .enumerate()
        .filter_map(|(position, index)| matcher(position, &tracks[*index]).then_some(position))
        .collect::<Vec<_>>();
    let mut increases = vec![CssPixels::default(); spanned.len()];
    if affected_positions.is_empty() {
        return increases;
    }

    // 1. Find the space to distribute:
    let spanned_size = spanned
        .iter()
        .fold(CssPixels::default(), |sum, index| sum + tracks[*index].base_size);
    // Subtract the corresponding size of every spanned track from the item’s size contribution to find the item’s
    // remaining size contribution.
    let mut extra_space = CssPixels::default().max(item_size_contribution - spanned_size);

    // 2. Distribute space up to limits:
    while extra_space > CssPixels::default() {
        if affected_positions
            .iter()
            .all(|position| tracks[spanned[*position]].base_size_frozen)
        {
            break;
        }
        // Find the item-incurred increase for each spanned track with an affected size by: distributing the space
        // equally among such tracks, freezing a track’s item-incurred increase as its affected size + item-incurred
        // increase reaches its limit
        let increase_per_track = CssPixels::from_raw(1).max(extra_space / affected_positions.len());
        for &position in &affected_positions {
            let index = spanned[position];
            if tracks[index].base_size_frozen {
                continue;
            }
            let mut increase = increase_per_track.min(extra_space);
            if let Some(growth_limit) = tracks[index].growth_limit {
                let maximum_increase = growth_limit - tracks[index].base_size;
                if increases[position] + increase >= maximum_increase {
                    tracks[index].base_size_frozen = true;
                    increase = maximum_increase - increases[position];
                }
            }
            increases[position] += increase;
            extra_space -= increase;
        }
    }

    // 3. Distribute space beyond limits
    if extra_space > CssPixels::default() {
        // If space remains after all tracks are frozen, unfreeze and continue to
        // distribute space to the item-incurred increase of...
        let mut beyond_limits = affected_positions
            .iter()
            .copied()
            .filter(|position| match phase {
                // when accommodating minimum contributions or accommodating min-content contributions: any affected track
                // that happens to also have an intrinsic max track sizing function
                SpaceDistributionPhase::Minimum | SpaceDistributionPhase::MinContent => {
                    tracks[spanned[*position]].max_is_intrinsic
                }
                // when accommodating max-content contributions into base sizes: any affected track that happens to also have
                // a max-content max track sizing function;
                SpaceDistributionPhase::MaxContent => tracks[spanned[*position]].max_is_max_content,
            })
            .collect::<Vec<_>>();
        if beyond_limits.is_empty() {
            // if there are no such tracks, then all affected tracks.
            beyond_limits.clone_from(&affected_positions);
        }

        let increase_per_track = extra_space / beyond_limits.len();
        for position in beyond_limits {
            let increase = increase_per_track.min(extra_space);
            increases[position] += increase;
            extra_space -= increase;
        }
    }

    // 4. For each affected track, if the track’s item-incurred increase is larger than the track’s planned increase
    //    set the track’s planned increase to that value.
    increases
}

#[derive(Clone, Debug)]
pub(crate) struct ItemContribution {
    /// Indices into the axis's interleaved track-and-gap array, in span order.
    pub(crate) spanned_tracks: Vec<usize>,
    pub(crate) span: usize,
    pub(crate) minimum: CssPixels,
    pub(crate) min_content: CssPixels,
    pub(crate) limited_min_content: CssPixels,
    pub(crate) max_content: CssPixels,
    pub(crate) limited_max_content: CssPixels,
    pub(crate) is_scroll_container: bool,
}

fn distribute_growth_limit(tracks: &mut [Track], spanned: &[usize], affected: &[usize], contribution: CssPixels) {
    if affected.is_empty() {
        return;
    }
    for &index in affected {
        tracks[index].item_incurred_increase = CssPixels::default();
    }
    // 1. Find the space to distribute:
    let accounted = spanned.iter().fold(CssPixels::default(), |sum, index| {
        sum + tracks[*index].growth_limit.unwrap_or(tracks[*index].base_size)
    });
    // Subtract the corresponding size of every spanned track from the item’s size contribution to find the item’s
    // remaining size contribution.
    let mut extra = CssPixels::default().max(contribution - accounted);
    // 2. Distribute space up to limits:
    while extra > CssPixels::default() {
        if affected.iter().all(|index| tracks[*index].growth_limit_frozen) {
            break;
        }
        // Find the item-incurred increase for each spanned track with an affected size by: distributing the space
        // equally among such tracks, freezing a track’s item-incurred increase as its affected size + item-incurred
        // increase reaches its limit
        let per_track = CssPixels::from_raw(1).max(extra / affected.len());
        for &index in affected {
            if tracks[index].growth_limit_frozen {
                continue;
            }
            let mut increase = per_track.min(extra);
            if !tracks[index].infinitely_growable
                && let Some(limit) = tracks[index].growth_limit
            {
                // For growth limits, the limit is infinity if it is marked as infinitely growable, and equal to the
                // growth limit otherwise.
                let maximum = limit - tracks[index].base_size;
                if tracks[index].item_incurred_increase + increase >= maximum {
                    tracks[index].growth_limit_frozen = true;
                    increase = maximum - tracks[index].item_incurred_increase;
                }
            }
            tracks[index].item_incurred_increase += increase;
            extra -= increase;
        }
    }
    // FIXME: 3. Distribute space beyond limits
    // 4. For each affected track, if the track’s item-incurred increase is larger than the track’s planned increase
    //    set the track’s planned increase to that value.
    for &index in spanned {
        tracks[index].planned_increase = tracks[index].planned_increase.max(tracks[index].item_incurred_increase);
    }
}

fn distribute_base_for_item(
    tracks: &mut [Track],
    spanned: &[usize],
    contribution: CssPixels,
    phase: SpaceDistributionPhase,
    matcher: impl Fn(&Track) -> bool,
) {
    let increases =
        distribute_spanning_base_size_for_indices(tracks, spanned, contribution, phase, |_, track| matcher(track));
    for (&index, increase) in spanned.iter().zip(increases) {
        if matcher(&tracks[index]) {
            tracks[index].item_incurred_increase = increase;
        }
        tracks[index].planned_increase = tracks[index].planned_increase.max(increase);
    }
}

fn apply_planned_base_increases(tracks: &mut [Track], spanned: &[usize]) {
    for &index in spanned {
        tracks[index].base_size += tracks[index].planned_increase;
        tracks[index].planned_increase = CssPixels::default();
    }
}

fn grow_content_sized_tracks_for_item(tracks: &mut [Track], item: &ItemContribution, available: AvailableSize) {
    let spanned = &item.spanned_tracks;
    let has_flexible = spanned
        .iter()
        .any(|index| tracks[*index].max_sizing.flex_factor().is_some());
    let has_intrinsic = spanned.iter().any(|index| {
        tracks[*index].min_sizing.is_intrinsic(available) || tracks[*index].max_sizing.is_intrinsic(available)
    });
    if !has_intrinsic || has_flexible {
        return;
    }

    // 1. For intrinsic minimums: First increase the base size of tracks with an intrinsic min track sizing
    //    function by distributing extra space as needed to accommodate these items’ minimum contributions.
    let minimum = if available.is_intrinsic_sizing_constraint() {
        // If the grid container is being sized under a min- or max-content constraint, use the items’ limited
        // min-content contributions in place of their minimum contributions here.
        item.limited_min_content
    } else {
        item.minimum
    };
    distribute_base_for_item(tracks, spanned, minimum, SpaceDistributionPhase::Minimum, |track| {
        track.min_sizing.is_intrinsic(available)
    });
    apply_planned_base_increases(tracks, spanned);

    // 2. For content-based minimums: Next continue to increase the base size of tracks with a min track
    //    sizing function of min-content or max-content by distributing extra space as needed to account for
    //    these items' min-content contributions.
    distribute_base_for_item(
        tracks,
        spanned,
        item.min_content,
        SpaceDistributionPhase::MinContent,
        |track| track.min_sizing.is_min_content() || track.min_sizing.is_max_content(),
    );
    apply_planned_base_increases(tracks, spanned);

    if available == AvailableSize::MaxContent {
        // 3. For max-content minimums: Next, if the grid container is being sized under a max-content constraint,
        //    continue to increase the base size of tracks with a min track sizing function of auto or max-content by
        //    distributing extra space as needed to account for these items' limited max-content contributions.
        distribute_base_for_item(
            tracks,
            spanned,
            item.limited_max_content,
            SpaceDistributionPhase::MaxContent,
            |track| track.min_sizing.is_auto(available) || track.min_sizing.is_max_content(),
        );
        apply_planned_base_increases(tracks, spanned);
    }

    // 4. If at this point any track’s growth limit is now less than its base size, increase its growth limit to
    //    match its base size.
    for track in tracks.iter_mut() {
        if !track.is_gap && track.growth_limit.is_some_and(|limit| limit < track.base_size) {
            track.growth_limit = Some(track.base_size);
        }
    }

    // 5. For intrinsic maximums: Next increase the growth limit of tracks with an intrinsic max track sizing
    let affected = spanned
        .iter()
        .copied()
        .filter(|index| tracks[*index].max_sizing.is_intrinsic(available))
        .collect::<Vec<_>>();
    distribute_growth_limit(tracks, spanned, &affected, item.min_content);
    for &index in spanned {
        if tracks[index].growth_limit.is_none() {
            // If the affected size is an infinite growth limit, set it to the track’s base size plus the planned increase.
            tracks[index].growth_limit = Some(tracks[index].base_size + tracks[index].planned_increase);
            // Mark any tracks whose growth limit changed from infinite to finite in this step as infinitely growable
            // for the next step.
            tracks[index].infinitely_growable = true;
        } else {
            tracks[index].growth_limit = Some(tracks[index].growth_limit.unwrap() + tracks[index].planned_increase);
        }
        tracks[index].planned_increase = CssPixels::default();
    }

    // 6. For max-content maximums: Lastly continue to increase the growth limit of tracks with a max track
    //    sizing function of max-content by distributing extra space as needed to account for these items' max-
    //    content contributions. However, limit the growth of any fit-content() tracks by their fit-content() argument.
    let affected = spanned
        .iter()
        .copied()
        .filter(|index| {
            tracks[*index].max_sizing.is_max_content()
                || tracks[*index].max_sizing.is_auto(available)
                || tracks[*index].max_sizing.is_fit_content()
        })
        .collect::<Vec<_>>();
    distribute_growth_limit(tracks, spanned, &affected, item.max_content);
    for &index in spanned {
        let increase = tracks[index].planned_increase;
        if let TrackSizingFunction::FitContent(_) = tracks[index].max_sizing {
            let mut limit = tracks[index].growth_limit.unwrap() + increase;
            limit = limit.max(tracks[index].base_size);
            let fit_limit = tracks[index].max_sizing.resolve(available);
            if limit > fit_limit {
                limit = tracks[index].base_size.max(fit_limit);
            }
            tracks[index].growth_limit = Some(limit);
        } else if tracks[index].growth_limit.is_none() {
            // If the affected size is an infinite growth limit, set it to the track’s base size plus the planned increase.
            tracks[index].growth_limit = Some(tracks[index].base_size + increase);
        } else {
            tracks[index].growth_limit = Some(tracks[index].growth_limit.unwrap() + increase);
        }
        tracks[index].planned_increase = CssPixels::default();
    }
}

pub(crate) fn resolve_intrinsic_track_sizes(
    tracks: &mut [Track],
    items: &[ItemContribution],
    available: AvailableSize,
    row_axis: bool,
) {
    // https://www.w3.org/TR/css-grid-2/#algo-content
    // 12.5. Resolve Intrinsic Track Sizes
    // This step resolves intrinsic track sizing functions to absolute lengths. First it resolves those
    // sizes based on items that are contained wholly within a single track. Then it gradually adds in
    // the space requirements of items that span multiple tracks, evenly distributing the extra space
    // across those tracks insofar as possible.
    //
    // FIXME: 1. Shim baseline-aligned items so their intrinsic size contributions reflect their baseline alignment.

    // 2. Size tracks to fit non-spanning items:

    // 3. Increase sizes to accommodate spanning items crossing content-sized tracks: Next, consider the
    // items with a span of 2 that do not span a track with a flexible sizing function.
    // Repeat incrementally for items with greater spans until all items have been considered.
    let max_span = items.iter().map(|item| item.span).max().unwrap_or(1).max(1);
    for span in 1..=max_span {
        for item in items.iter().filter(|item| item.span == span) {
            grow_content_sized_tracks_for_item(tracks, item, available);
        }
    }

    // 4. Increase sizes to accommodate spanning items crossing flexible tracks: Next, repeat the previous
    // step instead considering (together, rather than grouped by span size) all items that do span a
    // track with a flexible sizing function while
    //
    // https://www.w3.org/TR/css-grid-1/#algo-spanning-flex-items
    // 11.5.4. Increase sizes to accommodate spanning items crossing flexible tracks
    let dominated = |track: &Track| {
        available == AvailableSize::MaxContent
            || (row_axis && available == AvailableSize::MinContent)
            || track.min_sizing.is_intrinsic(available)
    };
    let mut contributions = vec![CssPixels::default(); tracks.len()];
    for item in items {
        // NB: This step repeats the content-sized track step, but only distributes space to flexible tracks. For
        //     min-content column sizing, the later "Expand Flexible Tracks" step resolves the flex fraction to zero, so
        //     flexible columns must not grow beyond their items' minimum contribution here. Keep min-content row sizing
        //     here so intrinsic-height grids still account for their contents.
        let mut total_flex = 0.0;
        let mut flexible_count = 0usize;
        let mut non_flexible_space = CssPixels::default();
        for &index in &item.spanned_tracks {
            if let Some(factor) = tracks[index].max_sizing.flex_factor()
                && dominated(&tracks[index])
            {
                total_flex += factor;
                flexible_count += 1;
            } else {
                non_flexible_space += tracks[index].base_size;
            }
        }
        if flexible_count == 0 {
            continue;
        }
        let use_limited_min_content =
            available == AvailableSize::MaxContent || (row_axis && available == AvailableSize::MinContent);
        let mut contribution = if use_limited_min_content {
            if total_flex == 0.0 && item.is_scroll_container {
                // https://drafts.csswg.org/css-grid-2/#min-size-auto
                // A grid item's automatic minimum size is zero if its computed overflow is a scrollable
                // overflow value. Preserve that zero minimum for collapsed zero-flex tracks.
                item.minimum
            } else {
                item.limited_min_content
            }
        } else {
            item.minimum
        };
        // NB: Subtract the space already accounted for by non-flexible spanned tracks (sized in 11.5.3), since only
        //     the remaining contribution needs to be distributed among flexible tracks.
        contribution = CssPixels::default().max(contribution - non_flexible_space);
        // Distributing space to flexible tracks:
        // - If the sum of the flexible sizing functions of all flexible tracks spanned by the item is greater
        //   than or equal to one, distributing space to such tracks according to the ratios of their flexible
        //   sizing functions rather than distributing space equally.
        // - If the sum is less than one, distributing that proportion of space according to the ratios of their
        //   flexible sizing functions and the rest equally.
        // FIXME: Handle 0 < total_flex < 1 case separately per spec.
        for &index in &item.spanned_tracks {
            let Some(factor) = tracks[index].max_sizing.flex_factor() else {
                continue;
            };
            if !dominated(&tracks[index]) {
                continue;
            }
            let share = if total_flex > 0.0 {
                CssPixels::nearest_value_for(contribution.to_double() * (factor / total_flex))
            } else {
                contribution / flexible_count
            };
            contributions[index] = contributions[index].max(share);
        }
    }
    for (track, contribution) in tracks.iter_mut().zip(contributions) {
        track.base_size = track.base_size.max(contribution);
        if track.growth_limit.is_some_and(|limit| limit < track.base_size) {
            // If at this point any track's growth limit is now less than its base size, increase its growth limit to match
            // its base size.
            track.growth_limit = Some(track.base_size);
        }
    }

    // 5. If any track still has an infinite growth limit (because, for example, it had no items placed in
    // it or it is a flexible track), set its growth limit to its base size.
    for track in tracks {
        if track.growth_limit.is_none() {
            track.growth_limit = Some(track.base_size);
        }
    }
}

pub(crate) fn maximize_tracks(tracks: &mut [Track], gap_size: CssPixels, available: AvailableSize) {
    // https://www.w3.org/TR/css-grid-2/#algo-grow-tracks
    // 12.6. Maximize Tracks
    // https://www.w3.org/TR/css-grid-2/#algo-terms
    // free space: Equal to the available grid space minus the sum of the base sizes of all the grid
    // tracks (including gutters), floored at zero. If available grid space is indefinite, the free
    // space is indefinite as well.
    // For the purpose of this step: if sizing the grid container under a max-content constraint, the
    // free space is infinite; if sizing under a min-content constraint, the free space is zero.
    let mut free_space = match available {
        AvailableSize::Definite(available_size) => {
            let track_sum = tracks.iter().fold(gap_size, |sum, track| sum + track.base_size);
            CssPixels::default().max(available_size - track_sum)
        }
        AvailableSize::MinContent => CssPixels::default(),
        AvailableSize::MaxContent | AvailableSize::Indefinite => CssPixels::from_raw(i32::MAX),
    };
    let mut growable = tracks
        .iter()
        .filter(|track| !track.base_size_frozen && track.growth_limit.is_some_and(|limit| track.base_size < limit))
        .count();
    // If the free space is positive, distribute it equally to the base sizes of all tracks, freezing
    // tracks as they reach their growth limits (and continuing to grow the unfrozen tracks as needed).
    while free_space > CssPixels::default() && growable > 0 {
        let per_track = free_space / growable;
        let old_free_space = free_space;
        for track in tracks.iter_mut() {
            if track.base_size_frozen {
                continue;
            }
            let Some(limit) = track.growth_limit else {
                continue;
            };
            if track.base_size >= limit {
                continue;
            }
            track.base_size = limit.min(track.base_size + per_track);
            if track.base_size >= limit {
                growable -= 1;
            }
        }
        free_space = match available {
            AvailableSize::Definite(available_size) => {
                let sum = tracks.iter().fold(gap_size, |sum, track| sum + track.base_size);
                CssPixels::default().max(available_size - sum)
            }
            AvailableSize::MinContent => CssPixels::default(),
            _ => CssPixels::from_raw(i32::MAX),
        };
        if free_space == old_free_space {
            break;
        }
    }
}

pub(crate) fn expand_flexible_tracks_indefinite(tracks: &mut [Track], items: &[ItemContribution]) {
    // First, find the grid’s used flex fraction:
    // Otherwise, if the free space is an indefinite length:
    // The used flex fraction is the maximum of:
    let mut flex_fraction = formatting_context::PixelFraction::zero();
    // For each flexible track, if the flexible track’s flex factor is greater than one, the result of dividing
    // the track’s base size by its flex factor; otherwise, the track’s base size.
    for track in tracks.iter() {
        if let Some(factor) = track.flex_factor {
            let divisor = CssPixels::nearest_value_for(factor.max(1.0));
            flex_fraction = flex_fraction.max(formatting_context::PixelFraction::new(track.base_size, divisor));
        }
    }
    // For each grid item that crosses a flexible track, the result of finding the size of an fr using all the
    // grid tracks that the item crosses and a space to fill of the item’s max-content contribution.
    for item in items {
        if !item
            .spanned_tracks
            .iter()
            .any(|index| tracks[*index].flex_factor.is_some())
        {
            continue;
        }
        let local = item
            .spanned_tracks
            .iter()
            .map(|index| tracks[*index])
            .collect::<Vec<_>>();
        flex_fraction = flex_fraction.max(find_fr_size(&local, item.max_content));
    }
    for track in tracks {
        if let Some(factor) = track.flex_factor {
            track.base_size = track
                .base_size
                .max(flex_fraction.multiply(CssPixels::nearest_value_for(factor)));
        }
    }
}

pub(crate) fn stretch_auto_tracks(
    tracks: &mut [Track],
    gap_size: CssPixels,
    available: AvailableSize,
    content_distribution_is_normal_or_stretch: bool,
) {
    // https://www.w3.org/TR/css-grid-2/#algo-stretch
    // 12.8. Stretch auto Tracks
    // This step expands tracks that have an auto max track sizing function by dividing any remaining positive,
    // definite free space equally amongst them. If the free space is indefinite, but the grid container has a
    // definite min-width/height, use that size to calculate the free space for this step instead.
    if !content_distribution_is_normal_or_stretch {
        return;
    }
    let auto_count = tracks
        .iter()
        .filter(|track| track.max_sizing.is_auto(available))
        .count();
    if auto_count == 0 {
        return;
    }
    let remaining = if let AvailableSize::Definite(available_size) = available {
        let sum = tracks.iter().fold(gap_size, |sum, track| sum + track.base_size);
        CssPixels::default().max(available_size - sum)
    } else {
        CssPixels::default()
    };
    let per_track = remaining / auto_count;
    for track in tracks {
        if track.max_sizing.is_auto(available) {
            track.base_size += per_track;
        }
    }
}

pub(crate) fn run_track_sizing<MaximumSize>(
    tracks: &mut [Track],
    gap_size: CssPixels,
    items: &[ItemContribution],
    available: AvailableSize,
    grid_container_maximum_size: MaximumSize,
    row_axis: bool,
    content_distribution_is_normal_or_stretch: bool,
) where
    MaximumSize: FnOnce() -> Option<CssPixels>,
{
    // https://www.w3.org/TR/css-grid-2/#algo-track-sizing
    // 12.3. Track Sizing Algorithm
    //
    // 1. Initialize Track Sizes
    let has_flexible = initialize_track_sizes(tracks, available);
    // 2. Resolve Intrinsic Track Sizes
    resolve_intrinsic_track_sizes(tracks, items, available, row_axis);
    // 3. Maximize Tracks
    let saved_base_sizes = tracks
        .iter()
        .filter(|track| !track.is_gap)
        .map(|track| track.base_size)
        .collect::<Vec<_>>();
    maximize_tracks(tracks, gap_size, available);
    // If this would cause the grid to be larger than the grid container’s inner size as limited by its
    // max-width/height, then redo this step, treating the available grid space as equal to the grid
    // container’s inner size when it’s sized to its max-width/height.
    let grid_container_inner_size = tracks
        .iter()
        .filter(|track| !track.is_gap)
        .fold(CssPixels::default(), |sum, track| sum + track.base_size);
    if let Some(maximum_size) = grid_container_maximum_size()
        && grid_container_inner_size > maximum_size
    {
        for (track, saved_base_size) in tracks.iter_mut().filter(|track| !track.is_gap).zip(saved_base_sizes) {
            track.base_size = saved_base_size;
        }
        maximize_tracks(tracks, gap_size, AvailableSize::definite(maximum_size));
    }
    // 4. Expand Flexible Tracks
    if has_flexible {
        // https://drafts.csswg.org/css-grid/#algo-flex-tracks
        // 12.7. Expand Flexible Tracks
        // This step sizes flexible tracks using the largest value it can assign to an fr without exceeding
        // the available space.
        if let AvailableSize::Definite(available_size) = available {
            // First, find the grid’s used flex fraction:
            // Otherwise, if the free space is a definite length:
            // The used flex fraction is the result of finding the size of an fr using all of the grid tracks and a space
            // to fill of the available grid space.
            expand_flexible_tracks(tracks, available_size - gap_size);
        // If the free space is zero or if sizing the grid container under a min-content constraint:
        // The used flex fraction is zero.
        } else if available != AvailableSize::MinContent {
            expand_flexible_tracks_indefinite(tracks, items);
        }
    }
    // 5. Expand Stretched auto Tracks
    stretch_auto_tracks(tracks, gap_size, available, content_distribution_is_normal_or_stretch);
    // If calculating the layout of a grid item in this step depends on the available space in the block
    // axis, assume the available space that it would have if any row with a definite max track sizing
    // function had that size and all other rows were infinite. If both the grid container and all
    // tracks have definite sizes, also apply align-content to find the final effective size of any gaps
    // spanned by such items; otherwise ignore the effects of track alignment in this estimation.
}

pub(crate) const REPEAT_AUTO_FIT: u8 = 0;
const REPEAT_AUTO_FILL: u8 = 1;
pub(crate) const REPEAT_FIXED: u8 = 2;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct LineName {
    pub(crate) name_index: u32,
    pub(crate) raw: usize,
    pub(crate) implicit: bool,
    pub(crate) adopted_from_parent: bool,
    pub(crate) area_name_raw: usize,
    pub(crate) area_is_start: bool,
}

impl LineName {
    pub(crate) fn explicit(name_index: u32, raw: usize) -> Self {
        Self {
            name_index,
            raw,
            implicit: false,
            adopted_from_parent: false,
            area_name_raw: 0,
            area_is_start: false,
        }
    }

    pub(crate) fn implicit(name_index: u32, raw: usize, area_name_raw: usize, area_is_start: bool) -> Self {
        Self {
            name_index,
            raw,
            implicit: true,
            adopted_from_parent: false,
            area_name_raw,
            area_is_start,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct TrackDefinition {
    pub(crate) min: GridTrackBreadth,
    pub(crate) max: GridTrackBreadth,
    pub(crate) is_auto_fit: bool,
    pub(crate) is_auto_repeat: bool,
}

#[derive(Clone, Debug, Default)]
pub(crate) struct ExpandedTrackList {
    pub(crate) lines: Vec<Vec<LineName>>,
    pub(crate) tracks: Vec<TrackDefinition>,
}

#[derive(Clone, Copy)]
pub(crate) struct TrackListSource {
    pub(crate) names: &'static [usize],
    pub(crate) entries: &'static [ComputedGridTrackEntry],
    pub(crate) name_indices: &'static [u32],
}

impl TrackListSource {
    fn from_grid_style(grid_style: &'static GridValues) -> Self {
        Self {
            names: grid_style.names.raws(),
            entries: grid_style.entries.as_slice(),
            name_indices: grid_style.name_indices.as_slice(),
        }
    }

    fn entry(&self, index: u32) -> &'static ComputedGridTrackEntry {
        assert_ne!(index, GRID_NO_INDEX);
        &self.entries[index as usize]
    }

    fn names(&self, entry: &ComputedGridTrackEntry) -> impl Iterator<Item = LineName> + 'static {
        let end = entry
            .name_index_start
            .checked_add(entry.name_index_count)
            .expect("grid line-name range overflow");
        let name_raws = self.names;
        self.name_indices[entry.name_index_start..end]
            .iter()
            .copied()
            .map(move |name_index| LineName::explicit(name_index, name_raws[name_index as usize]))
    }

    fn for_each_entry(
        &self,
        list: ComputedGridTrackList,
        mut callback: impl FnMut(u32, &'static ComputedGridTrackEntry),
    ) {
        let mut index = list.first_entry;
        let mut visited = 0usize;
        while index != GRID_NO_INDEX {
            assert!(visited < self.entries.len(), "cyclic grid track-list snapshot");
            let entry = self.entry(index);
            callback(index, entry);
            index = entry.next_sibling;
            visited += 1;
        }
    }
}

fn definition_for(entry: &'static ComputedGridTrackEntry, auto_fit: bool, auto_repeat: bool) -> TrackDefinition {
    match entry.kind {
        kind if kind == ComputedGridTrackEntryKind::TrackSize as u8 => {
            let size = grid_track_breadth_view(&entry.size);
            // https://drafts.csswg.org/css-grid-2/#algo-terms
            // min track sizing function:
            // If the track was sized with a minmax() function, this is the first argument to that function.
            // If the track was sized with a <flex> value or fit-content() function, auto. Otherwise, the track’s sizing function.
            let min = if matches!(size, GridTrackBreadth::Flex(_) | GridTrackBreadth::FitContent(_)) {
                GridTrackBreadth::Auto
            } else {
                size
            };
            TrackDefinition {
                min,
                max: size,
                is_auto_fit: auto_fit,
                is_auto_repeat: auto_repeat,
            }
        }
        kind if kind == ComputedGridTrackEntryKind::MinMax as u8 => TrackDefinition {
            min: grid_track_breadth_view(&entry.min_size),
            max: grid_track_breadth_view(&entry.max_size),
            is_auto_fit: auto_fit,
            is_auto_repeat: auto_repeat,
        },
        _ => unreachable!("only track-size entries form tracks"),
    }
}

#[allow(clippy::too_many_arguments)]
fn expand_standalone_list(
    source: TrackListSource,
    list: ComputedGridTrackList,
    lines: &mut Vec<Vec<LineName>>,
    tracks: &mut Vec<TrackDefinition>,
    pending_names: &mut Vec<LineName>,
    auto_repeat_count: &mut impl FnMut(u32, &ComputedGridTrackEntry) -> usize,
    inherited_auto_fit: bool,
    inherited_auto_repeat: bool,
) {
    source.for_each_entry(list, |entry_index, entry| match entry.kind {
        kind if kind == ComputedGridTrackEntryKind::LineNames as u8 => {
            pending_names.extend(source.names(entry));
        }
        kind if kind == ComputedGridTrackEntryKind::TrackSize as u8
            || kind == ComputedGridTrackEntryKind::MinMax as u8 =>
        {
            lines.push(std::mem::take(pending_names));
            tracks.push(definition_for(entry, inherited_auto_fit, inherited_auto_repeat));
        }
        kind if kind == ComputedGridTrackEntryKind::Repeat as u8 => {
            let is_auto_fit = entry.repeat_type == REPEAT_AUTO_FIT;
            let is_auto_repeat = is_auto_fit || entry.repeat_type == REPEAT_AUTO_FILL;
            let repeat_count = match entry.repeat_type {
                REPEAT_FIXED => entry.repeat_count,
                REPEAT_AUTO_FIT | REPEAT_AUTO_FILL => auto_repeat_count(entry_index, entry),
                _ => unreachable!("invalid grid repeat type"),
            };
            for _ in 0..repeat_count {
                expand_standalone_list(
                    source,
                    entry.repeat_list,
                    lines,
                    tracks,
                    pending_names,
                    auto_repeat_count,
                    inherited_auto_fit || is_auto_fit,
                    inherited_auto_repeat || is_auto_repeat,
                );
            }
        }
        _ => unreachable!("invalid grid track-list entry kind"),
    });
}

/// Expands the recursive FFI encoding into the exact line/track order consumed
/// by placement. `auto_repeat_count` performs the container-size-dependent
/// auto-fill/auto-fit calculation.
pub(crate) fn expand_standalone(
    source: TrackListSource,
    list: ComputedGridTrackList,
    mut auto_repeat_count: impl FnMut(u32, &ComputedGridTrackEntry) -> usize,
) -> ExpandedTrackList {
    if list.is_subgrid {
        // https://drafts.csswg.org/css-grid-2/#subgrid-listing
        // If there is no parent grid, or if the grid container is otherwise
        // forced to establish an independent formatting context, the used value
        // is the initial value, grid-template-rows/none, and the grid container
        // is not a subgrid.
        return ExpandedTrackList {
            lines: vec![Vec::new()],
            tracks: Vec::new(),
        };
    }

    let mut result = ExpandedTrackList::default();
    let mut pending_names = Vec::new();
    expand_standalone_list(
        source,
        list,
        &mut result.lines,
        &mut result.tracks,
        &mut pending_names,
        &mut auto_repeat_count,
        false,
        false,
    );
    result.lines.push(pending_names);
    result
}

pub(crate) fn count_subgrid_line_name_lists(source: TrackListSource, list: ComputedGridTrackList) -> usize {
    let mut count = 0usize;
    source.for_each_entry(list, |_index, entry| match entry.kind {
        kind if kind == ComputedGridTrackEntryKind::LineNames as u8 => count += 1,
        kind if kind == ComputedGridTrackEntryKind::Repeat as u8 => {
            let nested = count_subgrid_line_name_lists(source, entry.repeat_list);
            count += if entry.repeat_type == REPEAT_FIXED {
                nested.saturating_mul(entry.repeat_count)
            } else {
                nested
            };
        }
        _ => {}
    });
    count
}

pub(crate) fn automatic_subgrid_span(source: TrackListSource, list: ComputedGridTrackList) -> usize {
    count_subgrid_line_name_lists(source, list).saturating_sub(1).max(1)
}

fn expand_subgrid_names(
    source: TrackListSource,
    list: ComputedGridTrackList,
    lines: &mut [Vec<LineName>],
    line_index: &mut usize,
) {
    let entries = {
        let mut entries: Vec<(u32, &ComputedGridTrackEntry)> = Vec::new();
        source.for_each_entry(list, |index, entry| entries.push((index, entry)));
        entries
    };

    for (position, (_entry_index, entry)) in entries.iter().enumerate() {
        match entry.kind {
            kind if kind == ComputedGridTrackEntryKind::LineNames as u8 => {
                if let Some(line) = lines.get_mut(*line_index) {
                    line.extend(source.names(entry));
                }
                *line_index += 1;
            }
            kind if kind == ComputedGridTrackEntryKind::Repeat as u8 => {
                let repeat_count = match entry.repeat_type {
                    REPEAT_FIXED => entry.repeat_count,
                    REPEAT_AUTO_FILL => {
                        // https://drafts.csswg.org/css-grid-2/#auto-repeat
                        // On a subgridded axis, the auto-fill keyword is only valid once per
                        // <line-name-list>, and repeats enough times for the name list to match the
                        // subgrid's specified grid span, falling back to 0 if the span is already
                        // fulfilled.
                        let per_repeat = count_subgrid_line_name_lists(source, entry.repeat_list);
                        let remaining = entries[position + 1..]
                            .iter()
                            .map(|(_, entry)| match entry.kind {
                                kind if kind == ComputedGridTrackEntryKind::LineNames as u8 => 1,
                                kind if kind == ComputedGridTrackEntryKind::Repeat as u8 => {
                                    count_subgrid_line_name_lists(source, entry.repeat_list)
                                        * if entry.repeat_type == REPEAT_FIXED {
                                            entry.repeat_count
                                        } else {
                                            1
                                        }
                                }
                                _ => 0,
                            })
                            .sum::<usize>();
                        if per_repeat > 0 && *line_index < lines.len() && lines.len() - *line_index > remaining {
                            (lines.len() - *line_index - remaining) / per_repeat
                        } else {
                            0
                        }
                    }
                    REPEAT_AUTO_FIT => 0,
                    _ => unreachable!("invalid grid repeat type"),
                };
                for _ in 0..repeat_count {
                    expand_subgrid_names(source, entry.repeat_list, lines, line_index);
                }
            }
            _ => {}
        }
    }
}

pub(crate) fn expand_subgrid(
    source: TrackListSource,
    list: ComputedGridTrackList,
    track_count: usize,
    inherited_lines: &[Vec<LineName>],
) -> ExpandedTrackList {
    // https://drafts.csswg.org/css-grid-2/#subgrid-span
    // The number of explicit tracks in the subgrid in a subgridded dimension always corresponds
    // to the number of grid tracks that it spans in its parent grid.
    let mut lines = vec![Vec::new(); track_count.saturating_add(1)];
    // https://drafts.csswg.org/css-grid-2/#subgrid-line-name-inheritance
    // Since subgrids can be placed before their contents are placed, the subgridded lines
    // automatically receive the explicitly-assigned line names specified on the corresponding
    // lines of the parent grid. These names are in addition to any line names specified locally
    // on the subgrid.
    for (line, inherited) in lines.iter_mut().zip(inherited_lines) {
        line.extend(inherited.iter().filter(|name| !name.implicit).map(|name| LineName {
            name_index: name.name_index,
            raw: name.raw,
            implicit: false,
            adopted_from_parent: true,
            area_name_raw: 0,
            area_is_start: false,
        }));
    }
    let mut line_index = 0;
    expand_subgrid_names(source, list, &mut lines, &mut line_index);
    ExpandedTrackList {
        lines,
        tracks: Vec::new(),
    }
}

pub(crate) fn add_template_area_lines(
    columns: &mut Vec<Vec<LineName>>,
    rows: &mut Vec<Vec<LineName>>,
    areas: &[ComputedGridArea],
    names: &[usize],
) {
    // https://www.w3.org/TR/css-grid-2/#implicitly-assigned-line-name
    // 7.3.2. Implicitly-Assigned Line Names
    // The grid-template-areas property generates implicitly-assigned line names from the named grid areas in the
    // template. For each named grid area foo, four implicitly-assigned line names are created: two named foo-start,
    // naming the row-start and column-start lines of the named grid area, and two named foo-end, naming the row-end
    // and column-end lines of the named grid area.
    let max_column = areas.iter().map(|area| area.column_end).max().unwrap_or_default();
    let max_row = areas.iter().map(|area| area.row_end).max().unwrap_or_default();
    columns.resize_with(columns.len().max(max_column.saturating_add(1)), Vec::new);
    rows.resize_with(rows.len().max(max_row.saturating_add(1)), Vec::new);

    for area in areas {
        columns[area.column_start].push(LineName::implicit(
            area.implicit_start_name_index,
            names[area.implicit_start_name_index as usize],
            names[area.name_index as usize],
            true,
        ));
        columns[area.column_end].push(LineName::implicit(
            area.implicit_end_name_index,
            names[area.implicit_end_name_index as usize],
            names[area.name_index as usize],
            false,
        ));
        rows[area.row_start].push(LineName::implicit(
            area.implicit_start_name_index,
            names[area.implicit_start_name_index as usize],
            names[area.name_index as usize],
            true,
        ));
        rows[area.row_end].push(LineName::implicit(
            area.implicit_end_name_index,
            names[area.implicit_end_name_index as usize],
            names[area.name_index as usize],
            false,
        ));
    }
}

pub(crate) fn nth_named_line(lines: &[Vec<LineName>], name_raw: usize, nth_line: i32) -> Option<i32> {
    // FIXME: If not enough lines with the name exist, all implicit grid lines on the side
    // of the explicit grid corresponding to the search direction are assumed to have that name for the purpose of counting this span.
    // Source: https://drafts.csswg.org/css-grid/#line-placement
    let mut remaining = if nth_line < 0 {
        lines.len().wrapping_add_signed(nth_line as isize)
    } else {
        nth_line.saturating_sub(1) as usize
    };
    for (line_index, names) in lines.iter().enumerate() {
        for name in names {
            if name.raw != name_raw {
                continue;
            }
            if remaining == 0 {
                // https://drafts.csswg.org/css-grid/#line-placement
                // Contributes the nth grid line to the grid item’s placement.
                return Some(line_index as i32);
            }
            remaining = remaining.wrapping_sub(1);
        }
    }
    None
}

#[derive(Clone, Copy, Debug)]
pub(crate) enum TrackSizingFunction {
    Auto,
    Fixed(&'static ComputedSize),
    /// A synthesized fixed track with no backing style value: collapsed
    /// tracks, gap tracks, and fixed subgrid tracks carry a resolved px size.
    FixedPx(CssPixels),
    Flex(f64),
    MinContent,
    MaxContent,
    FitContent(&'static ComputedSize),
}

impl TrackSizingFunction {
    pub(crate) fn from_breadth(value: GridTrackBreadth) -> Self {
        match value {
            GridTrackBreadth::Auto => Self::Auto,
            GridTrackBreadth::LengthPercentage(size) => Self::Fixed(size),
            GridTrackBreadth::Flex(factor) => Self::Flex(factor),
            GridTrackBreadth::MinContent => Self::MinContent,
            GridTrackBreadth::MaxContent => Self::MaxContent,
            GridTrackBreadth::FitContent(size) => Self::FitContent(size),
        }
    }

    pub(crate) fn is_auto(self, available: AvailableSize) -> bool {
        match self {
            Self::Auto => true,
            Self::Fixed(value) => value.contains_percentage() && !matches!(available, AvailableSize::Definite(_)),
            _ => false,
        }
    }

    pub(crate) fn is_fixed(self, available: AvailableSize) -> bool {
        match self {
            Self::Fixed(value) => !value.contains_percentage() || matches!(available, AvailableSize::Definite(_)),
            Self::FixedPx(_) => true,
            _ => false,
        }
    }

    pub(crate) fn is_intrinsic(self, available: AvailableSize) -> bool {
        self.is_auto(available) || matches!(self, Self::MinContent | Self::MaxContent | Self::FitContent(_))
    }

    pub(crate) fn is_min_content(self) -> bool {
        matches!(self, Self::MinContent)
    }

    pub(crate) fn is_max_content(self) -> bool {
        matches!(self, Self::MaxContent)
    }

    pub(crate) fn is_fit_content(self) -> bool {
        matches!(self, Self::FitContent(_))
    }

    pub(crate) fn flex_factor(self) -> Option<f64> {
        match self {
            Self::Flex(factor) => Some(factor),
            _ => None,
        }
    }

    pub(crate) fn resolve(self, available: AvailableSize) -> CssPixels {
        match self {
            Self::Fixed(value) | Self::FitContent(value) => value.to_px(available.to_px_or_zero()),
            Self::FixedPx(px) => px,
            _ => CssPixels::default(),
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct Track {
    pub(crate) min_sizing: TrackSizingFunction,
    pub(crate) max_sizing: TrackSizingFunction,
    pub(crate) base_size: CssPixels,
    pub(crate) growth_limit: Option<CssPixels>,
    pub(crate) flex_factor: Option<f64>,
    pub(crate) max_is_intrinsic: bool,
    pub(crate) max_is_max_content: bool,
    pub(crate) base_size_frozen: bool,
    pub(crate) growth_limit_frozen: bool,
    pub(crate) infinitely_growable: bool,
    pub(crate) planned_increase: CssPixels,
    pub(crate) item_incurred_increase: CssPixels,
    pub(crate) is_gap: bool,
    pub(crate) is_auto_fit: bool,
    pub(crate) is_auto_repeat: bool,
    pub(crate) is_collapsed: bool,
}

impl Track {
    pub(crate) fn fixed(base_size: CssPixels) -> Self {
        Self {
            min_sizing: TrackSizingFunction::FixedPx(base_size),
            max_sizing: TrackSizingFunction::FixedPx(base_size),
            base_size,
            growth_limit: Some(base_size),
            flex_factor: None,
            max_is_intrinsic: false,
            max_is_max_content: false,
            base_size_frozen: false,
            growth_limit_frozen: false,
            infinitely_growable: false,
            planned_increase: CssPixels::default(),
            item_incurred_increase: CssPixels::default(),
            is_gap: false,
            is_auto_fit: false,
            is_auto_repeat: false,
            is_collapsed: false,
        }
    }

    pub(crate) fn auto() -> Self {
        Self {
            min_sizing: TrackSizingFunction::Auto,
            max_sizing: TrackSizingFunction::Auto,
            base_size: CssPixels::default(),
            growth_limit: Some(CssPixels::default()),
            flex_factor: None,
            max_is_intrinsic: true,
            max_is_max_content: false,
            base_size_frozen: false,
            growth_limit_frozen: false,
            infinitely_growable: false,
            planned_increase: CssPixels::default(),
            item_incurred_increase: CssPixels::default(),
            is_gap: false,
            is_auto_fit: false,
            is_auto_repeat: false,
            is_collapsed: false,
        }
    }

    pub(crate) fn from_definition(definition: TrackDefinition) -> Self {
        // NOTE: repeat() is expected to be expanded beforehand.
        let min_sizing = TrackSizingFunction::from_breadth(definition.min);
        let max_sizing = TrackSizingFunction::from_breadth(definition.max);
        Self {
            min_sizing,
            max_sizing,
            base_size: CssPixels::default(),
            growth_limit: Some(CssPixels::default()),
            flex_factor: max_sizing.flex_factor(),
            max_is_intrinsic: false,
            max_is_max_content: max_sizing.is_max_content(),
            base_size_frozen: false,
            growth_limit_frozen: false,
            infinitely_growable: false,
            planned_increase: CssPixels::default(),
            item_incurred_increase: CssPixels::default(),
            is_gap: false,
            is_auto_fit: definition.is_auto_fit,
            is_auto_repeat: definition.is_auto_repeat,
            is_collapsed: false,
        }
    }

    pub(crate) fn gap(size: CssPixels) -> Self {
        let mut track = Self::fixed(size);
        track.growth_limit = Some(CssPixels::default());
        track.is_gap = true;
        track
    }

    pub(crate) fn collapse(&mut self) {
        self.min_sizing = TrackSizingFunction::FixedPx(CssPixels::default());
        self.max_sizing = TrackSizingFunction::FixedPx(CssPixels::default());
        self.flex_factor = None;
        self.is_collapsed = true;
    }
}

pub(crate) fn initialize_track_sizes(tracks: &mut [Track], available: AvailableSize) -> bool {
    // https://www.w3.org/TR/css-grid-2/#algo-init
    // 12.4. Initialize Track Sizes
    // Initialize each track’s base size and growth limit.
    let mut has_flexible_tracks = false;
    for track in tracks {
        track.base_size_frozen = false;
        track.growth_limit_frozen = false;
        track.infinitely_growable = false;
        track.planned_increase = CssPixels::default();
        track.item_incurred_increase = CssPixels::default();
        if track.is_gap {
            continue;
        }

        if !matches!(available, AvailableSize::Definite(_))
            && matches!(track.max_sizing, TrackSizingFunction::FitContent(value) if value.contains_percentage())
        {
            // Normalize fit-content tracks with unresolvable percentage arguments to max-content,
            // since the percentage cannot be resolved against an indefinite available size.
            track.max_sizing = TrackSizingFunction::MaxContent;
        }

        if track.min_sizing.is_fixed(available) {
            track.base_size = track.min_sizing.resolve(available);
        } else if track.min_sizing.is_intrinsic(available) {
            track.base_size = CssPixels::default();
        } else {
            unreachable!("flexible values cannot be minimum track sizing functions");
        }

        if track.max_sizing.is_fixed(available) {
            track.growth_limit = Some(track.max_sizing.resolve(available));
        } else if let Some(factor) = track.max_sizing.flex_factor() {
            has_flexible_tracks = true;
            track.flex_factor = Some(factor);
            track.growth_limit = None;
        } else if track.max_sizing.is_intrinsic(available) {
            track.growth_limit = None;
        } else {
            unreachable!("invalid maximum track sizing function");
        }
        track.max_is_intrinsic = track.max_sizing.is_intrinsic(available);
        track.max_is_max_content = track.max_sizing.is_max_content();
        if track.growth_limit.is_some_and(|limit| limit < track.base_size) {
            // In all cases, if the growth limit is less than the base size, increase the growth limit to match
            // the base size.
            track.growth_limit = Some(track.base_size);
        }
    }
    has_flexible_tracks
}

pub(crate) fn find_fr_size(tracks: &[Track], space_to_fill: CssPixels) -> formatting_context::PixelFraction {
    // https://www.w3.org/TR/css-grid-2/#algo-find-fr-size
    let mut inflexible = vec![false; tracks.len()];
    loop {
        // 1. Let leftover space be the space to fill minus the base sizes of the non-flexible grid tracks.
        let mut leftover_space = space_to_fill;
        for (index, track) in tracks.iter().enumerate() {
            if inflexible[index] || track.flex_factor.is_none() {
                leftover_space -= track.base_size;
            }
        }

        // 2. Let flex factor sum be the sum of the flex factors of the flexible tracks.
        //    If this value is less than 1, set it to 1 instead.
        let mut flex_factor_sum = CssPixels::default();
        for (index, track) in tracks.iter().enumerate() {
            if inflexible[index] {
                continue;
            }
            if let Some(factor) = track.flex_factor {
                flex_factor_sum += CssPixels::nearest_value_for(factor);
            }
        }
        if flex_factor_sum < CssPixels::from_integer(1) {
            flex_factor_sum = CssPixels::from_integer(1);
        }
        // 3. Let the hypothetical fr size be the leftover space divided by the flex factor sum.
        let hypothetical_fr_size = formatting_context::PixelFraction::new(leftover_space, flex_factor_sum);

        // 4. If the product of the hypothetical fr size and a flexible track’s flex factor is less than the track’s
        //    base size, restart this algorithm treating all such tracks as inflexible.
        let mut restart = false;
        for (index, track) in tracks.iter().enumerate() {
            if inflexible[index] {
                continue;
            }
            let Some(factor) = track.flex_factor else {
                continue;
            };
            let scaled = hypothetical_fr_size.multiply(CssPixels::nearest_value_for(factor));
            if scaled < track.base_size {
                inflexible[index] = true;
                restart = true;
            }
        }
        if !restart {
            // 5. Return the hypothetical fr size.
            return hypothetical_fr_size;
        }
    }
}

pub(crate) fn expand_flexible_tracks(tracks: &mut [Track], space_to_fill: CssPixels) {
    let flex_fraction = find_fr_size(tracks, space_to_fill);
    // For each flexible track, if the product of the used flex fraction and the track’s flex factor is greater than
    // the track’s base size, set its base size to that product.
    for track in tracks {
        let Some(factor) = track.flex_factor else {
            continue;
        };
        let scaled = flex_fraction.multiply(CssPixels::nearest_value_for(factor));
        if scaled > track.base_size {
            track.base_size = scaled;
        }
    }
}
