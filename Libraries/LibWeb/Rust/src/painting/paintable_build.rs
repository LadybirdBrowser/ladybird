/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::layout::{formatting_context, fragment_tree, node_facts, used_values};
use crate::painting::node_painting;
use crate::painting::visual_context::dirty::VisualContextBoxDirtyKind;

#[derive(Clone, Copy, Debug)]
pub(crate) struct PreparedPaintable {
    pub(crate) has_paintable_row: bool,
    pub(crate) row_existed_before_this_commit: bool,
    pub(crate) previous_offset: Option<used_values::FfiCssPixelPoint>,
}

pub(crate) struct ReplacedCommittedFragmentLink {
    pub(crate) content_size_change: Option<(used_values::FfiCssPixelSize, used_values::FfiCssPixelSize)>,
    pub(crate) committed_fragment_identity_changed: bool,
}

pub(crate) struct PaintableCommit<'a> {
    arena: &'a mut LayoutNodeArena,
    committed_navigable_container_viewports: Vec<NodeSlotId>,
    row_reset_notifications: Vec<crate::painting::paintable_rows::PaintableRowReset>,
}

impl<'a> PaintableCommit<'a> {
    pub(crate) fn new(arena: &'a mut LayoutNodeArena) -> Self {
        Self {
            arena,
            committed_navigable_container_viewports: Vec::new(),
            row_reset_notifications: Vec::new(),
        }
    }

    pub(crate) fn arena(&self) -> &LayoutNodeArena {
        self.arena
    }

    fn arena_mut(&mut self) -> &mut LayoutNodeArena {
        self.arena
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
            let data = self.arena().data(node);
            let is_fragmented_inline = node_facts::node_is_fragmented_inline(data, node_facts::node_style_view(data));
            let has_dom_node = !node_facts::has_flag(data, crate::layout::node_data::NodeFlag::Anonymous);
            (
                (has_used_values || (is_fragmented_inline && has_dom_node))
                    && node_painting::has_paintable(data.kind.get()),
                data.kind.get(),
            )
        };
        let row_existed_before_this_commit = self.arena().paintable_rows().paintable_row_is_populated(node);
        let previous_offset =
            row_existed_before_this_commit.then(|| self.arena().paintable_rows().paintable_data(node).offset);
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
                self.row_reset_notifications.push(reset);
                self.arena_mut().paintable_row_cleared(reset);
            }
            return PreparedPaintable {
                has_paintable_row: false,
                row_existed_before_this_commit: false,
                previous_offset: None,
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
            return PreparedPaintable {
                has_paintable_row: true,
                row_existed_before_this_commit: true,
                previous_offset,
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
            let notification = self
                .arena()
                .paintable_rows()
                .prepare_paintable_row_recommit_notification(node);
            self.row_reset_notifications.push(notification);
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
            previous_offset,
        }
    }

    pub(crate) fn take_row_reset_notifications(&mut self) -> Vec<crate::painting::paintable_rows::PaintableRowReset> {
        std::mem::take(&mut self.row_reset_notifications)
    }

    pub(crate) fn committed_navigable_container_viewport_shells(&self) -> Vec<*mut std::ffi::c_void> {
        self.committed_navigable_container_viewports
            .iter()
            .map(|node| self.arena().node_shell(*node))
            .collect()
    }

    pub(crate) fn replace_committed_fragment_link(
        &mut self,
        node: formatting_context::Node,
        link: &fragment_tree::FragmentLink,
        reuses_committed_subtree: bool,
        enclosing_line_root_content_changed: bool,
        previous_offset: Option<used_values::FfiCssPixelPoint>,
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
        let painted_geometry_lives_in_enclosing_line_root = || {
            let data = self.arena().data(node);
            node_facts::node_is_fragmented_inline(data, node_facts::node_style_view(data))
        };
        let painted_content_changed = committed_fragment_identity_changed
            || (enclosing_line_root_content_changed && painted_geometry_lives_in_enclosing_line_root());
        // A reused committed subtree's root counts as unchanged even though its run-root
        // fragment is rebuilt with a fresh identity at placement: the reuse contract guarantees
        // identical replayed output, enforced by the content-size assertion above.
        let content_unchanged = reuses_committed_subtree || (old_identity != 0 && !painted_content_changed);
        let offset_unchanged = previous_offset == Some(link.committed_offset);
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
        if painted_content_changed {
            self.arena().paintable_rows().clear_cached_overflow_data(node);
        }
        {
            let arena = self.arena_mut();
            let mut paintable_rows = arena.paintable_rows_mut();
            let data = paintable_rows.paintable_data_mut(node);
            data.content_size = new_content_size;
            data.offset = link.committed_offset;
        }
        self.arena()
            .set_committed_fragment_link(self.arena().data(node), link.clone());
        ReplacedCommittedFragmentLink {
            content_size_change,
            committed_fragment_identity_changed,
        }
    }

    pub(crate) fn set_line_data(
        &self,
        slot: NodeSlotId,
        line_data: &std::rc::Rc<crate::layout::inline_content::InlineContent>,
    ) -> bool {
        if !node_painting::has_lines(self.arena(), slot) {
            return false;
        }
        let has_pieces = !line_data.inline_box_pieces.is_empty();
        let mut side = self.arena().paintable_side_data_mut(slot);
        side.inline_content = Some(line_data.clone());
        drop(side);
        if has_pieces {
            self.arena().note_line_root_needs_fragment_ownership(slot);
        }
        has_pieces
    }

    pub(crate) fn stamp_containing_block(&mut self, node: formatting_context::Node) {
        let containing_block = self.arena().data(node).containing_block.get();
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
            .inline_box_pieces()
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
                let piece = paintable_rows.paintable_side_data(slot).inline_box_pieces()[*piece_index as usize];
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
