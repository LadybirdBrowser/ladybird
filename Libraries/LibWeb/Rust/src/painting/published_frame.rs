/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a document publishes for the display list recording to read.
//!
//! A [`PublishedFrame`] is what one recording reads: [`PublishedRows`], one generation of a
//! document's layout tree shape, paintable rows and paint facts and of the columns read beside them,
//! with the paint damage and the paint state the recording reads. It is immutable: every column in
//! it is a [`ColumnSnapshot`] or an `Arc` the document shares with it, so the document writes its
//! live columns (copying a chunk a frame still shares) while it is read.

use crate::cow_column::ColumnSnapshot;
use crate::css::css_pixels::CssPixelPoint;
use crate::css::style::StyleRecordLease;
use crate::layout::LayoutNodeArena;
use crate::layout::PublishedTextSlot;
use crate::layout::SLOTS_PER_CHUNK;
use crate::layout::fragment_tree::FragmentLink;
use crate::layout::node_data::{NodeSlotId, PaintNode};
use crate::layout::tree_shape::PUBLISHED_ROWS_PER_CHUNK;
use crate::painting::layer_image_paint_facts::LayerImagePaintFactsTable;
use crate::painting::paint_state::{PaintState, SelectionPseudoStyles};
use crate::painting::paintable_data::{CommittedSideData, PaintableData};
use crate::painting::paintable_rows::{CommittedFragmentLinkSlot, PAINTABLE_SLOTS_PER_CHUNK};
use crate::painting::record::damage::FrameDamage;
use crate::painting::replaced_paint_facts::ReplacedPaintFactsTable;
use crate::painting::selection::{HighlightPseudoElement, SearchTextHighlights, SelectionRange};
use crate::painting::stacking_context::entries::StackingContextEntries;
use crate::painting::svg_paint_resources::SvgPaintResourceRows;
use crate::painting::visual_context::VisualContextTree;
use crate::painting::visual_context::{BoxVisualContextNodeHandles, EMPTY_BOX_VISUAL_CONTEXT_NODE_HANDLES};
use std::sync::Arc;

/// The document's text rows and its replaced, layer image and SVG paint resource tables, as they
/// were when published.
pub(crate) struct PublishedPaintFacts {
    pub(crate) text: ColumnSnapshot<PublishedTextSlot, SLOTS_PER_CHUNK>,
    pub(crate) replaced: Arc<ReplacedPaintFactsTable>,
    pub(crate) layer_images: Arc<LayerImagePaintFactsTable>,
    pub(crate) svg_paint_resources: Arc<SvgPaintResourceRows>,
}

/// One published generation of a document's layout tree shape and paintable rows, and of the
/// columns read beside them.
pub(crate) struct PublishedRows {
    /// The epoch of the geometry the rows were published with, which the absolute rects a
    /// recording computes from them are stamped with.
    pub(super) geometry_epoch: u64,
    pub(super) nodes: ColumnSnapshot<PaintNode, PUBLISHED_ROWS_PER_CHUNK>,
    pub(super) paint_facts: PublishedPaintFacts,
    pub(super) rows: ColumnSnapshot<PaintableData, PAINTABLE_SLOTS_PER_CHUNK>,
    pub(super) fragment_links: ColumnSnapshot<CommittedFragmentLinkSlot, PAINTABLE_SLOTS_PER_CHUNK>,
    pub(super) side_data: ColumnSnapshot<CommittedSideData, PAINTABLE_SLOTS_PER_CHUNK>,
    pub(super) stacking_context_entries: ColumnSnapshot<Option<Arc<StackingContextEntries>>, PAINTABLE_SLOTS_PER_CHUNK>,
    pub(super) visual_context_node_handles:
        ColumnSnapshot<Option<Arc<BoxVisualContextNodeHandles>>, PAINTABLE_SLOTS_PER_CHUNK>,
}

impl PublishedRows {
    /// What a live text row published.
    pub(crate) fn text(&self, id: NodeSlotId) -> Option<&PublishedTextSlot> {
        self.node(id)?;
        self.paint_facts
            .text
            .get(id.slot_index() as usize)
            .filter(|text| text.generation == id.generation())
    }

    /// The node in slot `id`, if the slot holds it: a node freed or replaced since reads as gone.
    pub(crate) fn node(&self, id: NodeSlotId) -> Option<&PaintNode> {
        if id.is_invalid() {
            return None;
        }
        self.nodes
            .get(id.slot_index() as usize)
            .filter(|node| node.generation != 0 && node.generation == id.generation())
    }

    pub(crate) fn paintable_data(&self, id: NodeSlotId) -> &PaintableData {
        assert!(!id.is_invalid(), "invalid paintable arena slot ID");
        let data = self
            .rows
            .get(id.slot_index() as usize)
            .expect("invalid paintable arena slot ID");
        assert_eq!(
            data.slot_generation,
            id.generation(),
            "paintable arena read a stale or unused slot"
        );
        data
    }

    pub(crate) fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool {
        if id.is_invalid() {
            return false;
        }
        self.rows
            .get(id.slot_index() as usize)
            .is_some_and(|data| data.slot_generation != 0 && data.slot_generation == id.generation())
    }

    pub(crate) fn with_committed_fragment_link<R>(
        &self,
        id: NodeSlotId,
        read: impl FnOnce(Option<&FragmentLink>) -> R,
    ) -> R {
        debug_assert!(self.paintable_row_is_populated(id));
        read(
            self.fragment_links
                .get(id.slot_index() as usize)
                .and_then(CommittedFragmentLinkSlot::link),
        )
    }

    pub(crate) fn committed_side_data(&self, id: NodeSlotId) -> &CommittedSideData {
        debug_assert!(self.paintable_row_is_populated(id));
        self.side_data
            .get(id.slot_index() as usize)
            .expect("a populated row has published side data")
    }

    pub(crate) fn visual_context_node_handles(&self, id: NodeSlotId) -> &BoxVisualContextNodeHandles {
        self.paintable_row_is_populated(id)
            .then(|| self.visual_context_node_handles.get(id.slot_index() as usize))
            .flatten()
            .and_then(|handles| handles.as_deref())
            .unwrap_or(&EMPTY_BOX_VISUAL_CONTEXT_NODE_HANDLES)
    }

    pub(crate) fn stacking_context_entries(&self, root: NodeSlotId) -> Option<&StackingContextEntries> {
        if !self.paintable_row_is_populated(root) {
            return None;
        }
        self.stacking_context_entries
            .get(root.slot_index() as usize)
            .and_then(|table| table.as_deref())
    }
}

/// What a recording reads of the document's paint state, as it was when the frame was published.
pub(crate) struct PublishedPaintState {
    pub(crate) visual_context_tree: Option<Arc<VisualContextTree>>,
    /// Each scroll state slot's own scroll offset, shared with the document's scroll state.
    scroll_own_offsets: Arc<Vec<CssPixelPoint>>,
    pub(crate) has_non_viewport_wheel_scroll_target_candidate: bool,
    pub(crate) selection: Option<Arc<SelectionRange>>,
    pub(crate) selection_pseudo_styles: Arc<SelectionPseudoStyles>,
    pub(crate) search_text: Arc<SearchTextHighlights>,
    pub(crate) search_text_pseudo_styles: Arc<SelectionPseudoStyles>,
    pub(crate) search_text_current_pseudo_styles: Arc<SelectionPseudoStyles>,
    pub(crate) hit_test_list_generation: u64,
    /// How many items the document's hit-test list held, which the recording's list reserves.
    pub(crate) hit_test_item_capacity_hint: usize,
}

impl PublishedPaintState {
    pub(crate) fn highlight_pseudo_styles(&self, highlight: HighlightPseudoElement) -> &SelectionPseudoStyles {
        match highlight {
            HighlightPseudoElement::Selection => &self.selection_pseudo_styles,
            HighlightPseudoElement::SearchText => &self.search_text_pseudo_styles,
            HighlightPseudoElement::SearchTextCurrent => &self.search_text_current_pseudo_styles,
        }
    }

    fn new(paint_state: &PaintState, hit_test_item_capacity_hint: usize) -> Self {
        let visual_context = &paint_state.visual_context;
        Self {
            visual_context_tree: visual_context.tree.clone(),
            scroll_own_offsets: visual_context.scroll_state.own_offsets().clone(),
            has_non_viewport_wheel_scroll_target_candidate: visual_context
                .scroll_state
                .has_non_viewport_wheel_scroll_target_candidate,
            selection: paint_state.selection.clone(),
            selection_pseudo_styles: paint_state.selection_pseudo_styles.clone(),
            search_text: paint_state.search_text.clone(),
            search_text_pseudo_styles: paint_state.search_text_pseudo_styles.clone(),
            search_text_current_pseudo_styles: paint_state.search_text_current_pseudo_styles.clone(),
            hit_test_list_generation: paint_state.hit_test_list_generation,
            hit_test_item_capacity_hint,
        }
    }

    pub(crate) fn structural_epoch(&self) -> u64 {
        self.visual_context_tree
            .as_ref()
            .map_or(0, |tree| tree.structural_epoch)
    }

    pub(crate) fn scroll_own_offset(&self, slot: usize) -> CssPixelPoint {
        let offset = self.scroll_own_offsets.get(slot).copied();
        debug_assert!(offset.is_some(), "the tree's scroll node has a scroll state");
        offset.unwrap_or_default()
    }
}

/// What a document published for one recording to read: its rows, its paint damage and the paint
/// state the recording reads, with a lease on the style records its rows name. [`LayoutNodeArena::freeze_frame`] is
/// its only constructor and it has no `Clone`: a recording takes a frame and drops it before it returns, and nothing
/// keeps one past the next write to the arena.
pub(crate) struct PublishedFrame {
    pub(super) rows: PublishedRows,
    damage: FrameDamage,
    paint_state: PublishedPaintState,
    _style_records: StyleRecordLease,
}

// A frame is read on whichever thread paints it while the document writes its live columns: it
// holds no cell and no borrow of the document, and owns everything it reads but its nodes' styles.
// Those are `HostShared` pointers to style records, which stay valid because the engine frees no
// record's payloads while the frame holds its lease, even after the document has replaced them.
const _: () = {
    const fn assert_published<T: Send + Sync + 'static>() {}
    assert_published::<PublishedFrame>();
};

impl LayoutNodeArena {
    /// What the next recording reads of the document, as it is now. The host says how many items
    /// the recording's hit-test list may hold.
    pub(crate) fn freeze_frame(&mut self, hit_test_item_capacity_hint: usize) -> PublishedFrame {
        let rows = self.publish_rows();
        PublishedFrame {
            rows,
            damage: self.paint_damage_for_frame(),
            paint_state: PublishedPaintState::new(&self.paint_state().borrow(), hit_test_item_capacity_hint),
            _style_records: self.with_style_engine(|engine| engine.lease_style_records()),
        }
    }
}

impl PublishedFrame {
    /// What the frame's recording reads of the document's paint state.
    pub(crate) fn paint_state(&self) -> &PublishedPaintState {
        &self.paint_state
    }

    /// The paint damage the frame was published with.
    pub(crate) fn damage(&self) -> &FrameDamage {
        &self.damage
    }

    /// The SVG paint resources the frame was published with.
    pub(crate) fn svg_paint_resources(&self) -> &Arc<SvgPaintResourceRows> {
        &self.rows.paint_facts.svg_paint_resources
    }

    /// How many paintable rows the frame has room for.
    pub(crate) fn paintable_row_capacity(&self) -> usize {
        self.rows.rows.slot_capacity()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_pixels::CssPixels;
    use crate::layout::LayoutNodeArena;
    use crate::layout::fragment_tree;
    use crate::layout::node_data::{NodeFlag, NodeKind};
    use crate::painting::paint_read::{GeometryRead, PaintRead, PaintSource};
    use crate::painting::record::recorder_state::AbsoluteRectMemo;
    use std::cell::RefCell;

    #[test]
    fn published_rows_keep_their_generation_after_a_later_write() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.populate_paintable_row(node);
        let mut link = fragment_tree::FragmentLink::for_test(node);
        link.inset_left = CssPixels::from_integer(10);
        arena.set_committed_fragment_link(arena.data(node), link.clone(), None);
        arena.paintable_rows_mut().paintable_data_mut(node).offset.x = CssPixels::from_integer(10).into();
        arena.committed_side_data_mut(node).piece_indices = Some([0].into());

        let published = arena.publish_rows();
        link.inset_left = CssPixels::from_integer(20);
        arena.set_committed_fragment_link(arena.data(node), link, None);
        arena.paintable_rows_mut().paintable_data_mut(node).offset.x = CssPixels::from_integer(20).into();
        arena.committed_side_data_mut(node).piece_indices = None;

        assert_eq!(
            published.paintable_data(node).offset.x,
            CssPixels::from_integer(10).into()
        );
        assert_eq!(
            published.with_committed_fragment_link(node, |link| link.map(|link| link.inset_left)),
            Some(CssPixels::from_integer(10))
        );
        assert_eq!(published.committed_side_data(node).piece_indices(), [0]);
        assert_eq!(
            arena.live_paintable_data(node).offset.x,
            CssPixels::from_integer(20).into()
        );
        assert_eq!(
            arena.with_committed_fragment_link(node, |link| link.map(|link| link.inset_left)),
            Some(CssPixels::from_integer(20))
        );
        assert!(arena.committed_side_data(node).piece_indices().is_empty());
    }

    #[test]
    fn published_rows_answer_every_row_read_as_the_arena_does() {
        let mut engine = crate::css::style::StyleEngine::new();
        let mut arena = LayoutNodeArena::new();
        arena.set_style_engine(crate::css::style::StyleEngineHandle::from_raw(&raw mut engine));
        let mut slots = Vec::new();
        for index in 0..PAINTABLE_SLOTS_PER_CHUNK + 3 {
            let node = arena.allocate_for_test().slot;
            slots.push(node);
            if index % 3 == 0 {
                continue;
            }
            arena.populate_paintable_row(node);
            arena.paintable_rows_mut().paintable_data_mut(node).offset.x = CssPixels::from_integer(index as i64).into();
            if index % 2 == 0 {
                let mut link = fragment_tree::FragmentLink::for_test(node);
                link.inset_top = CssPixels::from_integer(index as i64);
                arena.set_committed_fragment_link(arena.data(node), link, None);
                arena.committed_side_data_mut(node).overflow_valid_across_recommits = true;
            }
        }

        let root = slots[1];
        arena.write_shape(root).set_kind(NodeKind::BlockContainer);
        for &child in &slots[2..6] {
            arena.write_shape(child).set_kind(NodeKind::InlineNode);
            arena.insert_child(root, child, NodeSlotId::INVALID);
        }
        arena.set_node_flag(slots[3], NodeFlag::Anonymous, true);
        arena.write_shape(slots[4]).set_generated_for(1);

        let frame = arena.freeze_frame(0);
        let absolute_rects = RefCell::new(AbsoluteRectMemo::default());
        let source = PaintSource::new(&frame, &absolute_rects);
        for &node in &slots {
            assert_eq!(source.slot_is_live(node), arena.slot_is_live(node));
            assert_eq!(source.node_kind_if_live(node), arena.node_kind_if_live(node));
            assert_eq!(source.node_flags_if_live(node), arena.node_flags_if_live(node));
            assert_eq!(source.node_parent_if_live(node), arena.node_parent_if_live(node));
            assert_eq!(
                source.node_first_child_if_live(node),
                arena.node_first_child_if_live(node)
            );
            assert_eq!(
                source.node_next_sibling_if_live(node),
                arena.node_next_sibling_if_live(node)
            );
            assert_eq!(source.node_generated_for(node), arena.node_generated_for(node));
            assert_eq!(source.node_is_dom_backed(node), arena.node_is_dom_backed(node));
            assert_eq!(
                source.node_is_fragmented_inline(node),
                arena.node_is_fragmented_inline(node)
            );
            assert_eq!(
                source.node_is_out_of_flow_if_live(node),
                arena.node_is_out_of_flow_if_live(node)
            );
            assert_eq!(source.node_is_atomic_inline(node), arena.node_is_atomic_inline(node));
            assert_eq!(source.node_is_positioned(node), arena.node_is_positioned(node));
            assert_eq!(source.node_is_floating(node), arena.node_is_floating(node));
            assert_eq!(
                source.node_style_if_live(node).is_some(),
                arena.node_style_if_live(node).is_some()
            );
            assert_eq!(
                source.text_fragments(node).as_slice(),
                arena.text_fragments(node).as_slice()
            );
            assert_eq!(source.rendered_text(node).is_some(), arena.text_content(node).is_some());
            assert_eq!(source.replaced_paint_facts(node), arena.replaced_paint_facts(node));
        }
        for node in slots {
            assert_eq!(
                source.paintable_row_is_populated(node),
                arena.paintable_row_is_populated(node)
            );
            if !arena.paintable_row_is_populated(node) {
                continue;
            }
            assert_eq!(source.paintable_data(node), arena.live_paintable_data(node));
            assert_eq!(
                source.with_committed_fragment_link(node, |link| link.map(|link| link.inset_top)),
                arena.with_committed_fragment_link(node, |link| link.map(|link| link.inset_top))
            );
            assert!(*source.committed_side_data(node) == *arena.committed_side_data(node));
            assert_eq!(source.paint_damage_of_row(node), arena.paint_damage_of_row(node));
            assert!(source.stacking_context_entries(node).is_none() == arena.stacking_context_entries(node).is_none());
        }
    }
}
