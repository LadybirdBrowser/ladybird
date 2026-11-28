/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shaping and measuring text with HarfBuzz.

use crate::font::{FfiShapeFeature, FfiShapingFont, FontHandle};
use crate::harfbuzz::{self, HbBuffer, HbFeature, HbFont, HbGlyphInfo, HbGlyphPosition};
use crate::text_layout::{JoiningContext, ShapedGlyph, TextType};
use std::ffi::{c_int, c_uint, c_void};

/// HarfBuzz positions glyphs in 1/64ths of a pixel, as `Gfx::text_shaping_resolution` says.
pub(crate) const TEXT_SHAPING_RESOLUTION: f32 = 64.0;

unsafe extern "C" {
    fn ladybird_gfx_code_point_has_cursive_script_extension(code_point: u32) -> bool;
    fn ladybird_gfx_code_point_is_default_ignorable(code_point: u32) -> bool;
}

/// How the code units of a text are stored, as an AK `Utf16View` stores them.
#[allow(dead_code, reason = "Gfx::Font::width() picks the storage")]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum TextStorage {
    /// One byte per code unit, none above U+007F.
    Ascii,
    /// One byte per code unit.
    Latin1,
    Utf16,
}

/// The code units of a text, in the storage they have.
#[derive(Clone, Copy)]
enum ShapingText<'a> {
    Ascii(&'a [u8]),
    Latin1(&'a [u8]),
    Utf16(&'a [u16]),
}

impl<'a> ShapingText<'a> {
    /// A text as layout has it. An empty one is in the storage of an empty `Utf16View`, which is ASCII.
    fn from_utf16(text: &'a [u16]) -> Self {
        if text.is_empty() {
            Self::Ascii(&[])
        } else {
            Self::Utf16(text)
        }
    }
}

/// What HarfBuzz needs of a font to shape text with it.
#[derive(Clone, Copy)]
pub(crate) struct ShapingFont<'a> {
    pub(crate) harfbuzz_font: *mut HbFont,
    pub(crate) features: &'a [FfiShapeFeature],
}

impl<'a> From<&'a FontHandle> for ShapingFont<'a> {
    fn from(font: &'a FontHandle) -> Self {
        Self {
            harfbuzz_font: font.harfbuzz_font(),
            features: font.shape_features(),
        }
    }
}

/// A HarfBuzz buffer that this thread made and that nothing else refers to.
struct ShapingBuffer(*mut HbBuffer);

impl Drop for ShapingBuffer {
    fn drop(&mut self) {
        // SAFETY: The buffer was made by hb_buffer_create(), and is let go of once.
        unsafe { harfbuzz::hb_buffer_destroy(self.0) };
    }
}

impl ShapingBuffer {
    fn glyph_infos(&self) -> &[HbGlyphInfo] {
        let mut length: c_uint = 0;
        // SAFETY: The buffer is live, and the infos stay valid until the buffer changes, which it does not while they
        //         are borrowed from it.
        unsafe {
            let infos = harfbuzz::hb_buffer_get_glyph_infos(self.0, &raw mut length);
            ak::slice_from_raw_parts(infos, length as usize)
        }
    }

    fn glyph_positions(&self) -> &[HbGlyphPosition] {
        let mut length: c_uint = 0;
        // SAFETY: As for the infos above.
        unsafe {
            let positions = harfbuzz::hb_buffer_get_glyph_positions(self.0, &raw mut length);
            ak::slice_from_raw_parts(positions, length as usize)
        }
    }

    fn is_right_to_left(&self) -> bool {
        // SAFETY: The buffer is live.
        unsafe { harfbuzz::hb_buffer_get_direction(self.0) == harfbuzz::DIRECTION_RTL }
    }
}

const ZERO_WIDTH_JOINER: u32 = 0x200D;
const EMPTY_FEATURE: HbFeature = HbFeature {
    tag: 0,
    value: 0,
    start: 0,
    end: 0,
};

fn global_feature(tag: u32, value: u32) -> HbFeature {
    HbFeature {
        tag,
        value,
        start: 0,
        end: harfbuzz::FEATURE_GLOBAL_END,
    }
}

fn shape_into_buffer(
    font: ShapingFont,
    text: ShapingText,
    text_type: TextType,
    letter_spacing: f32,
    joining_context: JoiningContext,
    vertical_forms: bool,
) -> ShapingBuffer {
    // SAFETY: hb_buffer_create() gives a buffer that only this call refers to, or HarfBuzz's inert empty one, which
    //         every call below takes too.
    let buffer = ShapingBuffer(unsafe { harfbuzz::hb_buffer_create() });

    // A zero width joiner as context before or after the text gives the letters at its edges the forms that join them
    // to letters outside it.
    if joining_context.joins_before {
        // SAFETY: The buffer is live, and the joiner is one code point.
        unsafe { harfbuzz::hb_buffer_add_codepoints(buffer.0, &ZERO_WIDTH_JOINER, 1, 1, 0) };
    }

    // SAFETY: The buffer is live, and each text is as long as the length given with it.
    unsafe {
        match text {
            ShapingText::Ascii(text) => {
                harfbuzz::hb_buffer_add_utf8(buffer.0, text.as_ptr(), text.len() as c_int, 0, -1);
            }
            ShapingText::Latin1(text) => {
                harfbuzz::hb_buffer_add_latin1(buffer.0, text.as_ptr(), text.len() as c_int, 0, -1);
            }
            ShapingText::Utf16(text) => {
                harfbuzz::hb_buffer_add_utf16(buffer.0, text.as_ptr(), text.len() as c_int, 0, -1);
            }
        }
        if let ShapingText::Ascii(_) = text {
            // Fast path for ASCII: we know it's Latin script, LTR direction.
            harfbuzz::hb_buffer_set_script(buffer.0, harfbuzz::SCRIPT_LATIN);
            harfbuzz::hb_buffer_set_direction(buffer.0, harfbuzz::DIRECTION_LTR);
        } else {
            // For non-ASCII, set direction from text_type if known, otherwise guess.
            match text_type {
                TextType::Ltr => harfbuzz::hb_buffer_set_direction(buffer.0, harfbuzz::DIRECTION_LTR),
                TextType::Rtl => harfbuzz::hb_buffer_set_direction(buffer.0, harfbuzz::DIRECTION_RTL),
                TextType::Common | TextType::ContextDependent | TextType::EndPadding => {}
            }
            harfbuzz::hb_buffer_guess_segment_properties(buffer.0);
        }
    }

    if joining_context.joins_after {
        // SAFETY: The buffer is live, and the joiner is one code point.
        unsafe { harfbuzz::hb_buffer_add_codepoints(buffer.0, &ZERO_WIDTH_JOINER, 1, 0, 0) };
    }

    // NB: Most fonts are shaped with few features, which then need no allocation.
    const INLINE_FEATURE_COUNT: usize = 16;
    const LETTER_SPACING_FEATURE_COUNT: usize = 4;
    let most_features = usize::from(vertical_forms) + font.features.len() + LETTER_SPACING_FEATURE_COUNT;
    let mut inline_features = [EMPTY_FEATURE; INLINE_FEATURE_COUNT];
    let mut heap_features = Vec::new();
    let features: &mut [HbFeature] = if most_features <= INLINE_FEATURE_COUNT {
        &mut inline_features
    } else {
        heap_features.resize(most_features, EMPTY_FEATURE);
        &mut heap_features
    };
    let mut feature_count = 0;
    let mut push_feature = |feature: HbFeature| {
        features[feature_count] = feature;
        feature_count += 1;
    };

    // https://drafts.csswg.org/css-writing-modes-4/#typeset-upright
    // "Additionally, font features (such as alternate glyphs and other transformation) intended for use in vertical
    //  typesetting must be used. (E.g. the OpenType vert feature must be enabled.)"
    // NB: That gives glyphs set upright the vertical forms the font has for them: the vertical long vowel mark,
    //     brackets that open downwards, and punctuation that sits in the top-right corner of the em box.
    // NB: It goes first, so that font-feature-settings can still turn it off.
    if vertical_forms {
        push_feature(global_feature(harfbuzz::tag(*b"vert"), 1));
    }

    for feature in font.features {
        push_feature(global_feature(harfbuzz::tag(feature.tag), feature.value));
    }

    // https://drafts.csswg.org/css-fonts-4/#feature-variation-precedence
    // 11. Feature settings determined by properties other than font-variant or font-feature-settings are applied. For
    //     example, setting a non-default value for the letter-spacing property disables optional ligatures.
    // NB: The optional ligatures are the ones CSS Text names: those not required for fundamentally correct glyph
    //     shaping, which leaves rlig on. A feature that font-feature-settings set stays as it is, since step 13
    //     applies font-feature-settings after this step. And HarfBuzz applies the last of the features with one tag,
    //     so these go after the font's own.
    if letter_spacing != 0.0 {
        for tag in [*b"liga", *b"clig", *b"dlig", *b"hlig"].map(harfbuzz::tag) {
            let set_by_feature_settings = font
                .features
                .iter()
                .any(|feature| feature.set_by_feature_settings && harfbuzz::tag(feature.tag) == tag);
            if !set_by_feature_settings {
                push_feature(global_feature(tag, 0));
            }
        }
    }

    // SAFETY: The font is immutable, which makes it safe to shape with on any number of threads at once (see
    //         FontEntry), the buffer is this thread's own, and the features are feature_count initialized ones.
    unsafe { harfbuzz::hb_shape(font.harfbuzz_font, buffer.0, features.as_ptr(), feature_count as c_uint) };

    buffer
}

// https://drafts.csswg.org/css-text-4/#word-separator
fn is_word_separator(code_point: u32) -> bool {
    // Word-separator characters include the space (U+0020), the no-break space (U+00A0), the Ethiopic word space
    // (U+1361), the Aegean word separators (U+10100,U+10101), the Ugaritic word divider (U+1039F), and the Phoenician
    // Word Separator (U+1091F).
    // INTEROP: Only the space and no-break space are treated as word separators. The line feed is also included
    //          because whitespace collapsing can leave a segment break in shaped text, where it behaves as a space.
    code_point == 0x0020 || code_point == 0x00A0 || code_point == 0x000A
}

// https://drafts.csswg.org/css-text-3/#cursive-script
fn is_cursive_script(code_point: u32) -> bool {
    // SAFETY: The default Unicode functions are HarfBuzz's immutable built-in ones, which any thread may use.
    let script = unsafe { harfbuzz::hb_unicode_script(harfbuzz::hb_unicode_funcs_get_default(), code_point) };
    match script {
        harfbuzz::SCRIPT_ARABIC
        | harfbuzz::SCRIPT_HANIFI_ROHINGYA
        | harfbuzz::SCRIPT_MANDAIC
        | harfbuzz::SCRIPT_MONGOLIAN
        | harfbuzz::SCRIPT_NKO
        | harfbuzz::SCRIPT_PHAGS_PA
        | harfbuzz::SCRIPT_SYRIAC => true,
        // https://drafts.csswg.org/css-text-3/#character-properties
        // script property: Defined in Unicode Standard Annex #24 [UAX24] and given as the Script property in the
        // Unicode Character Database [UAX44]. (UAs must include any ScriptExtensions.txt assignments in this mapping.)
        // NB: So a character that serves several scripts, as the tatweel (U+0640) does, belongs to a cursive script
        //     when one of its script extensions is one.
        // SAFETY: The lookup reads only immutable Unicode tables.
        harfbuzz::SCRIPT_COMMON | harfbuzz::SCRIPT_INHERITED => unsafe {
            ladybird_gfx_code_point_has_cursive_script_extension(code_point)
        },
        _ => false,
    }
}

fn is_utf16_high_surrogate(code_unit: u16) -> bool {
    (0xD800..=0xDBFF).contains(&code_unit)
}

fn is_utf16_low_surrogate(code_unit: u16) -> bool {
    (0xDC00..=0xDFFF).contains(&code_unit)
}

fn decode_utf16_surrogate_pair(high_surrogate: u16, low_surrogate: u16) -> u32 {
    ((u32::from(high_surrogate) - 0xD800) << 10) + (u32::from(low_surrogate) - 0xDC00) + 0x10000
}

/// The code point at `index`, as `Utf16View::code_point_at()` gives it: a lone surrogate stands for itself.
fn code_point_at(text: &[u16], index: usize) -> u32 {
    let code_unit = text[index];
    if !is_utf16_high_surrogate(code_unit) || index + 1 == text.len() {
        return u32::from(code_unit);
    }
    let second = text[index + 1];
    if !is_utf16_low_surrogate(second) {
        return u32::from(code_unit);
    }
    decode_utf16_surrogate_pair(code_unit, second)
}

/// The code points of the text, as a `Utf16View` iterates them: a lone surrogate stands for itself.
fn code_points(text: &[u16]) -> impl Iterator<Item = u32> + '_ {
    let mut index = 0;
    std::iter::from_fn(move || {
        let &code_unit = text.get(index)?;
        if is_utf16_high_surrogate(code_unit)
            && let Some(&next_code_unit) = text.get(index + 1)
            && is_utf16_low_surrogate(next_code_unit)
        {
            index += 2;
            return Some(decode_utf16_surrogate_pair(code_unit, next_code_unit));
        }
        index += 1;
        Some(u32::from(code_unit))
    })
}

// Whether letter-spacing goes after the last glyph of the cluster starting at cluster_start: Every cluster takes it but
// one that starts with a character of a cursive script.
fn cluster_takes_letter_spacing(text: &[u16], cluster_start: u32) -> bool {
    // https://drafts.csswg.org/css-text-3/#cursive-tracking
    // Otherwise, if the UA cannot expand text from a cursive script without breaking its cursive connections, it must
    // not apply spacing between any pair of that script's typographic letter units at all (effectively treating each
    // word as a single typographic letter unit for the purpose of letter-spacing).
    // NB: Spaces still take it: Their script is Common, with no extensions.
    cluster_start as usize >= text.len() || !is_cursive_script(code_point_at(text, cluster_start as usize))
}

fn is_hangul_filler(code_point: u32) -> bool {
    code_point == 0x115F || code_point == 0x1160 || code_point == 0x3164 || code_point == 0xFFA0
}

fn is_ascii_space(code_unit: u16) -> bool {
    matches!(code_unit, 0x20 | 0x09 | 0x0A | 0x0B | 0x0C | 0x0D)
}

// https://drafts.csswg.org/css-text-3/#white-space-phase-2
// 4. If there remains any sequence of white space, other space separators, and/or preserved tabs at the end of a line
//    (after bidi reordering [CSS-WRITING-MODES-4]):
//    - If white-space is set to normal, nowrap, or pre-line, the UA must hang this sequence (unconditionally).
// AD-HOC: Of the other space separators, only the ideographic space (U+3000) counts, so the rest (the em space, e.g.)
//         stay on the line.
fn is_trailing_space(code_unit: u16) -> bool {
    is_ascii_space(code_unit) || code_unit == 0x3000
}

fn length_of_trailing_run(text: &[u16], is_in_run: impl Fn(u16) -> bool) -> usize {
    text.iter().rev().take_while(|&&code_unit| is_in_run(code_unit)).count()
}

/// The glyphs of a text shaped from its origin, with what layout needs to know about them.
pub(crate) struct Shape {
    pub(crate) glyphs: Vec<ShapedGlyph>,
    pub(crate) width: f32,
    pub(crate) trailing_whitespace_length_in_code_units: usize,
    /// The trailing whitespace advance is recorded so that trimming it at line end can subtract exactly what shaping
    /// added, spacing included.
    pub(crate) trailing_whitespace_advance: f32,
    /// The advance of the ASCII white space at the very end alone, which intrinsic sizing takes off, since an
    /// ideographic space that hangs at the end of a line still counts toward its intrinsic sizes.
    pub(crate) trailing_ascii_whitespace_advance: f32,
    pub(crate) requires_context_for_breaks: bool,
}

pub(crate) fn shape_text(
    font: &FontHandle,
    text: &[u16],
    text_type: TextType,
    letter_spacing: f32,
    word_spacing: f32,
    joining_context: JoiningContext,
    vertical_forms: bool,
) -> Shape {
    let ascent = font.facts().ascent;
    let buffer = shape_into_buffer(
        ShapingFont::from(font),
        ShapingText::from_utf16(text),
        text_type,
        letter_spacing,
        joining_context,
        vertical_forms,
    );
    let glyph_info = buffer.glyph_infos();
    let positions = buffer.glyph_positions();
    let glyph_count = glyph_info.len();
    let text_length = text.len();

    let mut glyphs = Vec::with_capacity(glyph_count);
    let mut point_x = 0.0f32;
    let mut point_y = 0.0f32;
    let mut requires_context_for_breaks = false;

    let trailing_whitespace_length_in_code_units = length_of_trailing_run(text, is_trailing_space);
    let mut trailing_whitespace_advance = 0.0f32;
    let mut trailing_ascii_whitespace_advance = 0.0f32;
    let first_trailing_whitespace_offset = text_length - trailing_whitespace_length_in_code_units;
    let first_trailing_ascii_whitespace_offset = text_length - length_of_trailing_run(text, is_ascii_space);

    let is_right_to_left = buffer.is_right_to_left();
    let cluster_length_in_code_units = |index: usize| -> usize {
        let starting_offset = glyph_info[index].cluster;
        let different_cluster = if is_right_to_left {
            glyph_info[..index]
                .iter()
                .rev()
                .find(|info| info.cluster != starting_offset)
        } else {
            glyph_info[index + 1..]
                .iter()
                .find(|info| info.cluster != starting_offset)
        };
        match different_cluster {
            Some(info) => info.cluster.wrapping_sub(starting_offset) as usize,
            None => text_length.wrapping_sub(starting_offset as usize),
        }
    };

    let glyph_length_in_code_units = |index: usize| -> usize {
        if index > 0 && glyph_info[index - 1].cluster == glyph_info[index].cluster {
            return 0;
        }
        cluster_length_in_code_units(index)
    };

    // A glyph is left unpainted only when everything in its cluster is default-ignorable. A cluster that merely starts
    // with one, such as a Hangul syllable whose initial is the choseong filler, still shows its other characters.
    let should_paint_glyph = |index: usize| -> bool {
        let starting_offset = glyph_info[index].cluster as usize;
        if starting_offset >= text_length {
            return true;
        }
        let cluster = &text[starting_offset..starting_offset + cluster_length_in_code_units(index)];
        let mut has_hangul_filler = false;
        for code_point in code_points(cluster) {
            if is_hangul_filler(code_point) {
                has_hangul_filler = true;
                continue;
            }
            // SAFETY: The lookup reads only immutable Unicode tables.
            if !unsafe { ladybird_gfx_code_point_is_default_ignorable(code_point) } {
                return true;
            }
        }
        // AD-HOC: The Hangul fillers have the Default_Ignorable_Code_Point property, but fonts give them a spacing
        //         glyph, and a filler that stands alone takes that glyph's advance. A filler no font has a glyph for
        //         stays invisible, rather than showing the missing-glyph box.
        has_hangul_filler && glyph_info[index].codepoint != 0
    };

    // NB: HarfBuzz positions glyphs in font space, where y grows upward, so its y offsets and advances are negated
    //     for our coordinates, where y grows downward.
    // NB: The arithmetic is that of Gfx::FloatPoint, operation by operation, so that every position has the same bits.
    let mut should_paint = true;
    for i in 0..glyph_count {
        let info = &glyph_info[i];
        let position = &positions[i];
        // Every glyph of a cluster gets the same answer, so it's worked out once, at the cluster's first glyph.
        if i == 0 || glyph_info[i - 1].cluster != info.cluster {
            should_paint = should_paint_glyph(i);
        }
        let glyph_length = glyph_length_in_code_units(i);
        requires_context_for_breaks |=
            glyph_length > 1 || (harfbuzz::glyph_info_glyph_flags(info) & harfbuzz::GLYPH_FLAG_UNSAFE_TO_BREAK) != 0;
        let x = (point_x - 0.0) + position.x_offset as f32 / TEXT_SHAPING_RESOLUTION;
        let y = (point_y - ascent) + position.y_offset.wrapping_neg() as f32 / TEXT_SHAPING_RESOLUTION;

        // https://drafts.csswg.org/css-text-3/#letter-spacing-property
        // This property specifies additional spacing (commonly called tracking) between adjacent typographic character
        // units.
        // NB: A cluster's glyphs (a base and its marks, or a conjunct's parts) are one typographic character unit, so
        //     the spacing goes after the cluster's last glyph only. Spacing after any other glyph of the cluster would
        //     push the glyphs after it away from the ones they attach to.
        let is_last_glyph_of_cluster = i + 1 == glyph_count || glyph_info[i + 1].cluster != info.cluster;
        let mut extra_advance = 0.0f32;
        if is_last_glyph_of_cluster {
            if letter_spacing != 0.0 && cluster_takes_letter_spacing(text, info.cluster) {
                extra_advance = letter_spacing;
            }
            let starting_offset = info.cluster as usize;
            if starting_offset < text_length && is_word_separator(code_point_at(text, starting_offset)) {
                extra_advance += word_spacing;
            }
        }

        let glyph_width = if should_paint {
            position.x_advance as f32 / TEXT_SHAPING_RESOLUTION + extra_advance
        } else {
            0.0
        };
        glyphs.push(ShapedGlyph {
            x,
            y,
            length_in_code_units: glyph_length,
            glyph_width,
            glyph_id: info.codepoint,
            cluster_start: info.cluster,
            should_paint,
            is_rtl: is_right_to_left,
        });

        if !should_paint {
            continue;
        }

        point_x += position.x_advance as f32 / TEXT_SHAPING_RESOLUTION;
        point_y += position.y_advance.wrapping_neg() as f32 / TEXT_SHAPING_RESOLUTION;

        // INTEROP: The spec says that we "really should not" apply letter-spacing to the trailing edge of a line, but
        //          we do. https://drafts.csswg.org/css-text/#example-7880704e
        point_x += extra_advance;
        point_y += 0.0;

        if info.cluster as usize >= first_trailing_whitespace_offset {
            trailing_whitespace_advance += glyph_width;
        }
        if info.cluster as usize >= first_trailing_ascii_whitespace_offset {
            trailing_ascii_whitespace_advance += glyph_width;
        }
    }

    Shape {
        glyphs,
        width: point_x,
        trailing_whitespace_length_in_code_units,
        trailing_whitespace_advance,
        trailing_ascii_whitespace_advance,
        requires_context_for_breaks,
    }
}

/// The advance of the text shaped as a whole, without any spacing.
fn measure_text_width(font: ShapingFont, text: ShapingText) -> f32 {
    let buffer = shape_into_buffer(font, text, TextType::Common, 0.0, JoiningContext::default(), false);
    // NB: The advances are added up in a double, which holds their sum exactly.
    let point_x: f64 = buffer
        .glyph_positions()
        .iter()
        .fold(0.0, |point_x, position| point_x + f64::from(position.x_advance));
    (point_x / f64::from(TEXT_SHAPING_RESOLUTION)) as f32
}

pub(crate) fn measure_utf16_text_width(font: &FontHandle, text: &[u16]) -> f32 {
    measure_text_width(ShapingFont::from(font), ShapingText::from_utf16(text))
}

/// `Gfx::Font::width()`: the advance of the text shaped as a whole with the font.
///
/// # Safety
///
/// `font` must be what a live `Gfx::Font` lends, and `code_units` must point to `length_in_code_units` code units in
/// the storage `storage` names, or may be null if there are none.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_gfx_font_measure_text_width(
    font: FfiShapingFont,
    storage: TextStorage,
    code_units: *const c_void,
    length_in_code_units: usize,
) -> f32 {
    assert!(!font.harfbuzz_font.is_null(), "a Gfx::Font always has a HarfBuzz font");
    // SAFETY: Guaranteed by the caller, for the duration of the call.
    let (features, text) = unsafe {
        let features = ak::slice_from_raw_parts(font.features, font.feature_count);
        let text = match storage {
            TextStorage::Ascii => ShapingText::Ascii(ak::slice_from_raw_parts(code_units.cast(), length_in_code_units)),
            TextStorage::Latin1 => {
                ShapingText::Latin1(ak::slice_from_raw_parts(code_units.cast(), length_in_code_units))
            }
            TextStorage::Utf16 => ShapingText::Utf16(ak::slice_from_raw_parts(code_units.cast(), length_in_code_units)),
        };
        (features, text)
    };
    let font = ShapingFont {
        harfbuzz_font: font.harfbuzz_font.cast(),
        features,
    };
    measure_text_width(font, text)
}
