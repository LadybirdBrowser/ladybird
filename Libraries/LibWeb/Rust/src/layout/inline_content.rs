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
use crate::css::style::fast_hash::FastSet as HashSet;

// Finalized inline output shared by committed fragments, formatting-context caches and painting.
// Glyph buffers move here from the line builder and own their fonts. No borrowed text or mutable
// line-building state escapes into a retained output.
#[derive(Clone, Default, PartialEq)]
pub struct InlineContent {
    pub lines: Vec<LineRecord>,
    pub fragments: Vec<FragmentRecord>,
    pub inline_box_pieces: Vec<InlineBoxPieceRecord>,
    /// Boxes and text in line order, then tree order within each line. Empty when
    /// there are no boxes to interleave, so text can be painted together.
    pub items: Vec<InlineItem>,
    pub lines_after_clamp_point_rect: Option<Box<used_values::FfiCssPixelRect>>,
    /// Where the content that text-overflow elided from the end of a line would be, by line index.
    pub elided_content_rects: Vec<(u32, used_values::FfiCssPixelRect)>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InlineItem {
    BoxPiece(u32),
    TextFragment(u32),
    AtomicInline(u32),
}

impl PartialEq for GlyphRunRecord {
    fn eq(&self, other: &Self) -> bool {
        self.font == other.font && self.glyphs == other.glyphs
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
    pub(crate) next_line_block_offset: CssPixels,
    pub baseline: CssPixels,
    pub block_level_box_block_end_margin: CssPixels,
    pub direction: u8,
    pub writing_mode: u8,
    pub original_available_inline_size: AvailableSize,
    pub has_break: bool,
    pub has_forced_break: bool,
    pub forced_break_node: NodeSlotId,
    pub has_block_level_box: bool,
    pub box_fragment: u32,
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
    pub glyphs: libgfx_rust::text_layout::GlyphBuffer,
    pub font: libgfx_rust::font::FontHandle,
    // Conservative painted bounds in CSS pixels, relative to the run's baseline origin.
    pub bounding_box: libgfx_rust::FloatRect,
}

impl GlyphRunRecord {
    pub fn glyph_intercepts(&self, scale: f32, y_top: f32, y_bottom: f32) -> Vec<f32> {
        libgfx_rust::text_layout::glyph_run_glyph_intercepts(&self.font, &self.glyphs, scale, y_top, y_bottom)
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
    pub glyph_run: Option<std::sync::Arc<GlyphRunRecord>>,
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
        let mut fragment_count = 0usize;
        let mut elided_content_rects = Vec::new();
        for (line_index, line) in data.line_boxes.iter().enumerate() {
            if let Some(rect) = inline_formatting_context::elided_content_rect(line) {
                elided_content_rects.push((line_index as u32, rect));
            }
            let visible_fragment_count = line.visible_fragments().count();
            fragment_count = fragment_count
                .checked_add(visible_fragment_count)
                .expect("fragment count overflowed");
            lines.push(LineRecord {
                rect: inline_formatting_context::line_rect(line, content_inline_size),
                fragment_count: visible_fragment_count as u32,
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
        }
        let mut fragments = Vec::with_capacity(fragment_count);
        for (line_index, line) in data.line_boxes.into_iter().enumerate() {
            for mut fragment in line.fragments {
                if fragment.is_fully_truncated {
                    continue;
                }
                fragment.record.glyph_run = fragment.glyphs.take().map(|glyph_data| {
                    let bounding_box = libgfx_rust::FloatRect::from_array(
                        libgfx_rust::text_layout::glyph_run_bounding_box(&glyph_data.font, &glyph_data.glyphs, 1.0),
                    );
                    std::sync::Arc::new(GlyphRunRecord {
                        glyphs: glyph_data.glyphs,
                        font: glyph_data.font,
                        bounding_box,
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
        let mut content = Self {
            lines,
            fragments,
            inline_box_pieces: pieces,
            items: Vec::new(),
            lines_after_clamp_point_rect: data.lines_after_clamp_point_rect,
            elided_content_rects,
        };
        // Plain text needs no interleaving and keeps its single foreground producer.
        if content.inline_box_pieces.is_empty() && !content.fragments.iter().any(|fragment| fragment.is_atomic_inline) {
            return content;
        }
        let box_pieces = content
            .inline_box_pieces
            .iter()
            .enumerate()
            .filter(|(_, piece)| !piece.is_geometry_only_placeholder)
            .map(|(index, _)| InlineItem::BoxPiece(index as u32));
        let fragment_items = content.fragments.iter().enumerate().filter_map(|(index, fragment)| {
            if fragment.glyph_run.is_some() {
                Some(InlineItem::TextFragment(index as u32))
            } else {
                fragment
                    .is_atomic_inline
                    .then_some(InlineItem::AtomicInline(index as u32))
            }
        });
        let mut items = Vec::with_capacity(box_pieces.clone().count() + fragment_items.clone().count());
        items.extend(box_pieces.chain(fragment_items));
        items.sort_by_key(|&item| {
            let (node, line) = content.item_position(item);
            (line, arena.node_pre_order_label(node))
        });
        content.items = items;
        content
    }

    pub(crate) fn merge_box_fragments<'content>(
        contents: impl Iterator<Item = Option<&'content InlineContent>>,
    ) -> Option<std::sync::Arc<InlineContent>> {
        let contents: Vec<(&InlineContent, u32, used_values::FfiCssPixelPoint)> = contents
            .enumerate()
            .filter_map(|(index, content)| {
                content.map(|content| (content, index as u32, used_values::FfiCssPixelPoint::default()))
            })
            .collect();
        if contents.is_empty() {
            return None;
        }
        Some(Self::combine(&contents))
    }

    pub(crate) fn concatenate_translated(
        contents: &[(std::sync::Arc<InlineContent>, used_values::FfiCssPixelPoint)],
    ) -> std::sync::Arc<InlineContent> {
        let contents: Vec<(&InlineContent, u32, used_values::FfiCssPixelPoint)> = contents
            .iter()
            .map(|(content, offset)| (&**content, 0, *offset))
            .collect();
        Self::combine(&contents)
    }

    fn combine(contents: &[(&InlineContent, u32, used_values::FfiCssPixelPoint)]) -> std::sync::Arc<InlineContent> {
        let interleaves = contents.iter().any(|(content, ..)| !content.items.is_empty());
        let mut combined = InlineContent::default();
        for (content, box_fragment, offset) in contents {
            combined.append_translated(content, interleaves, *box_fragment, *offset);
        }
        combined.drop_repeated_placeholder_pieces();
        std::sync::Arc::new(combined)
    }

    fn drop_repeated_placeholder_pieces(&mut self) {
        let is_placeholder =
            |piece: &InlineBoxPieceRecord| piece.is_geometry_only_placeholder && piece.fragment_count == 0;
        if !self.inline_box_pieces.iter().any(is_placeholder) {
            return;
        }
        let mut nodes_with_pieces: HashSet<NodeSlotId> = self
            .inline_box_pieces
            .iter()
            .filter(|piece| !is_placeholder(piece))
            .map(|piece| piece.node)
            .collect();
        let mut new_indices = Vec::with_capacity(self.inline_box_pieces.len());
        let mut kept_count = 0u32;
        self.inline_box_pieces.retain(|piece| {
            let keep = !is_placeholder(piece) || nodes_with_pieces.insert(piece.node);
            new_indices.push(kept_count);
            kept_count += u32::from(keep);
            keep
        });
        for item in &mut self.items {
            if let InlineItem::BoxPiece(index) = item {
                *index = new_indices[*index as usize];
            }
        }
    }

    fn append_translated(
        &mut self,
        content: &InlineContent,
        interleaves: bool,
        box_fragment: u32,
        offset: used_values::FfiCssPixelPoint,
    ) {
        let line_offset = self.lines.len() as u32;
        let fragment_offset = self.fragments.len() as u32;
        let piece_offset = self.inline_box_pieces.len() as u32;
        self.lines.extend(content.lines.iter().map(|line| {
            let mut line = LineRecord { box_fragment, ..*line };
            line.rect.x += offset.x;
            line.rect.y += offset.y;
            line.block_start += offset.y;
            line.block_end += offset.y;
            line.next_line_block_offset += offset.y;
            line
        }));
        self.elided_content_rects
            .extend(content.elided_content_rects.iter().map(|&(line_index, mut rect)| {
                rect.x += offset.x;
                rect.y += offset.y;
                (line_index + line_offset, rect)
            }));
        self.fragments
            .extend(content.fragments.iter().cloned().map(|mut fragment| {
                fragment.line_index += line_offset;
                let (inline_delta, block_delta) =
                    super::geometry::to_physical(fragment.writing_mode, offset.x, offset.y);
                fragment.inline_offset += inline_delta;
                fragment.block_offset += block_delta;
                fragment
            }));
        self.inline_box_pieces
            .extend(content.inline_box_pieces.iter().map(|piece| {
                let mut piece = *piece;
                piece.line_index += line_offset;
                piece.first_fragment_index += fragment_offset;
                piece.border_box_rect.x += offset.x;
                piece.border_box_rect.y += offset.y;
                piece.baseline += offset.y;
                piece
            }));
        if !interleaves {
            return;
        }
        if content.items.is_empty() {
            self.items.extend(
                content
                    .fragments
                    .iter()
                    .enumerate()
                    .filter(|(_, fragment)| fragment.glyph_run.is_some())
                    .map(|(index, _)| InlineItem::TextFragment(index as u32 + fragment_offset)),
            );
        } else {
            self.items.extend(content.items.iter().map(|item| match *item {
                InlineItem::BoxPiece(index) => InlineItem::BoxPiece(index + piece_offset),
                InlineItem::TextFragment(index) => InlineItem::TextFragment(index + fragment_offset),
                InlineItem::AtomicInline(index) => InlineItem::AtomicInline(index + fragment_offset),
            }));
        }
    }

    fn item_position(&self, item: InlineItem) -> (NodeSlotId, u32) {
        match item {
            InlineItem::BoxPiece(index) => {
                let piece = &self.inline_box_pieces[index as usize];
                (piece.node, piece.line_index)
            }
            InlineItem::TextFragment(index) | InlineItem::AtomicInline(index) => {
                let fragment = &self.fragments[index as usize];
                (fragment.layout_node, fragment.line_index)
            }
        }
    }

    pub(crate) fn has_same_item_order(&self, other: &Self) -> bool {
        self.items == other.items
            && self
                .items
                .iter()
                .all(|&item| self.item_position(item).0 == other.item_position(item).0)
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
