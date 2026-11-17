/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::record::trace::Observer;

use crate::css::css_enums::{
    text_decoration_line, text_decoration_skip_ink, text_decoration_style, text_underline_position_horizontal,
};
use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::recorder::{PaintStyleOrColor, StrokePathParams};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::record::PaintRecorder;
use libgfx_rust::path::Path;
use libgfx_rust::{CapStyle, Color, FloatPoint, IntPoint, IntRect, JoinStyle, LineStyle, ShouldAntiAlias};

fn decoration_color(style: crate::css::computed_value_views::ComputedValuesView<'_>) -> u32 {
    let text = style.inherited_text();
    // https://drafts.csswg.org/css-text-decor-3/#text-decoration-color-property
    // AD-HOC: currentColor normally resolves to color. For compatibility with stroked text, use a visible explicit
    // stroke color, falling back to the text fill color.
    if style.text_reset().text_decoration_color.is_current_color && text.webkit_text_stroke_width > 0.0 {
        if !text.webkit_text_stroke_color.is_current_color && Color(text.webkit_text_stroke_color.rgba).alpha() > 0 {
            return text.webkit_text_stroke_color.rgba;
        }
        return text.webkit_text_fill_color;
    }
    style.text_decoration_color()
}

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
    pub decorating_font_point_size: i32,
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
    unsafe { ak::slice_from_raw_parts(list.pointer, list.length) }
}

fn first_available_font(
    arena: &impl PaintRead,
    node: crate::layout::node_data::NodeSlotId,
) -> Option<libgfx_rust::font::FontHandle> {
    Some(arena.node_style_if_live(node)?.first_available_font())
}

fn resolve_text_decoration_thickness(
    arena: &impl PaintRead,
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
    arena: &impl PaintRead,
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
    let side = arena.committed_side_data(block);
    for piece in side.inline_box_pieces() {
        if piece.node == decorating_node && piece.line_index == fragment.line_index {
            return (
                piece.baseline - (fragment.offset().1 + fragment.relpos_delta.y),
                fragment.accumulated_vertical_shift == piece.accumulated_vertical_shift,
            );
        }
    }
    let line = &side.lines()[fragment.line_index as usize];
    let line_baseline = line.block_start + line.baseline;
    (
        line_baseline - (fragment.offset().1 + fragment.relpos_delta.y),
        fragment.accumulated_vertical_shift == CssPixels::from_raw(0),
    )
}

pub(crate) fn fragment_may_paint_text_decorations(
    arena: &impl PaintRead,
    block: NodeSlotId,
    fragment: &crate::painting::paintable_data::FragmentRecord,
    block_chain_has_text_decoration_lines: &mut Option<bool>,
) -> bool {
    let has_text_decoration_lines = |node: NodeSlotId| {
        arena
            .node_style_if_live(node)
            .is_some_and(|style| !text_decoration_lines(style).is_empty())
    };
    let mut ancestor = Some(fragment.style_source);
    while let Some(current) = ancestor {
        if current == block {
            break;
        }
        if has_text_decoration_lines(current) {
            return true;
        }
        if crate::painting::style_queries::is_text_decoration_propagation_boundary(arena, current) {
            return false;
        }
        ancestor = arena.node_parent_if_live(current);
    }
    *block_chain_has_text_decoration_lines.get_or_insert_with(|| {
        let mut propagated = Some(block);
        while let Some(current) = propagated {
            if has_text_decoration_lines(current) {
                return true;
            }
            if crate::painting::style_queries::is_text_decoration_propagation_boundary(arena, current) {
                return false;
            }
            propagated = arena.node_parent_if_live(current);
        }
        false
    })
}

pub(crate) fn decoration_sets_for_span(
    arena: &impl PaintRead,
    block: NodeSlotId,
    span: &crate::painting::record::paint::text::RenderSpan,
) -> Vec<TextDecorationSet> {
    let mut sets: Vec<TextDecorationSet> = Vec::new();
    if span.start_code_unit == span.end_code_unit {
        return sets;
    }
    let side = arena.committed_side_data(block);
    let fragment = &side.fragments()[span.fragment_index as usize];
    let text_parent = fragment.style_source;

    let mut push_set =
        |decorating_node: crate::layout::node_data::NodeSlotId,
         lines: &[u8],
         style_code: u8,
         color: u32,
         thickness: CssPixels,
         highlight_metrics: Option<&crate::painting::record::paint::text::HighlightDecorationMetrics>| {
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
            let (baseline, anchored) = anchor_for_decorating_box(arena, block, fragment, decorating_node, text_parent);
            set.baseline = baseline;
            set.anchored_at_fragment_baseline = anchored;
            // https://drafts.csswg.org/css-text-decor-4/#text-line-constancy
            // However, for underlines and overlines the UA must use a single thickness and position on each line for the
            // decorations deriving from a single decorating box.
            let Some(decorating_font) = first_available_font(arena, decorating_node) else {
                return;
            };
            let text_font = if decorating_node == text_parent {
                decorating_font.clone()
            } else {
                let source = crate::painting::text_fragment::style_source(arena, fragment);
                first_available_font(arena, source).unwrap_or_else(|| decorating_font.clone())
            };
            set.decorating_font_pixel_size = decorating_font.facts().pixel_size;
            set.decorating_font_descent = decorating_font.facts().descent;
            set.decorating_font_point_size = decorating_font.facts().point_size as i32;
            set.text_font_pixel_size = text_font.facts().pixel_size;
            match highlight_metrics {
                Some(metrics) => {
                    set.text_underline_offset = metrics.underline_offset;
                    set.text_underline_position_horizontal = metrics.underline_position_horizontal;
                    set.skip_ink = metrics.skip_ink;
                }
                None => {
                    let inherited_text = decorating_style.inherited_text();
                    set.text_underline_offset = inherited_text.text_underline_offset.used_value;
                    set.text_underline_position_horizontal = inherited_text.text_underline_position.horizontal;
                    set.skip_ink = arena
                        .node_style_if_live(text_parent)
                        .map_or(0, |style| style.inherited_text().text_decoration_skip_ink);
                }
            }
            sets.push(set);
        };

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
                    CssPixels::nearest_value_for_f32(font.facts().pixel_size)
                });
                let thickness = resolve_text_decoration_thickness(arena, current, current, glyph_height);
                let text_reset = style.text_reset();
                push_set(
                    current,
                    lines,
                    text_reset.text_decoration_style,
                    span.decoration_color.unwrap_or_else(|| decoration_color(style)),
                    thickness,
                    None,
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
                            CssPixels::nearest_value_for_f32(font.facts().pixel_size)
                        })
                    });
                    let thickness = resolve_text_decoration_thickness(arena, current, block_node, glyph_height);
                    let text_reset = style.text_reset();
                    push_set(
                        block_node,
                        lines,
                        text_reset.text_decoration_style,
                        span.decoration_color.unwrap_or_else(|| decoration_color(style)),
                        thickness,
                        None,
                    );
                }
            }
            if crate::painting::style_queries::is_text_decoration_propagation_boundary(arena, current) {
                break;
            }
            propagated = arena.node_parent_if_live(current);
        }
    }

    // https://drafts.csswg.org/css-pseudo-4/#highlight-text
    // Any text decorations introduced by each highlight pseudo-element are stacked in the same order as their
    // backgrounds over the text's original decorations and are all drawn, each decoration in its own color.
    for text_decoration in span
        .highlights
        .iter()
        .filter_map(|overlay| overlay.text_decoration.as_ref())
    {
        let glyph_height = first_available_font(arena, text_parent).map_or(CssPixels::from_raw(0), |font| {
            CssPixels::nearest_value_for_f32(font.facts().pixel_size)
        });
        let thickness = match text_decoration.metrics {
            Some(metrics) => metrics
                .thickness
                .unwrap_or_else(|| glyph_height.scaled(0.1).max(CssPixels::from_integer(1))),
            None => resolve_text_decoration_thickness(arena, text_parent, text_parent, glyph_height),
        };
        push_set(
            text_parent,
            &text_decoration.lines[..text_decoration.line_count as usize],
            text_decoration.style,
            text_decoration.color,
            thickness,
            text_decoration.metrics.as_ref(),
        );
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
fn compute_skip_ink_segments<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    block: NodeSlotId,
    fragment_index: u32,
    span_start_x: i32,
    span_end_x: i32,
    band_top: i32,
    band_thickness: i32,
    font_size: f32,
) -> Vec<DecorationSegment> {
    let side = recorder.source.committed_side_data(block);
    let fragment = &side.fragments()[fragment_index as usize];
    let Some(run) = &fragment.glyph_run else {
        return vec![DecorationSegment {
            start_x: span_start_x,
            end_x: span_end_x,
        }];
    };
    // The text blob is drawn at baseline_start on the canvas. Compute that same origin so we can convert between
    // device-pixel coordinates and blob-local coordinates.
    let scale = recorder.inputs.device_pixels_per_css_pixel;
    let fragment_absolute_rect = crate::painting::text_fragment::absolute_rect(recorder.source, fragment);
    let blob_origin_x = fragment_absolute_rect.x.to_float() * scale as f32;
    let blob_origin_y = (fragment_absolute_rect.y.to_float() + fragment.baseline.to_float()) * scale as f32;

    // Convert the band of pixel rows the line paints from device pixels to blob-local coordinates.
    let y_top = band_top as f32 - blob_origin_y;
    let y_bottom = (band_top + band_thickness) as f32 - blob_origin_y;

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

fn build_triangle_wave_path(from: IntPoint, to: IntPoint, amplitude: f32) -> Path {
    let mut path = Path::new();
    if from.y != to.y {
        return path;
    }
    path.move_to(FloatPoint {
        x: from.x as f32,
        y: from.y as f32,
    });

    let wavelength = amplitude * 2.0;
    let half_wavelength = amplitude;
    let quarter_wavelength = amplitude / 2.0;

    let mut position_x = from.x as f32;
    let position_y = from.y as f32;
    let mut remaining = (to.x as f32 - position_x).abs();
    while remaining > wavelength {
        // Draw a whole wave
        path.line_to(FloatPoint {
            x: position_x + quarter_wavelength,
            y: position_y - quarter_wavelength,
        });
        path.line_to(FloatPoint {
            x: position_x + quarter_wavelength + half_wavelength,
            y: position_y + quarter_wavelength,
        });
        path.line_to(FloatPoint {
            x: position_x + wavelength,
            y: position_y,
        });
        position_x += wavelength;
        remaining = (to.x as f32 - position_x).abs();
    }

    // Up
    if remaining > quarter_wavelength {
        path.line_to(FloatPoint {
            x: position_x + quarter_wavelength,
            y: position_y - quarter_wavelength,
        });
        position_x += quarter_wavelength;
        remaining = (to.x as f32 - position_x).abs();
    } else if remaining >= 1.0 {
        let fraction = remaining / quarter_wavelength;
        path.line_to(FloatPoint {
            x: position_x + (fraction * quarter_wavelength),
            y: position_y - (fraction * quarter_wavelength),
        });
        remaining = 0.0;
    }

    // Down
    if remaining > half_wavelength {
        path.line_to(FloatPoint {
            x: position_x + half_wavelength,
            y: position_y + quarter_wavelength,
        });
        position_x += half_wavelength;
        remaining = (to.x as f32 - position_x).abs();
    } else if remaining >= 1.0 {
        let fraction = remaining / half_wavelength;
        path.line_to(FloatPoint {
            x: position_x + (fraction * half_wavelength),
            y: position_y - quarter_wavelength + (fraction * half_wavelength),
        });
        remaining = 0.0;
    }

    // Back to middle
    if remaining >= 1.0 {
        let fraction = remaining / quarter_wavelength;
        path.line_to(FloatPoint {
            x: position_x + (fraction * quarter_wavelength),
            y: position_y + ((1.0 - fraction) * quarter_wavelength),
        });
    }
    path
}

pub(crate) enum DecorationStrokeGeometry {
    Single(LineStyle),
    Double { first_top: i32, second_top: i32 },
    Wavy { center_y: i32, amplitude: i32 },
}

pub(crate) struct DecorationLineGeometry {
    pub start: IntPoint,
    pub end: IntPoint,
    // NB: The line's edges go on device pixel boundaries, so that a line one device pixel thick covers one row of
    //     pixels rather than half of each of two rows: Its top edge is rounded to a device pixel, and a solid line
    //     is filled as a rectangle from there, while a dotted or dashed one is stroked along the true center.
    pub top: i32,
    pub thickness: i32,
    pub color: Color,
    pub should_skip_ink: bool,
    pub stroke: DecorationStrokeGeometry,
}

struct CssDecorationLine {
    line: u8,
    thickness: CssPixels,
    style: u8,
    color: Color,
    center_y: CssPixels,
}

fn css_decoration_lines(set: &TextDecorationSet) -> Vec<CssDecorationLine> {
    let mut lines = Vec::new();
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

        // Compute the center Y of the decoration stroke. For underline and overline, offset by half the thickness
        // so the near edge of the stroke aligns with the intended position.
        let line_center_y = match line {
            text_decoration_line::NONE => break,
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
                let point_size = CssPixels::from_integer(set.decorating_font_point_size as i64);
                baseline - point_size * CssPixels::nearest_value_for_f32(0.5)
            }
            // Blink: conforming user agents may simply not blink the text.
            _ => break,
        };

        lines.push(CssDecorationLine {
            line,
            thickness: line_thickness,
            style: line_style,
            color: line_color,
            center_y: line_center_y,
        });
    }
    lines
}

pub(crate) fn decoration_ink_bounds(fragment_box: CssPixelRect, set: &TextDecorationSet) -> CssPixelRect {
    let mut bounds = CssPixelRect::default();
    for line in css_decoration_lines(set) {
        let reach = line.thickness * 3;
        let rect = CssPixelRect::new(
            fragment_box.x,
            fragment_box.y + line.center_y,
            fragment_box.width,
            CssPixels::default(),
        );
        bounds.unite(rect.inflated(reach, reach, reach, reach));
    }
    bounds
}

pub(crate) fn decoration_line_geometry(
    fragment_box: CssPixelRect,
    set: &TextDecorationSet,
    device_pixel_ratio: f64,
) -> Vec<DecorationLineGeometry> {
    let converter = crate::painting::display_list::device_pixels::DevicePixelConverter::new(device_pixel_ratio);
    let mut lines = Vec::new();
    for css_line in css_decoration_lines(set) {
        let CssDecorationLine {
            line,
            thickness: line_thickness,
            style: line_style,
            color: line_color,
            center_y: line_center_y,
        } = css_line;
        let device_line_thickness = converter.rounded_device_pixels(line_thickness);
        let top = converter
            .rounded_device_point(fragment_box.location().translated(
                CssPixels::from_raw(0),
                line_center_y - line_thickness.div_as_fraction(CssPixels::from_integer(2)),
            ))
            .y;
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

        let line_y = line_start_point.y;
        let stroke = match line_style {
            text_decoration_style::SOLID => DecorationStrokeGeometry::Single(LineStyle::Solid),
            text_decoration_style::DASHED => DecorationStrokeGeometry::Single(LineStyle::Dashed),
            text_decoration_style::DOTTED => DecorationStrokeGeometry::Single(LineStyle::Dotted),
            text_decoration_style::DOUBLE => {
                // Two parallel lines with a 1px gap, expanding away from the text.
                let step = device_line_thickness.max(1) + 1;
                let (first_top, second_top) = match line {
                    text_decoration_line::UNDERLINE => (top, top + step),
                    text_decoration_line::OVERLINE => (top - step, top),
                    _ => (top - step / 2, top - step / 2 + step),
                };
                DecorationStrokeGeometry::Double { first_top, second_top }
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
                DecorationStrokeGeometry::Wavy {
                    center_y: wave_y,
                    amplitude,
                }
            }
        };
        lines.push(DecorationLineGeometry {
            start: line_start_point,
            end: line_end_point,
            top,
            thickness: device_line_thickness,
            color: line_color,
            should_skip_ink,
            stroke,
        });
    }
    lines
}

pub(crate) fn paint_decoration_lines<O: Observer>(
    recorder: &mut PaintRecorder<'_, O>,
    block: NodeSlotId,
    fragment_index: u32,
    fragment_box: CssPixelRect,
    set: &TextDecorationSet,
) {
    for geometry in decoration_line_geometry(fragment_box, set, recorder.inputs.device_pixels_per_css_pixel) {
        let line_start_point = geometry.start;
        let line_end_point = geometry.end;
        let device_line_thickness = geometry.thickness;
        let top_y = geometry.top;
        let thickness = device_line_thickness.max(1);
        let line_color = geometry.color;
        let should_skip_ink = geometry.should_skip_ink;
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
                top_y,
                thickness,
                set.text_font_pixel_size,
            )
        };

        let fill_line = |recorder: &mut PaintRecorder<'_, O>, segment: DecorationSegment, top: i32| {
            recorder.recorder.fill_rect(
                IntRect {
                    x: segment.start_x,
                    y: top,
                    width: segment.end_x - segment.start_x,
                    height: thickness,
                },
                line_color,
                ForceDarkRole::Foreground,
            );
        };
        let stroke_line = |recorder: &mut PaintRecorder<'_, O>,
                           segment: DecorationSegment,
                           dash_array: Vec<f32>,
                           cap_style: CapStyle| {
            let center_y = top_y as f32 + thickness as f32 / 2.0;
            let mut path = Path::new();
            path.move_to(FloatPoint {
                x: segment.start_x as f32,
                y: center_y,
            });
            path.line_to(FloatPoint {
                x: segment.end_x as f32,
                y: center_y,
            });
            recorder.recorder.stroke_path(StrokePathParams {
                force_dark_role: ForceDarkRole::Foreground,
                cap_style,
                join_style: JoinStyle::Miter,
                miter_limit: 4.0,
                dash_array,
                dash_offset: 0.0,
                path: &path,
                opacity: 1.0,
                paint_style_or_color: PaintStyleOrColor::Color(line_color),
                thickness: thickness as f32,
                should_anti_alias: ShouldAntiAlias::Yes,
            });
        };

        match geometry.stroke {
            DecorationStrokeGeometry::Single(LineStyle::Dashed) => {
                let dash = (thickness * 2) as f32;
                for segment in &segments {
                    stroke_line(recorder, *segment, vec![dash, dash], CapStyle::Butt);
                }
            }
            DecorationStrokeGeometry::Single(LineStyle::Dotted) => {
                // Dots one line thickness across, one thickness apart. A dot narrower than 3 device pixels is square,
                // since antialiasing would wash out a round one that small.
                let size = thickness as f32;
                for segment in &segments {
                    if thickness < 3 {
                        stroke_line(recorder, *segment, vec![size, size], CapStyle::Butt);
                    } else {
                        stroke_line(recorder, *segment, vec![0.0, size * 2.0], CapStyle::Round);
                    }
                }
            }
            DecorationStrokeGeometry::Single(_) => {
                for segment in &segments {
                    fill_line(recorder, *segment, top_y);
                }
            }
            DecorationStrokeGeometry::Double { first_top, second_top } => {
                for segment in &segments {
                    fill_line(recorder, *segment, first_top);
                    fill_line(recorder, *segment, second_top);
                }
            }
            DecorationStrokeGeometry::Wavy {
                center_y: wave_y,
                amplitude,
            } => {
                for segment in &segments {
                    let from = IntPoint {
                        x: segment.start_x,
                        y: wave_y,
                    };
                    let to = IntPoint {
                        x: segment.end_x,
                        y: wave_y,
                    };
                    let path = build_triangle_wave_path(from, to, amplitude as f32);
                    recorder.recorder.stroke_path(StrokePathParams {
                        force_dark_role: ForceDarkRole::Foreground,
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
