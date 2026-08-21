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
    FfiCommittedBoxMetrics, FfiCssPixelPoint, FfiCssPixelRect, FfiCssPixelSize, FfiLayoutFcCallbacks, LineData, Node,
    NodeFacts,
};
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::*;
use std::cell::RefCell;

#[derive(Clone, Copy, Debug)]
pub(crate) struct PreparedPaintable {
    pub(crate) slot: PaintableSlotId,
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
    arena: &PaintableArena,
    offsets_before_commit: &std::collections::HashMap<PaintableSlotId, FfiCssPixelPoint>,
    slot: PaintableSlotId,
) -> FfiCssPixelPoint {
    let Some(offset_before_commit) = offsets_before_commit.get(&slot) else {
        return FfiCssPixelPoint::default();
    };
    let offset = arena.data_ref(slot).offset;
    FfiCssPixelPoint {
        x: offset.x - offset_before_commit.x,
        y: offset.y - offset_before_commit.y,
    }
}

fn reused_subtree_absolute_position_delta(
    arena: &PaintableArena,
    offsets_before_commit: &std::collections::HashMap<PaintableSlotId, FfiCssPixelPoint>,
    root: PaintableSlotId,
) -> FfiCssPixelPoint {
    let mut delta = committed_offset_delta(arena, offsets_before_commit, root);
    if crate::painting::paintable_geometry::is_svg_paintable(arena.data_ref(root).kind) {
        return delta;
    }
    let mut block = arena.data_ref(root).containing_block;
    while !block.is_invalid() && arena.is_live(block) {
        let block_data = arena.data_ref(block);
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
    arena: &'a RefCell<PaintableArena>,
    offsets_before_commit: RefCell<std::collections::HashMap<PaintableSlotId, FfiCssPixelPoint>>,
    reused_subtree_roots: RefCell<Vec<PaintableSlotId>>,
    committed_navigable_container_viewports: RefCell<Vec<NodeSlotId>>,
}

impl<'a> PaintableCommit<'a> {
    pub(crate) fn new(callbacks: &'a FfiLayoutFcCallbacks) -> Self {
        Self {
            callbacks,
            arena: callbacks.arena().paintables(),
            offsets_before_commit: RefCell::new(std::collections::HashMap::new()),
            reused_subtree_roots: RefCell::new(Vec::new()),
            committed_navigable_container_viewports: RefCell::new(Vec::new()),
        }
    }

    pub(crate) fn discard_absolute_rects_memoized_during_commit(&self) {
        self.arena.borrow().clear_absolute_rect_memo();
    }

    pub(crate) fn translate_reused_subtrees(&self) {
        let roots = self.reused_subtree_roots.borrow();
        if roots.is_empty() {
            return;
        }
        let arena = self.arena.borrow();
        let layout_arena = self.callbacks.arena();
        let offsets_before_commit = self.offsets_before_commit.borrow();
        for &root in roots.iter() {
            let delta = reused_subtree_absolute_position_delta(&arena, &offsets_before_commit, root);
            if delta == FfiCssPixelPoint::default() {
                continue;
            }
            crate::painting::paint_order::for_each_in_paint_subtree(layout_arena, &arena, root, |slot| {
                arena.update_data(slot, |data| {
                    if data.has_overflow {
                        data.overflow.rect.x += delta.x;
                        data.overflow.rect.y += delta.y;
                    }
                });
                arena.invalidate_paint_cache(layout_arena, slot);
            });
        }
    }

    pub(crate) fn begin_commit(&self, root: Node) {
        let arena = self.arena.borrow();
        arena.clear_absolute_rect_memo();
        if self.callbacks.node_data(root).kind == NodeKind::Viewport {
            return;
        }
        arena.clear_descendant_subtree_caches_from_layout_node(self.callbacks.arena(), root);
    }

    pub(crate) fn prepare_node(
        &self,
        node: Node,
        has_used_values: bool,
        reuses_committed_subtree: bool,
    ) -> PreparedPaintable {
        let facts = NodeFacts::new(self.callbacks, node);
        let data = self.callbacks.node_data(node);
        let expected_kind = paintable_kind_for_node(&facts, data.kind);
        let wants_paintable = (has_used_values || (facts.is_fragmented_inline() && facts.has_dom_node()))
            && expected_kind != PaintableKind::None;
        let existing_slot = self.arena.borrow().paintable_of_node(node);
        if !wants_paintable {
            if !existing_slot.is_invalid() {
                let reset = {
                    let arena = self.arena.borrow();
                    arena.invalidate_paint_cache(self.callbacks.arena(), existing_slot);
                    arena
                        .prepare_node_cleared_reset(node, existing_slot)
                        .expect("live row for node could not be cleared")
                };
                reset.invoke_callback();
                self.arena.borrow_mut().node_cleared(self.callbacks.arena(), reset);
            }
            return PreparedPaintable {
                slot: PaintableSlotId::INVALID,
                row_existed_before_this_commit: false,
            };
        }
        if data.kind == NodeKind::NavigableContainerViewport {
            self.committed_navigable_container_viewports.borrow_mut().push(node);
        }
        if reuses_committed_subtree {
            assert!(!existing_slot.is_invalid(), "a kept subtree root has no committed row");
            let arena = self.arena.borrow();
            debug_assert_eq!(
                arena.paintable_of_node(node),
                existing_slot,
                "reused subtree root is not the node's own row"
            );
            self.offsets_before_commit
                .borrow_mut()
                .insert(existing_slot, arena.data_ref(existing_slot).offset);
            self.reused_subtree_roots.borrow_mut().push(existing_slot);
            return PreparedPaintable {
                slot: existing_slot,
                row_existed_before_this_commit: true,
            };
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
        let is_item = data.flags & (NodeFlag::IsFlexItem as u32 | NodeFlag::IsGridItem as u32) != 0;
        let row_existed_before_this_commit = !existing_slot.is_invalid();
        let slot = if row_existed_before_this_commit {
            let arena = self.arena.borrow();
            debug_assert_eq!(
                arena.paintable_of_node(node),
                existing_slot,
                "reused row is not the node's own"
            );
            self.offsets_before_commit
                .borrow_mut()
                .insert(existing_slot, arena.data_ref(existing_slot).offset);
            let reset = arena.prepare_reset_for_relayout(existing_slot);
            drop(arena);
            reset.invoke_callback();
            self.arena.borrow_mut().reset_for_relayout(reset);
            existing_slot
        } else {
            let mut arena = self.arena.borrow_mut();
            let slot = arena.row_for_node(node);
            if expected_kind == PaintableKind::ViewportPaintable {
                arena.reset_visual_context_state();
            }
            drop(arena);
            let arena = self.arena.borrow();
            arena.update_data(slot, |paintable| {
                paintable.layout_node = node;
                paintable.kind = expected_kind;
            });
            slot
        };
        let arena = self.arena.borrow();
        arena.update_data(slot, |paintable| {
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
                floating && data.flags & NodeFlag::IsFlexItem as u32 == 0,
            );
            paintable.set_flag(PaintableFlag::Inline, facts.is_inline());
            paintable.set_flag(PaintableFlag::Anonymous, data.flags & NodeFlag::Anonymous as u32 != 0);
            paintable.set_flag(
                PaintableFlag::Replaced,
                data.flags & NodeFlag::IsReplacedElement as u32 != 0,
            );
            paintable.set_flag(PaintableFlag::FlexOrGridItem, is_item);
            paintable.set_flag(
                PaintableFlag::ReplacedBox,
                crate::layout::kind_is_replaced_box(data.kind),
            );
            paintable.set_flag(PaintableFlag::PreparedByCommit, true);
            paintable.display = display.encoded();
        });
        PreparedPaintable {
            slot,
            row_existed_before_this_commit,
        }
    }

    pub(crate) fn committed_navigable_container_viewport_shells(&self) -> Vec<*mut std::ffi::c_void> {
        self.committed_navigable_container_viewports
            .borrow()
            .iter()
            .map(|node| self.callbacks.shell(*node))
            .collect()
    }

    pub(crate) fn set_box_metrics(
        &self,
        slot: PaintableSlotId,
        metrics: &FfiCommittedBoxMetrics,
    ) -> Option<(FfiCssPixelSize, FfiCssPixelSize)> {
        let arena = self.arena.borrow();
        let mut content_size_change = None;
        arena.update_data(slot, |data| {
            if data.layout_fragment_identity != metrics.fragment_identity {
                data.layout_fragment_identity = metrics.fragment_identity;
                data.cached_overflow = FfiOverflowData::default();
                data.has_cached_overflow = false;
            }
            data.inset = FfiPixelBox {
                top: metrics.inset_top,
                right: metrics.inset_right,
                bottom: metrics.inset_bottom,
                left: metrics.inset_left,
            };
            data.padding = FfiPixelBox {
                top: metrics.padding_top,
                right: metrics.padding_right,
                bottom: metrics.padding_bottom,
                left: metrics.padding_left,
            };
            data.border = FfiPixelBox {
                top: metrics.border_top,
                right: metrics.border_right,
                bottom: metrics.border_bottom,
                left: metrics.border_left,
            };
            data.margin = FfiPixelBox {
                top: metrics.margin_top,
                right: metrics.margin_right,
                bottom: metrics.margin_bottom,
                left: metrics.margin_left,
            };
            let new_content_size = FfiCssPixelSize {
                width: metrics.content_inline_size,
                height: metrics.content_block_size,
            };
            if data.content_size != new_content_size {
                assert!(
                    !metrics.reuses_committed_subtree,
                    "a reused committed subtree changed its content size"
                );
                content_size_change = Some((data.content_size, new_content_size));
            }
            data.content_size = new_content_size;
            data.offset = metrics.content_offset;
            if metrics.has_containing_line_box_index {
                data.containing_line_box_index = metrics.containing_line_box_index;
                data.has_containing_line_box_index = true;
            }
            data.uses_collapsing_borders_model = metrics.uses_collapsing_borders_model;
        });
        content_size_change
    }

    pub(crate) fn set_line_data(
        &self,
        slot: PaintableSlotId,
        line_data: &LineData,
        content_inline_size: CssPixels,
    ) -> bool {
        let lines_and_fragments = {
            let arena = self.arena.borrow();
            if !arena.data_ref(slot).kind.has_lines() {
                return false;
            }
            self.build_line_records(line_data, content_inline_size)
        };
        let (lines, fragments, pieces) = lines_and_fragments;
        let has_pieces = !pieces.is_empty();
        let mut arena = self.arena.borrow_mut();
        let side = arena.side_mut(slot);
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

    pub(crate) fn set_svg_viewport_transform(
        &self,
        slot: PaintableSlotId,
        transform: crate::layout::FfiAffineTransform,
    ) {
        let arena = self.arena.borrow();
        arena.update_data(slot, |data| {
            data.svg_viewport_transform = transform;
            data.has_svg_viewport_transform = true;
        });
    }

    pub(crate) fn set_svg_viewport_size(&self, slot: PaintableSlotId, size: FfiCssPixelSize) {
        let arena = self.arena.borrow();
        arena.update_data(slot, |data| {
            if data.kind == PaintableKind::SVGSVGPaintable {
                data.svg_viewport_size = size;
            }
        });
    }

    pub(crate) fn set_computed_svg_path(
        &self,
        slot: PaintableSlotId,
        path: &std::rc::Rc<libgfx_rust::path::OwnedPath>,
    ) {
        let mut arena = self.arena.borrow_mut();
        if arena.data_ref(slot).kind != PaintableKind::SVGPathPaintable {
            return;
        }
        let side = arena.side_mut(slot);
        if path.identity() != 0
            && path.identity() == side.computed_svg_path_identity
            && side.computed_svg_path.is_some()
        {
            return;
        }
        side.computed_svg_path = Some(path.clone());
        side.computed_svg_path_identity = path.identity();
    }

    pub(crate) fn set_grid_layout_data(
        &self,
        slot: PaintableSlotId,
        data: &std::rc::Rc<crate::layout::GridLayoutData>,
    ) {
        self.arena.borrow_mut().side_mut(slot).grid_layout_data = Some(data.clone());
    }

    pub(crate) fn set_flex_layout_data(
        &self,
        slot: PaintableSlotId,
        data: &std::rc::Rc<crate::layout::FlexLayoutData>,
    ) {
        self.arena.borrow_mut().side_mut(slot).flex_layout_data = Some(data.clone());
    }

    pub(crate) fn set_used_grid_tracks(
        &self,
        slot: PaintableSlotId,
        tracks: &std::rc::Rc<crate::layout::OwnedUsedGridTracks>,
    ) {
        self.arena.borrow_mut().side_mut(slot).used_grid_tracks = Some(tracks.clone());
    }

    pub(crate) fn set_collapsed_table_borders(
        &self,
        slot: PaintableSlotId,
        borders: &std::rc::Rc<crate::layout::OwnedCollapsedTableBorders>,
    ) {
        self.arena.borrow_mut().side_mut(slot).collapsed_table_borders = Some(borders.clone());
    }

    pub(crate) fn stamp_containing_block(&self, node: Node, slot: PaintableSlotId) {
        let arena = self.arena.borrow();
        if slot.is_invalid() {
            return;
        }
        let containing_block = self.callbacks.node_data(node).containing_block;
        let containing_block = if containing_block.is_invalid() {
            PaintableSlotId::INVALID
        } else {
            arena.paintable_of_node(containing_block)
        };
        arena.update_data(slot, |data| data.containing_block = containing_block);
    }

    pub(crate) fn assign_inline_box_geometry(&self, slot: PaintableSlotId) {
        let mut arena = self.arena.borrow_mut();
        let mut piece_indices_by_node: Vec<(NodeSlotId, Vec<u32>)> = Vec::new();
        for (piece_index, piece) in arena.side(slot).inline_box_pieces.iter().enumerate() {
            if piece.node.is_invalid() {
                continue;
            }
            match piece_indices_by_node.iter_mut().find(|(node, _)| *node == piece.node) {
                Some((_, indices)) => indices.push(piece_index as u32),
                None => piece_indices_by_node.push((piece.node, vec![piece_index as u32])),
            }
        }
        for (piece_node, piece_indices) in piece_indices_by_node {
            let inline_paintable = arena.paintable_of_node(piece_node);
            if inline_paintable.is_invalid() || arena.data_ref(inline_paintable).kind != PaintableKind::InlinePaintable
            {
                continue;
            }
            let (padding_widths, border_widths) = {
                let data = arena.data_ref(inline_paintable);
                (data.padding, data.border)
            };
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
                let piece = arena.side(slot).inline_box_pieces[*piece_index as usize];
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
            arena.update_data(inline_paintable, |data| {
                data.offset = content_union.location().into();
                data.content_size = content_union.size().into();
                data.local_padding_box_union = padding_union.translated(-content_union.x, -content_union.y).into();
                data.local_border_box_union = border_union.translated(-content_union.x, -content_union.y).into();
            });
            // This box has at most one piece per line, so its piece indices are ordered by line.
            arena.side_mut(inline_paintable).piece_indices = piece_indices;
        }
    }
}
