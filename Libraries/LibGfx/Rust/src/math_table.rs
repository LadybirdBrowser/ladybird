/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The OpenType MATH table of a font, as HarfBuzz reads it.
//! https://learn.microsoft.com/en-us/typography/opentype/spec/math

use crate::font::FontHandle;
use crate::harfbuzz::{self, HbFont, HbGlyphExtents, HbOtMathGlyphPart, HbOtMathGlyphVariant};
use crate::text_shaping::TEXT_SHAPING_RESOLUTION;
use std::ptr::null_mut;

/// What a font handle reads once from the OpenType MATH and post tables of its font. Lengths are in pixels.
#[derive(Clone, Copy, Debug)]
pub struct MathData {
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
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GlyphMetrics {
    pub advance: f32,
    /// How far the ink of the glyph extends above the baseline.
    pub ascent: f32,
    /// How far the ink of the glyph extends below the baseline.
    pub descent: f32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct MathGlyphVariant {
    pub glyph_id: u32,
    pub advance_measurement: f32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct MathGlyphPart {
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

impl MathData {
    /// https://learn.microsoft.com/en-us/typography/opentype/spec/math#mathconstants-table
    fn read(font: *mut HbFont) -> Self {
        let mut data = Self {
            has_math_table: false,
            constants: [0.0; 56],
            horizontal_min_connector_overlap: 0.0,
            vertical_min_connector_overlap: 0.0,
            has_underline_thickness: false,
            underline_thickness: 0.0,
        };
        // SAFETY: The caller gives a live, immutable HarfBuzz font, and the out-pointer addresses a local.
        unsafe {
            data.has_math_table = harfbuzz::hb_ot_math_has_data(harfbuzz::hb_font_get_face(font)) != 0;
            if data.has_math_table {
                for (constant, value) in (0..).zip(&mut data.constants) {
                    let position = harfbuzz::hb_ot_math_get_constant(font, constant);
                    // NB: HarfBuzz scales every constant to the font size, except for the three percentages.
                    let is_percentage = matches!(
                        constant,
                        harfbuzz::OT_MATH_CONSTANT_SCRIPT_PERCENT_SCALE_DOWN
                            | harfbuzz::OT_MATH_CONSTANT_SCRIPT_SCRIPT_PERCENT_SCALE_DOWN
                            | harfbuzz::OT_MATH_CONSTANT_RADICAL_DEGREE_BOTTOM_RAISE_PERCENT
                    );
                    *value = if is_percentage {
                        position as f32
                    } else {
                        position_to_pixels(position)
                    };
                }
                data.horizontal_min_connector_overlap = position_to_pixels(
                    harfbuzz::hb_ot_math_get_min_connector_overlap(font, harfbuzz::DIRECTION_LTR),
                );
                data.vertical_min_connector_overlap = position_to_pixels(
                    harfbuzz::hb_ot_math_get_min_connector_overlap(font, harfbuzz::DIRECTION_BTT),
                );
            }

            let mut position = 0;
            data.has_underline_thickness =
                harfbuzz::hb_ot_metrics_get_position(font, harfbuzz::OT_METRICS_TAG_UNDERLINE_SIZE, &raw mut position)
                    != 0;
            data.underline_thickness = position_to_pixels(position);
        }
        data
    }

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

impl StretchAxis {
    fn harfbuzz_direction(self) -> u32 {
        match self {
            Self::Inline => harfbuzz::DIRECTION_LTR,
            Self::Block => harfbuzz::DIRECTION_BTT,
        }
    }
}

/// A GlyphAssembly table, with the italics correction it gives.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MathGlyphAssembly {
    pub parts: Vec<MathGlyphPart>,
    pub italics_correction: f32,
}

fn position_to_pixels(position: i32) -> f32 {
    position as f32 / TEXT_SHAPING_RESOLUTION
}

impl FontHandle {
    /// The MATH table constants and related metrics of the font, read once for the lifetime of the handle.
    pub fn math_data(&self) -> &MathData {
        self.math_data_cell()
            .get_or_init(|| Box::new(MathData::read(self.harfbuzz_font())))
    }

    pub fn glyph_metrics(&self, glyph_id: u32) -> GlyphMetrics {
        let font = self.harfbuzz_font();
        let mut extents = HbGlyphExtents::default();
        // SAFETY: The handle keeps its immutable HarfBuzz font live, and the out-pointer addresses a local.
        unsafe {
            if harfbuzz::hb_font_get_glyph_extents(font, glyph_id, &raw mut extents) == 0 {
                extents = HbGlyphExtents::default();
            }
            GlyphMetrics {
                advance: position_to_pixels(harfbuzz::hb_font_get_glyph_h_advance(font, glyph_id)),
                ascent: position_to_pixels(extents.y_bearing),
                descent: -position_to_pixels(extents.y_bearing.wrapping_add(extents.height)),
            }
        }
    }

    /// The MathGlyphVariantRecords of the MathGlyphConstruction table of the glyph along the axis.
    pub fn math_glyph_variants(&self, glyph_id: u32, axis: StretchAxis) -> Vec<MathGlyphVariant> {
        let (font, direction) = (self.harfbuzz_font(), axis.harfbuzz_direction());
        // SAFETY: The handle keeps its immutable HarfBuzz font live. HarfBuzz gives the number of records to a call
        //         without a buffer, and the buffer of the second call holds that many.
        let variants = unsafe {
            let mut count =
                harfbuzz::hb_ot_math_get_glyph_variants(font, glyph_id, direction, 0, null_mut(), null_mut());
            let mut variants = vec![HbOtMathGlyphVariant::default(); count as usize];
            harfbuzz::hb_ot_math_get_glyph_variants(
                font,
                glyph_id,
                direction,
                0,
                &raw mut count,
                variants.as_mut_ptr(),
            );
            variants
        };
        variants
            .iter()
            .map(|variant| MathGlyphVariant {
                glyph_id: variant.glyph,
                advance_measurement: position_to_pixels(variant.advance),
            })
            .collect()
    }

    /// The GlyphAssembly table of the MathGlyphConstruction table of the glyph along the axis, if it has one.
    pub fn math_glyph_assembly(&self, glyph_id: u32, axis: StretchAxis) -> Option<MathGlyphAssembly> {
        let (font, direction) = (self.harfbuzz_font(), axis.harfbuzz_direction());
        let mut italics_correction = 0;
        // SAFETY: As in math_glyph_variants(), and the last out-pointer addresses a local.
        let parts = unsafe {
            let mut count = harfbuzz::hb_ot_math_get_glyph_assembly(
                font,
                glyph_id,
                direction,
                0,
                null_mut(),
                null_mut(),
                null_mut(),
            );
            let mut parts = vec![HbOtMathGlyphPart::default(); count as usize];
            harfbuzz::hb_ot_math_get_glyph_assembly(
                font,
                glyph_id,
                direction,
                0,
                &raw mut count,
                parts.as_mut_ptr(),
                &raw mut italics_correction,
            );
            parts
        };
        (!parts.is_empty()).then(|| MathGlyphAssembly {
            parts: parts
                .iter()
                .map(|part| MathGlyphPart {
                    glyph_id: part.glyph,
                    start_connector_length: position_to_pixels(part.start_connector_length),
                    end_connector_length: position_to_pixels(part.end_connector_length),
                    full_advance: position_to_pixels(part.full_advance),
                    is_extender: part.flags & harfbuzz::OT_MATH_GLYPH_PART_FLAG_EXTENDER != 0,
                })
                .collect(),
            italics_correction: position_to_pixels(italics_correction),
        })
    }
}
