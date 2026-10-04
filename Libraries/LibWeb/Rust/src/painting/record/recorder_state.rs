/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::NodeSlotId;
use crate::painting::record::inputs::RecordingInputs;
use crate::painting::record::order_tree::PaintOrderTree;
use crate::painting::record::scratch::RecordingScratch;
use crate::painting::record::{PublishedHitTestItems, RecordingOutput};
use std::cell::RefCell;
use std::sync::Arc;

/// What the display list recording keeps from one recording of a document to the next. Only the
/// recording and its publication write it, and nothing the document publishes for the paint side
/// is in it, so it moves with the recording rather than being shared with what it reads.
#[derive(Default)]
pub(crate) struct RecorderState {
    /// The last recording that published, which the next one copies clean output from.
    pub(crate) published_recording: Option<Arc<RecordingOutput>>,
    pub(crate) published_hit_test_items: Option<Arc<PublishedHitTestItems>>,
    /// The paint-order tree describing the published recording; a recording appends to it and
    /// publication or discarding decides what stays.
    pub(crate) paint_order_tree: PaintOrderTree,
    /// The recording's workspace, whose tables one recording leaves for the next to reuse.
    pub(crate) scratch: RecordingScratch,
    /// The absolute rects the recordings computed, which stay valid while the document's geometry
    /// does.
    pub(crate) absolute_rects: RefCell<AbsoluteRectMemo>,
    /// The inputs the last recording that publishes recorded with, which a clock lease's ticks record again with.
    pub(crate) published_inputs: Option<RecordingInputs>,
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
