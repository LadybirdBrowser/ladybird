/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::paint_read::PaintRead;
use crate::painting::record::trace::Observer;

use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::{direction, line_style};
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::NodeSlotId;
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::paintable_geometry;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::border::{BordersDataDevicePixels, border_side, paint_all_borders};
use libgfx_rust::CornerRadii;

pub(crate) fn has_visible_column_rule(style: ComputedValuesView<'_>) -> bool {
    !matches!(style.column_rule_style(), line_style::NONE | line_style::HIDDEN)
        && style.column_rule_width() > CssPixels::default()
        && style.establishes_multi_column_container()
}

// https://drafts.csswg.org/css-multicol-1/#column-gaps-and-rules
pub(crate) fn paint_column_rules<O: Observer>(recorder: &mut PaintRecorder<'_, O>, paintable: NodeSlotId) {
    let Some(style) = recorder.source.node_style_if_live(paintable) else {
        return;
    };
    let Some(data) = paintable_geometry::committed_multicol_layout_data(recorder.source, paintable) else {
        return;
    };
    let content_box = paintable_geometry::absolute_rect(recorder.source, paintable);
    let rule_width = style.column_rule_width();
    let is_right_to_left = style.direction() == direction::RTL;
    let converter = recorder.converter;
    let borders_data = BordersDataDevicePixels {
        left: border_side(
            &converter,
            true,
            style.column_rule_color(),
            match style.column_rule_style() {
                line_style::INSET => line_style::RIDGE,
                line_style::OUTSET => line_style::GROOVE,
                rule_style => rule_style,
            },
            rule_width,
        ),
        force_dark_role: ForceDarkRole::Border,
        ..BordersDataDevicePixels::default()
    };
    for row in &data.rows {
        for gap_index in 0..row.column_count.saturating_sub(1) {
            let gap_center = data.geometry.gap_center(gap_index, content_box.width, is_right_to_left);
            let rule_rect = CssPixelRect::new(
                content_box.x + gap_center - rule_width / 2usize,
                content_box.y + row.block_offset,
                rule_width,
                row.block_size,
            );
            paint_all_borders(
                &mut recorder.recorder,
                converter.rounded_device_rect(rule_rect),
                CornerRadii::default(),
                &borders_data,
            );
        }
    }
}
