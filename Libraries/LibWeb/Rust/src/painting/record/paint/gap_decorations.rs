/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::color_resolution::ColorResolutionInput;
use crate::css::computed_value_types::{ComputedGapRules, ComputedStyleValueHandle};
use crate::css::computed_value_views::{ComputedValuesView, LengthPercentageRef};
use crate::css::css_enums::{keyword, line_style, rule_break, rule_overlap, rule_visibility_items};
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::css::style_value::StyleValueData;
use crate::layout::grid_formatting_context::GridGapAxis;
use crate::layout::node_data::NodeSlotId;
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::paintable_geometry;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::border::{BordersDataDevicePixels, border_side, paint_all_borders};
use crate::painting::record::trace::Observer;
use libgfx_rust::CornerRadii;

/// One painted piece of a gap decoration, in absolute CSS pixels.
pub(crate) struct GapDecoration {
    pub(crate) rect: CssPixelRect,
    /// Whether the decoration runs vertically, between columns laid out side by side.
    pub(crate) is_vertical: bool,
    pub(crate) color: u32,
    pub(crate) line_style: u8,
    pub(crate) width: CssPixels,
}

pub(crate) fn has_visible_gap_decorations(style: ComputedValuesView<'_>) -> bool {
    if !style.display().is_grid_inside() {
        return false;
    }
    let misc_layout = style.misc_layout();
    [&misc_layout.column_rules, &misc_layout.row_rules]
        .into_iter()
        .any(|rules| {
            rules.style.style_value().is_some_and(|value| {
                GapValueList::new(value)
                    .values()
                    .any(|value| !matches!(line_style_of(value), line_style::NONE | line_style::HIDDEN))
            })
        })
}

// https://drafts.csswg.org/css-gaps-1/#gap-decorations
pub(crate) fn paint_gap_decorations<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId) {
    let Some(style) = recorder.source.node_style_if_live(paintable) else {
        return;
    };
    let converter = recorder.converter;
    for decoration in gap_decorations(recorder.source, paintable, style) {
        let side = border_side(
            &converter,
            true,
            decoration.color,
            match decoration.line_style {
                line_style::INSET => line_style::RIDGE,
                line_style::OUTSET => line_style::GROOVE,
                rule_style => rule_style,
            },
            decoration.width,
        );
        let borders_data = match decoration.is_vertical {
            true => BordersDataDevicePixels {
                left: side,
                force_dark_role: ForceDarkRole::Border,
                ..BordersDataDevicePixels::default()
            },
            false => BordersDataDevicePixels {
                top: side,
                force_dark_role: ForceDarkRole::Border,
                ..BordersDataDevicePixels::default()
            },
        };
        paint_all_borders(
            &mut recorder.recorder,
            converter.rounded_device_rect(decoration.rect),
            CornerRadii::default(),
            &borders_data,
        );
    }
}

/// The gap decorations of a grid container in the order they paint.
pub(crate) fn gap_decorations(
    arena: &impl GeometryRead,
    paintable: NodeSlotId,
    style: ComputedValuesView<'_>,
) -> Vec<GapDecoration> {
    let Some(tracks) = paintable_geometry::committed_used_grid_tracks(arena, paintable) else {
        return Vec::new();
    };
    let Some(geometry) = tracks.gaps.as_ref() else {
        return Vec::new();
    };
    let misc_layout = style.misc_layout();
    let color_input = ColorResolutionInput::for_style(style);
    let current_color = style.current_color();

    // The grid's columns run along the horizontal axis of the frame it was laid out in, unless its writing mode is
    // orthogonal to the frame's.
    let (column_axis, row_axis) = match geometry.axes_are_transposed {
        false => (&geometry.frame_horizontal, &geometry.frame_vertical),
        true => (&geometry.frame_vertical, &geometry.frame_horizontal),
    };
    let column_areas: Vec<ItemArea> = geometry
        .items
        .iter()
        .map(|area| match geometry.axes_are_transposed {
            false => ItemArea {
                columns: area.frame_horizontal.clone(),
                rows: area.frame_vertical.clone(),
            },
            true => ItemArea {
                columns: area.frame_vertical.clone(),
                rows: area.frame_horizontal.clone(),
            },
        })
        .collect();
    let row_areas: Vec<ItemArea> = column_areas
        .iter()
        .map(|area| ItemArea {
            columns: area.rows.clone(),
            rows: area.columns.clone(),
        })
        .collect();

    let column_gaps = AxisGaps::new(column_axis, &misc_layout.column_rules, &color_input, current_color);
    let row_gaps = AxisGaps::new(row_axis, &misc_layout.row_rules, &color_input, current_color);
    let columns = AxisDecorations {
        segments: decoration_segments(&column_gaps, &row_gaps, &column_areas, &misc_layout.column_rules),
        gaps: column_gaps,
        rules: &misc_layout.column_rules,
        is_vertical: !geometry.axes_are_transposed,
    };
    let rows = AxisDecorations {
        segments: decoration_segments(&row_gaps, &columns.gaps, &row_areas, &misc_layout.row_rules),
        gaps: row_gaps,
        rules: &misc_layout.row_rules,
        is_vertical: geometry.axes_are_transposed,
    };

    let mut column_decorations = Vec::new();
    let mut row_decorations = Vec::new();
    lay_out_decorations(&mut column_decorations, &columns, &rows);
    lay_out_decorations(&mut row_decorations, &rows, &columns);

    // https://drafts.csswg.org/css-gaps-1/#propdef-rule-overlap
    let mut decorations = match misc_layout.rule_overlap {
        rule_overlap::COLUMN_OVER_ROW => {
            row_decorations.append(&mut column_decorations);
            row_decorations
        }
        _ => {
            column_decorations.append(&mut row_decorations);
            column_decorations
        }
    };

    // NB: The decorations were laid out in the frame of the grid's layout, relative to its content box.
    let content_box = paintable_geometry::absolute_rect(arena, paintable);
    let content_size = paintable_geometry::committed_content_size(arena, paintable);
    for decoration in &mut decorations {
        decoration.rect = geometry
            .frame
            .rect_to_physical(decoration.rect, content_size)
            .translated(content_box.x, content_box.y);
        decoration.is_vertical ^= geometry.frame.is_transposed();
    }
    decorations
}

/// The gaps of one axis of a grid with what their decorations are laid out from.
struct AxisDecorations<'a> {
    gaps: AxisGaps<'a>,
    segments: AxisSegments,
    rules: &'a ComputedGapRules,
    /// Whether the decorations run along the frame's vertical axis, between tracks laid out side by side.
    is_vertical: bool,
}

/// The tracks of the grid's columns and rows that an item's grid area covers, with the axis of the gaps at hand as
/// its columns.
struct ItemArea {
    columns: std::ops::Range<usize>,
    rows: std::ops::Range<usize>,
}

/// The value a gap decoration uses for one gap.
#[derive(Clone, Copy)]
struct GapRule {
    color: u32,
    line_style: u8,
    width: CssPixels,
}

impl GapRule {
    fn is_visible(self) -> bool {
        !matches!(self.line_style, line_style::NONE | line_style::HIDDEN) && self.width > CssPixels::default()
    }
}

/// The gaps between the tracks of one axis of a grid, which collapsed gutters leave out.
struct AxisGaps<'a> {
    axis: &'a GridGapAxis,
    /// The gutter of each gap.
    gutters: Vec<usize>,
    rules: Vec<GapRule>,
    /// The nearest track before each gap that is not collapsed.
    tracks_before: Vec<usize>,
    /// The junctions the gaps form with the gaps crossing them, in order, and the junction of each gap.
    junctions: Vec<Junction>,
    junction_of_gap: Vec<usize>,
}

/// A stretch of an axis where the gaps crossing a gap meet it: one gap, or several whose gutters overlap, as those of
/// a subgrid with gaps wider than the parent tracks between them do.
// https://drafts.csswg.org/css-gaps-1/#segment-endpoints
struct Junction {
    start: CssPixels,
    end: CssPixels,
    gaps: std::ops::Range<usize>,
}

impl<'a> AxisGaps<'a> {
    fn new(
        axis: &'a GridGapAxis,
        rules: &ComputedGapRules,
        color_input: &ColorResolutionInput,
        current_color: u32,
    ) -> Self {
        let gutters: Vec<usize> = (0..axis.track_count().saturating_sub(1))
            .filter(|&gutter| !axis.collapsed_gutters[gutter])
            .collect();
        let values_for_gaps = |list: &ComputedStyleValueHandle| match list.style_value() {
            Some(list) => GapValueList::new(list).values_for_gaps(gutters.len()),
            None => vec![None; gutters.len()],
        };
        let colors = values_for_gaps(&rules.color);
        let styles = values_for_gaps(&rules.style);
        let widths = values_for_gaps(&rules.width);
        let rules = colors
            .into_iter()
            .zip(styles)
            .zip(widths)
            .map(|((color, style), width)| GapRule {
                color: color
                    .and_then(|color| crate::css::table_group_builder::style_color(color, color_input))
                    .map_or(current_color, |color| color.resolve(current_color)),
                line_style: style.map_or(line_style::NONE, line_style_of),
                width: width.map_or(CssPixels::default(), |width| match width {
                    StyleValueData::Length { .. } => LengthPercentageRef::over(width).absolute_length_to_px(),
                    _ => CssPixels::default(),
                }),
            })
            .collect();
        let tracks_before = gutters
            .iter()
            .map(|&gutter| {
                (0..=gutter)
                    .rev()
                    .find(|&track| !axis.collapsed_tracks[track])
                    .unwrap_or(gutter)
            })
            .collect();
        let mut junctions: Vec<Junction> = Vec::new();
        let mut junction_of_gap = Vec::with_capacity(gutters.len());
        for (gap, &gutter) in gutters.iter().enumerate() {
            let (start, end) = axis.gutter(gutter);
            match junctions.last_mut() {
                Some(junction) if start < junction.end => {
                    junction.end = junction.end.max(end);
                    junction.gaps.end = gap + 1;
                }
                _ => junctions.push(Junction {
                    start,
                    end,
                    gaps: gap..gap + 1,
                }),
            }
            junction_of_gap.push(junctions.len() - 1);
        }
        Self {
            axis,
            gutters,
            rules,
            tracks_before,
            junctions,
            junction_of_gap,
        }
    }

    fn count(&self) -> usize {
        self.gutters.len()
    }

    /// Where a gap starts and ends, measured from the side the tracks start at.
    fn bounds(&self, gap: usize) -> (CssPixels, CssPixels) {
        self.axis.gutter(self.gutters[gap])
    }

    /// The first and last gutters of a junction.
    fn junction_gutters(&self, junction: usize) -> (usize, usize) {
        let gaps = &self.junctions[junction].gaps;
        (self.gutters[gaps.start], self.gutters[gaps.end - 1])
    }
}

fn line_style_of(value: &StyleValueData) -> u8 {
    match value {
        StyleValueData::Keyword { keyword } => {
            crate::css::css_enums::keyword_to_line_style(*keyword).unwrap_or(line_style::NONE)
        }
        _ => line_style::NONE,
    }
}

/// A gap decoration list, with its repeaters.
// https://drafts.csswg.org/css-gaps-1/#lists-repeat
struct GapValueList<'a> {
    entries: Vec<GapValueEntry<'a>>,
}

enum GapValueEntry<'a> {
    Values {
        count: usize,
        values: Vec<&'a StyleValueData>,
    },
    Auto(Vec<&'a StyleValueData>),
}

impl<'a> GapValueEntry<'a> {
    fn len(&self) -> usize {
        match self {
            Self::Values { count, values } => count.saturating_mul(values.len()),
            Self::Auto(_) => 0,
        }
    }

    fn get(&self, index: usize) -> &'a StyleValueData {
        match self {
            Self::Values { values, .. } | Self::Auto(values) => values[index % values.len()],
        }
    }
}

impl<'a> GapValueList<'a> {
    fn new(value: &'a StyleValueData) -> Self {
        let items: Vec<&StyleValueData> = match value {
            StyleValueData::ValueList { values, .. } => values.as_slice().iter().map(|value| value.data()).collect(),
            value => vec![value],
        };
        let entries = items
            .into_iter()
            .filter_map(|item| {
                let StyleValueData::Function(function_payload) = item else {
                    return Some(GapValueEntry::Values {
                        count: 1,
                        values: vec![item],
                    });
                };
                let StyleValueData::ValueList { values, .. } = function_payload.value.data() else {
                    return None;
                };
                let (count, values) = values.as_slice().split_first()?;
                let values: Vec<&StyleValueData> = values.iter().map(|value| value.data()).collect();
                if values.is_empty() {
                    return None;
                }
                match count.data() {
                    StyleValueData::Integer { value } => Some(GapValueEntry::Values {
                        count: usize::try_from(*value).ok()?,
                        values,
                    }),
                    StyleValueData::Keyword { keyword: keyword::AUTO } => Some(GapValueEntry::Auto(values)),
                    _ => None,
                }
            })
            .collect();
        Self { entries }
    }

    fn values(&self) -> impl Iterator<Item = &'a StyleValueData> + '_ {
        self.entries.iter().flat_map(|entry| match entry {
            GapValueEntry::Values { values, .. } | GapValueEntry::Auto(values) => values.iter().copied(),
        })
    }

    /// The value of each of `gap_count` gaps in order, or None for every gap if the list has no values.
    // https://drafts.csswg.org/css-gaps-1/#assigning
    fn values_for_gaps(&self, gap_count: usize) -> Vec<Option<&'a StyleValueData>> {
        let Some(auto_index) = self
            .entries
            .iter()
            .position(|entry| matches!(entry, GapValueEntry::Auto(_)))
        else {
            // If the list has no auto repeater, assign its values to the gaps in order, starting over from its first
            // value as often as needed.
            if self.entries.iter().all(|entry| entry.len() == 0) {
                return vec![None; gap_count];
            }
            return ExpandedValues::new(&self.entries)
                .cycle()
                .take(gap_count)
                .map(Some)
                .collect();
        };

        // Otherwise the leading gaps take the values before the auto repeater, the trailing gaps those after it, and
        // the gaps in between the auto repeater's values.
        let (leading, rest) = self.entries.split_at(auto_index);
        let Some((GapValueEntry::Auto(auto), trailing)) = rest.split_first() else {
            unreachable!("the entry at the auto index is the auto repeater");
        };
        let expanded_length = |entries: &[GapValueEntry<'_>]| {
            entries
                .iter()
                .fold(0usize, |length, entry| length.saturating_add(entry.len()))
        };
        let leading_gap_count = expanded_length(leading).min(gap_count);
        let first_trailing_gap = gap_count
            .saturating_sub(expanded_length(trailing))
            .max(leading_gap_count);
        ExpandedValues::new(leading)
            .take(leading_gap_count)
            .chain(
                auto.iter()
                    .copied()
                    .cycle()
                    .take(first_trailing_gap - leading_gap_count),
            )
            .chain(ExpandedValues::new(trailing).take(gap_count - first_trailing_gap))
            .map(Some)
            .collect()
    }
}

/// The values a run of gap decoration list entries expands to, in order.
#[derive(Clone)]
struct ExpandedValues<'e, 'a> {
    entries: &'e [GapValueEntry<'a>],
    entry: usize,
    position: usize,
}

impl<'e, 'a> ExpandedValues<'e, 'a> {
    fn new(entries: &'e [GapValueEntry<'a>]) -> Self {
        Self {
            entries,
            entry: 0,
            position: 0,
        }
    }
}

impl<'a> Iterator for ExpandedValues<'_, 'a> {
    type Item = &'a StyleValueData;

    fn next(&mut self) -> Option<Self::Item> {
        while let Some(entry) = self.entries.get(self.entry) {
            if self.position < entry.len() {
                self.position += 1;
                return Some(entry.get(self.position - 1));
            }
            self.entry += 1;
            self.position = 0;
        }
        None
    }
}

/// The painted gap segments of the gaps of an axis, and the pieces they paint as. Gap segments are painted as
/// rule-visibility-items asks whether or not their gap's rule is visible, as other gaps' endpoints touching them need to
/// know. Each piece is a run of consecutive gap segments of one gap, which paints as one rect, and a gap whose rule is
/// not visible has none.
struct AxisSegments {
    painted: Vec<SegmentRuns>,
    pieces: Vec<SegmentRuns>,
}

/// Runs of gap segments, which the gaps whose sweeps cover the same tracks share.
type SegmentRuns = std::rc::Rc<[std::ops::Range<usize>]>;

/// Sorts ranges and merges those that overlap or touch, which the operations below expect of their inputs.
fn merged_ranges(mut ranges: Vec<std::ops::Range<usize>>) -> Vec<std::ops::Range<usize>> {
    ranges.retain(|range| !range.is_empty());
    ranges.sort_unstable_by_key(|range| range.start);
    let mut merged: Vec<std::ops::Range<usize>> = Vec::with_capacity(ranges.len());
    for range in ranges {
        match merged.last_mut() {
            Some(last) if range.start <= last.end => last.end = last.end.max(range.end),
            _ => merged.push(range),
        }
    }
    merged
}

fn intersected_ranges(
    first: &[std::ops::Range<usize>],
    second: &[std::ops::Range<usize>],
) -> Vec<std::ops::Range<usize>> {
    let (mut result, mut i, mut j) = (Vec::new(), 0, 0);
    while i < first.len() && j < second.len() {
        let (start, end) = (first[i].start.max(second[j].start), first[i].end.min(second[j].end));
        if start < end {
            result.push(start..end);
        }
        if first[i].end < second[j].end {
            i += 1;
        } else {
            j += 1;
        }
    }
    result
}

fn subtracted_ranges(
    ranges: &[std::ops::Range<usize>],
    removed: &[std::ops::Range<usize>],
) -> Vec<std::ops::Range<usize>> {
    let mut result = Vec::new();
    let mut removed = removed.iter().peekable();
    for range in ranges {
        let mut start = range.start;
        while let Some(next) = removed.peek()
            && next.start < range.end
        {
            if next.start > start {
                result.push(start..next.start);
            }
            start = start.max(next.end);
            if next.end > range.end {
                break;
            }
            removed.next();
        }
        if start < range.end {
            result.push(start..range.end);
        }
    }
    result
}

/// A sweep along the columns of a grid that tells which tracks of its other axis a set of items covers at the column
/// it has reached. It keeps each item once, however many columns the item covers.
struct ColumnSweep {
    /// The column each item starts at and the one it ends before, with the tracks it covers, by column.
    starts: Vec<(usize, std::ops::Range<usize>)>,
    ends: Vec<(usize, std::ops::Range<usize>)>,
    next_start: usize,
    next_end: usize,
    /// The tracks of the items at the column the sweep has reached, and them merged.
    active: TrackCoverageTree,
    covered: Vec<std::ops::Range<usize>>,
    /// The number of times the covered tracks changed, which tells callers whether what they derived from them is
    /// still current.
    revision: u64,
}

impl ColumnSweep {
    fn new(track_count: usize, items: impl Iterator<Item = (std::ops::Range<usize>, std::ops::Range<usize>)>) -> Self {
        let (mut starts, mut ends) = (Vec::new(), Vec::new());
        for (columns, tracks) in items {
            let tracks = tracks.start.min(track_count)..tracks.end.min(track_count);
            if columns.is_empty() || tracks.is_empty() {
                continue;
            }
            starts.push((columns.start, tracks.clone()));
            ends.push((columns.end, tracks));
        }
        starts.sort_unstable_by_key(|(column, _)| *column);
        ends.sort_unstable_by_key(|(column, _)| *column);
        Self {
            active: TrackCoverageTree::new(if starts.is_empty() { 0 } else { track_count }),
            starts,
            ends,
            next_start: 0,
            next_end: 0,
            covered: Vec::new(),
            revision: 0,
        }
    }

    /// Moves the sweep to a column, and returns the revision of the tracks the items there cover. The columns must be
    /// asked for in increasing order.
    fn advance_to(&mut self, column: usize) -> u64 {
        let mut changed = false;
        while let Some((start, tracks)) = self.starts.get(self.next_start)
            && *start <= column
        {
            changed |= self.active.add(tracks.clone(), 1);
            self.next_start += 1;
        }
        while let Some((end, tracks)) = self.ends.get(self.next_end)
            && *end <= column
        {
            changed |= self.active.add(tracks.clone(), -1);
            self.next_end += 1;
        }
        if changed {
            self.covered.clear();
            self.active.collect_covered(&mut self.covered);
            self.revision += 1;
        }
        self.revision
    }

    /// The tracks the items at the column the sweep has reached cover, as sorted ranges.
    fn covered(&self) -> &[std::ops::Range<usize>] {
        &self.covered
    }
}

/// How many ranges cover each track of an axis, as a segment tree that also knows how many of the tracks below each
/// node are covered. Adding or removing a range takes logarithmic time, and listing the covered tracks takes time in
/// proportion to the number of ranges they merge into.
struct TrackCoverageTree {
    track_count: usize,
    /// The number of ranges that cover all tracks of each node and none of its ancestors, and the number of tracks of
    /// each node that are covered.
    counts: Vec<i32>,
    covered: Vec<usize>,
}

impl TrackCoverageTree {
    fn new(track_count: usize) -> Self {
        let node_count = 4 * track_count.max(1);
        Self {
            track_count,
            counts: vec![0; node_count],
            covered: vec![0; node_count],
        }
    }

    /// Adds `delta` ranges over `tracks`, and returns whether that changed which tracks are covered.
    fn add(&mut self, tracks: std::ops::Range<usize>, delta: i32) -> bool {
        let covered_before = self.covered[1];
        self.add_in(1, 0..self.track_count, &tracks, delta);
        // NB: A change that keeps the number of covered tracks can only move coverage around if it both covers and
        //     uncovers tracks, which a single range cannot.
        self.covered[1] != covered_before
    }

    fn add_in(&mut self, node: usize, span: std::ops::Range<usize>, tracks: &std::ops::Range<usize>, delta: i32) {
        if tracks.end <= span.start || span.end <= tracks.start {
            return;
        }
        if tracks.start <= span.start && span.end <= tracks.end {
            self.counts[node] += delta;
        } else {
            let middle = span.start + (span.end - span.start) / 2;
            self.add_in(2 * node, span.start..middle, tracks, delta);
            self.add_in(2 * node + 1, middle..span.end, tracks, delta);
        }
        self.covered[node] = if self.counts[node] > 0 {
            span.end - span.start
        } else if span.end - span.start == 1 {
            0
        } else {
            self.covered[2 * node] + self.covered[2 * node + 1]
        };
    }

    /// Appends the covered tracks as sorted, merged ranges.
    fn collect_covered(&self, output: &mut Vec<std::ops::Range<usize>>) {
        self.collect_in(1, 0..self.track_count, output);
    }

    fn collect_in(&self, node: usize, span: std::ops::Range<usize>, output: &mut Vec<std::ops::Range<usize>>) {
        if span.is_empty() || self.covered[node] == 0 {
            return;
        }
        if self.covered[node] == span.end - span.start {
            match output.last_mut() {
                Some(last) if last.end == span.start => last.end = span.end,
                _ => output.push(span),
            }
            return;
        }
        let middle = span.start + (span.end - span.start) / 2;
        self.collect_in(2 * node, span.start..middle, output);
        self.collect_in(2 * node + 1, middle..span.end, output);
    }
}

/// Splits runs of gap segments at each junction between two of their segments that `flanked` does not hold.
fn split_at_unflanked_junctions(
    runs: &[std::ops::Range<usize>],
    flanked: &[std::ops::Range<usize>],
) -> Vec<std::ops::Range<usize>> {
    let mut pieces = Vec::with_capacity(runs.len());
    for run in runs {
        let mut start = run.start;
        while start < run.end {
            // A piece continues across the junction after its last gap segment while that junction is flanked.
            let mut end = start + 1;
            while end < run.end {
                let junction = end - 1;
                match flanked.get(flanked.partition_point(|flanked| flanked.end <= junction)) {
                    Some(flanked) if flanked.start <= junction => end = run.end.min(flanked.end + 1),
                    _ => break,
                }
            }
            pieces.push(start..end);
            start = end;
        }
    }
    pieces
}

/// The pieces a gap paints, with the revisions of the sweeps they were derived from.
struct DerivedPieces {
    revisions: [Option<u64>; 5],
    rule_is_visible: bool,
    painted: SegmentRuns,
    pieces: SegmentRuns,
}

// https://drafts.csswg.org/css-gaps-1/#segments
fn decoration_segments(
    gaps: &AxisGaps<'_>,
    crossing_gaps: &AxisGaps<'_>,
    items: &[ItemArea],
    rules: &ComputedGapRules,
) -> AxisSegments {
    let row_count = crossing_gaps.axis.track_count();
    let checks_flanking = rules.rule_break == rule_break::INTERSECTION;
    let checks_occupancy = matches!(
        rules.visibility_items,
        rule_visibility_items::AROUND | rule_visibility_items::BETWEEN
    );

    // https://drafts.csswg.org/css-gaps-1/#gap-segments
    // The gap segments of a gap lie between its junctions with the crossing gaps, one along the tracks before each
    // junction and one after the last.
    let segment_count = crossing_gaps.junctions.len() + 1;
    let (junction_first_gutters, junction_last_gutters): (Vec<usize>, Vec<usize>) = (0..crossing_gaps.junctions.len())
        .map(|junction| crossing_gaps.junction_gutters(junction))
        .unzip();
    let segments_of_tracks = |tracks: &[std::ops::Range<usize>]| {
        merged_ranges(
            tracks
                .iter()
                .map(|tracks| {
                    junction_last_gutters.partition_point(|&gutter| gutter < tracks.start)
                        ..junction_last_gutters.partition_point(|&gutter| gutter < tracks.end - 1) + 1
                })
                .collect(),
        )
    };
    // A junction lies between the segment before it and the one after it, and an item spans it if it spans each of
    // its crossing gutters.
    let junctions_of_crossing_gutters = |gutters: &[std::ops::Range<usize>]| {
        merged_ranges(
            gutters
                .iter()
                .map(|gutters| {
                    junction_first_gutters.partition_point(|&gutter| gutter < gutters.start)
                        ..junction_last_gutters.partition_point(|&gutter| gutter < gutters.end)
                })
                .collect(),
        )
    };

    let rows_in_columns = || items.iter().map(|area| (area.columns.clone(), area.rows.clone()));
    // An item spans the gutter after a row if it covers both that row and the next one, and spans the gutter after a
    // column likewise.
    let spanned_row_gutters_in_columns = || {
        items
            .iter()
            .map(|area| (area.columns.clone(), area.rows.start..area.rows.end.saturating_sub(1)))
    };
    let rows_in_spanned_gutters = items.iter().map(|area| {
        (
            area.columns.start..area.columns.end.saturating_sub(1),
            area.rows.clone(),
        )
    });
    let mut spanning_rows_sweep =
        (rules.rule_break != rule_break::NONE).then(|| ColumnSweep::new(row_count, rows_in_spanned_gutters));
    let mut occupied_sweeps = checks_occupancy.then(|| {
        (
            ColumnSweep::new(row_count, rows_in_columns()),
            ColumnSweep::new(row_count, rows_in_columns()),
        )
    });
    let mut spanned_sweeps = checks_flanking.then(|| {
        (
            ColumnSweep::new(row_count.saturating_sub(1), spanned_row_gutters_in_columns()),
            ColumnSweep::new(row_count.saturating_sub(1), spanned_row_gutters_in_columns()),
        )
    });

    // NB: A gap whose sweeps cover the same tracks as those of the previous gap with a visible rule paints the same
    //     pieces, which saves deriving them again across the many gaps of a large grid.
    let mut previous: Option<DerivedPieces> = None;
    let mut painted_segments = Vec::with_capacity(gaps.count());
    let mut pieces = Vec::with_capacity(gaps.count());
    for gap in 0..gaps.count() {
        let rule_is_visible = gaps.rules[gap].is_visible();
        // https://drafts.csswg.org/css-gaps-1/#visibility
        // A gap's areas are those of the nearest tracks on either side that are not collapsed.
        let (before, after) = (gaps.tracks_before[gap], gaps.gutters[gap] + 1);
        let revisions = [
            spanning_rows_sweep
                .as_mut()
                .map(|sweep| sweep.advance_to(gaps.gutters[gap])),
            occupied_sweeps.as_mut().map(|(sweep, _)| sweep.advance_to(before)),
            occupied_sweeps.as_mut().map(|(_, sweep)| sweep.advance_to(after)),
            spanned_sweeps.as_mut().map(|(sweep, _)| sweep.advance_to(before)),
            spanned_sweeps.as_mut().map(|(_, sweep)| sweep.advance_to(after)),
        ];
        if let Some(previous) = &previous
            && previous.revisions == revisions
            && previous.rule_is_visible == rule_is_visible
        {
            painted_segments.push(previous.painted.clone());
            pieces.push(previous.pieces.clone());
            continue;
        }

        let mut painted: Vec<_> = std::iter::once(0..segment_count).collect();
        if let Some((before, after)) = occupied_sweeps.as_ref() {
            let before = segments_of_tracks(before.covered());
            let after = segments_of_tracks(after.covered());
            let visible = match rules.visibility_items {
                rule_visibility_items::AROUND => merged_ranges(before.into_iter().chain(after).collect()),
                _ => intersected_ranges(&before, &after),
            };
            painted = intersected_ranges(&painted, &visible);
        }

        // https://drafts.csswg.org/css-gaps-1/#break
        // Two segment endpoints are discontiguous if a line between them with the gap's width intersects an item,
        // which for a grid is an item whose area spans the gap.
        if let Some(spanning_rows_sweep) = spanning_rows_sweep.as_ref()
            && !painted.is_empty()
        {
            painted = subtracted_ranges(&painted, &segments_of_tracks(spanning_rows_sweep.covered()));
        }

        // A junction is flanked by spanning items if the items on either side of the gap span the crossing gap. With
        // rule-break: intersection, decorations only continue across flanked junctions.
        let painted: SegmentRuns = painted.into();
        let gap_pieces: SegmentRuns = match spanned_sweeps.as_ref() {
            _ if !rule_is_visible => SegmentRuns::default(),
            Some((before, after)) if !painted.is_empty() => {
                let flanked = intersected_ranges(
                    &junctions_of_crossing_gutters(before.covered()),
                    &junctions_of_crossing_gutters(after.covered()),
                );
                split_at_unflanked_junctions(&painted, &flanked).into()
            }
            _ => painted.clone(),
        };
        previous = Some(DerivedPieces {
            revisions,
            rule_is_visible,
            painted: painted.clone(),
            pieces: gap_pieces.clone(),
        });
        painted_segments.push(painted);
        pieces.push(gap_pieces);
    }
    AxisSegments {
        painted: painted_segments,
        pieces,
    }
}

/// Which inset applies to an endpoint of a decoration segment.
// https://drafts.csswg.org/css-gaps-1/#insets-cap-junction
#[derive(Clone, Copy)]
enum Endpoint {
    /// An endpoint at the edge of the grid, where no gap crosses.
    Edge,
    /// An endpoint at a junction with the crossing gaps, which is a junction endpoint if another painted gap segment
    /// touches the junction, and a cap endpoint otherwise.
    Junction { junction: usize, is_junction: bool },
}

fn lay_out_decorations(output: &mut Vec<GapDecoration>, axis: &AxisDecorations<'_>, crossing: &AxisDecorations<'_>) {
    let AxisDecorations {
        gaps,
        segments,
        rules,
        is_vertical,
    } = axis;
    let crossing_gaps = &crossing.gaps;
    let junctions = &crossing_gaps.junctions;
    let last_segment = junctions.len();
    let segment_start = |segment: usize| match segment {
        0 => crossing_gaps.axis.tracks_start(),
        segment => junctions[segment - 1].end,
    };
    let segment_end = |segment: usize| match segment == last_segment {
        true => crossing_gaps.axis.tracks_end(),
        false => junctions[segment].start,
    };

    // https://drafts.csswg.org/css-gaps-1/#inset
    // Percentages refer to the crossing gap width, the size of the junction, which is zero at the edges of the grid.
    // overlap-join extends a junction endpoint into the junction by half the crossing gap width and half the used width
    // of its decoration, which a rule that does not paint has too.
    let junction_rule_widths: Vec<CssPixels> = junctions
        .iter()
        .map(|junction| {
            junction
                .gaps
                .clone()
                .map(|gap| crossing_gaps.rules[gap].width)
                .max()
                .unwrap_or_default()
        })
        .collect();
    let inset = |endpoint: Endpoint, cap: &ComputedStyleValueHandle, junction_inset: &ComputedStyleValueHandle| {
        let Endpoint::Junction { junction, is_junction } = endpoint else {
            return resolved_inset(cap, CssPixels::default(), None);
        };
        let width = junctions[junction].end - junctions[junction].start;
        match is_junction {
            true => resolved_inset(junction_inset, width, Some(junction_rule_widths[junction])),
            false => resolved_inset(cap, width, None),
        }
    };

    // NB: Gaps are laid out in order, so the gap segments asked for of each crossing gap only move forward, and a
    //     cursor into its painted runs finds them without a search. The gap segments of a crossing gap lie between the
    //     junctions of this axis's gaps.
    let mut crossing_cursors = vec![0usize; crossing_gaps.count()];
    for gap in 0..gaps.count() {
        let rule = gaps.rules[gap];
        if !rule.is_visible() {
            continue;
        }
        let (gap_start, gap_end) = gaps.bounds(gap);
        let thickness_start = gaps.axis.frame_offset(gap_start, gap_end) + (gap_end - gap_start - rule.width) / 2usize;
        // NB: A gap segment that rule-visibility-items leaves unpainted stays part of its decoration segment, which paints
        //     along its other gap segments. Each painted piece of them ends at an endpoint of its own.
        let pieces = &segments.pieces[gap];
        for (index, piece) in pieces.iter().enumerate() {
            let (first, last) = (piece.start, piece.end - 1);
            let own_junction = gaps.junction_of_gap[gap];
            let mut crossing_painted_at = |junction: usize| {
                junctions[junction].gaps.clone().any(|crossing_gap| {
                    let crossing_pieces = &crossing.segments.painted[crossing_gap];
                    let cursor = &mut crossing_cursors[crossing_gap];
                    while crossing_pieces
                        .get(*cursor)
                        .is_some_and(|piece| piece.end <= own_junction)
                    {
                        *cursor += 1;
                    }
                    crossing_pieces
                        .get(*cursor)
                        .is_some_and(|piece| piece.start <= own_junction + 1)
                })
            };
            let start_endpoint = match first {
                0 => Endpoint::Edge,
                first => Endpoint::Junction {
                    junction: first - 1,
                    is_junction: index > 0 && pieces[index - 1].end == first || crossing_painted_at(first - 1),
                },
            };
            let end_endpoint = match last == last_segment {
                true => Endpoint::Edge,
                false => Endpoint::Junction {
                    junction: last,
                    is_junction: pieces.get(index + 1).is_some_and(|next| next.start == piece.end)
                        || crossing_painted_at(last),
                },
            };
            let start =
                segment_start(first) + inset(start_endpoint, &rules.inset_cap_start, &rules.inset_junction_start);
            let end = segment_end(last) - inset(end_endpoint, &rules.inset_cap_end, &rules.inset_junction_end);
            if end <= start {
                continue;
            }
            let length_start = crossing_gaps.axis.frame_offset(start, end);
            let length = end - start;
            let rect = match is_vertical {
                true => CssPixelRect::new(thickness_start, length_start, rule.width, length),
                false => CssPixelRect::new(length_start, thickness_start, length, rule.width),
            };
            output.push(GapDecoration {
                rect,
                is_vertical: *is_vertical,
                color: rule.color,
                line_style: rule.line_style,
                width: rule.width,
            });
        }
    }
}

/// An inset resolved against the width of the crossing gap, with the rule width of the crossing gap's decoration at a
/// junction endpoint, which overlap-join extends into.
fn resolved_inset(
    value: &ComputedStyleValueHandle,
    crossing_gap_width: CssPixels,
    crossing_rule_width: Option<CssPixels>,
) -> CssPixels {
    match value.style_value() {
        Some(StyleValueData::Keyword {
            keyword: keyword::OVERLAP_JOIN,
        }) => match crossing_rule_width {
            Some(rule_width) => -((crossing_gap_width + rule_width) / 2usize),
            None => CssPixels::default(),
        },
        Some(
            value @ (StyleValueData::Length { .. }
            | StyleValueData::Percentage { .. }
            | StyleValueData::Calculated { .. }),
        ) => LengthPercentageRef::over(value).to_px(crossing_gap_width),
        _ => CssPixels::default(),
    }
}
