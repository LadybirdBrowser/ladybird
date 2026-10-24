/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The text of a 2D context: shaping it across the fonts of the cascade of the font of the context, laying it out
//! as the text preparation algorithm does, and its outlines.

use libgfx_rust::font::{EmojiPresentation, FontCascadeListHandle, FontHandle};
use libgfx_rust::path::{GlyphRun, Path};
use libgfx_rust::text_layout::{DrawGlyph, TextType, shape_text as shape_text_in_font};
use libgfx_rust::{AffineTransform, FloatPoint};

use super::context::multiply_transforms;
use super::drawing_state::DrawingState;
use crate::html::canvas_rendering_context2d::{CanvasTextAlign, CanvasTextBaseline};

/// A run of the text that one font of the cascade shapes.
pub(crate) struct ShapedRun {
    pub(crate) font: FontHandle,
    pub(crate) glyphs: Vec<DrawGlyph>,
    pub(crate) width: f32,
}

/// The code point at the index of the code units and how many code units it takes, as AK::Utf16View iterates them: a
/// surrogate that is not part of a pair is a code point of its own.
fn code_point_at(text: &[u16], index: usize) -> (u32, usize) {
    let code_unit = text[index];
    if (0xD800..0xDC00).contains(&code_unit)
        && let Some(&next_code_unit) = text.get(index + 1)
        && (0xDC00..0xE000).contains(&next_code_unit)
    {
        let code_point = 0x10000 + ((u32::from(code_unit) - 0xD800) << 10) + (u32::from(next_code_unit) - 0xDC00);
        return (code_point, 2);
    }
    (u32::from(code_unit), 1)
}

/// The text shaped from the baseline start, with a run for each part of it that one font of the cascade draws, as
/// Gfx::shape_text() shapes it across a Gfx::FontCascadeList.
pub(crate) fn shape_text(
    cascade: &FontCascadeListHandle,
    text: &[u16],
    baseline_start: FloatPoint,
    letter_spacing: f32,
    word_spacing: f32,
) -> Vec<ShapedRun> {
    const TEXT_PRESENTATION: EmojiPresentation = EmojiPresentation {
        is_emoji: false,
        forced: false,
    };

    let mut runs = Vec::new();
    if text.is_empty() {
        return runs;
    }

    let mut last_position = baseline_start;
    let mut add_run = |text: &[u16], font: &FontHandle| {
        let shaped = shape_text_in_font(font, text, TextType::Common, 0.0, letter_spacing, word_spacing);
        let mut glyphs = shaped.glyphs().to_vec();
        if last_position.x != 0.0 || last_position.y != 0.0 {
            for glyph in &mut glyphs {
                glyph.x += last_position.x;
                glyph.y += last_position.y;
            }
        }
        last_position.x += shaped.width();
        runs.push(ShapedRun {
            font: font.clone(),
            glyphs,
            width: shaped.width(),
        });
    };

    let (first_code_point, _) = code_point_at(text, 0);
    let mut last_font = cascade.font_for_code_point(first_code_point, TEXT_PRESENTATION, None);
    let mut run_start = 0;
    let mut index = 0;
    while index < text.len() {
        let (code_point, length) = code_point_at(text, index);
        let font = cascade.font_for_code_point(code_point, TEXT_PRESENTATION, Some(&last_font));
        if font.as_raw() != last_font.as_raw() {
            add_run(&text[run_start..index], &last_font);
            last_font = font;
            run_start = index;
        }
        index += length;
    }
    if run_start < text.len() {
        add_run(&text[run_start..], &last_font);
    }
    runs
}

/// The outlines of the glyphs of the runs, as one path.
pub(crate) fn path_of_runs(runs: &[ShapedRun]) -> Path {
    let glyph_runs: Vec<GlyphRun> = runs
        .iter()
        .map(|run| GlyphRun {
            font: run.font.clone(),
            glyphs: run.glyphs.clone(),
        })
        .collect();
    Path::from_glyph_runs(&glyph_runs)
}

/// The distance from the alphabetic baseline down to the baseline that textBaseline names, in the metrics of the
/// first font of the cascade, https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textbaseline
pub(crate) fn baseline_offset(text_baseline: CanvasTextBaseline, first_font: &FontHandle) -> f32 {
    let ascent = first_font.facts().ascent;
    let descent = first_font.facts().descent;
    match text_baseline {
        CanvasTextBaseline::Top => ascent,
        CanvasTextBaseline::Hanging => ascent * 0.8,
        CanvasTextBaseline::Middle => (ascent - descent) / 2.0,
        CanvasTextBaseline::Alphabetic => 0.0,
        CanvasTextBaseline::Ideographic | CanvasTextBaseline::Bottom => -descent,
    }
}

fn translation(x: f32, y: f32) -> AffineTransform {
    AffineTransform::new(1.0, 0.0, 0.0, 1.0, x, y)
}

/// How far the left edge of an inline box of the width is to the right of the anchor point, which textAlign and
/// the direction of the text determine, https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textalign
pub(crate) fn horizontal_alignment_offset(text_align: CanvasTextAlign, is_rtl: bool, text_width: f32) -> f32 {
    // The direction affects how "start" and "end" are interpreted:
    // - "ltr": start=left, end=right
    // - "rtl": start=right, end=left
    match text_align {
        // Center alignment is the same regardless of direction.
        CanvasTextAlign::Center => -text_width / 2.0,
        // In RTL, "start" means right-aligned. In LTR, it means left-aligned, which needs no translation.
        CanvasTextAlign::Start if is_rtl => -text_width,
        // In LTR, "end" means right-aligned. In RTL, it means left-aligned, which needs no translation.
        CanvasTextAlign::End if !is_rtl => -text_width,
        // Explicit "left" and "right" alignments ignore direction.
        CanvasTextAlign::Right => -text_width,
        CanvasTextAlign::Start | CanvasTextAlign::End | CanvasTextAlign::Left => 0.0,
    }
}

/// Where text of the width goes relative to the point that it is drawn at, and how much it is condensed to fit in the
/// maximum width. The text starts at x, around which it is condensed.
#[allow(
    clippy::cast_possible_truncation,
    reason = "the transform is in floats, as the C++ had it"
)]
pub(crate) fn text_transform(
    state: &DrawingState,
    is_rtl: bool,
    first_font: &FontHandle,
    x: f32,
    mut text_width: f32,
    max_width: Option<f64>,
) -> AffineTransform {
    let mut transform = AffineTransform::identity();

    // https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm:
    // 9. If maxWidth was provided and the hypothetical width of the inline box in the hypothetical line box is greater
    //    than maxWidth CSS pixels, then change font to have a more condensed font (if one is available or if a
    //    reasonably readable one can be synthesized by applying a horizontal scale factor to the font) or a smaller
    //    font, and return to the previous step.
    if let Some(max_width) = max_width
        && text_width > max_width as f32
    {
        let horizontal_scale = max_width as f32 / text_width;
        transform = AffineTransform::new(horizontal_scale, 0.0, 0.0, 1.0, x - horizontal_scale * x, 0.0);
        text_width *= horizontal_scale;
    }

    // Apply text align
    let alignment_offset = horizontal_alignment_offset(state.text_align, is_rtl, text_width);
    if alignment_offset != 0.0 {
        transform = multiply_transforms(translation(alignment_offset, 0.0), transform);
    }

    // Apply text baseline
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textbaseline
    let baseline_y_offset = baseline_offset(state.text_baseline, first_font);
    if baseline_y_offset != 0.0 {
        transform = multiply_transforms(translation(0.0, baseline_y_offset), transform);
    }

    transform
}

/// https://infra.spec.whatwg.org/#ascii-whitespace
fn is_ascii_whitespace(code_unit: u16) -> bool {
    matches!(code_unit, 0x09 | 0x0A | 0x0C | 0x0D | 0x20)
}

/// The text with every ASCII whitespace replaced by a U+0020 SPACE, step 2 of the text preparation algorithm.
pub(crate) fn replace_ascii_whitespace(text: &[u16]) -> Vec<u16> {
    text.iter()
        .map(|&code_unit| {
            if is_ascii_whitespace(code_unit) {
                0x20
            } else {
                code_unit
            }
        })
        .collect()
}
