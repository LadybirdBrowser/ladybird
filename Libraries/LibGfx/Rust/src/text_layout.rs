/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::font::FontRef;
use std::ffi::c_void;

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
}

unsafe extern "C" {
    fn ladybird_gfx_shape_text(
        font: *const c_void,
        text_utf16: *const u16,
        length_in_code_units: usize,
        text_type: TextType,
        letter_spacing: f32,
        word_spacing: f32,
    ) -> ShapedRunView;

    fn ladybird_gfx_glyph_run_bounding_box(
        font: *const c_void,
        glyphs: *const DrawGlyph,
        glyph_count: usize,
        scale: f32,
        out_rect: *mut f32,
    );

    fn ladybird_gfx_glyph_run_glyph_intercepts(
        font: *const c_void,
        glyphs: *const DrawGlyph,
        glyph_count: usize,
        scale: f32,
        y_top: f32,
        y_bottom: f32,
        sink: *mut c_void,
        push: unsafe extern "C" fn(*mut c_void, f32),
    );
}

#[must_use]
pub fn glyph_run_bounding_box(font: FontRef<'_>, glyphs: &[DrawGlyph], scale: f32) -> [f32; 4] {
    let mut out_rect = [0.0f32; 4];
    // SAFETY: FontRef keeps the font live and the glyph slice stays valid for
    // the synchronous call, which fills the four floats out_rect points at.
    unsafe {
        ladybird_gfx_glyph_run_bounding_box(
            font.as_ptr(),
            glyphs.as_ptr(),
            glyphs.len(),
            scale,
            out_rect.as_mut_ptr(),
        );
    }
    out_rect
}

#[must_use]
pub fn glyph_run_glyph_intercepts(
    font: FontRef<'_>,
    glyphs: &[DrawGlyph],
    scale: f32,
    y_top: f32,
    y_bottom: f32,
) -> Vec<f32> {
    unsafe extern "C" fn push(sink: *mut c_void, value: f32) {
        // SAFETY: `sink` is the Vec passed below, live for the synchronous call.
        unsafe { (*sink.cast::<Vec<f32>>()).push(value) };
    }
    let mut intercepts: Vec<f32> = Vec::new();
    // SAFETY: FontRef keeps the font live and the glyph slice stays valid for
    // the synchronous call; the push callback runs against the local Vec.
    unsafe {
        ladybird_gfx_glyph_run_glyph_intercepts(
            font.as_ptr(),
            glyphs.as_ptr(),
            glyphs.len(),
            scale,
            y_top,
            y_bottom,
            (&raw mut intercepts).cast(),
            push,
        );
    }
    intercepts
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
    pub fn into_glyphs(self) -> Vec<DrawGlyph> {
        self.glyphs
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
            letter_spacing,
            word_spacing,
        )
    };
    assert!(view.glyph_count == 0 || !view.glyphs.is_null());
    let cached_glyphs = if view.glyph_count == 0 {
        &[]
    } else {
        // SAFETY: The view points into the font's shaping cache, which nothing
        // mutates before this synchronous copy into Rust-owned storage is done.
        unsafe { std::slice::from_raw_parts(view.glyphs, view.glyph_count) }
    };
    let glyphs = if baseline_start_x == 0.0 {
        cached_glyphs.to_vec()
    } else {
        cached_glyphs
            .iter()
            .map(|glyph| DrawGlyph {
                x: glyph.x + baseline_start_x,
                ..*glyph
            })
            .collect()
    };
    ShapedText {
        glyphs,
        width: view.width,
        trailing_whitespace_length_in_code_units: view.trailing_whitespace_length_in_code_units,
        trailing_whitespace_advance: view.trailing_whitespace_advance,
    }
}
