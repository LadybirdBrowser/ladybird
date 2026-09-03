/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::layout::{formatting_context, fragment_tree, inline_formatting_context, node_facts, used_values};
use crate::painting::node_painting;
use crate::painting::paintable_data::*;
use crate::painting::visual_context::dirty::VisualContextBoxDirtyKind;

#[derive(Clone, Copy, Debug)]
pub(crate) struct PreparedPaintable {
    pub(crate) has_paintable_row: bool,
    pub(crate) row_existed_before_this_commit: bool,
}

pub(crate) struct ReplacedCommittedFragmentLink {
    pub(crate) content_size_change: Option<(used_values::FfiCssPixelSize, used_values::FfiCssPixelSize)>,
    pub(crate) committed_fragment_identity_changed: bool,
}

pub(crate) struct PaintableCommit<'a> {
    callbacks: &'a formatting_context::FfiLayoutFcCallbacks,
    committed_offsets_before_recommit_reset: std::collections::HashMap<NodeSlotId, used_values::FfiCssPixelPoint>,
    committed_navigable_container_viewports: Vec<NodeSlotId>,
}

impl<'a> PaintableCommit<'a> {
    pub(crate) fn new(callbacks: &'a formatting_context::FfiLayoutFcCallbacks) -> Self {
        Self {
            callbacks,
            committed_offsets_before_recommit_reset: std::collections::HashMap::new(),
            committed_navigable_container_viewports: Vec::new(),
        }
    }

    fn arena(&self) -> &LayoutNodeArena {
        self.callbacks.arena()
    }

    fn arena_mut(&mut self) -> &mut LayoutNodeArena {
        // SAFETY: Layout commit is synchronous on the document thread. The mutable
        // PaintableCommit borrow prevents Rust code from retaining another arena
        // borrow across this callback-free mutation phase.
        unsafe { LayoutNodeArena::from_handle_mut(self.callbacks.arena) }
    }

    pub(crate) fn discard_absolute_rects_memoized_during_commit(&self) {
        self.arena().clear_absolute_rect_memo();
    }

    pub(crate) fn begin_commit(&self) {
        self.arena().clear_absolute_rect_memo();
    }

    pub(crate) fn prepare_node(
        &mut self,
        node: formatting_context::Node,
        has_used_values: bool,
        reuses_committed_subtree: bool,
        enclosing_line_root_content_changed: bool,
    ) -> PreparedPaintable {
        let (wants_paintable, node_kind) = {
            let facts = node_facts::NodeFacts::new(self.callbacks, node);
            let data = self.callbacks.node_data(node);
            (
                (has_used_values || (facts.is_fragmented_inline() && facts.has_dom_node()))
                    && node_painting::has_paintable(data.kind.get()),
                data.kind.get(),
            )
        };
        let row_existed_before_this_commit = self.arena().paintable_rows().paintable_row_is_populated(node);
        if !wants_paintable {
            self.arena().clear_committed_fragment_link(node);
            if row_existed_before_this_commit {
                let reset = {
                    let arena = self.arena();
                    arena.invalidate_paint_cache(node);
                    arena
                        .prepare_paintable_row_cleared_reset(node)
                        .expect("live row for node could not be cleared")
                };
                reset.invoke_callback();
                self.arena_mut().paintable_row_cleared(reset);
            }
            return PreparedPaintable {
                has_paintable_row: false,
                row_existed_before_this_commit: false,
            };
        }
        if node_kind == NodeKind::NavigableContainerViewport {
            self.committed_navigable_container_viewports.push(node);
        }
        if reuses_committed_subtree {
            assert!(
                row_existed_before_this_commit,
                "a kept subtree root has no committed row"
            );
            let offset = self.arena().paintable_rows().paintable_data(node).offset;
            self.committed_offsets_before_recommit_reset.insert(node, offset);
            return PreparedPaintable {
                has_paintable_row: true,
                row_existed_before_this_commit: true,
            };
        }
        if !has_used_values {
            self.arena().clear_committed_fragment_link(node);
            // Fragmented inlines commit no fragment link, so the identity diff never sees them;
            // their painted output changes exactly when the enclosing line root's fragment did.
            if row_existed_before_this_commit && enclosing_line_root_content_changed {
                self.arena().paintable_rows().mark_paint_cache_self_dirty(node);
                self.arena()
                    .note_visual_context_box_dirty(node, VisualContextBoxDirtyKind::InlineGeometryChanged);
            }
        }
        if row_existed_before_this_commit {
            let (offset, notification) = {
                let paintable_rows = self.arena().paintable_rows();
                (
                    paintable_rows.paintable_data(node).offset,
                    paintable_rows.prepare_paintable_row_recommit_notification(node),
                )
            };
            self.committed_offsets_before_recommit_reset.insert(node, offset);
            notification.invoke_callback();
        }
        let arena = self.arena_mut();
        if row_existed_before_this_commit {
            arena.paintable_rows_mut().begin_paintable_row_recommit(node);
        } else {
            arena.populate_paintable_row(node);
            if node_kind == NodeKind::Viewport {
                arena.paint_state().borrow_mut().reset_visual_context_state();
            }
            arena.note_visual_context_box_dirty(node, VisualContextBoxDirtyKind::NewRow);
        }
        PreparedPaintable {
            has_paintable_row: true,
            row_existed_before_this_commit,
        }
    }

    pub(crate) fn committed_navigable_container_viewport_shells(&self) -> Vec<*mut std::ffi::c_void> {
        self.committed_navigable_container_viewports
            .iter()
            .map(|node| self.callbacks.shell(*node))
            .collect()
    }

    pub(crate) fn replace_committed_fragment_link(
        &mut self,
        node: formatting_context::Node,
        link: &fragment_tree::FragmentLink,
        reuses_committed_subtree: bool,
        enclosing_line_root_content_changed: bool,
    ) -> ReplacedCommittedFragmentLink {
        let fragment = &link.fragment;
        let new_content_size = used_values::FfiCssPixelSize {
            width: fragment.content_inline_size,
            height: fragment.content_block_size,
        };
        let mut content_size_change = None;
        let (old_identity, old_content_size) = self.arena().with_committed_fragment_link(node, |old_link| {
            old_link.map_or((0, used_values::FfiCssPixelSize::default()), |old_link| {
                (
                    old_link.fragment.identity,
                    used_values::FfiCssPixelSize {
                        width: old_link.fragment.content_inline_size,
                        height: old_link.fragment.content_block_size,
                    },
                )
            })
        });
        if old_content_size != new_content_size {
            assert!(
                !reuses_committed_subtree,
                "a reused committed subtree changed its content size"
            );
            content_size_change = Some((old_content_size, new_content_size));
        }
        let committed_fragment_identity_changed = old_identity != fragment.identity;
        let painted_geometry_lives_in_enclosing_line_root =
            || node_facts::NodeFacts::new(self.callbacks, node).is_fragmented_inline();
        let painted_content_changed = committed_fragment_identity_changed
            || (enclosing_line_root_content_changed && painted_geometry_lives_in_enclosing_line_root());
        // A reused committed subtree's root counts as unchanged even though its run-root
        // fragment is rebuilt with a fresh identity at placement: the reuse contract guarantees
        // identical replayed output, enforced by the content-size assertion above.
        let content_unchanged = reuses_committed_subtree || (old_identity != 0 && !painted_content_changed);
        let offset_unchanged = self
            .committed_offsets_before_recommit_reset
            .get(&node)
            .is_some_and(|&offset_before_commit| offset_before_commit == link.committed_offset);
        if !(content_unchanged && offset_unchanged) {
            self.arena().paintable_rows().mark_paint_cache_self_dirty(node);
        }
        if !offset_unchanged {
            self.arena()
                .note_visual_context_box_dirty(node, VisualContextBoxDirtyKind::MovedWithDescendants);
        } else if !content_unchanged {
            self.arena()
                .note_visual_context_box_dirty(node, VisualContextBoxDirtyKind::RecommittedInPlace);
        }
        {
            let arena = self.arena_mut();
            let mut paintable_rows = arena.paintable_rows_mut();
            let data = paintable_rows.paintable_data_mut(node);
            if painted_content_changed {
                data.overflow_valid_across_recommits = false;
            }
            data.content_size = new_content_size;
            data.offset = link.committed_offset;
        }
        self.callbacks.set_committed_fragment_link(node, link.clone());
        ReplacedCommittedFragmentLink {
            content_size_change,
            committed_fragment_identity_changed,
        }
    }

    pub(crate) fn set_line_data(
        &self,
        slot: NodeSlotId,
        line_data: &used_values::LineData,
        content_inline_size: CssPixels,
    ) -> bool {
        if !node_painting::has_lines(self.arena(), slot) {
            return false;
        }
        let (lines, fragments, pieces) = self.build_line_records(line_data, content_inline_size);
        let has_pieces = !pieces.is_empty();
        let mut side = self.arena().paintable_side_data_mut(slot);
        side.lines = lines;
        side.fragments = fragments;
        side.inline_box_pieces = pieces;
        for piece in &side.inline_box_pieces {
            assert!(
                (piece.first_fragment_index + piece.fragment_count) as usize <= side.fragments.len(),
                "inline box piece fragment range exceeds the committed fragments"
            );
        }
        drop(side);
        if has_pieces {
            self.arena().note_line_root_needs_fragment_ownership(slot);
        }
        has_pieces
    }

    fn build_line_records(
        &self,
        data: &used_values::LineData,
        content_inline_size: CssPixels,
    ) -> (Vec<LineRecord>, Vec<FragmentRecord>, Vec<InlineBoxPieceRecord>) {
        let mut lines = Vec::with_capacity(data.line_boxes.len());
        let mut fragments = Vec::new();
        for line in &data.line_boxes {
            let committed_fragment_count = line.visible_fragments().count() as u32;
            lines.push(LineRecord {
                rect: inline_formatting_context::line_rect(line, content_inline_size),
                baseline: line.block_start + line.baseline,
                fragment_count: committed_fragment_count,
            });
            let line_index = (lines.len() - 1) as u32;
            for fragment in &line.fragments {
                if fragment.is_fully_truncated {
                    continue;
                }
                let (x, y) = fragment.offset();
                let (x, y) = (x + fragment.relpos_delta.x, y + fragment.relpos_delta.y);
                let (width, height) = fragment.size();
                let glyph_run = fragment.glyphs.as_ref().map(|glyph_data| GlyphRunRecord {
                    glyphs: glyph_data.glyphs.clone(),
                    // SAFETY: The layout pass borrowed the font from a live cascade list; retaining
                    // it here keeps it alive for as long as the fragment record.
                    font: unsafe { libgfx_rust::font::RetainedFont::retain(glyph_data.font) },
                });
                let (
                    dom_start_offset_in_node,
                    dom_end_offset_in_node,
                    dom_end_offset_with_trailing_whitespace,
                    trailing_whitespace_length_in_code_units,
                ) = self.fragment_dom_offsets(
                    fragment.layout_node,
                    fragment.start,
                    fragment.length_in_code_units,
                    fragment.has_trailing_whitespace,
                );
                fragments.push(FragmentRecord {
                    layout_node: fragment.layout_node,
                    style_source: fragment.style_source,
                    offset: used_values::FfiCssPixelPoint { x, y },
                    size: used_values::FfiCssPixelSize { width, height },
                    line_index,
                    start_offset: fragment.start,
                    length_in_code_units: fragment.length_in_code_units,
                    dom_start_offset_in_node,
                    dom_end_offset_in_node,
                    dom_end_offset_with_trailing_whitespace,
                    trailing_whitespace_length_in_code_units,
                    baseline: fragment.baseline,
                    accumulated_vertical_shift: fragment.accumulated_vertical_shift,
                    writing_mode: fragment.writing_mode,
                    is_block_ellipsis: fragment.is_block_ellipsis,
                    selection_state: 0,
                    glyph_run,
                });
            }
        }
        let pieces = data
            .inline_box_pieces
            .iter()
            .map(|piece| InlineBoxPieceRecord {
                node: piece.node,
                first_fragment_index: piece.first_fragment_index,
                fragment_count: piece.fragment_count,
                line_index: piece.line_index,
                border_box_rect: used_values::FfiCssPixelRect {
                    x: piece.border_box_rect.x + piece.relpos_delta.x,
                    y: piece.border_box_rect.y + piece.relpos_delta.y,
                    width: piece.border_box_rect.width,
                    height: piece.border_box_rect.height,
                },
                baseline: piece.baseline + piece.relpos_delta.y,
                accumulated_vertical_shift: piece.accumulated_vertical_shift,
                present_edges: piece.present_edges,
                is_geometry_only_placeholder: piece.is_geometry_only_placeholder,
            })
            .collect();
        (lines, fragments, pieces)
    }

    fn fragment_dom_offsets(
        &self,
        layout_node: NodeSlotId,
        start_offset: usize,
        length_in_code_units: usize,
        has_trailing_whitespace: bool,
    ) -> (usize, usize, usize, usize) {
        let data = self.callbacks.node_data(layout_node);
        if !node_facts::kind_is_text(data.kind.get()) {
            return (
                start_offset,
                start_offset + length_in_code_units,
                start_offset + length_in_code_units,
                0,
            );
        }
        let content = self.callbacks.text_content(layout_node);
        let mut trailing_whitespace_length = 0;
        if has_trailing_whitespace {
            let position = start_offset + length_in_code_units;
            while let Some(code_unit) = content.text.get(position + trailing_whitespace_length) {
                if *code_unit != u16::from(b' ') && *code_unit != u16::from(b'\t') {
                    break;
                }
                trailing_whitespace_length += 1;
            }
        }
        let arena = self.callbacks.arena();
        let dom_start_offset_in_node = arena.dom_offset_for_rendered_text_offset(
            layout_node,
            start_offset,
            crate::layout::RenderedTextBoundary::Start,
        );
        let dom_end_offset_in_node = if length_in_code_units == 0 {
            dom_start_offset_in_node
        } else {
            arena.dom_offset_for_rendered_text_offset(
                layout_node,
                start_offset + length_in_code_units,
                crate::layout::RenderedTextBoundary::End,
            )
        };
        let dom_end_offset_with_trailing_whitespace = if trailing_whitespace_length == 0 {
            dom_end_offset_in_node
        } else {
            arena.dom_offset_for_rendered_text_offset(
                layout_node,
                start_offset + length_in_code_units + trailing_whitespace_length,
                crate::layout::RenderedTextBoundary::End,
            )
        };
        (
            dom_start_offset_in_node,
            dom_end_offset_in_node,
            dom_end_offset_with_trailing_whitespace,
            trailing_whitespace_length,
        )
    }

    pub(crate) fn stamp_containing_block(&mut self, node: formatting_context::Node) {
        let containing_block = self.callbacks.node_data(node).containing_block.get();
        let arena = self.arena_mut();
        let mut paintable_rows = arena.paintable_rows_mut();
        if !paintable_rows.paintable_row_is_populated(node) {
            return;
        }
        let containing_block = if paintable_rows.paintable_row_is_populated(containing_block) {
            containing_block
        } else {
            NodeSlotId::INVALID
        };
        let data = paintable_rows.paintable_data_mut(node);
        let containing_block_changed = data.containing_block != containing_block;
        data.containing_block = containing_block;
        if containing_block_changed {
            paintable_rows.note_visual_context_box_dirty(node, VisualContextBoxDirtyKind::ContainingBlockChanged);
        }
    }

    pub(crate) fn assign_inline_box_geometry(&mut self, slot: NodeSlotId) {
        let arena = self.arena_mut();
        let mut paintable_rows = arena.paintable_rows_mut();
        let mut piece_indices_by_node: Vec<(NodeSlotId, Vec<u32>)> = Vec::new();
        for (piece_index, piece) in paintable_rows
            .paintable_side_data(slot)
            .inline_box_pieces
            .iter()
            .enumerate()
        {
            if piece.node.is_invalid() {
                continue;
            }
            match piece_indices_by_node.iter_mut().find(|(node, _)| *node == piece.node) {
                Some((_, indices)) => indices.push(piece_index as u32),
                None => piece_indices_by_node.push((piece.node, vec![piece_index as u32])),
            }
        }
        for (piece_node, piece_indices) in piece_indices_by_node {
            if !paintable_rows.paintable_row_is_populated(piece_node)
                || !node_painting::is_inline(&paintable_rows, piece_node)
            {
                continue;
            }
            let padding_widths = crate::painting::paintable_geometry::committed_padding(&paintable_rows, piece_node);
            let border_widths = crate::painting::paintable_geometry::committed_border(&paintable_rows, piece_node);
            let mut content_union: Option<CssPixelRect> = None;
            let mut padding_union: Option<CssPixelRect> = None;
            let mut border_union: Option<CssPixelRect> = None;
            let unite = |target: &mut Option<CssPixelRect>, rect: CssPixelRect| {
                let Some(current) = target else {
                    *target = Some(rect);
                    return;
                };
                if rect.is_empty() {
                    return;
                }
                if current.is_empty() {
                    *current = rect;
                } else {
                    current.unite(rect);
                }
            };
            for piece_index in &piece_indices {
                let piece = paintable_rows.paintable_side_data(slot).inline_box_pieces[*piece_index as usize];
                let border_rect = CssPixelRect::from(piece.border_box_rect);
                if piece.is_geometry_only_placeholder {
                    let content_rect = border_rect;
                    let padding_rect = content_rect.inflated(
                        padding_widths.top,
                        padding_widths.right,
                        padding_widths.bottom,
                        padding_widths.left,
                    );
                    let border_rect = padding_rect.inflated(
                        border_widths.top,
                        border_widths.right,
                        border_widths.bottom,
                        border_widths.left,
                    );
                    unite(&mut content_union, content_rect);
                    unite(&mut padding_union, padding_rect);
                    unite(&mut border_union, border_rect);
                    continue;
                }
                let padding_rect = piece.shrunken_by_present_edges(border_rect, border_widths);
                let content_rect = piece.shrunken_by_present_edges(padding_rect, padding_widths);
                unite(&mut content_union, content_rect);
                unite(&mut padding_union, padding_rect);
                unite(&mut border_union, border_rect);
            }
            let Some(content_union) = content_union else {
                continue;
            };
            let padding_union = padding_union.expect("padding union set alongside content union");
            let border_union = border_union.expect("border union set alongside content union");
            {
                let data = paintable_rows.paintable_data_mut(piece_node);
                let new_offset = content_union.location().into();
                let new_content_size = content_union.size().into();
                let new_padding_box_union = padding_union.translated(-content_union.x, -content_union.y).into();
                let new_border_box_union = border_union.translated(-content_union.x, -content_union.y).into();
                let inline_geometry_changed = data.offset != new_offset
                    || data.content_size != new_content_size
                    || data.local_padding_box_union != new_padding_box_union
                    || data.local_border_box_union != new_border_box_union;
                data.offset = new_offset;
                data.content_size = new_content_size;
                data.local_padding_box_union = new_padding_box_union;
                data.local_border_box_union = new_border_box_union;
                if inline_geometry_changed {
                    paintable_rows
                        .note_visual_context_box_dirty(piece_node, VisualContextBoxDirtyKind::InlineGeometryChanged);
                }
            }
            // This box has at most one piece per line, so its piece indices are ordered by line.
            paintable_rows.paintable_side_data_mut(piece_node).piece_indices = piece_indices;
        }
    }
}
