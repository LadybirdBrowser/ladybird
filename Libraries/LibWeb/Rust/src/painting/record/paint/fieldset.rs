/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::paintable_data::PaintableSlotId;
use crate::painting::paintable_geometry::absolute_border_box_rect;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::background;
use crate::painting::record::paint::border::{BorderDataDevicePixels, BordersDataDevicePixels, paint_all_borders};
use libgfx_rust::{Color, IntRect};

fn rendered_legend(recorder: &PaintRecorder<'_>, fieldset: PaintableSlotId) -> Option<NodeSlotId> {
    let arena = recorder.layout_arena;
    let mut child = arena.node_first_child_if_live(recorder.data(fieldset).layout_node);
    while let Some(node) = child {
        if arena.node_kind_if_live(node) == Some(NodeKind::LegendBox) && !arena.node_is_out_of_flow_if_live(node) {
            return Some(node);
        }
        child = arena.node_next_sibling_if_live(node);
    }
    None
}

fn legend_paintable(recorder: &PaintRecorder<'_>, fieldset: PaintableSlotId) -> Option<PaintableSlotId> {
    let legend = rendered_legend(recorder, fieldset)?;
    let paintable = recorder.paintables.paintable_of_node(legend);
    (!paintable.is_invalid()).then_some(paintable)
}

fn css_border_top_width(recorder: &PaintRecorder<'_>, fieldset: PaintableSlotId) -> CssPixels {
    recorder
        .layout_arena
        .node_style_if_live(recorder.data(fieldset).layout_node)
        .map(|style| style.border_top_width())
        .unwrap_or_default()
}

fn effective_border_top(recorder: &PaintRecorder<'_>, fieldset: PaintableSlotId) -> CssPixels {
    let css_border_top = css_border_top_width(recorder, fieldset);
    if let Some(legend) = legend_paintable(recorder, fieldset) {
        let legend_data = recorder.data(legend);
        let legend_margin_box_height = legend_data.margin.top
            + absolute_border_box_rect(recorder.paintables, legend).height
            + legend_data.margin.bottom;
        return css_border_top.max(legend_margin_box_height);
    }
    css_border_top
}

fn visual_border_box_rect(recorder: &PaintRecorder<'_>, fieldset: PaintableSlotId) -> CssPixelRect {
    let css_border_top = css_border_top_width(recorder, fieldset);
    let allocated_border_top = effective_border_top(recorder, fieldset);
    let mut rect = absolute_border_box_rect(recorder.paintables, fieldset);
    if allocated_border_top <= css_border_top {
        return rect;
    }
    let taken = ((allocated_border_top - css_border_top) / 2usize).min(rect.height);
    rect.y += taken;
    rect.height -= taken;
    rect
}

pub(crate) fn paint_background(recorder: &mut PaintRecorder<'_>, fieldset: PaintableSlotId) {
    recorder.recorder.save();
    let clip = recorder
        .converter
        .rounded_device_rect(visual_border_box_rect(recorder, fieldset));
    recorder.recorder.add_clip_rect_int(clip);
    background::paint_background(recorder, fieldset);
    recorder.recorder.restore();
}

pub(crate) fn paint_border(recorder: &mut PaintRecorder<'_>, fieldset: PaintableSlotId) {
    let Some(legend) = legend_paintable(recorder, fieldset) else {
        super::paint_base(recorder, fieldset, crate::painting::record::PaintPhase::Border);
        return;
    };
    let Some(style) = recorder
        .layout_arena
        .node_style_if_live(recorder.data(fieldset).layout_node)
    else {
        return;
    };
    let converter = recorder.converter;
    let legend_border_rect = converter.rounded_device_rect(absolute_border_box_rect(recorder.paintables, legend));

    let side = |color: u32, line_style: u8, width: CssPixels| BorderDataDevicePixels {
        color: Color(color),
        line_style,
        width: converter.enclosing_device_pixels(width),
    };
    let none = BorderDataDevicePixels::default();
    let top_border_data = side(
        style.border_top_color(),
        style.border_top_style(),
        style.border_top_width(),
    );
    let top_border = top_border_data.width;

    let device_border_rect = converter.rounded_device_rect(visual_border_box_rect(recorder, fieldset));
    let corners = recorder.border_radii(fieldset).as_corners(&converter);

    let borders_data = BordersDataDevicePixels {
        top: none,
        right: side(
            style.border_right_color(),
            style.border_right_style(),
            style.border_right_width(),
        ),
        bottom: side(
            style.border_bottom_color(),
            style.border_bottom_style(),
            style.border_bottom_width(),
        ),
        left: side(
            style.border_left_color(),
            style.border_left_style(),
            style.border_left_width(),
        ),
    };
    paint_all_borders(&mut recorder.recorder, device_border_rect, corners, &borders_data);

    // The top border is not expected to be painted behind the border box of the legend.
    let top_border_only = BordersDataDevicePixels {
        top: top_border_data,
        right: none,
        bottom: none,
        left: none,
    };
    let paint_clipped = |recorder: &mut PaintRecorder<'_>, clip: IntRect| {
        recorder.recorder.save();
        recorder.recorder.add_clip_rect_int(clip);
        paint_all_borders(&mut recorder.recorder, device_border_rect, corners, &top_border_only);
        recorder.recorder.restore();
    };
    let left_segment = IntRect::new(
        device_border_rect.x,
        device_border_rect.y,
        legend_border_rect.x - device_border_rect.x,
        top_border,
    );
    paint_clipped(recorder, left_segment);
    let right_segment = IntRect::new(
        legend_border_rect.right(),
        device_border_rect.y,
        device_border_rect.right() - legend_border_rect.right(),
        top_border,
    );
    paint_clipped(recorder, right_segment);
}
