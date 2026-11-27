/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::SpatialNodeIndex;
use crate::painting::record::hit_test_items::HitTestFacts;
use crate::painting::record::paint::inline_box::{InlineStrip, InlineStripKind};
use crate::painting::record::paint::text::SelectionStyleAnswer;
use crate::painting::record::{BasePaintFacts, PatternTileKey};
use std::collections::HashMap;
use std::sync::Arc;

const ROWS_PER_PAGE: usize = 64;

struct Page<T> {
    /// The stamp of the last write to a row of the page.
    written_at: u64,
    rows: [Option<(NodeSlotId, T)>; ROWS_PER_PAGE],
}

/// What was worked out for a row at a stamp, by slot, for as long as the table's users ask at that stamp.
///
/// OPTIMIZATION: The rows are in pages that come to be as rows are written, and a page no row of which is written for
///               a whole stamp is dropped. So the table holds about what its users reached lately, and nothing for the
///               slots of rows they never reach: free slots, and boxes no recording visits.
pub(crate) struct StampedRows<T> {
    pages: Vec<Option<Box<Page<T>>>>,
    /// The stamp the rows are written at.
    stamp: u64,
}

impl<T> Default for StampedRows<T> {
    fn default() -> Self {
        Self {
            pages: Vec::new(),
            stamp: 0,
        }
    }
}

impl<T: Copy> StampedRows<T> {
    /// Moves the table on to `stamp`, at which no row written at another stamp is answered, and drops the pages no
    /// row of which was written at the stamp it leaves.
    pub(crate) fn move_to_stamp(&mut self, stamp: u64) {
        if stamp == self.stamp {
            return;
        }
        let left = self.stamp;
        for page in &mut self.pages {
            if page.as_ref().is_some_and(|page| page.written_at != left) {
                *page = None;
            }
        }
        while self.pages.last().is_some_and(Option::is_none) {
            self.pages.pop();
        }
        self.stamp = stamp;
    }

    pub(crate) fn stamp(&self) -> u64 {
        self.stamp
    }

    /// What was written for `row` at the table's stamp.
    pub(crate) fn get(&self, row: NodeSlotId) -> Option<T> {
        let index = row.slot_index() as usize;
        let page = self.pages.get(index / ROWS_PER_PAGE)?.as_deref()?;
        if page.written_at != self.stamp {
            return None;
        }
        let (written_for, value) = page.rows[index % ROWS_PER_PAGE]?;
        (written_for == row).then_some(value)
    }

    pub(crate) fn set(&mut self, row: NodeSlotId, value: T) {
        let index = row.slot_index() as usize;
        let page_index = index / ROWS_PER_PAGE;
        if self.pages.len() <= page_index {
            self.pages.resize_with(page_index + 1, || None);
        }
        let page = self.pages[page_index].get_or_insert_with(|| {
            Box::new(Page {
                written_at: self.stamp,
                rows: [None; ROWS_PER_PAGE],
            })
        });
        if page.written_at != self.stamp {
            // NB: The rows of the page are of an earlier stamp, which the table no longer answers.
            page.rows = [None; ROWS_PER_PAGE];
            page.written_at = self.stamp;
        }
        page.rows[index % ROWS_PER_PAGE] = Some((row, value));
    }
}

/// Reusable workspace borrowed exclusively by one recording at a time. Its values are temporary derived facts, never
/// persistent paint-cache entries or recording output.
#[derive(Default)]
pub(crate) struct RecordingScratch {
    base_paint_facts: StampedRows<BasePaintFacts>,
    hit_test_facts: StampedRows<HitTestFacts>,
    /// The records of each pattern tile, and whether they draw a placeholder for an SVG image.
    pub(super) pattern_tile_records: HashMap<PatternTileKey, (Arc<Vec<u8>>, bool)>,
    pub(super) selection_style_cache: HashMap<u32, Arc<SelectionStyleAnswer>>,
    pub(super) search_text_style_cache: HashMap<u32, Arc<SelectionStyleAnswer>>,
    pub(super) search_text_current_style_cache: HashMap<u32, Arc<SelectionStyleAnswer>>,
    pub(super) spelling_error_style_cache: HashMap<u32, Arc<SelectionStyleAnswer>>,
    pub(super) custom_highlight_style_cache:
        HashMap<(u32, crate::css::style::index::StyleAtomID), Option<Arc<SelectionStyleAnswer>>>,
    pub(super) wheel_hit_test_target_cache: HashMap<NodeSlotId, SpatialNodeIndex>,
    /// The strip each inline box sliced across lines draws its background images or its box shadow over, measured
    /// once for all of its pieces.
    pub(crate) inline_strips: HashMap<(NodeSlotId, InlineStripKind), Option<Arc<InlineStrip>>>,
}

macro_rules! memo_table {
    ($get:ident, $set:ident, $table:ident, $type:ty) => {
        pub(crate) fn $get(&self, paintable: NodeSlotId) -> Option<$type> {
            self.$table.get(paintable)
        }

        pub(crate) fn $set(&mut self, paintable: NodeSlotId, value: $type) {
            self.$table.set(paintable, value)
        }
    };
}

impl RecordingScratch {
    pub(crate) fn begin_recording(&mut self) {
        let stamp = self.base_paint_facts.stamp() + 1;
        self.base_paint_facts.move_to_stamp(stamp);
        self.hit_test_facts.move_to_stamp(stamp);
        self.clear_temporary_caches();
    }

    // Retain the allocations, but release temporary records and styles before publication.
    pub(super) fn clear_temporary_caches(&mut self) {
        self.pattern_tile_records.clear();
        self.selection_style_cache.clear();
        self.search_text_style_cache.clear();
        self.search_text_current_style_cache.clear();
        self.spelling_error_style_cache.clear();
        self.custom_highlight_style_cache.clear();
        self.wheel_hit_test_target_cache.clear();
        self.inline_strips.clear();
    }

    memo_table!(base_paint_facts, set_base_paint_facts, base_paint_facts, BasePaintFacts);
    memo_table!(hit_test_facts, set_hit_test_facts, hit_test_facts, HitTestFacts);
}
