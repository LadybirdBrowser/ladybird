/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::painting::chrome_geometry::{
    ChromeGeometry, is_chrome_mirrored, scrollbar_colors_for_paint, scrollbar_is_enlarged,
};
use crate::painting::display_list::commands::VisualContextIndex;
use crate::painting::display_list::recorder::{FillPathParams, PaintStyleOrColor};
use crate::painting::ffi::ScrollDirection;
use crate::painting::paintable_data::PaintableKind;
use crate::painting::record::PaintRecorder;
use libgfx_rust::{Color, FloatPoint, IntRect, LineStyle, ShouldAntiAlias, WindingRule};

pub(crate) fn paint_overlay(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    let kind = recorder.data(paintable).kind;
    let own_scroll_node_index = recorder.data(paintable).own_scroll_node_index;
    if kind != PaintableKind::ViewportPaintable
        && own_scroll_node_index == 0
        && !recorder.hit_test_facts(paintable).has_resizer
    {
        return;
    }
    let converter = recorder.converter;
    let chrome_geometry = ChromeGeometry::for_recording(recorder.layout_arena, &recorder.inputs);
    let style = recorder.layout_arena.node_style_if_live(paintable);
    let paints_scrollbars = style.is_some_and(|style| {
        ((recorder.inputs.paint_viewport_scrollbars && !recorder.inputs.async_scrolling_enabled)
            || kind != PaintableKind::ViewportPaintable)
            && style.misc_reset().scrollbar_width != crate::css::css_enums::scrollbar_width::NONE
    });

    if paints_scrollbars {
        let (thumb_color, track_color) = scrollbar_colors_for_paint(
            recorder.layout_arena,
            paintable,
            recorder.inputs.root_background_source,
            recorder.inputs.canvas_color.blend(recorder.inputs.background_color),
        );
        let scroll_node_index = VisualContextIndex(own_scroll_node_index);
        for direction in [ScrollDirection::Vertical, ScrollDirection::Horizontal] {
            let enlarged = scrollbar_is_enlarged(recorder.layout_arena, paintable, direction);
            let Some(scrollbar) = chrome_geometry.compute_scrollbar_data(paintable, direction, enlarged, None) else {
                continue;
            };
            recorder.recorder.paint_scrollbar(
                scroll_node_index,
                converter.rounded_device_rect(scrollbar.gutter_rect),
                converter.rounded_device_rect(scrollbar.thumb_rect),
                converter.rounded_device_rect(scrollbar.track_rect),
                scrollbar.thumb_travel_to_scroll_ratio.to_double(),
                thumb_color,
                track_color,
                direction == ScrollDirection::Vertical,
            );
        }
    }

    if let Some(mut css_rect) = chrome_geometry.absolute_resizer_rect(paintable) {
        let bottom_left_resizer = is_chrome_mirrored(recorder.layout_arena, paintable);
        let padding = recorder.inputs.chrome_metrics.resize_gripper_padding;
        let two = CssPixels::from_integer(2);
        let half = padding.div_as_fraction(two);
        let negative_half = (-padding).div_as_fraction(two);
        css_rect.x += half;
        css_rect.width -= padding;
        css_rect.y += half;
        css_rect.height -= padding;
        css_rect = css_rect.translated(if bottom_left_resizer { half } else { negative_half }, negative_half);
        let rect = converter.rounded_device_rect(css_rect);
        let dark = Color::from_rgba(0, 0, 0, 100);
        let light = Color::from_rgba(255, 255, 255, 100);
        let mut paint_resizer_line = |step: i32, color: Color| {
            let from = libgfx_rust::IntPoint {
                x: if bottom_left_resizer {
                    rect.x + step
                } else {
                    rect.right() - step
                },
                y: rect.bottom(),
            };
            let to = libgfx_rust::IntPoint {
                x: if bottom_left_resizer { rect.x } else { rect.right() },
                y: rect.bottom() - step,
            };
            recorder
                .recorder
                .draw_line(from, to, color, 1, LineStyle::Solid, Color::TRANSPARENT);
        };
        let third = rect.width / 3;
        let mut step = third - 1;
        while step < rect.width {
            paint_resizer_line(step, light);
            paint_resizer_line(step + 1, dark);
            if third == 0 {
                break;
            }
            step += third;
        }
    }

    paint_middle_button_scroll_indicator(recorder, paintable);
}

fn paint_middle_button_scroll_indicator(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    const CIRCLE_COLOR: Color = Color::from_rgba(255, 255, 255, 220);
    const ARROW_COLOR: Color = Color::from_rgb(64, 64, 64);
    const RADIUS: i32 = 16;
    const ARROW_SIZE: f32 = 6.0;
    const ARROW_OFFSET: f32 = 8.0;

    if recorder.data(paintable).kind != PaintableKind::ViewportPaintable || !recorder.inputs.middle_button_scroll_active
    {
        return;
    }
    let device_origin = recorder
        .converter
        .rounded_device_point(recorder.inputs.middle_button_scroll_origin.into());
    let circle = IntRect::new(
        device_origin.x - RADIUS,
        device_origin.y - RADIUS,
        RADIUS * 2,
        RADIUS * 2,
    );
    recorder
        .recorder
        .fill_rect_with_uniform_rounded_corners(circle, CIRCLE_COLOR, RADIUS);
    recorder.recorder.draw_ellipse(circle, ARROW_COLOR, 1);

    let x = device_origin.x as f32;
    let y = device_origin.y as f32;
    let mut paint_arrow = |p1: FloatPoint, p2: FloatPoint, p3: FloatPoint| {
        let mut builder = libgfx_rust::path::PathBuilder::new();
        builder.move_to(p1.x, p1.y);
        builder.line_to(p2.x, p2.y);
        builder.line_to(p3.x, p3.y);
        builder.close();
        let path = builder.build();
        recorder.recorder.fill_path(FillPathParams {
            path: &path,
            opacity: 1.0,
            paint_style_or_color: PaintStyleOrColor::Color(ARROW_COLOR),
            winding_rule: WindingRule::EvenOdd,
            should_anti_alias: ShouldAntiAlias::Yes,
        });
    };
    let point = |x: f32, y: f32| FloatPoint { x, y };
    // FIXME: We could paint a subset of these arrows depending on which direction the container may be scrolled.
    paint_arrow(
        point(x, y - ARROW_OFFSET - ARROW_SIZE),
        point(x - ARROW_SIZE, y - ARROW_OFFSET),
        point(x + ARROW_SIZE, y - ARROW_OFFSET),
    );
    paint_arrow(
        point(x, y + ARROW_OFFSET + ARROW_SIZE),
        point(x - ARROW_SIZE, y + ARROW_OFFSET),
        point(x + ARROW_SIZE, y + ARROW_OFFSET),
    );
    paint_arrow(
        point(x - ARROW_OFFSET - ARROW_SIZE, y),
        point(x - ARROW_OFFSET, y - ARROW_SIZE),
        point(x - ARROW_OFFSET, y + ARROW_SIZE),
    );
    paint_arrow(
        point(x + ARROW_OFFSET + ARROW_SIZE, y),
        point(x + ARROW_OFFSET, y - ARROW_SIZE),
        point(x + ARROW_OFFSET, y + ARROW_SIZE),
    );
}
