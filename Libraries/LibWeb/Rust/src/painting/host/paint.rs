/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::layout::{OptionalCssPixelRect, OptionalCssPixels, OptionalFloatSize, OptionalIntRect};
use crate::painting::display_list::commands::{OptionalAffineTransform, OptionalColor};
use libgfx_rust::{
    AffineTransform, Color, FloatMatrix4x4, FloatRect, FloatSize, IntPoint, IntRect, InterpolationColorSpace,
};
use std::ffi::c_void;

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiRecordingInputs {
    pub device_pixels_per_css_pixel: f64,
    pub device_viewport_rect: IntRect,
    pub css_viewport_rect: crate::layout::FfiCssPixelRect,
    pub should_show_line_box_borders: bool,
    pub should_paint_overlay: bool,
    pub is_recording_async_scrolling_metadata: bool,
    pub document_id: i64,
    pub has_blocking_wheel_event_region_covering_viewport: bool,
    pub viewport_wheel_overflow_x: u8,
    pub viewport_wheel_overflow_y: u8,
    pub chrome_metrics: crate::painting::ffi::FfiChromeMetrics,
    pub paint_viewport_scrollbars: bool,
    pub async_scrolling_enabled: bool,
    pub middle_button_scroll_active: bool,
    pub middle_button_scroll_origin: crate::layout::FfiCssPixelPoint,
    pub root_background_source: FfiRootBackgroundSource,
    pub canvas_fill_rect: OptionalIntRect,
    pub canvas_color: Color,
    pub opaque_canvas: bool,
    pub bitmap_rect: IntRect,
    pub background_color: Color,
    pub paint_command_cache_read_write: bool,
    pub display_list_id: u64,
    pub window_is_focused: bool,
    pub outline_auto_color: Color,
    pub has_inspector_highlight: bool,
    pub inspector_highlight_paintable: crate::layout::node_data::NodeSlotId,
    pub tooltip_color: Color,
    pub tooltip_text_color: Color,
    pub tooltip_border_color: Color,
    pub grid_overlays: *const FfiGridOverlayInput,
    pub grid_overlay_count: usize,
    pub flex_overlays: *const FfiFlexOverlayInput,
    pub flex_overlay_count: usize,
    pub caret_debug_rect: OptionalCssPixelRect,
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

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiOverlayLabelFacts {
    pub font_id: u64,
    pub css_width: f32,
    pub css_pixel_size: f32,
    pub device_glyph_width: f32,
    pub device_ascent: f32,
    pub device_descent: f32,
    pub blob_bounds: FloatRect,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiImageIntrinsicFacts {
    pub is_paintable: bool,
    pub natural_width: OptionalCssPixels,
    pub natural_height: OptionalCssPixels,
    pub has_natural_aspect_ratio: bool,
    pub natural_aspect_ratio_numerator: crate::css::css_pixels::CssPixels,
    pub natural_aspect_ratio_denominator: crate::css::css_pixels::CssPixels,
    pub has_selected_image_value: bool,
    pub selected_image_value: *const c_void,
}

impl Default for FfiImageIntrinsicFacts {
    fn default() -> Self {
        Self {
            is_paintable: false,
            natural_width: Default::default(),
            natural_height: Default::default(),
            has_natural_aspect_ratio: false,
            natural_aspect_ratio_numerator: Default::default(),
            natural_aspect_ratio_denominator: Default::default(),
            has_selected_image_value: false,
            selected_image_value: std::ptr::null(),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiVideoRepresentation {
    #[default]
    VideoFrame,
    PosterFrame,
    TransparentBlack,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiReplacedPaintFacts {
    pub has_decoded_image_data: bool,
    pub natural_width: OptionalCssPixels,
    pub natural_height: OptionalCssPixels,
    pub has_natural_aspect_ratio: bool,
    pub natural_aspect_ratio_numerator: crate::css::css_pixels::CssPixels,
    pub natural_aspect_ratio_denominator: crate::css::css_pixels::CssPixels,
    pub selection_background_color: Color,
    pub has_canvas_content: bool,
    pub canvas_content_width: i32,
    pub canvas_content_height: i32,
    pub canvas_id: u64,
    pub canvas_content_generation: u64,
    pub video_representation: FfiVideoRepresentation,
    pub has_video_frame: bool,
    pub video_src_width: i32,
    pub video_src_height: i32,
    pub video_sink_storage_id: u64,
    pub has_poster_frame: bool,
    pub poster_frame_id: u64,
    pub poster_width: i32,
    pub poster_height: i32,
    pub has_composited_context: bool,
    pub composited_context_id: u64,
    pub enabled: bool,
    pub checked: bool,
    pub indeterminate: bool,
    pub being_activated: bool,
    pub canvas_color: Color,
    pub canvas_text_color: Color,
    pub accent_color: Color,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiSvgImageFacts {
    pub has_decoded_image_data: bool,
    pub natural_size: OptionalFloatSize,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiSvgPaintContext {
    pub viewport: FloatRect,
    pub path_bounding_box: FloatRect,
    pub paint_transform: AffineTransform,
    pub content_scale: FloatSize,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiSvgPaintStyleKind {
    #[default]
    None,
    LinearGradient,
    RadialGradient,
    Pattern,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiSvgGradientSpreadMethod {
    #[default]
    Pad,
    Repeat,
    Reflect,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiSvgPaintStyle {
    pub kind: FfiSvgPaintStyleKind,
    pub gradient_transform: OptionalAffineTransform,
    pub spread_method: FfiSvgGradientSpreadMethod,
    pub color_space: InterpolationColorSpace,
    pub start: libgfx_rust::FloatPoint,
    pub end: libgfx_rust::FloatPoint,
    pub start_radius: f32,
    pub end_radius: f32,
    pub pattern_paintable: crate::layout::node_data::NodeSlotId,
    pub tile_content_transform: FloatMatrix4x4,
    pub tile_rect: FloatRect,
    pub content_scale: FloatSize,
    pub pattern_transform: OptionalAffineTransform,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiScrollNodeKind {
    #[default]
    None,
    Viewport,
    Element,
    PseudoElement,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiAsyncScrollFacts {
    pub is_nested_navigable_container: bool,
    pub scroll_node_kind: FfiScrollNodeKind,
    pub scrollable_node_id: i64,
    pub pseudo_element_type: u8,
    pub snaps_scroll_position_horizontally: bool,
    pub snaps_scroll_position_vertically: bool,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiOutlineFacts {
    pub paints_focused_area_outline: bool,
    pub focused_area_path: *mut c_void,
    pub focused_area_color: Color,
    pub focused_area_width: crate::css::css_pixels::CssPixels,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiTextControlSelection {
    pub has_selection: bool,
    pub start: usize,
    pub end: usize,
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

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiCursorFacts {
    pub paints: bool,
    pub rect: crate::layout::FfiCssPixelRect,
    pub color: Color,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiLayerImageList {
    Background,
    Mask,
    BorderImageSource,
    DocumentBackground,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiLayerImagePrepareFacts {
    pub is_image_style_value: bool,
    pub single_pixel_color: OptionalColor,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiLayerImageNestedDisplayListFacts {
    pub has_nested_display_list: bool,
    pub nested_display_list_id: u64,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiLayerImageFrameFacts {
    pub has_frame: bool,
    pub frame_id: u64,
    pub frame_width: i32,
    pub frame_height: i32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiImagePaintKind {
    #[default]
    None,
    DecodedFrame,
    NestedDisplayList,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiImagePaintFacts {
    pub image_paint_kind: FfiImagePaintKind,
    pub frame_id: u64,
    pub natural_width: i32,
    pub natural_height: i32,
    pub nested_display_list_id: u64,
    pub list_width: i32,
    pub list_height: i32,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiStackingContextNodeExport {
    pub layout_node_shell: *mut c_void,
    pub child_count: usize,
    pub has_effective_z_index: bool,
    pub effective_z_index: i32,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPaintTreeDumpEntry {
    pub layout_node_shell: *mut c_void,
    pub depth: u32,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPaintHostCallbacks {
    pub context: *mut c_void,
    pub async_scroll_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiAsyncScrollFacts,
    pub outline_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiOutlineFacts,
    pub image_intrinsic_facts:
        unsafe extern "C" fn(*mut c_void, *mut c_void, FfiLayerImageList, u32) -> FfiImageIntrinsicFacts,
    pub text_control_selection: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiTextControlSelection,
    pub selection_style_facts: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> FfiSelectionStyleFacts,
    pub register_font: unsafe extern "C" fn(*mut c_void, *const c_void) -> u64,
    pub cursor_facts: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> FfiCursorFacts,
    pub layer_image_prepare:
        unsafe extern "C" fn(*mut c_void, *mut c_void, FfiLayerImageList, u32) -> FfiLayerImagePrepareFacts,
    pub layer_image_nested_display_list: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        FfiLayerImageList,
        u32,
        IntRect,
    ) -> FfiLayerImageNestedDisplayListFacts,
    pub layer_image_current_frame:
        unsafe extern "C" fn(*mut c_void, *mut c_void, FfiLayerImageList, u32, IntRect) -> FfiLayerImageFrameFacts,
    pub layer_image_paint: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        FfiLayerImageList,
        u32,
        FloatRect,
        crate::layout::FfiCssPixelSize,
        u8,
        FloatSize,
    ) -> FfiImagePaintFacts,
    pub replaced_paint_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiReplacedPaintFacts,
    pub replaced_image_paint:
        unsafe extern "C" fn(*mut c_void, *mut c_void, FloatRect, FloatSize) -> FfiImagePaintFacts,
    pub backdrop_filter_bytes: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> bool,
    pub nested_display_list_from_bytes: unsafe extern "C" fn(*mut c_void, *const u8, usize, IntPoint) -> u64,
    pub svg_image_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiSvgImageFacts,
    pub svg_paint_style: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        bool,
        *const FfiSvgPaintContext,
        *mut c_void,
    ) -> FfiSvgPaintStyle,
    pub accumulated_2d_scale: unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> FloatSize,
    pub materialize_visual_context_tree: unsafe extern "C" fn(*mut c_void, *const c_void) -> *mut c_void,
    pub nested_display_list_from_tree:
        unsafe extern "C" fn(*mut c_void, *const u8, usize, *mut c_void, *const u64, usize) -> u64,
    pub overlay_label: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        *const u16,
        usize,
        usize,
        f32,
        *mut c_void,
    ) -> FfiOverlayLabelFacts,
}

#[derive(Default)]
pub struct ColorStopSink {
    pub colors: Vec<Color>,
    pub positions: Vec<f32>,
}

impl FfiPaintHostCallbacks {
    pub(crate) fn outline_facts(&self, layout_node_shell: *mut c_void) -> FfiOutlineFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.outline_facts)(self.context, layout_node_shell) }
    }
    pub(crate) fn async_scroll_facts(&self, layout_node_shell: *mut c_void) -> FfiAsyncScrollFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.async_scroll_facts)(self.context, layout_node_shell) }
    }
    pub(crate) fn overlay_label(
        &self,
        layout_node_shell: *mut c_void,
        text: &[u16],
        utf16_fly_string_raw: usize,
        css_font_size: f32,
    ) -> (
        FfiOverlayLabelFacts,
        Vec<crate::painting::display_list::commands::DisplayListGlyph>,
    ) {
        let mut glyphs: Vec<crate::painting::display_list::commands::DisplayListGlyph> = Vec::new();
        // SAFETY: The C++ host pushes into the sink through the exported function, synchronously.
        let facts = unsafe {
            (self.overlay_label)(
                self.context,
                layout_node_shell,
                text.as_ptr(),
                text.len(),
                utf16_fly_string_raw,
                css_font_size,
                (&raw mut glyphs).cast(),
            )
        };
        (facts, glyphs)
    }
    pub(crate) fn text_control_selection(&self, layout_node_shell: *mut c_void) -> FfiTextControlSelection {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.text_control_selection)(self.context, layout_node_shell) }
    }
    pub(crate) fn selection_style_facts(
        &self,
        layout_node_shell: *mut c_void,
    ) -> (
        FfiSelectionStyleFacts,
        Vec<crate::painting::record::paint::text::ShadowLayer>,
    ) {
        let mut shadows: Vec<crate::painting::record::paint::text::ShadowLayer> = Vec::new();
        // SAFETY: The C++ host answers synchronously, pushing shadow layers into
        // the sink through the exported function.
        let facts = unsafe { (self.selection_style_facts)(self.context, layout_node_shell, (&raw mut shadows).cast()) };
        (facts, shadows)
    }
    pub(crate) fn register_font(&self, font: *const c_void) -> u64 {
        // SAFETY: The C++ host registers the live font in the recording's
        // resource table synchronously.
        unsafe { (self.register_font)(self.context, font) }
    }
    pub(crate) fn cursor_facts(
        &self,
        layout_node_shell: *mut c_void,
        owner_layout_node_shell: *mut c_void,
    ) -> FfiCursorFacts {
        // SAFETY: The C++ host answers synchronously from live layout node shells.
        unsafe { (self.cursor_facts)(self.context, layout_node_shell, owner_layout_node_shell) }
    }
    pub(crate) fn layer_image_prepare(
        &self,
        layout_node_shell: *mut c_void,
        list: FfiLayerImageList,
        computed_index: u32,
    ) -> FfiLayerImagePrepareFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.layer_image_prepare)(self.context, layout_node_shell, list, computed_index) }
    }
    pub(crate) fn layer_image_nested_display_list(
        &self,
        layout_node_shell: *mut c_void,
        list: FfiLayerImageList,
        computed_index: u32,
        device_dest_rect: libgfx_rust::IntRect,
    ) -> FfiLayerImageNestedDisplayListFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe {
            (self.layer_image_nested_display_list)(
                self.context,
                layout_node_shell,
                list,
                computed_index,
                device_dest_rect,
            )
        }
    }
    pub(crate) fn layer_image_current_frame(
        &self,
        layout_node_shell: *mut c_void,
        list: FfiLayerImageList,
        computed_index: u32,
        device_dest_rect: libgfx_rust::IntRect,
    ) -> FfiLayerImageFrameFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe {
            (self.layer_image_current_frame)(self.context, layout_node_shell, list, computed_index, device_dest_rect)
        }
    }
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn layer_image_paint(
        &self,
        layout_node_shell: *mut c_void,
        list: FfiLayerImageList,
        computed_index: u32,
        dest: FloatRect,
        css_tile_size: crate::layout::FfiCssPixelSize,
        image_rendering: u8,
        accumulated_scale: libgfx_rust::FloatSize,
    ) -> FfiImagePaintFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe {
            (self.layer_image_paint)(
                self.context,
                layout_node_shell,
                list,
                computed_index,
                dest,
                css_tile_size,
                image_rendering,
                accumulated_scale,
            )
        }
    }
    pub(crate) fn image_intrinsic_facts(
        &self,
        layout_node_shell: *mut c_void,
        list: FfiLayerImageList,
        computed_index: u32,
    ) -> FfiImageIntrinsicFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.image_intrinsic_facts)(self.context, layout_node_shell, list, computed_index) }
    }
    pub(crate) fn replaced_paint_facts(&self, layout_node_shell: *mut c_void) -> FfiReplacedPaintFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.replaced_paint_facts)(self.context, layout_node_shell) }
    }
    pub(crate) fn replaced_image_paint(
        &self,
        layout_node_shell: *mut c_void,
        dest: FloatRect,
        accumulated_scale: libgfx_rust::FloatSize,
    ) -> FfiImagePaintFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.replaced_image_paint)(self.context, layout_node_shell, dest, accumulated_scale) }
    }
    pub(crate) fn backdrop_filter_bytes(&self, layout_node_shell: *mut c_void) -> Option<Vec<u8>> {
        let mut bytes: Vec<u8> = Vec::new();
        // SAFETY: The C++ host pushes into the Vec through the exported sink function, synchronously.
        let has_filter =
            unsafe { (self.backdrop_filter_bytes)(self.context, layout_node_shell, (&raw mut bytes).cast()) };
        has_filter.then_some(bytes)
    }
    pub(crate) fn nested_display_list_from_bytes(
        &self,
        bytes: &[u8],
        content_offset: libgfx_rust::IntPoint,
    ) -> crate::painting::display_list::commands::DisplayListResourceId {
        // SAFETY: The C++ host copies the bytes synchronously.
        let id =
            unsafe { (self.nested_display_list_from_bytes)(self.context, bytes.as_ptr(), bytes.len(), content_offset) };
        crate::painting::display_list::commands::DisplayListResourceId(id)
    }
    pub(crate) fn svg_image_facts(&self, layout_node_shell: *mut c_void) -> FfiSvgImageFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.svg_image_facts)(self.context, layout_node_shell) }
    }
    pub(crate) fn svg_paint_style(
        &self,
        layout_node_shell: *mut c_void,
        is_stroke: bool,
        paint_context: &FfiSvgPaintContext,
    ) -> (FfiSvgPaintStyle, ColorStopSink) {
        let mut sink = ColorStopSink::default();
        // SAFETY: The C++ host answers synchronously, pushing color stops into the sink through the exported function.
        let style = unsafe {
            (self.svg_paint_style)(
                self.context,
                layout_node_shell,
                is_stroke,
                paint_context,
                (&raw mut sink).cast(),
            )
        };
        (style, sink)
    }
    pub(crate) fn accumulated_2d_scale(
        &self,
        visual_context_tree: Option<&crate::painting::visual_context::VisualContextTree>,
        visual_context_index: usize,
    ) -> libgfx_rust::FloatSize {
        let tree = visual_context_tree.map_or(std::ptr::null(), |tree| std::ptr::from_ref(tree).cast());
        // SAFETY: The C++ host answers synchronously and only borrows the optional tree.
        unsafe { (self.accumulated_2d_scale)(self.context, tree, visual_context_index) }
    }
    pub(crate) fn materialize_visual_context_tree(
        &self,
        tree: &crate::painting::visual_context::VisualContextTree,
    ) -> *mut c_void {
        // SAFETY: The C++ host reads the tree synchronously through the exported node accessors.
        unsafe { (self.materialize_visual_context_tree)(self.context, std::ptr::from_ref(tree).cast()) }
    }
    pub(crate) fn nested_display_list_from_tree(
        &self,
        bytes: &[u8],
        tree_handle: *mut c_void,
        mask_registrations: &[(usize, crate::painting::display_list::commands::DisplayListResourceId)],
    ) -> crate::painting::display_list::commands::DisplayListResourceId {
        let pairs: Vec<u64> = mask_registrations
            .iter()
            .flat_map(|(index, id)| [*index as u64, id.0])
            .collect();
        // SAFETY: The C++ host copies the bytes and consumes the tree synchronously.
        let id = unsafe {
            (self.nested_display_list_from_tree)(
                self.context,
                bytes.as_ptr(),
                bytes.len(),
                tree_handle,
                pairs.as_ptr(),
                mask_registrations.len(),
            )
        };
        crate::painting::display_list::commands::DisplayListResourceId(id)
    }
}
