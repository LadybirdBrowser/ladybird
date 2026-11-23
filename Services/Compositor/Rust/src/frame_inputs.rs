/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a frame is painted with besides the display list and its resources.

use crate::context_scene::ReceivedLayerPlan;
use crate::overlays::OverlayPicture;
use libcompositing_rust::display_list::replay::ScrollbarShownByFrame;
use libcompositing_rust::scrolling::{AccumulatedVisualContextTree, ScrollStateSnapshot};
use libcompositing_rust::time::MonotonicTime;

pub struct FrameInputs {
    pub visual_context_tree: AccumulatedVisualContextTree,
    /// The plan of layers WebContent sent for the display list and the tree it published. None where it sent none, or
    /// where the frame draws the page through a visual viewport transform other than that of the published tree, as the
    /// plan does not hold for the frame then.
    pub layer_plan: Option<ReceivedLayerPlan>,
    pub presented_scroll_state_snapshot: ScrollStateSnapshot,
    /// The scrollbars of the page as the frame presents them, which replace the geometry the display list recorded for
    /// them.
    pub scrollbars: Vec<ScrollbarShownByFrame>,
    /// When the frame is painted, which is the time a blinking caret shows as of.
    pub painted_at: MonotonicTime,
    /// What the compositor draws over the page in the frame.
    pub overlays: OverlayPicture,
}
