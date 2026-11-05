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

/// A glyph as shaping makes it and as the glyph run readers in C++ read it.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct ShapedGlyph {
    x: f32,
    y: f32,
    length_in_code_units: usize,
    glyph_width: f32,
    glyph_id: u32,
    cluster_start: u32,
    should_paint: bool,
    is_rtl: bool,
}

impl From<PlacedGlyph> for ShapedGlyph {
    fn from(placed: PlacedGlyph) -> Self {
        Self {
            x: placed.x,
            y: placed.y,
            length_in_code_units: placed.glyph.length_in_code_units(),
            glyph_width: placed.glyph.advance,
            glyph_id: u32::from(placed.glyph.glyph_id),
            cluster_start: placed.cluster_start,
            should_paint: placed.glyph.should_paint(),
            is_rtl: placed.is_rtl,
        }
    }
}

const GLYPH_SHOULD_PAINT_BIT: u16 = 1 << 15;
const GLYPH_MAX_LENGTH_IN_CODE_UNITS: usize = (GLYPH_SHOULD_PAINT_BIT - 1) as usize;

/// A glyph of a run: what it draws and how far it moves the pen. Where it sits is up to the run that holds it.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct DrawGlyph {
    pub advance: f32,
    pub glyph_id: u16,
    length_and_should_paint: u16,
}

const _: () = assert!(size_of::<DrawGlyph>() == 8);

impl DrawGlyph {
    /// A cluster longer than the glyph can record counts as being as long as it can record.
    #[must_use]
    pub fn new(advance: f32, glyph_id: u32, length_in_code_units: usize, should_paint: bool) -> Self {
        let length = length_in_code_units.min(GLYPH_MAX_LENGTH_IN_CODE_UNITS) as u16;
        Self {
            advance,
            glyph_id: glyph_id as u16,
            length_and_should_paint: length | if should_paint { GLYPH_SHOULD_PAINT_BIT } else { 0 },
        }
    }

    /// How many code units of the text the glyph stands for: none for a glyph that continues the cluster of the glyph
    /// before it.
    #[inline]
    #[must_use]
    pub fn length_in_code_units(&self) -> usize {
        usize::from(self.length_and_should_paint & !GLYPH_SHOULD_PAINT_BIT)
    }

    #[inline]
    #[must_use]
    pub fn should_paint(&self) -> bool {
        self.length_and_should_paint & GLYPH_SHOULD_PAINT_BIT != 0
    }

    fn is_bitwise_equal_to(&self, other: &DrawGlyph) -> bool {
        self.advance.to_bits() == other.advance.to_bits()
            && self.glyph_id == other.glyph_id
            && self.length_and_should_paint == other.length_and_should_paint
    }
}

const PLACEMENT_IS_RTL_BIT: u32 = 1 << 31;

/// Where a run places a glyph, and what the glyph stands for in the text.
#[derive(Clone, Copy, Debug, PartialEq)]
struct GlyphPlacement {
    x: f32,
    y: f32,
    cluster_start_and_is_rtl: u32,
}

const _: () = assert!(size_of::<GlyphPlacement>() == 12);

impl GlyphPlacement {
    fn new(x: f32, y: f32, cluster_start: u32, is_rtl: bool) -> Self {
        assert!(cluster_start < PLACEMENT_IS_RTL_BIT);
        Self {
            x,
            y,
            cluster_start_and_is_rtl: cluster_start | if is_rtl { PLACEMENT_IS_RTL_BIT } else { 0 },
        }
    }

    #[inline]
    fn cluster_start(&self) -> u32 {
        self.cluster_start_and_is_rtl & !PLACEMENT_IS_RTL_BIT
    }

    #[inline]
    fn is_rtl(&self) -> bool {
        self.cluster_start_and_is_rtl & PLACEMENT_IS_RTL_BIT != 0
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
        emit: unsafe extern "C" fn(*mut c_void, *const ShapedGlyph, usize, f32, usize, f32),
    );

    fn ladybird_gfx_glyph_run_bounding_box(
        font: *const c_void,
        glyphs: *const ShapedGlyph,
        glyph_count: usize,
        scale: f32,
        out_rect: *mut f32,
    );

    fn ladybird_gfx_glyph_run_ink_bounding_box(
        font: *const c_void,
        glyphs: *const ShapedGlyph,
        glyph_count: usize,
        scale: f32,
        out_rect: *mut f32,
    );

    fn ladybird_gfx_glyph_run_glyph_intercepts(
        font: *const c_void,
        glyphs: *const ShapedGlyph,
        glyph_count: usize,
        scale: f32,
        y_top: f32,
        y_bottom: f32,
        sink: *mut c_void,
        push: unsafe extern "C" fn(*mut c_void, f32),
    );
}

/// The glyphs of the run in the form that the glyph run readers in C++ read them.
fn shaped_glyphs_of(glyphs: &GlyphBuffer) -> Vec<ShapedGlyph> {
    glyphs.placed().map(ShapedGlyph::from).collect()
}

#[must_use]
pub fn glyph_run_bounding_box(font: &FontHandle, glyphs: &GlyphBuffer, scale: f32) -> [f32; 4] {
    let glyphs = shaped_glyphs_of(glyphs);
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
pub fn glyph_run_ink_bounding_box(font: &FontHandle, glyphs: &GlyphBuffer, scale: f32) -> [f32; 4] {
    let glyphs = shaped_glyphs_of(glyphs);
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
    glyphs: &GlyphBuffer,
    scale: f32,
    y_top: f32,
    y_bottom: f32,
) -> Vec<f32> {
    unsafe extern "C" fn push(sink: *mut c_void, value: f32) {
        // SAFETY: `sink` is the Vec passed below, live for the synchronous call.
        unsafe { (*sink.cast::<Vec<f32>>()).push(value) };
    }
    let glyphs = shaped_glyphs_of(glyphs);
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

/// A glyph of a run, with where the run places it and what it stands for in the text.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PlacedGlyph {
    pub glyph: DrawGlyph,
    pub x: f32,
    pub y: f32,
    pub cluster_start: u32,
    pub is_rtl: bool,
}

impl PlacedGlyph {
    fn is_bitwise_equal_to(&self, other: &PlacedGlyph) -> bool {
        self.glyph.is_bitwise_equal_to(&other.glyph)
            && self.x.to_bits() == other.x.to_bits()
            && self.y.to_bits() == other.y.to_bits()
            && self.cluster_start == other.cluster_start
            && self.is_rtl == other.is_rtl
    }
}

/// The glyphs of a run in order, each with its placement.
pub struct PlacedGlyphs<'a> {
    glyphs: std::slice::Iter<'a, DrawGlyph>,
    placements: Option<std::slice::Iter<'a, GlyphPlacement>>,
    pen: f32,
    next_cluster_start: u32,
    baseline_y: f32,
}

impl Iterator for PlacedGlyphs<'_> {
    type Item = PlacedGlyph;

    #[inline]
    fn next(&mut self) -> Option<PlacedGlyph> {
        let glyph = *self.glyphs.next()?;
        if let Some(placements) = &mut self.placements {
            let placement = placements.next()?;
            return Some(PlacedGlyph {
                glyph,
                x: placement.x,
                y: placement.y,
                cluster_start: placement.cluster_start(),
                is_rtl: placement.is_rtl(),
            });
        }
        let placed = PlacedGlyph {
            glyph,
            x: self.pen,
            y: self.baseline_y,
            cluster_start: self.next_cluster_start,
            is_rtl: false,
        };
        self.pen += glyph.advance;
        self.next_cluster_start += glyph.length_in_code_units() as u32;
        Some(placed)
    }

    #[inline]
    fn size_hint(&self) -> (usize, Option<usize>) {
        self.glyphs.size_hint()
    }
}

impl ExactSizeIterator for PlacedGlyphs<'_> {}

#[derive(Clone, Debug)]
enum GlyphStorage<T> {
    Shared(Arc<[T]>),
    Owned(Vec<T>),
}

impl<T> Default for GlyphStorage<T> {
    fn default() -> Self {
        Self::Owned(Vec::new())
    }
}

impl<T: Clone> GlyphStorage<T> {
    #[inline]
    fn as_slice(&self) -> &[T] {
        match self {
            Self::Shared(items) => items,
            Self::Owned(items) => items,
        }
    }

    fn into_shared(self) -> Self {
        match self {
            Self::Owned(items) => Self::Shared(Arc::from(items)),
            shared @ Self::Shared(_) => shared,
        }
    }

    fn to_mut(&mut self) -> &mut Vec<T> {
        if let Self::Shared(shared) = self {
            *self = Self::Owned(shared.to_vec());
        }
        match self {
            Self::Owned(items) => items,
            Self::Shared(_) => unreachable!(),
        }
    }
}

/// Shaping gives advances in 1/64ths of a pixel, which an f32 adds up exactly below this.
const MAX_EXACT_PEN_POSITION: f32 = (1u32 << 18) as f32;

/// Where the pen ends up past the glyphs, starting at `pen`, as long as adding up their advances does not round. Any
/// sum of such advances is then exact too, so positions do not depend on the order they were added up in.
fn exact_pen_position_past(mut pen: f32, glyphs: &[DrawGlyph]) -> Option<f32> {
    for glyph in glyphs {
        if (glyph.advance * 64.0).fract() != 0.0 {
            return None;
        }
        pen += glyph.advance;
        if pen.abs() >= MAX_EXACT_PEN_POSITION {
            return None;
        }
    }
    Some(pen)
}

/// Whether the placements of the shaped glyphs follow from the glyphs: each glyph starts where the advances of the
/// glyphs before it end, on one baseline, at the code unit where the lengths of the glyphs before it end, in a left to
/// right run. Adding up the advances gives the bits of each position, as `PlacedGlyphs` adds them up.
fn placements_follow_from_glyphs(shaped: &[ShapedGlyph], glyphs: &[DrawGlyph]) -> bool {
    if exact_pen_position_past(0.0, glyphs).is_none() {
        return false;
    }
    let Some(first) = shaped.first() else {
        return true;
    };
    let mut pen = 0.0f32;
    let mut next_cluster_start = 0u32;
    std::iter::zip(shaped, glyphs).all(|(shaped_glyph, glyph)| {
        let is_derivable = !shaped_glyph.is_rtl
            && shaped_glyph.x.to_bits() == pen.to_bits()
            && shaped_glyph.y.to_bits() == first.y.to_bits()
            && shaped_glyph.cluster_start == next_cluster_start;
        pen += glyph.advance;
        next_cluster_start += glyph.length_in_code_units() as u32;
        is_derivable
    })
}

/// Where the glyphs of a run that stores no placements end, as of the run having `glyph_count` glyphs. A run holds no
/// such thing itself: only runs that grow need it, and they need not add up their advances for every piece they take.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DerivedPlacementsEnd {
    glyph_count: usize,
    pen: f32,
    code_units: u32,
}

/// The glyphs of a run, and where the run places them. A run stores no placements where they follow from the glyphs,
/// see `placements_follow_from_glyphs()`, which is what plain left to right text does.
#[derive(Clone, Debug, Default)]
pub struct GlyphBuffer {
    glyphs: GlyphStorage<DrawGlyph>,
    placements: Option<Box<GlyphStorage<GlyphPlacement>>>,
    baseline_y: f32,
}

const _: () = assert!(size_of::<GlyphBuffer>() == 40);

impl GlyphBuffer {
    fn from_shaped(shaped: &[ShapedGlyph]) -> Self {
        let glyphs: Arc<[DrawGlyph]> = shaped
            .iter()
            .map(|glyph| {
                DrawGlyph::new(
                    glyph.glyph_width,
                    glyph.glyph_id,
                    glyph.length_in_code_units,
                    glyph.should_paint,
                )
            })
            .collect();
        let placements = if placements_follow_from_glyphs(shaped, &glyphs) {
            None
        } else {
            Some(Box::new(GlyphStorage::Shared(
                shaped
                    .iter()
                    .map(|glyph| GlyphPlacement::new(glyph.x, glyph.y, glyph.cluster_start, glyph.is_rtl))
                    .collect(),
            )))
        };
        Self {
            glyphs: GlyphStorage::Shared(glyphs),
            baseline_y: shaped.first().map_or(0.0, |glyph| glyph.y),
            placements,
        }
    }

    /// The same glyphs in the form that clones without copying them.
    #[must_use]
    pub fn into_shared(self) -> Self {
        Self {
            glyphs: self.glyphs.into_shared(),
            placements: self.placements.map(|placements| Box::new(placements.into_shared())),
            baseline_y: self.baseline_y,
        }
    }

    #[inline]
    #[must_use]
    pub fn placed(&self) -> PlacedGlyphs<'_> {
        PlacedGlyphs {
            glyphs: self.glyphs.as_slice().iter(),
            placements: self.placements.as_ref().map(|placements| placements.as_slice().iter()),
            pen: 0.0,
            next_cluster_start: 0,
            baseline_y: self.baseline_y,
        }
    }

    /// The placements of the glyphs, stored from now on.
    fn placements_mut(&mut self) -> &mut Vec<GlyphPlacement> {
        if self.placements.is_none() {
            let placements = self
                .placed()
                .map(|placed| GlyphPlacement::new(placed.x, placed.y, placed.cluster_start, placed.is_rtl))
                .collect();
            self.placements = Some(Box::new(GlyphStorage::Owned(placements)));
        }
        self.placements
            .as_mut()
            .expect("the placements were stored above")
            .to_mut()
    }

    pub fn reserve(&mut self, additional_glyphs: usize) {
        self.glyphs.to_mut().reserve(additional_glyphs);
        if let Some(placements) = &mut self.placements {
            placements.to_mut().reserve(additional_glyphs);
        }
    }

    /// Has `update` move each glyph, given its x and y.
    pub fn update_positions(&mut self, mut update: impl FnMut(&DrawGlyph, &mut f32, &mut f32)) {
        self.placements_mut();
        let placements = self.placements.as_mut().expect("the placements are stored").to_mut();
        for (glyph, placement) in std::iter::zip(self.glyphs.as_slice(), placements) {
            update(glyph, &mut placement.x, &mut placement.y);
        }
    }

    /// Moves all glyphs along the block axis.
    pub fn shift_baseline(&mut self, offset: f32) {
        self.baseline_y += offset;
        if let Some(placements) = &mut self.placements {
            for placement in placements.to_mut() {
                placement.y += offset;
            }
        }
    }

    pub fn push_placed(&mut self, placed: PlacedGlyph) {
        self.placements_mut().push(GlyphPlacement::new(
            placed.x,
            placed.y,
            placed.cluster_start,
            placed.is_rtl,
        ));
        self.glyphs.to_mut().push(placed.glyph);
    }

    pub fn truncate(&mut self, glyph_count: usize) {
        self.glyphs.to_mut().truncate(glyph_count);
        if let Some(placements) = &mut self.placements {
            placements.to_mut().truncate(glyph_count);
        }
    }

    pub fn pop(&mut self) {
        self.glyphs.to_mut().pop();
        if let Some(placements) = &mut self.placements {
            placements.to_mut().pop();
        }
    }

    /// Drops all glyphs outside of `range`, leaving the placement of the others as it is.
    pub fn keep_range(&mut self, range: std::ops::Range<usize>) {
        let placements = self.placements_mut();
        placements.drain(range.end..);
        placements.drain(..range.start);
        let glyphs = self.glyphs.to_mut();
        glyphs.drain(range.end..);
        glyphs.drain(..range.start);
    }

    /// Appends the glyphs of `run` at `x_offset` and at `cluster_offset` code units into the text, if their placements
    /// then still follow from the glyphs, and answers whether it did. `end` is what the last call that appended to this
    /// run left there, if any.
    pub fn append_keeping_placements_derived(
        &mut self,
        end: &mut Option<DerivedPlacementsEnd>,
        run: &GlyphBuffer,
        x_offset: f32,
        cluster_offset: u32,
    ) -> bool {
        if self.placements.is_some()
            || run.placements.is_some()
            || self.baseline_y.to_bits() != run.baseline_y.to_bits()
        {
            return false;
        }
        let current_end = (*end)
            .filter(|end| end.glyph_count == self.len())
            .unwrap_or_else(|| DerivedPlacementsEnd {
                glyph_count: self.len(),
                pen: self
                    .iter()
                    .map(|glyph| glyph.advance)
                    .fold(0.0, |pen, advance| pen + advance),
                code_units: self.iter().map(|glyph| glyph.length_in_code_units() as u32).sum(),
            });
        if current_end.pen.to_bits() != x_offset.to_bits() || current_end.code_units != cluster_offset {
            return false;
        }
        let Some(pen) = exact_pen_position_past(current_end.pen, run) else {
            return false;
        };
        self.glyphs.to_mut().extend_from_slice(run);
        *end = Some(DerivedPlacementsEnd {
            glyph_count: self.len(),
            pen,
            code_units: current_end.code_units
                + run.iter().map(|glyph| glyph.length_in_code_units() as u32).sum::<u32>(),
        });
        true
    }

    /// The glyphs from the one at `index` on, with that glyph at the start of the run and of the text.
    #[must_use]
    pub fn remainder_from(&self, index: usize) -> Self {
        let glyphs = self.glyphs.as_slice()[index..].to_vec();
        let placements = if self.placements.is_none() && exact_pen_position_past(0.0, &glyphs).is_some() {
            None
        } else {
            let first = self
                .placed()
                .nth(index)
                .expect("the remainder starts at a glyph of the run");
            Some(Box::new(GlyphStorage::Owned(
                self.placed()
                    .skip(index)
                    .map(|placed| {
                        GlyphPlacement::new(
                            placed.x - first.x,
                            placed.y,
                            placed.cluster_start.saturating_sub(first.cluster_start),
                            placed.is_rtl,
                        )
                    })
                    .collect(),
            )))
        };
        Self {
            glyphs: GlyphStorage::Owned(glyphs),
            placements,
            baseline_y: self.baseline_y,
        }
    }
}

impl std::ops::Deref for GlyphBuffer {
    type Target = [DrawGlyph];

    #[inline]
    fn deref(&self) -> &[DrawGlyph] {
        self.glyphs.as_slice()
    }
}

impl PartialEq for GlyphBuffer {
    fn eq(&self, other: &Self) -> bool {
        self.placed().eq(other.placed())
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
    pub fn glyphs(&self) -> &GlyphBuffer {
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
    glyphs: GlyphBuffer,
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
            && std::iter::zip(self.glyphs.placed(), other.glyphs.placed())
                .all(|(glyph, other_glyph)| glyph.is_bitwise_equal_to(&other_glyph))
    }
}

#[derive(Clone, Debug)]
pub struct SharedShape(Arc<CachedShape>);

impl SharedShape {
    pub fn from_glyphs(
        glyphs: GlyphBuffer,
        width: f32,
        trailing_whitespace_length_in_code_units: usize,
        trailing_whitespace_advance: f32,
    ) -> Self {
        Self(Arc::new(CachedShape {
            glyphs: glyphs.into_shared(),
            width,
            trailing_whitespace_length_in_code_units,
            trailing_whitespace_advance,
        }))
    }

    #[inline]
    pub fn glyphs(&self) -> &GlyphBuffer {
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
        glyphs: *const ShapedGlyph,
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
            GlyphBuffer::from_shaped(&[])
        } else {
            // SAFETY: The C++ side hands a pointer to glyph_count glyphs that
            // outlive the callback.
            GlyphBuffer::from_shaped(unsafe { ak::slice_from_raw_parts(glyphs, glyph_count) })
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
    let mut glyphs = shape.glyphs.clone();
    if baseline_start_x != 0.0 {
        glyphs.update_positions(|_, x, _| *x += baseline_start_x);
    }
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

#[cfg(test)]
mod glyph_buffer_tests {
    use super::*;

    const ASCENT: f32 = 12.0;

    fn left_to_right_glyph(x: f32, advance: f32, cluster_start: u32) -> ShapedGlyph {
        ShapedGlyph {
            x,
            y: -ASCENT,
            length_in_code_units: 1,
            glyph_width: advance,
            glyph_id: 7,
            cluster_start,
            should_paint: true,
            is_rtl: false,
        }
    }

    fn plain_run() -> Vec<ShapedGlyph> {
        vec![
            left_to_right_glyph(0.0, 8.5, 0),
            left_to_right_glyph(8.5, 4.25, 1),
            left_to_right_glyph(12.75, 8.0, 2),
        ]
    }

    fn assert_places_glyphs_as_shaped(buffer: &GlyphBuffer, shaped: &[ShapedGlyph]) {
        let placed: Vec<ShapedGlyph> = buffer.placed().map(ShapedGlyph::from).collect();
        assert_eq!(placed.len(), shaped.len());
        for (placed, shaped) in std::iter::zip(placed, shaped) {
            assert_eq!(placed.x.to_bits(), shaped.x.to_bits());
            assert_eq!(placed.y.to_bits(), shaped.y.to_bits());
            assert_eq!(placed, *shaped);
        }
    }

    fn assert_stores_placements(shaped: &[ShapedGlyph], expected: bool) {
        let buffer = GlyphBuffer::from_shaped(shaped);
        assert_eq!(buffer.placements.is_some(), expected);
        assert_places_glyphs_as_shaped(&buffer, shaped);
    }

    #[test]
    fn plain_left_to_right_text_stores_no_placements() {
        assert_stores_placements(&plain_run(), false);
        assert_stores_placements(&[], false);
    }

    #[test]
    fn a_glyph_that_paints_nothing_stays_derivable() {
        let mut shaped = plain_run();
        shaped.insert(
            1,
            ShapedGlyph {
                should_paint: false,
                glyph_width: 0.0,
                ..left_to_right_glyph(8.5, 0.0, 1)
            },
        );
        shaped[2].cluster_start = 2;
        shaped[3].cluster_start = 3;
        assert_stores_placements(&shaped, false);
    }

    #[test]
    fn a_cluster_of_several_glyphs_stores_placements() {
        let mut shaped = plain_run();
        shaped[1].length_in_code_units = 0;
        shaped[1].cluster_start = 0;
        shaped[2].cluster_start = 1;
        assert_stores_placements(&shaped, true);
    }

    #[test]
    fn offsets_store_placements() {
        let mut shaped = plain_run();
        shaped[1].x += 0.5;
        assert_stores_placements(&shaped, true);

        let mut shaped = plain_run();
        shaped[1].y += 0.5;
        assert_stores_placements(&shaped, true);
    }

    #[test]
    fn right_to_left_text_stores_placements() {
        let mut shaped = vec![left_to_right_glyph(0.0, 8.0, 0)];
        shaped[0].is_rtl = true;
        assert_stores_placements(&shaped, true);
    }

    #[test]
    fn clusters_that_do_not_add_up_store_placements() {
        let mut shaped = plain_run();
        shaped[2].cluster_start = 5;
        assert_stores_placements(&shaped, true);

        let mut shaped = plain_run();
        shaped[0].length_in_code_units = GLYPH_MAX_LENGTH_IN_CODE_UNITS + 1;
        shaped[1].cluster_start = shaped[0].length_in_code_units as u32;
        shaped[2].cluster_start = shaped[1].cluster_start + 1;
        let buffer = GlyphBuffer::from_shaped(&shaped);
        assert!(buffer.placements.is_some());
        assert_eq!(buffer.placed().nth(2).unwrap().cluster_start, shaped[2].cluster_start);
    }

    #[test]
    fn advances_that_could_round_store_placements() {
        let mut shaped = plain_run();
        shaped[0].glyph_width = 0.1;
        shaped[1].x = 0.1;
        shaped[2].x = 0.1 + 4.25;
        assert_stores_placements(&shaped, true);

        let mut shaped = plain_run();
        shaped[0].glyph_width = MAX_EXACT_PEN_POSITION;
        shaped[1].x = MAX_EXACT_PEN_POSITION;
        shaped[2].x = MAX_EXACT_PEN_POSITION + 4.25;
        assert_stores_placements(&shaped, true);
    }

    #[test]
    fn negative_zero_positions_store_placements() {
        let mut shaped = plain_run();
        shaped[0].y = -0.0;
        shaped[1].y = 0.0;
        shaped[2].y = 0.0;
        assert_stores_placements(&shaped, true);
    }

    #[test]
    fn truncating_and_shifting_the_baseline_keep_placements_derived() {
        let shaped = plain_run();
        let mut buffer = GlyphBuffer::from_shaped(&shaped);
        buffer.truncate(2);
        buffer.shift_baseline(3.0);
        assert!(buffer.placements.is_none());
        let mut expected = shaped[..2].to_vec();
        for glyph in &mut expected {
            glyph.y += 3.0;
        }
        assert_places_glyphs_as_shaped(&buffer, &expected);
    }

    #[test]
    fn moving_a_glyph_stores_placements_as_they_were() {
        let shaped = plain_run();
        let mut buffer = GlyphBuffer::from_shaped(&shaped);
        buffer.update_positions(|_, _, _| {});
        assert!(buffer.placements.is_some());
        assert_places_glyphs_as_shaped(&buffer, &shaped);
    }

    #[test]
    fn a_remainder_starts_at_the_start_of_the_run_and_of_the_text() {
        let shaped = plain_run();
        let expected = [left_to_right_glyph(0.0, 4.25, 0), left_to_right_glyph(4.25, 8.0, 1)];

        let derived = GlyphBuffer::from_shaped(&shaped).remainder_from(1);
        assert!(derived.placements.is_none());
        assert_places_glyphs_as_shaped(&derived, &expected);

        let mut stored = GlyphBuffer::from_shaped(&shaped);
        stored.update_positions(|_, _, _| {});
        let stored = stored.remainder_from(1);
        assert!(stored.placements.is_some());
        assert_places_glyphs_as_shaped(&stored, &expected);
    }

    #[test]
    fn appending_at_the_end_of_a_run_keeps_placements_derived() {
        let first = plain_run();
        let second = plain_run();
        let mut buffer = GlyphBuffer::from_shaped(&first);
        let appended = GlyphBuffer::from_shaped(&second);
        let mut end = None;

        assert!(buffer.append_keeping_placements_derived(&mut end, &appended, 20.75, 3));
        assert!(buffer.append_keeping_placements_derived(&mut end, &appended, 41.5, 6));
        assert!(buffer.placements.is_none());

        let mut by_placing_glyphs = GlyphBuffer::from_shaped(&first);
        for (x_offset, cluster_offset) in [(20.75, 3), (41.5, 6)] {
            for placed in appended.placed() {
                by_placing_glyphs.push_placed(PlacedGlyph {
                    x: placed.x + x_offset,
                    cluster_start: placed.cluster_start + cluster_offset,
                    ..placed
                });
            }
        }
        assert!(by_placing_glyphs.placements.is_some());
        let shaped: Vec<ShapedGlyph> = by_placing_glyphs.placed().map(ShapedGlyph::from).collect();
        assert_places_glyphs_as_shaped(&buffer, &shaped);
    }

    #[test]
    fn appending_anywhere_else_changes_nothing() {
        let appended = GlyphBuffer::from_shaped(&plain_run());
        let original = GlyphBuffer::from_shaped(&plain_run());
        let mut buffer = original.clone();
        let mut end = None;

        assert!(!buffer.append_keeping_placements_derived(&mut end, &appended, 21.0, 3));
        assert!(!buffer.append_keeping_placements_derived(&mut end, &appended, 20.75, 4));

        let mut other_baseline = appended.clone();
        other_baseline.shift_baseline(1.0);
        assert!(!buffer.append_keeping_placements_derived(&mut end, &other_baseline, 20.75, 3));

        let mut stored = appended.clone();
        stored.update_positions(|_, _, _| {});
        assert!(!buffer.append_keeping_placements_derived(&mut end, &stored, 20.75, 3));

        assert!(buffer == original);
        assert!(buffer.placements.is_none());

        let mut far_along = buffer.clone();
        far_along.truncate(0);
        let mut huge = plain_run();
        huge[0].glyph_width = MAX_EXACT_PEN_POSITION - 4.0;
        huge[1].x = MAX_EXACT_PEN_POSITION - 4.0;
        huge[2].x = MAX_EXACT_PEN_POSITION + 0.25;
        huge.truncate(1);
        let huge = GlyphBuffer::from_shaped(&huge);
        assert!(huge.placements.is_none());
        assert!(far_along.append_keeping_placements_derived(&mut end, &huge, 0.0, 0));
        assert!(!far_along.append_keeping_placements_derived(&mut end, &appended, MAX_EXACT_PEN_POSITION - 4.0, 1));
    }

    #[test]
    fn appending_after_the_run_shrank_adds_up_where_it_ends_now() {
        let appended = GlyphBuffer::from_shaped(&plain_run());
        let mut buffer = GlyphBuffer::from_shaped(&plain_run());
        let mut end = None;
        assert!(buffer.append_keeping_placements_derived(&mut end, &appended, 20.75, 3));
        buffer.truncate(2);
        assert!(buffer.append_keeping_placements_derived(&mut end, &appended, 12.75, 2));
        assert_eq!(buffer.len(), 5);
    }

    #[test]
    fn runs_compare_by_where_they_place_glyphs_however_they_store_it() {
        let shaped = plain_run();
        let derived = GlyphBuffer::from_shaped(&shaped);
        let mut stored = derived.clone();
        stored.update_positions(|_, _, _| {});
        assert!(derived == stored);
        stored.update_positions(|_, x, _| *x += 1.0);
        assert!(derived != stored);
    }
}

#[cfg(test)]
mod shaping_cache_tests {
    use super::*;

    fn shape_with_glyph_count(glyph_count: usize) -> CachedShape {
        CachedShape {
            glyphs: GlyphBuffer::from_shaped(&vec![ShapedGlyph::default(); glyph_count]),
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
