/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

use crate::css::ffi_support::FfiUtf16View;
use crate::layout::used_values;
use crate::layout::used_values::OptionalCssPixelRect;
use crate::painting::display_list::commands::OptionalF32;
use libgfx_rust::filter::Filter;
use libgfx_rust::{
    Color, CompositingAndBlendingOperator, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, IntRect,
    InterpolationColorSpace,
};
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiVisualContextBoxDirtyKind {
    StyleValueChange,
    StyleStructuralChange,
    ScrollableOverflowFlipped,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiVisualContextGlobalRebuildReason {
    FirstBuild,
    DocumentWideStructuralChange,
    FilterResourcesChanged,
    ForcedForTesting,
    CanonicalDumpRequested,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiVisualContextBoxNodeList {
    SpatialNodes,
    ClipNodes,
    EffectNodes,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiVisualContextUpdateOutcome {
    pub performed_full_build: bool,
    pub structural_epoch_changed: bool,
    pub requires_display_list_recording: bool,
    pub structural_epoch: u64,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVisualContextTreeInputs {
    pub device_pixels_per_css_pixel: f64,
    pub visual_viewport_offset_x: f64,
    pub visual_viewport_offset_y: f64,
    pub visual_viewport_scale: f64,
    pub viewport_wheel_overflow_x: u8,
    pub viewport_wheel_overflow_y: u8,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiSvgFilterPrimitiveKind {
    #[default]
    Blend,
    ColorMatrix,
    ComponentTransfer,
    Composite,
    DisplacementMap,
    DropShadow,
    Flood,
    GaussianBlur,
    Image,
    Merge,
    Morphology,
    Offset,
    Turbulence,
}

/// The plain-value attribute facts of one primitive of an SVG `<filter>`. Lengths are in the
/// filtered element's user units; enumerations carry the IDL constant of the attribute. A kind
/// leaves the fields it has no use for zero.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct SvgFilterPrimitiveValues {
    pub kind: FfiSvgFilterPrimitiveKind,
    /// The `color-interpolation-filters` the primitive operates in.
    pub operating_color_space: InterpolationColorSpace,
    /// feColorMatrix: the `type` constant.
    pub color_matrix_type: u16,
    /// feComposite: the `operator` constant and the arithmetic coefficients.
    pub composite_operator: u8,
    pub k1: f32,
    pub k2: f32,
    pub k3: f32,
    pub k4: f32,
    /// feBlend.
    pub blend_mode: CompositingAndBlendingOperator,
    /// feFlood and feDropShadow.
    pub flood_color: Color,
    pub flood_opacity: f32,
    /// feGaussianBlur and feDropShadow.
    pub std_deviation_x: f32,
    pub std_deviation_y: f32,
    /// feOffset and feDropShadow.
    pub dx: f32,
    pub dy: f32,
    /// feMorphology: the `operator` constant and the radii.
    pub morphology_operator: u8,
    pub radius_x: f32,
    pub radius_y: f32,
    /// feTurbulence: the `type` constant, the base frequencies per user unit, and the tile to
    /// stitch, zero-sized when the primitive does not stitch.
    pub turbulence_type: u16,
    pub base_frequency_x: f32,
    pub base_frequency_y: f32,
    pub num_octaves: i32,
    pub seed: f32,
    pub stitch_tile_width: f32,
    pub stitch_tile_height: f32,
    /// feDisplacementMap: the scale and the channel selector constants.
    pub scale: f32,
    pub x_channel_selector: u16,
    pub y_channel_selector: u16,
    pub image_src_rect: IntRect,
}

/// One primitive of an SVG `<filter>` as the host flattens it: its values, and the names, lists
/// and tables it borrows from the element for the duration of one push.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSvgFilterPrimitive {
    pub values: SvgFilterPrimitiveValues,
    pub in1: FfiUtf16View,
    pub in2: FfiUtf16View,
    pub result: FfiUtf16View,
    /// feMerge: the `in` of each feMergeNode child.
    pub merge_inputs: *const FfiUtf16View,
    pub merge_input_count: usize,
    /// feColorMatrix: the parsed `values` list.
    pub color_matrix_values: *const f32,
    pub color_matrix_value_count: usize,
    /// feComponentTransfer: 256-entry lookup tables for A, R, G and B, null where the channel has no
    /// transfer function.
    pub component_transfer_tables: [*const u8; 4],
    pub image_frame: *const c_void,
}

#[derive(Default)]
pub(crate) struct ResolvedSvgFilter {
    pub failed: bool,
    /// The referenced filter's graph, already in device pixels; absent when the `<filter>` has no
    /// primitives.
    pub filter: Option<Filter>,
    pub svg_filter_bounds: OptionalCssPixelRect,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiVisualContextHostCallbacks {
    pub context: *mut c_void,
    pub tree_inputs: unsafe extern "C" fn(*mut c_void) -> FfiVisualContextTreeInputs,
    pub scroll_offset: unsafe extern "C" fn(*mut c_void, *mut c_void) -> used_values::FfiCssPixelPoint,
    pub node_identity: unsafe extern "C" fn(*mut c_void, *mut c_void) -> i64,
    pub root_background_source: unsafe extern "C" fn(*mut c_void) -> FfiRootBackgroundSource,
}

impl FfiVisualContextHostCallbacks {
    pub(crate) fn tree_inputs(&self) -> FfiVisualContextTreeInputs {
        // SAFETY: The C++ host answers synchronously.
        unsafe { (self.tree_inputs)(self.context) }
    }
    pub(crate) fn node_identity(&self, layout_node_shell: *mut c_void) -> i64 {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.node_identity)(self.context, layout_node_shell) }
    }
    pub(crate) fn scroll_offset(&self, layout_node_shell: *mut c_void) -> used_values::FfiCssPixelPoint {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.scroll_offset)(self.context, layout_node_shell) }
    }
    pub(crate) fn root_background_source(&self) -> FfiRootBackgroundSource {
        // SAFETY: The C++ host answers synchronously.
        unsafe { (self.root_background_source)(self.context) }
    }
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVisualViewportTransform {
    pub matrix: FloatMatrix4x4,
    pub origin: FloatPoint,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiEffectOpacitySample {
    pub effect: u32,
    pub opacity: f32,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiEffectBackgroundColorSample {
    pub effect: u32,
    pub color: libgfx_rust::Color,
}

#[derive(Clone, Copy, Debug)]
#[repr(u8)]
pub enum FfiVisualAnimationTargetKind {
    Opacity,
    BackgroundColor,
    Filter,
    Transform,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiEffectFilterSample {
    pub effect: u32,
    pub filter_bytes: *const u8,
    pub filter_size: usize,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiSpatialTransformSample {
    pub spatial: u32,
    pub matrix: FloatMatrix4x4,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiTestStickyConstraints {
    pub scroller: u32,
    pub has_parent_sticky: bool,
    pub parent_sticky: u32,
    pub position_relative_to_scroller: FloatPoint,
    pub border_box_size: FloatSize,
    pub scrollport_size: FloatSize,
    pub containing_block_region: FloatRect,
    pub needs_parent_offset_adjustment: bool,
    pub inset_top: OptionalF32,
    pub inset_right: OptionalF32,
    pub inset_bottom: OptionalF32,
    pub inset_left: OptionalF32,
}
