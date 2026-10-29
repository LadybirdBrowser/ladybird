/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

use libgfx_rust::font::{EmojiPresentation, FontHandle, FrozenFontList, emoji_presentation_for_code_point};

#[repr(u8)]
enum LineBreakMode {
    Strict,
    Normal,
    Loose,
    LooseJapanese,
    LooseChinese,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum LineBreakLanguage {
    Other,
    Japanese,
    Chinese,
}

impl LineBreakLanguage {
    pub(crate) fn from_tag(tag: &[u16]) -> Self {
        let is_alpha = |subtag: &[u16]| {
            subtag
                .iter()
                .all(|&unit| u8::try_from(unit).is_ok_and(|unit| unit.is_ascii_alphabetic()))
        };
        let matches = |subtag: &[u16], expected: &[u8]| {
            subtag.len() == expected.len()
                && subtag
                    .iter()
                    .zip(expected)
                    .all(|(&unit, &expected)| (unit | 0x20) == u16::from(expected))
        };
        let mut subtags = tag.split(|&unit| unit == u16::from(b'-'));
        let primary = subtags.next().unwrap_or_default();
        if !(2..=8).contains(&primary.len()) || !is_alpha(primary) {
            return Self::Other;
        }
        let mut candidate = subtags.next();
        // BCP47 permits up to three extlang subtags, only after a two- or three-letter primary language.
        if primary.len() <= 3 {
            for _ in 0..3 {
                if !candidate.is_some_and(|subtag| subtag.len() == 3 && is_alpha(subtag)) {
                    break;
                }
                candidate = subtags.next();
            }
        }
        // https://drafts.csswg.org/css-text-4/#script-tagging
        // Only this position can name a script. Region, variant, extension and private-use subtags cannot override it.
        if let Some(script) = candidate.filter(|subtag| subtag.len() == 4 && is_alpha(subtag)) {
            if [b"hant", b"hans", b"hani", b"hanb", b"bopo"]
                .iter()
                .any(|name| matches(script, *name))
            {
                return Self::Chinese;
            }
            if [b"jpan", b"hrkt", b"hira", b"kana"]
                .iter()
                .any(|name| matches(script, *name))
            {
                return Self::Japanese;
            }
            return Self::Other;
        }
        if matches(primary, b"ja") {
            Self::Japanese
        } else if matches(primary, b"zh") {
            Self::Chinese
        } else {
            Self::Other
        }
    }
}

#[cfg(test)]
mod language_tests {
    use super::LineBreakLanguage::{self, Chinese, Japanese, Other};

    #[test]
    fn explicit_script_overrides_line_break_language() {
        for (tag, expected) in [
            ("ko-Hani", Chinese),
            ("ja-Latn", Other),
            ("zh-Latn", Other),
            ("ja-Hans", Chinese),
            ("zh-Hira", Japanese),
            ("en-Jpan", Japanese),
            ("en-Hant", Chinese),
            ("en-Hans", Chinese),
            ("en-Hani", Chinese),
            ("en-Hanb", Chinese),
            ("en-Bopo", Chinese),
            ("en-Hrkt", Japanese),
            ("en-Hira", Japanese),
            ("en-Kana", Japanese),
            ("ja-Hang", Other),
            ("ja-Kore", Other),
            ("zh-Jamo", Other),
            ("zh-Zzzz", Other),
            ("JA-hAnS", Chinese),
            ("zh-cmn-Jpan", Japanese),
            ("ja-abc-def-ghi-Hans", Chinese),
            ("yue-Hant", Chinese),
        ] {
            let units: Vec<_> = tag.encode_utf16().collect();
            assert!(LineBreakLanguage::from_tag(&units) == expected, "{tag}");
        }
    }

    #[test]
    fn only_the_script_position_changes_line_break_language() {
        for (tag, expected) in [
            ("ja", Japanese),
            ("ja-JP", Japanese),
            ("JA-jp", Japanese),
            ("zh", Chinese),
            ("zh-Hant-TW", Chinese),
            ("ja-x-Latn", Japanese),
            ("ja-u-ca-hans", Japanese),
            ("ja-JP-Latn", Japanese),
            ("ja-1996-Latn", Japanese),
            ("ja-fonipa-Latn", Japanese),
            ("en-x-Jpan", Other),
            ("en-u-ca-jpan", Other),
            ("abcd-xyz-Jpan", Other),
            ("x-Jpan", Other),
            ("jあ-Jpan", Other),
            ("japanesee-Jpan", Other),
            ("", Other),
        ] {
            let units: Vec<_> = tag.encode_utf16().collect();
            assert!(LineBreakLanguage::from_tag(&units) == expected, "{tag}");
        }
    }
}

unsafe extern "C" {
    fn unicode_layout_grapheme_segmenter_create(text: *const u16, length_in_code_units: usize) -> *mut c_void;
    fn unicode_layout_line_segmenter_create(
        text: *const u16,
        length_in_code_units: usize,
        mode: LineBreakMode,
    ) -> *mut c_void;
    fn unicode_layout_segmenter_next_boundary(handle: *mut c_void, index: usize, inclusive: bool) -> i64;
    fn unicode_layout_segmenter_destroy(handle: *mut c_void);
    fn unicode_layout_word_boundaries(
        text: *const u16,
        length: usize,
        offset: usize,
        start: *mut usize,
        end: *mut usize,
    );
    safe fn unicode_layout_bidirectional_class(code_point: u32) -> u8;
    safe fn unicode_layout_line_break_class(code_point: u32, allow_conditional_japanese_starters: bool) -> u8;
    safe fn unicode_layout_code_point_has_emoji_property(code_point: u32) -> bool;
}

// LibUnicode's LineBreakClass, in declaration order.
mod line_break_class {
    pub(super) const ALPHABETIC: u8 = 0;
    pub(super) const NUMERIC: u8 = 1;
    pub(super) const IDEOGRAPHIC: u8 = 2;
    pub(super) const AMBIGUOUS: u8 = 3;
    pub(super) const COMPLEX_CONTEXT: u8 = 4;
    pub(super) const COMBINING_MARK: u8 = 5;
}

fn line_break_class(code_point: u32, allow_conditional_japanese_starters: bool) -> u8 {
    unicode_layout_line_break_class(code_point, allow_conditional_japanese_starters)
}

// Classifies a code point for direction-run splitting: strong LTR/RTL, direction-neutral Common, or ContextDependent
// (resolved from surrounding runs).
fn text_type_for_code_point(code_point: u32) -> u8 {
    use line_box_fragment::{
        GLYPH_TEXT_TYPE_COMMON as C, GLYPH_TEXT_TYPE_CONTEXT_DEPENDENT as X, GLYPH_TEXT_TYPE_LTR as L,
        GLYPH_TEXT_TYPE_RTL as R,
    };

    // Each ASCII character has a statically known bidi class.
    #[rustfmt::skip]
    const ASCII_TEXT_TYPES: [u8; 128] = [
        // 0x00-0x0F: Control characters (BN=Common, S/B/WS=ContextDependent)
        C, C, C, C, C, C, C, C, C, X, X, X, X, X, C, C,
        // 0x10-0x1F: Control characters
        C, C, C, C, C, C, C, C, C, C, C, C, X, X, X, X,
        // 0x20-0x2F: Space and punctuation
        X, C, C, X, X, X, C, C, C, C, C, X, X, X, X, X,
        // 0x30-0x3F: Digits and punctuation
        X, X, X, X, X, X, X, X, X, X, X, C, C, C, C, C,
        // 0x40-0x4F: @ and uppercase letters
        C, L, L, L, L, L, L, L, L, L, L, L, L, L, L, L,
        // 0x50-0x5F: Uppercase letters and punctuation
        L, L, L, L, L, L, L, L, L, L, L, C, C, C, C, C,
        // 0x60-0x6F: ` and lowercase letters
        C, L, L, L, L, L, L, L, L, L, L, L, L, L, L, L,
        // 0x70-0x7F: Lowercase letters and punctuation
        L, L, L, L, L, L, L, L, L, L, L, C, C, C, C, C,
    ];
    if let Some(&text_type) = ASCII_TEXT_TYPES.get(code_point as usize) {
        return text_type;
    }

    match unicode_layout_bidirectional_class(code_point) {
        // LibUnicode's BidiClass, in declaration order: AN, B, CS, NSM, EN, ES, ET, S, WS.
        0 | 1 | 3..=7 | 21 | 22 => X,
        // BN, FSI, ON, PDF, PDI.
        2 | 8 | 13..=15 => C,
        // L, LRE, LRI, LRO.
        9..=12 => L,
        // R, AL, RLE, RLI, RLO.
        16..=20 => R,
        _ => unreachable!("invalid bidi class"),
    }
}

// Stand-ins for the Unicode library's segmenters and the host's code point facts, which a unit test links through a
// document's render state but never reaches.
#[cfg(test)]
mod unicode_test_stubs {
    use std::ffi::c_void;

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_grapheme_segmenter_create(
        _text: *const u16,
        _length_in_code_units: usize,
    ) -> *mut c_void {
        unreachable!("no unit test segments text");
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_line_segmenter_create(
        _text: *const u16,
        _length_in_code_units: usize,
        _mode: super::LineBreakMode,
    ) -> *mut c_void {
        unreachable!("no unit test segments text");
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_segmenter_next_boundary(_handle: *mut c_void, _index: usize, _inclusive: bool) -> i64 {
        unreachable!("no unit test segments text");
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_word_boundaries(
        _text: *const u16,
        _length: usize,
        _offset: usize,
        _start: *mut usize,
        _end: *mut usize,
    ) {
        unreachable!("no unit test segments text");
    }

    #[unsafe(no_mangle)]
    extern "C" fn ladybird_layout_code_point_category_facts(
        code_point: u32,
    ) -> crate::layout::tree_builder::FfiCodePointCategoryFacts {
        let code_point = u8::try_from(code_point).expect("unit tests only match ASCII first-letter text");
        assert!(code_point.is_ascii_alphanumeric() || code_point.is_ascii_whitespace());
        crate::layout::tree_builder::FfiCodePointCategoryFacts {
            is_space_separator: code_point == b' ',
            is_punctuation: false,
            is_letter: code_point.is_ascii_alphabetic(),
            is_number: code_point.is_ascii_digit(),
            is_symbol: false,
            is_open_punctuation: false,
            is_dash_punctuation: false,
        }
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_bidirectional_class(_code_point: u32) -> u8 {
        unreachable!("no unit test segments text");
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_line_break_class(_code_point: u32, _allow_conditional_japanese_starters: bool) -> u8 {
        unreachable!("no unit test segments text");
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_code_point_has_emoji_property(_code_point: u32) -> bool {
        unreachable!("no unit test segments text");
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct TextChunk {
    pub start: usize,
    pub length: usize,
    pub font: FontHandle,
    pub has_breaking_newline: bool,
    pub has_breaking_tab: bool,
    pub is_all_whitespace: bool,
    pub can_break_after: bool,
    pub text_type: u8,
}

pub(crate) struct TextChunkInputs<'text> {
    pub text: &'text [u16],
    pub frozen_font_list: &'text FrozenFontList,
    pub white_space_collapse: u8,
    pub word_break: u8,
    pub line_break: u8,
    pub line_break_language: LineBreakLanguage,
    pub font_variant_emoji: u8,
    pub should_wrap_lines: bool,
    pub should_respect_linebreaks: bool,
    pub unidirectional_ltr: bool,
    /// Whether the text is vertical text in mixed orientation, where some characters are typeset upright and others
    /// sideways.
    pub splits_at_orientation_changes: bool,
}

pub(crate) fn chunk_text(inputs: TextChunkInputs<'_>) -> Vec<TextChunk> {
    let mut chunker = TextChunker::new(inputs);
    let mut chunks = Vec::new();
    while let Some(chunk) = chunker.next_chunk() {
        chunks.push(chunk);
    }
    chunks
}

pub(super) fn word_boundaries(text: &[u16], offset: usize) -> std::ops::Range<usize> {
    let mut start = 0;
    let mut end = text.len();
    // SAFETY: The Unicode service borrows the buffer only for this call and
    // writes its boundaries into the two live output slots.
    unsafe { unicode_layout_word_boundaries(text.as_ptr(), text.len(), offset, &raw mut start, &raw mut end) };
    start..end
}

pub(crate) struct IcuSegmenterHandle {
    raw: *mut c_void,
}

// SAFETY: The handle owns its segmenter, which shares nothing with another, so the thread that holds the handle may
// change.
unsafe impl Send for IcuSegmenterHandle {}

impl IcuSegmenterHandle {
    fn next_boundary(&self, index: usize, inclusive: bool) -> Option<usize> {
        // SAFETY: The handle is live until drop, and the text it references
        // outlives it per the segmenter constructor contracts below.
        let boundary = unsafe { unicode_layout_segmenter_next_boundary(self.raw, index, inclusive) };
        (boundary >= 0).then_some(boundary as usize)
    }
}

impl Drop for IcuSegmenterHandle {
    fn drop(&mut self) {
        // SAFETY: The handle was created by a unicode_layout_*_segmenter_create
        // call and is destroyed exactly once.
        unsafe { unicode_layout_segmenter_destroy(self.raw) };
    }
}

pub(crate) enum GraphemeSegmenter {
    Ascii {
        length_in_code_units: usize,
    },
    /// Text of code points below U+0300 only, where every code unit is a grapheme cluster except for CR LF.
    Latin {
        length_in_code_units: usize,
        /// Where each LF that follows a CR is, in increasing order.
        line_feeds_after_carriage_returns: Box<[usize]>,
    },
    Icu {
        handle: IcuSegmenterHandle,
        length_in_code_units: usize,
        /// One bit per code unit: whether it is a code point below U+0300 other than CR.
        latin_code_units: Box<[u64]>,
    },
}

// No code point below U+0300 extends or joins a grapheme cluster, and only CR joins the code point after it (CR LF).
fn is_latin_code_unit_outside_clusters(code_unit: u16) -> bool {
    code_unit < 0x300 && code_unit != 0x0d
}

impl GraphemeSegmenter {
    pub(crate) fn new(text: &[u16]) -> Self {
        let Some(first_non_ascii) = text.iter().position(|unit| *unit > 0x7f) else {
            return Self::Ascii {
                length_in_code_units: text.len(),
            };
        };
        if text[first_non_ascii..].iter().all(|unit| *unit < 0x300) {
            let line_feeds_after_carriage_returns = text
                .windows(2)
                .enumerate()
                .filter(|(_, pair)| *pair == [0x0d, 0x0a])
                .map(|(index, _)| index + 1)
                .collect();
            return Self::Latin {
                length_in_code_units: text.len(),
                line_feeds_after_carriage_returns,
            };
        }
        let mut latin_code_units = vec![0u64; text.len().div_ceil(64)].into_boxed_slice();
        for (index, code_unit) in text.iter().enumerate() {
            if is_latin_code_unit_outside_clusters(*code_unit) {
                latin_code_units[index / 64] |= 1 << (index % 64);
            }
        }
        // SAFETY: The chunker borrows the text for its whole lifetime and the
        // segmenter handle is dropped with the chunker, so the ICU segmenter
        // never outlives the buffer it references.
        let raw = unsafe { unicode_layout_grapheme_segmenter_create(text.as_ptr(), text.len()) };
        Self::Icu {
            handle: IcuSegmenterHandle { raw },
            length_in_code_units: text.len(),
            latin_code_units,
        }
    }

    /// Mirrors Unicode::AsciiGraphemeSegmenter for the ASCII case: every code
    /// unit index is a boundary.
    pub(crate) fn next_boundary(&self, index: usize, inclusive: bool) -> Option<usize> {
        match self {
            Self::Ascii { length_in_code_units } => {
                if inclusive && index <= *length_in_code_units {
                    return Some(index);
                }
                if index >= *length_in_code_units {
                    return None;
                }
                Some(index + 1)
            }
            Self::Latin {
                length_in_code_units,
                line_feeds_after_carriage_returns,
            } => {
                let is_boundary = |index: usize| line_feeds_after_carriage_returns.binary_search(&index).is_err();
                if inclusive && index <= *length_in_code_units && is_boundary(index) {
                    return Some(index);
                }
                if index >= *length_in_code_units {
                    return None;
                }
                (index + 1..=*length_in_code_units).find(|&boundary| is_boundary(boundary))
            }
            Self::Icu {
                handle,
                length_in_code_units,
                latin_code_units,
            } => {
                // OPTIMIZATION: Two code points below U+0300 have a grapheme cluster boundary between them, unless the
                //               first one is CR. So do such a code point and the end of the text.
                let is_latin = |index: usize| latin_code_units[index / 64] & (1 << (index % 64)) != 0;
                let length = *length_in_code_units;
                let boundary = if inclusive {
                    (index > 0 && index < length && is_latin(index - 1) && is_latin(index)).then_some(index)
                } else {
                    (index < length && is_latin(index) && (index + 1 == length || is_latin(index + 1)))
                        .then_some(index + 1)
                };
                if boundary.is_some() {
                    debug_assert_eq!(boundary, handle.next_boundary(index, inclusive));
                    return boundary;
                }
                handle.next_boundary(index, inclusive)
            }
        }
    }
}

struct LineSegmenter<'text> {
    handle: IcuSegmenterHandle,
    text: &'text [u16],
    /// Where the last search for a boundary started, and the boundary it found, so that no boundary lies before it.
    last_search: std::cell::Cell<Option<(usize, usize)>>,
}

impl<'text> LineSegmenter<'text> {
    fn new(text: &'text [u16], line_break: u8, language: LineBreakLanguage) -> Self {
        let mode = match (line_break, language) {
            (line_break::LOOSE, LineBreakLanguage::Japanese) => LineBreakMode::LooseJapanese,
            (line_break::LOOSE, LineBreakLanguage::Chinese) => LineBreakMode::LooseChinese,
            (line_break::LOOSE, LineBreakLanguage::Other) => LineBreakMode::Loose,
            (line_break::NORMAL, _) => LineBreakMode::Normal,
            _ => LineBreakMode::Strict,
        };
        // SAFETY: See GraphemeSegmenter::new; the same lifetime contract holds,
        // and the C++ side picks the ASCII line-breaking fast path internally.
        let raw = unsafe { unicode_layout_line_segmenter_create(text.as_ptr(), text.len(), mode) };
        Self {
            handle: IcuSegmenterHandle { raw },
            text,
            last_search: std::cell::Cell::new(None),
        }
    }

    fn next_boundary(&self, index: usize, inclusive: bool) -> Option<usize> {
        // NB: The segmenter moves a search that starts inside a surrogate pair to the start of the pair, which can
        //     find a boundary before where the search started.
        let starts_inside_surrogate_pair = index > 0
            && index < self.text.len()
            && is_utf16_low_surrogate(self.text[index])
            && is_utf16_high_surrogate(self.text[index - 1]);
        if starts_inside_surrogate_pair {
            return self.handle.next_boundary(index, inclusive);
        }
        // OPTIMIZATION: The chunker asks about every position from the start of the text to its end. A search finds
        //               the first boundary at or after where it starts, so the boundary the last search found answers
        //               every search that starts between the two.
        let search_start = if inclusive { index } else { index + 1 };
        if let Some((last_search_start, boundary)) = self.last_search.get()
            && (last_search_start..=boundary).contains(&search_start)
        {
            return Some(boundary);
        }
        let boundary = self.handle.next_boundary(index, inclusive)?;
        self.last_search.set(Some((search_start, boundary)));
        Some(boundary)
    }
}

fn is_utf16_high_surrogate(code_unit: u16) -> bool {
    (0xd800..=0xdbff).contains(&code_unit)
}

fn is_utf16_low_surrogate(code_unit: u16) -> bool {
    (0xdc00..=0xdfff).contains(&code_unit)
}

/// Whether a code point is typeset upright in vertical text whose text orientation is 'mixed'.
// FIXME: Use the Vertical_Orientation property of Unicode (UAX #50) instead of these blocks of characters, which are
//        mostly upright.
pub(crate) fn code_point_is_upright_in_mixed_orientation(code_point: u32) -> bool {
    matches!(
        code_point,
        0x1100..=0x11FF
            | 0x2E80..=0x303F
            | 0x3040..=0x31FF
            | 0x3200..=0x4DBF
            | 0x4E00..=0x9FFF
            | 0xA960..=0xA97F
            | 0xAC00..=0xD7FF
            | 0xF900..=0xFAFF
            | 0xFE10..=0xFE1F
            | 0xFE30..=0xFE4F
            | 0xFF01..=0xFF0C
            | 0xFF0E..=0xFF1B
            | 0xFF1F..=0xFF60
            | 0xFFE0..=0xFFE6
            | 0x20000..=0x3FFFF
    )
}

/// Mirrors AK::Utf16View::code_point_at, including its lone-surrogate
/// pass-through behavior.
pub(crate) fn code_point_at(text: &[u16], index: usize) -> u32 {
    let code_unit = text[index];
    let code_point = u32::from(code_unit);
    if !is_utf16_high_surrogate(code_unit) && !is_utf16_low_surrogate(code_unit) {
        return code_point;
    }
    if is_utf16_low_surrogate(code_unit) || index + 1 == text.len() {
        return code_point;
    }
    let second = text[index + 1];
    if !is_utf16_low_surrogate(second) {
        return code_point;
    }
    0x10000 + ((code_point - 0xd800) << 10) + (u32::from(second) - 0xdc00)
}

/// Mirrors AK::Utf16View::previous_code_point_at: steps `index` back over one
/// code point (surrogate-pair aware) and decodes it.
fn previous_code_point_at(text: &[u16], index: &mut usize) -> u32 {
    assert!(*index > 0 && *index <= text.len());
    *index -= 1;
    if *index > 0 && is_utf16_low_surrogate(text[*index]) && is_utf16_high_surrogate(text[*index - 1]) {
        *index -= 1;
    }
    code_point_at(text, *index)
}

pub(super) fn code_unit_length_for_code_point(code_point: u32) -> usize {
    if code_point >= 0x10000 { 2 } else { 1 }
}

fn code_point_is_ascii_space(code_point: u32) -> bool {
    matches!(code_point, 0x09..=0x0d | 0x20)
}

fn is_interword_space(code_point: u32) -> bool {
    code_point == 0x0020 || code_point == 0x00a0
}

// INTEROP: Use browser-compatible line-breaking rules for printable ASCII. Unicode line breaking alone permits
//          additional breaks within punctuation sequences, which also prevents contextual shaping when those
//          sequences are split into separate chunks.
fn ascii_line_break_at(text: &[u16], index: usize) -> Option<bool> {
    if index == 0 || index >= text.len() {
        return None;
    }
    let before = text[index - 1];
    let after = text[index];
    if !(0x21..=0x7f).contains(&before) || !(0x21..=0x7f).contains(&after) {
        return None;
    }

    let before = before as u8;
    let after = after as u8;
    if before == b'-' && after.is_ascii_digit() {
        return Some(index > 1 && text[index - 2] <= 0x7f && (text[index - 2] as u8).is_ascii_alphanumeric());
    }
    if before.is_ascii_alphanumeric()
        || matches!(
            before,
            b'$' | b'\'' | b'(' | b'/' | b'<' | b'@' | b'[' | b'^'..=b'`' | b'{' | 0x7f
        )
        || matches!(
            after,
            b'!' | b')' | b',' | b'.' | b'/' | b':' | b';' | b'?' | b']' | b'}'
        )
        || (before == b'?' && matches!(after, b'"' | b'\''))
        || (before == b'-' && after == b'$')
    {
        return Some(false);
    }
    Some(matches!(after, b'(' | b'<' | b'[' | b'{') || matches!(before, b'-' | b'?'))
}

// A chunk while it is still being accumulated: where it starts, plus the properties shared by
// every chunk that can be committed from it.
#[derive(Clone)]
struct PendingChunk {
    start: usize,
    font: FontHandle,
    text_type: u8,
    is_upright: bool,
    broken_on_tab: bool,
}

#[derive(Clone, Copy)]
struct ChunkBreakFlags {
    has_breaking_newline: bool,
    has_breaking_tab: bool,
    can_break_after: bool,
}

fn is_regional_indicator(code_point: u32) -> bool {
    (0x1F1E6..=0x1F1FF).contains(&code_point)
}

/// The number of regional indicators at the end of a text.
pub(crate) fn trailing_regional_indicator_count(text: &[u16]) -> usize {
    let mut index = text.len();
    let mut count = 0;
    while index > 0 && is_regional_indicator(previous_code_point_at(text, &mut index)) {
        count += 1;
    }
    count
}

/// Whether there is a soft wrap opportunity between the end of `before` and the start of `after`, such as the texts
/// of two text nodes that follow each other on a line. `code_unit_before` is the last code unit of the text before
/// `before`, if any. `regional_indicators_before` is the number of regional indicators that end `before`, including
/// any in text before it.
pub(crate) fn soft_wrap_opportunity_between(
    before: &[u16],
    after: &[u16],
    code_unit_before: Option<u16>,
    regional_indicators_before: usize,
    word_break: u8,
    line_break: u8,
    line_break_language: LineBreakLanguage,
) -> bool {
    let (Some(&last), Some(&first)) = (before.last(), after.first()) else {
        return false;
    };
    // NB: Whitespace gives its own soft wrap opportunities.
    if (last <= 0x7f && code_point_is_ascii_space(u32::from(last)))
        || (first <= 0x7f && code_point_is_ascii_space(u32::from(first)))
    {
        return false;
    }
    // NB: Only the number of regional indicators before the boundary tells whether it splits a flag, and they can
    //     reach further back than the context below.
    if regional_indicators_before > 0 && is_regional_indicator(code_point_at(after, 0)) {
        return regional_indicators_before.is_multiple_of(2);
    }
    // NB: A few code points on each side of the boundary are enough context to find a soft wrap opportunity there.
    const CONTEXT_LENGTH: usize = 8;
    let mut before_start = before.len().saturating_sub(CONTEXT_LENGTH);
    if before_start > 0 && is_utf16_low_surrogate(before[before_start]) {
        before_start -= 1;
    }
    let mut after_end = after.len().min(CONTEXT_LENGTH);
    if after_end < after.len() && is_utf16_low_surrogate(after[after_end]) {
        after_end += 1;
    }
    let mut text = Vec::with_capacity(before.len() - before_start + after_end + 1);
    // NB: Some rules look at the code point before the last one of `before`, which can be in the text before it.
    if before_start == 0
        && let Some(code_unit) = code_unit_before
        && !is_utf16_low_surrogate(code_unit)
    {
        text.push(code_unit);
    }
    text.extend_from_slice(&before[before_start..]);
    let boundary = text.len();
    text.extend_from_slice(&after[..after_end]);
    // OPTIMIZATION: Printable ASCII on both sides needs no line segmenter.
    if word_break != word_break::BREAK_ALL
        && let Some(is_boundary) = ascii_line_break_at(&text, boundary)
    {
        return is_boundary;
    }
    LineBreaker::new(&text, word_break, line_break, line_break_language).is_at_line_break_opportunity(boundary)
}

/// Finds the soft wrap opportunities of a text.
struct LineBreaker<'text> {
    text: &'text [u16],
    line_segmenter: LineSegmenter<'text>,
    word_break: u8,
    allow_conditional_japanese_starters: bool,
}

impl<'text> LineBreaker<'text> {
    fn new(text: &'text [u16], word_break: u8, line_break: u8, line_break_language: LineBreakLanguage) -> Self {
        Self {
            text,
            line_segmenter: LineSegmenter::new(text, line_break, line_break_language),
            word_break,
            // NB: Apply CJ tailoring to word-break classification as well as to ICU's line segmenter.
            allow_conditional_japanese_starters: matches!(line_break, line_break::NORMAL | line_break::LOOSE),
        }
    }

    fn is_at_line_break_opportunity(&self, index: usize) -> bool {
        let get_previous_code_point = || -> Option<u32> {
            if index == 0 {
                return None;
            }
            let mut index = index;
            let mut previous_code_point = previous_code_point_at(self.text, &mut index);
            while line_break_class(previous_code_point, self.allow_conditional_japanese_starters)
                == line_break_class::COMBINING_MARK
                && index > 0
            {
                previous_code_point = previous_code_point_at(self.text, &mut index);
            }
            Some(previous_code_point)
        };

        let is_at_line_segmenter_boundary = || {
            if self.word_break != word_break::BREAK_ALL
                && let Some(is_boundary) = ascii_line_break_at(self.text, index)
            {
                return is_boundary;
            }
            self.line_segmenter.next_boundary(index, true) == Some(index)
        };

        match self.word_break {
            word_break::NORMAL | word_break::BREAK_WORD => is_at_line_segmenter_boundary(),
            word_break::BREAK_ALL => {
                if index >= self.text.len() {
                    return false;
                }
                use line_break_class::*;
                let breaks_anywhere = |code_point| {
                    matches!(
                        line_break_class(code_point, self.allow_conditional_japanese_starters),
                        ALPHABETIC | NUMERIC | COMPLEX_CONTEXT | IDEOGRAPHIC
                    )
                };
                if let Some(previous_code_point) = get_previous_code_point()
                    && breaks_anywhere(previous_code_point)
                    && breaks_anywhere(code_point_at(self.text, index))
                {
                    return true;
                }
                is_at_line_segmenter_boundary()
            }
            word_break::KEEP_ALL => {
                if index >= self.text.len() {
                    return false;
                }
                use line_break_class::*;
                let keeps_together = |code_point| {
                    matches!(
                        line_break_class(code_point, self.allow_conditional_japanese_starters),
                        ALPHABETIC | NUMERIC | AMBIGUOUS | IDEOGRAPHIC
                    )
                };
                if let Some(previous_code_point) = get_previous_code_point()
                    && keeps_together(previous_code_point)
                    && keeps_together(code_point_at(self.text, index))
                {
                    return false;
                }
                is_at_line_segmenter_boundary()
            }
            _ => unreachable!("invalid word-break value"),
        }
    }
}

struct TextChunker<'text> {
    text: &'text [u16],
    frozen_font_list: &'text FrozenFontList,
    grapheme_segmenter: GraphemeSegmenter,
    line_breaker: LineBreaker<'text>,
    font_variant_emoji: u8,
    should_collapse_whitespace: bool,
    break_after_preserved_tabs: bool,
    should_wrap_lines: bool,
    should_respect_linebreaks: bool,
    unidirectional_ltr: bool,
    splits_at_orientation_changes: bool,
    current_index: usize,
    last_non_whitespace_font: Option<FontHandle>,
}

impl<'text> TextChunker<'text> {
    fn new(inputs: TextChunkInputs<'text>) -> Self {
        Self {
            text: inputs.text,
            frozen_font_list: inputs.frozen_font_list,
            grapheme_segmenter: GraphemeSegmenter::new(inputs.text),
            line_breaker: LineBreaker::new(
                inputs.text,
                inputs.word_break,
                inputs.line_break,
                inputs.line_break_language,
            ),
            font_variant_emoji: inputs.font_variant_emoji,
            should_collapse_whitespace: matches!(
                inputs.white_space_collapse,
                white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
            ),
            break_after_preserved_tabs: inputs.white_space_collapse == white_space_collapse::BREAK_SPACES
                && inputs.should_wrap_lines,
            should_wrap_lines: inputs.should_wrap_lines,
            should_respect_linebreaks: inputs.should_respect_linebreaks,
            unidirectional_ltr: inputs.unidirectional_ltr,
            splits_at_orientation_changes: inputs.splits_at_orientation_changes,
            current_index: 0,
            last_non_whitespace_font: None,
        }
    }

    fn current_code_point(&self) -> u32 {
        code_point_at(self.text, self.current_index)
    }

    fn current_text_type(&self) -> u8 {
        if self.unidirectional_ltr {
            return line_box_fragment::GLYPH_TEXT_TYPE_LTR;
        }
        text_type_for_code_point(self.current_code_point())
    }

    fn current_code_point_is_upright(&self) -> bool {
        self.splits_at_orientation_changes && code_point_is_upright_in_mixed_orientation(self.current_code_point())
    }

    fn next_grapheme_boundary(&self) -> usize {
        self.grapheme_segmenter
            .next_boundary(self.current_index, false)
            .unwrap_or(self.text.len())
    }

    fn is_collapsible(&self, code_point: u32) -> bool {
        self.should_collapse_whitespace && code_point_is_ascii_space(code_point)
    }

    fn is_at_line_break_opportunity(&self) -> bool {
        self.should_wrap_lines && self.line_breaker.is_at_line_break_opportunity(self.current_index)
    }

    fn emoji_presentation_at(&self, code_unit_offset: usize, code_point: u32) -> EmojiPresentation {
        let next_offset = code_unit_offset + code_unit_length_for_code_point(code_point);
        let next_code_point = (next_offset < self.text.len()).then(|| code_point_at(self.text, next_offset));
        // OPTIMIZATION: No code point below U+0300 has the Emoji_Presentation property, so only a variation selector
        //               can give one of them anything but the default text presentation.
        let default_presentation = if code_point < 0x300 && !matches!(next_code_point, Some(0xfe0e | 0xfe0f)) {
            EmojiPresentation {
                is_emoji: false,
                forced: false,
            }
        } else {
            emoji_presentation_for_code_point(code_point, next_code_point)
        };
        // OPTIMIZATION: Below U+0300, only '#', '*', the ASCII digits, U+00A9 and U+00AE have the Emoji property.
        let has_emoji_property = if code_point < 0x300 {
            matches!(code_point, 0x23 | 0x2a | 0x30..=0x39 | 0xa9 | 0xae)
        } else {
            unicode_layout_code_point_has_emoji_property(code_point)
        };
        debug_assert!(
            code_point >= 0x300 || {
                let presentation = emoji_presentation_for_code_point(code_point, next_code_point);
                let has_property = unicode_layout_code_point_has_emoji_property(code_point);
                presentation.is_emoji == default_presentation.is_emoji
                    && presentation.forced == default_presentation.forced
                    && has_property == has_emoji_property
            }
        );
        if default_presentation.forced || !has_emoji_property {
            return default_presentation;
        }

        match self.font_variant_emoji {
            font_variant_emoji::TEXT => EmojiPresentation {
                is_emoji: false,
                forced: true,
            },
            font_variant_emoji::EMOJI => EmojiPresentation {
                is_emoji: true,
                forced: true,
            },
            font_variant_emoji::UNICODE => EmojiPresentation {
                is_emoji: default_presentation.is_emoji,
                forced: true,
            },
            _ => default_presentation,
        }
    }

    fn font_for_space(&self, at_index: usize, space_code_point: u32) -> FontHandle {
        let has_glyph = |font: &FontHandle| font.contains_glyph(space_code_point);

        // 1. Prefer the last non-whitespace font in this node/run.
        if let Some(last_font) = &self.last_non_whitespace_font
            && !last_font.is_emoji_font()
            && has_glyph(last_font)
        {
            return last_font.clone();
        }

        // 2. Look ahead to the next non-space to infer the base font of this run.
        let mut i = at_index;
        while i < self.text.len() {
            let code_point = code_point_at(self.text, i);
            if !is_interword_space(code_point) && code_point != '\t' as u32 && code_point != '\n' as u32 {
                let font = self
                    .frozen_font_list
                    .font_for_code_point(code_point, self.emoji_presentation_at(i, code_point));
                if !font.is_emoji_font() && has_glyph(&font) {
                    return font;
                }
                // Text is coming from an emoji face; we'll fall back to (3).
                break;
            }
            i = self
                .grapheme_segmenter
                .next_boundary(i, false)
                .unwrap_or(self.text.len());
        }

        // 3. No text around (leading/trailing/all spaces) — pick a font with the glyph from the cascade.
        self.frozen_font_list.font_for_code_point(
            space_code_point,
            EmojiPresentation {
                is_emoji: false,
                forced: false,
            },
        )
    }

    fn expected_font_for(&self, code_point: u32) -> FontHandle {
        if is_interword_space(code_point) {
            self.font_for_space(self.current_index, code_point)
        } else {
            self.frozen_font_list
                .font_for_code_point(code_point, self.emoji_presentation_at(self.current_index, code_point))
        }
    }

    fn try_commit_chunk(
        &mut self,
        start: usize,
        end: usize,
        break_flags: ChunkBreakFlags,
        font: FontHandle,
        text_type: u8,
    ) -> Option<TextChunk> {
        let length_in_code_units = end - start;
        if length_in_code_units == 0 {
            return None;
        }
        let is_all_whitespace = self.text[start..end]
            .iter()
            .all(|unit| *unit <= 0x7f && code_point_is_ascii_space(u32::from(*unit)));
        if !is_all_whitespace {
            self.last_non_whitespace_font = Some(font.clone());
        }
        Some(TextChunk {
            start,
            length: length_in_code_units,
            font,
            has_breaking_newline: break_flags.has_breaking_newline,
            has_breaking_tab: break_flags.has_breaking_tab,
            is_all_whitespace,
            can_break_after: break_flags.can_break_after,
            text_type,
        })
    }

    fn next_chunk(&mut self) -> Option<TextChunk> {
        'restart: loop {
            if self.current_index >= self.text.len() {
                return None;
            }

            let mut code_point = self.current_code_point();
            let mut can_break_at_current_position = self.is_at_line_break_opportunity();
            let mut pending = PendingChunk {
                start: self.current_index,
                font: self.expected_font_for(code_point),
                text_type: self.current_text_type(),
                is_upright: self.current_code_point_is_upright(),
                broken_on_tab: false,
            };

            while self.current_index < self.text.len() {
                code_point = self.current_code_point();

                if code_point == '\t' as u32 {
                    if let Some(chunk) = self.try_commit_chunk_at_cursor(&pending, false) {
                        return Some(chunk);
                    }

                    pending.broken_on_tab = true;
                    self.current_index = tab_chunk_end(self.text, self.current_index, self.break_after_preserved_tabs);
                    can_break_at_current_position = self.is_at_line_break_opportunity();
                }

                let expected_font = self.expected_font_for(code_point);

                if pending.font != expected_font
                    && let Some(chunk) = self.try_commit_chunk_at_cursor(&pending, can_break_at_current_position)
                {
                    return Some(chunk);
                }

                if self.should_respect_linebreaks && code_point == '\n' as u32 {
                    // Newline encountered, and we're supposed to preserve them.
                    // If we have accumulated some code points in the current chunk, commit them now and continue with
                    // the newline next time.
                    if let Some(chunk) = self.try_commit_chunk_at_cursor(&pending, false) {
                        return Some(chunk);
                    }

                    // Otherwise, commit the newline!
                    self.current_index = self.next_grapheme_boundary();
                    let chunk = self.try_commit_chunk(
                        pending.start,
                        self.current_index,
                        ChunkBreakFlags {
                            has_breaking_newline: true,
                            has_breaking_tab: pending.broken_on_tab,
                            can_break_after: false,
                        },
                        pending.font.clone(),
                        pending.text_type,
                    );
                    return Some(chunk.expect("newline chunk must be non-empty"));
                }

                // If both this code point and the previous code point are collapsible, skip code points until we're at
                // a non-collapsible code point.
                if self.is_collapsible(code_point)
                    && self.current_index > 0
                    && self.is_collapsible(code_point_at(self.text, self.current_index - 1))
                {
                    let chunk = self.try_commit_chunk_at_cursor(&pending, false);

                    while self.current_index < self.text.len() && self.is_collapsible(self.current_code_point()) {
                        self.current_index = self.next_grapheme_boundary();
                    }

                    if let Some(chunk) = chunk {
                        return Some(chunk);
                    }

                    continue 'restart;
                }

                // Tabs can advance the cursor past the text. Split shaping runs at direction changes independently
                // of line wrapping, while keeping the direction lookup inside the text.
                if self.current_index < self.text.len()
                    && pending.text_type != self.current_text_type()
                    && let Some(chunk) = self.try_commit_chunk_at_cursor(&pending, can_break_at_current_position)
                {
                    return Some(chunk);
                }

                // NB: Each chunk is typeset either upright or sideways as a whole, so split it where that changes.
                if self.current_index < self.text.len()
                    && pending.is_upright != self.current_code_point_is_upright()
                    && let Some(chunk) = self.try_commit_chunk_at_cursor(&pending, can_break_at_current_position)
                {
                    return Some(chunk);
                }

                // NB: Leading collapsible whitespace must be a separate chunk even when wrapping is disabled.
                if self.should_wrap_lines || (self.should_collapse_whitespace && self.current_index == pending.start) {
                    if code_point_is_ascii_space(code_point) {
                        // Whitespace encountered, and we're allowed to break on whitespace.
                        // If we have accumulated some code points in the current chunk, commit them now and continue
                        // with the whitespace next time.
                        if let Some(chunk) = self.try_commit_chunk_at_cursor(&pending, false) {
                            return Some(chunk);
                        }

                        // Otherwise, commit the whitespace!
                        self.current_index = self.next_grapheme_boundary();
                        can_break_at_current_position = self.is_at_line_break_opportunity();
                        let space_font = self.font_for_space(self.current_index, code_point);
                        let space = PendingChunk {
                            font: space_font,
                            ..pending.clone()
                        };
                        if let Some(chunk) = self.try_commit_chunk_at_cursor(&space, false) {
                            return Some(chunk);
                        }
                        continue;
                    }

                    if can_break_at_current_position
                        && let Some(chunk) = self.try_commit_chunk_at_cursor(&pending, true)
                    {
                        return Some(chunk);
                    }
                }

                self.current_index = self.next_grapheme_boundary();
                can_break_at_current_position = self.is_at_line_break_opportunity();
            }

            if pending.start != self.text.len() {
                // Try to output whatever's left at the end of the text node.
                if let Some(chunk) = self.try_commit_chunk(
                    pending.start,
                    self.text.len(),
                    ChunkBreakFlags {
                        has_breaking_newline: false,
                        has_breaking_tab: pending.broken_on_tab,
                        can_break_after: false,
                    },
                    pending.font.clone(),
                    pending.text_type,
                ) {
                    return Some(chunk);
                }
            }

            return None;
        }
    }

    // Commits everything accumulated since `pending.start` up to the cursor, if that range is non-empty.
    fn try_commit_chunk_at_cursor(&mut self, pending: &PendingChunk, can_break_after: bool) -> Option<TextChunk> {
        self.try_commit_chunk(
            pending.start,
            self.current_index,
            ChunkBreakFlags {
                has_breaking_newline: false,
                has_breaking_tab: pending.broken_on_tab,
                can_break_after,
            },
            pending.font.clone(),
            pending.text_type,
        )
    }
}

pub(crate) fn text_chunks(
    callbacks: &LayoutPass<'_>,
    node: Node,
    should_wrap_lines: bool,
    should_respect_linebreaks: bool,
    unidirectional_ltr: bool,
) -> std::sync::Arc<super::rendered_text::CachedTextChunks> {
    let parent_style = StyleValues::for_node(callbacks, callbacks.parent(node));
    let key = super::rendered_text::TextChunkCacheKey {
        should_wrap_lines,
        should_respect_linebreaks,
        unidirectional_ltr,
        white_space_collapse: parent_style.white_space_collapse(),
        word_break: parent_style.word_break(),
        line_break: parent_style.line_break(),
        line_break_language: if parent_style.line_break() == line_break::LOOSE {
            callbacks.arena().text_line_break_language(node)
        } else {
            LineBreakLanguage::Other
        },
        font_variant_emoji: parent_style.font_variant_emoji(),
        frozen_font_list: parent_style.frozen_font_list_ref(),
        splits_at_orientation_changes: matches!(
            parent_style.writing_mode(),
            writing_mode::VERTICAL_RL | writing_mode::VERTICAL_LR
        ) && parent_style.text_orientation() == text_orientation::MIXED,
    };
    let text = &callbacks.text_content(node).text;
    callbacks.text_chunks(node, &key, || {
        chunk_text(TextChunkInputs {
            text,
            frozen_font_list: &key.frozen_font_list,
            white_space_collapse: key.white_space_collapse,
            word_break: key.word_break,
            line_break: key.line_break,
            line_break_language: key.line_break_language,
            font_variant_emoji: key.font_variant_emoji,
            should_wrap_lines,
            should_respect_linebreaks,
            unidirectional_ltr,
            splits_at_orientation_changes: key.splits_at_orientation_changes,
        })
    })
}

// Tabs occupy one UTF-16 code unit each. Other preserved whitespace is split below by next_chunk().
fn tab_chunk_end(text: &[u16], start: usize, break_after_each_tab: bool) -> usize {
    let mut end = start + 1;
    while !break_after_each_tab && text.get(end) == Some(&u16::from(b'\t')) {
        end += 1;
    }
    end
}

#[cfg(test)]
mod tests {
    use super::tab_chunk_end;

    #[test]
    fn preserved_tab_chunk_boundaries_respect_break_spaces() {
        let text: Vec<_> = "\t\t\tX".encode_utf16().collect();
        assert_eq!(tab_chunk_end(&text, 0, true), 1);
        assert_eq!(tab_chunk_end(&text, 1, true), 2);
        assert_eq!(tab_chunk_end(&text, 2, true), 3);
        // pre/pre-wrap and nowrap retain the existing tab-run grouping.
        assert_eq!(tab_chunk_end(&text, 0, false), 3);
        assert_eq!(tab_chunk_end(&text, 2, false), 3);
        assert_eq!(tab_chunk_end(&[u16::from(b'\t')], 0, true), 1);
    }
}
