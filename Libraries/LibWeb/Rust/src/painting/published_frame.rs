/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a document publishes for the display list recording to read.
//!
//! A [`PublishedFrame`] is what one recording reads: [`PublishedRows`], one generation of a
//! document's layout tree shape, paintable rows and paint facts and of the columns read beside them,
//! with the paint generation and the paint state the recording reads. It is immutable: every column in
//! it is a [`ColumnSnapshot`] or an `Arc` the document shares with it, so the document writes its
//! live columns (copying a chunk a frame still shares) while it is read.

use crate::cow_column::ColumnSnapshot;
use crate::css::css_pixels::CssPixelPoint;
use crate::css::style::{HoverFrameLease, StyleRecordLease};
use crate::layout::LayoutNodeArena;
use crate::layout::PublishedTextSlot;
use crate::layout::SLOTS_PER_CHUNK;
use crate::layout::fragment_tree::FragmentLink;
use crate::layout::node_data::{NodeSlotId, PaintNode};
use crate::layout::tree_shape::PUBLISHED_ROWS_PER_CHUNK;
use crate::painting::layer_image_paint_facts::LayerImagePaintFactsTable;
use crate::painting::paint_state::{CustomHighlightPseudoStyles, PaintState, SelectionPseudoStyles};
use crate::painting::paintable_data::{CommittedSideData, PaintableData};
use crate::painting::paintable_rows::{CommittedFragmentLinkSlot, PAINTABLE_SLOTS_PER_CHUNK};
use crate::painting::record::change_ticks::{PaintChangeStamps, PaintGeneration};
use crate::painting::replaced_paint_facts::ReplacedPaintFactsTable;
use crate::painting::selection::{
    CustomHighlights, HighlightPseudoElement, SearchTextHighlights, SelectionRange, SpellingErrorHighlights,
};
use crate::painting::stacking_context::entries::StackingContextEntries;
use crate::painting::svg_paint_resources::SvgPaintResourceRows;
use crate::painting::visual_context::{BoxVisualContextNodeHandles, EMPTY_BOX_VISUAL_CONTEXT_NODE_HANDLES};
use crate::painting::visual_context::{VisualContextTree, WorldTransforms};
use std::sync::Arc;

/// The document's text rows and its replaced, layer image and SVG paint resource tables, as they
/// were when published.
#[derive(Clone)]
pub(crate) struct PublishedPaintFacts {
    pub(crate) text: ColumnSnapshot<PublishedTextSlot, SLOTS_PER_CHUNK>,
    pub(crate) replaced: Arc<ReplacedPaintFactsTable>,
    pub(crate) layer_images: Arc<LayerImagePaintFactsTable>,
    pub(crate) svg_paint_resources: Arc<SvgPaintResourceRows>,
    pub(crate) image_maps: Arc<crate::painting::image_map_areas::ImageMaps>,
}

/// One published generation of a document's layout tree shape and paintable rows, and of the
/// columns read beside them.
#[derive(Clone)]
pub(crate) struct PublishedRows {
    /// The epoch of the geometry the rows were published with, which the absolute rects a
    /// recording computes from them are stamped with.
    pub(super) geometry_epoch: u64,
    pub(super) ink_overflow: ColumnSnapshot<crate::painting::ink_overflow::InkOverflow, PAINTABLE_SLOTS_PER_CHUNK>,
    pub(crate) ink_overflow_stamp: u64,
    pub(crate) hit_test_items_stamp: u64,
    pub(super) paint_change_stamps: ColumnSnapshot<PaintChangeStamps, PAINTABLE_SLOTS_PER_CHUNK>,
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
    /// The column of the nodes, as published.
    pub(crate) fn paint_nodes(&self) -> &ColumnSnapshot<PaintNode, PUBLISHED_ROWS_PER_CHUNK> {
        &self.nodes
    }

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

    /// The nodes the slots hold, with the slot of each.
    pub(crate) fn live_nodes(&self) -> impl Iterator<Item = (NodeSlotId, &PaintNode)> {
        (0..self.nodes.slot_capacity()).filter_map(|index| {
            let node = self.nodes.get(index).filter(|node| node.generation != 0)?;
            Some((NodeSlotId::new(index as u32, node.generation), node))
        })
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
        read(
            self.fragment_links
                .get(id.slot_index() as usize)
                .and_then(|slot| slot.link_for(id.generation())),
        )
    }

    pub(crate) fn paint_change_stamps(&self, id: NodeSlotId) -> PaintChangeStamps {
        if !self.paintable_row_is_populated(id) {
            return PaintChangeStamps::default();
        }
        self.paint_change_stamps
            .get(id.slot_index() as usize)
            .copied()
            .unwrap_or_default()
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
    unscrolled_world_transforms: std::sync::OnceLock<WorldTransforms>,
    /// Each scroll state slot's own scroll offset, shared with the document's scroll state.
    scroll_own_offsets: Arc<Vec<CssPixelPoint>>,
    pub(crate) has_non_viewport_wheel_scroll_target_candidate: bool,
    pub(crate) selection: Option<Arc<SelectionRange>>,
    pub(crate) selection_pseudo_styles: Arc<SelectionPseudoStyles>,
    pub(crate) search_text: Arc<SearchTextHighlights>,
    pub(crate) search_text_pseudo_styles: Arc<SelectionPseudoStyles>,
    pub(crate) search_text_current_pseudo_styles: Arc<SelectionPseudoStyles>,
    pub(crate) spelling_errors: Arc<SpellingErrorHighlights>,
    pub(crate) spelling_error_pseudo_styles: Arc<SelectionPseudoStyles>,
    pub(crate) custom_highlights: Arc<CustomHighlights>,
    pub(crate) custom_highlight_pseudo_styles: Arc<CustomHighlightPseudoStyles>,
}

impl PublishedPaintState {
    /// The tree's world transforms without scroll offsets, which is all that the scale of a space depends on.
    pub(crate) fn unscrolled_world_transforms(&self) -> &WorldTransforms {
        self.unscrolled_world_transforms.get_or_init(|| {
            let tree = self
                .visual_context_tree
                .as_deref()
                .expect("recording runs against a visual context tree");
            WorldTransforms::new(tree, &[], None)
        })
    }

    pub(crate) fn highlight_pseudo_styles(&self, highlight: HighlightPseudoElement) -> &SelectionPseudoStyles {
        match highlight {
            HighlightPseudoElement::Selection => &self.selection_pseudo_styles,
            HighlightPseudoElement::SearchText => &self.search_text_pseudo_styles,
            HighlightPseudoElement::SearchTextCurrent => &self.search_text_current_pseudo_styles,
            HighlightPseudoElement::SpellingError => &self.spelling_error_pseudo_styles,
        }
    }

    pub(crate) fn new(paint_state: &PaintState) -> Self {
        let visual_context = &paint_state.visual_context;
        Self {
            visual_context_tree: visual_context.tree.clone(),
            unscrolled_world_transforms: std::sync::OnceLock::new(),
            scroll_own_offsets: visual_context.scroll_state.own_offsets().clone(),
            has_non_viewport_wheel_scroll_target_candidate: visual_context
                .scroll_state
                .has_non_viewport_wheel_scroll_target_candidate,
            selection: paint_state.selection.clone(),
            selection_pseudo_styles: paint_state.selection_pseudo_styles.clone(),
            search_text: paint_state.search_text.clone(),
            search_text_pseudo_styles: paint_state.search_text_pseudo_styles.clone(),
            search_text_current_pseudo_styles: paint_state.search_text_current_pseudo_styles.clone(),
            spelling_errors: paint_state.spelling_errors.clone(),
            spelling_error_pseudo_styles: paint_state.spelling_error_pseudo_styles.clone(),
            custom_highlights: paint_state.custom_highlights.clone(),
            custom_highlight_pseudo_styles: paint_state.custom_highlight_pseudo_styles.clone(),
        }
    }

    pub(crate) fn structural_epoch(&self) -> u64 {
        self.visual_context_tree
            .as_ref()
            .map_or(0, |tree| tree.structural_epoch)
    }

    pub(crate) fn scroll_own_offsets(&self) -> &[CssPixelPoint] {
        &self.scroll_own_offsets
    }

    fn with_scroll_own_offsets(&self, scroll_own_offsets: Arc<Vec<CssPixelPoint>>) -> Self {
        Self {
            visual_context_tree: self.visual_context_tree.clone(),
            unscrolled_world_transforms: std::sync::OnceLock::new(),
            scroll_own_offsets,
            has_non_viewport_wheel_scroll_target_candidate: self.has_non_viewport_wheel_scroll_target_candidate,
            selection: self.selection.clone(),
            selection_pseudo_styles: self.selection_pseudo_styles.clone(),
            search_text: self.search_text.clone(),
            search_text_pseudo_styles: self.search_text_pseudo_styles.clone(),
            search_text_current_pseudo_styles: self.search_text_current_pseudo_styles.clone(),
            spelling_errors: self.spelling_errors.clone(),
            spelling_error_pseudo_styles: self.spelling_error_pseudo_styles.clone(),
            custom_highlights: self.custom_highlights.clone(),
            custom_highlight_pseudo_styles: self.custom_highlight_pseudo_styles.clone(),
        }
    }

    pub(crate) fn scroll_own_offset(&self, slot: usize) -> CssPixelPoint {
        let offset = self.scroll_own_offsets.get(slot).copied();
        debug_assert!(offset.is_some(), "the tree's scroll node has a scroll state");
        offset.unwrap_or_default()
    }
}

/// What a document published for one recording to read: its rows, the paint generation it was published as of and the
/// paint state the recording reads, with a lease on the style records its rows name. [`LayoutNodeArena::freeze_frame`] is
/// its only constructor and it has no `Clone`: a recording takes a frame and drops it before it returns, and nothing
/// keeps one past the next write to the arena.
pub(crate) struct PublishedFrame {
    pub(super) rows: PublishedRows,
    paint_generation: PaintGeneration,
    everything_changed_at: PaintGeneration,
    paint_state: PublishedPaintState,
    _style_records: FrameStyleRecords,
}

enum FrameStyleRecords {
    Every(#[expect(dead_code, reason = "held for the payloads it keeps")] StyleRecordLease),
    NamedByItsRows(#[expect(dead_code, reason = "held for the payloads it keeps")] Arc<HoverFrameLease>),
}

/// A committed frame that a clock lane keeps until the next one, to record it again where the compositor scrolled to
/// while the host is busy. It shares the frame's rows and paint state, and holds only the style records its rows name,
/// so that the engine goes on reclaiming the others.
pub(crate) struct HeldFrame {
    rows: PublishedRows,
    paint_generation: PaintGeneration,
    everything_changed_at: PaintGeneration,
    paint_state: PublishedPaintState,
    style_records: Arc<HoverFrameLease>,
    viewport: NodeSlotId,
}

impl HeldFrame {
    /// The row the frame's paint order starts at.
    pub(crate) fn viewport(&self) -> NodeSlotId {
        self.viewport
    }

    /// The frame with its scroll containers where `scrolled` says the compositor scrolled them to, by the identity of
    /// their nodes, and the others where the frame had them.
    pub(crate) fn frame_where_scrolled(&self, scrolled: &[(i64, CssPixelPoint)]) -> PublishedFrame {
        PublishedFrame {
            rows: self.rows.clone(),
            paint_generation: self.paint_generation,
            everything_changed_at: self.everything_changed_at,
            paint_state: paint_state_where_scrolled(&self.rows, &self.paint_state, scrolled),
            _style_records: FrameStyleRecords::NamedByItsRows(Arc::clone(&self.style_records)),
        }
    }
}

/// `paint_state`, the paint state of a frame of `rows`, with the frame's scroll containers where `scrolled` says the
/// compositor scrolled them to, by the identity of their nodes, and the others where the frame had them.
fn paint_state_where_scrolled(
    rows: &PublishedRows,
    paint_state: &PublishedPaintState,
    scrolled: &[(i64, CssPixelPoint)],
) -> PublishedPaintState {
    let mut scroll_own_offsets = paint_state.scroll_own_offsets.as_ref().clone();
    if let Some(tree) = paint_state.visual_context_tree.as_deref() {
        for node in &tree.spatial_nodes {
            let crate::painting::visual_context::SpatialData::Scroll(scroll) = &node.data else {
                continue;
            };
            if !rows.paintable_row_is_populated(scroll.owner_paintable) {
                continue;
            }
            let identity = rows.paintable_data(scroll.owner_paintable).node_identity;
            let Some(&(_, position)) = scrolled.iter().find(|(scroller, _)| *scroller == identity) else {
                continue;
            };
            if let Some(own_offset) = scroll_own_offsets.get_mut(scroll.state_slot) {
                *own_offset = CssPixelPoint::new(-position.x, -position.y);
            }
        }
    }
    paint_state.with_scroll_own_offsets(Arc::new(scroll_own_offsets))
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
    /// What the next recording reads of the document, as it is now.
    pub(crate) fn freeze_frame(&mut self) -> PublishedFrame {
        self.settle_ink_overflow();
        let rows = self.publish_rows();
        PublishedFrame {
            rows,
            paint_generation: self.take_paint_generation(),
            everything_changed_at: self.everything_changed_at(),
            paint_state: PublishedPaintState::new(self.paint_state()),
            _style_records: FrameStyleRecords::Every(self.with_style_store(|engine| engine.lease_style_records())),
        }
    }
}

impl PublishedFrame {
    /// Moves the frame's scroll containers to where `scrolled` says the compositor scrolled them to, by the identity of
    /// their nodes, and leaves the others where the frame had them.
    pub(crate) fn scroll_to_where_scrolled(&mut self, scrolled: &[(i64, CssPixelPoint)]) {
        self.paint_state = paint_state_where_scrolled(&self.rows, &self.paint_state, scrolled);
    }

    /// What the frame's recording reads of the document's paint state.
    pub(crate) fn paint_state(&self) -> &PublishedPaintState {
        &self.paint_state
    }

    /// The frame for a clock lane to keep, of which the row of `viewport` starts the paint order, under `style_records`,
    /// a lease on the records its rows name.
    pub(crate) fn held(&self, viewport: NodeSlotId, style_records: Arc<HoverFrameLease>) -> HeldFrame {
        HeldFrame {
            rows: self.rows.clone(),
            paint_generation: self.paint_generation,
            everything_changed_at: self.everything_changed_at,
            paint_state: self
                .paint_state
                .with_scroll_own_offsets(Arc::clone(&self.paint_state.scroll_own_offsets)),
            style_records,
            viewport,
        }
    }

    /// The column of the nodes the frame's rows name the style records of.
    pub(crate) fn paint_nodes(&self) -> &ColumnSnapshot<PaintNode, PUBLISHED_ROWS_PER_CHUNK> {
        self.rows.paint_nodes()
    }

    /// The generation the frame was published as of.
    pub(crate) fn paint_generation(&self) -> PaintGeneration {
        self.paint_generation
    }

    pub(crate) fn everything_paints_anew_since(&self, as_of: PaintGeneration) -> bool {
        self.everything_changed_at > as_of
    }

    /// The SVG paint resources the frame was published with.
    pub(crate) fn svg_paint_resources(&self) -> &Arc<SvgPaintResourceRows> {
        &self.rows.paint_facts.svg_paint_resources
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_pixels::CssPixels;
    use crate::layout::LayoutNodeArena;
    use crate::layout::fragment_tree;
    use crate::layout::node_data::{NodeFlag, NodeKind};
    use crate::painting::paint_read::{GeometryRead, PaintRead, PaintRow, PaintSource};
    use crate::painting::record::recorder_state::AbsoluteRectMemo;
    use std::cell::RefCell;

    #[test]
    fn published_rows_keep_their_generation_after_a_later_write() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.populate_paintable_row(node);
        let mut link = fragment_tree::FragmentLink::for_test(node);
        link.placement.set_insets(crate::layout::geometry::Sides {
            left: CssPixels::from_integer(10),
            ..Default::default()
        });
        arena.set_committed_fragment_link(node, link.clone(), None);
        arena.paintable_rows_mut().paintable_data_mut(node).node_identity = 10;
        arena.committed_side_data_mut(node).inline_content = Some(Default::default());

        let published = arena.publish_rows();
        link.placement.set_insets(crate::layout::geometry::Sides {
            left: CssPixels::from_integer(20),
            ..Default::default()
        });
        arena.set_committed_fragment_link(node, link, None);
        arena.paintable_rows_mut().paintable_data_mut(node).node_identity = 20;
        arena.committed_side_data_mut(node).inline_content = None;

        assert_eq!(published.paintable_data(node).node_identity, 10);
        assert_eq!(
            published.with_committed_fragment_link(node, |link| link.map(|link| link.placement.insets().left)),
            Some(CssPixels::from_integer(10))
        );
        assert!(published.committed_side_data(node).inline_content.is_some());
        assert_eq!(arena.live_paintable_data(node).node_identity, 20);
        assert_eq!(
            arena.with_committed_fragment_link(node, |link| link.map(|link| link.placement.insets().left)),
            Some(CssPixels::from_integer(20))
        );
        assert!(arena.committed_side_data(node).inline_content.is_none());
    }

    fn derived_facts<'a>(row: Option<impl PaintRow<'a>>) -> Option<[bool; 5]> {
        row.map(|row| {
            [
                row.is_fragmented_inline(),
                row.is_out_of_flow(),
                row.is_atomic_inline(),
                row.is_positioned(),
                row.is_floating(),
            ]
        })
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
            arena.paintable_rows_mut().paintable_data_mut(node).node_identity = index as i64;
            if index % 2 == 0 {
                let mut link = fragment_tree::FragmentLink::for_test(node);
                link.placement.set_insets(crate::layout::geometry::Sides {
                    top: CssPixels::from_integer(index as i64),
                    ..Default::default()
                });
                arena.set_committed_fragment_link(node, link, None);
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

        let frame = arena.freeze_frame();
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
            assert_eq!(derived_facts(source.node(node)), derived_facts(arena.node(node)));
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
                source.with_committed_fragment_link(node, |link| link.map(|link| link.placement.insets().top)),
                arena.with_committed_fragment_link(node, |link| link.map(|link| link.placement.insets().top))
            );
            assert!(*source.committed_side_data(node) == *arena.committed_side_data(node));
            assert_eq!(source.paint_change_stamps(node), arena.paint_change_stamps(node));
            assert!(source.stacking_context_entries(node).is_none() == arena.stacking_context_entries(node).is_none());
        }
    }
}
