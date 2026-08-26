/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

#[inline]
pub(crate) fn font_glyph_width(font: *const c_void, code_point: u32) -> f32 {
    // SAFETY: Font pointers in layout snapshots are borrowed from the host for
    // the synchronous layout pass.
    unsafe { libgfx_rust::font::FontRef::from_raw(font) }.glyph_width(code_point)
}

#[inline]
pub(crate) fn font_glyph_id(font: *const c_void, code_point: u32) -> u32 {
    // SAFETY: Font pointers in layout snapshots are borrowed from the host for
    // the synchronous layout pass.
    unsafe { libgfx_rust::font::FontRef::from_raw(font) }.glyph_id_for_code_point(code_point)
}

pub(crate) struct ShapedRun {
    pub(crate) glyphs: Vec<inline_level_iterator::FfiDrawGlyph>,
    pub(crate) width: f32,
    pub(crate) trailing_whitespace_length_in_code_units: usize,
    pub(crate) trailing_whitespace_advance: f32,
}

pub(crate) fn shape_text_with_font(
    font: *const c_void,
    text: &[u16],
    text_type: u8,
    baseline_start_x: f32,
    letter_spacing: f32,
    word_spacing: f32,
) -> ShapedRun {
    // SAFETY: Font pointers in layout snapshots are borrowed from the host for
    // the synchronous layout pass.
    let font = unsafe { libgfx_rust::font::FontRef::from_raw(font) };
    let text_type = libgfx_rust::text_layout::TextType::try_from(text_type).expect("invalid Gfx::GlyphRun::TextType");
    let shaped =
        libgfx_rust::text_layout::shape_text(font, text, text_type, baseline_start_x, letter_spacing, word_spacing);
    let glyphs = shaped
        .glyphs()
        .iter()
        .map(|glyph| inline_level_iterator::FfiDrawGlyph {
            x: glyph.x,
            y: glyph.y,
            length_in_code_units: glyph.length_in_code_units,
            glyph_width: glyph.glyph_width,
            glyph_id: glyph.glyph_id,
            should_paint: glyph.should_paint,
        })
        .collect();
    ShapedRun {
        glyphs,
        width: shaped.width(),
        trailing_whitespace_length_in_code_units: shaped.trailing_whitespace_length_in_code_units(),
        trailing_whitespace_advance: shaped.trailing_whitespace_advance(),
    }
}
