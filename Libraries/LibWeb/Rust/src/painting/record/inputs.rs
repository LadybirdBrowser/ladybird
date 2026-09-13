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
use crate::painting::host::{FfiFlexOverlayInput, FfiGridOverlayInput, FfiRootBackgroundSource};
use libgfx_rust::font::FontHandle;
use libgfx_rust::{Color, IntRect};
use std::borrow::Cow;

/// Inputs borrowed for one synchronous recording call. Recording results retain their own
/// resources and never borrow these inputs or the host's arrays and byte buffers.
#[derive(Clone)]
pub(crate) struct RecordingInputs<'a> {
    pub device_pixels_per_css_pixel: f64,
    pub viewport_wheel_overflow_x: u8,
    pub viewport_wheel_overflow_y: u8,
    pub root_background_source: FfiRootBackgroundSource,
    pub device_viewport_rect: IntRect,
    pub css_viewport_rect: CssPixelRect,
    pub should_show_line_box_borders: bool,
    pub force_dark_enabled: bool,
    pub force_dark_settings: ForceDarkSettings,
    pub should_paint_overlay: bool,
    pub is_recording_async_scrolling_metadata: bool,
    pub document_id: UniqueNodeId,
    pub has_blocking_wheel_event_region_covering_viewport: bool,
    pub wheel_event_listener_state_generation: u64,
    pub chrome_metrics: FfiChromeMetrics,
    pub paint_viewport_scrollbars: bool,
    pub async_scrolling_enabled: bool,
    pub middle_button_scroll_origin: Option<CssPixelPoint>,
    pub canvas_fill_rect: Option<IntRect>,
    pub canvas_color: Color,
    pub opaque_canvas: bool,
    pub bitmap_rect: IntRect,
    pub background_color: Color,
    pub paint_command_cache_read_write: bool,
    pub window_is_focused: bool,
    pub outline_auto_color: Color,
    pub selection_background_from_palette: Color,
    pub selection_background_light: Color,
    pub selection_background_dark: Color,
    pub palette_is_dark: bool,
    pub document_has_supported_color_schemes: bool,
    pub inspector_highlight: Option<InspectorHighlight<'a>>,
    pub tooltip_color: Color,
    pub tooltip_text_color: Color,
    pub tooltip_border_color: Color,
    pub grid_overlays: Option<GridOverlays<'a>>,
    // These array elements are already plain values without pointers or optional-value tags.
    pub flex_overlays: &'a [FfiFlexOverlayInput],
    pub caret_debug_rect: Option<CssPixelRect>,
    pub caret: Option<CaretPaint>,
    pub focused_text_control: Option<FocusedTextControlSelection>,
    pub focused_area_outline: Option<FocusedAreaOutline<'a>>,
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

#[derive(Clone, Copy)]
pub(crate) struct FocusedAreaOutline<'a> {
    pub image: NodeSlotId,
    pub path_bytes: &'a [u8],
    pub color: Color,
    pub width: CssPixels,
}

#[derive(Clone)]
pub(crate) struct OverlayLabelFonts {
    pub css_font: FontHandle,
    pub device_font: FontHandle,
}

#[derive(Clone)]
pub(crate) struct InspectorHighlight<'a> {
    pub paintable: NodeSlotId,
    pub label: Cow<'a, str>,
    pub fonts: OverlayLabelFonts,
}

#[derive(Clone)]
pub(crate) struct GridOverlays<'a> {
    pub inputs: &'a [FfiGridOverlayInput],
    pub fonts: OverlayLabelFonts,
}
