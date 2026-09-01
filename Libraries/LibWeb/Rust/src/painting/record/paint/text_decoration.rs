/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::{
    text_decoration_line, text_decoration_skip_ink, text_decoration_style, text_underline_position_horizontal,
};
use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::layout::node_facts;
use crate::painting::display_list::recorder::{PaintStyleOrColor, StrokePathParams};
use crate::painting::record::PaintRecorder;
use libgfx_rust::path::PathBuilder;
use libgfx_rust::{CapStyle, Color, IntPoint, JoinStyle, LineStyle, ShouldAntiAlias};

#[derive(Clone, Copy, Default)]
pub(crate) struct TextDecorationSet {
    pub lines: [u8; 8],
    pub line_count: u32,
    pub style: u8,
    pub color: u32,
    pub thickness: CssPixels,
    pub baseline: CssPixels,
    pub anchored_at_fragment_baseline: bool,
    pub decorating_font_pixel_size: f32,
    pub decorating_font_descent: f32,
    pub decorating_font_x_height: i32,
    pub text_font_pixel_size: f32,
    pub text_underline_offset: CssPixels,
    pub text_underline_position_horizontal: u8,
    pub skip_ink: u8,
}

fn text_decoration_lines(style: crate::css::computed_value_views::ComputedValuesView<'_>) -> &[u8] {
    let list = &style.text_reset().text_decoration_lines;
    if list.pointer.is_null() {
        return &[];
    }
    // SAFETY: The retained list belongs to the style group payload, which outlives the pass.
    unsafe { std::slice::from_raw_parts(list.pointer, list.length) }
}

fn first_available_font(
    arena: &crate::layout::LayoutNodeArena,
    node: crate::layout::node_data::NodeSlotId,
) -> Option<libgfx_rust::font::FontRef<'static>> {
    let style = arena.node_style_if_live(node)?;
    let font = style.first_available_font();
    if font.is_null() {
        return None;
    }
    // SAFETY: The style payload retains the font for the pass.
    Some(unsafe { libgfx_rust::font::FontRef::from_raw(font) })
}

fn resolve_text_decoration_thickness(
    arena: &crate::layout::LayoutNodeArena,
    value_node: crate::layout::node_data::NodeSlotId,
    basis_node: crate::layout::node_data::NodeSlotId,
    glyph_height: CssPixels,
) -> CssPixels {
    let one = CssPixels::from_integer(1);
    let Some(style) = arena.node_style_if_live(value_node) else {
        return one;
    };
    let text_reset = style.text_reset();
    // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-thickness-auto
    // The UA chooses an appropriate thickness for text decoration lines; see below.
    // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-thickness-from-font
    // If the first available font has metrics indicating a preferred underline width, use that width,
    // otherwise behaves as auto.
    // FIXME: Implement this properly.
    if text_reset.text_decoration_thickness_kind != 2 {
        return glyph_height.scaled(0.1).max(one);
    }
    // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-thickness-length-percentage
    let em = arena
        .node_style_if_live(basis_node)
        .map_or(CssPixels::from_raw(0), |basis_style| basis_style.font_size());
    text_reset
        .text_decoration_thickness
        .length_percentage()
        .map_or(one, |value| value.to_px(em))
        .max(one)
}

// https://drafts.csswg.org/css-text-decor-4/#text-line-constancy
fn anchor_for_decorating_box(
    recorder: &PaintRecorder<'_>,
    block: NodeSlotId,
    fragment: &crate::painting::paintable_data::FragmentRecord,
    decorating_node: crate::layout::node_data::NodeSlotId,
    text_parent: crate::layout::node_data::NodeSlotId,
) -> (CssPixels, bool) {
    // Text always sits at its parent's baseline, so the parent's decorations are anchored
    // exactly there.
    if decorating_node == text_parent {
        return (fragment.baseline, true);
    }
    let side = recorder.layout_arena.paintable_side_data(block);
    for piece in &side.inline_box_pieces {
        if piece.node == decorating_node && piece.line_index == fragment.line_index {
            return (
                piece.baseline - CssPixels::from_raw(fragment.offset.y.raw_value()),
                fragment.accumulated_vertical_shift == piece.accumulated_vertical_shift,
            );
        }
    }
    let line_baseline = side.lines[fragment.line_index as usize].baseline;
    (
        line_baseline - CssPixels::from_raw(fragment.offset.y.raw_value()),
        fragment.accumulated_vertical_shift == CssPixels::from_raw(0),
    )
}

pub(crate) fn decoration_sets_for_span(
    recorder: &PaintRecorder<'_>,
    block: NodeSlotId,
    span: &crate::painting::record::paint::text::RenderSpan,
) -> Vec<TextDecorationSet> {
    let mut sets: Vec<TextDecorationSet> = Vec::new();
    if span.start_code_unit == span.end_code_unit {
        return sets;
    }
    let arena = recorder.layout_arena;
    let fragment = &recorder.layout_arena.paintable_side_data(block).fragments[span.fragment_index as usize];
    let text_node = fragment.layout_node;
    if !arena.node_kind_if_live(text_node).is_some_and(node_facts::kind_is_text) {
        return sets;
    }
    let Some(text_parent) = arena.node_parent_if_live(text_node) else {
        return sets;
    };

    let mut push_set = |decorating_node: crate::layout::node_data::NodeSlotId,
                        lines: &[u8],
                        style_code: u8,
                        color: u32,
                        thickness: CssPixels| {
        let Some(decorating_style) = arena.node_style_if_live(decorating_node) else {
            return;
        };
        let mut set = TextDecorationSet {
            style: style_code,
            color,
            thickness,
            ..TextDecorationSet::default()
        };
        set.line_count = lines.len().min(set.lines.len()) as u32;
        set.lines[..set.line_count as usize].copy_from_slice(&lines[..set.line_count as usize]);
        let (baseline, anchored) = anchor_for_decorating_box(recorder, block, fragment, decorating_node, text_parent);
        set.baseline = baseline;
        set.anchored_at_fragment_baseline = anchored;
        // https://drafts.csswg.org/css-text-decor-4/#text-line-constancy
        // However, for underlines and overlines the UA must use a single thickness and position on each line for the
        // decorations deriving from a single decorating box.
        let Some(decorating_font) = first_available_font(arena, decorating_node) else {
            return;
        };
        let text_font = if decorating_node == text_parent {
            decorating_font
        } else {
            let source = crate::painting::text_fragment::style_source(arena, fragment);
            first_available_font(arena, source).unwrap_or(decorating_font)
        };
        set.decorating_font_pixel_size = decorating_font.pixel_size();
        set.decorating_font_descent = decorating_font.pixel_metrics_ascent_descent().1;
        set.decorating_font_x_height = decorating_font.x_height() as i32;
        set.text_font_pixel_size = text_font.pixel_size();
        let inherited_text = decorating_style.inherited_text();
        set.text_underline_offset = inherited_text.text_underline_offset.used_value;
        set.text_underline_position_horizontal = inherited_text.text_underline_position.horizontal;
        set.skip_ink = arena
            .node_style_if_live(text_parent)
            .map_or(0, |style| style.inherited_text().text_decoration_skip_ink);
        sets.push(set);
    };

    // A span with an explicit text decoration (from ::selection) replaces the decorations this text would
    // otherwise be painted with.
    if let Some(selection_text_decoration) = &span.selection_text_decoration {
        let glyph_height = first_available_font(arena, text_parent).map_or(CssPixels::from_raw(0), |font| {
            CssPixels::nearest_value_for_f32(font.pixel_size())
        });
        let thickness = resolve_text_decoration_thickness(arena, text_parent, text_parent, glyph_height);
        push_set(
            text_parent,
            &selection_text_decoration.lines[..selection_text_decoration.line_count as usize],
            selection_text_decoration.style,
            selection_text_decoration.color,
            thickness,
        );
        return sets;
    }

    let block_node = block;
    // https://drafts.csswg.org/css-text-decor-4/#decorating-box
    // When specified on or propagated to an inline box, that box becomes a decorating box for that decoration,
    // applying the decoration to all its box fragments.
    let mut stopped_at_boundary = false;
    let mut ancestor = Some(text_parent);
    while let Some(current) = ancestor {
        if current == block_node {
            break;
        }
        if let Some(style) = arena.node_style_if_live(current) {
            let lines = text_decoration_lines(style);
            if !lines.is_empty() {
                let glyph_height = first_available_font(arena, current).map_or(CssPixels::from_raw(0), |font| {
                    CssPixels::nearest_value_for_f32(font.pixel_size())
                });
                let thickness = resolve_text_decoration_thickness(arena, current, current, glyph_height);
                let text_reset = style.text_reset();
                push_set(
                    current,
                    lines,
                    text_reset.text_decoration_style,
                    text_reset.text_decoration_color,
                    thickness,
                );
            }
        }
        if crate::painting::style_queries::is_text_decoration_propagation_boundary(arena, current) {
            stopped_at_boundary = true;
            break;
        }
        ancestor = arena.node_parent_if_live(current);
    }

    if !stopped_at_boundary {
        // https://drafts.csswg.org/css-text-decor-4/#line-decoration
        // When specified on or propagated to a block container that establishes an inline formatting context,
        // the decorations are propagated to an anonymous inline box that wraps all the in-flow inline-level
        // children of the block container.
        // NB: The anonymous inline box inherits from the containing block, so these decorations derive their
        //     geometry from it.
        let mut block_glyph_height: Option<CssPixels> = None;
        let mut propagated = Some(block_node);
        while let Some(current) = propagated {
            if let Some(style) = arena.node_style_if_live(current) {
                let lines = text_decoration_lines(style);
                if !lines.is_empty() {
                    let glyph_height = *block_glyph_height.get_or_insert_with(|| {
                        first_available_font(arena, block_node).map_or(CssPixels::from_raw(0), |font| {
                            CssPixels::nearest_value_for_f32(font.pixel_size())
                        })
                    });
                    let thickness = resolve_text_decoration_thickness(arena, current, block_node, glyph_height);
                    let text_reset = style.text_reset();
                    push_set(
                        block_node,
                        lines,
                        text_reset.text_decoration_style,
                        text_reset.text_decoration_color,
                        thickness,
                    );
                }
            }
            if crate::painting::style_queries::is_text_decoration_propagation_boundary(arena, current) {
                break;
            }
            propagated = arena.node_parent_if_live(current);
        }
    }
    sets
}

#[derive(Clone, Copy)]
struct DecorationSegment {
    start_x: i32,
    end_x: i32,
}

// https://drafts.csswg.org/css-text-decor-4/#text-decoration-skip-ink-property
#[allow(clippy::too_many_arguments)]
fn compute_skip_ink_segments(
    recorder: &mut PaintRecorder<'_>,
    block: NodeSlotId,
    fragment_index: u32,
    span_start_x: i32,
    span_end_x: i32,
    line_y: i32,
    line_thickness: i32,
    font_size: f32,
) -> Vec<DecorationSegment> {
    let fragment = &recorder.layout_arena.paintable_side_data(block).fragments[fragment_index as usize];
    let Some(run) = &fragment.glyph_run else {
        return vec![DecorationSegment {
            start_x: span_start_x,
            end_x: span_end_x,
        }];
    };
    // The text blob is drawn at baseline_start on the canvas. Compute that same origin so we can convert between
    // device-pixel coordinates and blob-local coordinates.
    let scale = recorder.inputs.device_pixels_per_css_pixel;
    let fragment_absolute_rect = crate::painting::text_fragment::absolute_rect(recorder.layout_arena, fragment);
    let blob_origin_x = fragment_absolute_rect.x.to_float() * scale as f32;
    let blob_origin_y = (fragment_absolute_rect.y.to_float() + fragment.baseline.to_float()) * scale as f32;

    // Convert the underline's y-band from device pixels to blob-local coordinates.
    let half_thickness = line_thickness as f32 / 2.0;
    let y_top = line_y as f32 - half_thickness - blob_origin_y;
    let y_bottom = line_y as f32 + half_thickness - blob_origin_y;

    let intervals = run.glyph_intercepts(scale as f32, y_top, y_bottom);
    if intervals.is_empty() {
        return vec![DecorationSegment {
            start_x: span_start_x,
            end_x: span_end_x,
        }];
    }

    // Use the full fragment's X range for gap computation so intercepts aren't cut off at span boundaries.
    let full_fragment_rect = recorder.converter.rounded_device_rect(fragment_absolute_rect);
    let fragment_start_x = full_fragment_rect.x;

    // Convert intercepts from blob-local x to device pixels, and dilate to create visible gaps.
    let dilation = (font_size / 20.0).max(2.0) * scale as f32;

    let mut segments = Vec::new();
    let mut current_x = fragment_start_x;
    let mut i = 0;
    while i + 1 < intervals.len() {
        let gap_start = (intervals[i] + blob_origin_x - dilation).floor() as i32;
        let gap_end = (intervals[i + 1] + blob_origin_x + dilation).ceil() as i32;
        let seg_start = current_x.max(span_start_x);
        let seg_end = gap_start.min(span_end_x);
        if seg_start < seg_end {
            segments.push(DecorationSegment {
                start_x: seg_start,
                end_x: seg_end,
            });
        }
        current_x = gap_end.max(current_x);
        i += 2;
    }
    let seg_start = current_x.max(span_start_x);
    if seg_start < span_end_x {
        segments.push(DecorationSegment {
            start_x: seg_start,
            end_x: span_end_x,
        });
    }
    segments
}

fn build_triangle_wave_path(from: IntPoint, to: IntPoint, amplitude: f32) -> PathBuilder {
    let mut path = PathBuilder::new();
    if from.y != to.y {
        return path;
    }
    path.move_to(from.x as f32, from.y as f32);

    let wavelength = amplitude * 2.0;
    let half_wavelength = amplitude;
    let quarter_wavelength = amplitude / 2.0;

    let mut position_x = from.x as f32;
    let position_y = from.y as f32;
    let mut remaining = (to.x as f32 - position_x).abs();
    while remaining > wavelength {
        // Draw a whole wave
        path.line_to(position_x + quarter_wavelength, position_y - quarter_wavelength);
        path.line_to(
            position_x + quarter_wavelength + half_wavelength,
            position_y + quarter_wavelength,
        );
        path.line_to(position_x + wavelength, position_y);
        position_x += wavelength;
        remaining = (to.x as f32 - position_x).abs();
    }

    // Up
    if remaining > quarter_wavelength {
        path.line_to(position_x + quarter_wavelength, position_y - quarter_wavelength);
        position_x += quarter_wavelength;
        remaining = (to.x as f32 - position_x).abs();
    } else if remaining >= 1.0 {
        let fraction = remaining / quarter_wavelength;
        path.line_to(
            position_x + (fraction * quarter_wavelength),
            position_y - (fraction * quarter_wavelength),
        );
        remaining = 0.0;
    }

    // Down
    if remaining > half_wavelength {
        path.line_to(position_x + half_wavelength, position_y + quarter_wavelength);
        position_x += half_wavelength;
        remaining = (to.x as f32 - position_x).abs();
    } else if remaining >= 1.0 {
        let fraction = remaining / half_wavelength;
        path.line_to(
            position_x + (fraction * half_wavelength),
            position_y - quarter_wavelength + (fraction * half_wavelength),
        );
        remaining = 0.0;
    }

    // Back to middle
    if remaining >= 1.0 {
        let fraction = remaining / quarter_wavelength;
        path.line_to(
            position_x + (fraction * quarter_wavelength),
            position_y + ((1.0 - fraction) * quarter_wavelength),
        );
    }
    path
}

pub(crate) fn paint_decoration_lines(
    recorder: &mut PaintRecorder<'_>,
    block: NodeSlotId,
    fragment_index: u32,
    fragment_box: CssPixelRect,
    set: &TextDecorationSet,
) {
    let converter = recorder.converter;
    let glyph_height = CssPixels::nearest_value_for_f32(set.decorating_font_pixel_size);
    let baseline = set.baseline;
    let two = CssPixels::from_integer(2);

    for index in 0..set.line_count as usize {
        let mut line = set.lines[index];
        let mut line_thickness = set.thickness;
        let mut line_style = set.style;
        let mut line_color = Color(set.color);
        let mut text_underline_offset = set.text_underline_offset;

        if line == text_decoration_line::SPELLING_ERROR {
            // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-line-spelling-error
            // This value indicates the type of text decoration used by the user agent to highlight spelling mistakes.
            // Its appearance is UA-defined, and may be platform-dependent. It is often rendered as a red wavy underline.
            line_color = Color::from_rgb(255, 0, 0);
            line_thickness = CssPixels::from_integer(1);
            line_style = text_decoration_style::WAVY;
            line = text_decoration_line::UNDERLINE;
            // https://drafts.csswg.org/css-text-decor-4/#underline-offset
            // When the value of the text-decoration-line property is either spelling-error or grammar-error, the UA
            // must ignore the value of text-underline-position.
            text_underline_offset = CssPixels::from_integer(2);
        } else if line == text_decoration_line::GRAMMAR_ERROR {
            // https://drafts.csswg.org/css-text-decor-4/#valdef-text-decoration-line-grammar-error
            // This value indicates the type of text decoration used by the user agent to highlight grammar mistakes.
            // Its appearance is UA defined, and may be platform-dependent. It is often rendered as a green wavy underline.
            line_color = Color::from_rgb(0, 128, 0);
            line_thickness = CssPixels::from_integer(1);
            line_style = text_decoration_style::WAVY;
            line = text_decoration_line::UNDERLINE;
            // https://drafts.csswg.org/css-text-decor-4/#underline-offset
            // When the value of the text-decoration-line property is either spelling-error or grammar-error, the UA
            // must ignore the value of text-underline-position.
            text_underline_offset = CssPixels::from_integer(2);
        }

        let device_line_thickness = converter.rounded_device_pixels(line_thickness);

        // Compute the center Y of the decoration stroke. For underline and overline, offset by half the thickness
        // so the near edge of the stroke aligns with the intended position.
        let line_center_y = match line {
            text_decoration_line::NONE => return,
            text_decoration_line::UNDERLINE => {
                // https://drafts.csswg.org/css-text-decor-4/#text-underline-position-property
                // FIXME: Support text-decoration: underline on vertical text
                // The user agent may use any algorithm to determine the underline’s position; however it must be
                // placed at or under the alphabetic baseline.

                // Spec Note: It is suggested that the default underline position be close to the alphabetic
                //            baseline,
                // FIXME:     unless that would either cross subscripted (or otherwise lowered) text or draw over
                //            glyphs from Asian scripts such as Han or Tibetan for which an alphabetic underline is
                //            too high: in such cases, shifting the underline lower or aligning to the em box edge
                //            as described for under may be more appropriate.
                // FIXME: If the first available font has metrics indicating a preferred underline offset, use that
                //        offset, otherwise behaves as auto.
                // The underline is positioned under the element’s text content. In this case the underline usually
                // does not cross the descenders. (This is sometimes called “accounting” underline.)
                let underline_top_edge = match set.text_underline_position_horizontal {
                    text_underline_position_horizontal::UNDER => {
                        baseline + CssPixels::nearest_value_for_f32(set.decorating_font_descent) + text_underline_offset
                    }
                    _ => baseline + text_underline_offset,
                };
                underline_top_edge + line_thickness.div_as_fraction(two)
            }
            text_decoration_line::OVERLINE => baseline - glyph_height - line_thickness.div_as_fraction(two),
            text_decoration_line::LINE_THROUGH => {
                let x_height = CssPixels::from_integer(set.decorating_font_x_height as i64);
                baseline - x_height * CssPixels::nearest_value_for_f32(0.5)
            }
            // Blink: conforming user agents may simply not blink the text.
            _ => return,
        };

        let line_start_point = converter.rounded_device_point(
            fragment_box
                .location()
                .translated(CssPixels::from_raw(0), line_center_y),
        );
        let line_end_point = converter.rounded_device_point(
            crate::css::css_pixels::CssPixelPoint::new(fragment_box.right(), fragment_box.y)
                .translated(CssPixels::from_raw(0), line_center_y),
        );

        // https://drafts.csswg.org/css-text-decor-4/#text-decoration-skip-ink-property
        // FIXME: For text-decoration-skip-ink: auto, skip CJK ideographs and symbols from the intercept
        //        computation, since their complex strokes would create too many gaps in the decoration line.
        // NB: Ink is only skipped for decorations anchored at this fragment's own baseline. A fragment that is
        //     vertically shifted relative to a propagated decoration must not carve gaps into the decorating
        //     box's line.
        let should_skip_ink = set.skip_ink != text_decoration_skip_ink::NONE
            && set.anchored_at_fragment_baseline
            && (line == text_decoration_line::UNDERLINE || line == text_decoration_line::OVERLINE);

        let segments = if !should_skip_ink {
            vec![DecorationSegment {
                start_x: line_start_point.x,
                end_x: line_end_point.x,
            }]
        } else {
            compute_skip_ink_segments(
                recorder,
                block,
                fragment_index,
                line_start_point.x,
                line_end_point.x,
                line_start_point.y,
                device_line_thickness,
                set.text_font_pixel_size,
            )
        };

        let line_y = line_start_point.y;
        let draw_line_for_segment =
            |recorder: &mut PaintRecorder<'_>, segment: DecorationSegment, y: i32, style: LineStyle| {
                recorder.recorder.draw_line(
                    IntPoint { x: segment.start_x, y },
                    IntPoint { x: segment.end_x, y },
                    line_color,
                    device_line_thickness,
                    style,
                    Color::TRANSPARENT,
                );
            };

        match line_style {
            text_decoration_style::SOLID => {
                for segment in &segments {
                    draw_line_for_segment(recorder, *segment, line_y, LineStyle::Solid);
                }
            }
            text_decoration_style::DOUBLE => {
                // Two parallel lines with a 1px gap, expanding away from the text.
                let step = device_line_thickness + 1;
                let mut first_y = line_y;
                let mut second_y = line_y;
                match line {
                    text_decoration_line::UNDERLINE => second_y += step,
                    text_decoration_line::OVERLINE => second_y -= step,
                    _ => {
                        first_y -= step / 2;
                        second_y = first_y + step;
                    }
                }
                for segment in &segments {
                    draw_line_for_segment(recorder, *segment, first_y, LineStyle::Solid);
                    draw_line_for_segment(recorder, *segment, second_y, LineStyle::Solid);
                }
            }
            text_decoration_style::DASHED => {
                for segment in &segments {
                    draw_line_for_segment(recorder, *segment, line_y, LineStyle::Dashed);
                }
            }
            text_decoration_style::DOTTED => {
                for segment in &segments {
                    draw_line_for_segment(recorder, *segment, line_y, LineStyle::Dotted);
                }
            }
            _ => {
                // Wavy: the wave oscillates amplitude/2 above and below its center.
                let amplitude = device_line_thickness * 3;
                let mut wave_y = line_y;
                match line {
                    text_decoration_line::UNDERLINE => wave_y += device_line_thickness / 2 + 1,
                    text_decoration_line::OVERLINE => wave_y -= device_line_thickness / 2 + 1,
                    _ => {}
                }
                for segment in &segments {
                    let from = IntPoint {
                        x: segment.start_x,
                        y: wave_y,
                    };
                    let to = IntPoint {
                        x: segment.end_x,
                        y: wave_y,
                    };
                    let path = build_triangle_wave_path(from, to, amplitude as f32).build();
                    recorder.recorder.stroke_path(StrokePathParams {
                        cap_style: CapStyle::Round,
                        join_style: JoinStyle::Round,
                        miter_limit: 0.0,
                        dash_array: Vec::new(),
                        dash_offset: 0.0,
                        path: &path,
                        opacity: 1.0,
                        paint_style_or_color: PaintStyleOrColor::Color(line_color),
                        thickness: device_line_thickness as f32,
                        should_anti_alias: ShouldAntiAlias::Yes,
                    });
                }
            }
        }
    }
}
