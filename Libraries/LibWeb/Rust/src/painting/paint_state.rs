/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
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
    pub(crate) root_background_source: Option<crate::painting::host::FfiRootBackgroundSource>,
    pub(crate) hit_test_list: Option<crate::painting::hit_test::HitTestList>,
    pub(crate) hit_test_list_generation: u64,
    pub(crate) last_recording: Option<Rc<crate::painting::record::RecordingOutput>>,
    pub(crate) paint_command_cache_source: Option<Rc<crate::painting::record::RecordingOutput>>,
    pub(crate) hit_test_item_cache_source: Option<Rc<crate::painting::record::cache::HitTestItemCacheSource>>,
    // The paint-order tree describing the published frame; a recording appends to it and
    // publication or discarding decides what stays.
    pub(crate) paint_order_tree: std::cell::RefCell<crate::painting::record::order_tree::PaintOrderTree>,
    pub(crate) selection: Option<crate::painting::selection::SelectionRange>,
    pub(crate) selection_pseudo_styles:
        std::collections::HashMap<NodeSlotId, Rc<crate::painting::record::paint::text::SelectionStyleAnswer>>,
}

impl PaintState {
    pub(crate) fn update_root_background_source(
        &mut self,
        arena: &crate::layout::LayoutNodeArena,
        source: crate::painting::host::FfiRootBackgroundSource,
    ) -> bool {
        let Some(previous) = self.root_background_source.replace(source) else {
            return false;
        };
        if previous == source {
            return false;
        }
        use crate::painting::record::damage::PaintDamage;
        // Propagation changes which box paints the body's background. Invalidate both
        // the old and new owners, including inline pieces cached by their containing block.
        for source in [previous, source] {
            for slot in [source.root_layout_node, source.body_layout_node] {
                arena.invalidate_for_repaint(slot);
                arena.push_paint_damage_for_repaint(slot, PaintDamage::DRAW_BACKGROUND);
            }
            // The viewport's scrollbars take their colors from the propagated background.
            if let Some(viewport) = arena.node_parent_if_live(source.root_layout_node) {
                arena.push_paint_damage(viewport, PaintDamage::DRAW_OVERLAY | PaintDamage::SCROLL_METADATA);
            }
        }
        true
    }

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
