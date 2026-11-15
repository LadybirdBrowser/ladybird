/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::easing::FfiEasingDescriptor;
use crate::css::ffi_support::FfiUtf16View;
use crate::layout::svg_formatting_context::FfiSvgNumberPercentage;
use crate::layout::used_values::OptionalCssPixelRect;
use libgfx_rust::filter::Filter;
use libgfx_rust::{Color, CompositingAndBlendingOperator, IntRect, InterpolationColorSpace};
use std::ffi::c_void;

pub use crate::painting::visual_context::ffi_types::*;

/// How much of the visual context tree an update walks, in increasing order: the dirty boxes, every
/// box into the kept tree, or every box into a fresh tree.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u8)]
pub enum VisualContextUpdateScope {
    #[default]
    DirtyPath,
    EveryBox,
    FreshTree,
}

impl VisualContextUpdateScope {
    pub fn rebuilds_every_box(self) -> bool {
        self != Self::DirtyPath
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
#[expect(dead_code, reason = "C++ constructs the variants")]
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
    /// feTurbulence: whether `stitchTiles` is `stitch`.
    pub turbulence_stitches_tiles: bool,
    /// feDisplacementMap: the scale and the channel selector constants.
    pub scale: f32,
    pub x_channel_selector: u16,
    pub y_channel_selector: u16,
    pub image_src_rect: IntRect,
    /// feImage: the image's natural size, zero along an axis where it has none.
    pub image_natural_width: f32,
    pub image_natural_height: f32,
    /// feImage: how the image fits its subregion, in the encoding of its `preserveAspectRatio`
    /// attribute facts.
    pub image_preserve_aspect_ratio_align: u8,
    pub image_preserve_aspect_ratio_meet_or_slice: u8,
    /// The primitive's `x`, `y`, `width` and `height`, in `primitiveUnits`, each of them only where
    /// [`Self::subregion_specified`] has its bit set.
    pub subregion_x: FfiSvgNumberPercentage,
    pub subregion_y: FfiSvgNumberPercentage,
    pub subregion_width: FfiSvgNumberPercentage,
    pub subregion_height: FfiSvgNumberPercentage,
    /// Which of the subregion attributes the primitive specifies: x, y, width and height, from the
    /// lowest bit up.
    pub subregion_specified: u8,
}

pub(crate) const SUBREGION_SPECIFIES_X: u8 = 1 << 0;
pub(crate) const SUBREGION_SPECIFIES_Y: u8 = 1 << 1;
pub(crate) const SUBREGION_SPECIFIES_WIDTH: u8 = 1 << 2;
pub(crate) const SUBREGION_SPECIFIES_HEIGHT: u8 = 1 << 3;

/// The attribute facts of an SVG `<filter>` element itself: its filter region and the units its
/// region and its primitives' lengths are in.
// https://drafts.csswg.org/filter-effects-1/#FilterEffectsRegion
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FfiSvgFilterRegion {
    pub filter_units_are_object_bounding_box: bool,
    pub primitive_units_are_object_bounding_box: bool,
    pub x: FfiSvgNumberPercentage,
    pub y: FfiSvgNumberPercentage,
    pub width: FfiSvgNumberPercentage,
    pub height: FfiSvgNumberPercentage,
}

impl Default for FfiSvgFilterRegion {
    // https://drafts.csswg.org/filter-effects-1/#element-attrdef-filter-x
    // If the attribute is not specified, the effect is as if a value of -10% were specified.
    fn default() -> Self {
        let percentage = |value| FfiSvgNumberPercentage {
            value,
            is_percentage: true,
        };
        Self {
            filter_units_are_object_bounding_box: true,
            primitive_units_are_object_bounding_box: false,
            x: percentage(-0.1),
            y: percentage(-0.1),
            width: percentage(1.2),
            height: percentage(1.2),
        }
    }
}

/// One primitive of an SVG `<filter>` as the host flattens it: its values, and the names, lists
/// and tables it borrows from the element for the duration of one push.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSvgFilterPrimitive<'a> {
    pub values: SvgFilterPrimitiveValues,
    pub in1: FfiUtf16View<'a>,
    pub in2: FfiUtf16View<'a>,
    pub result: FfiUtf16View<'a>,
    /// feMerge: the `in` of each feMergeNode child.
    pub merge_inputs: *const FfiUtf16View<'a>,
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

/// The timing of an effect at the moment the main thread anchored it to the monotonic clock.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiCompositorAnimationTiming {
    pub monotonic_time_at_anchor_ns: i64,
    pub local_time_at_anchor_ms: f64,
    pub playback_rate: f64,
    /// The effect's timing, every time in milliseconds.
    pub effect: crate::animations::timing::AnimationEffectTiming,
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
    /// The target's InheritedUIValues and InheritedTextValues group payloads, whose used color-scheme and color
    /// the keyframes' colors resolve against.
    pub target_inherited_ui_values: *const std::ffi::c_void,
    pub target_inherited_text_values: *const std::ffi::c_void,
    pub timing: FfiCompositorAnimationTiming,
    /// The `KeyframeSet` the effect runs, which the host holds a reference to, or null for an effect with none.
    pub keyframe_set: *const std::ffi::c_void,
    /// Whether the effect's composite operation, which a keyframe whose own is `auto` runs, is `replace`.
    pub effect_composite_is_replace: bool,
    /// The easing a keyframe that declares none runs: its animation's.
    pub default_easing: FfiEasingDescriptor,
    /// The versions of the style the keyframes resolve against; the builder keeps the values it lowered while these
    /// and the keyframe set stay the same.
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
    /// The value `declared_value` a keyframe declares for the property, resolved and absolutized for the target, as a
    /// retained style value; null when it resolves to nothing usable. Where `declared_value` is null, the keyframe
    /// takes the target's underlying style, and the target's computed value without animations is answered instead.
    pub resolved_keyframe_value: unsafe extern "C" fn(
        context: *mut std::ffi::c_void,
        property_id: u16,
        declared_value: *const std::ffi::c_void,
    ) -> *const std::ffi::c_void,
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
