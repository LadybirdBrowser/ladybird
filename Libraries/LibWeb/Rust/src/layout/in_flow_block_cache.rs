/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Reuse of an in-flow block-level child of a block formatting context whose subtree did not change, moved to where
//! the flow now puts it. A block container that does not establish a formatting context is laid out inside its
//! parent's run, so the formatting context run cache cannot see it: besides its layout input, it reads the margins
//! its preceding siblings left open and the flow position, and it leaves margins and a flow position behind.

use super::*;

/// Everything the layout of an in-flow block container and its subtree reads from outside that subtree, apart from
/// the flow position it starts at. Reuse moves the stored layout by the difference in flow position.
///
/// NB: Reuse is exact only while this key covers every such read. A new read of block formatting context or
///     containing block state inside the in-flow child path must be added here. Floats and line clamping read the
///     position in the formatting context root; neither may be present for an entry to be stored or used.
#[derive(Clone, PartialEq)]
pub(super) struct InFlowBlockKey {
    pub(super) input: LayoutInput,
    pub(super) containing_block_inline_size: CssPixels,
    pub(super) containing_block_is_right_to_left: bool,
    pub(super) incoming_margins: block_formatting_context::BlockMarginState,
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
        incoming_margins: block_formatting_context::BlockMarginState,
    ) -> Self {
        Self {
            input: LayoutInput {
                content_box_position_in_bfc_root: None,
                ..input
            },
            containing_block_inline_size,
            containing_block_is_right_to_left,
            incoming_margins,
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

/// What the layout of an in-flow block container leaves behind for its parent, relative to the flow position it
/// started at.
#[derive(Clone)]
pub(super) struct InFlowBlockEntry {
    pub(super) key: InFlowBlockKey,
    pub(super) validity: fc_run_cache::FcRunCacheValidity,
    /// The child's record as it was before its placement.
    pub(super) cells: used_values::UsedValuesCellState,
    pub(super) offset_from_flow_position: FfiCssPixelPoint,
    pub(super) flow_position_advance: CssPixels,
    pub(super) outgoing_margins: block_formatting_context::BlockMarginState,
    pub(super) margin_box_bottom_from_flow_position: CssPixels,
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

/// Per-document store of in-flow block layouts, surviving across layout passes on the node arena.
#[derive(Clone, Default)]
pub(crate) struct InFlowBlockCacheStore {
    entries: RefCell<Vec<InFlowBlockSlot>>,
}

impl InFlowBlockCacheStore {
    /// The stored layout of the box in `slot` for `key` that placed the fragment the box committed, `committed_identity`.
    pub(super) fn matching(
        &self,
        slot: u32,
        validity: fc_run_cache::FcRunCacheValidity,
        key: &InFlowBlockKey,
        committed_identity: u64,
    ) -> Option<std::sync::Arc<InFlowBlockEntry>> {
        let entries = self.entries.borrow();
        let slot = entries.get(slot as usize)?;
        [&slot.committed, &slot.latest]
            .into_iter()
            .flatten()
            .find(|entry| {
                entry.validity == validity && entry.key == *key && entry.fragment_identity == committed_identity
            })
            .cloned()
    }

    /// Stores `entry`, keeping the entry that placed the box's committed fragment, `committed_identity`.
    pub(super) fn store(&self, slot: u32, entry: InFlowBlockEntry, committed_identity: Option<u64>) {
        let mut entries = self.entries.borrow_mut();
        if entries.len() <= slot as usize {
            entries.resize_with(slot as usize + 1, InFlowBlockSlot::default);
        }
        let slot = &mut entries[slot as usize];
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
    }

    pub(crate) fn remove_entry(&self, slot: u32) {
        if let Some(slot) = self.entries.borrow_mut().get_mut(slot as usize) {
            *slot = InFlowBlockSlot::default();
        }
    }
}

/// Whether every in-flow reuse also lays the child out and checks that both agree.
pub(super) fn verification_is_enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var_os("LIBWEB_VERIFY_IN_FLOW_REUSE").is_some())
}

pub(super) fn verify_reuse(
    node: Node,
    stored: &InFlowBlockEntry,
    committed: &fragment_tree::Fragment,
    fresh: &InFlowBlockEntry,
    fresh_fragment: &fragment_tree::Fragment,
    fresh_is_reusable: bool,
    was_reused: &impl Fn(u32) -> bool,
) {
    assert!(
        fresh_is_reusable,
        "a reused in-flow layout of {node:?} is no longer reusable when laid out"
    );
    assert_eq!(
        stored.cells, fresh.cells,
        "the reused record of {node:?} differs from its layout"
    );
    assert_eq!(
        stored.offset_from_flow_position, fresh.offset_from_flow_position,
        "the reused offset of {node:?} differs from its layout"
    );
    assert_eq!(
        stored.flow_position_advance, fresh.flow_position_advance,
        "the reused flow advance of {node:?} differs from its layout"
    );
    assert!(
        stored.outgoing_margins == fresh.outgoing_margins,
        "the reused outgoing margins of {node:?} differ from its layout"
    );
    assert_eq!(
        stored.margin_box_bottom_from_flow_position, fresh.margin_box_bottom_from_flow_position,
        "the reused margin box bottom of {node:?} differs from its layout"
    );
    assert_eq!(
        stored.depends_on_percentage_block_size, fresh.depends_on_percentage_block_size,
        "the reused percentage block size dependency of {node:?} differs from its layout"
    );
    assert!(
        fresh_fragment.lays_out_like(committed, was_reused),
        "the reused subtree of {node:?} differs from its layout"
    );
}
