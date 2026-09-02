/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::Cell;
use std::rc::Rc;

use crate::css::style::fast_hash::FastMap;
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
    pub recorded_with_should_paint_overlay: bool,
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
        "a capture site ran twice in one recording"
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
                source_position: (anchor.address.written_in_record_gen <= completed_record_gen)
                    .then(|| {
                        resolve_capture_address_in_source_tape(
                            completed_record_gen,
                            anchor.address,
                            lookup_enclosing_capture_anchor,
                            memo,
                        )
                    })
                    .flatten(),
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

    pub fn set_commands(&self, phase: PaintPhase, commands: CachedBoxPhaseCommands) {
        self.commands[phase as usize].set(Some(commands));
    }

    pub fn set_hit_test_items(&self, phase: PaintPhase, hit_test_items: CachedBoxPhaseHitTestItems) {
        self.hit_test_items[phase as usize].set(Some(hit_test_items));
    }

    pub(crate) fn subtree_capture(&self, kind: CaptureKind) -> Option<CachedSubtreeCapture> {
        match kind {
            CaptureKind::BoxPhase(_) => None,
            CaptureKind::DescendantSubtreePhase(phase) => self.descendant_subtrees[phase as usize].get(),
            CaptureKind::PaintedAsStackingContext => self.painted_as_stacking_context.get(),
        }
    }

    pub(crate) fn set_subtree_capture(&self, kind: CaptureKind, capture: CachedSubtreeCapture) {
        match kind {
            CaptureKind::BoxPhase(_) => unreachable!("a box phase capture is not a subtree capture"),
            CaptureKind::DescendantSubtreePhase(phase) => self.descendant_subtrees[phase as usize].set(Some(capture)),
            CaptureKind::PaintedAsStackingContext => self.painted_as_stacking_context.set(Some(capture)),
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

    pub(crate) fn is_self_dirty_since(&self, completed_record_gen: RecordGen) -> bool {
        self.self_dirty_gen.get() > u64::from(completed_record_gen)
    }

    pub(crate) fn has_dirty_descendants_since(&self, completed_record_gen: RecordGen) -> bool {
        self.descendant_dirty_gen.get() > u64::from(completed_record_gen)
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

    fn position(command_byte_offset: u32, hit_test_item_index: u32) -> Option<SourceTapePosition> {
        Some(SourceTapePosition {
            command_byte_offset,
            hit_test_item_index,
        })
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
            position(100, 4)
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
            position(1540, 17)
        );
        assert!(memo.get(&subtree(2)).is_some_and(|memo| memo.source_position.is_some()));
        assert_eq!(
            resolve(5, address(Some(subtree(2)), 8, 1, 3), &anchors, &mut memo),
            position(1508, 16)
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
            position(1540, 17)
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

    #[test]
    fn enclosing_capture_re_placed_by_the_current_recording_is_rejected_unless_memoized_first() {
        let mut anchors = HashMap::new();
        anchors.insert(subtree(1), anchor(address(None, 1000, 10, 5), 5));
        anchors.insert(subtree(2), anchor(address(Some(subtree(1)), 500, 5, 5), 4));
        let mut memo = ResolvedEnclosingCaptureMemo::default();
        {
            let anchors = Anchors(anchors.clone());
            assert_eq!(
                resolve(5, address(Some(subtree(2)), 40, 2, 4), &anchors, &mut memo),
                position(1540, 17)
            );
        }
        anchors.insert(subtree(2), anchor(address(Some(subtree(1)), 900, 9, 6), 4));
        let anchors = Anchors(anchors.clone());
        assert_eq!(
            resolve(5, address(Some(subtree(2)), 8, 1, 4), &anchors, &mut memo),
            position(1508, 16)
        );
        let mut fresh_memo = ResolvedEnclosingCaptureMemo::default();
        assert_eq!(
            resolve(5, address(Some(subtree(2)), 8, 1, 4), &anchors, &mut fresh_memo),
            None
        );
    }
}
