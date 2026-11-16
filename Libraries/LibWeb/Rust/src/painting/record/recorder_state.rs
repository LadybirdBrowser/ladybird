/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::NodeSlotId;
use crate::painting::record::RecordingOutput;
use crate::painting::record::inputs::RecordingInputs;
use crate::painting::record::scratch::RecordingScratch;
use std::cell::RefCell;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

/// Counts the recordings of a document's host that published. A fork of the recorder state copies from the
/// recording that was published when it forked, and tells by the count when the host has published another since.
#[derive(Default)]
pub(crate) struct PublishedRecordingCount {
    shared: Arc<AtomicU64>,
    as_of_fork: Option<u64>,
}

impl PublishedRecordingCount {
    fn fork(&self) -> Self {
        Self {
            shared: Arc::clone(&self.shared),
            as_of_fork: Some(self.shared.load(Ordering::Acquire)),
        }
    }

    pub(crate) fn is_of_a_fork(&self) -> bool {
        self.as_of_fork.is_some()
    }

    /// Whether the host published no recording since this fork was made. The host's own count is always current.
    pub(crate) fn is_current(&self) -> bool {
        self.as_of_fork
            .is_none_or(|count| count == self.shared.load(Ordering::Acquire))
    }

    /// Answers whether a recording that asks to publish does: only the host's recordings publish. Counts it where
    /// it does.
    pub(crate) fn note_recording(&self, asks_to_publish: bool) -> bool {
        let publishes = asks_to_publish && !self.is_of_a_fork();
        if publishes {
            self.shared.fetch_add(1, Ordering::AcqRel);
        }
        publishes
    }
}

/// What the display list recording keeps from one recording of a document to the next. Only the
/// recording and its publication write it, and nothing the document publishes for the paint side
/// is in it, so it moves with the recording rather than being shared with what it reads.
#[derive(Default)]
pub(crate) struct RecorderState {
    /// The last recording that published, which the next one reuses clean output from.
    pub(crate) published_recording: Option<Arc<RecordingOutput>>,
    pub(crate) published_recording_count: PublishedRecordingCount,
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
    /// from the same recording, and keeps no scratch or memo of its own.
    pub(crate) fn fork(&self) -> Self {
        Self {
            published_recording: self.published_recording.clone(),
            published_recording_count: self.published_recording_count.fork(),
            scratch: Default::default(),
            absolute_rects: Default::default(),
            published_inputs: self.published_inputs.clone(),
        }
    }
}

const ABSOLUTE_RECTS_PER_PAGE: usize = 64;

type MemoizedAbsoluteRect = Option<(NodeSlotId, u64, CssPixelRect)>;

/// Each row's absolute rect, with the geometry epoch of the frame it was computed from, in pages of rows made as a rect
/// in them is memoized: a memo of a few rows costs their pages, wherever the rows are in the arena.
#[derive(Default)]
pub(crate) struct AbsoluteRectMemo {
    pages: Vec<Option<Box<[MemoizedAbsoluteRect; ABSOLUTE_RECTS_PER_PAGE]>>>,
}

impl AbsoluteRectMemo {
    pub(crate) fn get(&self, id: NodeSlotId, epoch: u64) -> Option<CssPixelRect> {
        let index = id.slot_index() as usize;
        let page = self.pages.get(index / ABSOLUTE_RECTS_PER_PAGE)?.as_deref()?;
        let (memoized_id, memoized_epoch, rect) = page[index % ABSOLUTE_RECTS_PER_PAGE]?;
        (memoized_id == id && memoized_epoch == epoch).then_some(rect)
    }

    pub(crate) fn set(&mut self, id: NodeSlotId, epoch: u64, rect: CssPixelRect) {
        let index = id.slot_index() as usize;
        let page_index = index / ABSOLUTE_RECTS_PER_PAGE;
        if self.pages.len() <= page_index {
            self.pages.resize_with(page_index + 1, || None);
        }
        let page = self.pages[page_index].get_or_insert_with(|| Box::new([None; ABSOLUTE_RECTS_PER_PAGE]));
        page[index % ABSOLUTE_RECTS_PER_PAGE] = Some((id, epoch, rect));
    }
}

const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<RecorderState>();
};

impl RecorderState {
    /// Forgets the published recording: the next recording copies nothing and records from scratch.
    pub(crate) fn forget_published_recording(&mut self) {
        self.published_recording = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_fork_copies_from_its_recording_until_the_host_publishes_another() {
        let host = RecorderState::default();
        assert!(host.published_recording_count.note_recording(true));
        let fork = host.fork();
        assert!(fork.published_recording_count.is_of_a_fork());
        assert!(!host.published_recording_count.is_of_a_fork());

        // A recording of the host that publishes nothing leaves the fork's recording the published one.
        assert!(!host.published_recording_count.note_recording(false));
        assert!(fork.published_recording_count.is_current());

        // The fork never publishes, whatever its recording asks.
        assert!(!fork.published_recording_count.note_recording(true));
        assert!(fork.published_recording_count.is_current());

        assert!(host.published_recording_count.note_recording(true));
        assert!(!fork.published_recording_count.is_current());
        assert!(host.published_recording_count.is_current());
    }
}
