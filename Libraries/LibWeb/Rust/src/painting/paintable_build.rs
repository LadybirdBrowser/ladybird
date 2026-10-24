/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{DomPaintFact, NodeKind, NodeSlotId};
use crate::layout::{formatting_context, fragment_tree, node_facts, used_values};
use crate::painting::node_painting;
use crate::painting::paint_read::PaintRead;
use crate::painting::record::damage::PaintDamage;
use crate::painting::visual_context::dirty::VisualContextBoxDirtyKind;

#[derive(Clone, Copy, Debug)]
pub(crate) struct PreparedPaintable {
    pub(crate) has_paintable_row: bool,
    pub(crate) row_existed_before_this_commit: bool,
    pub(crate) previous_offset: Option<used_values::FfiCssPixelPoint>,
}

pub(crate) struct ReplacedCommittedFragmentLink {
    pub(crate) content_size_change: Option<(used_values::FfiCssPixelSize, used_values::FfiCssPixelSize)>,
    pub(crate) line_root_changes: LineRootChanges,
}

#[derive(Clone, Copy, Default)]
pub(crate) struct LineDataChange {
    pub(crate) has_pieces: bool,
    pub(crate) content_changed: bool,
    pub(crate) item_order_changed: bool,
}

#[derive(Clone, Copy, Default)]
pub(crate) struct LineRootChanges {
    pub(crate) fragment_changed: bool,
    pub(crate) inline_content_changed: bool,
    pub(crate) inline_item_order_changed: bool,
}

fn same_inline_content(left: &fragment_tree::Fragment, right: &fragment_tree::Fragment) -> bool {
    match (&left.line_data, &right.line_data) {
        (None, None) => true,
        (Some(left), Some(right)) => std::sync::Arc::ptr_eq(left, right) || left == right,
        _ => false,
    }
}

fn has_descendant_dependent_paint(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    let kind = arena.data(node).kind.get();
    if node_painting::is_svg(kind)
        || matches!(
            kind,
            NodeKind::FieldSetBox | NodeKind::SVGSVGBox | NodeKind::SVGForeignObjectBox
        )
    {
        return true;
    }
    let Some(style) = arena.node_style_if_live(node) else {
        return true;
    };
    let display = style.display();
    if display.is_table_inside() || display.is_internal_table() || kind == NodeKind::TableWrapper {
        return true;
    }
    // A text-clipped background can collect glyphs from multiple descendant line roots.
    fn clips_text(value: &crate::css::style_value::StyleValueData) -> bool {
        use crate::css::{css_enums, style_value::StyleValueData};
        match value {
            StyleValueData::Keyword { keyword } => {
                css_enums::keyword_to_background_box(*keyword) == Some(css_enums::background_box::TEXT)
            }
            StyleValueData::ValueList { values, .. } => values.as_slice().iter().any(|value| clips_text(value.data())),
            _ => false,
        }
    }
    crate::painting::style_queries::handle_value(&style.background().background_clip).is_some_and(clips_text)
}

pub(crate) struct PaintableCommit<'a> {
    arena: &'a mut LayoutNodeArena,
    is_full_layout: bool,
    committed_navigable_container_viewports: Vec<NodeSlotId>,
    row_reset_notifications: Vec<crate::painting::paintable_rows::PaintableRowReset>,
    overflow_invalidated_boxes: crate::fast_hash::FastSet<NodeSlotId>,
}

impl<'a> PaintableCommit<'a> {
    pub(crate) fn new(arena: &'a mut LayoutNodeArena, root: NodeSlotId) -> Self {
        let is_full_layout = arena.data(root).kind.get() == NodeKind::Viewport;
        Self {
            arena,
            is_full_layout,
            committed_navigable_container_viewports: Vec::new(),
            row_reset_notifications: Vec::new(),
            overflow_invalidated_boxes: Default::default(),
        }
    }

    pub(crate) fn arena(&self) -> &LayoutNodeArena {
        self.arena
    }

    pub(crate) fn arena_mut(&mut self) -> &mut LayoutNodeArena {
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
        enclosing_line_root_changes: LineRootChanges,
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
        let previous_offset = row_existed_before_this_commit.then(|| self.arena().live_paintable_data(node).offset);
        if !wants_paintable {
            self.arena().clear_committed_fragment_link(node);
            if row_existed_before_this_commit {
                let reset = {
                    let arena = self.arena();
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
            // Fragmented inlines get their painted pieces from the enclosing line root.
            let inline_paint_changed = enclosing_line_root_changes.inline_content_changed
                || (enclosing_line_root_changes.fragment_changed && has_descendant_dependent_paint(self.arena(), node));
            if row_existed_before_this_commit && inline_paint_changed {
                self.arena().push_paint_damage(node, PaintDamage::ALL_PRODUCERS);
            }
            if row_existed_before_this_commit && enclosing_line_root_changes.fragment_changed {
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
            if node_kind == NodeKind::Viewport {
                // A recommitted viewport row starts its paint state over: the scroll registry is
                // dropped here and rebuilt by the next tree update, while the host resets its
                // snapshot from the row reset notification.
                arena.paint_state().borrow_mut().visual_context.clear_scroll_state();
            }
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

    pub(crate) fn committed_navigable_container_viewports(&self) -> &[NodeSlotId] {
        &self.committed_navigable_container_viewports
    }

    pub(crate) fn replace_committed_fragment_link(
        &mut self,
        node: formatting_context::Node,
        link: &fragment_tree::FragmentLink,
        reuses_committed_subtree: bool,
        enclosing_line_root_changes: LineRootChanges,
        previous_offset: Option<used_values::FfiCssPixelPoint>,
    ) -> ReplacedCommittedFragmentLink {
        let fragment = &link.fragment;
        let frame = link.frame;
        let new_content_size = frame.size_to_physical(fragment.content_inline_size, fragment.content_block_size);
        let new_offset: used_values::FfiCssPixelPoint = if frame.is_physical() {
            link.committed_offset
        } else {
            crate::painting::paintable_geometry::physical_content_rect_in_containing_block(
                self.arena(),
                frame,
                self.committed_containing_block(node, Some(link)),
                CssPixelRect::new(
                    link.committed_offset.x,
                    link.committed_offset.y,
                    fragment.content_inline_size,
                    fragment.content_block_size,
                ),
            )
            .location()
            .into()
        };
        let mut content_size_change = None;
        let mut own_paint_unchanged = false;
        let mut root_box_unchanged = false;
        let mut child_placements_unchanged = false;
        let mut inline_content_unchanged = false;
        let mut inline_item_order_unchanged = false;
        let mut child_sequence_unchanged = false;
        let (old_identity, old_content_size, old_content_inline_size) =
            self.arena().with_committed_fragment_link(node, |old_link| {
                old_link.map_or((0, used_values::FfiCssPixelSize::default(), None), |old_link| {
                    let previous = &old_link.fragment;
                    if reuses_committed_subtree || previous.identity == fragment.identity {
                        root_box_unchanged =
                            std::sync::Arc::ptr_eq(fragment, previous) || fragment.has_same_box_properties(previous);
                        own_paint_unchanged = root_box_unchanged;
                        child_placements_unchanged = true;
                        inline_content_unchanged = true;
                        inline_item_order_unchanged = true;
                        child_sequence_unchanged = true;
                    } else {
                        inline_content_unchanged = same_inline_content(fragment, previous);
                        inline_item_order_unchanged = inline_content_unchanged
                            || match (&fragment.line_data, &previous.line_data) {
                                (Some(left), Some(right)) => left.has_same_item_order(right),
                                _ => false,
                            };
                        own_paint_unchanged = inline_content_unchanged
                            && fragment.has_same_box_properties(previous)
                            && !has_descendant_dependent_paint(self.arena(), node);
                        child_placements_unchanged = fragment.has_same_child_placements(previous);
                        child_sequence_unchanged = fragment.has_same_child_sequence(previous);
                    }
                    own_paint_unchanged &= old_link.has_same_placement(link);
                    (
                        old_link.fragment.identity,
                        frame.size_to_physical(
                            old_link.fragment.content_inline_size,
                            old_link.fragment.content_block_size,
                        ),
                        Some(old_link.fragment.content_inline_size),
                    )
                })
            });
        if old_content_size != new_content_size {
            if self.arena().data(node).kind.get() == NodeKind::Viewport {
                self.arena().set_needs_full_scrollable_overflow_recalculation();
            }
            assert!(
                !reuses_committed_subtree || old_content_inline_size == Some(fragment.content_inline_size),
                "a reused committed subtree changed its content inline size"
            );
            content_size_change = Some((old_content_size, new_content_size));
        }
        let committed_fragment_identity_changed = old_identity != fragment.identity;
        // Inserted, removed or reordered children change which scopes this row's plans list,
        // independently of where the children were placed.
        if !child_sequence_unchanged {
            self.arena().push_paint_damage(node, PaintDamage::ORDER);
        }
        let painted_geometry_lives_in_enclosing_line_root = {
            let data = self.arena().data(node);
            node_facts::node_is_fragmented_inline(data, node_facts::node_style_view(data))
        };
        let fragment_content_changed = committed_fragment_identity_changed
            || (enclosing_line_root_changes.fragment_changed && painted_geometry_lives_in_enclosing_line_root);
        // Keep fragment identity as the conservative signal for overflow and visual contexts.
        // A reused run root can have a new identity while its box and subtree are unchanged.
        let fragment_content_unchanged =
            (reuses_committed_subtree && root_box_unchanged) || (old_identity != 0 && !fragment_content_changed);
        let offset_unchanged = previous_offset == Some(new_offset);
        let enclosing_inline_paint_changed = painted_geometry_lives_in_enclosing_line_root
            && (enclosing_line_root_changes.inline_content_changed
                || (enclosing_line_root_changes.fragment_changed
                    && has_descendant_dependent_paint(self.arena(), node)));
        // Fragmented inline offsets are finalized from the line root's pieces after this
        // commit. Comparing that final offset with the fragment's temporary one would dirty
        // identical inlines on every relayout; their line content and box properties suffice.
        let paint_offset_unchanged = offset_unchanged || painted_geometry_lives_in_enclosing_line_root;
        // record_lines_hit_test_items() emits an EmptyEditable target only without paint
        // children. An out-of-flow child can change that eligibility without changing the
        // editor's box properties or inline content, so descendant dirtiness alone is insufficient.
        let empty_editable_children_changed = !child_placements_unchanged
            && node_painting::has_lines(self.arena(), node)
            && self
                .arena()
                .node_has_dom_paint_fact(node, DomPaintFact::EditableOrEditingHost)
            && fragment
                .line_data
                .as_ref()
                .is_none_or(|content| content.fragments.is_empty());
        let mut damage = PaintDamage::NONE;
        if !own_paint_unchanged || enclosing_inline_paint_changed || empty_editable_children_changed {
            damage |= PaintDamage::ALL_PRODUCERS;
        }
        if !inline_item_order_unchanged
            || (painted_geometry_lives_in_enclosing_line_root && enclosing_line_root_changes.inline_item_order_changed)
        {
            damage |= PaintDamage::ORDER;
        }
        if !paint_offset_unchanged {
            damage |= PaintDamage::MOVED;
        }
        self.arena().push_paint_damage(node, damage);
        if !offset_unchanged {
            self.arena()
                .note_visual_context_box_dirty(node, VisualContextBoxDirtyKind::MovedWithDescendants);
        }
        if !fragment_content_unchanged {
            self.arena()
                .note_visual_context_box_dirty(node, VisualContextBoxDirtyKind::RecommittedInPlace);
        }
        if !fragment_content_unchanged || !self.arena().committed_side_data(node).overflow_valid_across_recommits {
            self.schedule_scrollable_overflow_recalculation(node);
        } else if !offset_unchanged {
            // NB: Moving an unchanged subtree preserves its overflow relative to its padding
            //     box. Only its contribution to containing blocks needs to be measured again.
            let containing_block = self.committed_containing_block(node, Some(link));
            self.schedule_scrollable_overflow_recalculation(containing_block);
        }
        {
            let arena = self.arena_mut();
            let mut paintable_rows = arena.paintable_rows_mut();
            let mut data = paintable_rows.paintable_data_mut(node);
            data.set_frame(frame);
            data.content_size = new_content_size;
            data.offset = new_offset;
        }
        self.arena().set_committed_fragment_link(
            node,
            link.clone(),
            self.arena()
                .epoch_of_geometry_laid_out_in_this_pass(self.arena().data(node)),
        );
        ReplacedCommittedFragmentLink {
            content_size_change,
            line_root_changes: LineRootChanges {
                fragment_changed: committed_fragment_identity_changed,
                inline_content_changed: !inline_content_unchanged,
                inline_item_order_changed: !inline_item_order_unchanged,
            },
        }
    }

    pub(crate) fn set_box_fragments(&mut self, slot: NodeSlotId, fragments: &[fragment_tree::SlotFragment<'_>]) {
        let later_fragments = fragments.get(1..).unwrap_or_default();
        let (moved, containing_block_fragment, identical, geometry_unchanged) = {
            let side = self.arena().committed_side_data(slot);
            let containing_block_fragment = fragments
                .first()
                .and_then(|fragment| fragment.containing_block_fragment)
                .unwrap_or(side.containing_block_fragment);
            let first_fragment_moved = side.containing_block_fragment != containing_block_fragment;
            let same_count = !first_fragment_moved && side.box_fragments().len() == later_fragments.len();
            let all_pairs = |same: fn(&fragment_tree::FragmentLink, &fragment_tree::FragmentLink) -> bool| {
                side.box_fragments()
                    .iter()
                    .zip(later_fragments)
                    .all(|(committed, fragment)| {
                        Some(committed.containing_block_fragment) == fragment.containing_block_fragment
                            && same(fragment.link, &committed.link)
                    })
            };
            (
                first_fragment_moved || !all_pairs(|link, committed| link.has_same_placement(committed)),
                containing_block_fragment,
                same_count && all_pairs(|link, committed| link.places_same_fragment_identically_to(committed)),
                same_count
                    && all_pairs(|link, committed| {
                        link.has_same_placement(committed)
                            && link.fragment.has_same_box_properties(&committed.fragment)
                            && same_inline_content(&link.fragment, &committed.fragment)
                    }),
            )
        };
        let flags = [
            (
                crate::painting::paintable_data::PaintableFlag::InLaterContainingBlockFragment,
                containing_block_fragment != 0,
            ),
            (
                crate::painting::paintable_data::PaintableFlag::HasLaterBoxFragments,
                !later_fragments.is_empty(),
            ),
        ];
        let data = self.arena().live_paintable_data(slot);
        if flags.iter().any(|&(flag, value)| data.has_flag(flag) != value) {
            let arena = self.arena_mut();
            let mut paintable_rows = arena.paintable_rows_mut();
            let mut data = paintable_rows.paintable_data_mut(slot);
            for (flag, value) in flags {
                data.set_flag(flag, value);
            }
        }
        if identical {
            return;
        }
        {
            let mut side = self.arena().committed_side_data_mut(slot);
            side.containing_block_fragment = containing_block_fragment;
            side.box_fragments = (!later_fragments.is_empty()).then(|| {
                std::sync::Arc::new(
                    later_fragments
                        .iter()
                        .map(|fragment| crate::painting::paintable_data::CommittedBoxFragment {
                            link: fragment.link.clone(),
                            containing_block_fragment: fragment.containing_block_fragment.unwrap_or_default(),
                        })
                        .collect(),
                )
            });
        }
        if geometry_unchanged {
            return;
        }
        self.arena()
            .push_paint_damage(slot, PaintDamage::ALL_PRODUCERS | PaintDamage::MOVED);
        self.arena().note_visual_context_box_dirty(
            slot,
            if moved {
                VisualContextBoxDirtyKind::MovedWithDescendants
            } else {
                VisualContextBoxDirtyKind::RecommittedInPlace
            },
        );
        self.schedule_scrollable_overflow_recalculation(slot);
    }

    pub(crate) fn set_line_data(
        &self,
        slot: NodeSlotId,
        line_data: &std::sync::Arc<crate::layout::inline_content::InlineContent>,
        previous_fragmented_content: Option<std::sync::Arc<crate::layout::inline_content::InlineContent>>,
    ) -> LineDataChange {
        if !node_painting::has_lines(self.arena(), slot) {
            return LineDataChange::default();
        }
        let writing_mode = line_data.writing_mode;
        let physical_line_data;
        let line_data = if writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB {
            line_data
        } else {
            let content_size = self.arena().live_paintable_data(slot).content_size;
            physical_line_data = std::sync::Arc::new(line_data.in_physical_plane(writing_mode, content_size));
            &physical_line_data
        };
        let has_pieces = !line_data.inline_box_pieces.is_empty();
        let (content_changed, item_order_changed) = previous_fragmented_content.map_or((false, false), |previous| {
            if std::sync::Arc::ptr_eq(&previous, line_data) || *previous == **line_data {
                return (false, false);
            }
            (true, !previous.has_same_item_order(line_data))
        });
        self.arena().committed_side_data_mut(slot).inline_content = Some(line_data.clone());
        if item_order_changed {
            self.arena().push_paint_damage(slot, PaintDamage::ORDER);
        }
        if has_pieces {
            self.arena().note_line_root_needs_fragment_ownership(slot);
        }
        LineDataChange {
            has_pieces,
            content_changed,
            item_order_changed,
        }
    }

    pub(crate) fn schedule_scrollable_overflow_recalculation(&mut self, mut node: NodeSlotId) {
        // NB: Commit visits changed boxes before their descendants. Invalidate each containing
        //     block once, including blocks outside the committed layout subtree.
        while !node.is_invalid() && self.arena().slot_is_live(node) {
            if !self.overflow_invalidated_boxes.insert(node) {
                break;
            }
            let arena = self.arena();
            if arena.paintable_row_is_populated(node) {
                arena.paintable_rows().clear_cached_overflow_data(node);
                // Commit queues are consumed immediately and are bounded by the committed tree.
                if self.is_full_layout && !arena.needs_full_scrollable_overflow_recalculation.get() {
                    arena
                        .boxes_needing_scrollable_overflow_recalculation
                        .borrow_mut()
                        .push(node);
                }
            }
            node = arena.containing_block_by_walking_ancestors(node);
        }
    }

    fn committed_containing_block(
        &self,
        node: formatting_context::Node,
        link: Option<&fragment_tree::FragmentLink>,
    ) -> NodeSlotId {
        match link.map(|link| link.containing_block) {
            Some(containing_block) if !containing_block.is_invalid() => containing_block,
            _ => self.arena().containing_block_by_walking_ancestors(node),
        }
    }

    pub(crate) fn stamp_containing_block(
        &mut self,
        node: formatting_context::Node,
        link: Option<&fragment_tree::FragmentLink>,
    ) {
        if !self.arena().paintable_row_is_populated(node) {
            return;
        }
        let containing_block = self.committed_containing_block(node, link);
        let arena = self.arena_mut();
        let mut paintable_rows = arena.paintable_rows_mut();
        let containing_block = if paintable_rows.paintable_row_is_populated(containing_block) {
            containing_block
        } else {
            NodeSlotId::INVALID
        };
        let containing_block_changed = {
            let mut data = paintable_rows.paintable_data_mut(node);
            std::mem::replace(&mut data.containing_block, containing_block) != containing_block
        };
        if containing_block_changed {
            paintable_rows.note_visual_context_box_dirty(node, VisualContextBoxDirtyKind::ContainingBlockChanged);
            paintable_rows.push_paint_damage(node, PaintDamage::MOVED);
        }
    }

    pub(crate) fn assign_inline_box_geometry(&mut self, slot: NodeSlotId) {
        let arena = self.arena_mut();
        let mut paintable_rows = arena.paintable_rows_mut();
        let mut piece_indices_by_node: Vec<(NodeSlotId, Vec<u32>)> = Vec::new();
        for (piece_index, piece) in paintable_rows
            .committed_side_data(slot)
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
        let root_positions = (crate::painting::paintable_geometry::fragment_count(&paintable_rows, slot) > 1)
            .then(|| crate::painting::paintable_geometry::LineRootPositions::new(&paintable_rows, slot));
        for (piece_node, piece_indices) in piece_indices_by_node {
            if !paintable_rows.paintable_row_is_populated(piece_node)
                || !node_painting::is_fragmented_inline(&paintable_rows, piece_node)
            {
                continue;
            }
            let origin = root_positions.as_ref().map(|positions| {
                positions.of_fragment(paintable_rows.committed_side_data(piece_node).containing_block_fragment)
            });
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
                let (piece, root_fragment_offset) = {
                    let side = paintable_rows.committed_side_data(slot);
                    let piece = side.inline_box_pieces()[*piece_index as usize];
                    let offset = root_positions.as_ref().zip(origin).map_or_else(
                        CssPixelPoint::default,
                        |(positions, origin)| {
                            let location = positions.of_line(side.lines(), piece.line_index as usize);
                            CssPixelPoint {
                                x: location.x - origin.x,
                                y: location.y - origin.y,
                            }
                        },
                    );
                    (piece, offset)
                };
                let border_rect = CssPixelRect::from(piece.border_box_rect).translated_by(root_fragment_offset);
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
            let inline_geometry_changed = {
                let mut data = paintable_rows.paintable_data_mut(piece_node);
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
                inline_geometry_changed
            };
            if inline_geometry_changed {
                paintable_rows
                    .note_visual_context_box_dirty(piece_node, VisualContextBoxDirtyKind::InlineGeometryChanged);
            }
            // This box has at most one piece per line, so its piece indices are ordered by line.
            paintable_rows.committed_side_data_mut(piece_node).piece_indices = Some(piece_indices.into());
        }
    }
}
