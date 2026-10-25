/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The OpenType MATH table of a font, as HarfBuzz reads it.
//! https://learn.microsoft.com/en-us/typography/opentype/spec/math

use crate::font::FontHandle;
use std::ffi::c_void;

unsafe extern "C" {
    fn ladybird_gfx_font_math_data(font: *const c_void) -> *const FfiMathData;
    fn ladybird_gfx_font_glyph_metrics(font: *const c_void, glyph_id: u32, out_metrics: *mut FfiGlyphMetrics);
    fn ladybird_gfx_font_math_glyph_variants(
        font: *const c_void,
        glyph_id: u32,
        horizontal: bool,
        out_variants: *mut FfiMathGlyphVariant,
        capacity: usize,
    ) -> usize;
    fn ladybird_gfx_font_math_glyph_assembly(
        font: *const c_void,
        glyph_id: u32,
        horizontal: bool,
        out_parts: *mut FfiMathGlyphPart,
        capacity: usize,
        out_italics_correction: *mut f32,
    ) -> usize;
}

/// What LibGfx reads once per font from its OpenType MATH and post tables. Lengths are in pixels.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct FfiMathData {
    pub has_math_table: bool,
    /// The MathConstants table, indexed by `MathConstant`. Percentages are in percent, every other constant is a
    /// length.
    pub constants: [f32; 56],
    pub horizontal_min_connector_overlap: f32,
    pub vertical_min_connector_overlap: f32,
    pub has_underline_thickness: bool,
    /// post.underlineThickness.
    pub underline_thickness: f32,
}

/// The ink extents of a glyph and its advance, in pixels.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FfiGlyphMetrics {
    pub advance: f32,
    /// How far the ink of the glyph extends above the baseline.
    pub ascent: f32,
    /// How far the ink of the glyph extends below the baseline.
    pub descent: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FfiMathGlyphVariant {
    pub glyph_id: u32,
    pub advance_measurement: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FfiMathGlyphPart {
    pub glyph_id: u32,
    pub start_connector_length: f32,
    pub end_connector_length: f32,
    pub full_advance: f32,
    pub is_extender: bool,
}

/// The constants of the MathConstants table, numbered as HarfBuzz numbers them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum MathConstant {
    ScriptPercentScaleDown = 0,
    ScriptScriptPercentScaleDown = 1,
    DelimitedSubFormulaMinHeight = 2,
    DisplayOperatorMinHeight = 3,
    MathLeading = 4,
    AxisHeight = 5,
    AccentBaseHeight = 6,
    FlattenedAccentBaseHeight = 7,
    SubscriptShiftDown = 8,
    SubscriptTopMax = 9,
    SubscriptBaselineDropMin = 10,
    SuperscriptShiftUp = 11,
    SuperscriptShiftUpCramped = 12,
    SuperscriptBottomMin = 13,
    SuperscriptBaselineDropMax = 14,
    SubSuperscriptGapMin = 15,
    SuperscriptBottomMaxWithSubscript = 16,
    SpaceAfterScript = 17,
    UpperLimitGapMin = 18,
    UpperLimitBaselineRiseMin = 19,
    LowerLimitGapMin = 20,
    LowerLimitBaselineDropMin = 21,
    StackTopShiftUp = 22,
    StackTopDisplayStyleShiftUp = 23,
    StackBottomShiftDown = 24,
    StackBottomDisplayStyleShiftDown = 25,
    StackGapMin = 26,
    StackDisplayStyleGapMin = 27,
    StretchStackTopShiftUp = 28,
    StretchStackBottomShiftDown = 29,
    StretchStackGapAboveMin = 30,
    StretchStackGapBelowMin = 31,
    FractionNumeratorShiftUp = 32,
    FractionNumeratorDisplayStyleShiftUp = 33,
    FractionDenominatorShiftDown = 34,
    FractionDenominatorDisplayStyleShiftDown = 35,
    FractionNumeratorGapMin = 36,
    FractionNumDisplayStyleGapMin = 37,
    FractionRuleThickness = 38,
    FractionDenominatorGapMin = 39,
    FractionDenomDisplayStyleGapMin = 40,
    SkewedFractionHorizontalGap = 41,
    SkewedFractionVerticalGap = 42,
    OverbarVerticalGap = 43,
    OverbarRuleThickness = 44,
    OverbarExtraAscender = 45,
    UnderbarVerticalGap = 46,
    UnderbarRuleThickness = 47,
    UnderbarExtraDescender = 48,
    RadicalVerticalGap = 49,
    RadicalDisplayStyleVerticalGap = 50,
    RadicalRuleThickness = 51,
    RadicalExtraAscender = 52,
    RadicalKernBeforeDegree = 53,
    RadicalKernAfterDegree = 54,
    RadicalDegreeBottomRaisePercent = 55,
}

impl FfiMathData {
    /// The value of a constant of the MathConstants table, or `None` if the font has no MATH table.
    pub fn constant(&self, constant: MathConstant) -> Option<f32> {
        self.has_math_table.then(|| self.constants[constant as usize])
    }

    pub fn underline_thickness(&self) -> Option<f32> {
        self.has_underline_thickness.then_some(self.underline_thickness)
    }

    pub fn min_connector_overlap(&self, axis: StretchAxis) -> f32 {
        match axis {
            StretchAxis::Inline => self.horizontal_min_connector_overlap,
            StretchAxis::Block => self.vertical_min_connector_overlap,
        }
    }
}

/// The direction a glyph is stretched along, in a horizontal writing mode.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StretchAxis {
    Inline,
    Block,
}

/// A GlyphAssembly table, with the italics correction it gives.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MathGlyphAssembly {
    pub parts: Vec<FfiMathGlyphPart>,
    pub italics_correction: f32,
}

impl FontHandle {
    /// The MATH table constants and related metrics of the font, read once for the font's lifetime.
    pub fn math_data(&self) -> &FfiMathData {
        // SAFETY: The handle's retained reference keeps the font live, and the font keeps the data it read for as long
        // as it lives, never changing it.
        unsafe { &*ladybird_gfx_font_math_data(self.as_raw()) }
    }

    pub fn glyph_metrics(&self, glyph_id: u32) -> FfiGlyphMetrics {
        let mut metrics = FfiGlyphMetrics::default();
        // SAFETY: The handle's retained reference keeps the font live, and the out-pointer addresses a local.
        unsafe { ladybird_gfx_font_glyph_metrics(self.as_raw(), glyph_id, &raw mut metrics) };
        metrics
    }

    /// The MathGlyphVariantRecords of the MathGlyphConstruction table of the glyph along the axis.
    pub fn math_glyph_variants(&self, glyph_id: u32, axis: StretchAxis) -> Vec<FfiMathGlyphVariant> {
        let horizontal = axis == StretchAxis::Inline;
        let mut variants = vec![FfiMathGlyphVariant::default(); 8];
        // SAFETY: The handle's retained reference keeps the font live, and the buffer holds `capacity` entries.
        let count = unsafe {
            ladybird_gfx_font_math_glyph_variants(
                self.as_raw(),
                glyph_id,
                horizontal,
                variants.as_mut_ptr(),
                variants.len(),
            )
        };
        if count > variants.len() {
            variants.resize(count, FfiMathGlyphVariant::default());
            // SAFETY: As above.
            unsafe {
                ladybird_gfx_font_math_glyph_variants(
                    self.as_raw(),
                    glyph_id,
                    horizontal,
                    variants.as_mut_ptr(),
                    variants.len(),
                )
            };
        }
        variants.truncate(count);
        variants
    }

    /// The GlyphAssembly table of the MathGlyphConstruction table of the glyph along the axis, if it has one.
    pub fn math_glyph_assembly(&self, glyph_id: u32, axis: StretchAxis) -> Option<MathGlyphAssembly> {
        let horizontal = axis == StretchAxis::Inline;
        let mut parts = vec![FfiMathGlyphPart::default(); 8];
        let mut italics_correction = 0.0;
        // SAFETY: The handle's retained reference keeps the font live, the buffer holds `capacity` entries, and the
        // out-pointer addresses a local.
        let count = unsafe {
            ladybird_gfx_font_math_glyph_assembly(
                self.as_raw(),
                glyph_id,
                horizontal,
                parts.as_mut_ptr(),
                parts.len(),
                &raw mut italics_correction,
            )
        };
        if count == 0 {
            return None;
        }
        if count > parts.len() {
            parts.resize(count, FfiMathGlyphPart::default());
            // SAFETY: As above.
            unsafe {
                ladybird_gfx_font_math_glyph_assembly(
                    self.as_raw(),
                    glyph_id,
                    horizontal,
                    parts.as_mut_ptr(),
                    parts.len(),
                    &raw mut italics_correction,
                )
            };
        }
        parts.truncate(count);
        Some(MathGlyphAssembly {
            parts,
            italics_correction,
        })
    }
}
