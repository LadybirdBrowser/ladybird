/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use std::rc::Rc;

#[derive(Default)]
pub struct PaintState {
    pub(crate) visual_context: crate::painting::visual_context::VisualContextState,
    pub(crate) hit_test_list: Option<crate::painting::hit_test::HitTestList>,
    pub(crate) hit_test_list_generation: u64,
    pub(crate) last_recording: Option<Rc<crate::painting::record::RecordingOutput>>,
    pub(crate) paint_command_cache_source: Option<Rc<crate::painting::record::RecordingOutput>>,
    pub(crate) hit_test_item_cache_source: Option<Rc<crate::painting::record::cache::HitTestItemCacheSource>>,
    pub(crate) recorded_wheel_event_listener_state_generation: Option<u64>,
    pub(crate) selection: Option<crate::painting::selection::SelectionRange>,
    pub(crate) scrollable_overflow_contained_boxes: std::collections::HashMap<NodeSlotId, Vec<NodeSlotId>>,
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
