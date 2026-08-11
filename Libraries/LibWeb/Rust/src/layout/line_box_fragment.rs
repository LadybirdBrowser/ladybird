/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub(crate) const GLYPH_TEXT_TYPE_COMMON: u8 = 0;
pub(crate) const GLYPH_TEXT_TYPE_CONTEXT_DEPENDENT: u8 = 1;
pub(crate) const GLYPH_TEXT_TYPE_END_PADDING: u8 = 2;
pub(crate) const GLYPH_TEXT_TYPE_LTR: u8 = 3;
pub(crate) const GLYPH_TEXT_TYPE_RTL: u8 = 4;

pub(crate) fn is_ascii_space(code_unit: u16) -> bool {
    matches!(code_unit, 0x09..=0x0d | 0x20)
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct GlyphData {
    pub(crate) glyphs: Vec<FfiDrawGlyph>,
    pub(crate) font: *const c_void,
    pub(crate) text_type: u8,
    // GlyphRun::width is not updated by the C++ fragment merge machine.
    pub(crate) width: f32,
}

// The advance of a run's trailing whitespace, recorded at shaping time so that trimming it subtracts exactly what
// shaping added.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct TrailingWhitespace {
    pub(crate) length_in_code_units: usize,
    pub(crate) inline_size: CssPixels,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct LineBoxFragmentData {
    pub(crate) layout_node: Node,
    pub(crate) style_source: Node,
    pub(crate) start: usize,
    pub(crate) length_in_code_units: usize,
    pub(crate) inline_offset: CssPixels,
    pub(crate) block_offset: CssPixels,
    pub(crate) relpos_delta: FfiCssPixelPoint,
    pub(crate) inline_length: CssPixels,
    pub(crate) block_length: CssPixels,
    pub(crate) border_box_block_start: CssPixels,
    pub(crate) baseline: CssPixels,
    pub(crate) direction: u8,
    pub(crate) writing_mode: u8,
    pub(crate) glyphs: Option<GlyphData>,
    pub(crate) insert_position: f32,
    pub(crate) current_insert_direction: u8,
    pub(crate) has_trailing_whitespace: bool,
    pub(crate) is_fully_truncated: bool,
    pub(crate) is_atomic_inline: bool,
    pub(crate) white_space_collapse: u8,
    pub(crate) trailing_whitespace: TrailingWhitespace,
    pub(crate) text_utf16: *const u16,
    pub(crate) text_length_in_code_units: usize,
    pub(crate) content_baselines: Option<DerivedBaselines>,
}

#[derive(Clone, Copy)]
pub(crate) struct FragmentBuildFacts {
    pub(crate) style_source: Node,
    pub(crate) is_atomic_inline: bool,
    pub(crate) white_space_collapse: u8,
    pub(crate) text_utf16: *const u16,
    pub(crate) text_length_in_code_units: usize,
}

impl LineBoxFragmentData {
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn new(
        layout_node: Node,
        start: usize,
        length_in_code_units: usize,
        inline_offset: CssPixels,
        block_offset: CssPixels,
        inline_length: CssPixels,
        block_length: CssPixels,
        border_box_block_start: CssPixels,
        direction: u8,
        writing_mode: u8,
        glyphs: Option<GlyphData>,
        facts: FragmentBuildFacts,
    ) -> Self {
        let mut fragment = Self {
            layout_node,
            style_source: facts.style_source,
            start,
            length_in_code_units,
            inline_offset,
            block_offset,
            relpos_delta: FfiCssPixelPoint::default(),
            inline_length,
            block_length,
            border_box_block_start,
            baseline: CssPixels::default(),
            direction,
            writing_mode,
            glyphs,
            insert_position: 0.0,
            current_insert_direction: direction::LTR,
            has_trailing_whitespace: false,
            is_fully_truncated: false,
            is_atomic_inline: facts.is_atomic_inline,
            white_space_collapse: facts.white_space_collapse,
            trailing_whitespace: TrailingWhitespace::default(),
            text_utf16: facts.text_utf16,
            text_length_in_code_units: facts.text_length_in_code_units,
            content_baselines: None,
        };
        if let Some(glyphs) = &fragment.glyphs {
            fragment.current_insert_direction = fragment.resolve_glyph_run_direction(glyphs.text_type);
            if fragment.direction == direction::RTL {
                fragment.insert_position = fragment.inline_length.to_double() as f32;
            }
        }
        fragment
    }

    pub(crate) fn offset(&self) -> (CssPixels, CssPixels) {
        to_physical(self.writing_mode, self.inline_offset, self.block_offset)
    }

    pub(crate) fn size(&self) -> (CssPixels, CssPixels) {
        to_physical(self.writing_mode, self.inline_length, self.block_length)
    }

    pub(crate) fn physical_horizontal_extent(&self) -> CssPixels {
        to_physical(self.writing_mode, self.inline_length, self.block_length).0
    }

    pub(crate) fn physical_vertical_extent(&self) -> CssPixels {
        to_physical(self.writing_mode, self.inline_length, self.block_length).1
    }

    pub(crate) fn text(&self) -> &[u16] {
        if self.length_in_code_units == 0 {
            return &[];
        }
        assert!(!self.text_utf16.is_null());
        assert!(self.start + self.length_in_code_units <= self.text_length_in_code_units);
        // SAFETY: The text snapshot owns this buffer for the layout-state
        // lifetime, and the asserted range comes from its chunk snapshot.
        unsafe { std::slice::from_raw_parts(self.text_utf16.add(self.start), self.length_in_code_units) }
    }

    pub(crate) fn ends_in_whitespace(&self) -> bool {
        self.text().last().is_some_and(|code_unit| is_ascii_space(*code_unit))
    }

    pub(crate) fn is_justifiable_whitespace(&self) -> bool {
        self.text() == [b' ' as u16]
    }

    fn resolve_glyph_run_direction(&self, text_type: u8) -> u8 {
        match text_type {
            GLYPH_TEXT_TYPE_COMMON | GLYPH_TEXT_TYPE_CONTEXT_DEPENDENT | GLYPH_TEXT_TYPE_END_PADDING => self.direction,
            GLYPH_TEXT_TYPE_LTR => direction::LTR,
            GLYPH_TEXT_TYPE_RTL => direction::RTL,
            _ => panic!("invalid glyph text type"),
        }
    }

    pub(crate) fn append_glyph_run(&mut self, run: GlyphData, run_inline_size: CssPixels) {
        match self.direction {
            direction::LTR => self.append_glyph_run_ltr(run, run_inline_size),
            direction::RTL => self.append_glyph_run_rtl(run, run_inline_size),
            _ => panic!("invalid direction"),
        }
    }

    fn append_glyph_run_ltr(&mut self, run: GlyphData, run_inline_size: CssPixels) {
        let run_direction = self.resolve_glyph_run_direction(run.text_type);
        let inline_offset = self.inline_length.to_double() as f32;

        if self.current_insert_direction != run_direction {
            if run_direction == direction::RTL {
                self.insert_position = inline_offset;
            }
            self.current_insert_direction = run_direction;
        }

        let existing = self.glyphs.as_mut().expect("merged fragment must have a glyph run");
        existing.glyphs.reserve(run.glyphs.len());
        match run_direction {
            direction::LTR => {
                for mut glyph in run.glyphs {
                    glyph.x += inline_offset;
                    existing.glyphs.push(glyph);
                }
            }
            direction::RTL => {
                let run_offset = run_inline_size.to_double() as f32;
                for glyph in &mut existing.glyphs {
                    if glyph.x >= self.insert_position {
                        glyph.x += run_offset;
                    }
                }
                for mut glyph in run.glyphs {
                    glyph.x += self.insert_position;
                    existing.glyphs.push(glyph);
                }
            }
            _ => unreachable!(),
        }
        self.inline_length += run_inline_size;
    }

    fn append_glyph_run_rtl(&mut self, run: GlyphData, run_inline_size: CssPixels) {
        let run_direction = self.resolve_glyph_run_direction(run.text_type);
        let run_offset = run_inline_size.to_double() as f32;

        if self.current_insert_direction != run_direction {
            if run_direction == direction::LTR {
                self.insert_position = 0.0;
            }
            self.current_insert_direction = run_direction;
        }

        let existing = self.glyphs.as_mut().expect("merged fragment must have a glyph run");
        existing.glyphs.reserve(run.glyphs.len());
        match run_direction {
            direction::LTR => {
                for glyph in &mut existing.glyphs {
                    if glyph.x >= self.insert_position {
                        glyph.x += run_offset;
                    }
                }
                for mut glyph in run.glyphs {
                    glyph.x += self.insert_position;
                    existing.glyphs.push(glyph);
                }
            }
            direction::RTL => {
                if run.text_type != GLYPH_TEXT_TYPE_END_PADDING {
                    for glyph in &mut existing.glyphs {
                        glyph.x += run_offset;
                    }
                }
                existing.glyphs.extend(run.glyphs);
            }
            _ => unreachable!(),
        }

        self.inline_length += run_inline_size;
        self.insert_position += run_offset;
    }
}
