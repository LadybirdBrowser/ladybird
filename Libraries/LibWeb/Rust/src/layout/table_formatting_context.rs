/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub(crate) const LINE_STYLE_NONE: u8 = 0;
pub(crate) const LINE_STYLE_HIDDEN: u8 = 1;
pub(crate) const LINE_STYLE_DOTTED: u8 = 2;
pub(crate) const LINE_STYLE_DASHED: u8 = 3;
pub(crate) const LINE_STYLE_SOLID: u8 = 4;
pub(crate) const LINE_STYLE_DOUBLE: u8 = 5;
const LINE_STYLE_GROOVE: u8 = 6;
const LINE_STYLE_RIDGE: u8 = 7;
const LINE_STYLE_INSET: u8 = 8;
const LINE_STYLE_OUTSET: u8 = 9;

fn line_style_score(line_style: u8) -> u8 {
    match line_style {
        LINE_STYLE_INSET => 0,
        LINE_STYLE_GROOVE => 1,
        LINE_STYLE_OUTSET => 2,
        LINE_STYLE_RIDGE => 3,
        LINE_STYLE_DOTTED => 4,
        LINE_STYLE_DASHED => 5,
        LINE_STYLE_SOLID => 6,
        LINE_STYLE_DOUBLE => 7,
        _ => panic!("line style has no conflict-resolution score"),
    }
}

pub(crate) fn child_participates_in_table_run(container_display: FfiDisplay, child_facts: &NodeFacts<'_>) -> bool {
    if container_display.is_table_inside() {
        let child_display = child_facts.display();
        child_display.is_table_row() || child_display.is_table_row_group_kind()
    } else if container_display.is_table_row_group_kind() {
        child_facts.display().is_table_row()
    } else if container_display.is_table_row() {
        child_facts.display().is_table_cell()
    } else {
        true
    }
}

pub(crate) fn border_is_less_specific(
    incumbent: formatting_context::BorderData,
    candidate: formatting_context::BorderData,
) -> bool {
    // Implements criteria for steps 1, 2 and 3 of border conflict resolution algorithm, as described in
    // https://www.w3.org/TR/CSS22/tables.html#border-conflict-resolution.

    // 1. Borders with the 'border-style' of 'hidden' take precedence over all other conflicting borders. Any border with this
    //    value suppresses all borders at this location.
    if incumbent.line_style == LINE_STYLE_HIDDEN {
        return false;
    }
    if candidate.line_style == LINE_STYLE_HIDDEN {
        return true;
    }
    // 2. Borders with a style of 'none' have the lowest priority. Only if the border properties of all the elements meeting
    //    at this edge are 'none' will the border be omitted (but note that 'none' is the default value for the border style.)
    if incumbent.line_style == LINE_STYLE_NONE {
        return true;
    }
    if candidate.line_style == LINE_STYLE_NONE {
        return false;
    }
    // 3. If none of the styles are 'hidden' and at least one of them is not 'none', then narrow borders are discarded in favor
    //    of wider ones. If several have the same 'border-width' then styles are preferred in this order: 'double', 'solid',
    //    'dashed', 'dotted', 'ridge', 'outset', 'groove', and the lowest: 'inset'.
    if incumbent.width != candidate.width {
        return incumbent.width < candidate.width;
    }
    line_style_score(incumbent.line_style) < line_style_score(candidate.line_style)
}

fn candidate_wins(candidate: formatting_context::BorderData, incumbent: formatting_context::BorderData) -> bool {
    candidate.line_style != LINE_STYLE_NONE && border_is_less_specific(incumbent, candidate)
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ElementBorders {
    pub(crate) top: formatting_context::BorderData,
    pub(crate) right: formatting_context::BorderData,
    pub(crate) bottom: formatting_context::BorderData,
    pub(crate) left: formatting_context::BorderData,
}

// Each segment stores the border that currently wins at one slot boundary of the table grid,
// together with the kind of element it came from. A value-initialized segment acts as a sentinel
// meaning "no border applied yet": since candidates with a line style of 'none' never replace an
// incumbent, a segment whose winner still has style 'none' received no visible contribution at all.

// Implements border conflict resolution, as described in
// https://www.w3.org/TR/CSS22/tables.html#border-conflict-resolution, with a "push" model over a
// grid of border line segments: each boundary between two grid slots is a single shared segment, so
// borders of adjacent elements collapse by construction instead of requiring neighbor lookups.
//
// Horizontal border lines run between (and around) rows: there are row_count + 1 of them, each with
// one segment per column. Vertical border lines run between (and around) columns: column_count + 1
// of them, each with one segment per row.
//
// Table parts must be applied in order of decreasing precedence — cells, rows, row groups, columns,
// column groups, and lastly the table — with parts of equal precedence applied leftmost/topmost
// first. A candidate border only replaces the current winner of a segment when it is strictly more
// specific (steps 1-3 of the border conflict resolution algorithm), so ties resolve towards the
// earlier-applied part, which implements step 4 without tracking element kinds or coordinates.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct BorderWidths {
    pub(crate) top: CssPixels,
    pub(crate) right: CssPixels,
    pub(crate) bottom: CssPixels,
    pub(crate) left: CssPixels,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CollapsedBorderEdge {
    pub border_data: formatting_context::BorderData,
    pub source_order: u32,
}

#[derive(PartialEq, Eq)]
pub(crate) struct OwnedCollapsedTableBorders {
    pub(crate) row_offsets: Vec<CssPixels>,
    pub(crate) column_offsets: Vec<CssPixels>,
    pub(crate) horizontal_edges: Vec<CollapsedBorderEdge>,
    pub(crate) vertical_edges: Vec<CollapsedBorderEdge>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum TableParticipantPreparation {
    CreateUsedValues,
    TrackPercentageDependenciesOnly,
}

fn grid_line_offsets(sizes: impl ExactSizeIterator<Item = CssPixels>) -> Vec<CssPixels> {
    let mut offsets = Vec::with_capacity(sizes.len() + 1);
    let mut offset = CssPixels::default();
    offsets.push(offset);
    for size in sizes {
        offset += size;
        offsets.push(offset);
    }
    offsets
}

pub(crate) struct CollapsedBorderGrid {
    horizontal_lines: Vec<Vec<CollapsedBorderEdge>>,
    vertical_lines: Vec<Vec<CollapsedBorderEdge>>,
}

impl CollapsedBorderGrid {
    pub(crate) fn new(row_count: usize, column_count: usize) -> Self {
        Self {
            horizontal_lines: vec![vec![CollapsedBorderEdge::default(); column_count]; row_count + 1],
            vertical_lines: vec![vec![CollapsedBorderEdge::default(); row_count]; column_count + 1],
        }
    }

    pub(crate) fn apply_borders(
        &mut self,
        borders: ElementBorders,
        row_start: usize,
        row_end: usize,
        column_start: usize,
        column_end: usize,
        source_order: u32,
    ) {
        Self::apply_to_segments(
            &mut self.horizontal_lines[row_start],
            column_start,
            column_end,
            borders.top,
            source_order,
        );
        Self::apply_to_segments(
            &mut self.horizontal_lines[row_end],
            column_start,
            column_end,
            borders.bottom,
            source_order,
        );
        Self::apply_to_segments(
            &mut self.vertical_lines[column_start],
            row_start,
            row_end,
            borders.left,
            source_order,
        );
        Self::apply_to_segments(
            &mut self.vertical_lines[column_end],
            row_start,
            row_end,
            borders.right,
            source_order,
        );
    }

    pub(crate) fn hide_segments_inside_span(
        &mut self,
        row_start: usize,
        row_end: usize,
        column_start: usize,
        column_end: usize,
        source_order: u32,
    ) {
        // Segments strictly inside a spanning cell are not borders of any element; mark them as hidden
        // so that borders of rows and columns crossing the span cannot win there.
        let hidden = CollapsedBorderEdge {
            border_data: formatting_context::BorderData {
                color: 0,
                line_style: LINE_STYLE_HIDDEN,
                width: CssPixels::default(),
            },
            source_order,
        };
        for row in row_start + 1..row_end {
            for column in column_start..column_end {
                self.horizontal_lines[row][column] = hidden;
            }
        }
        for column in column_start + 1..column_end {
            for row in row_start..row_end {
                self.vertical_lines[column][row] = hidden;
            }
        }
    }

    pub(crate) fn resolve_used_widths_for_cell(
        &self,
        row_start: usize,
        row_end: usize,
        column_start: usize,
        column_end: usize,
    ) -> BorderWidths {
        BorderWidths {
            top: Self::most_specific(&self.horizontal_lines[row_start], column_start, column_end)
                .border_data
                .width,
            right: Self::most_specific(&self.vertical_lines[column_end], row_start, row_end)
                .border_data
                .width,
            bottom: Self::most_specific(&self.horizontal_lines[row_end], column_start, column_end)
                .border_data
                .width,
            left: Self::most_specific(&self.vertical_lines[column_start], row_start, row_end)
                .border_data
                .width,
        }
    }

    fn apply_to_segments(
        line: &mut [CollapsedBorderEdge],
        start: usize,
        end: usize,
        data: formatting_context::BorderData,
        source_order: u32,
    ) {
        if data.line_style == LINE_STYLE_NONE {
            return;
        }
        for segment in &mut line[start..end] {
            if candidate_wins(data, segment.border_data) {
                *segment = CollapsedBorderEdge {
                    border_data: data,
                    source_order,
                };
            }
        }
    }

    pub(crate) fn outer_edge_widths(&self) -> BorderWidths {
        let max_width = |segments: &[CollapsedBorderEdge]| {
            segments
                .iter()
                .fold(CssPixels::default(), |max, segment| max.max(segment.border_data.width))
        };
        BorderWidths {
            top: max_width(&self.horizontal_lines[0]),
            right: max_width(self.vertical_lines.last().expect("grid has vertical lines")),
            bottom: max_width(self.horizontal_lines.last().expect("grid has horizontal lines")),
            left: max_width(&self.vertical_lines[0]),
        }
    }

    pub(crate) fn has_paintable_edges(&self) -> bool {
        let paints = |segment: &CollapsedBorderEdge| {
            segment.border_data.width > CssPixels::default()
                && segment.border_data.line_style != LINE_STYLE_NONE
                && segment.border_data.line_style != LINE_STYLE_HIDDEN
        };
        self.horizontal_lines.iter().flatten().any(paints) || self.vertical_lines.iter().flatten().any(paints)
    }

    pub(crate) fn take_edges(self) -> (Vec<CollapsedBorderEdge>, Vec<CollapsedBorderEdge>) {
        (
            self.horizontal_lines.iter().flatten().copied().collect(),
            self.vertical_lines.iter().flatten().copied().collect(),
        )
    }

    fn most_specific(line: &[CollapsedBorderEdge], start: usize, end: usize) -> CollapsedBorderEdge {
        let mut winner = line[start];
        for segment in &line[start + 1..end] {
            if candidate_wins(segment.border_data, winner.border_data) {
                winner = *segment;
            }
        }
        winner
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct Column {
    pub(crate) inline_offset: CssPixels,
    pub(crate) min_size: CssPixels,
    pub(crate) max_size: CssPixels,
    pub(crate) used_inline_size: CssPixels,
    pub(crate) has_intrinsic_percentage: bool,
    pub(crate) intrinsic_percentage: f64,
    // Store whether the column is constrained: https://www.w3.org/TR/css-tables-3/#constrainedness
    pub(crate) is_constrained: bool,
    // Store whether the column has originating cells, defined in https://www.w3.org/TR/css-tables-3/#terminology.
    pub(crate) has_originating_cells: bool,
}

fn total_used(columns: &[Column]) -> CssPixels {
    columns
        .iter()
        .fold(CssPixels::default(), |sum, column| sum + column.used_inline_size)
}

fn total_candidates(candidates: &[CssPixels]) -> CssPixels {
    candidates
        .iter()
        .copied()
        .fold(CssPixels::default(), std::ops::Add::add)
}

fn commit_candidates(columns: &mut [Column], candidates: &[CssPixels]) {
    assert_eq!(columns.len(), candidates.len());
    for (column, candidate) in columns.iter_mut().zip(candidates) {
        column.used_inline_size = *candidate;
    }
}

fn assign_linear_combination(columns: &mut [Column], candidates: &[CssPixels], available: CssPixels) {
    let candidate_total = total_candidates(candidates);
    let used_total = total_used(columns);
    if candidate_total == used_total {
        return;
    }
    let weight = (available - used_total).to_double() / (candidate_total - used_total).to_double();
    for (column, candidate) in columns.iter_mut().zip(candidates) {
        column.used_inline_size = CssPixels::nearest_value_for(
            weight * candidate.to_double() + (1.0 - weight) * column.used_inline_size.to_double(),
        );
    }
}

fn distribute_proportionally(
    columns: &mut [Column],
    excess: CssPixels,
    filter: impl Fn(&Column) -> bool,
    base: impl Fn(&Column) -> CssPixels,
) -> bool {
    let mut found = false;
    let mut total = CssPixels::default();
    for column in columns.iter() {
        if filter(column) {
            total += base(column);
            found = true;
        }
    }
    if !found {
        return false;
    }
    assert!(total > CssPixels::default());
    for column in columns.iter_mut() {
        if filter(column) {
            column.used_inline_size +=
                CssPixels::nearest_value_for(excess.to_double() * base(column).to_double() / total.to_double());
        }
    }
    true
}

fn distribute_equally(columns: &mut [Column], excess: CssPixels, filter: impl Fn(&Column) -> bool) -> bool {
    let count = columns.iter().filter(|column| filter(column)).count();
    if count == 0 {
        return false;
    }
    for column in columns.iter_mut() {
        if filter(column) {
            column.used_inline_size += excess / count;
        }
    }
    true
}

fn distribute_by_percentage(columns: &mut [Column], excess: CssPixels, filter: impl Fn(&Column) -> bool) -> bool {
    let mut found = false;
    let mut total = 0.0;
    for column in columns.iter() {
        if filter(column) {
            found = true;
            total += column.intrinsic_percentage;
        }
    }
    if !found {
        return false;
    }
    for column in columns.iter_mut() {
        if filter(column) {
            column.used_inline_size +=
                CssPixels::nearest_value_for(excess.to_double() * column.intrinsic_percentage / total);
        }
    }
    true
}

fn distribute_excess_fixed(columns: &mut [Column], excess: CssPixels) {
    // Implements the fixed mode for https://www.w3.org/TR/css-tables-3/#distributing-width-to-columns.

    // If any columns have no specified inline size, distribute the excess inline size equally to them.
    if distribute_equally(columns, excess, |column| {
        !column.is_constrained && !column.has_intrinsic_percentage
    }) {
        return;
    }
    // Otherwise, distribute proportionally among columns with non-zero length sizes from the base assignment.
    if distribute_proportionally(
        columns,
        excess,
        |column| column.used_inline_size > CssPixels::default(),
        |column| column.used_inline_size,
    ) {
        return;
    }
    // Otherwise, distribute proportionally among columns with non-zero percentage sizes from the base assignment.
    if distribute_by_percentage(columns, excess, |column| column.intrinsic_percentage > 0.0) {
        return;
    }
    // Otherwise, distribute the excess inline size equally to the zero-sized columns.
    distribute_equally(columns, excess, |column| {
        column.used_inline_size == CssPixels::default()
    });
}

fn distribute_excess(columns: &mut [Column], available: CssPixels, fixed: bool) {
    // Implements https://www.w3.org/TR/css-tables-3/#distributing-width-to-columns
    let used = total_used(columns);
    if used >= available {
        return;
    }
    let mut excess = available - used;
    if excess == CssPixels::default() {
        return;
    }
    if fixed {
        distribute_excess_fixed(columns, excess);
        return;
    }

    // 1. If there are non-constrained columns that have originating cells with intrinsic percentage width of 0% and with nonzero
    //    max-content width (aka the columns allowed to grow by this rule), the distributed widths of the columns allowed to grow
    //    by this rule are increased in proportion to max-content width so the total increase adds to the excess width.
    if distribute_proportionally(
        columns,
        excess,
        |column| {
            !column.is_constrained
                && column.has_originating_cells
                && column.intrinsic_percentage == 0.0
                && column.max_size > CssPixels::default()
        },
        |column| column.max_size,
    ) {
        excess = available - total_used(columns);
    }
    if excess == CssPixels::default() {
        return;
    }
    // 2. Otherwise, if there are non-constrained columns that have originating cells with intrinsic percentage width of 0% (aka the columns
    //    allowed to grow by this rule, which thanks to the previous rule must have zero max-content width), the distributed widths of the
    //    columns allowed to grow by this rule are increased by equal amounts so the total increase adds to the excess width.
    if distribute_equally(columns, excess, |column| {
        !column.is_constrained && column.has_originating_cells && column.intrinsic_percentage == 0.0
    }) {
        excess = available - total_used(columns);
    }
    if excess == CssPixels::default() {
        return;
    }
    // 3. Otherwise, if there are constrained columns with intrinsic percentage width of 0% and with nonzero max-content width
    //    (aka the columns allowed to grow by this rule, which, due to other rules, must have originating cells), the distributed widths of the
    //    columns allowed to grow by this rule are increased in proportion to max-content width so the total increase adds to the excess width.
    if distribute_proportionally(
        columns,
        excess,
        |column| column.is_constrained && column.intrinsic_percentage == 0.0 && column.max_size > CssPixels::default(),
        |column| column.max_size,
    ) {
        excess = available - total_used(columns);
    }
    if excess == CssPixels::default() {
        return;
    }
    // 4. Otherwise, if there are columns with intrinsic percentage width greater than 0% (aka the columns allowed to grow by this rule,
    //    which, due to other rules, must have originating cells), the distributed widths of the columns allowed to grow by this rule are
    //    increased in proportion to intrinsic percentage width so the total increase adds to the excess width.
    if distribute_by_percentage(columns, excess, |column| column.intrinsic_percentage > 0.0) {
        excess = available - total_used(columns);
    }
    if excess == CssPixels::default() {
        return;
    }
    // 5. Otherwise, if there is any such column, the distributed widths of all columns that have originating cells are increased by equal amounts
    //    so the total increase adds to the excess width.
    if distribute_equally(columns, excess, |column| column.has_originating_cells) {
        excess = available - total_used(columns);
    }
    if excess == CssPixels::default() {
        return;
    }
    // 6. Otherwise, the distributed widths of all columns are increased by equal amounts so the total increase adds to the excess width.
    distribute_equally(columns, excess, |_| true);
}

pub(crate) fn distribute_inline_size(columns: &mut [Column], available: CssPixels, fixed: bool) {
    // Implements https://www.w3.org/TR/css-tables-3/#width-distribution-algorithm

    let mut candidates = vec![CssPixels::default(); columns.len()];
    // 1. The min-content sizing guess assigns every column its min-content inline size.
    for (index, column) in columns.iter_mut().enumerate() {
        // In fixed mode, the min-content width of percent-columns and auto-columns is considered to be zero:
        // https://www.w3.org/TR/css-tables-3/#width-distribution-in-fixed-mode
        if fixed && !column.is_constrained {
            continue;
        }
        column.used_inline_size = column.min_size;
        candidates[index] = column.min_size;
    }
    // 2. The min-content-percentage sizing-guess is the set of column width assignments where:
    //    - each percent-column is assigned the larger of:
    //      - its intrinsic percentage width times the assignable width and
    //      - its min-content width.
    //    - all other columns are assigned their min-content width.
    for (candidate, column) in candidates.iter_mut().zip(columns.iter()) {
        if column.has_intrinsic_percentage {
            *candidate = column.min_size.max(CssPixels::nearest_value_for(
                column.intrinsic_percentage / 100.0 * available.to_double(),
            ));
        }
    }
    // If the assignable inline size is no larger than the max-content sizing guess, use the linear combination
    // of the consecutive sizing guesses whose sums bound the available inline size.
    if available < total_candidates(&candidates) {
        assign_linear_combination(columns, &candidates, available);
        return;
    }
    commit_candidates(columns, &candidates);

    // 3. The min-content-specified sizing-guess is the set of column width assignments where:
    //    - each percent-column is assigned the larger of:
    //      - its intrinsic percentage width times the assignable width and
    //      - its min-content width
    //    - any other column that is constrained is assigned its max-content width
    //    - all other columns are assigned their min-content width.
    for (candidate, column) in candidates.iter_mut().zip(columns.iter()) {
        if column.is_constrained {
            *candidate = column.max_size;
        }
    }
    if available < total_candidates(&candidates) {
        assign_linear_combination(columns, &candidates, available);
        return;
    }
    commit_candidates(columns, &candidates);

    // 4. The max-content sizing-guess is the set of column width assignments where:
    //    - each percent-column is assigned the larger of:
    //      - its intrinsic percentage width times the assignable width and
    //      - its min-content width
    //    - all other columns are assigned their max-content width.
    for (candidate, column) in candidates.iter_mut().zip(columns.iter()) {
        if !column.has_intrinsic_percentage {
            *candidate = column.max_size;
        }
    }
    if available < total_candidates(&candidates) {
        assign_linear_combination(columns, &candidates, available);
        return;
    }
    commit_candidates(columns, &candidates);
    // Otherwise, start from the max-content sizing guess and distribute the excess inline size to the columns.
    distribute_excess(columns, available, fixed);
}

#[derive(Clone, Copy)]
pub(crate) struct TableCell {
    pub(crate) box_: Node,
    pub(crate) column_index: usize,
    pub(crate) row_index: usize,
    pub(crate) column_span: usize,
    pub(crate) row_span: usize,
    pub(crate) baseline: CssPixels,
    pub(crate) outer_min_inline_size: CssPixels,
    pub(crate) outer_max_inline_size: CssPixels,
    pub(crate) outer_min_block_size: CssPixels,
    pub(crate) outer_max_block_size: CssPixels,
}

pub(crate) struct Row {
    pub(crate) box_: Node,
    pub(crate) base_block_size: CssPixels,
    pub(crate) reference_block_size: CssPixels,
    pub(crate) final_block_size: CssPixels,
    pub(crate) baseline: CssPixels,
    pub(crate) min_size: CssPixels,
    pub(crate) max_size: CssPixels,
    pub(crate) is_collapsed: bool,
    pub(crate) has_intrinsic_percentage: bool,
    pub(crate) intrinsic_percentage: f64,
    pub(crate) is_constrained: bool,
}

impl Row {
    fn new(box_: Node, is_collapsed: bool) -> Self {
        Self {
            box_,
            base_block_size: CssPixels::default(),
            reference_block_size: CssPixels::default(),
            final_block_size: CssPixels::default(),
            baseline: CssPixels::default(),
            min_size: CssPixels::default(),
            max_size: CssPixels::default(),
            is_collapsed,
            has_intrinsic_percentage: false,
            intrinsic_percentage: 0.0,
            is_constrained: false,
        }
    }
}

pub(crate) struct TableGrid {
    pub(crate) column_count: usize,
    pub(crate) cells: Vec<TableCell>,
    pub(crate) rows: Vec<Row>,
    pub(crate) occupancy: HashSet<(usize, usize)>,
}

pub(crate) struct TableInlineLayout {
    table_box: Node,
    table_constraints: ContainingBlockConstraints,
    cells: Vec<TableCell>,
    columns: Vec<Column>,
    rows: Vec<Row>,
}

impl TableInlineLayout {
    pub(crate) fn table_box(&self) -> Node {
        self.table_box
    }
}

fn matching_children<T: TableTree>(tree: &T, parent: Node, predicate: impl Fn(FfiDisplay) -> bool) -> Vec<Node> {
    let mut result = Vec::new();
    let mut child = tree.first_child(parent);
    while !child.is_invalid() {
        if node_facts::kind_is_box(tree.node_data(child).kind) && predicate(tree.display(child)) {
            result.push(child);
        }
        child = tree.next_sibling(child);
    }
    result
}

fn count_columns_in_subtree<T: TableTree>(tree: &T, root: Node) -> usize {
    let mut count = 0usize;
    let mut stack = vec![root];
    while let Some(node) = stack.pop() {
        let mut child = tree.first_child(node);
        let mut children = Vec::new();
        while !child.is_invalid() {
            children.push(child);
            child = tree.next_sibling(child);
        }
        for child in children.into_iter().rev() {
            if node_facts::kind_is_box(tree.node_data(child).kind) && tree.display(child).is_table_column() {
                count = count.saturating_add(tree.table_column_span(child));
            }
            stack.push(child);
        }
    }
    count
}

pub(crate) fn calculate_table_grid<T: TableTree>(tree: &T, table: Node) -> TableGrid {
    let mut cells = Vec::new();
    let mut rows = Vec::new();
    let mut occupancy = HashSet::default();
    let mut column_count = 0usize;
    let mut row_count = 0usize;
    let mut current_row = 0usize;

    for column_group in matching_children(tree, table, |display| display.is_table_column_group()) {
        column_count = column_count.saturating_add(count_columns_in_subtree(tree, column_group));
    }

    let process_row = |tree: &T,
                       row: Node,
                       row_group: Option<Node>,
                       cells: &mut Vec<TableCell>,
                       rows: &mut Vec<Row>,
                       occupancy: &mut HashSet<(usize, usize)>,
                       column_count: &mut usize,
                       row_count: &mut usize,
                       current_row: &mut usize| {
        if *row_count == *current_row {
            *row_count += 1;
        }
        let mut current_column = 0usize;
        for cell_box in matching_children(tree, row, |display| display.is_table_cell()) {
            while current_column < *column_count && occupancy.contains(&(current_column, *current_row)) {
                current_column += 1;
            }
            if current_column == *column_count {
                *column_count += 1;
            }

            let column_span = tree.table_column_span(cell_box);
            let mut row_span = tree.table_row_span(cell_box);
            if row_span == 0 {
                // Downward-growing cells remain deliberately unimplemented.
                row_span = 1;
            }
            *column_count = (*column_count).max(current_column + column_span);
            *row_count = (*row_count).max(*current_row + row_span);
            for row_index in *current_row..*current_row + row_span {
                for column_index in current_column..current_column + column_span {
                    occupancy.insert((column_index, row_index));
                }
            }
            cells.push(TableCell {
                box_: cell_box,
                column_index: current_column,
                row_index: *current_row,
                column_span,
                row_span,
                baseline: CssPixels::default(),
                outer_min_inline_size: CssPixels::default(),
                outer_max_inline_size: CssPixels::default(),
                outer_min_block_size: CssPixels::default(),
                outer_max_block_size: CssPixels::default(),
            });
            current_column += column_span;
        }

        rows.push(Row::new(row, tree.row_is_collapsed(row, row_group)));
        *current_row += 1;
    };

    let mut child = tree.first_child(table);
    while !child.is_invalid() {
        let child_is_box = node_facts::kind_is_box(tree.node_data(child).kind);
        let child_display = tree.display(child);
        if child_is_box
            && (child_display.is_table_row_group()
                || child_display.is_table_header_group()
                || child_display.is_table_footer_group())
        {
            for row in matching_children(tree, child, |display| display.is_table_row()) {
                process_row(
                    tree,
                    row,
                    Some(child),
                    &mut cells,
                    &mut rows,
                    &mut occupancy,
                    &mut column_count,
                    &mut row_count,
                    &mut current_row,
                );
            }
        } else if child_is_box && child_display.is_table_row() {
            process_row(
                tree,
                child,
                None,
                &mut cells,
                &mut rows,
                &mut occupancy,
                &mut column_count,
                &mut row_count,
                &mut current_row,
            );
        }
        child = tree.next_sibling(child);
    }

    for cell in &mut cells {
        cell.row_span = cell.row_span.min(rows.len() - cell.row_index);
        cell.column_span = cell.column_span.min(column_count - cell.column_index);
    }

    TableGrid {
        column_count,
        cells,
        rows,
        occupancy,
    }
}

const BORDER_COLLAPSE_SEPARATE: u8 = 0;
const TABLE_LAYOUT_FIXED: u8 = 1;

pub(crate) trait TableTree {
    fn first_child(&self, node: Node) -> Node;
    fn next_sibling(&self, node: Node) -> Node;
    fn node_data(&self, node: Node) -> &NodeData;
    fn display(&self, node: Node) -> FfiDisplay;

    fn table_column_span(&self, node: Node) -> usize {
        self.node_data(node).table_column_span as usize
    }

    fn table_row_span(&self, node: Node) -> usize {
        self.node_data(node).table_row_span as usize
    }

    fn row_is_collapsed(&self, _row: Node, _row_group: Option<Node>) -> bool {
        false
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum TrackAxis {
    Row,
    Column,
}

pub(super) struct TableFormattingContext {
    purpose: formatting_context::LayoutPurpose,
    records: std::rc::Rc<RunRecords>,
    table_box: Node,
    layout_mode: LayoutMode,
    callbacks: FfiLayoutFcCallbacks,
    fragments: Option<std::rc::Rc<fragment_tree::RunFragmentBuilder>>,
    should_collect_devtools_layout_data: bool,
    treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
    table_constraints: ContainingBlockConstraints,
    participant_constraints: ContainingBlockConstraints,
    available_space: AvailableSpace,
    table_block_size: CssPixels,
    pub(super) automatic_content_block_size: CssPixels,
    table_box_content_block_offset_in_wrapper: CssPixels,
    min_border_box_block_size_from_flex_item: Option<CssPixels>,
    needs_fixed_mode_row_measurement: bool,
    cells: Vec<TableCell>,
    cell_inside_layout_inputs: Vec<AvailableSpace>,
    cell_pre_layout_content_block_sizes: Vec<CssPixels>,
    deferred_cell_inside_layouts: Vec<bool>,
    columns: Vec<Column>,
    rows: Vec<Row>,
    collapsed_border_grid: Option<CollapsedBorderGrid>,
    derived_baselines_of_root_box: Cell<DerivedBaselines>,
}

impl TableTree for TableFormattingContext {
    fn first_child(&self, node: Node) -> Node {
        self.callbacks.first_child(node)
    }

    fn next_sibling(&self, node: Node) -> Node {
        self.callbacks.next_sibling(node)
    }

    fn node_data(&self, node: Node) -> &NodeData {
        self.callbacks.node_data(node)
    }

    fn display(&self, node: Node) -> FfiDisplay {
        self.callbacks
            .computed_values_view_if_styled(node)
            .map_or_else(FfiDisplay::block, |style| style.display())
    }

    fn row_is_collapsed(&self, row: Node, row_group: Option<Node>) -> bool {
        // CSS::Visibility::Collapse is pinned to zero in
        // LayoutRustBridge.cpp.
        self.style(row).visibility() == 0 || row_group.is_some_and(|group| self.style(group).visibility() == 0)
    }
}

impl TableFormattingContext {
    fn node_facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.callbacks, node)
    }

    fn style(&self, node: Node) -> StyleValues<'static> {
        StyleValues::for_node(&self.callbacks, node)
    }

    fn raw_column_span(&self, column: Node) -> usize {
        self.callbacks.arena().raw_table_column_span(column) as usize
    }

    pub(super) fn new(run: &FormattingContextRun) -> Self {
        Self {
            purpose: run.purpose,
            records: run.records.clone(),
            table_box: run.box_,
            layout_mode: run.layout_mode,
            callbacks: run.callbacks,
            fragments: run.fragments.clone(),
            should_collect_devtools_layout_data: run.should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root: run
                .treat_block_axis_percentage_insets_as_auto_beyond_root,
            table_constraints: ContainingBlockConstraints::default(),
            participant_constraints: ContainingBlockConstraints::default(),
            available_space: AvailableSpace::default(),
            table_block_size: CssPixels::default(),
            automatic_content_block_size: CssPixels::default(),
            table_box_content_block_offset_in_wrapper: CssPixels::default(),
            min_border_box_block_size_from_flex_item: None,
            needs_fixed_mode_row_measurement: false,
            cells: Vec::new(),
            cell_inside_layout_inputs: Vec::new(),
            cell_pre_layout_content_block_sizes: Vec::new(),
            deferred_cell_inside_layouts: Vec::new(),
            columns: Vec::new(),
            rows: Vec::new(),
            collapsed_border_grid: None,
            derived_baselines_of_root_box: Cell::new(DerivedBaselines::default()),
        }
    }

    pub(super) fn take_inline_layout(&mut self) -> TableInlineLayout {
        TableInlineLayout {
            table_box: self.table_box,
            table_constraints: self.table_constraints,
            cells: std::mem::take(&mut self.cells),
            columns: std::mem::take(&mut self.columns),
            rows: std::mem::take(&mut self.rows),
        }
    }

    fn reuse_inline_layout(&mut self, input: LayoutInput, layout: TableInlineLayout) -> bool {
        if layout.table_box != self.table_box || layout.table_constraints != input.containing_block_constraints {
            return false;
        }

        self.available_space = input.available_space;
        self.table_constraints = input.containing_block_constraints;
        self.participant_constraints = ContainingBlockConstraints {
            percentage_basis_inline_size: self.table_constraints.percentage_basis_inline_size,
            percentage_basis_block_size: if self.style(self.table_box).height().is_auto() {
                None
            } else {
                self.table_constraints.percentage_basis_block_size
            },
            quirks_mode_percentage_basis_block_size: self.table_constraints.quirks_mode_percentage_basis_block_size,
        };
        self.cells = layout.cells;
        self.columns = layout.columns;
        self.rows = layout.rows;

        // The wrapper sizing pass deliberately omits intrinsic row measurement. Reusing its
        // inline result is only sufficient when the committing pass would omit it as well.
        if !self.can_skip_row_intrinsic_measurement() {
            return false;
        }

        self.cell_inside_layout_inputs = vec![AvailableSpace::default(); self.cells.len()];
        self.cell_pre_layout_content_block_sizes = vec![CssPixels::default(); self.cells.len()];
        self.deferred_cell_inside_layouts = vec![false; self.cells.len()];
        self.needs_fixed_mode_row_measurement = false;
        self.prepare_table_participants(TableParticipantPreparation::CreateUsedValues);
        self.border_conflict_resolution();
        self.compute_table_inline_size();
        true
    }

    fn sizing(&self) -> sizing_context::SizingContext {
        sizing_context::SizingContext::new(self.purpose, self.records.clone(), self.callbacks)
    }

    fn formatting_context_run(&self) -> FormattingContextRun {
        FormattingContextRun {
            purpose: self.purpose,
            records: self.records.clone(),
            box_: self.table_box,
            layout_mode: self.layout_mode,
            callbacks: self.callbacks,
            should_collect_devtools_layout_data: self.should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root: self
                .treat_block_axis_percentage_insets_as_auto_beyond_root,
            fragments: self.fragments.clone(),
            previous_line_data: None,
        }
    }

    fn parent(&self, node: Node) -> Node {
        self.callbacks.parent(node)
    }

    fn matching_children(&mut self, parent: Node, predicate: impl Fn(NodeFacts<'_>) -> bool) -> Vec<Node> {
        let mut children = Vec::new();
        let mut child = self.first_child(parent);
        while !child.is_invalid() {
            let facts = self.node_facts(child);
            if facts.is_box() && predicate(facts) {
                children.push(child);
            }
            child = self.next_sibling(child);
        }
        children
    }

    #[track_caller]
    fn used_values(&self, node: Node) -> std::rc::Rc<UsedValues> {
        self.records.used_values(node)
    }

    fn create_used_values(&self, node: Node, constraints: ContainingBlockConstraints) -> std::rc::Rc<UsedValues> {
        self.records.create_used_values(&self.callbacks, node, constraints)
    }

    fn place_child(&self, node: Node, x: CssPixels, y: CssPixels) {
        formatting_context::place_child(&self.formatting_context_run(), node, FfiCssPixelPoint { x, y }, None);
    }

    fn border_spacing_inline(&mut self) -> CssPixels {
        let style = self.style(self.table_box);
        // When a table is laid out in collapsed-borders mode, the border-spacing of the table-root is ignored (as if it was set to 0px):
        // https://www.w3.org/TR/css-tables-3/#collapsed-style-overrides
        if style.border_collapse() != BORDER_COLLAPSE_SEPARATE {
            CssPixels::default()
        } else {
            style.border_spacing_horizontal()
        }
    }

    fn border_spacing_block(&mut self) -> CssPixels {
        let style = self.style(self.table_box);
        // When a table is laid out in collapsed-borders mode, the border-spacing of the table-root is ignored (as if it was set to 0px):
        // https://www.w3.org/TR/css-tables-3/#collapsed-style-overrides
        if style.border_collapse() != BORDER_COLLAPSE_SEPARATE {
            CssPixels::default()
        } else {
            style.border_spacing_vertical()
        }
    }

    fn element_borders(&mut self, node: Node) -> ElementBorders {
        let style = self.style(node);
        ElementBorders {
            top: formatting_context::BorderData {
                color: style.border_top_color(),
                line_style: style.border_top_style(),
                width: style.border_top_width(),
            },
            right: formatting_context::BorderData {
                color: style.border_right_color(),
                line_style: style.border_right_style(),
                width: style.border_right_width(),
            },
            bottom: formatting_context::BorderData {
                color: style.border_bottom_color(),
                line_style: style.border_bottom_style(),
                width: style.border_bottom_width(),
            },
            left: formatting_context::BorderData {
                color: style.border_left_color(),
                line_style: style.border_left_style(),
                width: style.border_left_width(),
            },
        }
    }

    fn border_conflict_resolution(&mut self) {
        if self.style(self.table_box).border_collapse() == BORDER_COLLAPSE_SEPARATE {
            return;
        }

        let row_count = self.rows.len();
        let column_count = self.columns.len();
        let mut grid = CollapsedBorderGrid::new(row_count, column_count);
        let mut next_source_order = 0u32;
        let mut take_source_order = move || {
            let source_order = next_source_order;
            next_source_order += 1;
            source_order
        };
        // Cells, column by column so that on ties the cell further to the left, then further to the
        // top, wins. TableCell spans are already clipped to the table end by TableGrid.
        let mut cells = self.cells.clone();
        cells.sort_by_key(|cell| (cell.column_index, cell.row_index));
        for cell in cells {
            let source_order = take_source_order();
            if cell.row_span > 1 || cell.column_span > 1 {
                grid.hide_segments_inside_span(
                    cell.row_index,
                    cell.row_index + cell.row_span,
                    cell.column_index,
                    cell.column_index + cell.column_span,
                    source_order,
                );
            }
            let borders = self.element_borders(cell.box_);
            grid.apply_borders(
                borders,
                cell.row_index,
                cell.row_index + cell.row_span,
                cell.column_index,
                cell.column_index + cell.column_span,
                source_order,
            );
        }
        for row_index in 0..row_count {
            let row_box = self.rows[row_index].box_;
            let borders = self.element_borders(row_box);
            grid.apply_borders(borders, row_index, row_index + 1, 0, column_count, take_source_order());
        }
        // Row groups, in the order their rows appear in the grid. Rows of a group are contiguous in
        // m_rows, since TableGrid collects them in tree order.
        let mut row_index = 0;
        while row_index < row_count {
            let group = self.parent(self.rows[row_index].box_);
            if group.is_invalid() {
                row_index += 1;
                continue;
            }
            let facts = self.node_facts(group);
            if !(facts.is_table_row_group() || facts.is_table_header_group() || facts.is_table_footer_group()) {
                row_index += 1;
                continue;
            }
            let start = row_index;
            while row_index < row_count && self.parent(self.rows[row_index].box_) == group {
                row_index += 1;
            }
            let borders = self.element_borders(group);
            grid.apply_borders(borders, start, row_index, 0, column_count, take_source_order());
        }

        // Column (<col>) elements.
        let mut column_index = 0usize;
        let mut column_group_ranges = Vec::new();
        for column_group in self.matching_children(self.table_box, |facts| facts.is_table_column_group()) {
            let group_start = column_index;
            for column in self.matching_children(column_group, |facts| facts.is_table_column()) {
                let span = self.table_column_span(column);
                let end = (column_index + span).min(column_count);
                let borders = self.element_borders(column);
                let source_order = take_source_order();
                while column_index < end {
                    grid.apply_borders(borders, 0, row_count, column_index, column_index + 1, source_order);
                    column_index += 1;
                }
            }
            column_group_ranges.push((column_group, group_start, column_index));
        }
        for (column_group, group_start, group_end) in column_group_ranges {
            if group_start < group_end {
                let borders = self.element_borders(column_group);
                grid.apply_borders(borders, 0, row_count, group_start, group_end, take_source_order());
            }
        }
        let table_borders = self.element_borders(self.table_box);
        grid.apply_borders(table_borders, 0, row_count, 0, column_count, take_source_order());

        let outer = grid.outer_edge_widths();
        let table_used = self.used_values(self.table_box);
        let old_border_box_top = table_used.border_box_top(false);
        let old_inline_borders = table_used.border_box_left(false) + table_used.border_box_right(false);
        table_used.border_top.set(outer.top);
        table_used.border_right.set(outer.right);
        table_used.border_bottom.set(outer.bottom);
        table_used.border_left.set(outer.left);
        table_used.uses_collapsing_borders_model.set(true);
        self.table_box_content_block_offset_in_wrapper += table_used.border_box_top(true) - old_border_box_top;
        let freed_inline = old_inline_borders - (table_used.border_box_left(true) + table_used.border_box_right(true));
        if let AvailableSize::Definite(available) = self.available_space.inline_size {
            self.available_space.inline_size = AvailableSize::definite(available + freed_inline);
        }

        for cell_index in 0..self.cells.len() {
            let cell = self.cells[cell_index];
            let row_end = cell.row_index + cell.row_span;
            let column_end = cell.column_index + cell.column_span;
            let widths = grid.resolve_used_widths_for_cell(cell.row_index, row_end, cell.column_index, column_end);
            let used = self.used_values(cell.box_);
            used.border_top.set(widths.top);
            used.border_right.set(widths.right);
            used.border_bottom.set(widths.bottom);
            used.border_left.set(widths.left);
            used.uses_collapsing_borders_model.set(true);
        }
        self.collapsed_border_grid = Some(grid);
    }

    fn materialize_collapsed_table_borders(&mut self) {
        let Some(grid) = self.collapsed_border_grid.take() else {
            return;
        };
        if self.purpose.is_measurement() {
            return;
        }
        if !grid.has_paintable_edges() {
            return;
        }
        let column_offsets = grid_line_offsets(self.columns.iter().map(|column| column.used_inline_size));
        let row_offsets = grid_line_offsets(self.rows.iter().map(|row| {
            if row.is_collapsed {
                CssPixels::default()
            } else {
                row.final_block_size
            }
        }));
        let (horizontal_edges, vertical_edges) = grid.take_edges();
        self.used_values(self.table_box).rare_data_mut().collapsed_table_borders =
            Some(std::rc::Rc::new(OwnedCollapsedTableBorders {
                row_offsets,
                column_offsets,
                horizontal_edges,
                vertical_edges,
            }));
    }

    // Participants resolve percentages against the table's input basis inside this context, so
    // their dependency is charged here rather than through child runs.
    fn prepare_table_participants(&mut self, preparation: TableParticipantPreparation) {
        let row_groups = self.matching_children(self.table_box, |display| display.is_table_row_group_kind());
        let participants = row_groups
            .into_iter()
            .chain(self.rows.iter().map(|row| row.box_))
            .chain(self.cells.iter().map(|cell| cell.box_));
        let sizing = self.sizing();
        let table_record = self.used_values(self.table_box);
        for participant in participants {
            if preparation == TableParticipantPreparation::CreateUsedValues {
                self.create_used_values(participant, self.participant_constraints);
            }
            if sizing.own_style_depends_on_percentage_block_size(participant) {
                table_record
                    .has_descendant_that_depends_on_percentage_block_size
                    .set(true);
            }
        }
    }

    fn use_fixed_mode_layout(&mut self) -> bool {
        // Implements https://www.w3.org/TR/css-tables-3/#in-fixed-mode.
        // A table-root is said to be laid out in fixed mode whenever the computed value of the table-layout property is equal to fixed, and the
        // specified width of the table root is either a <length-percentage>, min-content or fit-content. When the specified width is not one of
        // those values, or if the computed value of the table-layout property is auto, then the table-root is said to be laid out in auto mode.
        let style = self.style(self.table_box);
        let width = style.width();
        style.table_layout() == TABLE_LAYOUT_FIXED
            && (width.is_length() || width.is_percentage() || width.is_min_content() || width.is_fit_content())
    }

    fn compute_constrainedness(&mut self) {
        // Definition of constrainedness: https://www.w3.org/TR/css-tables-3/#constrainedness
        // NB: The definition uses https://www.w3.org/TR/CSS21/visudet.html#propdef-width for width, which doesn't include
        //     keyword values. The remaining checks can be simplified to checking whether the size is a length.
        let mut column_index = 0usize;
        for group in self.matching_children(self.table_box, |facts| facts.is_table_column_group()) {
            for column in self.matching_children(group, |facts| facts.is_table_column()) {
                if self.style(column).width().is_length() {
                    self.columns[column_index].is_constrained = true;
                }
                column_index += self.raw_column_span(column);
            }
        }
        for row_index in 0..self.rows.len() {
            let row_box = self.rows[row_index].box_;
            if self.style(row_box).height().is_length() {
                self.rows[row_index].is_constrained = true;
            }
        }
        for cell_index in 0..self.cells.len() {
            let cell = self.cells[cell_index];
            let style = self.style(cell.box_);
            if style.width().is_length() {
                self.columns[cell.column_index].is_constrained = true;
            }
            if style.height().is_length() {
                self.rows[cell.row_index].is_constrained = true;
            }
        }
    }

    fn calculate_min_content_inline_size(&self, node: Node) -> CssPixels {
        self.sizing()
            .calculate_min_content_inline_size(node, self.participant_constraints)
    }

    fn calculate_max_content_inline_size(&self, node: Node) -> CssPixels {
        self.sizing()
            .calculate_max_content_inline_size(node, self.participant_constraints)
    }

    fn calculate_min_content_block_size(&self, node: Node, inline_size: CssPixels) -> CssPixels {
        self.sizing()
            .calculate_min_content_block_size(node, inline_size, self.participant_constraints)
    }

    fn calculate_max_content_block_size(&self, node: Node, inline_size: CssPixels) -> CssPixels {
        self.sizing()
            .calculate_max_content_block_size(node, inline_size, self.participant_constraints)
    }

    fn compute_cell_measures(&mut self, include_rows: bool) {
        // Implements https://www.w3.org/TR/css-tables-3/#computing-cell-measures.
        let inline_basis = self.table_constraints.inline_basis();
        let block_basis = self.table_constraints.block_basis();
        self.compute_constrainedness();
        let fixed = self.use_fixed_mode_layout();
        let collapsed = self.style(self.table_box).border_collapse() != BORDER_COLLAPSE_SEPARATE;

        for cell_index in 0..self.cells.len() {
            let cell = self.cells[cell_index];
            let style = self.style(cell.box_);
            let padding_block_start = style.padding_top().to_px(block_basis);
            let padding_block_end = style.padding_bottom().to_px(block_basis);
            let padding_inline_start = style.padding_left().to_px(inline_basis);
            let padding_inline_end = style.padding_right().to_px(inline_basis);
            // Implement the collapsing border model https://www.w3.org/TR/CSS22/tables.html#collapsing-borders.
            let (border_block_start, border_block_end, border_inline_start, border_inline_end) = if collapsed {
                let used = self.used_values(cell.box_);
                (
                    used.border_top_collapsed(true),
                    used.border_bottom_collapsed(true),
                    used.border_left_collapsed(true),
                    used.border_right_collapsed(true),
                )
            } else {
                (
                    style.border_top_width(),
                    style.border_bottom_width(),
                    style.border_left_width(),
                    style.border_right_width(),
                )
            };
            let inline_offsets = padding_inline_start + padding_inline_end + border_inline_start + border_inline_end;
            let width = style.width();
            let max_width = style.max_width();
            let mut min_inline = style.min_width().to_px(inline_basis);
            let mut inline_size = if width.is_length() {
                width.to_px(inline_basis)
            } else {
                CssPixels::default()
            };
            let mut max_inline = if max_width.is_length() {
                max_width.to_px(inline_basis)
            } else {
                CssPixels::from_raw(i32::MAX)
            };
            if style.box_sizing() == box_sizing::BORDER_BOX {
                min_inline -= inline_offsets;
                inline_size -= inline_offsets;
                max_inline -= inline_offsets;
            }

            // https://drafts.csswg.org/css-tables-3/#computing-column-measures
            // For the purpose of measuring a column when laid out in fixed mode [...] the min-content and max-content width
            // of cells is considered zero unless they are directly specified as a length-percentage, in which case they are
            // resolved based on the table width (if it is definite, otherwise use 0).
            let (min_content_inline, max_content_inline) = if fixed {
                if width.is_length_percentage() {
                    (inline_size, inline_size)
                } else {
                    (CssPixels::default(), CssPixels::default())
                }
            } else {
                (
                    self.calculate_min_content_inline_size(cell.box_),
                    self.calculate_max_content_inline_size(cell.box_),
                )
            };
            // The outer min-content inline size of a table cell is its minimum inline size adjusted by the cell intrinsic offsets.
            self.cells[cell_index].outer_min_inline_size = min_inline.max(min_content_inline) + inline_offsets;

            if include_rows {
                let min_content_block = self.calculate_min_content_block_size(cell.box_, max_content_inline);
                let max_content_block = self.calculate_max_content_block_size(cell.box_, min_content_inline);
                let min_block = style.min_height().to_px(block_basis);
                let block_offsets = padding_block_start + padding_block_end + border_block_start + border_block_end;
                // The outer min-content block size of a table cell is its minimum block size adjusted by the cell intrinsic offsets.
                self.cells[cell_index].outer_min_block_size = min_block.max(min_content_block) + block_offsets;
                // The tables specification isn't explicit on how to use the height and max-height CSS properties in the outer max-content formulas.
                // However, during this early phase we don't have enough information to resolve percentage sizes yet and the formulas for outer sizes
                // in the specification give enough clues to pick defaults in a way that makes sense.
                let height = style.height();
                let max_height = style.max_height();
                let block_size = if height.is_length() {
                    height.to_px(block_basis)
                } else {
                    CssPixels::default()
                };
                let max_block = if max_height.is_length() {
                    max_height.to_px(block_basis)
                } else {
                    CssPixels::from_raw(i32::MAX)
                };
                self.cells[cell_index].outer_max_block_size = if self.rows[cell.row_index].is_constrained {
                    // The outer max-content height of a table-cell in a constrained row is
                    // max(min-height, height, min-content height, min(max-height, height)) adjusted by the cell intrinsic offsets.
                    // NB: min(max-height, height) doesn't have any effect here, we can simplify the expression to max(min-height, height, min-content height).
                    min_block.max(block_size.max(min_content_block)) + block_offsets
                } else {
                    // The outer max-content height of a table-cell in a non-constrained row is
                    // max(min-height, height, min-content height, min(max-height, max-content height)) adjusted by the cell intrinsic offsets.
                    min_block.max(block_size.max(min_content_block.max(max_block.min(max_content_block))))
                        + block_offsets
                };
            }

            // See the explanation for block_size and max_block_size above.
            self.cells[cell_index].outer_max_inline_size = if self.columns[cell.column_index].is_constrained {
                // The outer max-content width of a table-cell in a constrained column is
                // max(min-width, width, min-content width, min(max-width, width)) adjusted by the cell intrinsic offsets.

                // AD-HOC: The formula defined by the spec doesn't respect max-width. We use a different formula that
                //         matches the behavior that is expected by WPT and is implemented by other browsers.
                // FIXME: Open a spec issue about this.
                css_clamp(inline_size.max(min_content_inline), min_inline, max_inline) + inline_offsets
            } else {
                // The outer max-content width of a table-cell in a non-constrained column is
                // max(min-width, width, min-content width, min(max-width, max-content width)) adjusted by the cell intrinsic offsets.
                min_inline.max(inline_size.max(min_content_inline.max(max_inline.min(max_content_inline))))
                    + inline_offsets
            };
        }
    }

    fn initialize_row_content_sizes(&mut self) {
        let basis = self.table_constraints.block_basis();
        for row_index in 0..self.rows.len() {
            let style = self.style(self.rows[row_index].box_);
            let min_size = style.min_height().to_px(basis);
            let max_size = if style.max_height().is_length() {
                style.max_height().to_px(basis)
            } else {
                CssPixels::from_raw(i32::MAX)
            };
            let size = style.height().to_px(basis);
            // The outer min-content block size of a table row or row group is max(min-block-size, block-size).
            self.rows[row_index].min_size = min_size.max(size);
            // The outer max-content block size is max(min-block-size, min(max-block-size, block-size)).
            self.rows[row_index].max_size = min_size.max(max_size.min(size));
        }
    }

    fn compute_outer_content_sizes(&mut self) {
        let basis = self.table_constraints.inline_basis();
        let mut column_index = 0usize;
        for group in self.matching_children(self.table_box, |facts| facts.is_table_column_group()) {
            for column in self.matching_children(group, |facts| facts.is_table_column()) {
                let style = self.style(column);
                let min_size = style.min_width().to_px(basis);
                let max_size = if style.max_width().is_length() {
                    style.max_width().to_px(basis)
                } else {
                    CssPixels::from_raw(i32::MAX)
                };
                let size = style.width().to_px(basis);
                // The outer min-content inline size of a table-column or table-column-group is max(min-width, width).
                self.columns[column_index].min_size = min_size.max(size);
                // The outer max-content inline size of a table-column or table-column-group is max(min-width, min(max-width, width)).
                self.columns[column_index].max_size = min_size.max(max_size.min(size));
                column_index += self.raw_column_span(column);
            }
        }
        self.initialize_row_content_sizes();
    }

    // Accessors to enable direction-agnostic table measurement.

    fn track_count(&self, axis: TrackAxis) -> usize {
        match axis {
            TrackAxis::Row => self.rows.len(),
            TrackAxis::Column => self.columns.len(),
        }
    }

    fn track_min(&self, axis: TrackAxis, index: usize) -> CssPixels {
        match axis {
            TrackAxis::Row => self.rows[index].min_size,
            TrackAxis::Column => self.columns[index].min_size,
        }
    }

    fn set_track_min(&mut self, axis: TrackAxis, index: usize, value: CssPixels) {
        match axis {
            TrackAxis::Row => self.rows[index].min_size = value,
            TrackAxis::Column => self.columns[index].min_size = value,
        }
    }

    fn track_max(&self, axis: TrackAxis, index: usize) -> CssPixels {
        match axis {
            TrackAxis::Row => self.rows[index].max_size,
            TrackAxis::Column => self.columns[index].max_size,
        }
    }

    fn set_track_max(&mut self, axis: TrackAxis, index: usize, value: CssPixels) {
        match axis {
            TrackAxis::Row => self.rows[index].max_size = value,
            TrackAxis::Column => self.columns[index].max_size = value,
        }
    }

    fn track_percentage(&self, axis: TrackAxis, index: usize) -> f64 {
        match axis {
            TrackAxis::Row => self.rows[index].intrinsic_percentage,
            TrackAxis::Column => self.columns[index].intrinsic_percentage,
        }
    }

    fn set_track_percentage(&mut self, axis: TrackAxis, index: usize, value: f64) {
        match axis {
            TrackAxis::Row => self.rows[index].intrinsic_percentage = value,
            TrackAxis::Column => self.columns[index].intrinsic_percentage = value,
        }
    }

    fn set_track_has_percentage(&mut self, axis: TrackAxis, index: usize, value: bool) {
        match axis {
            TrackAxis::Row => self.rows[index].has_intrinsic_percentage = value,
            TrackAxis::Column => self.columns[index].has_intrinsic_percentage = value,
        }
    }

    fn cell_span(cell: TableCell, axis: TrackAxis) -> usize {
        match axis {
            TrackAxis::Row => cell.row_span,
            TrackAxis::Column => cell.column_span,
        }
    }

    fn cell_index(cell: TableCell, axis: TrackAxis) -> usize {
        match axis {
            TrackAxis::Row => cell.row_index,
            TrackAxis::Column => cell.column_index,
        }
    }

    fn cell_min(cell: TableCell, axis: TrackAxis) -> CssPixels {
        match axis {
            TrackAxis::Row => cell.outer_min_block_size,
            TrackAxis::Column => cell.outer_min_inline_size,
        }
    }

    fn cell_max(cell: TableCell, axis: TrackAxis) -> CssPixels {
        match axis {
            TrackAxis::Row => cell.outer_max_block_size,
            TrackAxis::Column => cell.outer_max_inline_size,
        }
    }

    fn cell_percentage(style: StyleValues, axis: TrackAxis) -> f64 {
        // Definition of percentage contribution: https://www.w3.org/TR/css-tables-3/#percentage-contribution
        let (size, max_size) = match axis {
            TrackAxis::Row => (style.height(), style.max_height()),
            TrackAxis::Column => (style.width(), style.max_width()),
        };
        let maximum = if max_size.is_percentage() {
            max_size.length_percentage().as_fraction() * 100.0
        } else {
            f64::INFINITY
        };
        let preferred = if size.is_percentage() {
            size.length_percentage().as_fraction() * 100.0
        } else {
            0.0
        };
        preferred.min(maximum)
    }

    fn initialize_intrinsic_percentages(&mut self, axis: TrackAxis) {
        if axis == TrackAxis::Row {
            for index in 0..self.rows.len() {
                let style = self.style(self.rows[index].box_);
                // Definition of percentage contribution: https://www.w3.org/TR/css-tables-3/#percentage-contribution
                self.rows[index].has_intrinsic_percentage =
                    style.max_height().is_percentage() || style.height().is_percentage();
                self.rows[index].intrinsic_percentage = Self::cell_percentage(style, axis);
            }
        } else {
            let mut column_index = 0usize;
            for group in self.matching_children(self.table_box, |facts| facts.is_table_column_group()) {
                for column in self.matching_children(group, |facts| facts.is_table_column()) {
                    let style = self.style(column);
                    // Definition of percentage contribution: https://www.w3.org/TR/css-tables-3/#percentage-contribution
                    self.columns[column_index].has_intrinsic_percentage =
                        style.max_width().is_percentage() || style.width().is_percentage();
                    self.columns[column_index].intrinsic_percentage = Self::cell_percentage(style, axis);
                    column_index += self.raw_column_span(column);
                }
            }
        }

        for cell_index in 0..self.cells.len() {
            let cell = self.cells[cell_index];
            let style = self.style(cell.box_);
            let size = match axis {
                TrackAxis::Row => style.height(),
                TrackAxis::Column => style.width(),
            };
            if !size.is_percentage() {
                continue;
            }
            let start = Self::cell_index(cell, axis);
            let span = Self::cell_span(cell, axis);
            for index in start..start + span {
                self.set_track_has_percentage(axis, index, true);
            }
            if span == 1 {
                self.set_track_percentage(
                    axis,
                    start,
                    self.track_percentage(axis, start)
                        .max(Self::cell_percentage(style, axis)),
                );
            }
        }
    }

    fn compute_intrinsic_percentage(&mut self, axis: TrackAxis, max_span: usize) {
        // https://www.w3.org/TR/css-tables-3/#intrinsic-percentage-width-of-a-column-based-on-cells-of-span-up-to-1
        self.initialize_intrinsic_percentages(axis);
        let count = self.track_count(axis);
        // Stores intermediate values for intrinsic percentage based on cells of span up to N for the iterative algorithm, to store them back at the end of the step.
        let mut contributions = (0..count)
            .map(|index| self.track_percentage(axis, index))
            .collect::<Vec<_>>();
        for current_span in 2..=max_span {
            // https://www.w3.org/TR/css-tables-3/#intrinsic-percentage-width-of-a-column-based-on-cells-of-span-up-to-n-n--1
            for cell_index in 0..self.cells.len() {
                let cell = self.cells[cell_index];
                if Self::cell_span(cell, axis) != current_span {
                    continue;
                }
                let style = self.style(cell.box_);
                let start = Self::cell_index(cell, axis);
                let end = start + current_span;
                // 1. Start with the percentage contribution of the cell.
                let mut contribution = CssPixels::nearest_value_for(Self::cell_percentage(style, axis));
                // 2. Subtract the intrinsic percentage width of the column based on cells of span up to N-1 of all columns
                //    that the cell spans. If this gives a negative result, change it to 0%.
                for index in start..end {
                    contribution -= CssPixels::nearest_value_for(self.track_percentage(axis, index));
                    contribution = contribution.max(CssPixels::default());
                }
                // Compute the sum of the non-spanning max-content sizes of all rows / columns spanned by the cell that have an intrinsic percentage
                // size of the row / column based on cells of span up to N-1 equal to 0%, to be used in step 3 of the cell contribution algorithm.
                let mut zero_sum = CssPixels::default();
                let mut zero_count = 0usize;
                for index in start..end {
                    if self.track_percentage(axis, index) == 0.0 {
                        zero_sum += self.track_max(axis, index);
                        zero_count += 1;
                    }
                }
                for (index, saved) in contributions.iter_mut().enumerate().take(end).skip(start) {
                    // If the intrinsic percentage width of a column based on cells of span up to N-1 is greater than 0%, then the intrinsic percentage width of
                    // the column based on cells of span up to N is the same as the intrinsic percentage width of the column based on cells of span up to N-1.
                    if self.track_percentage(axis, index) > 0.0 {
                        continue;
                    }
                    // Otherwise, it is the largest of the contributions of the cells in the column whose colSpan is N,
                    // where the contribution of a cell is the result of taking the following steps:
                    // 1. Start with the percentage contribution of the cell.
                    // 2. Subtract the intrinsic percentage width of the column based on cells of span up to N-1 of all columns
                    //    that the cell spans. If this gives a negative result, change it to 0%.
                    // 3. Multiply by the ratio of the column’s non-spanning max-content width to the sum of the non-spanning max-content widths of all
                    //    columns spanned by the cell that have an intrinsic percentage width of the column based on cells of span up to N-1 equal to 0%.
                    let adjusted = if zero_sum != CssPixels::default() {
                        contribution.scaled(self.track_max(axis, index).to_double() / zero_sum.to_double())
                    } else {
                        // However, if this ratio is undefined because the denominator is zero, instead use the 1 divided by the number of columns
                        // spanned by the cell that have an intrinsic percentage width of the column based on cells of span up to N-1 equal to zero.
                        contribution / zero_count
                    };
                    *saved = saved.max(adjusted.to_double());
                }
            }
            for (index, value) in contributions.iter().copied().enumerate() {
                self.set_track_percentage(axis, index, value);
            }
        }
        // Clamp total intrinsic percentage to 100%: https://www.w3.org/TR/css-tables-3/#intrinsic-percentage-width-of-a-column
        let mut total = 0.0;
        for index in 0..count {
            let value = self.track_percentage(axis, index).min(100.0 - total).max(0.0);
            self.set_track_percentage(axis, index, value);
            total += value;
        }
    }

    fn initialize_table_measures(&mut self, axis: TrackAxis) {
        if axis == TrackAxis::Row {
            let basis = self.table_constraints.block_basis();
            for cell_index in 0..self.cells.len() {
                let cell = self.cells[cell_index];
                if cell.row_span == 1 {
                    let specified = self.style(cell.box_).height().to_px(basis);
                    // https://www.w3.org/TR/css-tables-3/#row-layout makes specified cell height part of the initialization formula for row table measures:
                    // This is done by running the same algorithm as the column measurement, with the span=1 value being initialized (for min-content) with
                    // the largest of the resulting height of the previous row layout, the height specified on the corresponding table-row (if any), and
                    // the largest height specified on cells that span this row only (the algorithm starts by considering cells of span 2 on top of that assignment).
                    let row = &mut self.rows[cell.row_index];
                    row.min_size = row.min_size.max(cell.outer_min_block_size.max(specified));
                    row.max_size = row.max_size.max(cell.outer_max_block_size);
                }
            }
        } else {
            // Implement the following parts of the specification, accounting for fixed layout mode:
            // https://www.w3.org/TR/css-tables-3/#min-content-width-of-a-column-based-on-cells-of-span-up-to-1
            // https://www.w3.org/TR/css-tables-3/#max-content-width-of-a-column-based-on-cells-of-span-up-to-1
            let fixed = self.use_fixed_mode_layout();
            for cell in self.cells.iter().copied() {
                if cell.column_span == 1 && (cell.row_index == 0 || !fixed) {
                    let column = &mut self.columns[cell.column_index];
                    column.min_size = column.min_size.max(cell.outer_min_inline_size);
                    column.max_size = column.max_size.max(cell.outer_max_inline_size);
                }
            }
        }
    }

    fn compute_table_measures(&mut self, axis: TrackAxis) {
        self.initialize_table_measures(axis);
        let max_span = self
            .cells
            .iter()
            .copied()
            .map(|cell| Self::cell_span(cell, axis))
            .max()
            .unwrap_or(1)
            .max(1);
        // Since the intrinsic percentage specification uses non-spanning max-content size for the iterative algorithm,
        // run it before we compute the spanning max-content size with its own iterative algorithm for span up to N.
        self.compute_intrinsic_percentage(axis, max_span);
        let track_count = self.track_count(axis);
        for current_span in 2..=max_span {
            // https://www.w3.org/TR/css-tables-3/#min-content-width-of-a-column-based-on-cells-of-span-up-to-n-n--1
            let mut min_contributions = vec![Vec::new(); track_count];
            // https://www.w3.org/TR/css-tables-3/#max-content-width-of-a-column-based-on-cells-of-span-up-to-n-n--1
            let mut max_contributions = vec![Vec::new(); track_count];
            let track_spacing = match axis {
                TrackAxis::Row => self.border_spacing_block(),
                TrackAxis::Column => self.border_spacing_inline(),
            };
            for cell in self.cells.iter().copied() {
                if Self::cell_span(cell, axis) != current_span {
                    continue;
                }
                let start = Self::cell_index(cell, axis);
                let end = start + current_span;
                // Define the baseline max-content size as the sum of the max-content sizes based on cells of span up to N-1 of all columns that the cell spans.
                let baseline_max =
                    (start..end).fold(CssPixels::default(), |sum, index| sum + self.track_max(axis, index));
                let baseline_min =
                    (start..end).fold(CssPixels::default(), |sum, index| sum + self.track_min(axis, index));
                // Define the baseline border spacing as the sum of the horizontal border-spacing for any columns spanned by the cell, other than the one in which the cell originates.
                let spacing = track_spacing * (current_span - 1);
                // Add contribution from all rows / columns, since we've weighted the gap to the desired spanned size by the the
                // ratio of the max-content size based on cells of span up to N-1 of the row / column to the baseline max-content width.
                for index in start..end {
                    // The contribution of the cell is the sum of:
                    // the min-content size of the column based on cells of span up to N-1
                    let mut min_contribution = self.track_min(axis, index);
                    // the product of:
                    // - the ratio of:
                    //   - the max-content size of the row / column based on cells of span up to N-1 of the row / column minus the
                    //     min-content size of the row / column based on cells of span up to N-1 of the row / column, to
                    //   - the baseline max-content size minus the baseline min-content size
                    //   or zero if this ratio is undefined, and
                    // - the outer min-content size of the cell minus the baseline min-content size and the baseline border spacing, clamped
                    //   to be at least 0 and at most the difference between the baseline max-content size and the baseline min-content size
                    let normalized = if baseline_max != baseline_min {
                        (self.track_max(axis, index) - self.track_min(axis, index)).to_double()
                            / (baseline_max - baseline_min).to_double()
                    } else {
                        0.0
                    };
                    let clamped = (Self::cell_min(cell, axis) - baseline_min - spacing)
                        .max(CssPixels::default())
                        .min(baseline_max - baseline_min);
                    min_contribution += CssPixels::nearest_value_for(normalized * clamped.to_double());
                    // the product of:
                    // - the ratio of the max-content size based on cells of span up to N-1 of the column to the baseline max-content size
                    // - the outer min-content size of the cell minus the baseline max-content size and baseline border spacing, or 0 if this is negative
                    if baseline_max != CssPixels::default() {
                        min_contribution += CssPixels::nearest_value_for(
                            self.track_max(axis, index).to_double() / baseline_max.to_double(),
                        ) * (Self::cell_min(cell, axis) - baseline_max - spacing)
                            .max(CssPixels::default());
                    } else {
                        // AD-HOC: The spec does not define behavior when baseline is zero. We distribute equally.
                        //         This matches how undefined ratios are handled elsewhere.
                        min_contribution +=
                            (Self::cell_min(cell, axis) - spacing).max(CssPixels::default()) / current_span;
                    }

                    // The contribution of the cell is the sum of:
                    // the max-content size of the column based on cells of span up to N-1
                    let mut max_contribution = self.track_max(axis, index);
                    // and the product of:
                    // - the ratio of the max-content size based on cells of span up to N-1 of the column to the baseline max-content size
                    // - the outer max-content size of the cell minus the baseline max-content size and the baseline border spacing, or 0 if this is negative
                    if baseline_max != CssPixels::default() {
                        max_contribution += CssPixels::nearest_value_for(
                            self.track_max(axis, index).to_double() / baseline_max.to_double(),
                        ) * (Self::cell_max(cell, axis) - baseline_max - spacing)
                            .max(CssPixels::default());
                    } else {
                        // AD-HOC: The spec does not define behavior when baseline is zero. We distribute equally,
                        //         This matches how undefined ratios are handled elsewhere.
                        max_contribution +=
                            (Self::cell_max(cell, axis) - spacing).max(CssPixels::default()) / current_span;
                    }
                    min_contributions[index].push(min_contribution);
                    max_contributions[index].push(max_contribution);
                }
            }
            for index in 0..track_count {
                // min-content size of a row / column based on cells of span up to N (N > 1) is
                // the largest of the min-content size of the row / column based on cells of span up to N-1 and
                // the contributions of the cells in the row / column whose rowSpan / colSpan is N
                let mut min_size = self.track_min(axis, index);
                for contribution in &min_contributions[index] {
                    min_size = min_size.max(*contribution);
                }
                self.set_track_min(axis, index, min_size);
                // max-content size of a row / column based on cells of span up to N (N > 1) is
                // the largest of the max-content size based on cells of span up to N-1 and the contributions of
                // the cells in the row / column whose rowSpan / colSpan is N
                let mut max_size = self.track_max(axis, index);
                for contribution in &max_contributions[index] {
                    max_size = max_size.max(*contribution);
                }
                self.set_track_max(axis, index, max_size);
            }
        }
    }

    fn compute_capmin(&mut self) -> CssPixels {
        // The caption width minimum (CAPMIN) is the largest of the table captions min-content contribution:
        // https://drafts.csswg.org/css-tables-3/#computing-the-table-width
        let basis = self.table_constraints.inline_basis();
        let mut capmin = CssPixels::default();
        for caption in self.matching_children(self.table_box, |facts| facts.is_table_caption()) {
            let style = self.style(caption);
            let outer = |inner: CssPixels| {
                inner
                    + style.margin_left().to_px(basis)
                    + style.border_left_width()
                    + style.padding_left().to_px(basis)
                    + style.padding_right().to_px(basis)
                    + style.border_right_width()
                    + style.margin_right().to_px(basis)
            };
            let mut contribution = outer(self.calculate_min_content_inline_size(caption));
            let width = style.width();
            if width.is_length_percentage() && !width.contains_percentage() {
                let mut preferred = width.to_px(basis);
                if style.box_sizing() == box_sizing::BORDER_BOX {
                    preferred = formatting_context::subtract_border_box_adjustment(
                        preferred,
                        style.border_left_width(),
                        style.padding_left().to_px(basis),
                        style.border_right_width(),
                        style.padding_right().to_px(basis),
                    );
                }
                contribution = contribution.max(outer(preferred));
            } else if width.is_max_content() {
                contribution = contribution.max(outer(self.calculate_max_content_inline_size(caption)));
            }
            capmin = capmin.max(contribution);
        }
        capmin
    }

    fn resolve_inline_constraint(
        &mut self,
        constraint: &ComputedSize,
        grid_min: CssPixels,
        grid_max: CssPixels,
        basis: CssPixels,
    ) -> CssPixels {
        if constraint.is_min_content() {
            return grid_min;
        }
        if constraint.is_max_content() {
            return grid_max;
        }
        if constraint.is_fit_content() {
            let limit = if constraint.fit_content_available_space().is_some() {
                constraint.to_px(basis)
            } else {
                basis
            };
            return grid_max.min(grid_min.max(limit));
        }
        // CSS Sizing says box-sizing:border-box applies length/percentage width/min-width/max-width constraints to
        // the border box. The table inline-size algorithm compares content inline sizes, so convert them before comparing.
        let mut resolved = constraint.to_px(basis);
        if self.style(self.table_box).box_sizing() == box_sizing::BORDER_BOX {
            let used = self.used_values(self.table_box);
            let collapsed = used.uses_collapsing_borders_model.get();
            resolved -= used.border_box_left(collapsed) + used.border_box_right(collapsed);
        }
        resolved.max(CssPixels::default())
    }

    fn should_treat_max_inline_size_as_none(&self, available: AvailableSize) -> bool {
        self.sizing()
            .should_treat_max_inline_size_as_none(self.table_box, available, self.table_constraints)
    }

    fn compute_table_inline_size(&mut self) {
        // https://drafts.csswg.org/css-tables-3/#computing-the-table-width

        let table_style = self.style(self.table_box);
        let available_inline = self.available_space.inline_size;
        // Percentages on 'width' and 'height' on the table are relative to the table wrapper box's containing block,
        // not the table wrapper box itself.
        let basis = self.table_constraints.inline_basis();
        // Compute undistributable space due to border spacing: https://www.w3.org/TR/css-tables-3/#computing-undistributable-space.
        let spacing = (self.columns.len() + 1) * self.border_spacing_inline();
        // The row/column-grid inline-size minimum (GRIDMIN) is the sum of the min-content inline size
        // of all the columns plus cell spacing or borders.
        let grid_min = self
            .columns
            .iter()
            .fold(CssPixels::default(), |sum, column| sum + column.min_size)
            + spacing;
        // The row/column-grid inline-size maximum (GRIDMAX) is the sum of the max-content inline size
        // of all the columns plus cell spacing or borders.
        let grid_max = self
            .columns
            .iter()
            .fold(CssPixels::default(), |sum, column| sum + column.max_size)
            + spacing;
        let capmin = self.compute_capmin();
        // The used minimum inline size of a table is the greater of the resolved min-width, CAPMIN, and GRIDMIN.
        let mut used_min = grid_min.max(capmin);
        if !table_style.min_width().is_auto() {
            used_min = used_min.max(self.resolve_inline_constraint(table_style.min_width(), grid_min, grid_max, basis));
        }
        let width_is_auto_or_indefinite_percentage = table_style.width().is_auto()
            || (table_style.width().contains_percentage()
                && self.table_constraints.percentage_basis_inline_size.is_none());
        let mut used = if width_is_auto_or_indefinite_percentage {
            // If the table-root has 'width: auto', the used inline size is the greater of
            // min(GRIDMAX, the table’s containing block inline size), the used minimum inline size of the table.
            // NOTE: In normal layout the available inline size already is the wrapper's used inline size, which the
            //       parent context resolved with shrink-to-fit; filling it keeps the table and its
            //       wrapper consistent without reading the wrapper's state from here.
            let mut value = match available_inline {
                AvailableSize::MinContent => grid_min,
                AvailableSize::MaxContent => grid_max,
                AvailableSize::Definite(available) if self.layout_mode == LayoutMode::Normal => available.max(used_min),
                AvailableSize::Definite(available) => grid_max.min(available).max(used_min),
                AvailableSize::Indefinite => grid_max.max(used_min),
            };
            if !matches!(available_inline, AvailableSize::MinContent | AvailableSize::MaxContent) {
                // https://www.w3.org/TR/CSS22/tables.html#auto-table-layout
                // A percentage value for a column inline size is relative to the table inline size. If the table has
                // 'width: auto', a percentage represents a constraint on the column's inline size, which a UA should try to satisfy.
                for cell_index in 0..self.cells.len() {
                    let cell = self.cells[cell_index];
                    let cell_width = self.style(cell.box_).width();
                    if cell_width.is_percentage() {
                        let mut adjusted = spacing;
                        let percentage = cell_width.length_percentage().as_fraction() * 100.0;
                        if percentage != 0.0 {
                            adjusted += CssPixels::nearest_value_for(
                                (100.0 / percentage * cell.outer_max_inline_size.to_double()).ceil(),
                            );
                        }
                        value = if let AvailableSize::Definite(available) = available_inline {
                            value.max(adjusted).min(available)
                        } else {
                            value.max(adjusted)
                        };
                    }
                }
            }
            value
        } else if table_style.width().is_max_content() {
            grid_max
        } else {
            // If the table-root’s width property has a computed value (resolving to the table inline size) other than auto,
            // the used inline size is the greater of the resolved table inline size and the used minimum inline size.
            let mut value = self
                .resolve_inline_constraint(table_style.width(), grid_min, grid_max, basis)
                .max(used_min);
            if !self.should_treat_max_inline_size_as_none(available_inline) {
                value = value.min(self.resolve_inline_constraint(table_style.max_width(), grid_min, grid_max, basis));
            }
            value
        };
        if !self.should_treat_max_inline_size_as_none(available_inline) {
            used = used.min(self.resolve_inline_constraint(table_style.max_width(), grid_min, grid_max, basis));
        }
        used = used.max(used_min);
        let table_used = self.used_values(self.table_box);
        table_used.set_content_inline_size(used);
    }

    fn can_skip_row_intrinsic_measurement(&mut self) -> bool {
        for row_index in 0..self.rows.len() {
            if self.rows[row_index].is_collapsed {
                return false;
            }
            let style = self.style(self.rows[row_index].box_);
            if !style.height().is_auto() || !style.min_height().is_auto() || !style.max_height().is_none() {
                return false;
            }
        }
        for cell_index in 0..self.cells.len() {
            let cell = self.cells[cell_index];
            if cell.row_span != 1 {
                return false;
            }
            let style = self.style(cell.box_);
            if !style.height().is_auto() || !style.min_height().is_auto() || !style.max_height().is_none() {
                return false;
            }
        }
        true
    }

    pub(super) fn run_until_inline_size_calculation(&mut self, input: LayoutInput, skip_row_measurement: bool) {
        self.available_space = input.available_space;
        // Determine the number of rows/columns the table requires.
        let table_grid = calculate_table_grid(self, self.table_box);
        self.cells = table_grid.cells;
        self.cell_inside_layout_inputs = vec![AvailableSpace::default(); self.cells.len()];
        self.cell_pre_layout_content_block_sizes = vec![CssPixels::default(); self.cells.len()];
        self.deferred_cell_inside_layouts = vec![false; self.cells.len()];
        self.rows = table_grid.rows;
        self.columns = vec![Column::default(); table_grid.column_count];
        for cell in &self.cells {
            self.columns[cell.column_index].has_originating_cells = true;
        }

        // The containing block of every internal table box and caption is the table wrapper;
        // the table's own input carries the wrapper's constraints, and participant percentages
        // resolve against those. Percentage block sizes of participants only resolve once the table
        // itself has a non-auto block size.
        self.table_constraints = input.containing_block_constraints;
        let table_height_auto = self.style(self.table_box).height().is_auto();
        self.participant_constraints = ContainingBlockConstraints {
            percentage_basis_inline_size: self.table_constraints.percentage_basis_inline_size,
            percentage_basis_block_size: if table_height_auto {
                None
            } else {
                self.table_constraints.percentage_basis_block_size
            },
            quirks_mode_percentage_basis_block_size: self.table_constraints.quirks_mode_percentage_basis_block_size,
        };
        let participant_preparation =
            if skip_row_measurement && self.style(self.table_box).border_collapse() == BORDER_COLLAPSE_SEPARATE {
                // OPTIMIZATION: Wrapper inline sizing measures cell contents in isolated measurement runs and does not
                //               otherwise read participant UsedValues in the separated-borders model.
                TableParticipantPreparation::TrackPercentageDependenciesOnly
            } else {
                TableParticipantPreparation::CreateUsedValues
            };
        self.prepare_table_participants(participant_preparation);
        self.border_conflict_resolution();

        let mut include_rows = !skip_row_measurement;
        self.needs_fixed_mode_row_measurement = false;
        // OPTIMIZATION: Row intrinsic measurements are only needed when row block-size constraints or row spans can affect
        //               the later row distribution. Simple tables get their actual row block sizes from cell layout.
        if include_rows && self.can_skip_row_intrinsic_measurement() {
            include_rows = false;
        }
        if include_rows && self.use_fixed_mode_layout() {
            // https://drafts.csswg.org/css-tables-3/#computing-column-measures
            // For the purpose of measuring a column when laid out in fixed mode ... the min-content and max-content width
            // of cells is considered zero.
            //
            // https://drafts.csswg.org/css-tables-3/#ROWMIN
            // ROWMIN is defined as the sum of the minimum block sizes of the rows after a first row layout pass.
            // NB: So defer fixed-mode row measurement until after columns have their used widths.
            include_rows = false;
            self.needs_fixed_mode_row_measurement = true;
        }
        // Compute the minimum width of each column.
        self.compute_cell_measures(include_rows);
        self.compute_outer_content_sizes();
        self.compute_table_measures(TrackAxis::Column);
        if include_rows {
            // https://www.w3.org/TR/css-tables-3/#row-layout
            // Since specified cell block sizes were ignored during row layout and cells spanning multiple rows were not
            // sized correctly, their block size must eventually be distributed to the rows they span. This is done
            // by running the same algorithm as the column measurement, with the span=1 value being initialized (for min-content) with the largest
            // of the resulting block size of the previous row layout, the size specified on the corresponding table row,
            // and the largest block size specified on cells that span only this row.
            self.compute_table_measures(TrackAxis::Row);
        }
        // Compute the inline size of the table.
        self.compute_table_inline_size();
    }

    fn layout_inside_cell(
        &mut self,
        run: &FormattingContextRun,
        cell: TableCell,
        input: AvailableSpace,
        adopt_automatic_content_block_size: bool,
        intrinsic_block_padding: Option<(CssPixels, CssPixels)>,
    ) -> DerivedBaselines {
        let layout_input = LayoutInput {
            available_space: input,
            containing_block_constraints: ContainingBlockConstraints::default(),
            content_box_position_in_bfc_root: None,
            sizing: RootSizingDirectives {
                adopt_automatic_content_block_size,
                table_cell_intrinsic_block_padding: intrinsic_block_padding,
                ..RootSizingDirectives::default()
            },
            participation: ParticipationInParentFormattingContext::Item,
        };
        match formatting_context::layout_inside_child(run, None, None, cell.box_, self.layout_mode, layout_input, false)
        {
            ChildLayoutOutcome::Created(result) => result.baselines,
            ChildLayoutOutcome::ReenterCurrent => {
                self.run(run, layout_input, None);
                self.used_values(cell.box_).content_baselines_from_cells()
            }
            ChildLayoutOutcome::Skipped => self.used_values(cell.box_).content_baselines_from_cells(),
        }
    }

    fn cell_box_baseline(&self, cell_box: Node, committing_run_baselines: Option<DerivedBaselines>) -> CssPixels {
        let Some(content_baselines) = committing_run_baselines else {
            return self.box_baseline(cell_box);
        };
        formatting_context::box_baseline_with_content_baselines(
            &self.callbacks,
            cell_box,
            &self.used_values(cell_box),
            formatting_context::BaselineSet::First,
            content_baselines,
        )
    }

    fn box_baseline(&self, node: Node) -> CssPixels {
        formatting_context::box_baseline(
            &self.callbacks,
            node,
            &self.used_values(node),
            formatting_context::BaselineSet::First,
        )
    }

    fn measure_cell(
        &self,
        cell: TableCell,
        used: &UsedValues,
        inner: AvailableSpace,
        adopt_automatic_content_block_size: bool,
    ) -> Option<TableCellMeasurement> {
        let facts = NodeFacts::new(&self.callbacks, cell.box_);
        if self.layout_mode == LayoutMode::IntrinsicSizing
            && !facts.is_inline()
            && used.inline_size_constraint.get() == SizeConstraint::None
            && used.block_size_constraint.get() == SizeConstraint::None
            && used.has_definite_inline_size()
            && used.has_definite_block_size()
        {
            return None;
        }
        if !facts.can_have_children() {
            return None;
        }

        let key = TableCellMeasurementKey {
            layout_mode: self.layout_mode,
            available_space: inner,
            content_inline_size: used.content_inline_size.get(),
            content_block_size: used.content_block_size.get(),
            has_definite_inline_size: used.has_definite_inline_size(),
            has_definite_block_size: used.has_definite_block_size(),
            inline_size_constraint: used.inline_size_constraint.get(),
            block_size_constraint: used.block_size_constraint.get(),
            uses_collapsing_borders_model: used.uses_collapsing_borders_model.get(),
            adopt_automatic_content_block_size,
        };
        let arena = self.callbacks.arena();
        let data = self.callbacks.node_data(cell.box_);
        if let Some(cached) = arena.table_cell_measurement_cache_get(data, key) {
            return Some(cached);
        }

        let measured = self.measure_cell_content(cell, used, inner, adopt_automatic_content_block_size);
        arena.note_table_cell_measurement_cache_miss();
        arena.table_cell_measurement_cache_put(data, key, measured);
        Some(measured)
    }

    fn measure_cell_content(
        &self,
        cell: TableCell,
        used: &UsedValues,
        inner: AvailableSpace,
        adopt_automatic_content_block_size: bool,
    ) -> TableCellMeasurement {
        // The table formatting context owns the cell's outer geometry. Seed the inputs
        // needed to lay out its contents without copying placement or layout outputs.
        let measurement = formatting_context::MeasurementState::create(self.callbacks);
        let measured_root = measurement.create_used_values(cell.box_, ContainingBlockConstraints::default());
        used.mirror_box_metrics_and_size_constraints_into(&measured_root);
        measured_root
            .has_definite_inline_size
            .set(used.has_definite_inline_size());
        measured_root
            .has_definite_block_size
            .set(used.has_definite_block_size());
        measured_root
            .uses_collapsing_borders_model
            .set(used.uses_collapsing_borders_model.get());

        let result = measurement.run_with_layout_mode(
            cell.box_,
            &measured_root,
            self.layout_mode,
            LayoutInput {
                available_space: inner,
                containing_block_constraints: ContainingBlockConstraints::default(),
                content_box_position_in_bfc_root: None,
                sizing: RootSizingDirectives {
                    adopt_automatic_content_block_size,
                    ..RootSizingDirectives::default()
                },
                participation: ParticipationInParentFormattingContext::Item,
            },
        );
        TableCellMeasurement {
            automatic_content_block_size: result.automatic_content_block_size,
            baselines: result.baselines,
        }
    }

    fn compute_table_block_size(&mut self, run: &FormattingContextRun) {
        // First pass of row block-size calculation:
        for row_index in 0..self.rows.len() {
            if self.rows[row_index].is_collapsed {
                self.rows[row_index].base_block_size = CssPixels::default();
                continue;
            }
            let height = self.style(self.rows[row_index].box_).height();
            if height.is_length() {
                // NOTE: A <length> block size resolves without a percentage basis.
                self.rows[row_index].base_block_size = self.rows[row_index]
                    .base_block_size
                    .max(height.to_px(CssPixels::default()));
            }
        }
        let inline_basis = self.participant_constraints.inline_basis();
        let participant_block_basis = self.participant_constraints.block_basis();
        let collapsed = self.style(self.table_box).border_collapse() != BORDER_COLLAPSE_SEPARATE;
        let inline_spacing = self.border_spacing_inline();
        // First pass of cells layout:
        for cell_index in 0..self.cells.len() {
            let cell = self.cells[cell_index];
            let style = self.style(cell.box_);
            let span_inline = (0..cell.column_span).fold(CssPixels::default(), |sum, index| {
                sum + self.columns[cell.column_index + index].used_inline_size
            });
            let used = self.used_values(cell.box_);
            used.padding_top.set(style.padding_top().to_px(inline_basis));
            used.padding_bottom.set(style.padding_bottom().to_px(inline_basis));
            used.padding_left.set(style.padding_left().to_px(inline_basis));
            used.padding_right.set(style.padding_right().to_px(inline_basis));
            if !collapsed {
                used.border_top.set(style.border_top_width());
                used.border_bottom.set(style.border_bottom_width());
                used.border_left.set(style.border_left_width());
                used.border_right.set(style.border_right_width());
            }
            let height = style.height();
            if !self.rows[cell.row_index].is_collapsed && height.is_length() {
                let cell_size = height.to_px(participant_block_basis);
                used.set_content_block_size(
                    cell_size - used.border_box_top(collapsed) - used.border_box_bottom(collapsed),
                );
                self.rows[cell.row_index].base_block_size = self.rows[cell.row_index].base_block_size.max(cell_size);
            }
            // Compute cell inline size as specified by https://www.w3.org/TR/css-tables-3/#bounding-box-assignment:
            // The position of any table cell, track, or track group is defined by the sums of its spanned columns and rows:
            // - the inline/block sizes of all spanned visible columns/rows
            // - the inline/block border spacing times the amount of spanned visible columns/rows minus one
            // FIXME: Account for visibility.
            used.set_content_inline_size(
                span_inline - used.border_box_left(collapsed) - used.border_box_right(collapsed)
                    + inline_spacing * (cell.column_span - 1),
            );

            let outer_space = self.available_space;
            let inner = used.available_inner_space_or_constraints_from(outer_space);
            self.cell_inside_layout_inputs[cell_index] = inner;
            self.cell_pre_layout_content_block_sizes[cell_index] = used.content_block_size.get();
            let defer_inside_layout = style.height().is_percentage()
                || style.vertical_align_is_keyword()
                || self.anonymous_cell_wraps_flex_or_grid(cell);
            self.deferred_cell_inside_layouts[cell_index] = defer_inside_layout;
            let mut content_baselines = None;
            if defer_inside_layout {
                // This cell's final inside layout happens once row heights are final; measure its
                // content in a throwaway state instead of laying out the committing state twice.
                if let Some(measured) = self.measure_cell(cell, &used, inner, true) {
                    used.set_content_block_size(measured.automatic_content_block_size);
                    content_baselines = Some(measured.baselines);
                }
            } else {
                content_baselines = Some(self.layout_inside_cell(run, cell, inner, true, None));
            }
            if self.needs_fixed_mode_row_measurement {
                let min_size = style.min_height().to_px(participant_block_basis);
                let offsets = used.border_box_top(collapsed) + used.border_box_bottom(collapsed);
                let measured = used.border_box_block_size(collapsed).max(min_size + offsets);
                self.cells[cell_index].outer_min_block_size = measured;
                self.cells[cell_index].outer_max_block_size = measured;
            }
            // https://drafts.csswg.org/css2/#height-layout
            // The baseline of a cell is the baseline of the first in-flow line box in the cell, or the first in-flow
            // table-row in the cell, whichever comes first.
            let baseline = self.cell_box_baseline(cell.box_, content_baselines);
            self.cells[cell_index].baseline = baseline;
            // Implements https://www.w3.org/TR/css-tables-3/#computing-the-table-height

            // The minimum block size of a row is the maximum of:
            // - the computed block size (if definite, percentages being considered 0px) of its corresponding table row,
            // - the computed block size of each cell spanning the current row exclusively (if definite, percentages being treated as 0px), and
            // - the minimum block size (ROWMIN) required by the cells spanning the row.
            // Note that we've already applied the first rule at the top of the method.
            if !self.rows[cell.row_index].is_collapsed {
                if cell.row_span == 1 {
                    let size = used.border_box_block_size(collapsed);
                    self.rows[cell.row_index].base_block_size = self.rows[cell.row_index].base_block_size.max(size);
                }
                if !self.needs_fixed_mode_row_measurement {
                    self.rows[cell.row_index].base_block_size = self.rows[cell.row_index]
                        .base_block_size
                        .max(self.rows[cell.row_index].min_size);
                }
                self.rows[cell.row_index].baseline = self.rows[cell.row_index].baseline.max(baseline);
            }
        }

        if self.needs_fixed_mode_row_measurement {
            self.initialize_row_content_sizes();
            self.compute_table_measures(TrackAxis::Row);
            for row in &mut self.rows {
                if !row.is_collapsed {
                    row.base_block_size = row.base_block_size.max(row.min_size);
                }
            }
        }
        self.table_block_size = self
            .rows
            .iter()
            .fold(CssPixels::default(), |sum, row| sum + row.base_block_size);
        if let Some(minimum) = self.min_border_box_block_size_from_flex_item {
            let used = self.used_values(self.table_box);
            let collapsed = used.uses_collapsing_borders_model.get();
            let content_min = minimum - used.border_box_top(collapsed) - used.border_box_bottom(collapsed);
            self.table_block_size = self.table_block_size.max(content_min);
        }
        let table_style = self.style(self.table_box);
        if !table_style.height().is_auto() {
            // If the table has a `height` property other than auto, it is treated as a minimum block size for the
            // table grid, and will eventually be distributed to the rows if their collective minimum block size is smaller.
            let mut specified = table_style.height().to_px(self.table_constraints.block_basis());
            if table_style.box_sizing() == box_sizing::BORDER_BOX {
                let used = self.used_values(self.table_box);
                let collapsed = used.uses_collapsing_borders_model.get();
                specified -= used.border_box_top(collapsed) + used.border_box_bottom(collapsed);
            }
            self.table_block_size = self.table_block_size.max(specified);
        }
        for row in &mut self.rows {
            // Reference size is the largest of
            // - its initial base block size and
            // - its new base block size (the one evaluated during the second layout pass, where percentages used in
            //   row groups, rows, and cells were resolved according to the table block size, instead of
            //   being ignored as 0px).

            // Assign reference size to base size. Later, the reference size might change to a larger value during
            // the second pass of rows layout.
            row.reference_block_size = row.base_block_size;
        }
        // Second pass of row block-size calculation:
        // At this point, percentage row block sizes can be resolved because the final table block size is calculated.
        for row_index in 0..self.rows.len() {
            if self.rows[row_index].is_collapsed {
                self.rows[row_index].reference_block_size = CssPixels::default();
                continue;
            }
            let style = self.style(self.rows[row_index].box_);
            if style.height().is_percentage() {
                let used = style.height().to_px(self.table_block_size);
                self.rows[row_index].reference_block_size = self.rows[row_index].reference_block_size.max(used);
            }
        }
        // Second pass cells layout:
        // At this point, percentage cell block sizes can be resolved because the final table block size is calculated.
        for cell_index in 0..self.cells.len() {
            let cell = self.cells[cell_index];
            let style = self.style(cell.box_);
            if !style.height().is_percentage() {
                continue;
            }
            let cell_size = style.height().to_px(self.table_block_size);
            let used = self.used_values(cell.box_);
            used.set_content_block_size(cell_size - used.border_box_top(collapsed) - used.border_box_bottom(collapsed));
            self.cell_pre_layout_content_block_sizes[cell_index] = used.content_block_size.get();
            if !self.rows[cell.row_index].is_collapsed {
                self.rows[cell.row_index].reference_block_size =
                    self.rows[cell.row_index].reference_block_size.max(cell_size);
            }
            let span_inline = (0..cell.column_span).fold(CssPixels::default(), |sum, index| {
                sum + self.columns[cell.column_index + index].used_inline_size
            });
            used.set_content_inline_size(
                span_inline - used.border_box_left(collapsed) - used.border_box_right(collapsed)
                    + inline_spacing * (cell.column_span - 1),
            );
            let inner = used.available_inner_space_or_constraints_from(self.available_space);
            self.cell_inside_layout_inputs[cell_index] = inner;
            // The first pass measured this cell at its automatic block size; measure it again at
            // the percentage-resolved size to preserve the baseline its final inside layout will use.
            let content_baselines = self
                .measure_cell(cell, &used, inner, false)
                .map(|measured| measured.baselines);
            let baseline = self.cell_box_baseline(cell.box_, content_baselines);
            self.cells[cell_index].baseline = baseline;
            if !self.rows[cell.row_index].is_collapsed {
                let border_size = used.border_box_block_size(collapsed);
                self.rows[cell.row_index].reference_block_size =
                    self.rows[cell.row_index].reference_block_size.max(border_size);
                self.rows[cell.row_index].baseline = self.rows[cell.row_index].baseline.max(baseline);
            }
        }
    }

    fn distribute_block_size_to_rows(&mut self) {
        let sum = self
            .rows
            .iter()
            .fold(CssPixels::default(), |sum, row| sum + row.reference_block_size);
        let visible = self.rows.iter().filter(|row| !row.is_collapsed).count();
        if sum == CssPixels::default() {
            return;
        }
        let auto_rows = (0..self.rows.len())
            .filter(|index| !self.rows[*index].is_collapsed && self.style(self.rows[*index].box_).height().is_auto())
            .collect::<Vec<_>>();
        if self.table_block_size <= sum {
            // If the table block size is no larger than the sum of reference sizes, each final row block size is the
            // weighted mean of the base and reference sizes that yields the correct total block size.

            for row in &mut self.rows {
                if row.is_collapsed {
                    row.final_block_size = CssPixels::default();
                } else {
                    row.final_block_size = CssPixels::nearest_value_for(
                        self.table_block_size.to_double() * row.reference_block_size.to_double() / sum.to_double(),
                    );
                }
            }
        } else if !auto_rows.is_empty() {
            // Else, if the table owns any auto-block-size row, each non-auto row receives its reference block size and
            // auto rows receive their reference size plus an equal share of the missing table block size.

            for row in &mut self.rows {
                row.final_block_size = row.reference_block_size;
            }
            let increment = (self.table_block_size - sum) / auto_rows.len();
            for index in auto_rows {
                self.rows[index].final_block_size += increment;
            }
        } else {
            // Else, all rows receive their reference size plus an equal share of the missing table block size.

            let increment = (self.table_block_size - sum) / visible;
            for row in &mut self.rows {
                row.final_block_size = if row.is_collapsed {
                    CssPixels::default()
                } else {
                    row.reference_block_size + increment
                };
            }
        }
        // Add undistributable space due to border spacing: https://www.w3.org/TR/css-tables-3/#computing-undistributable-space.
        let spacing = self.border_spacing_block();
        self.table_block_size += (visible + 1) * spacing;
    }

    fn position_row_boxes(&mut self) {
        let table_used = self.used_values(self.table_box);
        let block_spacing = self.border_spacing_block();
        let inline_spacing = self.border_spacing_inline();
        let inline_offset = table_used.border_box_left(table_used.uses_collapsing_borders_model.get()) + inline_spacing;
        let mut row_block_offset = self.table_box_content_block_offset_in_wrapper + block_spacing;
        for row_index in 0..self.rows.len() {
            let row = &self.rows[row_index];
            let inline_size = self
                .columns
                .iter()
                .fold(CssPixels::default(), |sum, column| sum + column.used_inline_size)
                + if self.columns.len() >= 2 {
                    inline_spacing * (self.columns.len() - 1)
                } else {
                    CssPixels::default()
                };
            let used = self.used_values(row.box_);
            used.set_content_block_size(row.final_block_size);
            used.set_content_inline_size(inline_size);
            self.place_child(row.box_, inline_offset, row_block_offset);
            if !row.is_collapsed {
                row_block_offset += row.final_block_size + block_spacing;
            }
        }

        let mut group_block_offset = self.table_box_content_block_offset_in_wrapper + block_spacing;
        for group in self.matching_children(self.table_box, |display| display.is_table_row_group_kind()) {
            let group_rows = self.matching_children(group, |facts| facts.is_table_row());
            let mut block_size = CssPixels::default();
            let mut inline_size = CssPixels::default();
            for row in &group_rows {
                let used = self.used_values(*row);
                block_size += used.border_box_block_size(false);
                inline_size = inline_size.max(used.border_box_inline_size(false));
            }
            if group_rows.len() >= 2 {
                block_size += block_spacing * (group_rows.len() - 1);
            }
            let used = self.used_values(group);
            used.set_content_block_size(block_size);
            used.set_content_inline_size(inline_size);
            self.place_child(group, inline_offset, group_block_offset);
            group_block_offset += block_size
                + if group_rows.is_empty() {
                    CssPixels::default()
                } else {
                    block_spacing
                };
        }
        let padding_top = table_used.padding_top.get();
        let total =
            row_block_offset.max(group_block_offset) - self.table_box_content_block_offset_in_wrapper - padding_top;
        self.table_block_size = self.table_block_size.max(total);
    }

    fn layout_deferred_cells_inside(&mut self, run: &FormattingContextRun) {
        // Deferred cells get their one and only committing inside layout here, once row
        // block sizes are final.
        let collapsed = self.style(self.table_box).border_collapse() != BORDER_COLLAPSE_SEPARATE;
        for cell_index in 0..self.cells.len() {
            if !self.deferred_cell_inside_layouts[cell_index] {
                continue;
            }
            let cell = self.cells[cell_index];
            let adopt_automatic_content_block_size = !self.style(cell.box_).height().is_percentage();
            let intrinsic_block_padding = self.cell_intrinsic_block_padding(cell, collapsed);
            let used = self.used_values(cell.box_);
            let measured_content_block_size = used.content_block_size.get();
            // The first pass adopted the measured automatic block size so row sizing could read
            // it; restore the pre-layout size so the cell's children resolve percentages against
            // the same basis the measurement saw.
            used.set_content_block_size(self.cell_pre_layout_content_block_sizes[cell_index]);
            let inner = self.cell_inside_layout_inputs[cell_index];
            self.layout_inside_cell(
                run,
                cell,
                inner,
                adopt_automatic_content_block_size,
                intrinsic_block_padding,
            );
            if adopt_automatic_content_block_size {
                debug_assert_eq!(
                    used.content_block_size.get(),
                    measured_content_block_size,
                    "measured and committed automatic block sizes diverged"
                );
            }
        }
    }

    fn cell_intrinsic_block_padding(&mut self, cell: TableCell, collapsed: bool) -> Option<(CssPixels, CssPixels)> {
        let row_size = self.compute_row_content_block_size(cell);
        let used = self.used_values(cell.box_);
        let style = self.style(cell.box_);
        // When a table cell is an anonymous wrapper around a flex or grid container (e.g., a <td> with display:flex is
        // wrapped in an anonymous table-cell box per CSS Tables 3), the cell should be aligned to the top. This allows
        // the flex/grid container to fill the cell and handle alignment of its children via its own properties.
        if self.anonymous_cell_wraps_flex_or_grid(cell) {
            return Some((CssPixels::default(), row_size - used.border_box_block_size(collapsed)));
        }
        if !style.vertical_align_is_keyword() {
            return None;
        }
        // The following image shows various alignment lines of a row:
        // https://www.w3.org/TR/css-tables-3/images/cell-align-explainer.png
        // https://drafts.csswg.org/css2/#height-layout
        // In the context of tables, values for vertical-align have the following meanings:
        match style.vertical_align_keyword() {
            vertical_align::MIDDLE => {
                // The center of the cell is aligned with the center of the rows it spans.
                let difference = row_size - used.border_box_block_size(collapsed);
                Some((difference / 2, difference / 2))
            }
            vertical_align::TOP => {
                // The top of the cell box is aligned with the top of the first row it spans.
                Some((CssPixels::default(), row_size - used.border_box_block_size(collapsed)))
            }
            vertical_align::BOTTOM => {
                // The bottom of the cell box is aligned with the bottom of the last row it spans.
                Some((row_size - used.border_box_block_size(collapsed), CssPixels::default()))
            }
            vertical_align::SUB
            | vertical_align::SUPER
            | vertical_align::TEXT_BOTTOM
            | vertical_align::TEXT_TOP
            | vertical_align::BASELINE => {
                // These values do not apply to cells; the cell is aligned at the baseline instead.

                // The baseline of the cell is put at the same height as the baseline of the first of the rows it spans.
                let padding_top = self.rows[cell.row_index].baseline - cell.baseline;
                Some((
                    padding_top,
                    row_size - (used.border_box_block_size(collapsed) + padding_top),
                ))
            }
            _ => panic!("invalid vertical-align keyword"),
        }
    }

    fn compute_row_content_block_size(&mut self, cell: TableCell) -> CssPixels {
        // The block size of a cell is the sum of all spanned rows, as described in
        // https://www.w3.org/TR/css-tables-3/#bounding-box-assignment
        let first = self.used_values(self.rows[cell.row_index].box_);
        if cell.row_span == 1 {
            return first.content_block_size.get();
        }
        // When the row span is greater than 1, the borders of inner rows within the span have to be
        // included in the content block size of the spanning cell. First top and final bottom borders are
        // excluded to be consistent with the handling of row span 1 case above, which uses the content
        // block size (no top and bottom borders) of the row.
        let mut span = CssPixels::default();
        for index in 0..cell.row_span {
            let used = self.used_values(self.rows[cell.row_index + index].box_);
            if index == 0 {
                span += used.content_block_size.get() + used.border_box_bottom(false);
            } else if index == cell.row_span - 1 {
                span += used.border_box_top(false) + used.content_block_size.get();
            } else {
                span += used.border_box_block_size(false);
            }
        }
        // Compute cell block size as specified by https://www.w3.org/TR/css-tables-3/#bounding-box-assignment:
        // The logical size is the sum of:
        // - the inline/block sizes of all spanned visible columns/rows
        // - the inline/block border spacing times the amount of spanned visible columns/rows minus one
        // FIXME: Account for visibility.
        span + self.border_spacing_block() * (cell.row_span - 1)
    }

    fn anonymous_cell_wraps_flex_or_grid(&self, cell: TableCell) -> bool {
        if !self.node_facts(cell.box_).is_anonymous() {
            return false;
        }
        let child = self.first_child(cell.box_);
        if child.is_invalid() || !self.next_sibling(child).is_invalid() {
            return false;
        }
        let facts = self.node_facts(child);
        facts.is_box() && (facts.display().is_flex_inside() || facts.display().is_grid_inside())
    }

    fn position_cell_boxes(&mut self) {
        let mut offset = CssPixels::default();
        for column in &mut self.columns {
            column.inline_offset = offset;
            offset += column.used_inline_size;
        }
        let spacing = self.border_spacing_inline();
        let collapsed = self.style(self.table_box).border_collapse() != BORDER_COLLAPSE_SEPARATE;
        for cell_index in 0..self.cells.len() {
            let cell = self.cells[cell_index];
            let used = self.used_values(cell.box_);
            let row_used = self.used_values(self.rows[cell.row_index].box_);
            // Compute cell position as specified by https://www.w3.org/TR/css-tables-3/#bounding-box-assignment:
            // left/top location is the sum of:
            // - for top: the height reserved for top captions (including margins), if any
            // - the padding-left/padding-top and border-left-width/border-top-width of the table
            // FIXME: Account for visibility.
            let x = row_used.content_offset.get().x
                + used.border_box_left(collapsed)
                + self.columns[cell.column_index].inline_offset
                + spacing * cell.column_index;
            let y = row_used.content_offset.get().y + used.border_box_top(collapsed);
            self.place_child(cell.box_, x, y);
        }
    }

    fn compute_and_store_baselines(&self, node: Node) {
        let baselines = formatting_context::derive_baselines(&self.records, &self.callbacks, node, false);
        if node == self.table_box {
            self.derived_baselines_of_root_box.set(baselines);
        } else {
            formatting_context::store_derived_baselines(&self.used_values(node), baselines);
        }
    }

    pub(super) fn run(
        &mut self,
        run: &FormattingContextRun,
        input: LayoutInput,
        inline_layout: Option<TableInlineLayout>,
    ) {
        self.available_space = input.available_space;
        self.min_border_box_block_size_from_flex_item = input.sizing.forced_min_border_box_block_size;
        self.table_box_content_block_offset_in_wrapper = input
            .sizing
            .table_box_content_block_offset_in_wrapper
            .unwrap_or_default();
        if inline_layout.is_none_or(|layout| !self.reuse_inline_layout(input, layout)) {
            self.run_until_inline_size_calculation(input, false);
        }
        if matches!(
            self.available_space.inline_size,
            AvailableSize::MinContent | AvailableSize::MaxContent
        ) && !matches!(
            self.available_space.block_size,
            AvailableSize::MinContent | AvailableSize::MaxContent
        ) {
            return;
        }

        let table_used = self.used_values(self.table_box);
        // The total inline-axis border spacing is defined for each table:
        // - For tables laid out in separated-borders mode containing at least one column, the inline-axis component of the computed value of the border-spacing property times one plus the number of columns in the table
        // - Otherwise, 0
        let total_spacing = if self.columns.is_empty() {
            CssPixels::default()
        } else {
            (self.columns.len() + 1) * self.border_spacing_inline()
        };
        // The assignable table inline size is its used inline size minus the inline-axis border spacing.
        let assignable = table_used.content_inline_size.get() - total_spacing;
        let fixed = self.use_fixed_mode_layout();
        // Distribute the inline size of the table among columns.
        distribute_inline_size(&mut self.columns, assignable, fixed);
        self.compute_table_block_size(run);
        self.distribute_block_size_to_rows();
        self.position_row_boxes();
        self.layout_deferred_cells_inside(run);
        self.position_cell_boxes();
        self.materialize_collapsed_table_borders();
        table_used.set_content_block_size(self.table_block_size);
        // Derive baselines for the table internals bottom-up (rows, then row groups, then the table box)
        // now that all offsets are final, so the table exports its baseline to outside consumers
        // (e.g. an inline-table participating in a line box).
        for row in &self.rows {
            self.compute_and_store_baselines(row.box_);
        }
        for group in self.matching_children(self.table_box, |display| display.is_table_row_group_kind()) {
            self.compute_and_store_baselines(group);
        }
        self.compute_and_store_baselines(self.table_box);
        self.automatic_content_block_size = self.table_block_size;
    }

    pub(super) fn derived_baselines_of_root_box(&self) -> DerivedBaselines {
        self.derived_baselines_of_root_box.get()
    }

    pub(super) fn automatic_content_inline_size(&self) -> CssPixels {
        let used = self.used_values(self.table_box);
        used.content_inline_size.get()
    }
}
