/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use std::cell::RefCell;
use std::rc::Rc;

pub(crate) struct PendingRecording {
    pub(crate) recording: crate::painting::record::RecordingResult,
    pub(crate) recording_from_scratch: Option<crate::painting::record::RecordingResult>,
    pub(crate) paint_command_cache_read_write: bool,
}

pub(crate) struct PendingRecordingTrace {
    pub(crate) viewport: NodeSlotId,
    pub(crate) should_paint_overlay: bool,
}

#[derive(Default)]
pub struct PaintState {
    pub(crate) trace_recordings: bool,
    pub(crate) pending_recording_trace: Option<PendingRecordingTrace>,
    pub(crate) pending_recording: Option<PendingRecording>,
    pub(crate) visual_context: crate::painting::visual_context::VisualContextState,
    pub(crate) hit_test_list: Option<crate::painting::hit_test::HitTestList>,
    pub(crate) hit_test_list_generation: u64,
    pub(crate) last_recording: Option<Rc<crate::painting::record::RecordingOutput>>,
    pub(crate) paint_command_cache_source: Option<Rc<crate::painting::record::RecordingOutput>>,
    pub(crate) hit_test_item_cache_source: Option<Rc<crate::painting::record::cache::HitTestItemCacheSource>>,
    pub(crate) selection: Option<crate::painting::selection::SelectionRange>,
    pub(crate) selection_pseudo_styles:
        std::collections::HashMap<NodeSlotId, Rc<crate::painting::record::paint::text::SelectionStyleAnswer>>,
    pub(crate) per_recording_memo_tables: RefCell<crate::painting::record::scratch::PerRecordingMemoTables>,
}

impl PaintState {
    pub(crate) fn reset_visual_context_state(&mut self) {
        self.visual_context = crate::painting::visual_context::VisualContextState {
            needs_to_refresh_scroll_state: true,
            ..Default::default()
        };
        self.visual_context
            .dirty_boxes
            .request_full_rebuild(crate::painting::visual_context::dirty::VisualContextGlobalRebuildReason::FirstBuild);
    }
}
