/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::CssPixels;
use super::geometry::AvailableSize;
use super::layout_node_arena::LayoutNodeArena;
use super::node_data::NodeSlotId;
use super::{formatting_context, inline_formatting_context, used_values};
use std::rc::Rc;

// Finalized inline output shared by committed fragments, formatting-context caches and painting.
// Glyph buffers move here from the line builder and own their fonts. No borrowed text or mutable
// line-building state escapes into a retained output.
#[derive(Clone, Default, PartialEq)]
pub struct InlineContent {
    pub lines: Vec<LineRecord>,
    pub fragments: Vec<FragmentRecord>,
    pub inline_box_pieces: Vec<InlineBoxPieceRecord>,
}

impl PartialEq for GlyphRunRecord {
    fn eq(&self, other: &Self) -> bool {
        self.font.as_raw() == other.font.as_raw() && self.glyphs == other.glyphs
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LineRecord {
    pub rect: used_values::FfiCssPixelRect,
    pub fragment_count: u32,
    pub(crate) layout_fragment_count: usize,
    pub(crate) first_fragment_node: Option<NodeSlotId>,
    pub(crate) is_empty: bool,
    pub(crate) horizontal_extent: CssPixels,
    pub(crate) reusable_atomic_prefix: bool,
    pub inline_length: CssPixels,
    pub inline_length_before_block_ellipsis: Option<CssPixels>,
    pub block_length: CssPixels,
    pub block_start: CssPixels,
    pub block_end: CssPixels,
    pub baseline: CssPixels,
    pub block_level_box_block_end_margin: CssPixels,
    pub direction: u8,
    pub writing_mode: u8,
    pub original_available_inline_size: AvailableSize,
    pub has_break: bool,
    pub has_forced_break: bool,
    pub has_block_level_box: bool,
}

impl LineRecord {
    pub(crate) fn physical_horizontal_extent(&self) -> CssPixels {
        super::geometry::to_physical(self.writing_mode, self.inline_length, self.block_length).0
    }

    pub(crate) fn physical_vertical_extent(&self) -> CssPixels {
        super::geometry::to_physical(self.writing_mode, self.inline_length, self.block_length).1
    }

    pub(crate) fn physical_vertical_end(&self) -> CssPixels {
        super::geometry::to_physical(self.writing_mode, self.inline_length, self.block_end).1
    }
}

pub struct GlyphRunRecord {
    pub glyphs: Vec<libgfx_rust::text_layout::DrawGlyph>,
    pub font: libgfx_rust::font::RetainedFont,
}

impl GlyphRunRecord {
    fn font_ref(&self) -> libgfx_rust::font::FontRef<'_> {
        // SAFETY: The record's RetainedFont keeps the font live for the
        // lifetime of the returned borrow.
        unsafe { libgfx_rust::font::FontRef::from_raw(self.font.as_raw()) }
    }

    pub fn bounding_box(&self, scale: f32) -> [f32; 4] {
        libgfx_rust::text_layout::glyph_run_bounding_box(self.font_ref(), &self.glyphs, scale)
    }

    pub fn glyph_intercepts(&self, scale: f32, y_top: f32, y_bottom: f32) -> Vec<f32> {
        libgfx_rust::text_layout::glyph_run_glyph_intercepts(self.font_ref(), &self.glyphs, scale, y_top, y_bottom)
    }
}

#[derive(Clone, PartialEq)]
pub struct FragmentRecord {
    pub layout_node: NodeSlotId,
    pub style_source: NodeSlotId,
    pub start: usize,
    pub length_in_code_units: usize,
    pub inline_offset: CssPixels,
    pub block_offset: CssPixels,
    pub relpos_delta: used_values::FfiCssPixelPoint,
    pub inline_length: CssPixels,
    pub block_length: CssPixels,
    pub border_box_block_start: CssPixels,
    pub baseline: CssPixels,
    pub accumulated_vertical_shift: CssPixels,
    pub direction: u8,
    pub writing_mode: u8,
    pub is_block_ellipsis: bool,
    pub is_atomic_inline: bool,
    pub white_space_collapse: u8,
    pub(crate) content_baselines: Option<formatting_context::DerivedBaselines>,
    pub line_index: u32,
    pub dom_start_offset_in_node: usize,
    pub dom_end_offset_in_node: usize,
    pub dom_end_offset_with_trailing_whitespace: usize,
    pub trailing_whitespace_length_in_code_units: usize,
    pub glyph_run: Option<Rc<GlyphRunRecord>>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct InlineBoxPieceRecord {
    pub node: NodeSlotId,
    pub first_fragment_index: u32,
    pub fragment_count: u32,
    pub line_index: u32,
    pub border_box_rect: used_values::FfiCssPixelRect,
    pub relpos_delta: used_values::FfiCssPixelPoint,
    pub baseline: CssPixels,
    pub accumulated_vertical_shift: CssPixels,
    pub present_edges: u8,
    pub is_geometry_only_placeholder: bool,
}

impl InlineContent {
    pub(crate) fn finish(data: used_values::LineData, arena: &LayoutNodeArena, content_inline_size: CssPixels) -> Self {
        let mut lines = Vec::with_capacity(data.line_boxes.len());
        let mut fragments = Vec::new();
        for (line_index, line) in data.line_boxes.into_iter().enumerate() {
            lines.push(LineRecord {
                rect: inline_formatting_context::line_rect(&line, content_inline_size),
                fragment_count: line.visible_fragments().count() as u32,
                reusable_atomic_prefix: !line.fragments.is_empty()
                    && !line.has_block_level_box
                    && line.static_position_markers.is_empty()
                    && line.inline_box_baselines.is_empty()
                    && line
                        .fragments
                        .iter()
                        .all(|fragment| fragment.is_atomic_inline && !fragment.is_fully_truncated),
                ..line.retained_metrics()
            });
            for mut fragment in line.fragments {
                if fragment.is_fully_truncated {
                    continue;
                }
                fragment.record.glyph_run = fragment.glyphs.take().map(|glyph_data| {
                    Rc::new(GlyphRunRecord {
                        glyphs: glyph_data.glyphs,
                        // SAFETY: The layout pass borrowed the font from a live cascade list; retaining
                        // it here keeps it alive for as long as the fragment record.
                        font: unsafe { libgfx_rust::font::RetainedFont::retain(glyph_data.font) },
                    })
                });
                fragment.record.line_index = line_index as u32;
                fragment
                    .record
                    .finish_text_offsets(arena, fragment.has_trailing_whitespace);
                fragments.push(fragment.record);
            }
        }
        let mut pieces = data.inline_box_pieces;
        for piece in &mut pieces {
            assert!((piece.first_fragment_index + piece.fragment_count) as usize <= fragments.len());
            piece.border_box_rect.x += piece.relpos_delta.x;
            piece.border_box_rect.y += piece.relpos_delta.y;
            piece.baseline += piece.relpos_delta.y;
            piece.relpos_delta = used_values::FfiCssPixelPoint::default();
        }
        Self {
            lines,
            fragments,
            inline_box_pieces: pieces,
        }
    }
}

impl FragmentRecord {
    fn finish_text_offsets(&mut self, arena: &LayoutNodeArena, has_trailing_whitespace: bool) {
        let end = self.start + self.length_in_code_units;
        self.trailing_whitespace_length_in_code_units = if has_trailing_whitespace {
            arena.text_content(self.layout_node).map_or(0, |content| {
                content
                    .text
                    .iter()
                    .skip(end)
                    .take_while(|&&unit| unit == u16::from(b' ') || unit == u16::from(b'\t'))
                    .count()
            })
        } else {
            0
        };
        let dom_offset =
            |offset, boundary| arena.dom_offset_for_rendered_text_offset(self.layout_node, offset, boundary);
        self.dom_start_offset_in_node = dom_offset(self.start, super::RenderedTextBoundary::Start);
        self.dom_end_offset_in_node = if self.length_in_code_units == 0 {
            self.dom_start_offset_in_node
        } else {
            dom_offset(end, super::RenderedTextBoundary::End)
        };
        self.dom_end_offset_with_trailing_whitespace = if self.trailing_whitespace_length_in_code_units == 0 {
            self.dom_end_offset_in_node
        } else {
            dom_offset(
                end + self.trailing_whitespace_length_in_code_units,
                super::RenderedTextBoundary::End,
            )
        };
    }

    pub(crate) fn offset(&self) -> (CssPixels, CssPixels) {
        super::geometry::to_physical(self.writing_mode, self.inline_offset, self.block_offset)
    }

    pub(crate) fn size(&self) -> (CssPixels, CssPixels) {
        super::geometry::to_physical(self.writing_mode, self.inline_length, self.block_length)
    }

    pub(crate) fn physical_horizontal_extent(&self) -> CssPixels {
        super::geometry::to_physical(self.writing_mode, self.inline_length, self.block_length).0
    }

    pub(crate) fn physical_vertical_extent(&self) -> CssPixels {
        super::geometry::to_physical(self.writing_mode, self.inline_length, self.block_length).1
    }
}
