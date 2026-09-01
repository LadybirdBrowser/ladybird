/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::flex_direction;
use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::layout::grid_formatting_context;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::{ContextRef, DisplayListGlyph, FontResourceId};
use crate::painting::display_list::recorder::GlyphRunForRecording;
use crate::painting::host::{FfiFlexOverlayInput, FfiGridOverlayInput};
use crate::painting::paintable_geometry;
use crate::painting::record::PaintRecorder;
use libgfx_rust::{Color, FloatPoint, IntPoint, IntRect, LineStyle, Orientation};

pub(crate) fn record_inspector_overlays(recorder: &mut PaintRecorder<'_>) {
    let inputs = recorder.inputs;
    recorder.recorder.set_accumulated_visual_context(ContextRef::default());
    if inputs.has_inspector_highlight {
        with_highlight_context(recorder, inputs.inspector_highlight_paintable, |recorder, paintable| {
            paint_box_model_highlight(recorder, paintable);
        });
    }
    // SAFETY: The host borrows the overlay input arrays to the recording, which is synchronous.
    let flex_overlays: &[FfiFlexOverlayInput] = if inputs.flex_overlay_count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(inputs.flex_overlays, inputs.flex_overlay_count) }
    };
    for input in flex_overlays {
        with_highlight_context(recorder, input.paintable, |recorder, paintable| {
            paint_flex_overlay(recorder, paintable, input);
        });
    }
    // SAFETY: See above.
    let grid_overlays: &[FfiGridOverlayInput] = if inputs.grid_overlay_count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(inputs.grid_overlays, inputs.grid_overlay_count) }
    };
    for input in grid_overlays {
        with_highlight_context(recorder, input.paintable, |recorder, paintable| {
            paint_grid_overlay(recorder, paintable, input);
        });
    }
    if inputs.caret_debug_rect.has_value {
        paint_caret_debug_marker(recorder, CssPixelRect::from(inputs.caret_debug_rect.value));
    }
}

fn with_highlight_context(
    recorder: &mut PaintRecorder<'_>,
    paintable: NodeSlotId,
    callback: impl FnOnce(&mut PaintRecorder<'_>, NodeSlotId),
) {
    if paintable.is_invalid() || !recorder.layout_arena.paintable_row_is_populated(paintable) {
        return;
    }
    // Overlays follow the highlighted element's transforms and scroll offsets, but must not be clipped
    // or faded by its ancestors, so their commands apply the element's spatial node without its frame.
    let context = ContextRef::spatial_only(recorder.data(paintable).accumulated_visual_context.spatial);
    recorder.with_context(context, |recorder| callback(recorder, paintable));
}

fn paint_box_model_highlight(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId) {
    let content_rect = paintable_geometry::absolute_rect(recorder.layout_arena, paintable);
    let margin = paintable_geometry::committed_margin(recorder.layout_arena, paintable);
    let border = paintable_geometry::committed_border(recorder.layout_arena, paintable);
    let padding = paintable_geometry::committed_padding(recorder.layout_arena, paintable);
    let margin_rect = content_rect.inflated(
        margin.top + border.top + padding.top,
        margin.right + border.right + padding.right,
        margin.bottom + border.bottom + padding.bottom,
        margin.left + border.left + padding.left,
    );
    let border_rect = paintable_geometry::absolute_border_box_rect(recorder.layout_arena, paintable);
    let padding_rect = paintable_geometry::absolute_padding_box_rect(recorder.layout_arena, paintable);

    let converter = recorder.converter;
    let paint_inspector_rect = |recorder: &mut PaintRecorder<'_>, rect: CssPixelRect, color: Color| {
        let device_rect = converter.enclosing_device_rect(rect);
        recorder.recorder.fill_rect(device_rect, color.with_alpha(100));
        recorder.recorder.draw_rect(device_rect, color, false);
    };
    paint_inspector_rect(recorder, margin_rect, Color::from_rgb(255, 255, 0));
    paint_inspector_rect(recorder, padding_rect, Color::from_rgb(0, 255, 255));
    paint_inspector_rect(recorder, border_rect, Color::from_rgb(0, 255, 0));
    paint_inspector_rect(recorder, content_rect, Color::from_rgb(255, 0, 255));

    let text = recorder
        .paint_host
        .overlay_node_label_text(recorder.layout_node_shell(paintable));
    let label = shape_overlay_label(recorder, &text, 12.0);
    let mut size_text_rect = border_rect;
    size_text_rect.y = border_rect.y + border_rect.height;
    size_text_rect.width = CssPixels::nearest_value_for_f32(label.css_width) + CssPixels::from_integer(4);
    size_text_rect.height = CssPixels::nearest_value_for_f32(label.css_pixel_size) + CssPixels::from_integer(4);
    let device_rect = converter.enclosing_device_rect(size_text_rect);
    let inputs = recorder.inputs;
    recorder.recorder.fill_rect(device_rect, inputs.tooltip_color);
    recorder
        .recorder
        .draw_rect(device_rect, inputs.tooltip_border_color, false);
    draw_label(recorder, &label, device_rect, inputs.tooltip_text_color);
}

fn paint_flex_overlay(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId, input: &FfiFlexOverlayInput) {
    let Some(flex_layout_data) =
        crate::painting::paintable_geometry::committed_flex_layout_data(recorder.layout_arena, paintable)
    else {
        return;
    };
    let content_rect = paintable_geometry::absolute_rect(recorder.layout_arena, paintable);
    let origin = CssPixelPoint {
        x: content_rect.x,
        y: content_rect.y,
    };
    let viewport_rect = CssPixelRect::from(recorder.inputs.css_viewport_rect);
    let color = input.color;
    let line_color = color.with_alpha(220);
    let container_fill_color = color.with_alpha(28);
    let line_fill_color = color.with_alpha(18);
    let item_fill_color = color.with_alpha(32);
    let line_thickness = CssPixels::from_integer(1);
    let main_axis_is_horizontal = matches!(
        flex_layout_data.flex_direction,
        flex_direction::ROW | flex_direction::ROW_REVERSE
    );

    let converter = recorder.converter;
    let paint_rect = |recorder: &mut PaintRecorder<'_>, rect: CssPixelRect, rect_color: Color| {
        let visible_rect = rect.intersected(viewport_rect);
        if visible_rect.is_empty() {
            return;
        }
        recorder
            .recorder
            .fill_rect(converter.enclosing_device_rect(visible_rect), rect_color);
    };
    let paint_outline = |recorder: &mut PaintRecorder<'_>, rect: CssPixelRect, rect_color: Color| {
        let visible_rect = rect.intersected(viewport_rect);
        if visible_rect.is_empty() {
            return;
        }
        recorder
            .recorder
            .draw_rect(converter.enclosing_device_rect(visible_rect), rect_color, false);
    };

    paint_rect(recorder, content_rect, container_fill_color);
    paint_outline(recorder, content_rect, line_color);

    for line in &flex_layout_data.lines {
        let line_rect = if main_axis_is_horizontal {
            CssPixelRect {
                x: content_rect.x,
                y: origin.y + line.cross_start,
                width: content_rect.width,
                height: line.cross_size,
            }
        } else {
            CssPixelRect {
                x: origin.x + line.cross_start,
                y: content_rect.y,
                width: line.cross_size,
                height: content_rect.height,
            }
        };
        paint_rect(recorder, line_rect, line_fill_color);
        paint_outline(recorder, line_rect, line_color);

        for item in &line.items {
            let item_rect = CssPixelRect {
                x: item.rect.x,
                y: item.rect.y,
                width: item.rect.width,
                height: item.rect.height,
            }
            .translated(origin.x, origin.y);
            paint_rect(recorder, item_rect, item_fill_color);
            paint_outline(recorder, item_rect, line_color);
        }
    }

    // Repaint the container border last so adjacent flex lines and items do not obscure it.
    paint_outline(recorder, content_rect, line_color);

    if main_axis_is_horizontal {
        for line in &flex_layout_data.lines {
            let y = origin.y + line.cross_start;
            paint_rect(
                recorder,
                CssPixelRect {
                    x: content_rect.x,
                    y,
                    width: content_rect.width,
                    height: line_thickness,
                },
                line_color,
            );
            paint_rect(
                recorder,
                CssPixelRect {
                    x: content_rect.x,
                    y: y + line.cross_size,
                    width: content_rect.width,
                    height: line_thickness,
                },
                line_color,
            );
        }
    } else {
        for line in &flex_layout_data.lines {
            let x = origin.x + line.cross_start;
            paint_rect(
                recorder,
                CssPixelRect {
                    x,
                    y: content_rect.y,
                    width: line_thickness,
                    height: content_rect.height,
                },
                line_color,
            );
            paint_rect(
                recorder,
                CssPixelRect {
                    x: x + line.cross_size,
                    y: content_rect.y,
                    width: line_thickness,
                    height: content_rect.height,
                },
                line_color,
            );
        }
    }
}

fn paint_grid_overlay(recorder: &mut PaintRecorder<'_>, paintable: NodeSlotId, input: &FfiGridOverlayInput) {
    let Some(grid_layout_data) =
        crate::painting::paintable_geometry::committed_grid_layout_data(recorder.layout_arena, paintable)
    else {
        return;
    };
    let content_rect = paintable_geometry::absolute_rect(recorder.layout_arena, paintable);
    let origin = CssPixelPoint {
        x: content_rect.x,
        y: content_rect.y,
    };
    let viewport_rect = CssPixelRect::from(recorder.inputs.css_viewport_rect);
    let color = input.color;
    let label_height = CssPixels::nearest_value_for_f32(input.label_css_pixel_size) + CssPixels::from_integer(4);
    let two = CssPixels::from_integer(2);

    let converter = recorder.converter;
    let paint_rect = |recorder: &mut PaintRecorder<'_>, rect: CssPixelRect, rect_color: Color| {
        recorder
            .recorder
            .fill_rect(converter.enclosing_device_rect(rect), rect_color);
    };
    let paint_label = |recorder: &mut PaintRecorder<'_>, top_left: CssPixelPoint, text: &[u16]| {
        let label = shape_overlay_label(recorder, text, 10.0);
        let label_width = label_width_for(&label);
        let label_rect = CssPixelRect {
            x: top_left.x,
            y: top_left.y,
            width: label_width,
            height: label_height,
        };
        let label_device_rect = converter.enclosing_device_rect(label_rect);
        let label_background = color.with_alpha(235);
        recorder.recorder.fill_rect(label_device_rect, label_background);
        recorder
            .recorder
            .draw_rect(label_device_rect, color.with_alpha(255), false);
        draw_label(recorder, &label, label_device_rect, input.label_foreground_color);
    };
    let paint_centered_label = |recorder: &mut PaintRecorder<'_>, rect: CssPixelRect, text: &[u16]| {
        let label = shape_overlay_label(recorder, text, 10.0);
        let label_width = label_width_for(&label);
        let top_left = CssPixelPoint {
            x: rect.center().x - label_width.div_as_fraction(two),
            y: rect.center().y - label_height.div_as_fraction(two),
        };
        paint_label(recorder, top_left, text);
    };

    let line_start_for_number = |lines: &[grid_formatting_context::GridLayoutLine], number: u32| -> Option<CssPixels> {
        lines.iter().find(|line| line.number == number).map(|line| line.start)
    };
    let line_number_label = |line: &grid_formatting_context::GridLayoutLine| -> Vec<u16> {
        let text = if line.negative_number < 0 {
            format!("{} / {}", line.number, line.negative_number)
        } else {
            format!("{}", line.number)
        };
        text.encode_utf16().collect()
    };
    let track_size_label = |track: &grid_formatting_context::GridLayoutTrack| -> Vec<u16> {
        format!("{:.2}px", track.breadth.to_double().max(0.0))
            .encode_utf16()
            .collect()
    };

    let line_color = color.with_alpha(220);
    let gap_color = color.with_alpha(45);
    let line_thickness = CssPixels::from_integer(1);
    let zero = CssPixels::from_raw(0);

    for fragment in &grid_layout_data.fragments {
        for column_line in &fragment.columns.lines {
            let x = origin.x + column_line.start;
            let (line_top, line_height) = if input.show_infinite_lines {
                (viewport_rect.y, viewport_rect.height)
            } else {
                (content_rect.y, content_rect.height)
            };

            if column_line.breadth > zero {
                paint_rect(
                    recorder,
                    CssPixelRect {
                        x,
                        y: line_top,
                        width: column_line.breadth,
                        height: line_height,
                    },
                    gap_color,
                );
            }
            paint_rect(
                recorder,
                CssPixelRect {
                    x,
                    y: line_top,
                    width: line_thickness,
                    height: line_height,
                },
                line_color,
            );

            if input.show_line_numbers {
                let top_left = CssPixelPoint {
                    x: x + two,
                    y: line_top + two,
                };
                paint_label(recorder, top_left, &line_number_label(column_line));
            }
        }

        for row_line in &fragment.rows.lines {
            let y = origin.y + row_line.start;
            let (line_left, line_width) = if input.show_infinite_lines {
                (viewport_rect.x, viewport_rect.width)
            } else {
                (content_rect.x, content_rect.width)
            };

            if row_line.breadth > zero {
                paint_rect(
                    recorder,
                    CssPixelRect {
                        x: line_left,
                        y,
                        width: line_width,
                        height: row_line.breadth,
                    },
                    gap_color,
                );
            }
            paint_rect(
                recorder,
                CssPixelRect {
                    x: line_left,
                    y,
                    width: line_width,
                    height: line_thickness,
                },
                line_color,
            );

            if input.show_line_numbers {
                let top_left = CssPixelPoint {
                    x: line_left + two,
                    y: y + two,
                };
                paint_label(recorder, top_left, &line_number_label(row_line));
            }
        }

        if input.show_track_sizes {
            for column_track in &fragment.columns.tracks {
                let track_rect = CssPixelRect {
                    x: origin.x + column_track.start,
                    y: content_rect.y,
                    width: column_track.breadth,
                    height: content_rect.height.min(label_height + CssPixels::from_integer(4)),
                };
                paint_centered_label(recorder, track_rect, &track_size_label(column_track));
            }

            for row_track in &fragment.rows.tracks {
                let track_rect = CssPixelRect {
                    x: content_rect.x,
                    y: origin.y + row_track.start,
                    width: content_rect.width.min(CssPixels::from_integer(72)),
                    height: row_track.breadth,
                };
                paint_centered_label(recorder, track_rect, &track_size_label(row_track));
            }
        }

        if input.show_area_names {
            for area in &fragment.areas {
                let column_start = line_start_for_number(&fragment.columns.lines, area.column_start);
                let column_end = line_start_for_number(&fragment.columns.lines, area.column_end);
                let row_start = line_start_for_number(&fragment.rows.lines, area.row_start);
                let row_end = line_start_for_number(&fragment.rows.lines, area.row_end);
                let (Some(column_start), Some(column_end), Some(row_start), Some(row_end)) =
                    (column_start, column_end, row_start, row_end)
                else {
                    continue;
                };

                let area_rect = CssPixelRect {
                    x: origin.x + column_start,
                    y: origin.y + row_start,
                    width: column_end - column_start,
                    height: row_end - row_start,
                };
                paint_rect(recorder, area_rect, color.with_alpha(24));

                let visible_area_rect = area_rect.intersected(viewport_rect);
                if !visible_area_rect.is_empty() {
                    let name = area.name.encode_utf16().collect::<Vec<_>>();
                    paint_centered_label(recorder, visible_area_rect, &name);
                }
            }
        }
    }
}

struct OverlayLabel {
    font_id: u64,
    css_width: f32,
    css_pixel_size: f32,
    device_glyph_width: f32,
    device_ascent: f32,
    device_descent: f32,
    blob_bounds: [f32; 4],
    glyphs: Vec<DisplayListGlyph>,
}

fn shape_overlay_label(recorder: &mut PaintRecorder<'_>, text: &[u16], css_font_size: f32) -> OverlayLabel {
    let device_scale = recorder.inputs.device_pixels_per_css_pixel as f32;
    // SAFETY: The host resolves the fonts from the platform font caches, which
    // keep them live through this synchronous call; retaining them here keeps
    // them alive for the rest of the recording.
    let css_font =
        unsafe { libgfx_rust::font::RetainedFont::retain(recorder.paint_host.overlay_label_font(css_font_size)) };
    // SAFETY: See above.
    let device_font = unsafe {
        libgfx_rust::font::RetainedFont::retain(recorder.paint_host.overlay_label_font(css_font_size * device_scale))
    };
    // SAFETY: The RetainedFont handles keep both fonts live for these borrows.
    let css_font_ref = unsafe { libgfx_rust::font::FontRef::from_raw(css_font.as_raw()) };
    // SAFETY: See above.
    let device_font_ref = unsafe { libgfx_rust::font::FontRef::from_raw(device_font.as_raw()) };
    let shaped = libgfx_rust::text_layout::shape_text(
        device_font_ref,
        text,
        libgfx_rust::text_layout::TextType::Ltr,
        0.0,
        0.0,
        0.0,
    );
    let blob_bounds = libgfx_rust::text_layout::glyph_run_bounding_box(device_font_ref, shaped.glyphs(), 1.0);
    let (device_ascent, device_descent) = device_font_ref.pixel_metrics_ascent_descent();
    let font_id = recorder.register_font(device_font.as_raw());
    let glyphs = shaped
        .glyphs()
        .iter()
        .map(|glyph| DisplayListGlyph {
            position: FloatPoint { x: glyph.x, y: glyph.y },
            glyph_id: glyph.glyph_id,
        })
        .collect();
    OverlayLabel {
        font_id,
        css_width: css_font_ref.measure_text_width(text),
        css_pixel_size: css_font_ref.pixel_size(),
        device_glyph_width: shaped.width(),
        device_ascent,
        device_descent,
        blob_bounds,
        glyphs,
    }
}

fn label_width_for(label: &OverlayLabel) -> CssPixels {
    let label_padding = CssPixels::from_integer(4);
    CssPixels::nearest_value_for_f32(label.css_width) + label_padding * 2usize
}

fn draw_label(recorder: &mut PaintRecorder<'_>, label: &OverlayLabel, rect: IntRect, color: Color) {
    if rect.width <= 0 || rect.height <= 0 || color.alpha() == 0 {
        return;
    }
    let baseline_start = FloatPoint {
        x: rect.x as f32 + (rect.width as f32 - label.device_glyph_width) / 2.0,
        y: rect.y as f32
            + label.device_ascent
            + (rect.height as f32 - (label.device_ascent + label.device_descent)) / 2.0,
    };
    let glyph_bounding_rect = IntRect::new(
        (label.blob_bounds[0] + baseline_start.x).round_ties_even() as i32,
        (label.blob_bounds[1] + baseline_start.y).round_ties_even() as i32,
        label.blob_bounds[2].round_ties_even() as i32,
        label.blob_bounds[3].round_ties_even() as i32,
    );
    recorder.recorder.draw_glyph_run(
        baseline_start,
        GlyphRunForRecording {
            font_id: FontResourceId(label.font_id),
            glyphs: &label.glyphs,
        },
        color,
        rect,
        1.0,
        Orientation::Horizontal,
        glyph_bounding_rect,
    );
}

fn paint_caret_debug_marker(recorder: &mut PaintRecorder<'_>, css_rect: CssPixelRect) {
    let caret_rect = recorder.converter.enclosing_device_rect(css_rect);
    let caret_x = caret_rect.x;
    let caret_top = caret_rect.y;
    let caret_bottom = caret_rect.y + caret_rect.height;
    let marker_color = Color::from_rgb(255, 0, 255);

    let mut draw_marker_line = |from: IntPoint, to: IntPoint| {
        recorder
            .recorder
            .draw_line(from, to, marker_color, 2, LineStyle::Solid, Color::TRANSPARENT);
    };
    draw_marker_line(
        IntPoint {
            x: caret_x,
            y: caret_top,
        },
        IntPoint {
            x: caret_x,
            y: caret_bottom,
        },
    );
    draw_marker_line(
        IntPoint {
            x: caret_x - 4,
            y: caret_top,
        },
        IntPoint {
            x: caret_x + 4,
            y: caret_top,
        },
    );
    draw_marker_line(
        IntPoint {
            x: caret_x - 4,
            y: caret_bottom,
        },
        IntPoint {
            x: caret_x + 4,
            y: caret_bottom,
        },
    );
}
