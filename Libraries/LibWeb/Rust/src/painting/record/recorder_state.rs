/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::NodeSlotId;
use crate::painting::record::inputs::RecordingInputs;
use crate::painting::record::order_tree::{PaintOrderTree, PaintOrderTreeAccess};
use crate::painting::record::scratch::RecordingScratch;
use crate::painting::record::{PublishedHitTestItems, RecordingOutput};
use std::cell::RefCell;
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};

#[derive(Default)]
struct VersionedPaintOrderTree {
    tree: PaintOrderTree,
    rewrites: u64,
}

/// A hold on a document's one paint-order tree, which only the recordings of its host rewrite. A fork of the recorder
/// state reads the tree as the recording it forked from left it, up to the next recording that rewrites it.
#[derive(Default)]
pub(crate) struct PaintOrderTreeHandle {
    shared: Arc<Mutex<VersionedPaintOrderTree>>,
    reads_as_of: Option<u64>,
}

impl PaintOrderTreeHandle {
    fn lock(&self) -> MutexGuard<'_, VersionedPaintOrderTree> {
        self.shared.lock().unwrap_or_else(PoisonError::into_inner)
    }

    fn reader(&self) -> Self {
        Self {
            shared: Arc::clone(&self.shared),
            reads_as_of: Some(self.lock().rewrites),
        }
    }

    pub(crate) fn is_reader(&self) -> bool {
        self.reads_as_of.is_some()
    }

    /// Whether the tree is as this hold first found it, or this hold is the host's, which always reads it as it is.
    pub(crate) fn reads_current_tree(&self) -> bool {
        self.reads_as_of.is_none_or(|rewrites| rewrites == self.lock().rewrites)
    }

    /// Runs `recording` on the tree, which it rewrites where `publishes_recording` and this hold is the host's, and
    /// otherwise only reads. Answers what it answered, and whether it rewrote the tree.
    pub(crate) fn with_access<R>(
        &self,
        publishes_recording: bool,
        recording: impl FnOnce(PaintOrderTreeAccess<'_>) -> R,
    ) -> (R, bool) {
        let mut held = self.lock();
        let held = &mut *held;
        let rewrites = publishes_recording && !self.is_reader();
        let access = match rewrites {
            true => {
                held.rewrites += 1;
                PaintOrderTreeAccess::Rewrites(&mut held.tree)
            }
            false => PaintOrderTreeAccess::Reads(&held.tree),
        };
        (recording(access), rewrites)
    }
}

/// What the display list recording keeps from one recording of a document to the next. Only the
/// recording and its publication write it, and nothing the document publishes for the paint side
/// is in it, so it moves with the recording rather than being shared with what it reads.
#[derive(Default)]
pub(crate) struct RecorderState {
    /// The last recording that published, which the next one copies clean output from.
    pub(crate) published_recording: Option<Arc<RecordingOutput>>,
    pub(crate) published_hit_test_items: Option<Arc<PublishedHitTestItems>>,
    /// The paint-order tree describing the published recording; a recording that publishes
    /// appends to it and publication or discarding decides what stays.
    pub(crate) paint_order_tree: PaintOrderTreeHandle,
    /// The recording's workspace, whose tables one recording leaves for the next to reuse.
    pub(crate) scratch: RecordingScratch,
    /// The absolute rects the recordings computed, which stay valid while the document's geometry
    /// does.
    pub(crate) absolute_rects: RefCell<AbsoluteRectMemo>,
    /// The inputs the last recording that publishes recorded with, which a clock lane's ticks record again with.
    pub(crate) published_inputs: Option<RecordingInputs>,
}

impl RecorderState {
    /// A fork of the recorder state, for a fork of the document's render state to record with: it copies clean output
    /// from the same recording, reads the same paint-order tree, and keeps no scratch or memo of its own.
    pub(crate) fn fork(&self) -> Self {
        Self {
            published_recording: self.published_recording.clone(),
            published_hit_test_items: self.published_hit_test_items.clone(),
            paint_order_tree: self.paint_order_tree.reader(),
            scratch: Default::default(),
            absolute_rects: Default::default(),
            published_inputs: self.published_inputs.clone(),
        }
    }
}

/// Each row's absolute rect, with the geometry epoch of the frame it was computed from.
#[derive(Default)]
pub(crate) struct AbsoluteRectMemo {
    rects: Vec<Option<(NodeSlotId, u64, CssPixelRect)>>,
}

impl AbsoluteRectMemo {
    pub(crate) fn get(&self, id: NodeSlotId, epoch: u64) -> Option<CssPixelRect> {
        let (memoized_id, memoized_epoch, rect) = (*self.rects.get(id.slot_index() as usize)?)?;
        (memoized_id == id && memoized_epoch == epoch).then_some(rect)
    }

    pub(crate) fn set(&mut self, id: NodeSlotId, epoch: u64, rect: CssPixelRect) {
        let index = id.slot_index() as usize;
        if self.rects.len() <= index {
            self.rects.resize(index + 1, None);
        }
        self.rects[index] = Some((id, epoch, rect));
    }
}

const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<RecorderState>();
};

impl RecorderState {
    /// A recording that publishes assembles its frame in the retained paint-order tree, which no
    /// longer describes the published recording once that recording is dropped: the next recording
    /// copies nothing from it and records from scratch into a tree of its own.
    pub(crate) fn forget_published_recording(&mut self) {
        self.published_recording = None;
        self.published_hit_test_items = None;
        self.paint_order_tree = Default::default();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::painting::paint_order_plan::PaintScope;
    use crate::painting::record::order_tree::{ChildEntry, OutputSize, ScopeId};

    fn publish_a_tree(handle: &PaintOrderTreeHandle) -> bool {
        let (_, rewrote) = handle.with_access(true, |mut access| {
            if let Some(tree) = access.rewrites() {
                let scope = PaintScope::stacking_context(NodeSlotId::new(1, 1));
                let root = tree.allocate_scope(scope, ScopeId::NONE);
                tree.finish_recording(root, ChildEntry::scope(root, OutputSize::default()));
            }
        });
        rewrote
    }

    fn scope_count(handle: &PaintOrderTreeHandle) -> usize {
        handle.with_access(false, |access| access.read().scope_count()).0
    }

    #[test]
    fn a_fork_reads_the_tree_of_its_frame_until_the_host_rewrites_it() {
        let mut host = RecorderState::default();
        assert!(publish_a_tree(&host.paint_order_tree));
        let fork = host.fork();
        assert!(fork.paint_order_tree.is_reader());
        assert!(!host.paint_order_tree.is_reader());
        assert_eq!(scope_count(&fork.paint_order_tree), 1);

        // A recording of the host that publishes nothing leaves the tree as the fork read it.
        scope_count(&host.paint_order_tree);
        assert!(fork.paint_order_tree.reads_current_tree());

        // The fork never rewrites the tree, whatever its recording publishes.
        assert!(!publish_a_tree(&fork.paint_order_tree));
        assert!(fork.paint_order_tree.reads_current_tree());

        assert!(publish_a_tree(&host.paint_order_tree));
        assert!(!fork.paint_order_tree.reads_current_tree());
        assert!(host.paint_order_tree.reads_current_tree());

        // Forgetting the published recording starts a tree of the host's own, and leaves the one the fork holds.
        let fork = host.fork();
        let scopes_the_fork_reads = scope_count(&fork.paint_order_tree);
        host.forget_published_recording();
        assert_eq!(scope_count(&host.paint_order_tree), 0);
        assert_eq!(scope_count(&fork.paint_order_tree), scopes_the_fork_reads);
        assert!(fork.paint_order_tree.reads_current_tree());
    }
}
