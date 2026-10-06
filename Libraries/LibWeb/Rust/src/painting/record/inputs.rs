/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixels};
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::UniqueNodeId;
use crate::painting::ffi::FfiChromeMetrics;
use crate::painting::force_dark::ForceDarkSettings;
use crate::painting::host::{FfiFlexOverlayInput, FfiGridOverlayInput, RootBackgroundSource};
use libgfx_rust::font::FontHandle;
use libgfx_rust::{Color, IntRect, IntSize};

/// The inputs read by content that is recorded outside per-box captures: scroll metadata,
/// viewport scrollbars, the wheel-target facts of hit-test items. Reusing a subtree capture from
/// the previous tape requires this whole bundle to be unchanged, so every reader takes these
/// values from here and a new one is part of that check automatically.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct UncapturedContentInputs {
    pub root_background_source: RootBackgroundSource,
    // Scroll commands use a scrollport at the origin. Its position is compositor state.
    pub device_viewport_size: IntSize,
    pub document_id: UniqueNodeId,
    pub has_blocking_wheel_event_region_covering_viewport: bool,
    pub chrome_metrics: FfiChromeMetrics,
    pub paint_viewport_scrollbars: bool,
    pub middle_button_scroll_origin: Option<CssPixelPoint>,
    pub canvas_color: Color,
    pub background_color: Color,
}

/// The inputs of one recording, which own what they read, so that the recording may outlive the
/// call that started it. Recording results retain their own resources and never borrow these inputs.
#[derive(Clone)]
pub(crate) struct RecordingInputs {
    pub device_pixels_per_css_pixel: f64,
    pub uncaptured: UncapturedContentInputs,
    // Carried on the recording so the compositor can tell which listener state it saw; no
    // recorded content reads it.
    pub wheel_event_listener_state_generation: u64,
    pub css_viewport_rect: CssPixelRect,
    pub should_show_line_box_borders: bool,
    pub force_dark_enabled: bool,
    pub force_dark_settings: ForceDarkSettings,
    pub should_paint_overlay: bool,
    pub canvas_fill_rect: Option<IntRect>,
    pub opaque_canvas: bool,
    pub bitmap_rect: IntRect,
    pub publishes_recording: bool,
    pub window_is_focused: bool,
    pub outline_auto_color: Color,
    pub selection_background_from_palette: Color,
    pub selection_background_light: Color,
    pub selection_background_dark: Color,
    pub inactive_selection_background_from_palette: Color,
    pub inactive_selection_background_light: Color,
    pub inactive_selection_background_dark: Color,
    pub palette_is_dark: bool,
    pub document_has_supported_color_schemes: bool,
    pub inspector_highlight: Option<InspectorHighlight>,
    pub tooltip_color: Color,
    pub tooltip_text_color: Color,
    pub tooltip_border_color: Color,
    pub grid_overlays: Option<GridOverlays>,
    // These array elements are already plain values without pointers or optional-value tags.
    pub flex_overlays: Box<[FfiFlexOverlayInput]>,
    pub caret_debug_rect: Option<CssPixelRect>,
    pub caret: Option<CaretPaint>,
    pub focused_text_control: Option<FocusedTextControlSelection>,
    pub focused_area_outline: Option<FocusedAreaOutline>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum CaretTarget {
    InBlock {
        block: NodeSlotId,
        owner: Option<NodeSlotId>,
    },
    EmptyInline(NodeSlotId),
}

#[derive(Clone, Copy)]
pub(crate) struct CaretPaint {
    pub target: CaretTarget,
    pub rect: CssPixelRect,
    pub color: Color,
    pub blink_cycle_start_time_ns: i64,
    pub should_blink: bool,
}

#[derive(Clone, Copy)]
pub(crate) struct FocusedTextControlSelection {
    pub text_node: NodeSlotId,
    pub start: usize,
    pub end: usize,
}

#[derive(Clone)]
pub(crate) struct FocusedAreaOutline {
    pub image: NodeSlotId,
    pub path_bytes: Box<[u8]>,
    pub color: Color,
    pub width: CssPixels,
}

#[derive(Clone)]
pub(crate) struct OverlayLabelFonts {
    pub css_font: FontHandle,
    pub device_font: FontHandle,
}

#[derive(Clone)]
pub(crate) struct InspectorHighlight {
    pub paintable: NodeSlotId,
    pub label: String,
    pub fonts: OverlayLabelFonts,
}

#[derive(Clone)]
pub(crate) struct GridOverlays {
    pub inputs: Box<[FfiGridOverlayInput]>,
    pub fonts: OverlayLabelFonts,
}
