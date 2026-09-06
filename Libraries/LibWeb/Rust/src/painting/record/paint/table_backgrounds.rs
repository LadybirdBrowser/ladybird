/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Backgrounds of table rows, row groups, columns and column groups.

use crate::css::display::FfiDisplay;
use crate::layout::node_data::NodeSlotId;
use crate::painting::border_radii::BorderRadii;
use crate::painting::display_list::builder::PendingInlineClip;
use crate::painting::host::FfiLayerImageList;
use crate::painting::paint_order;
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::background::paint_resolved_background;
use crate::painting::record::paint::background_resolution::{BackgroundPaintInputs, resolve_background_layers};
use crate::painting::style_queries;

/// Whether a box with this display is a table part whose background is painted in the cells that originate in it:
/// a row, a row group, a column or a column group.
pub(crate) fn paints_background_in_cells(display: FfiDisplay) -> bool {
    display.is_table_row()
        || display.is_table_row_group_kind()
        || display.is_table_column()
        || display.is_table_column_group()
}

/// Paints the background of a table row, row group, column or column group.
///
/// CSS 2.2 §17.5.1 stacks these between the table and the cells, and for each of them "the background covers exactly
/// the full area of all cells that originate in the [row, column or group], even if they span outside [it], but this
/// difference in area does not affect background image positioning".
/// https://www.w3.org/TR/CSS22/tables.html#table-layers
/// In the separated borders model the spacing between cells is not covered: "In this space, the row, column, row
/// group, and column group backgrounds are invisible, allowing the table background to show through."
/// https://www.w3.org/TR/CSS22/tables.html#separated-borders
/// So the background layers are resolved against the part's own box and painted once per originating cell, clipped
/// to that cell's border box.
pub(crate) fn paint_table_part_background(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    let layout_arena = recorder.layout_arena;
    let Some(style) = layout_arena.node_style_if_live(paintable) else {
        return;
    };
    let part_rect = paintable_geometry::absolute_border_box_rect(layout_arena, paintable);
    let resolved = resolve_background_layers(
        recorder,
        paintable,
        style,
        FfiLayerImageList::Background,
        libgfx_rust::Color(style.background().background_color),
        style.background().background_color_clip,
        part_rect,
        BorderRadii::default(),
    );
    let inputs = BackgroundPaintInputs {
        resolved,
        border_radii: BorderRadii::default(),
        image_rendering: style.image_rendering(),
        is_root_element: false,
    };
    for cell in originating_cells(layout_arena, paintable) {
        let cell_rect = paintable_geometry::absolute_border_box_rect(layout_arena, cell);
        let clip_rect = recorder.converter.rounded_device_rect(cell_rect).to_float();
        recorder.record_with_inline_clips(&[PendingInlineClip::intersecting_float_rect(clip_rect)], |recorder| {
            paint_resolved_background(recorder, paintable, &inputs);
        });
    }
}

/// The cells that originate in a table row, row group, column or column group, in paint order.
fn originating_cells(arena: &impl PaintableRowsRead, part: NodeSlotId) -> Vec<NodeSlotId> {
    let mut cells = Vec::new();
    let display = style_queries::display(arena, part);
    if display.is_table_row() {
        push_cells_of_row(arena, part, &mut cells);
    } else if display.is_table_row_group_kind() {
        paint_order::for_each_paint_child(arena, part, |row| {
            if style_queries::display(arena, row).is_table_row() {
                push_cells_of_row(arena, row, &mut cells);
            }
        });
    } else {
        // A cell originates in a column if the first grid column it occupies belongs to the column box.
        let (start, span) = paintable_geometry::committed_table_column_range(arena, part);
        let Some(table) = table_box_of(arena, part) else {
            return cells;
        };
        for_each_row_of_table(arena, table, |row| {
            paint_order::for_each_paint_child(arena, row, |cell| {
                if !style_queries::display(arena, cell).is_table_cell() {
                    return;
                }
                let (column, _) = paintable_geometry::committed_table_column_range(arena, cell);
                if column >= start && column < start + span {
                    cells.push(cell);
                }
            });
        });
    }
    cells
}

fn push_cells_of_row(arena: &impl PaintableRowsRead, row: NodeSlotId, cells: &mut Vec<NodeSlotId>) {
    paint_order::for_each_paint_child(arena, row, |cell| {
        if style_queries::display(arena, cell).is_table_cell() {
            cells.push(cell);
        }
    });
}

/// The rows of a table box: the children of its row groups and its own child rows, in tree order.
fn for_each_row_of_table(arena: &impl PaintableRowsRead, table: NodeSlotId, mut callback: impl FnMut(NodeSlotId)) {
    paint_order::for_each_paint_child(arena, table, |child| {
        let display = style_queries::display(arena, child);
        if display.is_table_row() {
            callback(child);
        } else if display.is_table_row_group_kind() {
            paint_order::for_each_paint_child(arena, child, |row| {
                if style_queries::display(arena, row).is_table_row() {
                    callback(row);
                }
            });
        }
    });
}

/// The table box that a column or column group box belongs to.
fn table_box_of(arena: &impl PaintableRowsRead, part: NodeSlotId) -> Option<NodeSlotId> {
    let mut ancestor = paint_order::paint_parent(arena, part);
    while let Some(node) = ancestor {
        if style_queries::display(arena, node).is_table_inside() {
            return Some(node);
        }
        ancestor = paint_order::paint_parent(arena, node);
    }
    None
}
