/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::svg_formatting_context::FfiSvgNumberPercentage;
use crate::layout::used_values;
use crate::painting::display_list::builder::RecordedDisplayList;
use crate::painting::display_list::commands::DisplayListCommandRun;
use crate::painting::display_list::commands::{OptionalAffineTransform, OptionalColor};
use libgfx_rust::{Color, IntRect, InterpolationColorSpace};
use std::ffi::c_void;

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiRecordingInputs {
    pub device_viewport_rect: IntRect,
    pub css_viewport_rect: used_values::FfiCssPixelRect,
    pub should_show_line_box_borders: bool,
    pub force_dark_enabled: bool,
    pub force_dark_foreground_threshold: i32,
    pub force_dark_background_threshold: i32,
    pub should_paint_overlay: bool,
    pub is_recording_async_scrolling_metadata: bool,
    pub document_id: i64,
    pub has_blocking_wheel_event_region_covering_viewport: bool,
    pub wheel_event_listener_state_generation: u64,
    pub chrome_metrics: crate::painting::ffi::FfiChromeMetrics,
    pub paint_viewport_scrollbars: bool,
    pub async_scrolling_enabled: bool,
    pub middle_button_scroll_active: bool,
    pub middle_button_scroll_origin: used_values::FfiCssPixelPoint,
    pub canvas_fill_rect: used_values::OptionalIntRect,
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
    pub has_inspector_highlight: bool,
    pub inspector_highlight_paintable: crate::layout::node_data::NodeSlotId,
    pub tooltip_color: Color,
    pub tooltip_text_color: Color,
    pub tooltip_border_color: Color,
    pub grid_overlays: *const FfiGridOverlayInput,
    pub grid_overlay_count: usize,
    pub flex_overlays: *const FfiFlexOverlayInput,
    pub flex_overlay_count: usize,
    pub caret_debug_rect: used_values::OptionalCssPixelRect,
    // Per-document facts the host resolves once per recording.
    pub caret: FfiCaretPaint,
    pub focused_text_control: FfiFocusedTextControlSelection,
    pub focused_area_outline: FfiFocusedAreaOutline,
    pub inspector_highlight_label: FfiInspectorHighlightLabel,
    pub grid_label_fonts: FfiOverlayLabelFonts,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiCaretPaintKind {
    None,
    /// `block` paints the caret, in the fragment run owned by the self-painting inline `owner`
    /// (`INVALID` when the block itself owns it).
    InBlock,
    /// The empty editable inline `block` paints the caret at its own position.
    EmptyInline,
}

/// Where the document's caret paints, resolved once per recording by the host.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiCaretPaint {
    pub kind: FfiCaretPaintKind,
    pub block: crate::layout::node_data::NodeSlotId,
    pub owner: crate::layout::node_data::NodeSlotId,
    pub rect: used_values::FfiCssPixelRect,
    pub color: Color,
    pub blink_cycle_start_time_ns: i64,
    pub should_blink: bool,
}

/// The focused text control's selection, keyed by its primary layout text node. Equal start
/// and end offsets mean no selection.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiFocusedTextControlSelection {
    pub text_node: crate::layout::node_data::NodeSlotId,
    pub start: usize,
    pub end: usize,
}

/// The focus ring of a focused image-map area, painted by the image whose rendering makes the
/// area's shape focusable. No path bytes means no focus ring.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiFocusedAreaOutline {
    pub image: crate::layout::node_data::NodeSlotId,
    /// A serialised `Gfx::Path` in the image's own coordinate space, live for the recording call.
    pub path_bytes: *const u8,
    pub path_byte_count: usize,
    pub color: Color,
    pub width: crate::css::css_pixels::CssPixels,
}

/// The platform default font at an overlay label's CSS size and at that size in device pixels.
/// Both are null unless the recording paints the overlay they belong to.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiOverlayLabelFonts {
    pub css_font: *const c_void,
    pub device_font: *const c_void,
}

/// The inspector's box-model label for the highlighted node: UTF-8 text live for the recording call.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiInspectorHighlightLabel {
    pub fonts: FfiOverlayLabelFonts,
    pub text: *const u8,
    pub text_byte_count: usize,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiGridOverlayInput {
    pub paintable: crate::layout::node_data::NodeSlotId,
    pub color: Color,
    pub label_foreground_color: Color,
    pub label_css_pixel_size: f32,
    pub show_area_names: bool,
    pub show_line_numbers: bool,
    pub show_track_sizes: bool,
    pub show_infinite_lines: bool,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiFlexOverlayInput {
    pub paintable: crate::layout::node_data::NodeSlotId,
    pub color: Color,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiImageContentKind {
    #[default]
    None,
    Raster,
    Vector,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiLayerImagePaintFacts {
    pub is_paintable: bool,
    pub natural_width: used_values::OptionalCssPixels,
    pub natural_height: used_values::OptionalCssPixels,
    pub has_natural_aspect_ratio: bool,
    pub natural_aspect_ratio_numerator: crate::css::css_pixels::CssPixels,
    pub natural_aspect_ratio_denominator: crate::css::css_pixels::CssPixels,
    pub has_image_set_selected_option: bool,
    pub image_set_selected_option_index: u32,
    pub content_kind: FfiImageContentKind,
    pub vector_content_identity: u64,
    pub vector_has_active_view_box: bool,
    pub frame: *const c_void,
    pub single_pixel_color: OptionalColor,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiLayerImagePaintFactsEntry {
    pub list: FfiLayerImageList,
    pub computed_index: u32,
    pub facts: FfiLayerImagePaintFacts,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiVideoRepresentation {
    #[default]
    VideoFrame,
    PosterFrame,
    TransparentBlack,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiReplacedImagePaintFacts {
    pub has_decoded_image_data: bool,
    pub natural_width: used_values::OptionalCssPixels,
    pub natural_height: used_values::OptionalCssPixels,
    pub has_natural_aspect_ratio: bool,
    pub natural_aspect_ratio_numerator: crate::css::css_pixels::CssPixels,
    pub natural_aspect_ratio_denominator: crate::css::css_pixels::CssPixels,
    pub content_kind: FfiImageContentKind,
    pub vector_content_identity: u64,
    pub vector_has_active_view_box: bool,
    pub frame: *const c_void,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVideoPaintFacts {
    pub representation: FfiVideoRepresentation,
    pub has_video_frame: bool,
    pub video_src_width: i32,
    pub video_src_height: i32,
    pub video_sink_resource_id: u64,
    pub video_sink_handle: u64,
    pub poster_frame: *const c_void,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiSvgGradientSpreadMethod {
    #[default]
    Pad,
    Repeat,
    Reflect,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiSvgGradientKind {
    #[default]
    Linear,
    Radial,
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FfiSvgGradientDescription {
    pub kind: FfiSvgGradientKind,
    pub units_are_object_bounding_box: bool,
    pub spread_method: FfiSvgGradientSpreadMethod,
    pub color_space: InterpolationColorSpace,
    pub gradient_transform: OptionalAffineTransform,
    pub x1: FfiSvgNumberPercentage,
    pub y1: FfiSvgNumberPercentage,
    pub x2: FfiSvgNumberPercentage,
    pub y2: FfiSvgNumberPercentage,
    pub cx: FfiSvgNumberPercentage,
    pub cy: FfiSvgNumberPercentage,
    pub r: FfiSvgNumberPercentage,
    pub fx: FfiSvgNumberPercentage,
    pub fy: FfiSvgNumberPercentage,
    pub fr: FfiSvgNumberPercentage,
}

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FfiSvgPatternDescription {
    pub pattern_box: crate::layout::node_data::NodeSlotId,
    pub units_are_object_bounding_box: bool,
    pub content_units_are_object_bounding_box: bool,
    pub has_view_box: bool,
    pub x: FfiSvgNumberPercentage,
    pub y: FfiSvgNumberPercentage,
    pub width: FfiSvgNumberPercentage,
    pub height: FfiSvgNumberPercentage,
    pub pattern_transform_attribute: OptionalAffineTransform,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiSelectionShadowLayer {
    pub color: Color,
    pub offset_x: CssPixels,
    pub offset_y: CssPixels,
    pub blur_radius: CssPixels,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiSelectionStyleFacts {
    pub background_color: Color,
    pub text_color: OptionalColor,
    pub has_text_shadow: bool,
    pub has_text_decoration: bool,
    pub text_decoration_lines: [u8; 8],
    pub text_decoration_line_count: u32,
    pub text_decoration_style: u8,
    pub text_decoration_color: Color,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum FfiLayerImageList {
    Background,
    Mask,
    BorderImageSource,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVectorImageRenderRequest {
    pub owner: crate::layout::node_data::NodeSlotId,
    pub is_replaced_content: bool,
    pub list: FfiLayerImageList,
    pub computed_index: u32,
    pub css_width: crate::css::css_pixels::CssPixels,
    pub css_height: crate::css::css_pixels::CssPixels,
    pub raster_scale: f32,
}

// A recording lent to C++ for the duration of one call. An empty Vec's pointer is dangling, so
// the C++ side never dereferences a pointer whose count is zero.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiRecordedDisplayList {
    pub bytes: *const u8,
    pub byte_count: usize,
    pub command_runs: *const DisplayListCommandRun,
    pub command_run_count: usize,
}

impl FfiRecordedDisplayList {
    pub const fn empty() -> Self {
        Self {
            bytes: std::ptr::null(),
            byte_count: 0,
            command_runs: std::ptr::null(),
            command_run_count: 0,
        }
    }
}

impl From<&RecordedDisplayList> for FfiRecordedDisplayList {
    fn from(recorded: &RecordedDisplayList) -> Self {
        Self {
            bytes: recorded.bytes.as_ptr(),
            byte_count: recorded.bytes.len(),
            command_runs: recorded.command_runs.as_ptr(),
            command_run_count: recorded.command_runs.len(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiFormControlPaintFacts {
    pub enabled: bool,
    pub checked: bool,
    pub indeterminate: bool,
    pub being_activated: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCanvasPaintFacts {
    pub has_content: bool,
    pub content_width: i32,
    pub content_height: i32,
    pub canvas_id: u64,
    pub content_generation: u64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiNavigableContainerPaintFacts {
    pub has_composited_context: bool,
    pub composited_context_id: u64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiSnapAxes {
    pub x: bool,
    pub y: bool,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiRecordingPublishCallbacks {
    pub context: *mut c_void,
    pub add_font: unsafe extern "C" fn(*mut c_void, *const c_void),
    pub add_image_frame: unsafe extern "C" fn(*mut c_void, *const c_void),
    pub resolve_vector_image_display_list: unsafe extern "C" fn(*mut c_void, *const FfiVectorImageRenderRequest) -> u64,
    pub add_video_sink: unsafe extern "C" fn(*mut c_void, u64, u64),
}

impl FfiRecordingPublishCallbacks {
    pub(crate) fn add_font(&self, font: &libgfx_rust::font::FontHandle) {
        // SAFETY: The C++ host registers the live font synchronously.
        unsafe { (self.add_font)(self.context, font.as_raw()) };
    }

    pub(crate) fn add_image_frame(&self, frame: &libgfx_rust::image_frame::ImageFrameHandle) {
        // SAFETY: The C++ host copies the live frame synchronously.
        unsafe { (self.add_image_frame)(self.context, frame.as_raw()) };
    }

    pub(crate) fn resolve_vector_image_display_list(&self, request: &FfiVectorImageRenderRequest) -> u64 {
        // SAFETY: The C++ host records the image's display list synchronously and reads the
        // request only for the duration of the call.
        unsafe { (self.resolve_vector_image_display_list)(self.context, request) }
    }

    pub(crate) fn add_video_sink(&self, resource_id: u64, sink_handle: u64) {
        // SAFETY: The C++ host registers the sink synchronously.
        unsafe { (self.add_video_sink)(self.context, resource_id, sink_handle) };
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPaintHostCallbacks {
    pub context: *mut c_void,
}

impl FfiPaintHostCallbacks {}
