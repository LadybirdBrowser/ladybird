/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::font::FontHandle;
use hashbrown::hash_map::EntryRef;
use std::ffi::c_void;
use std::sync::Arc;

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
    pub cluster_start: u32,
    pub should_paint: bool,
    pub is_rtl: bool,
}

impl DrawGlyph {
    fn is_bitwise_equal_to(&self, other: &DrawGlyph) -> bool {
        self.x.to_bits() == other.x.to_bits()
            && self.y.to_bits() == other.y.to_bits()
            && self.length_in_code_units == other.length_in_code_units
            && self.glyph_width.to_bits() == other.glyph_width.to_bits()
            && self.glyph_id == other.glyph_id
            && self.cluster_start == other.cluster_start
            && self.should_paint == other.should_paint
            && self.is_rtl == other.is_rtl
    }
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

    fn ladybird_gfx_glyph_run_ink_bounding_box(
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
pub fn glyph_run_bounding_box(font: &FontHandle, glyphs: &[DrawGlyph], scale: f32) -> [f32; 4] {
    let mut out_rect = [0.0f32; 4];
    // SAFETY: The handle keeps the font live and the glyph slice stays valid
    // for the synchronous call, which fills the four floats out_rect points at.
    unsafe {
        ladybird_gfx_glyph_run_bounding_box(
            font.as_raw(),
            glyphs.as_ptr(),
            glyphs.len(),
            scale,
            out_rect.as_mut_ptr(),
        );
    }
    out_rect
}

/// Tight bounds of the ink of the painted glyphs, from their own extents, in the coordinates of
/// `glyph_run_bounding_box()`, as x, y, width and height. Empty if no painted glyph has ink.
#[must_use]
pub fn glyph_run_ink_bounding_box(font: &FontHandle, glyphs: &[DrawGlyph], scale: f32) -> [f32; 4] {
    let mut out_rect = [0.0f32; 4];
    // SAFETY: The handle keeps the font live and the glyph slice stays valid
    // for the synchronous call, which fills the four floats out_rect points at.
    unsafe {
        ladybird_gfx_glyph_run_ink_bounding_box(
            font.as_raw(),
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
    font: &FontHandle,
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
    // SAFETY: The handle keeps the font live and the glyph slice stays valid
    // for the synchronous call; the push callback runs against the local Vec.
    unsafe {
        ladybird_gfx_glyph_run_glyph_intercepts(
            font.as_raw(),
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

#[derive(Clone, Debug)]
pub enum GlyphBuffer {
    Shared(Arc<[DrawGlyph]>),
    Owned(Vec<DrawGlyph>),
}

impl GlyphBuffer {
    pub fn to_mut(&mut self) -> &mut Vec<DrawGlyph> {
        if let Self::Shared(shared) = self {
            *self = Self::Owned(shared.to_vec());
        }
        match self {
            Self::Owned(glyphs) => glyphs,
            Self::Shared(_) => unreachable!(),
        }
    }
}

impl Default for GlyphBuffer {
    fn default() -> Self {
        Self::Owned(Vec::new())
    }
}

impl From<Vec<DrawGlyph>> for GlyphBuffer {
    fn from(glyphs: Vec<DrawGlyph>) -> Self {
        Self::Owned(glyphs)
    }
}

impl std::ops::Deref for GlyphBuffer {
    type Target = [DrawGlyph];

    fn deref(&self) -> &[DrawGlyph] {
        match self {
            Self::Shared(glyphs) => glyphs,
            Self::Owned(glyphs) => glyphs,
        }
    }
}

impl PartialEq for GlyphBuffer {
    fn eq(&self, other: &Self) -> bool {
        **self == **other
    }
}

#[derive(Debug)]
pub struct ShapedText {
    glyphs: GlyphBuffer,
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
    pub fn into_glyphs(self) -> GlyphBuffer {
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
const MAX_SHAPE_CACHE_TEXTS_PER_FONT: usize = 16384;

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

#[derive(Debug)]
struct CachedShape {
    glyphs: Arc<[DrawGlyph]>,
    width: f32,
    trailing_whitespace_length_in_code_units: usize,
    trailing_whitespace_advance: f32,
}

impl CachedShape {
    fn is_bitwise_equal_to(&self, other: &CachedShape) -> bool {
        self.width.to_bits() == other.width.to_bits()
            && self.trailing_whitespace_length_in_code_units == other.trailing_whitespace_length_in_code_units
            && self.trailing_whitespace_advance.to_bits() == other.trailing_whitespace_advance.to_bits()
            && self.glyphs.len() == other.glyphs.len()
            && std::iter::zip(self.glyphs.iter(), other.glyphs.iter())
                .all(|(glyph, other_glyph)| glyph.is_bitwise_equal_to(other_glyph))
    }
}

#[derive(Clone, Debug)]
pub struct SharedShape(Arc<CachedShape>);

impl SharedShape {
    pub fn from_glyphs(
        glyphs: Vec<DrawGlyph>,
        width: f32,
        trailing_whitespace_length_in_code_units: usize,
        trailing_whitespace_advance: f32,
    ) -> Self {
        Self(Arc::new(CachedShape {
            glyphs: Arc::from(glyphs),
            width,
            trailing_whitespace_length_in_code_units,
            trailing_whitespace_advance,
        }))
    }

    #[inline]
    pub fn glyphs(&self) -> &Arc<[DrawGlyph]> {
        &self.0.glyphs
    }

    #[inline]
    pub fn width(&self) -> f32 {
        self.0.width
    }

    #[inline]
    pub fn trailing_whitespace_length_in_code_units(&self) -> usize {
        self.0.trailing_whitespace_length_in_code_units
    }

    #[inline]
    pub fn trailing_whitespace_advance(&self) -> f32 {
        self.0.trailing_whitespace_advance
    }

    pub fn with_baseline_start(&self, baseline_start_x: f32) -> ShapedText {
        shaped_text_with_baseline_start(&self.0, baseline_start_x)
    }
}

impl PartialEq for SharedShape {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0) || self.0.is_bitwise_equal_to(&other.0)
    }
}

struct ShapeForParams {
    params: ShapeParams,
    shape: Arc<CachedShape>,
}

struct ShapesForText {
    last_used_tick: u64,
    shapes: Vec<ShapeForParams>,
}

type FastMap<K, V> = hashbrown::HashMap<K, V, foldhash::fast::RandomState>;

struct FontShapeCache {
    last_used_tick: u64,
    single_ascii_common_shapes: [Option<Arc<CachedShape>>; SINGLE_ASCII_SHAPE_CACHE_SIZE],
    shapes_by_text: FastMap<Box<[u16]>, ShapesForText>,
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
        tick: u64,
        text: &[u16],
        params: ShapeParams,
        shape_uncached: impl FnOnce() -> CachedShape,
    ) -> &Arc<CachedShape> {
        if let &[code_unit] = text
            && (code_unit as usize) < SINGLE_ASCII_SHAPE_CACHE_SIZE
            && params.text_type == TextType::Common
            && params.letter_spacing == 0.0
            && params.word_spacing == 0.0
        {
            return self.single_ascii_common_shapes[code_unit as usize]
                .get_or_insert_with(|| Arc::new(shape_uncached()));
        }

        if self.shapes_by_text.len() > MAX_SHAPE_CACHE_TEXTS_PER_FONT {
            self.evict_least_recently_used_texts();
        }
        let entry = match self.shapes_by_text.entry_ref(text) {
            EntryRef::Occupied(entry) => entry.into_mut(),
            EntryRef::Vacant(entry) => entry.insert_with_key(
                Box::from(text),
                ShapesForText {
                    last_used_tick: tick,
                    shapes: Vec::new(),
                },
            ),
        };
        entry.last_used_tick = tick;
        let shapes = &mut entry.shapes;
        let index = shapes
            .iter()
            .position(|entry| entry.params.matches(&params))
            .unwrap_or_else(|| {
                shapes.push(ShapeForParams {
                    params,
                    shape: Arc::new(shape_uncached()),
                });
                shapes.len() - 1
            });
        &shapes[index].shape
    }

    fn evict_least_recently_used_texts(&mut self) {
        let mut ticks: Vec<u64> = self.shapes_by_text.values().map(|entry| entry.last_used_tick).collect();
        let median_index = ticks.len() / 2;
        let (_, median_tick, _) = ticks.select_nth_unstable(median_index);
        let threshold = *median_tick;
        self.shapes_by_text.retain(|_, entry| entry.last_used_tick > threshold);
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
    ) -> &Arc<CachedShape> {
        self.tick += 1;
        if self.caches_by_font_id.len() > MAX_SHAPE_CACHE_FONTS {
            self.evict_least_recently_used_font();
        }
        let font_cache = self.caches_by_font_id.entry(font_id).or_default();
        font_cache.last_used_tick = self.tick;
        font_cache.shape_for(self.tick, text, params, shape_uncached)
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
    font: &FontHandle,
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
        // SAFETY: `sink` is the Option<CachedShape> passed below, and the
        // glyph pointer stays valid for this synchronous callback.
        let shape = unsafe { &mut *sink.cast::<Option<CachedShape>>() };
        assert!(glyph_count == 0 || !glyphs.is_null());
        let glyphs = if glyph_count == 0 {
            Arc::from([])
        } else {
            // SAFETY: The C++ side hands a pointer to glyph_count glyphs that
            // outlive the callback.
            Arc::from(unsafe { ak::slice_from_raw_parts(glyphs, glyph_count) })
        };
        *shape = Some(CachedShape {
            glyphs,
            width,
            trailing_whitespace_length_in_code_units,
            trailing_whitespace_advance,
        });
    }
    // The shape is built in the callback, so a cache miss allocates the glyph
    // storage once rather than an empty placeholder first.
    let mut shape: Option<CachedShape> = None;
    // SAFETY: FontRef keeps the font live and the text slice stays valid for
    // the synchronous shaping call; emit runs against the local Option.
    unsafe {
        ladybird_gfx_shape_text_uncached(
            font.as_raw(),
            text.as_ptr(),
            text.len(),
            text_type,
            letter_spacing,
            word_spacing,
            (&raw mut shape).cast(),
            emit,
        );
    }
    shape.expect("shaping emits exactly one shape")
}

fn shaped_text_with_baseline_start(shape: &CachedShape, baseline_start_x: f32) -> ShapedText {
    let glyphs = if baseline_start_x == 0.0 {
        GlyphBuffer::Shared(shape.glyphs.clone())
    } else {
        GlyphBuffer::Owned(
            shape
                .glyphs
                .iter()
                .map(|glyph| DrawGlyph {
                    x: glyph.x + baseline_start_x,
                    ..*glyph
                })
                .collect(),
        )
    };
    ShapedText {
        glyphs,
        width: shape.width,
        trailing_whitespace_length_in_code_units: shape.trailing_whitespace_length_in_code_units,
        trailing_whitespace_advance: shape.trailing_whitespace_advance,
    }
}

fn with_cached_shape<R>(
    font: &FontHandle,
    text: &[u16],
    text_type: TextType,
    letter_spacing: f32,
    word_spacing: f32,
    use_shape: impl FnOnce(&Arc<CachedShape>) -> R,
) -> R {
    let params = ShapeParams {
        text_type,
        letter_spacing,
        word_spacing,
    };
    SHAPING_CACHE.with_borrow_mut(|cache| {
        use_shape(cache.shape_for(font.id().0, text, params, || {
            shape_text_uncached(font, text, text_type, letter_spacing, word_spacing)
        }))
    })
}

pub fn shape_text_shared(
    font: &FontHandle,
    text: &[u16],
    text_type: TextType,
    letter_spacing: f32,
    word_spacing: f32,
) -> SharedShape {
    with_cached_shape(font, text, text_type, letter_spacing, word_spacing, |shape| {
        SharedShape(shape.clone())
    })
}

pub fn shaped_text_width(
    font: &FontHandle,
    text: &[u16],
    text_type: TextType,
    letter_spacing: f32,
    word_spacing: f32,
) -> f32 {
    with_cached_shape(font, text, text_type, letter_spacing, word_spacing, |shape| shape.width)
}

pub fn shape_text(
    font: &FontHandle,
    text: &[u16],
    text_type: TextType,
    baseline_start_x: f32,
    letter_spacing: f32,
    word_spacing: f32,
) -> ShapedText {
    with_cached_shape(font, text, text_type, letter_spacing, word_spacing, |shape| {
        shaped_text_with_baseline_start(shape, baseline_start_x)
    })
}

/// # Safety
/// `font` must point to a live Gfx::Font, and `text` must contain `length` code
/// units (or be null when empty). The callback must consume the borrowed glyphs
/// synchronously and must not reenter the shaping cache.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_gfx_shape_text_cached(
    font: *const c_void,
    text: *const u16,
    length: usize,
    text_type: TextType,
    letter_spacing: f32,
    word_spacing: f32,
    sink: *mut c_void,
    emit: unsafe extern "C" fn(*mut c_void, *const DrawGlyph, usize, f32, usize, f32),
) {
    // SAFETY: The caller supplies a live font and a valid text slice.
    let font = unsafe { FontHandle::intern(font) };
    let text = if length == 0 {
        &[]
    } else {
        unsafe { ak::slice_from_raw_parts(text, length) }
    };
    with_cached_shape(&font, text, text_type, letter_spacing, word_spacing, |shape| {
        // SAFETY: The glyphs remain live for the synchronous callback.
        unsafe {
            emit(
                sink,
                shape.glyphs.as_ptr(),
                shape.glyphs.len(),
                shape.width,
                shape.trailing_whitespace_length_in_code_units,
                shape.trailing_whitespace_advance,
            );
        }
    });
}

#[cfg(test)]
mod shaping_cache_tests {
    use super::*;

    fn shape_with_glyph_count(glyph_count: usize) -> CachedShape {
        CachedShape {
            glyphs: Arc::from(vec![DrawGlyph::default(); glyph_count]),
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
        cache.shape_for(0, &text, params_with_letter_spacing(0.0), || unreachable!());
        assert_eq!(cache.caches_by_font_id.len(), MAX_SHAPE_CACHE_FONTS);
        assert!(cache.caches_by_font_id.contains_key(&0));
        assert!(!cache.caches_by_font_id.contains_key(&1));
        assert!(cache.caches_by_font_id.contains_key(&one_font_over_capacity));
    }

    #[test]
    fn overflowing_the_per_font_text_capacity_evicts_the_least_recently_used_half_of_that_font() {
        let mut cache = ShapingCache::default();
        let texts: Vec<Vec<u16>> = (0..MAX_SHAPE_CACHE_TEXTS_PER_FONT)
            .map(|text_index| format!("text-{text_index}").encode_utf16().collect())
            .collect();
        for text in &texts {
            cache.shape_for(1, text, params_with_letter_spacing(0.0), || shape_with_glyph_count(1));
        }
        cache.shape_for(1, &texts[0], params_with_letter_spacing(0.0), || unreachable!());
        let other_font_text: Vec<u16> = "other".encode_utf16().collect();
        cache.shape_for(2, &other_font_text, params_with_letter_spacing(0.0), || {
            shape_with_glyph_count(1)
        });

        let overflowing_text: Vec<u16> = "one-more".encode_utf16().collect();
        cache.shape_for(1, &overflowing_text, params_with_letter_spacing(0.0), || {
            shape_with_glyph_count(1)
        });
        cache.shape_for(1, &texts[0], params_with_letter_spacing(0.0), || unreachable!());
        let font_cache = &cache.caches_by_font_id[&1];
        assert_eq!(font_cache.shapes_by_text.len(), MAX_SHAPE_CACHE_TEXTS_PER_FONT / 2);
        assert!(font_cache.shapes_by_text.contains_key(texts[0].as_slice()));
        assert!(!font_cache.shapes_by_text.contains_key(texts[1].as_slice()));
        assert!(
            font_cache
                .shapes_by_text
                .contains_key(texts[texts.len() - 1].as_slice())
        );
        assert!(font_cache.shapes_by_text.contains_key(overflowing_text.as_slice()));
        assert_eq!(cache.caches_by_font_id[&2].shapes_by_text.len(), 1);

        cache.shape_for(1, &overflowing_text, params_with_letter_spacing(0.0), || unreachable!());
    }
}
