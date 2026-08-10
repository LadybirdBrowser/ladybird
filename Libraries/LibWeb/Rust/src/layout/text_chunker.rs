/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libgfx_rust::font::{EmojiPresentation, FontCascadeListRef, FontRef, emoji_presentation_for_code_point};

unsafe extern "C" {
    fn unicode_layout_grapheme_segmenter_create(text: *const u16, length_in_code_units: usize) -> *mut c_void;
    fn unicode_layout_line_segmenter_create(text: *const u16, length_in_code_units: usize) -> *mut c_void;
    fn unicode_layout_segmenter_next_boundary(handle: *mut c_void, index: usize, inclusive: bool) -> i64;
    fn unicode_layout_segmenter_destroy(handle: *mut c_void);
    fn ladybird_layout_text_type_for_code_point(code_point: u32) -> u8;
    fn ladybird_layout_code_point_has_break_all_line_break_class(code_point: u32) -> bool;
    fn ladybird_layout_code_point_has_keep_all_line_break_class(code_point: u32) -> bool;
    fn ladybird_layout_code_point_has_combining_mark_line_break_class(code_point: u32) -> bool;
    fn ladybird_layout_code_point_has_emoji_property(code_point: u32) -> bool;
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct TextChunk {
    pub start: usize,
    pub length: usize,
    pub font: *const c_void,
    pub has_breaking_newline: bool,
    pub has_breaking_tab: bool,
    pub is_all_whitespace: bool,
    pub can_break_after: bool,
    pub text_type: u8,
}

pub(crate) struct TextChunkInputs<'text> {
    pub text: &'text [u16],
    pub font_cascade_list: *const c_void,
    pub white_space_collapse: u8,
    pub word_break: u8,
    pub font_variant_emoji: u8,
    pub should_wrap_lines: bool,
    pub should_respect_linebreaks: bool,
    pub unidirectional_ltr: bool,
}

pub(crate) fn chunk_text(inputs: TextChunkInputs<'_>) -> Vec<TextChunk> {
    let mut chunker = TextChunker::new(inputs);
    let mut chunks = Vec::new();
    while let Some(chunk) = chunker.next_chunk() {
        chunks.push(chunk);
    }
    chunks
}

struct IcuSegmenterHandle {
    raw: *mut c_void,
}

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

enum GraphemeSegmenter {
    Ascii { length_in_code_units: usize },
    Icu(IcuSegmenterHandle),
}

impl GraphemeSegmenter {
    fn new(text: &[u16]) -> Self {
        if text.iter().all(|unit| *unit <= 0x7f) {
            return Self::Ascii {
                length_in_code_units: text.len(),
            };
        }
        // SAFETY: The chunker borrows the text for its whole lifetime and the
        // segmenter handle is dropped with the chunker, so the ICU segmenter
        // never outlives the buffer it references.
        let raw = unsafe { unicode_layout_grapheme_segmenter_create(text.as_ptr(), text.len()) };
        Self::Icu(IcuSegmenterHandle { raw })
    }

    /// Mirrors Unicode::AsciiGraphemeSegmenter for the ASCII case: every code
    /// unit index is a boundary.
    fn next_boundary(&self, index: usize, inclusive: bool) -> Option<usize> {
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
            Self::Icu(handle) => handle.next_boundary(index, inclusive),
        }
    }
}

struct LineSegmenter {
    handle: IcuSegmenterHandle,
}

impl LineSegmenter {
    fn new(text: &[u16]) -> Self {
        // SAFETY: See GraphemeSegmenter::new; the same lifetime contract holds,
        // and the C++ side picks the ASCII line-breaking fast path internally.
        let raw = unsafe { unicode_layout_line_segmenter_create(text.as_ptr(), text.len()) };
        Self {
            handle: IcuSegmenterHandle { raw },
        }
    }

    fn next_boundary(&self, index: usize, inclusive: bool) -> Option<usize> {
        self.handle.next_boundary(index, inclusive)
    }
}

fn is_utf16_high_surrogate(code_unit: u16) -> bool {
    (0xd800..=0xdbff).contains(&code_unit)
}

fn is_utf16_low_surrogate(code_unit: u16) -> bool {
    (0xdc00..=0xdfff).contains(&code_unit)
}

/// Mirrors AK::Utf16View::code_point_at, including its lone-surrogate
/// pass-through behavior.
fn code_point_at(text: &[u16], index: usize) -> u32 {
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

fn code_unit_length_for_code_point(code_point: u32) -> usize {
    if code_point >= 0x10000 { 2 } else { 1 }
}

fn code_point_is_ascii_space(code_point: u32) -> bool {
    matches!(code_point, 0x09..=0x0d | 0x20)
}

fn is_interword_space(code_point: u32) -> bool {
    code_point == 0x0020 || code_point == 0x00a0
}

// A chunk while it is still being accumulated: where it starts, plus the properties shared by
// every chunk that can be committed from it.
#[derive(Clone, Copy)]
struct PendingChunk<'text> {
    start: usize,
    font: FontRef<'text>,
    text_type: u8,
    broken_on_tab: bool,
}

#[derive(Clone, Copy)]
struct ChunkBreakFlags {
    has_breaking_newline: bool,
    has_breaking_tab: bool,
    can_break_after: bool,
}

struct TextChunker<'text> {
    text: &'text [u16],
    font_cascade_list: FontCascadeListRef<'text>,
    grapheme_segmenter: GraphemeSegmenter,
    line_segmenter: LineSegmenter,
    word_break: u8,
    font_variant_emoji: u8,
    should_collapse_whitespace: bool,
    should_wrap_lines: bool,
    should_respect_linebreaks: bool,
    unidirectional_ltr: bool,
    current_index: usize,
    last_non_whitespace_font: Option<FontRef<'text>>,
}

impl<'text> TextChunker<'text> {
    fn new(inputs: TextChunkInputs<'text>) -> Self {
        Self {
            text: inputs.text,
            // SAFETY: The caller guarantees the cascade list outlives the
            // chunking run (layout retains the style that owns it).
            font_cascade_list: unsafe { FontCascadeListRef::from_raw(inputs.font_cascade_list) },
            grapheme_segmenter: GraphemeSegmenter::new(inputs.text),
            line_segmenter: LineSegmenter::new(inputs.text),
            word_break: inputs.word_break,
            font_variant_emoji: inputs.font_variant_emoji,
            should_collapse_whitespace: matches!(
                inputs.white_space_collapse,
                white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
            ),
            should_wrap_lines: inputs.should_wrap_lines,
            should_respect_linebreaks: inputs.should_respect_linebreaks,
            unidirectional_ltr: inputs.unidirectional_ltr,
            current_index: 0,
            last_non_whitespace_font: None,
        }
    }

    fn current_code_point(&self) -> u32 {
        code_point_at(self.text, self.current_index)
    }

    fn current_text_type(&self) -> u8 {
        if self.unidirectional_ltr {
            return GLYPH_TEXT_TYPE_LTR;
        }
        // SAFETY: Pure Unicode table lookup.
        unsafe { ladybird_layout_text_type_for_code_point(self.current_code_point()) }
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
        if !self.should_wrap_lines {
            return false;
        }

        let get_previous_code_point = || -> Option<u32> {
            if self.current_index == 0 {
                return None;
            }
            let mut index = self.current_index;
            let mut previous_code_point = previous_code_point_at(self.text, &mut index);
            // SAFETY: Pure Unicode table lookups.
            while unsafe { ladybird_layout_code_point_has_combining_mark_line_break_class(previous_code_point) }
                && index > 0
            {
                previous_code_point = previous_code_point_at(self.text, &mut index);
            }
            Some(previous_code_point)
        };

        let is_at_line_segmenter_boundary =
            || self.line_segmenter.next_boundary(self.current_index, true) == Some(self.current_index);

        match self.word_break {
            word_break::NORMAL | word_break::BREAK_WORD => is_at_line_segmenter_boundary(),
            word_break::BREAK_ALL => {
                if self.current_index >= self.text.len() {
                    return false;
                }
                // SAFETY: Pure Unicode table lookups.
                if let Some(previous_code_point) = get_previous_code_point()
                    && unsafe { ladybird_layout_code_point_has_break_all_line_break_class(previous_code_point) }
                    && unsafe { ladybird_layout_code_point_has_break_all_line_break_class(self.current_code_point()) }
                {
                    return true;
                }
                is_at_line_segmenter_boundary()
            }
            word_break::KEEP_ALL => {
                if self.current_index >= self.text.len() {
                    return false;
                }
                // SAFETY: Pure Unicode table lookups.
                if let Some(previous_code_point) = get_previous_code_point()
                    && unsafe { ladybird_layout_code_point_has_keep_all_line_break_class(previous_code_point) }
                    && unsafe { ladybird_layout_code_point_has_keep_all_line_break_class(self.current_code_point()) }
                {
                    return false;
                }
                is_at_line_segmenter_boundary()
            }
            _ => unreachable!("invalid word-break value"),
        }
    }

    fn emoji_presentation_at(&self, code_unit_offset: usize, code_point: u32) -> EmojiPresentation {
        let next_offset = code_unit_offset + code_unit_length_for_code_point(code_point);
        let next_code_point = (next_offset < self.text.len()).then(|| code_point_at(self.text, next_offset));
        let default_presentation = emoji_presentation_for_code_point(code_point, next_code_point);

        // SAFETY: Pure Unicode table lookup.
        if default_presentation.forced || !unsafe { ladybird_layout_code_point_has_emoji_property(code_point) } {
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

    fn font_for_space(&self, at_index: usize, space_code_point: u32) -> FontRef<'text> {
        let has_glyph = |font: FontRef<'text>| font.contains_glyph(space_code_point);

        // 1. Prefer the last non-whitespace font in this node/run.
        if let Some(last_font) = self.last_non_whitespace_font
            && !last_font.is_emoji_font()
            && has_glyph(last_font)
        {
            return last_font;
        }

        // 2. Look ahead to the next non-space to infer the base font of this run.
        let mut i = at_index;
        while i < self.text.len() {
            let code_point = code_point_at(self.text, i);
            if !is_interword_space(code_point) && code_point != '\t' as u32 && code_point != '\n' as u32 {
                let font =
                    self.font_cascade_list
                        .font_for_code_point(code_point, true, self.emoji_presentation_at(i, code_point));
                if !font.is_emoji_font() && has_glyph(font) {
                    return font;
                }
                // Text is coming from an emoji face; we'll fall back to (3).
                break;
            }
            i = self.grapheme_segmenter.next_boundary(i, false).unwrap_or(self.text.len());
        }

        // 3. No text around (leading/trailing/all spaces) — pick a font with the glyph from the cascade.
        self.font_cascade_list.font_for_code_point(
            space_code_point,
            true,
            EmojiPresentation {
                is_emoji: false,
                forced: false,
            },
        )
    }

    fn expected_font_for(&self, code_point: u32) -> FontRef<'text> {
        if is_interword_space(code_point) {
            self.font_for_space(self.current_index, code_point)
        } else {
            self.font_cascade_list.font_for_code_point(
                code_point,
                true,
                self.emoji_presentation_at(self.current_index, code_point),
            )
        }
    }

    fn try_commit_chunk(
        &mut self,
        start: usize,
        end: usize,
        break_flags: ChunkBreakFlags,
        font: FontRef<'text>,
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
            self.last_non_whitespace_font = Some(font);
        }
        Some(TextChunk {
            start,
            length: length_in_code_units,
            font: font.as_raw(),
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
                broken_on_tab: false,
            };

            while self.current_index < self.text.len() {
                code_point = self.current_code_point();

                if code_point == '\t' as u32 {
                    if let Some(chunk) = self.try_commit_chunk_at_cursor(pending, false) {
                        return Some(chunk);
                    }

                    pending.broken_on_tab = true;
                    // consume any consecutive tabs
                    while self.current_index < self.text.len() && self.current_code_point() == '\t' as u32 {
                        self.current_index = self.next_grapheme_boundary();
                    }
                    can_break_at_current_position = self.is_at_line_break_opportunity();
                }

                let expected_font = self.expected_font_for(code_point);

                if pending.font != expected_font
                    && let Some(chunk) = self.try_commit_chunk_at_cursor(pending, can_break_at_current_position)
                {
                    return Some(chunk);
                }

                if self.should_respect_linebreaks && code_point == '\n' as u32 {
                    // Newline encountered, and we're supposed to preserve them.
                    // If we have accumulated some code points in the current chunk, commit them now and continue with
                    // the newline next time.
                    if let Some(chunk) = self.try_commit_chunk_at_cursor(pending, false) {
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
                        pending.font,
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
                    let chunk = self.try_commit_chunk_at_cursor(pending, false);

                    while self.current_index < self.text.len() && self.is_collapsible(self.current_code_point()) {
                        self.current_index = self.next_grapheme_boundary();
                    }

                    if let Some(chunk) = chunk {
                        return Some(chunk);
                    }

                    continue 'restart;
                }

                // NB: The current index can sit past the end here after consuming trailing tabs; the retired C++
                // iterator read the direction class through it anyway, so the bounds check turns an out-of-bounds
                // read into a non-split.
                if self.should_wrap_lines
                    && self.current_index < self.text.len()
                    && pending.text_type != self.current_text_type()
                    && let Some(chunk) = self.try_commit_chunk_at_cursor(pending, can_break_at_current_position)
                {
                    return Some(chunk);
                }

                if self.should_wrap_lines {
                    if code_point_is_ascii_space(code_point) {
                        // Whitespace encountered, and we're allowed to break on whitespace.
                        // If we have accumulated some code points in the current chunk, commit them now and continue
                        // with the whitespace next time.
                        if let Some(chunk) = self.try_commit_chunk_at_cursor(pending, false) {
                            return Some(chunk);
                        }

                        // Otherwise, commit the whitespace!
                        self.current_index = self.next_grapheme_boundary();
                        can_break_at_current_position = self.is_at_line_break_opportunity();
                        let space_font = self.font_for_space(self.current_index, code_point);
                        let space = PendingChunk {
                            font: space_font,
                            ..pending
                        };
                        if let Some(chunk) = self.try_commit_chunk_at_cursor(space, false) {
                            return Some(chunk);
                        }
                        continue;
                    }

                    if can_break_at_current_position
                        && let Some(chunk) = self.try_commit_chunk_at_cursor(pending, true)
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
                    pending.font,
                    pending.text_type,
                ) {
                    return Some(chunk);
                }
            }

            return None;
        }
    }

    // Commits everything accumulated since `pending.start` up to the cursor, if that range is non-empty.
    fn try_commit_chunk_at_cursor(&mut self, pending: PendingChunk<'text>, can_break_after: bool) -> Option<TextChunk> {
        self.try_commit_chunk(
            pending.start,
            self.current_index,
            ChunkBreakFlags {
                has_breaking_newline: false,
                has_breaking_tab: pending.broken_on_tab,
                can_break_after,
            },
            pending.font,
            pending.text_type,
        )
    }
}

pub(crate) fn text_chunks(
    callbacks: &FfiLayoutFcCallbacks,
    node: Node,
    should_wrap_lines: bool,
    should_respect_linebreaks: bool,
    unidirectional_ltr: bool,
) -> &'static [TextChunk] {
    let parent_style = StyleValues::for_node(callbacks, callbacks.parent(node));
    let key = crate::layout::layout_node_arena::TextChunkCacheKey {
        should_wrap_lines,
        should_respect_linebreaks,
        unidirectional_ltr,
        white_space_collapse: parent_style.white_space_collapse(),
        word_break: parent_style.word_break(),
        font_variant_emoji: parent_style.font_variant_emoji(),
        font_cascade_list: parent_style.font_cascade_list(),
    };
    let text = &callbacks.text_content(node).text;
    callbacks.arena().text_chunks(node, key, || {
        chunk_text(TextChunkInputs {
            text,
            font_cascade_list: key.font_cascade_list,
            white_space_collapse: key.white_space_collapse,
            word_break: key.word_break,
            font_variant_emoji: key.font_variant_emoji,
            should_wrap_lines,
            should_respect_linebreaks,
            unidirectional_ltr,
        })
    })
}
