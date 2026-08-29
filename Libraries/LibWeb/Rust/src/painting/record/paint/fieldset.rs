/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::display_list::builder::PendingInlineClip;
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::paintable_geometry::absolute_border_box_rect;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::background;
use crate::painting::record::paint::border::{BorderDataDevicePixels, BordersDataDevicePixels, paint_all_borders};
use libgfx_rust::{Color, IntRect};

pub(crate) fn legend_paintable(arena: &impl PaintableRowsRead, fieldset: NodeSlotId) -> Option<NodeSlotId> {
    let mut child = arena.node_first_child_if_live(fieldset);
    while let Some(node) = child {
        if arena.node_kind_if_live(node) == Some(NodeKind::LegendBox) && !arena.node_is_out_of_flow_if_live(node) {
            return arena.paintable_row_is_populated(node).then_some(node);
        }
        child = arena.node_next_sibling_if_live(node);
    }
    None
}

fn css_border_top_width(arena: &impl PaintableRowsRead, fieldset: NodeSlotId) -> CssPixels {
    arena
        .node_style_if_live(fieldset)
        .map(|style| style.border_top_width())
        .unwrap_or_default()
}

fn effective_border_top(arena: &impl PaintableRowsRead, fieldset: NodeSlotId) -> CssPixels {
    let css_border_top = css_border_top_width(arena, fieldset);
    if let Some(legend) = legend_paintable(arena, fieldset) {
        let legend_margin = crate::painting::paintable_geometry::committed_margin(arena, legend);
        let legend_margin_box_height =
            legend_margin.top + absolute_border_box_rect(arena, legend).height + legend_margin.bottom;
        return css_border_top.max(legend_margin_box_height);
    }
    css_border_top
}

pub(crate) fn visual_border_box_rect(arena: &impl PaintableRowsRead, fieldset: NodeSlotId) -> CssPixelRect {
    let css_border_top = css_border_top_width(arena, fieldset);
    let allocated_border_top = effective_border_top(arena, fieldset);
    let mut rect = absolute_border_box_rect(arena, fieldset);
    if allocated_border_top <= css_border_top {
        return rect;
    }
    let taken = ((allocated_border_top - css_border_top) / 2usize).min(rect.height);
    rect.y += taken;
    rect.height -= taken;
    rect
}

pub(crate) struct FieldsetBordersData {
    pub without_top: BordersDataDevicePixels,
    pub top_only: BordersDataDevicePixels,
}

pub(crate) fn fieldset_borders_data(
    style: ComputedValuesView<'_>,
    converter: &DevicePixelConverter,
) -> FieldsetBordersData {
    let side = |color: u32, line_style: u8, width: CssPixels| BorderDataDevicePixels {
        color: Color(color),
        line_style,
        width: converter.enclosing_device_pixels(width),
    };
    let none = BorderDataDevicePixels::default();
    let all = BordersDataDevicePixels {
        force_dark_role: ForceDarkRole::Border,
        top: side(
            style.border_top_color(),
            style.border_top_style(),
            style.border_top_width(),
        ),
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
    FieldsetBordersData {
        without_top: BordersDataDevicePixels { top: none, ..all },
        top_only: BordersDataDevicePixels {
            right: none,
            bottom: none,
            left: none,
            ..all
        },
    }
}

pub(crate) fn paint_background(recorder: &mut PaintRecorder<'_>, fieldset: NodeSlotId) {
    let device_border_rect = recorder
        .converter
        .rounded_device_rect(visual_border_box_rect(recorder.layout_arena, fieldset));
    let visual_border_box_clip = PendingInlineClip::intersecting_float_rect(device_border_rect.to_float());
    recorder.record_with_inline_clips(&[visual_border_box_clip], |recorder| {
        background::paint_background(recorder, fieldset);
    });
}

pub(crate) fn paint_border(recorder: &mut PaintRecorder<'_>, fieldset: NodeSlotId) {
    let Some(legend) = legend_paintable(recorder.layout_arena, fieldset) else {
        super::paint_base(recorder, fieldset, crate::painting::record::PaintPhase::Border);
        return;
    };
    let Some(style) = recorder.layout_arena.node_style_if_live(fieldset) else {
        return;
    };
    let converter = recorder.converter;
    let device_border_rect = converter.rounded_device_rect(visual_border_box_rect(recorder.layout_arena, fieldset));
    let corners = recorder.border_radii(fieldset).as_corners(&converter);
    let borders = fieldset_borders_data(style, &converter);
    paint_all_borders(
        &mut recorder.recorder,
        device_border_rect,
        corners,
        &borders.without_top,
    );

    // The top border is not expected to be painted behind the border box of the legend.
    let top_border = converter.enclosing_device_pixels(css_border_top_width(recorder.layout_arena, fieldset));
    let top_border_band = IntRect::new(
        device_border_rect.x,
        device_border_rect.y,
        device_border_rect.width,
        top_border,
    );
    let legend_border_rect = converter.rounded_device_rect(absolute_border_box_rect(recorder.layout_arena, legend));
    let legend_cutout = IntRect::new(
        legend_border_rect.x,
        device_border_rect.y,
        legend_border_rect.width,
        top_border,
    );
    let top_border_inline_clips = [
        PendingInlineClip::intersecting_float_rect(top_border_band.to_float()),
        PendingInlineClip::subtracting_rect(legend_cutout.to_float()),
    ];
    recorder.record_with_inline_clips(&top_border_inline_clips, |recorder| {
        paint_all_borders(&mut recorder.recorder, device_border_rect, corners, &borders.top_only);
    });
}
