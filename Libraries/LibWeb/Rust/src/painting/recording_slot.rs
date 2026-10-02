/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a document keeps of its display list recordings from one to the next.
//!
//! Only the recording entry, the publication and the trace reach it, through
//! [`LayoutNodeArena::recording`].

use crate::layout::LayoutNodeArena;
use crate::painting::paint_state::{PendingRecording, PendingRecordingTrace};
use crate::painting::record::recorder_state::RecorderState;
use std::cell::RefMut;

/// What a document keeps of its recordings: the recording its host is to publish, the trace that
/// recording left, and the recorder state the next recording records with.
#[derive(Default)]
pub(crate) struct RecordingSlot {
    pending_recording: Option<PendingRecording>,
    pending_recording_trace: Option<PendingRecordingTrace>,
    recorder: RecorderState,
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

    /// Leaves a recording, and the trace it left, pending for the host to publish.
    pub(crate) fn leave_pending(&mut self, pending: PendingRecording, trace: Option<PendingRecordingTrace>) {
        self.pending_recording = Some(pending);
        self.pending_recording_trace = trace;
    }

    /// Drops the pending recording unpublished.
    pub(crate) fn discard_pending_recording(&mut self) {
        self.pending_recording = None;
        self.pending_recording_trace = None;
    }

    /// The recorder state, which the recording entry and the publication read and write.
    pub(crate) fn recorder(&mut self) -> &mut RecorderState {
        &mut self.recorder
    }

    /// The recorder state, for a recording to take.
    pub(crate) fn take_recorder(&mut self) -> RecorderState {
        std::mem::take(&mut self.recorder)
    }

    /// Takes back the recorder state a recording was handed.
    pub(crate) fn give_back_recorder(&mut self, recorder: RecorderState) {
        self.recorder = recorder;
    }
}

impl LayoutNodeArena {
    /// What the document keeps of its recordings.
    pub(crate) fn recording(&self) -> RefMut<'_, RecordingSlot> {
        self.recording_slot().borrow_mut()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::painting::record::RecordingOutput;
    use std::sync::Arc;

    #[test]
    fn a_recording_hands_the_recorder_state_back_to_the_slot() {
        let arena = LayoutNodeArena::new();
        let published = Arc::new(RecordingOutput::default());
        arena.recording().recorder().published_recording = Some(published.clone());

        let recorder = arena.recording().take_recorder();
        assert!(arena.recording().recorder().published_recording.is_none());
        assert!(Arc::ptr_eq(recorder.published_recording.as_ref().unwrap(), &published));

        arena.recording().give_back_recorder(recorder);
        assert!(Arc::ptr_eq(
            arena.recording().recorder().published_recording.as_ref().unwrap(),
            &published
        ));
    }
}
