/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::BoxVisualContextNodeHandles;
use crate::layout::node_data::NodeSlotId;
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
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RemovedBoxBlocks {
    pub slot: NodeSlotId,
    pub node_handles: BoxVisualContextNodeHandles,
    pub former_paint_parent: NodeSlotId,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u8)]
pub enum VisualContextGlobalRebuildReason {
    #[default]
    None = 0,
    AnchorsRegistered = 1,
    TreeInputsChanged = 2,
    DocumentWideStructuralChange = 3,
    SvgResourceSubtreeChanged = 4,
    FilterResourcesChanged = 5,
    FirstBuild = 6,
    Compaction = 7,
    ForcedForTesting = 8,
    CanonicalDumpRequested = 9,
    InvalidIncrementalReferences = 10,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum VisualContextUpdateScope {
    DirtyPath,
    EveryBox,
    FreshTree,
}

impl VisualContextUpdateScope {
    pub fn for_reason(reason: VisualContextGlobalRebuildReason) -> Self {
        use VisualContextGlobalRebuildReason as Reason;
        match reason {
            Reason::None => Self::DirtyPath,
            Reason::AnchorsRegistered
            | Reason::TreeInputsChanged
            | Reason::DocumentWideStructuralChange
            | Reason::SvgResourceSubtreeChanged
            | Reason::FilterResourcesChanged => Self::EveryBox,
            Reason::FirstBuild
            | Reason::Compaction
            | Reason::ForcedForTesting
            | Reason::CanonicalDumpRequested
            | Reason::InvalidIncrementalReferences => Self::FreshTree,
        }
    }

    pub fn rebuilds_every_box(self) -> bool {
        self != Self::DirtyPath
    }
}

#[derive(Default)]
pub struct VisualContextDirtySet {
    pub boxes: HashMap<NodeSlotId, BoxDirtyBits>,
    pub removed: Vec<RemovedBoxBlocks>,
    pub global_reason: VisualContextGlobalRebuildReason,
}

pub const MINIMUM_PENDING_DIRTY_BOX_LIMIT: usize = 1024;

impl VisualContextDirtySet {
    pub fn note_box(&mut self, slot: NodeSlotId, kind: VisualContextBoxDirtyKind, pending_box_limit: usize) {
        if slot.is_invalid() || self.global_reason != VisualContextGlobalRebuildReason::None {
            return;
        }
        if self.boxes.len() >= pending_box_limit && !self.boxes.contains_key(&slot) {
            self.request_full_rebuild(VisualContextGlobalRebuildReason::DocumentWideStructuralChange);
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

    pub fn request_full_rebuild(&mut self, reason: VisualContextGlobalRebuildReason) {
        self.global_reason = self.global_reason.max(reason);
        self.boxes.clear();
    }

    pub fn is_empty(&self) -> bool {
        self.boxes.is_empty() && self.removed.is_empty() && self.global_reason == VisualContextGlobalRebuildReason::None
    }

    pub fn is_value_only(&self) -> bool {
        self.global_reason == VisualContextGlobalRebuildReason::None
            && self.removed.is_empty()
            && self.boxes.values().all(BoxDirtyBits::is_value_only)
    }

    pub fn clear(&mut self) {
        self.boxes.clear();
        self.removed.clear();
        self.global_reason = VisualContextGlobalRebuildReason::None;
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
    fn global_reasons_merge_to_the_strongest() {
        let mut dirty = VisualContextDirtySet::default();
        dirty.request_full_rebuild(VisualContextGlobalRebuildReason::FirstBuild);
        dirty.request_full_rebuild(VisualContextGlobalRebuildReason::None);
        assert_eq!(dirty.global_reason, VisualContextGlobalRebuildReason::FirstBuild);
        dirty.request_full_rebuild(VisualContextGlobalRebuildReason::ForcedForTesting);
        dirty.request_full_rebuild(VisualContextGlobalRebuildReason::Compaction);
        assert_eq!(dirty.global_reason, VisualContextGlobalRebuildReason::ForcedForTesting);
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
        assert_eq!(dirty.global_reason, VisualContextGlobalRebuildReason::None);
        dirty.note_removed(RemovedBoxBlocks {
            slot: slot(9, 1),
            node_handles: BoxVisualContextNodeHandles::default(),
            former_paint_parent: NodeSlotId::INVALID,
        });
        dirty.note_box(slot(2, 1), VisualContextBoxDirtyKind::StyleValueChange, 2);
        assert!(dirty.boxes.is_empty());
        assert_eq!(dirty.removed.len(), 1);
        assert_eq!(
            dirty.global_reason,
            VisualContextGlobalRebuildReason::DocumentWideStructuralChange
        );
    }

    #[test]
    fn a_pending_full_rebuild_absorbs_per_box_notes_and_keeps_removed_blocks() {
        let mut dirty = VisualContextDirtySet::default();
        dirty.note_box(slot(0, 1), VisualContextBoxDirtyKind::StyleValueChange, usize::MAX);
        dirty.request_full_rebuild(VisualContextGlobalRebuildReason::FirstBuild);
        dirty.note_box(slot(1, 1), VisualContextBoxDirtyKind::NewRow, usize::MAX);
        dirty.note_removed(RemovedBoxBlocks {
            slot: slot(2, 1),
            node_handles: BoxVisualContextNodeHandles::default(),
            former_paint_parent: NodeSlotId::INVALID,
        });
        assert!(dirty.boxes.is_empty());
        assert_eq!(dirty.removed.len(), 1);
        assert_eq!(dirty.global_reason, VisualContextGlobalRebuildReason::FirstBuild);
    }

    #[test]
    fn removed_blocks_survive_a_pending_full_rebuild() {
        let mut dirty = VisualContextDirtySet::default();
        dirty.note_removed(RemovedBoxBlocks {
            slot: slot(1, 1),
            node_handles: BoxVisualContextNodeHandles::default(),
            former_paint_parent: NodeSlotId::INVALID,
        });
        dirty.request_full_rebuild(VisualContextGlobalRebuildReason::DocumentWideStructuralChange);
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
    fn scopes_are_monotone_over_the_reason_order() {
        use VisualContextGlobalRebuildReason as Reason;
        let reasons_in_ascending_order = [
            Reason::None,
            Reason::AnchorsRegistered,
            Reason::TreeInputsChanged,
            Reason::DocumentWideStructuralChange,
            Reason::SvgResourceSubtreeChanged,
            Reason::FilterResourcesChanged,
            Reason::FirstBuild,
            Reason::Compaction,
            Reason::ForcedForTesting,
            Reason::CanonicalDumpRequested,
            Reason::InvalidIncrementalReferences,
        ];
        let mut previous_reason = Reason::None;
        let mut previous_scope = VisualContextUpdateScope::DirtyPath;
        for reason in reasons_in_ascending_order {
            assert!(reason >= previous_reason);
            let scope = VisualContextUpdateScope::for_reason(reason);
            assert!(scope >= previous_scope);
            previous_reason = reason;
            previous_scope = scope;
        }
        assert_eq!(
            VisualContextUpdateScope::for_reason(Reason::FirstBuild.max(Reason::AnchorsRegistered)),
            VisualContextUpdateScope::FreshTree
        );
    }

    #[test]
    fn forgetting_a_box_drops_its_pending_entry() {
        let mut dirty = VisualContextDirtySet::default();
        dirty.note_box(slot(0, 1), VisualContextBoxDirtyKind::NewRow, usize::MAX);
        dirty.forget_box(slot(0, 1));
        assert!(dirty.is_empty());
    }
}
