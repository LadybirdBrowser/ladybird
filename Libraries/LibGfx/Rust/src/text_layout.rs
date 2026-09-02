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

unsafe extern "C" {
    fn ladybird_gfx_shape_text_uncached(
        font: *const c_void,
        text_utf16: *const u16,
        length_in_code_units: usize,
        text_type: TextType,
        letter_spacing: f32,
        word_spacing: f32,
        sink: *mut c_void,
        emit: unsafe extern "C" fn(*mut c_void, *const DrawGlyph, usize, f32, usize, f32),
    );

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

const SINGLE_ASCII_SHAPE_CACHE_SIZE: usize = 128;
const MAX_SHAPE_CACHE_FONTS: usize = 64;
const MAX_SHAPE_CACHE_TEXTS_PER_FONT: usize = 4096;

#[derive(Clone, Copy)]
struct ShapeParams {
    text_type: TextType,
    letter_spacing: f32,
    word_spacing: f32,
}

impl ShapeParams {
    // Spacing values are compared bitwise so that, like the C++ shaping cache
    // key, every distinct bit pattern gets its own entry.
    fn matches(&self, other: &ShapeParams) -> bool {
        self.text_type == other.text_type
            && self.letter_spacing.to_bits() == other.letter_spacing.to_bits()
            && self.word_spacing.to_bits() == other.word_spacing.to_bits()
    }
}

#[derive(Default)]
struct CachedShape {
    glyphs: Vec<DrawGlyph>,
    width: f32,
    trailing_whitespace_length_in_code_units: usize,
    trailing_whitespace_advance: f32,
}

struct ShapeForParams {
    params: ShapeParams,
    shape: CachedShape,
}

type FastMap<K, V> = std::collections::HashMap<K, V, foldhash::fast::RandomState>;

struct FontShapeCache {
    last_used_tick: u64,
    single_ascii_common_shapes: [Option<Box<CachedShape>>; SINGLE_ASCII_SHAPE_CACHE_SIZE],
    shapes_by_text: FastMap<Box<[u16]>, Vec<ShapeForParams>>,
}

impl Default for FontShapeCache {
    fn default() -> Self {
        Self {
            last_used_tick: 0,
            single_ascii_common_shapes: [const { None }; SINGLE_ASCII_SHAPE_CACHE_SIZE],
            shapes_by_text: FastMap::default(),
        }
    }
}

impl FontShapeCache {
    fn shape_for(
        &mut self,
        text: &[u16],
        params: ShapeParams,
        shape_uncached: impl FnOnce() -> CachedShape,
    ) -> &CachedShape {
        if let &[code_unit] = text
            && (code_unit as usize) < SINGLE_ASCII_SHAPE_CACHE_SIZE
            && params.text_type == TextType::Common
            && params.letter_spacing == 0.0
            && params.word_spacing == 0.0
        {
            return self.single_ascii_common_shapes[code_unit as usize]
                .get_or_insert_with(|| Box::new(shape_uncached()));
        }

        if !self.shapes_by_text.contains_key(text) {
            if self.shapes_by_text.len() >= MAX_SHAPE_CACHE_TEXTS_PER_FONT {
                self.shapes_by_text.clear();
            }
            self.shapes_by_text.insert(Box::from(text), Vec::new());
        }
        let shapes = self
            .shapes_by_text
            .get_mut(text)
            .expect("the entry was inserted just above");
        let index = shapes
            .iter()
            .position(|entry| entry.params.matches(&params))
            .unwrap_or_else(|| {
                shapes.push(ShapeForParams {
                    params,
                    shape: shape_uncached(),
                });
                shapes.len() - 1
            });
        &shapes[index].shape
    }
}

#[derive(Default)]
struct ShapingCache {
    caches_by_font_id: FastMap<u64, FontShapeCache>,
    tick: u64,
}

impl ShapingCache {
    fn shape_for(
        &mut self,
        font_id: u64,
        text: &[u16],
        params: ShapeParams,
        shape_uncached: impl FnOnce() -> CachedShape,
    ) -> &CachedShape {
        self.tick += 1;
        if !self.caches_by_font_id.contains_key(&font_id) && self.caches_by_font_id.len() >= MAX_SHAPE_CACHE_FONTS {
            self.evict_least_recently_used_font();
        }
        let font_cache = self.caches_by_font_id.entry(font_id).or_default();
        font_cache.last_used_tick = self.tick;
        font_cache.shape_for(text, params, shape_uncached)
    }

    fn evict_least_recently_used_font(&mut self) {
        let least_recently_used_font_id = self
            .caches_by_font_id
            .iter()
            .min_by_key(|(_, font_cache)| font_cache.last_used_tick)
            .map(|(font_id, _)| *font_id);
        if let Some(font_id) = least_recently_used_font_id {
            self.caches_by_font_id.remove(&font_id);
        }
    }
}

thread_local! {
    static SHAPING_CACHE: std::cell::RefCell<ShapingCache> = std::cell::RefCell::new(ShapingCache::default());
}

fn shape_text_uncached(
    font: FontRef<'_>,
    text: &[u16],
    text_type: TextType,
    letter_spacing: f32,
    word_spacing: f32,
) -> CachedShape {
    unsafe extern "C" fn emit(
        sink: *mut c_void,
        glyphs: *const DrawGlyph,
        glyph_count: usize,
        width: f32,
        trailing_whitespace_length_in_code_units: usize,
        trailing_whitespace_advance: f32,
    ) {
        // SAFETY: `sink` is the CachedShape passed below, and the glyph
        // pointer stays valid for this synchronous callback.
        let shape = unsafe { &mut *sink.cast::<CachedShape>() };
        assert!(glyph_count == 0 || !glyphs.is_null());
        if glyph_count != 0 {
            // SAFETY: The C++ side hands a pointer to glyph_count glyphs that
            // outlive the callback.
            shape.glyphs = unsafe { std::slice::from_raw_parts(glyphs, glyph_count) }.to_vec();
        }
        shape.width = width;
        shape.trailing_whitespace_length_in_code_units = trailing_whitespace_length_in_code_units;
        shape.trailing_whitespace_advance = trailing_whitespace_advance;
    }
    let mut shape = CachedShape::default();
    // SAFETY: FontRef keeps the font live and the text slice stays valid for
    // the synchronous shaping call; emit runs against the local CachedShape.
    unsafe {
        ladybird_gfx_shape_text_uncached(
            font.as_ptr(),
            text.as_ptr(),
            text.len(),
            text_type,
            letter_spacing,
            word_spacing,
            (&raw mut shape).cast(),
            emit,
        );
    }
    shape
}

fn shaped_text_with_baseline_start(shape: &CachedShape, baseline_start_x: f32) -> ShapedText {
    let glyphs = if baseline_start_x == 0.0 {
        shape.glyphs.clone()
    } else {
        shape
            .glyphs
            .iter()
            .map(|glyph| DrawGlyph {
                x: glyph.x + baseline_start_x,
                ..*glyph
            })
            .collect()
    };
    ShapedText {
        glyphs,
        width: shape.width,
        trailing_whitespace_length_in_code_units: shape.trailing_whitespace_length_in_code_units,
        trailing_whitespace_advance: shape.trailing_whitespace_advance,
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
    let params = ShapeParams {
        text_type,
        letter_spacing,
        word_spacing,
    };
    SHAPING_CACHE.with_borrow_mut(|cache| {
        let shape = cache.shape_for(font.id(), text, params, || {
            shape_text_uncached(font, text, text_type, letter_spacing, word_spacing)
        });
        shaped_text_with_baseline_start(shape, baseline_start_x)
    })
}

#[cfg(test)]
mod shaping_cache_tests {
    use super::*;

    fn shape_with_glyph_count(glyph_count: usize) -> CachedShape {
        CachedShape {
            glyphs: vec![DrawGlyph::default(); glyph_count],
            width: glyph_count as f32,
            trailing_whitespace_length_in_code_units: 0,
            trailing_whitespace_advance: 0.0,
        }
    }

    fn params_with_letter_spacing(letter_spacing: f32) -> ShapeParams {
        ShapeParams {
            text_type: TextType::Common,
            letter_spacing,
            word_spacing: 0.0,
        }
    }

    #[test]
    fn repeated_lookups_shape_once() {
        let mut cache = ShapingCache::default();
        let text: Vec<u16> = "word".encode_utf16().collect();
        let mut shape_calls = 0;
        for _ in 0..3 {
            let shape = cache.shape_for(1, &text, params_with_letter_spacing(0.0), || {
                shape_calls += 1;
                shape_with_glyph_count(4)
            });
            assert_eq!(shape.glyphs.len(), 4);
        }
        assert_eq!(shape_calls, 1);
    }

    #[test]
    fn distinct_params_for_the_same_text_shape_separately() {
        let mut cache = ShapingCache::default();
        let text: Vec<u16> = "word".encode_utf16().collect();
        cache.shape_for(1, &text, params_with_letter_spacing(0.0), || shape_with_glyph_count(1));
        let mut second_params_shaped = false;
        cache.shape_for(1, &text, params_with_letter_spacing(2.0), || {
            second_params_shaped = true;
            shape_with_glyph_count(2)
        });
        assert!(second_params_shaped);
        let shape = cache.shape_for(1, &text, params_with_letter_spacing(0.0), || unreachable!());
        assert_eq!(shape.glyphs.len(), 1);
    }

    #[test]
    fn single_ascii_fast_path_requires_common_text_without_spacing() {
        let mut cache = ShapingCache::default();
        let text = [b'a' as u16];
        cache.shape_for(1, &text, params_with_letter_spacing(0.0), || shape_with_glyph_count(1));
        let font_cache = &cache.caches_by_font_id[&1];
        assert!(font_cache.single_ascii_common_shapes[b'a' as usize].is_some());
        assert!(font_cache.shapes_by_text.is_empty());

        cache.shape_for(1, &text, params_with_letter_spacing(2.0), || shape_with_glyph_count(1));
        let font_cache = &cache.caches_by_font_id[&1];
        assert_eq!(font_cache.shapes_by_text.len(), 1);
    }

    #[test]
    fn least_recently_used_font_is_evicted_at_capacity() {
        let mut cache = ShapingCache::default();
        let text = [b'a' as u16];
        for font_id in 0..MAX_SHAPE_CACHE_FONTS as u64 {
            cache.shape_for(font_id, &text, params_with_letter_spacing(0.0), || {
                shape_with_glyph_count(1)
            });
        }
        cache.shape_for(0, &text, params_with_letter_spacing(0.0), || unreachable!());

        let one_font_over_capacity = MAX_SHAPE_CACHE_FONTS as u64;
        cache.shape_for(one_font_over_capacity, &text, params_with_letter_spacing(0.0), || {
            shape_with_glyph_count(1)
        });
        assert_eq!(cache.caches_by_font_id.len(), MAX_SHAPE_CACHE_FONTS);
        assert!(cache.caches_by_font_id.contains_key(&0));
        assert!(!cache.caches_by_font_id.contains_key(&1));
    }

    #[test]
    fn overflowing_the_per_font_text_capacity_clears_that_font_only() {
        let mut cache = ShapingCache::default();
        for text_index in 0..MAX_SHAPE_CACHE_TEXTS_PER_FONT as u32 {
            let text: Vec<u16> = format!("text-{text_index}").encode_utf16().collect();
            cache.shape_for(1, &text, params_with_letter_spacing(0.0), || shape_with_glyph_count(1));
        }
        let other_font_text: Vec<u16> = "other".encode_utf16().collect();
        cache.shape_for(2, &other_font_text, params_with_letter_spacing(0.0), || {
            shape_with_glyph_count(1)
        });

        let overflowing_text: Vec<u16> = "one-more".encode_utf16().collect();
        cache.shape_for(1, &overflowing_text, params_with_letter_spacing(0.0), || {
            shape_with_glyph_count(1)
        });
        assert_eq!(cache.caches_by_font_id[&1].shapes_by_text.len(), 1);
        assert_eq!(cache.caches_by_font_id[&2].shapes_by_text.len(), 1);
    }
}
