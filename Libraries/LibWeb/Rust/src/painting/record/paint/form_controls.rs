/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libgfx_rust::path::{OwnedPath, PathBuilder};

use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::recorder::{FillPathParams, PaintStyleOrColor};
use crate::painting::host::FfiReplacedPaintFacts;
use crate::painting::paintable_geometry::absolute_rect;
use crate::painting::record::PaintRecorder;
use libgfx_rust::{Color, IntRect, ShouldAntiAlias, WindingRule};

// Note: the color names reflect what the colors would be for a light theme,
// not necessary the actual colors.
struct InputColors {
    accent: Color,
    base: Color,
    dark_gray: Color,
    gray: Color,
    mid_gray: Color,
    light_gray: Color,
}

impl InputColors {
    fn background_color(&self, enabled: bool) -> Color {
        if enabled { self.base } else { self.light_gray }
    }

    fn border_color(&self, enabled: bool) -> Color {
        if enabled { self.gray } else { self.mid_gray }
    }

    fn get_shade(color: Color, amount: f32, canvas_color: Color) -> Color {
        color.mixed_with(canvas_color, amount)
    }
}

fn compute_input_colors(facts: &FfiReplacedPaintFacts) -> InputColors {
    // These shades have been picked to work well for all themes and have enough variation to paint
    // all input states (disabled, enabled, checked, etc).
    let canvas = Color(facts.canvas_color);
    let base_text_color = Color(facts.canvas_text_color);
    let accent = Color(facts.accent_color);
    let base = InputColors::get_shade(base_text_color.inverted(), 0.8, canvas);
    let dark_gray = InputColors::get_shade(base_text_color, 0.3, canvas);
    let gray = InputColors::get_shade(dark_gray, 0.4, canvas);
    let mid_gray = InputColors::get_shade(gray, 0.3, canvas);
    let light_gray = InputColors::get_shade(mid_gray, 0.3, canvas);
    InputColors {
        accent,
        base,
        dark_gray,
        gray,
        mid_gray,
        light_gray,
    }
}

fn centered_square_device_rect(recorder: &PaintRecorder<'_>, outer_rect: CssPixelRect, size: CssPixels) -> IntRect {
    let center_x = outer_rect.x + outer_rect.width / 2usize;
    let center_y = outer_rect.y + outer_rect.height / 2usize;
    let rect = CssPixelRect::new(center_x - size / 2usize, center_y - size / 2usize, size, size);
    recorder.converter.enclosing_device_rect(rect)
}

fn check_mark_path(checkbox_rect: IntRect) -> OwnedPath {
    let mut builder = PathBuilder::new();
    builder.move_to(72.0, 14.0);
    builder.line_to(37.0, 64.0);
    builder.line_to(19.0, 47.0);
    builder.line_to(8.0, 58.0);
    builder.line_to(40.0, 89.0);
    builder.line_to(85.0, 24.0);
    builder.close();
    let path = builder.build();

    let checkmark_width = 100.0f32;
    let checkmark_height = 100.0f32;
    let scale_x = checkbox_rect.width as f32 / checkmark_width;
    let scale_y = checkbox_rect.height as f32 / checkmark_height;
    path.copy_transformed([scale_x, 0.0, 0.0, scale_y, 0.0, 0.0])
}

pub(crate) fn paint_check_box_foreground(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    let facts = recorder
        .paint_host
        .replaced_paint_facts(recorder.layout_node_shell(paintable));
    let enabled = facts.enabled;
    let canvas_color = Color(facts.canvas_color);

    // Keep checkboxes painted as square, centered within the space they occupy.
    let outer_rect = absolute_rect(recorder.layout_arena, paintable);
    let checkbox_size = outer_rect.width.min(outer_rect.height);
    let checkbox_rect = centered_square_device_rect(recorder, outer_rect, checkbox_size);
    let checkbox_radius = checkbox_rect.width / 5;

    let shade = |color: Color, amount: f32| InputColors::get_shade(color, amount, canvas_color);

    let modify_color = |color: Color| {
        // FIXME: Make this only take effect while this element or its labels are hovered.
        if facts.being_activated && enabled {
            return shade(color, 0.3);
        }
        color
    };

    let input_colors = compute_input_colors(&facts);

    let increase_contrast = |mut color: Color, background: Color| {
        let min_contrast = 2.0;
        if color.contrast_ratio(background) < min_contrast {
            color = color.inverted();
            if color.contrast_ratio(background) > min_contrast {
                return color;
            }
        }
        color
    };

    // Little heuristic that smaller things look better with more smoothness.
    if facts.checked && !facts.indeterminate {
        let background_color = if enabled {
            input_colors.accent
        } else {
            input_colors.mid_gray
        };
        recorder.recorder.fill_rect_with_uniform_rounded_corners(
            checkbox_rect,
            modify_color(background_color),
            checkbox_radius,
        );
        let mut tick_color = increase_contrast(input_colors.base, background_color);
        if !enabled {
            tick_color = shade(tick_color, 0.5);
        }
        let path = check_mark_path(checkbox_rect).copy_transformed([
            1.0,
            0.0,
            0.0,
            1.0,
            checkbox_rect.x as f32,
            checkbox_rect.y as f32,
        ]);
        recorder.recorder.fill_path(FillPathParams {
            path: &path,
            opacity: 1.0,
            paint_style_or_color: PaintStyleOrColor::Color(tick_color),
            winding_rule: WindingRule::EvenOdd,
            should_anti_alias: ShouldAntiAlias::Yes,
        });
    } else {
        let background_color = input_colors.background_color(enabled);
        let border_thickness = 1.max(checkbox_rect.width / 10);
        recorder.recorder.fill_rect_with_uniform_rounded_corners(
            checkbox_rect,
            modify_color(input_colors.border_color(enabled)),
            checkbox_radius,
        );
        recorder.recorder.fill_rect_with_uniform_rounded_corners(
            checkbox_rect.shrunken(border_thickness, border_thickness, border_thickness, border_thickness),
            background_color,
            0.max(checkbox_radius - border_thickness),
        );
        if facts.indeterminate {
            let radius = (0.05 * checkbox_rect.width as f64) as i32;
            let dash_color = increase_contrast(input_colors.dark_gray, background_color);
            let dash_rect = checkbox_rect.inflated(
                (-0.4 * checkbox_rect.width as f64) as i32,
                (-0.8 * checkbox_rect.height as f64) as i32,
            );
            recorder
                .recorder
                .fill_rect_with_uniform_rounded_corners(dash_rect, dash_color, radius);
        }
    }
}

pub(crate) fn paint_radio_button_foreground(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    let facts = recorder
        .paint_host
        .replaced_paint_facts(recorder.layout_node_shell(paintable));
    let canvas_color = Color(facts.canvas_color);

    let enabled = facts.enabled;
    let input_colors = compute_input_colors(&facts);

    let mut background_color = input_colors.background_color(enabled);
    let accent = input_colors.accent;

    let fill_color = if !enabled {
        input_colors.mid_gray
    } else {
        let mut color = if facts.checked {
            // Handle the awkward case where a light color has been used for the accent color.
            if accent.contrast_ratio(background_color) < 2.0 && accent.contrast_ratio(input_colors.dark_gray) > 2.0 {
                background_color = input_colors.dark_gray;
            }
            accent
        } else {
            input_colors.gray
        };
        // FIXME: Make this only take effect while this element or its labels are hovered.
        if facts.being_activated {
            color = InputColors::get_shade(color, 0.3, canvas_color);
        }
        color
    };

    // Keep radio buttons painted as circles, centered within the space they occupy.
    let outer_rect = absolute_rect(recorder.layout_arena, paintable);
    let radio_button_size = outer_rect.width.min(outer_rect.height);

    // This is based on a 1px outer border and 2px inner border when drawn at 13x13.
    let radio_button_rect = centered_square_device_rect(recorder, outer_rect, radio_button_size);
    let outer_border_width = 1.max((radio_button_rect.width as f32 / 13.0).ceil() as i32);
    let inner_border_width = 2.max((radio_button_rect.width as f32 / 4.0).ceil() as i32);

    let draw_circle = |recorder: &mut PaintRecorder<'_>, rect: IntRect, color: Color| {
        // Note: Doing this is a bit more forgiving than draw_circle() which will round to the nearest even radius.
        // This will fudge it (which works better here).
        recorder
            .recorder
            .fill_rect_with_uniform_rounded_corners(rect, color, rect.width / 2);
    };
    let shrink_all = |rect: IntRect, amount: i32| rect.shrunken(amount, amount, amount, amount);

    draw_circle(recorder, radio_button_rect, fill_color);
    draw_circle(
        recorder,
        shrink_all(radio_button_rect, outer_border_width),
        background_color,
    );
    if facts.checked {
        draw_circle(recorder, shrink_all(radio_button_rect, inner_border_width), fill_color);
    }
}
