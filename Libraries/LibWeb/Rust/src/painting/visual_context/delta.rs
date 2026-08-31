/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct VisualContextTreeDelta {
    pub structural_epoch_changed: bool,
    pub requires_display_list_recording: bool,
    pub patched_spatial_node_indices: Vec<u32>,
    pub patched_frame_node_indices: Vec<u32>,
    pub tombstoned_spatial_node_indices: Vec<u32>,
    pub tombstoned_frame_node_indices: Vec<u32>,
}

impl VisualContextTreeDelta {
    pub fn note_patched_spatial(&mut self, index: u32) {
        self.patched_spatial_node_indices.push(index);
    }

    pub fn note_patched_frame(&mut self, index: u32) {
        self.patched_frame_node_indices.push(index);
    }

    pub fn note_tombstoned_spatial(&mut self, index: u32) {
        self.tombstoned_spatial_node_indices.push(index);
        self.structural_epoch_changed = true;
        self.requires_display_list_recording = true;
    }

    pub fn note_tombstoned_frame(&mut self, index: u32) {
        self.tombstoned_frame_node_indices.push(index);
        self.structural_epoch_changed = true;
        self.requires_display_list_recording = true;
    }

    pub fn note_repurposed_in_place(&mut self) {
        self.structural_epoch_changed = true;
        self.requires_display_list_recording = true;
    }

    pub fn note_allocated_spatial(&mut self, index: u32, reused_freed_slot: bool) {
        self.patched_spatial_node_indices.push(index);
        self.requires_display_list_recording = true;
        if reused_freed_slot {
            self.structural_epoch_changed = true;
        }
    }

    pub fn note_allocated_frame(&mut self, index: u32, reused_freed_slot: bool) {
        self.patched_frame_node_indices.push(index);
        self.requires_display_list_recording = true;
        if reused_freed_slot {
            self.structural_epoch_changed = true;
        }
    }

    pub fn finish(&mut self) {
        sort_and_deduplicate(&mut self.tombstoned_spatial_node_indices);
        sort_and_deduplicate(&mut self.tombstoned_frame_node_indices);
        let tombstoned_spatial = &self.tombstoned_spatial_node_indices;
        self.patched_spatial_node_indices
            .retain(|index| tombstoned_spatial.binary_search(index).is_err());
        sort_and_deduplicate(&mut self.patched_spatial_node_indices);
        let tombstoned_frames = &self.tombstoned_frame_node_indices;
        self.patched_frame_node_indices
            .retain(|index| tombstoned_frames.binary_search(index).is_err());
        sort_and_deduplicate(&mut self.patched_frame_node_indices);
    }
}

fn sort_and_deduplicate(indices: &mut Vec<u32>) {
    indices.sort_unstable();
    indices.dedup();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finishing_sorts_deduplicates_and_drops_patches_of_tombstoned_indices() {
        let mut delta = VisualContextTreeDelta::default();
        delta.note_patched_spatial(5);
        delta.note_patched_spatial(3);
        delta.note_patched_spatial(3);
        delta.note_patched_spatial(9);
        delta.note_tombstoned_spatial(5);
        delta.note_patched_frame(12);
        delta.note_patched_frame(2);
        delta.finish();
        assert_eq!(delta.patched_spatial_node_indices, vec![3, 9]);
        assert_eq!(delta.tombstoned_spatial_node_indices, vec![5]);
        assert_eq!(delta.patched_frame_node_indices, vec![2, 12]);
        assert!(delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
    }

    #[test]
    fn value_patches_alone_need_no_recording() {
        let mut delta = VisualContextTreeDelta::default();
        delta.note_patched_frame(1);
        delta.finish();
        assert!(!delta.structural_epoch_changed);
        assert!(!delta.requires_display_list_recording);
    }

    #[test]
    fn allocating_a_fresh_slot_requires_recording_without_an_epoch_change() {
        let mut delta = VisualContextTreeDelta::default();
        delta.note_allocated_spatial(7, false);
        delta.note_allocated_frame(9, false);
        delta.finish();
        assert_eq!(delta.patched_spatial_node_indices, vec![7]);
        assert_eq!(delta.patched_frame_node_indices, vec![9]);
        assert!(!delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
    }

    #[test]
    fn reusing_a_freed_slot_changes_the_structural_epoch() {
        let mut delta = VisualContextTreeDelta::default();
        delta.note_allocated_frame(4, true);
        delta.finish();
        assert_eq!(delta.patched_frame_node_indices, vec![4]);
        assert!(delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
    }
}
