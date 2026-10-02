/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a document keeps of its display list recordings from one to the next, and the recording
//! that runs on it.
//!
//! A [`RecordingJob`] owns the frame it records and the recorder state it records with, and returns
//! what it recorded with that state in a [`RecordingAnswer`]. Neither names the layout arena. The
//! slot is the document host's, on the host's thread: only the recording entry, the publication
//! and the trace reach it, through [`crate::render_state::DocumentHost::recording`].

use crate::layout::node_data::NodeSlotId;
use crate::painting::hit_test::HitTestList;
use crate::painting::paint_read::PaintSource;
use crate::painting::paint_state::{PendingRecording, PendingRecordingTrace};
use crate::painting::published_frame::PublishedFrame;
use crate::painting::record::RecordingInputs;
use crate::painting::record::recorder_state::RecorderState;

/// A display list recording: the frame it records, which it drops before it returns, and the
/// recorder state it records with, which it returns in its answer.
pub(crate) struct RecordingJob {
    frame: PublishedFrame,
    recorder: RecorderState,
    viewport: NodeSlotId,
    trace_recordings: bool,
}

/// What a recording answers: the recorder state it was handed, and what it recorded.
pub(crate) struct RecordingAnswer {
    recorder: RecorderState,
    pending: PendingRecording,
    trace: Option<PendingRecordingTrace>,
}

// A job reads nothing the document goes on writing, and an answer holds nothing of the document.
const _: () = {
    const fn assert_send<T: Send + 'static>() {}
    assert_send::<RecordingJob>();
    assert_send::<RecordingAnswer>();
};

impl RecordingJob {
    pub(crate) fn new(
        frame: PublishedFrame,
        recorder: RecorderState,
        viewport: NodeSlotId,
        trace_recordings: bool,
    ) -> Self {
        Self {
            frame,
            recorder,
            viewport,
            trace_recordings,
        }
    }

    /// Records the frame with the host's `inputs`. It takes no main thread token, so nothing it calls
    /// can reach the host.
    pub(crate) fn run(self, inputs: &RecordingInputs<'_>) -> RecordingAnswer {
        let Self {
            frame,
            mut recorder,
            viewport,
            trace_recordings,
        } = self;
        let RecorderState {
            published_recording,
            published_hit_test_items,
            paint_order_tree,
            scratch,
            absolute_rects,
        } = &mut recorder;
        let source = PaintSource::new(&frame, absolute_rects);
        // The retained tree describes the published tape and is written in place while a frame
        // is assembled, so only a recording that publishes may copy from that frame or touch
        // the tree; any other recording records from scratch into a tree of its own.
        let mut throwaway_tree = crate::painting::record::order_tree::PaintOrderTree::default();
        let (tree, source_recording, source_items) = if inputs.publishes_recording {
            (
                paint_order_tree,
                published_recording.clone(),
                published_hit_test_items.clone(),
            )
        } else {
            (&mut throwaway_tree, None, None)
        };
        let copies_from_published_recording = source_recording.is_some();
        let recording = crate::painting::record::traversal::record_display_list(
            &source,
            scratch,
            tree,
            viewport,
            inputs,
            source_recording,
            source_items,
            true,
            trace_recordings || crate::painting::record::verify::enabled_by_environment(),
        );
        // The oracle records the same frame from scratch into a throwaway tree whenever the
        // published recording could have been copied from.
        let recording_from_scratch =
            (crate::painting::record::verify::enabled_by_environment() && copies_from_published_recording).then(|| {
                let mut inputs_for_recording_from_scratch = inputs.clone();
                inputs_for_recording_from_scratch.publishes_recording = false;
                let mut tree_for_recording_from_scratch =
                    crate::painting::record::order_tree::PaintOrderTree::default();
                crate::painting::record::traversal::record_display_list(
                    &source,
                    scratch,
                    &mut tree_for_recording_from_scratch,
                    viewport,
                    &inputs_for_recording_from_scratch,
                    None,
                    None,
                    false,
                    false,
                )
            });
        let trace = (trace_recordings && recording.output.capture_log_for_verification.is_some()).then_some(
            PendingRecordingTrace {
                viewport,
                should_paint_overlay: inputs.should_paint_overlay,
            },
        );
        let svg_paint_resources = frame.svg_paint_resources().clone();
        drop(frame);
        RecordingAnswer {
            recorder,
            pending: PendingRecording {
                recording,
                recording_from_scratch,
                publishes_recording: inputs.publishes_recording,
                svg_paint_resources,
            },
            trace,
        }
    }
}

/// What a document keeps of its recordings: the recording its host is to publish, the trace that
/// recording left, the recorder state the next recording records with, and the hit-test list of the
/// last recording published, which hit testing reads.
#[derive(Default)]
pub(crate) struct RecordingSlot {
    pending_recording: Option<PendingRecording>,
    pending_recording_trace: Option<PendingRecordingTrace>,
    pub(super) recorder: RecorderState,
    pub(super) hit_test_list: Option<HitTestList>,
}

impl RecordingSlot {
    pub(crate) fn has_pending_recording(&self) -> bool {
        self.pending_recording.is_some()
    }

    /// The pending recording, for the host to publish.
    pub(crate) fn take_pending_recording(&mut self) -> Option<PendingRecording> {
        self.pending_recording.take()
    }

    /// The trace the pending recording left, for the host to read.
    pub(crate) fn take_pending_recording_trace(&mut self) -> Option<PendingRecordingTrace> {
        self.pending_recording_trace.take()
    }

    /// The recorder state, which the recording entry and the publication read and write.
    pub(crate) fn recorder(&mut self) -> &mut RecorderState {
        &mut self.recorder
    }

    /// The hit-test list of the last recording published, if any was.
    pub(crate) fn hit_test_list(&mut self) -> &mut Option<HitTestList> {
        &mut self.hit_test_list
    }

    /// How many items the next recording's hit-test list may hold, going by the last one's.
    pub(crate) fn hit_test_item_capacity_hint(&self) -> usize {
        self.hit_test_list.as_ref().map_or(0, |list| list.items.len())
    }

    /// The recorder state, for a recording to take until it answers.
    pub(crate) fn take_recorder(&mut self) -> RecorderState {
        std::mem::take(&mut self.recorder)
    }

    /// Takes in what a recording answered, leaving the recording pending for the host to publish.
    pub(crate) fn accept_recording_answer(&mut self, answer: RecordingAnswer) {
        self.recorder = answer.recorder;
        self.pending_recording_trace = answer.trace;
        self.pending_recording = Some(answer.pending);
    }

    /// Drops the pending recording unpublished. A recording that publishes wrote the retained
    /// paint-order tree, so the next one copies nothing from the published recording.
    pub(crate) fn discard_pending_recording(&mut self) {
        self.pending_recording_trace = None;
        if self
            .pending_recording
            .take()
            .is_some_and(|pending| pending.publishes_recording)
        {
            self.recorder.forget_published_recording();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::LayoutNodeArena;
    use crate::painting::record::{RecordingOutput, RecordingResult};
    use std::sync::Arc;

    fn pending(publishes_recording: bool) -> PendingRecording {
        PendingRecording {
            recording: RecordingResult {
                output: RecordingOutput::default(),
                resources: Default::default(),
            },
            recording_from_scratch: None,
            publishes_recording,
            svg_paint_resources: Default::default(),
        }
    }

    /// Leaves `pending` in the slot as a recording's answer would.
    fn accept_pending(slot: &mut RecordingSlot, pending: PendingRecording) {
        let recorder = slot.take_recorder();
        slot.accept_recording_answer(RecordingAnswer {
            recorder,
            pending,
            trace: None,
        });
    }

    #[test]
    fn the_slot_lends_its_recorder_state_to_a_job_and_takes_it_back_with_the_answer() {
        let mut arena = LayoutNodeArena::new();
        let mut slot = RecordingSlot::default();
        let published = Arc::new(RecordingOutput::default());
        slot.recorder().published_recording = Some(published.clone());

        let job = RecordingJob::new(arena.freeze_frame(0), slot.take_recorder(), NodeSlotId::INVALID, false);
        assert!(slot.recorder().published_recording.is_none());
        let RecordingJob { recorder, .. } = job;
        slot.accept_recording_answer(RecordingAnswer {
            recorder,
            pending: pending(true),
            trace: None,
        });
        assert!(Arc::ptr_eq(
            slot.recorder().published_recording.as_ref().unwrap(),
            &published
        ));
        assert!(slot.has_pending_recording());
    }

    #[test]
    fn a_discarded_publishing_recording_forgets_its_source() {
        let mut slot = RecordingSlot::default();
        let published = Arc::new(RecordingOutput::default());
        slot.recorder().published_recording = Some(published.clone());

        accept_pending(&mut slot, pending(false));
        slot.discard_pending_recording();
        assert!(!slot.has_pending_recording());
        assert!(slot.recorder().published_recording.is_some());

        accept_pending(&mut slot, pending(true));
        slot.discard_pending_recording();
        assert!(slot.recorder().published_recording.is_none());
    }
}
