/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums;
use crate::css::css_pixels::CssPixelRect;
use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::layout::{
    FfiCssPixelPoint, FfiCssPixelRect, FfiCssPixelSize, FfiLayoutFcCallbacks, FragmentLink, LayoutNodeArena, LineData,
    Node, NodeFacts,
};
use crate::painting::paintable_data::*;
use crate::painting::paintable_rows::PaintableRowsRead;

#[derive(Clone, Copy, Debug)]
pub(crate) struct PreparedPaintable {
    pub(crate) has_paintable_row: bool,
    pub(crate) row_existed_before_this_commit: bool,
}

pub(crate) fn paintable_kind_for_node(facts: &NodeFacts<'_>, kind: NodeKind) -> PaintableKind {
    match kind {
        NodeKind::Viewport => PaintableKind::ViewportPaintable,
        NodeKind::BlockContainer
        | NodeKind::LegendBox
        | NodeKind::TableWrapper
        | NodeKind::TextAreaBox
        | NodeKind::TextInputBox
        | NodeKind::RangeInputBox
        | NodeKind::ListItemMarkerBox => PaintableKind::PaintableWithLines,
        NodeKind::ListItemBox => {
            if facts.is_fragmented_inline() {
                PaintableKind::InlinePaintable
            } else {
                PaintableKind::PaintableWithLines
            }
        }
        NodeKind::InlineNode => PaintableKind::InlinePaintable,
        NodeKind::Box | NodeKind::ReplacedBox | NodeKind::AudioBox | NodeKind::SVGBox => PaintableKind::Paintable,
        NodeKind::ImageBox => PaintableKind::ImagePaintable,
        NodeKind::CanvasBox => PaintableKind::CanvasPaintable,
        NodeKind::VideoBox => PaintableKind::VideoPaintable,
        NodeKind::CheckBox => PaintableKind::CheckBoxPaintable,
        NodeKind::RadioButton => PaintableKind::RadioButtonPaintable,
        NodeKind::FieldSetBox => PaintableKind::FieldSetPaintable,
        NodeKind::NavigableContainerViewport => PaintableKind::NavigableContainerViewportPaintable,
        NodeKind::SVGSVGBox => PaintableKind::SVGSVGPaintable,
        NodeKind::SVGGeometryBox | NodeKind::SVGTextBox | NodeKind::SVGTextPathBox => PaintableKind::SVGPathPaintable,
        NodeKind::SVGGraphicsBox => PaintableKind::SVGGraphicsPaintable,
        NodeKind::SVGImageBox => PaintableKind::SVGImagePaintable,
        NodeKind::SVGMaskBox => PaintableKind::SVGMaskPaintable,
        NodeKind::SVGClipBox => PaintableKind::SVGClipPaintable,
        NodeKind::SVGPatternBox => PaintableKind::SVGPatternPaintable,
        NodeKind::SVGForeignObjectBox => PaintableKind::SVGForeignObjectPaintable,
        NodeKind::Unset
        | NodeKind::BreakNode
        | NodeKind::GeneratedTextNode
        | NodeKind::Node
        | NodeKind::NodeWithStyle
        | NodeKind::TextNode
        | NodeKind::TextSliceNode => PaintableKind::None,
    }
}

fn committed_offset_delta(
    arena: &impl PaintableRowsRead,
    offsets_before_commit: &std::collections::HashMap<NodeSlotId, FfiCssPixelPoint>,
    slot: NodeSlotId,
) -> FfiCssPixelPoint {
    let Some(offset_before_commit) = offsets_before_commit.get(&slot) else {
        return FfiCssPixelPoint::default();
    };
    let offset = arena.paintable_data(slot).offset;
    FfiCssPixelPoint {
        x: offset.x - offset_before_commit.x,
        y: offset.y - offset_before_commit.y,
    }
}

fn reused_subtree_absolute_position_delta(
    arena: &impl PaintableRowsRead,
    offsets_before_commit: &std::collections::HashMap<NodeSlotId, FfiCssPixelPoint>,
    root: NodeSlotId,
) -> FfiCssPixelPoint {
    let mut delta = committed_offset_delta(arena, offsets_before_commit, root);
    if crate::painting::paintable_geometry::is_svg_paintable(arena.paintable_data(root).kind) {
        return delta;
    }
    let mut block = arena.paintable_data(root).containing_block;
    while !block.is_invalid() && arena.paintable_row_is_populated(block) {
        let block_data = arena.paintable_data(block);
        if block_data.kind == PaintableKind::SVGSVGPaintable
            || crate::painting::paintable_geometry::is_svg_paintable(block_data.kind)
        {
            break;
        }
        let block_delta = committed_offset_delta(arena, offsets_before_commit, block);
        delta.x += block_delta.x;
        delta.y += block_delta.y;
        if block_data.kind == PaintableKind::SVGForeignObjectPaintable {
            break;
        }
        block = block_data.containing_block;
    }
    delta
}

pub(crate) struct PaintableCommit<'a> {
    callbacks: &'a FfiLayoutFcCallbacks,
    offsets_before_commit: std::collections::HashMap<NodeSlotId, FfiCssPixelPoint>,
    reused_subtree_roots: Vec<NodeSlotId>,
    committed_navigable_container_viewports: Vec<NodeSlotId>,
}

impl<'a> PaintableCommit<'a> {
    pub(crate) fn new(callbacks: &'a FfiLayoutFcCallbacks) -> Self {
        Self {
            callbacks,
            offsets_before_commit: std::collections::HashMap::new(),
            reused_subtree_roots: Vec::new(),
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

    pub(crate) fn translate_reused_subtrees(&mut self) {
        let roots = std::mem::take(&mut self.reused_subtree_roots);
        if roots.is_empty() {
            return;
        }
        let offsets_before_commit = std::mem::take(&mut self.offsets_before_commit);
        let arena = self.arena_mut();
        let mut paintable_rows = arena.paintable_rows_mut();
        for root in roots {
            let delta = reused_subtree_absolute_position_delta(&paintable_rows, &offsets_before_commit, root);
            if delta == FfiCssPixelPoint::default() {
                continue;
            }
            let mut slots = Vec::new();
            crate::painting::paint_order::for_each_in_paint_subtree(&paintable_rows, root, |slot| {
                slots.push(slot);
            });
            for slot in slots {
                let data = paintable_rows.paintable_data_mut(slot);
                if data.has_overflow {
                    data.overflow.rect.x += delta.x;
                    data.overflow.rect.y += delta.y;
                }
                paintable_rows.invalidate_paint_cache(slot);
            }
        }
    }

    pub(crate) fn begin_commit(&self, root: Node) {
        let arena = self.arena();
        arena.clear_absolute_rect_memo();
        if self.callbacks.node_data(root).kind == NodeKind::Viewport {
            return;
        }
        arena.clear_descendant_subtree_caches_from_layout_node(root);
    }

    pub(crate) fn prepare_node(
        &mut self,
        node: Node,
        has_used_values: bool,
        reuses_committed_subtree: bool,
    ) -> PreparedPaintable {
        let (expected_kind, wants_paintable, node_kind, node_flags, is_inline) = {
            let facts = NodeFacts::new(self.callbacks, node);
            let data = self.callbacks.node_data(node);
            let expected_kind = paintable_kind_for_node(&facts, data.kind);
            (
                expected_kind,
                (has_used_values || (facts.is_fragmented_inline() && facts.has_dom_node()))
                    && expected_kind != PaintableKind::None,
                data.kind,
                data.flags,
                facts.is_inline(),
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
            self.offsets_before_commit.insert(node, offset);
            self.reused_subtree_roots.push(node);
            return PreparedPaintable {
                has_paintable_row: true,
                row_existed_before_this_commit: true,
            };
        }
        if !has_used_values {
            self.arena().clear_committed_fragment_link(node);
        }
        let style = self.callbacks.computed_values_view_if_styled(node);
        let (position, floating, has_z_index, display) = match style {
            Some(style) => {
                let box_values = style.box_values();
                (
                    box_values.position,
                    box_values.float_ != css_enums::float::NONE,
                    box_values.has_z_index,
                    style.display(),
                )
            }
            None => (
                css_enums::positioning::STATIC,
                false,
                false,
                crate::css::display::FfiDisplay::none(),
            ),
        };
        let is_item = node_flags & (NodeFlag::IsFlexItem as u32 | NodeFlag::IsGridItem as u32) != 0;
        if row_existed_before_this_commit {
            let (offset, notification) = {
                let paintable_rows = self.arena().paintable_rows();
                (
                    paintable_rows.paintable_data(node).offset,
                    paintable_rows.prepare_paintable_row_recommit_notification(node),
                )
            };
            self.offsets_before_commit.insert(node, offset);
            notification.invoke_callback();
        }
        let arena = self.arena_mut();
        if row_existed_before_this_commit {
            arena.paintable_rows_mut().begin_paintable_row_recommit(node);
        } else {
            arena.populate_paintable_row(node);
            if expected_kind == PaintableKind::ViewportPaintable {
                arena.paint_state().borrow_mut().reset_visual_context_state();
            }
            arena.paintable_rows_mut().paintable_data_mut(node).kind = expected_kind;
        }
        {
            let mut paintable_rows = arena.paintable_rows_mut();
            let paintable = paintable_rows.paintable_data_mut(node);
            // Flex and grid items with a z-index other than auto behave as if positioned.
            paintable.set_flag(
                PaintableFlag::Positioned,
                (is_item && has_z_index) || position != css_enums::positioning::STATIC,
            );
            paintable.set_flag(PaintableFlag::FixedPosition, position == css_enums::positioning::FIXED);
            paintable.set_flag(
                PaintableFlag::StickyPosition,
                position == css_enums::positioning::STICKY,
            );
            paintable.set_flag(
                PaintableFlag::AbsolutelyPositioned,
                position == css_enums::positioning::ABSOLUTE,
            );
            paintable.set_flag(
                PaintableFlag::Floating,
                floating && node_flags & NodeFlag::IsFlexItem as u32 == 0,
            );
            paintable.set_flag(PaintableFlag::Inline, is_inline);
            paintable.set_flag(PaintableFlag::Anonymous, node_flags & NodeFlag::Anonymous as u32 != 0);
            paintable.set_flag(
                PaintableFlag::Replaced,
                node_flags & NodeFlag::IsReplacedElement as u32 != 0,
            );
            paintable.set_flag(PaintableFlag::FlexOrGridItem, is_item);
            paintable.set_flag(
                PaintableFlag::ReplacedBox,
                crate::layout::kind_is_replaced_box(node_kind),
            );
            paintable.display = display.encoded();
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
        node: Node,
        link: &FragmentLink,
        reuses_committed_subtree: bool,
    ) -> Option<(FfiCssPixelSize, FfiCssPixelSize)> {
        let fragment = &link.fragment;
        let new_content_size = FfiCssPixelSize {
            width: fragment.content_inline_size,
            height: fragment.content_block_size,
        };
        let mut content_size_change = None;
        let (old_identity, old_content_size) = self.arena().with_committed_fragment_link(node, |old_link| {
            old_link.map_or((0, FfiCssPixelSize::default()), |old_link| {
                (
                    old_link.fragment.identity,
                    FfiCssPixelSize {
                        width: old_link.fragment.content_inline_size,
                        height: old_link.fragment.content_block_size,
                    },
                )
            })
        });
        let previous_content_size_for_diff = if reuses_committed_subtree {
            old_content_size
        } else {
            FfiCssPixelSize::default()
        };
        if previous_content_size_for_diff != new_content_size {
            assert!(
                !reuses_committed_subtree,
                "a reused committed subtree changed its content size"
            );
            content_size_change = Some((previous_content_size_for_diff, new_content_size));
        }
        {
            let arena = self.arena_mut();
            let mut paintable_rows = arena.paintable_rows_mut();
            let data = paintable_rows.paintable_data_mut(node);
            if old_identity != fragment.identity {
                data.cached_overflow = FfiOverflowData::default();
                data.has_cached_overflow = false;
            }
            data.content_size = new_content_size;
            data.offset = link.committed_offset;
        }
        self.callbacks.set_committed_fragment_link(node, link.clone());
        content_size_change
    }

    pub(crate) fn set_line_data(&self, slot: NodeSlotId, line_data: &LineData, content_inline_size: CssPixels) -> bool {
        if !self.arena().paintable_rows().paintable_data(slot).kind.has_lines() {
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
        has_pieces
    }

    fn build_line_records(
        &self,
        data: &LineData,
        content_inline_size: CssPixels,
    ) -> (Vec<LineRecord>, Vec<FragmentRecord>, Vec<InlineBoxPieceRecord>) {
        let mut lines = Vec::with_capacity(data.line_boxes.len());
        let mut fragments = Vec::new();
        for line in &data.line_boxes {
            let committed_fragment_count = line
                .fragments
                .iter()
                .filter(|fragment| !fragment.is_fully_truncated)
                .count() as u32;
            lines.push(LineRecord {
                rect: crate::layout::line_rect(line, content_inline_size),
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
                let glyph_run = fragment.glyphs.as_ref().map(|glyph_data| {
                    let text_type = libgfx_rust::text_layout::TextType::try_from(glyph_data.text_type)
                        .expect("committed glyph run carries a valid text type");
                    const _: () = assert!(
                        std::mem::size_of::<crate::layout::FfiDrawGlyph>()
                            == std::mem::size_of::<libgfx_rust::text_layout::DrawGlyph>()
                    );
                    const _: () = assert!(
                        std::mem::align_of::<crate::layout::FfiDrawGlyph>()
                            == std::mem::align_of::<libgfx_rust::text_layout::DrawGlyph>()
                    );
                    // SAFETY: FfiDrawGlyph mirrors libgfx's DrawGlyph layout (asserted above), and
                    // the glyph slice stays valid for the synchronous creation call.
                    let glyphs_for_gfx: &[libgfx_rust::text_layout::DrawGlyph] = unsafe {
                        std::slice::from_raw_parts(glyph_data.glyphs.as_ptr().cast(), glyph_data.glyphs.len())
                    };
                    // SAFETY: The layout pass borrowed the font from a live cascade list, which is
                    // live for the duration of this commit.
                    let retained = unsafe {
                        libgfx_rust::text_layout::create_glyph_run(
                            glyph_data.font,
                            glyphs_for_gfx,
                            text_type,
                            glyph_data.width,
                        )
                    };
                    GlyphRunRecord {
                        glyphs: glyph_data.glyphs.clone(),
                        // SAFETY: The layout pass borrowed the font from a live cascade list; retaining
                        // it here keeps it alive for as long as the fragment record.
                        font: unsafe { libgfx_rust::font::RetainedFont::retain(glyph_data.font) },
                        retained,
                    }
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
                    offset: FfiCssPixelPoint { x, y },
                    size: FfiCssPixelSize { width, height },
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
                border_box_rect: FfiCssPixelRect {
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
        if !crate::layout::kind_is_text(data.kind) {
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

    pub(crate) fn stamp_containing_block(&mut self, node: Node) {
        let containing_block = self.callbacks.node_data(node).containing_block;
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
        paintable_rows.paintable_data_mut(node).containing_block = containing_block;
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
                || paintable_rows.paintable_data(piece_node).kind != PaintableKind::InlinePaintable
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
                data.offset = content_union.location().into();
                data.content_size = content_union.size().into();
                data.local_padding_box_union = padding_union.translated(-content_union.x, -content_union.y).into();
                data.local_border_box_union = border_union.translated(-content_union.x, -content_union.y).into();
            }
            // This box has at most one piece per line, so its piece indices are ordered by line.
            paintable_rows.paintable_side_data_mut(piece_node).piece_indices = piece_indices;
        }
    }
}
