/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::cow_column::{CowColumn, RowMut};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeFlag, NodeSlotId};
use crate::layout::{fragment_tree, used_values};
use crate::painting::node_painting;
use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::paintable_data::*;
use crate::painting::published_frame::PublishedRows;
use crate::painting::record::damage::{DamageSet, PaintDamage, RowPaintState};
use crate::painting::stacking_context::entries::{StackingContextEntryColumn, drop_table};
use crate::painting::visual_context::dirty::{RemovedBoxBlocks, VisualContextBoxDirtyKind, VisualContextUpdateScope};
use crate::painting::visual_context::{
    BoxVisualContextNodeHandles, EMPTY_BOX_VISUAL_CONTEXT_NODE_HANDLES, PaintableVisualContextRecord,
};
use smallvec::{SmallVec, smallvec};
use std::cell::{Cell, Ref, RefCell, RefMut};
use std::ffi::c_void;
use std::ops::{Deref, DerefMut};
use std::sync::Arc;

pub(crate) const PAINTABLE_SLOTS_PER_CHUNK: usize = 64;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn committed_geometry_validity_fits_in_the_link_slots_existing_padding() {
        assert_eq!(std::mem::size_of::<CommittedFragmentLinkSlot>(), 16);
    }

    #[test]
    fn committed_fragment_link_slots_are_equal_when_they_place_the_same_fragment_the_same_way() {
        let node = NodeSlotId::new(1, 1);
        let link = fragment_tree::FragmentLink::for_test(node);
        let slot_with = |link: fragment_tree::FragmentLink| CommittedFragmentLinkSlot {
            layout_slot_generation: 1,
            link: Some(Arc::new(link)),
            ..Default::default()
        };
        assert!(slot_with(link.clone()) == slot_with(link.clone()));

        let mut moved = link.clone();
        moved.committed_offset.x = crate::css::css_pixels::CssPixels::from_integer(10);
        assert!(slot_with(link.clone()) != slot_with(moved));

        // An equal fragment laid out again is a different fragment.
        assert!(slot_with(link.clone()) != slot_with(fragment_tree::FragmentLink::for_test(node)));

        let mut stale = slot_with(link.clone());
        stale.geometry_is_current = true;
        assert!(slot_with(link) != stale);
        assert!(CommittedFragmentLinkSlot::default() == CommittedFragmentLinkSlot::default());
    }

    #[test]
    fn committed_side_data_compares_shared_records_by_allocation_or_value() {
        use crate::layout::inline_content::InlineContent;
        use std::sync::Arc;

        let side_data = CommittedSideData {
            inline_content: Some(Arc::new(InlineContent::default())),
            piece_indices: Some(Arc::from([1, 2])),
            ..Default::default()
        };
        assert!(side_data == side_data.clone());

        let equal_records = CommittedSideData {
            inline_content: Some(Arc::new(InlineContent::default())),
            piece_indices: Some(Arc::from([1, 2])),
            ..Default::default()
        };
        assert!(side_data == equal_records);

        let other_pieces = CommittedSideData {
            piece_indices: Some(Arc::from([1])),
            ..side_data.clone()
        };
        assert!(side_data != other_pieces);

        let without_content = CommittedSideData {
            inline_content: None,
            ..side_data.clone()
        };
        assert!(side_data != without_content);

        let measured = CommittedSideData {
            overflow_valid_across_recommits: true,
            ..side_data.clone()
        };
        assert!(side_data != measured);
    }

    #[test]
    fn overflow_queries_do_not_measure_ordinary_inline_fragments() {
        use crate::css::css_pixels::{CssPixelRect, CssPixels};
        use crate::layout::node_data::NodeKind;
        use crate::painting::paintable_geometry;

        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.write_shape(node).set_kind(NodeKind::InlineNode);
        arena.populate_paintable_row(node);
        arena.scrollable_overflow.viewport.set(Some(node));
        let rect = CssPixelRect::new(
            CssPixels::from_integer(0),
            CssPixels::from_integer(0),
            CssPixels::from_integer(100),
            CssPixels::from_integer(80),
        );
        arena
            .paintable_rows_mut()
            .paintable_data_mut(node)
            .local_padding_box_union = rect.into();

        arena.measure_scrollable_overflow();
        let rows = arena.paintable_rows();
        assert_eq!(paintable_geometry::scrollable_overflow_rect(&rows, node), None);
        assert!(!paintable_geometry::has_scrollable_overflow(&rows, node));
        assert!(!arena.paintable_side_data(node).overflow_measured_this_commit.get());
    }

    #[test]
    fn inline_that_starts_storing_a_scroll_offset_is_measured_before_recording() {
        use crate::css::css_pixels::{CssPixelRect, CssPixels};
        use crate::layout::node_data::NodeKind;
        use crate::painting::paintable_geometry;

        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.write_shape(node).set_kind(NodeKind::InlineNode);
        arena.populate_paintable_row(node);
        arena.scrollable_overflow.viewport.set(Some(node));
        let rect = CssPixelRect::new(
            CssPixels::from_integer(0),
            CssPixels::from_integer(0),
            CssPixels::from_integer(100),
            CssPixels::from_integer(80),
        );
        arena
            .paintable_rows_mut()
            .paintable_data_mut(node)
            .local_padding_box_union = rect.into();
        arena.measure_scrollable_overflow();
        assert_eq!(
            paintable_geometry::scrollable_overflow_rect(&arena.paintable_rows(), node),
            None
        );

        arena.set_node_flag(node, NodeFlag::HasScrollOffset, true);
        arena.measure_scrollable_overflow();
        assert_eq!(
            paintable_geometry::scrollable_overflow_rect(&arena.paintable_rows(), node),
            Some(rect)
        );
    }

    #[test]
    fn overflow_is_measured_before_recording_while_geometry_is_borrowed() {
        use crate::css::css_pixels::{CssPixelRect, CssPixels};
        use crate::layout::node_data::NodeKind;
        use crate::painting::paintable_geometry;

        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.write_shape(node).set_kind(NodeKind::InlineNode);
        arena.set_node_flag(node, NodeFlag::HasScrollOffset, true);
        arena.populate_paintable_row(node);
        arena.scrollable_overflow.viewport.set(Some(node));
        let rect = CssPixelRect::new(
            CssPixels::from_integer(0),
            CssPixels::from_integer(0),
            CssPixels::from_integer(100),
            CssPixels::from_integer(80),
        );
        arena
            .paintable_rows_mut()
            .paintable_data_mut(node)
            .local_padding_box_union = rect.into();
        {
            let mut committed = arena.committed_side_data_mut(node);
            committed.overflow_relative_to_padding_box = FfiOverflowData {
                rect: CssPixelRect::new(rect.x, rect.y, CssPixels::from_integer(500), rect.height).into(),
                has_scrollable_overflow: true,
            };
            committed.overflow_valid_across_recommits = true;
        }
        arena.paintable_side_data(node).overflow_measured_this_commit.set(true);

        let rows = arena.paintable_rows();
        let geometry = rows.paintable_data(node);
        let previous_geometry = *geometry;
        rows.clear_cached_overflow_data(node);
        // Reading overflow never measures it.
        assert_eq!(paintable_geometry::scrollable_overflow_rect(&rows, node), None);
        assert!(!arena.scrollable_overflow.geometry_changed.get());
        arena.measure_scrollable_overflow();
        assert_eq!(paintable_geometry::scrollable_overflow_rect(&rows, node), Some(rect));
        assert!(!paintable_geometry::has_scrollable_overflow(&rows, node));
        assert_eq!(*geometry, previous_geometry);
        assert!(arena.scrollable_overflow.geometry_changed.get());
        assert!(arena.scrollable_overflow.scrollability_changed.get());
    }

    #[test]
    fn overflow_cache_invalidation_is_independent_of_borrowed_geometry() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.populate_paintable_row(node);
        arena.committed_side_data_mut(node).overflow_valid_across_recommits = true;
        arena.paintable_side_data(node).overflow_measured_this_commit.set(true);
        arena.paintable_rows_mut().begin_paintable_row_recommit(node);
        assert!(arena.committed_side_data(node).overflow_valid_across_recommits);

        let rows = arena.paintable_rows();
        let geometry = rows.paintable_data(node);
        let previous_geometry = *geometry;
        rows.clear_cached_overflow_data(node);
        assert_eq!(*geometry, previous_geometry);
        assert!(!arena.committed_side_data(node).overflow_valid_across_recommits);
        assert!(!arena.paintable_side_data(node).overflow_measured_this_commit.get());
    }
}

/// How the document's chrome state hears that a row was reset.
pub(crate) type ChromeStateCallback = (
    *mut c_void,
    unsafe extern "C" fn(*mut c_void, NodeSlotId, PaintableRowResetKind),
);

#[derive(Clone, Copy)]
pub(crate) struct PaintableRowReset {
    slot: NodeSlotId,
    kind: PaintableRowResetKind,
}

impl PaintableRowReset {
    /// Tells the document's chrome state, if it listens, that the row was reset.
    pub(crate) fn tell(self, main_thread: &crate::stage::MainThread) {
        let callback = main_thread
            .host_tables()
            .and_then(|host_tables| host_tables.chrome_state_callback.get());
        if let Some((context, callback)) = callback {
            // SAFETY: Registration and unregistration keep the callback context live.
            unsafe { callback(context, self.slot, self.kind) };
        }
    }
}

#[derive(Clone, Default)]
pub(crate) struct CommittedFragmentLinkSlot {
    layout_slot_generation: u8,
    geometry_epoch: u32,
    geometry_is_current: bool,
    link: Option<Arc<fragment_tree::FragmentLink>>,
}

/// Slots are the same when they hold the same link, or links that place the same fragment the
/// same way.
impl PartialEq for CommittedFragmentLinkSlot {
    fn eq(&self, other: &Self) -> bool {
        self.layout_slot_generation == other.layout_slot_generation
            && self.geometry_epoch == other.geometry_epoch
            && self.geometry_is_current == other.geometry_is_current
            && crate::cow_column::same_payload(self.link.as_ref(), other.link.as_ref(), |a, b| {
                a.places_same_fragment_identically_to(b)
            })
    }
}

// The unique node id of what each box is the box of, as the document names it: an element, the
// element a pseudo-element was generated for, or the document itself for the viewport. It is the
// name the compositor scrolls and snaps by, and it is not the style node id, which a node gives up
// when it disconnects, so the build stamps it onto a row from what the style mirror publishes.
//
// Dense by slot, because nearly every element box has one. Each entry names the row it was stamped
// for, so a slot that has been recycled since answers for the new row and not the old one.
#[derive(Clone, Default)]
pub(crate) struct UniqueNodeIdColumn {
    ids: RefCell<Vec<(NodeSlotId, i64)>>,
}

impl UniqueNodeIdColumn {
    pub(crate) fn id(&self, slot: NodeSlotId) -> i64 {
        match self.ids.borrow().get(slot.slot_index() as usize) {
            Some(&(stamped_for, id)) if !slot.is_invalid() && stamped_for == slot => id,
            _ => 0,
        }
    }

    pub(crate) fn publish(&self, slot: NodeSlotId, id: i64) {
        if slot.is_invalid() || self.id(slot) == id {
            return;
        }
        let index = slot.slot_index() as usize;
        let mut ids = self.ids.borrow_mut();
        if ids.len() <= index {
            ids.resize(index + 1, (NodeSlotId::INVALID, 0));
        }
        ids[index] = (slot, id);
    }
}

#[derive(Clone, Default)]
pub(crate) struct PaintableRowStore {
    rows: CowColumn<PaintableData, PAINTABLE_SLOTS_PER_CHUNK>,
    side_data: RefCell<Vec<PaintableSideData>>,
    committed_side_data: RefCell<CowColumn<CommittedSideData, PAINTABLE_SLOTS_PER_CHUNK>>,
    pub(crate) row_paint_states: RefCell<Vec<RowPaintState>>,
    pub(crate) damage: DamageSet,
    visual_context_records: RefCell<CowColumn<Option<PaintableVisualContextRecord>, PAINTABLE_SLOTS_PER_CHUNK>>,
    /// The visual context node handles of each row that has a record. They are kept here only, in
    /// a column a frame publishes, so the visual context update and the recording read the same.
    visual_context_node_handles: RefCell<VisualContextNodeHandleColumn>,
    pub(crate) stacking_context_entries: RefCell<StackingContextEntryColumn>,
    pub(crate) stacking_context_roots_flagged_for_resort: RefCell<Vec<NodeSlotId>>,
    line_roots_needing_fragment_ownership: RefCell<Vec<NodeSlotId>>,
    absolute_rect_memo: RefCell<Vec<Option<(NodeSlotId, u64, crate::css::css_pixels::CssPixelRect)>>>,
    absolute_rect_memo_epoch: Cell<u64>,
    committed_fragment_links: RefCell<CowColumn<CommittedFragmentLinkSlot, PAINTABLE_SLOTS_PER_CHUNK>>,
    image_map_areas: crate::painting::image_map_areas::ImageMapAreaColumn,
    unique_node_ids: UniqueNodeIdColumn,
    /// How many times a row was populated or reset, which moves whether a row is populated.
    population_writes: u64,
}

pub(crate) type VisualContextNodeHandleColumn =
    CowColumn<Option<Arc<BoxVisualContextNodeHandles>>, PAINTABLE_SLOTS_PER_CHUNK>;

/// Sets a row's node handles, copying its chunk only when they change.
fn set_visual_context_node_handles(
    column: &mut VisualContextNodeHandleColumn,
    index: usize,
    handles: Option<BoxVisualContextNodeHandles>,
) {
    if column
        .get(index)
        .is_none_or(|current| current.as_deref() == handles.as_ref())
    {
        return;
    }
    column
        .set(index, handles.map(Arc::new))
        .expect("the row is in the column");
}

pub(crate) struct PaintableRows<Arena> {
    arena: Arena,
}

pub(crate) type PaintableRowsRef<'a> = PaintableRows<&'a LayoutNodeArena>;
pub(crate) type PaintableRowsMut<'a> = PaintableRows<&'a mut LayoutNodeArena>;

impl Clone for PaintableRowsRef<'_> {
    fn clone(&self) -> Self {
        Self { arena: self.arena }
    }
}

/// A view of the live rows that also reads the rest of the arena.
pub(crate) trait PaintableRowsRead: PaintRead + Deref<Target = LayoutNodeArena> {}

/// A row's paintable data, for writing. See [`RowMut`].
pub(crate) type PaintableDataMut<'a> =
    RowMut<&'a mut CowColumn<PaintableData, PAINTABLE_SLOTS_PER_CHUNK>, PaintableData, PAINTABLE_SLOTS_PER_CHUNK>;

/// A row's committed side data, for writing. See [`RowMut`].
pub(crate) type CommittedSideDataMut<'a> = RowMut<
    RefMut<'a, CowColumn<CommittedSideData, PAINTABLE_SLOTS_PER_CHUNK>>,
    CommittedSideData,
    PAINTABLE_SLOTS_PER_CHUNK,
>;

pub(crate) trait PaintableRowsWrite: PaintableRowsRead {
    fn paintable_data_mut(&mut self, id: NodeSlotId) -> PaintableDataMut<'_>;
}

impl<Arena> Deref for PaintableRows<Arena>
where
    Arena: Deref<Target = LayoutNodeArena>,
{
    type Target = LayoutNodeArena;

    fn deref(&self) -> &Self::Target {
        self.arena.deref()
    }
}

impl<Arena> AsRef<LayoutNodeArena> for PaintableRows<Arena>
where
    Arena: Deref<Target = LayoutNodeArena>,
{
    fn as_ref(&self) -> &LayoutNodeArena {
        self
    }
}

impl<Arena> DerefMut for PaintableRows<Arena>
where
    Arena: DerefMut<Target = LayoutNodeArena>,
{
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.arena.deref_mut()
    }
}

impl<Arena> PaintableRows<Arena>
where
    Arena: Deref<Target = LayoutNodeArena>,
{
    pub(crate) fn clear_cached_overflow_data(&self, id: NodeSlotId) {
        if !self.paintable_row_is_populated(id) {
            return;
        }
        if std::mem::take(&mut self.arena.committed_side_data_mut(id).overflow_valid_across_recommits) {
            self.arena.note_row_overflow_unmeasured(id);
        }
    }

    /// A style repaint of a row also repaints the anonymous boxes it generated and, for an
    /// inline, the ancestors up to the line root that paints its pieces.
    pub(crate) fn for_each_row_repainted_with(&self, id: NodeSlotId, mut repaint: impl FnMut(NodeSlotId)) {
        if !self.paintable_row_is_populated(id) {
            return;
        }
        repaint(id);
        let mut stack: SmallVec<[NodeSlotId; 16]> = smallvec![id];
        while let Some(current) = stack.pop() {
            let mut child = crate::painting::paint_order::first_paint_child(self, current);
            while let Some(child_slot) = child {
                let child_flags = self.arena.node_flags_if_live(child_slot);
                if child_flags & NodeFlag::Anonymous as u32 != 0 {
                    repaint(child_slot);
                    stack.push(child_slot);
                }
                child = crate::painting::paint_order::next_paint_sibling(self, child_slot);
            }
        }
        if node_painting::is_fragmented_inline(self, id) {
            let mut ancestor = crate::painting::paint_order::paint_parent(self, id);
            while let Some(current) = ancestor {
                repaint(current);
                if node_painting::has_lines(self, current) {
                    break;
                }
                ancestor = crate::painting::paint_order::paint_parent(self, current);
            }
        }
    }

    /// A text-decoration change on a box repaints the text of every line root and inline
    /// below it that inherits the decoration.
    pub(crate) fn push_propagated_text_decoration_damage(&self, root: NodeSlotId) {
        if !self.paintable_row_is_populated(root) {
            return;
        }

        let mut stack = Vec::new();
        if let Some(first_child) = crate::painting::paint_order::first_paint_child(self, root) {
            stack.push(first_child);
        }
        while let Some(current) = stack.pop() {
            if let Some(next_sibling) = crate::painting::paint_order::next_paint_sibling(self, current) {
                stack.push(next_sibling);
            }

            if crate::painting::style_queries::is_text_decoration_propagation_boundary(self.arena.deref(), current) {
                continue;
            }
            if node_painting::has_lines(self, current) || node_painting::is_fragmented_inline(self, current) {
                self.arena.push_paint_damage(current, PaintDamage::DRAW_FOREGROUND);
            }
            if let Some(first_child) = crate::painting::paint_order::first_paint_child(self, current) {
                stack.push(first_child);
            }
        }
    }

    pub(crate) fn prepare_paintable_row_recommit_notification(&self, id: NodeSlotId) -> PaintableRowReset {
        assert!(self.paintable_row_is_populated(id));
        // The host hears which row is the viewport's here: a layout pass is under way, so it cannot read the rows.
        let kind = if id == self.arena.bound_viewport_row() {
            PaintableRowResetKind::ViewportRecommitted
        } else {
            PaintableRowResetKind::Recommitted
        };
        self.arena.prepare_paintable_row_reset(id, kind)
    }
}

impl<Arena> PaintableRows<Arena>
where
    Arena: DerefMut<Target = LayoutNodeArena>,
{
    pub(crate) fn paintable_data_mut(&mut self, id: NodeSlotId) -> PaintableDataMut<'_> {
        assert!(!id.is_invalid(), "invalid paintable arena slot ID");
        let data = self
            .arena
            .paintable_rows
            .rows
            .row_mut(id.slot_index() as usize)
            .expect("invalid paintable arena slot ID");
        assert_eq!(
            data.slot_generation,
            id.generation(),
            "paintable arena read a stale or unused slot"
        );
        data
    }

    pub(crate) fn begin_paintable_row_recommit(&mut self, id: NodeSlotId) {
        {
            let mut data = self.paintable_data_mut(id);
            data.offset = used_values::FfiCssPixelPoint::default();
            data.content_size = used_values::FfiCssPixelSize::default();
            data.local_padding_box_union = used_values::FfiCssPixelRect::default();
            data.local_border_box_union = used_values::FfiCssPixelRect::default();
        }
        // The row's damage is deliberately kept; the commit diff pushes what actually changed.
        let filter = {
            let mut committed = self.arena.committed_side_data_mut(id);
            committed.clear_committed_records();
            committed.fragment_ownership.take()
        };
        let mut side = self.arena.paintable_side_data_mut(id);
        side.overflow_measured_this_commit.set(false);
        if filter.is_some() {
            side.fragment_ownership_before_recommit = filter;
        }
    }
}

impl<Arena> PaintableRowsRead for PaintableRows<Arena> where Arena: Deref<Target = LayoutNodeArena> {}

impl<Arena> PaintableRowsWrite for PaintableRows<Arena>
where
    Arena: DerefMut<Target = LayoutNodeArena>,
{
    fn paintable_data_mut(&mut self, id: NodeSlotId) -> PaintableDataMut<'_> {
        PaintableRows::paintable_data_mut(self, id)
    }
}

impl CommittedFragmentLinkSlot {
    pub(crate) fn link(&self) -> Option<&fragment_tree::FragmentLink> {
        self.link.as_deref()
    }
}

impl PaintableRowStore {
    pub(crate) fn with_current_committed_fragment<R>(
        &self,
        layout_slot_index: u32,
        layout_slot_generation: u8,
        geometry_epoch: u32,
        read: impl FnOnce(&fragment_tree::Fragment) -> R,
    ) -> Option<R> {
        let slots = self.committed_fragment_links.borrow();
        let slot = slots.get(layout_slot_index as usize)?;
        if slot.layout_slot_generation != layout_slot_generation
            || !slot.geometry_is_current
            || slot.geometry_epoch != geometry_epoch
        {
            return None;
        }
        Some(read(&slot.link.as_deref()?.fragment))
    }

    pub(crate) fn invalidate_committed_geometry(&self, layout_slot_index: u32) {
        if let Some(mut slot) = RowMut::new(self.committed_fragment_links.borrow_mut(), layout_slot_index as usize) {
            slot.geometry_is_current = false;
        }
    }

    pub(crate) fn committed_fragment_link_cloned(
        &self,
        layout_slot_index: u32,
        layout_slot_generation: u8,
    ) -> Option<fragment_tree::FragmentLink> {
        self.committed_fragment_links
            .borrow()
            .get(layout_slot_index as usize)
            .filter(|slot| slot.layout_slot_generation == layout_slot_generation)
            .and_then(|slot| slot.link.as_deref().cloned())
    }

    pub(crate) fn with_committed_fragment_link<R>(
        &self,
        layout_slot_index: u32,
        layout_slot_generation: u8,
        read: impl FnOnce(Option<&fragment_tree::FragmentLink>) -> R,
    ) -> R {
        let slots = self.committed_fragment_links.borrow();
        read(
            slots
                .get(layout_slot_index as usize)
                .filter(|slot| slot.layout_slot_generation == layout_slot_generation)
                .and_then(|slot| slot.link.as_deref()),
        )
    }

    pub(crate) fn set_committed_fragment_link(
        &self,
        layout_slot_index: u32,
        layout_slot_generation: u8,
        geometry_epoch: Option<u32>,
        link: fragment_tree::FragmentLink,
    ) {
        let mut slots = self.committed_fragment_links.borrow_mut();
        slots.grow_to(layout_slot_index as usize + 1);
        let mut slot = slots
            .row_mut(layout_slot_index as usize)
            .expect("the column grew to hold the slot");
        if slot.layout_slot_generation != layout_slot_generation {
            *slot = CommittedFragmentLinkSlot {
                layout_slot_generation,
                link: Some(Arc::new(link)),
                ..Default::default()
            };
        } else if let Some(retained_link) = &mut slot.link {
            *Arc::make_mut(retained_link) = link;
        } else {
            slot.link = Some(Arc::new(link));
        }
        slot.geometry_epoch = geometry_epoch.unwrap_or_default();
        slot.geometry_is_current = geometry_epoch.is_some();
    }

    pub(crate) fn take_committed_fragment_link(
        &self,
        layout_slot_index: u32,
        layout_slot_generation: u8,
    ) -> Option<fragment_tree::FragmentLink> {
        RowMut::new(self.committed_fragment_links.borrow_mut(), layout_slot_index as usize)
            .filter(|slot| slot.layout_slot_generation == layout_slot_generation)
            .and_then(|mut slot| slot.link.take())
            .map(Arc::unwrap_or_clone)
    }

    pub(crate) fn reset_committed_fragment_link_slot(&self, layout_slot_index: u32) {
        self.committed_fragment_links
            .borrow_mut()
            .set(layout_slot_index as usize, CommittedFragmentLinkSlot::default());
    }
}

impl LayoutNodeArena {
    pub(crate) fn refresh_paint_order_inputs(&self, row: NodeSlotId) {
        if !self.paintable_row_is_populated(row) {
            return;
        }
        let inputs = crate::painting::paint_order_plan::PaintOrderInputs::gather(&self.paintable_rows(), row);
        if self.update_paint_order_inputs(row, inputs) {
            self.note_paint_order_changed(row);
        }
    }

    // A row whose ordering decisions changed is placed differently by its ancestors' plans and
    // may plan its own descendants differently.
    pub(crate) fn note_paint_order_changed(&self, row: NodeSlotId) {
        self.push_paint_damage(row, PaintDamage::ORDER);
        self.push_enclosing_paint_order_damage(row);
    }

    // The entry tables decide how a stacking context composes its hoisted content, so a table
    // change reorders the context's own painting even when no row changed its own decisions.
    pub(crate) fn note_stacking_context_composition_changed(&self, context_root: NodeSlotId) {
        self.push_paint_damage(context_root, PaintDamage::CONTEXT_ORDER);
    }

    pub(crate) fn paintable_rows(&self) -> PaintableRowsRef<'_> {
        PaintableRows { arena: self }
    }

    pub(crate) fn schedule_scrollable_overflow_recalculation(&self, node: NodeSlotId) {
        // SAFETY: The caller supplies a live slot; data() generation-checks every slot the
        // containing-block walk visits.
        let node_kind = self.data(node).kind.get();
        if crate::layout::node_facts::kind_is_box(node_kind) {
            let paintable_rows = self.paintable_rows();
            let mut containing_box = node;
            loop {
                if paintable_rows.paintable_row_is_populated(containing_box) {
                    paintable_rows.clear_cached_overflow_data(containing_box);
                }
                // SAFETY: As above.
                let Some(next) = self.node_containing_block_if_live(containing_box) else {
                    break;
                };
                containing_box = next;
            }
        }

        if self.needs_full_scrollable_overflow_recalculation.get() {
            return;
        }

        // NB: Cap the queue in case it's never consumed (e.g. forced style updates in a document
        //     that never updates layout).
        const MAX_PENDING_SCROLLABLE_OVERFLOW_RECALCULATIONS: usize = 1024;
        let mut boxes = self.boxes_needing_scrollable_overflow_recalculation.borrow_mut();
        if boxes.len() >= MAX_PENDING_SCROLLABLE_OVERFLOW_RECALCULATIONS {
            boxes.clear();
            self.needs_full_scrollable_overflow_recalculation.set(true);
            return;
        }

        if self.paintable_rows().paintable_row_is_populated(node) {
            boxes.push(node);
        }
    }

    pub(crate) fn set_needs_full_scrollable_overflow_recalculation(&self) {
        self.needs_full_scrollable_overflow_recalculation.set(true);
    }

    /// Whether a scrollable overflow recalculation is scheduled, which the next rendering preparation settles.
    pub(crate) fn scrollable_overflow_recalculation_is_scheduled(&self) -> bool {
        self.needs_full_scrollable_overflow_recalculation.get()
            || !self.boxes_needing_scrollable_overflow_recalculation.borrow().is_empty()
    }

    pub(crate) fn take_scrollable_overflow_recalculation_state(&self) -> (Vec<NodeSlotId>, bool) {
        (
            std::mem::take(&mut *self.boxes_needing_scrollable_overflow_recalculation.borrow_mut()),
            self.needs_full_scrollable_overflow_recalculation.replace(false),
        )
    }

    /// The layout tree's shape and paint facts, the paintable rows and the columns read beside them
    /// as they are now. The live columns go on being written, copying only the
    /// chunks the publication shares.
    pub(crate) fn publish_rows(&mut self) -> PublishedRows {
        let nodes = self.publish_paint_tree();
        let paint_facts = self.publish_paint_facts();
        let store = &mut self.paintable_rows;
        PublishedRows {
            geometry_epoch: store.absolute_rect_memo_epoch.get(),
            nodes,
            paint_facts,
            rows: store.rows.publish(),
            // Borrowed rather than reached through `&mut`, so a publication that a host read makes while
            // a writer of the columns is still on the stack panics instead of racing it.
            fragment_links: store.committed_fragment_links.borrow_mut().publish(),
            side_data: store.committed_side_data.borrow_mut().publish(),
            stacking_context_entries: store.stacking_context_entries.borrow_mut().publish(),
            visual_context_node_handles: store.visual_context_node_handles.borrow_mut().publish(),
        }
    }

    /// How far the paintable rows and the columns published beside them have been written. See
    /// [`crate::cow_column::CowColumn::version`].
    /// How far which rows are populated has been written since the arena was made.
    pub(crate) fn paintable_population_version(&self) -> u64 {
        self.paintable_rows.population_writes
    }

    pub(crate) fn paintable_rows_version(&self) -> u64 {
        let store = &self.paintable_rows;
        store.rows.version()
            + store.committed_fragment_links.borrow().version()
            + store.committed_side_data.borrow().version()
            + store.stacking_context_entries.borrow().version()
            + store.visual_context_node_handles.borrow().version()
    }

    pub(crate) fn paintable_rows_mut(&mut self) -> PaintableRowsMut<'_> {
        PaintableRows { arena: self }
    }

    pub(crate) fn with_committed_fragment_link<R>(
        &self,
        node: NodeSlotId,
        read: impl FnOnce(Option<&fragment_tree::FragmentLink>) -> R,
    ) -> R {
        debug_assert!(self.paintable_row_is_populated(node));
        let slots = self.paintable_rows.committed_fragment_links.borrow();
        read(
            slots
                .get(node.slot_index() as usize)
                .and_then(|entry| entry.link.as_deref()),
        )
    }

    pub(crate) fn with_committed_fragment_link_during_layout<R>(
        &self,
        node: NodeSlotId,
        read: impl FnOnce(Option<&fragment_tree::FragmentLink>) -> R,
    ) -> R {
        self.paintable_rows
            .with_committed_fragment_link(node.slot_index(), node.generation(), read)
    }

    fn prepare_paintable_row_reset(&self, slot: NodeSlotId, kind: PaintableRowResetKind) -> PaintableRowReset {
        PaintableRowReset { slot, kind }
    }

    pub(crate) fn memoized_absolute_rect(&self, id: NodeSlotId) -> Option<crate::css::css_pixels::CssPixelRect> {
        let store = &self.paintable_rows;
        let (memoized_id, memoized_epoch, rect) = store
            .absolute_rect_memo
            .borrow()
            .get(id.slot_index() as usize)
            .copied()
            .flatten()?;
        (memoized_id == id && memoized_epoch == store.absolute_rect_memo_epoch.get()).then_some(rect)
    }

    pub(crate) fn memoize_absolute_rect(&self, id: NodeSlotId, rect: crate::css::css_pixels::CssPixelRect) {
        let store = &self.paintable_rows;
        store.absolute_rect_memo.borrow_mut()[id.slot_index() as usize] =
            Some((id, store.absolute_rect_memo_epoch.get(), rect));
    }

    pub(crate) fn clear_absolute_rect_memo(&self) {
        let epoch = &self.paintable_rows.absolute_rect_memo_epoch;
        epoch.set(epoch.get().checked_add(1).expect("absolute rect memo epoch overflowed"));
    }

    pub(crate) fn paintable_row_count(&self) -> usize {
        self.paintable_rows.side_data.borrow().len()
    }

    pub(crate) fn populate_paintable_row(&mut self, layout_node: NodeSlotId) {
        self.note_overflow_contained_box_added(layout_node);
        let overflow_style = self
            .node_style_if_live(layout_node)
            .map(crate::painting::scrollable_overflow::OverflowStyle::new);
        let store = &mut self.paintable_rows;
        let index = layout_node.slot_index() as usize;
        let mut side_data = store.side_data.borrow_mut();
        let mut row_paint_states = store.row_paint_states.borrow_mut();
        let mut absolute_rect_memo = store.absolute_rect_memo.borrow_mut();
        let mut visual_context_records = store.visual_context_records.borrow_mut();
        store.rows.grow_to(index + 1);
        visual_context_records.grow_to(index + 1);
        store.committed_side_data.get_mut().grow_to(index + 1);
        store.visual_context_node_handles.get_mut().grow_to(index + 1);
        store.stacking_context_entries.get_mut().grow_to(index + 1);
        while side_data.len() <= index {
            side_data.push(PaintableSideData::default());
            row_paint_states.push(RowPaintState::default());
            absolute_rect_memo.push(None);
        }

        store.population_writes += 1;
        store.rows.set(
            index,
            PaintableData {
                slot_generation: layout_node.generation(),
                ..PaintableData::default()
            },
        );
        side_data[index] = PaintableSideData {
            overflow_style,
            ..Default::default()
        };
        store
            .committed_side_data
            .get_mut()
            .set(index, CommittedSideData::default());
        row_paint_states[index].clear();
        absolute_rect_memo[index] = None;
        visual_context_records.set(index, None);
        set_visual_context_node_handles(store.visual_context_node_handles.get_mut(), index, None);
        drop_table(store.stacking_context_entries.get_mut(), index);
        self.scrollable_overflow.rows_to_measure.get_mut().push(layout_node);
        drop((side_data, row_paint_states, absolute_rect_memo, visual_context_records));
        self.notify_committed_box_changed(layout_node);
    }

    fn reset_paintable_row(&mut self, row_is_still_linked: bool, reset: PaintableRowReset) {
        let id = reset.slot;
        if row_is_still_linked {
            // A cleared row is still linked, so the ancestor whose plans listed it is known now.
            self.push_enclosing_paint_order_damage(id);
        }
        if let Some((record, node_handles)) = self.take_paintable_visual_context_record(id) {
            self.withdraw_stacking_context_state_of_reset_row(id, Some(record.stacking_context));
            let former_paint_parent =
                crate::painting::paint_order::paint_parent(&self.paintable_rows(), id).unwrap_or(NodeSlotId::INVALID);
            self.paint_state()
                .borrow_mut()
                .visual_context
                .dirty_boxes
                .note_removed(RemovedBoxBlocks {
                    slot: id,
                    node_handles: Arc::unwrap_or_clone(node_handles),
                    former_paint_parent,
                });
        } else {
            self.withdraw_stacking_context_state_of_reset_row(id, None);
            self.paint_state()
                .borrow_mut()
                .visual_context
                .dirty_boxes
                .forget_box(id);
        }
        self.clear_absolute_rect_memo();
        let store = &mut self.paintable_rows;
        let index = id.slot_index() as usize;
        store.population_writes += 1;
        store.rows.set(index, PaintableData::default());
        store.side_data.borrow_mut()[index] = PaintableSideData::default();
        store
            .committed_side_data
            .get_mut()
            .set(index, CommittedSideData::default());
        store.row_paint_states.borrow()[index].clear();
        store.visual_context_records.borrow_mut().set(index, None);
        set_visual_context_node_handles(store.visual_context_node_handles.get_mut(), index, None);
        drop_table(store.stacking_context_entries.get_mut(), index);
        if reset.kind == crate::painting::paintable_data::PaintableRowResetKind::Freed {
            store.image_map_areas.forget(id);
        }
        self.notify_committed_box_changed(id);
    }

    /// The areas of the image map each image is associated with, as the document published them.
    pub(crate) fn image_map_areas(&self) -> &crate::painting::image_map_areas::ImageMapAreaColumn {
        &self.paintable_rows.image_map_areas
    }

    /// The unique node id of what each box is the box of, as the build stamped it.
    pub(crate) fn unique_node_ids(&self) -> &UniqueNodeIdColumn {
        &self.paintable_rows.unique_node_ids
    }

    pub(crate) fn paintable_visual_context_record(
        &self,
        id: NodeSlotId,
    ) -> Option<Ref<'_, PaintableVisualContextRecord>> {
        if !self.paintable_row_is_populated(id) {
            return None;
        }
        Ref::filter_map(self.paintable_rows.visual_context_records.borrow(), |records| {
            records.get(id.slot_index() as usize).and_then(Option::as_ref)
        })
        .ok()
    }

    /// A row's visual context record and node handles, which the row no longer has.
    pub(crate) fn take_paintable_visual_context_record(
        &self,
        id: NodeSlotId,
    ) -> Option<(PaintableVisualContextRecord, Arc<BoxVisualContextNodeHandles>)> {
        if !self.paintable_row_is_populated(id) {
            return None;
        }
        let index = id.slot_index() as usize;
        let record = self
            .paintable_rows
            .visual_context_records
            .borrow_mut()
            .row_mut(index)
            .and_then(|mut record| record.take())?;
        let node_handles = self
            .paintable_rows
            .visual_context_node_handles
            .borrow_mut()
            .row_mut(index)
            .and_then(|mut handles| handles.take())
            .expect("a row with a record has node handles");
        Some((record, node_handles))
    }

    pub(crate) fn set_paintable_visual_context_record(
        &self,
        id: NodeSlotId,
        record: PaintableVisualContextRecord,
        node_handles: BoxVisualContextNodeHandles,
    ) {
        debug_assert!(self.paintable_row_is_populated(id));
        let inputs = self.committed_side_data(id).prepared_order_inputs().map(|inputs| {
            inputs.with_visual_context(
                &record.stacking_context,
                crate::painting::style_queries::z_index(self, id),
            )
        });
        set_visual_context_node_handles(
            &mut self.paintable_rows.visual_context_node_handles.borrow_mut(),
            id.slot_index() as usize,
            Some(node_handles),
        );
        self.paintable_rows
            .visual_context_records
            .borrow_mut()
            .set(id.slot_index() as usize, Some(record));
        if let Some(inputs) = inputs {
            if self.update_paint_order_inputs(id, inputs) {
                self.note_paint_order_changed(id);
            }
        } else {
            // Some resource paintables are first prepared outside a layout commit.
            self.refresh_paint_order_inputs(id);
        }
    }

    pub(crate) fn drop_all_visual_context_records(&self) {
        let rows = self.paintable_rows.side_data.borrow().len();
        let mut records = self.paintable_rows.visual_context_records.borrow_mut();
        for index in 0..rows {
            records.set(index, None);
        }
        drop(records);
        let mut handles = self.paintable_rows.visual_context_node_handles.borrow_mut();
        for index in 0..self.paintable_row_count() {
            set_visual_context_node_handles(&mut handles, index, None);
        }
    }

    /// A snapshot of every row's visual context node handles, as they are now, which later writes leave as it is.
    pub(crate) fn visual_context_node_handles_snapshot(
        &self,
    ) -> crate::cow_column::ColumnSnapshot<Option<Arc<BoxVisualContextNodeHandles>>, PAINTABLE_SLOTS_PER_CHUNK> {
        self.paintable_rows.visual_context_node_handles.borrow_mut().publish()
    }

    /// A row's visual context node handles, if it has a visual context record.
    pub(crate) fn paintable_visual_context_node_handles(
        &self,
        id: NodeSlotId,
    ) -> Option<Ref<'_, BoxVisualContextNodeHandles>> {
        if !self.paintable_row_is_populated(id) {
            return None;
        }
        Ref::filter_map(self.paintable_rows.visual_context_node_handles.borrow(), |column| {
            column.get(id.slot_index() as usize).and_then(Option::as_deref)
        })
        .ok()
    }

    /// The effect and spatial nodes of the box `id`, which compositor animations of their kinds drive.
    pub(crate) fn box_animation_nodes(&self, id: NodeSlotId) -> BoxAnimationNodes {
        self.with_paintable_visual_context_node_handles(id, |handles| BoxAnimationNodes {
            effects: handles.effects.iter().map(|index| index.0).collect(),
            spatial: handles.spatial.iter().map(|index| index.0).collect(),
        })
    }

    pub(crate) fn with_paintable_visual_context_node_handles<R>(
        &self,
        id: NodeSlotId,
        read: impl FnOnce(&BoxVisualContextNodeHandles) -> R,
    ) -> R {
        match self.paintable_visual_context_node_handles(id) {
            Some(handles) => read(&handles),
            None => read(&EMPTY_BOX_VISUAL_CONTEXT_NODE_HANDLES),
        }
    }

    /// The nodes of the box that an animation of the kind drives: the effect nodes of an opacity,
    /// background color or filter animation, or the spatial nodes of a transform animation, as far
    /// as the tree holds nodes of the right kind for them.
    pub(crate) fn paintable_visual_animation_target_indices(
        &self,
        id: NodeSlotId,
        tree: Option<&crate::painting::visual_context::VisualContextTree>,
        target_kind: crate::painting::host::FfiVisualAnimationTargetKind,
    ) -> Vec<u32> {
        use crate::painting::host::FfiVisualAnimationTargetKind;
        let Some(tree) = tree else {
            return Vec::new();
        };
        self.with_paintable_visual_context_node_handles(id, |handles| {
            let valid_targets = |indices: &mut dyn Iterator<Item = u32>| -> Vec<u32> {
                indices
                    .filter(|&index| tree.visual_animation_target_is_valid(target_kind, index))
                    .collect()
            };
            match target_kind {
                FfiVisualAnimationTargetKind::Opacity
                | FfiVisualAnimationTargetKind::BackgroundColor
                | FfiVisualAnimationTargetKind::Filter => {
                    valid_targets(&mut handles.effects.iter().map(|index| index.0))
                }
                FfiVisualAnimationTargetKind::Transform => {
                    valid_targets(&mut handles.spatial.iter().map(|index| index.0))
                }
            }
        })
    }

    pub(crate) fn mark_paintable_subtree_may_own_geometry_dependent_nodes(&self, id: NodeSlotId) -> Option<bool> {
        if !self.paintable_row_is_populated(id) {
            return None;
        }
        let mut records = self.paintable_rows.visual_context_records.borrow_mut();
        let mut row = records.row_mut(id.slot_index() as usize)?;
        let record = row.as_mut()?;
        let was_flagged = record.subtree_may_own_geometry_dependent_nodes;
        record.subtree_may_own_geometry_dependent_nodes = true;
        Some(was_flagged)
    }

    pub(crate) fn set_paintable_record_stacking_context_contribution_registered(&self, id: NodeSlotId) {
        debug_assert!(self.paintable_row_is_populated(id));
        let mut records = self.paintable_rows.visual_context_records.borrow_mut();
        if let Some(mut row) = records.row_mut(id.slot_index() as usize)
            && let Some(record) = row.as_mut()
        {
            record.stacking_context.contribution_is_registered = true;
        }
    }

    pub(crate) fn note_line_root_needs_fragment_ownership(&self, line_root: NodeSlotId) {
        self.paintable_rows
            .line_roots_needing_fragment_ownership
            .borrow_mut()
            .push(line_root);
    }

    pub(crate) fn take_line_roots_needing_fragment_ownership(&self) -> Vec<NodeSlotId> {
        std::mem::take(&mut *self.paintable_rows.line_roots_needing_fragment_ownership.borrow_mut())
    }

    pub(crate) fn note_visual_context_box_dirty(&self, id: NodeSlotId, kind: VisualContextBoxDirtyKind) {
        let pending_box_limit = self
            .paintable_row_count()
            .max(crate::painting::visual_context::dirty::MINIMUM_PENDING_DIRTY_BOX_LIMIT);
        self.paint_state()
            .borrow_mut()
            .visual_context
            .dirty_boxes
            .note_box(id, kind, pending_box_limit);
    }

    pub(crate) fn request_full_visual_context_rebuild(&self, scope: VisualContextUpdateScope) {
        self.paint_state()
            .borrow_mut()
            .visual_context
            .dirty_boxes
            .request_full_rebuild(scope);
    }

    pub(crate) fn prepare_paintable_row_freed_reset(&self, layout_slot_index: u32) -> Option<PaintableRowReset> {
        if layout_slot_index as usize >= self.paintable_row_count() {
            return None;
        }
        let generation = self.paintable_data_by_index(layout_slot_index).slot_generation;
        if generation == 0 {
            return None;
        }
        Some(self.prepare_paintable_row_reset(
            NodeSlotId::new(layout_slot_index, generation),
            PaintableRowResetKind::Freed,
        ))
    }

    pub(crate) fn paintable_row_freed(&mut self, reset: PaintableRowReset) {
        self.reset_paintable_row(false, reset);
    }

    pub(crate) fn prepare_paintable_row_cleared_reset(&self, layout_node: NodeSlotId) -> Option<PaintableRowReset> {
        self.paintable_row_is_populated(layout_node)
            .then(|| self.prepare_paintable_row_reset(layout_node, PaintableRowResetKind::Cleared))
    }

    pub(crate) fn paintable_row_cleared(&mut self, reset: PaintableRowReset) {
        if !self.scrollable_overflow.non_child_boxes.borrow().is_empty() {
            self.scrollable_overflow.contained_boxes_dirty.set(true);
        }
        self.reset_paintable_row(true, reset);
    }

    pub(crate) fn paintable_row_is_populated(&self, id: NodeSlotId) -> bool {
        if id.is_invalid() {
            return false;
        }
        self.paintable_rows
            .rows
            .get(id.slot_index() as usize)
            .is_some_and(|data| data.slot_generation != 0 && data.slot_generation == id.generation())
    }

    pub(crate) fn live_paintable_data(&self, id: NodeSlotId) -> &PaintableData {
        assert!(!id.is_invalid(), "invalid paintable arena slot ID");
        let data = self
            .paintable_rows
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

    fn paintable_data_by_index(&self, index: u32) -> &PaintableData {
        self.paintable_rows
            .rows
            .get(index as usize)
            .expect("invalid paintable arena slot index")
    }

    pub(crate) fn transfer_fragments_to_replacement_node(
        &self,
        containing_block: NodeSlotId,
        old_node: NodeSlotId,
        new_node: NodeSlotId,
    ) {
        if !self.paintable_row_is_populated(containing_block) {
            return;
        }
        self.push_paint_damage(containing_block, PaintDamage::ALL_PRODUCERS);
        let mut side = self.committed_side_data_mut(containing_block);
        let Some(content) = side.inline_content.as_mut() else {
            return;
        };
        // Replacement preserves current paint geometry until the next layout commit. Give that
        // version new node identities without modifying retained run outputs or copying glyphs.
        let content = std::sync::Arc::make_mut(content);
        for fragment in &mut content.fragments {
            if fragment.layout_node == old_node {
                fragment.layout_node = new_node;
            }
        }
        for piece in &mut content.inline_box_pieces {
            if piece.node == old_node {
                piece.node = new_node;
            }
        }
    }

    pub(crate) fn push_propagated_text_decoration_damage(&self, root: NodeSlotId) {
        self.paintable_rows().push_propagated_text_decoration_damage(root);
    }

    /// Stores the paint-order inputs gathered for a row, answering whether they changed.
    pub(crate) fn update_paint_order_inputs(
        &self,
        row: NodeSlotId,
        inputs: crate::painting::paint_order_plan::PaintOrderInputs,
    ) -> bool {
        std::mem::replace(&mut self.committed_side_data_mut(row).order_inputs, inputs) != inputs
    }

    pub(crate) fn committed_side_data(&self, id: NodeSlotId) -> Ref<'_, CommittedSideData> {
        debug_assert!(self.paintable_row_is_populated(id));
        Ref::map(self.paintable_rows.committed_side_data.borrow(), |side_data| {
            side_data
                .get(id.slot_index() as usize)
                .expect("a populated row has side data")
        })
    }

    /// A populated row's committed side data, for writing. See [`RowMut`].
    pub(crate) fn committed_side_data_mut(&self, id: NodeSlotId) -> CommittedSideDataMut<'_> {
        debug_assert!(self.paintable_row_is_populated(id));
        RowMut::new(
            self.paintable_rows.committed_side_data.borrow_mut(),
            id.slot_index() as usize,
        )
        .expect("a populated row has side data")
    }

    pub(crate) fn paintable_side_data(&self, id: NodeSlotId) -> Ref<'_, PaintableSideData> {
        debug_assert!(self.paintable_row_is_populated(id));
        Ref::map(self.paintable_rows.side_data.borrow(), |side_data| {
            &side_data[id.slot_index() as usize]
        })
    }

    pub(crate) fn paintable_side_data_mut(&self, id: NodeSlotId) -> RefMut<'_, PaintableSideData> {
        debug_assert!(self.paintable_row_is_populated(id));
        RefMut::map(self.paintable_rows.side_data.borrow_mut(), |side_data| {
            &mut side_data[id.slot_index() as usize]
        })
    }
}

pub(crate) fn with_inline_pieces(
    arena: &impl GeometryRead,
    inline_paintable: NodeSlotId,
    mut callback: impl FnMut(&InlineBoxPieceRecord, &PaintableData) -> bool,
) {
    let Some(root) = arena.inline_pieces_root(inline_paintable) else {
        return;
    };
    let data = arena.paintable_data(inline_paintable);
    let root_side = arena.committed_side_data(root);
    for piece_index in arena.committed_side_data(inline_paintable).piece_indices() {
        let piece = &root_side.inline_box_pieces()[*piece_index as usize];
        if !callback(piece, data) {
            return;
        }
    }
}

/// The effect and spatial nodes of a box, which compositor animations of their kinds drive.
pub(crate) struct BoxAnimationNodes {
    pub(crate) effects: smallvec::SmallVec<[u32; 2]>,
    pub(crate) spatial: smallvec::SmallVec<[u32; 2]>,
}

impl BoxAnimationNodes {
    /// Whether `animation` drives a node of the box: a transform animation one of its spatial nodes, any other one of
    /// its effect nodes.
    pub(crate) fn driven_by(&self, animation: &crate::painting::visual_animation::VisualAnimation) -> bool {
        let nodes = match animation.target_kind {
            crate::painting::host::FfiVisualAnimationTargetKind::Transform => &self.spatial,
            _ => &self.effects,
        };
        animation.node_indices.iter().any(|node| nodes.contains(node))
    }
}
