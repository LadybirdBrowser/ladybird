/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::rc::Rc;

use crate::painting::display_list::builder::CommandRange;
use crate::painting::hit_test::HitTestItem;
use crate::painting::record::PaintPhase;

#[derive(Clone, Copy, Debug, Default)]
pub struct CachedCommands {
    // Display list ids start at 1, so a default-constructed entry never matches a real source list.
    pub source_display_list_id: u64,
    pub range: CommandRange,
    pub recorded_context_index: usize,
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
    pub recorded_context_index: usize,
    pub recorded_context_for_descendants_index: usize,
}

// Paint commands and hit-test items are stamped independently within a frame, so each setter
// must only ever assign its own sub-struct of the shared entry.
#[derive(Clone, Copy, Debug, Default)]
pub struct PhaseCacheEntry {
    pub commands: Option<CachedCommands>,
    pub hit_test_items: Option<CachedHitTestItems>,
}

#[derive(Clone, Debug, Default)]
pub struct PaintCache {
    pub phases: [PhaseCacheEntry; PaintPhase::COUNT],
}

impl PaintCache {
    pub fn entry(&self, phase: PaintPhase) -> &PhaseCacheEntry {
        &self.phases[phase as usize]
    }
    pub fn entry_mut(&mut self, phase: PaintPhase) -> &mut PhaseCacheEntry {
        &mut self.phases[phase as usize]
    }
}

pub struct HitTestItemCacheSource {
    pub id: u64,
    pub visual_context_tree_version: u64,
    pub items: Rc<Vec<HitTestItem>>,
}
