/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! When each row's paint output last had reason to change.
//!
//! A write to what a row paints stamps the row with the document's current paint generation, and stamps each paint
//! ancestor with it as something that changed below, up to the first one already stamped so in that generation. A
//! reader takes a generation as it reads the rows, and asks later whether a stamp is newer: a recording reuses what one
//! recorded as of an older generation where nothing it read was stamped since. Nothing is consumed or cleared, so each
//! reader keeps its own "as of", and a write stamped that changed nothing costs a recording that comes out the same.

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::node_painting;
use crate::painting::paint_order;
use crate::painting::paint_read::GeometryRead;

pub(crate) type PaintGeneration = u64;

/// What a write changed of a row, which says what learns of it besides the recordings: what the row draws, what it
/// tells the compositor about scrolling, what it can be hit by, its place in paint order and how it places what it
/// holds, or where its boxes are.
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub(crate) struct RowPaintChange(u8);

impl RowPaintChange {
    pub(crate) const NONE: Self = Self(0);
    pub(crate) const DRAWING: Self = Self(1 << 0);
    pub(crate) const SCROLL_METADATA: Self = Self(1 << 1);
    pub(crate) const HIT_TESTING: Self = Self(1 << 2);
    pub(crate) const ORDER: Self = Self(1 << 3);
    pub(crate) const PLACE: Self = Self(1 << 4);
    pub(crate) const EVERYTHING_IT_PAINTS: Self = Self(Self::DRAWING.0 | Self::SCROLL_METADATA.0 | Self::HIT_TESTING.0);

    pub(crate) const fn is_empty(self) -> bool {
        self.0 == 0
    }

    pub(crate) const fn intersects(self, other: Self) -> bool {
        self.0 & other.0 != 0
    }
}

impl std::ops::BitOr for RowPaintChange {
    type Output = Self;

    fn bitor(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

impl std::ops::BitOrAssign for RowPaintChange {
    fn bitor_assign(&mut self, other: Self) {
        self.0 |= other.0;
    }
}

impl std::fmt::Debug for RowPaintChange {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        const NAMES: [(RowPaintChange, &str); 5] = [
            (RowPaintChange::DRAWING, "drawing"),
            (RowPaintChange::SCROLL_METADATA, "scroll-metadata"),
            (RowPaintChange::HIT_TESTING, "hit-testing"),
            (RowPaintChange::ORDER, "order"),
            (RowPaintChange::PLACE, "place"),
        ];
        let mut list = formatter.debug_set();
        for (change, name) in NAMES {
            if self.intersects(change) {
                list.entry(&format_args!("{name}"));
            }
        }
        list.finish()
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct PaintChangeStamps {
    /// What the row's own producers read: its style, its geometry, its paint facts and where it is.
    pub(crate) inputs_changed_at: PaintGeneration,
    /// What the row tells the compositor about scrolling, which only its scroll metadata producer reads.
    pub(crate) scroll_metadata_changed_at: PaintGeneration,
    /// Its place in paint order, and how it places what it holds.
    pub(crate) order_changed_at: PaintGeneration,
    /// Anything in its paint subtree.
    pub(crate) changed_below_at: PaintGeneration,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct RowChanges {
    pub(crate) inputs: bool,
    pub(crate) scroll_metadata: bool,
    pub(crate) order: bool,
    pub(crate) below: bool,
}

impl RowChanges {
    pub(crate) const NONE: Self = Self {
        inputs: false,
        scroll_metadata: false,
        order: false,
        below: false,
    };

    pub(crate) fn any(self) -> bool {
        self.inputs || self.scroll_metadata || self.order || self.below
    }
}

impl PaintChangeStamps {
    pub(crate) fn of_a_row_new_to_the_document(generation_writes_stamp_with: PaintGeneration) -> Self {
        Self {
            inputs_changed_at: generation_writes_stamp_with,
            scroll_metadata_changed_at: generation_writes_stamp_with,
            order_changed_at: generation_writes_stamp_with,
            changed_below_at: 0,
        }
    }

    pub(crate) fn changes_since(&self, as_of: PaintGeneration) -> RowChanges {
        RowChanges {
            inputs: self.inputs_changed_at > as_of,
            scroll_metadata: self.scroll_metadata_changed_at > as_of,
            order: self.order_changed_at > as_of,
            below: self.changed_below_at > as_of,
        }
    }

    fn newest(&self) -> PaintGeneration {
        self.inputs_changed_at
            .max(self.scroll_metadata_changed_at)
            .max(self.order_changed_at)
            .max(self.changed_below_at)
    }

    pub(crate) fn row_or_anything_below_it_changed_since(&self, as_of: PaintGeneration) -> bool {
        self.newest() > as_of
    }
}

#[derive(Clone)]
pub(crate) struct PaintGenerations {
    generation_writes_stamp_with: PaintGeneration,
    everything_changed_at: PaintGeneration,
}

impl Default for PaintGenerations {
    fn default() -> Self {
        Self {
            generation_writes_stamp_with: 1,
            everything_changed_at: 1,
        }
    }
}

impl PaintGenerations {
    pub(crate) fn generation_writes_stamp_with(&self) -> PaintGeneration {
        self.generation_writes_stamp_with
    }
}

#[derive(Clone, Copy, PartialEq)]
enum OwnInkOverflow {
    MayChange,
    Kept,
}

impl LayoutNodeArena {
    pub(crate) fn take_paint_generation(&mut self) -> PaintGeneration {
        let generations = &mut self.paintable_rows.paint_generations;
        let taken = generations.generation_writes_stamp_with;
        generations.generation_writes_stamp_with = taken.checked_add(1).expect("paint generations do not run out");
        taken
    }

    pub(crate) fn paint_change_stamps(&self, row: NodeSlotId) -> PaintChangeStamps {
        if !self.paintable_row_is_populated(row) {
            return PaintChangeStamps::default();
        }
        self.paintable_rows
            .paint_change_stamps
            .get(row.slot_index() as usize)
            .copied()
            .unwrap_or_default()
    }

    pub(crate) fn everything_paints_anew_since(&self, as_of: PaintGeneration) -> bool {
        self.paintable_rows.paint_generations.everything_changed_at > as_of
    }

    pub(crate) fn everything_changed_at(&self) -> PaintGeneration {
        self.paintable_rows.paint_generations.everything_changed_at
    }

    fn stamp(&mut self, row: NodeSlotId, stamp: impl FnOnce(&mut PaintChangeStamps, PaintGeneration)) {
        let current = self.paintable_rows.paint_generations.generation_writes_stamp_with;
        let index = row.slot_index() as usize;
        let mut stamps = self.paint_change_stamps(row);
        stamp(&mut stamps, current);
        self.paintable_rows
            .paint_change_stamps
            .set(index, stamps)
            .expect("a populated row has stamps");
    }

    pub(crate) fn note_paint_change(&mut self, row: NodeSlotId, change: RowPaintChange) {
        self.note_paint_change_to_row(row, change, OwnInkOverflow::MayChange);
    }

    /// Notes a change that leaves the ink overflow of the row itself, relative to its padding box, as it was.
    pub(crate) fn note_paint_change_keeping_own_ink_overflow(&mut self, row: NodeSlotId, change: RowPaintChange) {
        self.note_paint_change_to_row(row, change, OwnInkOverflow::Kept);
    }

    fn note_paint_change_to_row(&mut self, row: NodeSlotId, change: RowPaintChange, own_ink_overflow: OwnInkOverflow) {
        if change.is_empty() || !self.paintable_row_is_populated(row) {
            return;
        }
        if change.intersects(RowPaintChange::HIT_TESTING | RowPaintChange::PLACE | RowPaintChange::ORDER) {
            self.note_hit_test_items_changed();
        }
        if change.intersects(RowPaintChange::DRAWING | RowPaintChange::PLACE | RowPaintChange::ORDER) {
            if own_ink_overflow == OwnInkOverflow::MayChange {
                self.request_ink_overflow_recalculation(row);
            }
            self.request_inline_ink_overflow_recalculation(row);
            if change.intersects(RowPaintChange::PLACE) {
                self.request_containing_block_ink_overflow_recalculation(row);
            }
        }
        let changes_inputs = change.intersects(RowPaintChange::DRAWING | RowPaintChange::PLACE);
        let changes_scroll_metadata = change.intersects(RowPaintChange::SCROLL_METADATA);
        let changes_order = change.intersects(RowPaintChange::ORDER);
        if !changes_inputs && !changes_scroll_metadata && !changes_order {
            return;
        }
        self.stamp(row, |stamps, current| {
            if changes_inputs {
                stamps.inputs_changed_at = current;
            }
            if changes_scroll_metadata {
                stamps.scroll_metadata_changed_at = current;
            }
            if changes_order {
                stamps.order_changed_at = current;
            }
        });
        self.stamp_paint_ancestors_as_changed_below(row);
    }

    // Producers that read what is below them, such as an SVG root or a snap container's scroll metadata, learn of a
    // change below them from this stamp. The walk stops at the first ancestor stamped so in this generation.
    fn stamp_paint_ancestors_as_changed_below(&mut self, row: NodeSlotId) {
        let current = self.paintable_rows.paint_generations.generation_writes_stamp_with;
        let mut current_row = row;
        while let Some(ancestor) = self.paint_ancestor_for_change_stamps(current_row) {
            if self.paint_change_stamps(ancestor).changed_below_at == current {
                break;
            }
            self.stamp(ancestor, |stamps, current| stamps.changed_below_at = current);
            current_row = ancestor;
        }
    }

    // Resource subtrees (masks, clips, patterns) are not painted where they sit in the layout tree but by the element
    // referencing them; the stamp continues from the nearest painted layout ancestor so a change inside the resource
    // reaches the content painted around it.
    fn paint_ancestor_for_change_stamps(&self, row: NodeSlotId) -> Option<NodeSlotId> {
        let rows = self.paintable_rows();
        if let Some(parent) = paint_order::paint_parent(&rows, row) {
            return Some(parent);
        }
        if !self
            .node_kind_if_live(row)
            .is_some_and(crate::layout::node_facts::kind_is_svg_resource_box)
        {
            return None;
        }
        let mut ancestor = self.node_parent_if_live(row);
        while let Some(node) = ancestor {
            if rows.paintable_row_is_populated(node) {
                return Some(node);
            }
            ancestor = self.node_parent_if_live(node);
        }
        None
    }

    pub(crate) fn note_paint_change_in_paint_subtree(&mut self, root: NodeSlotId, change: RowPaintChange) {
        let mut subtree = Vec::new();
        paint_order::for_each_in_paint_subtree(&self.paintable_rows(), root, |row| subtree.push(row));
        for row in subtree {
            self.note_paint_change(row, change);
        }
    }

    /// The fan-out of a style repaint: the row, the anonymous boxes it generated, and for an inline the ancestors up to
    /// the line root that paints its pieces.
    pub(crate) fn note_paint_change_for_repaint(&mut self, row: NodeSlotId, change: RowPaintChange) {
        let mut repainted_rows = smallvec::SmallVec::<[NodeSlotId; 4]>::new();
        self.paintable_rows()
            .for_each_row_repainted_with(row, |repainted| repainted_rows.push(repainted));
        for repainted in repainted_rows {
            self.note_paint_change(repainted, change);
        }
    }

    /// The row's producers and scopes are listed by the plans of its nearest paint ancestor; resolved now, while the
    /// links are intact, for rows about to appear, disappear or move. The ancestor's content changed below it as well.
    pub(crate) fn note_enclosing_paint_order_change(&mut self, node: NodeSlotId) {
        if let Some(owner) = self.enclosing_paint_order_owner(node) {
            // A line's plan also places content nested inside ordinary inline boxes. Changes there must reach the
            // block or self-painting inline that owns it.
            let rows = self.paintable_rows();
            let line_owner = node_painting::is_fragmented_inline(&rows, owner)
                .then(|| {
                    crate::painting::fragment_ownership::nearest_self_painting_inline_box(&rows, node)
                        .unwrap_or(rows.paintable_data(owner).containing_block)
                })
                .filter(|line_owner| !line_owner.is_invalid());
            self.note_paint_change(owner, RowPaintChange::ORDER);
            if self.paintable_row_is_populated(owner) {
                self.stamp(owner, |stamps, current| stamps.changed_below_at = current);
            }
            if let Some(line_owner) = line_owner {
                self.note_paint_change(line_owner, RowPaintChange::ORDER);
            }
        }
    }

    fn enclosing_paint_order_owner(&self, node: NodeSlotId) -> Option<NodeSlotId> {
        let rows = self.paintable_rows();
        if rows.paintable_row_is_populated(node) {
            return paint_order::paint_parent(&rows, node);
        }
        let mut ancestor = self.node_parent_if_live(node);
        while let Some(current) = ancestor {
            if rows.paintable_row_is_populated(current) {
                return Some(current);
            }
            ancestor = self.node_parent_if_live(current);
        }
        None
    }

    pub(crate) fn note_hit_test_items_changed(&mut self) {
        self.paintable_rows.hit_test_items_stamp = crate::cow_column::new_edit_token();
    }

    pub(crate) fn note_everything_paints_anew(&mut self) {
        self.note_hit_test_items_changed();
        let generations = &mut self.paintable_rows.paint_generations;
        generations.everything_changed_at = generations.generation_writes_stamp_with;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeKind;

    fn populated_child(arena: &mut LayoutNodeArena, parent: NodeSlotId) -> NodeSlotId {
        let row = arena.allocate_for_test().slot;
        arena.write_shape(row).set_kind(NodeKind::Box);
        if !parent.is_invalid() {
            arena.insert_child(parent, row, NodeSlotId::INVALID);
        }
        arena.populate_paintable_row(row);
        row
    }

    #[test]
    fn a_write_is_newer_than_every_generation_a_reader_took_before_it() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let child = populated_child(&mut arena, root);
        let unrelated = populated_child(&mut arena, root);
        let as_of = arena.take_paint_generation();

        arena.note_paint_change(child, RowPaintChange::DRAWING);

        assert_eq!(
            arena.paint_change_stamps(child).changes_since(as_of),
            RowChanges {
                inputs: true,
                ..RowChanges::NONE
            }
        );
        assert_eq!(
            arena.paint_change_stamps(root).changes_since(as_of),
            RowChanges {
                below: true,
                ..RowChanges::NONE
            }
        );
        assert!(
            !arena
                .paint_change_stamps(unrelated)
                .row_or_anything_below_it_changed_since(as_of)
        );
        // A reader that took its generation after the write finds nothing newer.
        let later = arena.take_paint_generation();
        assert!(
            !arena
                .paint_change_stamps(child)
                .row_or_anything_below_it_changed_since(later)
        );
        assert!(
            !arena
                .paint_change_stamps(root)
                .row_or_anything_below_it_changed_since(later)
        );
    }

    #[test]
    fn readers_keep_their_own_generation() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let first = arena.take_paint_generation();
        arena.note_paint_change(root, RowPaintChange::ORDER);
        let second = arena.take_paint_generation();
        assert!(arena.paint_change_stamps(root).changes_since(first).order);
        assert!(!arena.paint_change_stamps(root).changes_since(second).order);
        assert!(!arena.paint_change_stamps(root).changes_since(first).inputs);
    }

    #[test]
    fn what_changes_only_what_can_be_hit_stamps_nothing_a_recording_reads() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let as_of = arena.take_paint_generation();
        let hit_test_items_stamp = arena.paintable_rows.hit_test_items_stamp;
        arena.note_paint_change(root, RowPaintChange::HIT_TESTING);
        assert!(
            !arena
                .paint_change_stamps(root)
                .row_or_anything_below_it_changed_since(as_of)
        );
        assert_ne!(arena.paintable_rows.hit_test_items_stamp, hit_test_items_stamp);
    }

    #[test]
    fn what_changes_only_scroll_metadata_leaves_what_the_row_draws_as_it_was() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let scroller = populated_child(&mut arena, root);
        let as_of = arena.take_paint_generation();

        arena.note_paint_change(scroller, RowPaintChange::SCROLL_METADATA);

        assert_eq!(
            arena.paint_change_stamps(scroller).changes_since(as_of),
            RowChanges {
                scroll_metadata: true,
                ..RowChanges::NONE
            }
        );
        assert!(arena.paint_change_stamps(root).changes_since(as_of).below);
    }

    #[test]
    fn a_change_below_a_row_new_to_the_document_reaches_the_rows_above_it() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let as_of = arena.take_paint_generation();
        let new_row = populated_child(&mut arena, root);
        let below_new_row = populated_child(&mut arena, new_row);

        arena.note_paint_change(below_new_row, RowPaintChange::DRAWING);

        assert!(arena.paint_change_stamps(new_row).changes_since(as_of).inputs);
        assert!(arena.paint_change_stamps(root).changes_since(as_of).below);
    }

    #[test]
    fn order_changes_resolve_to_the_nearest_paint_ancestor_and_keep_siblings_clean() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let owner = populated_child(&mut arena, root);
        let sibling = populated_child(&mut arena, root);
        let unpopulated = arena.allocate_for_test().slot;
        arena.write_shape(unpopulated).set_kind(NodeKind::InlineNode);
        arena.insert_child(owner, unpopulated, NodeSlotId::INVALID);
        let child = populated_child(&mut arena, unpopulated);
        let as_of = arena.take_paint_generation();

        arena.note_paint_order_changed(child);

        assert!(arena.paint_change_stamps(child).changes_since(as_of).order);
        assert!(arena.paint_change_stamps(owner).changes_since(as_of).order);
        assert!(arena.paint_change_stamps(root).changes_since(as_of).below);
        assert!(!arena.paint_change_stamps(root).changes_since(as_of).order);
        assert!(
            !arena
                .paint_change_stamps(sibling)
                .row_or_anything_below_it_changed_since(as_of)
        );
    }

    #[test]
    fn resource_subtrees_stamp_the_nearest_painted_layout_ancestor() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let svg_root = populated_child(&mut arena, root);
        let mask = populated_child(&mut arena, svg_root);
        arena.write_shape(mask).set_kind(NodeKind::SVGMaskBox);
        let mask_content = populated_child(&mut arena, mask);
        let as_of = arena.take_paint_generation();

        arena.note_paint_change(mask_content, RowPaintChange::DRAWING);

        for ancestor in [mask, svg_root, root] {
            assert!(arena.paint_change_stamps(ancestor).changes_since(as_of).below);
        }
    }

    #[test]
    fn everything_paints_anew_for_readers_of_earlier_generations_only() {
        let mut arena = LayoutNodeArena::new();
        populated_child(&mut arena, NodeSlotId::INVALID);
        let before = arena.take_paint_generation();
        arena.note_everything_paints_anew();
        let after = arena.take_paint_generation();
        assert!(arena.everything_paints_anew_since(before));
        assert!(!arena.everything_paints_anew_since(after));
    }

    #[test]
    fn repaint_fan_out_reaches_anonymous_boxes_and_the_line_root_of_an_inline() {
        let mut arena = LayoutNodeArena::new();
        let line_root = populated_child(&mut arena, NodeSlotId::INVALID);
        arena.write_shape(line_root).set_kind(NodeKind::BlockContainer);
        let inline = populated_child(&mut arena, line_root);
        arena.write_shape(inline).set_kind(NodeKind::InlineNode);
        let anonymous = populated_child(&mut arena, inline);
        arena.write_shape(anonymous).set_kind(NodeKind::BlockContainer);
        arena.set_node_flag(anonymous, crate::layout::node_data::NodeFlag::Anonymous, true);
        let as_of = arena.take_paint_generation();

        arena.note_paint_change_for_repaint(inline, RowPaintChange::DRAWING);

        for row in [inline, anonymous, line_root] {
            assert!(arena.paint_change_stamps(row).changes_since(as_of).inputs);
        }
    }
}
