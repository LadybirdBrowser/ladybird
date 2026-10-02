/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::painting::record::paint::text::SelectionStyleAnswer;
use crate::painting::selection::{HighlightPseudoElement, SearchTextHighlights, SelectionRange};
use std::collections::HashMap;
use std::sync::Arc;

pub(crate) struct PendingRecording {
    pub(crate) recording: crate::painting::record::RecordingResult,
    pub(crate) recording_from_scratch: Option<crate::painting::record::RecordingResult>,
    pub(crate) publishes_recording: bool,
    /// The SVG paint resources the recording's frame was published with, whose filter images
    /// its publication hands to the host.
    pub(crate) svg_paint_resources: Arc<crate::painting::svg_paint_resources::SvgPaintResourceRows>,
}

pub(crate) struct PendingRecordingTrace {
    pub(crate) viewport: NodeSlotId,
    pub(crate) should_paint_overlay: bool,
}

#[derive(Default)]
pub struct PaintState {
    pub(crate) trace_recordings: bool,
    pub(crate) visual_context: crate::painting::visual_context::VisualContextState,
    pub(crate) root_background_source: Option<crate::painting::host::RootBackgroundSource>,
    pub(crate) hit_test_list_generation: u64,
    pub(crate) last_recording: Option<Arc<crate::painting::record::RecordingOutput>>,
    /// The selection, shared with the frames published while it holds.
    pub(crate) selection: Option<Arc<SelectionRange>>,
    /// The `::selection` styles, shared with the frames published while they hold, so a write
    /// copies the table only while a frame still holds it.
    pub(crate) selection_pseudo_styles: Arc<SelectionPseudoStyles>,
    /// The find-in-page matches, shared with the frames published while they hold.
    pub(crate) search_text: Arc<SearchTextHighlights>,
    /// The `::search-text` styles, shared as the `::selection` styles are.
    pub(crate) search_text_pseudo_styles: Arc<SelectionPseudoStyles>,
    /// The `::search-text:current` styles, shared as the `::selection` styles are.
    pub(crate) search_text_current_pseudo_styles: Arc<SelectionPseudoStyles>,
}

/// Each row's committed style for one highlight pseudo-element.
pub(crate) type SelectionPseudoStyles = HashMap<NodeSlotId, Arc<SelectionStyleAnswer>>;

impl PaintState {
    pub(crate) fn highlight_pseudo_styles_mut(
        &mut self,
        highlight: HighlightPseudoElement,
    ) -> &mut Arc<SelectionPseudoStyles> {
        match highlight {
            HighlightPseudoElement::Selection => &mut self.selection_pseudo_styles,
            HighlightPseudoElement::SearchText => &mut self.search_text_pseudo_styles,
            HighlightPseudoElement::SearchTextCurrent => &mut self.search_text_current_pseudo_styles,
        }
    }

    pub(crate) fn update_root_background_source(
        &mut self,
        arena: &crate::layout::LayoutNodeArena,
        source: crate::painting::host::RootBackgroundSource,
    ) -> bool {
        let Some(previous) = self.root_background_source.replace(source) else {
            return false;
        };
        if previous == source {
            return false;
        }
        use crate::painting::record::damage::PaintDamage;
        // Propagation changes which box paints the body's background. Push both the old
        // and new owners, including inline pieces painted by their containing block.
        for source in [previous, source] {
            for slot in [source.root_layout_node, source.body_layout_node] {
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
