/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::rc::Rc;

use crate::layout::used_values::FfiCssPixelPoint;
use crate::painting::display_list::builder::CommandRange;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::hit_test::HitTestItem;
use crate::painting::record::PaintPhase;
use crate::painting::record::traversal::StackingContextPaintPhase;

#[derive(Clone, Copy, Debug, Default)]
pub struct CachedCommands {
    // Display list ids start at 1, so a default-constructed entry never matches a real source list.
    pub source_display_list_id: u64,
    pub range: CommandRange,
    pub recorded_context: ContextRef,
    // Commands recorded under an empty effective clip are dropped at append time, so a cached range is
    // usable only while the emptiness of the phase's effective clip matches what it was at capture time.
    pub captured_under_empty_effective_clip: bool,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct CachedHitTestItems {
    // Hit-test display list ids start at 1, so a default-constructed entry never matches a real source list.
    pub source_hit_test_display_list_id: u64,
    pub start: u32,
    pub count: u32,
    // A capture may hold hit-test items recorded under both this paintable's own context index and its
    // descendants' context index, so spliced items are not rewritten; instead a cached range is usable
    // only while both indices still match what they were at capture time.
    pub recorded_context: ContextRef,
    pub recorded_context_for_descendants: ContextRef,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct CachedDescendantSubtree {
    pub source_display_list_id: u64,
    pub command_range: CommandRange,
    pub source_hit_test_display_list_id: u64,
    pub hit_test_start: u32,
    pub hit_test_count: u32,
}

pub struct PaintCache {
    commands: [Cell<Option<CachedCommands>>; PaintPhase::COUNT],
    hit_test_items: [Cell<Option<CachedHitTestItems>>; PaintPhase::COUNT],
    descendant_subtrees: [Cell<Option<CachedDescendantSubtree>>; StackingContextPaintPhase::COUNT],
    // One captured position per row covers every entry: register_capture_position() drops the
    // row's entries whenever a registration moves the stamp, so live entries are always captures
    // taken at this position.
    captured_absolute_position: Cell<FfiCssPixelPoint>,
    // Dirty while greater than the arena's completed-record generation; aged out by the bump
    // after a cache-writing recording, never cleared by walks.
    self_dirty_gen: Cell<u64>,
    descendant_dirty_gen: Cell<u64>,
}

impl Default for PaintCache {
    fn default() -> Self {
        Self {
            commands: std::array::from_fn(|_| Cell::new(None)),
            hit_test_items: std::array::from_fn(|_| Cell::new(None)),
            descendant_subtrees: std::array::from_fn(|_| Cell::new(None)),
            captured_absolute_position: Cell::new(FfiCssPixelPoint::default()),
            self_dirty_gen: Cell::new(0),
            descendant_dirty_gen: Cell::new(0),
        }
    }
}

impl PaintCache {
    pub fn commands(&self, phase: PaintPhase) -> Option<CachedCommands> {
        self.commands[phase as usize].get()
    }

    pub fn hit_test_items(&self, phase: PaintPhase) -> Option<CachedHitTestItems> {
        self.hit_test_items[phase as usize].get()
    }

    pub fn set_commands(&self, phase: PaintPhase, commands: CachedCommands) {
        self.commands[phase as usize].set(Some(commands));
    }

    pub fn set_hit_test_items(&self, phase: PaintPhase, hit_test_items: CachedHitTestItems) {
        self.hit_test_items[phase as usize].set(Some(hit_test_items));
    }

    pub(crate) fn descendant_subtree(&self, phase: StackingContextPaintPhase) -> Option<CachedDescendantSubtree> {
        self.descendant_subtrees[phase as usize].get()
    }

    pub(crate) fn set_descendant_subtree(&self, phase: StackingContextPaintPhase, subtree: CachedDescendantSubtree) {
        self.descendant_subtrees[phase as usize].set(Some(subtree));
    }

    pub fn clear_descendant_subtrees(&self) {
        for subtree in &self.descendant_subtrees {
            subtree.set(None);
        }
    }

    pub fn clear(&self) {
        for commands in &self.commands {
            commands.set(None);
        }
        for hit_test_items in &self.hit_test_items {
            hit_test_items.set(None);
        }
        self.clear_descendant_subtrees();
    }

    pub(crate) fn captured_absolute_position(&self) -> FfiCssPixelPoint {
        self.captured_absolute_position.get()
    }

    /// The row keeps one captured position for all of its entries, which is only sound while
    /// every live entry was captured at that position. A phase walk can re-register one entry
    /// kind of a moved row (e.g. the always-empty descendant subtree of a positioned child)
    /// while the row's other entries still hold output captured at the old position, so
    /// registering at a new position must drop the remaining entries: a moved box can never
    /// validly splice them again, and keeping them would let later position checks accept them
    /// against the freshly moved stamp.
    pub(crate) fn register_capture_position(&self, position: FfiCssPixelPoint) {
        if self.captured_absolute_position.get() != position {
            self.clear();
            self.captured_absolute_position.set(position);
        }
    }

    pub(crate) fn mark_self_dirty(&self, next_dirty_gen: u64) {
        self.self_dirty_gen.set(next_dirty_gen);
    }

    /// Returns whether the cache already carried this generation, so marking walks stop early.
    pub(crate) fn mark_descendants_dirty(&self, next_dirty_gen: u64) -> bool {
        let already_marked = self.descendant_dirty_gen.get() == next_dirty_gen;
        self.descendant_dirty_gen.set(next_dirty_gen);
        already_marked
    }

    pub(crate) fn is_self_dirty_since(&self, completed_record_gen: u64) -> bool {
        self.self_dirty_gen.get() > completed_record_gen
    }

    pub(crate) fn has_dirty_descendants_since(&self, completed_record_gen: u64) -> bool {
        self.descendant_dirty_gen.get() > completed_record_gen
    }
}

pub struct HitTestItemCacheSource {
    pub id: u64,
    pub visual_context_tree_version: u64,
    pub items: Rc<Vec<HitTestItem>>,
}
