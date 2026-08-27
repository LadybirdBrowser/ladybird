/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::paintable_geometry::absolute_border_box_rect;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::background;
use crate::painting::record::paint::border::{
    BorderDataDevicePixels, BordersDataDevicePixels, local_solid_edge_region_frames, paint_all_borders,
};
use crate::painting::visual_context::{FrameRole, PatternedEdgeOwner, PieceKey};
use libgfx_rust::Color;

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
    pub all: BordersDataDevicePixels,
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
        all,
        without_top: BordersDataDevicePixels { top: none, ..all },
        top_only: BordersDataDevicePixels {
            top: all.top,
            right: none,
            bottom: none,
            left: none,
        },
    }
}

pub(crate) fn paint_background(recorder: &mut PaintRecorder<'_>, fieldset: NodeSlotId) {
    let clip = recorder.expected_local_context(fieldset, FrameRole::FieldsetBackgroundClip);
    recorder.with_context(clip, |recorder| background::paint_background(recorder, fieldset));
}

pub(crate) fn paint_border(recorder: &mut PaintRecorder<'_>, fieldset: NodeSlotId) {
    if legend_paintable(recorder.layout_arena, fieldset).is_none() {
        super::paint_base(recorder, fieldset, crate::painting::record::PaintPhase::Border);
        return;
    }
    let Some(style) = recorder.layout_arena.node_style_if_live(fieldset) else {
        return;
    };
    let converter = recorder.converter;
    let device_border_rect = converter.rounded_device_rect(visual_border_box_rect(recorder.layout_arena, fieldset));
    let corners = recorder.border_radii(fieldset).as_corners(&converter);
    let borders = fieldset_borders_data(style, &converter);
    let solid_edge_region_frames = local_solid_edge_region_frames(
        recorder,
        fieldset,
        PatternedEdgeOwner::Border,
        PieceKey::Box,
        &borders.all,
    );
    paint_all_borders(
        &mut recorder.recorder,
        device_border_rect,
        corners,
        &borders.without_top,
        &solid_edge_region_frames,
    );

    // The top border is not expected to be painted behind the border box of the legend.
    let cutout = recorder.expected_local_context(fieldset, FrameRole::LegendCutout);
    recorder.with_context(cutout, |recorder| {
        paint_all_borders(
            &mut recorder.recorder,
            device_border_rect,
            corners,
            &borders.top_only,
            &solid_edge_region_frames,
        );
    });
}
