/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::line_style;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::layout::table_formatting_context;
use crate::layout::used_values::{collapsed_border_part_after_line, collapsed_border_part_before_line};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::record::PaintRecorder;
use libgfx_rust::{Color, IntPoint, IntRect, LineStyle};
use table_formatting_context::CollapsedBorderEdge;

#[derive(Clone, Copy, Default)]
struct Edge {
    color: Color,
    line_style: u8,
    width: CssPixels,
    source_order: u32,
}

impl Edge {
    fn paints(self) -> bool {
        self.width > CssPixels::default() && self.line_style != line_style::NONE && self.line_style != line_style::HIDDEN
    }
}

impl From<CollapsedBorderEdge> for Edge {
    fn from(edge: CollapsedBorderEdge) -> Self {
        Self {
            color: Color(edge.border_data.color),
            line_style: edge.border_data.line_style,
            width: edge.border_data.width,
            source_order: edge.source_order,
        }
    }
}

#[derive(Clone, Copy)]
enum EdgeDirection {
    Horizontal,
    Vertical,
}

fn edge_style_score(style: u8) -> i32 {
    match style {
        line_style::INSET => 1,
        line_style::GROOVE => 2,
        line_style::OUTSET => 3,
        line_style::RIDGE => 4,
        line_style::DOTTED => 5,
        line_style::DASHED => 6,
        line_style::SOLID => 7,
        line_style::DOUBLE => 8,
        _ => 0,
    }
}

fn beats_at_joint(a: Edge, b: Edge) -> bool {
    if a.paints() != b.paints() {
        return a.paints();
    }
    if !a.paints() {
        return false;
    }
    if a.width != b.width {
        return a.width > b.width;
    }
    let a_score = edge_style_score(a.line_style);
    let b_score = edge_style_score(b.line_style);
    if a_score != b_score {
        return a_score > b_score;
    }
    a.source_order < b.source_order
}

/// How an edge fares at one of its joints: whether it is painted across the joint, and the width of the border
/// crossing the joint that it either covers or stops short of.
#[derive(Clone, Copy, Default)]
struct JointOutcome {
    survives: bool,
    crossing_width: CssPixels,
}

fn resolve_joint(self_edge: Edge, collinear: Edge, perpendicular_a: Edge, perpendicular_b: Edge) -> JointOutcome {
    let survives = !beats_at_joint(collinear, self_edge)
        && !beats_at_joint(perpendicular_a, self_edge)
        && !beats_at_joint(perpendicular_b, self_edge);
    let crossing = if beats_at_joint(perpendicular_b, perpendicular_a) {
        perpendicular_b
    } else {
        perpendicular_a
    };
    JointOutcome {
        survives,
        crossing_width: if crossing.paints() {
            crossing.width
        } else {
            CssPixels::default()
        },
    }
}

// The crossing border is centered on the grid line at `line` and split like the boxes around it split it (see
// collapsed_border_part_after_line): an edge that survives the joint runs across the whole crossing border, an edge
// that loses stops where the crossing border begins.
fn joint_start_coordinate(line: CssPixels, joint: JointOutcome) -> CssPixels {
    if joint.survives {
        line - collapsed_border_part_before_line(joint.crossing_width)
    } else {
        line + collapsed_border_part_after_line(joint.crossing_width)
    }
}

fn joint_end_coordinate(line: CssPixels, joint: JointOutcome) -> CssPixels {
    if joint.survives {
        line + collapsed_border_part_after_line(joint.crossing_width)
    } else {
        line - collapsed_border_part_before_line(joint.crossing_width)
    }
}

/// The device pixels covered by the CSS pixel rectangle with the given edges. Each edge is snapped to the nearest
/// device pixel on its own, like the edges of the boxes around the border, so that the painted border meets the cell
/// backgrounds and the borders it joins without gaps or overlaps even on a fractional grid line. None if empty.
fn device_rect(
    recorder: &PaintRecorder<'_>,
    left: CssPixels,
    top: CssPixels,
    right: CssPixels,
    bottom: CssPixels,
) -> Option<IntRect> {
    let converter = recorder.converter;
    let x = converter.rounded_device_pixels(left);
    let y = converter.rounded_device_pixels(top);
    let width = converter.rounded_device_pixels(right) - x;
    let height = converter.rounded_device_pixels(bottom) - y;
    (width > 0 && height > 0).then_some(IntRect { x, y, width, height })
}

fn paint_edge(recorder: &mut PaintRecorder<'_>, rect: IntRect, edge: Edge, direction: EdgeDirection) {
    let line_style = match edge.line_style {
        line_style::DOTTED => Some(LineStyle::Dotted),
        line_style::DASHED => Some(LineStyle::Dashed),
        _ => None,
    };
    if let Some(line_style) = line_style {
        let from = IntPoint { x: rect.x, y: rect.y };
        let (to, thickness) = match direction {
            EdgeDirection::Horizontal => (
                IntPoint {
                    x: rect.right(),
                    y: rect.y,
                },
                rect.height,
            ),
            EdgeDirection::Vertical => (
                IntPoint {
                    x: rect.x,
                    y: rect.bottom(),
                },
                rect.width,
            ),
        };
        recorder.recorder.draw_line(
            from,
            to,
            edge.color,
            thickness,
            line_style,
            Color::TRANSPARENT,
            ForceDarkRole::Border,
        );
        return;
    }
    // FIXME: Support the remaining line styles instead of rendering them as solid.
    recorder.recorder.fill_rect(rect, edge.color, ForceDarkRole::Border);
}

pub(crate) fn paint_table_borders(recorder: &mut PaintRecorder<'_>, table_paintable: NodeSlotId) {
    // Painting according to the collapsing border model:
    // https://www.w3.org/TR/CSS22/tables.html#collapsing-borders
    let Some(borders) =
        crate::painting::paintable_geometry::committed_collapsed_table_borders(recorder.layout_arena, table_paintable)
    else {
        return;
    };
    let Some(rows) = borders.row_offsets.len().checked_sub(1) else {
        return;
    };
    let Some(columns) = borders.column_offsets.len().checked_sub(1) else {
        return;
    };
    if rows == 0 || columns == 0 {
        return;
    }
    debug_assert_eq!(borders.horizontal_edges.len(), (rows + 1) * columns);
    debug_assert_eq!(borders.vertical_edges.len(), (columns + 1) * rows);

    // The grid lines, in CSS pixels: each border is centered on its grid line and its rectangle is only snapped to
    // device pixels once its extent is known (see device_rect).
    let origin = crate::painting::paintable_geometry::absolute_rect(recorder.layout_arena, table_paintable).location();
    let xs: Vec<CssPixels> = borders
        .column_offsets
        .iter()
        .map(|offset| origin.x + *offset)
        .collect();
    let ys: Vec<CssPixels> = borders.row_offsets.iter().map(|offset| origin.y + *offset).collect();
    let horizontal_edges: Vec<Edge> = borders.horizontal_edges.iter().map(|edge| Edge::from(*edge)).collect();
    let vertical_edges: Vec<Edge> = borders.vertical_edges.iter().map(|edge| Edge::from(*edge)).collect();
    let no_edge = Edge::default();
    let horizontal = |line: usize, column: usize| horizontal_edges[line * columns + column];
    let vertical = |line: usize, row: usize| vertical_edges[line * rows + row];

    for (i, &y) in ys.iter().enumerate().take(rows + 1) {
        for j in 0..columns {
            let self_edge = horizontal(i, j);
            if !self_edge.paints() {
                continue;
            }
            let start_joint = resolve_joint(
                self_edge,
                if j > 0 { horizontal(i, j - 1) } else { no_edge },
                if i > 0 { vertical(j, i - 1) } else { no_edge },
                if i < rows { vertical(j, i) } else { no_edge },
            );
            let end_joint = resolve_joint(
                self_edge,
                if j + 1 < columns { horizontal(i, j + 1) } else { no_edge },
                if i > 0 { vertical(j + 1, i - 1) } else { no_edge },
                if i < rows { vertical(j + 1, i) } else { no_edge },
            );
            let Some(rect) = device_rect(
                recorder,
                joint_start_coordinate(xs[j], start_joint),
                y - collapsed_border_part_before_line(self_edge.width),
                joint_end_coordinate(xs[j + 1], end_joint),
                y + collapsed_border_part_after_line(self_edge.width),
            ) else {
                continue;
            };
            paint_edge(recorder, rect, self_edge, EdgeDirection::Horizontal);
        }
    }

    for (j, &x) in xs.iter().enumerate().take(columns + 1) {
        for i in 0..rows {
            let self_edge = vertical(j, i);
            if !self_edge.paints() {
                continue;
            }
            let start_joint = resolve_joint(
                self_edge,
                if i > 0 { vertical(j, i - 1) } else { no_edge },
                if j > 0 { horizontal(i, j - 1) } else { no_edge },
                if j < columns { horizontal(i, j) } else { no_edge },
            );
            let end_joint = resolve_joint(
                self_edge,
                if i + 1 < rows { vertical(j, i + 1) } else { no_edge },
                if j > 0 { horizontal(i + 1, j - 1) } else { no_edge },
                if j < columns { horizontal(i + 1, j) } else { no_edge },
            );
            let Some(rect) = device_rect(
                recorder,
                x - collapsed_border_part_before_line(self_edge.width),
                joint_start_coordinate(ys[i], start_joint),
                x + collapsed_border_part_after_line(self_edge.width),
                joint_end_coordinate(ys[i + 1], end_joint),
            ) else {
                continue;
            };
            paint_edge(recorder, rect, self_edge, EdgeDirection::Vertical);
        }
    }
}
