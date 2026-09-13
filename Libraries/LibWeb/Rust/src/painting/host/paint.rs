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

impl FfiRecordingInputs {
    /// # Safety
    ///
    /// Nonempty arrays and byte buffers must be aligned, valid and immutable for the
    /// returned inputs' lifetime. Fonts for enabled overlays must point to live `Gfx::Font`s.
    pub(crate) unsafe fn borrow_recording_inputs(
        &self,
        tree_inputs: super::FfiVisualContextTreeInputs,
        root_background_source: super::FfiRootBackgroundSource,
    ) -> crate::painting::record::inputs::RecordingInputs<'_> {
        use crate::painting::display_list::commands::UniqueNodeId;
        use crate::painting::ffi::ffi_slice;
        use crate::painting::force_dark::ForceDarkSettings;
        use crate::painting::record::inputs::{
            CaretPaint, CaretTarget, FocusedAreaOutline, FocusedTextControlSelection, GridOverlays, InspectorHighlight,
            RecordingInputs,
        };

        // SAFETY: The caller lends these arrays and buffers for the returned inputs' lifetime.
        let (grid_overlays, flex_overlays, outline_path) = unsafe {
            (
                ffi_slice(self.grid_overlays, self.grid_overlay_count),
                ffi_slice(self.flex_overlays, self.flex_overlay_count),
                ffi_slice(
                    self.focused_area_outline.path_bytes,
                    self.focused_area_outline.path_byte_count,
                ),
            )
        };
        let caret = self.caret;
        let caret_target = match caret.kind {
            FfiCaretPaintKind::None => None,
            FfiCaretPaintKind::InBlock => Some(CaretTarget::InBlock {
                block: caret.block,
                owner: (!caret.owner.is_invalid()).then_some(caret.owner),
            }),
            FfiCaretPaintKind::EmptyInline => Some(CaretTarget::EmptyInline(caret.block)),
        };
        let control = self.focused_text_control;
        RecordingInputs {
            device_pixels_per_css_pixel: tree_inputs.device_pixels_per_css_pixel,
            viewport_wheel_overflow_x: tree_inputs.viewport_wheel_overflow_x,
            viewport_wheel_overflow_y: tree_inputs.viewport_wheel_overflow_y,
            root_background_source,
            device_viewport_rect: self.device_viewport_rect,
            css_viewport_rect: self.css_viewport_rect.into(),
            should_show_line_box_borders: self.should_show_line_box_borders,
            force_dark_enabled: self.force_dark_enabled,
            force_dark_settings: ForceDarkSettings {
                foreground_brightness_threshold: self.force_dark_foreground_threshold,
                background_brightness_threshold: self.force_dark_background_threshold,
            },
            should_paint_overlay: self.should_paint_overlay,
            is_recording_async_scrolling_metadata: self.is_recording_async_scrolling_metadata,
            document_id: UniqueNodeId(self.document_id),
            has_blocking_wheel_event_region_covering_viewport: self.has_blocking_wheel_event_region_covering_viewport,
            wheel_event_listener_state_generation: self.wheel_event_listener_state_generation,
            chrome_metrics: self.chrome_metrics,
            paint_viewport_scrollbars: self.paint_viewport_scrollbars,
            async_scrolling_enabled: self.async_scrolling_enabled,
            middle_button_scroll_origin: self
                .middle_button_scroll_active
                .then(|| self.middle_button_scroll_origin.into()),
            canvas_fill_rect: self.canvas_fill_rect.has_value.then_some(self.canvas_fill_rect.value),
            canvas_color: self.canvas_color,
            opaque_canvas: self.opaque_canvas,
            bitmap_rect: self.bitmap_rect,
            background_color: self.background_color,
            paint_command_cache_read_write: self.paint_command_cache_read_write,
            window_is_focused: self.window_is_focused,
            outline_auto_color: self.outline_auto_color,
            selection_background_from_palette: self.selection_background_from_palette,
            selection_background_light: self.selection_background_light,
            selection_background_dark: self.selection_background_dark,
            palette_is_dark: self.palette_is_dark,
            document_has_supported_color_schemes: self.document_has_supported_color_schemes,
            inspector_highlight: self.has_inspector_highlight.then(|| {
                // SAFETY: The caller lends the label bytes and supplies live fonts for this overlay.
                let (text, fonts) = unsafe {
                    (
                        ffi_slice(
                            self.inspector_highlight_label.text,
                            self.inspector_highlight_label.text_byte_count,
                        ),
                        self.inspector_highlight_label.fonts.retain(),
                    )
                };
                InspectorHighlight {
                    paintable: self.inspector_highlight_paintable,
                    label: String::from_utf8_lossy(text),
                    fonts,
                }
            }),
            tooltip_color: self.tooltip_color,
            tooltip_text_color: self.tooltip_text_color,
            tooltip_border_color: self.tooltip_border_color,
            grid_overlays: (!grid_overlays.is_empty()).then(|| GridOverlays {
                inputs: grid_overlays,
                // SAFETY: The caller supplies live label fonts when grid overlays are enabled.
                fonts: unsafe { self.grid_label_fonts.retain() },
            }),
            flex_overlays,
            caret_debug_rect: self
                .caret_debug_rect
                .has_value
                .then(|| self.caret_debug_rect.value.into()),
            caret: caret_target.map(|target| CaretPaint {
                target,
                rect: caret.rect.into(),
                color: caret.color,
                blink_cycle_start_time_ns: caret.blink_cycle_start_time_ns,
                should_blink: caret.should_blink,
            }),
            focused_text_control: (control.start != control.end).then_some(FocusedTextControlSelection {
                text_node: control.text_node,
                start: control.start,
                end: control.end,
            }),
            focused_area_outline: (!outline_path.is_empty()).then_some(FocusedAreaOutline {
                image: self.focused_area_outline.image,
                path_bytes: outline_path,
                color: self.focused_area_outline.color,
                width: self.focused_area_outline.width,
            }),
        }
    }
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

impl FfiOverlayLabelFonts {
    /// # Safety
    ///
    /// Both pointers must refer to live `Gfx::Font`s.
    unsafe fn retain(self) -> crate::painting::record::inputs::OverlayLabelFonts {
        // SAFETY: The caller supplies live fonts; the handles retain them for recording.
        unsafe {
            crate::painting::record::inputs::OverlayLabelFonts {
                css_font: libgfx_rust::font::FontHandle::intern(self.css_font),
                device_font: libgfx_rust::font::FontHandle::intern(self.device_font),
            }
        }
    }
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
pub struct FfiNaturalSize {
    pub width: used_values::OptionalCssPixels,
    pub height: used_values::OptionalCssPixels,
    pub has_aspect_ratio: bool,
    pub aspect_ratio_numerator: crate::css::css_pixels::CssPixels,
    pub aspect_ratio_denominator: crate::css::css_pixels::CssPixels,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiImageContent {
    pub kind: FfiImageContentKind,
    pub vector_content_identity: u64,
    pub vector_has_active_view_box: bool,
    pub frame: *const c_void,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiLayerImagePaintFacts {
    pub is_paintable: bool,
    pub natural: FfiNaturalSize,
    pub has_image_set_selected_option: bool,
    pub image_set_selected_option_index: u32,
    pub content: FfiImageContent,
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
    pub natural: FfiNaturalSize,
    pub content: FfiImageContent,
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
    pub colors_authored: bool,
    pub background_color: Color,
    pub text_color: OptionalColor,
    pub wash_color: Color,
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

/// The geometry snap position selection runs over for a snap container, in CSS pixels so that the
/// selection is exact.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiSnapContainerGeometry {
    pub snapport: crate::layout::used_values::FfiCssPixelRect,
    pub min_scroll_offset: crate::layout::used_values::FfiCssPixelPoint,
    pub max_scroll_offset: crate::layout::used_values::FfiCssPixelPoint,
    pub strictness: u8,
    pub axes: FfiSnapAxes,
    pub horizontal_writing_mode: bool,
}

/// A snap area's geometry in its snap container's coordinate space: the transformed border box with
/// the scroll margin added, and the alignment along each physical axis.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiSnapAreaGeometry {
    /// The unique ID of the element the area's box belongs to.
    pub node_id: i64,
    /// 0 for the element's own box; otherwise the pseudo-element the box is generated for, plus one.
    pub pseudo_element_type: u8,
    pub rect: crate::layout::used_values::FfiCssPixelRect,
    pub align_x: u8,
    pub align_y: u8,
    pub always_stop: bool,
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
