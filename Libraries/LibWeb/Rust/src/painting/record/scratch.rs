/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::layout::used_values::FfiCssPixelPoint;
use crate::painting::record::BasePaintFacts;
use crate::painting::record::cache::ResolvedEnclosingCaptureMemo;
use crate::painting::record::hit_test_items::HitTestFacts;

#[derive(Clone, Copy)]
struct StampedEntry<T> {
    recording_stamp: u32,
    paintable: NodeSlotId,
    value: T,
}

impl<T: Default> Default for StampedEntry<T> {
    fn default() -> Self {
        Self {
            recording_stamp: 0,
            paintable: NodeSlotId::INVALID,
            value: T::default(),
        }
    }
}

#[derive(Default)]
pub(crate) struct PerRecordingMemoTables {
    recording_stamp: u32,
    base_paint_facts: Vec<StampedEntry<BasePaintFacts>>,
    hit_test_facts: Vec<StampedEntry<HitTestFacts>>,
    absolute_positions: Vec<StampedEntry<FfiCssPixelPoint>>,
    // The first phase that re-records a moved row updates the live per-row cell, so later
    // phases must compare against this snapshot or they would accept their stale captures.
    captured_positions_snapshotted_at_recording_start: Vec<StampedEntry<FfiCssPixelPoint>>,
    resolved_enclosing_capture_memo: ResolvedEnclosingCaptureMemo,
}

fn entry_if_stamped_for_current_recording<T: Copy>(
    table: &[StampedEntry<T>],
    recording_stamp: u32,
    paintable: NodeSlotId,
) -> Option<T> {
    let entry = table.get(paintable.slot_index() as usize)?;
    (entry.recording_stamp == recording_stamp && entry.paintable == paintable).then_some(entry.value)
}

fn stamp_entry_for_current_recording<T>(
    table: &mut [StampedEntry<T>],
    recording_stamp: u32,
    paintable: NodeSlotId,
    value: T,
) {
    if let Some(entry) = table.get_mut(paintable.slot_index() as usize) {
        *entry = StampedEntry {
            recording_stamp,
            paintable,
            value,
        };
    }
}

macro_rules! memo_table {
    ($get:ident, $set:ident, $table:ident, $type:ty) => {
        pub(crate) fn $get(&self, paintable: NodeSlotId) -> Option<$type> {
            entry_if_stamped_for_current_recording(&self.$table, self.recording_stamp, paintable)
        }

        pub(crate) fn $set(&mut self, paintable: NodeSlotId, value: $type) {
            stamp_entry_for_current_recording(&mut self.$table, self.recording_stamp, paintable, value)
        }
    };
}

impl PerRecordingMemoTables {
    pub(crate) fn begin_recording(&mut self, row_count: usize) {
        self.recording_stamp = match self.recording_stamp.checked_add(1) {
            Some(recording_stamp) => recording_stamp,
            None => {
                self.base_paint_facts.clear();
                self.hit_test_facts.clear();
                self.absolute_positions.clear();
                self.captured_positions_snapshotted_at_recording_start.clear();
                1
            }
        };
        if self.base_paint_facts.len() < row_count {
            self.base_paint_facts.resize(row_count, StampedEntry::default());
            self.hit_test_facts.resize(row_count, StampedEntry::default());
            self.absolute_positions.resize(row_count, StampedEntry::default());
            self.captured_positions_snapshotted_at_recording_start
                .resize(row_count, StampedEntry::default());
        }
        self.resolved_enclosing_capture_memo.clear();
    }

    pub(crate) fn resolved_enclosing_capture_memo(&mut self) -> &mut ResolvedEnclosingCaptureMemo {
        &mut self.resolved_enclosing_capture_memo
    }

    memo_table!(base_paint_facts, set_base_paint_facts, base_paint_facts, BasePaintFacts);
    memo_table!(hit_test_facts, set_hit_test_facts, hit_test_facts, HitTestFacts);
    memo_table!(
        absolute_position,
        set_absolute_position,
        absolute_positions,
        FfiCssPixelPoint
    );
    memo_table!(
        captured_position_at_recording_start,
        set_captured_position_at_recording_start,
        captured_positions_snapshotted_at_recording_start,
        FfiCssPixelPoint
    );
}
