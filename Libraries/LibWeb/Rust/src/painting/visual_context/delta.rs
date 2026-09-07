/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct VisualContextTreeDelta {
    pub structural_epoch_changed: bool,
    pub requires_display_list_recording: bool,
    pub tombstoned_any_node: bool,
}

impl VisualContextTreeDelta {
    pub fn note_tombstoned(&mut self) {
        self.tombstoned_any_node = true;
        self.structural_epoch_changed = true;
        self.requires_display_list_recording = true;
    }

    pub fn note_repurposed_in_place(&mut self) {
        self.structural_epoch_changed = true;
        self.requires_display_list_recording = true;
    }

    pub fn note_allocated(&mut self, reused_freed_slot: bool) {
        self.requires_display_list_recording = true;
        self.structural_epoch_changed |= reused_freed_slot;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn allocating_a_fresh_slot_requires_recording_without_an_epoch_change() {
        let mut delta = VisualContextTreeDelta::default();
        delta.note_allocated(false);
        assert!(!delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
    }

    #[test]
    fn reusing_a_freed_slot_changes_the_structural_epoch() {
        let mut delta = VisualContextTreeDelta::default();
        delta.note_allocated(true);
        assert!(delta.structural_epoch_changed);
        assert!(delta.requires_display_list_recording);
    }
}
