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

pub(crate) fn shape_text_with_font(
    font: *const c_void,
    text: &[u16],
    text_type: u8,
    baseline_start_x: f32,
    letter_spacing: f32,
    word_spacing: f32,
) -> libgfx_rust::text_layout::ShapedText {
    // SAFETY: Font pointers in layout snapshots are borrowed from the host for
    // the synchronous layout pass.
    let font = unsafe { libgfx_rust::font::FontRef::from_raw(font) };
    let text_type = libgfx_rust::text_layout::TextType::try_from(text_type).expect("invalid Gfx::GlyphRun::TextType");
    libgfx_rust::text_layout::shape_text(font, text, text_type, baseline_start_x, letter_spacing, word_spacing)
}
