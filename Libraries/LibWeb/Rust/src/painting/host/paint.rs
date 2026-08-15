/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use std::ffi::c_void;

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiRecordingInputs {
    pub device_pixels_per_css_pixel: f64,
    pub device_viewport_rect: [i32; 4],
    pub css_viewport_rect: crate::layout::FfiCssPixelRect,
    pub should_show_line_box_borders: bool,
    pub should_paint_overlay: bool,
    pub is_recording_async_scrolling_metadata: bool,
    pub document_id: i64,
    pub has_blocking_wheel_event_region_covering_viewport: bool,
    pub has_canvas_fill_rect: bool,
    pub canvas_fill_rect: [i32; 4],
    pub canvas_color: u32,
    pub opaque_canvas: bool,
    pub bitmap_rect: [i32; 4],
    pub background_color: u32,
    pub paint_command_cache_read_write: bool,
    pub display_list_id: u64,
    pub window_is_focused: bool,
    pub outline_auto_color: u32,
    pub has_inspector_highlight: bool,
    pub inspector_highlight_paintable: crate::painting::paintable_data::PaintableSlotId,
    pub tooltip_color: u32,
    pub tooltip_text_color: u32,
    pub tooltip_border_color: u32,
    pub grid_overlays: *const FfiGridOverlayInput,
    pub grid_overlay_count: usize,
    pub flex_overlays: *const FfiFlexOverlayInput,
    pub flex_overlay_count: usize,
    pub has_caret_debug_rect: bool,
    pub caret_debug_rect: crate::layout::FfiCssPixelRect,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiGridOverlayInput {
    pub paintable: crate::painting::paintable_data::PaintableSlotId,
    pub color: u32,
    pub label_foreground_color: u32,
    pub label_css_pixel_size: f32,
    pub show_area_names: bool,
    pub show_line_numbers: bool,
    pub show_track_sizes: bool,
    pub show_infinite_lines: bool,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiFlexOverlayInput {
    pub paintable: crate::painting::paintable_data::PaintableSlotId,
    pub color: u32,
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
    pub blob_bounds: [f32; 4],
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiImageIntrinsicFacts {
    pub is_paintable: bool,
    pub has_natural_width: bool,
    pub natural_width: i32,
    pub has_natural_height: bool,
    pub natural_height: i32,
    pub has_natural_aspect_ratio: bool,
    pub natural_aspect_ratio_numerator: i32,
    pub natural_aspect_ratio_denominator: i32,
    pub has_selected_image_value: bool,
    pub selected_image_value: *const c_void,
}

impl Default for FfiImageIntrinsicFacts {
    fn default() -> Self {
        Self {
            is_paintable: false,
            has_natural_width: false,
            natural_width: 0,
            has_natural_height: false,
            natural_height: 0,
            has_natural_aspect_ratio: false,
            natural_aspect_ratio_numerator: 0,
            natural_aspect_ratio_denominator: 0,
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
    pub has_natural_width: bool,
    pub natural_width: crate::css::css_pixels::CssPixels,
    pub has_natural_height: bool,
    pub natural_height: crate::css::css_pixels::CssPixels,
    pub has_natural_aspect_ratio: bool,
    pub natural_aspect_ratio_numerator: crate::css::css_pixels::CssPixels,
    pub natural_aspect_ratio_denominator: crate::css::css_pixels::CssPixels,
    pub selection_background_color: u32,
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
    pub canvas_color: u32,
    pub canvas_text_color: u32,
    pub accent_color: u32,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiSvgHostFacts {
    pub has_viewport: bool,
    pub viewport: [f32; 4],
    pub percentage_basis: i32,
    pub has_decoded_image_data: bool,
    pub has_natural_size: bool,
    pub natural_width: f32,
    pub natural_height: f32,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiSvgPaintContext {
    pub viewport: [f32; 4],
    pub path_bounding_box: [f32; 4],
    pub paint_transform: [f32; 6],
    pub content_scale: [f32; 2],
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
    pub has_gradient_transform: bool,
    pub gradient_transform: [f32; 6],
    pub spread_method: FfiSvgGradientSpreadMethod,
    pub color_space: u8,
    pub start: [f32; 2],
    pub end: [f32; 2],
    pub start_radius: f32,
    pub end_radius: f32,
    pub pattern_paintable: crate::painting::paintable_data::PaintableSlotId,
    pub tile_content_transform: [f32; 16],
    pub tile_rect: [f32; 4],
    pub content_scale: [f32; 2],
    pub has_pattern_transform: bool,
    pub pattern_transform: [f32; 6],
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
pub struct FfiViewportScrollbarFacts {
    pub present: bool,
    pub gutter_rect: [i32; 4],
    pub thumb_rect: [i32; 4],
    pub expanded_gutter_rect: [i32; 4],
    pub expanded_thumb_rect: [i32; 4],
    pub scroll_size: f64,
    pub expanded_scroll_size: f64,
    pub thumb_color: u32,
    pub track_color: u32,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiAsyncScrollFacts {
    pub is_nested_navigable_container: bool,
    pub scroll_node_kind: FfiScrollNodeKind,
    pub scrollable_node_id: i64,
    pub pseudo_element_type: u8,
    pub inside_blocking_wheel_event_handler: bool,
    pub records_viewport_scrollbars: bool,
    pub viewport_scrollbars: [FfiViewportScrollbarFacts; 2],
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiScrollbarPaintFacts {
    pub present: bool,
    pub gutter_rect: crate::layout::FfiCssPixelRect,
    pub thumb_rect: crate::layout::FfiCssPixelRect,
    pub track_rect: crate::layout::FfiCssPixelRect,
    pub thumb_travel_to_scroll_ratio: f64,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiOverlayFacts {
    pub paints_scrollbars: bool,
    pub scrollbars: [FfiScrollbarPaintFacts; 2],
    pub thumb_color: u32,
    pub track_color: u32,
    pub has_resizer_rect: bool,
    pub resizer_rect: crate::layout::FfiCssPixelRect,
    pub resize_gripper_padding: i32,
    pub middle_button_scroll_active: bool,
    pub middle_button_scroll_origin: crate::layout::FfiCssPixelPoint,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiOutlineFacts {
    pub paints_focused_area_outline: bool,
    pub focused_area_path: *mut c_void,
    pub focused_area_color: u32,
    pub focused_area_width: crate::css::css_pixels::CssPixels,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiTextSpan {
    pub fragment_index: u32,
    pub start_code_unit: usize,
    pub end_code_unit: usize,
    pub text_color: u32,
    pub background_color: u32,
    pub shadow_layer_count: u32,
    pub has_selection_offsets: bool,
    pub selection_start: usize,
    pub selection_end: usize,
    pub has_selection_text_decoration: bool,
    pub selection_text_decoration_lines: [u8; 8],
    pub selection_text_decoration_line_count: u32,
    pub selection_text_decoration_style: u8,
    pub selection_text_decoration_color: u32,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiTextShadowLayer {
    pub color: u32,
    pub offset_x: crate::css::css_pixels::CssPixels,
    pub offset_y: crate::css::css_pixels::CssPixels,
    pub blur_radius: crate::css::css_pixels::CssPixels,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiGlyphRunFacts {
    pub font_id: u64,
    pub blob_bounds: [f32; 4],
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiCursorFacts {
    pub paints: bool,
    pub rect: crate::layout::FfiCssPixelRect,
    pub color: u32,
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
    pub has_single_pixel_color: bool,
    pub single_pixel_color: u32,
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
    pub paintable_shell: *mut c_void,
    pub child_count: usize,
    pub has_effective_z_index: bool,
    pub effective_z_index: i32,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPaintHostCallbacks {
    pub context: *mut c_void,
    pub async_scroll_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiAsyncScrollFacts,
    pub overlay_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiOverlayFacts,
    pub outline_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiOutlineFacts,
    pub image_intrinsic_facts:
        unsafe extern "C" fn(*mut c_void, *mut c_void, FfiLayerImageList, u32) -> FfiImageIntrinsicFacts,
    pub root_background_source: unsafe extern "C" fn(*mut c_void) -> FfiRootBackgroundSource,
    pub text_spans: unsafe extern "C" fn(*mut c_void, *mut c_void, *const u32, usize, *mut c_void),
    pub glyph_run_facts: unsafe extern "C" fn(*mut c_void, *mut c_void, u32, f64) -> FfiGlyphRunFacts,
    pub cursor_facts: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> FfiCursorFacts,
    pub glyph_intercepts: unsafe extern "C" fn(*mut c_void, *mut c_void, u32, f64, f32, f32, *mut c_void),
    pub layer_image_prepare:
        unsafe extern "C" fn(*mut c_void, *mut c_void, FfiLayerImageList, u32) -> FfiLayerImagePrepareFacts,
    pub layer_image_nested_display_list: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        FfiLayerImageList,
        u32,
        *const i32,
    ) -> FfiLayerImageNestedDisplayListFacts,
    pub layer_image_current_frame:
        unsafe extern "C" fn(*mut c_void, *mut c_void, FfiLayerImageList, u32, *const i32) -> FfiLayerImageFrameFacts,
    pub layer_image_paint: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        FfiLayerImageList,
        u32,
        *const f32,
        *const i32,
        u8,
        *const f32,
    ) -> FfiImagePaintFacts,
    pub replaced_paint_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiReplacedPaintFacts,
    pub replaced_image_paint:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *const f32, *const f32) -> FfiImagePaintFacts,
    pub backdrop_filter_bytes: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> bool,
    pub nested_display_list_from_bytes: unsafe extern "C" fn(*mut c_void, *const u8, usize, i32, i32) -> u64,
    pub svg_host_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiSvgHostFacts,
    pub svg_paint_style: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        bool,
        *const FfiSvgPaintContext,
        *mut c_void,
    ) -> FfiSvgPaintStyle,
    pub accumulated_2d_scale: unsafe extern "C" fn(*mut c_void, *const c_void, usize, *mut f32),
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
    pub colors: Vec<u32>,
    pub positions: Vec<f32>,
}

#[derive(Default)]
pub struct TextSpanSink {
    pub spans: Vec<FfiTextSpan>,
    pub shadows: Vec<FfiTextShadowLayer>,
}

impl FfiPaintHostCallbacks {
    pub(crate) fn outline_facts(&self, paintable_shell: *mut c_void) -> FfiOutlineFacts {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.outline_facts)(self.context, paintable_shell) }
    }
    pub(crate) fn overlay_facts(&self, paintable_shell: *mut c_void) -> FfiOverlayFacts {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.overlay_facts)(self.context, paintable_shell) }
    }
    pub(crate) fn async_scroll_facts(&self, paintable_shell: *mut c_void) -> FfiAsyncScrollFacts {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.async_scroll_facts)(self.context, paintable_shell) }
    }
    pub(crate) fn overlay_label(
        &self,
        paintable_shell: *mut c_void,
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
                paintable_shell,
                text.as_ptr(),
                text.len(),
                utf16_fly_string_raw,
                css_font_size,
                (&raw mut glyphs).cast(),
            )
        };
        (facts, glyphs)
    }
    pub(crate) fn text_spans(&self, paintable_shell: *mut c_void, owned_fragment_indices: &[u32]) -> TextSpanSink {
        let mut sink = TextSpanSink::default();
        // SAFETY: The C++ host pushes into the sink through the exported functions, synchronously.
        unsafe {
            (self.text_spans)(
                self.context,
                paintable_shell,
                owned_fragment_indices.as_ptr(),
                owned_fragment_indices.len(),
                (&raw mut sink).cast(),
            );
        };
        sink
    }
    pub(crate) fn glyph_run_facts(
        &self,
        paintable_shell: *mut c_void,
        fragment_index: u32,
        scale: f64,
    ) -> FfiGlyphRunFacts {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.glyph_run_facts)(self.context, paintable_shell, fragment_index, scale) }
    }
    pub(crate) fn cursor_facts(&self, paintable_shell: *mut c_void, owner_shell: *mut c_void) -> FfiCursorFacts {
        // SAFETY: The C++ host answers synchronously from live paintable shells.
        unsafe { (self.cursor_facts)(self.context, paintable_shell, owner_shell) }
    }
    pub(crate) fn glyph_intercepts(
        &self,
        paintable_shell: *mut c_void,
        fragment_index: u32,
        scale: f64,
        y_top: f32,
        y_bottom: f32,
    ) -> Vec<f32> {
        let mut intercepts: Vec<f32> = Vec::new();
        // SAFETY: The C++ host pushes into the Vec through the exported function, synchronously.
        unsafe {
            (self.glyph_intercepts)(
                self.context,
                paintable_shell,
                fragment_index,
                scale,
                y_top,
                y_bottom,
                (&raw mut intercepts).cast(),
            );
        };
        intercepts
    }
    pub(crate) fn layer_image_prepare(
        &self,
        paintable_shell: *mut c_void,
        list: FfiLayerImageList,
        computed_index: u32,
    ) -> FfiLayerImagePrepareFacts {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.layer_image_prepare)(self.context, paintable_shell, list, computed_index) }
    }
    pub(crate) fn layer_image_nested_display_list(
        &self,
        paintable_shell: *mut c_void,
        list: FfiLayerImageList,
        computed_index: u32,
        device_dest_rect: libgfx_rust::IntRect,
    ) -> FfiLayerImageNestedDisplayListFacts {
        let dest = [
            device_dest_rect.x,
            device_dest_rect.y,
            device_dest_rect.width,
            device_dest_rect.height,
        ];
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe {
            (self.layer_image_nested_display_list)(self.context, paintable_shell, list, computed_index, dest.as_ptr())
        }
    }
    pub(crate) fn layer_image_current_frame(
        &self,
        paintable_shell: *mut c_void,
        list: FfiLayerImageList,
        computed_index: u32,
        device_dest_rect: libgfx_rust::IntRect,
    ) -> FfiLayerImageFrameFacts {
        let dest = [
            device_dest_rect.x,
            device_dest_rect.y,
            device_dest_rect.width,
            device_dest_rect.height,
        ];
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.layer_image_current_frame)(self.context, paintable_shell, list, computed_index, dest.as_ptr()) }
    }
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn layer_image_paint(
        &self,
        paintable_shell: *mut c_void,
        list: FfiLayerImageList,
        computed_index: u32,
        dest: [f32; 4],
        css_tile_size: [i32; 2],
        image_rendering: u8,
        accumulated_scale: libgfx_rust::FloatSize,
    ) -> FfiImagePaintFacts {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe {
            (self.layer_image_paint)(
                self.context,
                paintable_shell,
                list,
                computed_index,
                dest.as_ptr(),
                css_tile_size.as_ptr(),
                image_rendering,
                [accumulated_scale.width, accumulated_scale.height].as_ptr(),
            )
        }
    }
    pub(crate) fn image_intrinsic_facts(
        &self,
        paintable_shell: *mut c_void,
        list: FfiLayerImageList,
        computed_index: u32,
    ) -> FfiImageIntrinsicFacts {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.image_intrinsic_facts)(self.context, paintable_shell, list, computed_index) }
    }
    pub(crate) fn root_background_source(&self) -> FfiRootBackgroundSource {
        // SAFETY: The C++ host answers synchronously.
        unsafe { (self.root_background_source)(self.context) }
    }
    pub(crate) fn replaced_paint_facts(&self, paintable_shell: *mut c_void) -> FfiReplacedPaintFacts {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.replaced_paint_facts)(self.context, paintable_shell) }
    }
    pub(crate) fn replaced_image_paint(
        &self,
        paintable_shell: *mut c_void,
        dest: [f32; 4],
        accumulated_scale: libgfx_rust::FloatSize,
    ) -> FfiImagePaintFacts {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe {
            (self.replaced_image_paint)(
                self.context,
                paintable_shell,
                dest.as_ptr(),
                [accumulated_scale.width, accumulated_scale.height].as_ptr(),
            )
        }
    }
    pub(crate) fn backdrop_filter_bytes(&self, paintable_shell: *mut c_void) -> Option<Vec<u8>> {
        let mut bytes: Vec<u8> = Vec::new();
        // SAFETY: The C++ host pushes into the Vec through the exported sink function, synchronously.
        let has_filter =
            unsafe { (self.backdrop_filter_bytes)(self.context, paintable_shell, (&raw mut bytes).cast()) };
        has_filter.then_some(bytes)
    }
    pub(crate) fn nested_display_list_from_bytes(
        &self,
        bytes: &[u8],
        content_offset: libgfx_rust::IntPoint,
    ) -> crate::painting::display_list::commands::DisplayListResourceId {
        // SAFETY: The C++ host copies the bytes synchronously.
        let id = unsafe {
            (self.nested_display_list_from_bytes)(
                self.context,
                bytes.as_ptr(),
                bytes.len(),
                content_offset.x,
                content_offset.y,
            )
        };
        crate::painting::display_list::commands::DisplayListResourceId(id)
    }
    pub(crate) fn svg_host_facts(&self, paintable_shell: *mut c_void) -> FfiSvgHostFacts {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.svg_host_facts)(self.context, paintable_shell) }
    }
    pub(crate) fn svg_paint_style(
        &self,
        paintable_shell: *mut c_void,
        is_stroke: bool,
        paint_context: &FfiSvgPaintContext,
    ) -> (FfiSvgPaintStyle, ColorStopSink) {
        let mut sink = ColorStopSink::default();
        // SAFETY: The C++ host answers synchronously, pushing color stops into the sink through the exported function.
        let style = unsafe {
            (self.svg_paint_style)(
                self.context,
                paintable_shell,
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
        let mut out = [0f32; 2];
        let tree = visual_context_tree.map_or(std::ptr::null(), |tree| std::ptr::from_ref(tree).cast());
        // SAFETY: The C++ host answers synchronously and only borrows the optional tree.
        unsafe { (self.accumulated_2d_scale)(self.context, tree, visual_context_index, out.as_mut_ptr()) };
        libgfx_rust::FloatSize {
            width: out[0],
            height: out[1],
        }
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
