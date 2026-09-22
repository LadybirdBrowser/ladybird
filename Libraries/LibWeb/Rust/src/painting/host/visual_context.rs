/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::easing::FfiEasingDescriptor;
use crate::css::ffi_support::FfiUtf16View;
use crate::layout::used_values;
use crate::layout::used_values::OptionalCssPixelRect;
use libgfx_rust::filter::Filter;
use libgfx_rust::{Color, CompositingAndBlendingOperator, IntRect, InterpolationColorSpace};
use std::ffi::c_void;

pub use crate::painting::visual_context::ffi_types::*;

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
}

/// Whether a keyframe gives a property a value of its own, takes the target's underlying style
/// for it, or leaves it out.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiCompositorKeyframeValueState {
    Absent,
    UsesUnderlyingStyle,
    Present,
}

/// One keyframe of an effect as the compositor animation builder reads it. The values themselves
/// are resolved through the host on demand; the keyframe only says which properties it carries.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiCompositorAnimationKeyframe {
    pub offset: f64,
    pub easing: FfiEasingDescriptor,
    /// An easing the main thread could not describe keeps the effect off the compositor.
    pub easing_is_supported: bool,
    pub composite_is_replace: bool,
    pub opacity: FfiCompositorKeyframeValueState,
    pub background_color: FfiCompositorKeyframeValueState,
    pub filter: FfiCompositorKeyframeValueState,
    pub translate: FfiCompositorKeyframeValueState,
    pub rotate: FfiCompositorKeyframeValueState,
    pub scale: FfiCompositorKeyframeValueState,
    pub transform: FfiCompositorKeyframeValueState,
}

/// The timing of an effect at the moment the main thread anchored it to the monotonic clock.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiCompositorAnimationTiming {
    pub monotonic_time_at_anchor_ns: i64,
    pub local_time_at_anchor_ms: f64,
    pub playback_rate: f64,
    pub start_delay_ms: f64,
    pub iteration_duration_ms: f64,
    pub iteration_count: f64,
    pub iteration_start: f64,
    pub playback_direction: FfiVisualAnimationPlaybackDirection,
    pub fill_mode: FfiVisualAnimationFillMode,
    pub easing: FfiEasingDescriptor,
}

pub const TARGETED_TRANSFORM_PROPERTY_TRANSLATE: u8 = 1;
pub const TARGETED_TRANSFORM_PROPERTY_ROTATE: u8 = 2;
pub const TARGETED_TRANSFORM_PROPERTY_SCALE: u8 = 4;
pub const TARGETED_TRANSFORM_PROPERTY_TRANSFORM: u8 = 8;

/// What the builder needs from an effect to build the compositor animation of one target kind, once
/// the main thread has found the effect eligible.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiCompositorAnimationRequest {
    pub target_kind: FfiVisualAnimationTargetKind,
    /// The target's box, whose visual context nodes the animation drives.
    pub layout_node: crate::layout::node_data::NodeSlotId,
    pub timing: FfiCompositorAnimationTiming,
    pub keyframes: *const FfiCompositorAnimationKeyframe,
    pub keyframe_count: usize,
    /// The identity of the effect's keyframe set and the versions of the style it resolves against;
    /// the builder keeps the values it lowered while these stay the same.
    pub key_frame_set_identity: u64,
    pub target_style_generation: u64,
    pub style_environment_version: u64,
    /// The box transform percentages resolve against, in CSS pixels; zero for the other kinds.
    pub reference_box_width: f32,
    pub reference_box_height: f32,
    pub device_pixels_per_css_pixel: f32,
    /// The transform-family properties the effect targets, as TARGETED_TRANSFORM_PROPERTY flags.
    pub targeted_transform_properties: u8,
    /// Whether the effect targets the transform property and nothing else.
    pub targets_only_transform: bool,
}

/// What the builder asks the main thread for while lowering keyframe values.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiCompositorAnimationHost {
    pub context: *mut std::ffi::c_void,
    /// The keyframe's value of the property, resolved and absolutized for the target, as a
    /// retained style value; null when it resolves to nothing usable. With
    /// `uses_underlying_style` set, the target's computed value without animations instead.
    pub resolved_keyframe_value: unsafe extern "C" fn(
        context: *mut std::ffi::c_void,
        keyframe_index: usize,
        property_id: u16,
        uses_underlying_style: bool,
    ) -> *const std::ffi::c_void,
    /// The color a resolved color value names for the target; false when it names none.
    pub resolve_color: unsafe extern "C" fn(
        context: *mut std::ffi::c_void,
        value: *const std::ffi::c_void,
        color: *mut libgfx_rust::Color,
    ) -> bool,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiCompositorAnimationBuildOutcome {
    /// The animation is built and waits with the effect's other pending animations.
    pub built: bool,
    /// The animation was valid but the target owns no node of the kind it drives yet.
    pub missing_visual_context_node: bool,
    /// Whether a transform animation's keyframes only ever translate horizontally, once known.
    pub only_translates_horizontally_is_known: bool,
    pub only_translates_horizontally: bool,
}
