/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::BoxVisualContextNodeHandles;
use crate::layout::node_data::NodeSlotId;
pub use crate::painting::host::VisualContextUpdateScope;
use std::collections::HashMap;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum VisualContextBoxDirtyKind {
    NewRow = 0,
    RecommittedInPlace = 1,
    MovedWithDescendants = 2,
    ContainingBlockChanged = 3,
    InlineGeometryChanged = 4,
    StyleValueChange = 5,
    StyleStructuralChange = 6,
    ScrollableOverflowFlipped = 7,
    ReattachedInLayoutTree = 8,
    DefaultScrollShiftInputsChanged = 9,
}

impl VisualContextBoxDirtyKind {
    const fn bit(self) -> u16 {
        1 << (self as u16)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BoxDirtyBits(u16);

impl BoxDirtyBits {
    pub fn insert(&mut self, kind: VisualContextBoxDirtyKind) {
        self.0 |= kind.bit();
    }

    pub fn contains(&self, kind: VisualContextBoxDirtyKind) -> bool {
        self.0 & kind.bit() != 0
    }

    pub fn merge(&mut self, other: BoxDirtyBits) {
        self.0 |= other.0;
    }

    pub fn moves_descendants(&self) -> bool {
        self.contains(VisualContextBoxDirtyKind::MovedWithDescendants)
            || self.contains(VisualContextBoxDirtyKind::ContainingBlockChanged)
    }

    pub fn is_value_only(&self) -> bool {
        self.0 == VisualContextBoxDirtyKind::StyleValueChange.bit()
    }

    /// Whether only the box's geometry changed, with its row, its place in the layout tree and its style as they were.
    pub fn is_geometry_only(&self) -> bool {
        use VisualContextBoxDirtyKind as Kind;
        const GEOMETRY: u16 = Kind::RecommittedInPlace.bit()
            | Kind::MovedWithDescendants.bit()
            | Kind::ContainingBlockChanged.bit()
            | Kind::InlineGeometryChanged.bit();
        self.0 & !GEOMETRY == 0
    }

    /// Whether the box only moved, with its style and content as they were.
    pub fn is_move_only(&self) -> bool {
        self.0 == VisualContextBoxDirtyKind::MovedWithDescendants.bit()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RemovedBoxBlocks {
    pub slot: NodeSlotId,
    pub node_handles: BoxVisualContextNodeHandles,
    pub former_paint_parent: NodeSlotId,
}

#[derive(Default)]
pub struct VisualContextDirtySet {
    pub boxes: HashMap<NodeSlotId, BoxDirtyBits>,
    pub removed: Vec<RemovedBoxBlocks>,
    pub scope: VisualContextUpdateScope,
}

pub const MINIMUM_PENDING_DIRTY_BOX_LIMIT: usize = 1024;

impl VisualContextDirtySet {
    pub fn note_box(&mut self, slot: NodeSlotId, kind: VisualContextBoxDirtyKind, pending_box_limit: usize) {
        if slot.is_invalid() || self.scope.rebuilds_every_box() {
            return;
        }
        if self.boxes.len() >= pending_box_limit && !self.boxes.contains_key(&slot) {
            self.request_full_rebuild(VisualContextUpdateScope::EveryBox);
            return;
        }
        self.boxes.entry(slot).or_default().insert(kind);
    }

    pub fn forget_box(&mut self, slot: NodeSlotId) {
        self.boxes.remove(&slot);
    }

    pub fn note_removed(&mut self, removed: RemovedBoxBlocks) {
        self.boxes.remove(&removed.slot);
        self.removed.push(removed);
    }

    pub fn request_full_rebuild(&mut self, scope: VisualContextUpdateScope) {
        self.scope = self.scope.max(scope);
        self.boxes.clear();
    }

    pub fn is_empty(&self) -> bool {
        self.boxes.is_empty() && self.removed.is_empty() && self.scope == VisualContextUpdateScope::DirtyPath
    }

    pub fn is_value_only(&self) -> bool {
        self.scope == VisualContextUpdateScope::DirtyPath
            && self.removed.is_empty()
            && self.boxes.values().all(BoxDirtyBits::is_value_only)
    }

    pub fn clear(&mut self) {
        self.boxes.clear();
        self.removed.clear();
        self.scope = VisualContextUpdateScope::DirtyPath;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn slot(index: u32, generation: u8) -> NodeSlotId {
        NodeSlotId::new(index, generation)
    }

    #[test]
    fn kinds_coalesce_per_box_and_generations_stay_distinct() {
        let mut dirty = VisualContextDirtySet::default();
        dirty.note_box(slot(3, 1), VisualContextBoxDirtyKind::StyleValueChange, usize::MAX);
        dirty.note_box(slot(3, 1), VisualContextBoxDirtyKind::MovedWithDescendants, usize::MAX);
        dirty.note_box(slot(3, 2), VisualContextBoxDirtyKind::NewRow, usize::MAX);
        assert_eq!(dirty.boxes.len(), 2);
        let bits = dirty.boxes[&slot(3, 1)];
        assert!(bits.contains(VisualContextBoxDirtyKind::StyleValueChange));
        assert!(bits.moves_descendants());
        assert!(!bits.is_value_only());
        assert!(dirty.boxes[&slot(3, 2)].contains(VisualContextBoxDirtyKind::NewRow));
        assert!(!dirty.is_value_only());
    }

    #[test]
    fn value_only_sets_are_recognized() {
        let mut dirty = VisualContextDirtySet::default();
        assert!(dirty.is_empty());
        dirty.note_box(slot(1, 1), VisualContextBoxDirtyKind::StyleValueChange, usize::MAX);
        assert!(dirty.is_value_only());
        dirty.note_removed(RemovedBoxBlocks {
            slot: slot(1, 1),
            node_handles: BoxVisualContextNodeHandles::default(),
            former_paint_parent: NodeSlotId::INVALID,
        });
        assert!(!dirty.boxes.contains_key(&slot(1, 1)));
        assert!(!dirty.is_value_only());
    }

    #[test]
    fn requested_scopes_merge_to_the_widest() {
        let mut dirty = VisualContextDirtySet::default();
        dirty.request_full_rebuild(VisualContextUpdateScope::FreshTree);
        dirty.request_full_rebuild(VisualContextUpdateScope::EveryBox);
        assert_eq!(dirty.scope, VisualContextUpdateScope::FreshTree);
        dirty.clear();
        assert!(dirty.is_empty());
    }

    #[test]
    fn invalid_slots_are_ignored() {
        let mut dirty = VisualContextDirtySet::default();
        dirty.note_box(NodeSlotId::INVALID, VisualContextBoxDirtyKind::NewRow, usize::MAX);
        assert!(dirty.is_empty());
    }

    #[test]
    fn exceeding_the_pending_box_limit_coalesces_into_a_full_rebuild() {
        let mut dirty = VisualContextDirtySet::default();
        dirty.note_box(slot(0, 1), VisualContextBoxDirtyKind::StyleValueChange, 2);
        dirty.note_box(slot(1, 1), VisualContextBoxDirtyKind::StyleValueChange, 2);
        dirty.note_box(slot(0, 1), VisualContextBoxDirtyKind::NewRow, 2);
        assert_eq!(dirty.boxes.len(), 2);
        assert_eq!(dirty.scope, VisualContextUpdateScope::DirtyPath);
        dirty.note_removed(RemovedBoxBlocks {
            slot: slot(9, 1),
            node_handles: BoxVisualContextNodeHandles::default(),
            former_paint_parent: NodeSlotId::INVALID,
        });
        dirty.note_box(slot(2, 1), VisualContextBoxDirtyKind::StyleValueChange, 2);
        assert!(dirty.boxes.is_empty());
        assert_eq!(dirty.removed.len(), 1);
        assert_eq!(dirty.scope, VisualContextUpdateScope::EveryBox);
    }

    #[test]
    fn a_pending_full_rebuild_absorbs_per_box_notes_and_keeps_removed_blocks() {
        let mut dirty = VisualContextDirtySet::default();
        dirty.note_box(slot(0, 1), VisualContextBoxDirtyKind::StyleValueChange, usize::MAX);
        dirty.request_full_rebuild(VisualContextUpdateScope::FreshTree);
        dirty.note_box(slot(1, 1), VisualContextBoxDirtyKind::NewRow, usize::MAX);
        dirty.note_removed(RemovedBoxBlocks {
            slot: slot(2, 1),
            node_handles: BoxVisualContextNodeHandles::default(),
            former_paint_parent: NodeSlotId::INVALID,
        });
        assert!(dirty.boxes.is_empty());
        assert_eq!(dirty.removed.len(), 1);
        assert_eq!(dirty.scope, VisualContextUpdateScope::FreshTree);
    }

    #[test]
    fn removed_blocks_survive_a_pending_full_rebuild() {
        let mut dirty = VisualContextDirtySet::default();
        dirty.note_removed(RemovedBoxBlocks {
            slot: slot(1, 1),
            node_handles: BoxVisualContextNodeHandles::default(),
            former_paint_parent: NodeSlotId::INVALID,
        });
        dirty.request_full_rebuild(VisualContextUpdateScope::EveryBox);
        dirty.note_removed(RemovedBoxBlocks {
            slot: slot(2, 1),
            node_handles: BoxVisualContextNodeHandles::default(),
            former_paint_parent: NodeSlotId::INVALID,
        });
        assert_eq!(dirty.removed.len(), 2);
        assert!(!dirty.is_empty());
        dirty.clear();
        assert!(dirty.is_empty());
    }

    #[test]
    fn forgetting_a_box_drops_its_pending_entry() {
        let mut dirty = VisualContextDirtySet::default();
        dirty.note_box(slot(0, 1), VisualContextBoxDirtyKind::NewRow, usize::MAX);
        dirty.forget_box(slot(0, 1));
        assert!(dirty.is_empty());
    }
}
