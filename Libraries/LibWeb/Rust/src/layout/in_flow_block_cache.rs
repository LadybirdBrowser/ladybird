/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Reuse of an in-flow block-level child of a block formatting context whose subtree did not change, moved to where
//! the flow now puts it. A block container that does not establish a formatting context is laid out inside its
//! parent's run, so the formatting context run cache cannot see it. Its contents collapse their margins in a margin
//! state of their own, so reuse collapses what they left there with the margins before the box again, as the flow does
//! after laying them out.

use super::*;

/// Everything the layout of an in-flow block container and its subtree reads from outside that subtree, apart from
/// the flow position it starts at and the margins before it. Reuse moves the stored layout by the difference in flow
/// position, and collapses the margins its contents left with those now before it.
///
/// NB: Reuse is exact only while this key covers every such read. A new read of block formatting context or
///     containing block state inside the in-flow child path must be added here. Line clamping reads the position in
///     the formatting context root, so it may not be present for an entry to be stored or used. What the subtree
///     reads of floats is kept in the entry and has to hold where the box is placed now.
#[derive(Clone, PartialEq)]
pub(super) struct InFlowBlockKey {
    pub(super) input: LayoutInput,
    pub(super) containing_block_inline_size: CssPixels,
    pub(super) containing_block_is_right_to_left: bool,
    /// The block margins of the child that stretch-fit sizing subtracts, which it treats as zero on a side where the
    /// containing block has no border or padding.
    pub(super) stretch_fit_block_margins: (CssPixels, CssPixels),
    pub(super) incoming_outside_marker_line: Option<block_formatting_context::OutsideMarkerLine>,
    pub(super) run_root: Node,
    pub(super) treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
    pub(super) initial_containing_block_size: (CssPixels, CssPixels),
    pub(super) document_in_quirks_mode: bool,
}

impl InFlowBlockKey {
    pub(super) fn new(
        run: &FormattingContextRun<'_>,
        input: LayoutInput,
        containing_block_inline_size: CssPixels,
        containing_block_is_right_to_left: bool,
        stretch_fit_block_margins: (CssPixels, CssPixels),
        incoming_outside_marker_line: Option<block_formatting_context::OutsideMarkerLine>,
    ) -> Self {
        Self {
            input,
            containing_block_inline_size,
            containing_block_is_right_to_left,
            stretch_fit_block_margins,
            incoming_outside_marker_line,
            run_root: run.box_,
            treat_block_axis_percentage_insets_as_auto_beyond_root: run
                .treat_block_axis_percentage_insets_as_auto_beyond_root,
            initial_containing_block_size: (
                run.callbacks.initial_containing_block_inline_size,
                run.callbacks.initial_containing_block_block_size,
            ),
            document_in_quirks_mode: run.callbacks.document_in_quirks_mode,
        }
    }
}

#[derive(Clone)]
pub(super) struct FloatsReadByContents {
    pub(super) margins_before_box: block_formatting_context::CollapsedMargins,
    pub(super) border_box_offset_from_flow_position: CssPixels,
    pub(super) read_start_from_border_box: Option<CssPixels>,
    pub(super) floats_in_reach: smallvec::SmallVec<[block_formatting_context::FloatInReach; 2]>,
}

/// What the layout of an in-flow block container leaves behind for its parent, apart from the flow position it started
/// at and the margins before it.
#[derive(Clone)]
pub(super) struct InFlowBlockEntry {
    pub(super) key: InFlowBlockKey,
    pub(super) validity: fc_run_cache::FcRunCacheValidity,
    /// The child's cells as its layout left them.
    pub(super) cells: used_values::RootInputCells,
    /// The baselines of the child's contents as its layout left them.
    pub(super) baselines: formatting_context::DerivedBaselines,
    /// The inline offset of the child's content box in its containing block.
    pub(super) content_inline_offset: CssPixels,
    /// What the child's contents left in their margin state, with the top margin group they collapsed into.
    pub(super) margins_left_by_contents: block_formatting_context::BlockMarginState,
    pub(super) floats_read_by_contents: Option<FloatsReadByContents>,
    pub(super) outgoing_outside_marker_line: Option<block_formatting_context::OutsideMarkerLine>,
    pub(super) depends_on_percentage_block_size: bool,
    /// The fragment the stored layout placed. The committed subtree is reused only while it is still that fragment.
    pub(super) fragment_identity: u64,
}

/// The in-flow block layouts kept for one slot: the latest one, and the one whose fragment the box committed, which a
/// later layout whose result is not committed must not displace.
#[derive(Clone, Default)]
struct InFlowBlockSlot {
    latest: Option<std::sync::Arc<InFlowBlockEntry>>,
    committed: Option<std::sync::Arc<InFlowBlockEntry>>,
}

/// Per-document store of in-flow block layouts, surviving across layout passes in the layout stage's scratch.
#[derive(Default)]
pub(crate) struct InFlowBlockCacheStore {
    /// Written in place, while a fork of the arena reads them as they were when it was made.
    entries: super::pass_column::ForkSharedRows<InFlowBlockSlot>,
}

impl InFlowBlockCacheStore {
    pub(super) fn fork(&self) -> Self {
        Self {
            entries: self.entries.fork(),
        }
    }

    pub(super) fn cover_slots(&mut self, slot_count: usize) {
        self.entries.cover_slots(slot_count);
    }

    /// The stored layout of the box in `slot` for `key` that placed the fragment the box committed, `committed_identity`.
    pub(super) fn matching(
        &self,
        slot: u32,
        validity: fc_run_cache::FcRunCacheValidity,
        key: &InFlowBlockKey,
        committed_identity: u64,
    ) -> Option<std::sync::Arc<InFlowBlockEntry>> {
        self.entries.with_row(slot as usize, |slot| {
            [&slot?.committed, &slot?.latest]
                .into_iter()
                .flatten()
                .find(|entry| {
                    entry.validity == validity && entry.key == *key && entry.fragment_identity == committed_identity
                })
                .cloned()
        })
    }

    /// Stores `entry`, keeping the entry that placed the box's committed fragment, `committed_identity`.
    pub(super) fn store(&self, slot: u32, entry: InFlowBlockEntry, committed_identity: Option<u64>) {
        self.entries.with_row_mut(slot as usize, |slot| {
            let places_committed_fragment = |entry: &Option<std::sync::Arc<InFlowBlockEntry>>| {
                entry
                    .as_ref()
                    .is_some_and(|entry| Some(entry.fragment_identity) == committed_identity)
            };
            if places_committed_fragment(&slot.latest) {
                slot.committed = slot.latest.take();
            } else if !places_committed_fragment(&slot.committed) {
                slot.committed = None;
            }
            // An entry nothing else holds is written in place rather than allocated again.
            if let Some(existing) = slot.latest.as_mut().and_then(std::sync::Arc::get_mut) {
                *existing = entry;
                return;
            }
            slot.latest = Some(std::sync::Arc::new(entry));
        });
    }

    pub(crate) fn remove_entry(&mut self, slot: u32) {
        self.entries.reset_row_where(slot as usize, |entries| {
            entries.latest.is_some() || entries.committed.is_some()
        });
    }
}
