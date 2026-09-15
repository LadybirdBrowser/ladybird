/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Paint damage is pushed by every mutation site that changes what a row paints or where its
//! output sits in CSS paint order. Recording reads the accumulated set and validates nothing.

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::node_painting;
use crate::painting::paint_order;
use crate::painting::paint_order_plan::PaintOrderInputs;
use std::cell::{Cell, Ref, RefCell};

#[derive(Clone, Copy, Default, PartialEq, Eq, Hash)]
pub(crate) struct PaintDamage(u32);

impl PaintDamage {
    pub(crate) const NONE: Self = Self(0);

    // Producers of a row whose output must be recorded again.
    pub(crate) const DRAW_BACKGROUND: Self = Self(1 << 0);
    pub(crate) const DRAW_BORDER: Self = Self(1 << 1);
    pub(crate) const DRAW_TABLE_COLLAPSED_BORDER: Self = Self(1 << 2);
    pub(crate) const DRAW_FOREGROUND: Self = Self(1 << 3);
    pub(crate) const DRAW_OUTLINE: Self = Self(1 << 4);
    pub(crate) const DRAW_OVERLAY: Self = Self(1 << 5);
    pub(crate) const HIT_BACKGROUND: Self = Self(1 << 6);
    pub(crate) const HIT_FOREGROUND: Self = Self(1 << 7);
    pub(crate) const HIT_OVERLAY: Self = Self(1 << 8);
    pub(crate) const SCROLL_METADATA: Self = Self(1 << 9);
    pub(crate) const SCOPE_PREAMBLE: Self = Self(1 << 10);
    pub(crate) const SVG: Self = Self(1 << 11);
    pub(crate) const ALL_DRAW: Self = Self(
        Self::DRAW_BACKGROUND.0
            | Self::DRAW_BORDER.0
            | Self::DRAW_TABLE_COLLAPSED_BORDER.0
            | Self::DRAW_FOREGROUND.0
            | Self::DRAW_OUTLINE.0
            | Self::DRAW_OVERLAY.0
            | Self::SCOPE_PREAMBLE.0
            | Self::SVG.0,
    );
    pub(crate) const ALL_HIT: Self = Self(Self::HIT_BACKGROUND.0 | Self::HIT_FOREGROUND.0 | Self::HIT_OVERLAY.0);
    pub(crate) const ALL_PRODUCERS: Self = Self(Self::ALL_DRAW.0 | Self::ALL_HIT.0 | Self::SCROLL_METADATA.0);

    // The row's absolute position changed: every producer of the row and of each populated row
    // in its layout subtree records again. The subtree is expanded when recording plans.
    pub(crate) const MOVED: Self = Self(1 << 16);
    // Every scope the row owns is planned again: the row places its own descendants
    // differently, or a descendant it is the nearest paint ancestor of appeared, disappeared or
    // changed how it is placed.
    pub(crate) const ORDER: Self = Self(1 << 17);
    // The stacking context rooted at the row composes its hoisted content differently.
    pub(crate) const CONTEXT_ORDER: Self = Self(1 << 20);
    // Whether the row's stacking context paints at all flipped.
    pub(crate) const ELIGIBILITY: Self = Self(1 << 21);
    // Some paint descendant was damaged: producers of this row that read descendants run again.
    pub(crate) const DESCENDANT_READERS: Self = Self(1 << 22);

    pub(crate) const fn is_empty(self) -> bool {
        self.0 == 0
    }

    pub(crate) const fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }
}

impl std::ops::BitOr for PaintDamage {
    type Output = Self;

    fn bitor(self, other: Self) -> Self {
        Self(self.0 | other.0)
    }
}

impl std::ops::BitOrAssign for PaintDamage {
    fn bitor_assign(&mut self, other: Self) {
        self.0 |= other.0;
    }
}

impl std::fmt::Debug for PaintDamage {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        const NAMES: [(PaintDamage, &str); 17] = [
            (PaintDamage::DRAW_BACKGROUND, "draw-background"),
            (PaintDamage::DRAW_BORDER, "draw-border"),
            (PaintDamage::DRAW_TABLE_COLLAPSED_BORDER, "draw-table-collapsed-border"),
            (PaintDamage::DRAW_FOREGROUND, "draw-foreground"),
            (PaintDamage::DRAW_OUTLINE, "draw-outline"),
            (PaintDamage::DRAW_OVERLAY, "draw-overlay"),
            (PaintDamage::HIT_BACKGROUND, "hit-background"),
            (PaintDamage::HIT_FOREGROUND, "hit-foreground"),
            (PaintDamage::HIT_OVERLAY, "hit-overlay"),
            (PaintDamage::SCROLL_METADATA, "scroll-metadata"),
            (PaintDamage::SCOPE_PREAMBLE, "scope-preamble"),
            (PaintDamage::SVG, "svg"),
            (PaintDamage::MOVED, "moved"),
            (PaintDamage::ORDER, "order"),
            (PaintDamage::CONTEXT_ORDER, "context-order"),
            (PaintDamage::ELIGIBILITY, "eligibility"),
            (PaintDamage::DESCENDANT_READERS, "descendant-readers"),
        ];
        let mut list = formatter.debug_set();
        for (bit, name) in NAMES {
            if self.contains(bit) {
                list.entry(&format_args!("{name}"));
            }
        }
        list.finish()
    }
}

/// The per-row state of the retained paint output, kept next to the other parallel row vectors.
#[derive(Default)]
pub(crate) struct RowPaintState {
    damage: Cell<PaintDamage>,
    // The recording sequence number the row was last pushed in; publication clears rows pushed
    // before the recording it publishes started and keeps rows pushed after.
    damage_stamp: Cell<u32>,
    order_inputs: Cell<PaintOrderInputs>,
}

impl RowPaintState {
    pub(crate) fn damage(&self) -> PaintDamage {
        self.damage.get()
    }

    pub(crate) fn order_inputs(&self) -> Option<PaintOrderInputs> {
        let inputs = self.order_inputs.get();
        inputs.is_initialized().then_some(inputs)
    }

    pub(crate) fn update_order_inputs(&self, inputs: PaintOrderInputs) -> bool {
        self.order_inputs.replace(inputs) != inputs
    }

    pub(crate) fn clear(&self) {
        self.damage.set(PaintDamage::NONE);
        self.damage_stamp.set(0);
        self.order_inputs.set(PaintOrderInputs::default());
    }

    fn add(&self, damage: PaintDamage, stamp: u32) -> bool {
        let before = self.damage.get();
        self.damage.set(before | damage);
        self.damage_stamp.set(stamp);
        before.is_empty()
    }
}

#[derive(Default)]
pub(crate) struct DamageSet {
    // Every row whose damage is non-empty, listed once when its first bit is pushed. Entries of
    // rows reset since are skipped by their slot generation.
    rows: RefCell<Vec<NodeSlotId>>,
    // Zero, or the stamp the whole-document damage was pushed at.
    all: Cell<u32>,
    scroll_metadata_everywhere: Cell<u32>,
    started_cache_writing_recordings: Cell<u32>,
}

impl DamageSet {
    fn stamp_for_push(&self) -> u32 {
        self.started_cache_writing_recordings.get() + 1
    }
}

impl LayoutNodeArena {
    pub(crate) fn row_paint_state(&self, row: NodeSlotId) -> Ref<'_, RowPaintState> {
        Ref::map(self.paintable_rows.row_paint_states.borrow(), |states| {
            &states[row.slot_index() as usize]
        })
    }

    #[cfg(test)]
    pub(crate) fn paint_damage_of_row(&self, row: NodeSlotId) -> PaintDamage {
        if !self.paintable_row_is_populated(row) {
            return PaintDamage::NONE;
        }
        self.row_paint_state(row).damage()
    }

    pub(crate) fn push_paint_damage(&self, row: NodeSlotId, damage: PaintDamage) {
        self.debug_assert_not_recording();
        if damage.is_empty() || !self.paintable_row_is_populated(row) {
            return;
        }
        let set = &self.paintable_rows.damage;
        let stamp = set.stamp_for_push();
        // Whole-document damage pushed for the same recording already covers every row.
        if set.all.get() == stamp {
            return;
        }
        let states = self.paintable_rows.row_paint_states.borrow();
        // A listed row already hinted its ancestors: every push hints up to the root or to an
        // ancestor whose own push did, and publication hints again for the rows it keeps.
        if states[row.slot_index() as usize].add(damage, stamp) {
            set.rows.borrow_mut().push(row);
            self.hint_descendant_readers_above(row, stamp, &states);
        }
    }

    // Producers that read descendants, such as an SVG root or a snap container's scroll
    // metadata, learn about damage below them from this hint. The walk stops at the first
    // ancestor already hinted.
    fn hint_descendant_readers_above(&self, row: NodeSlotId, stamp: u32, states: &[RowPaintState]) {
        let set = &self.paintable_rows.damage;
        let mut current = row;
        while let Some(ancestor) = self.paint_ancestor_for_damage_hints(current) {
            let state = &states[ancestor.slot_index() as usize];
            if state.damage().contains(PaintDamage::DESCENDANT_READERS) {
                break;
            }
            if state.add(PaintDamage::DESCENDANT_READERS, stamp) {
                set.rows.borrow_mut().push(ancestor);
            }
            current = ancestor;
        }
    }

    // Resource subtrees (masks, clips, patterns) are not painted where they sit in the layout
    // tree but by the element referencing them; the hint continues from the nearest painted
    // layout ancestor so a change inside the resource reaches the content painted around it.
    fn paint_ancestor_for_damage_hints(&self, row: NodeSlotId) -> Option<NodeSlotId> {
        let rows = self.paintable_rows();
        if let Some(parent) = paint_order::paint_parent(&rows, row) {
            return Some(parent);
        }
        if !self
            .node_kind_if_live(row)
            .is_some_and(node_painting::forms_unconnected_subtree)
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

    pub(crate) fn push_paint_damage_to_paint_subtree(&self, root: NodeSlotId, damage: PaintDamage) {
        let rows = self.paintable_rows();
        paint_order::for_each_in_paint_subtree(&rows, root, |row| self.push_paint_damage(row, damage));
    }

    /// The fan-out of a style repaint: the row, the anonymous boxes it generated, and for an
    /// inline the ancestors up to the line root that paints its pieces.
    pub(crate) fn push_paint_damage_for_repaint(&self, row: NodeSlotId, damage: PaintDamage) {
        self.paintable_rows()
            .for_each_row_repainted_with(row, |repainted| self.push_paint_damage(repainted, damage));
    }

    /// The row's producers and scopes are listed by the plans of its nearest paint ancestor;
    /// resolved now, while the links are intact, for rows about to appear, disappear or move.
    /// The ancestor's content changed below it as well.
    pub(crate) fn push_enclosing_paint_order_damage(&self, node: NodeSlotId) {
        if let Some(owner) = self.enclosing_paint_order_owner(node) {
            self.push_paint_damage(owner, PaintDamage::ORDER | PaintDamage::DESCENDANT_READERS);
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

    pub(crate) fn push_all_paint_damage(&self) {
        self.debug_assert_not_recording();
        let set = &self.paintable_rows.damage;
        set.all.set(set.stamp_for_push());
    }

    pub(crate) fn push_scroll_metadata_damage_everywhere(&self) {
        self.debug_assert_not_recording();
        let set = &self.paintable_rows.damage;
        set.scroll_metadata_everywhere.set(set.stamp_for_push());
    }

    #[cfg(test)]
    pub(crate) fn paint_damage_covers_everything(&self) -> bool {
        self.paintable_rows.damage.all.get() != 0
    }

    #[cfg(test)]
    pub(crate) fn scroll_metadata_damaged_everywhere(&self) -> bool {
        self.paintable_rows.damage.scroll_metadata_everywhere.get() != 0
    }

    #[cfg(test)]
    pub(crate) fn damaged_paint_rows(&self) -> Vec<NodeSlotId> {
        let mut rows: Vec<NodeSlotId> = self
            .paintable_rows
            .damage
            .rows
            .borrow()
            .iter()
            .copied()
            .filter(|row| self.paintable_row_is_populated(*row) && !self.row_paint_state(*row).damage().is_empty())
            .collect();
        rows.sort_unstable_by_key(|row| (row.slot_index(), row.generation()));
        rows.dedup();
        rows
    }

    pub(crate) fn note_cache_writing_paint_recording_started(&self) {
        let counter = &self.paintable_rows.damage.started_cache_writing_recordings;
        counter.set(
            counter
                .get()
                .checked_add(1)
                .expect("paint recording sequence overflowed"),
        );
    }

    /// The published recording consumed everything pushed before it started. Rows pushed since
    /// stay listed, and their hints are renewed because the ancestors they hinted were cleared.
    pub(crate) fn clear_paint_damage_consumed_by_published_recording(&self) {
        let set = &self.paintable_rows.damage;
        let consumed_through = set.started_cache_writing_recordings.get();
        if set.all.get() != 0 && set.all.get() <= consumed_through {
            set.all.set(0);
        }
        if set.scroll_metadata_everywhere.get() != 0 && set.scroll_metadata_everywhere.get() <= consumed_through {
            set.scroll_metadata_everywhere.set(0);
        }
        let listed = std::mem::take(&mut *set.rows.borrow_mut());
        let states = self.paintable_rows.row_paint_states.borrow();
        let mut kept = Vec::new();
        for row in listed {
            if !self.paintable_row_is_populated(row) {
                continue;
            }
            let state = &states[row.slot_index() as usize];
            if state.damage_stamp.get() <= consumed_through {
                state.damage.set(PaintDamage::NONE);
                state.damage_stamp.set(0);
            } else if !kept.contains(&row) {
                kept.push(row);
            }
        }
        *set.rows.borrow_mut() = kept.clone();
        for row in kept {
            let stamp = states[row.slot_index() as usize].damage_stamp.get();
            self.hint_descendant_readers_above(row, stamp, &states);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeKind;

    fn populated_child(arena: &mut LayoutNodeArena, parent: NodeSlotId) -> NodeSlotId {
        let row = arena.allocate_for_test().slot;
        arena.data(row).kind.set(NodeKind::Box);
        if !parent.is_invalid() {
            arena.insert_child(parent, row, NodeSlotId::INVALID);
        }
        arena.populate_paintable_row(row);
        row
    }

    // Building the tree pushes attachment damage; a published recording consumes it.
    fn settle(arena: &LayoutNodeArena) {
        arena.note_cache_writing_paint_recording_started();
        arena.clear_paint_damage_consumed_by_published_recording();
        assert_eq!(arena.damaged_paint_rows(), Vec::new());
    }

    #[test]
    fn a_push_lists_the_row_once_and_hints_every_ancestor_once() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let middle = populated_child(&mut arena, root);
        let leaf = populated_child(&mut arena, middle);
        let unrelated = populated_child(&mut arena, root);
        settle(&arena);

        arena.push_paint_damage(leaf, PaintDamage::DRAW_BACKGROUND);
        arena.push_paint_damage(leaf, PaintDamage::HIT_FOREGROUND);

        assert_eq!(
            arena.paint_damage_of_row(leaf),
            PaintDamage::DRAW_BACKGROUND | PaintDamage::HIT_FOREGROUND
        );
        assert_eq!(arena.paint_damage_of_row(middle), PaintDamage::DESCENDANT_READERS);
        assert_eq!(arena.paint_damage_of_row(root), PaintDamage::DESCENDANT_READERS);
        assert_eq!(arena.paint_damage_of_row(unrelated), PaintDamage::NONE);
        assert_eq!(arena.damaged_paint_rows(), {
            let mut expected = vec![leaf, middle, root];
            expected.sort_unstable_by_key(|row| row.slot_index());
            expected
        });
    }

    #[test]
    fn order_changes_resolve_to_the_nearest_paint_ancestor_and_keep_siblings_clean() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let owner = populated_child(&mut arena, root);
        let sibling = populated_child(&mut arena, root);
        let unpopulated = arena.allocate_for_test().slot;
        arena.data(unpopulated).kind.set(NodeKind::InlineNode);
        arena.insert_child(owner, unpopulated, NodeSlotId::INVALID);
        let child = populated_child(&mut arena, unpopulated);
        settle(&arena);

        arena.note_paint_order_changed(child);

        assert!(arena.paint_damage_of_row(child).contains(PaintDamage::ORDER));
        assert!(
            arena
                .paint_damage_of_row(owner)
                .contains(PaintDamage::ORDER | PaintDamage::DESCENDANT_READERS)
        );
        assert_eq!(arena.paint_damage_of_row(root), PaintDamage::DESCENDANT_READERS);
        assert_eq!(arena.paint_damage_of_row(sibling), PaintDamage::NONE);
    }

    #[test]
    fn reparenting_damages_both_owners_and_renews_the_hints_above_the_new_parent() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let old_owner = populated_child(&mut arena, root);
        let new_owner = populated_child(&mut arena, root);
        let moved = populated_child(&mut arena, old_owner);
        let leaf = populated_child(&mut arena, moved);
        settle(&arena);

        arena.push_paint_damage(leaf, PaintDamage::DRAW_FOREGROUND);
        arena.remove_child(old_owner, moved);
        arena.insert_child(new_owner, moved, NodeSlotId::INVALID);
        arena.push_paint_damage(leaf, PaintDamage::DRAW_OUTLINE);

        assert!(arena.paint_damage_of_row(old_owner).contains(PaintDamage::ORDER));
        assert!(arena.paint_damage_of_row(new_owner).contains(PaintDamage::ORDER));
        assert!(
            arena
                .paint_damage_of_row(new_owner)
                .contains(PaintDamage::DESCENDANT_READERS)
        );
        assert!(arena.paint_damage_of_row(moved).contains(PaintDamage::ORDER));
        assert!(
            arena
                .paint_damage_of_row(root)
                .contains(PaintDamage::DESCENDANT_READERS)
        );
    }

    #[test]
    fn resource_subtrees_hint_the_nearest_painted_layout_ancestor() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let svg_root = populated_child(&mut arena, root);
        let mask = populated_child(&mut arena, svg_root);
        arena.data(mask).kind.set(NodeKind::SVGMaskBox);
        let mask_content = populated_child(&mut arena, mask);
        settle(&arena);

        arena.push_paint_damage(mask_content, PaintDamage::SVG);

        assert_eq!(arena.paint_damage_of_row(mask), PaintDamage::DESCENDANT_READERS);
        assert_eq!(arena.paint_damage_of_row(svg_root), PaintDamage::DESCENDANT_READERS);
        assert_eq!(arena.paint_damage_of_row(root), PaintDamage::DESCENDANT_READERS);
    }

    #[test]
    fn whole_document_damage_makes_later_pushes_for_the_same_recording_free() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let child = populated_child(&mut arena, root);
        settle(&arena);

        arena.push_all_paint_damage();
        arena.push_paint_damage(child, PaintDamage::ALL_PRODUCERS);

        assert!(arena.paint_damage_covers_everything());
        assert_eq!(arena.damaged_paint_rows(), Vec::new());

        arena.note_cache_writing_paint_recording_started();
        arena.push_paint_damage(child, PaintDamage::DRAW_BORDER);
        arena.clear_paint_damage_consumed_by_published_recording();

        assert!(!arena.paint_damage_covers_everything());
        assert_eq!(arena.paint_damage_of_row(child), PaintDamage::DRAW_BORDER);
        assert_eq!(arena.paint_damage_of_row(root), PaintDamage::DESCENDANT_READERS);
    }

    #[test]
    fn publication_clears_what_the_recording_consumed_and_keeps_later_pushes() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let early = populated_child(&mut arena, root);
        let late = populated_child(&mut arena, root);
        settle(&arena);
        arena.push_paint_damage(early, PaintDamage::DRAW_FOREGROUND);
        arena.push_scroll_metadata_damage_everywhere();

        arena.note_cache_writing_paint_recording_started();
        arena.push_paint_damage(late, PaintDamage::HIT_OVERLAY);
        arena.clear_paint_damage_consumed_by_published_recording();

        assert_eq!(arena.paint_damage_of_row(early), PaintDamage::NONE);
        assert_eq!(arena.paint_damage_of_row(late), PaintDamage::HIT_OVERLAY);
        assert_eq!(arena.paint_damage_of_row(root), PaintDamage::DESCENDANT_READERS);
        assert!(!arena.scroll_metadata_damaged_everywhere());
        assert_eq!(arena.damaged_paint_rows(), {
            let mut expected = vec![late, root];
            expected.sort_unstable_by_key(|row| row.slot_index());
            expected
        });

        arena.note_cache_writing_paint_recording_started();
        arena.clear_paint_damage_consumed_by_published_recording();
        assert_eq!(arena.damaged_paint_rows(), Vec::new());
    }

    #[test]
    fn a_reset_row_retires_from_the_set_and_damages_its_former_owner() {
        let mut arena = LayoutNodeArena::new();
        let root = populated_child(&mut arena, NodeSlotId::INVALID);
        let owner = populated_child(&mut arena, root);
        let cleared = populated_child(&mut arena, owner);
        settle(&arena);
        arena.push_paint_damage(cleared, PaintDamage::DRAW_BACKGROUND);

        let reset = arena.prepare_paintable_row_cleared_reset(cleared).unwrap();
        arena.paintable_row_cleared(reset);

        assert!(!arena.paintable_row_is_populated(cleared));
        assert!(arena.paint_damage_of_row(owner).contains(PaintDamage::ORDER));
        assert!(!arena.damaged_paint_rows().contains(&cleared));
    }

    #[test]
    fn repaint_fan_out_reaches_anonymous_boxes_and_the_line_root_of_an_inline() {
        let mut arena = LayoutNodeArena::new();
        let line_root = populated_child(&mut arena, NodeSlotId::INVALID);
        arena.data(line_root).kind.set(NodeKind::BlockContainer);
        let inline = populated_child(&mut arena, line_root);
        arena.data(inline).kind.set(NodeKind::InlineNode);
        let anonymous = populated_child(&mut arena, inline);
        arena.data(anonymous).kind.set(NodeKind::BlockContainer);
        let flags = &arena.data(anonymous).flags;
        flags.set(flags.get() | crate::layout::node_data::NodeFlag::Anonymous as u32);
        settle(&arena);

        arena.push_paint_damage_for_repaint(inline, PaintDamage::ALL_DRAW);

        assert!(arena.paint_damage_of_row(inline).contains(PaintDamage::ALL_DRAW));
        assert!(arena.paint_damage_of_row(anonymous).contains(PaintDamage::ALL_DRAW));
        assert!(arena.paint_damage_of_row(line_root).contains(PaintDamage::ALL_DRAW));
    }
}
