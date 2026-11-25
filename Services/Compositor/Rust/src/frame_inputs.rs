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
use libgfx_rust::AffineTransform;

pub struct FrameInputs {
    /// The tree the frame rasters the page with. Its visual viewport transform can differ from the one the frame shows,
    /// which `raster_to_frame` then maps to.
    pub visual_context_tree: AccumulatedVisualContextTree,
    /// How the frame maps the raster of the page to its own pixels, where it shows the page through a visual viewport
    /// transform other than that of `visual_context_tree`: a scale and a translation. None where it shows the page as
    /// the tree places it.
    pub raster_to_frame: Option<AffineTransform>,
    /// The plan of layers WebContent sent for the display list and a tree it published, which holds wherever the visual
    /// viewport is. None where it sent none, or where the frame shows the page through a visual viewport that rotates,
    /// skews or shrinks it.
    pub layer_plan: Option<ReceivedLayerPlan>,
    /// How many raster pixels the frame rasters each device pixel of the layout viewport at, a power of two, which
    /// `raster_to_frame` maps to the frame's pixels.
    pub raster_scale: i32,
    pub presented_scroll_state_snapshot: ScrollStateSnapshot,
    /// The scrollbars of the page as the frame presents them, which replace the geometry the display list recorded for
    /// them.
    pub scrollbars: Vec<ScrollbarShownByFrame>,
    /// When the frame is painted, which is the time a blinking caret shows as of.
    pub painted_at: MonotonicTime,
    /// What the compositor draws over the page in the frame.
    pub overlays: OverlayPicture,
}
