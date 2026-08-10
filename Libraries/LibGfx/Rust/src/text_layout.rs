/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::font::FontRef;
use std::ffi::c_void;
use std::ptr::NonNull;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
pub enum TextType {
    Common,
    ContextDependent,
    EndPadding,
    Ltr,
    Rtl,
}

impl TryFrom<u8> for TextType {
    type Error = ();

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(Self::Common),
            1 => Ok(Self::ContextDependent),
            2 => Ok(Self::EndPadding),
            3 => Ok(Self::Ltr),
            4 => Ok(Self::Rtl),
            _ => Err(()),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct DrawGlyph {
    pub x: f32,
    pub y: f32,
    pub length_in_code_units: usize,
    pub glyph_width: f32,
    pub glyph_id: u32,
    pub should_paint: bool,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct ShapedRunView {
    pub glyphs: *const DrawGlyph,
    pub glyph_count: usize,
    pub width: f32,
    pub trailing_whitespace_length_in_code_units: usize,
    pub trailing_whitespace_advance: f32,
    pub retained: *mut c_void,
}

unsafe extern "C" {
    fn ladybird_gfx_shape_text(
        font: *const c_void,
        text_utf16: *const u16,
        length_in_code_units: usize,
        text_type: TextType,
        baseline_start_x: f32,
        letter_spacing: f32,
        word_spacing: f32,
    ) -> ShapedRunView;

    fn ladybird_gfx_glyph_run_unref(retained: *mut c_void);
}

struct RetainedGlyphRun {
    raw: NonNull<c_void>,
}

impl Drop for RetainedGlyphRun {
    fn drop(&mut self) {
        // SAFETY: The shaping entry point returned one retained GlyphRun
        // reference, and this guard releases it exactly once.
        unsafe { ladybird_gfx_glyph_run_unref(self.raw.as_ptr()) };
    }
}

#[derive(Debug)]
pub struct ShapedText {
    glyphs: Vec<DrawGlyph>,
    width: f32,
    trailing_whitespace_length_in_code_units: usize,
    trailing_whitespace_advance: f32,
}

impl ShapedText {
    #[inline]
    pub fn glyphs(&self) -> &[DrawGlyph] {
        &self.glyphs
    }

    #[inline]
    pub fn width(&self) -> f32 {
        self.width
    }

    #[inline]
    pub fn trailing_whitespace_length_in_code_units(&self) -> usize {
        self.trailing_whitespace_length_in_code_units
    }

    #[inline]
    pub fn trailing_whitespace_advance(&self) -> f32 {
        self.trailing_whitespace_advance
    }
}

pub fn shape_text(
    font: FontRef<'_>,
    text: &[u16],
    text_type: TextType,
    baseline_start_x: f32,
    letter_spacing: f32,
    word_spacing: f32,
) -> ShapedText {
    // SAFETY: FontRef keeps the font live, and the text slice remains valid
    // for the duration of the synchronous shaping call.
    let view = unsafe {
        ladybird_gfx_shape_text(
            font.as_ptr(),
            text.as_ptr(),
            text.len(),
            text_type,
            baseline_start_x,
            letter_spacing,
            word_spacing,
        )
    };
    let retained = RetainedGlyphRun {
        raw: NonNull::new(view.retained).expect("Gfx::shape_text must return a retained GlyphRun"),
    };
    assert!(view.glyph_count == 0 || !view.glyphs.is_null());
    let glyphs = if view.glyph_count == 0 {
        Vec::new()
    } else {
        // SAFETY: retained keeps the GlyphRun and its glyph storage live while
        // the view is copied into Rust-owned storage.
        unsafe { std::slice::from_raw_parts(view.glyphs, view.glyph_count) }.to_vec()
    };
    let width = view.width;
    let trailing_whitespace_length_in_code_units = view.trailing_whitespace_length_in_code_units;
    let trailing_whitespace_advance = view.trailing_whitespace_advance;
    drop(retained);
    ShapedText {
        glyphs,
        width,
        trailing_whitespace_length_in_code_units,
        trailing_whitespace_advance,
    }
}
