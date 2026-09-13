/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::rc::Rc;

use crate::css::style::fast_hash::FastMap;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::layout::used_values::FfiCssPixelPoint;
use crate::painting::display_list::commands::ContextRef;
use crate::painting::hit_test::HitTestItem;
use crate::painting::record::PaintPhase;
use crate::painting::record::traversal::StackingContextPaintPhase;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum CaptureKind {
    BoxPhase(PaintPhase),
    DescendantSubtreePhase(StackingContextPaintPhase),
    PaintedAsStackingContext,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct CaptureSite {
    pub paintable: NodeSlotId,
    pub kind: CaptureKind,
}

pub(crate) type RecordGen = u32;

pub(crate) fn narrow_record_gen(generation: u64) -> RecordGen {
    RecordGen::try_from(generation).expect("paint cache record generation exceeds u32")
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct CaptureAddress {
    pub enclosing_capture: Option<CaptureSite>,
    pub command_byte_offset_from_enclosing_start: u32,
    pub hit_test_item_index_from_enclosing_start: u32,
    pub written_in_record_gen: RecordGen,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct SourceTapePosition {
    pub command_byte_offset: u32,
    pub hit_test_item_index: u32,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct CachedBoxPhaseCommands {
    pub(crate) address: CaptureAddress,
    pub command_byte_count: u32,
    pub recorded_context: ContextRef,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct CachedBoxPhaseHitTestItems {
    pub(crate) address: CaptureAddress,
    pub count: u32,
    // A capture may hold hit-test items recorded under both this paintable's own context index and its
    // descendants' context index, so spliced items are not rewritten; instead a cached range is usable
    // only while both indices still match what they were at capture time.
    pub recorded_context: ContextRef,
    pub recorded_context_for_descendants: ContextRef,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct CachedSubtreeCapture {
    pub(crate) address: CaptureAddress,
    pub command_byte_count: u32,
    pub hit_test_item_count: u32,
    pub(crate) gen_of_last_fresh_walk: RecordGen,
    pub may_be_spliced_verbatim: bool,
    pub contains_blocking_wheel_event_region: bool,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct SubtreeCaptureWalkOutcome {
    pub gen_of_last_fresh_walk: RecordGen,
    pub may_be_spliced_verbatim: bool,
    pub contains_blocking_wheel_event_region: bool,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct OpenCapture {
    pub site: CaptureSite,
    pub command_byte_start: u32,
    pub hit_test_item_start: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct EnclosingCaptureAnchor {
    pub address: CaptureAddress,
    pub gen_of_last_fresh_walk: RecordGen,
}

impl From<CachedSubtreeCapture> for EnclosingCaptureAnchor {
    fn from(capture: CachedSubtreeCapture) -> Self {
        Self {
            address: capture.address,
            gen_of_last_fresh_walk: capture.gen_of_last_fresh_walk,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ResolvedEnclosingCapture {
    pub gen_of_last_fresh_walk: RecordGen,
    pub source_position: Option<SourceTapePosition>,
}

pub(crate) type ResolvedEnclosingCaptureMemo = FastMap<CaptureSite, ResolvedEnclosingCapture>;

pub(crate) fn resolve_capture_address_in_source_tape(
    completed_record_gen: RecordGen,
    address: CaptureAddress,
    lookup_enclosing_capture_anchor: &impl Fn(CaptureSite) -> Option<EnclosingCaptureAnchor>,
    memo: &mut ResolvedEnclosingCaptureMemo,
) -> Option<SourceTapePosition> {
    debug_assert!(
        address.written_in_record_gen <= completed_record_gen,
        "source capture metadata must belong to a completed recording"
    );
    let Some(enclosing_capture) = address.enclosing_capture else {
        return (address.written_in_record_gen == completed_record_gen).then_some(SourceTapePosition {
            command_byte_offset: address.command_byte_offset_from_enclosing_start,
            hit_test_item_index: address.hit_test_item_index_from_enclosing_start,
        });
    };
    let enclosing_start = resolve_enclosing_capture_start(
        completed_record_gen,
        enclosing_capture,
        address.written_in_record_gen,
        lookup_enclosing_capture_anchor,
        memo,
    )?;
    Some(SourceTapePosition {
        command_byte_offset: enclosing_start.command_byte_offset + address.command_byte_offset_from_enclosing_start,
        hit_test_item_index: enclosing_start.hit_test_item_index + address.hit_test_item_index_from_enclosing_start,
    })
}

fn resolve_enclosing_capture_start(
    completed_record_gen: RecordGen,
    site: CaptureSite,
    child_written_in_record_gen: RecordGen,
    lookup_enclosing_capture_anchor: &impl Fn(CaptureSite) -> Option<EnclosingCaptureAnchor>,
    memo: &mut ResolvedEnclosingCaptureMemo,
) -> Option<SourceTapePosition> {
    let resolved = if let Some(memoized) = memo.get(&site).copied() {
        memoized
    } else {
        let resolved = match lookup_enclosing_capture_anchor(site) {
            None => ResolvedEnclosingCapture {
                gen_of_last_fresh_walk: 0,
                source_position: None,
            },
            Some(anchor) => ResolvedEnclosingCapture {
                gen_of_last_fresh_walk: anchor.gen_of_last_fresh_walk,
                source_position: resolve_capture_address_in_source_tape(
                    completed_record_gen,
                    anchor.address,
                    lookup_enclosing_capture_anchor,
                    memo,
                ),
            },
        };
        memo.insert(site, resolved);
        resolved
    };
    let start = resolved.source_position?;
    let enclosing_walk_placed_the_child = child_written_in_record_gen >= resolved.gen_of_last_fresh_walk;
    enclosing_walk_placed_the_child.then_some(start)
}

pub struct PaintCache {
    commands: [Cell<Option<CachedBoxPhaseCommands>>; PaintPhase::COUNT],
    hit_test_items: [Cell<Option<CachedBoxPhaseHitTestItems>>; PaintPhase::COUNT],
    descendant_subtrees: [Cell<Option<CachedSubtreeCapture>>; StackingContextPaintPhase::COUNT],
    painted_as_stacking_context: Cell<Option<CachedSubtreeCapture>>,
    // Source entries and their position stay unchanged throughout recording. Publication drops
    // the old entries if the staged captures were recorded at a different position.
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
            painted_as_stacking_context: Cell::new(None),
            captured_absolute_position: Cell::new(FfiCssPixelPoint::default()),
            self_dirty_gen: Cell::new(0),
            descendant_dirty_gen: Cell::new(0),
        }
    }
}

impl PaintCache {
    pub fn commands(&self, phase: PaintPhase) -> Option<CachedBoxPhaseCommands> {
        self.commands[phase as usize].get()
    }

    pub fn hit_test_items(&self, phase: PaintPhase) -> Option<CachedBoxPhaseHitTestItems> {
        self.hit_test_items[phase as usize].get()
    }

    pub(crate) fn subtree_capture(&self, kind: CaptureKind) -> Option<CachedSubtreeCapture> {
        match kind {
            CaptureKind::BoxPhase(_) => None,
            CaptureKind::DescendantSubtreePhase(phase) => self.descendant_subtrees[phase as usize].get(),
            CaptureKind::PaintedAsStackingContext => self.painted_as_stacking_context.get(),
        }
    }

    pub(crate) fn enclosing_capture_anchor(&self, kind: CaptureKind) -> Option<EnclosingCaptureAnchor> {
        self.subtree_capture(kind).map(EnclosingCaptureAnchor::from)
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
        self.painted_as_stacking_context.set(None);
    }

    pub(crate) fn reset_entries_position_and_dirty_gens(&self) {
        self.clear();
        self.captured_absolute_position.set(FfiCssPixelPoint::default());
        self.self_dirty_gen.set(0);
        self.descendant_dirty_gen.set(0);
    }

    pub(crate) fn captured_absolute_position(&self) -> FfiCssPixelPoint {
        self.captured_absolute_position.get()
    }

    fn apply_update(&self, update: PaintCacheUpdate) {
        if self.captured_absolute_position.get() != update.absolute_position {
            self.clear();
            self.captured_absolute_position.set(update.absolute_position);
        }
        // Captures skipped by this recording remain available through their enclosing anchors.
        // In particular, reusing an entire subtree only updates its root capture.
        for (destination, source) in self.commands.iter().zip(update.commands) {
            if let Some(entry) = source {
                destination.set(Some(entry));
            }
        }
        for (destination, source) in self.hit_test_items.iter().zip(update.hit_test_items) {
            if let Some(entry) = source {
                destination.set(Some(entry));
            }
        }
        for (destination, source) in self.descendant_subtrees.iter().zip(update.descendant_subtrees) {
            if let Some(entry) = source {
                destination.set(Some(entry));
            }
        }
        if let Some(entry) = update.painted_as_stacking_context {
            self.painted_as_stacking_context.set(Some(entry));
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

    pub(crate) fn is_self_dirty_since(&self, completed_record_gen: RecordGen) -> bool {
        self.self_dirty_gen.get() > u64::from(completed_record_gen)
    }

    pub(crate) fn has_dirty_descendants_since(&self, completed_record_gen: RecordGen) -> bool {
        self.descendant_dirty_gen.get() > u64::from(completed_record_gen)
    }
}

#[derive(Default)]
struct PaintCacheUpdate {
    absolute_position: FfiCssPixelPoint,
    commands: [Option<CachedBoxPhaseCommands>; PaintPhase::COUNT],
    hit_test_items: [Option<CachedBoxPhaseHitTestItems>; PaintPhase::COUNT],
    descendant_subtrees: [Option<CachedSubtreeCapture>; StackingContextPaintPhase::COUNT],
    painted_as_stacking_context: Option<CachedSubtreeCapture>,
}

// Only rows that record or splice a capture get an update. The arena's caches remain the
// immutable source until publication, including during the optional verification recording.
#[derive(Default)]
pub(crate) struct PendingPaintCacheUpdates {
    rows: FastMap<NodeSlotId, PaintCacheUpdate>,
}

impl PendingPaintCacheUpdates {
    fn for_paintable(&mut self, paintable: NodeSlotId, absolute_position: FfiCssPixelPoint) -> &mut PaintCacheUpdate {
        let update = self.rows.entry(paintable).or_insert_with(|| PaintCacheUpdate {
            absolute_position,
            ..Default::default()
        });
        debug_assert_eq!(
            update.absolute_position, absolute_position,
            "a row moved during recording"
        );
        update
    }

    pub(crate) fn set_commands(
        &mut self,
        paintable: NodeSlotId,
        position: FfiCssPixelPoint,
        phase: PaintPhase,
        commands: CachedBoxPhaseCommands,
    ) {
        let previous = self.for_paintable(paintable, position).commands[phase as usize].replace(commands);
        debug_assert!(
            previous.is_none(),
            "a per-phase command capture site ran twice in one recording"
        );
    }

    pub(crate) fn set_hit_test_items(
        &mut self,
        paintable: NodeSlotId,
        position: FfiCssPixelPoint,
        phase: PaintPhase,
        items: CachedBoxPhaseHitTestItems,
    ) {
        let previous = self.for_paintable(paintable, position).hit_test_items[phase as usize].replace(items);
        debug_assert!(
            previous.is_none(),
            "a per-phase hit-test capture site ran twice in one recording"
        );
    }

    pub(crate) fn set_subtree_capture(
        &mut self,
        site: CaptureSite,
        position: FfiCssPixelPoint,
        capture: CachedSubtreeCapture,
    ) {
        let update = self.for_paintable(site.paintable, position);
        let destination = match site.kind {
            CaptureKind::BoxPhase(_) => unreachable!("a box phase capture is not a subtree capture"),
            CaptureKind::DescendantSubtreePhase(phase) => &mut update.descendant_subtrees[phase as usize],
            CaptureKind::PaintedAsStackingContext => &mut update.painted_as_stacking_context,
        };
        let previous = destination.replace(capture);
        debug_assert!(previous.is_none(), "a capture site ran twice in one recording");
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.rows.is_empty()
    }

    pub(crate) fn commit(self, arena: &LayoutNodeArena) {
        for (paintable, update) in self.rows {
            assert!(
                arena.paintable_row_is_populated(paintable),
                "a captured row was removed before publication"
            );
            arena.paintable_paint_cache(paintable).apply_update(update);
        }
    }
}

pub struct HitTestItemCacheSource {
    pub items: Rc<Vec<HitTestItem>>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    fn slot(index: u32) -> NodeSlotId {
        NodeSlotId { index }
    }

    fn subtree(index: u32) -> CaptureSite {
        CaptureSite {
            paintable: slot(index),
            kind: CaptureKind::DescendantSubtreePhase(StackingContextPaintPhase::Foreground),
        }
    }

    fn address(
        enclosing_capture: Option<CaptureSite>,
        command_byte_offset_from_enclosing_start: u32,
        hit_test_item_index_from_enclosing_start: u32,
        written_in_record_gen: RecordGen,
    ) -> CaptureAddress {
        CaptureAddress {
            enclosing_capture,
            command_byte_offset_from_enclosing_start,
            hit_test_item_index_from_enclosing_start,
            written_in_record_gen,
        }
    }

    fn anchor(address: CaptureAddress, gen_of_last_fresh_walk: RecordGen) -> EnclosingCaptureAnchor {
        EnclosingCaptureAnchor {
            address,
            gen_of_last_fresh_walk,
        }
    }

    fn position(command_byte_offset: u32, hit_test_item_index: u32) -> SourceTapePosition {
        SourceTapePosition {
            command_byte_offset,
            hit_test_item_index,
        }
    }

    struct Anchors(HashMap<CaptureSite, EnclosingCaptureAnchor>);

    impl Anchors {
        fn lookup(&self) -> impl Fn(CaptureSite) -> Option<EnclosingCaptureAnchor> + '_ {
            move |site| self.0.get(&site).copied()
        }
    }

    fn resolve(
        completed: RecordGen,
        address: CaptureAddress,
        anchors: &Anchors,
        memo: &mut ResolvedEnclosingCaptureMemo,
    ) -> Option<SourceTapePosition> {
        resolve_capture_address_in_source_tape(completed, address, &anchors.lookup(), memo)
    }

    #[test]
    fn top_level_entry_resolves_only_from_the_source_recording() {
        let anchors = Anchors(HashMap::new());
        let mut memo = ResolvedEnclosingCaptureMemo::default();
        assert_eq!(
            resolve(7, address(None, 100, 4, 7), &anchors, &mut memo),
            Some(position(100, 4))
        );
        assert_eq!(resolve(7, address(None, 100, 4, 6), &anchors, &mut memo), None);
    }

    #[test]
    fn nested_entry_sums_offsets_through_enclosing_captures_that_were_only_copied() {
        let mut anchors = HashMap::new();
        anchors.insert(subtree(1), anchor(address(None, 1000, 10, 5), 4));
        anchors.insert(subtree(2), anchor(address(Some(subtree(1)), 500, 5, 4), 3));
        let anchors = Anchors(anchors);
        let mut memo = ResolvedEnclosingCaptureMemo::default();
        assert_eq!(
            resolve(5, address(Some(subtree(2)), 40, 2, 3), &anchors, &mut memo),
            Some(position(1540, 17))
        );
        assert!(memo.get(&subtree(2)).is_some_and(|memo| memo.source_position.is_some()));
        assert_eq!(
            resolve(5, address(Some(subtree(2)), 8, 1, 3), &anchors, &mut memo),
            Some(position(1508, 16))
        );
    }

    #[test]
    fn entry_whose_site_did_not_run_in_the_enclosing_captures_last_walk_is_rejected() {
        let mut anchors = HashMap::new();
        anchors.insert(subtree(1), anchor(address(None, 1000, 10, 4), 4));
        anchors.insert(subtree(2), anchor(address(Some(subtree(1)), 500, 5, 4), 4));
        let anchors = Anchors(anchors);
        let mut memo = ResolvedEnclosingCaptureMemo::default();
        assert_eq!(
            resolve(4, address(Some(subtree(2)), 40, 2, 3), &anchors, &mut memo),
            None
        );
        assert_eq!(
            resolve(4, address(Some(subtree(2)), 40, 2, 4), &anchors, &mut memo),
            Some(position(1540, 17))
        );
    }

    #[test]
    fn cleared_enclosing_capture_and_stale_top_level_enclosing_capture_invalidate_the_chain() {
        let mut anchors = HashMap::new();
        anchors.insert(subtree(2), anchor(address(Some(subtree(1)), 500, 5, 3), 3));
        let anchors = Anchors(anchors);
        let mut memo = ResolvedEnclosingCaptureMemo::default();
        assert_eq!(
            resolve(5, address(Some(subtree(2)), 40, 2, 3), &anchors, &mut memo),
            None
        );

        let mut anchors = HashMap::new();
        anchors.insert(subtree(1), anchor(address(None, 1000, 10, 4), 4));
        anchors.insert(subtree(2), anchor(address(Some(subtree(1)), 500, 5, 4), 3));
        let anchors = Anchors(anchors);
        let mut memo = ResolvedEnclosingCaptureMemo::default();
        assert_eq!(
            resolve(5, address(Some(subtree(2)), 40, 2, 3), &anchors, &mut memo),
            None
        );
    }

    fn allocate_paintable(arena: &mut LayoutNodeArena) -> NodeSlotId {
        let slot = arena.allocate_for_test().slot;
        arena.populate_paintable_row(slot);
        slot
    }

    fn capture(address: CaptureAddress, gen_of_last_fresh_walk: RecordGen) -> CachedSubtreeCapture {
        CachedSubtreeCapture {
            address,
            gen_of_last_fresh_walk,
            may_be_spliced_verbatim: true,
            ..Default::default()
        }
    }

    fn resolve_in_arena(arena: &LayoutNodeArena, address: CaptureAddress) -> Option<SourceTapePosition> {
        resolve_capture_address_in_source_tape(
            narrow_record_gen(arena.paint_cache_completed_record_gen()),
            address,
            &|site| {
                arena
                    .paintable_paint_cache(site.paintable)
                    .enclosing_capture_anchor(site.kind)
            },
            &mut ResolvedEnclosingCaptureMemo::default(),
        )
    }

    #[test]
    fn staging_enclosing_captures_in_either_order_preserves_unmemoized_source_addresses() {
        let mut arena = LayoutNodeArena::new();
        let root = CaptureSite {
            paintable: allocate_paintable(&mut arena),
            kind: CaptureKind::PaintedAsStackingContext,
        };
        let child = CaptureSite {
            paintable: allocate_paintable(&mut arena),
            kind: CaptureKind::DescendantSubtreePhase(StackingContextPaintPhase::Foreground),
        };
        let point = FfiCssPixelPoint::default();
        let leaf_address = address(Some(child), 40, 2, 1);
        let mut source = PendingPaintCacheUpdates::default();
        source.set_subtree_capture(root, point, capture(address(None, 1000, 10, 1), 1));
        source.set_subtree_capture(child, point, capture(address(Some(root), 500, 5, 1), 1));
        source.set_commands(
            child.paintable,
            point,
            PaintPhase::Foreground,
            CachedBoxPhaseCommands {
                address: leaf_address,
                command_byte_count: 32,
                ..Default::default()
            },
        );
        source.set_hit_test_items(
            child.paintable,
            point,
            PaintPhase::Foreground,
            CachedBoxPhaseHitTestItems {
                address: leaf_address,
                count: 1,
                ..Default::default()
            },
        );
        source.commit(&arena);
        arena.note_paint_record_completed_with_cache_writes();

        for order in [[root, child], [child, root]] {
            let mut pending = PendingPaintCacheUpdates::default();
            for site in order {
                let updated = if site == root {
                    capture(address(None, 1000, 10, 2), 2)
                } else {
                    // The child is spliced to a different offset; its interior captures are untouched.
                    capture(address(Some(root), 900, 9, 2), 1)
                };
                pending.set_subtree_capture(site, point, updated);
            }
            let cache = arena.paintable_paint_cache(child.paintable);
            // Each lookup starts with an empty memo, after both replacements have been staged.
            assert_eq!(
                resolve_in_arena(&arena, cache.commands(PaintPhase::Foreground).unwrap().address),
                Some(position(1540, 17))
            );
            assert_eq!(
                resolve_in_arena(&arena, cache.hit_test_items(PaintPhase::Foreground).unwrap().address),
                Some(position(1540, 17))
            );
            drop(cache);
            assert_eq!(arena.paint_cache_completed_record_gen(), 1);
            if order[0] == child {
                pending.commit(&arena);
                arena.note_paint_record_completed_with_cache_writes();
            }
            // Dropping the first pending recording does not change the source.
        }
        assert_eq!(resolve_in_arena(&arena, leaf_address), Some(position(1940, 21)));

        // A quiet recording only updates the root. Nested captures must still resolve through it.
        let mut quiet = PendingPaintCacheUpdates::default();
        quiet.set_subtree_capture(root, point, capture(address(None, 1000, 10, 3), 2));
        assert_eq!(quiet.rows.len(), 1);
        quiet.commit(&arena);
        arena.note_paint_record_completed_with_cache_writes();
        let cache = arena.paintable_paint_cache(child.paintable);
        assert_eq!(cache.commands(PaintPhase::Foreground).unwrap().command_byte_count, 32);
        assert_eq!(cache.hit_test_items(PaintPhase::Foreground).unwrap().count, 1);
        assert_eq!(resolve_in_arena(&arena, leaf_address), Some(position(1940, 21)));
    }

    #[test]
    fn moved_row_keeps_its_source_position_and_entries_until_commit() {
        let mut arena = LayoutNodeArena::new();
        let row = allocate_paintable(&mut arena);
        let stacking = CaptureSite {
            paintable: row,
            kind: CaptureKind::PaintedAsStackingContext,
        };
        let descendants = CaptureSite {
            paintable: row,
            kind: CaptureKind::DescendantSubtreePhase(StackingContextPaintPhase::Foreground),
        };
        let old_position = FfiCssPixelPoint::default();
        let new_position = FfiCssPixelPoint {
            x: crate::layout::CssPixels::from_integer(10),
            ..old_position
        };
        let mut source = PendingPaintCacheUpdates::default();
        source.set_commands(
            row,
            old_position,
            PaintPhase::Foreground,
            CachedBoxPhaseCommands::default(),
        );
        source.set_hit_test_items(
            row,
            old_position,
            PaintPhase::Foreground,
            CachedBoxPhaseHitTestItems::default(),
        );
        source.set_subtree_capture(stacking, old_position, capture(address(None, 100, 2, 1), 1));
        source.commit(&arena);
        arena.note_paint_record_completed_with_cache_writes();
        arena.invalidate_paint_cache(row);

        let mut pending = PendingPaintCacheUpdates::default();
        // Even an empty new capture must retire every old-position entry at publication.
        pending.set_subtree_capture(descendants, new_position, capture(address(None, 200, 4, 2), 2));
        let cache = arena.paintable_paint_cache(row);
        assert_eq!(cache.captured_absolute_position(), old_position);
        assert!(cache.commands(PaintPhase::Foreground).is_some());
        assert!(cache.hit_test_items(PaintPhase::Foreground).is_some());
        assert!(cache.subtree_capture(stacking.kind).is_some());
        assert!(cache.subtree_capture(descendants.kind).is_none());
        drop(cache);

        pending.commit(&arena);
        let cache = arena.paintable_paint_cache(row);
        assert_eq!(cache.captured_absolute_position(), new_position);
        assert!(cache.commands(PaintPhase::Foreground).is_none());
        assert!(cache.hit_test_items(PaintPhase::Foreground).is_none());
        assert!(cache.subtree_capture(stacking.kind).is_none());
        assert_eq!(
            cache.subtree_capture(descendants.kind).unwrap().address,
            address(None, 200, 4, 2)
        );
        // Capture publication does not itself consume dirty state or advance the recording generation.
        assert!(cache.is_self_dirty_since(1));
        assert_eq!(arena.paint_cache_completed_record_gen(), 1);
    }
}
