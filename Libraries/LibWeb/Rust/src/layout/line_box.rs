/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct StaticPositionMarker {
    pub(crate) box_: Node,
    pub(crate) inline_offset: CssPixels,
    pub(crate) block_offset: CssPixels,
    pub(crate) writing_mode: u8,
    pub(crate) preceded_by_in_flow_content: bool,
}

impl StaticPositionMarker {
    pub(crate) fn offset(self) -> (CssPixels, CssPixels) {
        geometry::to_physical(self.writing_mode, self.inline_offset, self.block_offset)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct InlineBoxBaseline {
    pub(crate) box_: Node,
    pub(crate) baseline: CssPixels,
    pub(crate) accumulated_vertical_shift: CssPixels,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct LineBoxData {
    pub(crate) fragments: Vec<line_box_fragment::LineBoxFragmentData>,
    pub(crate) static_position_markers: Vec<StaticPositionMarker>,
    pub(crate) inline_box_baselines: Vec<InlineBoxBaseline>,
    pub(crate) inline_length: CssPixels,
    pub(crate) block_length: CssPixels,
    pub(crate) block_start: CssPixels,
    pub(crate) block_end: CssPixels,
    pub(crate) baseline: CssPixels,
    pub(crate) block_level_box_block_end_margin: CssPixels,
    pub(crate) direction: u8,
    pub(crate) writing_mode: u8,
    pub(crate) original_available_inline_size: AvailableSize,
    pub(crate) has_break: bool,
    pub(crate) has_forced_break: bool,
    pub(crate) has_block_level_box: bool,
}

impl LineBoxData {
    pub(crate) fn new(direction: u8, writing_mode: u8) -> Self {
        Self {
            fragments: Vec::new(),
            static_position_markers: Vec::new(),
            inline_box_baselines: Vec::new(),
            inline_length: CssPixels::default(),
            block_length: CssPixels::default(),
            block_start: CssPixels::default(),
            block_end: CssPixels::default(),
            baseline: CssPixels::default(),
            block_level_box_block_end_margin: CssPixels::default(),
            direction,
            writing_mode,
            original_available_inline_size: AvailableSize::Indefinite,
            has_break: false,
            has_forced_break: false,
            has_block_level_box: false,
        }
    }

    pub(crate) fn physical_horizontal_extent(&self) -> CssPixels {
        geometry::to_physical(self.writing_mode, self.inline_length, self.block_length).0
    }

    pub(crate) fn physical_vertical_extent(&self) -> CssPixels {
        geometry::to_physical(self.writing_mode, self.inline_length, self.block_length).1
    }

    pub(crate) fn physical_vertical_end(&self) -> CssPixels {
        geometry::to_physical(self.writing_mode, self.inline_length, self.block_end).1
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn add_fragment(
        &mut self,
        layout_node: Node,
        start: usize,
        length: usize,
        leading_size: CssPixels,
        trailing_size: CssPixels,
        leading_margin: CssPixels,
        trailing_margin: CssPixels,
        content_inline_size: CssPixels,
        content_block_size: CssPixels,
        border_box_block_start: CssPixels,
        border_box_block_end: CssPixels,
        glyphs: Option<line_box_fragment::GlyphData>,
        facts: line_box_fragment::FragmentBuildFacts,
        text_align_is_justify: bool,
        trailing_whitespace: line_box_fragment::TrailingWhitespace,
    ) {
        let can_merge = glyphs.as_ref().is_some_and(|glyphs| {
            !text_align_is_justify
                && self.fragments.last().is_some_and(|last| {
                    last.layout_node == layout_node
                        && last
                            .glyphs
                            .as_ref()
                            .is_some_and(|last_glyphs| last_glyphs.font == glyphs.font)
                        && last.start + last.length_in_code_units == start
                })
        });
        if can_merge {
            let last = self.fragments.last_mut().unwrap();
            last.length_in_code_units += length;
            last.append_glyph_run(glyphs.unwrap(), content_inline_size);
            if trailing_whitespace.length_in_code_units == length {
                last.trailing_whitespace.length_in_code_units += trailing_whitespace.length_in_code_units;
                last.trailing_whitespace.inline_size += trailing_whitespace.inline_size;
            } else {
                last.trailing_whitespace = trailing_whitespace;
            }
        } else {
            let inline_offset = leading_margin + leading_size + self.inline_length;
            let mut fragment = line_box_fragment::LineBoxFragmentData::new(
                layout_node,
                start,
                length,
                inline_offset,
                CssPixels::default(),
                content_inline_size,
                content_block_size,
                border_box_block_start,
                self.direction,
                self.writing_mode,
                glyphs,
                facts,
            );
            fragment.trailing_whitespace = trailing_whitespace;
            self.fragments.push(fragment);
        }
        self.inline_length += leading_margin + leading_size + content_inline_size + trailing_size + trailing_margin;
        self.block_length = self
            .block_length
            .max(content_block_size + border_box_block_start + border_box_block_end);
    }

    pub(crate) fn add_static_position_marker(&mut self, box_: Node, preceded_by_inline_box_start_edges: bool) {
        self.static_position_markers.push(StaticPositionMarker {
            box_,
            inline_offset: self.inline_length,
            block_offset: CssPixels::default(),
            writing_mode: self.writing_mode,
            preceded_by_in_flow_content: !self.fragments.is_empty() || preceded_by_inline_box_start_edges,
        });
    }

    pub(crate) fn set_inline_box_baseline(
        &mut self,
        box_: Node,
        baseline: CssPixels,
        accumulated_vertical_shift: CssPixels,
    ) -> bool {
        if self.inline_box_baselines.iter().any(|entry| entry.box_ == box_) {
            return false;
        }
        self.inline_box_baselines.push(InlineBoxBaseline {
            box_,
            baseline,
            accumulated_vertical_shift,
        });
        true
    }

    pub(crate) fn inline_box_baseline(&self, box_: Node) -> Option<InlineBoxBaseline> {
        self.inline_box_baselines
            .iter()
            .find(|entry| entry.box_ == box_)
            .copied()
    }

    pub(crate) fn clamp_static_position_markers_to_inline_length(&mut self) {
        for marker in &mut self.static_position_markers {
            if marker.inline_offset > self.inline_length {
                marker.inline_offset = self.inline_length;
            }
        }
    }

    fn calculate_or_trim_trailing_whitespace(&mut self, should_remove: bool) -> CssPixels {
        let mut whitespace_inline_size = CssPixels::default();
        let mut trailing_whitespace_inline_size = CssPixels::default();
        let mut fragment_index = self.fragments.len();

        let last_fragment_index = loop {
            if fragment_index == 0 {
                return whitespace_inline_size;
            }
            fragment_index -= 1;
            let fragment = &self.fragments[fragment_index];
            if !matches!(
                fragment.white_space_collapse,
                white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
            ) {
                return whitespace_inline_size;
            }
            if !fragment.is_justifiable_whitespace() {
                break fragment_index;
            }

            whitespace_inline_size += fragment.inline_length;
            trailing_whitespace_inline_size += fragment.inline_length;
            if should_remove {
                self.inline_length -= fragment.inline_length;
                self.fragments.remove(fragment_index);
                self.clamp_static_position_markers_to_inline_length();
            }
        };

        let last_fragment = &self.fragments[last_fragment_index];
        if last_fragment.text().is_empty() {
            if should_remove && trailing_whitespace_inline_size > CssPixels::default() {
                self.fragments[last_fragment_index].has_trailing_whitespace = true;
            }
            return whitespace_inline_size;
        }

        // Trim the last fragment's trailing whitespace by the advance recorded when it was shaped.
        let fragment_trailing_whitespace = last_fragment.trailing_whitespace;
        whitespace_inline_size += fragment_trailing_whitespace.inline_size;
        trailing_whitespace_inline_size += fragment_trailing_whitespace.inline_size;
        if should_remove {
            let fragment = &mut self.fragments[last_fragment_index];
            fragment.length_in_code_units -= fragment_trailing_whitespace.length_in_code_units;
            fragment.inline_length -= fragment_trailing_whitespace.inline_size;
            fragment.trailing_whitespace = line_box_fragment::TrailingWhitespace::default();
            self.inline_length -= fragment_trailing_whitespace.inline_size;
            self.clamp_static_position_markers_to_inline_length();
        }

        if should_remove
            && (trailing_whitespace_inline_size > CssPixels::default()
                || !self.fragments[last_fragment_index].has_trailing_whitespace)
        {
            self.fragments[last_fragment_index].has_trailing_whitespace =
                trailing_whitespace_inline_size > CssPixels::default();
        }
        whitespace_inline_size
    }

    pub(crate) fn trailing_whitespace_inline_size(&mut self) -> CssPixels {
        self.calculate_or_trim_trailing_whitespace(false)
    }

    pub(crate) fn trim_trailing_whitespace(&mut self) {
        self.calculate_or_trim_trailing_whitespace(true);
    }

    pub(crate) fn is_empty_or_ends_in_whitespace(&self) -> bool {
        self.fragments
            .last()
            .is_none_or(line_box_fragment::LineBoxFragmentData::ends_in_whitespace)
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.fragments.is_empty() && !self.has_break
    }
}
