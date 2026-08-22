/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::line_style;
use crate::layout::CollapsedBorderEdge;
use crate::layout::node_data::NodeSlotId;
use crate::painting::record::PaintRecorder;
use libgfx_rust::{Color, IntPoint, IntRect, LineStyle};

#[derive(Clone, Copy, Default)]
struct DeviceBorderData {
    color: Color,
    line_style: u8,
    width: i32,
}

#[derive(Clone, Copy, Default)]
struct DeviceEdge {
    data: DeviceBorderData,
    source_order: u32,
}

impl DeviceEdge {
    fn paints(self) -> bool {
        self.data.width > 0 && self.data.line_style != line_style::NONE && self.data.line_style != line_style::HIDDEN
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

fn beats_at_joint(a: DeviceEdge, b: DeviceEdge) -> bool {
    if a.paints() != b.paints() {
        return a.paints();
    }
    if !a.paints() {
        return false;
    }
    if a.data.width != b.data.width {
        return a.data.width > b.data.width;
    }
    let a_score = edge_style_score(a.data.line_style);
    let b_score = edge_style_score(b.data.line_style);
    if a_score != b_score {
        return a_score > b_score;
    }
    a.source_order < b.source_order
}

#[derive(Clone, Copy, Default)]
struct JointOutcome {
    survives: bool,
    crossing_size: i32,
}

fn resolve_joint(
    self_edge: DeviceEdge,
    collinear: DeviceEdge,
    perpendicular_a: DeviceEdge,
    perpendicular_b: DeviceEdge,
) -> JointOutcome {
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
        crossing_size: if crossing.paints() { crossing.data.width } else { 0 },
    }
}

fn joint_start_coordinate(line: i32, joint: JointOutcome) -> i32 {
    let half = joint.crossing_size / 2;
    if joint.survives {
        line - half
    } else {
        line + (joint.crossing_size - half)
    }
}

fn joint_end_coordinate(line: i32, joint: JointOutcome) -> i32 {
    let half = joint.crossing_size / 2;
    if joint.survives {
        line + (joint.crossing_size - half)
    } else {
        line - half
    }
}

fn paint_edge(recorder: &mut PaintRecorder<'_>, rect: IntRect, edge: DeviceEdge, direction: EdgeDirection) {
    let line_style = match edge.data.line_style {
        line_style::DOTTED => Some(LineStyle::Dotted),
        line_style::DASHED => Some(LineStyle::Dashed),
        _ => None,
    };
    if let Some(line_style) = line_style {
        let from = IntPoint { x: rect.x, y: rect.y };
        let to = match direction {
            EdgeDirection::Horizontal => IntPoint {
                x: rect.right(),
                y: rect.y,
            },
            EdgeDirection::Vertical => IntPoint {
                x: rect.x,
                y: rect.bottom(),
            },
        };
        recorder.recorder.draw_line(
            from,
            to,
            edge.data.color,
            edge.data.width,
            line_style,
            Color::TRANSPARENT,
        );
        return;
    }
    // FIXME: Support the remaining line styles instead of rendering them as solid.
    recorder.recorder.fill_rect(rect, edge.data.color);
}

fn to_device_edge(recorder: &PaintRecorder<'_>, edge: CollapsedBorderEdge) -> DeviceEdge {
    DeviceEdge {
        data: DeviceBorderData {
            color: Color(edge.border_data.color),
            line_style: edge.border_data.line_style,
            width: recorder.converter.rounded_device_pixels(edge.border_data.width),
        },
        source_order: edge.source_order,
    }
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

    let origin = crate::painting::paintable_geometry::absolute_rect(recorder.layout_arena, table_paintable).location();
    let xs: Vec<i32> = borders
        .column_offsets
        .iter()
        .map(|offset| recorder.converter.rounded_device_pixels(origin.x + *offset))
        .collect();
    let ys: Vec<i32> = borders
        .row_offsets
        .iter()
        .map(|offset| recorder.converter.rounded_device_pixels(origin.y + *offset))
        .collect();
    let horizontal_edges: Vec<DeviceEdge> = borders
        .horizontal_edges
        .iter()
        .map(|edge| to_device_edge(recorder, *edge))
        .collect();
    let vertical_edges: Vec<DeviceEdge> = borders
        .vertical_edges
        .iter()
        .map(|edge| to_device_edge(recorder, *edge))
        .collect();
    let no_edge = DeviceEdge::default();
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
            let x0 = joint_start_coordinate(xs[j], start_joint);
            let x1 = joint_end_coordinate(xs[j + 1], end_joint);
            if x1 <= x0 {
                continue;
            }
            let rect = IntRect::new(x0, y - self_edge.data.width / 2, x1 - x0, self_edge.data.width);
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
            let y0 = joint_start_coordinate(ys[i], start_joint);
            let y1 = joint_end_coordinate(ys[i + 1], end_joint);
            if y1 <= y0 {
                continue;
            }
            let rect = IntRect::new(x - self_edge.data.width / 2, y0, self_edge.data.width, y1 - y0);
            paint_edge(recorder, rect, self_edge, EdgeDirection::Vertical);
        }
    }
}
